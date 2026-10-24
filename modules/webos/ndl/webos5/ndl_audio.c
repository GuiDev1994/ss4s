#include <stddef.h>
#include <dlfcn.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "ndl_common.h"
#include "opus_empty.h"
#include "opus_fix.h"

static bool IsOpusPassthroughSupported(const OpusConfig *config);

static bool ParseOpusConfig(const unsigned char *codecData, size_t codecDataLen, OpusConfig *config);

static int SupportsPCM6Channel = 0;

static int DriverInit(int argc, char *argv[]) {
    (void) argc;
    (void) argv;
    /*
     * webOS 7.0 introduced PCM 6-channel support as well as NDL_DirectAudioRegisterCallback.
     * We can use this to determine if the platform supports 6-channel PCM. 
     */
    SupportsPCM6Channel = dlsym(RTLD_DEFAULT, "NDL_DirectAudioRegisterCallback") != 0;
    return 0;
}

static bool GetCapabilities(SS4S_AudioCapabilities *capabilities, SS4S_AudioCodec wantedCodecs) {
    enum SS4S_AudioCodec opusCodec = SS4S_AUDIO_OPUS;
    SS4S_AudioCodec matchedCodecs = wantedCodecs & (SS4S_AUDIO_PCM_S16LE | opusCodec);
    if (matchedCodecs == 0) {
        return false;
    }
    capabilities->codecs = SS4S_AUDIO_PCM_S16LE | opusCodec;
    /*
     * Don't check for system settings to determine the number of channels, just provide 6 channels.
     * webOS should be able to down-mix that anyway.
     */
    capabilities->maxChannels = 6;
    return true;
}

static SS4S_AudioCodec GetPreferredCodecs(const SS4S_AudioInfo *info) {
    if (info->numOfChannels == 6) {
        return SS4S_AUDIO_OPUS;
    }
    return SS4S_AUDIO_PCM_S16LE;
}

static SS4S_AudioOpenResult OpenAudio(const SS4S_AudioInfo *info, SS4S_AudioInstance **instance,
                                      SS4S_PlayerContext *context) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    context->audioLeadMs = 0;
    context->audioPlcFrames = 0;
    context->audioWinFeeds = 0;
    context->audioWinRepeats = 0;
    context->audioWinMaxGapMs = 0;
    context->audioWinMaxAheadMs = 0;
    context->audioLastFeedMs = 0;
    context->audioWinShed = 0;
    context->audioFrameMs = 5;
    const char *seqEnv = getenv("SS4S_NDL_AUDIO_SEQUENTIAL_PTS");
    context->audioSequentialPts = info->codec == SS4S_AUDIO_OPUS && (seqEnv == NULL || strcmp(seqEnv, "0") != 0);
    const char *leadEnv = getenv("SS4S_NDL_AUDIO_LEAD_MS");
    if (leadEnv != NULL && leadEnv[0] != 0) {
        long lead = strtol(leadEnv, NULL, 10);
        if (lead > 0 && lead <= 150) {
            context->audioLeadMs = lead;
        }
    }
    if (info->samplesPerFrame > 0 && info->sampleRate > 0) {
        context->audioFrameMs = (int64_t) info->samplesPerFrame * 1000 / info->sampleRate;
        if (context->audioFrameMs <= 0) {
            context->audioFrameMs = 1;
        }
    }
    /* Pairs arrive together (~10 ms apart), so one frame ahead is normal; past a few frames
     * the host clock is running ahead of ours and the excess is shed instead of kept. */
    context->audioMaxAheadMs = 4 * context->audioFrameMs;
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Audio stamp lead %lld ms, sequential stamps %s (frame %lld ms)",
                        (long long) context->audioLeadMs, context->audioSequentialPts ? "on" : "off",
                        (long long) context->audioFrameMs);
    SS4S_AudioOpenResult result;
    switch (info->codec) {
        case SS4S_AUDIO_PCM_S16LE: {
            const char *mode = "stereo";
            if (info->numOfChannels == 1) {
                mode = "mono";
            } else if (info->numOfChannels == 6) {
                if (SupportsPCM6Channel) {
                    mode = "6-channel";
                } else {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "6-channel PCM is not supported, "
                                                                  "falling back to stereo");
                }
            }
            NDL_DIRECTAUDIO_PCM_INFO_T pcmInfo = {
                    .type = NDL_AUDIO_TYPE_PCM,
                    .format = NDL_DIRECTMEDIA_AUDIO_PCM_FORMAT_S16LE,
                    .channelMode = mode,
                    .sampleRate = NDL_DIRECTMEDIA_AUDIO_PCM_SAMPLE_RATE_OF(info->sampleRate),
            };
            context->mediaInfo.audio.pcm = pcmInfo;
            break;
        }
        case SS4S_AUDIO_OPUS: {
            NDL_DIRECTMEDIA_AUDIO_OPUS_INFO_T opusInfo = {
                    .type = NDL_AUDIO_TYPE_OPUS,
                    .channels = info->numOfChannels,
                    .sampleRate = info->sampleRate / 1000.0,
            };
            if (info->codecData && info->codecDataLen) {
                OpusConfig opusConfig;
                if (!ParseOpusConfig(info->codecData, info->codecDataLen, &opusConfig)) {
                    result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
                    goto finish;
                }
                /*
                 * Empty-frame helper is the LOADCOMPLETED prime only. A steady-state
                 * metronome (silence packets after the prime) is what punktfunk
                 * removed and what made 5.1 cut: those packets raced real Opus
                 * frames and the feed path dropped the real ones.
                 */
                uint32_t samplesPerFrame = info->samplesPerFrame ? info->samplesPerFrame : 240;
                uint32_t frameDurationUs = 1000000u /
                                           (opusConfig.sampleRate / samplesPerFrame);
                context->opusEmpty = SS4S_OpusEmptyCreate(opusConfig.channels, opusConfig.streamCount,
                                                          opusConfig.coupledCount, frameDurationUs);
                if (!context->opusEmpty) {
                    result = SS4S_AUDIO_OPEN_ERROR;
                    goto finish;
                }
                if (opusConfig.channels == 6) {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                                        "Opus 5.1 streams=%d coupled=%d mapping=[%u,%u,%u,%u,%u,%u] "
                                        "frame=%uus",
                                        opusConfig.streamCount, opusConfig.coupledCount,
                                        opusConfig.mapping[0], opusConfig.mapping[1], opusConfig.mapping[2],
                                        opusConfig.mapping[3], opusConfig.mapping[4], opusConfig.mapping[5],
                                        (unsigned) frameDurationUs);
                }
                if (opusConfig.channels == 6 && !IsOpusPassthroughSupported(&opusConfig)) {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL",
                                        "Channel config is not supported, enabling re-encoding. "
                                        "This will introduce audio latency");
                    context->opusFix = SS4S_NDLOpusFixCreate(&opusConfig);
                    if (!context->opusFix) {
                        result = SS4S_AUDIO_OPEN_ERROR;
                        goto finish;
                    }
                } else if (opusConfig.channels == 6) {
                    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "Opus 5.1 passthrough (no re-encode)");
                }
            }
            context->mediaInfo.audio.opus = opusInfo;
            break;
        }
        default: {
            result = SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
            goto finish;
        }
    }
    if (SS4S_NDL_webOS5_ReloadMedia(context) != 0) {
        result = SS4S_AUDIO_OPEN_ERROR;
        goto finish;
    }
    *instance = (SS4S_AudioInstance *) context;
    result = SS4S_AUDIO_OPEN_OK;

    finish:
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "OpenAudio returned %d", result);
    return result;
}

typedef struct {
    int64_t pts;
} PlcStamp;

static int PlayPlcSilence(void *arg, const unsigned char *data, size_t size) {
    const PlcStamp *stamp = arg;
    return NDL_DirectAudioPlay((void *) data, size, (long long) stamp->pts);
}

static SS4S_AudioFeedResult FeedAudio(SS4S_AudioInstance *instance, const unsigned char *data, size_t size) {
    SS4S_PlayerContext *context = (void *) instance;
    if (!context->mediaLoaded) {
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    /*
     * Limelight signals a lost packet with (NULL, 0) so the decoder can conceal it. Opus is
     * passed through to NDL undecoded, so there is no PLC here: feeding NULL fails and leaves
     * a hole in the plane. Substitute one silent frame of the same layout instead, stamped
     * like a real packet, so the timeline keeps moving.
     */
    const bool lost = data == NULL || size == 0;
    if (lost && context->opusEmpty == NULL) {
        return SS4S_AUDIO_FEED_OK;
    }
    if (!lost && context->opusFix) {
        int fixedSize = SS4S_NDLOpusFixProcess(context->opusFix, data, size);
        if (fixedSize < 0) {
            SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "SS4S_NDLOpusFixProcess returned %d", fixedSize);
            return SS4S_AUDIO_FEED_ERROR;
        }
        data = SS4S_NDLOpusFixGetBuffer(context->opusFix);
        size = fixedSize;
    }
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    if (!context->mediaLoaded) {
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_NOT_READY;
    }
    /*
     * Stamp on the player clock plus a fixed lead, and do not step below the last stamp.
     * A +5 ms or +1 ms per-packet grid ratchets ahead on every burst (the lead only grows)
     * and NDL then cuts 5.1; a constant lead does not. Floor at the last stamp so a packet
     * cannot rewind under the prime — a rewind mutes the plane. The lead is what lets a late
     * burst arrive before the plane runs dry (punktfunk: raise it first if offload drops out).
     */
    int64_t now_ms = (int64_t) SS4S_NDL_webOS5_GetPts(context);
    if (now_ms < 0) {
        now_ms = 0;
    }
    int64_t due_ms = now_ms + context->audioLeadMs;
    int64_t pts = context->lastAudioPtsMs > due_ms ? context->lastAudioPtsMs : due_ms;
    if (context->audioSequentialPts && context->lastAudioPtsMs > 0) {
        /*
         * Limelight hands packets over in pairs, so the arrival millisecond repeats for ~45% of
         * them (C5/G5 logs: 80-96 repeated stamps per 200). NDL's Opus plane treats a repeated
         * stamp as an overlapping buffer and drops audio: 5.1 cuts while PCM, which plays by
         * sample count, does not. Step one frame past the last stamp instead. That can only run
         * ahead (the old +5 ms grid ratcheted), so a packet more than audioMaxAheadMs ahead of
         * the clock is shed: one 5 ms frame lost beats a lead that never comes back.
         */
        int64_t next_ms = context->lastAudioPtsMs + context->audioFrameMs;
        if (next_ms > due_ms) {
            if (next_ms - due_ms > context->audioMaxAheadMs) {
                context->audioWinShed++;
                pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
                return SS4S_AUDIO_FEED_OK;
            }
            pts = next_ms;
        }
    }
    if (pts == context->lastAudioPtsMs) {
        context->audioWinRepeats++;
    }
    if (pts - due_ms > context->audioWinMaxAheadMs) {
        context->audioWinMaxAheadMs = pts - due_ms;
    }
    if (context->audioLastFeedMs > 0 && now_ms - context->audioLastFeedMs > context->audioWinMaxGapMs) {
        context->audioWinMaxGapMs = now_ms - context->audioLastFeedMs;
    }
    context->audioLastFeedMs = now_ms;
    context->audioWinFeeds++;
    context->lastAudioPtsMs = pts;
    int rc;
    if (lost) {
        PlcStamp stamp = {.pts = pts};
        rc = SS4S_OpusEmptyFeed(context->opusEmpty, PlayPlcSilence, &stamp);
        context->audioPlcFrames++;
    } else {
        rc = NDL_DirectAudioPlay((void *) data, size, (long long) pts);
    }
    if (rc != 0) {
        SS4S_NDL_webOS5_Log(SS4S_LogLevelWarn, "NDL", "NDL_DirectAudioPlay%s returned %d: %s",
                            lost ? " (PLC silence)" : "", rc, NDL_DirectMediaGetError());
        pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
        return SS4S_AUDIO_FEED_ERROR;
    }
    if (now_ms - context->lastAudioFillLogMs >= 1000) {
        context->lastAudioFillLogMs = now_ms;
        /* 5 ms Opus frames: ~200 feeds/s. Repeats = packets stamped on the previous stamp. */
        SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL",
                            "Audio feed 1s: %u packets, %u repeated stamps, max gap %lld ms, "
                            "max stamp ahead of due %lld ms, lead %lld ms, PLC %u, shed %u",
                            context->audioWinFeeds, context->audioWinRepeats,
                            (long long) context->audioWinMaxGapMs, (long long) context->audioWinMaxAheadMs,
                            (long long) context->audioLeadMs, context->audioPlcFrames, context->audioWinShed);
        context->audioWinFeeds = 0;
        context->audioWinRepeats = 0;
        context->audioWinMaxGapMs = 0;
        context->audioWinMaxAheadMs = 0;
        context->audioWinShed = 0;
    }
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    return SS4S_AUDIO_FEED_OK;
}

static void CloseAudio(SS4S_AudioInstance *instance) {
    SS4S_NDL_webOS5_Log(SS4S_LogLevelInfo, "NDL", "CloseAudio called");
    pthread_mutex_lock(&SS4S_NDL_webOS5_Lock);
    SS4S_PlayerContext *context = (void *) instance;
    context->mediaInfo.audio.type = 0;
    if (context->opusFix) {
        SS4S_NDLOpusFixDestroy(context->opusFix);
        context->opusFix = NULL;
    }
    /* Join the gap-filler outside the driver lock — its feed callback takes it. */
    SS4S_OpusEmpty *empty = context->opusEmpty;
    context->opusEmpty = NULL;
    SS4S_NDL_webOS5_UnloadMedia(context);
    pthread_mutex_unlock(&SS4S_NDL_webOS5_Lock);
    SS4S_OpusEmptyDestroy(empty);
}


bool IsOpusPassthroughSupported(const OpusConfig *config) {
    static const uint8_t wantedMapping[6] = {0, 1, 4, 5, 2, 3};
    return config->streamCount == 4 && config->coupledCount == 2 && memcmp(config->mapping, wantedMapping, 6) == 0;
}

bool ParseOpusConfig(const unsigned char *codecData, size_t codecDataLen, OpusConfig *config) {
    if (codecDataLen < 20 || memcmp(codecData, "OpusHead", 8) != 0) {
        return false;
    }
    memcpy(&config->sampleRate, codecData + 12, 4);
    config->channels = codecData[9];
    config->streamCount = codecData[19];
    config->coupledCount = codecData[20];
    if (config->channels > sizeof(config->mapping)) {
        // We can only represent up to sizeof(mapping) channels (5.1 layout).
        // Anything wider (e.g. 7.1 surround = 8 channels) would overflow the
        // fixed-size mapping array on the memcpy below.
        return false;
    }
    if ((int) codecDataLen >= 21 + config->channels) {
        memcpy(config->mapping, codecData + 21, config->channels);
    } else {
        config->mapping[0] = 0;
        config->mapping[1] = 0;
    }
    return true;
}

const SS4S_AudioDriver SS4S_NDL_webOS5_AudioDriver = {
        .Base = {
                .Init = DriverInit,
                .PostInit = SS4S_NDL_webOS5_Driver_PostInit,
                .Quit = SS4S_NDL_webOS5_Driver_Quit,
        },
        .GetCapabilities = GetCapabilities,
        .GetPreferredCodecs = GetPreferredCodecs,
        .Open = OpenAudio,
        .Feed = FeedAudio,
        .Close = CloseAudio,
};