/* SPDX-License-Identifier: MIT */
/* Entry point shared by all game shells (see cmake/game_main.c.in). */
#ifndef RC_GAME_H
#define RC_GAME_H

#include "rc/cpu.h"
#include "rc/dispatch.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rc_game_info {
    const char *id;                /* manifest id, e.g. "cultures1" */
    const char *title;
    const char *const *exe_names;  /* NULL-terminated; looked up case-insensitively */
    const char *guest_dir;         /* where the game believes it is installed, e.g. "C:\\Cultures" */
    /* The game project's patches (games/<id>/patches.c, rc_add_game's PATCHES): registers its
     * hooks and points once the HLE modules are there; NULL for none. */
    void (*patches)(void);
} rc_game_info;

/* One lifted binary, emitted by `recomp lift` (`binary.c` in its directory below
 * generated/<game>/). The shell picks the binaries whose hashes match the user's files. */
typedef struct rc_generated_binary {
    const char *name;            /* e.g. "Cultures.exe" */
    const char *variant;         /* first variant of the [[binary]], e.g. "gog-en" */
    const char *sha256;          /* pinned hash from game.toml */
    uint32_t image_base;         /* after rebasing (the manifest's `base`) */
    uint32_t entry;              /* guest VA of the PE entry point */
    const rc_fn_entry *table;    /* lifted functions, sorted by address */
    uint32_t count;
} rc_generated_binary;

/* Everything lifted for one game (`set.c` below generated/<game>/). */
typedef struct rc_generated_set {
    const char *game_id;
    size_t count;
    const rc_generated_binary *const *binaries;
} rc_generated_set;

/* Exit codes of rc_game_main(). */
enum {
    RC_EXIT_OK = 0,
    RC_EXIT_ERROR = 1,
    RC_EXIT_USAGE = 2,
    RC_EXIT_NOT_LIFTED = 3
};

int rc_game_main(const rc_game_info *game, const rc_generated_set *generated, int argc,
                 char **argv);

/* The language of the install being played, as it calls itself ("English", "Deutsch", ...); NULL
 * if unknown. For a game project's patch that writes text into the game. */
const char *rc_game_language(void);

#ifdef __cplusplus
}
#endif
#endif
