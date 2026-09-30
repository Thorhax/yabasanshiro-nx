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

#include <switch/types.h>
#include <switch/result.h>
#include <switch/arm/counter.h>
#include <switch/kernel/thread.h>
#include <switch/kernel/uevent.h>
#include <switch/kernel/svc.h>
#include <switch/kernel/mutex.h>
#include <switch/kernel/condvar.h>
#include "core.h"
#include "threads.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

// Cores 0-2 belong to applications; core 3 is reserved for the system.
#define NX_APP_CORES 3
#define NX_THREAD_STACK_SIZE (1024 * 1024)
#define NX_THREAD_PRIORITY 0x2C

//////////////////////////////////////////////////////////////////////////////

typedef struct
{
  Thread thread;
  UEvent wake;     // auto-clear; backs YabThreadSleep / YabThreadWake
  void * (*func)(void *);
  void * arg;
  int running;
} YabThreadNx;

static YabThreadNx threads[YAB_NUM_THREADS];

// YAB_NUM_THREADS for threads not started by YabThreadStart (e.g. main)
static __thread int current_id = YAB_NUM_THREADS;

static Mutex core_mutex;
static int next_core = 0;

int YabThreadInit(){
    return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void ThreadEntry(void * p)
{
  YabThreadNx * t = (YabThreadNx *)p;
  current_id = (int)(t - threads);
  t->func(t->arg);
}

int YabThreadStart(unsigned int id, const char * name, void * (*func)(void *), void *arg)
{
  Result rc;
  YabThreadNx * t = &threads[id];

  if (t->running)
  {
    printf("YabThreadStart: thread %u (%s) is already started\n", id, name);
    return -1;
  }

  t->func = func;
  t->arg = arg;
  ueventCreate(&t->wake, true);

  // -2: let the kernel pick; YabThreadSetCurrentThreadAffinityMask pins it later
  rc = threadCreate(&t->thread, ThreadEntry, t, NULL, NX_THREAD_STACK_SIZE, NX_THREAD_PRIORITY, -2);
  if (R_FAILED(rc))
  {
    printf("YabThreadStart: failed to create thread %u (%s): 0x%x\n", id, name, rc);
    return -1;
  }

  rc = threadStart(&t->thread);
  if (R_FAILED(rc))
  {
    printf("YabThreadStart: failed to start thread %u (%s): 0x%x\n", id, name, rc);
    threadClose(&t->thread);
    return -1;
  }

  t->running = 1;
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

void YabThreadWait(unsigned int id)
{
  YabThreadNx * t = &threads[id];

  if (!t->running)
    return;  // Thread wasn't running in the first place

  threadWaitForExit(&t->thread);
  threadClose(&t->thread);
  t->running = 0;
}

//////////////////////////////////////////////////////////////////////////////

void YabThreadYield(void)
{
  svcSleepThread(YieldType_WithoutCoreMigration);
}

//////////////////////////////////////////////////////////////////////////////

void YabThreadSleep(void)
{
  if (current_id >= YAB_NUM_THREADS)
  {
    YabThreadYield();
    return;
  }
  waitSingle(waiterForUEvent(&threads[current_id].wake), -1);
}

void YabThreadUSleep( unsigned int stime )
{
  // stime is in microseconds, like usleep()
  svcSleepThread((s64)stime * 1000);
}

//////////////////////////////////////////////////////////////////////////////

void YabThreadRemoteSleep(unsigned int id)
{
}

//////////////////////////////////////////////////////////////////////////////

void YabThreadWake(unsigned int id)
{
  if (!threads[id].running)
    return;  // Thread isn't running

  ueventSignal(&threads[id].wake);
}

//////////////////////////////////////////////////////////////////////////////

typedef struct YabEventQueue_nx
{
  int *buffer;
  int capacity;
  int size;
  int in;
  int out;
  Mutex mutex;
  CondVar cond_full;
  CondVar cond_empty;
} YabEventQueue_nx;


YabEventQueue * YabThreadCreateQueue( int qsize ){
  YabEventQueue_nx * p = (YabEventQueue_nx*)malloc(sizeof(YabEventQueue_nx));
  p->buffer = (int*)malloc( sizeof(int)* qsize);
  p->capacity = qsize;
  p->size = 0;
  p->in = 0;
  p->out = 0;
  mutexInit(&p->mutex);
  condvarInit(&p->cond_full);
  condvarInit(&p->cond_empty);
  return (YabEventQueue *)p;
}

void YabThreadDestoryQueue( YabEventQueue * queue_t ){
  YabEventQueue_nx * queue = (YabEventQueue_nx*)queue_t;
  mutexLock(&queue->mutex);
  while (queue->size == queue->capacity)
    condvarWait(&queue->cond_full, &queue->mutex);
  mutexUnlock(&queue->mutex);
  free(queue->buffer);
  free(queue);
}

void YabAddEventQueue( YabEventQueue * queue_t, int evcode ){
  YabEventQueue_nx * queue = (YabEventQueue_nx*)queue_t;
  mutexLock(&queue->mutex);
  while (queue->size == queue->capacity)
    condvarWait(&queue->cond_full, &queue->mutex);
  queue->buffer[queue->in] = evcode;
  ++ queue->size;
  ++ queue->in;
  queue->in %= queue->capacity;
  condvarWakeOne(&queue->cond_empty);
  mutexUnlock(&queue->mutex);
}

int YabClearEventQueue(YabEventQueue * queue_t) {
  YabEventQueue_nx * queue = (YabEventQueue_nx*)queue_t;
  mutexLock(&queue->mutex);
  queue->size = 0;
  queue->out = queue->in;
  condvarWakeAll(&queue->cond_full);
  mutexUnlock(&queue->mutex);
  return 0;
}

int YabWaitEventQueue( YabEventQueue * queue_t ){
  int value;
  YabEventQueue_nx * queue = (YabEventQueue_nx*)queue_t;
  mutexLock(&queue->mutex);
  while (queue->size == 0)
    condvarWait(&queue->cond_empty, &queue->mutex);
  value = queue->buffer[queue->out];
  -- queue->size;
  ++ queue->out;
  queue->out %= queue->capacity;
  condvarWakeOne(&queue->cond_full);
  mutexUnlock(&queue->mutex);
  return value;
}

int YaGetQueueSize(YabEventQueue * queue_t){
  int size = 0;
  YabEventQueue_nx * queue = (YabEventQueue_nx*)queue_t;
  mutexLock(&queue->mutex);
  size = queue->size;
  mutexUnlock(&queue->mutex);
  return size;
}

//////////////////////////////////////////////////////////////////////////////

typedef struct YabMutex_nx
{
  Mutex mutex;
} YabMutex_nx;

void YabThreadLock( YabMutex * mtx ){
  mutexLock(&((YabMutex_nx *)mtx)->mutex);
}

void YabThreadUnLock( YabMutex * mtx ){
  mutexUnlock(&((YabMutex_nx *)mtx)->mutex);
}

YabMutex * YabThreadCreateMutex(){
  YabMutex_nx * mtx = (YabMutex_nx *)malloc(sizeof(YabMutex_nx));
  mutexInit(&mtx->mutex);
  return (YabMutex *)mtx;
}

void YabThreadFreeMutex( YabMutex * mtx ){
  free(mtx);
}

//////////////////////////////////////////////////////////////////////////////

// All three application cores are identical, so "fastest" just means
// "not handed out yet": spread the emulator's busy threads across them.
int YabThreadGetFastestCpuIndex(){
  int core;
  mutexLock(&core_mutex);
  core = next_core;
  next_core = (next_core + 1) % NX_APP_CORES;
  mutexUnlock(&core_mutex);
  return core;
}

void YabThreadSetCurrentThreadAffinityMask(int mask)
{
  // Like the Linux port, the argument is a core index, not a bit mask
  Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, mask, 1ULL << mask);
  if (R_FAILED(rc))
    printf("YabThreadSetCurrentThreadAffinityMask(%d) failed: 0x%x\n", mask, rc);
}

int YabThreadGetCurrentThreadAffinityMask()
{
  return svcGetCurrentProcessorNumber();
}

//////////////////////////////////////////////////////////////////////////////

// Same unit as the Linux port: despite the name, ns is in microseconds
int YabNanosleep(u64 ns)
{
  svcSleepThread((s64)ns * 1000);
  return 0;
}

static void RemoveTree(const char * path)
{
  DIR * d = opendir(path);
  if (d) {
    struct dirent * e;
    char child[PATH_MAX];
    while ((e = readdir(d)) != NULL) {
      if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
      snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
      RemoveTree(child);
    }
    closedir(d);
    rmdir(path);
  } else {
    unlink(path);
  }
}

int YabMakeCleanDir( const char * dirname )
{
  RemoveTree(dirname);
  if (mkdir(dirname, 0777) != 0) {
    printf("YabMakeCleanDir: failed to create %s\n", dirname);
  }
  return 0;
}

//////////////////////////////////////////////////////////////////////////////

// For the port's hang watchdog: the kernel handle of a running Yabause
// thread, or 0 (INVALID_HANDLE) if that thread isn't running.
u32 YabThreadGetNxHandle(unsigned int id)
{
  if (id >= YAB_NUM_THREADS || !threads[id].running) return 0;
  return threads[id].thread.handle;
}
