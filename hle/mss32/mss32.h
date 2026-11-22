/* SPDX-License-Identifier: MIT */
#ifndef RC_MSS32_H
#define RC_MSS32_H

#include "rc/hle.h"

/* Register what Miles needs besides its imports. */
void rc_mss32_init(void);

extern const rc_import_def rc_mss32_defs[];
extern const uint32_t rc_mss32_count;

/* RIB: the provider interfaces (ASI codecs) the lifted Miles plug-ins register (rib.c). */
extern const rc_import_def rc_rib_defs[];
extern const uint32_t rc_rib_count;

#endif
