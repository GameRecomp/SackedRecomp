/* SPDX-License-Identifier: MIT */
/* Exception unwinding: a handler's jump to an outer frame, and the frame that takes it over. */
#include "rc/dispatch.h"
#include "test.h"

static int continued;

static void continuation(rc_cpu *c)
{
    continued++;
    c->r[RC_EAX] = 42;
}

static const rc_fn_entry table[] = {{0x00401234u, continuation}};

void test_unwind(void)
{
    rc_cpu c = {0};
    rc_set_lifted(table, 1);

    /* No exception in flight: an indirect jump is an ordinary transfer. */
    c.r[RC_ESP] = 0x2000;
    rc_dispatch_jump(&c, 0x00401234u);
    CHECK_EQ(continued, 1);
    CHECK_EQ(c.unwind, 0);

    /* A handler of an exception raised with esp 0x1000 jumps with the stack cut back to 0x2000:
     * the unwinding starts and nothing runs yet. */
    c.eh_depth = 1;
    c.eh_esp = 0x1000;
    c.r[RC_EBP] = 0x2010;
    rc_dispatch_jump(&c, 0x00401234u);
    CHECK_EQ(continued, 1);
    CHECK_EQ(c.unwind, 1);
    CHECK_EQ(c.unwind_eip, 0x00401234u);
    CHECK_EQ(c.unwind_ebp, 0x2010);
    c.eh_depth = 0;

    /* A frame with another ebp, or code elsewhere, passes it on. */
    rc_unwind_claim(&c, 0x2020, 0x00401000u, 0x00402000u, 0, 0);
    rc_unwind_claim(&c, 0x2010, 0x00402000u, 0x00403000u, 0, 0);
    CHECK_EQ(c.unwind, 1);
    CHECK_EQ(continued, 1);

    /* The frame that holds the place continues there (here: a place outside its code). */
    static const uint32_t at[] = {0x00405000u, 0x00401234u};
    rc_unwind_claim(&c, 0x2010, 0x00402000u, 0x00403000u, at, 2);
    CHECK_EQ(c.unwind, 0);
    CHECK_EQ(continued, 2);
    CHECK_EQ(c.r[RC_EAX], 42);
}
