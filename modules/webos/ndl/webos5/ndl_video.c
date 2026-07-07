#include <string.h>
#include <dlfcn.h>
#include "ndl_common.h"
#include "highend_check.h"
#include "max_res.h"

static SS4S_VideoOpenResult ReloadWithSize(SS4S_PlayerContext *context, int width, int height);

static uint64_t GetTimeUs();

/* NDL_DirectVideoFlushRenderBuffer may be missing on older webOS versions, resolve it at runtime */
static int FlushRenderBuffer() {
    static int (*flushFn)(void) = NULL;
    static bool resolved = false;
    if (!resolved) {
        flushFn = dlsym(RTLD_DEFAULT, "NDL_DirectVideoFlushRenderBuffer");
        resolved = true;
        if (flushFn == NULL) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                "NDL_DirectVideoFlushRenderBuffer is not available on this platform");
        }
    }
    if (flushFn == NULL) {
        return -1;
    }
    return flushFn();
}

static bool GetCapabilities(SS4S_VideoCapabilities *capabilities) {
    capabilities->codecs = SS4S_VIDEO_H264 | SS4S_VIDEO_H265 | SS4S_VIDEO_VP9 | SS4S_VIDEO_AV1;
    capabilities->transform = SS4S_VIDEO_CAP_TRANSFORM_UI_COMPOSITING;
    if (SS4S_webOS_Is_HighEnd_SoC()) {
        capabilities->maxBitrate = 95000;
        capabilities->suggestedBitrate = 45000;
    } else {
        capabilities->maxBitrate = 65000;
        capabilities->suggestedBitrate = 35000;
    }
    capabilities->hdr = true;
    // If fullColorRange is set to true, the video will be displayed with over saturated colors.
    capabilities->fullColorRange = false;
    capabilities->colorSpace = SS4S_VIDEO_CAP_COLORSPACE_BT2020 | SS4S_VIDEO_CAP_COLORSPACE_BT709;
    if (SS4S_webOS_GetMaxVideoResolution(SS4S_VIDEO_H265, &capabilities->maxWidth, &capabilities->maxHeight, NULL)) {
        capabilities->maxWidth = 0;
        capabilities->maxHeight = 0;
    }
    return true;
}

static SS4S_VideoOpenResult OpenVideo(const SS4S_VideoInfo *info, const SS4S_VideoExtraInfo *extraInfo,
                                      SS4S_VideoInstance **instance, SS4S_PlayerContext *context) {
    (void) extraInfo;
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenVideo called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    memset(&context->mediaInfo.video, 0, sizeof(context->mediaInfo.video));
    SS4S_VideoOpenResult result;
    switch (info->codec) {
        case SS4S_VIDEO_H264: {
            context->mediaInfo.video.type = NDL_VIDEO_TYPE_H264;
            break;
        }
        case SS4S_VIDEO_H265: {
            context->mediaInfo.video.type = NDL_VIDEO_TYPE_H265;
            break;
        }
        case SS4S_VIDEO_VP9: {
            context->mediaInfo.video.type = NDL_VIDEO_TYPE_VP9;
            break;
        }
        case SS4S_VIDEO_AV1: {
            context->mediaInfo.video.type = NDL_VIDEO_TYPE_AV1;
            break;
        }
        default:
            result = SS4S_VIDEO_OPEN_UNSUPPORTED_CODEC;
            goto finish;
    }
    context->mediaInfo.video.unknown1 = 0;
    result = ReloadWithSize(context, info->width, info->height);
    if (result != SS4S_VIDEO_OPEN_OK) {
        goto finish;
    }
    *instance = (SS4S_VideoInstance *) context;
    result = SS4S_VIDEO_OPEN_OK;

    finish:
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenVideo returned %d", result);
    return result;
}

/* Smoothing factor for the feed interval and play duration EMAs */
#define FEED_EMA_ALPHA 0.1f
/* Minimum interval between decoder saturation warnings, in microseconds */
#define SATURATION_LOG_INTERVAL_US 5000000
/* How long the render queue must stay above the threshold before the watchdog flushes, in microseconds */
#define QUEUE_OVERLOAD_GRACE_US 500000

static SS4S_VideoFeedResult FeedVideo(SS4S_VideoInstance *instance, const unsigned char *data, size_t size,
                                      SS4S_VideoFeedFlags flags) {
    (void) flags;
    SS4S_PlayerContext *context = (void *) instance;
    if (!context->mediaLoaded) {
        return SS4S_VIDEO_FEED_NOT_READY;
    }
    uint64_t pts = SS4S_NDL_webOS5_GetPts(context);
    uint64_t playStart = GetTimeUs();
    int rc = NDL_DirectVideoPlay((void *) data, size, (long long) pts);
    uint64_t now = GetTimeUs();
    if (rc != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectVideoPlay returned %d: %s", rc,
                            NDL_DirectMediaGetError());
        return SS4S_VIDEO_FEED_ERROR;
    }
    float playDuration = (float) (now - playStart);
    if (context->avgPlayDurationUs <= 0) {
        context->avgPlayDurationUs = playDuration;
    } else {
        context->avgPlayDurationUs += FEED_EMA_ALPHA * (playDuration - context->avgPlayDurationUs);
    }
    int renderBufferLength = -1;
    if (NDL_DirectVideoGetRenderBufferLength(&renderBufferLength) != 0) {
        renderBufferLength = -1;
    }
    if (context->lastFrameTime > 0) {
        float interval = (float) (now - context->lastFrameTime);
        if (context->avgFrameIntervalUs <= 0) {
            context->avgFrameIntervalUs = interval;
        } else {
            context->avgFrameIntervalUs += FEED_EMA_ALPHA * (interval - context->avgFrameIntervalUs);
        }
        if (renderBufferLength >= 0) {
            /* Estimate latency as the queue depth times the average feed interval. An empty queue is counted
             * as half a frame since the current frame is somewhere between decode and display. */
            float bufLen = renderBufferLength > 0 ? (float) renderBufferLength : 0.5f;
            float latency = bufLen * context->avgFrameIntervalUs;
            SS4S_NDL_webOS5_Lib->VideoStats.ReportFrame(context->player, (uint32_t) latency);
        }
        /* If NDL_DirectVideoPlay itself takes longer than the frame interval on average,
         * the decoder can't keep up with the incoming frame rate. */
        if (context->avgFrameIntervalUs > 0 && context->avgPlayDurationUs > context->avgFrameIntervalUs &&
            now - context->lastSaturationLogUs > SATURATION_LOG_INTERVAL_US) {
            context->lastSaturationLogUs = now;
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                "Decoder saturated: NDL_DirectVideoPlay takes %.0f us on average, "
                                "but frames arrive every %.0f us",
                                context->avgPlayDurationUs, context->avgFrameIntervalUs);
        }
    }
    if (renderBufferLength >= 0 && SS4S_NDL_webOS5_Lib->VideoStats.ReportQueueDepth != NULL) {
        SS4S_NDL_webOS5_Lib->VideoStats.ReportQueueDepth(context->player, renderBufferLength);
    }
    context->lastFrameTime = now;
    /* Low latency watchdog: when the render queue stays above the configured depth for longer than
     * the grace period, flush the queued frames and ask the host for an IDR frame. */
    if (SS4S_NDL_webOS5_Config.lowLatency && renderBufferLength >= 0) {
        if (renderBufferLength > SS4S_NDL_webOS5_Config.maxQueueFrames) {
            if (context->queueOverloadSinceUs == 0) {
                context->queueOverloadSinceUs = now;
            } else if (now - context->queueOverloadSinceUs > QUEUE_OVERLOAD_GRACE_US) {
                context->queueOverloadSinceUs = 0;
                int flushRc = FlushRenderBuffer();
                SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                    "Render queue stuck at %d frames (limit %d), flushed (rc=%d) and "
                                    "requesting keyframe", renderBufferLength,
                                    SS4S_NDL_webOS5_Config.maxQueueFrames, flushRc);
                return SS4S_VIDEO_FEED_REQUEST_KEYFRAME;
            }
        } else {
            context->queueOverloadSinceUs = 0;
        }
    }
    return SS4S_VIDEO_FEED_OK;
}

static bool SizeChanged(SS4S_VideoInstance *instance, int width, int height) {
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_PlayerContext *context = (void *) instance;
    if (width <= 0 || height <= 0) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return false;
    }
    int aspectRatio = width * 100 / height;
    if (context->aspectRatio != aspectRatio) {
        ReloadWithSize(context, width, height);
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    return true;
}

static bool SetHDRInfo(SS4S_VideoInstance *instance, const SS4S_VideoHDRInfo *info) {
    SS4S_PlayerContext *context = (void *) instance;
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    bool hasHdrInfo = info != NULL, needsReload = context->hasHdrInfo && !hasHdrInfo;
    context->hasHdrInfo = hasHdrInfo;
    if (needsReload) {
        int reloadResult = SS4S_NDL_webOS5_ReloadMedia(context);
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return reloadResult == 0;
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    if (info == NULL) {
        return true;
    }
    NDL_DIRECTVIDEO_HDR_INFO_T hdrInfo = {
        .displayPrimariesX0 = info->displayPrimariesX.g,
        .displayPrimariesY0 = info->displayPrimariesY.g,
        .displayPrimariesX1 = info->displayPrimariesX.b,
        .displayPrimariesY1 = info->displayPrimariesY.b,
        .displayPrimariesX2 = info->displayPrimariesX.r,
        .displayPrimariesY2 = info->displayPrimariesY.r,
        .whitePointX = info->whitePointX,
        .whitePointY = info->whitePointY,
        .maxDisplayMasteringLuminance = info->maxDisplayMasteringLuminance,
        .minDisplayMasteringLuminance = info->minDisplayMasteringLuminance,
        .maxContentLightLevel = info->maxContentLightLevel,
        .maxPicAverageLightLevel = info->maxPicAverageLightLevel,
        .transferCharacteristics = info->transferCharacteristics,
        .colorPrimaries = info->colorPrimaries,
        .matrixCoeffs = info->matrixCoefficients,
    };
    SS4S_NDL_webOS5_Lib->Log(SS4S_LogLevelInfo, "NDL", "Setting HDR info: "
                                                       "displayPrimariesX=[%d, %d, %d], displayPrimariesY=[%d, %d, %d], "
                                                       "whitePoint=[%d, %d], displayMasteringLuminance=[%d, %d], "
                                                       "maxContentLightLevel=%d, maxPicAverageLightLevel=%d, "
                                                       "transferCharacteristics=%d, colorPrimaries=%d, matrixCoeffs=%d",
                             hdrInfo.displayPrimariesX0, hdrInfo.displayPrimariesX1, hdrInfo.displayPrimariesX2,
                             hdrInfo.displayPrimariesY0, hdrInfo.displayPrimariesY1, hdrInfo.displayPrimariesY2,
                             hdrInfo.whitePointX, hdrInfo.whitePointY,
                             hdrInfo.maxDisplayMasteringLuminance, hdrInfo.minDisplayMasteringLuminance,
                             hdrInfo.maxContentLightLevel, hdrInfo.maxPicAverageLightLevel,
                             hdrInfo.transferCharacteristics, hdrInfo.colorPrimaries, hdrInfo.matrixCoeffs);

    return NDL_DirectVideoSetHDRInfo(hdrInfo) == 0;
}

static void CloseVideo(SS4S_VideoInstance *instance) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "CloseVideo called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_PlayerContext *context = (void *) instance;
    context->mediaInfo.video.type = 0;
    SS4S_NDL_webOS5_UnloadMedia(context);
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
}

static SS4S_VideoOpenResult ReloadWithSize(SS4S_PlayerContext *context, int width, int height) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "ReloadWithSize(%d, %d) called", width, height);
    context->mediaInfo.video.width = width;
    context->mediaInfo.video.height = height;
    context->aspectRatio = width * 100 / height;

    if (SS4S_NDL_webOS5_ReloadMedia(context) != 0) {
        return SS4S_VIDEO_OPEN_ERROR;
    }
    return SS4S_VIDEO_OPEN_OK;
}

static uint64_t GetTimeUs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000 + (uint64_t) ts.tv_nsec / 1000;
}

const SS4S_VideoDriver SS4S_NDL_webOS5_VideoDriver = {
    .Base = {
        .PostInit = SS4S_NDL_webOS5_Driver_PostInit,
        .Quit = SS4S_NDL_webOS5_Driver_Quit,
    },
    .GetCapabilities = GetCapabilities,
    .Open = OpenVideo,
    .Feed = FeedVideo,
    .SizeChanged = SizeChanged,
    .SetHDRInfo = SetHDRInfo,
    .Close = CloseVideo,
};
