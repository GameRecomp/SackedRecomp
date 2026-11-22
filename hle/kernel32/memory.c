/* SPDX-License-Identifier: MIT */
/*
 * kernel32: heaps and virtual memory, on the runtime's guest allocator.
 * Every HeapCreate gets a heap of its own (rc_heap_alloc_in), which reuses
 * only its own freed blocks, as on Windows: the C runtimes of the program
 * and of each DLL do not get each other's freed (and filled) blocks. Handles
 * the program did not create (the process heap) use heap 0.
 */
#include <string.h>

#include "k32.h"
#include "rc/vm.h"

#define HEAP_ZERO_MEMORY 0x08u
#define HEAP_REALLOC_IN_PLACE_ONLY 0x10u
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u

/* Handles, never dereferenced by the game: HEAP_HANDLES + 0x10 * index. */
#define HEAP_HANDLES 0x00500000u
static uint32_t heaps_made;

/* The runtime's heap for a handle (the last one serves every heap after
 * RC_HEAPS - 1 of them). */
static uint32_t heap_of(uint32_t handle)
{
    if (handle <= HEAP_HANDLES || handle > HEAP_HANDLES + 0x10u * heaps_made || (handle & 0xfu)) {
        return 0;
    }
    uint32_t i = (handle - HEAP_HANDLES) / 0x10u;
    return i < RC_HEAPS ? i : RC_HEAPS - 1;
}

static void k_HeapCreate(rc_cpu *c)
{
    heaps_made++;
    RC_RETURN(c, HEAP_HANDLES + 0x10u * heaps_made, 3);
}

static void k_HeapDestroy(rc_cpu *c) { RC_RETURN(c, 1, 1); }

static void k_HeapAlloc(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), size = RC_ARG(c, 2);
    uint32_t p = rc_heap_alloc_in(heap_of(RC_ARG(c, 0)), size, (flags & HEAP_ZERO_MEMORY) != 0);
    if (!p) {
        rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    RC_RETURN(c, p, 3);
}

static void k_HeapFree(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 2);
    if (p && !rc_heap_owns(p)) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 3);
    }
    rc_heap_free(p);
    RC_RETURN(c, 1, 3);
}

static void k_HeapReAlloc(rc_cpu *c)
{
    uint32_t flags = RC_ARG(c, 1), p = RC_ARG(c, 2), size = RC_ARG(c, 3);
    uint32_t q = rc_heap_realloc(p, size, (flags & HEAP_ZERO_MEMORY) != 0,
                                 (flags & HEAP_REALLOC_IN_PLACE_ONLY) != 0);
    if (!q) {
        rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
    }
    RC_RETURN(c, q, 4);
}

static void k_HeapSize(rc_cpu *c) { RC_RETURN(c, rc_heap_size(RC_ARG(c, 2)), 3); }

static void k_VirtualAlloc(rc_cpu *c)
{
    uint32_t addr = RC_ARG(c, 0), size = RC_ARG(c, 1), type = RC_ARG(c, 2);
    uint32_t base, rsize;
    if ((type & MEM_RESERVE) || !rc_vm_region(addr, &base, &rsize)) {
        if (addr && !(type & MEM_RESERVE)) {
            rc_last_error = 487; /* ERROR_INVALID_ADDRESS */
            RC_RETURN(c, 0, 4);
        }
        uint32_t r = rc_vm_alloc(addr, size, (type & MEM_COMMIT) != 0);
        if (!r) {
            rc_last_error = ERROR_NOT_ENOUGH_MEMORY;
        }
        RC_RETURN(c, r, 4);
    }
    /* Commit inside an existing reservation. */
    uint32_t start = addr & ~(RC_PAGE - 1);
    if (!rc_vm_commit(start, addr + size - start)) {
        rc_last_error = ERROR_INVALID_PARAMETER;
        RC_RETURN(c, 0, 4);
    }
    RC_RETURN(c, start, 4);
}

static void k_VirtualFree(rc_cpu *c)
{
    uint32_t addr = RC_ARG(c, 0), size = RC_ARG(c, 1), type = RC_ARG(c, 2);
    bool ok = false;
    if (type & MEM_RELEASE) {
        ok = rc_vm_release(addr);
    } else if (type & MEM_DECOMMIT) {
        ok = rc_vm_decommit(addr, size);
    }
    if (!ok) {
        rc_last_error = ERROR_INVALID_PARAMETER;
    }
    RC_RETURN(c, ok, 3);
}

static void k_GlobalMemoryStatus(rc_cpu *c)
{
    /* A 2001 machine with plenty: 512 MiB of RAM, 1 GiB of page file. */
    uint32_t p = RC_ARG(c, 0);
    rc_st32(p + 0, 32);
    rc_st32(p + 4, 25);
    rc_st32(p + 8, 512u << 20);
    rc_st32(p + 12, 384u << 20);
    rc_st32(p + 16, 1024u << 20);
    rc_st32(p + 20, 900u << 20);
    rc_st32(p + 24, 2047u << 20);
    rc_st32(p + 28, 1900u << 20);
    RC_RETURN(c, 0, 1);
}

static void k_IsBadWritePtr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), n = RC_ARG(c, 1);
    RC_RETURN(c, !rc_vm_committed(p, n), 2);
}

static void k_IsBadReadPtr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), n = RC_ARG(c, 1);
    RC_RETURN(c, n && !rc_vm_committed(p, n), 2);
}

static void k_IsBadCodePtr(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    RC_RETURN(c, !rc_vm_committed(p, 1), 1);
}

/* ---- global and local memory ----
 * Windows has movable blocks with handles that are locked into pointers; here every block is
 * fixed and its handle is its address (GlobalLock returns it). The blocks handed out are kept in
 * a set, so freeing anything else (a handle of another kind) does no harm. */

#define GMEM_MOVEABLE 0x02u
#define GMEM_ZEROINIT 0x40u
#define BLOCKS 16384u

static uint32_t blocks[BLOCKS]; /* open addressing, 0 free, 1 deleted */

static uint32_t *slot_of(uint32_t p, bool insert)
{
    uint32_t i = (p >> 3) * 2654435761u % BLOCKS, del = BLOCKS;
    for (uint32_t k = 0; k < BLOCKS; k++, i = (i + 1) % BLOCKS) {
        if (blocks[i] == p) {
            return &blocks[i];
        }
        if (blocks[i] == 1 && del == BLOCKS) {
            del = i;
        }
        if (blocks[i] == 0) {
            return insert ? &blocks[del < BLOCKS ? del : i] : NULL;
        }
    }
    return insert && del < BLOCKS ? &blocks[del] : NULL;
}

static uint32_t block_alloc(uint32_t size, uint32_t flags)
{
    uint32_t p = rc_heap_alloc(size ? size : 1, (flags & GMEM_ZEROINIT) != 0);
    uint32_t *s = p ? slot_of(p, true) : NULL;
    if (s) {
        *s = p;
    }
    return p;
}

static bool block_known(uint32_t p) { return p > 1 && slot_of(p, false) != NULL; }

static uint32_t block_free(uint32_t p)
{
    uint32_t *s = p > 1 ? slot_of(p, false) : NULL;
    if (!s) {
        return p; /* not ours: as a failed free */
    }
    *s = 1;
    rc_heap_free(p);
    return 0;
}

static uint32_t block_realloc(uint32_t p, uint32_t size, uint32_t flags)
{
    if (!block_known(p)) {
        return 0;
    }
    uint32_t q = rc_heap_realloc(p, size ? size : 1, (flags & GMEM_ZEROINIT) != 0, false);
    if (q && q != p) {
        *slot_of(p, false) = 1;
        uint32_t *s = slot_of(q, true);
        if (s) {
            *s = q;
        }
    }
    return q;
}

/* GlobalAlloc(flags, size), LocalAlloc(flags, size) */
static void k_GlobalAlloc(rc_cpu *c) { RC_RETURN(c, block_alloc(RC_ARG(c, 1), RC_ARG(c, 0)), 2); }
/* GlobalReAlloc(handle, size, flags) */
static void k_GlobalReAlloc(rc_cpu *c) { RC_RETURN(c, block_realloc(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2)), 3); }
static void k_GlobalFree(rc_cpu *c) { RC_RETURN(c, block_free(RC_ARG(c, 0)), 1); }
static void k_GlobalLock(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0), 1); }
static void k_GlobalUnlock(rc_cpu *c) { RC_RETURN(c, 0, 1); } /* no lock count left: unlocked */
static void k_GlobalHandle(rc_cpu *c) { RC_RETURN(c, RC_ARG(c, 0), 1); }
static void k_GlobalFlags(rc_cpu *c) { RC_RETURN(c, 0, 1); }
static void k_GlobalSize(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0);
    RC_RETURN(c, block_known(p) ? rc_heap_size(p) : 0, 1);
}

static const rc_import_def global_defs[] = {
    {K32, "GlobalAlloc", k_GlobalAlloc},     {K32, "GlobalReAlloc", k_GlobalReAlloc},
    {K32, "GlobalFree", k_GlobalFree},       {K32, "GlobalLock", k_GlobalLock},
    {K32, "GlobalUnlock", k_GlobalUnlock},   {K32, "GlobalHandle", k_GlobalHandle},
    {K32, "GlobalFlags", k_GlobalFlags},     {K32, "GlobalSize", k_GlobalSize},
    {K32, "LocalAlloc", k_GlobalAlloc},      {K32, "LocalReAlloc", k_GlobalReAlloc},
    {K32, "LocalFree", k_GlobalFree},        {K32, "LocalLock", k_GlobalLock},
    {K32, "LocalUnlock", k_GlobalUnlock},    {K32, "LocalHandle", k_GlobalHandle},
    {K32, "LocalFlags", k_GlobalFlags},      {K32, "LocalSize", k_GlobalSize},
};

void rc_k32_global_init(void) { rc_register_imports(global_defs, sizeof global_defs / sizeof global_defs[0]); }

/* GetProcessHeap(): a handle that names heap 0 (heap_of gives 0 for it). */
static void k_GetProcessHeap(rc_cpu *c) { RC_RETURN(c, HEAP_HANDLES, 0); }

#define MEM_FREE 0x10000u
#define MEM_PRIVATE 0x20000u
#define MEM_IMAGE 0x1000000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READWRITE 0x40u

/* MEMORY_BASIC_INFORMATION for `addr` at `out`: a reservation of the virtual-memory layer, the
 * program's image, or free space up to the next 64 KiB. Returns the bytes written. */
static uint32_t query(uint32_t addr, uint32_t out, uint32_t len)
{
    if (len < 28) {
        rc_last_error = 24; /* ERROR_BAD_LENGTH */
        return 0;
    }
    uint32_t page = addr & ~(RC_PAGE - 1);
    uint32_t base, size, state = MEM_FREE, type = 0, protect = PAGE_NOACCESS, alloc = 0, aprot = 0;
    uint32_t region = RC_VM_GRANULE - (page & (RC_VM_GRANULE - 1));
    uint32_t img = rc_proc.image_base, img_size = rc_ld32(img + rc_ld32(img + 0x3c) + 0x50);
    if (page >= img && page < img + img_size) {
        alloc = img;
        state = MEM_COMMIT;
        type = MEM_IMAGE;
        protect = aprot = PAGE_EXECUTE_READWRITE;
        region = img + img_size - page;
    } else if (rc_vm_region(page, &base, &size)) {
        alloc = base;
        type = MEM_PRIVATE;
        aprot = PAGE_READWRITE;
        bool committed = rc_vm_committed(page, RC_PAGE);
        state = committed ? MEM_COMMIT : MEM_RESERVE;
        protect = committed ? PAGE_READWRITE : 0;
        uint32_t end = page + RC_PAGE;
        while (end < base + size && rc_vm_committed(end, RC_PAGE) == committed) {
            end += RC_PAGE;
        }
        region = end - page;
    }
    rc_st32(out + 0, page);
    rc_st32(out + 4, alloc);
    rc_st32(out + 8, aprot);
    rc_st32(out + 12, region);
    rc_st32(out + 16, state);
    rc_st32(out + 20, protect);
    rc_st32(out + 24, type);
    return 28;
}

/* VirtualQuery(addr, info, len), VirtualQueryEx(process, addr, info, len) */
static void k_VirtualQuery(rc_cpu *c) { RC_RETURN(c, query(RC_ARG(c, 0), RC_ARG(c, 1), RC_ARG(c, 2)), 3); }
static void k_VirtualQueryEx(rc_cpu *c) { RC_RETURN(c, query(RC_ARG(c, 1), RC_ARG(c, 2), RC_ARG(c, 3)), 4); }
/* Nothing is ever paged out. */
static void k_VirtualLock(rc_cpu *c) { RC_RETURN(c, 1, 2); }

const rc_import_def rc_k32_memory[] = {
    {K32, "GetProcessHeap", k_GetProcessHeap},
    {K32, "VirtualQuery", k_VirtualQuery},
    {K32, "VirtualQueryEx", k_VirtualQueryEx},
    {K32, "VirtualLock", k_VirtualLock},
    {K32, "VirtualUnlock", k_VirtualLock},
    {K32, "IsBadReadPtr", k_IsBadReadPtr},
    {K32, "IsBadCodePtr", k_IsBadCodePtr},
    {K32, "HeapCreate", k_HeapCreate},
    {K32, "HeapDestroy", k_HeapDestroy},
    {K32, "HeapAlloc", k_HeapAlloc},
    {K32, "HeapFree", k_HeapFree},
    {K32, "HeapReAlloc", k_HeapReAlloc},
    {K32, "HeapSize", k_HeapSize},
    {K32, "VirtualAlloc", k_VirtualAlloc},
    {K32, "VirtualFree", k_VirtualFree},
    {K32, "GlobalMemoryStatus", k_GlobalMemoryStatus},
    {K32, "IsBadWritePtr", k_IsBadWritePtr},
};
const uint32_t rc_k32_memory_count = sizeof rc_k32_memory / sizeof rc_k32_memory[0];
