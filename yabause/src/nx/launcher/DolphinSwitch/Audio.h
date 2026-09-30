// Launcher sound effects output. Dolphin's frontend shares its emulator audio stream for this;
// the YabaSanshiro launcher uses the audout service instead (see FrontendAudio.cpp) and closes
// it before a game starts.
#pragma once

#include <cstddef>

#include "Common/CommonTypes.h"

namespace DolphinSwitch::Audio
{
bool InitializeFrontendAudio();
void SetFrontendAudioEnabled(bool enabled);
// Interleaved stereo, 48 kHz
void QueueFrontendAudio(const s16* samples, std::size_t sample_count);
void ReleaseFrontendAudio();
}  // namespace DolphinSwitch::Audio
