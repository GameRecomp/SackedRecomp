/* SPDX-License-Identifier: MIT */
/* Shared by the user32 sources (and comctl32, comdlg32). */
#ifndef RC_USER32_H
#define RC_USER32_H

#include <stdbool.h>
#include <stdint.h>

#include "gdi32/dc.h"
#include "rc/hle.h"

/* ---- window classes (user32.c) ---- */

typedef struct rc_wclass {
    char name[64];
    uint32_t atom, style, wndproc, cls_extra, wnd_extra, instance, icon, cursor, background, menu_name, icon_small;
    uint8_t cls_data[64];
} rc_wclass;

typedef struct rc_wclass rc_wclass_info;

/* A class by name (or "#atom"), case-insensitive. */
rc_wclass *rc_class_find(const char *name);
/* A built-in class whose window procedure is the host function `fn` (controls, dialogs). */
void rc_class_builtin(const char *name, uint32_t style, void (*fn)(rc_cpu *c), uint32_t wnd_extra, uint32_t background,
                      uint32_t cursor);
/* A registered window class: its window procedure and style (old interface). */
const rc_wclass_info *rc_find_class(const char *name, uint32_t *wndproc, uint32_t *style);
/* The arrow cursor's handle. */
uint32_t rc_arrow_cursor(void);
/* SetCursor from host code. */
void rc_set_cursor(uint32_t cursor);

/* ---- windows (window.c) ---- */

#define WS_OVERLAPPED 0x00000000u
#define WS_POPUP 0x80000000u
#define WS_CHILD 0x40000000u
#define WS_MINIMIZE 0x20000000u
#define WS_VISIBLE 0x10000000u
#define WS_DISABLED 0x08000000u
#define WS_CLIPSIBLINGS 0x04000000u
#define WS_CLIPCHILDREN 0x02000000u
#define WS_MAXIMIZE 0x01000000u
#define WS_CAPTION 0x00c00000u
#define WS_BORDER 0x00800000u
#define WS_DLGFRAME 0x00400000u
#define WS_VSCROLL 0x00200000u
#define WS_HSCROLL 0x00100000u
#define WS_SYSMENU 0x00080000u
#define WS_THICKFRAME 0x00040000u
#define WS_GROUP 0x00020000u
#define WS_TABSTOP 0x00010000u
#define WS_EX_DLGMODALFRAME 0x00000001u
#define WS_EX_TOPMOST 0x00000008u
#define WS_EX_TOOLWINDOW 0x00000080u
#define WS_EX_WINDOWEDGE 0x00000100u
#define WS_EX_CLIENTEDGE 0x00000200u
#define WS_EX_CONTROLPARENT 0x00010000u
#define WS_EX_STATICEDGE 0x00020000u

bool rc_wnd_ok(uint32_t hwnd);
uint32_t rc_wnd_style(uint32_t hwnd);
void rc_wnd_set_style(uint32_t hwnd, uint32_t style);
uint32_t rc_wnd_exstyle(uint32_t hwnd);
uint32_t rc_wnd_parent(uint32_t hwnd);   /* the parent of a child window, else 0 */
uint32_t rc_wnd_owner(uint32_t hwnd);    /* the owner of a top-level window */
uint32_t rc_wnd_id(uint32_t hwnd);
uint32_t rc_wnd_instance(uint32_t hwnd);
uint32_t rc_wnd_menu(uint32_t hwnd);     /* a top-level window's menu bar */
/* A menu resource (LoadMenuA): the menu's handle, or 0. */
uint32_t rc_menu_load(uint32_t inst, uint32_t name);
void rc_wnd_set_menu(uint32_t hwnd, uint32_t menu);
const char *rc_wnd_text(uint32_t hwnd);
void rc_wnd_set_text(uint32_t hwnd, const char *text);
const char *rc_wnd_class_name(uint32_t hwnd);
/* Host data a built-in control keeps per window (freed with the window). */
void *rc_wnd_data(uint32_t hwnd);
void rc_wnd_set_data(uint32_t hwnd, void *data);
/* The client area: (0, 0, width, height); and its top-left on the screen. */
rc_rect rc_wnd_client(uint32_t hwnd);
void rc_wnd_client_origin(uint32_t hwnd, int32_t *x, int32_t *y);
/* The window rectangle on the screen. */
rc_rect rc_wnd_screen_rect(uint32_t hwnd);
bool rc_wnd_visible(uint32_t hwnd); /* it and all its parents */
bool rc_wnd_enabled(uint32_t hwnd);
/* Children in z-order from the top (first: the first created). */
uint32_t rc_wnd_first_child(uint32_t hwnd);
uint32_t rc_wnd_next_sibling(uint32_t hwnd);
uint32_t rc_wnd_top_level(uint32_t hwnd);

/* Messages: sent now (the window procedure is called), or posted. */
uint32_t rc_wnd_send(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
/* The thread a window belongs to; messages sent to this thread's windows by others, handled now. */
uint32_t rc_wnd_thread(uint32_t hwnd);
void rc_wnd_process_sent(rc_cpu *c);
void rc_post_message(uint32_t hwnd, uint32_t message, uint32_t wparam, uint32_t lparam);
/* CreateWindowExA from host code; class and text are host strings. */
uint32_t rc_wnd_create(rc_cpu *c, uint32_t exstyle, const char *cls, const char *text, uint32_t style, int32_t x,
                       int32_t y, int32_t w, int32_t h, uint32_t parent, uint32_t menu, uint32_t instance,
                       uint32_t param);
void rc_wnd_destroy(rc_cpu *c, uint32_t hwnd);
void rc_wnd_show(rc_cpu *c, uint32_t hwnd, int cmd);
void rc_wnd_move(rc_cpu *c, uint32_t hwnd, int32_t x, int32_t y, int32_t w, int32_t h, bool repaint);
void rc_wnd_enable(rc_cpu *c, uint32_t hwnd, bool enable);
/* The frame changed (style, menu, scroll bars): the client area is computed again. */
void rc_wnd_frame_changed(rc_cpu *c, uint32_t hwnd);
void rc_wnd_redraw_frame(uint32_t hwnd);
/* Invalidate the client (NULL: all of it), and erase on the next paint. */
void rc_wnd_invalidate(uint32_t hwnd, const rc_rect *r, bool erase);
/* Paint what is invalid now (UpdateWindow). */
void rc_wnd_update(rc_cpu *c, uint32_t hwnd);
/* A DC on the client area (or the whole window), clipped to what is visible; release it. */
uint32_t rc_wnd_dc(uint32_t hwnd, bool whole_window);
void rc_wnd_release_dc(uint32_t hdc);
uint32_t rc_wnd_focus(void);
uint32_t rc_wnd_set_focus(rc_cpu *c, uint32_t hwnd);
uint32_t rc_wnd_capture(void);
void rc_wnd_set_capture(uint32_t hwnd);
uint32_t rc_wnd_active(void);
void rc_wnd_activate(rc_cpu *c, uint32_t hwnd);
/* The window at a screen point (enabled and visible ones), and the hit test of that window. */
uint32_t rc_wnd_from_point(int32_t x, int32_t y);
/* Whether windows are shown on our own desktop (no DirectDraw primary): then the desktop's
 * pixels are what the platform shows. */
bool rc_desktop_active(void);
void rc_desktop_size(int32_t *w, int32_t *h);
/* Present the desktop if anything changed (and count the frame). */
void rc_desktop_present(bool force);
/* Paint every window that is invalid, parents before children (sends WM_NCPAINT, WM_PAINT). */
void rc_wnd_validate(uint32_t hwnd);
/* Paint frames that are due; the next window whose client needs WM_PAINT (0: none). */
uint32_t rc_wnd_paint_next(rc_cpu *c, uint32_t hwnd);
/* DefWindowProc for host window procedures. */
uint32_t rc_def_window_proc(rc_cpu *c, uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp);
void rc_wnd_paint_all(rc_cpu *c);
/* The next window to get WM_PAINT (0: none), restricted to `hwnd` and its children if given. */
uint32_t rc_wnd_paint_due(uint32_t hwnd);

/* The first top-level window, the one the platform shows. */
uint32_t rc_main_window(void);
/* Make a window cover a screen of this size (a display mode change). */
void rc_window_resize(uint32_t hwnd, int32_t width, int32_t height);
/* The window procedure of a window, or 0. */
uint32_t rc_window_proc(uint32_t hwnd);
/* The device context of a window (0: the screen); in a game a stand-in. */
uint32_t rc_window_dc(uint32_t hwnd);

/* ---- input and the message queue (input.c) ---- */

/* Deliver host events (input, window changes) into the message queue. */
void rc_pump_events(void);
struct rc_event;
/* Deliver an input event as if the host had sent it (scripted input). */
void rc_inject_event(const struct rc_event *e);
/* RC_RECORD (frames.c): the user's input written as an RC_INPUT script. */
void rc_record_event(const struct rc_event *e);
/* A filter for the host's input, before the program gets it: true when it
 * takes the event (holds it back, or acts on it itself). One filter. */
void rc_set_input_filter(bool (*fn)(const struct rc_event *e));
/* Give the program an event now, past the filter. */
void rc_input_deliver(const struct rc_event *e);
/* Called with every input event the program gets, before the window messages (at most 4). */
void rc_add_input_observer(void (*fn)(const struct rc_event *e));
/* What the last pointer event came from (rc_event_source). */
int rc_input_source(void);
/* Run `fn` whenever the program looks for messages. */
void rc_add_pump_hook(void (*fn)(rc_cpu *c));
/* Where the mouse is, in frame (screen) coordinates. */
void rc_input_mouse(int *x, int *y);
/* The keys' state (GetKeyState). */
uint8_t rc_key_state(uint32_t vk);
/* The next message into `out` (MSG*), 0 if none; `remove` takes it. */
int rc_peek_message(rc_cpu *c, uint32_t out, uint32_t hwnd, uint32_t lo, uint32_t hi, bool remove);
/* Run a modal loop: messages are read and dispatched (with IsDialogMessage for `dialog`) until
 * *done; `idle` is called when there is nothing to do. */
void rc_modal_loop(rc_cpu *c, uint32_t dialog, const bool *done);
/* Follow the mouse while a button is held: the next position; false when it is released. */
bool rc_input_track(rc_cpu *c, int32_t *x, int32_t *y);
/* Raw desktop input for modal tracking (menus): the next event (false: none), or put one back. */
bool rc_input_next_raw(struct rc_event *e);
void rc_input_unread_raw(const struct rc_event *e);
/* Call the hook chain of a kind (WH_CBT 5, ...): the result of the first hook. */
uint32_t rc_call_hooks(rc_cpu *c, int kind, uint32_t code, uint32_t wp, uint32_t lp, bool *called);
/* Remove a destroyed window's timers and messages. */
void rc_input_forget(uint32_t hwnd);

/* ---- drawing helpers (uidraw.c) ---- */

uint32_t rc_sys_color(uint32_t index);
uint32_t rc_sys_brush(uint32_t index);
/* The brush of a WNDCLASS hbrBackground value or a COLOR_* + 1 value, as a COLORREF; false: none. */
bool rc_brush_color(uint32_t brush, uint32_t *colorref);
/* A 3D edge (BDR_*, BF_*) around `r` (device coordinates of the DC). */
void rc_draw_edge(rc_dc *d, rc_rect r, uint32_t edge, uint32_t flags);
/* DrawText's work: the height of the text drawn (or measured with DT_CALCRECT into *r). */
int rc_draw_text(rc_dc *d, const char *text, int32_t n, rc_rect *r, uint32_t format);
/* A dotted focus rectangle (drawn by inverting). */
void rc_draw_focus(rc_dc *d, rc_rect r);
/* The default font for controls and dialogs. */
uint32_t rc_gui_font(void);

/* ---- menus (menu.c) ---- */

int rc_menu_bar_height(uint32_t hwnd);
void rc_menu_draw_bar(rc_dc *d, uint32_t hwnd, rc_rect r);
/* A click on the menu bar (screen coordinates): the menu runs and WM_COMMAND follows. */
void rc_menu_bar_track(rc_cpu *c, uint32_t hwnd, int32_t x, int32_t y, uint32_t key);
bool rc_menu_valid(uint32_t hmenu);

/* ---- scroll bars (controls.c) ---- */

typedef struct rc_scroll {
    int32_t min, max, page, pos, track;
    bool shown, disabled;
} rc_scroll;
rc_scroll *rc_wnd_scroll(uint32_t hwnd, int bar); /* SB_HORZ 0, SB_VERT 1 of a window */
void rc_scroll_draw(rc_dc *d, rc_rect r, bool vertical, const rc_scroll *s, bool enabled);
/* Track a click on a scroll bar (r: its rectangle in screen coordinates): the scroll messages
 * go to `to` (WM_HSCROLL/WM_VSCROLL with `ctl` as lParam). */
void rc_scroll_track(rc_cpu *c, uint32_t to, uint32_t ctl, rc_rect r, bool vertical, rc_scroll *s, int32_t x,
                     int32_t y);

/* ---- dialogs (dialog.c) ---- */

bool rc_is_dialog_message(rc_cpu *c, uint32_t dialog, uint32_t msg);
uint32_t rc_next_tab(rc_cpu *c, uint32_t dlgw, uint32_t from, bool back);
/* TranslateMessage's work on a MSG (WM_CHAR after WM_KEYDOWN). */
void rc_translate_key(uint32_t msg);
/* GetWindowLongA and SetWindowLongA (extra bytes) from host code. */
uint32_t rc_wnd_long(uint32_t hwnd, int32_t index);
void rc_wnd_set_long(uint32_t hwnd, int32_t index, uint32_t v);
uint32_t rc_load_icon_or_cursor(rc_cpu *c, uint32_t inst, uint32_t name, uint32_t type);
/* LoadBitmapA's work: a 32-bit bitmap from a resource. */
uint32_t rc_load_bitmap(uint32_t inst, uint32_t name);
void rc_dialog_init(void);
/* A dialog made in code: a modal-frame #32770 window of client size cw x ch, centred, with the
 * dialog procedure `proc` (a guest or import address, or 0) and the default font. */
uint32_t rc_dialog_create(rc_cpu *c, uint32_t owner, const char *title, int32_t cw, int32_t ch, uint32_t proc);
/* A visible control in it (a BS_DEFPUSHBUTTON becomes the default button). */
uint32_t rc_dialog_item(rc_cpu *c, uint32_t hwnd, const char *cls, const char *text, uint32_t style, int32_t x, int32_t y,
                        int32_t w, int32_t h, uint32_t id);
/* Show it modally (`owner` disabled meanwhile, `focus` or the first tab stop focused) until
 * rc_dialog_end; destroys it and returns the result. */
uint32_t rc_dialog_run(rc_cpu *c, uint32_t hwnd, uint32_t owner, uint32_t focus);
void rc_dialog_end(rc_cpu *c, uint32_t hwnd, uint32_t result);
/* MessageBoxA's box (answered at once when there is no desktop). */
uint32_t rc_message_box(rc_cpu *c, uint32_t owner, const char *text, const char *caption, uint32_t type);
void rc_controls_init(void);
void rc_mdi_init(void);
void rc_menu_init(void);

extern const rc_import_def rc_user32_windows[];
extern const uint32_t rc_user32_windows_count;
extern const rc_import_def rc_user32_input[];
extern const uint32_t rc_user32_input_count;
extern const rc_import_def rc_user32_draw[];
extern const uint32_t rc_user32_draw_count;
extern const rc_import_def rc_user32_setup[];
extern const uint32_t rc_user32_setup_count;

#endif
