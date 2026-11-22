/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT's C++ exceptions and __try/__except, end to end: frames and tables laid out as
 * Visual C++ 6 lays them out (FuncInfo, unwind map, try blocks, ThrowInfo; the scope table of
 * _except_handler3), "lifted" functions written by hand in C, and kernel32's RaiseException and
 * RtlUnwind walking the guest's registration chain. Each case checks the order in which the
 * pieces run (catch object, destructors, catch block, continuation) and the frame afterwards.
 */
#include <string.h>

#include "rc/cpu.h"
#include "rc/dispatch.h"
#include "rc/hle.h"
#include "rc/hle_modules.h"
#include "rc/mem.h"
#include "test.h"

int rc_test_failures;

#define MSVCRT "MSVCRT.dll"
#define K32 "KERNEL32.dll"
#define CHAIN_END 0xffffffffu
#define TEB 0x00300000u
#define STACK_TOP 0x001ff000u
#define DATA 0x00500000u

/* The "lifted" code. */
enum {
    OUTER = 0x401000u, /* a function with a try block: its code up to 0x401100 */
    INNER = 0x401100u, /* a function with an object to destroy, which throws */
    STUB_OUTER = 0x401200u,
    STUB_INNER = 0x401210u,
    CATCH = 0x401300u,
    CATCH_OTHER = 0x401310u,
    RETHROW = 0x401320u, /* a catch block with `throw;` */
    DTOR_A = 0x401400u,
    DTOR_B = 0x401410u,
    DTOR_C = 0x401420u,
    CONT = 0x401500u, /* where the catch block returns to: a label in OUTER */
    COPY = 0x401600u,
    OBJ_DTOR = 0x401700u,
    SEH_FRAME = 0x402000u, /* a function with __try/__except and __try/__finally inside */
    RAISER = 0x402100u,
    FILTER = 0x402200u,
    FINALLY = 0x402300u,
    EXCEPT = 0x402400u, /* the __except block: a label in SEH_FRAME */
    OUTERMOST = 0x403000u, /* try { OUTER } catch (...) */
    STUB_OUTERMOST = 0x403100u,
    CATCH_ALL = 0x403200u,
    CONT_OUTERMOST = 0x403300u,
};

static char events[256];
static void event(const char *what)
{
    strcat(events, what);
    strcat(events, " ");
}

static void ret(rc_cpu *c, uint32_t nargs) { c->r[RC_ESP] += 4u + 4u * nargs; }

/* Tables in guest memory. */
static uint32_t next_data = DATA;
static uint32_t put(const uint32_t *w, uint32_t n)
{
    uint32_t a = next_data;
    for (uint32_t i = 0; i < n; i++) {
        rc_st32(a + 4 * i, w[i]);
    }
    next_data += 4 * n;
    return a;
}
#define PUT(...) put((const uint32_t[]){__VA_ARGS__}, sizeof((const uint32_t[]){__VA_ARGS__}) / 4)

static uint32_t type_descriptor(const char *name)
{
    uint32_t td = PUT(0, 0);
    size_t n = strlen(name) + 1;
    memcpy(rc_ptr(next_data), name, n);
    next_data += (uint32_t)(n + 3) & ~3u;
    return td;
}

/* What a case throws and how the frames are set up. */
static uint32_t funcinfo_outer, funcinfo_inner, funcinfo_outermost, throw_info, win32_code;
static uint32_t thrown_obj, catch_object, catch_state, filter_result;

/* push ebp; mov ebp, esp; push -1; push stub; push fs:[0]; mov fs:[0], esp; sub esp, 20h;
 * mov [ebp-10h], esp. */
static uint32_t cxx_prologue(rc_cpu *c, uint32_t stub)
{
    rc_push32(c, c->r[RC_EBP]);
    c->r[RC_EBP] = c->r[RC_ESP];
    uint32_t ebp = c->r[RC_EBP];
    rc_push32(c, 0xffffffffu);
    rc_push32(c, stub);
    rc_push32(c, rc_ld32(c->fs_base));
    rc_st32(c->fs_base, c->r[RC_ESP]);
    c->r[RC_ESP] -= 0x20;
    rc_st32(ebp - 0x10, c->r[RC_ESP]);
    return ebp;
}

/* mov fs:[0], [ebp-0Ch]; mov esp, ebp; pop ebp; ret */
static void cxx_epilogue(rc_cpu *c, uint32_t eax)
{
    uint32_t ebp = c->r[RC_EBP];
    rc_st32(c->fs_base, rc_ld32(ebp - 0xc));
    c->r[RC_ESP] = ebp;
    c->r[RC_EBP] = rc_pop32(c);
    c->r[RC_EAX] = eax;
    ret(c, 0);
}

static void outer(rc_cpu *c)
{
    uint32_t ebp = cxx_prologue(c, STUB_OUTER);
    rc_st32(ebp - 4, 0); /* A constructed */
    rc_st32(ebp - 4, 1); /* try { */
    rc_st32(ebp - 4, 2); /* B constructed */
    rc_call_guest(c, INNER, 0, NULL);
    if (c->unwind) {
        static const uint32_t places[] = {CONT};
        rc_unwind_claim(c, ebp, OUTER, OUTER + 0x100, places, 1);
        return;
    }
    event("no-throw");
    cxx_epilogue(c, 0);
}

static void inner(rc_cpu *c)
{
    uint32_t ebp = cxx_prologue(c, STUB_INNER);
    rc_st32(ebp - 4, 0); /* C constructed */
    if (win32_code) {
        uint32_t a[4] = {win32_code, 0, 0, 0};
        rc_call_guest(c, rc_import_address(K32, "RaiseException"), 4, a);
    } else {
        thrown_obj = ebp - 0x20; /* the thrown object, in the thrower's frame */
        rc_st32(thrown_obj, 0x11111111u);
        rc_st32(thrown_obj + 4, 0x22222222u);
        uint32_t a[2] = {thrown_obj, throw_info};
        rc_call_guest(c, rc_import_address(MSVCRT, "_CxxThrowException"), 2, a);
    }
    if (c->unwind) {
        return;
    }
    event("inner-returned");
    cxx_epilogue(c, 0);
}

/* mov eax, FuncInfo; jmp __CxxFrameHandler */
static void stub_outer(rc_cpu *c)
{
    c->r[RC_EAX] = funcinfo_outer;
    rc_dispatch(c, rc_import_address(MSVCRT, "__CxxFrameHandler"));
}
static void stub_outermost(rc_cpu *c)
{
    c->r[RC_EAX] = funcinfo_outermost;
    rc_dispatch(c, rc_import_address(MSVCRT, "__CxxFrameHandler"));
}
static void stub_inner(rc_cpu *c)
{
    c->r[RC_EAX] = funcinfo_inner;
    rc_dispatch(c, rc_import_address(MSVCRT, "__CxxFrameHandler"));
}

/* Funclets run with the frame's ebp. */
static void catch_block(rc_cpu *c)
{
    event("catch");
    catch_object = rc_ld32(c->r[RC_EBP] - 0x18);
    catch_state = rc_ld32(c->r[RC_EBP] - 4);
    c->r[RC_EAX] = CONT;
    ret(c, 0);
}
static void catch_other(rc_cpu *c)
{
    event("catch-other");
    c->r[RC_EAX] = CONT;
    ret(c, 0);
}
static void rethrow(rc_cpu *c)
{
    event("rethrow");
    uint32_t a[2] = {0, 0};
    rc_call_guest(c, rc_import_address(MSVCRT, "_CxxThrowException"), 2, a);
    if (c->unwind) {
        return;
    }
    event("rethrow-returned");
    ret(c, 0);
}
static void dtor_a(rc_cpu *c) { event("~A"); ret(c, 0); }
static void dtor_b(rc_cpu *c) { event("~B"); ret(c, 0); }
static void dtor_c(rc_cpu *c) { event("~C"); ret(c, 0); }

/* The continuation: the rest of OUTER after the catch, on its own stack pointer. */
static void cont(rc_cpu *c)
{
    event("cont");
    CHECK_EQ(c->r[RC_ESP], rc_ld32(c->r[RC_EBP] - 0x10));
    CHECK_EQ(rc_ld32(c->fs_base), c->r[RC_EBP] - 0xc);
    cxx_epilogue(c, 0x600d);
}

/* Thing(const Thing &) (thiscall) and ~Thing() of the thrown object. */
static void copy(rc_cpu *c)
{
    event("copy");
    uint32_t src = rc_ld32(c->r[RC_ESP] + 4);
    CHECK_EQ(src, thrown_obj);
    rc_st32(c->r[RC_ECX], rc_ld32(src) + 1);
    rc_st32(c->r[RC_ECX] + 4, rc_ld32(src + 4));
    c->r[RC_EAX] = c->r[RC_ECX];
    ret(c, 1);
}
static void obj_dtor(rc_cpu *c)
{
    event("~Thing");
    CHECK_EQ(c->r[RC_ECX], thrown_obj);
    ret(c, 0);
}

/* try { OUTER } catch (...) { } */
static void outermost(rc_cpu *c)
{
    uint32_t ebp = cxx_prologue(c, STUB_OUTERMOST);
    rc_st32(ebp - 4, 0); /* try { */
    rc_call_guest(c, OUTER, 0, NULL);
    if (c->unwind) {
        static const uint32_t places[] = {CONT_OUTERMOST};
        rc_unwind_claim(c, ebp, OUTERMOST, OUTERMOST + 0x100, places, 1);
        return;
    }
    event("no-throw");
    cxx_epilogue(c, 0);
}

static void catch_all(rc_cpu *c)
{
    event("catch-all");
    c->r[RC_EAX] = CONT_OUTERMOST;
    ret(c, 0);
}

static void cont_outermost(rc_cpu *c)
{
    event("cont-outermost");
    CHECK_EQ(c->r[RC_ESP], rc_ld32(c->r[RC_EBP] - 0x10));
    cxx_epilogue(c, 0x0123);
}

/* ---- __try ---- */

/* push ebp; mov ebp, esp; push -1; push scope table; push _except_handler3; push fs:[0];
 * mov fs:[0], esp; sub esp, 20h; mov [ebp-18h], esp. */
static uint32_t scope_table;

static void seh_frame(rc_cpu *c)
{
    rc_push32(c, c->r[RC_EBP]);
    c->r[RC_EBP] = c->r[RC_ESP];
    uint32_t ebp = c->r[RC_EBP];
    rc_push32(c, 0xffffffffu);
    rc_push32(c, scope_table);
    rc_push32(c, rc_import_address(MSVCRT, "_except_handler3"));
    rc_push32(c, rc_ld32(c->fs_base));
    rc_st32(c->fs_base, c->r[RC_ESP]);
    c->r[RC_ESP] -= 0x20;
    rc_st32(ebp - 0x18, c->r[RC_ESP]);
    rc_st32(ebp - 4, 0); /* __try { */
    rc_st32(ebp - 4, 1); /*     __try { */
    rc_call_guest(c, RAISER, 0, NULL);
    if (c->unwind) {
        static const uint32_t places[] = {EXCEPT};
        rc_unwind_claim(c, ebp, SEH_FRAME, SEH_FRAME + 0x100, places, 1);
        return;
    }
    event("seh-done");
    rc_st32(ebp - 4, 0xffffffffu);
    rc_st32(c->fs_base, rc_ld32(ebp - 0x10));
    c->r[RC_ESP] = ebp;
    c->r[RC_EBP] = rc_pop32(c);
    c->r[RC_EAX] = 0;
    ret(c, 0);
}

static void raiser(rc_cpu *c)
{
    uint32_t a[4] = {0xe0000001u, 0, 0, 0};
    rc_call_guest(c, rc_import_address(K32, "RaiseException"), 4, a);
    if (c->unwind) {
        return;
    }
    event("resumed");
    ret(c, 0);
}

static void filter(rc_cpu *c)
{
    event("filter");
    uint32_t ptrs = rc_ld32(c->r[RC_EBP] - 0x14); /* GetExceptionInformation() */
    CHECK_EQ(rc_ld32(rc_ld32(ptrs)), 0xe0000001u);
    c->r[RC_EAX] = filter_result;
    ret(c, 0);
}

static void finally_block(rc_cpu *c)
{
    event("finally");
    ret(c, 0);
}

static void except_block(rc_cpu *c)
{
    event("except");
    uint32_t ebp = c->r[RC_EBP];
    CHECK_EQ(rc_ld32(ebp - 4), 0xffffffffu); /* the try level of the __except's __try */
    CHECK_EQ(c->r[RC_ESP], rc_ld32(ebp - 0x18));
    rc_st32(c->fs_base, rc_ld32(ebp - 0x10));
    c->r[RC_ESP] = ebp;
    c->r[RC_EBP] = rc_pop32(c);
    c->r[RC_EAX] = 0xe5;
    ret(c, 0);
}

static const rc_fn_entry lifted[] = {
    {OUTER, outer},
    {INNER, inner},
    {STUB_OUTER, stub_outer},
    {STUB_INNER, stub_inner},
    {CATCH, catch_block},
    {CATCH_OTHER, catch_other},
    {RETHROW, rethrow},
    {DTOR_A, dtor_a},
    {DTOR_B, dtor_b},
    {DTOR_C, dtor_c},
    {CONT, cont},
    {COPY, copy},
    {OBJ_DTOR, obj_dtor},
    {SEH_FRAME, seh_frame},
    {RAISER, raiser},
    {FILTER, filter},
    {FINALLY, finally_block},
    {EXCEPT, except_block},
    {OUTERMOST, outermost},
    {STUB_OUTERMOST, stub_outermost},
    {CATCH_ALL, catch_all},
    {CONT_OUTERMOST, cont_outermost},
};

/* Run `fn` on a fresh stack; its EAX. */
static uint32_t run(rc_cpu *c, uint32_t fn)
{
    events[0] = 0;
    catch_object = catch_state = 0;
    c->r[RC_ESP] = STACK_TOP;
    c->r[RC_EBP] = 0;
    uint32_t r = rc_call_guest(c, fn, 0, NULL);
    CHECK_EQ(c->r[RC_ESP], STACK_TOP);
    CHECK_EQ(rc_ld32(c->fs_base), CHAIN_END);
    CHECK_EQ(c->unwind, 0);
    CHECK_EQ(c->eh_depth, 0);
    return r;
}

#define CHECK_EVENTS(want)                                                                         \
    do {                                                                                           \
        if (strcmp(events, want) != 0) {                                                           \
            fprintf(stderr, "%s:%d: events \"%s\", want \"%s\"\n", __FILE__, __LINE__, events,     \
                    want);                                                                         \
            rc_test_failures++;                                                                    \
        }                                                                                          \
    } while (0)

int main(void)
{
    if (!rc_mem_reserve(UINT64_C(64) << 20) || !rc_mem_commit(0x00100000u, 0x00100000u) ||
        !rc_mem_commit(TEB, 0x1000) || !rc_mem_commit(DATA, 0x10000)) {
        fprintf(stderr, "no guest memory\n");
        return 1;
    }
    rc_hle_kernel32();
    rc_hle_msvcrt();
    rc_set_lifted(lifted, sizeof lifted / sizeof lifted[0]);
    rc_cpu c = {0};
    c.fs_base = TEB;
    rc_st32(TEB, CHAIN_END);

    /* Visual C++ 6's tables. TypeDescriptor {vftable, spare, name}; CatchableType {properties,
     * type, PMD {mdisp, pdisp, vdisp}, size, copy constructor}; ThrowInfo {attributes,
     * destructor, forward compat, catchable types}; HandlerType {adjectives, type, catch object
     * at ebp+, handler}; TryBlockMapEntry {low, high, catch high, catches, handlers};
     * UnwindMapEntry {to state, action}; FuncInfo {magic, states, unwind map, try blocks,
     * try block map, ip map entries, ip map}. */
    uint32_t td_thing = type_descriptor(".?AVThing@@");
    uint32_t td_base = type_descriptor(".?AVBase@@");
    uint32_t td_other = type_descriptor(".?AVOther@@");
    uint32_t ct_thing = PUT(0, td_thing, 0, 0xffffffffu, 0, 8, COPY);
    uint32_t ct_base = PUT(0, td_base, 4, 0xffffffffu, 0, 4, 0); /* Base at +4 in Thing */
    uint32_t ti_thing = PUT(0, OBJ_DTOR, 0, PUT(2, ct_thing, ct_base));
    uint32_t umap_inner = PUT(0xffffffffu, DTOR_C);
    funcinfo_inner = PUT(0x19930520u, 1, umap_inner, 0, 0, 0, 0);
    /* OUTER: state 0 A alive, 1 in the try block, 2 B alive (in it), 3 in the catch block. */
    uint32_t umap_outer = PUT(0xffffffffu, DTOR_A, 0, 0, 1, DTOR_B, 0, 0);
    const uint32_t HT_REFERENCE = 8;

    /* catch (Other &) passes; catch (Thing t) takes it by value, through the copy constructor. */
    throw_info = ti_thing;
    win32_code = 0;
    uint32_t by_value = PUT(HT_REFERENCE, td_other, 0xffffffd0u, CATCH_OTHER, 0, td_thing, 0xffffffe8u, CATCH);
    funcinfo_outer = PUT(0x19930520u, 4, umap_outer, 1, PUT(1, 2, 3, 2, by_value), 0, 0);
    CHECK_EQ(run(&c, OUTER), 0x600d);
    CHECK_EVENTS("copy ~C ~B catch ~Thing cont ");
    CHECK_EQ(catch_object, 0x11111112u); /* the copy constructor's work */
    CHECK_EQ(catch_state, 3);

    /* catch (Base &): the second catchable type, a reference adjusted to the base. */
    uint32_t by_base = PUT(HT_REFERENCE, td_base, 0xffffffe8u, CATCH);
    funcinfo_outer = PUT(0x19930520u, 4, umap_outer, 1, PUT(1, 2, 3, 1, by_base), 0, 0);
    CHECK_EQ(run(&c, OUTER), 0x600d);
    CHECK_EVENTS("~C ~B catch ~Thing cont ");
    CHECK_EQ(catch_object, thrown_obj + 4);

    /* catch (...) takes a Win32 exception too (Visual C++ 6). */
    win32_code = 0xc0000094u;
    uint32_t any = PUT(0, 0, 0, CATCH);
    funcinfo_outer = PUT(0x19930520u, 4, umap_outer, 1, PUT(1, 2, 3, 1, any), 0, 0);
    CHECK_EQ(run(&c, OUTER), 0x600d);
    CHECK_EVENTS("~C ~B catch cont ");

    /* catch (Base &) { throw; } in OUTER, caught again by catch (...) further out: OUTER is
     * unwound from its catch block (~A), and the object is destroyed once, by the last catch. */
    win32_code = 0;
    uint32_t base_rethrow = PUT(HT_REFERENCE, td_base, 0xffffffe8u, RETHROW);
    funcinfo_outer = PUT(0x19930520u, 4, umap_outer, 1, PUT(1, 2, 3, 1, base_rethrow), 0, 0);
    uint32_t any_outermost = PUT(0, 0, 0, CATCH_ALL);
    uint32_t umap_outermost = PUT(0xffffffffu, 0, 0xffffffffu, 0); /* 0 in the try block, 1 in the catch */
    funcinfo_outermost = PUT(0x19930520u, 2, umap_outermost, 1, PUT(0, 0, 1, 1, any_outermost), 0, 0);
    CHECK_EQ(run(&c, OUTERMOST), 0x0123);
    CHECK_EVENTS("~C ~B rethrow ~A catch-all ~Thing cont-outermost ");

    /* __try { __try { raise } __finally { } } __except (1) { }: the filter, the __finally on
     * the way out, then the __except block in its frame. */
    scope_table = PUT(0xffffffffu, FILTER, EXCEPT, 0, 0, FINALLY);
    filter_result = 1;
    CHECK_EQ(run(&c, SEH_FRAME), 0xe5);
    CHECK_EVENTS("filter finally except ");

    /* A filter of -1 (EXCEPTION_CONTINUE_EXECUTION) resumes after the raise. */
    filter_result = 0xffffffffu;
    CHECK_EQ(run(&c, SEH_FRAME), 0);
    CHECK_EVENTS("filter resumed seh-done ");

    if (rc_test_failures) {
        fprintf(stderr, "%d check(s) failed\n", rc_test_failures);
        return 1;
    }
    puts("hle tests passed");
    return 0;
}
