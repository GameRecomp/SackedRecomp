/* SPDX-License-Identifier: MIT */
/* SHA-256 (FIPS 180-4): the game shell picks the lifted binaries whose hashes match the
 * user's files, as the manifests pin them. */
#ifndef RC_SHA256_H
#define RC_SHA256_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The digest of data[0..n) as 64 lower-case hex digits and a NUL. */
void rc_sha256_hex(const uint8_t *data, size_t n, char out[65]);

#ifdef __cplusplus
}
#endif
#endif
