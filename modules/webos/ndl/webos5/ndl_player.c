#include "ndl_common.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

#include "opus_empty.h"
#include "../../common/aurora_frame_diag.h"

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player);

static void DestroyPlayerContext(SS4S_PlayerContext *context);

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option);

static void PlayerSetPanelPhaseLoosen(SS4S_PlayerContext *context, bool loosen);

static int OpusFeedEmpty(void *arg, const unsigned char *data, size_t size);

const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver = {
    .Create = CreatePlayerContext,
    .Destroy = DestroyPlayerContext,
    .SetWaitAudioVideoReady = PlayerSetWaitAudioVideoReady,
    .SetPanelPhaseLoosen = PlayerSetPanelPhaseLoosen,
};

static int UnloadMedia(SS4S_PlayerContext *context);

static int LoadMedia(SS4S_PlayerContext *context);

static void LoadCallback(int type, long long numValue, const char *strValue);

static SS4S_PlayerContext *ActivatePlayerContext = NULL;

/*
 * Render-buffer pacing.
 *
 * NDL's only video entry point is Play(buffer, size, pts). Aurora has always passed
 * "now" as the PTS, which tells the renderer the frame is already due, so it shows it
 * immediately and its render buffer never fills. With an empty buffer the renderer has
 * nothing in hand when a vsync arrives, so presentation phase simply follows arrival
 * phase and no amount of client-side scheduling can align it — which is why every
 * previous pacing attempt measured as no change.
 *
 * Here the PTS is instead placed `renderQueueTarget` frames in the future, advancing on
 * the host capture clock. The renderer then holds frames and releases them on its own
 * vsync, which is the only place on this platform where a real V-Sync can happen.
 * `renderTrimMs` is a slow integrator on the observed queue depth that absorbs the
 * host/TV clock difference without moving presentation visibly.
 */
#define NDL_RENDER_TRIM_STEP_MS 0.05
#define NDL_RENDER_TRIM_LIMIT_FRAMES 1.5

static void ConfigureRenderPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen) {
    int target = 0;
    const char *targetEnv = getenv("SS4S_RENDER_QUEUE_TARGET");
    if (targetEnv != NULL && targetEnv[0] != '\0') {
        long frames = strtol(targetEnv, NULL, 10);
        if (frames > 0 && frames <= 8) {
            target = (int) frames;
        }
    }
    double frameMs = 1000.0 / 60.0;
    if (fpsNum > 0 && fpsDen > 0) {
        frameMs = 1000.0 * (double) fpsDen / (double) fpsNum;
    }
    if (frameMs < 1.0) {
        frameMs = 1.0;
    }
    context->renderQueueTarget = target;
    context->renderFrameMs = frameMs;
    if (target > 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Render pacing on: target=%d frames, frame=%.3fms, lead=%.1fms",
                            target, frameMs, target * frameMs);
    } else {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Render pacing off (PTS = now)");
    }
}

static void ResetRenderPacing(SS4S_PlayerContext *context) {
    context->renderAnchored = false;
    context->renderHostAnchorUs = 0;
    context->renderAnchorMs = 0;
    context->renderLastPts = 0;
    context->renderTrimMs = 0;
    context->renderFrames = 0;
    context->renderQueueSum = 0;
    context->renderStarved = 0;
    context->renderResyncs = 0;
    context->renderQueueMax = 0;
}

bool SS4S_NDL_webOS5_RenderPacingEnabled(const SS4S_PlayerContext *context) {
    return context->renderQueueTarget > 0;
}

void SS4S_NDL_webOS5_LogRenderPacing(const SS4S_PlayerContext *context) {
    if (context->renderQueueTarget <= 0 || context->renderFrames == 0) {
        return;
    }
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                        "Render pacing: %llu frames, queue avg %.2f max %d (target %d), "
                        "%llu starved (%.1f%%), %llu resyncs, trim %.1fms",
                        (unsigned long long) context->renderFrames,
                        (double) context->renderQueueSum / (double) context->renderFrames,
                        context->renderQueueMax, context->renderQueueTarget,
                        (unsigned long long) context->renderStarved,
                        100.0 * (double) context->renderStarved / (double) context->renderFrames,
                        (unsigned long long) context->renderResyncs, context->renderTrimMs);
}

uint64_t SS4S_NDL_webOS5_RenderPacedPts(SS4S_PlayerContext *context, int64_t hostPtsUs, int queueLen) {
    const double wall = (double) SS4S_NDL_webOS5_GetPts(context);
    const double lead = context->renderQueueTarget * context->renderFrameMs;
    if (hostPtsUs < 0) {
        /* No host clock: the best we can do is a constant offset from arrival. */
        return (uint64_t) (wall + lead + 0.5);
    }
    if (!context->renderAnchored) {
        context->renderAnchored = true;
        context->renderHostAnchorUs = hostPtsUs;
        context->renderAnchorMs = wall + lead;
        context->renderTrimMs = 0;
        context->renderLastPts = 0;
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Render pacing anchored hostUs=%lld playerMs=%.1f lead=%.1fms",
                            (long long) hostPtsUs, context->renderAnchorMs, lead);
    }

    /* Steer the queue back to target slowly enough to stay below the visible threshold. */
    if (queueLen >= 0) {
        const double trimLimit = NDL_RENDER_TRIM_LIMIT_FRAMES * context->renderFrameMs;
        if (queueLen < context->renderQueueTarget && context->renderTrimMs < trimLimit) {
            context->renderTrimMs += NDL_RENDER_TRIM_STEP_MS;
        } else if (queueLen > context->renderQueueTarget && context->renderTrimMs > -trimLimit) {
            context->renderTrimMs -= NDL_RENDER_TRIM_STEP_MS;
        }
        context->renderQueueSum += (uint64_t) queueLen;
        if (queueLen > context->renderQueueMax) {
            context->renderQueueMax = queueLen;
        }
        if (queueLen == 0) {
            context->renderStarved++;
        }
    }
    context->renderFrames++;

    double pts = context->renderAnchorMs + context->renderTrimMs +
                 (double) (hostPtsUs - context->renderHostAnchorUs) / 1000.0;

    /* A stall, IDR gap or host clock jump leaves the mapping meaningless. */
    const double behind = wall - pts;
    const double ahead = pts - wall;
    if (behind > context->renderFrameMs || ahead > lead + 4.0 * context->renderFrameMs) {
        context->renderHostAnchorUs = hostPtsUs;
        context->renderAnchorMs = wall + lead;
        context->renderTrimMs = 0;
        context->renderResyncs++;
        pts = context->renderAnchorMs;
    }
    if (pts <= context->renderLastPts) {
        pts = context->renderLastPts + 1.0;
    }
    context->renderLastPts = pts;
    return (uint64_t) (pts + 0.5);
}

static void NdlTimespecAddNs(struct timespec *ts, uint64_t ns) {
    ts->tv_sec += (time_t) (ns / 1000000000ULL);
    ts->tv_nsec += (long) (ns % 1000000000ULL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool NdlPresentGateInit(SS4S_PlayerContext *ctx) {
    /* webOS libpthread has no pthread_condattr_setclock; match CLOCK_REALTIME. */
    int rc = pthread_cond_init(&ctx->presentCond, NULL);
    ctx->presentGateEnabled = (rc == 0);
    ctx->presentSeen = false;
    ctx->presentTimeouts = 0;
    ctx->presentGen = 0;
    return rc == 0;
}

static void NdlPresentGateReset(SS4S_PlayerContext *ctx) {
    ctx->presentSeen = false;
    ctx->presentTimeouts = 0;
    ctx->presentGen = 0;
}

bool SS4S_NDL_webOS5_WaitPresent(SS4S_PlayerContext *ctx) {
    if (!ctx->presentSeen || !ctx->presentGateEnabled) {
        return false;
    }
    uint64_t waitNs = ctx->panelPhaseIntervalUs * 1000ULL;
    if (waitNs == 0) {
        waitNs = 16000000ULL;
    }
    /* Wait at most 75% of a refresh so a late RENDERED_FRAME cannot skip the next vsync. */
    waitNs = waitNs * 3ULL / 4ULL;
    if (waitNs < 2000000ULL) {
        waitNs = 2000000ULL;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    NdlTimespecAddNs(&ts, waitNs);
    unsigned gen = ctx->presentGen;
    while (ctx->presentGen == gen && ctx->presentGateEnabled) {
        int rc = pthread_cond_timedwait(&ctx->presentCond, &SS4S_NDL_webOS5_Lock, &ts);
        if (rc == ETIMEDOUT) {
            ctx->presentTimeouts++;
            if (ctx->presentTimeouts >= 30) {
                ctx->presentGateEnabled = false;
                SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                    "RENDERED_FRAME not firing; present-gate off");
            }
            return false;
        }
        if (rc != 0) {
            return false;
        }
    }
    ctx->presentTimeouts = 0;
    return ctx->presentGateEnabled;
}

static void NdlPresentGateSignal(SS4S_PlayerContext *ctx) {
    uint64_t wallUs = SS4S_NDL_webOS5_GetPts(ctx) * 1000ULL;
    ctx->presentGen++;
    if (!ctx->presentSeen) {
        ctx->presentSeen = true;
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "RENDERED_FRAME present-gate armed wall=%llu us",
                            (unsigned long long) wallUs);
    }
    SS4S_PanelPhaseClockAlign(&ctx->panelPhaseClock, wallUs);
    pthread_cond_signal(&ctx->presentCond);
}

int SS4S_NDL_webOS5_ReloadMedia(SS4S_PlayerContext *context) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Reloading media");
    if (UnloadMedia(context) != 0) {
        return -1;
    }
    return LoadMedia(context);
}

int SS4S_NDL_webOS5_UnloadMedia(SS4S_PlayerContext *context) {
    return UnloadMedia(context);
}

uint64_t SS4S_NDL_webOS5_GetPts(const SS4S_PlayerContext *context) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    uint64_t pts = (now.tv_sec * 1000) + (now.tv_nsec / 1000000) - context->mediaLoadedTime.tv_sec * 1000 -
                   context->mediaLoadedTime.tv_nsec / 1000000;
    return pts;
}

void SS4S_NDL_webOS5_ConfigureSmoothPacing(SS4S_PlayerContext *context, int fpsNum, int fpsDen) {
    if (!context) {
        return;
    }
    const char *env = getenv("SS4S_SMOOTH_PACING");
    if (env == NULL || env[0] == '\0') {
        env = getenv("SS4S_NDL_SMOOTH_PACING");
    }
    /* Default ON unless explicitly disabled with "0"/"false"/"off". */
    bool enabled = true;
    if (env != NULL && env[0] != '\0') {
        if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0 ||
            strcmp(env, "FALSE") == 0 || strcmp(env, "OFF") == 0) {
            enabled = false;
        }
    }
    context->smoothPacing = enabled;
    context->smoothPtsInitialized = false;
    context->smoothLastPts = 0;
    context->hostPtsAnchored = false;
    context->hostPtsAnchorUs = 0;
    context->hostPtsPlayerAnchorMs = 0;

    uint64_t intervalUs = 1000000ULL / 60ULL;
    const char *intervalEnv = getenv("SS4S_PANEL_PHASE_INTERVAL_US");
    if (intervalEnv != NULL && intervalEnv[0] != '\0') {
        long us = strtol(intervalEnv, NULL, 10);
        if (us > 1000 && us < 100000) {
            intervalUs = (uint64_t) us;
        } else if (fpsNum > 0 && fpsDen > 0) {
            intervalUs = (uint64_t) (1000000.0 * (double) fpsDen / (double) fpsNum);
        }
    } else if (fpsNum > 0 && fpsDen > 0) {
        intervalUs = (uint64_t) (1000000.0 * (double) fpsDen / (double) fpsNum);
    }
    if (intervalUs < 1000ULL) {
        intervalUs = 1000ULL;
    }
    {
        const char *panelEnv = getenv("SS4S_PANEL_PHASE_PACING");
        bool panel = true;
        if (panelEnv != NULL && panelEnv[0] != '\0') {
            panel = !(panelEnv[0] == '0' || strcmp(panelEnv, "false") == 0 ||
                      strcmp(panelEnv, "off") == 0 || strcmp(panelEnv, "FALSE") == 0 ||
                      strcmp(panelEnv, "OFF") == 0);
        }
        if (panel) {
            context->panelPhasePacing = true;
            context->panelPhaseIntervalUs = intervalUs;
            context->smoothPacing = false;
            enabled = false;
            SS4S_PanelPhaseClockInit(&context->panelPhaseClock, intervalUs);
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                "Panel-phase pacing interval=%.2fms",
                                (double) context->panelPhaseIntervalUs / 1000.0);
        } else {
            context->panelPhasePacing = false;
            context->panelPhaseIntervalUs = intervalUs;
            context->smoothPacing = enabled;
            SS4S_PanelPhaseClockInit(&context->panelPhaseClock, intervalUs);
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                "Experimental PTS smooth pacing %s (interval=%.2fms)",
                                enabled ? "ON" : "OFF",
                                (double) intervalUs / 1000.0);
        }
    }

    context->smoothHostOnly = false;
    context->presentationOffsetMs = 0;
    {
        const char *hostOnly = getenv("SS4S_SMOOTH_PACING_HOST_ONLY");
        if (hostOnly != NULL && hostOnly[0] != '\0' && hostOnly[0] != '0' &&
            strcmp(hostOnly, "false") != 0 && strcmp(hostOnly, "off") != 0 &&
            strcmp(hostOnly, "FALSE") != 0 && strcmp(hostOnly, "OFF") != 0) {
            context->smoothHostOnly = true;
        }
        const char *offEnv = getenv("SS4S_PRESENTATION_OFFSET_US");
        if (offEnv != NULL && offEnv[0] != '\0') {
            long us = strtol(offEnv, NULL, 10);
            if (us > 0 && us < 100000) {
                context->presentationOffsetMs = (double) us / 1000.0;
            }
        }
    }

    double intervalMs = 1000.0 / 60.0;
    const char *legacyIntervalEnv = getenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    if (legacyIntervalEnv == NULL || legacyIntervalEnv[0] == '\0') {
        legacyIntervalEnv = getenv("SS4S_NDL_PACING_INTERVAL_US");
    }
    if (legacyIntervalEnv != NULL && legacyIntervalEnv[0] != '\0') {
        long us = strtol(legacyIntervalEnv, NULL, 10);
        if (us > 1000 && us < 100000) {
            intervalMs = (double) us / 1000.0;
        }
    } else if (fpsNum > 0 && fpsDen > 0) {
        intervalMs = 1000.0 * (double) fpsDen / (double) fpsNum;
    }
    if (intervalMs < 1.0) {
        intervalMs = 1.0;
    }
    context->smoothIntervalMs = intervalMs;
    double driftFrames = 0.5;
    const char *driftEnv = getenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    if (driftEnv != NULL && driftEnv[0] != '\0') {
        double d = strtod(driftEnv, NULL);
        if (d >= 0.15 && d <= 4.0) {
            driftFrames = d;
        }
    }
    context->smoothMaxDriftMs = intervalMs * driftFrames;

    ConfigureRenderPacing(context, fpsNum, fpsDen);

    if (enabled && context->smoothHostOnly) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Smooth pacing host-PTS-only offset=%.2fms (no interval grid)",
                            context->presentationOffsetMs);
    } else if (enabled) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Smooth pacing enabled interval=%.2fms maxDrift=%.2fms (%.2f frames)",
                            context->smoothIntervalMs, context->smoothMaxDriftMs, driftFrames);
    } else {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Smooth pacing disabled (wall-clock PTS)");
    }
}

uint64_t SS4S_NDL_webOS5_NextVideoPts(SS4S_PlayerContext *context, int64_t hostPtsUs) {
    uint64_t wall = SS4S_NDL_webOS5_GetPts(context);
    if (context->panelPhasePacing) {
        bool loosen = atomic_load(&context->panelPhaseLoosen);
        uint64_t wallUs = wall * 1000ULL;
        if (loosen) {
            context->panelPhaseClock.last_pts = wallUs;
            context->panelPhaseClock.last_emitted = wallUs;
            context->panelPhaseClock.initialized = true;
            context->smoothLastPts = (double) wall;
            return wall;
        }
        uint64_t ptsUs = SS4S_PanelPhaseClockNext(&context->panelPhaseClock, wallUs, 1000ULL);
        uint64_t pts = ptsUs / 1000ULL;
        context->smoothLastPts = (double) pts;
        return pts;
    }
    uint64_t base = wall;
    if (context->smoothPacing && hostPtsUs >= 0) {
        if (!context->hostPtsAnchored) {
            context->hostPtsAnchorUs = hostPtsUs;
            context->hostPtsPlayerAnchorMs = (double) wall;
            context->hostPtsAnchored = true;
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                "Host PTS anchored hostUs=%lld playerMs=%llu",
                                (long long) hostPtsUs, (unsigned long long) wall);
            base = wall;
        } else {
            double mapped = context->hostPtsPlayerAnchorMs +
                            (double) (hostPtsUs - context->hostPtsAnchorUs) / 1000.0;
            if (mapped < 0) {
                mapped = 0;
            }
            base = (uint64_t) (mapped + 0.5);
        }
    }
    if (!context->smoothPacing || context->smoothIntervalMs <= 0) {
        return base;
    }
    /* Host PTS only: skip synthetic grid; optional presentation slack. */
    if (context->smoothHostOnly) {
        double pts = (double) base + context->presentationOffsetMs;
        if (context->smoothPtsInitialized && pts < context->smoothLastPts + 1.0) {
            pts = context->smoothLastPts + 1.0;
        }
        context->smoothLastPts = pts;
        context->smoothPtsInitialized = true;
        return (uint64_t) (pts + 0.5);
    }
    if (!context->smoothPtsInitialized) {
        context->smoothLastPts = (double) base;
        context->smoothPtsInitialized = true;
        return base;
    }
    double ideal = context->smoothLastPts + context->smoothIntervalMs;
    double minPts = (double) base - context->smoothMaxDriftMs;
    double maxPts = (double) base + context->smoothMaxDriftMs;
    double pts = ideal;
    if (pts < minPts) {
        pts = minPts;
    } else if (pts > maxPts) {
        pts = maxPts;
    }
    /* Monotonic: at least +1 ms from last video PTS. */
    if (pts < context->smoothLastPts + 1.0) {
        pts = context->smoothLastPts + 1.0;
    }
    context->smoothLastPts = pts;
    return (uint64_t) (pts + 0.5);
}

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player) {
    assert(ActivatePlayerContext == NULL);
    SS4S_PlayerContext *created = calloc(1, sizeof(SS4S_PlayerContext));
    created->player = player;
    atomic_init(&created->panelPhaseLoosen, false);
    if (!NdlPresentGateInit(created)) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "present-gate cond init failed");
    }
    ActivatePlayerContext = created;
    return created;
}

static void DestroyPlayerContext(SS4S_PlayerContext *context) {
    assert(context == ActivatePlayerContext);
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    context->presentGateEnabled = false;
    pthread_cond_broadcast(&context->presentCond);
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    UnloadMedia(context);
    pthread_cond_destroy(&context->presentCond);
    ActivatePlayerContext = NULL;
    free(context);
}

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option) {
    context->waitAudioVideoReady = option;
}

static void PlayerSetPanelPhaseLoosen(SS4S_PlayerContext *context, bool loosen) {
    atomic_store(&context->panelPhaseLoosen, loosen);
}

static int UnloadMedia(SS4S_PlayerContext *context) {
    int ret = 0;
    if (context->mediaLoaded) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Unloading media");
        SS4S_NDL_webOS5_LogRenderPacing(context);
        context->mediaLoaded = false;
        AuroraFrameDiagEndSession();
        ret = NDL_DirectMediaUnload();
    }
    return ret;
}

static int LoadMedia(SS4S_PlayerContext *context) {
    int ret;
    if (!SS4S_NDL_webOS5_Initialized) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Initializing NDL");
        if ((ret = NDL_DirectMediaInit(getenv("APPID"), NULL)) != 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelError, "NDL", "Failed to init: ret=%d, error=%s", ret,
                                NDL_DirectMediaGetError());
            return ret;
        }
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "NDL_DirectMediaInit succeeded");
        SS4S_NDL_webOS5_Initialized = true;
    }
    assert(SS4S_NDL_webOS5_Initialized);
    assert(!context->mediaLoaded);
    NDL_DIRECTMEDIA_DATA_INFO_T info = context->mediaInfo;
    if (info.video.type == 0 && info.audio.type == 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "LoadMedia but audio and video has no type");
        return -1;
    } else if (context->waitAudioVideoReady && (info.video.type == 0 || info.audio.type == 0)) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Defer LoadMedia because audio or video has no type");
        return 0;
    }
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "NDL_DirectMediaLoad(video=%u (%d*%d), atype=%u)", info.video.type,
                        info.video.width, info.video.height, info.audio.type);
    if ((ret = NDL_DirectMediaLoad(&info, LoadCallback)) != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectMediaLoad returned %d: %s", ret,
                            NDL_DirectMediaGetError());
        return ret;
    }
    if (context->mediaInfo.audio.type == NDL_AUDIO_TYPE_PCM) {
        unsigned short empty_buf[8] = {0};
        int numChannels = 2;
        if (strncmp(context->mediaInfo.audio.pcm.channelMode, "mono", 4) == 0) {
            numChannels = 1;
        } else if (strncmp(context->mediaInfo.audio.pcm.channelMode, "6-channel", 10) == 0) {
            numChannels = 6;
        }
        size_t size = numChannels * sizeof(unsigned short);
        NDL_DirectAudioPlay(empty_buf, size, (long long) SS4S_NDL_webOS5_GetPts(context));
    } else if (context->opusEmpty != NULL) {
        SS4S_OpusEmptyFeed(context->opusEmpty, OpusFeedEmpty, context);
    }

    context->mediaLoaded = true;
    AuroraFrameDiagBeginSession("ndl");
    clock_gettime(CLOCK_MONOTONIC, &context->mediaLoadedTime);
    context->smoothPtsInitialized = false;
    context->smoothLastPts = 0;
    context->hostPtsAnchored = false;
    context->hostPtsAnchorUs = 0;
    context->hostPtsPlayerAnchorMs = 0;
    NdlPresentGateReset(context);
    ResetRenderPacing(context);
    SS4S_PanelPhaseClockInit(&context->panelPhaseClock, context->panelPhaseIntervalUs);
    context->lastFrameTime = 0;
    return ret;
}

static void LoadCallback(int type, long long numValue, const char *strValue) {
    switch (type) {
        case 0x16: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_LOADCOMPLETED: %s", __FUNCTION__, strValue);
            break;
        }
        case 0x17: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_UNLOADCOMPLETED: %s", __FUNCTION__,
                                strValue);
            break;
        }
        case 0x1a: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s STATE_UPDATE_PLAYING: %s", __FUNCTION__, strValue);
            break;
        }
        case 0x31: {
            pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
            if (ActivatePlayerContext != NULL && ActivatePlayerContext->mediaLoaded) {
                NdlPresentGateSignal(ActivatePlayerContext);
            }
            pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
            AuroraFrameDiagLogEvent(type, numValue, strValue);
            break;
        }
        default: {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "%s type=0x%02x, numValue=0x%llx, strValue=%p", __FUNCTION__,
                                type, numValue, strValue);
            break;
        }
    }
}

static int OpusFeedEmpty(void *arg, const unsigned char *data, size_t size) {
    return NDL_DirectAudioPlay((void *) data, size, (long long) SS4S_NDL_webOS5_GetPts(arg));
}