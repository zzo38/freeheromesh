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
#include "cursorshapes.h"

#define N_STANDARD_SOUNDS 50

typedef struct {
  Uint8*data;
  Uint32 len; // length in bytes
} WaveSound;

static Uint8 sound_on;
static Sint16 mmlvolume=10000;
static SDL_AudioSpec spec;
static WaveSound*standardsounds;
static Uint16 nstandardsounds;
static WaveSound*usersounds;
static Uint16 nusersounds;
static Uint8**user_sound_names;
static FILE*l_fp;
static long l_offset,l_size;
static float wavevolume=1.0;
static Uint8 needs_amplify=0;
static Uint32*mmltuning;
static Uint32 mmltempo;

static Uint8*volatile wavesound;
static volatile Uint32 wavelen;
static volatile Uint8 mmlsound[512];
static volatile Uint16 mmlpos;
static volatile Uint32 mmltime;

static void audio_callback(void*userdata,Uint8*stream,int len) {
  static Uint32 phase;
  if(wavesound) {
    if(wavelen<=len) {
      memcpy(stream,wavesound,wavelen);
      memset(stream+wavelen,0,len-wavelen);
      wavesound=0;
      wavelen=0;
    } else {
      memcpy(stream,wavesound,len);
      wavesound+=len;
      wavelen-=len;
    }
  } else if(mmlpos) {
    Uint16*out=(Uint16*)stream;
    Uint32 t=mmltime;
    int re=len>>1;
    int m=mmlpos;
    int n=mmlsound[m-1];
    memset(stream,0,len);
    while(re) {
      if(n!=255) {
        phase+=mmltuning[n];
        if(t>1) *out=phase&0x80000000U?-mmlvolume:mmlvolume;
      }
      if(!--t) {
        m+=2;
        if(m>=512) {
          m=0;
          break;
        }
        n=mmlsound[m-1];
        t=mmltempo*mmlsound[m];
        if(!t) {
          m=0;
          break;
        }
      }
      out++; re--;
    }
    mmlpos=m;
    mmltime=t;
  } else {
    memset(stream,0,len);
  }
}

static int my_seek(SDL_RWops*cxt,int o,int w) {
  switch(w) {
    case RW_SEEK_SET: fseek(l_fp,l_offset+o,SEEK_SET); break;
    case RW_SEEK_CUR: fseek(l_fp,o,SEEK_CUR); break;
    case RW_SEEK_END: fseek(l_fp,l_offset+l_size+o,SEEK_SET); break;
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
    if(main_options['v']) fprintf(stderr,"[Cannot load wave audio at %ld (%ld bytes): %s]\n",offset,size,SDL_GetError());
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
  ws->len=cvt.len_cvt;
  amplify_wave_sound(ws);
  return;
  fail:
  if(main_options['v']) fprintf(stderr,"[Failed to convert wave audio at %ld (%ld bytes)]\n",offset,size);
  SDL_FreeWAV(buf);
}

static void load_sound_set(int is_user) {
  const char*v;
  char*nam;
  FILE*fp;
  Uint32 i,j;
  WaveSound*ws;
  if(is_user) {
    if(main_options['z']) {
      fp=composite_slice(".xclass",0);
    } else {
      nam=sqlite3_mprintf("%s.xclass",basefilename);
      if(!nam) return;
      fp=fopen(nam,"r");
      sqlite3_free(nam);
    }
    if(!fp) return;
  } else {
    optionquery[2]=Q_standardSounds;
    v=xrm_get_resource(resourcedb,optionquery,optionquery,3);
    if(!v) return;
    fp=fopen(v,"r");
    if(!fp) {
      fprintf(stderr,"Cannot open standard sounds file (%m)\n");
      return;
    }
    nstandardsounds=N_STANDARD_SOUNDS+N_MESSAGES;
    standardsounds=malloc(nstandardsounds*sizeof(WaveSound));
    if(!standardsounds) fatal("Allocation failed\n");
    for(i=0;i<nstandardsounds;i++) standardsounds[i].data=0,standardsounds[i].len=0;
  }
  nam=malloc(256);
  for(;;) {
    for(i=0;;) {
      if(i==255) goto done;
      nam[i++]=j=fgetc(fp);
      if(j==(Uint32)EOF) goto done;
      if(!j) break;
    }
    i--;
    j=fgetc(fp)<<16; j|=fgetc(fp)<<24; j|=fgetc(fp)<<0; j|=fgetc(fp)<<8;
    l_offset=ftell(fp); l_size=j;
    if(i>4 && nam[i-4]=='.' && nam[i-3]=='W' && nam[i-1]=='V' && (nam[i-2]=='A' || nam[i-2]=='Z')) {
      j=nam[i-2];
      nam[i-4]=0;
      if(is_user) {
        if(nusersounds>0x03FD) goto done;
        i=nusersounds++;
        usersounds=realloc(usersounds,nusersounds*sizeof(WaveSound));
        user_sound_names=realloc(user_sound_names,nusersounds*sizeof(Uint8*));
        if(!usersounds || !user_sound_names) fatal("Allocation failed\n");
        user_sound_names[i]=strdup(nam);
        if(!user_sound_names[i]) fatal("Allocation failed\n");
        ws=usersounds+i;
        ws->data=0;
        ws->len=0;
      } else {
        for(i=0;i<N_STANDARD_SOUNDS;i++) if(!sqlite3_stricmp(nam,standard_sound_names[i])) goto found;
        for(i=0;i<N_MESSAGES;i++) if(!sqlite3_stricmp(nam,standard_message_names[i])) {
          i+=N_STANDARD_SOUNDS;
          goto found;
        }
        goto notfound;
        found:
        ws=standardsounds+i;
        if(ws->data) goto notfound;
      }
      if(j=='A') {
        load_sound(fp,l_offset,l_size,ws);
      } else {
        //TODO: Compressed sounds.
      }
    }
    notfound:
    fseek(fp,l_offset+l_size,SEEK_SET);
  }
  done:
  fclose(fp);
  free(nam);
}

void init_sound(void) {
  const char*v;
  double f;
  int i;
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
  if(wavevolume>0.00001) {
    load_sound_set(0); // Standard sounds
    load_sound_set(1); // User sounds
  }
  if(mmlvolume) {
    mmltuning=malloc(256*sizeof(Uint32));
    if(!mmltuning) fatal("Allocation failed\n");
    optionquery[2]=Q_mmlTuning;
    if(v=xrm_get_resource(resourcedb,optionquery,optionquery,3)) f=strtod(v,0); else f=440.0;
    f*=0x80000000U/(double)spec.freq;
    for(i=0;i<190;i++) mmltuning[i]=f*pow(2.0,(i-96)/24.0);
    for(i=0;i<64;i++) mmltuning[i+190]=(((long long)(i+2))<<37)/spec.freq;
    optionquery[2]=Q_mmlTempo;
    if(v=xrm_get_resource(resourcedb,optionquery,optionquery,3)) i=strtol(v,0,10); else i=120;
    // Convert quarter notes per minute to samples per sixty-fourth note
    mmltempo=(spec.freq*60)/(i*16);
  }
  fprintf(stderr,"Done.\n");
  wavesound=0;
  mmlpos=0;
  SDL_PauseAudio(0);
  sound_on=1;
}

static void set_mml(const unsigned char*s) {
  const Sint8 y[8]={2,0,4,-18,-14,-10,-8,-4};
  int m=0;
  int o=96;
  int t=2;
  int n;
  while(*s && m<512) {
    switch(*s++) {
      case '0' ... '8': o=24*(s[-1]-'0'); break;
      case '-': if(o) o-=24; break;
      case '+': o+=24; break;
      case '.': t+=t/2; break;
      case 'A' ... 'G': case 'a' ... 'g':
        n=o+y[s[-1]&7];
        while(*s) switch(*s) {
          case '!': n-=2; s++; break;
          case '#': n+=2; s++; break;
          case ',': n-=1; s++; break;
          case '\'': n+=1; s++; break;
          default: goto send;
        }
        goto send;
      case 'H': case 'h': t=32; break;
      case 'I': case 'i': t=8; break;
      case 'L': case 'l':
        t=0;
        while(*s>='0' && *s<='9') t=10*t+*s++-'0';
        break;
      case 'N': case 'n':
        n=0;
        while(*s>='0' && *s<='9') n=10*n+*s++-'0';
        send:
        mmlsound[m++]=n; mmlsound[m++]=t;
        break;
      case 'Q': case 'q': t=16; break;
      case 'S': case 's': t=4; break;
      case 'T': case 't': t=2; break;
      case 'W': case 'w': t=64; break;
      case 'X': case 'x': mmlsound[m++]=255; mmlsound[m++]=t; break;
      case 'Z': case 'z': t=1; break;
    }
  }
  if(!m) return;
  if(m<511) mmlsound[m+1]=0;
  mmlpos=1;
  mmltime=mmlsound[1]*mmltempo;
}

void set_sound_effect(Value v1,Value v2) {
  static const unsigned char*const builtin[8]={
    "s.g",
    "scdefgab+c-bagfedc-c",
    "i1c+c+c+c+c+c+cx",
    "-cc'c#d,dd'd#ee'ff'f#",
    "sn190n191n192n193n194n195n196n197n198n199n200n201n202n203n204n205n206n207n208n209n210",
    "z+c-gec-gec",
    "t+c-gec",
    "tc-c+d-d+e-e+f-f+g-g",
  };
  const unsigned char*s;
  if(!sound_on) return;
  if(!v2.t && !v2.u && (mmlpos || wavesound)) return;
  SDL_LockAudio();
  wavesound=0;
  mmlpos=0;
  switch(v1.t) {
    case TY_MESSAGE:
      v1.u+=N_STANDARD_SOUNDS;
      //fallthrough
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
      if(s=value_string_ptr(v1)) set_mml(s);
      break;
    case TY_FOR:
      // (only used for the sound test)
      if(!mmlvolume) break;
      set_mml(builtin[v1.u&7]);
      break;
  }
  SDL_UnlockAudio();
}

Uint16 find_user_sound(const char*name) {
  int i;
  for(i=0;i<nusersounds;i++) if(!strcmp(name,user_sound_names[i])) return i+0x0400;
  return 0x03FF;
}

void set_sound_on(int on) {
  if(!sound_on) return;
  set_sound_effect(NVALUE(0),NVALUE(1));
  SDL_PauseAudio(!on);
}

void sound_test(void) {
  Uint8 columns;
  SDL_Event ev;
  SDL_Rect r;
  int scroll=0;
  int nitems;
  int scrmax;
  int i,j,k,x,y;
  Value v;
  char buf[256];
  if(main_options['T'] && main_options['v']) {
    if(mmltuning) printf("mmltempo=%d; mmlvolume=%d; mmltuning[96]=%d\n",(int)mmltempo,(int)mmlvolume,(int)mmltuning[96]);
    for(i=0;i<nusersounds;i++) printf("%d: %s (ptr=%p, len=%d bytes)\n",i,user_sound_names[i],usersounds[i].data,usersounds[i].len);
    fflush(stdout);
  }
  if(!screen) return;
  nitems=nstandardsounds+nusersounds+8;
  columns=(screen->w-16)/240?:1;
  scrmax=(nitems+columns-1)/columns;
  set_cursor(XC_arrow);
  redraw:
  SDL_FillRect(screen,0,0x02);
  r.x=r.y=0;
  r.w=screen->w;
  r.h=8;
  SDL_FillRect(screen,&r,0xF7);
  SDL_LockSurface(screen);
  draw_text(0,0,"  <F1> Mute  <F2> Stop  <F3> Status  <ESC> Cancel",0xF7,0xF0);
  if(SDL_GetAudioStatus()==SDL_AUDIO_PLAYING) draw_text(0,0,"\x0E",0xF7,0xF1);
  for(i=scroll*columns,x=0,y=8;i<nitems;i++) {
    if(y>screen->h-24) break;
    if(i<nstandardsounds) {
      k=standardsounds[i].data?0xF9:0xF8;
      snprintf(buf,29,"%s",i<N_STANDARD_SOUNDS?standard_sound_names[i]:standard_message_names[i-N_STANDARD_SOUNDS]); //TODO
    } else if(i<nstandardsounds+nusersounds) {
      k=0xFA;
      snprintf(buf,29,"%s",user_sound_names[i-nstandardsounds]);
    } else {
      k=0xFB;
      snprintf(buf,29,"TEST %d",i-nstandardsounds-nusersounds);
    }
    r.x=x*240+20; r.y=y+4;
    r.w=232; r.h=16;
    SDL_FillRect(screen,&r,k);
    draw_text(r.x+4,r.y+4,buf,k,0xF0);
    if(++x==columns) x=0,y+=24;
  }
  SDL_UnlockSurface(screen);
  r.x=r.w=0; r.y=8; r.h=screen->h-8;
  scrollbar(&scroll,screen->h/24-1,scrmax,0,&r);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE && scrollbar(&scroll,screen->h/24-1,scrmax,&ev,&r)) goto redraw;
    switch(ev.type) {
      case SDL_MOUSEMOTION:
        x=ev.button.x-16; y=ev.button.y-8;
        if(x<0 || y<0) goto arrow;
        i=x/240+columns*(y/24); x%=240; y%=24;
        if(x<4 || x>236 || y<4 || y>20 || i<0 || i>=nitems) goto arrow;
        set_cursor(XC_hand1);
        break;
        arrow: set_cursor(XC_arrow);
        break;
      case SDL_MOUSEBUTTONDOWN:
        x=ev.button.x-16; y=ev.button.y-8;
        if(x<0 || y<0) break;
        i=x/240+columns*(y/24)+scroll*columns; x%=240; y%=24;
        if(x<4 || x>236 || y<4 || y>20 || i<0 || i>=nitems) break;
        if(i<nstandardsounds) {
          if(i<N_STANDARD_SOUNDS) {
            v.t=TY_SOUND;
            v.u=i;
          } else {
            v.t=TY_MESSAGE;
            v.u=i-N_STANDARD_SOUNDS;
          }
        } else if(i<nstandardsounds+nusersounds) {
          v.t=TY_USOUND;
          v.u=i-nstandardsounds;
        } else {
          v.t=TY_FOR;
          v.u=i-nstandardsounds-nusersounds;
        }
        set_sound_effect(v,NVALUE(ev.button.button-1));
        break;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: return;
          case SDLK_F1: set_sound_on(SDL_GetAudioStatus()==SDL_AUDIO_PAUSED); goto redraw;
          case SDLK_F2: set_sound_effect(NVALUE(0),NVALUE(1)); break;
          case SDLK_F3:
            snprintf(buf,255,"Sample rate: %d Hz\nBuffer size: %d samples (%d bytes)\nStatus: %d\nWave queue: %d bytes\nMML position: %d"
             ,(int)spec.freq,(int)spec.samples,(int)spec.size,(int)SDL_GetAudioStatus(),(int)wavelen,(int)mmlpos);
            modal_draw_popup(buf);
            goto redraw;
          case SDLK_HOME: case SDLK_KP7: scroll=0; goto redraw;
          case SDLK_UP: case SDLK_KP8: if(scroll) --scroll; goto redraw;
          case SDLK_DOWN: case SDLK_KP2: ++scroll; goto redraw;
        }
        break;
      case SDL_VIDEOEXPOSE: goto redraw;
      case SDL_QUIT: SDL_PushEvent(&ev); return;
    }
  }
}
