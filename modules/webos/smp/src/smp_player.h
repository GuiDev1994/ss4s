#pragma once

#include "ss4s/modapi.h"
#include "../../common/panel_phase_pts.h"
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

typedef enum PlayerState {
    SMP_STATE_UNLOADED,
    SMP_STATE_LOADED,
    SMP_STATE_PLAYING,
} PlayerState;

typedef enum FeedResult {
    SMP_FEED_OK,
    SMP_FEED_NOT_READY,
    SMP_FEED_BUFFER_FULL,
    SMP_FEED_ERROR = -1,
} FeedResult;

struct SS4S_PlayerContext {
    char *appId;
    pthread_mutex_t lock;
    SS4S_Player *player;

    SS4S_AudioInfo audioInfo;
    bool hasAudio;
    SS4S_VideoInfo videoInfo;
    bool hasVideo;

    PlayerState state;
    uint64_t openTime;
    int aspectRatio;
    bool hdr, shouldStop;

    bool waitAudioVideoReady;
    _Atomic bool panelPhaseLoosen;

    /* Smooth presentation pacing (virtual PTS grid, nanoseconds). */
    bool smoothPacing;
    /* When true: map host PTS only (no interval grid / drift clamp). */
    bool smoothHostOnly;
    /* Extra delay added to host-mapped PTS (ns), from SS4S_PRESENTATION_OFFSET_US. */
    double presentationOffsetNs;
    bool smoothPtsInitialized;
    double smoothIntervalNs;
    double smoothMaxDriftNs;
    double smoothLastPts;

    /* Panel-phase pacing (wall-clock PTS, nanoseconds). Always on for webOS. */
    bool panelPhasePacing;
    uint64_t panelPhaseIntervalNs;
    SS4S_PanelPhaseClock panelPhaseClock;

    /* Host presentationTimeUs → player PTS mapping. */
    bool hostPtsAnchored;
    int64_t hostPtsAnchorUs;
    double hostPtsPlayerAnchorNs;

    /* Present gate: wait for STARFISH_EVENT_RENDERED_FRAME before the next feed. */
    pthread_cond_t presentCond;
    bool presentSeen;
    bool presentGateEnabled;
    unsigned presentTimeouts;
    unsigned presentGen;

    struct StarfishMediaAPIs_C *api;
    struct StarfishResource *res;

};
extern const SS4S_LibraryContext *StarfishLibContext;

typedef struct StarfishPlayer StarfishPlayer;

bool StarfishPlayerLoadInner(SS4S_PlayerContext *ctx);

bool StarfishPlayerUnloadInner(SS4S_PlayerContext *ctx);

FeedResult StarfishPlayerFeed(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int esData);

FeedResult StarfishPlayerFeedVideo(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int64_t hostPtsUs);

uint64_t StarfishPlayerGetTime();

void StarfishPlayerConfigureSmoothPacing(SS4S_PlayerContext *ctx, int fpsNum, int fpsDen);

uint64_t StarfishPlayerNextVideoPts(SS4S_PlayerContext *ctx, int64_t hostPtsUs);

void StarfishPlayerLock(SS4S_PlayerContext *ctx);

void StarfishPlayerUnlock(SS4S_PlayerContext *ctx);
