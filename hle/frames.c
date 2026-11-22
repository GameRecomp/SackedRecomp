/* SPDX-License-Identifier: MIT */
/*
 * What happens after each presented frame: screenshots and the end of a
 * scripted run.
 *
 *   RC_SCREENSHOT=N:path.bmp   write frame N (several, separated by ',')
 *   RC_FRAMES=N                end the run, successfully, after frame N
 *   RC_INPUT=path              scripted input, keyed to frames:
 *       FRAME move X Y         the mouse moves to (X, Y)
 *       FRAME down|up BUTTON   a mouse button (1 left, 2 right, 3 middle)
 *       FRAME wheel N          the wheel turns N notches (positive away
 *                              from the user) where the pointer is
 *       FRAME key|keyup VK     a key, by Windows virtual-key code
 *       FRAME focus 0|1        the window loses (0) or gains (1) the focus,
 *                              as when the player switches to another program
 *       FRAME fps N            from here on frames take at least 1/N s of the
 *                              virtual clock (as RC_CLOCK_FPS; 0: no pacing)
 *       FRAME area W H         the window becomes W x H (rc_plat_area_set)
 *       FRAME touch|pen|mouse  what the pointer events from here on come
 *                              from (a finger, a pen; at first the mouse)
 *       FRAME wait PATH        hold the script (not the game) here until the
 *                              file PATH exists; the frames of the lines
 *                              after it count from when it appeared. Runs
 *                              of several programs (a network game) are
 *                              lined up that way by their test script
 *       FRAME shot PATH        a screenshot of the next frame (frames
 *                              after a wait are not known in advance)
 *   RC_RECORD=path             write the input of this run as such a script
 *                              (with RC_CLOCK=virtual it replays exactly)
 *   Lines starting with '#' are comments. Keying input to frames rather
 *   than time makes a run replay the same on a fast and a slow machine.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ddraw/ddraw.h"
#include "rc/log.h"
#include "rc/platform.h"
#include "rc/process.h"
#include "user32/user32.h"

static FILE *script, *record;
static unsigned current_frame;
static unsigned next_frame;
static char next_line[256];
static int script_done;
static rc_event_source source;

static unsigned shift;      /* frames the script waited (added to the frames of later lines) */
static char wait_path[240]; /* the file a wait line waits for */
static char shot_path[240]; /* a screenshot a shot line asked for (taken at the next frame) */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
    }
    return f != NULL;
}

static bool read_line(void)
{
    while (fgets(next_line, sizeof next_line, script)) {
        if (next_line[0] == '#' || next_line[0] == '\n') {
            continue;
        }
        next_frame = (unsigned)strtoul(next_line, NULL, 10) + shift;
        return true;
    }
    return false;
}

static void run_script(unsigned frame)
{
    if (script_done) {
        return;
    }
    if (!script) {
        const char *path = getenv("RC_INPUT");
        if (!path || !(script = fopen(path, "r")) || !read_line()) {
            script_done = 1;
            return;
        }
    }
    if (wait_path[0]) {
        if (!file_exists(wait_path)) {
            return;
        }
        rc_log("RC_INPUT: %s is there at frame %u", wait_path, frame);
        wait_path[0] = 0;
        shift += frame - next_frame;
        if (!read_line()) {
            script_done = 1;
            return;
        }
    }
    while (next_frame <= frame) {
        char what[16] = {0};
        long a = 0, b = 0;
        sscanf(next_line, "%*u %15s %li %li", what, &a, &b);
        rc_event e = {0};
        /* Where the script last put the pointer, as the device has it (the
         * program may see it elsewhere: rc_set_pointer_map). */
        static int moved, px, py;
        if (!strcmp(what, "move")) {
            e.type = RC_EV_MOUSE_MOVE;
            e.x = (int)a;
            e.y = (int)b;
            moved = 1;
            px = e.x;
            py = e.y;
        } else if (!strcmp(what, "down") || !strcmp(what, "up")) {
            int x = px, y = py;
            if (!moved) {
                rc_input_mouse(&x, &y);
            }
            e.type = RC_EV_MOUSE_BUTTON;
            e.button = (int)a;
            e.down = what[0] == 'd';
            e.x = x;
            e.y = y;
        } else if (!strcmp(what, "wheel")) {
            int x = px, y = py;
            if (!moved) {
                rc_input_mouse(&x, &y);
            }
            e.type = RC_EV_WHEEL;
            e.delta = (int)a * 120;
            e.x = x;
            e.y = y;
        } else if (!strcmp(what, "key") || !strcmp(what, "keyup")) {
            e.type = RC_EV_KEY;
            e.vk = (int)a;
            e.down = !strcmp(what, "key");
        } else if (!strcmp(what, "focus")) {
            e.type = RC_EV_FOCUS;
            e.down = a != 0;
        } else if (!strcmp(what, "area") || !strcmp(what, "touch") || !strcmp(what, "pen") ||
                   !strcmp(what, "mouse") || !strcmp(what, "wait") || !strcmp(what, "shot") ||
                   !strcmp(what, "fps")) {
            if (what[0] == 'f') {
                rc_clock_set_fps((uint32_t)a);
            } else if (what[0] == 's' && what[1] == 'h') {
                sscanf(next_line, "%*u %*s %239[^\r\n]", shot_path);
            } else if (what[0] == 'w') {
                if (sscanf(next_line, "%*u %*s %239[^\r\n]", wait_path) == 1) {
                    return; /* the next frames check for it */
                }
            } else if (what[0] == 'a') {
                rc_plat_area_set((int)a, (int)b);
            } else {
                source = what[0] == 't' ? RC_SRC_TOUCH : what[0] == 'p' ? RC_SRC_PEN : RC_SRC_MOUSE;
            }
            if (!read_line()) {
                script_done = 1;
                return;
            }
            continue;
        } else {
            rc_log("RC_INPUT: cannot read \"%s\"", next_line);
        }
        if (e.type != RC_EV_KEY && e.type != RC_EV_FOCUS) {
            e.source = source;
        }
        rc_inject_event(&e);
        if (!read_line()) {
            script_done = 1;
            return;
        }
    }
}

int rc_screenshot_due(unsigned frame)
{
    if (shot_path[0]) {
        return 1;
    }
    const char *shots = getenv("RC_SCREENSHOT");
    if (!shots) {
        return 0;
    }
    for (const char *p = shots; *p;) {
        if ((unsigned)strtoul(p, NULL, 10) == frame) {
            return 1;
        }
        const char *comma = strchr(p, ',');
        if (!comma) {
            break;
        }
        p = comma + 1;
    }
    return 0;
}

/* RC_RECORD: the user's input as script lines, keyed to the frame it arrived after. */
void rc_record_event(const rc_event *e)
{
    static int tried;
    if (!record && !tried) {
        tried = 1;
        const char *path = getenv("RC_RECORD");
        if (path && *path && !(record = fopen(path, "w"))) {
            rc_log("RC_RECORD: cannot write %s", path);
        }
    }
    if (!record) {
        return;
    }
    unsigned f = current_frame + 1;
    switch (e->type) {
    case RC_EV_MOUSE_MOVE:
        fprintf(record, "%u move %d %d\n", f, e->x, e->y);
        break;
    case RC_EV_MOUSE_BUTTON:
        fprintf(record, "%u move %d %d\n%u %s %d\n", f, e->x, e->y, f, e->down ? "down" : "up", e->button);
        break;
    case RC_EV_WHEEL:
        fprintf(record, "%u move %d %d\n%u wheel %d\n", f, e->x, e->y, f, e->delta / 120);
        break;
    case RC_EV_KEY:
        if (!e->repeat) {
            fprintf(record, "%u %s %d\n", f, e->down ? "key" : "keyup", e->vk);
        }
        break;
    case RC_EV_FOCUS:
        fprintf(record, "%u focus %d\n", f, e->down ? 1 : 0);
        break;
    default:
        return;
    }
    fflush(record);
}

void rc_present_hook(unsigned frame)
{
    current_frame = frame;
    rc_clock_frame();
    if (shot_path[0]) {
        rc_log("screenshot of frame %u: %s%s", frame, shot_path, rc_plat_screenshot(shot_path) ? "" : " failed");
        shot_path[0] = 0;
    }
    run_script(frame);
    const char *shots = getenv("RC_SCREENSHOT");
    if (shots) {
        static char buf[16384];
        snprintf(buf, sizeof buf, "%s", shots);
        for (char *tok = buf, *next; tok && *tok; tok = next) {
            next = strchr(tok, ',');
            if (next) {
                *next++ = 0;
            }
            char *colon = strchr(tok, ':');
            if (colon && (unsigned)strtoul(tok, NULL, 10) == frame) {
                if (rc_plat_screenshot(colon + 1)) {
                    rc_log("screenshot of frame %u: %s", frame, colon + 1);
                } else {
                    rc_log("screenshot of frame %u failed", frame);
                }
            }
        }
    }
    const char *end = getenv("RC_FRAMES");
    if (end && frame >= (unsigned)strtoul(end, NULL, 10)) {
        rc_log("reached frame %u", frame);
        rc_exit(0);
    }
}
