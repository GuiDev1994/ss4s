#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
        uint64_t bumped = last_pts + min_step;
        uint64_t next_slot = last_pts + interval;
        if (bumped < next_slot) {
            pts = bumped;
        } else {
            pts = next_slot;
            if (pts < last_pts + min_step) {
                pts = last_pts + min_step;
            }
        }
    }
    return pts;
}

typedef struct SS4S_PanelPhaseClock {
    bool initialized;
    uint64_t interval;
    uint64_t last_pts;
    uint64_t last_emitted;
} SS4S_PanelPhaseClock;

static inline void SS4S_PanelPhaseClockInit(SS4S_PanelPhaseClock *clock, uint64_t interval) {
    memset(clock, 0, sizeof(*clock));
    if (interval == 0) {
        interval = 1;
    }
    clock->interval = interval;
}

static inline uint64_t SS4S_PanelPhaseClockNext(SS4S_PanelPhaseClock *clock, uint64_t wall,
                                               uint64_t min_step) {
    if (clock->interval == 0) {
        clock->interval = 1;
    }
    if (!clock->initialized) {
        clock->initialized = true;
        clock->last_pts = wall;
        clock->last_emitted = wall;
        return wall;
    }
    uint64_t scheduled = clock->last_pts + clock->interval;
    /* Skip missed slots so a hitch does not leave the grid in the past. */
    while (scheduled + clock->interval < wall) {
        clock->last_pts = scheduled;
        scheduled = clock->last_pts + clock->interval;
    }
    uint64_t pts = scheduled;
    if (wall > scheduled) {
        pts = wall; /* Starfish cannot present in the past */
    }
    if (min_step > 0 && pts < clock->last_emitted + min_step) {
        uint64_t bumped = clock->last_emitted + min_step;
        if (bumped < scheduled) {
            pts = bumped;
        } else {
            pts = scheduled;
            if (pts < clock->last_emitted + min_step) {
                pts = clock->last_emitted + min_step;
            }
        }
    }
    clock->last_pts = scheduled;
    clock->last_emitted = pts;
    return pts;
}

/* Lock grid phase to an observed present. Never rewinds last_pts; interval unchanged. */
static inline void SS4S_PanelPhaseClockAlign(SS4S_PanelPhaseClock *clock, uint64_t vsync_pts) {
    if (!clock->initialized || clock->interval == 0) {
        return;
    }
    if (vsync_pts > clock->last_pts) {
        clock->last_pts = vsync_pts;
    }
}

/* After a real present, emit immediately (wall). Keeps PTS monotonic. */
static inline uint64_t SS4S_PanelPhaseClockEmitNow(SS4S_PanelPhaseClock *clock, uint64_t wall, uint64_t min_step) {
    if (clock->interval == 0) {
        clock->interval = 1;
    }
    uint64_t pts = wall;
    if (clock->initialized && min_step > 0 && pts < clock->last_emitted + min_step) {
        pts = clock->last_emitted + min_step;
    }
    clock->initialized = true;
    clock->last_pts = pts;
    clock->last_emitted = pts;
    return pts;
}
