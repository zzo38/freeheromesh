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
  sqlite3_str*s=sqlite3_str_new(0);
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
  free(data);
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
  draw_text(24,32,"x",0xF0,0xF5);
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
          case SDLK_RETURN:
            add_mru(cl,img);
            return;
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
  objects[n]->x=x;
  objects[n]->y=y;
  objects[n]->image=m->img;
  objects[n]->dir=m->dir;
  objects[n]->misc1=m->misc1;
  objects[n]->misc2=m->misc2;
  objects[n]->misc3=m->misc3;
  pflink(n);
}

static int editor_command(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux) {
  switch(cmd) {
    case '^a': // Add object (no duplicates)
      if(prev) return prev;
      add_object_at(number&63?:64,number/64?:64,mru+curmru,1);
      return 0;
    case '^c': // Select class/image
      class_image_select();
      return 0;
    case '^u': // Add object (allow duplicates)
      if(prev) return prev;
      add_object_at(number&63?:64,number/64?:64,mru+curmru,0);
      return 0;
    case '^P': // Play
      return -2;
    case '^Q': // Quit
      return -1;
    case 'go': // Select level
      load_level(number);
      return 1;
    case 'mR': // Select MRU relative
      number+=curmru;
      // fall through
    case 'mr': // Select MRU absolute
      if(number>=0 && number<MRUCOUNT) curmru=number;
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
