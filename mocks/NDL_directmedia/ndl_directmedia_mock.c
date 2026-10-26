#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "NDL_directmedia.h"
#include "ndl_directmedia_mock.h"

bool ndl_mock_init;
bool audio_opened = false, video_opened = false;

static pthread_mutex_t pipeline_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mock_state_lock = PTHREAD_MUTEX_INITIALIZER;

static NDLMediaLoadCallback g_load_callback = NULL;
static int g_load_completed_delay_ms = -1;
static bool g_load_completed_fired = false;
static int g_audio_play_count = 0;
static int g_video_play_count = 0;

static pthread_t g_delayed_load_thread;
static bool g_delayed_load_thread_running = false;
static int g_delayed_load_generation = 0;

static void mock_ndl_write_stats_unlocked(void) {
    const char *path = getenv("SS4S_NDL_MOCK_STATS");
    if (path == NULL || path[0] == '\0') {
        return;
    }
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "audio_play=%d\nvideo_play=%d\nload_completed=%d\n",
            g_audio_play_count, g_video_play_count, g_load_completed_fired ? 1 : 0);
    fclose(f);
}

static int mock_ndl_env_delay_ms(void) {
    const char *env = getenv("SS4S_NDL_MOCK_LOAD_DELAY_MS");
    if (env == NULL || env[0] == '\0') {
        return g_load_completed_delay_ms;
    }
    return atoi(env);
}

void mock_ndl_lock(const char *func) {
    if (pthread_mutex_trylock(&pipeline_lock) != 0) {
        fprintf(stderr, "[NDL] mock_pipeline_lock failed on %s\n", func);
        abort();
    }
}

void mock_ndl_unlock(const char *func) {
    if (pthread_mutex_unlock(&pipeline_lock) != 0) {
        fprintf(stderr, "[NDL] mock_pipeline_unlock failed on %s\n", func);
        abort();
    }
}

void mock_ndl_reset(void) {
    pthread_mutex_lock(&mock_state_lock);
    g_delayed_load_generation++;
    g_load_callback = NULL;
    g_load_completed_delay_ms = -1;
    g_load_completed_fired = false;
    g_audio_play_count = 0;
    g_video_play_count = 0;
    mock_ndl_write_stats_unlocked();
    pthread_mutex_unlock(&mock_state_lock);

    if (g_delayed_load_thread_running) {
        pthread_join(g_delayed_load_thread, NULL);
        g_delayed_load_thread_running = false;
    }

    audio_opened = false;
    video_opened = false;
    ndl_mock_init = false;
}

void mock_ndl_set_load_completed_delay_ms(int delay_ms) {
    pthread_mutex_lock(&mock_state_lock);
    g_load_completed_delay_ms = delay_ms;
    pthread_mutex_unlock(&mock_state_lock);
}

void mock_ndl_fire_load_completed(void) {
    NDLMediaLoadCallback cb;
    pthread_mutex_lock(&mock_state_lock);
    if (g_load_completed_fired) {
        pthread_mutex_unlock(&mock_state_lock);
        return;
    }
    g_load_completed_fired = true;
    cb = g_load_callback;
    mock_ndl_write_stats_unlocked();
    pthread_mutex_unlock(&mock_state_lock);
    if (cb != NULL) {
        cb(0x16, 0, "LOADCOMPLETED");
    }
}

bool mock_ndl_load_completed_fired(void) {
    pthread_mutex_lock(&mock_state_lock);
    bool fired = g_load_completed_fired;
    pthread_mutex_unlock(&mock_state_lock);
    return fired;
}

int mock_ndl_audio_play_count(void) {
    pthread_mutex_lock(&mock_state_lock);
    int count = g_audio_play_count;
    pthread_mutex_unlock(&mock_state_lock);
    return count;
}

int mock_ndl_video_play_count(void) {
    pthread_mutex_lock(&mock_state_lock);
    int count = g_video_play_count;
    pthread_mutex_unlock(&mock_state_lock);
    return count;
}

NDLMediaLoadCallback mock_ndl_load_callback(void) {
    pthread_mutex_lock(&mock_state_lock);
    NDLMediaLoadCallback cb = g_load_callback;
    pthread_mutex_unlock(&mock_state_lock);
    return cb;
}

void mock_ndl_note_audio_play(void) {
    pthread_mutex_lock(&mock_state_lock);
    g_audio_play_count++;
    mock_ndl_write_stats_unlocked();
    pthread_mutex_unlock(&mock_state_lock);
}

void mock_ndl_note_video_play(void) {
    pthread_mutex_lock(&mock_state_lock);
    g_video_play_count++;
    mock_ndl_write_stats_unlocked();
    pthread_mutex_unlock(&mock_state_lock);
}

void mock_ndl_store_load_callback(NDLMediaLoadCallback callback) {
    pthread_mutex_lock(&mock_state_lock);
    g_load_callback = callback;
    g_load_completed_fired = false;
    mock_ndl_write_stats_unlocked();
    pthread_mutex_unlock(&mock_state_lock);
}

int mock_ndl_load_completed_delay_ms(void) {
    return mock_ndl_env_delay_ms();
}

static void *delayed_load_completed_thread(void *arg) {
    int generation = (int) (intptr_t) arg;
    int delay_ms = mock_ndl_load_completed_delay_ms();
    if (delay_ms > 0) {
        usleep((useconds_t) delay_ms * 1000);
    }
    pthread_mutex_lock(&mock_state_lock);
    bool still_current = (generation == g_delayed_load_generation);
    pthread_mutex_unlock(&mock_state_lock);
    if (still_current) {
        mock_ndl_fire_load_completed();
    }
    return NULL;
}

void mock_ndl_maybe_schedule_load_completed(void) {
    int delay = mock_ndl_load_completed_delay_ms();
    if (delay < 0) {
        return;
    }
    if (delay == 0) {
        mock_ndl_fire_load_completed();
        return;
    }
    pthread_mutex_lock(&mock_state_lock);
    int generation = g_delayed_load_generation;
    pthread_mutex_unlock(&mock_state_lock);
    if (g_delayed_load_thread_running) {
        pthread_join(g_delayed_load_thread, NULL);
        g_delayed_load_thread_running = false;
    }
    if (pthread_create(&g_delayed_load_thread, NULL, delayed_load_completed_thread,
                       (void *) (intptr_t) generation) == 0) {
        g_delayed_load_thread_running = true;
    }
}

int NDL_DirectMediaInit(const char *app_id, ResourceReleased cb) {
    (void) cb;
    assert(app_id != NULL);
    mock_ndl_lock(__func__);
    if (ndl_mock_init) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    ndl_mock_init = true;
    mock_ndl_unlock(__func__);
    return 0;
}

const char *NDL_DirectMediaGetError() {
    return "";
}

int NDL_DirectMediaQuit() {
    mock_ndl_lock(__func__);
    if (!ndl_mock_init) {
        mock_ndl_unlock(__func__);
        return -1;
    }
    ndl_mock_init = false;
    mock_ndl_unlock(__func__);
    return 0;
}
