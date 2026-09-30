/*
        Copyright 2019 devMiyax(smiyaxdev@gmail.com)

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

#include <algorithm>
#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <switch.h>
#include <SDL2/SDL.h>
#include <zlib.h>

extern "C" {
#include "../config.h"
#include "yabause.h"
#include "vdp2.h"
#include "scsp.h"
#include "vidogl.h"
#include "peripheral.h"
#include "m68kcore.h"
#include "sh2core.h"
#include "sh2int.h"
#include "cdbase.h"
#include "cs0.h"
#include "cs2.h"
#include "debug.h"
#include "sndsdl.h"
#include "osdcore.h"
#include "ygl.h"
#include "yui.h"
#include "threads.h"
#include "memory.h"
}

#include <EGL/eglext.h>

#include "cheats.h"
#include "config.h"
#include "input.h"
#include "DolphinSwitch/Forwarder.h"
#include "DolphinSwitch/Launcher.h"
#include "DolphinSwitch/RuntimeOverlay.h"
#include "overlay.h"
#include "UICommon/GameFile.h"
#include "sh2_dynarec_devmiyax/dynarec_jit_nx.h"

// Must match sh2_dynarec_devmiyax/DynarecSh2CInterface.cpp
#define SH2CORE_DYNAMIC 3

static EGLDisplay s_display;
static EGLContext s_context;
static EGLSurface s_surface;
static EGLConfig s_config;
static int s_surface_width = 1280;
static int s_surface_height = 720;

static char biospath[512];
static char cdpath[512];
static char buppath[512];
static char mpegpath[512] = "";
static char cartpath[512] = "";
static std::string shader_cache_path;

static nx::Settings s_settings;
static nx::Input s_input;

//////////////////////////////////////////////////////////////////////////////
// Core lists and UI callbacks the emulator core expects from a port

extern "C" {

M68K_struct * M68KCoreList[] = {
  &M68KDummy,
#ifdef HAVE_MUSASHI
  &M68KMusashi,
#endif
  NULL
};

SH2Interface_struct *SH2CoreList[] = {
  &SH2Interpreter,
  &SH2DebugInterpreter,
#if DYNAREC_DEVMIYAX
  &SH2Dyn,
  &SH2DynDebug,
#endif
  NULL
};

PerInterface_struct *PERCoreList[] = {
  &PERDummy,
  NULL
};

CDInterface *CDCoreList[] = {
  &DummyCD,
  &ISOCD,
  NULL
};

SoundInterface_struct *SNDCoreList[] = {
  &SNDDummy,
#ifdef HAVE_LIBSDL
  &SNDSDL,
#endif
  NULL
};

VideoInterface_struct *VIDCoreList[] = {
  &VIDDummy,
  &VIDOGL,
  NULL
};

OSD_struct *OSDCoreList[] = {
  &OSDDummy,
  NULL
};

PFNGLTEXTUREBARRIERNVPROC nx_glTextureBarrierNV = NULL;

void * getGlobalNanoVGContext() { return NULL; }
void DrawDebugInfo() {}

void YuiErrorMsg(const char *string)
{
  printf("YuiErrorMsg: %s\n", string);
}

void YuiSwapBuffers(void)
{
  // Render thread, with the finished frame in the back buffer
  nx::overlay::onPresent(s_surface_width, s_surface_height);
  eglSwapBuffers(s_display, s_surface);
}

int YuiRevokeOGLOnThisThread()
{
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  return 0;
}

int YuiUseOGLOnThisThread()
{
  if (!eglMakeCurrent(s_display, s_surface, s_surface, s_context)) {
    printf("YuiUseOGLOnThisThread: eglMakeCurrent failed: 0x%x\n", eglGetError());
    return -1;
  }
  return 0;
}

const char * YuiGetShaderCachePath()
{
  return shader_cache_path.c_str();
}

// Hooks for PlayRecorder (input recording/playback)
static bool s_use_bios = false;
static int yabauseinit(bool use_bios);

// Like the other ports, this reports whether the *emulated* BIOS is in use
int YabauseThread_IsUseBios() { return s_use_bios ? 0 : 1; }
const char * YabauseThread_getBackupPath() { return buppath; }
void YabauseThread_setUseBios(int use) {}
void YabauseThread_setBackupPath(const char * buf) { snprintf(buppath, sizeof(buppath), "%s", buf); }
void YabauseThread_resetPlaymode() {}

void YabauseThread_coldBoot()
{
  YabauseDeInit();
  yabauseinit(s_use_bios);
  YabauseReset();
}

} // extern "C"

//////////////////////////////////////////////////////////////////////////////
// Logging: nxlink when a host is listening, otherwise a file on the SD card

static int s_nxlink_sock = -1;
static bool s_sockets = false;
static FILE * s_logfile = NULL;

// Sockets stay up for the whole run: the launcher uses them for SMB shares and SteamGridDB
extern "C" void userAppInit()
{
  s_sockets = R_SUCCEEDED(socketInitializeDefault());
  if (s_sockets)
    s_nxlink_sock = nxlinkStdio();
}

extern "C" void userAppExit()
{
  if (s_nxlink_sock >= 0) {
    close(s_nxlink_sock);
    s_nxlink_sock = -1;
  }
  if (s_sockets) {
    socketExit();
    s_sockets = false;
  }
}

static void initLog()
{
  if (s_nxlink_sock >= 0) return;
  // Leaving a game relaunches the app, so keep the previous run's log too
  const std::string log = nx::dataPath("log.txt");
  const std::string previous = nx::dataPath("log.prev.txt");
  remove(previous.c_str());
  rename(log.c_str(), previous.c_str());
  s_logfile = freopen(log.c_str(), "w", stdout);
  if (s_logfile) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    dup2(fileno(stdout), fileno(stderr));
  }
}

//////////////////////////////////////////////////////////////////////////////
// EGL: desktop OpenGL 4.3 core on Mesa/nouveau

static bool initEgl(NWindow* win)
{
  s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  if (!s_display) {
    printf("eglGetDisplay failed: 0x%x\n", eglGetError());
    return false;
  }

  eglInitialize(s_display, NULL, NULL);

  if (!eglBindAPI(EGL_OPENGL_API)) {
    printf("eglBindAPI(EGL_OPENGL_API) failed: 0x%x\n", eglGetError());
    goto fail_display;
  }

  {
    EGLConfig config;
    EGLint numConfigs = 0;
    static const EGLint framebufferAttributeList[] = {
      EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
      EGL_RED_SIZE,     8,
      EGL_GREEN_SIZE,   8,
      EGL_BLUE_SIZE,    8,
      EGL_ALPHA_SIZE,   8,
      EGL_DEPTH_SIZE,   24,
      EGL_STENCIL_SIZE, 8,
      EGL_NONE
    };
    eglChooseConfig(s_display, framebufferAttributeList, &config, 1, &numConfigs);
    if (numConfigs == 0) {
      printf("eglChooseConfig: no config found: 0x%x\n", eglGetError());
      goto fail_display;
    }

    s_config = config;
    s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
    if (!s_surface) {
      printf("eglCreateWindowSurface failed: 0x%x\n", eglGetError());
      goto fail_display;
    }

    static const EGLint contextAttributeList[] = {
      EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
      EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
      EGL_CONTEXT_MINOR_VERSION_KHR, 3,
      EGL_NONE
    };
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, contextAttributeList);
    if (!s_context) {
      printf("eglCreateContext failed: 0x%x\n", eglGetError());
      goto fail_surface;
    }
  }

  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, s_settings.vsync ? 1 : 0);
  return true;

fail_surface:
  eglDestroySurface(s_display, s_surface);
  s_surface = NULL;
fail_display:
  eglTerminate(s_display);
  s_display = NULL;
  return false;
}

// GL errors go to the log; capped per game so a per-frame error can't flood it
static int s_gl_log_count = 0;

static void GLAPIENTRY glDebugLog(GLenum source, GLenum type, GLuint id, GLenum severity,
                                  GLsizei length, const GLchar * message, const void * user)
{
  // Performance hints (e.g. updating a GL_STATIC_DRAW buffer) are just noise here
  if (severity == GL_DEBUG_SEVERITY_NOTIFICATION || type == GL_DEBUG_TYPE_PERFORMANCE) return;
  if (s_gl_log_count >= 100) return;
  if (++s_gl_log_count == 100) printf("GL: further messages suppressed\n");
  printf("GL: type 0x%x id %u: %s\n", type, id, message);
}

static void deinitEgl()
{
  if (!s_display) return;

  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  if (s_context) {
    eglDestroyContext(s_display, s_context);
    s_context = NULL;
  }
  if (s_surface) {
    eglDestroySurface(s_display, s_surface);
    s_surface = NULL;
  }
  eglTerminate(s_display);
  s_display = NULL;
}

//////////////////////////////////////////////////////////////////////////////

static bool fileExists(const std::string & path)
{
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

static int yabauseinit(bool use_bios)
{
  yabauseinit_struct yinit = {};

  yinit.m68kcoretype = M68KCORE_MUSASHI;
  yinit.percoretype = PERCORE_DUMMY;
  yinit.sh2coretype = s_settings.dynarec ? SH2CORE_DYNAMIC : SH2CORE_INTERPRETER;
  yinit.vidcoretype = VIDCORE_OGL;
#ifdef HAVE_LIBSDL
  yinit.sndcoretype = SNDCORE_SDL;
#else
  yinit.sndcoretype = SNDCORE_DUMMY;
#endif
  yinit.cdcoretype = CDCORE_ISO;
  yinit.carttype = s_settings.cart;
  yinit.regionid = 0;
  yinit.biospath = use_bios ? biospath : NULL;
  yinit.cdpath = cdpath;
  yinit.buppath = buppath;
  yinit.mpegpath = mpegpath;
  yinit.cartpath = cartpath;
  yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
  yinit.frameskip = s_settings.frameskip;
  yinit.framelimit = s_settings.framelimit;
  yinit.usethreads = 0;
  yinit.skip_load = 0;
  yinit.video_filter_type = 0;
  yinit.polygon_generation_mode = s_settings.polygon_mode;
  yinit.use_new_scsp = 1;
  yinit.resolution_mode = s_settings.resolution_mode;
  yinit.rbg_resolution_mode = s_settings.rbg_resolution_mode;
  yinit.rbg_use_compute_shader = s_settings.rbg_compute_shader;
  yinit.rotate_screen = 0;
  yinit.scsp_sync_count_per_frame = s_settings.scsp_sync_per_frame;
  yinit.extend_backup = 1;
  yinit.scsp_main_mode = s_settings.scsp_main_mode;
  yinit.use_cpu_affinity = 1;
  yinit.use_sh2_cache = s_settings.sh2_cache;

  Vdp2SyncVBlankOut = s_settings.sync_render;
  if (YabauseInit(&yinit) == -1)
    return -1;

  OSDInit(0);
  OSDChangeCore(OSDCORE_DUMMY);
  return 0;
}

extern "C" u64 getM68KCounter();
extern "C" u32 YabThreadGetNxHandle(unsigned int id);
extern "C" char _start[];   // entry point, at offset 0 of our code (libnx crt0)

static std::atomic<u32> s_frames_done{0};
static std::atomic<bool> s_watchdog_running{true};
static Handle s_main_thread;

// What the app is doing, for the hang watchdog
enum class Phase { Launcher, Game, GameTeardown };
static std::atomic<Phase> s_phase{Phase::Launcher};
// Bumped by every launcher frame, launcher step and emulated frame
static std::atomic<u32> s_progress{0};

void NxLauncherHeartbeat()
{
  s_progress++;
}

void NxLauncherStep(const char * step)
{
  printf("Launcher: %s\n", step);
  s_progress++;
}

static bool readableAddress(u64 address)
{
  MemoryInfo info;
  u32 page;
  if (R_FAILED(svcQueryMemory(&info, &page, address)))
    return false;
  return info.type != MemType_Unmapped && (info.perm & Perm_R) &&
         address + 16 <= info.addr + info.size;
}

static void printAddress(const char * label, u64 address)
{
  const u64 base = (u64)_start;
  if (address >= base && address < base + 0x4000000)
    printf("%self+0x%llx", label, (unsigned long long)(address - base));
  else
    printf("%s0x%llx", label, (unsigned long long)address);
}

// Pauses a thread just long enough to read where it is, including the call
// stack from its frame-pointer chain. Addresses inside our code are logged
// relative to _start so they can be looked up in yabasanshiro.elf
// (aarch64-none-elf-addr2line -f -e yabasanshiro.elf 0x...).
static void sampleThread(const char * name, Handle h)
{
  ThreadContext ctx;
  Result rc;

  if (h == INVALID_HANDLE) return;
  rc = svcSetThreadActivity(h, ThreadActivity_Paused);
  if (R_FAILED(rc)) {
    printf("  %-5s pause failed 0x%x\n", name, rc);
    return;
  }
  rc = svcGetThreadContext3(&ctx, h);
  if (R_FAILED(rc)) {
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    printf("  %-5s context failed 0x%x\n", name, rc);
    return;
  }

  printf("  %-5s", name);
  printAddress(" pc ", ctx.pc.x);
  printAddress("  lr ", ctx.lr);
  printf("\n        stack:");
  // AArch64 frame records: [fp] = caller's fp, [fp + 8] = return address
  u64 fp = ctx.fp;
  for (int depth = 0; depth < 16 && fp && (fp & 7) == 0 && readableAddress(fp); depth++) {
    const u64 * frame = (const u64 *)fp;
    printAddress(" ", frame[1]);
    if (frame[0] <= fp) break;
    fp = frame[0];
  }
  printf("\n");
  svcSetThreadActivity(h, ThreadActivity_Runnable);
}

static void sampleAllThreads()
{
  // A few samples show whether a thread is parked or looping
  for (int i = 0; i < 3; i++) {
    printf(" sample %d:\n", i);
    sampleThread("main", s_main_thread);
    sampleThread("vdp", YabThreadGetNxHandle(YAB_THREAD_VDP));
    sampleThread("scsp", YabThreadGetNxHandle(YAB_THREAD_SCSP));
    svcSleepThread(50000000LL);
  }
}

// Logs where the app is when it stops making progress, for diagnosing hangs:
// launcher frames or steps stopping for 5s, or emulated frames for 3s. If game
// teardown hangs it also ends the process, so the user isn't left stuck (save
// RAM is flushed before teardown starts).
static void watchdogMain(void *)
{
  u32 last = 0;
  Phase last_phase = Phase::Launcher;
  int stalled_secs = 0;
  bool reported = false;
  while (s_watchdog_running) {
    svcSleepThread(1000000000LL);

    const Phase phase = s_phase;
    const u32 now = s_progress;
    if (now != last || phase != last_phase) {
      last = now;
      last_phase = phase;
      stalled_secs = 0;
      reported = false;
      continue;
    }
    stalled_secs++;

    if (phase == Phase::GameTeardown) {
      if (stalled_secs < 3) continue;
      printf("watchdog: game teardown stuck for 3s\n");
      sampleAllThreads();
      printf("watchdog: forcing exit\n");
      fflush(stdout);
      svcExitProcess();
    }

    if (reported) continue;
    if (phase == Phase::Game && stalled_secs >= 3) {
      reported = true;
      printf("watchdog: no frame for 3s at frame %u, line %d/%d (vblank %d), m68k counter %llu\n",
        (u32)s_frames_done, yabsys.LineCount, yabsys.MaxLineCount, yabsys.VBlankLineCount,
        (unsigned long long)(getM68KCounter() >> SCSP_FRACTIONAL_BITS));
      if (SH2Core && MSH2 && SSH2)
        printf("  SH2 master pc %08X, slave pc %08X (slave running %d)\n",
          SH2Core->GetPC(MSH2), SH2Core->GetPC(SSH2), yabsys.IsSSH2Running);
      sampleAllThreads();
    } else if (phase == Phase::Launcher && stalled_secs >= 5) {
      reported = true;
      printf("watchdog: launcher made no progress for 5s\n");
      for (int i = 0; i < 3; i++) {
        sampleThread("main", s_main_thread);
        svcSleepThread(50000000LL);
      }
    }
  }
}

// Resize touches GL, and with async rendering the VDP thread owns the
// context, so borrow it for the duration.
static void resizeVideo(int width, int height)
{
  VdpRevoke();
  YuiUseOGLOnThisThread();
  VIDCore->Resize(0, 0, width, height, 1, s_settings.aspect_mode);
  YuiRevokeOGLOnThisThread();
  VdpResume();
}

// The display size for the current mode: 1080p docked, 720p handheld
static void displaySize(bool docked, u32 * width, u32 * height)
{
  *width = docked ? 1920 : 1280;
  *height = docked ? 1080 : 720;
}

// Docking or undocking mid-game: the window's buffers can't change size while EGL holds
// them, so the surface is rebuilt at the new size (the GL context, and with it everything
// the renderer has uploaded, stays). False when there's nothing left to draw to.
static bool switchDisplayMode(bool docked)
{
  u32 width, height;
  displaySize(docked, &width, &height);
  printf("Display mode: %s, %ux%u\n", docked ? "docked" : "handheld", width, height);

  VdpRevoke();
  YuiUseOGLOnThisThread();
  glFinish();
  eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_context);
  eglDestroySurface(s_display, s_surface);
  s_surface = NULL;

  NWindow * window = nwindowGetDefault();
  nwindowReleaseBuffers(window);
  if (R_FAILED(nwindowSetDimensions(window, width, height)) ||
      R_FAILED(nwindowSetCrop(window, 0, 0, width, height)))
    printf("Could not resize the display window\n");
  s_surface = eglCreateWindowSurface(s_display, s_config, window, NULL);
  if (!s_surface) {
    // Hand the render thread back its (surfaceless) context; the caller ends the game
    printf("eglCreateWindowSurface failed: 0x%x\n", eglGetError());
    YuiRevokeOGLOnThisThread();
    VdpResume();
    return false;
  }
  eglMakeCurrent(s_display, s_surface, s_surface, s_context);
  eglSwapInterval(s_display, s_settings.vsync ? 1 : 0);

  EGLint surface_width = 0, surface_height = 0;
  if (!eglQuerySurface(s_display, s_surface, EGL_WIDTH, &surface_width) ||
      !eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &surface_height) ||
      surface_width <= 0 || surface_height <= 0) {
    surface_width = width;
    surface_height = height;
  }
  printf("Display: %dx%d\n", surface_width, surface_height);
  s_surface_width = surface_width;
  s_surface_height = surface_height;
  VIDCore->Resize(0, 0, surface_width, surface_height, 1, s_settings.aspect_mode);

  YuiRevokeOGLOnThisThread();
  VdpResume();
  return true;
}

enum class BiosRegion { Unknown, Japan, Overseas };

struct BiosFile {
  std::string path;
  BiosRegion region;
};

// Known dumps by CRC32, else a hint in the file name
static BiosRegion biosRegion(const std::string & path)
{
  FILE * fp = fopen(path.c_str(), "rb");
  if (fp) {
    std::vector<unsigned char> data(512 * 1024);
    const size_t size = fread(data.data(), 1, data.size(), fp);
    fclose(fp);
    const uLong crc = crc32(crc32(0L, Z_NULL, 0), data.data(), (uInt)size);
    // The dumps MAME knows (src/mame/sega/sat_console.cpp)
    switch (crc) {
    case 0x224b752c: // Japan v1.01, sega_101.bin
    case 0xb3c63c25: // Japan v1.003, sega1003.bin
    case 0x2aba43c2: // Japan v1.00, sega_100.bin
    case 0xe4d61811: // JVC V-Saturn, vsaturn.bin
    case 0x3408dbf4: // Hitachi HiSaturn v1.02, mpr-18100.bin
    case 0x721e1b60: // Hitachi HiSaturn v1.01, hisaturn.bin
      return BiosRegion::Japan;
    case 0x4afcf0fa: // Overseas (US / Europe) v1.01a, mpr-17933.bin
    case 0xf90f0089: // Overseas (US / Europe) v1.00a, sega_100a.bin
      return BiosRegion::Overseas;
    }
  }

  std::string name = path.substr(path.find_last_of('/') + 1);
  for (char & c : name) c = tolower((unsigned char)c);
  const auto has = [&](const char * word) { return name.find(word) != std::string::npos; };
  if (has("us") || has("eu") || has("mpr-17933") || has("sega_100a") || has("pal") ||
      has("ntsc-u"))
    return BiosRegion::Overseas;
  if (has("jp") || has("jap") || has("sega_10") || has("sega1003") || has("hisaturn") ||
      has("vsaturn") || has("mpr-18100"))
    return BiosRegion::Japan;
  return BiosRegion::Unknown;
}

// Saturn BIOS images (512 KB) in bios/ and, as earlier versions used, next to the app
static std::vector<BiosFile> findBiosFiles()
{
  std::vector<BiosFile> files;
  // The names earlier versions looked for come first
  for (const char * name : { "bios.bin", "saturn_bios.bin", "sega_101.bin", "mpr-17933.bin" }) {
    const std::string path = nx::dataPath(name);
    if (fileExists(path)) files.push_back({ path, biosRegion(path) });
  }
  for (const std::string & directory : { nx::dataPath("bios"), std::string(NX_DATA_DIR) }) {
    DIR * dir = opendir(directory.c_str());
    if (!dir) continue;
    std::vector<std::string> names;
    while (dirent * entry = readdir(dir))
      names.push_back(entry->d_name);
    closedir(dir);
    std::sort(names.begin(), names.end());
    for (const std::string & name : names) {
      const std::string path = directory + "/" + name;
      struct stat st;
      if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 512 * 1024)
        continue;
      if (std::any_of(files.begin(), files.end(), [&](const BiosFile & f) { return f.path == path; }))
        continue;
      files.push_back({ path, biosRegion(path) });
    }
  }
  return files;
}

// Picks the BIOS for 'choice' (auto / jp / us / hle): auto uses a Japanese BIOS for Japanese
// discs and a US / European one for the rest. Without a BIOS file the core falls back to its
// high-level emulated one.
static bool findBios(const std::string & choice, DiscIO::Region disc_region)
{
  if (choice == "hle")
    return false;
  const std::vector<BiosFile> files = findBiosFiles();
  for (const BiosFile & file : files)
    printf("BIOS file: %s (%s)\n", file.path.c_str(),
           file.region == BiosRegion::Japan ? "Japan" :
           file.region == BiosRegion::Overseas ? "US / Europe" : "region unknown");
  if (files.empty())
    return false;

  BiosRegion wanted;
  if (choice == "jp")
    wanted = BiosRegion::Japan;
  else if (choice == "us")
    wanted = BiosRegion::Overseas;
  else
    wanted = disc_region == DiscIO::Region::NTSC_J || disc_region == DiscIO::Region::NTSC_K ?
             BiosRegion::Japan : BiosRegion::Overseas;

  // The wanted region, else one of unknown region, else whatever there is
  const BiosFile * pick = nullptr;
  for (BiosRegion region : { wanted, BiosRegion::Unknown }) {
    for (const BiosFile & file : files) {
      if (file.region == region) { pick = &file; break; }
    }
    if (pick) break;
  }
  if (!pick) {
    printf("No %s BIOS found, using another\n", wanted == BiosRegion::Japan ? "Japanese" : "US / European");
    pick = &files.front();
  }
  snprintf(biospath, sizeof(biospath), "%s", pick->path.c_str());
  return true;
}

enum class SessionEnd {
  BackToLauncher,  // the player quit the game (MINUS + PLUS)
  ExitApp,         // the system asked the app to close, or the game couldn't start
};

// Runs one game until the player quits it, then tears the emulator down again so the
// launcher can take over the screen.
enum class MenuEnd {
  Resume,
  BackToLauncher,
  ExitApp,
};

static std::string stateDirectory()
{
  return nx::dataPath("states");
}

// Hands the cheat list to the quick menu
static void publishCheats()
{
  std::vector<DolphinSwitch::RuntimeOverlay::CheatEntry> entries;
  for (const nx::cheats::Cheat & cheat : nx::cheats::list())
    entries.push_back({cheat.desc, cheat.enabled, cheat.supported});
  DolphinSwitch::RuntimeOverlay::SetCheats(std::move(entries), nx::cheats::filePath(),
                                           nx::cheats::fileNames());
}

// The system keyboard; false if cancelled or left empty
static bool promptText(const char * header, const char * guide, const std::string & initial,
                       std::string * output)
{
  SwkbdConfig keyboard;
  if (R_FAILED(swkbdCreate(&keyboard, 0)))
    return false;
  swkbdConfigMakePresetDefault(&keyboard);
  swkbdConfigSetHeaderText(&keyboard, header);
  swkbdConfigSetGuideText(&keyboard, guide);
  if (!initial.empty())
    swkbdConfigSetInitialText(&keyboard, initial.c_str());
  char buffer[512] = {};
  swkbdConfigSetStringLenMax(&keyboard, sizeof(buffer) - 1);
  const Result rc = swkbdShow(&keyboard, buffer, sizeof(buffer));
  swkbdClose(&keyboard);
  if (R_FAILED(rc) || buffer[0] == '\0')
    return false;
  *output = buffer;
  return true;
}

// Asks for an Action Replay code and a name for it, then turns it on
static void addCheat()
{
  namespace menu = DolphinSwitch::RuntimeOverlay;
  std::string code, desc, error;
  if (!promptText("Action Replay code", "e.g. 1602E8F0 0063 (separate several with +)", "", &code))
    return;
  // Checked before asking for the name, so a typo doesn't cost both prompts
  if (!nx::cheats::check(code, &error)) {
    menu::ShowAlert("Cheat code not added", error);
    return;
  }
  promptText("Cheat name", "e.g. Infinite lives (optional)", "", &desc);
  if (!nx::cheats::add(desc, code, &error)) {
    menu::ShowAlert("Cheat code", error);
  } else {
    menu::SetStatus("Cheat added and enabled");
  }
  publishCheats();
}

// Carries out what the player picked in the quick menu. Runs on the main thread while the
// game is paused and the main thread holds the GL context (saving a state reads the sprite
// framebuffer back from the GPU).
static bool runMenuAction(const DolphinSwitch::RuntimeOverlay::Action & action)
{
  using DolphinSwitch::RuntimeOverlay::ActionType;
  namespace menu = DolphinSwitch::RuntimeOverlay;
  const std::string slot = std::to_string(action.value);
  switch (action.type) {
  case ActionType::StopToLauncher:
    return true;
  case ActionType::SaveState: {
    const int rc = YabSaveStateSlot(stateDirectory().c_str(), (u8)action.value);
    printf("Save state slot %d: %d\n", action.value, rc);
    menu::RefreshStateInfo();
    menu::SetStatus(rc == 0 ? "Saved state to slot " + slot : "Failed to save state to slot " + slot);
    break;
  }
  case ActionType::LoadState: {
    const int rc = YabLoadStateSlot(stateDirectory().c_str(), (u8)action.value);
    printf("Load state slot %d: %d\n", action.value, rc);
    if (rc == 0) {
      menu::Close();
      menu::SetStatus("Loaded state from slot " + slot);
    } else {
      menu::SetStatus("Failed to load state from slot " + slot);
    }
    break;
  }
  case ActionType::Reset:
    YabauseResetButton();
    menu::SetStatus("Console reset");
    break;
  case ActionType::EjectDisc:
    Cs2ForceOpenTray();
    menu::SetStatus("Disc tray opened");
    break;
  case ActionType::ChangeDisc:
    if (!DolphinSwitch::PrepareLaunchStorage(action.path)) {
      menu::ShowAlert("Change disc", "The selected disc's storage device is no longer available.");
      break;
    }
    snprintf(cdpath, sizeof(cdpath), "%s", action.path.c_str());
    printf("Change disc: %s\n", cdpath);
    Cs2ForceCloseTray(CDCORE_ISO, cdpath);
    menu::SetStatus("Disc changed");
    break;
  case ActionType::ToggleFPS:
    menu::SetShowFPS(!menu::ShowFPS());
    break;
  case ActionType::ToggleCheat: {
    const auto & list = nx::cheats::list();
    if (action.value >= 0 && action.value < (int)list.size()) {
      const bool enable = !list[action.value].enabled;
      nx::cheats::setEnabled(action.value, enable);
      // Values a cheat wrote stay until the game changes them
      menu::SetStatus(enable ? "Cheat enabled" : "Cheat disabled");
      publishCheats();
    }
    break;
  }
  case ActionType::AddCheat:
    addCheat();
    break;
  case ActionType::DeleteCheat:
    if (nx::cheats::remove(action.value))
      menu::SetStatus("Cheat deleted");
    publishCheats();
    break;
  }
  return false;
}

// The quick menu: pauses the game and shows the menu over its last frame until closed.
static MenuEnd runQuickMenu(int width, int height)
{
  namespace menu = DolphinSwitch::RuntimeOverlay;

  // One more frame, with the pads released, so the render thread copies it for the background
  s_input.apply(false);
  nx::overlay::requestCapture();
  YabauseExec();
  s_frames_done++;
  s_progress++;

  ScspMuteAudio(SCSP_MUTE_SYSTEM);
  VdpRevoke();
  YuiUseOGLOnThisThread();

  MenuEnd end = MenuEnd::Resume;
  while (end == MenuEnd::Resume) {
    const u64 frame_start = armGetSystemTick();
    if (!appletMainLoop()) {
      end = MenuEnd::ExitApp;
      break;
    }
    s_input.poll();
    menu::UpdateInput(s_input.down(0), s_input.held(0));
    for (const menu::Action & action : menu::TakeActions())
      if (runMenuAction(action)) end = MenuEnd::BackToLauncher;
    if (end != MenuEnd::Resume || !menu::IsVisible())
      break;

    nx::overlay::drawPaused(width, height);
    eglSwapBuffers(s_display, s_surface);
    s_progress++;

    // About 60 frames a second
    const u64 elapsed = armTicksToNs(armGetSystemTick() - frame_start);
    if (elapsed < 16000000ULL)
      svcSleepThread(16000000ULL - elapsed);
  }

  YuiRevokeOGLOnThisThread();
  VdpResume();
  ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
  return end;
}

// 'game_inis' are the game's own settings files (overrides of settings.ini and input.ini),
// later ones winning
static SessionEnd runGame(const std::string & game, const std::vector<std::string> & game_inis)
{
  snprintf(cdpath, sizeof(cdpath), "%s", game.c_str());
  printf("Game: %s\n", cdpath);
  for (const std::string & ini : game_inis)
    printf("Game settings: %s%s\n", ini.c_str(), fileExists(ini) ? "" : " (none)");

  s_settings = nx::loadSettings(game_inis);
  if (s_settings.dynarec && !DynaJitAvailable()) {
    printf("JIT unavailable, falling back to the SH2 interpreter\n");
    s_settings.dynarec = false;
  }

  const UICommon::GameFile disc(game);
  s_use_bios = findBios(s_settings.bios, disc.IsValid() ? disc.GetRegion() : DiscIO::Region::Unknown);
  printf("BIOS (%s): %s\n", s_settings.bios.c_str(), s_use_bios ? biospath : "(emulated)");

  // The launcher's SDL window released the default window's buffers on shutdown, which
  // also clears its dimensions; without this the EGL surface would be 0x0.
  NWindow * window = nwindowGetDefault();
  bool docked = appletGetOperationMode() == AppletOperationMode_Console;
  u32 window_width, window_height;
  displaySize(docked, &window_width, &window_height);
  if (!nwindowIsValid(window) || R_FAILED(nwindowSetDimensions(window, window_width, window_height)) ||
      R_FAILED(nwindowSetCrop(window, 0, 0, window_width, window_height))) {
    printf("Could not configure the display window\n");
    return SessionEnd::ExitApp;
  }

  if (!initEgl(window))
    return SessionEnd::ExitApp;

  if (!gladLoadGL()) {
    printf("gladLoadGL failed\n");
    deinitEgl();
    return SessionEnd::ExitApp;
  }
  nx_glTextureBarrierNV = (PFNGLTEXTUREBARRIERNVPROC)eglGetProcAddress("glTextureBarrierNV");
  if (!nx_glTextureBarrierNV)
    nx_glTextureBarrierNV = (PFNGLTEXTUREBARRIERNVPROC)eglGetProcAddress("glTextureBarrier");
  printf("GL_RENDERER: %s\n", glGetString(GL_RENDERER));
  printf("GL_VERSION: %s\n", glGetString(GL_VERSION));
  printf("GL_TextureBarrier: %s\n", nx_glTextureBarrierNV ? "yes" : "no");

  // Each game gets a fresh context
  nx_glShimReset();
  nx::overlay::attachContext();
  s_gl_log_count = 0;
  glEnable(GL_DEBUG_OUTPUT);
  glDebugMessageCallback(glDebugLog, NULL);

  // The launcher's window may have left the display at a different size (1080p docked)
  EGLint width = 0, height = 0;
  if (!eglQuerySurface(s_display, s_surface, EGL_WIDTH, &width) ||
      !eglQuerySurface(s_display, s_surface, EGL_HEIGHT, &height) || width <= 0 || height <= 0) {
    width = window_width;
    height = window_height;
  }
  printf("Display: %dx%d (%s)\n", width, height, docked ? "docked" : "handheld");
  s_surface_width = width;
  s_surface_height = height;

  if (yabauseinit(s_use_bios) == -1) {
    printf("YabauseInit failed\n");
    nx::overlay::detachContext();
    deinitEgl();
    return SessionEnd::ExitApp;
  }
  printf("SH2 core: %s, frame skip %s, sound sync %d\n", SH2Core ? SH2Core->Name : "(none)",
    s_settings.frameskip ? "on" : "off", s_settings.scsp_main_mode);
  printf("vsync %s, sync_render %s\n", s_settings.vsync ? "on" : "off", s_settings.sync_render ? "on" : "off");

  // After YabauseInit, which resets the controller ports
  s_input.init(game_inis);

  resizeVideo(width, height);

  // Save states are named after the disc's product number
  const char * game_code = Cs2GetCurrentGmaecode();
  DolphinSwitch::RuntimeOverlay::BeginSession(game, stateDirectory(), game_code ? game_code : "",
                                              false);
  nx::cheats::load(game, game_code);
  publishCheats();

  s_frames_done = 0;
  s_phase = Phase::Game;

  u64 stats_start = armGetSystemTick();
  int stats_frames = 0;
  SessionEnd end = SessionEnd::ExitApp;

  while (appletMainLoop()) {
    s_input.poll();

    // Docked or undocked: draw at the new display's resolution
    const bool now_docked = appletGetOperationMode() == AppletOperationMode_Console;
    if (now_docked != docked) {
      docked = now_docked;
      if (!switchDisplayMode(docked))
        break;
    }

    // MINUS + PLUS opens the quick menu, which pauses the game
    DolphinSwitch::RuntimeOverlay::UpdateInput(s_input.down(0), s_input.held(0));
    if (DolphinSwitch::RuntimeOverlay::IsVisible()) {
      const MenuEnd menu_end = runQuickMenu(s_surface_width, s_surface_height);
      if (menu_end == MenuEnd::BackToLauncher) {
        end = SessionEnd::BackToLauncher;
        break;
      }
      if (menu_end == MenuEnd::ExitApp)
        break;
      stats_frames = 0;
      stats_start = armGetSystemTick();
      continue;
    }
    s_input.apply(!DolphinSwitch::RuntimeOverlay::IsInputCaptured());

    YabauseExec(); // one frame
    s_frames_done++;
    s_progress++;

    // Persist save RAM changes every couple of seconds
    if (s_frames_done % 120 == 0)
      YabMemMapFlush();

    // Emulation speed and disc status in the log every few seconds
    stats_frames++;
    u64 elapsed_ns = armTicksToNs(armGetSystemTick() - stats_start);
    // The FPS counter wants fresher numbers than the log
    if (elapsed_ns >= 1000000000ULL && DolphinSwitch::RuntimeOverlay::ShowFPS()) {
      nx::overlay::setFps((float)(stats_frames * 1e9 / (double)elapsed_ns));
    }
    if (elapsed_ns >= 5000000000ULL) {
      const double fps = stats_frames * 1e9 / (double)elapsed_ns;
      printf("frames/s %.1f, game code '%s'\n", fps, Cs2GetCurrentGmaecode());
      nx::overlay::setFps((float)fps);
      stats_frames = 0;
      stats_start = armGetSystemTick();
    }
  }

  // Saves first, so nothing is lost if teardown goes wrong
  printf("Stopping game\n");
  YabMemMapFlush();
  s_phase = Phase::GameTeardown;

  // The render thread owns the GL context and would exit still holding it,
  // which leaves Mesa unable to free the context: EGL teardown then waits
  // forever for the GPU driver to return its memory. Take it back first, so
  // the video core's own cleanup also runs with a current context.
  printf("Taking back GL context\n");
  VdpRevoke();
  YuiUseOGLOnThisThread();
  glFinish();

  DolphinSwitch::RuntimeOverlay::EndSession();
  nx::cheats::unload();
  nx::overlay::detachContext();

  printf("YabauseDeInit\n");
  YabauseDeInit();
  printf("deinitEgl\n");
  deinitEgl();
  // The sound core initialises SDL audio but doesn't release it
  SDL_QuitSubSystem(SDL_INIT_AUDIO);

  s_phase = Phase::Launcher;
  printf("Game stopped\n");
  return end;
}

// Arguments from a HOME Menu shortcut: --game <path> [--library-id <id>] [--game-config <ini>];
// a bare path (nxlink -a) works too
static std::optional<DolphinSwitch::LaunchRequest> directLaunchRequest(int argc, char** argv)
{
  DolphinSwitch::LaunchRequest request;
  bool found = false;
  for (int index = 1; index < argc; ++index) {
    if (!argv[index] || !argv[index][0]) continue;
    const std::string argument = argv[index];
    const bool has_value = index + 1 < argc && argv[index + 1];
    if ((argument == "--game" || argument == "-g") && has_value) {
      request.path = argv[++index];
      found = true;
    } else if (argument == "--game-config" && has_value) {
      request.game_config_path = argv[++index];
    } else if (argument == "--library-id" && has_value) {
      request.library_id = argv[++index];
    } else if (!found && argument[0] != '-') {
      request.path = argument;
      found = true;
    }
  }
  if (!found || request.path.empty()) return std::nullopt;
  return request;
}

int main(int argc, char** argv)
{
  nx::ensureDataDirs();
  initLog();
  nx::migrateOldDataDir();
  printf("YabaSanshiro NX %s (core %s, %s)\n", YAB_NX_RELEASE_VERSION, YAB_VERSION, GIT_SHA1);

  shader_cache_path = nx::dataPath("cache/");
  snprintf(buppath, sizeof(buppath), "%s", nx::dataPath("backup.bin").c_str());

  // The launcher's translations and artwork
  const bool romfs = R_SUCCEEDED(romfsInit());
  if (!romfs)
    printf("romfsInit failed\n");
  mkdir(stateDirectory().c_str(), 0777);
  nx::overlay::init();

  // Higher priority than the emulator threads so a spinning thread can't starve it
  s_main_thread = threadGetCurHandle();
  Thread watchdog;
  const bool watchdog_started =
    R_SUCCEEDED(threadCreate(&watchdog, watchdogMain, NULL, NULL, 0x4000, 0x20, -2)) &&
    R_SUCCEEDED(threadStart(&watchdog));

  // Where hbloader should start us again when a game is left
  const std::string self_path = argc > 0 && argv[0] && argv[0][0] ? argv[0] : NX_DATA_DIR "/yabasanshiro.nro";

  // One game per process: running a second game after the first in the same process isn't
  // reliable (the launcher's SDL video and the emulator's EGL keep taking the display window
  // from each other, and emulator state carries over), so leaving a game restarts the app.
  DolphinSwitch::Forwarder::SetSelfPath(self_path);

  std::string game;
  std::vector<std::string> game_inis;
  std::optional<DolphinSwitch::LaunchRequest> direct = directLaunchRequest(argc, argv);
  if (direct) {
    // HOME Menu shortcuts (--game ...) and nxlink -a <path> boot the game straight away
    std::string path;
    const bool ready = direct->library_id.empty() ?
      DolphinSwitch::PrepareLaunchStorage(direct->path, &path) :
      DolphinSwitch::ResolveLibraryLaunchPath(direct->library_id, direct->path, &path);
    if (!ready || path.empty())
      path = direct->path;
    printf("Direct launch: %s%s\n", path.c_str(), fileExists(path) ? "" : " (not found)");
    if (fileExists(path)) {
      game = path;
      const UICommon::GameFile file(game);
      if (file.IsValid() && !file.GetGameID().empty())
        game_inis.push_back(nx::dataPath("GameSettings/") + file.GetGameID() + ".ini");
      if (!direct->game_config_path.empty())
        game_inis.push_back(direct->game_config_path);
    }
  }
  if (game.empty()) {
    std::string startup_message;
    while (appletMainLoop()) {
      std::optional<DolphinSwitch::LaunchRequest> request =
        DolphinSwitch::RunLauncher(startup_message, self_path);
      printf("Launcher closed%s\n", request ? "" : " (no game chosen)");
      startup_message.clear();
      if (!request)
        break;
      if (!fileExists(request->path)) {
        startup_message = "The game file could not be found: " + request->path;
        continue;
      }
      game = request->path;
      // Where the launcher keeps this game's settings (see Launcher::GetGameSetting): the
      // file for its product number, and for a renamed game its own entry file over that
      if (!request->game_id.empty())
        game_inis.push_back(nx::dataPath("GameSettings/") + request->game_id + ".ini");
      if (!request->game_config_path.empty())
        game_inis.push_back(request->game_config_path);
      break;
    }
  }

  if (!game.empty() && runGame(game, game_inis) == SessionEnd::BackToLauncher) {
    // Ask hbloader to start us again once we've exited: back to a fresh launcher. Without
    // hbloader support this simply returns to the homebrew menu.
    if (envHasNextLoad()) {
      const std::string arguments = "\"" + self_path + "\"";
      Result rc = envSetNextLoad(self_path.c_str(), arguments.c_str());
      printf("Relaunching %s: 0x%x\n", self_path.c_str(), rc);
    } else {
      printf("hbloader can't relaunch; returning to the homebrew menu\n");
    }
  }

  DolphinSwitch::ShutdownLauncherStorage();
  // The dynarec's code buffer lives as long as the app; hbloader aborts if
  // code memory is still mapped when we return to it
  DynaJitShutdown();
  nx::overlay::shutdown();
  SDL_Quit();
  if (romfs)
    romfsExit();
  s_watchdog_running = false;
  if (watchdog_started) {
    threadWaitForExit(&watchdog);
    threadClose(&watchdog);
  }
  printf("Exited cleanly\n");
  return 0;
}
