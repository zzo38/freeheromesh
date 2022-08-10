#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-unused-result picture.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define _BSD_SOURCE
#include "SDL.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "pcfont.h"
#include "quarks.h"
#include "heromesh.h"
#include "cursorshapes.h"
#include "keyicons.xbm"

SDL_Surface*screen;
Uint16 picture_size;
int left_margin;
Uint32 codepage;

static SDL_Surface*picts;
static Uint8*curpic;
static const unsigned char*fontdata;
static const Uint8 bytewidth[32];

static const char default_palette[]=
  "C020FF "
  "000000 222222 333333 444444 555555 666666 777777 888888 999999 AAAAAA BBBBBB CCCCCC DDDDDD EEEEEE FFFFFF "
  "281400 412300 5F3200 842100 A05000 C35F14 E1731E FF8232 FF9141 FFA050 FFAF5F FFBE73 FFD282 FFE191 FFF0A0 "
  "321E1E 412220 5F2830 823040 A03A4C BE4658 E15464 FF6670 FF7F7B FF8E7F FF9F7F FFAF7F FFBF7F FFCF7F FFDF7F "
  "280D0D 401515 602020 802A2A A03535 C04040 E04A4A FF5555 FF6764 FF6F64 FF7584 FF849D FF94B7 FF9FD1 FFAEEA "
  "901400 A02000 B03000 C04000 D05000 E06000 F07000 FF8000 FF9000 FFA000 FFB000 FFC000 FFD000 FFE000 FFF000 "
  "280000 400000 600000 800000 A00000 C00000 E00000 FF0000 FF2828 FF4040 FF6060 FF8080 FFA0A0 FFC0C0 FFE0E0 "
  "280028 400040 600060 800080 A000A0 C000C0 E000E0 FF00FF FF28FF FF40FF FF60FF FF80FF FFA0FF FFC0FF FFE0FF "
  "281428 402040 603060 804080 A050A0 C060C0 E070E0 FF7CFF FF8CFF FF9CFF FFACFF FFBCFF FFCCFF FFDCFF FFECFF "
  "280050 350566 420A7C 4F0F92 5C14A8 6919BE 761ED4 8323EA 9028FF A040FF B060FF C080FF D0A0FF E0C0FF F0E0FF "
  "000028 000040 000060 000080 0000A0 0000C0 0000E0 0000FF 0A28FF 284AFF 466AFF 678AFF 87AAFF A7CAFF C7EBFF "
  "0F1E1E 142323 193232 1E4141 285050 325F5F 377373 418282 469191 50A0A0 5AAFAF 5FC3C3 69D2D2 73E1E1 78F0F0 "
  "002828 004040 006060 008080 00A0A0 00C0C0 00E0E0 00FFFF 28FFFF 40FFFF 60FFFF 80FFFF A0FFFF C0FFFF E0FFFF "
  "002800 004000 006000 008000 00A000 00C000 00E000 00FF00 28FF28 40FF40 60FF60 80FF80 A0FFA0 C0FFC0 E0FFE0 "
  "002110 234123 325F32 418241 50A050 5FC35F 73E173 85FF7A 91FF6E A0FF5F B4FF50 C3FF41 D2FF32 E1FF23 F0FF0F "
  "282800 404000 606000 808000 A0A000 C0C000 E0E000 FFFF00 FFFF28 FFFF40 FFFF60 FFFF80 FFFFA0 FFFFC0 FFFFE0 "
  //
  "442100 00FF55 0055FF FF5500 55FF00 FF0055 5500FF CA8B25 F078F0 F0F078 FF7F00 DD6D01 7AFF00 111111 "
  //
  "000000 0000AA 00AA00 00AAAA AA0000 AA00AA AAAA00 AAAAAA "
  "555555 5555FF 55FF55 55FFFF FF5555 FF55FF FFFF55 FFFFFF "
;

void init_palette(void) {
  double gamma;
  int usegamma=1;
  int i,j;
  SDL_Color pal[256];
  FILE*fp=0;
  const char*v;
  optionquery[1]=Q_gamma;
  gamma=strtod(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"0",0);
  if(gamma<=0.0 || gamma==1.0) usegamma=0;
  optionquery[1]=Q_palette;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(v && *v) {
    fp=fopen(v,"r");
    if(!fp) fatal("Unable to load palette file '%s'\n%m",v);
  }
  for(i=0;i<256;i++) {
    if(fp) {
      if(fscanf(fp,"%2hhX%2hhX%2hhX ",&pal[i].r,&pal[i].g,&pal[i].b)!=3) fatal("Invalid palette file\n");
    } else {
      sscanf(default_palette+i*7,"%2hhX%2hhX%2hhX ",&pal[i].r,&pal[i].g,&pal[i].b);
    }
    if(usegamma) {
      j=(int)(255.0*pow(pal[i].r/255.0,gamma)+0.2); pal[i].r=j<0?0:j>255?255:j;
      j=(int)(255.0*pow(pal[i].g/255.0,gamma)+0.2); pal[i].g=j<0?0:j>255?255:j;
      j=(int)(255.0*pow(pal[i].b/255.0,gamma)+0.2); pal[i].b=j<0?0:j>255?255:j;
    }
  }
  if(fp) fclose(fp);
  SDL_SetColors(screen,pal,0,256);
  SDL_SetColors(picts,pal,0,256);
}

void draw_picture(int x,int y,Uint16 img) {
  // To be called only when screen is unlocked!
  SDL_Rect src={(img&15)*picture_size,(img>>4)*picture_size,picture_size,picture_size};
  SDL_Rect dst={x,y,picture_size,picture_size};
  SDL_BlitSurface(picts,&src,screen,&dst);
}

void draw_cell(int x,int y) {
  // To be called only when screen is unlocked!
  Uint32 o;
  Class*c;
  int i;
  SDL_Rect dst={(x-1)*picture_size+left_margin,(y-1)*picture_size,picture_size,picture_size};
  if(x<1 || x>64 || y<1 || y>64) return;
  SDL_FillRect(screen,&dst,back_color);
  o=playfield[y*64+x-65];
  while(o!=VOIDLINK) {
    if(main_options['e'] || !(objects[o]->oflags&OF_INVISIBLE)) {
      c=classes[objects[o]->class];
      if(objects[o]->anim && (objects[o]->anim->status&ANISTAT_VISUAL)) i=objects[o]->anim->vimage; else i=objects[o]->image;
      if(i<c->nimages) draw_picture((x-1)*picture_size+left_margin,(y-1)*picture_size,c->images[i]&0x7FFF);
    }
    o=objects[o]->up;
  }
}

void draw_selection_rectangle(void) {
  Uint16 pitch=screen->pitch;
  Uint8*p=screen->pixels+left_margin+(editrect.x0-1)*picture_size+pitch*(editrect.y0-1)*picture_size;
  int xr=(editrect.x1+1-editrect.x0)*picture_size-1;
  int yr=(editrect.y1+1-editrect.y0)*picture_size-1;
  int i;
  if(p+xr+yr*pitch>=((Uint8*)screen->pixels)+screen->h*pitch+screen->w) return;
  memset(p,0xF7,xr);
  memset(p+yr*pitch,0xF7,xr);
  for(i=1;i<yr;i++) {
    p[i*pitch]=p[i*pitch+xr]=0xF7;
    p[i*pitch+1]=p[i*pitch+xr-1]=0xF0;
  }
  memset(p+pitch+1,0xF0,xr-2);
  memset(p+(yr-1)*pitch+1,0xF0,xr-2);
  p[0]=p[xr]=p[yr*pitch]=p[yr*pitch+xr]=0xF8;
}

void draw_text(int x,int y,const unsigned char*t,int bg,int fg) {
  // To be called only when screen is locked!
  int len=strlen(t);
  Uint8*pix=screen->pixels;
  Uint8*p;
  Uint16 pitch=screen->pitch;
  int xx,yy;
  const unsigned char*f;
  if(x+8*len>screen->w) len=(screen->w-x)>>3;
  if(len<=0 || y+8>screen->h) return;
  pix+=y*pitch+x;
  while(*t) {
    f=fontdata+(*t<<3);
    p=pix;
    for(yy=0;yy<8;yy++) {
      for(xx=0;xx<8;xx++) p[xx]=(*f<<xx)&128?fg:bg;
      p+=pitch;
      ++f;
    }
    t++;
    if(!--len) return;
    pix+=8;
  }
}

void draw_text_v(int x,int y,const unsigned char*t,int bg,int fg) {
  // Draw text using sizes other than 8x8 and possible multibyte encodings.
  // To be called only when screen is locked!
  //TODO
}

int measure_text_v(const unsigned char*t,int len) {
  // Returns number of character cells of text (t).
  // If len is positive then it is the maximum number of bytes to measure.
  int r=0;
  int c;
  while((len<0 || len--) && (c=*t++)) {
    r++;
    if(c&0x80) r-=(bytewidth[(c&0x7C)>>2]>>((c&3)<<1))&3;
  }
  return r>0?r:0;
}

int draw_text_line(int x,int y,unsigned char*t,int cur,Uint8**cp) {
  // To be called only when screen is locked!
  int len=strlen(t);
  const unsigned char*s=t;
  Uint8*pix=screen->pixels;
  Uint8*p;
  Uint16 pitch=screen->pitch;
  int bg,fg,xx,yy;
  char isimg=0;
  char e=0;
  const unsigned char*f;
  if(!*t) return 0;
  len=(screen->w-x)>>3;
  if(len<=0 || y+8>screen->h) return len<1?1:len;
  pix+=y*pitch+x;
  for(;;) {
    if(!cur) *cp=t;
    if(*t==10) {
      f=fontdata+(17<<3);
      bg=0xF0,fg=0xF1;
      e=1;
    } else if(!*t) {
      f=fontdata+(254<<3);
      bg=0xF0,fg=0xF1;
      e=1;
    } else if(*t<31) {
      f=fontdata+(" 01234567?NLC#IBQ?????????????D?"[*t]<<3);
      if(*t==14 || *t==30) isimg=1;
      bg=0xF3,fg=0xF0;
    } else {
      if(*t==31 && t[1]) t++;
      f=fontdata+(*t<<3);
      bg=0xF0,fg=isimg?0xF2:0xF7;
    }
    if(!cur--) bg^=15,fg^=15;
    p=pix;
    for(yy=0;yy<8;yy++) {
      for(xx=0;xx<8;xx++) p[xx]=(*f<<xx)&128?fg:bg;
      p+=pitch;
      ++f;
    }
    if(*t=='\\') isimg=0;
    if(!--len || e) return t-s;
    t++;
    pix+=8;
  }
}

int draw_text_line_v(int x,int y,unsigned char*t,int cur,Uint8**cp) {
  //TODO
}

int measure_text_line_v(unsigned char*t,int len,Uint8**cp) {
  //TODO
}

void draw_key(int x,int y,int k,int bg,int fg) {
  // To be called only when screen is locked!
  Uint8*p=screen->pixels;
  Uint16 pitch=screen->pitch;
  int xx,yy;
  const unsigned char*f;
  if(x<0 || y<0 || x+16>screen->w || y+16>screen->h) return;
  p+=y*pitch+x;
  f=keyicons_bits+(k<<5);
  for(yy=0;yy<16;yy++) {
    for(xx=0;xx<8;xx++) p[xx]=(*f>>xx)&1?fg:bg;
    ++f;
    for(xx=0;xx<8;xx++) p[xx+8]=(*f>>xx)&1?fg:bg;
    p+=pitch;
    ++f;
  }
}

const char*screen_prompt(const char*txt) {
  static char*t=0;
  int n=0;
  SDL_Rect r={0,0,screen->w,16};
  int m=r.w>>3;
  SDL_Event ev;
  if(!t) {
    t=malloc(m+2);
    if(!t) fatal("Allocation failed\n");
  }
  *t=0;
  SDL_FillRect(screen,&r,0xF1);
  r.y=16;
  r.h=1;
  SDL_FillRect(screen,&r,0xF8);
  SDL_LockSurface(screen);
  draw_text(0,0,txt,0xF1,0xFE);
  draw_text(0,8,"\xB1",0xF1,0xFB);
  SDL_UnlockSurface(screen);
  set_cursor(XC_iron_cross);
  SDL_Flip(screen);
  r.y=8;
  r.h=8;
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        SDL_PushEvent(&ev);
        return 0;
      case SDL_KEYDOWN:
        SDL_FillRect(screen,&r,0xF1);
        switch(ev.key.keysym.sym) {
          case SDLK_RETURN: case SDLK_KP_ENTER:
            r.y=0;
            r.h=17;
            SDL_FillRect(screen,&r,0xF0);
            t[n]=0;
            return t;
          case SDLK_BACKSPACE: case SDLK_DELETE:
            if(n) t[n--]=0;
            break;
          case SDLK_CLEAR:
            t[n=0]=0;
            break;
          case SDLK_INSERT:
            if(ev.key.keysym.mod&KMOD_SHIFT) {
              const char*s;
              FILE*fp;
              int c;
              paste:
              optionquery[1]=Q_pasteCommand;
              if((s=xrm_get_resource(resourcedb,optionquery,optionquery,2)) && (fp=popen(s,"r"))) {
                for(;;) {
                  c=fgetc(fp);
                  if(c=='\t') c=' ';
                  if(c>=32 && n<m) t[n++]=c;
                  if(c=='\n' || c<0 || n>=m) break;
                }
                t[n]=0;
                pclose(fp);
              }
            }
            break;
          default:
            if(ev.key.keysym.sym==SDLK_u && (ev.key.keysym.mod&KMOD_CTRL)) {
              t[n=0]=0;
            } else if(ev.key.keysym.sym==SDLK_c && (ev.key.keysym.mod&KMOD_CTRL)) {
              r.y=0;
              r.h=17;
              SDL_FillRect(screen,&r,0xF0);
              return 0;
            } else if(n<m && ev.key.keysym.unicode<127 && ev.key.keysym.unicode>=32 && !(ev.key.keysym.mod&KMOD_CTRL)) {
              t[n++]=ev.key.keysym.unicode;
              t[n]=0;
            }
        }
        t[n]=0;
        SDL_FillRect(screen,&r,0xF1);
        SDL_LockSurface(screen);
        draw_text(0,8,t,0xF1,0xFF);
        draw_text(n<<3,8,"\xB1",0xF1,0xFB);
        SDL_UnlockSurface(screen);
        SDL_Flip(screen);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.button==2) goto paste;
        break;
    }
  }
  return 0;
}

int screen_message(const char*txt) {
  int n=0;
  SDL_Rect r={0,0,0,16};
  SDL_Event ev;
  if(!screen) {
    fprintf(stderr," * %s\n",txt);
    return 0;
  }
  r.w=screen->w;
  SDL_FillRect(screen,&r,0xF4);
  r.y=16;
  r.h=1;
  SDL_FillRect(screen,&r,0xF8);
  SDL_LockSurface(screen);
  draw_text(0,0,txt,0xF4,0xFE);
  draw_text(0,8,"<Push ENTER to continue>",0xF4,0xF7);
  SDL_UnlockSurface(screen);
  set_cursor(XC_iron_cross);
  SDL_Flip(screen);
  r.y=8;
  r.h=8;
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        SDL_PushEvent(&ev);
        return -1;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_RETURN: case SDLK_KP_ENTER:
            r.y=0;
            r.h=17;
            SDL_FillRect(screen,&r,0xF0);
            return 0;
        }
        break;
    }
  }
  return -1;
}

static Uint16 decide_picture_size(int nwantsize,const Uint8*wantsize,const Uint16*havesize,int n) {
  int i,j;
  if(!nwantsize) fatal("Unable to determine what picture size is wanted\n");
  for(i=0;i<nwantsize;i++) if(havesize[j=wantsize[i]]==n) return j;
  for(i=*wantsize;i;--i) if(havesize[i]) return i;
  fatal("Unable to determine what picture size is wanted\n");
}

static void load_one_picture_sub(FILE*fp,int size,int meth) {
  Uint8*p=curpic;
  int c,n,t,x,y;
  if(meth==15) {
    fread(p,size,size,fp);
    return;
  }
  n=t=0;
  y=size*size;
  while(y--) {
    if(!n) {
      n=fgetc(fp);
      if(n<85) {
        // Homogeneous run
        n++;
        x=fgetc(fp);
        if(t==1 && x==c) n*=85; else n++;
        c=x;
        t=1;
      } else if(n<170) {
        // Heterogeneous run
        n-=84;
        t=2;
      } else {
        // Copy-above run
        n-=169;
        if(t==3) n*=85;
        t=3;
      }
    }
    n--;
    if(t==2) c=fgetc(fp);
    if(t==3) c=p-curpic>=size?p[-size]:0;
    *p++=c;
  }
}

static void load_one_picture(FILE*fp,Uint16 img,int alt) {
  int h,i,j,k,pitch,which,meth,size,psize,zoom;
  Uint8 buf[32];
  Uint8*pix;
  *buf=fgetc(fp);
  j=*buf&15;
  fread(buf+1,1,j+(j>>1),fp);
  k=0;
  zoom=1;
  for(i=1;i<=j;i++) if(buf[i]==picture_size) ++k;
  if(k) {
    psize=picture_size;
  } else {
    for(zoom=2;zoom<=picture_size;zoom++) if(!(picture_size%zoom)) {
      psize=picture_size/zoom;
      for(i=1;i<=j;i++) if(buf[i]==psize) ++k;
      if(k) break;
    }
  }
  alt%=k;
  for(i=1;i<=j;i++) if(buf[i]==psize && !alt--) break;
  which=i;
  i=1;
  while(which--) load_one_picture_sub(fp,size=buf[i],meth=(i==1?*buf>>4:buf[(*buf&15)+1+((i-2)>>1)]>>(i&1?4:0))&15),i++;
  if(meth==5 || meth==6) meth^=3;
  if(meth==15) meth=0;
  SDL_LockSurface(picts);
  pitch=picts->pitch;
  pix=picts->pixels+((img&15)+pitch*(img>>4))*picture_size;
  for(i=0;i<size;i++) {
    for(h=0;h<zoom;h++) {
      for(j=0;j<size;j++) {
        if(meth&1) j=size-j-1;
        if(meth&2) i=size-i-1;
        for(k=0;k<zoom;k++) *pix++=curpic[meth&4?j*size+i:i*size+j];
        if(meth&1) j=size-j-1;
        if(meth&2) i=size-i-1;
      }
      pix+=pitch-picture_size;
    }
  }
  SDL_UnlockSurface(picts);
}

static void pic_shift_right(SDL_Rect*r,int sh) {
  Uint8*p;
  int y;
  char buf[256];
  sh%=r->w;
  if(!sh) return;
  SDL_LockSurface(picts);
  p=picts->pixels+r->y*picts->pitch+r->x;
  for(y=0;y<r->h;y++) {
    memcpy(buf,p,r->w);
    memcpy(p,buf+r->w-sh,sh);
    memcpy(p+sh,buf,r->w-sh);
    p+=picts->pitch;
  }
  SDL_UnlockSurface(picts);
}

static void pic_shift_down(SDL_Rect*r,int sh) {
  Uint8*p;
  int y;
  char b1[256];
  char b2[256];
  sh%=r->h;
  SDL_LockSurface(picts);
  // This implementation is probably inefficient.
  while(sh--) {
    p=picts->pixels+r->y*picts->pitch+r->x;
    memcpy(b1,p+picts->pitch*(r->h-1),r->w);
    for(y=0;y<r->h;y++) {
      memcpy(b2,p,r->w);
      memcpy(p,b1,r->w);
      p+=picts->pitch;
      memcpy(b1,b2,r->w);
    }
  }
  SDL_UnlockSurface(picts);
}

static void pic_orientation(SDL_Rect*r,Uint8 m) {
  Uint8*d=malloc(r->w*r->h);
  Uint8*p;
  Uint8*q;
  int i,j;
  if(!d) fatal("Allocation failed\n");
  SDL_LockSurface(picts);
  p=picts->pixels+r->y*picts->pitch+r->x;
  q=d;
  for(i=0;i<r->h;i++) {
    for(j=0;j<r->w;j++) {
      if(m&1) j=r->w-j-1;
      if(m&2) i=r->h-i-1;
      *q++=p[m&4?j*picts->pitch+i:i*picts->pitch+j];
      if(m&1) j=r->w-j-1;
      if(m&2) i=r->h-i-1;
    }
  }
  q=d;
  for(i=0;i<r->h;i++) {
    memcpy(p,q,r->w);
    p+=picts->pitch;
    q+=r->w;
  }
  SDL_UnlockSurface(picts);
  free(d);
}

static void load_dependent_picture(FILE*fp,Sint32 sz,Uint16 img,int alt) {
  SDL_Rect src={0,0,picture_size,picture_size};
  SDL_Rect dst={(img&15)*picture_size,(img>>4)*picture_size,picture_size,picture_size};
  sqlite3_stmt*st;
  int c,i,x,y;
  char nam[128];
  Uint8 buf[512];
  Uint8*p;
  if(sqlite3_prepare_v2(userdb,"SELECT `ID` FROM `PICTURES` WHERE `NAME` = ?1 AND NOT `DEPENDENT`;",-1,&st,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  i=0;
  while(sz) {
    c=fgetc(fp);
    if(c<32) break;
    if(i<127) nam[i++]=c;
    sz--;
  }
  if(sz) ungetc(c,fp);
  if(i) {
    sqlite3_bind_text(st,1,nam,i,0);
    i=sqlite3_step(st);
    if(i==SQLITE_DONE) {
      fprintf(stderr,"Cannot find base picture for a dependent picture; ignoring\n");
      sqlite3_finalize(st);
      return;
    } else if(i!=SQLITE_ROW) {
      fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
    }
    i=sqlite3_column_int(st,0);
    sqlite3_reset(st);
    src.x=(i&15)*picture_size;
    src.y=(i>>4)*picture_size;
    SDL_SetColorKey(picts,0,0);
    SDL_BlitSurface(picts,&src,picts,&dst);
  } else {
    SDL_FillRect(picts,&dst,0);
  }
  while(sz-->0) switch(c=fgetc(fp)) {
    case 0 ... 7: // Flip/rotate
      pic_orientation(&dst,c);
      break;
    case 8: // *->* *->* *->*
      c=fgetc(fp);
      sz-=c+1;
      fread(buf,1,c&255,fp);
      SDL_LockSurface(picts);
      p=picts->pixels+((img&15)+picts->pitch*(img>>4))*picture_size;
      for(y=0;y<picture_size;y++) {
        for(x=0;x<picture_size;x++) {
          for(i=0;i<c;i+=2) {
            if(p[x]==buf[i]) {
              p[x]=buf[i+1];
              break;
            }
          }
        }
        p+=picts->pitch;
      }
      SDL_UnlockSurface(picts);
      break;
    case 9: // *->*->*->*->*
      c=fgetc(fp);
      sz-=c+1;
      fread(buf,1,c&255,fp);
      SDL_LockSurface(picts);
      p=picts->pixels+((img&15)+picts->pitch*(img>>4))*picture_size;
      for(y=0;y<picture_size;y++) {
        for(x=0;x<picture_size;x++) {
          for(i=0;i<c-1;i++) {
            if(p[x]==buf[i]) {
              p[x]=buf[i+1];
              break;
            }
          }
        }
        p+=picts->pitch;
      }
      SDL_UnlockSurface(picts);
      break;
    case 10: // *<->* *<->* *<->*
      c=fgetc(fp);
      sz-=c+1;
      fread(buf,1,c&255,fp);
      SDL_LockSurface(picts);
      p=picts->pixels+((img&15)+picts->pitch*(img>>4))*picture_size;
      for(y=0;y<picture_size;y++) {
        for(x=0;x<picture_size;x++) {
          for(i=0;i<c;i+=2) {
            if(p[x]==buf[i]) {
              p[x]=buf[i+1];
              break;
            } else if(p[x]==buf[i+1]) {
              p[x]=buf[i];
              break;
            }
          }
        }
        p+=picts->pitch;
      }
      SDL_UnlockSurface(picts);
      break;
    case 11: // Overlay
      SDL_SetColorKey(picts,SDL_SRCCOLORKEY,0);
      i=0;
      while(sz>0) {
        c=fgetc(fp);
        if(c<32) break;
        if(i<127) nam[i++]=c;
        sz--;
      }
      if(sz) ungetc(c,fp);
      sqlite3_bind_text(st,1,nam,i,0);
      i=sqlite3_step(st);
      if(i==SQLITE_DONE) {
        fprintf(stderr,"Cannot find overlay for a dependent picture; ignoring\n");
        break;
      } else if(i!=SQLITE_ROW) {
        fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
      }
      i=sqlite3_column_int(st,0);
      sqlite3_reset(st);
      src.x=(i&15)*picture_size;
      src.y=(i>>4)*picture_size;
      SDL_BlitSurface(picts,&src,picts,&dst);
      break;
    case 12 ... 15: // Shift (up/down/right/left)
      while(sz>0) {
        x=fgetc(fp);
        y=fgetc(fp);
        sz-=2;
        if(y && !(picture_size%y)) {
          if(c==12) pic_shift_down(&dst,picture_size-((x&127)*picture_size)/y);
          if(c==13) pic_shift_down(&dst,((x&127)*picture_size)/y);
          if(c==14) pic_shift_right(&dst,((x&127)*picture_size)/y);
          if(c==15) pic_shift_right(&dst,picture_size-((x&127)*picture_size)/y);
          break;
        }
        if(x&128) break;
      }
      while(sz>0 && x<128) {
        x=fgetc(fp);
        fgetc(fp);
        sz-=2;
      }
      break;
    default:
      fprintf(stderr,"Unrecognized command in dependent picture (%d)\n",c);
      goto done;
  }
  if(sz<-1) fprintf(stderr,"Lump size of dependent picture is too short\n");
  done: sqlite3_finalize(st);
}

static int find_multidependent(sqlite3_stmt*st,FILE*fp,int len,int npic) {
  sqlite3_stmt*s1;
  int at=4;
  long rew=ftell(fp);
  Uint8*mem=malloc(len+1);
  int i,j,zt;
  if(!mem) fatal("Allocation failed\n");
  fread(mem,1,len,fp);
  mem[len]=0;
  fseek(fp,rew+len,SEEK_SET);
  if(sqlite3_exec(userdb,"CREATE TEMPORARY TABLE IF NOT EXISTS `_MUL`(`X`,`Y`,`N`,`Z`);",0,0,0))
   fatal("SQL error: %s\n",sqlite3_errmsg(userdb));
  if(sqlite3_prepare_v2(userdb,"INSERT INTO `_MUL`(`X`,`Y`,`N`,`Z`) VALUES(?1,?2,SUBSTR('._-',?5,1)||?3,?4);",-1,&s1,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  sqlite3_bind_int(s1,2,0);
  sqlite3_bind_text(s1,3,"",0,0);
  sqlite3_bind_int(s1,5,4);
  for(i=0;i<4;i++) if((mem[i]&128) || !mem[i]) {
    sqlite3_reset(s1);
    sqlite3_bind_int(s1,1,i);
    sqlite3_bind_blob(s1,4,"",i?0:1,0);
    while(sqlite3_step(s1)==SQLITE_ROW);
  }
  sqlite3_reset(s1);
  sqlite3_bind_int(s1,1,0);
  for(i=0;i<(*mem&63);i++) {
    if(at>=len) fatal("Malformed multidependent picture lump\n");
    sqlite3_reset(s1);
    sqlite3_bind_int(s1,2,i+1);
    j=strlen(mem+at);
    sqlite3_bind_text(s1,3,mem+at,j,0);
    sqlite3_bind_blob(s1,4,mem+at,j+1,0);
    at+=j+1;
    while(sqlite3_step(s1)==SQLITE_ROW);
  }
  zt=at;
  for(j=1;j<3;j++) for(i=0;i<(mem[j]&63);i++) {
    if(zt+2>=len) fatal("Malformed multidependent picture lump\n");
    zt+=((mem[zt+1]>>3)&7)+2;
  }
  for(j=1;j<3;j++) for(i=0;i<(mem[j]&63);i++) {
    sqlite3_reset(s1);
    sqlite3_bind_int(s1,1,j);
    sqlite3_bind_int(s1,2,i+1);
    sqlite3_bind_text(s1,3,mem+at+2,(mem[at+1]>>3)&7,0);
    if(zt+mem[at]+((mem[at+1]&7)<<8)>len) fatal("Malformed multidependent picture lump\n");
    sqlite3_bind_blob(s1,4,mem+zt,mem[at]|((mem[at+1]&7)<<8),0);
    sqlite3_bind_int(s1,5,4-(mem[at+1]>>6));
    zt+=mem[at]|((mem[at+1]&7)<<8);
    at+=((mem[at+1]>>3)&7)+2;
    while(sqlite3_step(s1)==SQLITE_ROW);
  }
  sqlite3_finalize(s1);
  free(mem);
  if(sqlite3_prepare_v2(userdb,"SELECT A.N||B.N||C.N||D.N,BCAT(A.Z,B.Z,C.Z,D.Z) FROM _MUL A,_MUL B,_MUL C,_MUL D WHERE A.X=0 AND B.X=1 AND C.X=2 AND D.X=3 AND B.Y+C.Y+D.Y<>0;",-1,&s1,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  while((j=sqlite3_step(s1))==SQLITE_ROW) {
    if(npic++==32768) fatal("Too many pictures\n");
    sqlite3_reset(st);
    sqlite3_bind_int(st,1,npic);
    sqlite3_bind_value(st,2,sqlite3_column_value(s1,0));
    sqlite3_bind_value(st,5,sqlite3_column_value(s1,1));
    while(sqlite3_step(st)==SQLITE_ROW);
  }
  if(j!=SQLITE_DONE) fatal("SQL error (%d): %s\n",j,sqlite3_errmsg(userdb));
  sqlite3_finalize(s1);
  if(sqlite3_exec(userdb,"DELETE FROM `_MUL`;",0,0,0))
   fatal("SQL error: %s\n",sqlite3_errmsg(userdb));
  return npic;
}

static void load_multidependent(int n,const char*data,int len) {
  FILE*fp=fmemopen((char*)data,len,"r");
  if(!fp) fatal("Allocation failed\n");
  load_dependent_picture(fp,len,n,0);
  fclose(fp);
}

void load_pictures(void) {
  sqlite3_stmt*st=0;
  FILE*fp;
  Uint8 wantsize[32];
  Uint8 nwantsize=0;
  Uint8 altImage;
  Uint8 havesize1[256];
  Uint16 havesize[256];
  char*nam=sqlite3_mprintf("%s.xclass",basefilename);
  const char*v;
  int i,j,n;
  if(!nam) fatal("Allocation failed\n");
  printStatus("Loading pictures...\n");
  fp=main_options['z']?composite_slice(".xclass",1):fopen(nam,"r");
  if(!fp) fatal("Failed to open xclass file (%m)\n");
  sqlite3_free(nam);
  optionquery[1]=Q_altImage;
  altImage=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"0",0,10);
  optionquery[1]=Q_imageSize;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(v) while(nwantsize<32) {
    i=j=0;
    sscanf(v," %d %n",&i,&j);
    if(!j) break;
    if(i<2 || i>255) fatal("Invalid picture size %d\n",i);
    wantsize[nwantsize++]=i;
    v+=j;
  }
  if(n=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
  if(sqlite3_prepare_v2(userdb,"INSERT INTO `PICTURES`(`ID`,`NAME`,`OFFSET`,`DEPENDENT`,`MISC`) VALUES(?1,?2,?3,?4,?5);",-1,&st,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  nam=malloc(256);
  if(!nam) fatal("Allocation failed\n");
  n=0;
  memset(havesize,0,256*sizeof(Uint16));
  while(!feof(fp)) {
    i=0;
    while(j=fgetc(fp)) {
      if(j==EOF) goto nomore1;
      if(i<255) nam[i++]=j;
    }
    nam[i]=0;
    if(i>4 && (!memcmp(".IMG",nam+i-4,4) || !memcmp(".DEP",nam+i-4,4))) {
      if(nam[i-3]=='I') j=1; else j=0;
      if(n++==32768) fatal("Too many pictures\n");
      sqlite3_reset(st);
      sqlite3_bind_int(st,1,n);
      sqlite3_bind_text(st,2,nam,i-4,SQLITE_TRANSIENT);
      sqlite3_bind_int64(st,3,ftell(fp)+4);
      sqlite3_bind_int(st,4,j^1);
      sqlite3_bind_null(st,5);
      while((i=sqlite3_step(st))==SQLITE_ROW);
      if(i!=SQLITE_DONE) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
    } else if(i>4 && !memcmp(".MUL",nam+i-4,4)) {
      j=2;
    } else {
      j=0;
    }
    i=fgetc(fp)<<16;
    i|=fgetc(fp)<<24;
    i|=fgetc(fp)<<0;
    i|=fgetc(fp)<<8;
    if(j==1 && i>1) {
      memset(havesize1,0,256);
      i-=j=fgetc(fp)&15;
      while(j--) havesize1[fgetc(fp)&255]=1;
      fseek(fp,i-1,SEEK_CUR);
      for(i=1;i<256;i++) if(havesize1[i]) for(j=i+i;j<256;j+=i) havesize1[j]=1;
      for(j=1;j<256;j++) havesize[j]+=havesize1[j];
    } else if(j==2 && i>4) {
      sqlite3_reset(st);
      sqlite3_bind_int64(st,3,ftell(fp));
      sqlite3_bind_int(st,4,1);
      n=find_multidependent(st,fp,i,n);
    } else {
      fseek(fp,i,SEEK_CUR);
    }
  }
nomore1:
  if(!n) fatal("Cannot find any pictures in this puzzle set\n");
  free(nam);
  sqlite3_finalize(st);
  rewind(fp);
  for(i=255;i;--i) if(havesize[i]) {
    curpic=malloc(i*i);
    break;
  }
  if(!curpic) fatal("Allocation failed\n");
  picture_size=decide_picture_size(nwantsize,wantsize,havesize,n);
  if(main_options['x']) goto done;
  if(sqlite3_prepare_v2(userdb,"SELECT `ID`, `OFFSET` FROM `PICTURES` WHERE NOT `DEPENDENT`;",-1,&st,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  optionquery[1]=Q_screenFlags;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  i=v&&strchr(v,'h');
  picts=SDL_CreateRGBSurface((i?SDL_HWSURFACE:SDL_SWSURFACE)|SDL_SRCCOLORKEY,picture_size<<4,picture_size*((n+16)>>4),8,0,0,0,0);
  if(!picts) fatal("Error allocating surface for pictures: %s\n",SDL_GetError());
  init_palette();
  for(i=0;i<n;i++) {
    if((j=sqlite3_step(st))!=SQLITE_ROW) {
      if(j==SQLITE_DONE) break;
      fatal("SQL error (%d): %s\n",j,sqlite3_errmsg(userdb));
    }
    fseek(fp,sqlite3_column_int64(st,1),SEEK_SET);
    load_one_picture(fp,sqlite3_column_int(st,0),altImage);
  }
  sqlite3_finalize(st);
  if(sqlite3_prepare_v2(userdb,"SELECT `ID`, `OFFSET`, `MISC` FROM `PICTURES` WHERE `DEPENDENT`;",-1,&st,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  for(i=0;i<n;i++) {
    if((j=sqlite3_step(st))!=SQLITE_ROW) {
      if(j==SQLITE_DONE) break;
      fatal("SQL error (%d): %s\n",j,sqlite3_errmsg(userdb));
    }
    if(sqlite3_column_type(st,2)==SQLITE_NULL) {
      fseek(fp,sqlite3_column_int64(st,1)-4,SEEK_SET);
      j=fgetc(fp)<<16;
      j|=fgetc(fp)<<24;
      j|=fgetc(fp);
      j|=fgetc(fp)<<8;
      load_dependent_picture(fp,j,sqlite3_column_int(st,0),0);
    } else {
      v=sqlite3_column_blob(st,2);
      j=sqlite3_column_bytes(st,2);
      load_multidependent(sqlite3_column_int(st,0),v,j);
    }
  }
  sqlite3_finalize(st);
  fclose(fp);
  SDL_SetColorKey(picts,SDL_SRCCOLORKEY|SDL_RLEACCEL,0);
done:
  if(n=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
  printStatus("Done\n");
}

void init_screen(void) {
  const char*v;
  int w,h,i;
  if(main_options['x']) return;
  if(!fontdata) fontdata=pcfont;
  optionquery[1]=Q_screenWidth;
  w=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"800",0,10);
  optionquery[1]=Q_screenHeight;
  h=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"600",0,10);
  optionquery[1]=Q_screenFlags;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  if(SDL_Init(SDL_INIT_VIDEO|(strchr(v,'z')?SDL_INIT_NOPARACHUTE:0)|SDL_INIT_TIMER)) fatal("Error initializing SDL: %s\n",SDL_GetError());
  atexit(SDL_Quit);
  i=0;
  while(*v) switch(*v++) {
    case 'd': i|=SDL_DOUBLEBUF; break;
    case 'f': i|=SDL_FULLSCREEN; break;
    case 'h': i|=SDL_HWSURFACE; break;
    case 'n': i|=SDL_NOFRAME; break;
    case 'p': i|=SDL_HWPALETTE; break;
    case 'r': i|=SDL_RESIZABLE; break;
    case 'y': i|=SDL_ASYNCBLIT; break;
  }
  if(!(i&SDL_HWSURFACE)) i|=SDL_SWSURFACE;
  screen=SDL_SetVideoMode(w,h,8,i);
  if(!screen) fatal("Failed to initialize screen mode: %s\n",SDL_GetError());
  optionquery[1]=Q_keyRepeat;
  if(v=xrm_get_resource(resourcedb,optionquery,optionquery,2)) {
    w=strtol(v,(void*)&v,10);
    h=strtol(v,0,10);
    SDL_EnableKeyRepeat(w,h);
  } else {
    SDL_EnableKeyRepeat(SDL_DEFAULT_REPEAT_DELAY,SDL_DEFAULT_REPEAT_INTERVAL);
  }
  SDL_EnableUNICODE(1);
  optionquery[1]=Q_margin;
  left_margin=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"65",0,10);
}

void set_code_page(Uint32 n) {
  int c,i,j,s;
  const char*v;
  unsigned char*d;
  Uint8 b[32];
  FILE*fp;
  if(!n) return;
  if(n==367) n=437;
  if(fontdata && fontdata!=pcfont) fatal("Multiple code page specifications\n");
  optionquery[1]=Q_codepage;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(!v || !*v) {
    if(n==437) return;
    fatal("Cannot load code page %d; code page file is not configured\n",n);
  }
  fp=fopen(v,"r");
  if(!fp) {
    perror(0);
    fatal("Cannot open code page file\n");
  }
  fontdata=d=malloc(0x800);
  if(!d) fatal("Allocation failed\n");
  memcpy(d,pcfont,0x800);
  name:
  s=i=0;
  for(;;) {
    c=fgetc(fp);
    if(c<0) {
      if(n!=437) fatal("Cannot find code page %d\n",n);
      goto done;
    }
    if(!c) break;
    if(!s) {
      if(c<'0' || c>'9') s=1;
      else if(c=='0' && !i) s=1;
      else i=10*i+c-'0';
      if(i>0x7FFFFF) s=1;
    }
  }
  if(s || i!=n) goto skip;
  i=fgetc(fp)<<16;
  i|=fgetc(fp)<<24;
  i|=fgetc(fp)<<0;
  i|=fgetc(fp)<<8;
  if(i==0x800) {
    fread(d,8,256,fp);
    goto done;
  } else if(i==0x400) {
    fread(d+0x400,8,128,fp);
  } else if(i<32 || i>0x800) {
    fatal("Unrecognized format of code page %d\n",n);
  } else {
    fread(b,32,1,fp);
    for(s=0;s<256;s++) if(!(b[s>>3]&(1<<(s&7)))) {
      if(c=fgetc(fp)) {
        i=fgetc(fp);
        if(i&~255) fatal("Error reading data for character %d in code page %d\n",s,n);
        if(i!=s) memcpy(d+s*8,d+i*8,8);
      }
      for(j=0;j<8;j++) if(!(c&(1<<j))) d[j+s*8]=fgetc(fp);
    }
  }
  memset(d,0,8);
  goto done;
  skip:
  i=fgetc(fp)<<16;
  i|=fgetc(fp)<<24;
  i|=fgetc(fp)<<0;
  i|=fgetc(fp)<<8;
  fseek(fp,i,SEEK_CUR);
  goto name;
  done:
  codepage=n;
  fclose(fp);
}

// Widgets

static void draw_scrollbar(int*cur,int page,int max,int x0,int y0,int x1,int y1) {
  Uint8*pix=screen->pixels;
  Uint16 pitch=screen->pitch;
  int x,y,m0,m1;
  double f;
  f=(y1-y0)/(double)(max+page);
  if(*cur<0) *cur=0; else if(*cur>max) *cur=max;
  m0=*cur*f+y0;
  m1=(*cur+page)*f+y0;
  pix+=y0*pitch;
  SDL_LockSurface(screen);
  for(y=y0;y<y1;y++) {
    for(x=x0;x<x1;x++) pix[x]=y<m0?0xFF:y>m1?0xFF:(y^x)&1?0xF0:0xFF;
    pix[x1]=0xFF;
    pix+=pitch;
  }
  SDL_UnlockSurface(screen);
}

int scrollbar(int*cur,int page,int max,SDL_Event*ev,SDL_Rect*re) {
  int x0=re?re->x:0;
  int y0=re?re->y:0;
  int x1=re?re->x+12:12;
  int y1=re?re->y+re->h:screen->h;
  int y;
  double f;
  max-=page;
  if(max<0) max=0;
  switch(ev?ev->type:SDL_VIDEOEXPOSE) {
    case SDL_MOUSEMOTION:
      if(ev->motion.x<x0 || ev->motion.x>x1 || ev->motion.y<y0 || ev->motion.y>=y1) return 0;
      if(ev->motion.state&SDL_BUTTON(2)) {
        y=ev->motion.y;
        goto move;
      } else if(ev->motion.state&SDL_BUTTON(1)) {
        set_cursor(XC_sb_up_arrow);
      } else if(ev->motion.state&SDL_BUTTON(3)) {
        set_cursor(XC_sb_down_arrow);
      } else {
        set_cursor(XC_sb_v_double_arrow);
      }
      return 1;
    case SDL_MOUSEBUTTONDOWN:
      if(ev->button.x<x0 || ev->button.x>x1 || ev->button.y<y0 || ev->button.y>=y1) return 0;
      if(ev->button.button==2) {
        y=ev->button.y;
        goto move;
      } else if(ev->button.button==1) {
        set_cursor(XC_sb_up_arrow);
      } else if(ev->button.button==3) {
        set_cursor(XC_sb_down_arrow);
      }
      return 1;
    case SDL_MOUSEBUTTONUP:
      if(ev->button.x<x0 || ev->button.x>x1 || ev->button.y<y0 || ev->button.y>=y1) return 0;
      f=(y1-y0)/(double)page;
      y=(ev->button.y-y0+0.5)/f;
      if(ev->button.button==1) {
        *cur+=y;
      } else if(ev->button.button==3) {
        *cur-=y;
      }
      draw_scrollbar(cur,page,max,x0,y0,x1,y1);
      SDL_Flip(screen);
      set_cursor(XC_sb_v_double_arrow);
      return 1;
    case SDL_VIDEOEXPOSE:
      draw_scrollbar(cur,page,max,x0,y0,x1,y1);
      return 0;
    default:
      return 0;
  }
  move:
  f=(y1-y0)/(double)(max+page);
  *cur=(y-y0+0.5)/f;
  draw_scrollbar(cur,page,max,x0,y0,x1,y1);
  SDL_Flip(screen);
  set_cursor(XC_sb_right_arrow);
  return 1;
}

// Popup text

/*
  \0 to \7 (1 to 8) - Colours
  \b (15) - Horizontal line
  \c (12) - Centre align
  \i (14) - Image of class
  \l (11) - Left align
  \n (10) - Line break
  \q (16) - Quiz button
  \x (31) - Hex escape
*/

typedef struct {
  Uint8 a,h;
  Uint16 w;
} PopLine;

static void pop_bar(int x,int y,PopLine*li,int w) {
  Uint8*p=screen->pixels;
  Uint16 pitch=screen->pitch;
  if(x+w>=screen->w || y+8>screen->h) return;
  p+=(y+3)*pitch+x;
  while(w--) *p++=w&1?7:5;
}

static void pop_char(int x,int y,PopLine*li,Uint8 c,Uint8 v) {
  Uint8*p=screen->pixels;
  Uint16 pitch=screen->pitch;
  int xx,yy;
  const unsigned char*f=fontdata+(v<<3);
  if(li->h>8) y+=(li->h-8)/2;
  if(y+8>=screen->h || x+8>=screen->w) return;
  p+=y*pitch+x;
  for(yy=0;yy<8;yy++) {
    for(xx=0;xx<8;xx++) p[xx]=(*f<<xx)&128?c:0xF7;
    p+=pitch;
    ++f;
  }
}

static void pop_image(int x,int y,PopLine*li,const unsigned char*t) {
  const unsigned char*p=strchr(t,':');
  int i,n;
  if(!p) return;
  n=strtol(p+1,0,10);
  if(li->h>picture_size) y+=(li->h-picture_size)/2;
  if(y+picture_size>=screen->h || x+picture_size>=screen->w) return;
  SDL_UnlockSurface(screen);
  if(p==t+1 && *t==7) {
    // This case is used for %i substitutions
    draw_picture(x,y,n);
  } else if(p) {
    for(i=1;i<0x4000;i++) if(classes[i]) {
      if(strlen(classes[i]->name)==p-t && !memcmp(t,classes[i]->name,p-t)) {
        if(n>=0 && n<classes[i]->nimages) draw_picture(x,y,classes[i]->images[n]&0x7FFF);
        break;
      }
    }
  }
  SDL_LockSurface(screen);
}

static void pop_quiz(int x,int y,PopLine*li,Uint8 c,Uint8 v) {
  SDL_Rect r;
  Uint8*p=screen->pixels;
  Uint16 pitch=screen->pitch;
  int xx,yy;
  const unsigned char*f=fontdata+(v<<3);
  if(li->h>16) y+=(li->h-16)/2;
  r.x=x;
  r.y=y;
  r.w=r.h=16;
  SDL_FillRect(screen,&r,0x07);
  if(y+16>=screen->h || x+24>=screen->w) return;
  p+=(y+4)*pitch+x+4;
  for(yy=0;yy<8;yy++) {
    for(xx=0;xx<8;xx++) p[xx]=(*f<<xx)&128?c:0x07;
    p+=pitch;
    ++f;
  }
}

void draw_popup(const unsigned char*txt) {
  static colo[8]={1,144,188,173,83,98,218,15};
  static PopLine li[64];
  SDL_Rect r;
  int bx,by,x,y,c;
  int ln=0; // line number
  int lh=8; // height of current line
  int th=0; // total height
  int lw=0; // width of current line
  int tw=0; // total width
  const unsigned char*p=txt;
  li[0].w=li[0].h=li[0].a=0;
  // Figure out size
  while(ln<64 && *p) switch(*p++) {
    case 1 ... 8:
      // needing doing nothing yet
      break;
    case 10:
      th+=lh;
      li[ln].h=lh;
      li[ln].w=lw;
      if(tw<lw) tw=lw;
      if(*p && *p!=15) lh=8; else lh=0;
      lw=0;
      ln++;
      if(ln<64) li[ln].a=li[ln-1].a;
      break;
    case 11: case 12:
      li[ln].a=p[-1]-11;
      break;
    case 14:
      if(lh<picture_size) lh=picture_size;
      lw+=picture_size;
      p=strchr(p,'\\')?:"";
      if(*p) p++;
      break;
    case 15:
      th+=lh+8;
      li[ln].h=lh;
      li[ln].w=lw;
      if(tw<lw) tw=lw;
      lh=8;
      lw=0;
      ln++;
      if(ln<64) li[ln].a=li[ln-1].a;
      break;
    case 16:
      lw+=24;
      if(lh<16) lh=16;
      if(*p) p++;
      break;
    case 30:
      p=strchr(p,'\\')?:"";
      if(*p) p++;
      break;
    case 31:
      lw+=8;
      if(*p) p++;
      break;
    default:
      lw+=8;
  }
  done:
  if(ln<64) li[ln].h=lh,li[ln].w=lw;
  p=txt;
  th+=lh;
  if(tw<lw) tw=lw;
  if(tw>screen->w-32) tw=screen->w-32;
  if(th>screen->h-32) th=screen->h-32;
  bx=(screen->w-tw-8)/2;
  by=(screen->h-th-8)/2;
  // Draw box
  r.x=bx; r.y=by; r.w=tw+16; r.h=th+16;
  SDL_FillRect(screen,&r,0xF7);
  r.w=tw+16; r.h=1; SDL_FillRect(screen,&r,0x0F);
  r.w=1; r.h=th+16; SDL_FillRect(screen,&r,0x0F);
  r.x++; r.y++;
  r.w=tw+14; r.h=1; SDL_FillRect(screen,&r,0x0D);
  r.w=1; r.h=th+14; SDL_FillRect(screen,&r,0x0D);
  r.x++; r.y++;
  r.w=tw+12; r.h=1; SDL_FillRect(screen,&r,0x0B);
  r.w=1; r.h=th+12; SDL_FillRect(screen,&r,0x0B);
  r.x=bx+tw+15; r.y=by;
  r.w=1; r.h=th+16; SDL_FillRect(screen,&r,0x02);
  r.x--; r.y++; r.h-=2; SDL_FillRect(screen,&r,0x05);
  r.x--; r.y++; r.h-=2; SDL_FillRect(screen,&r,0x08);
  r.x=bx; r.y=by+th+15;
  r.w=tw+16; r.h=1; SDL_FillRect(screen,&r,0x02);
  r.x++; r.y--; r.w-=2; SDL_FillRect(screen,&r,0x05);
  r.x++; r.y--; r.w-=2; SDL_FillRect(screen,&r,0x08);
  // Draw text
  bx+=8; by+=8; tw-=8; th-=8;
  x=bx; y=by;
  c=colo[0];
  ln=0;
  SDL_LockSurface(screen);
  while(*p && ln<64 && y+li[ln].h<screen->h) switch(*p++) {
    case 1 ... 8:
      c=colo[p[-1]-1];
      break;
    case 10:
      y+=li[ln++].h;
      if(ln<64) x=li[ln].a?bx+(tw-li[ln].w)/2:bx;
      break;
    case 11:
      // do nothing
      break;
    case 12:
      if(x==bx) x+=(tw-li[ln].w)/2;
      break;
    case 14:
      pop_image(x,y,li+ln,p);
      x+=picture_size;
      p=strchr(p,'\\')?:"";
      if(*p) p++;
      break;
    case 15:
      y+=li[ln++].h;
      pop_bar(bx-8,y,li+ln,tw+22);
      y+=8;
      if(ln<64) x=li[ln].a?bx+(tw-li[ln].w)/2:bx;
      break;
    case 16:
      pop_quiz(x,y,li+ln,c,*p);
      if(*p) p++;
      x+=24;
      break;
    case 30:
      p=strchr(p,'\\')?:"";
      if(*p) p++;
      break;
    case 31:
      pop_char(x,y,li+ln,c,*p);
      if(*p) p++;
      x+=8;
      break;
    default:
      pop_char(x,y,li+ln,c,p[-1]);
      x+=8;
      break;
  }
  SDL_UnlockSurface(screen);
}

int modal_draw_popup(const unsigned char*txt) {
  SDL_Event ev;
  set_cursor(XC_iron_cross);
  redraw:
  draw_popup(txt);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_QUIT:
      SDL_PushEvent(&ev);
      return -1;
    case SDL_KEYDOWN:
      switch(ev.key.keysym.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_ESCAPE:
          return 0;
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      return 0;
    case SDL_VIDEOEXPOSE:
      goto redraw;
  }
  return -1;
}

