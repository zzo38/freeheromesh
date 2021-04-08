#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-unused-result sound.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "quarks.h"
#include "heromesh.h"

typedef struct {
  Uint8*data;
  Uint32 len; // length in bytes
} WaveSound;

static Uint8 sound_on;
static Sint16 mmlvolume=32767;
static SDL_AudioSpec spec;
static WaveSound*standardsounds;
static Uint16 nstandardsounds;
static WaveSound*usersounds;
static Uint16 nusersounds;
static FILE*l_fp;
static long l_offset,l_size;
static float wavevolume=1.0;
static Uint8 needs_amplify=0;

static Uint8*volatile wavesound;
static volatile Uint32 wavelen;

static void audio_callback(void*userdata,Uint8*stream,int len) {
  if(wavesound) {
    if(wavelen<len) {
      memcpy(stream,wavesound,wavelen);
      memset(stream+wavelen,0,len-wavelen);
      wavesound=0;
      wavelen=0;
    } else {
      memcpy(stream,wavesound,len);
      wavesound+=len;
      len-=len;
    }
  } else {
    memset(stream,0,len);
  }
}

static int my_seek(SDL_RWops*cxt,int o,int w) {
  switch(w) {
    case RW_SEEK_SET: fseek(l_fp,l_offset+o,SEEK_SET); break;
    case RW_SEEK_CUR: fseek(l_fp,o,SEEK_CUR); break;
    case RW_SEEK_END: fseek(l_fp,l_offset-o,SEEK_SET); break;
  }
  return ftell(l_fp)-l_offset;
}

static int my_read(SDL_RWops*cxt,void*ptr,int size,int maxnum) {
  if(size*maxnum+l_offset>ftell(l_fp)+l_size) maxnum=(ftell(l_fp)-l_offset)/size;
  return fread(ptr,size,maxnum,l_fp);
}

static int my_close(SDL_RWops*cxt) {
  return 0;
}

static SDL_RWops my_rwops={
  .seek=my_seek,
  .read=my_read,
  .close=my_close,
};

static void amplify_wave_sound(const WaveSound*ws) {
  Uint32 n;
  Sint16*b=(Sint16*)ws->data;
  Uint32 m=ws->len/sizeof(Sint16);
  if(!needs_amplify || !b) return;
  for(n=0;n<m;n++) b[n]*=wavevolume;
}

static void load_sound(FILE*fp,long offset,long size,WaveSound*ws) {
  SDL_AudioSpec src;
  SDL_AudioCVT cvt;
  Uint32 len=0;
  Uint8*buf=0;
  ws->data=0;
  ws->len=0;
  l_fp=fp;
  l_offset=offset;
  l_size=size;
  if(!SDL_LoadWAV_RW(&my_rwops,0,&src,&buf,&len)) {
    //fprintf(stderr,"[Cannot load wave audio at %ld (%ld bytes): %s]\n",offset,size,SDL_GetError());
    return;
  }
  memset(&cvt,0,sizeof(SDL_AudioCVT));
  if(SDL_BuildAudioCVT(&cvt,src.format,src.channels,src.freq,spec.format,spec.channels,spec.freq)<0) goto fail;
  cvt.buf=malloc(len*cvt.len_mult);
  cvt.len=len;
  if(!cvt.buf) goto fail;
  memcpy(cvt.buf,buf,len);
  if(SDL_ConvertAudio(&cvt)) goto fail;
  SDL_FreeWAV(buf);
  ws->data=cvt.buf;
  ws->len=cvt.len;
  amplify_wave_sound(ws);
  return;
  fail:
  //fprintf(stderr,"[Failed to convert wave audio at %ld (%ld bytes)]\n",offset,size);
  SDL_FreeWAV(buf);
}

void init_sound(void) {
  const char*v;
  optionquery[1]=Q_audio;
  optionquery[2]=Q_rate;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,3);
  if(!v) return;
  spec.freq=strtol(v,0,10);
  optionquery[2]=Q_buffer;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,3);
  if(!v) return;
  spec.samples=strtol(v,0,10);
  if(!spec.freq || !spec.samples) return;
  fprintf(stderr,"Initializing audio...\n");
  spec.channels=1;
  spec.format=AUDIO_S16SYS;
  spec.callback=audio_callback;
  if(SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    fprintf(stderr,"Cannot initalize audio subsystem.\n");
    return;
  }
  if(SDL_OpenAudio(&spec,0)) {
    fprintf(stderr,"Cannot open audio device.\n");
    return;
  }
  optionquery[2]=Q_waveVolume;
  if(v=xrm_get_resource(resourcedb,optionquery,optionquery,3)) {
    needs_amplify=1;
    wavevolume=strtod(v,0);
  }
  optionquery[2]=Q_mmlVolume;
  if(v=xrm_get_resource(resourcedb,optionquery,optionquery,3)) mmlvolume=fmin(strtod(v,0)*32767.0,32767.0);
  
  fprintf(stderr,"Done.\n");
  wavesound=0;
  SDL_PauseAudio(0);
  sound_on=1;
}

void set_sound_effect(Value v1,Value v2) {
  if(!sound_on) return;
  if(!v2.t && !v2.u && wavesound) return;
  SDL_LockAudio();
  wavesound=0;
  switch(v1.t) {
    case TY_SOUND:
      if(v1.u<nstandardsounds) {
        wavesound=standardsounds[v1.u].data;
        wavelen=standardsounds[v1.u].len;
      }
      break;
    case TY_USOUND:
      if(v1.u<nusersounds) {
        wavesound=usersounds[v1.u].data;
        wavelen=usersounds[v1.u].len;
      }
      break;
    case TY_STRING: case TY_LEVELSTRING:
      if(!mmlvolume) break;
      
      break;
  }
  SDL_UnlockAudio();
}