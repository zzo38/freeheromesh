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

SDL_Surface*screen;
Uint16 picture_size;

static SDL_Surface*picts;
static Uint8*curpic;

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

static void init_palette(void) {
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
  fprintf(stderr,"Loading pictures...\n");
  fp=fopen(nam,"r");
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
  if(sqlite3_prepare_v2(userdb,"INSERT INTO `PICTURES`(`ID`,`NAME`,`OFFSET`) VALUES(?1,?2,?3);",-1,&st,0))
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
    if(i>4 && !memcmp(".IMG",nam+i-4,4)) {
      j=1;
      if(n++==32768) fatal("Too many pictures\n");
      sqlite3_reset(st);
      sqlite3_bind_int(st,1,n);
      sqlite3_bind_text(st,2,nam,i-4,SQLITE_TRANSIENT);
      sqlite3_bind_int64(st,3,ftell(fp)+4);
      while((i=sqlite3_step(st))==SQLITE_ROW);
      if(i!=SQLITE_DONE) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
    } else {
      j=0;
    }
    i=fgetc(fp)<<16;
    i|=fgetc(fp)<<24;
    i|=fgetc(fp)<<0;
    i|=fgetc(fp)<<8;
    if(j) {
      memset(havesize1,0,256);
      i-=j=fgetc(fp)&15;
      while(j--) havesize1[fgetc(fp)&255]=1;
      fseek(fp,i-1,SEEK_CUR);
      for(i=1;i<256;i++) if(havesize1[i]) for(j=i+i;j<256;j+=i) havesize1[j]=1;
      for(j=1;j<256;j++) havesize[j]+=havesize1[j];
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
  if(sqlite3_prepare_v2(userdb,"SELECT `ID`, `OFFSET` FROM `PICTURES`;",-1,&st,0))
   fatal("Unable to prepare SQL statement while loading pictures: %s\n",sqlite3_errmsg(userdb));
  optionquery[1]=Q_screenFlags;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  i=v&&strchr(v,'h');
  picts=SDL_CreateRGBSurface((i?SDL_HWSURFACE:SDL_SWSURFACE)|SDL_SRCCOLORKEY,picture_size<<4,picture_size*((n+15)>>4),8,0,0,0,0);
  if(!picts) fatal("Error allocating surface for pictures: %s\n",SDL_GetError());
  init_palette();
  for(i=0;i<n;i++) {
    if((j=sqlite3_step(st))!=SQLITE_ROW) fatal("SQL error (%d): %s\n",j,j==SQLITE_DONE?"Incorrect number of rows in a temporary table":sqlite3_errmsg(userdb));
    fseek(fp,sqlite3_column_int64(st,1),SEEK_SET);
    load_one_picture(fp,sqlite3_column_int(st,0),altImage);
  }
  sqlite3_finalize(st);
  fclose(fp);
  SDL_SetColorKey(picts,SDL_SRCCOLORKEY|SDL_RLEACCEL,0);
done:
  if(n=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
  fprintf(stderr,"Done\n");
}

void init_screen(void) {
  const char*v;
  int w,h,i;
  if(main_options['x']) return;
  optionquery[1]=Q_screenWidth;
  w=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"800",0,10);
  optionquery[1]=Q_screenHeight;
  h=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"600",0,10);
  optionquery[1]=Q_screenFlags;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  if(SDL_Init(SDL_INIT_VIDEO|(strchr(v,'z')?SDL_INIT_NOPARACHUTE:0))) fatal("Error initializing SDL: %s\n",SDL_GetError());
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
}
