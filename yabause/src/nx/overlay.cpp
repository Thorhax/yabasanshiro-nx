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

#include "overlay.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>

#include <switch.h>
#include <glad/glad.h>

#include "imgui.h"
#include "backends/imgui_impl_opengl3.h"
#include "DolphinSwitch/RuntimeOverlay.h"

namespace nx {
namespace overlay {

static bool s_initialized = false;
static bool s_pl_ready = false;
static bool s_attached = false;
static u64 s_last_tick = 0;

static std::atomic<bool> s_capture_requested{false};
static std::atomic<bool> s_capture_done{false};
static GLuint s_capture_texture = 0;
static int s_capture_width = 0;
static int s_capture_height = 0;

static std::atomic<float> s_fps{0.0f};

bool init()
{
  if (s_initialized) return true;

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO & io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui::StyleColorsDark();

  // The system font, as the launcher uses; ImGui 1.92 rasterises sizes on demand
  if (R_SUCCEEDED(plInitialize(PlServiceType_User))) {
    s_pl_ready = true;
    PlFontData font;
    if (R_SUCCEEDED(plGetSharedFontByType(&font, PlSharedFontType_Standard)) && font.address) {
      ImFontConfig config;
      config.FontDataOwnedByAtlas = false;  // shared memory owned by the pl service
      io.Fonts->AddFontFromMemoryTTF(font.address, (int)font.size, 25.0f, &config);
    }
  }
  if (io.Fonts->Fonts.empty())
    io.Fonts->AddFontDefault();

  s_initialized = true;
  return true;
}

void shutdown()
{
  if (!s_initialized) return;
  ImGui::DestroyContext();
  if (s_pl_ready) plExit();
  s_pl_ready = false;
  s_initialized = false;
}

bool attachContext()
{
  if (!s_initialized || s_attached) return s_attached;
  s_attached = ImGui_ImplOpenGL3_Init("#version 330 core");
  if (!s_attached) printf("ImGui OpenGL backend failed to start\n");
  s_last_tick = armGetSystemTick();
  return s_attached;
}

void detachContext()
{
  if (!s_attached) return;
  ImGui_ImplOpenGL3_Shutdown();
  if (s_capture_texture) {
    glDeleteTextures(1, &s_capture_texture);
    s_capture_texture = 0;
  }
  s_capture_requested = false;
  s_capture_done = false;
  s_attached = false;
}

void requestCapture()
{
  s_capture_done = false;
  s_capture_requested = true;
}

bool captureDone()
{
  return s_capture_done;
}

void setFps(float fps)
{
  s_fps = fps;
}

static void beginFrame(int width, int height)
{
  ImGuiIO & io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)width, (float)height);
  const u64 now = armGetSystemTick();
  io.DeltaTime = s_last_tick ? std::max(1e-4f, armTicksToNs(now - s_last_tick) / 1e9f) : 1.0f / 60.0f;
  s_last_tick = now;
  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
}

static void endFrame()
{
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

// The frame about to be presented, copied from the default framebuffer
static void captureFrame(int width, int height)
{
  GLint read_framebuffer = 0, texture = 0;
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);

  glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
  if (!s_capture_texture) glGenTextures(1, &s_capture_texture);
  glBindTexture(GL_TEXTURE_2D, s_capture_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 0, 0, width, height, 0);
  s_capture_width = width;
  s_capture_height = height;

  glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)read_framebuffer);
}

void onPresent(int width, int height)
{
  if (!s_attached) return;

  if (s_capture_requested.exchange(false)) {
    captureFrame(width, height);
    s_capture_done = true;
  }

  const bool show_fps = DolphinSwitch::RuntimeOverlay::ShowFPS();
  const std::string status = DolphinSwitch::RuntimeOverlay::CurrentStatus();
  if (!show_fps && status.empty()) {
    // Keep the frame clock current so the next ImGui frame gets a sane delta
    s_last_tick = armGetSystemTick();
    return;
  }

  GLint framebuffer = 0;
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

  beginFrame(width, height);
  const float scale = std::max(0.72f, height / 1080.0f);
  ImDrawList * draw = ImGui::GetForegroundDrawList();
  ImFont * font = ImGui::GetFont();
  if (show_fps) {
    char text[32];
    snprintf(text, sizeof(text), "%.1f FPS", s_fps.load());
    const float size = 26.0f * scale;
    const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0.0f, text);
    const ImVec2 origin(16.0f * scale, 12.0f * scale);
    draw->AddRectFilled(ImVec2(origin.x - 8 * scale, origin.y - 4 * scale),
                        ImVec2(origin.x + extent.x + 8 * scale, origin.y + extent.y + 4 * scale),
                        IM_COL32(0, 0, 0, 150), 6.0f * scale);
    draw->AddText(font, size, origin, IM_COL32(140, 235, 255, 255), text);
  }
  if (!status.empty()) {
    const float size = 26.0f * scale;
    const ImVec2 extent = font->CalcTextSizeA(size, 1e9f, 0.0f, status.c_str());
    const ImVec2 origin(16.0f * scale, height - extent.y - 16.0f * scale);
    draw->AddRectFilled(ImVec2(origin.x - 8 * scale, origin.y - 4 * scale),
                        ImVec2(origin.x + extent.x + 8 * scale, origin.y + extent.y + 4 * scale),
                        IM_COL32(7, 28, 44, 220), 6.0f * scale);
    draw->AddText(font, size, origin, IM_COL32(140, 235, 255, 255), status.c_str());
  }
  endFrame();

  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)framebuffer);
}

void drawPaused(int width, int height)
{
  if (!s_attached) return;

  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glViewport(0, 0, width, height);
  glDisable(GL_SCISSOR_TEST);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  beginFrame(width, height);
  if (s_capture_texture) {
    // GL textures start at the bottom row, so flip vertically
    ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)s_capture_texture,
                                             ImVec2(0, 0), ImVec2((float)width, (float)height),
                                             ImVec2(0, 1), ImVec2(1, 0));
  }
  DolphinSwitch::RuntimeOverlay::Draw();
  endFrame();
}

} // namespace overlay
} // namespace nx
