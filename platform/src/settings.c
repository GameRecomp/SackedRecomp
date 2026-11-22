/* SPDX-License-Identifier: MIT */
/*
 * Display settings from settings.ini in the user directory, and from the
 * environment (rc/platform.h, rc_plat_display_load).
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rc/platform.h"

#include <stdbool.h>

/* At most so many frames a second unless settings.ini says otherwise: as
 * many as a common screen shows; a game drawing as many as it can keeps a
 * processor busy (and a phone warm). */
#define DEFAULT_FPS 60
#define DEFAULT_FPS_TEXT "60"

#if !defined(__EMSCRIPTEN__) /* (a browser has no settings.ini) */
static const char DEFAULTS[] =
    "# How the game is shown. Change a value and start the game again.\n"
    "\n"
    "# fit: as large as fits the window or the screen.\n"
    "# 1, 2, 3 or 4: that many pixels of the screen for each pixel of the game\n"
    "# (sharpest; the window is sized to it).\n"
    "scale = fit\n"
    "\n"
    "# sharp or smooth: how the picture is scaled.\n"
    "pixels = sharp\n"
    "\n"
    "# yes or no. Alt+Enter switches while playing.\n"
    "fullscreen = no\n"
    "\n"
    "# The resolution the game plays at (its menus may keep their own).\n"
    "# window x1 (the default): the size of the window (of the screen when full\n"
    "# screen), following it when it changes. window x1.5, x2, x2.5, x3: that\n"
    "# size divided by 1.5, 2, 2.5 or 3 (larger interface and figures). The\n"
    "# game screen is at least 800x600. screen: the size of the screen. A size\n"
    "# such as 1920x1080 or 2560x1080 (any shape). off: only the game's own\n"
    "# resolutions and its own setting. A game's options page may choose too.\n"
    "resolution = window x1\n"
    "\n"
    "# yes (the default): the mouse pointer is the system's (smooth, at the\n"
    "# screen's rate). no: the game's own setting.\n"
    "hardware_cursor = yes\n"
    "\n"
    "# At most this many frames a second, so that the processor rests between\n"
    "# frames (cooler and quieter; fewer are less smooth): for example 30.\n"
    "# max: as many as the screen shows.\n"
    "fps = " DEFAULT_FPS_TEXT "\n";
#endif

/* A factor such as 1.5 (or 1,5) in tenths, 10 to 80; 10 for anything else. */
static int scale_tenths(const char *text)
{
    char buf[16];
    snprintf(buf, sizeof buf, "%s", text);
    char *comma = strchr(buf, ',');
    if (comma) {
        *comma = '.';
    }
    double v = atof(buf);
    return v >= 1.0 && v <= 8.0 ? (int)(v * 10.0 + 0.5) : 10;
}

/* A game project's own settings (rc_plat_setting): the keys of settings.ini the display settings
 * do not use, as read (lower case). */
static struct {
    char key[32], value[64];
} own[16];
static int n_own;

static void set_own(const char *key, const char *value)
{
    int i = 0;
    while (i < n_own && strcmp(own[i].key, key)) {
        i++;
    }
    if (i == n_own) {
        if (n_own == (int)(sizeof own / sizeof own[0])) {
            return;
        }
        n_own++;
    }
    snprintf(own[i].key, sizeof own[i].key, "%s", key);
    snprintf(own[i].value, sizeof own[i].value, "%s", value);
}

static void set(rc_display *d, const char *key, const char *value)
{
    if (!strcmp(key, "scale")) {
        d->scale = !strcmp(value, "fit") ? 0 : atoi(value);
        if (d->scale < 0 || d->scale > 8) {
            d->scale = 0;
        }
    } else if (!strcmp(key, "pixels")) {
        d->smooth = !strcmp(value, "smooth");
    } else if (!strcmp(key, "fullscreen")) {
        d->fullscreen = !strcmp(value, "yes") || !strcmp(value, "1") || !strcmp(value, "true");
    } else if (!strcmp(key, "fps")) {
        d->max_fps = atoi(value);
        if (d->max_fps < 0 || d->max_fps > 1000) {
            d->max_fps = 0;
        }
    } else if (!strcmp(key, "game_scale")) { /* (an older settings.ini: now `window x1.5`) */
        d->game_scale = scale_tenths(value);
    } else if (!strcmp(key, "hardware_cursor")) {
        d->hardware_cursor = !strcmp(value, "yes") || !strcmp(value, "1") || !strcmp(value, "true");
    } else if (!strcmp(key, "resolution")) {
        int w = 0, h = 0;
        bool window = !strncmp(value, "window", 6), screen = !strncmp(value, "screen", 6);
        if (window || screen) {
            /* "window", "window x1.5" (also "window 1.5", "window*1.5"). */
            const char *f = value + 6;
            while (*f == ' ' || *f == 'x' || *f == '*') {
                f++;
            }
            d->game_w = d->game_h = window ? -1 : -2;
            if (*f) {
                d->game_scale = scale_tenths(f);
            } else if (!d->game_scale) {
                d->game_scale = 10;
            }
        } else if (sscanf(value, "%dx%d", &w, &h) == 2 && w > 0 && h > 0 && w <= 8192 && h <= 8192) {
            d->game_w = w;
            d->game_h = h;
        } else {
            d->game_w = d->game_h = 0;
        }
    } else {
        set_own(key, value);
    }
}

#if !defined(__EMSCRIPTEN__)
/* `key = value` lines; '#' starts a comment. */
static void parse(rc_display *d, FILE *f)
{
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *hash = strchr(line, '#');
        if (hash) {
            *hash = 0;
        }
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = 0;
        char key[64] = "", value[64] = "";
        if (sscanf(line, " %63s", key) != 1) {
            continue;
        }
        /* The value: the rest of the line, without the spaces around it ("window x1.5"). */
        const char *v = eq + 1;
        while (*v == ' ' || *v == '\t') {
            v++;
        }
        snprintf(value, sizeof value, "%s", v);
        size_t vn = strlen(value);
        while (vn && (value[vn - 1] == ' ' || value[vn - 1] == '\t' || value[vn - 1] == '\r' || value[vn - 1] == '\n')) {
            value[--vn] = 0;
        }
        if (!vn) {
            continue;
        }
        for (char *p = value; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        set(d, key, value);
    }
}
#endif

static char settings_path[1024];

void rc_plat_display_store(void)
{
    if (!settings_path[0]) {
        return;
    }
    /* The file as it is, with the lines of the settings that change while
     * playing (full screen, and the resolution on the options page)
     * replaced. */
    char text[8192] = "";
    size_t n = 0;
    FILE *f = fopen(settings_path, "r");
    if (f) {
        n = fread(text, 1, sizeof text - 1, f);
        fclose(f);
    }
    text[n] = 0;
    rc_display d = rc_plat_display_get();
    char lines[2][64];
    snprintf(lines[0], sizeof lines[0], "fullscreen = %s\n", d.fullscreen ? "yes" : "no");
    if (d.game_w < 0) {
        int s = d.game_scale >= 10 ? d.game_scale : 10;
        snprintf(lines[1], sizeof lines[1], s % 10 ? "resolution = %s x%d.%d\n" : "resolution = %s x%d\n",
                 d.game_w == -2 ? "screen" : "window", s / 10, s % 10);
    } else if (d.game_w > 0 && d.game_h > 0) {
        snprintf(lines[1], sizeof lines[1], "resolution = %dx%d\n", d.game_w, d.game_h);
    } else {
        snprintf(lines[1], sizeof lines[1], "resolution = off\n");
    }
    /* (game_scale, from an older settings.ini, is in the resolution now: its line goes.) */
    static const char *const keys[3] = {"fullscreen", "resolution", "game_scale"};
    bool done[3] = {false, false, false};
    char out[8400] = "";
    size_t o = 0;
    for (char *line = text; *line;) {
        char *nl = strchr(line, '\n');
        size_t len = nl ? (size_t)(nl - line) + 1 : strlen(line);
        const char *p = line;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        const char *eq = strchr(p, '=');
        const char *put = line;
        size_t put_len = len;
        for (int k = 0; k < 3; k++) {
            if (!strncmp(p, keys[k], strlen(keys[k])) && eq && (size_t)(eq - line) < len) {
                put = k < 2 ? lines[k] : "";
                put_len = strlen(put);
                done[k] = true;
            }
        }
        if (o + put_len < sizeof out) {
            memcpy(out + o, put, put_len);
            o += put_len;
            out[o] = 0;
        }
        line += len;
    }
    for (int k = 0; k < 2; k++) {
        size_t len = strlen(lines[k]);
        if (!done[k] && o + len < sizeof out) {
            memcpy(out + o, lines[k], len + 1);
            o += len;
        }
    }
    if ((f = fopen(settings_path, "w")) != NULL) {
        fputs(out, f);
        fclose(f);
    }
}

void rc_plat_display_load(const char *user_dir)
{
    rc_display d = {0};
    d.max_fps = DEFAULT_FPS;
    d.game_scale = 10;
    d.hardware_cursor = true;
#if !defined(__EMSCRIPTEN__)
    /* (A browser's page decides for itself. On Android the file keeps the
     * resolution too; full screen is always on there, whatever it says.) */
    if (user_dir && *user_dir) {
        char path[1024];
        size_t n = strlen(user_dir);
        snprintf(path, sizeof path, "%s%ssettings.ini", user_dir,
                 n && (user_dir[n - 1] == '/' || user_dir[n - 1] == '\\') ? "" : "/");
        snprintf(settings_path, sizeof settings_path, "%s", path);
        FILE *f = fopen(path, "r");
        if (f) {
            parse(&d, f);
            fclose(f);
        } else {
            /* A new user directory: the defaults, as written. */
            set(&d, "resolution", "window");
            if ((f = fopen(path, "w")) != NULL) {
                fputs(DEFAULTS, f);
                fclose(f);
            }
        }
    }
#else
    /* In a browser: the page's size, as large (window x1). */
    (void)user_dir;
    d.game_w = d.game_h = -1;
#endif
    static const char *const env[][2] = {{"RC_SCALE", "scale"}, {"RC_PIXELS", "pixels"}, {"RC_FULLSCREEN", "fullscreen"},
                                         {"RC_RESOLUTION", "resolution"}, {"RC_FPS", "fps"},
                                         {"RC_GAME_SCALE", "game_scale"}, {"RC_HARDWARE_CURSOR", "hardware_cursor"}};
    for (size_t i = 0; i < sizeof env / sizeof env[0]; i++) {
        const char *v = getenv(env[i][0]);
        if (v && *v) {
            char low[64];
            snprintf(low, sizeof low, "%s", v);
            for (char *p = low; *p; p++) {
                *p = (char)tolower((unsigned char)*p);
            }
            set(&d, env[i][1], low);
        }
    }
    rc_plat_display(&d);
}

bool rc_plat_game_size(int *width, int *height)
{
    rc_display d = rc_plat_display_get();
    int w = d.game_w, h = d.game_h;
    if (w == 0 || h == 0) {
        return false;
    }
    if (w < 0) {
        if (w == -2) {
            rc_plat_screen_size(&w, &h);
        } else {
            rc_plat_area(&w, &h);
        }
        int scale = d.game_scale >= 10 ? d.game_scale : 10;
        w = w * 10 / scale;
        h = h * 10 / scale;
    }
    /* At least 800x600 in its shape: the games' interfaces need that much. */
    if (w < 800 || h < 600) {
        double k = w * 600.0 > h * 800.0 ? 600.0 / h : 800.0 / w;
        w = (int)(w * k + 0.5);
        h = (int)(h * k + 0.5);
    }
    w &= ~7;
    h &= ~1;
    if (w > 8192 || h > 8192 || w <= 0 || h <= 0) {
        return false;
    }
    *width = w;
    *height = h;
    return true;
}

const char *rc_plat_setting(const char *key, const char *fallback, const char *about)
{
    const char *value = NULL;
    for (int i = 0; i < n_own && !value; i++) {
        if (!strcmp(own[i].key, key)) {
            value = own[i].value;
        }
    }
    if (!value) {
#if !defined(__EMSCRIPTEN__)
        /* Not in settings.ini yet: added, with its explanation, so that the player finds it. */
        FILE *f = settings_path[0] ? fopen(settings_path, "a") : NULL;
        if (f) {
            fputs("\n", f);
            for (const char *line = about; line && *line;) {
                const char *nl = strchr(line, '\n');
                int len = nl ? (int)(nl - line) : (int)strlen(line);
                fprintf(f, "# %.*s\n", len, line);
                line += len + (nl ? 1 : 0);
            }
            fprintf(f, "%s = %s\n", key, fallback);
            fclose(f);
        }
#else
        (void)about;
#endif
        set_own(key, fallback);
        value = fallback;
    }
    /* The environment over the file (as RC_RESOLUTION and the others). */
    char name[48] = "RC_";
    for (size_t i = 0; key[i] && i + 4 < sizeof name; i++) {
        name[i + 3] = (char)toupper((unsigned char)key[i]);
        name[i + 4] = 0;
    }
    static char env[64];
    const char *v = getenv(name);
    if (v && *v) {
        snprintf(env, sizeof env, "%s", v);
        for (char *p = env; *p; p++) {
            *p = (char)tolower((unsigned char)*p);
        }
        value = env;
    }
    return value;
}
