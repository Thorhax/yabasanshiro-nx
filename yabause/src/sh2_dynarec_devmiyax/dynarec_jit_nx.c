/*
        Copyright 2026

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

// Horizon (Switch) code buffer for the devMiyax SH2 dynarec.
//
// Horizon never allows RWX pages. libnx's JIT maps the same memory twice:
// a RW alias the compiler writes blocks into, and a RX alias they run from.
// The dynarec keeps using the RW pointer for everything (block metadata lives
// next to the code), and only translates to the RX alias when calling a block.
//
// Kept apart from DynarecSh2.cpp so switch.h's integer typedefs never meet
// Yabause's core.h.

#include <stdio.h>
#include <stdint.h>
#include <switch.h>

#include "dynarec_jit_nx.h"

static Jit s_jit;
static int s_jit_created = 0;

intptr_t g_dyna_jit_rx_offset = 0;

int DynaJitAvailable(void)
{
  Jit probe;
  Result rc = jitCreate(&probe, 0x1000);
  int ok;

  if (R_FAILED(rc)) {
    printf("DynaJitAvailable: jitCreate failed: 0x%x\n", rc);
    return 0;
  }
  ok = probe.type == JitType_CodeMemory;
  if (!ok) printf("DynaJitAvailable: CodeMemory JIT unavailable (type %d)\n", probe.type);
  jitClose(&probe);
  return ok;
}

void * DynaJitAlloc(size_t size)
{
  Result rc;

  if (s_jit_created) {
    printf("DynaJitAlloc: code buffer already allocated\n");
    return NULL;
  }

  rc = jitCreate(&s_jit, size);
  if (R_FAILED(rc)) {
    printf("DynaJitAlloc: jitCreate(%zu) failed: 0x%x\n", size, rc);
    return NULL;
  }

  if (s_jit.type != JitType_CodeMemory) {
    // The SetProcessMemoryPermission fallback has a single alias that must be
    // flipped between RW and RX as a whole; the dynarec can't work that way.
    printf("DynaJitAlloc: CodeMemory JIT unavailable (type %d)\n", s_jit.type);
    jitClose(&s_jit);
    return NULL;
  }

  s_jit_created = 1;
  g_dyna_jit_rx_offset = (intptr_t)jitGetRxAddr(&s_jit) - (intptr_t)jitGetRwAddr(&s_jit);
  printf("DynaJitAlloc: %zu bytes rw=%p rx=%p\n", size, jitGetRwAddr(&s_jit), jitGetRxAddr(&s_jit));
  return jitGetRwAddr(&s_jit);
}

void DynaJitFree(void * rw, size_t size)
{
  Result rc;

  if (!s_jit_created || rw != jitGetRwAddr(&s_jit)) return;

  rc = jitClose(&s_jit);
  if (R_FAILED(rc)) {
    printf("DynaJitFree: jitClose failed: 0x%x\n", rc);
  }
  s_jit_created = 0;
  g_dyna_jit_rx_offset = 0;
}

void DynaJitFlush(void * rw_begin, void * rw_end)
{
  size_t size = (uintptr_t)rw_end - (uintptr_t)rw_begin;

  // Push the new code out of the D-cache through the alias it was written to,
  // then drop stale I-cache lines on the alias it will be fetched from.
  armDCacheFlush(rw_begin, size);
  armICacheInvalidate((void *)((uintptr_t)rw_begin + g_dyna_jit_rx_offset), size);
}

void DynaJitShutdown(void)
{
  // The dynarec's code cache is a singleton that is never destroyed, so the
  // buffer outlives YabauseDeInit. It must be unmapped before returning to
  // hbloader, which aborts (kernel InvalidState) if code memory is still mapped.
  if (s_jit_created) DynaJitFree(jitGetRwAddr(&s_jit), s_jit.size);
}
