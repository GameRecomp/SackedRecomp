/* SPDX-License-Identifier: MIT */
/* The single generated table rc_run falls back to when its configuration lists no lifted
 * binaries (rc_config.lifted). This project's shells always list them, and its generated code
 * defines per-binary tables only, so these empty ones are linked from the archive unless a
 * program brings its own (as TropicoRecomp's generated dispatch.c does). */
#include "rc/dispatch.h"

const rc_fn_entry rc_lifted_table[1] = {{0}};
const uint32_t rc_lifted_count = 0;
const char *const rc_lifted_modules[] = {0};
