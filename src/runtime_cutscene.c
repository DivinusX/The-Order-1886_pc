// SPDX-License-Identifier: GPL-2.0-or-later
/* The prologue lift cutscene at more than 30 FPS (#38): the lift carries the camera away from the
 * characters, so its shots show the empty cage or the inside of Galahad's head, and the voices
 * fade. Only this scene does it (the owner played the game through at 60 and 120 FPS), so the
 * runtime holds the native 30 FPS for it alone.
 *
 * It starts when the game opens the scene's sound mix (a 56 s, 6-channel stream in the primary
 * archive) and ends at the checkpoint save the game writes when the next scene begins (52 s
 * later in play). A save opened in the first 10 s is the one the game writes as the lift starts;
 * after 120 s the hold ends whatever happened (a quit to the menu, a skipped scene). */
#include "runtime.h"
#include "gpu/ps4gpu.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The scene's 6-channel, 56 s sound mix opens as it begins; the second part's dialogue 16 s
 * later (a hold that starts there is too late: the camera has already left). */
#define LIFT_MIX "/000000003989d3f9"
#define LIFT_DIALOGUE "/00000000268ec257"
#define CHECKPOINT "/SYSTEM.DAT"
#define MIN_SCENE_MS 10000
#define MAX_SCENE_MS 120000

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int held;
static int64_t held_since;

static int ends_with(const char *text,const char *suffix) {
    size_t n=strlen(text),m=strlen(suffix);
    return n>=m && !strcmp(text+n-m,suffix);
}

static int64_t now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC,&t);
    return (int64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}

static void release(const char *why) {
    held=0;
    ps4gpu_hold_native_frame_rate(0);
    printf("Runtime: lift cutscene over (%s): back to the chosen frame rate\n",why);
    fflush(stdout);
}

static void cutscene_open_at(const char *guest,int64_t now) {
    if (!guest) return;
    pthread_mutex_lock(&lock);
    if ((ends_with(guest,LIFT_MIX) || ends_with(guest,LIFT_DIALOGUE)) && strstr(guest,"/primary/")) {
        if (!held && ps4gpu_hold_native_frame_rate(1)) {
            held=1;
            printf("Runtime: lift cutscene: holding 30 FPS until it ends (#38)\n");
            fflush(stdout);
        }
        held_since=now; /* a checkpoint reload replays the scene from the start */
    } else if (held) {
        const int64_t elapsed=now-held_since;
        if (elapsed>=MAX_SCENE_MS) release("time limit");
        else if (elapsed>=MIN_SCENE_MS && !strncmp(guest,"/savedata",9) && ends_with(guest,CHECKPOINT))
            release("checkpoint");
    }
    pthread_mutex_unlock(&lock);
}

void runtime_cutscene_open(const char *guest) {
    cutscene_open_at(guest,now_ms());
}
