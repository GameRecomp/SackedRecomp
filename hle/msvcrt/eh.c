/* SPDX-License-Identifier: MIT */
/*
 * MSVCRT: C++ exceptions, __try/__except, RTTI and the exception class, as Visual C++ 6 lays
 * them out (our own implementation of the published structures).
 *
 * A throw (_CxxThrowException) raises exception 0xE06D7363 with the object and its ThrowInfo;
 * kernel32's RaiseException asks the handlers on the guest's registration chain. A function
 * with try blocks or objects to destroy registers __CxxFrameHandler (through a stub that loads
 * its FuncInfo into EAX): its record sits at ebp-0Ch (next, handler, state at ebp-4), the stack
 * pointer is saved at ebp-10h. The handler looks for a try block around the current state with
 * a catch whose type the thrown object has, builds the catch object in the frame, unwinds the
 * frames above (RtlUnwind) and its own state down to the try block (calling the destructor
 * funclets of the unwind map), calls the catch funclet, destroys the exception object and
 * continues at the address the catch returned, in its frame: the runtime unwinds the lifted
 * functions there (c->unwind, rc_unwind_claim).
 *
 * __except_handler3 serves __try: its record sits at ebp-10h with the scope table and the try
 * level after it; filters and __finally blocks are funclets called with the frame's ebp.
 */
#include "msvcrt.h"

#include <stdio.h>
#include <string.h>

#include "rc/log.h"
#include "rc/mem.h"
#include "rc/process.h"
#include "rc/vm.h"

#define K32 "KERNEL32.dll"

#define MSC_EXCEPTION 0xe06d7363u
#define EH_MAGIC 0x19930520u
#define EXCEPTION_NONCONTINUABLE 0x1u
#define EXCEPTION_UNWINDING 0x2u
#define EXCEPTION_EXIT_UNWIND 0x4u
#define CONTINUE_EXECUTION 0u
#define CONTINUE_SEARCH 1u

/* HandlerType adjectives, CatchableType properties, ThrowInfo attributes. */
#define HT_IS_CONST 0x01u
#define HT_IS_VOLATILE 0x02u
#define HT_IS_REFERENCE 0x08u
#define CT_IS_SIMPLE 0x01u
#define CT_BY_REFERENCE_ONLY 0x02u
#define CT_HAS_VIRTUAL_BASE 0x04u
#define TI_IS_CONST 0x01u
#define TI_IS_VOLATILE 0x02u

/* The exceptions being handled (a catch block runs), innermost last: what `throw;` rethrows. */
#define DEPTH 16
static struct {
    uint32_t obj, info;
    bool rethrown;
} current[DEPTH];
static int depth;

static const char *type_name(uint32_t td) { return td ? rc_str(td + 8) : ""; }

/* ---- throwing ---- */

/* _CxxThrowException(object, ThrowInfo) (stdcall); both 0: rethrow the exception being handled. */
static void k__CxxThrowException(rc_cpu *c)
{
    uint32_t obj = RC_ARG(c, 0), info = RC_ARG(c, 1);
    if (!obj && !info) {
        if (!depth) {
            rc_fatal("throw; with no exception being handled");
        }
        obj = current[depth - 1].obj;
        info = current[depth - 1].info;
        current[depth - 1].rethrown = true;
    }
    uint32_t params = (c->r[RC_ESP] - 16) & ~3u;
    rc_st32(params, EH_MAGIC);
    rc_st32(params + 4, obj);
    rc_st32(params + 8, info);
    uint32_t esp = c->r[RC_ESP];
    c->r[RC_ESP] = params - 4;
    uint32_t a[4] = {MSC_EXCEPTION, EXCEPTION_NONCONTINUABLE, 3, params};
    rc_call_guest(c, rc_import_address(K32, "RaiseException"), 4, a);
    if (c->unwind) {
        return; /* a catch continues elsewhere */
    }
    c->r[RC_ESP] = esp;
    rc_fatal("a C++ exception was continued");
}

/* ---- catching ---- */

/* `p` adjusted to a base class by a PMD {mdisp, pdisp, vdisp}. */
static uint32_t adjust(uint32_t p, uint32_t pmd)
{
    uint32_t r = p + rc_ld32(pmd);
    int32_t pdisp = (int32_t)rc_ld32(pmd + 4);
    if (pdisp >= 0) {
        r += (uint32_t)pdisp + rc_ld32(rc_ld32(p + (uint32_t)pdisp) + rc_ld32(pmd + 8));
    }
    return r;
}

/* Whether catch handler `ht` takes catchable type `ct` of a throw described by `info`. */
static bool type_match(uint32_t ht, uint32_t ct, uint32_t info)
{
    uint32_t adj = rc_ld32(ht), td = rc_ld32(ht + 4);
    if (!td || !type_name(td)[0]) {
        return true; /* catch (...) */
    }
    uint32_t ctd = rc_ld32(ct + 4);
    if (td != ctd && strcmp(type_name(td), type_name(ctd)) != 0) {
        return false;
    }
    uint32_t props = rc_ld32(ct), attr = rc_ld32(info);
    if ((props & CT_BY_REFERENCE_ONLY) && !(adj & HT_IS_REFERENCE)) {
        return false;
    }
    if ((attr & TI_IS_CONST) && !(adj & HT_IS_CONST)) {
        return false;
    }
    if ((attr & TI_IS_VOLATILE) && !(adj & HT_IS_VOLATILE)) {
        return false;
    }
    return true;
}

/* The catch object in the frame (at ebp + its displacement): a reference, a copy of a simple
 * type, or an object made by its copy constructor. */
static void build_catch_object(rc_cpu *c, uint32_t ht, uint32_t ct, uint32_t obj, uint32_t ebp)
{
    uint32_t adj = rc_ld32(ht), td = rc_ld32(ht + 4);
    int32_t disp = (int32_t)rc_ld32(ht + 8);
    if (!td || !type_name(td)[0] || !disp || !ct) {
        return;
    }
    uint32_t dest = ebp + (uint32_t)disp, props = rc_ld32(ct), size = rc_ld32(ct + 20), copy = rc_ld32(ct + 24);
    if (adj & HT_IS_REFERENCE) {
        rc_st32(dest, adjust(obj, ct + 8));
    } else if (props & CT_IS_SIMPLE) {
        memmove(rc_ptr(dest), rc_ptr(obj), size);
        if (size == 4 && rc_ld32(dest)) {
            rc_st32(dest, adjust(rc_ld32(dest), ct + 8));
        }
    } else {
        uint32_t src = adjust(obj, ct + 8);
        if (!copy) {
            memmove(rc_ptr(dest), rc_ptr(src), size);
        } else if (props & CT_HAS_VIRTUAL_BASE) {
            uint32_t a[2] = {src, 1};
            rc_crt_call_this(c, copy, dest, 2, a);
        } else {
            uint32_t a[1] = {src};
            rc_crt_call_this(c, copy, dest, 1, a);
        }
    }
}

/* Unwind the frame whose record is `rn` to state `target`: the destructor funclets of the states
 * on the way, each with its next state set first. */
static void unwind_to_state(rc_cpu *c, uint32_t rn, uint32_t fi, int32_t target)
{
    int32_t cur = (int32_t)rc_ld32(rn + 8);
    int32_t max = (int32_t)rc_ld32(fi + 4);
    uint32_t map = rc_ld32(fi + 8);
    while (cur != target) {
        if (cur < 0 || cur >= max) {
            rc_fatal("C++ unwinding: state %d out of range (%d states)", cur, max);
        }
        int32_t to = (int32_t)rc_ld32(map + 8u * (uint32_t)cur);
        uint32_t action = rc_ld32(map + 8u * (uint32_t)cur + 4);
        rc_st32(rn + 8, (uint32_t)to);
        if (action) {
            rc_crt_call_funclet(c, action, rn + 0xc);
            if (c->unwind) {
                return;
            }
        }
        cur = to;
    }
    rc_st32(rn + 8, (uint32_t)target);
}

/* Continue at `eip` in the frame whose ebp is `ebp` and stack pointer `esp`: the lifted
 * functions return until that frame claims the place. */
static void continue_at(rc_cpu *c, uint32_t eip, uint32_t ebp, uint32_t esp, uint32_t rn)
{
    c->r[RC_ESP] = esp;
    c->r[RC_EBP] = ebp;
    rc_st32(c->fs_base, rn);
    c->unwind = 1;
    c->unwind_eip = eip;
    c->unwind_ebp = ebp;
    c->eip = eip;
}

/* The catch: catch object, the frames above unwound, this frame down to the try block, the
 * catch block, the exception object destroyed, and on to the continuation. */
static void catch_it(rc_cpu *c, uint32_t rec, uint32_t rn, uint32_t fi, uint32_t tb, uint32_t ht, uint32_t ct,
                     uint32_t obj, uint32_t info)
{
    uint32_t ebp = rn + 0xc;
    build_catch_object(c, ht, ct, obj, ebp);
    if (c->unwind) {
        return;
    }
    uint32_t a[4] = {rn, 0, rec, 0};
    rc_crt_call(c, rc_import_address(K32, "RtlUnwind"), 4, a);
    if (c->unwind) {
        return;
    }
    unwind_to_state(c, rn, fi, (int32_t)rc_ld32(tb));
    if (c->unwind) {
        return;
    }
    rc_st32(rn + 8, rc_ld32(tb + 4) + 1);
    if (depth == DEPTH) {
        rc_fatal("C++ exceptions nested too deep");
    }
    current[depth].obj = obj;
    current[depth].info = info;
    current[depth].rethrown = false;
    depth++;
    uint32_t cont = rc_crt_call_funclet(c, rc_ld32(ht + 12), ebp);
    depth--;
    if (c->unwind) {
        return; /* the catch block threw, and that went elsewhere */
    }
    if (!current[depth].rethrown && obj && info && rc_ld32(info + 4)) {
        rc_crt_call_this(c, rc_ld32(info + 4), obj, 0, NULL); /* the exception object's destructor */
    }
    continue_at(c, cont, ebp, rc_ld32(rn - 4), rn);
}

/* __CxxFrameHandler(record, frame, context, dispatcher context) (cdecl), FuncInfo in EAX. */
static void k___CxxFrameHandler(rc_cpu *c)
{
    uint32_t fi = c->r[RC_EAX];
    uint32_t rec = RC_ARG(c, 0), rn = RC_ARG(c, 1);
    uint32_t magic = rc_ld32(fi);
    if (magic < EH_MAGIC || magic > EH_MAGIC + 2) {
        rc_fatal("__CxxFrameHandler: FuncInfo %#x has magic %#x", fi, magic);
    }
    if (rc_ld32(rec + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        if (rc_ld32(fi + 4) && (int32_t)rc_ld32(rn + 8) >= 0) {
            unwind_to_state(c, rn, fi, -1);
            if (c->unwind) {
                return;
            }
        }
        RC_RETURN(c, CONTINUE_SEARCH, 0);
    }
    uint32_t ntry = rc_ld32(fi + 12), tmap = rc_ld32(fi + 16);
    if (!ntry) {
        RC_RETURN(c, CONTINUE_SEARCH, 0);
    }
    bool msc = rc_ld32(rec) == MSC_EXCEPTION && rc_ld32(rec + 16) >= 3 && rc_ld32(rec + 20) == EH_MAGIC;
    uint32_t obj = msc ? rc_ld32(rec + 24) : 0, info = msc ? rc_ld32(rec + 28) : 0;
    int32_t state = (int32_t)rc_ld32(rn + 8);
    for (uint32_t i = 0; i < ntry; i++) {
        uint32_t tb = tmap + 20 * i;
        if (state < (int32_t)rc_ld32(tb) || state > (int32_t)rc_ld32(tb + 4)) {
            continue;
        }
        uint32_t ncatch = rc_ld32(tb + 12), handlers = rc_ld32(tb + 16);
        for (uint32_t j = 0; j < ncatch; j++) {
            uint32_t ht = handlers + 16 * j;
            uint32_t td = rc_ld32(ht + 4);
            if (!msc) {
                /* Visual C++ 6's catch (...) takes Win32 exceptions too. */
                if (!td || !type_name(td)[0]) {
                    catch_it(c, rec, rn, fi, tb, ht, 0, 0, 0);
                    return;
                }
                continue;
            }
            uint32_t types = rc_ld32(info + 12);
            uint32_t ntypes = types ? rc_ld32(types) : 0;
            for (uint32_t k = 0; k < ntypes; k++) {
                uint32_t ct = rc_ld32(types + 4 + 4 * k);
                if (type_match(ht, ct, info)) {
                    catch_it(c, rec, rn, fi, tb, ht, ct, obj, info);
                    return;
                }
            }
        }
    }
    RC_RETURN(c, CONTINUE_SEARCH, 0);
}

/* ---- __try ---- */

/* The __finally blocks of the frame (record `rn`) from its try level down to `stop`. */
static void local_unwind2(rc_cpu *c, uint32_t rn, uint32_t stop)
{
    uint32_t scope = rc_ld32(rn + 8);
    for (;;) {
        uint32_t level = rc_ld32(rn + 12);
        if (level == stop || level == 0xffffffffu) {
            return;
        }
        uint32_t e = scope + 12 * level;
        rc_st32(rn + 12, rc_ld32(e));
        if (!rc_ld32(e + 4) && rc_ld32(e + 8)) {
            rc_crt_call_funclet(c, rc_ld32(e + 8), rn + 0x10);
            if (c->unwind) {
                return;
            }
        }
    }
}

/* _except_handler3(record, frame, context, dispatcher context) (cdecl). */
static void k__except_handler3(rc_cpu *c)
{
    uint32_t rec = RC_ARG(c, 0), rn = RC_ARG(c, 1), ctx = RC_ARG(c, 2);
    uint32_t ebp = rn + 0x10, scope = rc_ld32(rn + 8);
    if (rc_ld32(rec + 4) & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND)) {
        local_unwind2(c, rn, 0xffffffffu);
        if (c->unwind) {
            return;
        }
        RC_RETURN(c, CONTINUE_SEARCH, 0);
    }
    /* EXCEPTION_POINTERS for GetExceptionInformation, where the frame keeps them. */
    uint32_t esp = c->r[RC_ESP];
    uint32_t ptrs = (esp - 16) & ~3u;
    rc_st32(ptrs, rec);
    rc_st32(ptrs + 4, ctx);
    rc_st32(rn - 4, ptrs);
    c->r[RC_ESP] = ptrs - 4;
    for (uint32_t level = rc_ld32(rn + 12); level != 0xffffffffu;) {
        uint32_t e = scope + 12 * level;
        uint32_t prev = rc_ld32(e), filter = rc_ld32(e + 4);
        if (filter) {
            int32_t r = (int32_t)rc_crt_call_funclet(c, filter, ebp);
            if (c->unwind) {
                return;
            }
            if (r < 0) {
                c->r[RC_ESP] = esp;
                RC_RETURN(c, CONTINUE_EXECUTION, 0);
            }
            if (r > 0) {
                uint32_t a[4] = {rn, 0, rec, 0};
                rc_crt_call(c, rc_import_address(K32, "RtlUnwind"), 4, a);
                if (c->unwind) {
                    return;
                }
                local_unwind2(c, rn, level);
                if (c->unwind) {
                    return;
                }
                rc_st32(rn + 12, prev);
                continue_at(c, rc_ld32(e + 8), ebp, rc_ld32(rn - 8), rn);
                return;
            }
        }
        level = prev;
    }
    c->r[RC_ESP] = esp;
    RC_RETURN(c, CONTINUE_SEARCH, 0);
}

/* _XcptFilter(code, pointers): the C runtime's default filter: nothing to do, search on. */
static void k__XcptFilter(rc_cpu *c) { RC_RETURN(c, 0, 0); }

static void k_terminate(rc_cpu *c)
{
    (void)c;
    rc_fatal("terminate() called (an exception nobody catches, or one thrown during unwinding)");
}

static void k__purecall(rc_cpu *c)
{
    rc_fatal("pure virtual function call from %#010x", rc_ld32(c->r[RC_ESP]));
}

/* ---- RTTI ---- */

/* __RTDynamicCast(object, vfptr offset, source type, target type, is reference): the target
 * type's base class instance in the complete object, or 0 (a bad reference cast is fatal). */
static void k___RTDynamicCast(rc_cpu *c)
{
    uint32_t p = RC_ARG(c, 0), vfdelta = RC_ARG(c, 1), target = RC_ARG(c, 3), isref = RC_ARG(c, 4);
    if (!p) {
        RC_RETURN(c, 0, 0);
    }
    uint32_t col = rc_ld32(rc_ld32(p + vfdelta) - 4); /* the complete object locator */
    uint32_t complete = p + vfdelta - rc_ld32(col + 4);
    uint32_t chd = rc_ld32(col + 16);
    uint32_t n = rc_ld32(chd + 8), bases = rc_ld32(chd + 12);
    const char *want = type_name(target);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t bcd = rc_ld32(bases + 4 * i);
        uint32_t td = rc_ld32(bcd);
        if (td == target || strcmp(type_name(td), want) == 0) {
            RC_RETURN(c, adjust(complete, bcd + 8), 0);
        }
    }
    if (isref) {
        rc_fatal("dynamic_cast to %s& failed (std::bad_cast)", want);
    }
    RC_RETURN(c, 0, 0);
}

/* type_info::~type_info() (thiscall) */
static void k_type_info_dtor(rc_cpu *c) { RC_RETURN(c, 0, 0); }

/* type_info::operator!=(const type_info&) (thiscall) */
static void k_type_info_ne(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX], other = RC_ARG(c, 0);
    RC_RETURN(c, strcmp(type_name(self), type_name(other)) != 0, 1);
}

/* ---- class exception: { vftable, what, owns what } ---- */

static uint32_t exception_vftable;

static uint32_t vftable(void)
{
    if (!exception_vftable) {
        /* [-1] no locator, [0] the deleting destructor, [1] what */
        uint32_t t = rc_heap_alloc(12, true);
        rc_st32(t + 4, rc_import_address(MSVCRT, "??_Gexception@@UAEPAXI@Z"));
        rc_st32(t + 8, rc_import_address(MSVCRT, "?what@exception@@UBEPBDXZ"));
        exception_vftable = t + 4;
    }
    return exception_vftable;
}

static uint32_t copy_string(uint32_t s)
{
    uint32_t n = (uint32_t)strlen(rc_str(s)) + 1;
    uint32_t p = rc_crt_malloc(n);
    memcpy(rc_ptr(p), rc_ptr(s), n);
    return p;
}

static void exception_set(uint32_t self, uint32_t what, bool own)
{
    rc_st32(self, vftable());
    rc_st32(self + 4, what && own ? copy_string(what) : what);
    rc_st32(self + 8, what && own ? 1 : 0);
}

static void exception_clear(uint32_t self)
{
    if (rc_ld32(self + 8)) {
        rc_crt_free(rc_ld32(self + 4));
    }
    rc_st32(self + 4, 0);
    rc_st32(self + 8, 0);
}

/* exception::exception() */
static void k_exception_ctor(rc_cpu *c)
{
    exception_set(c->r[RC_ECX], 0, false);
    RC_RETURN(c, c->r[RC_ECX], 0);
}

/* exception::exception(const char *const &) */
static void k_exception_ctor_str(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX];
    exception_set(self, rc_ld32(RC_ARG(c, 0)), true);
    RC_RETURN(c, self, 1);
}

/* exception::exception(const exception &) */
static void k_exception_copy(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX], other = RC_ARG(c, 0);
    exception_set(self, rc_ld32(other + 4), rc_ld32(other + 8) != 0);
    RC_RETURN(c, self, 1);
}

/* exception &exception::operator=(const exception &) */
static void k_exception_assign(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX], other = RC_ARG(c, 0);
    if (self != other) {
        exception_clear(self);
        rc_st32(self + 4, rc_ld32(other + 8) ? copy_string(rc_ld32(other + 4)) : rc_ld32(other + 4));
        rc_st32(self + 8, rc_ld32(other + 8));
    }
    RC_RETURN(c, self, 1);
}

/* exception::~exception() */
static void k_exception_dtor(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX];
    rc_st32(self, vftable());
    exception_clear(self);
    RC_RETURN(c, 0, 0);
}

/* The scalar deleting destructor: flags bit 0 frees the object. */
static void k_exception_deleting_dtor(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX];
    exception_clear(self);
    if (RC_ARG(c, 0) & 1) {
        rc_crt_free(self);
    }
    RC_RETURN(c, self, 1);
}

/* const char *exception::what() const */
static void k_exception_what(rc_cpu *c)
{
    static uint32_t unknown;
    uint32_t what = rc_ld32(c->r[RC_ECX] + 4);
    if (!what) {
        if (!unknown) {
            unknown = copy_string(rc_proc_str("Unknown exception"));
        }
        what = unknown;
    }
    RC_RETURN(c, what, 0);
}

const rc_import_def rc_msvcrt_eh[] = {
    {MSVCRT, "_CxxThrowException", k__CxxThrowException},
    {MSVCRT, "__CxxFrameHandler", k___CxxFrameHandler},
    {MSVCRT, "_except_handler3", k__except_handler3},
    {MSVCRT, "_XcptFilter", k__XcptFilter},
    {MSVCRT, "?terminate@@YAXXZ", k_terminate},
    {MSVCRT, "_purecall", k__purecall},
    {MSVCRT, "__RTDynamicCast", k___RTDynamicCast},
    {MSVCRT, "??1type_info@@UAE@XZ", k_type_info_dtor},
    {MSVCRT, "??9type_info@@QBEHABV0@@Z", k_type_info_ne},
    {MSVCRT, "??0exception@@QAE@XZ", k_exception_ctor},
    {MSVCRT, "??0exception@@QAE@ABQBD@Z", k_exception_ctor_str},
    {MSVCRT, "??0exception@@QAE@ABV0@@Z", k_exception_copy},
    {MSVCRT, "??4exception@@QAEAAV0@ABV0@@Z", k_exception_assign},
    {MSVCRT, "??1exception@@UAE@XZ", k_exception_dtor},
    {MSVCRT, "??_Gexception@@UAEPAXI@Z", k_exception_deleting_dtor},
    {MSVCRT, "?what@exception@@UBEPBDXZ", k_exception_what},
};
const uint32_t rc_msvcrt_eh_count = sizeof rc_msvcrt_eh / sizeof rc_msvcrt_eh[0];

void rc_msvcrt_eh_init(void) { rc_register_imports(rc_msvcrt_eh, rc_msvcrt_eh_count); }

/* ---- MSVCP60: std::bad_alloc, which derives from exception ---- */

static uint32_t bad_alloc_vftable;

static uint32_t bad_alloc_vt(void)
{
    if (!bad_alloc_vftable) {
        uint32_t t = rc_heap_alloc(12, true);
        rc_st32(t + 4, rc_import_address(MSVCP60, "??_Gbad_alloc@std@@UAEPAXI@Z"));
        rc_st32(t + 8, rc_import_address(MSVCRT, "?what@exception@@UBEPBDXZ"));
        bad_alloc_vftable = t + 4;
    }
    return bad_alloc_vftable;
}

/* bad_alloc::bad_alloc(const char *) */
static void k_bad_alloc_ctor(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX];
    exception_set(self, RC_ARG(c, 0), false);
    rc_st32(self, bad_alloc_vt());
    RC_RETURN(c, self, 1);
}

/* bad_alloc::bad_alloc(const bad_alloc &) */
static void k_bad_alloc_copy(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX], other = RC_ARG(c, 0);
    exception_set(self, rc_ld32(other + 4), rc_ld32(other + 8) != 0);
    rc_st32(self, bad_alloc_vt());
    RC_RETURN(c, self, 1);
}

static void k_bad_alloc_dtor(rc_cpu *c)
{
    exception_clear(c->r[RC_ECX]);
    RC_RETURN(c, 0, 0);
}

static void k_bad_alloc_deleting_dtor(rc_cpu *c)
{
    uint32_t self = c->r[RC_ECX];
    exception_clear(self);
    if (RC_ARG(c, 0) & 1) {
        rc_crt_free(self);
    }
    RC_RETURN(c, self, 1);
}

/* std::_Xlen() and std::_Xran(): the string is too long, a position out of range. They throw
 * length_error and out_of_range; nothing in the supported games catches them. */
static void k_xlen(rc_cpu *c)
{
    (void)c;
    rc_fatal("std::_Xlen: string too long (std::length_error)");
}

static void k_xran(rc_cpu *c)
{
    (void)c;
    rc_fatal("std::_Xran: position out of range (std::out_of_range)");
}

const rc_import_def rc_msvcp60_defs[] = {
    {MSVCP60, "??0bad_alloc@std@@QAE@PBD@Z", k_bad_alloc_ctor},
    {MSVCP60, "??0bad_alloc@std@@QAE@ABV01@@Z", k_bad_alloc_copy},
    {MSVCP60, "??1bad_alloc@std@@UAE@XZ", k_bad_alloc_dtor},
    {MSVCP60, "??_Gbad_alloc@std@@UAEPAXI@Z", k_bad_alloc_deleting_dtor},
    {MSVCP60, "?_Xlen@std@@YAXXZ", k_xlen},
    {MSVCP60, "?_Xran@std@@YAXXZ", k_xran},
};
const uint32_t rc_msvcp60_count = sizeof rc_msvcp60_defs / sizeof rc_msvcp60_defs[0];
