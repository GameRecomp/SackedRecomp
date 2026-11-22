/* SPDX-License-Identifier: MIT */
/*
 * gdi32: regions as lists of rectangles that do not overlap, the region objects, and the clip
 * region of a DC (SelectClipRgn, IntersectClipRect, ...). A region that would need more
 * rectangles than it has room for keeps the ones it has (it draws a little less).
 */
#include <string.h>

#include "dc.h"
#include "gdi32.h"
#include "objects.h"
#include "rc/hle.h"
#include "rc/vm.h"

#define G32 "GDI32.dll"

bool rc_rect_empty(rc_rect r) { return r.l >= r.r || r.t >= r.b; }

rc_rect rc_rect_and(rc_rect a, rc_rect b)
{
    rc_rect r = {a.l > b.l ? a.l : b.l, a.t > b.t ? a.t : b.t, a.r < b.r ? a.r : b.r, a.b < b.b ? a.b : b.b};
    return r;
}

void rc_rgn_empty(rc_rgn *g) { g->n = 0; }

void rc_rgn_set(rc_rgn *g, int32_t l, int32_t t, int32_t r, int32_t b)
{
    g->n = 0;
    rc_rect x = {l, t, r, b};
    if (!rc_rect_empty(x)) {
        g->r[g->n++] = x;
    }
}

void rc_rgn_and_rect(rc_rgn *g, rc_rect r)
{
    int k = 0;
    for (int i = 0; i < g->n; i++) {
        rc_rect x = rc_rect_and(g->r[i], r);
        if (!rc_rect_empty(x)) {
            g->r[k++] = x;
        }
    }
    g->n = k;
}

static void push(rc_rgn *g, rc_rect r)
{
    if (!rc_rect_empty(r) && g->n < RC_RGN_RECTS) {
        g->r[g->n++] = r;
    }
}

void rc_rgn_cut_rect(rc_rgn *g, rc_rect c)
{
    rc_rgn out;
    out.n = 0;
    for (int i = 0; i < g->n; i++) {
        rc_rect a = g->r[i], x = rc_rect_and(a, c);
        if (rc_rect_empty(x)) {
            push(&out, a);
            continue;
        }
        push(&out, (rc_rect){a.l, a.t, a.r, x.t});   /* above */
        push(&out, (rc_rect){a.l, x.b, a.r, a.b});   /* below */
        push(&out, (rc_rect){a.l, x.t, x.l, x.b});   /* left */
        push(&out, (rc_rect){x.r, x.t, a.r, x.b});   /* right */
    }
    *g = out;
}

void rc_rgn_add_rect(rc_rgn *g, rc_rect r)
{
    if (rc_rect_empty(r)) {
        return;
    }
    /* The new rectangle without what is there already. */
    rc_rgn add;
    rc_rgn_set(&add, r.l, r.t, r.r, r.b);
    for (int i = 0; i < g->n; i++) {
        rc_rgn_cut_rect(&add, g->r[i]);
    }
    for (int i = 0; i < add.n; i++) {
        if (g->n == RC_RGN_RECTS) {
            /* Full: the bounding box of everything (draws a little more). */
            rc_rect b = rc_rgn_box(g);
            rc_rect a = rc_rgn_box(&add);
            rc_rgn_set(g, a.l < b.l ? a.l : b.l, a.t < b.t ? a.t : b.t, a.r > b.r ? a.r : b.r, a.b > b.b ? a.b : b.b);
            return;
        }
        g->r[g->n++] = add.r[i];
    }
}

void rc_rgn_and(rc_rgn *g, const rc_rgn *h)
{
    rc_rgn out;
    out.n = 0;
    for (int i = 0; i < g->n; i++) {
        for (int j = 0; j < h->n; j++) {
            push(&out, rc_rect_and(g->r[i], h->r[j]));
        }
    }
    *g = out;
}

void rc_rgn_cut(rc_rgn *g, const rc_rgn *h)
{
    for (int j = 0; j < h->n; j++) {
        rc_rgn_cut_rect(g, h->r[j]);
    }
}

void rc_rgn_add(rc_rgn *g, const rc_rgn *h)
{
    for (int j = 0; j < h->n; j++) {
        rc_rgn_add_rect(g, h->r[j]);
    }
}

void rc_rgn_offset(rc_rgn *g, int32_t dx, int32_t dy)
{
    for (int i = 0; i < g->n; i++) {
        g->r[i].l += dx;
        g->r[i].r += dx;
        g->r[i].t += dy;
        g->r[i].b += dy;
    }
}

rc_rect rc_rgn_box(const rc_rgn *g)
{
    if (g->n <= 0) {
        return (rc_rect){0, 0, 0, 0};
    }
    rc_rect b = g->r[0];
    for (int i = 1; i < g->n; i++) {
        const rc_rect *r = &g->r[i];
        b.l = r->l < b.l ? r->l : b.l;
        b.t = r->t < b.t ? r->t : b.t;
        b.r = r->r > b.r ? r->r : b.r;
        b.b = r->b > b.b ? r->b : b.b;
    }
    return b;
}

bool rc_rgn_has(const rc_rgn *g, int32_t x, int32_t y)
{
    for (int i = 0; i < g->n; i++) {
        const rc_rect *r = &g->r[i];
        if (x >= r->l && x < r->r && y >= r->t && y < r->b) {
            return true;
        }
    }
    return false;
}

/* ---- region objects ---- */

#define NULLREGION 1u
#define SIMPLEREGION 2u
#define COMPLEXREGION 3u
#define RGN_ERROR 0u

static uint32_t kind_of(const rc_rgn *g) { return g->n == 0 ? NULLREGION : g->n == 1 ? SIMPLEREGION : COMPLEXREGION; }

static rc_rect get_rect(uint32_t p)
{
    return (rc_rect){(int32_t)rc_ld32(p), (int32_t)rc_ld32(p + 4), (int32_t)rc_ld32(p + 8), (int32_t)rc_ld32(p + 12)};
}

static void put_rect(uint32_t p, rc_rect r)
{
    rc_st32(p, (uint32_t)r.l);
    rc_st32(p + 4, (uint32_t)r.t);
    rc_st32(p + 8, (uint32_t)r.r);
    rc_st32(p + 12, (uint32_t)r.b);
}

static uint32_t new_region(rc_rect r)
{
    uint32_t h = rc_gobj_new(OB_REGION);
    gobj *o = rc_gobj_get(h, OB_REGION);
    rc_rgn_set(&o->rgn, r.l, r.t, r.r, r.b);
    return h;
}

static void k_CreateRectRgn(rc_cpu *c)
{
    rc_rect r = {(int32_t)RC_ARG(c, 0), (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3)};
    RC_RETURN(c, new_region(r), 4);
}

static void k_CreateRectRgnIndirect(rc_cpu *c) { RC_RETURN(c, new_region(get_rect(RC_ARG(c, 0))), 1); }

static void k_SetRectRgn(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    if (o) {
        rc_rgn_set(&o->rgn, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3), (int32_t)RC_ARG(c, 4));
    }
    RC_RETURN(c, o != NULL, 5);
}

/* CombineRgn(dest, a, b, mode): RGN_AND 1, OR 2, XOR 3, DIFF 4, COPY 5. */
static void k_CombineRgn(rc_cpu *c)
{
    gobj *d = rc_gobj_get(RC_ARG(c, 0), OB_REGION), *a = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    gobj *b = rc_gobj_get(RC_ARG(c, 2), OB_REGION);
    uint32_t mode = RC_ARG(c, 3);
    if (!d || !a || (mode != 5 && !b)) {
        RC_RETURN(c, RGN_ERROR, 4);
    }
    rc_rgn r = a->rgn;
    switch (mode) {
    case 1: rc_rgn_and(&r, &b->rgn); break;
    case 2: rc_rgn_add(&r, &b->rgn); break;
    case 3: {
        rc_rgn both = a->rgn, other = b->rgn;
        rc_rgn_and(&both, &b->rgn);
        rc_rgn_add(&r, &other);
        rc_rgn_cut(&r, &both);
        break;
    }
    case 4: rc_rgn_cut(&r, &b->rgn); break;
    default: break;
    }
    d->rgn = r;
    RC_RETURN(c, kind_of(&r), 4);
}

static void k_OffsetRgn(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    if (!o) {
        RC_RETURN(c, RGN_ERROR, 3);
    }
    rc_rgn_offset(&o->rgn, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2));
    RC_RETURN(c, kind_of(&o->rgn), 3);
}

static void k_GetRgnBox(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    if (!o) {
        RC_RETURN(c, RGN_ERROR, 2);
    }
    put_rect(RC_ARG(c, 1), rc_rgn_box(&o->rgn));
    RC_RETURN(c, kind_of(&o->rgn), 2);
}

/* GetRegionData(region, size, data): RGNDATAHEADER (size 32, RDH_RECTANGLES, count, bytes of
 * rectangles, bounds) and the rectangles; the size needed when data is NULL or too small. */
static void k_GetRegionData(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    uint32_t size = RC_ARG(c, 1), p = RC_ARG(c, 2);
    if (!o) {
        RC_RETURN(c, 0, 3);
    }
    uint32_t need = 32 + 16 * (uint32_t)o->rgn.n;
    if (!p || size < need) {
        RC_RETURN(c, need, 3);
    }
    rc_st32(p, 32);
    rc_st32(p + 4, 1);
    rc_st32(p + 8, (uint32_t)o->rgn.n);
    rc_st32(p + 12, 16 * (uint32_t)o->rgn.n);
    put_rect(p + 16, rc_rgn_box(&o->rgn));
    for (int i = 0; i < o->rgn.n; i++) {
        put_rect(p + 32 + 16 * (uint32_t)i, o->rgn.r[i]);
    }
    RC_RETURN(c, need, 3);
}

/* FillRgn(dc, region, brush) */
static void k_FillRgn(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    if (!d || !o) {
        RC_RETURN(c, 0, 3);
    }
    for (int i = 0; i < o->rgn.n; i++) {
        rc_rect r = o->rgn.r[i];
        r = (rc_rect){rc_dc_x(d, r.l), rc_dc_y(d, r.t), rc_dc_x(d, r.r), rc_dc_y(d, r.b)};
        rc_dc_pat(d, r, RC_ARG(c, 2), 0x00f00021u); /* PATCOPY */
    }
    RC_RETURN(c, 1, 3);
}

/* CreatePolygonRgn(points, count, mode): scan lines (even-odd or winding), runs of equal lines
 * merged into rectangles. */
static void k_CreatePolygonRgn(rc_cpu *c)
{
    uint32_t pts = RC_ARG(c, 0), n = RC_ARG(c, 1), winding = RC_ARG(c, 2) == 2;
    uint32_t h = rc_gobj_new(OB_REGION);
    gobj *o = rc_gobj_get(h, OB_REGION);
    rc_rgn_empty(&o->rgn);
    if (n < 3 || n > 1024) {
        RC_RETURN(c, h, 3);
    }
    int32_t ymin = INT32_MAX, ymax = INT32_MIN;
    for (uint32_t i = 0; i < n; i++) {
        int32_t y = (int32_t)rc_ld32(pts + 8 * i + 4);
        ymin = y < ymin ? y : ymin;
        ymax = y > ymax ? y : ymax;
    }
    int32_t prev[64], np = 0, run_top = ymin;
    for (int32_t y = ymin; y <= ymax; y++) {
        /* crossings of the line through the middle of row y */
        int32_t xs[64];
        int dir[64];
        int k = 0;
        for (uint32_t i = 0; i < n && k < 64; i++) {
            int32_t x0 = (int32_t)rc_ld32(pts + 8 * i), y0 = (int32_t)rc_ld32(pts + 8 * i + 4);
            uint32_t j = (i + 1) % n;
            int32_t x1 = (int32_t)rc_ld32(pts + 8 * j), y1 = (int32_t)rc_ld32(pts + 8 * j + 4);
            if ((y0 <= y && y1 > y) || (y1 <= y && y0 > y)) {
                xs[k] = x0 + (int32_t)((int64_t)(x1 - x0) * (y - y0) / (y1 - y0));
                dir[k++] = y1 > y0 ? 1 : -1;
            }
        }
        for (int a = 1; a < k; a++) { /* sort by x */
            for (int b = a; b > 0 && xs[b] < xs[b - 1]; b--) {
                int32_t t = xs[b]; xs[b] = xs[b - 1]; xs[b - 1] = t;
                int td = dir[b]; dir[b] = dir[b - 1]; dir[b - 1] = td;
            }
        }
        int32_t spans[64];
        int ns = 0, w = 0;
        for (int a = 0; a + 1 < k + 1 && a < k; a++) {
            int before = w;
            w += winding ? dir[a] : 1;
            bool inside = winding ? w != 0 : (w & 1);
            bool was = winding ? before != 0 : (before & 1);
            if (inside && !was && ns < 63) {
                spans[ns++] = xs[a];
            } else if (!inside && was && ns < 64) {
                spans[ns++] = xs[a];
            }
        }
        ns &= ~1;
        bool same = ns == np && memcmp(spans, prev, sizeof spans[0] * (size_t)ns) == 0;
        if (!same || y == ymax) {
            for (int a = 0; a + 1 < np; a += 2) {
                rc_rgn_add_rect(&o->rgn, (rc_rect){prev[a], run_top, prev[a + 1], y});
            }
            memcpy(prev, spans, sizeof spans[0] * (size_t)ns);
            np = ns;
            run_top = y;
        }
    }
    RC_RETURN(c, h, 3);
}

static void k_PtInRegion(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    RC_RETURN(c, o && rc_rgn_has(&o->rgn, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2)), 3);
}

static void k_RectInRegion(rc_cpu *c)
{
    gobj *o = rc_gobj_get(RC_ARG(c, 0), OB_REGION);
    if (!o) {
        RC_RETURN(c, 0, 2);
    }
    rc_rgn r = o->rgn;
    rc_rgn_and_rect(&r, get_rect(RC_ARG(c, 1)));
    RC_RETURN(c, r.n > 0, 2);
}

/* ---- the clip region of a DC (device coordinates of the DC) ---- */

/* The DC's whole drawing area, as the start of a clip region. */
static void whole(const rc_dc *d, rc_rgn *g)
{
    if (d->vis.n >= 0) {
        *g = d->vis;
        rc_rgn_offset(g, -d->org_x, -d->org_y);
    } else {
        rc_rgn_set(g, 0, 0, (int32_t)d->target.w, (int32_t)d->target.h);
    }
}

static void clip_box(const rc_dc *d, rc_rect *out)
{
    rc_rgn g;
    whole(d, &g);
    if (d->has_clip) {
        rc_rgn_and(&g, &d->clip);
    }
    rc_rect b = rc_rgn_box(&g);
    /* in logical coordinates */
    b.l += d->worg_x - d->vorg_x;
    b.r += d->worg_x - d->vorg_x;
    b.t += d->worg_y - d->vorg_y;
    b.b += d->worg_y - d->vorg_y;
    *out = b;
}

/* SelectClipRgn(dc, region): a copy of the region (device coordinates); NULL: none. */
static void k_SelectClipRgn(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, RGN_ERROR, 2);
    }
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    d->has_clip = o != NULL;
    if (o) {
        d->clip = o->rgn;
    }
    rc_rgn g;
    whole(d, &g);
    if (d->has_clip) {
        rc_rgn_and(&g, &d->clip);
    }
    RC_RETURN(c, kind_of(&g), 2);
}

/* ExtSelectClipRgn(dc, region, mode) */
static void k_ExtSelectClipRgn(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    uint32_t mode = RC_ARG(c, 2);
    if (!d) {
        RC_RETURN(c, RGN_ERROR, 3);
    }
    if (mode == 5 || !o) {
        d->has_clip = o != NULL;
        if (o) {
            d->clip = o->rgn;
        }
    } else {
        if (!d->has_clip) {
            whole(d, &d->clip);
            d->has_clip = true;
        }
        if (mode == 1) {
            rc_rgn_and(&d->clip, &o->rgn);
        } else if (mode == 2) {
            rc_rgn_add(&d->clip, &o->rgn);
        } else if (mode == 4) {
            rc_rgn_cut(&d->clip, &o->rgn);
        }
    }
    RC_RETURN(c, kind_of(d->has_clip ? &d->clip : &d->vis), 3);
}

static rc_rect logical_rect(const rc_dc *d, int32_t l, int32_t t, int32_t r, int32_t b)
{
    return (rc_rect){rc_dc_x(d, l), rc_dc_y(d, t), rc_dc_x(d, r), rc_dc_y(d, b)};
}

/* IntersectClipRect(dc, l, t, r, b), logical coordinates. */
static void k_IntersectClipRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, RGN_ERROR, 5);
    }
    if (!d->has_clip) {
        whole(d, &d->clip);
        d->has_clip = true;
    }
    rc_rgn_and_rect(&d->clip, logical_rect(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3),
                                           (int32_t)RC_ARG(c, 4)));
    RC_RETURN(c, kind_of(&d->clip), 5);
}

/* ExcludeClipRect(dc, l, t, r, b), logical coordinates. */
static void k_ExcludeClipRect(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, RGN_ERROR, 5);
    }
    if (!d->has_clip) {
        whole(d, &d->clip);
        d->has_clip = true;
    }
    rc_rgn_cut_rect(&d->clip, logical_rect(d, (int32_t)RC_ARG(c, 1), (int32_t)RC_ARG(c, 2), (int32_t)RC_ARG(c, 3),
                                           (int32_t)RC_ARG(c, 4)));
    RC_RETURN(c, kind_of(&d->clip), 5);
}

/* GetClipBox(dc, rect): logical coordinates. */
static void k_GetClipBox(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, RGN_ERROR, 2);
    }
    rc_rect b;
    clip_box(d, &b);
    put_rect(RC_ARG(c, 1), b);
    RC_RETURN(c, rc_rect_empty(b) ? NULLREGION : SIMPLEREGION, 2);
}

/* GetClipRgn(dc, region): 1 with a copy of the clip region, 0 if there is none. */
static void k_GetClipRgn(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    gobj *o = rc_gobj_get(RC_ARG(c, 1), OB_REGION);
    if (!d || !o) {
        RC_RETURN(c, 0xffffffffu, 2);
    }
    if (!d->has_clip) {
        RC_RETURN(c, 0, 2);
    }
    o->rgn = d->clip;
    RC_RETURN(c, 1, 2);
}

static void k_PtVisible(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 3);
    }
    rc_rgn g;
    whole(d, &g);
    if (d->has_clip) {
        rc_rgn_and(&g, &d->clip);
    }
    RC_RETURN(c, rc_rgn_has(&g, rc_dc_x(d, (int32_t)RC_ARG(c, 1)), rc_dc_y(d, (int32_t)RC_ARG(c, 2))), 3);
}

static void k_RectVisible(rc_cpu *c)
{
    rc_dc *d = rc_dc_get(RC_ARG(c, 0));
    if (!d) {
        RC_RETURN(c, 0, 2);
    }
    rc_rgn g;
    whole(d, &g);
    if (d->has_clip) {
        rc_rgn_and(&g, &d->clip);
    }
    rc_rect r = get_rect(RC_ARG(c, 1));
    rc_rgn_and_rect(&g, logical_rect(d, r.l, r.t, r.r, r.b));
    RC_RETURN(c, g.n > 0, 2);
}

const rc_import_def rc_gdi32_region[] = {
    {G32, "CreateRectRgn", k_CreateRectRgn},
    {G32, "CreateRectRgnIndirect", k_CreateRectRgnIndirect},
    {G32, "SetRectRgn", k_SetRectRgn},
    {G32, "CombineRgn", k_CombineRgn},
    {G32, "OffsetRgn", k_OffsetRgn},
    {G32, "GetRgnBox", k_GetRgnBox},
    {G32, "GetRegionData", k_GetRegionData},
    {G32, "FillRgn", k_FillRgn},
    {G32, "CreatePolygonRgn", k_CreatePolygonRgn},
    {G32, "PtInRegion", k_PtInRegion},
    {G32, "RectInRegion", k_RectInRegion},
    {G32, "SelectClipRgn", k_SelectClipRgn},
    {G32, "ExtSelectClipRgn", k_ExtSelectClipRgn},
    {G32, "IntersectClipRect", k_IntersectClipRect},
    {G32, "ExcludeClipRect", k_ExcludeClipRect},
    {G32, "GetClipBox", k_GetClipBox},
    {G32, "GetClipRgn", k_GetClipRgn},
    {G32, "PtVisible", k_PtVisible},
    {G32, "RectVisible", k_RectVisible},
};
const uint32_t rc_gdi32_region_count = sizeof rc_gdi32_region / sizeof rc_gdi32_region[0];
