/* SPDX-License-Identifier: MIT */
#ifndef RC_DC_H
#define RC_DC_H

#include <stdbool.h>
#include <stdint.h>

/* Pixels in guest memory a DC draws into (pixels == 0: nowhere). */
typedef struct rc_pixels {
    uint32_t pixels, w, h, pitch, bpp;
    uint32_t rmask, gmask, bmask;
} rc_pixels;

/* A rectangle (right and bottom exclusive). */
typedef struct rc_rect {
    int32_t l, t, r, b;
} rc_rect;

/* A region: rectangles that do not overlap. */
#define RC_RGN_RECTS 128
typedef struct rc_rgn {
    int n;
    rc_rect r[RC_RGN_RECTS];
} rc_rgn;

void rc_rgn_empty(rc_rgn *g);
void rc_rgn_set(rc_rgn *g, int32_t l, int32_t t, int32_t r, int32_t b);
void rc_rgn_and_rect(rc_rgn *g, rc_rect r);
void rc_rgn_cut_rect(rc_rgn *g, rc_rect r);
void rc_rgn_add_rect(rc_rgn *g, rc_rect r);
void rc_rgn_and(rc_rgn *g, const rc_rgn *h);
void rc_rgn_cut(rc_rgn *g, const rc_rgn *h);
void rc_rgn_add(rc_rgn *g, const rc_rgn *h);
void rc_rgn_offset(rc_rgn *g, int32_t dx, int32_t dy);
rc_rect rc_rgn_box(const rc_rgn *g);
bool rc_rgn_has(const rc_rgn *g, int32_t x, int32_t y);
bool rc_rect_empty(rc_rect r);
rc_rect rc_rect_and(rc_rect a, rc_rect b);

/* What SaveDC keeps. */
typedef struct rc_dc_state {
    uint32_t bitmap, font, pen, brush, text_color, bk_color, bk_mode, text_align, rop2, map_mode;
    int32_t cur_x, cur_y, vorg_x, vorg_y, worg_x, worg_y;
    bool has_clip;
    rc_rgn clip;
} rc_dc_state;

typedef struct rc_dc {
    int used;
    rc_pixels target;
    uint32_t palette[256];
    int has_palette;
    uint32_t bitmap, font;   /* selected objects */
    uint32_t pen;            /* 0: the default pen, black and one pixel wide */
    uint32_t brush;          /* 0: the default brush, white */
    int32_t cur_x, cur_y;    /* the current position (MoveToEx, LineTo) */
    uint32_t text_color;     /* COLORREF: 0x00BBGGRR */
    uint32_t bk_color;
    uint32_t bk_mode;        /* 1 TRANSPARENT, 2 OPAQUE */
    uint32_t text_align;
    uint32_t rop2;           /* R2_COPYPEN 13 ... */
    uint32_t map_mode;       /* MM_TEXT 1 (others are drawn as MM_TEXT) */
    int32_t vorg_x, vorg_y;  /* viewport origin (device) */
    int32_t worg_x, worg_y;  /* window origin (logical) */
    /* A window's DC: where its (0, 0) is in the target, and where it may draw there at all
     * (the window's visible part, device coordinates); vis.n < 0: the whole target. */
    uint32_t hwnd;
    int32_t org_x, org_y;
    rc_rgn vis;
    /* 1: a window's client DC, 2: its whole-window DC, whose origin and region follow the window
     * (rc_dc_refresh, user32's, when vis_gen is stale); 0: fixed. */
    uint8_t vis_kind;
    uint32_t vis_gen;
    /* The program's clip region (SelectClipRgn), in device coordinates of the DC. */
    bool has_clip;
    rc_rgn clip;
    rc_dc_state saved[8];
    int nsaved;
} rc_dc;

rc_dc *rc_dc_get(uint32_t hdc);
/* Set by user32: brings a window DC (vis_kind != 0) up to date with the windows. */
extern void (*rc_dc_refresh)(rc_dc *d);
uint32_t rc_dc_new(void);
void rc_dc_free(uint32_t hdc);

/* ---- drawing (draw.c), device coordinates of the DC (origin and clipping applied) ---- */

/* Logical to device coordinates of the DC (viewport and window origins). */
static inline int32_t rc_dc_x(const rc_dc *d, int32_t x) { return x - d->worg_x + d->vorg_x; }
static inline int32_t rc_dc_y(const rc_dc *d, int32_t y) { return y - d->worg_y + d->vorg_y; }

/* Where the DC draws, in target coordinates. */
void rc_dc_region(const rc_dc *d, rc_rgn *out);
/* A COLORREF as a pixel value of the target, and back. */
uint32_t rc_dc_pixel(const rc_dc *d, uint32_t colorref);
uint32_t rc_dc_colorref(const rc_dc *d, uint32_t pixel);
uint32_t rc_dc_read(const rc_dc *d, int32_t x, int32_t y); /* target coordinates */
void rc_dc_write(const rc_dc *d, int32_t x, int32_t y, uint32_t pixel);
/* Fill a rectangle (DC device coordinates) with a COLORREF, clipped. */
void rc_dc_fill(rc_dc *d, rc_rect r, uint32_t colorref);
/* Fill with the brush `brush` (0: the DC's) and a raster operation (PATCOPY, PATINVERT, ...). */
void rc_dc_pat(rc_dc *d, rc_rect r, uint32_t brush, uint32_t rop);
/* Set one pixel (DC device coordinates), clipped. */
void rc_dc_plot(rc_dc *d, int32_t x, int32_t y, uint32_t pixel);

#endif
