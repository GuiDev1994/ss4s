#include "ss4s/modapi.h"

#include <pulse/simple.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/error.h>
#include <assert.h>
#include <stdint.h>

static const SS4S_LibraryContext *LibContext;

struct SS4S_AudioInstance {
    pa_simple *dev;
};

static bool GetCapabilities(SS4S_AudioCapabilities *capabilities, SS4S_AudioCodec wantedCodecs) {
    (void) wantedCodecs;
    capabilities->codecs = SS4S_AUDIO_PCM_S16LE;
    capabilities->maxChannels = 8;
    return true;
}

static SS4S_AudioOpenResult Open(const SS4S_AudioInfo *info, SS4S_AudioInstance **instance,
                                 SS4S_PlayerContext *context) {
    (void) context;
    if (info->codec != SS4S_AUDIO_PCM_S16LE) {
        return SS4S_AUDIO_OPEN_UNSUPPORTED_CODEC;
    }
    pa_sample_spec spec = {
            .format = PA_SAMPLE_S16LE,
            .rate = info->sampleRate,
            .channels = info->numOfChannels,
    };
    size_t frame_size = info->samplesPerFrame * info->numOfChannels * sizeof(uint16_t);
    pa_buffer_attr buffer_attr = {
            .maxlength = (uint32_t) (frame_size * 8 /*40ms*/),
            .tlength = (uint32_t) -1,
            .prebuf = (uint32_t) -1,
            .minreq = (uint32_t) -1,
    };
    /* WAVE / SDL / Vorbis order — matches Moonlight Opus decode output. */
    pa_channel_map channel_map;
    pa_channel_map_init_auto(&channel_map, info->numOfChannels, PA_CHANNEL_MAP_WAVEEX);

    assert(info->appName != NULL);
    assert(info->streamName != NULL);
    int error = 0;
    pa_simple *dev = pa_simple_new(NULL, info->appName, PA_STREAM_PLAYBACK, NULL,
                                   info->streamName, &spec, &channel_map, &buffer_attr, &error);
    if (error != 0) {
        LibContext->Log(SS4S_LogLevelError, "Pulse", "Can't open audio device: %s", pa_strerror(error));
    }
    if (!dev) {
        return SS4S_AUDIO_OPEN_ERROR;
    }
    LibContext->Log(SS4S_LogLevelInfo, "Pulse", "Opened %d ch @ %d Hz (WAVEEX map)",
                    info->numOfChannels, info->sampleRate);
    SS4S_AudioInstance *newInstance = calloc(1, sizeof(SS4S_AudioInstance));
    newInstance->dev = dev;
    *instance = newInstance;
    return SS4S_AUDIO_OPEN_OK;
}

static SS4S_AudioFeedResult Feed(SS4S_AudioInstance *instance, const unsigned char *data, size_t size) {
    int error = 0;
    pa_simple_write(instance->dev, data, size, &error);
    return SS4S_AUDIO_FEED_OK;
}

static void Close(SS4S_AudioInstance *instance) {
    pa_simple_free(instance->dev);
    free(instance);
}

static const SS4S_AudioDriver PulseDriver = {
        .GetCapabilities = GetCapabilities,
        .Open = Open,
        .Feed = Feed,
        .Close = Close,
};

SS4S_EXPORTED bool SS4S_ModuleOpen_PULSE(SS4S_Module *module, const SS4S_LibraryContext *context) {
    module->Name = "pulse";
    module->AudioDriver = &PulseDriver;
    LibContext = context;
    return true;
}
