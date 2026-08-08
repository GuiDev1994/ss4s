#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Units must be consistent per caller (SMP=ns, NDL=µs). */
static inline uint64_t SS4S_PanelPhaseSnapPts(uint64_t wall, uint64_t anchor, uint64_t interval,
                                             uint64_t max_hold, uint64_t last_pts, uint64_t min_step,
                                             bool *initialized) {
    if (interval == 0) {
        return wall;
    }
    if (!*initialized) {
        *initialized = true;
        return wall;
    }
    uint64_t phase = (wall >= anchor) ? ((wall - anchor) % interval) : 0;
    uint64_t wait = (phase == 0) ? 0 : (interval - phase);
    if (wait > max_hold) {
        wait = 0; /* present ASAP — do not backlog */
    }
    uint64_t pts = wall + wait;
    if (pts < last_pts + min_step) {
        pts = last_pts + min_step;
    }
    return pts;
}
