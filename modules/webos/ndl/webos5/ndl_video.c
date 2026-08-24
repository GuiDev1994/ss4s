#include <string.h>
#include "ndl_common.h"
#include "highend_check.h"
#include "max_res.h"
#include "../../common/aurora_frame_diag.h"

static SS4S_VideoOpenResult ReloadWithSize(SS4S_PlayerContext *context, int width, int height);

static uint64_t GetTimeUs();

/*
 * AV1-only PTS. Same Series S 120-looks-like-60 failure: the sink vsync-schedules
 * on timestamps. HEVC on C5 ignores PTS (arrival = present). A synthetic 8 ms
 * *future* grid tells AV1 to wait for the next 60 Hz tick and drop every other
 * frame. Analog of DXGI ALLOW_TEARING: already-due, unique millisecond PTS so
 * Play() presents immediately. HEVC NextVideoPts is untouched.
 */
static uint64_t av1_last_pts_ms;
static bool av1_pts_inited;

static void Av1PtsReset(int fpsNum, int fpsDen) {
    (void) fpsNum;
    (void) fpsDen;
    av1_last_pts_ms = 0;
    av1_pts_inited = false;
}

static uint64_t Av1NextPtsMs(SS4S_PlayerContext *context) {
    uint64_t wall = SS4S_NDL_webOS5_GetPts(context);
    uint64_t pts = wall;
    if (av1_pts_inited && pts <= av1_last_pts_ms) {
        pts = av1_last_pts_ms + 1;
    }
    av1_pts_inited = true;
    av1_last_pts_ms = pts;
    return pts;
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
    SS4S_NDL_webOS5_ConfigureSmoothPacing(context, info->frameRateNumerator, info->frameRateDenominator);
    result = ReloadWithSize(context, info->width, info->height);
    if (result != SS4S_VIDEO_OPEN_OK) {
        goto finish;
    }
    *instance = (SS4S_VideoInstance *) context;
    result = SS4S_VIDEO_OPEN_OK;

    finish:
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    if (result == SS4S_VIDEO_OPEN_OK && info->codec == SS4S_VIDEO_AV1) {
        Av1PtsReset(info->frameRateNumerator, info->frameRateDenominator);
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "AV1 PTS immediate unique-ms (HEVC feed/PTS unchanged)");
    }
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenVideo returned %d", result);
    return result;
}

static SS4S_VideoFeedResult FeedVideoWithPTS(SS4S_VideoInstance *instance, const unsigned char *data, size_t size,
                                             SS4S_VideoFeedFlags flags, int64_t ptsUs) {
    (void) flags;
    SS4S_PlayerContext *context = (void *) instance;
    if (!context->mediaLoaded) {
        return SS4S_VIDEO_FEED_NOT_READY;
    }
    if (context->mediaInfo.video.type == NDL_VIDEO_TYPE_AV1) {
        pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
        uint64_t pts = Av1NextPtsMs(context);
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        int av1_rc = NDL_DirectVideoPlay((void *) data, size, (long long) pts);
        if (av1_rc != 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "AV1 NDL_DirectVideoPlay returned %d: %s",
                                av1_rc, NDL_DirectMediaGetError());
            return SS4S_VIDEO_FEED_ERROR;
        }
        /* Same overlay D as HEVC (queue × interval). Do not report Play() duration:
         * that is submit time (~0.2 ms), not decode, and made AV1 look "faster". */
        uint64_t now = GetTimeUs();
        int renderBufferLength = 0;
        if (context->lastFrameTime > 0 &&
            NDL_DirectVideoGetRenderBufferLength(&renderBufferLength) == 0) {
            float bufLen = renderBufferLength > 0 ? (float) renderBufferLength : 0.5f;
            float latency = bufLen * (float) (now - context->lastFrameTime);
            SS4S_NDL_webOS5_Lib->VideoStats.ReportFrame(context->player, (int) latency);
        }
        context->lastFrameTime = now;
        return SS4S_VIDEO_FEED_OK;
    }
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    /* Never block the decode thread on RENDERED_FRAME — a late compositor
     * callback cascades into the next feed interval (visible pan hitch). */
    const bool paced = SS4S_NDL_webOS5_RenderPacingEnabled(context);
    const bool diag = AuroraFrameDiagEnabled();
    int rq = -1;
    if (paced || diag) {
        int length = 0;
        if (NDL_DirectVideoGetRenderBufferLength(&length) == 0) {
            rq = length;
        }
    }
    uint64_t pts = paced ? SS4S_NDL_webOS5_RenderPacedPts(context, ptsUs, rq)
                         : SS4S_NDL_webOS5_NextVideoPts(context, ptsUs);
    if (paced && context->renderFrames % 3600 == 0) {
        SS4S_NDL_webOS5_LogRenderPacing(context);
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    uint64_t submitStart = diag ? AuroraFrameDiagNowNs() : 0;
    int rc = NDL_DirectVideoPlay((void *) data, size, (long long) pts);
    if (diag) {
        AuroraFrameDiagLogFeedAt(pts, rq, submitStart,
                                 (AuroraFrameDiagNowNs() - submitStart) / 1000ULL, (uint32_t) size);
    }
    if (rc != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectVideoPlay returned %d: %s", rc,
                            NDL_DirectMediaGetError());
        return SS4S_VIDEO_FEED_ERROR;
    }
    uint64_t now = GetTimeUs();
    int renderBufferLength = 0;
    if (context->lastFrameTime > 0 && NDL_DirectVideoGetRenderBufferLength(&renderBufferLength) == 0) {
        float bufLen = renderBufferLength > 0 ? (float) renderBufferLength : 0.5f;
        float latency = bufLen * (float) (now - context->lastFrameTime);
        SS4S_NDL_webOS5_Lib->VideoStats.ReportFrame(context->player, (int) latency);
    }
    context->lastFrameTime = now;
    return SS4S_VIDEO_FEED_OK;
}

static SS4S_VideoFeedResult FeedVideo(SS4S_VideoInstance *instance, const unsigned char *data, size_t size,
                                      SS4S_VideoFeedFlags flags) {
    return FeedVideoWithPTS(instance, data, size, flags, -1);
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
    Av1PtsReset(0, 0);
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
    .FeedWithPTS = FeedVideoWithPTS,
    .SizeChanged = SizeChanged,
    .SetHDRInfo = SetHDRInfo,
    .Close = CloseVideo,
};
