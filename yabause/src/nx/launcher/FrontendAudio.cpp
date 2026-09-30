// SPDX-License-Identifier: GPL-2.0-or-later
//
// Launcher sound effects through the audout service.
//
// This used an SDL audio device at first, but closing it before a game starts could hang
// forever: SDL waits for its audio thread, which sometimes never returns from the audio
// renderer (the service the emulator's own SDL audio uses too). audout has no thread of our
// own, closing it can't block, and it is separate from the renderer.

#include "DolphinSwitch/Audio.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include <switch.h>

namespace DolphinSwitch::Audio
{
namespace
{
// audout plays 48 kHz, 2 channel, 16-bit PCM. Each buffer holds one sound effect (the longest
// is 48 ms, about 9 KB); a few let quick navigation overlap.
constexpr std::size_t BUFFER_SIZE = 0x4000;
constexpr int BUFFER_COUNT = 4;

struct Slot
{
  AudioOutBuffer buffer{};
  void* memory = nullptr;
  bool queued = false;
};

std::array<Slot, BUFFER_COUNT> s_slots;
bool s_ready = false;
bool s_enabled = true;

// Marks buffers audout has finished with as free again
void ReclaimBuffers()
{
  AudioOutBuffer* released = nullptr;
  u32 count = 0;
  while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&released, &count)) && count > 0 && released)
  {
    for (Slot& slot : s_slots)
    {
      if (&slot.buffer == released)
        slot.queued = false;
    }
  }
}
}  // namespace

bool InitializeFrontendAudio()
{
  if (s_ready)
    return true;
  if (R_FAILED(audoutInitialize()))
    return false;
  if (R_FAILED(audoutStartAudioOut()))
  {
    audoutExit();
    return false;
  }
  for (Slot& slot : s_slots)
  {
    // audout buffers must be page aligned
    slot.memory = std::aligned_alloc(0x1000, BUFFER_SIZE);
    slot.queued = false;
  }
  s_ready = true;
  return true;
}

void SetFrontendAudioEnabled(bool enabled)
{
  s_enabled = enabled;
}

void QueueFrontendAudio(const s16* samples, std::size_t sample_count)
{
  if (!s_ready || !s_enabled || !samples || sample_count == 0)
    return;

  ReclaimBuffers();
  const auto free_slot =
      std::find_if(s_slots.begin(), s_slots.end(),
                   [](const Slot& slot) { return !slot.queued && slot.memory; });
  // Everything still playing: skip this click rather than wait
  if (free_slot == s_slots.end())
    return;

  const std::size_t bytes = std::min(sample_count * sizeof(s16), BUFFER_SIZE);
  std::memcpy(free_slot->memory, samples, bytes);
  armDCacheFlush(free_slot->memory, bytes);

  free_slot->buffer = {};
  free_slot->buffer.buffer = free_slot->memory;
  free_slot->buffer.buffer_size = BUFFER_SIZE;
  free_slot->buffer.data_size = bytes;
  free_slot->buffer.data_offset = 0;
  if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&free_slot->buffer)))
    free_slot->queued = true;
}

void ReleaseFrontendAudio()
{
  if (!s_ready)
    return;
  audoutStopAudioOut();
  audoutExit();
  for (Slot& slot : s_slots)
  {
    std::free(slot.memory);
    slot.memory = nullptr;
    slot.queued = false;
  }
  s_ready = false;
}
}  // namespace DolphinSwitch::Audio
