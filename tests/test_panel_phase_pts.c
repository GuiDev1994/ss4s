#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../modules/webos/common/panel_phase_pts.h"

static void test_first_frame_returns_wall(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t pts = SS4S_PanelPhaseClockNext(&clock, 100, 1);
    assert(pts == 100);
}

static void test_steady_arrivals_keep_even_cadence(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t last = SS4S_PanelPhaseClockNext(&clock, 0, 1);
    for (int i = 1; i < 24; i++) {
        uint64_t wall = (uint64_t) i * 8333ULL;
        uint64_t pts = SS4S_PanelPhaseClockNext(&clock, wall, 1);
        uint64_t delta = pts - last;
        assert(delta >= 8332 && delta <= 8334);
        last = pts;
    }
}

static void test_same_slot_goes_to_next_interval_not_min_step(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t p0 = SS4S_PanelPhaseClockNext(&clock, 0, 1000);
    uint64_t p1 = SS4S_PanelPhaseClockNext(&clock, 1000, 1000);
    uint64_t p2 = SS4S_PanelPhaseClockNext(&clock, 2000, 1000);
    assert(p0 == 0);
    assert(p1 == 8333);
    assert(p2 == 16666);
    assert(p1 - p0 != 1000);
    assert(p2 - p1 >= 8000);
}

static void test_late_frame_presents_asap(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    (void) SS4S_PanelPhaseClockNext(&clock, 0, 1);
    (void) SS4S_PanelPhaseClockNext(&clock, 8333, 1);
    uint64_t pts = SS4S_PanelPhaseClockNext(&clock, 40000, 1);
    assert(pts == 40000);
}

static void test_late_frame_does_not_shift_following_cadence(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t p0 = SS4S_PanelPhaseClockNext(&clock, 0, 1);
    uint64_t p1 = SS4S_PanelPhaseClockNext(&clock, 8333, 1);
    uint64_t p2 = SS4S_PanelPhaseClockNext(&clock, 17000, 1);
    uint64_t p3 = SS4S_PanelPhaseClockNext(&clock, 24999, 1);
    assert(p0 == 0);
    assert(p1 == 8333);
    assert(p2 == 17000);
    assert(p3 == 24999);
}

static void test_arrival_jitter_does_not_retune_interval(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t wall = 0;
    (void) SS4S_PanelPhaseClockNext(&clock, wall, 1);
    for (int i = 0; i < 64; i++) {
        wall += 8342;
        (void) SS4S_PanelPhaseClockNext(&clock, wall, 1);
    }
    assert(clock.interval == 8333);
}

static void test_pts_never_go_backwards(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    uint64_t last = SS4S_PanelPhaseClockNext(&clock, 0, 1000);
    uint64_t walls[] = {100, 200, 8333, 8400, 16000, 25000, 25100};
    for (size_t i = 0; i < sizeof(walls) / sizeof(walls[0]); i++) {
        uint64_t pts = SS4S_PanelPhaseClockNext(&clock, walls[i], 1000);
        assert(pts >= last + 1000);
        last = pts;
    }
}

static void test_align_locks_next_slot_to_vsync(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    (void) SS4S_PanelPhaseClockNext(&clock, 0, 1);
    (void) SS4S_PanelPhaseClockNext(&clock, 8333, 1);
    SS4S_PanelPhaseClockAlign(&clock, 10000);
    uint64_t pts = SS4S_PanelPhaseClockNext(&clock, 10000, 1);
    assert(pts == 18333);
    assert(clock.interval == 8333);
}

static void test_align_does_not_rewind_grid(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    (void) SS4S_PanelPhaseClockNext(&clock, 0, 1);
    uint64_t after = SS4S_PanelPhaseClockNext(&clock, 8333, 1);
    assert(after == 8333);
    SS4S_PanelPhaseClockAlign(&clock, 100);
    uint64_t pts = SS4S_PanelPhaseClockNext(&clock, 8400, 1);
    assert(pts >= 16666);
}

static void test_emit_now_after_present_returns_wall(void) {
    SS4S_PanelPhaseClock clock;
    SS4S_PanelPhaseClockInit(&clock, 8333);
    (void) SS4S_PanelPhaseClockNext(&clock, 0, 1);
    uint64_t pts = SS4S_PanelPhaseClockEmitNow(&clock, 9000, 1);
    assert(pts == 9000);
    assert(clock.last_pts == 9000);
    assert(clock.last_emitted == 9000);
}

int main(void) {
    test_first_frame_returns_wall();
    test_steady_arrivals_keep_even_cadence();
    test_same_slot_goes_to_next_interval_not_min_step();
    test_late_frame_presents_asap();
    test_late_frame_does_not_shift_following_cadence();
    test_arrival_jitter_does_not_retune_interval();
    test_pts_never_go_backwards();
    test_align_locks_next_slot_to_vsync();
    test_align_does_not_rewind_grid();
    test_emit_now_after_present_returns_wall();
    printf("ok\n");
    return 0;
}
