/*  This file is part of YabaSanshiro NX.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// Switch sound output: the SCSP's 44.1 kHz samples go into a ring buffer, and a
// thread of our own resamples them to audout's 48 kHz, nudging the rate by up
// to half a percent so the ring stays at its target level (the two clocks
// never quite agree). Replaces SDL audio, whose 2048-sample periods and fixed
// rate left the buffer either starved or overflowing.

#pragma once

#include "../scsp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SNDCORE_NX 21

extern SoundInterface_struct SNDNX;

// 44.1 kHz frames waiting in the ring; 0xFFFFFFFF when the core isn't running
u32 SndNxBufferedSamples(void);

typedef struct {
  u32 underruns;      // times the ring ran dry
  u32 dropped;        // frames thrown away because the ring was full
  u32 fill_min;       // lowest ring level seen when a period was produced
  u32 fill_avg;       // average ring level
  float ratio;        // average rate correction (1.0 = none)
} SndNxStats;

// Stats since the last call
void SndNxTakeStats(SndNxStats * stats);

// Record `seconds` of the output (48 kHz) and of the SCSP's own samples
// (44.1 kHz), starting `start` seconds after the game starts, to
// sdmc:/switch/yabasanshiro/audio_out.wav and audio_in.wav. seconds 0 = off.
// Call before init.
void SndNxSetDump(int start, int seconds);

#ifdef __cplusplus
}
#endif
