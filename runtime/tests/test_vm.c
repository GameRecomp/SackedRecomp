/* SPDX-License-Identifier: MIT */
#include "rc/mem.h"
#include "rc/vm.h"
#include "test.h"

void test_vm(void)
{
    CHECK(rc_mem_reserve(UINT64_C(1) << 32));

    /* Reservations: fixed, anywhere, overlapping, released. */
    CHECK_EQ(rc_vm_alloc(0x00400000, 0x1234, true), 0x00400000);
    CHECK_EQ(rc_vm_alloc(0x00400000, 0x1000, false), 0); /* taken */
    uint32_t a = rc_vm_alloc(0, 0x20000, false);
    CHECK(a >= 0x00700000 && a % 0x10000 == 0);
    CHECK(!rc_vm_committed(a, 1));
    CHECK(rc_vm_commit(a + 0x1000, 0x2000));
    CHECK(rc_vm_committed(a + 0x1000, 0x2000));
    CHECK(!rc_vm_committed(a, 0x2000));
    rc_st32(a + 0x1000, 0xfeedface);
    CHECK(rc_vm_decommit(a + 0x1000, 0x1000));
    CHECK(rc_vm_commit(a + 0x1000, 0x1000));
    CHECK_EQ(rc_ld32(a + 0x1000), 0); /* recommitted memory is zero */
    uint32_t b = rc_vm_alloc(0, 0x10000, true);
    CHECK_EQ(b, a + 0x20000);
    CHECK(rc_vm_release(a));
    CHECK_EQ(rc_vm_alloc(0, 0x10000, false), a); /* first fit reuses the gap */

    /* The heap. */
    uint32_t p = rc_heap_alloc(10, true);
    CHECK(p % 8 == 0 && rc_heap_size(p) == 10);
    CHECK_EQ(rc_ld32(p), 0);
    uint32_t q = rc_heap_alloc(10, false);
    CHECK(q != p);
    rc_heap_free(p);
    CHECK_EQ(rc_heap_alloc(12, false), p); /* same class: reused */
    CHECK(rc_heap_owns(q));
    CHECK(!rc_heap_owns(q + 4));
    rc_st32(q, 0x11223344);
    uint32_t r = rc_heap_realloc(q, 5000, true, false);
    CHECK(r != 0 && rc_ld32(r) == 0x11223344 && rc_heap_size(r) == 5000);
    CHECK_EQ(rc_ld32(r + 4996), 0);
    CHECK(!rc_heap_owns(q));
    uint32_t big = rc_heap_alloc(1u << 20, true);
    CHECK(big != 0 && rc_heap_size(big) == (1u << 20));
    CHECK_EQ(rc_heap_realloc(big, 2u << 20, false, true), 0); /* cannot grow in place */
    rc_heap_free(big);
    CHECK(!rc_heap_owns(big));

    /* Heaps reuse only their own blocks (a DLL's C runtime never gets the
     * program's freed ones), and a block stays in its heap. */
    uint32_t h1 = rc_heap_alloc_in(1, 400, false);
    rc_st32(h1 + 0x100, 0xededededu);
    rc_heap_free(h1);
    uint32_t h2 = rc_heap_alloc_in(2, 400, false);
    CHECK(h2 != h1);
    CHECK_EQ(rc_ld32(h2 + 0x100), 0); /* fresh memory */
    CHECK_EQ(rc_heap_alloc_in(1, 400, false), h1);
    uint32_t moved = rc_heap_realloc(h2, 3000, false, false);
    CHECK(moved != 0 && moved != h2);
    CHECK_EQ(rc_heap_alloc_in(2, 400, false), h2); /* freed into heap 2 */
    rc_heap_free(moved);
    CHECK(rc_heap_alloc_in(1, 3000, false) != moved);
    CHECK_EQ(rc_heap_alloc_in(2, 3000, false), moved); /* realloc kept heap 2 */
    CHECK(rc_heap_alloc_in(RC_HEAPS + 5, 16, false) != 0); /* unknown heap: heap 0 */

    rc_mem_release();
}
