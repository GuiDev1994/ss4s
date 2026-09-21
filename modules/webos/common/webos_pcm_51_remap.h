#pragma once

#include <stdint.h>

/**
 * NDL 6-channel PCM on this C5 + eARC bar, after WAVE decode
 * (FL FR C LFE RL RR) and host surroundParams=642014523:
 *
 *   0 E  (FL), 1 PD (RR), 2 D (FR), 3 PE (RL), 4 C, 5 Sub (LFE)
 *
 * Unity gain on every slot. PE is the correct speaker but quieter than PD;
 * digital boost (×2.5 wrap or +3 dB saturate) distorted on this eARC bar.
 * Do not send a second Sunshine surround-params on top.
 */
static inline void SS4S_WebOS_RemapPcm51ToDevice(const int16_t *in, int16_t *out, int frames) {
    for (int f = 0; f < frames; f++) {
        const int16_t *s = in + f * 6;
        int16_t *d = out + f * 6;
        const int16_t fl = s[0], fr = s[1], c = s[2], lfe = s[3], rl = s[4], rr = s[5];
        d[0] = fl;  /* E   */
        d[1] = rr;  /* PD  */
        d[2] = fr;  /* D   */
        d[3] = rl;  /* PE  */
        d[4] = c;   /* C   */
        d[5] = lfe; /* Sub */
    }
}
