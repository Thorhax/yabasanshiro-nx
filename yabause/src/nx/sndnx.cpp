/*  This file is part of YabaSanshiro NX.

    YabaSanshiro is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

// Switch sound output through audout. See sndnx.h for the design.

#include "sndnx.h"
#include "config.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <switch.h>

namespace {

constexpr u32 kInRate = 44100;
// Ring of stereo 16-bit frames at 44.1 kHz (power of two), about 370 ms
constexpr u32 kRingFrames = 16384;
constexpr u32 kRingMask = kRingFrames - 1;
// Frames the ring tries to hold: a bit over two video frames of sound. The
// SCSP delivers 735 frames at a time, once per emulated frame.
constexpr u32 kTarget = 2100;
// After running dry, wait for this much before playing again, so one late
// frame gives one short gap rather than a burst of crackle
constexpr u32 kRestart = 1100;
// audout periods: 4 buffers of 256 frames at 48 kHz (5.3 ms each)
constexpr int kBuffers = 4;
constexpr u32 kPeriod = 256;
constexpr size_t kBufferBytes = (kPeriod * 4 + 0xFFF) & ~0xFFF;
// Largest rate correction, as RetroArch's default
constexpr double kMaxDelta = 0.005;
constexpr int kFadeFrames = 48;

s16 * s_ring = nullptr;                // interleaved L R
std::atomic<u32> s_write{0};           // frames ever written
std::atomic<u32> s_read{0};            // frames ever consumed
std::atomic<bool> s_running{false};
std::atomic<bool> s_muted{false};
std::atomic<int> s_volume{256};        // 0 .. 256

Thread s_thread;
bool s_thread_made = false;
bool s_audout = false;
void * s_bufmem[kBuffers] = {};
AudioOutBuffer s_bufs[kBuffers] = {};
u32 s_out_rate = 48000;

// Stats, reset by SndNxTakeStats
std::atomic<u32> s_underruns{0};
std::atomic<u32> s_dropped{0};
std::atomic<u32> s_fill_min{0xFFFFFFFF};
std::atomic<u64> s_fill_sum{0};
std::atomic<u32> s_periods{0};
std::atomic<u64> s_ratio_sum_ppm{0};

// Debug capture
int s_dump_start = 0, s_dump_secs = 0;
s16 * s_dump_in = nullptr, * s_dump_out = nullptr;
u32 s_dump_in_len = 0, s_dump_out_len = 0;   // frames wanted
u32 s_dump_in_pos = 0, s_dump_out_pos = 0;   // frames captured
u64 s_in_total = 0, s_out_total = 0;         // frames seen since init
std::atomic<bool> s_dump_written{false};

void writeWav(const char * name, const s16 * data, u32 frames, u32 rate)
{
  const std::string path = nx::dataPath(name);
  FILE * fp = fopen(path.c_str(), "wb");
  if (!fp) return;
  const u32 bytes = frames * 4;
  u8 h[44];
  auto le32 = [&](int o, u32 v) { h[o] = v; h[o+1] = v >> 8; h[o+2] = v >> 16; h[o+3] = v >> 24; };
  auto le16 = [&](int o, u16 v) { h[o] = v; h[o+1] = v >> 8; };
  memcpy(h, "RIFF", 4); le32(4, 36 + bytes); memcpy(h + 8, "WAVEfmt ", 8);
  le32(16, 16); le16(20, 1); le16(22, 2); le32(24, rate); le32(28, rate * 4); le16(32, 4); le16(34, 16);
  memcpy(h + 36, "data", 4); le32(40, bytes);
  fwrite(h, 1, 44, fp);
  fwrite(data, 4, frames, fp);
  fclose(fp);
  printf("audio dump: wrote %s (%u frames)\n", path.c_str(), (unsigned)frames);
}

void writeDumps()
{
  if (!s_dump_secs || s_dump_written.exchange(true)) return;
  if (s_dump_in) writeWav("audio_in.wav", s_dump_in, s_dump_in_pos, kInRate);
  if (s_dump_out) writeWav("audio_out.wav", s_dump_out, s_dump_out_pos, s_out_rate);
}

inline s16 clamp16(s32 v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (s16)v); }

// Catmull-Rom between y1 and y2
inline float cubic(float y0, float y1, float y2, float y3, float t)
{
  const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
  const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
  const float c = -0.5f * y0 + 0.5f * y2;
  return ((a * t + b) * t + c) * t + y1;
}

// ---------------------------------------------------------------------------
// Time stretch (WSOLA). When the game runs slower than full speed it makes
// less than 44100 samples a second; rather than run dry every few frames
// (heard as stutter), stretch the sound to the length of time it actually
// takes, keeping its pitch. Segments of kWin frames are overlap-added with a
// Hann window every kHop output frames; each segment is taken from near where
// the slower input clock says it should be, at the offset that lines up best
// with the end of the previous one. At full speed the tempo is exactly 1 and
// the segments line up perfectly, so the sound passes through unchanged.
constexpr u32 kWin = 640;               // 14.5 ms
constexpr u32 kHop = kWin / 2;
constexpr int kSeek = 128;               // search +-2.9 ms
constexpr u32 kStFrames = 8192;          // stretched sound waiting for the resampler
constexpr u32 kStMask = kStFrames - 1;
float s_hann[kWin];
float * s_st = nullptr;                  // stretched frames, interleaved float L R
u32 s_st_w = 0, s_st_r = 0;
float s_olap[kHop * 2];                  // second half of the previous segment, windowed
double s_in_pos = 0.0;                   // input position (frames, relative to s_in_base)
u32 s_in_base = 0;
u32 s_q = 0;                             // start of the previous segment (absolute frame)
double s_tempo = 1.0;
double s_prod = 1.0;                     // smoothed rate the game makes sound at (1 = full speed)
u32 s_last_write = 0;

// Resampler state, owned by the output thread
double s_pos = 0.0;              // fractional position past s_st_r
double s_fill_smooth = kTarget;
bool s_playing = false;          // false: waiting for kRestart frames
int s_fade = 0;                  // fade-out progress after running dry
float s_lastL = 0, s_lastR = 0;  // last sample played, faded out on underrun

inline const s16 * inFrame(u32 abs) { return &s_ring[(abs & kRingMask) * 2]; }
inline float inMono(u32 abs) { const s16 * f = inFrame(abs); return (float)f[0] + (float)f[1]; }

void stretchReset()
{
  const u32 r = s_read.load(std::memory_order_relaxed);
  s_in_base = r;
  s_in_pos = 0.0;
  s_q = r - kHop;                // so the natural continuation starts at r
  memset(s_olap, 0, sizeof(s_olap));  // the first segment fades in
  s_st_r = s_st_w;
  s_pos = 0.0;
}

// One hop of output. False when the input doesn't reach far enough yet.
bool stretchHop(u32 w)
{
  const u32 target = s_in_base + (u32)s_in_pos;
  const u32 natural = s_q + kHop;
  // At full speed just continue from the previous segment: identical windows
  // overlap-add back to the original sound
  const s32 drift = (s32)(natural - target);
  const bool exact = s_tempo >= 1.0 && drift >= -kSeek && drift <= kSeek;
  const int seek = exact ? 0 : kSeek;
  // Everything the search and the chosen segment can touch must be in the ring
  if ((s32)(target + (exact ? drift : seek) + kWin - w) > 0 || (s32)(natural + kHop - w) > 0)
    return false;

  int best = exact ? drift : 0;
  if (!exact) {
    float best_score = -1e30f;
    for (int d = -seek; d <= seek; d += 2) {
      const u32 q = target + d;
      float c = 0.0f, e = 1.0f;
      for (u32 i = 0; i < kHop; i += 4) {
        const float a = inMono(q + i);
        c += a * inMono(natural + i);
        e += a * a;
      }
      const float score = c / sqrtf(e);
      if (score > best_score) { best_score = score; best = d; }
    }
  }
  const u32 q = target + best;

  for (u32 i = 0; i < kHop; i++) {
    const s16 * f = inFrame(q + i);
    const s16 * g = inFrame(q + kHop + i);
    float * o = &s_st[((s_st_w + i) & kStMask) * 2];
    o[0] = s_olap[i * 2] + f[0] * s_hann[i];
    o[1] = s_olap[i * 2 + 1] + f[1] * s_hann[i];
    s_olap[i * 2] = g[0] * s_hann[kHop + i];
    s_olap[i * 2 + 1] = g[1] * s_hann[kHop + i];
  }
  s_st_w += kHop;
  s_q = q;
  s_in_pos += kHop * s_tempo;
  if (s_in_pos >= 65536.0) {
    const u32 whole = (u32)s_in_pos;
    s_in_base += whole;
    s_in_pos -= whole;
  }

  // Hand the input before the search window back to the writer
  const u32 now_target = s_in_base + (u32)s_in_pos;
  u32 keep = now_target - kSeek - 2;
  if ((s32)(s_q + kHop - keep) < 0) keep = s_q + kHop;
  const u32 r = s_read.load(std::memory_order_relaxed);
  if ((s32)(keep - r) > 0) s_read.store(keep, std::memory_order_release);
  return true;
}

void produce(s16 * out)
{
  const u32 w = s_write.load(std::memory_order_acquire);
  const u32 written = w - s_last_write;
  s_last_write = w;

  // How fast the game is making sound, smoothed over about a second
  const double per_period = (double)kPeriod * kInRate / s_out_rate;
  s_prod += ((double)written / per_period - s_prod) * 0.005;

  const u32 avail_in = w - (s_in_base + (u32)s_in_pos);
  const u32 fill = (s32)avail_in > 0 ? avail_in : 0;
  s_fill_sum += fill;
  s_periods++;
  if (fill < s_fill_min) s_fill_min = fill;

  if (!s_playing && w - s_read.load(std::memory_order_relaxed) >= kRestart) {
    s_playing = true;
    stretchReset();
  }

  // Fine rate: slow down a hair when the input is low, speed up when it is high
  s_fill_smooth += ((double)fill - s_fill_smooth) * 0.02;
  double err = (s_fill_smooth - kTarget) / kTarget;
  if (err > 1.0) err = 1.0;
  if (err < -1.0) err = -1.0;
  const double ratio = 1.0 + kMaxDelta * err;
  // Coarse rate: stretch while the game runs clearly slower than full speed
  const double want = s_prod * (1.0 + 0.25 * err);
  if (s_tempo >= 1.0) {
    if (want < 0.95) s_tempo = want;
  } else {
    s_tempo = want > 0.985 ? 1.0 : want;
  }
  if (s_tempo < 0.5) s_tempo = 0.5;
  s_ratio_sum_ppm += (u64)(ratio * s_tempo * 1000000.0);
  const double step = (double)kInRate / s_out_rate * ratio;

  const bool muted = s_muted.load(std::memory_order_relaxed);
  const float vol = s_volume.load(std::memory_order_relaxed) / 256.0f;
  u32 i = 0;
  if (s_playing) {
    const u32 need = (u32)(s_pos + kPeriod * step) + 4;
    while (s_st_w - s_st_r < need && stretchHop(w)) {}
    const u32 avail = s_st_w - s_st_r;
    for (; i < kPeriod; i++) {
      const u32 ip = (u32)s_pos;
      if (ip + 2 >= avail) break;  // need frames ip-1 .. ip+2
      const float t = (float)(s_pos - ip);
      const float * f0 = &s_st[((s_st_r + ip - 1) & kStMask) * 2];
      const float * f1 = &s_st[((s_st_r + ip) & kStMask) * 2];
      const float * f2 = &s_st[((s_st_r + ip + 1) & kStMask) * 2];
      const float * f3 = &s_st[((s_st_r + ip + 2) & kStMask) * 2];
      const float l = cubic(f0[0], f1[0], f2[0], f3[0], t);
      const float rr = cubic(f0[1], f1[1], f2[1], f3[1], t);
      s_lastL = l; s_lastR = rr;
      out[i * 2] = muted ? 0 : clamp16((s32)(l * vol));
      out[i * 2 + 1] = muted ? 0 : clamp16((s32)(rr * vol));
      s_pos += step;
    }
    const u32 adv = (u32)s_pos;
    s_pos -= adv;
    s_st_r += adv;
    if (i < kPeriod) {
      // Ran dry even with stretching (a load or a long stall): fade out
      s_underruns++;
      s_playing = false;
    }
  }
  for (int k = 0; i < kPeriod; i++, k++) {
    const float g = k < kFadeFrames ? 1.0f - (float)k / kFadeFrames : 0.0f;
    out[i * 2] = muted ? 0 : clamp16((s32)(s_lastL * g * vol));
    out[i * 2 + 1] = muted ? 0 : clamp16((s32)(s_lastR * g * vol));
  }
  if (!s_playing) { s_lastL = 0; s_lastR = 0; }

  if (s_dump_out) {
    for (u32 k = 0; k < kPeriod; k++, s_out_total++) {
      if (s_out_total < (u64)s_dump_start * s_out_rate) continue;
      if (s_dump_out_pos >= s_dump_out_len) break;
      s_dump_out[s_dump_out_pos * 2] = out[k * 2];
      s_dump_out[s_dump_out_pos * 2 + 1] = out[k * 2 + 1];
      s_dump_out_pos++;
    }
  }
}

void outputThread(void *)
{
  printf("sndnx: output thread on core %d\n", (int)svcGetCurrentProcessorNumber());
  // Prime every buffer with silence, then refill each one as audout hands it back
  for (int b = 0; b < kBuffers; b++) {
    memset(s_bufmem[b], 0, kBufferBytes);
    s_bufs[b].next = nullptr;
    s_bufs[b].buffer = s_bufmem[b];
    s_bufs[b].buffer_size = kBufferBytes;
    s_bufs[b].data_size = kPeriod * 4;
    s_bufs[b].data_offset = 0;
    armDCacheFlush(s_bufmem[b], kBufferBytes);
    audoutAppendAudioOutBuffer(&s_bufs[b]);
  }
  while (s_running.load()) {
    AudioOutBuffer * released = nullptr;
    u32 count = 0;
    if (R_FAILED(audoutWaitPlayFinish(&released, &count, 50000000ULL)) || !released || count == 0)
      continue;
    produce((s16 *)released->buffer);
    armDCacheFlush(released->buffer, kBufferBytes);
    audoutAppendAudioOutBuffer(released);
    if (s_dump_out && s_dump_out_pos >= s_dump_out_len && s_dump_in_pos >= s_dump_in_len &&
        !s_dump_written.load()) {
      // Takes a moment; one dropout while writing is expected in a debug capture
      writeDumps();
    }
  }
}

int Init(void)
{
  s_ring = (s16 *)calloc(kRingFrames, 4);
  if (!s_ring) return -1;
  s_write = 0; s_read = 0;
  s_pos = 0.0; s_fill_smooth = kTarget; s_playing = false; s_fade = 0;
  s_st = (float *)calloc(kStFrames, 2 * sizeof(float));
  if (!s_st) return -1;
  s_st_w = s_st_r = 0;
  s_tempo = 1.0; s_prod = 1.0; s_last_write = 0;
  for (u32 n = 0; n < kWin; n++)
    s_hann[n] = 0.5f - 0.5f * cosf(2.0f * 3.14159265358979f * n / kWin);
  stretchReset();
  s_lastL = s_lastR = 0;
  s_in_total = s_out_total = 0;

  if (R_FAILED(audoutInitialize())) {
    printf("sndnx: audoutInitialize failed\n");
    return -1;
  }
  s_audout = true;
  if (R_FAILED(audoutStartAudioOut())) {
    printf("sndnx: audoutStartAudioOut failed\n");
    return -1;
  }
  s_out_rate = audoutGetSampleRate();
  if (s_out_rate == 0) s_out_rate = 48000;
  for (int b = 0; b < kBuffers; b++) {
    s_bufmem[b] = aligned_alloc(0x1000, kBufferBytes);
    if (!s_bufmem[b]) return -1;
  }

  if (s_dump_secs > 0) {
    s_dump_in_len = kInRate * s_dump_secs;
    s_dump_out_len = s_out_rate * s_dump_secs;
    s_dump_in = (s16 *)calloc(s_dump_in_len, 4);
    s_dump_out = (s16 *)calloc(s_dump_out_len, 4);
    s_dump_in_pos = s_dump_out_pos = 0;
    s_dump_written = false;
  }

  s_running = true;
  // Above the emulator's threads (0x2C) so a period is never late. Core 1, with
  // the renderer: core 0 runs the SH-2s and core 2 the sound CPU, which must not
  // be interrupted (it falls behind and the SH-2s wait for it at VBlank-IN).
  if (R_FAILED(threadCreate(&s_thread, outputThread, nullptr, nullptr, 0x8000, 0x24, 1))) {
    printf("sndnx: threadCreate failed\n");
    s_running = false;
    return -1;
  }
  s_thread_made = true;
  threadStart(&s_thread);
  printf("sndnx: audout %u Hz, ring %u, target %u frames, period %u x %d\n",
         (unsigned)s_out_rate, (unsigned)kRingFrames, (unsigned)kTarget, (unsigned)kPeriod, kBuffers);
  return 0;
}

void DeInit(void)
{
  s_running = false;
  if (s_thread_made) {
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
    s_thread_made = false;
  }
  if (s_audout) {
    audoutStopAudioOut();
    audoutExit();
    s_audout = false;
  }
  for (int b = 0; b < kBuffers; b++) {
    free(s_bufmem[b]);
    s_bufmem[b] = nullptr;
  }
  writeDumps();
  free(s_dump_in); s_dump_in = nullptr;
  free(s_dump_out); s_dump_out = nullptr;
  free(s_ring);
  s_ring = nullptr;
  free(s_st);
  s_st = nullptr;
}

int Reset(void) { return 0; }
int ChangeVideoFormat(int) { return 0; }

void UpdateAudio(u32 * left, u32 * right, u32 num)
{
  if (!s_ring) return;
  const s32 * l = (const s32 *)left;
  const s32 * r = (const s32 *)right;

  if (s_dump_in) {
    for (u32 k = 0; k < num; k++, s_in_total++) {
      if (s_in_total < (u64)s_dump_start * kInRate) continue;
      if (s_dump_in_pos >= s_dump_in_len) break;
      s_dump_in[s_dump_in_pos * 2] = clamp16(l[k]);
      s_dump_in[s_dump_in_pos * 2 + 1] = clamp16(r[k]);
      s_dump_in_pos++;
    }
  }

  const u32 w = s_write.load(std::memory_order_relaxed);
  // Keep a few frames behind the reader intact: the resampler looks one back
  const u32 space = kRingFrames - 4 - (w - s_read.load(std::memory_order_acquire));
  if (num > space) {
    s_dropped += num - space;
    num = space;
  }
  for (u32 k = 0; k < num; k++) {
    s16 * f = &s_ring[((w + k) & kRingMask) * 2];
    f[0] = clamp16(l[k]);
    f[1] = clamp16(r[k]);
  }
  s_write.store(w + num, std::memory_order_release);
}

u32 GetAudioSpace(void)
{
  if (!s_ring) return 0;
  // Hand the SCSP room only up to 1.5x the target (plus what the stretcher
  // keeps behind its read position), so a burst of fast frames stays in its
  // own buffer instead of adding latency here
  const u32 fill = s_write.load() - s_read.load();
  const u32 limit = kTarget + kTarget / 2 + kSeek;
  return fill >= limit ? 0 : limit - fill;
}

void MuteAudio(void) { s_muted = true; }
void UnMuteAudio(void) { s_muted = false; }
void SetVolume(int volume)
{
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  s_volume = volume * 256 / 100;
}

#ifdef USE_SCSPMIDI
int MidiChangePorts(int, int) { return 0; }
u8 MidiIn(int * isdata) { *isdata = 0; return 0; }
int MidiOut(u8) { return 1; }
#endif

} // namespace

extern "C" {

SoundInterface_struct SNDNX = {
  SNDCORE_NX,
  "Switch audout",
  Init,
  DeInit,
  Reset,
  ChangeVideoFormat,
  UpdateAudio,
  GetAudioSpace,
  MuteAudio,
  UnMuteAudio,
  SetVolume,
#ifdef USE_SCSPMIDI
  MidiChangePorts,
  MidiIn,
  MidiOut
#endif
};

u32 SndNxBufferedSamples(void)
{
  if (!s_ring) return 0xFFFFFFFF;
  return s_write.load() - s_read.load();
}

void SndNxTakeStats(SndNxStats * st)
{
  const u32 periods = s_periods.exchange(0);
  st->underruns = s_underruns.exchange(0);
  st->dropped = s_dropped.exchange(0);
  const u32 mn = s_fill_min.exchange(0xFFFFFFFF);
  st->fill_min = mn == 0xFFFFFFFF ? 0 : mn;
  const u64 sum = s_fill_sum.exchange(0);
  const u64 rsum = s_ratio_sum_ppm.exchange(0);
  st->fill_avg = periods ? (u32)(sum / periods) : 0;
  st->ratio = periods ? (float)(rsum / (double)periods / 1000000.0) : 1.0f;
}

void SndNxSetDump(int start, int seconds)
{
  s_dump_start = start < 0 ? 0 : start;
  s_dump_secs = seconds < 0 ? 0 : seconds;
}

}
