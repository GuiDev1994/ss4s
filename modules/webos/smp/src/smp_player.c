#include "smp_player.h"
#include "smp_resource.h"
#include "StarfishMediaAPIs_C.h"
#include "../../common/panel_phase_pts.h"
#include "../../common/aurora_frame_diag.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

static void LoadCallback(int type, int64_t numValue, const char *strValue, void *data);

static jvalue_ref MakeLoadPayload(SS4S_PlayerContext *ctx, const SS4S_AudioInfo *audioInfo,
                                  const SS4S_VideoInfo *videoInfo);

static jvalue_ref AudioCreatePcmInfo(const SS4S_AudioInfo *audioInfo);

static jvalue_ref AudioCreateOpusInfo(const SS4S_AudioInfo *audioInfo);

static jvalue_ref AudioCreateAacInfo(const SS4S_AudioInfo *audioInfo);

static jvalue_ref AudioCreateAc3PlusInfo(const SS4S_AudioInfo *audioInfo);

static void TimespecAddNs(struct timespec *ts, uint64_t ns) {
    ts->tv_sec += (time_t) (ns / 1000000000ULL);
    ts->tv_nsec += (long) (ns % 1000000000ULL);
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

static bool PresentGateInit(SS4S_PlayerContext *ctx) {
    /* webOS libpthread has no pthread_condattr_setclock; match CLOCK_REALTIME. */
    int rc = pthread_cond_init(&ctx->presentCond, NULL);
    ctx->presentGateEnabled = (rc == 0);
    ctx->presentSeen = false;
    ctx->presentTimeouts = 0;
    ctx->presentGen = 0;
    return rc == 0;
}

static void PresentGateReset(SS4S_PlayerContext *ctx) {
    ctx->presentSeen = false;
    ctx->presentTimeouts = 0;
    ctx->presentGen = 0;
}

static bool PresentGateWait(SS4S_PlayerContext *ctx) {
    if (!ctx->presentSeen || !ctx->presentGateEnabled) {
        return false;
    }
    uint64_t waitNs = ctx->panelPhaseIntervalNs;
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
    TimespecAddNs(&ts, waitNs);
    unsigned gen = ctx->presentGen;
    while (ctx->presentGen == gen && ctx->presentGateEnabled) {
        int rc = pthread_cond_timedwait(&ctx->presentCond, &ctx->lock, &ts);
        if (rc == ETIMEDOUT) {
            ctx->presentTimeouts++;
            if (ctx->presentTimeouts >= 30) {
                ctx->presentGateEnabled = false;
                StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
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

static void PresentGateSignal(SS4S_PlayerContext *ctx, uint64_t wallNs) {
    ctx->presentGen++;
    if (!ctx->presentSeen) {
        ctx->presentSeen = true;
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                "RENDERED_FRAME present-gate armed wall=%llu ns",
                                (unsigned long long) wallNs);
    }
    SS4S_PanelPhaseClockAlign(&ctx->panelPhaseClock, wallNs);
    pthread_cond_signal(&ctx->presentCond);
}

static SS4S_PlayerContext *CreatePlayer(SS4S_Player *player) {
    const char *appId = getenv("APPID");
    if (appId == NULL) {
        return NULL;
    }
    SS4S_PlayerContext *context = calloc(1, sizeof(SS4S_PlayerContext));
    context->appId = strdup(appId);
    pthread_mutex_init(&context->lock, NULL);
    if (!PresentGateInit(context)) {
        StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP", "present-gate cond init failed");
    }
    context->player = player;
    atomic_init(&context->panelPhaseLoosen, false);

    context->api = StarfishMediaAPIs_create(NULL);
    if (context->api == NULL) {
        StarfishLibContext->Log(SS4S_LogLevelError, "SMP", "Failed to instantiate media APIs");
        free(context->appId);
        pthread_cond_destroy(&context->presentCond);
        pthread_mutex_destroy(&context->lock);
        free(context);
        return NULL;
    }
    context->res = StarfishResourceCreate(context->appId);
    if (context->res == NULL) {
        StarfishLibContext->Log(SS4S_LogLevelError, "SMP", "Failed to allocate resource");
        free(context->appId);
        StarfishMediaAPIs_destroy(context->api);
        pthread_cond_destroy(&context->presentCond);
        pthread_mutex_destroy(&context->lock);
        free(context);
        return NULL;
    }
    context->openTime = StarfishPlayerGetTime();
    context->state = SMP_STATE_UNLOADED;
    return context;
}

static void DestroyPlayer(SS4S_PlayerContext *context) {
    pthread_mutex_lock(&context->lock);
    context->presentGateEnabled = false;
    pthread_cond_broadcast(&context->presentCond);
    if (context->api != NULL) {
        StarfishMediaAPIs_destroy(context->api);
    }
    StarfishResourceDestroy(context->res);
    free(context->appId);
    pthread_mutex_unlock(&context->lock);
    pthread_cond_destroy(&context->presentCond);
    pthread_mutex_destroy(&context->lock);
    free(context);
}

static void SetWaitAudioVideoReady(SS4S_PlayerContext *context, bool wait) {
    context->waitAudioVideoReady = wait;
}

static void SetPanelPhaseLoosen(SS4S_PlayerContext *context, bool loosen) {
    atomic_store(&context->panelPhaseLoosen, loosen);
}

bool StarfishPlayerLoadInner(SS4S_PlayerContext *ctx) {
    if (ctx->state != SMP_STATE_UNLOADED) {
        StarfishLibContext->Log(SS4S_LogLevelError, "SMP", "Media already loaded");
        return false;
    }
    bool result = false;
    StarfishMediaAPIs_notifyForeground(ctx->api);
    StarfishResourceSetMediaId(ctx->res, StarfishMediaAPIs_getMediaID(ctx->api));

    jvalue_ref payload = MakeLoadPayload(ctx, ctx->hasAudio ? &ctx->audioInfo : NULL,
                                         ctx->hasVideo ? &ctx->videoInfo : NULL);
    const char *payload_str = jvalue_stringify(payload);
    StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "Load(payload=%s)", payload_str);
    if (StarfishMediaAPIs_load(ctx->api, payload_str, LoadCallback, ctx)) {
        result = true;
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "Media loaded");
        ctx->state = SMP_STATE_LOADED;
        AuroraFrameDiagBeginSession("smp");
        ctx->smoothPtsInitialized = false;
        ctx->smoothLastPts = 0;
        ctx->hostPtsAnchored = false;
        ctx->hostPtsAnchorUs = 0;
        ctx->hostPtsPlayerAnchorNs = 0;
        PresentGateReset(ctx);
        SS4S_PanelPhaseClockInit(&ctx->panelPhaseClock, ctx->panelPhaseIntervalNs);
        StarfishResourcePostLoad(ctx->res, &ctx->videoInfo);
    } else {
        StarfishLibContext->Log(SS4S_LogLevelError, "SMP", "Media load failed");
    }
    j_release(&payload);
    return result;
}

bool StarfishPlayerUnloadInner(SS4S_PlayerContext *ctx) {
    PlayerState state = ctx->state;
    if (state != SMP_STATE_LOADED && state != SMP_STATE_PLAYING) {
        return false;
    }
    ctx->state = SMP_STATE_UNLOADED;
    AuroraFrameDiagEndSession();
    ctx->smoothPtsInitialized = false;
    ctx->smoothLastPts = 0;
    ctx->hostPtsAnchored = false;
    ctx->hostPtsAnchorUs = 0;
    ctx->hostPtsPlayerAnchorNs = 0;
    PresentGateReset(ctx);
    SS4S_PanelPhaseClockInit(&ctx->panelPhaseClock, ctx->panelPhaseIntervalNs);
    if (state == SMP_STATE_PLAYING) {
        StarfishMediaAPIs_pushEOS(ctx->api);
    }
    StarfishMediaAPIs_unload(ctx->api);
    StarfishResourcePostUnload(ctx->res);
    return true;
}

FeedResult StarfishPlayerFeed(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int esData) {
    if (esData == 1) {
        return StarfishPlayerFeedVideo(ctx, data, size, -1);
    }
    StarfishPlayerLock(ctx);
    if (ctx->state == SMP_STATE_UNLOADED && ctx->waitAudioVideoReady) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_OK;
    }
    if (ctx->shouldStop) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_ERROR;
    }
    if (ctx->state == SMP_STATE_UNLOADED) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_NOT_READY;
    }
    char payload[256], result[256];
    uint64_t diff = StarfishPlayerGetTime() - ctx->openTime;
    snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%llu,\"esData\":%d}",
             data, size, diff, esData);
    StarfishMediaAPIs_feed(ctx->api, payload, result, 256);
    if (strstr(result, "Ok") == NULL) {
        StarfishPlayerUnlock(ctx);
        if (strstr(result, "BufferFull") != NULL) {
            return SMP_FEED_BUFFER_FULL;
        }
        return SMP_FEED_ERROR;
    }
    if (ctx->state == SMP_STATE_LOADED) {
        ctx->state = SMP_STATE_PLAYING;
        StarfishResourceStartPlaying(ctx->res);
    }
    StarfishPlayerUnlock(ctx);
    return SMP_FEED_OK;
}

FeedResult StarfishPlayerFeedVideo(SS4S_PlayerContext *ctx, const unsigned char *data, size_t size, int64_t hostPtsUs) {
    StarfishPlayerLock(ctx);
    if (ctx->state == SMP_STATE_UNLOADED && ctx->waitAudioVideoReady) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_OK;
    }
    if (ctx->shouldStop) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_ERROR;
    }
    if (ctx->state == SMP_STATE_UNLOADED) {
        StarfishPlayerUnlock(ctx);
        return SMP_FEED_NOT_READY;
    }
    char payload[256], result[256];
    /* Non-blocking PTS only — waiting on RENDERED_FRAME delayed the next feed. */
    uint64_t diff = StarfishPlayerNextVideoPts(ctx, hostPtsUs);
    int rq = -1;
    if (AuroraFrameDiagEnabled()) {
        int length = 0;
        if (StarfishMediaAPIs_getVideoRenderQueueLength(ctx->api, &length)) {
            rq = length;
        }
        AuroraFrameDiagLogFeed(diff, rq);
    }
    snprintf(payload, sizeof(payload), "{\"bufferAddr\":\"%p\",\"bufferSize\":%u,\"pts\":%llu,\"esData\":%d}",
             data, size, diff, 1);
    StarfishMediaAPIs_feed(ctx->api, payload, result, 256);
    if (strstr(result, "Ok") == NULL) {
        StarfishPlayerUnlock(ctx);
        if (strstr(result, "BufferFull") != NULL) {
            static uint64_t last_bf_log_ns;
            uint64_t now_ns = StarfishPlayerGetTime();
            if (last_bf_log_ns == 0 || now_ns - last_bf_log_ns > 500000000ULL) {
                last_bf_log_ns = now_ns;
                StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                       "BufferFull — drop frame (no immediate IDR)");
            }
            return SMP_FEED_BUFFER_FULL;
        }
        return SMP_FEED_ERROR;
    }
    if (ctx->state == SMP_STATE_LOADED) {
        ctx->state = SMP_STATE_PLAYING;
        StarfishResourceStartPlaying(ctx->res);
    }
    StarfishPlayerUnlock(ctx);
    return SMP_FEED_OK;
}

uint64_t StarfishPlayerGetTime() {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1000000000LL + now.tv_nsec);
}

static bool SmoothPacingEnvEnabled(void) {
    const char *env = getenv("SS4S_SMOOTH_PACING");
    if (env == NULL || env[0] == '\0') {
        env = getenv("SS4S_NDL_SMOOTH_PACING");
    }
    /* Default ON unless explicitly disabled with "0"/"false"/"off". */
    if (env == NULL || env[0] == '\0') {
        return true;
    }
    if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0 ||
        strcmp(env, "FALSE") == 0 || strcmp(env, "OFF") == 0) {
        return false;
    }
    return true;
}

static bool PauseAtDecodeTimeEnvEnabled(void) {
    const char *env = getenv("SS4S_PAUSE_AT_DECODE_TIME");
    /* Default ON (historical Starfish Load behavior) unless explicitly disabled. */
    if (env == NULL || env[0] == '\0') {
        return true;
    }
    if (env[0] == '0' || strcmp(env, "false") == 0 || strcmp(env, "off") == 0 ||
        strcmp(env, "FALSE") == 0 || strcmp(env, "OFF") == 0) {
        return false;
    }
    return true;
}

static const char *SmoothPacingIntervalEnv(void) {
    const char *env = getenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    if (env != NULL && env[0] != '\0') {
        return env;
    }
    return getenv("SS4S_NDL_PACING_INTERVAL_US");
}

void StarfishPlayerConfigureSmoothPacing(SS4S_PlayerContext *ctx, int fpsNum, int fpsDen) {
    if (!ctx) {
        return;
    }
    bool enabled = SmoothPacingEnvEnabled();
    ctx->smoothPacing = enabled;
    ctx->smoothPtsInitialized = false;
    ctx->smoothLastPts = 0;
    ctx->hostPtsAnchored = false;
    ctx->hostPtsAnchorUs = 0;
    ctx->hostPtsPlayerAnchorNs = 0;

    uint64_t intervalNs = 1000000000ULL / 60ULL;
    const char *intervalEnv = getenv("SS4S_PANEL_PHASE_INTERVAL_US");
    if (intervalEnv != NULL && intervalEnv[0] != '\0') {
        long us = strtol(intervalEnv, NULL, 10);
        if (us > 1000 && us < 100000) {
            intervalNs = (uint64_t) us * 1000ULL;
        } else if (fpsNum > 0 && fpsDen > 0) {
            intervalNs = (uint64_t) (1000000000.0 * (double) fpsDen / (double) fpsNum);
        }
    } else if (fpsNum > 0 && fpsDen > 0) {
        intervalNs = (uint64_t) (1000000000.0 * (double) fpsDen / (double) fpsNum);
    }
    if (intervalNs < 1000000ULL) {
        intervalNs = 1000000ULL;
    }
    /* Panel-phase is the stable default. Experimental frame pacer (punktfunk-style
     * PTS smooth) is SS4S_SMOOTH_PACING=1 with SS4S_PANEL_PHASE_PACING=0. */
    {
        const char *panelEnv = getenv("SS4S_PANEL_PHASE_PACING");
        bool panel = true;
        if (panelEnv != NULL && panelEnv[0] != '\0') {
            panel = !(panelEnv[0] == '0' || strcmp(panelEnv, "false") == 0 ||
                      strcmp(panelEnv, "off") == 0 || strcmp(panelEnv, "FALSE") == 0 ||
                      strcmp(panelEnv, "OFF") == 0);
        }
        if (panel) {
            ctx->panelPhasePacing = true;
            ctx->panelPhaseIntervalNs = intervalNs;
            ctx->smoothPacing = false;
            enabled = false;
            SS4S_PanelPhaseClockInit(&ctx->panelPhaseClock, intervalNs);
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                    "Panel-phase pacing interval=%.2fms",
                                    ctx->panelPhaseIntervalNs / 1000000.0);
        } else {
            ctx->panelPhasePacing = false;
            ctx->panelPhaseIntervalNs = intervalNs;
            ctx->smoothPacing = enabled;
            SS4S_PanelPhaseClockInit(&ctx->panelPhaseClock, intervalNs);
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                    "Experimental PTS smooth pacing %s (interval=%.2fms)",
                                    enabled ? "ON" : "OFF",
                                    intervalNs / 1000000.0);
        }
    }

    ctx->smoothHostOnly = false;
    ctx->presentationOffsetNs = 0;
    {
        const char *hostOnly = getenv("SS4S_SMOOTH_PACING_HOST_ONLY");
        if (hostOnly != NULL && hostOnly[0] != '\0' && hostOnly[0] != '0' &&
            strcmp(hostOnly, "false") != 0 && strcmp(hostOnly, "off") != 0 &&
            strcmp(hostOnly, "FALSE") != 0 && strcmp(hostOnly, "OFF") != 0) {
            ctx->smoothHostOnly = true;
        }
        const char *offEnv = getenv("SS4S_PRESENTATION_OFFSET_US");
        if (offEnv != NULL && offEnv[0] != '\0') {
            long us = strtol(offEnv, NULL, 10);
            if (us > 0 && us < 100000) {
                ctx->presentationOffsetNs = (double) us * 1000.0;
            }
        }
    }

    double legacyIntervalNs = 1000000000.0 / 60.0;
    const char *legacyIntervalEnv = SmoothPacingIntervalEnv();
    if (legacyIntervalEnv != NULL && legacyIntervalEnv[0] != '\0') {
        long us = strtol(legacyIntervalEnv, NULL, 10);
        if (us > 1000 && us < 100000) {
            legacyIntervalNs = (double) us * 1000.0;
        }
    } else if (fpsNum > 0 && fpsDen > 0) {
        legacyIntervalNs = 1000000000.0 * (double) fpsDen / (double) fpsNum;
    }
    if (legacyIntervalNs < 1000000.0) {
        legacyIntervalNs = 1000000.0;
    }
    ctx->smoothIntervalNs = legacyIntervalNs;
    double driftFrames = 0.5;
    const char *driftEnv = getenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    if (driftEnv != NULL && driftEnv[0] != '\0') {
        double d = strtod(driftEnv, NULL);
        if (d >= 0.15 && d <= 4.0) {
            driftFrames = d;
        }
    }
    ctx->smoothMaxDriftNs = legacyIntervalNs * driftFrames;

    if (enabled && ctx->smoothHostOnly) {
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                "Smooth pacing host-PTS-only offset=%.2fms (no interval grid)",
                                ctx->presentationOffsetNs / 1000000.0);
    } else if (enabled) {
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                "Smooth pacing enabled interval=%.2fms maxDrift=%.2fms (%.2f frames)",
                                ctx->smoothIntervalNs / 1000000.0, ctx->smoothMaxDriftNs / 1000000.0,
                                driftFrames);
    } else {
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "Smooth pacing disabled (wall-clock PTS)");
    }
}

static uint64_t StarfishPlayerMapBasePts(SS4S_PlayerContext *ctx, int64_t hostPtsUs) {
    uint64_t wall = StarfishPlayerGetTime() - ctx->openTime;
    if (!ctx->smoothPacing || hostPtsUs < 0) {
        return wall;
    }
    if (!ctx->hostPtsAnchored) {
        ctx->hostPtsAnchorUs = hostPtsUs;
        ctx->hostPtsPlayerAnchorNs = (double) wall;
        ctx->hostPtsAnchored = true;
        StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                "Host PTS anchored hostUs=%lld playerNs=%llu",
                                (long long) hostPtsUs, (unsigned long long) wall);
        return wall;
    }
    double mapped = ctx->hostPtsPlayerAnchorNs + (double) (hostPtsUs - ctx->hostPtsAnchorUs) * 1000.0;
    if (mapped < 0) {
        mapped = 0;
    }
    return (uint64_t) (mapped + 0.5);
}

uint64_t StarfishPlayerNextVideoPts(SS4S_PlayerContext *ctx, int64_t hostPtsUs) {
    uint64_t wall = StarfishPlayerGetTime() - ctx->openTime;
    if (ctx->panelPhasePacing) {
        bool loosen = atomic_load(&ctx->panelPhaseLoosen);
        if (loosen) {
            ctx->panelPhaseClock.last_pts = wall;
            ctx->panelPhaseClock.last_emitted = wall;
            ctx->panelPhaseClock.initialized = true;
            ctx->smoothLastPts = (double) wall;
            return wall;
        }
        uint64_t pts = SS4S_PanelPhaseClockNext(&ctx->panelPhaseClock, wall, 1000000ULL);
        ctx->smoothLastPts = (double) pts;
        return pts;
    }
    uint64_t base = StarfishPlayerMapBasePts(ctx, hostPtsUs);
    if (!ctx->smoothPacing || ctx->smoothIntervalNs <= 0) {
        return base;
    }
    /* Host PTS only: skip synthetic grid; optional presentation slack. */
    if (ctx->smoothHostOnly) {
        double pts = (double) base + ctx->presentationOffsetNs;
        if (ctx->smoothPtsInitialized && pts < ctx->smoothLastPts + 1000000.0) {
            pts = ctx->smoothLastPts + 1000000.0;
        }
        ctx->smoothLastPts = pts;
        ctx->smoothPtsInitialized = true;
        return (uint64_t) (pts + 0.5);
    }
    if (!ctx->smoothPtsInitialized) {
        ctx->smoothLastPts = (double) base;
        ctx->smoothPtsInitialized = true;
        return base;
    }
    double ideal = ctx->smoothLastPts + ctx->smoothIntervalNs;
    double minPts = (double) base - ctx->smoothMaxDriftNs;
    double maxPts = (double) base + ctx->smoothMaxDriftNs;
    double pts = ideal;
    if (pts < minPts) {
        pts = minPts;
    } else if (pts > maxPts) {
        pts = maxPts;
    }
    /* Monotonic: at least +1 ms from last video PTS. */
    if (pts < ctx->smoothLastPts + 1000000.0) {
        pts = ctx->smoothLastPts + 1000000.0;
    }
    ctx->smoothLastPts = pts;
    return (uint64_t) (pts + 0.5);
}

void StarfishPlayerLock(SS4S_PlayerContext *ctx) {
    pthread_mutex_lock(&ctx->lock);
}

void StarfishPlayerUnlock(SS4S_PlayerContext *ctx) {
    pthread_mutex_unlock(&ctx->lock);
}


static void LoadCallback(int type, int64_t numValue, const char *strValue, void *data) {
    SS4S_PlayerContext *ctx = data;
    switch (type) {
        case STARFISH_EVENT_FRAMEREADY: {
            StarfishPlayerLock(ctx);
            if (ctx->state == SMP_STATE_PLAYING) {
                uint64_t ptsNow = StarfishPlayerGetTime() - ctx->openTime;
                StarfishLibContext->VideoStats.ReportFrame(ctx->player, (ptsNow - numValue) / 1000);
            }
            StarfishPlayerUnlock(ctx);
            break;
        }
        case STARFISH_EVENT_STR_ERROR:
            StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP",
                                    "LoadCallback STARFISH_EVENT_STR_ERROR, numValue: %lld, strValue: %p\n",
                                    numValue, strValue);
            break;
        case STARFISH_EVENT_INT_ERROR: {
            StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP",
                                    "LoadCallback STARFISH_EVENT_INT_ERROR, numValue: %lld, strValue: %p\n",
                                    numValue, strValue);
            break;
        }
        case STARFISH_EVENT_INT_NUM_PROGRAM: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "LoadCallback STARFISH_EVENT_INT_NUM_PROGRAM %lld\n",
                                    numValue);
            break;
        }
        case STARFISH_EVENT_INT_NUM_VIDEO_TRACK: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                    "LoadCallback STARFISH_EVENT_INT_NUM_VIDEO_TRACK %lld\n", numValue);
            break;
        }
        case STARFISH_EVENT_STR_VIDEO_TRACK_INFO: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "LoadCallback STARFISH_EVENT_STR_VIDEO_TRACK_INFO %s\n",
                                    strValue);
            break;
        }
        case STARFISH_EVENT_INT_NUM_AUDIO_TRACK: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                    "LoadCallback STARFISH_EVENT_INT_NUM_AUDIO_TRACK %lld\n", numValue);
            break;
        }
        case STARFISH_EVENT_STR_AUDIO_TRACK_INFO: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "LoadCallback STARFISH_EVENT_STR_AUDIO_TRACK_INFO %s\n",
                                    strValue);
            break;
        }
        case STARFISH_EVENT_STR_RESOURCE_INFO: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "LoadCallback STARFISH_EVENT_STR_RESOURCE_INFO %s\n",
                                    strValue);
            break;
        }
        case STARFISH_EVENT_STR_AUDIO_INFO: {
            StarfishPlayerLock(ctx);
            StarfishResourceSetMediaAudioData(ctx->res, strValue);
            StarfishPlayerUnlock(ctx);
            break;
        }
        case STARFISH_EVENT_INT_BUFFERLOW:
        case STARFISH_EVENT_STR_BUFFERLOW:
            AuroraFrameDiagLogEvent(type, numValue, strValue);
            break;
        case STARFISH_EVENT_STR_BUFFERFULL:
            AuroraFrameDiagLogEvent(type, numValue, strValue);
            StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP", "LoadCallback STARFISH_EVENT_STR_BUFFERFULL %s",
                                    strValue);
            break;
        case STARFISH_EVENT_STR_STATE_UPDATE_LOADCOMPLETED: {
            StarfishPlayerLock(ctx);
            StarfishResourceLoadCompleted(ctx->res, StarfishMediaAPIs_getMediaID(ctx->api));
            StarfishMediaAPIs_play(ctx->api);
            StarfishPlayerUnlock(ctx);
            if (ctx->audioInfo.codec == SS4S_AUDIO_PCM_S16LE) {
                unsigned short empty_buf[8] = {0};
                int numChannels = ctx->audioInfo.numOfChannels;
                size_t size = numChannels * sizeof(unsigned short);
                StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "Playing empty PCM audio frame (%u bytes)", size);
                StarfishPlayerFeed(ctx, (unsigned char *) empty_buf, size, 2);
            }
            break;
        }
        case STARFISH_EVENT_STR_STATE_UPDATE_UNLOADCOMPLETED: {
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP",
                                    "LoadCallback STARFISH_EVENT_STR_STATE_UPDATE_UNLOADCOMPLETED\n");
            break;
        }
        case STARFISH_EVENT_STR_STATE_UPDATE_PLAYING:
            break;
        case STARFISH_EVENT_STR_VIDEO_INFO: {
            StarfishPlayerLock(ctx);
            StarfishResourceSetMediaVideoData(ctx->res, strValue, ctx->hdr);
            StarfishPlayerUnlock(ctx);
            break;
        }
        case STARFISH_EVENT_INT_NEED_DATA:
        case STARFISH_EVENT_INT_ENOUGH_DATA:
            AuroraFrameDiagLogEvent(type, numValue, strValue);
            break;
        case STARFISH_EVENT_INT_SVP_VDEC_READY:
            break;
        case STARFISH_EVENT_DROPPED_FRAME: {
            AuroraFrameDiagLogEvent(type, numValue, strValue);
            StarfishLibContext->Log(SS4S_LogLevelWarn, "SMP",
                                    "LoadCallback STARFISH_EVENT_DROPPED_FRAME, numValue: %lld, strValue: %p\n",
                                    numValue, strValue);
            break;
        }
        case STARFISH_EVENT_RENDERED_FRAME: {
            StarfishPlayerLock(ctx);
            if (ctx->state == SMP_STATE_PLAYING || ctx->state == SMP_STATE_LOADED) {
                uint64_t wall = StarfishPlayerGetTime() - ctx->openTime;
                PresentGateSignal(ctx, wall);
            }
            StarfishPlayerUnlock(ctx);
            break;
        }
        default:
            StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "LoadCallback unhandled 0x%02x\n", type);
            break;
    }
}

static inline jvalue_ref CreateMinMax(int minimum, int maximum) {
    return jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("minimum"), jnumber_create_i32(minimum)),
        jkeyval(J_CSTR_TO_JVAL("maximum"), jnumber_create_i32(maximum)),
        J_END_OBJ_DECL
    );
}

jvalue_ref MakeLoadPayload(SS4S_PlayerContext *ctx, const SS4S_AudioInfo *audioInfo,
                           const SS4S_VideoInfo *videoInfo) {
    const char *audioCodec = NULL, *videoCodec = NULL;
    if (audioInfo != NULL) {
        audioCodec = StarfishAudioCodecName(audioInfo->codec);
        if (audioCodec == NULL) {
            return NULL;
        }
    }
    if (videoInfo != NULL) {
        videoCodec = StarfishVideoCodecName(videoInfo->codec);
        if (videoCodec == NULL) {
            return NULL;
        }
    }
    const bool pauseAtDecodeTime = PauseAtDecodeTimeEnvEnabled();
    StarfishLibContext->Log(SS4S_LogLevelInfo, "SMP", "pauseAtDecodeTime=%s",
                            pauseAtDecodeTime ? "true" : "false");
    jvalue_ref codec = jobject_create();
    jvalue_ref contents = jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("codec"), codec),
        jkeyval(J_CSTR_TO_JVAL("esInfo"), jobject_create_var(
            jkeyval(J_CSTR_TO_JVAL("pauseAtDecodeTime"), jboolean_create(pauseAtDecodeTime)),
            jkeyval(J_CSTR_TO_JVAL("ptsToDecode"), jnumber_create_i64(0)),
            jkeyval(J_CSTR_TO_JVAL("seperatedPTS"), jboolean_true()),
            J_END_OBJ_DECL
        )),
        jkeyval(J_CSTR_TO_JVAL("format"), J_CSTR_TO_JVAL("RAW")),
        jkeyval(J_CSTR_TO_JVAL("provider"), J_CSTR_TO_JVAL("Chrome")),
        J_END_OBJ_DECL
    );
    if (videoInfo) {
        jobject_set(codec, J_CSTR_TO_BUF("video"), jstring_create(videoCodec));
    }
    if (audioInfo) {
        jobject_set(codec, J_CSTR_TO_BUF("audio"), jstring_create(audioCodec));
        switch (audioInfo->codec) {
            case SS4S_AUDIO_PCM_S16LE: {
                jobject_set(contents, J_CSTR_TO_BUF("pcmInfo"), AudioCreatePcmInfo(audioInfo));
                break;
            }
            case SS4S_AUDIO_OPUS: {
                jobject_set(contents, J_CSTR_TO_BUF("opusInfo"), AudioCreateOpusInfo(audioInfo));
                break;
            }
            case SS4S_AUDIO_AAC: {
                jobject_set(contents, J_CSTR_TO_BUF("aacInfo"), AudioCreateAacInfo(audioInfo));
                break;
            }
            case SS4S_AUDIO_AC3: {
                jobject_set(contents, J_CSTR_TO_BUF("ac3PlusInfo"), AudioCreateAc3PlusInfo(audioInfo));
                break;
            }
            default: {
                break;
            }
        }
    }


    jvalue_ref option = jobject_create();
    jobject_set(option, J_CSTR_TO_BUF("appId"), jstring_create(ctx->appId));
    jobject_set(option, J_CSTR_TO_BUF("externalStreamingInfo"), jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("contents"), contents),
        jkeyval(J_CSTR_TO_JVAL("streamQualityInfo"), jboolean_true()),
        jkeyval(J_CSTR_TO_JVAL("audioSync"), jboolean_true()),
        jkeyval(J_CSTR_TO_JVAL("streamQualityInfoCorruptedFrame"), jboolean_true()),
        jkeyval(J_CSTR_TO_JVAL("streamQualityInfoNonFlushable"), jboolean_true()),
        jkeyval(J_CSTR_TO_JVAL("restartStreaming"), jboolean_false()),
        jkeyval(J_CSTR_TO_JVAL("bufferingCtrInfo"), jobject_create_var(
            jkeyval(J_CSTR_TO_JVAL("bufferMaxLevel"), jnumber_create_i32(0)),
            jkeyval(J_CSTR_TO_JVAL("bufferMinLevel"), jnumber_create_i32(0)),
            jkeyval(J_CSTR_TO_JVAL("preBufferByte"), jnumber_create_i32(0)),
            jkeyval(J_CSTR_TO_JVAL("qBufferLevelAudio"), jnumber_create_i32(0)),
            jkeyval(J_CSTR_TO_JVAL("qBufferLevelVideo"), jnumber_create_i32(0)),
            /* This affects pipeline appsrc.
             * A very low maximum is meaningless because it only causes the pipeline to discard buffers
             */
            jkeyval(J_CSTR_TO_JVAL("srcBufferLevelAudio"), CreateMinMax(1, 32768)),
            jkeyval(J_CSTR_TO_JVAL("srcBufferLevelVideo"), CreateMinMax(1, 1048576)),
            J_END_OBJ_DECL
        )),
        J_END_OBJ_DECL
    ));
    // Setting contentsType to WEBRTC will reduce video latency significantly,
    // while setting to LIVE will improve audio sync
    jobject_set(option, J_CSTR_TO_BUF("transmission"), jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("contentsType"), J_CSTR_TO_JVAL("WEBRTC")),
        J_END_OBJ_DECL
    ));
    jobject_set(option, J_CSTR_TO_BUF("needAudio"), jboolean_create(false));
    // When queryPosition is set to true, STARFISH_EVENT_FRAMEREADY will not be sent
    jobject_set(option, J_CSTR_TO_BUF("queryPosition"), jboolean_false());
    // Recognized on webOS 5+, doesn't seem to have any effect
    jobject_set(option, J_CSTR_TO_BUF("lowDelayMode"), jboolean_true());

    if (videoInfo) {
        int frameRate = 6000;
        if (videoInfo->frameRateNumerator != 0 && videoInfo->frameRateDenominator != 0) {
            frameRate = videoInfo->frameRateNumerator * 100 / videoInfo->frameRateDenominator;
        }
        jobject_set(option, J_CSTR_TO_BUF("adaptiveStreaming"), jobject_create_var(
            jkeyval(J_CSTR_TO_JVAL("audioOnly"), jboolean_false()),
            jkeyval(J_CSTR_TO_JVAL("maxWidth"), jnumber_create_i32(videoInfo->width)),
            jkeyval(J_CSTR_TO_JVAL("maxHeight"), jnumber_create_i32(videoInfo->height)),
            jkeyval(J_CSTR_TO_JVAL("maxFrameRate"), jnumber_create_f64(frameRate / 100.0)),
            J_END_OBJ_DECL
        ));
    }

    jvalue_ref arg = jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("mediaTransportType"), J_CSTR_TO_JVAL("BUFFERSTREAM")),
        jkeyval(J_CSTR_TO_JVAL("option"), option),
        J_END_OBJ_DECL
    );
    StarfishResourcePopulateLoadPayload(ctx->res, arg, ctx->hasAudio ? &ctx->audioInfo : NULL,
                                        ctx->hasVideo ? &ctx->videoInfo : NULL);
    return jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("args"), jarray_create_var(NULL, arg, J_END_ARRAY_DECL)),
        J_END_OBJ_DECL
    );
}

jvalue_ref AudioCreatePcmInfo(const SS4S_AudioInfo *audioInfo) {
    const char *channelMode = "stereo";
    if (audioInfo->numOfChannels == 1) {
        channelMode = "mono";
    } else if (audioInfo->numOfChannels == 6) {
        channelMode = "6-channel";
    }
    return jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("channelMode"), j_cstr_to_jval(channelMode)),
        jkeyval(J_CSTR_TO_JVAL("format"), j_cstr_to_jval("S16LE")),
        jkeyval(J_CSTR_TO_JVAL("sampleRate"), jnumber_create_f64(audioInfo->sampleRate / 1000.0)),
        jkeyval(J_CSTR_TO_JVAL("bitsPerSample"), jnumber_create_i32(16)),
        jkeyval(J_CSTR_TO_JVAL("layout"), j_cstr_to_jval("interleaved")),
        J_END_OBJ_DECL
    );
}

jvalue_ref AudioCreateOpusInfo(const SS4S_AudioInfo *audioInfo) {
    jvalue_ref info = jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("channels"), jnumber_create_i32(audioInfo->numOfChannels)),
        jkeyval(J_CSTR_TO_JVAL("sampleRate"), jnumber_create_f64(audioInfo->sampleRate / 1000.0)),
        J_END_OBJ_DECL
    );
    if (audioInfo->codecData && audioInfo->codecDataLen > 0) {
        gchar *encoded = g_base64_encode(audioInfo->codecData, audioInfo->codecDataLen);
        jvalue_ref streamHeader = jstring_create(encoded);
        g_free(encoded);
        jobject_set(info, J_CSTR_TO_BUF("streamHeader"), streamHeader);
    }
    return info;
}

jvalue_ref AudioCreateAacInfo(const SS4S_AudioInfo *audioInfo) {
    return jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("channels"), jnumber_create_i32(audioInfo->numOfChannels)),
        jkeyval(J_CSTR_TO_JVAL("format"), j_cstr_to_jval("adts")),
        jkeyval(J_CSTR_TO_JVAL("frequency"), jnumber_create_i32(audioInfo->sampleRate / 1000)),
        jkeyval(J_CSTR_TO_JVAL("profile"), jnumber_create_i32(2)),
        J_END_OBJ_DECL
    );
}

jvalue_ref AudioCreateAc3PlusInfo(const SS4S_AudioInfo *audioInfo) {
    return jobject_create_var(
        jkeyval(J_CSTR_TO_JVAL("channels"), jnumber_create_i32(audioInfo->numOfChannels)),
        jkeyval(J_CSTR_TO_JVAL("frequency"), jnumber_create_i32(audioInfo->sampleRate / 1000)),
        J_END_OBJ_DECL
    );
}

const SS4S_PlayerDriver StarfishPlayerDriver = {
    .Create = CreatePlayer,
    .Destroy = DestroyPlayer,
    .SetWaitAudioVideoReady = SetWaitAudioVideoReady,
    .SetPanelPhaseLoosen = SetPanelPhaseLoosen,
};
