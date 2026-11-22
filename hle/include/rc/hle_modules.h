/* SPDX-License-Identifier: MIT */
/* The reimplemented DLLs. A game project registers the ones it needs
 * before its executable is loaded (rc_config.setup). */
#ifndef RC_HLE_MODULES_H
#define RC_HLE_MODULES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void rc_hle_kernel32(void);
void rc_hle_user32(void);
void rc_hle_gdi32(void);
void rc_hle_ddraw(void);
void rc_hle_mss32(void);
void rc_hle_winmm(void);
void rc_hle_msvcrt(void);
void rc_hle_advapi32(void);
void rc_hle_dinput(void);
void rc_hle_dmusic(void);
void rc_hle_dsound(void);
void rc_hle_shell32(void);
void rc_hle_wsock(void);
void rc_hle_dshow(void);
void rc_hle_dplay(void);

/* Everything above. */
void rc_hle_all(void);

/* Work for a game project's patches that needs the guest CPU and must
 * happen regularly: `fn` runs whenever the program looks for messages. */
struct rc_cpu;
void rc_add_pump_hook(void (*fn)(struct rc_cpu *c));

/* A filter for the host's input before the program gets it (one): true
 * when it takes the event, to hold it back or act on it itself. An event
 * held back is given to the program later with rc_input_deliver. */
struct rc_event;
void rc_set_input_filter(bool (*fn)(const struct rc_event *e));
void rc_input_deliver(const struct rc_event *e);
/* A game project's patch may move where the pointer is for the program
 * (`fn` changes the point, in the program's coordinates), for example to
 * where something it draws elsewhere stands in the program's own. */
void rc_set_pointer_map(void (*fn)(int *x, int *y));
/* What the last pointer event came from (rc_event_source). */
int rc_input_source(void);

/* kernel32's wait for one (or all) of `n` handles, or for `extra(ctx)` to become true, which
 * returns WAIT_OBJECT_0 + n (MsgWaitForMultipleObjects). */
uint32_t rc_k32_wait(const uint32_t *h, uint32_t n, bool all, uint32_t timeout, bool (*extra)(void *), void *ctx);

/* DirectDraw's display mode (0 x 0 before one is set). */
void rc_ddraw_mode(uint32_t *w, uint32_t *h);
/* Whether a DirectDraw primary surface exists (the program shows itself through DirectDraw). */
bool rc_ddraw_has_primary(void);

/* A program that draws its own pointer, moved by DirectInput's relative mouse motion, keeps it at
 * these guest addresses (32-bit x and y, in its screen's pixels). The mouse's state then reports
 * the way from there to the host's pointer, so that the program's pointer is where the host's
 * is (an absolute mouse, as in a window), rather than motion added up on its own. Moves from a
 * script stay relative. A game project's patch says where (0: nowhere). */
void rc_dinput_pointer_at(uint32_t x, uint32_t y);

/* A display mode DirectDraw offers besides its own list (a game project's
 * resolution patch); 0 x 0 for none. */
void rc_ddraw_extra_mode(uint32_t w, uint32_t h);
/* Further display modes DirectDraw lists beside its own, kept (up to 8);
 * for a game that enumerates the modes once and looks its size up there. */
void rc_ddraw_add_mode(uint32_t w, uint32_t h);
/* Whether DirectDraw lists the resolution of the settings (rc_plat_game_size) among its modes (the
 * default): off for a game project whose patch offers the window's size its own way. */
void rc_ddraw_list_settings_size(bool list);
/* The next Blt from a source one column wide stretches it to `width`
 * columns; the column it takes is, within `range` columns of its own, the
 * one whose colours are closest to theirs on average (neither a shadow nor
 * a highlight). For a game project's patch that widens a picture; 0 cancels. */
void rc_ddraw_stretch_next(uint32_t width, uint32_t range);
/* The next Blt of a picture with a repeating pattern (a balustrade) draws
 * it `width` wide: its left half at the left, its right half at the right
 * end, and between them its pattern's period (found in the picture) as
 * often as fits, each copy stretched a little so that they fit exactly.
 * 0 cancels. */
void rc_ddraw_widen_next(uint32_t width);

#ifdef __cplusplus
}
#endif

#endif
