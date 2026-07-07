#include "ndl_common.h"

#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <dlfcn.h>

#include "opus_empty.h"

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player);

static void DestroyPlayerContext(SS4S_PlayerContext *context);

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option);

static int OpusFeedEmpty(void *arg, const unsigned char *data, size_t size);

const SS4S_PlayerDriver SS4S_NDL_webOS5_PlayerDriver = {
    .Create = CreatePlayerContext,
    .Destroy = DestroyPlayerContext,
    .SetWaitAudioVideoReady = PlayerSetWaitAudioVideoReady,
};

static int UnloadMedia(SS4S_PlayerContext *context);

static int LoadMedia(SS4S_PlayerContext *context);

static void LoadConfigFromEnv();

static void ApplyFrameDropThreshold();

static void LoadCallback(int type, long long numValue, const char *strValue);

static SS4S_PlayerContext *ActivatePlayerContext = NULL;

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

static SS4S_PlayerContext *CreatePlayerContext(SS4S_Player *player) {
    assert(ActivatePlayerContext == NULL);
    SS4S_PlayerContext *created = calloc(1, sizeof(SS4S_PlayerContext));
    created->player = player;
    ActivatePlayerContext = created;
    return created;
}

static void DestroyPlayerContext(SS4S_PlayerContext *context) {
    UnloadMedia(context);
    free(context);
    assert(context == ActivatePlayerContext);
    ActivatePlayerContext = NULL;
}

static void PlayerSetWaitAudioVideoReady(SS4S_PlayerContext *context, bool option) {
    context->waitAudioVideoReady = option;
}

static int UnloadMedia(SS4S_PlayerContext *context) {
    int ret = 0;
    if (context->mediaLoaded) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Unloading media");
        context->mediaLoaded = false;
        ret = NDL_DirectMediaUnload();
    }
    return ret;
}

/* Read tuning options from the environment on every media load, so the app can change them between sessions. */
static void LoadConfigFromEnv() {
    const char *value = getenv("SS4S_NDL_LOW_LATENCY");
    SS4S_NDL_webOS5_Config.lowLatency = value != NULL && value[0] == '1';
    SS4S_NDL_webOS5_Config.maxQueueFrames = 30;
    if ((value = getenv("SS4S_NDL_MAX_QUEUE_FRAMES")) != NULL) {
        int parsed = (int) strtol(value, NULL, 10);
        if (parsed > 0) {
            SS4S_NDL_webOS5_Config.maxQueueFrames = parsed;
        }
    }
    SS4S_NDL_webOS5_Config.frameDropThreshold = -1;
    if ((value = getenv("SS4S_NDL_FRAME_DROP_THRESHOLD")) != NULL) {
        SS4S_NDL_webOS5_Config.frameDropThreshold = (int) strtol(value, NULL, 10);
    }
    if (SS4S_NDL_webOS5_Config.lowLatency) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Low latency mode enabled: maxQueueFrames=%d, "
                                                      "frameDropThreshold=%d",
                            SS4S_NDL_webOS5_Config.maxQueueFrames, SS4S_NDL_webOS5_Config.frameDropThreshold);
    }
}

/* NDL_DirectVideoSetFrameDropThreshold is not present on all webOS versions, resolve it at runtime.
 * The units of the threshold value are undocumented, so it's only applied when explicitly configured. */
static void ApplyFrameDropThreshold() {
    if (SS4S_NDL_webOS5_Config.frameDropThreshold < 0) {
        return;
    }
    int (*setFrameDropThreshold)(int) = dlsym(RTLD_DEFAULT, "NDL_DirectVideoSetFrameDropThreshold");
    if (setFrameDropThreshold == NULL) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                            "NDL_DirectVideoSetFrameDropThreshold is not available on this platform");
        return;
    }
    int rc = setFrameDropThreshold(SS4S_NDL_webOS5_Config.frameDropThreshold);
    SS4S_NDL_webOS5_Log(rc == 0 ? SS4S_LogLevelInfo : SS4S_LogLevelWarn, "NDL",
                        "NDL_DirectVideoSetFrameDropThreshold(%d) returned %d",
                        SS4S_NDL_webOS5_Config.frameDropThreshold, rc);
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
    LoadConfigFromEnv();
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "NDL_DirectMediaLoad(video=%u (%d*%d), atype=%u)", info.video.type,
                        info.video.width, info.video.height, info.audio.type);
    if ((ret = NDL_DirectMediaLoad(&info, LoadCallback)) != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectMediaLoad returned %d: %s", ret,
                            NDL_DirectMediaGetError());
        return ret;
    }
    if (info.video.type != 0) {
        ApplyFrameDropThreshold();
    }
    context->lastFrameTime = 0;
    context->avgFrameIntervalUs = 0;
    context->avgPlayDurationUs = 0;
    context->queueOverloadSinceUs = 0;
    context->saturatedSinceUs = 0;
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
    clock_gettime(CLOCK_MONOTONIC, &context->mediaLoadedTime);
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