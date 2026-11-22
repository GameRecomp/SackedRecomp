/* SPDX-License-Identifier: MIT */
#ifndef RC_WINMM_H
#define RC_WINMM_H

#include "rc/hle.h"

extern const rc_import_def rc_winmm_defs[];
extern const uint32_t rc_winmm_count;
/* CD audio (MCI) on the install's MUSIC/TrackNN files, and a mixer without devices. */
extern const rc_import_def rc_winmm_mci_defs[];
extern const uint32_t rc_winmm_mci_count;

#endif
