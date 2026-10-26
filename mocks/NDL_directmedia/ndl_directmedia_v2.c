#define NDL_DIRECTMEDIA_API_VERSION 2

#include <unistd.h>
#include "NDL_directmedia.h"

#include "ndl_directmedia_mock.h"

int NDL_DirectMediaUnload(void) {
    mock_ndl_lock(__func__);
    if (!audio_opened && !video_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    audio_opened = false;
    video_opened = false;
    mock_ndl_unlock(__func__);
    return 0;
}

int NDL_DirectMediaLoad(NDL_DIRECTMEDIA_DATA_INFO_T *data, NDLMediaLoadCallback callback) {
    mock_ndl_lock(__func__);
    if (audio_opened || video_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    /* Load acceptance is immediate; LOADCOMPLETED is scheduled separately so tests can
     * expire the audio-prime budget before confirmation (webOS 26 / issue #188 shape). */
    audio_opened = data->audio.type != 0;
    video_opened = data->video.type != 0;
    mock_ndl_store_load_callback(callback);
    mock_ndl_unlock(__func__);
    mock_ndl_maybe_schedule_load_completed();
    return 0;
}

int NDL_DirectAudioSupportMultiChannel(int *isSupported) {
    mock_ndl_lock(__func__);
    usleep(100000);
    *isSupported = 0;
    mock_ndl_unlock(__func__);
    return 0;
}

int NDL_DirectVideoGetRenderBufferLength(int *length) {
    mock_ndl_lock(__func__);
    if (!video_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    *length = 0;
    mock_ndl_unlock(__func__);
    return 0;
}

int NDL_DirectVideoSetHDRInfo(NDL_DIRECTVIDEO_HDR_INFO_T hdrInfo) {
    (void) hdrInfo;
    mock_ndl_lock(__func__);
    if (!video_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    mock_ndl_unlock(__func__);
    return 0;
}

/* webOS5 links these 3-arg entry points by name. Keep them here so the mock matches the
 * v2 header; v1's 2-arg copies remain for webos4 and share the same counters via notes. */
int NDL_DirectAudioPlay(void *buffer, unsigned int size, long long pts) {
    (void) buffer;
    (void) size;
    (void) pts;
    mock_ndl_lock(__func__);
    if (!audio_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    mock_ndl_note_audio_play();
    mock_ndl_unlock(__func__);
    return 0;
}

int NDL_DirectVideoPlay(void *buffer, unsigned int size, long long pts) {
    (void) buffer;
    (void) size;
    (void) pts;
    mock_ndl_lock(__func__);
    if (!video_opened) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    mock_ndl_note_video_play();
    mock_ndl_unlock(__func__);
    return 0;
}

int NDL_DirectVideoSetFrameDropThreshold(int threshold) {
    (void) threshold;
    return 0;
}

int NDL_DirectVideoFlushRenderBuffer(void) {
    return 0;
}
