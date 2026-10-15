#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct SS4S_OpusEmpty SS4S_OpusEmpty;

typedef int (*SS4S_NDLOpusEmptyFeedFunc)(void *arg, const unsigned char *data, size_t size);

/**
 * Create an empty-frame helper. Spawns a background thread that waits for
 * SS4S_OpusEmptyStart before producing silence on gaps.
 *
 * @param frameDurationUs Duration of one Opus frame in microseconds (e.g. 5000)
 */
SS4S_OpusEmpty *SS4S_OpusEmptyCreate(uint32_t channels, uint32_t streams, uint32_t coupled,
                                     uint32_t frameDurationUs);

/**
 * Begin runtime gap filling. After ~10 missing frames of real audio, empty
 * Opus packets are fed so NDL/eARC does not underrun into mono distortion.
 */
void SS4S_OpusEmptyStart(SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg);

/**
 * Real audio arrived: postpone empty filling. Returns true while an empty
 * packet is being submitted. Callers must still play the real frame; both
 * paths take the NDL lock, and dropping the real frame is an audible cut.
 */
bool SS4S_OpusEmptyFrameArrived(SS4S_OpusEmpty *instance);

/**
 * Feed exactly one empty frame (used by the LOADCOMPLETED prime path).
 * Caller supplies the feed callback; no gap timer is involved.
 */
int SS4S_OpusEmptyFeed(SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg);

void SS4S_OpusEmptyDestroy(SS4S_OpusEmpty *instance);
