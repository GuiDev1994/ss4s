#include "opus_empty.h"
#include "opus_empty_samples.h"

#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>

struct SS4S_OpusEmpty {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    SS4S_NDLOpusEmptyFeedFunc feed;
    void *feedArg;

    uint32_t channels, streams, coupled, frameDurationUs;
    bool started, running, feeding;
    struct timespec feedEmptyAfter;
};

static void *OpusEmptyProc(void *arg);

static int OpusEmptyPlayWith(const SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg);

SS4S_OpusEmpty *SS4S_OpusEmptyCreate(uint32_t channels, uint32_t streams, uint32_t coupled,
                                     uint32_t frameDurationUs) {
    SS4S_OpusEmpty *instance = calloc(1, sizeof(SS4S_OpusEmpty));
    if (!instance) {
        return NULL;
    }
    pthread_mutex_init(&instance->lock, NULL);
    pthread_cond_init(&instance->cond, NULL);
    instance->channels = channels;
    instance->streams = streams;
    instance->coupled = coupled;
    instance->frameDurationUs = frameDurationUs ? frameDurationUs : 5000;
    if (pthread_create(&instance->thread, NULL, OpusEmptyProc, instance) != 0) {
        pthread_cond_destroy(&instance->cond);
        pthread_mutex_destroy(&instance->lock);
        free(instance);
        return NULL;
    }
    return instance;
}

void SS4S_OpusEmptyStart(SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg) {
    if (!instance || !feed) {
        return;
    }
    pthread_mutex_lock(&instance->lock);
    instance->feed = feed;
    instance->feedArg = feedArg;
    instance->started = true;
    instance->running = true;
    clock_gettime(CLOCK_MONOTONIC, &instance->feedEmptyAfter);
    /* Arm after 10 frames so continuousAudio hosts (5 ms cadence) never trip. */
    instance->feedEmptyAfter.tv_nsec += (int64_t) instance->frameDurationUs * 1000 * 10;
    while (instance->feedEmptyAfter.tv_nsec >= 1000000000L) {
        instance->feedEmptyAfter.tv_sec++;
        instance->feedEmptyAfter.tv_nsec -= 1000000000L;
    }
    pthread_cond_signal(&instance->cond);
    pthread_mutex_unlock(&instance->lock);
}

bool SS4S_OpusEmptyFrameArrived(SS4S_OpusEmpty *instance) {
    if (!instance) {
        return false;
    }
    pthread_mutex_lock(&instance->lock);
    clock_gettime(CLOCK_MONOTONIC, &instance->feedEmptyAfter);
    instance->feedEmptyAfter.tv_nsec += (int64_t) instance->frameDurationUs * 1000 * 10;
    while (instance->feedEmptyAfter.tv_nsec >= 1000000000L) {
        instance->feedEmptyAfter.tv_sec++;
        instance->feedEmptyAfter.tv_nsec -= 1000000000L;
    }
    bool feeding = instance->feeding;
    pthread_mutex_unlock(&instance->lock);
    return feeding;
}

int SS4S_OpusEmptyFeed(SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg) {
    if (!instance || !feed) {
        return 0;
    }
    return OpusEmptyPlayWith(instance, feed, feedArg);
}

void SS4S_OpusEmptyDestroy(SS4S_OpusEmpty *instance) {
    if (!instance) {
        return;
    }
    pthread_mutex_lock(&instance->lock);
    instance->started = true;
    instance->running = false;
    pthread_cond_signal(&instance->cond);
    pthread_mutex_unlock(&instance->lock);
    pthread_join(instance->thread, NULL);
    pthread_cond_destroy(&instance->cond);
    pthread_mutex_destroy(&instance->lock);
    free(instance);
}

static void WaitForStart(SS4S_OpusEmpty *instance) {
    pthread_mutex_lock(&instance->lock);
    while (!instance->started) {
        pthread_cond_wait(&instance->cond, &instance->lock);
    }
    pthread_mutex_unlock(&instance->lock);
}

static bool IsRunning(SS4S_OpusEmpty *instance) {
    pthread_mutex_lock(&instance->lock);
    bool running = instance->running;
    pthread_mutex_unlock(&instance->lock);
    return running;
}

static int64_t TimeDiffUs(const struct timespec *a, const struct timespec *b) {
    return (int64_t) (a->tv_sec - b->tv_sec) * 1000000LL +
           (int64_t) (a->tv_nsec - b->tv_nsec) / 1000LL;
}

static bool IsPastFeedEmptyAfter(SS4S_OpusEmpty *instance) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    pthread_mutex_lock(&instance->lock);
    bool ret = TimeDiffUs(&now, &instance->feedEmptyAfter) >= 0;
    pthread_mutex_unlock(&instance->lock);
    return ret;
}

static void *OpusEmptyProc(void *arg) {
    SS4S_OpusEmpty *instance = (SS4S_OpusEmpty *) arg;
    WaitForStart(instance);
    while (IsRunning(instance)) {
        if (IsPastFeedEmptyAfter(instance)) {
            pthread_mutex_lock(&instance->lock);
            instance->feeding = true;
            SS4S_NDLOpusEmptyFeedFunc feed = instance->feed;
            void *feedArg = instance->feedArg;
            pthread_mutex_unlock(&instance->lock);
            if (feed) {
                OpusEmptyPlayWith(instance, feed, feedArg);
            }
            usleep(instance->frameDurationUs);
            pthread_mutex_lock(&instance->lock);
            instance->feeding = false;
            pthread_mutex_unlock(&instance->lock);
        } else {
            usleep(instance->frameDurationUs);
        }
    }
    return NULL;
}

static int OpusEmptyPlayWith(const SS4S_OpusEmpty *instance, SS4S_NDLOpusEmptyFeedFunc feed, void *feedArg) {
    switch (instance->channels * 100 + instance->streams * 10 + instance->coupled) {
        case 220:
            return feed(feedArg, opus_empty_frame_220, sizeof(opus_empty_frame_220));
        case 211:
            return feed(feedArg, opus_empty_frame_211, sizeof(opus_empty_frame_211));
        case 660:
            /* Fallthrough: re-encode path remaps to 642. */
        case 642:
            return feed(feedArg, opus_empty_frame_642, sizeof(opus_empty_frame_642));
        case 880:
            return feed(feedArg, opus_empty_frame_880, sizeof(opus_empty_frame_880));
        case 853:
            return feed(feedArg, opus_empty_frame_853, sizeof(opus_empty_frame_853));
        default:
            return 0;
    }
}
