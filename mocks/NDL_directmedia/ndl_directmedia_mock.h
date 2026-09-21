#pragma once

#include <stdbool.h>

/* Avoid pulling API-versioned NDL headers into every mock TU: declare the
 * load-callback type locally (matches NDL_directmedia_types.h for API >= 2). */
typedef void (*NDLMediaLoadCallback)(int, long long, const char *);

extern bool ndl_mock_init;
extern bool audio_opened, video_opened;

void mock_ndl_lock(const char *func);

void mock_ndl_unlock(const char *func);

/** Reset mock pipeline + counters. Call at the start of each test. */
void mock_ndl_reset(void);

/**
 * When to auto-fire STATE_UPDATE_LOADCOMPLETED (0x16) after NDL_DirectMediaLoad.
 *  -1 (default): never auto-fire — test must call mock_ndl_fire_load_completed().
 *   0: fire synchronously before Load returns.
 *  >0: fire on a background thread after this many milliseconds.
 * Overridden by SS4S_NDL_MOCK_LOAD_DELAY_MS when that env var is set.
 */
void mock_ndl_set_load_completed_delay_ms(int delay_ms);

/** Manually deliver LOADCOMPLETED to the stored load callback. */
void mock_ndl_fire_load_completed(void);

bool mock_ndl_load_completed_fired(void);

int mock_ndl_audio_play_count(void);

int mock_ndl_video_play_count(void);

NDLMediaLoadCallback mock_ndl_load_callback(void);

/* Used by v1/v2 mock translation units. */
void mock_ndl_note_audio_play(void);
void mock_ndl_note_video_play(void);
void mock_ndl_store_load_callback(NDLMediaLoadCallback callback);
void mock_ndl_maybe_schedule_load_completed(void);
