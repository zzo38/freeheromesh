#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-multichar edit.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"
#include "quarks.h"
#include "cursorshapes.h"

typedef struct {
  Uint16 class;
  Uint8 img,dir;
  Value misc1,misc2,misc3;
} MRU;

#define MRUCOUNT 32
static MRU mru[MRUCOUNT];
static int curmru;

static void rewrite_class_def(void) {
  Uint32 i,n;
  Uint8 cu[0x4000/8];
  Uint8 mu[0x4000/8];
  Object*o;
  long size=0;
  unsigned char*data=read_lump(FIL_LEVEL,LUMP_CLASS_DEF,&size);
  sqlite3_str*s;
  memset(cu,0,0x4000/8);
  memset(mu,0,0x4000/8);
  if(data && size) {
    for(i=0;i<size-3;) {
      n=data[i]|(data[i+1]<<8);
      if(!n) break;
      i+=2;
      while(i<size && data[i++]);
      cu[n/8]|=1<<(n&7);
    }
    i+=2;
    for(;i<size-3;) {
      n=data[i]|(data[i+1]<<8);
      n-=256;
      i+=2;
      while(i<size && data[i++]);
      if(n>=0) mu[n/8]|=1<<(n&7);
    }
  }
  free(data);
  for(i=0;i<nobjects;i++) if(o=objects[i]) {
    cu[o->class/8]|=1<<(o->class&7);
#define DoMisc(a) \
  if(o->a.t==TY_CLASS) { \
    cu[o->a.u/8]|=1<<(o->a.u&7); \
  } else if(o->a.t==TY_MESSAGE && o->a.u>=256) { \
    mu[(o->a.u-256)/8]|=1<<(o->a.u&7); \
  }
    DoMisc(misc1)
    DoMisc(misc2)
    DoMisc(misc3)
#undef DoMisc
  }
  // Now write out the new data
  s=sqlite3_str_new(0);
  for(i=0;i<0x4000;i++) if(cu[i/8]&(1<<(i&7))) {
    sqlite3_str_appendchar(s,1,i&255);
    sqlite3_str_appendchar(s,1,i>>8);
    sqlite3_str_appendall(s,classes[i]->name);
    sqlite3_str_appendchar(s,1,0);
  }
  sqlite3_str_appendchar(s,2,0);
  for(i=0;i<0x4000;i++) if(mu[i/8]&(1<<(i&7))) {
    sqlite3_str_appendchar(s,1,i&255);
    sqlite3_str_appendchar(s,1,i>>8);
    sqlite3_str_appendall(s,messages[i]);
    sqlite3_str_appendchar(s,1,0);
  }
  // End of data
  size=sqlite3_str_length(s);
  data=sqlite3_str_finish(s);
  if(!size || !data) fatal("Error in string builder\n");
  write_lump(FIL_LEVEL,LUMP_CLASS_DEF,size,data);
  sqlite3_free(data);
}

static inline void version_change(void) {
  long sz=0;
  unsigned char*buf=read_lump(FIL_SOLUTION,level_id,&sz);
  if(!buf) return;
  if(sz>2 && (buf[0]|(buf[1]<<8))==level_version) ++level_version;
  free(buf);
}

static void save_obj(sqlite3_str*s,const Object*o,const Object**m,Uint8 x,Uint8 y) {
  static Uint8 r=0;
  static Uint8 rx,ry;
  const Object*p=0;
  Uint8 b;
  Uint16 c;
  if(!o) goto nrle;
  b=o->dir&7;
  if(o->x==x+1) b|=0x40; else if(o->x!=x) b|=0x20;
  if(o->y!=y) b|=0x10;
  p=m[b&0x70?0:1];
  if((b&0x70)!=0x40 || !r || !p) goto nrle;
  if(o->class!=p->class || o->image!=p->image || !ValueEq(o->misc1,p->misc1) || !ValueEq(o->misc2,p->misc2) || !ValueEq(o->misc3,p->misc3)) goto nrle;
  if(0x0F&~r) {
    r++;
  } else {
    sqlite3_str_appendchar(s,1,r);
    if(r&0x20) sqlite3_str_appendchar(s,1,rx);
    if(r&0x10) sqlite3_str_appendchar(s,1,ry);
    r=0xC0;
  }
  return;
  nrle:
  if(r) {
    sqlite3_str_appendchar(s,1,r);
    if(r&0x20) sqlite3_str_appendchar(s,1,rx);
    if(r&0x10) sqlite3_str_appendchar(s,1,ry);
  }
  r=0;
  if(!o) return;
  if(o->misc1.t|o->misc1.u|o->misc2.t|o->misc2.u|o->misc3.t|o->misc3.u) b|=0x08;
  if(p && o->class==p->class && o->image==p->image && ValueEq(o->misc1,p->misc1) && ValueEq(o->misc2,p->misc2) && ValueEq(o->misc3,p->misc3)) {
    // Use RLE
    r=0x80|b&0xF0;
    rx=o->x;
    ry=o->y;
    return;
  }
  m[b&0x70?0:1]=o;
  sqlite3_str_appendchar(s,1,b);
  if(b&0x20) sqlite3_str_appendchar(s,1,o->x);
  if(b&0x10) sqlite3_str_appendchar(s,1,o->y);
  c=o->class;
  for(x=0;x<=o->image && x<classes[c]->nimages;x++) if(classes[c]->images[x]&0x8000) break;
  if(x==o->image) c|=0x8000;
  sqlite3_str_appendchar(s,1,c&0xFF);
  sqlite3_str_appendchar(s,1,c>>8);
  if(c<0x8000) sqlite3_str_appendchar(s,1,o->image);
  if(b&0x08) {
    b=o->misc1.t|(o->misc2.t<<2)|(o->misc3.t<<4);
    if(o->misc1.t|o->misc1.u) {
      if(o->misc3.t|o->misc3.u) b|=0xC0;
      else if(o->misc2.t|o->misc2.u) b|=0x80;
      else b|=0x40;
    }
    sqlite3_str_appendchar(s,1,b);
    if(b&0xC0) {
      sqlite3_str_appendchar(s,1,o->misc1.u&0xFF);
      sqlite3_str_appendchar(s,1,o->misc1.u>>8);
    }
    if((b&0xC0)!=0x40) {
      sqlite3_str_appendchar(s,1,o->misc2.u&0xFF);
      sqlite3_str_appendchar(s,1,o->misc2.u>>8);
    }
    if(!((b&0xC0)%0xC0)) {
      sqlite3_str_appendchar(s,1,o->misc3.u&0xFF);
      sqlite3_str_appendchar(s,1,o->misc3.u>>8);
    }
  }
}

static void update_level_index(void) {
  Uint8*data;
  int i;
  if(level_ord>level_nindex) {
    level_index=realloc(level_index,++level_nindex*sizeof(Uint16));
    if(!level_index) fatal("Allocation failed\n");
    level_index[level_ord-1]=level_id;
  }
  data=malloc(level_nindex<<1);
  if(!data) fatal("Allocation failed\n");
  for(i=0;i<level_nindex;i++) data[i+i]=level_index[i]&255,data[i+i+1]=level_index[i]>>8;
  write_lump(FIL_LEVEL,LUMP_LEVEL_IDX,2*level_nindex,data);
  free(data);
}

static void save_level(void) {
  /*
   Format of objects:
     * bit flags (or 0xFF for end):
       bit7 = MRU (omit everything but position)
       bit6 = Next position
       bit5 = New X position
       bit4 = New Y position
       bit3 = Has MiscVars (RLE in case of MRU)
       bit2-bit0 = LastDir (RLE in case of MRU)
     * new X if applicable
     * new Y if applicable
     * class (one-based; add 0x8000 for default image) (two bytes)
     * image (one byte)
     * data types (if has MiscVars):
       bit7-bit6 = How many (0=has Misc2 and Misc3, not Misc1)
       bit5-bit4 = Misc3 type
       bit3-bit2 = Misc2 type
       bit1-bit0 = Misc1 type
     * misc data (variable size)
   Store/use MRU slot 0 if any bits of 0x70 set in flag byte; slot 1 otherwise
  */
  Uint8 x=0;
  Uint8 y=1;
  const Object*m[2]={0,0};
  sqlite3_str*str=sqlite3_str_new(0);
  Uint32 n;
  long sz;
  char*data;
  int i;
  // Header
  if(level_changed) version_change();
  level_changed=0;
  sqlite3_str_appendchar(str,1,level_version&255);
  sqlite3_str_appendchar(str,1,level_version>>8);
  sqlite3_str_appendchar(str,1,level_code&255);
  sqlite3_str_appendchar(str,1,level_code>>8);
  sqlite3_str_appendchar(str,1,pfwidth-1);
  sqlite3_str_appendchar(str,1,pfheight-1);
  if(level_title) sqlite3_str_appendall(str,level_title);
  sqlite3_str_appendchar(str,1,0);
  // Objects
  for(i=0;i<64*64;i++) {
    n=playfield[i];
    while(n!=VOIDLINK) {
      save_obj(str,objects[n],m,x,y);
      x=objects[n]->x;
      y=objects[n]->y;
      n=objects[n]->up;
    }
  }
  save_obj(str,0,m,x,y);
  sqlite3_str_appendchar(str,1,0xFF);
  // Level strings
  for(i=0;i<nlevelstrings;i++) sqlite3_str_appendall(str,levelstrings[i]);
  // Done
  sz=sqlite3_str_length(str);
  if(i=sqlite3_str_errcode(str)) fatal("SQL string error (%d)\n",i);
  data=sqlite3_str_finish(str);
  if(!data) fatal("Allocation failed\n");
  sqlite3_exec(userdb,"BEGIN;",0,0,0);
  write_lump(FIL_LEVEL,level_id,sz,data);
  sqlite3_free(data);
  if(level_ord==level_nindex+1) update_level_index();
  rewrite_class_def();
  sqlite3_exec(userdb,"COMMIT;",0,0,0);
}

static void redraw_editor(void) {
  char buf[32];
  SDL_Rect r;
  int x,y;
  r.x=r.y=0;
  r.h=screen->h;
  r.w=left_margin;
  SDL_FillRect(screen,&r,0xF0);
  r.x=left_margin-1;
  r.w=1;
  SDL_FillRect(screen,&r,0xF7);
  r.x=left_margin;
  r.w=screen->w-r.x;
  SDL_FillRect(screen,&r,back_color);
  for(x=1;x<=pfwidth;x++) for(y=1;y<=pfheight;y++) draw_cell(x,y);
  x=y=0;
  SDL_GetMouseState(&x,&y);
  SDL_LockSurface(screen);
  if(left_margin>=88) {
    snprintf(buf,32,"%5d/%5d",level_ord,level_nindex);
    draw_text(0,0,buf,0xF0,0xFC);
    snprintf(buf,32,"%5d",level_id);
    draw_text(0,8,"ID",0xF0,0xF7);
    draw_text(48,8,buf,0xF0,0xFF);
    snprintf(buf,32,"%5d",level_version);
    draw_text(0,16,"VER",0xF0,0xF7);
    draw_text(48,16,buf,0xF0,0xFF);
    snprintf(buf,32,"%5d",level_code);
    draw_text(0,24,"CODE",0xF0,0xF7);
    draw_text(48,24,buf,0xF0,0xFF);
  } else {
    snprintf(buf,32,"%5d",level_ord);
    draw_text(16,0,buf,0xF0,0xFC);
    snprintf(buf,32,"%5d",level_id);
    draw_text(0,8,"I",0xF0,0xF7);
    draw_text(16,8,buf,0xF0,0xFF);
    snprintf(buf,32,"%5d",level_version);
    draw_text(0,16,"V",0xF0,0xF7);
    draw_text(16,16,buf,0xF0,0xFF);
    snprintf(buf,32,"%5d",level_code);
    draw_text(0,24,"C",0xF0,0xF7);
    draw_text(16,24,buf,0xF0,0xFF);
  }
  snprintf(buf,8,"%2dx%2d",pfwidth,pfheight);
  draw_text(8,32,buf,0xF0,0xFD);
  draw_text(24,32,"x",0xF0,level_changed?0xF4:0xF5);
  x=x>=left_margin?(x-left_margin)/picture_size+1:0;
  y=y/picture_size+1;
  if(x>0 && y>0 && x<=pfwidth && y<=pfheight) snprintf(buf,8,"(%2d,%2d)",x,y);
  else strcpy(buf,"       ");
  draw_text(0,40,buf,0xF0,0xF3);
  for(x=0;x<MRUCOUNT;x++) {
    y=picture_size*x+56;
    if(y+picture_size>screen->h) break;
    if(x==curmru) draw_text(0,y,">",0xF0,0xFE);
    if(mru[x].misc1.u|mru[x].misc1.t|mru[x].misc2.u|mru[x].misc2.t|mru[x].misc3.u|mru[x].misc3.t) draw_text(picture_size+16,y,"*",0xF0,0xFB);
  }
  SDL_UnlockSurface(screen);
  r.w=r.h=picture_size;
  r.x=8;
  for(x=0;x<MRUCOUNT;x++) {
    y=picture_size*x+56;
    if(y+picture_size>screen->h) break;
    if(mru[x].class) {
      r.y=y;
      SDL_FillRect(screen,&r,0x00);
      draw_picture(8,y,classes[mru[x].class]->images[mru[x].img]&0x7FFF);
    }
  }
  SDL_Flip(screen);
}

static void show_mouse_xy(SDL_Event*ev) {
  char buf[32];
  int x,y;
  x=(ev->motion.x-left_margin)/picture_size+1;
  y=ev->motion.y/picture_size+1;
  if(ev->motion.x<left_margin) x=0;
  if(x>0 && y>0 && x<=pfwidth && y<=pfheight) snprintf(buf,8,"(%2d,%2d)",x,y);
  else strcpy(buf,"       ");
  SDL_LockSurface(screen);
  draw_text(0,40,buf,0xF0,0xF3);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
}

static void set_caption(void) {
  const char*r;
  char*s;
  sqlite3_str*m;
  int c;
  optionquery[1]=Q_editTitle;
  r=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"Free Hero Mesh - ~ - Edit";
  m=sqlite3_str_new(0);
  c=strcspn(r,"~");
  sqlite3_str_append(m,r,c);
  if(r[c]=='~') {
    sqlite3_str_appendall(m,basefilename);
    sqlite3_str_appendall(m,r+c+1);
  }
  s=sqlite3_str_finish(m);
  if(s) SDL_WM_SetCaption(s,s); else SDL_WM_SetCaption("Free Hero Mesh","Free Hero Mesh");
  sqlite3_free(s);
}

static void add_mru(int cl,int img) {
  int i;
  if(!cl) return;
  for(i=0;i<MRUCOUNT-1;i++) if(mru[i].class==cl && mru[i].img==img) break;
  memmove(mru+1,mru,i*sizeof(MRU));
  mru[0].class=cl;
  mru[0].img=img;
  mru[0].dir=0;
  mru[0].misc1=mru[0].misc2=mru[0].misc3=NVALUE(0);
}

static void class_image_select(void) {
  SDL_Event ev;
  SDL_Rect r;
  int clscroll=0;
  int imgscroll=0;
  int clcount=0;
  int imgcount=0;
  int cl=0;
  int img=0;
  Uint16 cllist[0x4000];
  Uint8 imglist[256];
  char name[256];
  int namei=0;
  char buf[4];
  int i,j;
  sqlite3_stmt*st;
  if(sqlite3_prepare_v2(userdb,"SELECT `ID` FROM `CLASSES` WHERE `GROUP` IS NULL AND CLASS_DATA(`ID`,7) NOT NULL ORDER BY `NAME` COLLATE NOCASE;",-1,&st,0))
   fatal("Unexpected SQL error: %s\n",sqlite3_errmsg(userdb));
  while(sqlite3_step(st)==SQLITE_ROW) cllist[clcount++]=sqlite3_column_int(st,0);
  sqlite3_finalize(st);
  redraw:
  r.y=r.x=0;
  r.w=picture_size+40;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  r.x=picture_size+40;
  r.w=screen->w-r.x;
  SDL_FillRect(screen,&r,0xF1);
  scrollbar(&clscroll,screen->h/8,clcount,0,&r);
  r.x+=15;
  r.h=8;
  SDL_LockSurface(screen);
  for(i=0;i<screen->h/8;i++) {
    if(i+clscroll>=clcount) break;
    j=cllist[i+clscroll];
    if(j==cl) SDL_FillRect(screen,&r,0xF3);
    draw_text(r.x,r.y,classes[j]->name,j==cl?0xF3:0xF1,classes[j]->cflags&CF_PLAYER?0xFE:0xFF);
    if(j==cl && namei) {
      memcpy(name,classes[j]->name,namei);
      name[namei]=0;
      draw_text(r.x,r.y,name,0xF4,classes[j]->cflags&CF_PLAYER?0xFE:0xFF);
    }
    r.y+=8;
  }
  SDL_UnlockSurface(screen);
  scrollbar(&imgscroll,screen->h/picture_size,imgcount,0,0);
  r.x=14;
  r.y=0;
  r.w=r.h=picture_size;
  for(i=0;i<screen->h/picture_size;i++) {
    if(i+imgscroll>=imgcount) break;
    j=imglist[i+imgscroll];
    SDL_FillRect(screen,&r,0);
    draw_picture(14,r.y,classes[cl]->images[j]&0x7FFF);
    r.y+=picture_size;
  }
  r.y=0;
  SDL_LockSurface(screen);
  for(i=0;i<screen->h/picture_size;i++) {
    if(i+imgscroll>=imgcount) break;
    j=imglist[i+imgscroll];
    sprintf(buf,"%3d",j);
    draw_text(picture_size+15,r.y,buf,j==img?0xF2:0xF0,j==img?0xFF:0xF7);
    r.y+=picture_size;
  }
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE) {
      if(scrollbar(&imgscroll,screen->h/picture_size,imgcount,&ev,0)) goto redraw;
      r.h=screen->h;
      r.x=picture_size+40;
      r.y=0;
      if(scrollbar(&clscroll,screen->h/8,clcount,&ev,&r)) goto redraw;
    }
    switch(ev.type) {
      case SDL_MOUSEMOTION:
        set_cursor(XC_draft_small);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x>picture_size+40) {
          i=clscroll+ev.button.y/8;
          if(i<0 || i>=clcount) break;
          namei=0;
          cl=cllist[i];
          setclass:
          imgscroll=imgcount=0;
          for(i=0;i<classes[cl]->nimages;i++) if(classes[cl]->images[i]&0x8000) imglist[imgcount++]=i;
          img=*imglist;
        } else {
          i=imgscroll+ev.button.y/picture_size;
          if(i<0 || i>=imgcount) break;
          img=imglist[i];
        }
        if(ev.button.button==3 || ev.button.button==2) add_mru(cl,img);
        if(ev.button.button==2) return;
        goto redraw;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_HOME: clscroll=0; goto redraw;
          case SDLK_END: clscroll=clcount-screen->h/8; goto redraw;
          case SDLK_PAGEDOWN: clscroll+=screen->h/8; goto redraw;
          case SDLK_PAGEUP: clscroll-=screen->h/8; goto redraw;
          case SDLK_ESCAPE: return;
          case SDLK_CLEAR: case SDLK_DELETE: namei=0; goto redraw;
          case SDLK_BACKSPACE: if(namei) --namei; goto redraw;
          case SDLK_TAB:
            if(!cl) break;
            strncpy(name,classes[cl]->name,254);
            for(j=0;j<clcount;j++) if(cl==cllist[j]) break;
            // Increase namei according to all directly following letters that are matching
            while(namei<253 && classes[cl]->name[namei]) {
              for(i=j+1;i<clcount;i++) {
                if(sqlite3_strnicmp(name,classes[cllist[i]]->name,namei)) {
                  // The first part of the name doesn't match; all others did, so accept the next letter.
                  break;
                } else {
                  if(sqlite3_strnicmp(classes[cl]->name+namei,classes[cllist[i]]->name+namei,1)) {
                    // The first part of the name matches but the next letter doesn't.
                    goto redraw;
                  } else {
                    // The first part and next letter are both matching; see if the next one also matches.
                    continue;
                  }
                }
              }
              ++namei;
            }
            goto redraw;
          case SDLK_LEFT:
            for(i=0;i<imgcount-1;i++) if(img==imglist[i+1]) {
              img=imglist[i];
              goto redraw;
            }
            break;
          case SDLK_RIGHT:
            for(i=0;i<imgcount-1;i++) if(img==imglist[i]) {
              img=imglist[i+1];
              goto redraw;
            }
            break;
          case SDLK_UP:
            namei=0;
            for(i=1;i<clcount;i++) if(cl==cllist[i]) {
              cl=cllist[i-1];
              goto setclass;
            }
            break;
          case SDLK_DOWN:
            namei=0;
            if(!cl) {
              cl=*cllist;
              goto setclass;
            }
            for(i=0;i<clcount-1;i++) if(cl==cllist[i]) {
              cl=cllist[i+1];
              goto setclass;
            }
            break;
          case SDLK_RETURN: case SDLK_KP_ENTER:
            add_mru(cl,img);
            return;
          case SDLK_F1:
            if(cl && classes[cl] && classes[cl]->gamehelp) modal_draw_popup(classes[cl]->gamehelp);
            goto redraw;
          case SDLK_F2:
            if(cl && classes[cl] && classes[cl]->edithelp) modal_draw_popup(classes[cl]->edithelp);
            goto redraw;
          default:
            j=ev.key.keysym.unicode;
            if(j=='$' || j==21) {
              namei=0;
              goto redraw;
            }
            if(j==27) return;
            if(j>32 && j<127 && namei<254) {
              name[namei++]=j;
              for(i=0;i<clcount;i++) {
                if(!sqlite3_strnicmp(name,classes[cllist[i]]->name,namei)) {
                  ev.button.button=1;
                  cl=cllist[i];
                  goto setclass;
                }
              }
              --namei;
            }
        }
        break;
      case SDL_VIDEOEXPOSE:
        goto redraw;
      case SDL_QUIT:
        exit(0);
        break;
    }
  }
}

static int string_list(void);

static Value ask_value(const char*s) {
  int i;
  s=screen_prompt(s);
  if(!s || !*s) return UVALUE(0,4);
  if(*s=='-' || *s=='+' || (*s>='0' && *s<='9')) {
    if(*s=='0' && s[1]=='x') return NVALUE(strtol(s+2,0,16)&0xFFFF);
    if(*s=='0' && s[1]=='o') return NVALUE(strtol(s+2,0,8)&0xFFFF);
    return NVALUE(strtol(s,0,10)&0xFFFF);
  } else if(*s=='$') {
    for(i=1;i<0x4000;i++) {
      if(classes[i] && !(classes[i]->cflags&CF_NOCLASS2) && !strcmp(s+1,classes[i]->name)) return CVALUE(i);
    }
    screen_message("Undefined class");
  } else if(*s=='#') {
    for(i=0;i<0x4000;i++) {
      if(messages[i] && !strcmp(s+1,messages[i])) return MVALUE(i+256);
    }
    screen_message("Undefined message");
  } else if(*s>='A' && *s<='Z') {
    for(i=0;i<N_MESSAGES;i++) {
      if(!strcmp(s,standard_message_names[i])) return MVALUE(i);
    }
    screen_message("Undefined message");
  } else if(*s=='\'') {
    for(i=0;i<256;i++) {
      if(heromesh_key_names[i] && !strcmp(s+1,heromesh_key_names[i])) return NVALUE(i);
    }
    screen_message("Invalid key");
  } else {
    screen_message("Syntax error");
  }
  return UVALUE(0,4);
}

static void mru_edit(MRU*m) {
  SDL_Event ev;
  SDL_Rect r;
  char buf[256];
  int i,j;
  Value v;
  if(!m->class) return;
  redraw:
  set_cursor(XC_arrow);
  r.x=r.y=0;
  r.w=screen->w;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  SDL_LockSurface(screen);
  draw_text(0,0,"Class:",0xF0,0xF7);
  draw_text(64,0,"$",0xF0,0xFB);
  draw_text(72,0,classes[m->class]->name,0xF0,0xFB);
  draw_text(0,8,"Image:",0xF0,0xF7);
  snprintf(buf,255,"%d",m->img);
  draw_text(64,8,buf,0xF0,0xFE);
  draw_text(0,24,"Dir:",0xF0,0xF7);
  snprintf(buf,255,"%2.2s","E NEN NWW SWS SE"+(m->dir&7)*2);
  draw_text(64,24,buf,0xF0,0xFF);
  for(i=32;i<56;i+=8) {
    switch(i) {
      case 32: v=m->misc1; draw_text(0,32,"Misc1:",0xF0,0xF7); break;
      case 40: v=m->misc2; draw_text(0,40,"Misc2:",0xF0,0xF7); break;
      case 48: v=m->misc3; draw_text(0,48,"Misc3:",0xF0,0xF7); break;
    }
    switch(v.t) {
      case TY_NUMBER:
        snprintf(buf,255,"%u",(int)v.u);
        draw_text(64,i,buf,0xF0,0xFE);
        break;
      case TY_CLASS:
        draw_text(64,i,"$",0xF0,0xFB);
        draw_text(72,i,classes[v.u]->name,0xF0,0xFB);
        break;
      case TY_MESSAGE:
        snprintf(buf,255,"%s%s",v.u<256?"":"#",v.u<256?standard_message_names[v.u]:messages[v.u-256]);
        draw_text(64,i,buf,0xF0,0xFD);
        break;
      case TY_LEVELSTRING:
        snprintf(buf,255,"String %u",(int)v.u);
        draw_text(200,i,buf,0xF0,0xF9);
        break;
    }
  }
  draw_text(0,64,"Str:",0xF0,0xF7);
  snprintf(buf,255,"%d",nlevelstrings);
  draw_text(64,64,buf,0xF0,0xF7);
  draw_text(0,80,"<1-3> Misc   <4-6> Misc=String   <NumPad> Dir",0xF0,0xF3);
  draw_text(0,88,"<F1> Help   <F2> EditorHelp   <RET> Exit",0xF0,0xF3);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      switch(ev.key.keysym.sym) {
        case SDLK_RETURN: case SDLK_KP_ENTER: case SDLK_ESCAPE: return;
        case SDLK_1: v=ask_value("Misc1:"); if(v.t<4) m->misc1=v; break;
        case SDLK_2: v=ask_value("Misc2:"); if(v.t<4) m->misc2=v; break;
        case SDLK_3: v=ask_value("Misc3:"); if(v.t<4) m->misc3=v; break;
        case SDLK_4: case SDLK_q: j=string_list(); if(j!=-1) m->misc1=UVALUE(j,TY_LEVELSTRING); break;
        case SDLK_5: case SDLK_w: j=string_list(); if(j!=-1) m->misc2=UVALUE(j,TY_LEVELSTRING); break;
        case SDLK_6: case SDLK_e: j=string_list(); if(j!=-1) m->misc3=UVALUE(j,TY_LEVELSTRING); break;
        case SDLK_KP1: m->dir=5; break;
        case SDLK_KP2: m->dir=6; break;
        case SDLK_KP3: m->dir=7; break;
        case SDLK_KP4: m->dir=4; break;
        case SDLK_KP6: m->dir=0; break;
        case SDLK_KP7: m->dir=3; break;
        case SDLK_KP8: m->dir=2; break;
        case SDLK_KP9: m->dir=1; break;
        case SDLK_KP_PLUS: m->dir=(m->dir+1)&7; break;
        case SDLK_KP_MINUS: m->dir=(m->dir-1)&7; break;
        case SDLK_F1: case SDLK_g: if(classes[m->class]->gamehelp) modal_draw_popup(classes[m->class]->gamehelp); break;
        case SDLK_F2: case SDLK_h: if(classes[m->class]->edithelp) modal_draw_popup(classes[m->class]->edithelp); break;
      }
      goto redraw;
    case SDL_VIDEOEXPOSE:
      goto redraw;
    case SDL_QUIT:
      exit(0);
      break;
  }
}

static void add_object_at(int x,int y,MRU*m,int d) {
  Uint32 n,u;
  Class*c;
  if(x<1 || x>pfwidth || y<1 || y>pfheight || !m || !m->class) return;
  c=classes[m->class];
  if(!c) return;
  n=playfield[y*64+x-65];
  while(n!=VOIDLINK) {
    if(d && objects[n]->class==m->class) return;
    if(c->collisionLayers&classes[objects[n]->class]->collisionLayers) {
      u=objects[n]->up;
      objtrash(n);
      n=u;
    } else {
      n=objects[n]->up;
    }
  }
  generation_number_inc=0;
  n=objalloc(m->class);
  if(n==VOIDLINK) return;
  level_changed=1;
  objects[n]->x=x;
  objects[n]->y=y;
  objects[n]->image=m->img;
  objects[n]->dir=m->dir;
  objects[n]->misc1=m->misc1;
  objects[n]->misc2=m->misc2;
  objects[n]->misc3=m->misc3;
  pflink(n);
}

static void fprint_esc(FILE*fp,Uint8 c,const char*t) {
  char isimg=0;
  if(!c) return;
  fputc(c,fp);
  while(*t) switch(c=*t++) {
    case 1 ... 8: fprintf(fp,"\\%c",c+'0'-1); break;
    case 10: fprintf(fp,"\\n"); break;
    case 11: fprintf(fp,"\\l"); break;
    case 12: fprintf(fp,"\\c"); break;
    case 14: fprintf(fp,"\\i"); isimg=1; break;
    case 15: fprintf(fp,"\\b"); break;
    case 16: fprintf(fp,"\\q"); break;
    case 31:
      if(!*t) break;
      fprintf(fp,"\\x%02X",*t++);
      break;
    case 32 ... 127:
      if(c=='\\') {
        if(isimg) isimg=0; else fputc(c,fp);
      }
      fputc(c,fp);
      break;
    default:
      fprintf(fp,"\\x%02X",c);
  }
  fputc('\n',fp);
}

static void fprint_misc(FILE*fp,Value v) {
  switch(v.t) {
    case TY_NUMBER: fprintf(fp," %u",(int)v.u); break;
    case TY_CLASS: fprintf(fp," $%s",classes[v.u]->name); break;
    case TY_MESSAGE: fprintf(fp," %s%s",v.u<256?"":"#",v.u<256?standard_message_names[v.u]:messages[v.u-256]);
    case TY_LEVELSTRING: fprintf(fp," %%%u",(int)v.u); break;
    default: fprintf(fp," ???"); break;
  }
}

static void export_level(const char*cmd) {
  int i;
  Uint32 n;
  Object*o;
  FILE*fp;
  if(!cmd || !*cmd) return;
  fp=popen(cmd,"w");
  if(!fp) {
    screen_message("Cannot open pipe");
    return;
  }
  fprintf(fp,"; Free Hero Mesh exported level ID=%d ORD=%d\n",level_id,level_ord);
  fprint_esc(fp,'@',level_title);
  fprintf(fp,"C %d\nD %d %d\n",level_code,pfwidth,pfheight);
  for(i=0;i<64*64;i++) {
    n=playfield[i];
    while(n!=VOIDLINK) {
      o=objects[n];
      fprintf(fp,"%d %d $%s %d",o->x,o->y,classes[o->class]->name,o->image);
      fprint_misc(fp,o->misc1);
      fprint_misc(fp,o->misc2);
      fprint_misc(fp,o->misc3);
      fprintf(fp," %d\n",o->dir);
      n=o->up;
    }
  }
  for(i=0;i<nlevelstrings;i++) fprint_esc(fp,'%',levelstrings[i]);
  pclose(fp);
}

static char*import_numbers(char*p,int*x,int*y) {
  if(!p) return 0;
  while(*p==' ' || *p=='\t') p++;
  if(*p<'0' || *p>'9') return 0;
  *x=strtol(p,&p,10);
  if(*p && *p!=' ' && *p!='\t') return 0;
  while(*p==' ' || *p=='\t') p++;
  if(!y) return p;
  if(*p<'0' || *p>'9') return 0;
  *y=strtol(p,&p,10);
  if(*p && *p!=' ' && *p!='\t') return 0;
  while(*p==' ' || *p=='\t') p++;
  return p;
}

static char*import_value(char*p,Value*v) {
  int i;
  int n=strcspn(p," \t");
  if(!p) return 0;
  while(*p==' ' || *p=='\t') p++;
  if(*p>='0' && *p<='9') {
    v->t=TY_NUMBER;
    v->u=strtol(p,&p,10)&0xFFFF;
  } else if(*p=='$') {
    v->t=TY_CLASS;
    p++; n--;
    for(i=1;i<0x4000;i++) {
      if(classes[i] && !(classes[i]->cflags&CF_NOCLASS2) && strlen(classes[i]->name)==n && !memcmp(p,classes[i]->name,n)) {
        v->u=i;
        p+=n;
        goto ok;
      }
    }
    return 0;
  } else if(*p=='%') {
    v->t=TY_LEVELSTRING;
    v->u=strtol(p+1,&p,10)&0xFFFF;
  } else if(*p=='#') {
    v->t=TY_MESSAGE;
    p++; n--;
    for(i=0;i<0x4000;i++) {
      if(messages[i] && strlen(messages[i])==n && !memcmp(p,messages[i],n)) {
        v->u=i+256;
        p+=n;
        goto ok;
      }
    }
    return 0;
  } else if(*p>='A' && *p<='Z') {
    v->t=TY_MESSAGE;
    for(i=0;i<N_MESSAGES;i++) {
      if(strlen(standard_message_names[i])==n && !memcmp(p,standard_message_names[i],n)) {
        v->u=i;
        p+=n;
        goto ok;
      }
    }
    return 0;
  } else {
    return 0;
  }
  ok: if(*p==' ' || *p=='\t') return p+1; else return 0;
}

static char*import_string(char*p) {
  char isimg=0;
  char*s=malloc(strlen(p)+3);
  char*q=s;
  if(!s) fatal("Allocation failed\n");
  while(*p) {
    if(*p=='\\') {
      p++;
      if(isimg) {
        isimg=0;
        *q++='\\';
      } else {
        switch(*p) {
          case '0' ... '7': *q++=*p+1-'0'; break;
          case 'b': *q++=15; break;
          case 'c': *q++=12; break;
          case 'i': *q++=14; isimg=1; break;
          case 'l': *q++=11; break;
          case 'n': *q++=10; break;
          case 'q': *q++=16; break;
          case 'x':
            *q++=31;
            *q=0;
            if(p[1]>='0' && p[1]<='9') *q=(p[1]-'0')<<4;
            else if(p[1]>='A' && p[1]<='F') *q=(p[1]+10-'A')<<4;
            else if(p[1]>='a' && p[1]<='f') *q=(p[1]+10-'a')<<4;
            else goto bad;
            if(p[2]>='0' && p[2]<='9') *q|=(p[2]-'0');
            else if(p[2]>='A' && p[2]<='F') *q|=(p[2]+10-'A');
            else if(p[2]>='a' && p[2]<='f') *q|=(p[2]+10-'a');
            else goto bad;
            p+=2; q++; break;
          default: *q++=*p;
        }
        p++;
      }
    } else {
      *q++=*p++;
    }
  }
  if(0) {
    bad:
    screen_message("Bad \\x escape");
  }
  if(isimg) *q++='\\';
  *q=0;
  return s;
}

static void import_level(const char*cmd) {
  char d=0;
  int x,y;
  Object*o;
  FILE*fp;
  size_t size=0;
  char*buf=0;
  char*p;
  Value v;
  if(!cmd || !*cmd) return;
  fp=popen(cmd,"r");
  if(!fp) {
    screen_message("Cannot open pipe");
    return;
  }
  level_changed=1;
  while(getline(&buf,&size,fp)>0) {
    p=buf;
    p[strcspn(p,"\r\n\f")]=0;
    p+=strspn(p,"\t ");
    switch(*p) {
      case '0' ... '9':
        if(!d) {
          missd:
          screen_message("Missing D line");
          goto done;
        }
        p=import_numbers(p,&x,&y);
        if(!p) goto bad;
        if(x<1 || x>pfwidth || y<1 || y>pfheight) {
          range:
          fprintf(stderr,"Imported level coordinates out of range: %d %d\n",x,y);
          screen_message("Coordinates out of range");
          goto done;
        }
        p=import_value(p,&v);
        if(!p || v.t!=TY_CLASS) goto bad;
        v.u=objalloc(v.u);
        if(v.u==VOIDLINK) goto bad;
        o=objects[v.u];
        o->x=x;
        o->y=y;
        p=import_value(p,&o->misc1);
        p=import_value(p,&o->misc2);
        p=import_value(p,&o->misc3);
        p=import_numbers(p,&x,0);
        o->dir=x;
        if((x&~7) || !p) {
          objtrash(v.u);
          goto bad;
        }
        pflink(v.u);
        break;
      case 'C':
        p=import_numbers(p+1,&x,0);
        if(!p || *p) goto bad;
        level_code=x;
        break;
      case 'D':
        p=import_numbers(p+1,&x,&y);
        if(!p || *p) goto bad;
        if(x<1 || x>64 || y<1 || y>64) goto range;
        if(d) {
          screen_message("Duplicate D line");
          goto done;
        }
        d=1;
        annihilate();
        pfwidth=x;
        pfheight=y;
        break;
      case '@':
        free(level_title);
        level_title=import_string(p+1);
        break;
      case '%':
        if(nlevelstrings>0x2000) {
          screen_message("Too many level strings");
        } else {
          levelstrings=realloc(levelstrings,(nlevelstrings+1)*sizeof(unsigned char*));
          if(!levelstrings) fatal("Allocation failed\n");
          levelstrings[nlevelstrings++]=import_string(p+1);
        }
        break;
      default:
        if(*p && *p!=';') {
          bad:
          fprintf(stderr,"Invalid record in imported data:  %s\n",buf);
          screen_message("Invalid record");
          goto done;
        }
    }
  }
  done:
  free(buf);
  pclose(fp);
  generation_number_inc=0;
}

static void new_level(void) {
  sqlite3_stmt*st;
  int i;
  if(i=sqlite3_prepare_v2(userdb,"SELECT COALESCE(MAX(`LEVEL`),-1)+1 FROM `USERCACHEDATA` WHERE `FILE` = LEVEL_CACHEID();",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return;
  }
  i=sqlite3_step(st);
  if(i!=SQLITE_ROW) {
    sqlite3_finalize(st);
    screen_message(sqlite3_errmsg(userdb));
    return;
  }
  i=sqlite3_column_int(st,0);
  sqlite3_finalize(st);
  if(i>0xFFFE) return;
  annihilate();
  level_id=i;
  free(level_title);
  level_title=strdup("New Level");
  if(!level_title) fatal("Allocation failed\n");
  level_changed=level_version=level_code=0;
  level_ord=level_nindex+1;
}

static int cursor_end(const Uint8*s) {
  int i=0;
  for(;;) {
    if(*s==10 || !*s) return i;
    if(*s==31 && s[1]) s++;
    i++;
    s++;
  }
}

static inline Uint8 pick_character(void) {
  SDL_Rect r;
  SDL_Event ev;
  Uint8 buf[17];
  int i,j;
  static Uint8 p=0;
  redraw:
  r.x=r.y=4;
  r.w=r.h=0x9C;
  SDL_FillRect(screen,&r,0xF8);
  SDL_LockSurface(screen);
  draw_text(24,8,"0123456789ABCDEF",0xF8,0xF3);
  buf[16]=0;
  for(i=0;i<16;i++) {
    snprintf(buf,2,"%X",i);
    draw_text(8,(i+3)<<3,buf,0xF8,0xF3);
    for(j=0;j<16;j++) buf[j]=(i<<4)|j?:255;
    draw_text(24,(i+3)<<3,buf,0xF8,0xFF);
  }
  buf[0]=p?:255;
  buf[1]=0;
  i=p>>4;
  j=p&15;
  draw_text((j+3)<<3,(i+3)<<3,buf,0xF2,0xFE);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        SDL_PushEvent(&ev);
        return;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_RETURN: case SDLK_SPACE: case SDLK_KP_ENTER: return p?:255;
          case SDLK_KP2: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_DOWN: p+=16; break;
          case SDLK_KP4: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_LEFT: p--; break;
          case SDLK_KP6: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_RIGHT: p++; break;
          case SDLK_KP8: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_UP: p-=16; break;
          default: norm:
            i=ev.key.keysym.unicode;
            if(i>='0' && i<='9') p=(p<<4)|(i-'0');
            if(i>='A' && i<='F') p=(p<<4)|(i+10-'A');
            if(i>='a' && i<='f') p=(p<<4)|(i+10-'a');
            if(i==13 || i==10) return p?:255;
        }
        goto redraw;
      case SDL_VIDEOEXPOSE:
        goto redraw;
    }
  }
}

static void edit_string(unsigned char**ps) {
  SDL_Rect rect;
  SDL_Event ev;
  char buf[19];
  Uint8*s=malloc(0x3000);
  Uint8*cp;
  Uint16 li[64];
  int c,e,i,j,n,r,sz;
  char o=0;
  char d=0;
  if(!s) fatal("Allocation failed\n");
  if(*ps) strncpy(s,*ps,0x2FFD); else *s=0;
  s[0x2FFD]=0;
  *li=0;
  c=r=0;
  redraw:
  e=0;
  redraw2:
  set_cursor(XC_xterm);
  rect.x=rect.y=0;
  rect.w=screen->w;
  rect.h=screen->h;
  SDL_FillRect(screen,&rect,0xF0);
  rect.h=24;
  SDL_FillRect(screen,&rect,0xF1);
  SDL_LockSurface(screen);
  if(ps==&level_title) {
    draw_text(0,16," Title: ",0xF9,0xFF);
  } else {
    snprintf(buf,19," String %d: ",(int)(ps-levelstrings));
    draw_text(0,16,buf,0xF9,0xFF);
  }
  draw_text(160,16,o?"OVR":"INS",0xF1,0xFE);
  draw_text(0,0,"<Esc> Cancel  <F1> Preview  <F2> Save  <\x18\x19\x1A\x1B> MoveCursor  <^P> InsertChar  <^Y> DelLine",0xF1,0xFB);
  draw_text(0,8,"ALT+  <0-7> Color  <B> Bar  <C> Center  <I> Image  <L> Left  <Q> Quiz",0xF1,0xFB);
  draw_text(0,24,"\x10",0xF0,0xF1);
  cp=0;
  for(i=j=n=0;s[i] && n<63;) {
    draw_text(0,(n+3)<<3,"\x10",0xF0,0xF1);
    i+=draw_text_line(8,(n+3)<<3,s+i,r==n?c:-1,&cp);
    if(j=(s[i]==10)) i++;
    sz=li[++n]=i;
  }
  if(!i) {
    sz=li[n=1]=r=c=0;
    cp=s;
    draw_text(8,24,"\xFE",0xFF,0xFE);
  } else if(j && n<63) {
    draw_text(0,(n+3)<<3,"\x10",0xF0,0xF1);
    draw_text(8,(n+3)<<3,"\xFE",r==n?0xFF:0xF0,r==n?0xFE:0xF1);
    if(r==n) cp=s+i,c=0;
    li[++n]=i;
  }
  if(r && r>=n) r--,e=0;
  if(n<63) li[n+1]=0xFFFF;
  SDL_UnlockSurface(screen);
  if(!cp) {
    if(e) fatal("Confusion in edit_string\n");
    c=cursor_end(s+li[r]);
    e=1;
    goto redraw2;
  }
  if(d) {
    d=0;
    goto del;
  }
  snprintf(buf,19,"%d,%d / %d [%d]",r+1,c,n,sz);
  draw_text(200,16,buf,0xF1,0xFA);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        SDL_PushEvent(&ev);
        return;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: return;
          case SDLK_F1: modal_draw_popup(s); break;
          case SDLK_F2:
            free(*ps);
            *ps=s;
            return;
          case SDLK_KP0: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_INSERT: ins: o^=1; break;
          case SDLK_KP1: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_END: end: c=cursor_end(s+li[r]); break;
          case SDLK_KP2: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_DOWN: down: if(r<63 && li[r+1]!=0xFFFF) ++r; break;
          case SDLK_KP4: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_LEFT: left: if(c) --c; break;
          case SDLK_KP6: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_RIGHT: right: ++c; break;
          case SDLK_KP7: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_HOME: home: c=0; break;
          case SDLK_KP8: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_UP: up: if(r) --r; break;
          case SDLK_KP_PERIOD: if(ev.key.keysym.mod&KMOD_NUM) goto norm; //fallthrough
          case SDLK_DELETE: del:
            if(!*cp) break;
            i=(*cp==31?2:1);
            memmove(cp,cp+i,(s+sz)-(cp+i-1));
            break;
          case SDLK_BACKSPACE: back:
            if(cp==s || !c) break;
            c--;
            d=1;
            break;
          default: norm:
            i=ev.key.keysym.unicode;
            if(ev.key.keysym.mod&(KMOD_ALT|KMOD_META)) {
              switch(ev.key.keysym.sym) {
                case SDLK_0: i=1; goto addch;
                case SDLK_1: i=2; goto addch;
                case SDLK_2: i=3; goto addch;
                case SDLK_3: i=4; goto addch;
                case SDLK_4: i=5; goto addch;
                case SDLK_5: i=6; goto addch;
                case SDLK_6: i=7; goto addch;
                case SDLK_7: i=8; goto addch;
                case SDLK_b: i=15; goto addch;
                case SDLK_c: i=12; goto addch;
                case SDLK_i: i=14; goto addch;
                case SDLK_l: i=11; goto addch;
                case SDLK_n: i=10; goto addch;
                case SDLK_q: i=16; goto addch;
              }
            }
            if(i<32) {
              switch(i+64) {
                case 'A': goto home;
                case 'D': goto right;
                case 'E': goto up;
                case 'F': goto end;
                case 'G': goto del;
                case 'H': goto back;
                case 'M': i=10; goto addch;
                case 'P': goto specialchar;
                case 'S': goto left;
                case 'W': r=c=0; break;
                case 'X': goto down;
                case 'V': goto ins;
                case 'Y': goto erase;
                case 'Z': r=n?n-1:0; c=0; break;
              }
            } else if(i<127) {
              addch:
              if(i==10 && n>62) break;
              if(sz>=0x2FFC) break;
              if(o && *cp==31) memmove(cp,cp+1,(s+sz)-cp);
              if(!*cp) cp[1]=0;
              if(!o) memmove(cp+1,cp,(s+sz+1)-cp);
              *cp=i;
              if(i==10) r++,c=0; else c++;
            }
            break;
        }
        goto redraw;
      case SDL_VIDEOEXPOSE:
        goto redraw;
    }
  }
  erase:
  c=0;
  if(li[r+1]==0xFFFF) s[li[r]]=0; else memmove(s+li[r],s+li[r+1],sz+1-li[r+1]);
  if(r && r==n-1) --r;
  goto redraw;
  specialchar:
  if(sz>=0x2FFA) goto redraw;
  i=pick_character();
  if(i>=32) goto addch;
  if(o && *cp==31) memmove(cp,cp+1,(s+sz)-cp);
  if(!*cp) cp[1]=0;
  memmove(cp+1,cp,(s+sz+1)-cp);
  if(!o) memmove(cp+1,cp,(s+sz+1)-cp);
  *cp=31;
  cp[1]=i;
  c++;
  goto redraw;
}

static inline void replace_level_string_uses(int x) {
  Uint32 n;
  Object*o;
  for(n=0;n<nobjects;n++) if(o=objects[n]) {
    if(o->misc1.t==TY_LEVELSTRING && o->misc1.u>=x) --o->misc1.u;
    if(o->misc2.t==TY_LEVELSTRING && o->misc2.u>=x) --o->misc2.u;
    if(o->misc3.t==TY_LEVELSTRING && o->misc3.u>=x) --o->misc3.u;
  }
}

static inline int count_level_string_uses(int x) {
  int c=0;
  Uint32 n;
  Object*o;
  for(n=0;n<nobjects;n++) if(o=objects[n]) {
    if(o->misc1.t==TY_LEVELSTRING && o->misc1.u==x) c++;
    if(o->misc2.t==TY_LEVELSTRING && o->misc2.u==x) c++;
    if(o->misc3.t==TY_LEVELSTRING && o->misc3.u==x) c++;
  }
  return c;
}

static int string_list(void) {
  char buf[18];
  SDL_Event ev;
  SDL_Rect r;
  int n=0;
  int scroll=0;
  int i,j;
  redraw:
  r.x=r.y=0;
  r.w=screen->w;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  r.h=8;
  SDL_FillRect(screen,&r,0xF1);
  r.y=8;
  r.h=screen->h-8;
  scrollbar(&scroll,screen->h/8-1,nlevelstrings,0,&r);
  SDL_LockSurface(screen);
  draw_text(0,0,"Level Strings:   <INS> Add  <DEL> Delete  <SP> Edit  <RET> Pick  <F1> Preview  <F2> Uses  <ESC> Cancel",0xF1,0xFF);
  for(i=0;i<screen->h/8-1;i++) {
    j=i+scroll;
    if(j>=nlevelstrings) break;
    snprintf(buf,8,"%c%5d%c",n==j?'<':' ',j,n==j?'>':' ');
    draw_text(16,(i+1)<<3,buf,0xF0,0xFB);
    draw_text(80,(i+1)<<3,levelstrings[j],0xF0,0xF7);
  }
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE && scrollbar(&scroll,screen->h/8-1,nlevelstrings,&ev,&r)) goto redraw;
    switch(ev.type) {
      case SDL_MOUSEMOTION:
        set_cursor(XC_draft_small);
        break;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_0: case SDLK_KP0: n*=10; break;
          case SDLK_1: case SDLK_KP1: n=10*n+1; break;
          case SDLK_2: case SDLK_KP2: n=10*n+2; break;
          case SDLK_3: case SDLK_KP3: n=10*n+3; break;
          case SDLK_4: case SDLK_KP4: n=10*n+4; break;
          case SDLK_5: case SDLK_KP5: n=10*n+5; break;
          case SDLK_6: case SDLK_KP6: n=10*n+6; break;
          case SDLK_7: case SDLK_KP7: n=10*n+7; break;
          case SDLK_8: case SDLK_KP8: n=10*n+8; break;
          case SDLK_9: case SDLK_KP9: n=10*n+9; break;
          case SDLK_BACKSPACE: n/=10; break;
          case SDLK_ESCAPE: return -1;
          case SDLK_RETURN: case SDLK_KP_ENTER: if(n<nlevelstrings) return n;
          case SDLK_SPACE: edit_string(levelstrings+n); break;
          case SDLK_INSERT:
            if(nlevelstrings<0x2000) {
              levelstrings=realloc(levelstrings,(nlevelstrings+1)*sizeof(unsigned char*));
              if(!levelstrings) fatal("Allocation failed\n");
              levelstrings[n=nlevelstrings++]=strdup("");
              if(!levelstrings[n]) fatal("Allocation failed\n");
              edit_string(levelstrings+n);
            }
            break;
          case SDLK_DELETE:
            if(n>=nlevelstrings) break;
            if(n==nlevelstrings-1) {
              free(levelstrings[--nlevelstrings]);
            } else {
              replace_level_string_uses(n+1);
              free(levelstrings[n]);
              --nlevelstrings;
              for(i=n;i<nlevelstrings;i++) levelstrings[i]=levelstrings[i+1];
            }
            break;
          case SDLK_F1: if(nlevelstrings) modal_draw_popup(levelstrings[n]); set_cursor(XC_draft_small); break;
          case SDLK_F2:
            if(nlevelstrings) {
              snprintf(buf,17,"%d uses",count_level_string_uses(n));
              modal_draw_popup(buf);
            }
            break;
          case SDLK_UP: case SDLK_KP_MINUS: n--; break;
          case SDLK_DOWN: case SDLK_KP_PLUS: n++; break;
        }
        if(n<=0) n=0; else if(n>=nlevelstrings) n=nlevelstrings-1;
        goto redraw;
      case SDL_VIDEOEXPOSE:
        goto redraw;
      case SDL_QUIT:
        exit(0);
        return -1;
    }
  }
}

static int editor_command(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux) {
  int x,y;
  switch(cmd) {
    case '^a': // Add object (no duplicates)
      if(prev) return prev;
      add_object_at(number&63?:64,number/64?:64,mru+curmru,1);
      return 0;
    case '^c': // Select class/image
      class_image_select();
      return 0;
    case '^e': // Edit Misc/Dir of MRU slot
      mru_edit(mru+curmru);
      return 0;
    case '^s': // String list/edit
      string_list();
      return 0;
    case '^u': // Add object (allow duplicates)
      if(prev) return prev;
      add_object_at(number&63?:64,number/64?:64,mru+curmru,0);
      return 0;
    case '^N': // New level
      if(level_nindex>0xFFFE) return 0;
      new_level();
      return 1;
    case '^P': // Play
      return -2;
    case '^Q': // Quit
      return -1;
    case '^S': // Save level
      save_level();
      return 1;
    case '^T': // Level title
      edit_string(&level_title);
      return 0;
    case 'ex': // Export level
      if(argc<2) return prev;
      export_level(sqlite3_column_text(args,1));
      return prev;
    case 'go': // Select level
      load_level(number);
      return 1;
    case 'im': // Import level
      if(argc<2) return prev;
      import_level(sqlite3_column_text(args,1));
      return 0;
    case 'lc': // Set level code
      level_code=number;
      level_changed=1;
      return 0;
    case 'lt': // Set level title
      if(argc<2 || !sqlite3_column_text(args,1)) break;
      free(level_title);
      level_title=strdup(sqlite3_column_text(args,1));
      if(!level_title) fatal("Allocation failed\n");
      return 0;
    case 'lv': // Set level version
      level_version=number;
      level_changed=0;
      return 0;
    case 'mR': // Select MRU relative
      number+=curmru;
      // fall through
    case 'mr': // Select MRU absolute
      if(number>=0 && number<MRUCOUNT) curmru=number;
      return 0;
    case 're': // Resize and clear
      if(argc<3) return 0;
      x=sqlite3_column_int(args,1);
      y=sqlite3_column_int(args,2);
      if(x<1 || y<1 || x>64 || y>64) return 0;
      level_changed=1;
      annihilate();
      pfwidth=x;
      pfheight=y;
      return 0;
    default:
      return prev;
  }
}

void run_editor(void) {
  SDL_Event ev;
  int i;
  curmru=0;
  set_caption();
  load_level(level_id);
  redraw_editor();
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_VIDEOEXPOSE:
        redraw_editor();
        break;
      case SDL_QUIT:
        exit(0);
        break;
      case SDL_MOUSEMOTION:
        set_cursor(ev.motion.x<left_margin?XC_arrow:XC_tcross);
        show_mouse_xy(&ev);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x<left_margin) {
          if(ev.button.y<56) break;
          i=(ev.button.y-56)/picture_size;
          if(i>=0 && i<MRUCOUNT) {
            curmru=i;
            if(ev.button.button==3) mru_edit(mru+i);
            redraw_editor();
          }
          break;
        } else {
          i=exec_key_binding(&ev,1,(ev.button.x-left_margin)/picture_size+1,ev.button.y/picture_size+1,editor_command,0);
          goto command;
        }
      case SDL_KEYDOWN:
        i=exec_key_binding(&ev,1,0,0,editor_command,0);
      command:
        if(i==-1) exit(0);
        if(i==-2) {
          main_options['e']=0;
          return;
        }
        redraw_editor();
        break;
    }
  }
}

void write_empty_level_set(FILE*fp) {
  static const unsigned char d[]=
    "CLASS.DEF\0\x00\x00\x02\x00"
    "\x00\x00" // no classes or messages
    "LEVEL.IDX\0\x00\x00\x02\x00"
    "\x00\x00" // only one level
    "0.LVL\0\x00\x00\x0D\x00"
    "\x00\x00" // level version
    "\x00\x00" // level code
    "\x00\x00" // width/height
    "Blank\0" // title
    "\xFF" // objects
  ;
  fwrite(d,1,sizeof(d)-1,fp);
  fflush(fp);
  rewind(fp);
}
