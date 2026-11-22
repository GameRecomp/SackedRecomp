/* SPDX-License-Identifier: MIT */
#ifndef RC_DPLAY_H
#define RC_DPLAY_H

#include "rc/hle.h"

/* DirectPlay 4 and its lobby (dplay.c), DirectPlay 8 (dpnet.c), on the network of net.c. */
void rc_dplay_init(void);
void rc_dpnet_init(void);

#endif
