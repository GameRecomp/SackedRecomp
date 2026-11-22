/* SPDX-License-Identifier: MIT */
/* The Windows ANSI code pages the games were made for: 1252 (Western), 1250 (Central European),
 * 1251 (Cyrillic). */
#ifndef RC_CODEPAGE_H
#define RC_CODEPAGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The Unicode character of byte `b` in code page `cp` (1250, 1251; anything else is 1252). */
uint32_t rc_codepage_unicode(uint32_t cp, uint8_t b);

/* The ANSI code page of the install being played (GetACP), set from its language. */
uint32_t rc_game_codepage(void);
void rc_set_game_codepage(uint32_t cp);
/* The ANSI code page of a language as rc_game_language names it ("Polski": 1250). */
uint32_t rc_codepage_of_language(const char *lang);

#ifdef __cplusplus
}
#endif

#endif
