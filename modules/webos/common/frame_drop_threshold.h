#pragma once

#include <stdbool.h>
#include <stdlib.h>

/* SS4S_NDL_FRAME_DROP_THRESHOLD: unset = 2 at 4K and off below, "0" or below 2 = off, 2 to 8 = that value. */
static inline int SS4S_FrameDropThreshold(int width, bool av1, const char **setting) {
    const char *env = getenv("SS4S_NDL_FRAME_DROP_THRESHOLD");
    int threshold = width >= 3840 ? 2 : 0;
    if (env != NULL && env[0] != '\0') {
        long frames = strtol(env, NULL, 10);
        if (frames < 2) {
            threshold = 0;
        } else if (frames <= 8) {
            threshold = (int) frames;
        }
    } else {
        env = "unset";
    }
    *setting = env;
    return av1 ? 0 : threshold;
}
