/* SPDX-License-Identifier: MIT */
/*
 * Miles' RIB (Run-time Interface Binding): how Miles plug-ins offer their functions. A plug-in
 * (an ASI codec such as Mp3dec.asi, lifted with the game) registers its interfaces from its
 * DllMain: RIB_provider_library_handle gives it its provider handle, RIB_register_interface a
 * table of entries (functions with their addresses, attributes with their numbers). The game
 * finds a provider by its attributes (RIB_find_files_provider: the codec whose "Input file types"
 * include *.mp3) and asks for the entries it wants by name (RIB_request_interface).
 *
 * An entry is 16 bytes: type (0 function, 1 attribute, 2 preference), name (a string), token
 * (registered: the function's address, or the attribute's number; requested: the address of the
 * variable that receives it), subtype. A provider answers questions
 * about its attributes through its own PROVIDER_query_attribute function, which is lifted code.
 */
#include <stdlib.h>
#include <string.h>

#include "mss32.h"
#include "rc/log.h"
#include "rc/process.h"

#define MSS "mss32.dll"
#define RIB_FUNCTION 0u
#define RIB_ATTRIBUTE 1u

/* Names compare without case, in ASCII (strcasecmp is POSIX, not on Windows). */
static int lower(int ch) { return ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch; }

static bool same_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower((unsigned char)a[i]) != lower((unsigned char)b[i])) {
            return false;
        }
        if (!a[i]) {
            return true;
        }
    }
    return true;
}

static bool same(const char *a, const char *b) { return same_n(a, b, (size_t)-1); }

#define ENTRIES 256
static struct rib_entry {
    uint32_t provider;
    char iface[48];
    uint32_t type;
    char name[48];
    uint32_t token, subtype;
} entries[ENTRIES];
static uint32_t nentries;

/* Provider handles: one per plug-in module, 1-based. */
#define PROVIDERS 16
static uint32_t provider_base[PROVIDERS];
static bool provider_started[PROVIDERS];
static uint32_t nproviders;

/* _RIB_provider_library_handle@0: the provider handle of the plug-in that calls (from its
 * DllMain), by the module its return address lies in. */
static void k_provider_library_handle(rc_cpu *c)
{
    const rc_module *m = rc_module_containing(rc_ld32(c->r[RC_ESP]));
    uint32_t base = m ? m->base : 0;
    for (uint32_t i = 0; i < nproviders; i++) {
        if (provider_base[i] == base) {
            RC_RETURN(c, i + 1, 0);
        }
    }
    if (nproviders == PROVIDERS) {
        RC_RETURN(c, 0, 0);
    }
    provider_base[nproviders++] = base;
    RC_RETURN(c, nproviders, 0);
}

/* RIB_register_interface(provider, interface, count, entries): stdcall, though its exported name
 * is undecorated (as the other RIB_*_interface functions). */
static void k_register_interface(rc_cpu *c)
{
    uint32_t prov = RC_ARG(c, 0), iface = RC_ARG(c, 1), n = RC_ARG(c, 2), list = RC_ARG(c, 3);
    const char *in = iface ? rc_str(iface) : "";
    for (uint32_t i = 0; i < n && nentries < ENTRIES; i++) {
        uint32_t e = list + 16 * i;
        struct rib_entry *r = &entries[nentries++];
        r->provider = prov;
        strncpy(r->iface, in, sizeof r->iface - 1);
        r->type = rc_ld32(e);
        uint32_t name = rc_ld32(e + 4);
        strncpy(r->name, name ? rc_str(name) : "", sizeof r->name - 1);
        r->token = rc_ld32(e + 8);
        r->subtype = rc_ld32(e + 12);
        if (getenv("RC_RIB_DUMP")) {
            rc_log("mss32:   %s %u \"%s\" token %#x subtype %u", r->iface, r->type, r->name, r->token, r->subtype);
        }
    }
    rc_log("mss32: provider %u registers \"%s\" (%u entries)", prov, in, n);
    RC_RETURN(c, 0, 4);
}

/* RIB_unregister_interface(provider, interface, count, entries): stdcall; everything of the
 * provider's interface goes (Miles does the same when the entries are NULL). */
static void k_unregister_interface(rc_cpu *c)
{
    uint32_t prov = RC_ARG(c, 0), iface = RC_ARG(c, 1);
    const char *in = iface ? rc_str(iface) : NULL;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < nentries; i++) {
        if (entries[i].provider == prov && (!in || same(entries[i].iface, in))) {
            continue;
        }
        entries[keep++] = entries[i];
    }
    nentries = keep;
    RC_RETURN(c, 0, 4);
}

static const struct rib_entry *find(uint32_t prov, const char *iface, uint32_t type, const char *name)
{
    for (uint32_t i = 0; i < nentries; i++) {
        const struct rib_entry *r = &entries[i];
        if (r->provider == prov && r->type == type && same(r->iface, iface) &&
            same(r->name, name)) {
            return r;
        }
    }
    return NULL;
}

/* Whether `value` matches one of the patterns in `list` ("*.mp3;*.mp2"), ignoring case. */
static bool matches(const char *list, const char *value)
{
    size_t vl = strlen(value);
    while (*list) {
        const char *end = strchr(list, ';');
        size_t n = end ? (size_t)(end - list) : strlen(list);
        while (n && list[0] == ' ') {
            list++;
            n--;
        }
        if (n == vl && same_n(list, value, n)) {
            return true;
        }
        /* "*.mp3" against a file name or a suffix */
        if (n > 1 && list[0] == '*' && vl >= n - 1 && same_n(value + vl - (n - 1), list + 1, n - 1)) {
            return true;
        }
        list += n;
        if (*list == ';') {
            list++;
        }
    }
    return false;
}

/* The guest address of attribute `name` of provider `prov`'s interface, or 0. */
static uint32_t attribute(rc_cpu *c, uint32_t prov, const char *iface, const char *name)
{
    const struct rib_entry *a = find(prov, iface, RIB_ATTRIBUTE, name);
    const struct rib_entry *q = find(prov, iface, RIB_FUNCTION, "PROVIDER_query_attribute");
    if (!a || !q) {
        return 0;
    }
    uint32_t arg = a->token;
    return rc_call_guest(c, q->token, 1, &arg);
}

/* Whether a file-type attribute matches `value`. Such an attribute is a list of strings ended by
 * an empty one, as a Windows file filter: a description, then its patterns ("MPEG Layer 3 audio
 * files", "*.mp3;*.mp2", ...); every string is tried. */
static bool attribute_matches(uint32_t v, const char *value)
{
    for (int i = 0; v && rc_ld8(v) && i < 32; i++) {
        const char *s = rc_str(v);
        if (matches(s, value)) {
            return true;
        }
        v += (uint32_t)strlen(s) + 1;
    }
    return false;
}

/* _RIB_find_files_provider@20(interface, attribute, value, attribute 2, value 2): the first
 * provider of the interface whose attributes match both values (the second may be NULL). */
static void k_find_files_provider(rc_cpu *c)
{
    uint32_t iface = RC_ARG(c, 0), a1 = RC_ARG(c, 1), v1 = RC_ARG(c, 2), a2 = RC_ARG(c, 3), v2 = RC_ARG(c, 4);
    if (!iface || !a1 || !v1) {
        RC_RETURN(c, 0, 5);
    }
    char in[48], n1[48], s1[64], n2[48] = "", s2[64] = "";
    strncpy(in, rc_str(iface), sizeof in - 1);
    in[sizeof in - 1] = 0;
    strncpy(n1, rc_str(a1), sizeof n1 - 1);
    n1[sizeof n1 - 1] = 0;
    strncpy(s1, rc_str(v1), sizeof s1 - 1);
    s1[sizeof s1 - 1] = 0;
    if (a2 && v2) {
        strncpy(n2, rc_str(a2), sizeof n2 - 1);
        n2[sizeof n2 - 1] = 0;
        strncpy(s2, rc_str(v2), sizeof s2 - 1);
        s2[sizeof s2 - 1] = 0;
    }
    for (uint32_t p = 1; p <= nproviders; p++) {
        if (!attribute_matches(attribute(c, p, in, n1), s1)) {
            continue;
        }
        if (n2[0] && !attribute_matches(attribute(c, p, in, n2), s2)) {
            continue;
        }
        /* Miles starts a codec before it is used (the program asks only for its streams). */
        const struct rib_entry *start = find(p, "ASI codec", RIB_FUNCTION, "ASI_startup");
        if (start && !provider_started[p - 1]) {
            provider_started[p - 1] = true;
            rc_call_guest(c, start->token, 0, NULL);
        }
        RC_RETURN(c, p, 5);
    }
    rc_log("mss32: no \"%s\" provider with %s = %s", in, n1, s1);
    RC_RETURN(c, 0, 5);
}

/* RIB_request_interface(provider, interface, count, entries): stdcall. In a request the token is
 * the address of the program's variable that receives the entry's value (a function's address,
 * an attribute's or preference's number). 0 when all were found, 1 (RIB_NOT_ALL_AVAILABLE)
 * otherwise; the variables of missing entries keep their values. */
static void k_request_interface(rc_cpu *c)
{
    uint32_t prov = RC_ARG(c, 0), iface = RC_ARG(c, 1), n = RC_ARG(c, 2), list = RC_ARG(c, 3);
    char in[48];
    strncpy(in, iface ? rc_str(iface) : "", sizeof in - 1);
    in[sizeof in - 1] = 0;
    uint32_t missing = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e = list + 16 * i, name = rc_ld32(e + 4);
        const struct rib_entry *r = name ? find(prov, in, rc_ld32(e), rc_str(name)) : NULL;
        uint32_t var = rc_ld32(e + 8);
        if (r && var) {
            rc_st32(var, r->token);
        } else {
            rc_log("mss32: provider %u has no \"%s\" (type %u) in \"%s\"", prov, name ? rc_str(name) : "?", rc_ld32(e), in);
            missing++;
        }
    }
    RC_RETURN(c, missing ? 1u : 0u, 4);
}

/* _AIL_MMX_available@0: as the processor cpuid describes, which has MMX. */
static void k_MMX_available(rc_cpu *c) { RC_RETURN(c, 1, 0); }

const rc_import_def rc_rib_defs[] = {
    {MSS, "_RIB_provider_library_handle@0", k_provider_library_handle},
    {MSS, "RIB_register_interface", k_register_interface},
    {MSS, "RIB_unregister_interface", k_unregister_interface},
    {MSS, "_RIB_find_files_provider@20", k_find_files_provider},
    {MSS, "RIB_request_interface", k_request_interface},
    {MSS, "_AIL_MMX_available@0", k_MMX_available},
};
const uint32_t rc_rib_count = sizeof rc_rib_defs / sizeof rc_rib_defs[0];
