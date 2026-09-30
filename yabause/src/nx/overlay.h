/*
This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Dear ImGui on top of the emulator's picture: the quick menu while the game is paused
// (drawn by the main thread) and the FPS counter and status messages during play (drawn by
// the render thread, just before it presents a frame). Only one thread holds the GL context
// at a time, so ImGui is never used from two threads at once.

#pragma once

namespace nx {
namespace overlay {

// Once per app: ImGui context, style and the Switch system font
bool init();
void shutdown();

// Once per GL context (each game): the ImGui OpenGL backend. Needs the context current.
bool attachContext();
void detachContext();

// Copy the next presented frame, so the paused menu can show it behind itself
void requestCapture();
bool captureDone();

// Render thread, from YuiSwapBuffers, before presenting: finishes a requested capture and
// draws the FPS counter / status message over the frame
void onPresent(int width, int height);

// Main thread, holding the GL context while the game is paused: the captured frame and the
// quick menu. The caller presents it.
void drawPaused(int width, int height);

// Emulated frames per second, for the FPS counter
void setFps(float fps);

} // namespace overlay
} // namespace nx
