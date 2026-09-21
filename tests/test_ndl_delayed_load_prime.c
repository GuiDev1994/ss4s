/* Lifecycle: delayed LOADCOMPLETED must keep the audio plane primed.
 *
 * Mirrors the pf-webOS NDL v2 invariant (client-webos f93738278 / PR #249):
 *   1. load starts with an audio arm
 *   2. prime runs before confirmation
 *   3. the initial prime window expires
 *   4. LOADCOMPLETED arrives late
 *   5. prime resumes and the first video feed is accepted
 *
 * Mock controls ride SS4S_NDL_MOCK_* environment variables because the NDL
 * mock is linked into the ndl-webos5 module (.so) with hidden visibility.
 */

#include "ss4s.h"
#include "test_common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *STATS_PATH = "/tmp/ss4s_ndl_delayed_load_prime.stats";

static int read_stat(const char *key) {
    FILE *f = fopen(STATS_PATH, "r");
    if (f == NULL) {
        return -1;
    }
    char line[128];
    int value = -1;
    size_t key_len = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            value = atoi(line + key_len + 1);
            break;
        }
    }
    fclose(f);
    return value;
}

int main(int argc, char *argv[]) {
    setenv("APPID", "com.example.test", 1);
    setenv("SS4S_NDL_MOCK_STATS", STATS_PATH, 1);
    /* Prime budget in production is 500ms; confirm late so continuation must run. */
    setenv("SS4S_NDL_MOCK_LOAD_DELAY_MS", "800", 1);
    unlink(STATS_PATH);

    char driver[32] = {'\0'};
    single_test_infer_module(driver, sizeof(driver), "ss4s_test_ndl_delayed_load_prime_",
                             argc, argv);
    printf("Request A/V driver: %s\n", driver);
    if (!SS4S_ModuleAvailable(driver, SS4S_MODULE_CHECK_VIDEO | SS4S_MODULE_CHECK_AUDIO)) {
        printf("Skipping unsupported driver: %s\n", driver);
        return 127;
    }

    SS4S_Config config = {.audioDriver = driver, .videoDriver = driver};
    SS4S_Init(argc, argv, &config);
    SS4S_PostInit(argc, argv);

    SS4S_Player *player = SS4S_PlayerOpen();
    assert(player != NULL);
    SS4S_PlayerSetWaitAudioVideoReady(player, true);

    SS4S_VideoInfo video = {
            .codec = SS4S_VIDEO_H264,
            .width = 2560,
            .height = 1440,
            .frameRateNumerator = 120,
            .frameRateDenominator = 1,
    };
    SS4S_AudioInfo audio = {
            .codec = SS4S_AUDIO_PCM_S16LE,
            .numOfChannels = 2,
            .sampleRate = 48000,
            .samplesPerFrame = 240,
    };

    assert(SS4S_PlayerVideoOpen(player, &video) == SS4S_VIDEO_OPEN_OK);
    /* OpenAudio blocks through the 500ms prime budget when LOADCOMPLETED is late. */
    assert(SS4S_PlayerAudioOpen(player, &audio) == SS4S_AUDIO_OPEN_OK);

    int audio_after_open = read_stat("audio_play");
    printf("audio_play after open (post prime budget): %d\n", audio_after_open);
    assert(audio_after_open > 0);
    assert(read_stat("load_completed") == 0);

    /* Continuation must keep owning the plane until the late callback. */
    usleep(200 * 1000);
    int audio_during_wait = read_stat("audio_play");
    printf("audio_play during late wait: %d\n", audio_during_wait);
    assert(audio_during_wait > audio_after_open);

    int spins = 0;
    while (read_stat("load_completed") != 1 && spins < 50) {
        usleep(20 * 1000);
        spins++;
    }
    assert(read_stat("load_completed") == 1);
    printf("LOADCOMPLETED observed\n");

    unsigned char frame[64] = {0};
    SS4S_VideoFeedResult accepted = SS4S_PlayerVideoFeed(player, frame, sizeof(frame),
                                                         SS4S_VIDEO_FEED_DATA_FRAME_START |
                                                         SS4S_VIDEO_FEED_DATA_FRAME_END);
    printf("post-LOADCOMPLETED feed result: %d\n", accepted);
    assert(accepted == SS4S_VIDEO_FEED_OK);
    assert(read_stat("video_play") >= 1);

    SS4S_PlayerAudioClose(player);
    SS4S_PlayerVideoClose(player);
    SS4S_PlayerClose(player);
    SS4S_Quit();
    unlink(STATS_PATH);
    printf("OK: delayed LOADCOMPLETED prime lifecycle\n");
    return 0;
}
