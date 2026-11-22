/* SPDX-License-Identifier: MIT */
#include "advapi32/advapi32.h"
#include "ddraw/ddraw.h"
#include "dinput/dinput.h"
#include "dmusic/dmusic.h"
#include "dplay/dplay.h"
#include "dshow/dshow.h"
#include "dsound/dsound.h"
#include "shell32/shell32.h"
#include "wsock/wsock.h"
#include "mss32/mss32.h"
#include "msvcrt/msvcrt.h"
#include "gdi32/gdi32.h"
#include "kernel32/k32.h"
#include "rc/hle_modules.h"
#include "comctl32/comctl32.h"
#include "comdlg32/comdlg32.h"
#include "user32/user32.h"
#include "winmm/winmm.h"

void rc_hle_kernel32(void)
{
    rc_register_imports(rc_k32_process, rc_k32_process_count);
    rc_register_imports(rc_k32_memory, rc_k32_memory_count);
    rc_register_imports(rc_k32_locale, rc_k32_locale_count);
    rc_register_imports(rc_k32_file, rc_k32_file_count);
    rc_register_imports(rc_k32_profile, rc_k32_profile_count);
    rc_register_imports(rc_k32_sync, rc_k32_sync_count);
    rc_k32_global_init();
    rc_k32_misc_init();
    rc_register_imports(rc_version_defs, rc_version_count); /* VERSION.dll, on the same files */
    rc_register_imports(rc_ole32_defs, rc_ole32_count);
}

void rc_hle_user32(void)
{
    rc_register_imports(rc_user32_setup, rc_user32_setup_count);
    rc_register_imports(rc_user32_windows, rc_user32_windows_count);
    rc_register_imports(rc_user32_input, rc_user32_input_count);
    rc_register_imports(rc_user32_draw, rc_user32_draw_count);
    rc_menu_init();
    rc_controls_init();
    rc_mdi_init();
    rc_dialog_init();
    rc_comctl32_init();
    rc_comdlg32_init();
}

void rc_hle_gdi32(void)
{
    rc_register_imports(rc_gdi32_setup, rc_gdi32_setup_count);
    rc_register_imports(rc_gdi32_objects, rc_gdi32_objects_count);
    rc_register_imports(rc_gdi32_draw, rc_gdi32_draw_count);
    rc_register_imports(rc_gdi32_region, rc_gdi32_region_count);
    rc_register_imports(rc_gdi32_text, rc_gdi32_text_count);
    rc_register_imports(rc_gdi32_palette, rc_gdi32_palette_count);
}

void rc_hle_ddraw(void)
{
    rc_register_imports(rc_ddraw_defs, rc_ddraw_count);
    rc_register_imports(rc_d3d_defs, rc_d3d_count);
}

void rc_hle_mss32(void)
{
    rc_register_imports(rc_mss32_defs, rc_mss32_count);
    rc_register_imports(rc_rib_defs, rc_rib_count);
    rc_mss32_init();
}

void rc_hle_winmm(void)
{
    rc_register_imports(rc_winmm_defs, rc_winmm_count);
    rc_register_imports(rc_winmm_mci_defs, rc_winmm_mci_count);
}

void rc_hle_msvcrt(void) { rc_msvcrt_init(); }

void rc_hle_advapi32(void) { rc_register_imports(rc_advapi32_defs, rc_advapi32_count); }

void rc_hle_dinput(void) { rc_register_imports(rc_dinput_defs, rc_dinput_count); }

void rc_hle_dmusic(void) { rc_dmusic_init(); }

void rc_hle_dsound(void) { rc_register_imports(rc_dsound_defs, rc_dsound_count); }

void rc_hle_shell32(void) { rc_register_imports(rc_shell32_defs, rc_shell32_count); }

void rc_hle_wsock(void) { rc_register_imports(rc_wsock_defs, rc_wsock_count); }

void rc_hle_dshow(void) { rc_dshow_init(); }

void rc_hle_dplay(void) { rc_dplay_init(); }

void rc_hle_all(void)
{
    rc_hle_dplay();
    rc_hle_dshow();
    rc_hle_wsock();
    rc_hle_advapi32();
    rc_hle_dinput();
    rc_hle_dmusic();
    rc_hle_dsound();
    rc_hle_shell32();
    rc_hle_winmm();
    rc_hle_mss32();
    rc_hle_msvcrt();
    rc_hle_ddraw();
    rc_hle_kernel32();
    rc_hle_user32();
    rc_hle_gdi32();
}
