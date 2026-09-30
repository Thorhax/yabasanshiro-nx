#ifndef DYNAREC_JIT_NX_H
#define DYNAREC_JIT_NX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Distance from the RW alias of the code buffer to its RX alias.
extern intptr_t g_dyna_jit_rx_offset;

// Non-zero if a dual-mapped (CodeMemory) JIT buffer can be created.
int DynaJitAvailable(void);

// Returns the RW alias of a new code buffer, or NULL if JIT is unavailable.
void * DynaJitAlloc(size_t size);
void DynaJitFree(void * rw, size_t size);

// Unmaps the code buffer at exit; the dynarec must not run afterwards.
void DynaJitShutdown(void);

// Makes code written to [rw_begin, rw_end) visible to the RX alias.
void DynaJitFlush(void * rw_begin, void * rw_end);

#ifdef __cplusplus
}
#endif

#endif
