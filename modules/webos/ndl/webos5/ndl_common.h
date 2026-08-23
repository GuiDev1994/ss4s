#pragma once

#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <NDL_directmedia_v2.h>

#include "ss4s/player.h"
#include "ss4s/modapi.h"
#include "ndl_logging.h"
#include "../../common/panel_phase_pts.h"

extern bool SS4S_NDL_webOS5_Initialized;
extern pthread_mutex_t SS4S_NDL_webOS5_Lock;
extern const SS4S_LibraryContext *SS4S_NDL_webOS5_Lib;

struct SS4S_PlayerContext {
    SS4S_Player *player;
    uint64_t lastFrameTime;
    NDL_DIRECTMEDIA_DATA_INFO_T mediaInfo;
    struct SS4S_OpusEmpty *opusEmpty;
    struct SS4S_NDLOpusFix *opusFix;
    bool mediaLoaded;
    struct timespec mediaLoadedTime;
    bool waitAudioVideoReady;
    _Atomic bool panelPhaseLoosen;
    int aspectRatio;
    bool hasHdrInfo;
    /* Smooth presentation pacing (virtual PTS grid). */
    bool smoothPacing;
    /* When true: map host PTS only (no interval grid / drift clamp). */
    bool smoothHostOnly;
    /* Extra delay added to host-mapped PTS (ms), from SS4S_PRESENTATION_OFFSET_US. */
    double presentationOffsetMs;
    bool smoothPtsInitialized;
    double smoothIntervalMs;
    double smoothMaxDriftMs;
    double smoothLastPts;
    /* Panel-phase pacing (internal µs; NDL PTS API uses ms at boundary). Always on. */
    bool panelPhasePacing;
    uint64_t panelPhaseIntervalUs;
    SS4S_PanelPhaseClock panelPhaseClock;
    /* Host presentationTimeUs → player PTS mapping (ms). */
    bool hostPtsAnchored;
    int64_t hostPtsAnchorUs;
    double hostPtsPlayerAnchorMs;
    /* Render-buffer pacing: hand NDL a future PTS so its renderer owns the vsync. */
    int renderQueueTarget;
    double renderFrameMs;
    bool renderAnchored;
    int64_t renderHostAnchorUs;
    double renderAnchorMs;
    double renderLastPts;
    double renderTrimMs;
    uint64_t renderFrames;
    uint64_t renderQueueSum;
    uint64_t renderStarved;
    uint64_t renderResyncs;
    int renderQueueMax;
    pthread_cond_t presentCond;
    bool presentSeen;
    bool presentGateEnabled;
    unsigned presentTimeouts;
    unsigned presentGen;
};

extern const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver;
extern const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver;
extern const SS4S_VideoDriver SS4S_NDL_webOS5_VideoDriver;

int SS4S_NDL_webOS5_ReloadMedia(SS4S_PlayerContext *context);

int SS4S_NDL_webOS5_UnloadMedia(SS4S_PlayerContext *context);

uint64_t SS4S_NDL_webOS5_GetPts(const SS4S_PlayerContext *context);

/** Wall-clock / host-mapped PTS, optionally smoothed on a virtual grid. */
uint64_t SS4S_NDL_webOS5_NextVideoPts(SS4S_PlayerContext *context, int64_t hostPtsUs);

/** True when render-buffer pacing is configured for this session. */
bool SS4S_NDL_webOS5_RenderPacingEnabled(const SS4S_PlayerContext *context);

/**
 * PTS (ms) placed far enough ahead that NDL's renderer queues the frame instead of
 * showing it on arrival. `queueLen` is the render buffer depth read just before the
 * call, or negative when unknown.
 */
uint64_t SS4S_NDL_webOS5_RenderPacedPts(SS4S_PlayerContext *context, int64_t hostPtsUs, int queueLen);

/** Log render-buffer pacing health (mean queue depth, starvation, resyncs). */
void SS4S_NDL_webOS5_LogRenderPacing(const SS4S_PlayerContext *context);

/** Wait for STARFISH RENDERED_FRAME. Caller holds SS4S_NDL_webOS5_Lock. */
bool SS4S_NDL_webOS5_WaitPresent(SS4S_PlayerContext *context);

void SS4S_NDL_webOS5_ConfigureSmoothPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen);

int SS4S_NDL_webOS5_Driver_PostInit(int argc, char *argv[]);

void SS4S_NDL_webOS5_Driver_Quit();
