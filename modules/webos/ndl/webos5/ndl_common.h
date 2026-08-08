#pragma once

#include <pthread.h>
#include <stdint.h>
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
    /* Panel-phase pacing (wall-clock PTS, milliseconds). */
    bool panelPhasePacing;
    uint64_t panelPhaseIntervalMs;
    uint64_t panelPhaseAnchorMs;
    bool panelPhaseAnchored;
    /* Host presentationTimeUs → player PTS mapping (ms). */
    bool hostPtsAnchored;
    int64_t hostPtsAnchorUs;
    double hostPtsPlayerAnchorMs;
};

extern const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver;
extern const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver;
extern const SS4S_VideoDriver SS4S_NDL_webOS5_VideoDriver;

int SS4S_NDL_webOS5_ReloadMedia(SS4S_PlayerContext *context);

int SS4S_NDL_webOS5_UnloadMedia(SS4S_PlayerContext *context);

uint64_t SS4S_NDL_webOS5_GetPts(const SS4S_PlayerContext *context);

/** Wall-clock / host-mapped PTS, optionally smoothed on a virtual grid. */
uint64_t SS4S_NDL_webOS5_NextVideoPts(SS4S_PlayerContext *context, int64_t hostPtsUs);

void SS4S_NDL_webOS5_ConfigureSmoothPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen);

int SS4S_NDL_webOS5_Driver_PostInit(int argc, char *argv[]);

void SS4S_NDL_webOS5_Driver_Quit();
