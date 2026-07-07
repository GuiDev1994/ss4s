#pragma once

#include "video.h"
#include "audio.h"

#ifndef SS4S_MODAPI_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>

typedef struct SS4S_Player SS4S_Player;

typedef struct SS4S_PlayerInfo {
    struct {
        bool enabled;
        const char *module;
        SS4S_AudioCapabilities capabilities;
    } audio;
    struct {
        bool enabled;
        const char *module;
        SS4S_VideoCapabilities capabilities;
    } video;
    int viewportWidth, viewportHeight;
} SS4S_PlayerInfo;

SS4S_Player *SS4S_PlayerOpen();

void SS4S_PlayerClose(SS4S_Player *player);

/**
 * If audio module and video module is the same, ask it to start playing only when both are opened.
 * @param player Player instance
 * @param value Preferred option
 */
void SS4S_PlayerSetWaitAudioVideoReady(SS4S_Player *player, bool value);

bool SS4S_PlayerGetInfo(SS4S_Player *player, SS4S_PlayerInfo *info);

void SS4S_PlayerSetUserdata(SS4S_Player *player, void *userdata);

void SS4S_PlayerSetViewportSize(SS4S_Player *player, int width, int height);

void *SS4S_PlayerGetUserdata(SS4S_Player *player);

bool SS4S_PlayerGetVideoLatency(SS4S_Player *player, int avgIntervalUs, int *latencyUs);

/**
 * Get the number of frames currently queued in the decoder/renderer, as last reported by the video module.
 * @param player Player instance
 * @param queueDepth Queue depth field to assign
 * @return true if the module has reported a queue depth, false otherwise
 */
bool SS4S_PlayerGetVideoQueueDepth(SS4S_Player *player, int *queueDepth);

/**
 * Get the average time the platform decoder takes to accept one video frame, as last reported by the
 * video module. Values above the frame interval indicate the decoder is applying backpressure.
 * @param player Player instance
 * @param feedTimeUs Feed time field to assign, in microseconds
 * @return true if the module has reported a feed time, false otherwise
 */
bool SS4S_PlayerGetVideoFeedTime(SS4S_Player *player, int *feedTimeUs);

#ifdef __cplusplus
}
#endif

#endif // SS4S_MODAPI_H
