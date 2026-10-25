/* NDL clears the frame drop threshold on unload, so it must be armed before every NDL_DirectMediaLoad. */

#include "ss4s.h"
#include "test_common.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *STATS_PATH = "/tmp/ss4s_ndl_frame_drop_threshold.stats";

static int read_stat(const char *key) {
    FILE *f = fopen(STATS_PATH, "r");
    if (f == NULL) {
        return 0;
    }
    char line[128];
    int value = 0;
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

static void set_env(const char *name, const char *value) {
    if (value != NULL) {
        setenv(name, value, 1);
    } else {
        unsetenv(name);
    }
}

static SS4S_Player *open_stream(SS4S_VideoCodec codec, int width, int height) {
    SS4S_Player *player = SS4S_PlayerOpen();
    assert(player != NULL);
    /* Like Aurora: the load waits for both arms, so the audio open performs it. */
    SS4S_PlayerSetWaitAudioVideoReady(player, true);
    SS4S_VideoInfo video = {
            .codec = codec,
            .width = width,
            .height = height,
            .frameRateNumerator = 120,
            .frameRateDenominator = 1,
    };
    SS4S_AudioInfo audio = {
            .codec = SS4S_AUDIO_PCM_S16LE,
            .numOfChannels = 2,
            .sampleRate = 48000,
            .samplesPerFrame = 240,
    };
    SS4S_VideoOpenResult video_result = SS4S_PlayerVideoOpen(player, &video);
    SS4S_AudioOpenResult audio_result = SS4S_PlayerAudioOpen(player, &audio);
    assert(video_result == SS4S_VIDEO_OPEN_OK);
    assert(audio_result == SS4S_AUDIO_OPEN_OK);
    (void) video_result;
    (void) audio_result;
    return player;
}

static void close_stream(SS4S_Player *player) {
    SS4S_PlayerAudioClose(player);
    SS4S_PlayerVideoClose(player);
    SS4S_PlayerClose(player);
}

/* Opens one stream and checks the threshold its load picked up (0 = none, and no call made). */
static void check_case(const char *name, const char *threshold_env, const char *render_target_env,
                       SS4S_VideoCodec codec, int width, int height, int expected) {
    set_env("SS4S_NDL_FRAME_DROP_THRESHOLD", threshold_env);
    set_env("SS4S_RENDER_QUEUE_TARGET", render_target_env);
    int loads = read_stat("load_count");
    int calls = read_stat("drop_threshold_calls");

    SS4S_Player *player = open_stream(codec, width, height);
    int load_threshold = read_stat("load_drop_threshold");
    int new_calls = read_stat("drop_threshold_calls") - calls;
    printf("%s: loads +%d, threshold calls +%d, load picked up %d (expected %d)\n", name,
           read_stat("load_count") - loads, new_calls, load_threshold, expected);
    assert(read_stat("load_count") == loads + 1);
    assert(load_threshold == expected);
    assert(new_calls == (expected > 0 ? 1 : 0));
    close_stream(player);

    unsetenv("SS4S_NDL_FRAME_DROP_THRESHOLD");
    unsetenv("SS4S_RENDER_QUEUE_TARGET");
}

int main(int argc, char *argv[]) {
    setenv("APPID", "com.example.test", 1);
    setenv("SS4S_NDL_MOCK_STATS", STATS_PATH, 1);
    setenv("SS4S_NDL_MOCK_LOAD_DELAY_MS", "0", 1);
    unlink(STATS_PATH);

    char driver[32] = {'\0'};
    single_test_infer_module(driver, sizeof(driver), "ss4s_test_ndl_frame_drop_threshold_", argc, argv);
    printf("Request A/V driver: %s\n", driver);
    if (!SS4S_ModuleAvailable(driver, SS4S_MODULE_CHECK_VIDEO | SS4S_MODULE_CHECK_AUDIO)) {
        printf("Skipping unsupported driver: %s\n", driver);
        return 127;
    }

    SS4S_Config config = {.audioDriver = driver, .videoDriver = driver};
    SS4S_Init(argc, argv, &config);
    SS4S_PostInit(argc, argv);

    check_case("HEVC 4K, unset", NULL, NULL, SS4S_VIDEO_H265, 3840, 2160, 2);
    check_case("HEVC 4K, \"0\"", "0", NULL, SS4S_VIDEO_H265, 3840, 2160, 0);
    check_case("HEVC 4K, \"1\"", "1", NULL, SS4S_VIDEO_H265, 3840, 2160, 0);
    check_case("AV1 4K, unset", NULL, NULL, SS4S_VIDEO_AV1, 3840, 2160, 0);
    check_case("AV1 4K, \"3\"", "3", NULL, SS4S_VIDEO_AV1, 3840, 2160, 0);
    check_case("HEVC 1080p, unset", NULL, NULL, SS4S_VIDEO_H265, 1920, 1080, 0);
    check_case("HEVC 1080p, \"3\"", "3", NULL, SS4S_VIDEO_H265, 1920, 1080, 3);
    check_case("HEVC 4K, unset, render target 2", NULL, "2", SS4S_VIDEO_H265, 3840, 2160, 3);
    check_case("HEVC 4K, \"5\", render target 2", "5", "2", SS4S_VIDEO_H265, 3840, 2160, 5);

    /* SizeChanged unloads, which clears the threshold, so the reload must arm it again. */
    SS4S_Player *player = open_stream(SS4S_VIDEO_H265, 3840, 2160);
    int loads = read_stat("load_count");
    int calls = read_stat("drop_threshold_calls");
    bool resized = SS4S_PlayerVideoSizeChanged(player, 3840, 1600);
    assert(resized);
    (void) resized;
    printf("SizeChanged 3840x1600: loads +%d, threshold calls +%d, load picked up %d\n",
           read_stat("load_count") - loads, read_stat("drop_threshold_calls") - calls,
           read_stat("load_drop_threshold"));
    assert(read_stat("load_count") == loads + 1);
    assert(read_stat("drop_threshold_calls") == calls + 1);
    assert(read_stat("load_drop_threshold") == 2);
    close_stream(player);

    SS4S_Quit();
    unlink(STATS_PATH);
    printf("OK: NDL frame drop threshold armed before every load\n");
    return 0;
}
