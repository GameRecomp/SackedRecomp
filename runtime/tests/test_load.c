/* SPDX-License-Identifier: MIT */
/* What this project adds to the shared runtime: several lifted tables, and SHA-256. */
#include <string.h>

#include "rc/dispatch.h"
#include "rc/sha256.h"
#include "test.h"

static void fa(rc_cpu *c) { c->r[RC_EAX] = 0xa; }
static void fb(rc_cpu *c) { c->r[RC_EAX] = 0xb; }

static void test_tables(void)
{
    static const rc_fn_entry exe[] = {{0x401000, fa}, {0x401010, fb}, {0x402000, fa}};
    static const rc_fn_entry dll[] = {{0x10001000, fb}, {0x11001000, fa}};
    rc_set_lifted(exe, 3);
    CHECK(rc_lookup(0x401010) == fb);
    CHECK(rc_lookup(0x10001000) == NULL);
    CHECK(rc_add_lifted(dll, 2));
    CHECK(rc_lookup(0x401000) == fa);
    CHECK(rc_lookup(0x402000) == fa);
    CHECK(rc_lookup(0x10001000) == fb);
    CHECK(rc_lookup(0x11001000) == fa);
    CHECK(rc_lookup(0x401008) == NULL);
    CHECK(rc_lookup(0x20000000) == NULL);
    /* rc_set_lifted starts over with one table. */
    rc_set_lifted(dll, 2);
    CHECK(rc_lookup(0x401000) == NULL);
    CHECK(rc_lookup(0x11001000) == fa);
    rc_set_lifted(NULL, 0);
}

static void test_sha256(void)
{
    char h[65];
    rc_sha256_hex((const uint8_t *)"", 0, h);
    CHECK(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);
    rc_sha256_hex((const uint8_t *)"abc", 3, h);
    CHECK(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);
    /* Two blocks of padding (56 bytes of input). */
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    rc_sha256_hex((const uint8_t *)m, strlen(m), h);
    CHECK(strcmp(h, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0);
}

void test_load(void)
{
    test_tables();
    test_sha256();
}
