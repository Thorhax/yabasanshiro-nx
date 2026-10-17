/*  Copyright 2005-2006 Theo Berkau

    This file is part of Yabause.

    Yabause is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    Yabause is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Yabause; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

/*! \file sndsdl.c
    \brief SDL sound interface.
*/

#ifdef HAVE_LIBSDL

#include <stdlib.h>

#if defined(__APPLE__) || defined(GEKKO)
 #ifdef HAVE_LIBSDL2
  #include <SDL2/SDL.h>
 #else
  #include <SDL/SDL.h>
 #endif
#else
 #include "SDL.h"
#endif
#include "error.h"
#include "scsp.h"
#include "sndsdl.h"
#include "debug.h"

static int SNDSDLInit(void);
static void SNDSDLDeInit(void);
static int SNDSDLReset(void);
static int SNDSDLChangeVideoFormat(int vertfreq);
static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len);
static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples);
static u32 SNDSDLGetAudioSpace(void);
static void SNDSDLMuteAudio(void);
static void SNDSDLUnMuteAudio(void);
static void SNDSDLSetVolume(int volume);
#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport);
u8 SNDSDLMidiIn(int *isdata);
int SNDSDLMidiOut(u8 data);
#endif

SoundInterface_struct SNDSDL = {
SNDCORE_SDL,
"SDL Sound Interface",
SNDSDLInit,
SNDSDLDeInit,
SNDSDLReset,
SNDSDLChangeVideoFormat,
SNDSDLUpdateAudio,
SNDSDLGetAudioSpace,
SNDSDLMuteAudio,
SNDSDLUnMuteAudio,
SNDSDLSetVolume,
#ifdef USE_SCSPMIDI
SNDSDLMidiChangePorts,
SNDSDLMidiIn,
SNDSDLMidiOut
#endif
};

// Ring buffer of NUMSOUNDBLOCKS frames of sound. soundfill counts the bytes
// written but not yet played: upstream had no fill count, so when the game
// ran a little slow the callback replayed old sound (heard as stutter) and an
// empty buffer looked full to the writer.
#define NUMSOUNDBLOCKS  6

static u16 *stereodata16;
static u32 soundoffset;
static volatile u32 soundpos;
static volatile u32 soundfill;
static volatile u32 soundunderruns;
static u32 soundlen;
static u32 soundbufsize;
static SDL_AudioSpec audiofmt;
static u8 soundvolume;
static int muted = 0;

//////////////////////////////////////////////////////////////////////////////

static void MixAudio(UNUSED void *userdata, Uint8 *stream, int len) {
	int i;
	Uint8* soundbuf = (Uint8*)stereodata16;
	int avail = (int)soundfill;

	// Whole stereo samples only, so the channels never swap
	if (avail > len)
		avail = len;
	avail &= ~3;

	for (i = 0; i < avail; i++)
	{
		if (soundpos >= soundbufsize)
			soundpos = 0;
		stream[i] = muted ? audiofmt.silence : soundbuf[soundpos];
		soundpos++;
	}
	soundfill -= avail;

	// Ran dry: play silence rather than old sound
	if (i < len)
		soundunderruns++;
	for (; i < len; i++)
		stream[i] = audiofmt.silence;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLInit(void)
{
   //samples should be a power of 2 according to SDL-doc
   //so normalize it to the nearest power of 2 here
   u32 normSamples = 512;

#if defined (_MSC_VER) && SDL_VERSION_ATLEAST(2,0,0)
   SDL_SetMainReady();
#endif
   SDL_InitSubSystem(SDL_INIT_AUDIO);
//   if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0);
//      return -1;

   audiofmt.freq = 44100;
   audiofmt.format = AUDIO_S16SYS;
   audiofmt.channels = 2;
   audiofmt.samples = (audiofmt.freq / 60) * 2;
   audiofmt.callback = MixAudio;
   audiofmt.userdata = NULL;

   while (normSamples < audiofmt.samples) 
      normSamples <<= 1;

   audiofmt.samples = normSamples;
   
   soundlen = audiofmt.freq / 60; // 60 for NTSC or 50 for PAL. Initially assume it's going to be NTSC.
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;
   
   soundvolume = SDL_MIX_MAXVOLUME;

   if (SDL_OpenAudio(&audiofmt, NULL) != 0)
   {
      YabSetError(YAB_ERR_SDL, (void *)SDL_GetError());
      return -1;
   }

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL)
      return -1;

   memset(stereodata16, 0, soundbufsize);

   soundpos = 0;
   soundoffset = 0;
   soundfill = 0;

   SDL_PauseAudio(0);

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLDeInit(void)
{
   SDL_CloseAudio();

   if (stereodata16)
      free(stereodata16);
   stereodata16 = NULL;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLReset(void)
{
   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static int SNDSDLChangeVideoFormat(int vertfreq)
{
   SDL_LockAudio();
   soundlen = audiofmt.freq / vertfreq;
   soundbufsize = soundlen * NUMSOUNDBLOCKS * 2 * 2;

   if (stereodata16)
      free(stereodata16);

   if ((stereodata16 = (u16 *)malloc(soundbufsize)) == NULL) {
      soundbufsize = 0;
      SDL_UnlockAudio();
      return -1;
   }

   memset(stereodata16, 0, soundbufsize);
   soundpos = 0;
   soundoffset = 0;
   soundfill = 0;
   SDL_UnlockAudio();

   return 0;
}

//////////////////////////////////////////////////////////////////////////////

static void sdlConvert32uto16s(s32 *srcL, s32 *srcR, s16 *dst, u32 len) {
   u32 i;

   for (i = 0; i < len; i++)
   {
      // Left Channel
      *srcL = ( *srcL *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcL > 0x7FFF) *dst = 0x7FFF;
      else if (*srcL < -0x8000) *dst = -0x8000;
      else *dst = *srcL;
      srcL++;
      dst++;
      // Right Channel
	  *srcR = ( *srcR *soundvolume ) /SDL_MIX_MAXVOLUME;
      if (*srcR > 0x7FFF) *dst = 0x7FFF;
      else if (*srcR < -0x8000) *dst = -0x8000;
      else *dst = *srcR;
      srcR++;
      dst++;
   } 
}

static void SNDSDLUpdateAudio(u32 *leftchanbuffer, u32 *rightchanbuffer, u32 num_samples)
{
   u32 copy1size=0, copy2size=0;
   SDL_LockAudio();

   // Never overwrite sound that hasn't been played yet
   if (num_samples > (soundbufsize - soundfill) / (sizeof(s16) * 2))
      num_samples = (soundbufsize - soundfill) / (sizeof(s16) * 2);

   if ((soundbufsize - soundoffset) < (num_samples * sizeof(s16) * 2))
   {
      copy1size = (soundbufsize - soundoffset);
      copy2size = (num_samples * sizeof(s16) * 2) - copy1size;
   }
   else
   {
      copy1size = (num_samples * sizeof(s16) * 2);
      copy2size = 0;
   }

   sdlConvert32uto16s((s32 *)leftchanbuffer, (s32 *)rightchanbuffer, 
   (s16 *)(((u8 *)stereodata16)+soundoffset), copy1size / sizeof(s16) / 2);

   if (copy2size)
      sdlConvert32uto16s((s32 *)leftchanbuffer + (copy1size / sizeof(s16) / 2), 
      (s32 *)rightchanbuffer + (copy1size / sizeof(s16) / 2), (s16 *)stereodata16,
       copy2size / sizeof(s16) / 2);

   soundoffset += copy1size + copy2size;   
   soundoffset %= soundbufsize;
   soundfill += copy1size + copy2size;

   SDL_UnlockAudio();
}

//////////////////////////////////////////////////////////////////////////////

static u32 SNDSDLGetAudioSpace(void)
{
   return ((soundbufsize - soundfill) / sizeof(s16) / 2);
}

//////////////////////////////////////////////////////////////////////////////

// Samples queued for playback, for the frame limiter's sound check
u32 SNDSDLBufferedSamples(void)
{
   // Not the active sound core: report plenty so nobody waits on it
   if (stereodata16 == NULL)
      return 0xFFFFFFFF;
   return soundfill / sizeof(s16) / 2;
}

// Times the sound ran dry since the last call, for the port's log
u32 SNDSDLTakeUnderruns(void)
{
   u32 count = soundunderruns;
   soundunderruns -= count;
   return count;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLMuteAudio(void)
{
   muted = 1;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLUnMuteAudio(void)
{
   muted = 0;
}

//////////////////////////////////////////////////////////////////////////////

static void SNDSDLSetVolume(int volume)
{
   soundvolume = ( (double)SDL_MIX_MAXVOLUME /(double)100 ) *volume;
}

//////////////////////////////////////////////////////////////////////////////

#ifdef USE_SCSPMIDI
int SNDSDLMidiChangePorts(int inport, int outport)
{
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

u8 SNDSDLMidiIn(int *isdata)
{
	*isdata = 0;
	return 0;
}

//////////////////////////////////////////////////////////////////////////////

int SNDSDLMidiOut(u8 data)
{
	return 1;
}

//////////////////////////////////////////////////////////////////////////////
#endif
#endif
