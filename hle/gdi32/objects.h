/* SPDX-License-Identifier: MIT */
/* GDI objects (objects.c): bitmaps, fonts, pens, brushes and regions in one table. */
#ifndef RC_GDI_OBJECTS_H
#define RC_GDI_OBJECTS_H

#include <stdbool.h>
#include <stdint.h>

#include "dc.h"

enum okind { OB_FREE, OB_BITMAP, OB_FONT, OB_PEN, OB_BRUSH, OB_REGION };

typedef struct gobj {
    enum okind kind;
    bool stock;
    /* bitmap: pixels in guest memory (rows top-down for DDBs, as the DIB says for sections) */
    uint32_t w, h, bpp, widthbytes, pixels;
    bool dib, bottom_up, own_pixels;
    uint32_t colors[256]; /* a DIB's color table (0x00RRGGBB), for 1, 4 and 8 bits */
    uint32_t ncolors;
    uint32_t masks[3];    /* a DIB's 16- or 32-bit masks */
    /* font */
    char face[64];
    int32_t height, width;
    uint32_t weight, italic, underline, charset, pitch;
    /* pen and brush: style, width (pen), color (COLORREF), hatch, pattern bitmap (brush) */
    uint32_t style, pwidth, color, hatch, pattern;
    /* region */
    rc_rgn rgn;
} gobj;

uint32_t rc_gobj_new(enum okind k);
gobj *rc_gobj_get(uint32_t h, enum okind k);
enum okind rc_gobj_kind(uint32_t h);
void rc_gobj_free(uint32_t h);

/* Stock objects (GetStockObject) have fixed handles. */
uint32_t rc_gdi_stock(uint32_t index);

/* A bitmap as pixels a DC can draw into or read from. */
void rc_gdi_bitmap_pixels(const gobj *b, rc_pixels *out);

/* Selecting an object into a DC: the object it replaced. */
uint32_t rc_gdi_select(uint32_t hdc, uint32_t h);
uint32_t rc_gdi_solid_brush(uint32_t colorref);
/* Copy a DIB (bits, BITMAPINFO, DIB_RGB_COLORS 0 / DIB_PAL_COLORS 1) into a bitmap of the same size. */
void rc_gdi_draw_dib_into(uint32_t hbitmap, uint32_t bits, uint32_t bmi, uint32_t usage);
/* A 32-bit bitmap with the picture of a DIB. */
uint32_t rc_gdi_bitmap_from_dib(uint32_t bmi, uint32_t bits);

/* Text (text.c) for the drawing core and user32's controls. */
struct rc_font;
struct rc_font *rc_gdi_font_of(const rc_dc *d);
/* Draw code page 1252 text at (x, y) in DC device coordinates (cell top-left), clipped, in the
 * DC's text color, without background. */
int rc_gdi_text(rc_dc *d, int32_t x, int32_t y, const char *text, uint32_t n);
/* The size of code page 1252 text in the DC's font. */
void rc_gdi_text_size(rc_dc *d, const char *text, uint32_t n, int *w, int *h);

#endif
