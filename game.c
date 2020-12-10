#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-multichar game.c `sdl-config --cflags`
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

static volatile Uint8 timerflag;

static void redraw_game(void) {
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
  draw_text(0,40,buf,0xF0,0xF1);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  set_cursor(XC_arrow);
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
  draw_text(0,40,buf,0xF0,0xF1);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
}

static void begin_level(int id) {
  const char*t;
  t=load_level(id)?:init_level();
  if(t) {
    gameover=-1;
    screen_message(t);
  } else {
    gameover=0;
  }
}

static void list_objects_at(int xy) {
  static const char*const dirs[8]={"E ","NE","N ","NW","W ","SW","S ","SE"};
  SDL_Event ev;
  SDL_Rect r;
  char buf[256];
  int scroll=0;
  int count=0;
  Uint32 n,t;
  Object*o;
  int i,j;
  if(xy<0 || xy>=64*64) return;
  n=playfield[xy];
  if(n==VOIDLINK) return;
  while(n!=VOIDLINK) t=n,count++,n=objects[n]->up;
  redraw:
  r.x=r.y=0;
  r.w=screen->w;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF1);
  r.y=8;
  r.h-=8;
  scrollbar(&scroll,r.h/8,count,0,&r);
  snprintf(buf,255," %d objects at (%d,%d): ",count,(xy&63)+1,(xy/64)+1);
  SDL_LockSurface(screen);
  draw_text(0,0,buf,0xF7,0xF0);
  n=t;
  for(i=0;i<scroll && n!=VOIDLINK;i++) n=objects[n]->down;
  for(i=0;i<screen->h/8 && n!=VOIDLINK;i++) {
    o=objects[n];
    snprintf(buf,255," %8d: %-14.14s %3d %s",n,classes[o->class]->name,o->image,dirs[o->dir&7]);
    draw_text(24,r.y,buf,0xF1,o->generation?0xFF:0xF8);
    n=o->down;
    r.y+=8;
  }
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_MOUSEMOTION:
        set_cursor(XC_draft_small);
        break;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: case SDLK_RETURN: return;
        }
        goto redraw;
      case SDL_VIDEOEXPOSE:
        goto redraw;
      case SDL_QUIT:
        exit(0);
        break;
    }
  }
}

static int game_command(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux) {
  switch(cmd) {
    case '\' ': // Play a move
      return number;
    case '^E': // Edit
      return -2;
    case '^L': // Select level
      begin_level(number);
      return 1;
    case '^Q': // Quit
      return -1;
    case '^o': // List objects
      list_objects_at(number-65);
      return prev;
    default:
      return prev;
  }
}

static void set_caption(void) {
  const char*r;
  char*s;
  sqlite3_str*m;
  int c;
  optionquery[1]=Q_gameTitle;
  r=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"Free Hero Mesh - ~ - Game";
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

static Uint32 timer_callback(Uint32 n) {
  if(!timerflag) {
    static SDL_Event ev={SDL_USEREVENT};
    SDL_PushEvent(&ev);
  }
  timerflag=1;
  return n;
}

void run_game(void) {
  int i;
  SDL_Event ev;
  set_caption();
  begin_level(level_id);
  redraw_game();
  timerflag=0;
  SDL_SetTimer(10,timer_callback);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_VIDEOEXPOSE:
        redraw_game();
        break;
      case SDL_QUIT:
        exit(0);
        break;
      case SDL_MOUSEMOTION:
        show_mouse_xy(&ev);
        break;
      case SDL_USEREVENT:
        //TODO: animation
        timerflag=0;
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x<left_margin) {
          
          break;
        } else {
          i=exec_key_binding(&ev,0,(ev.button.x-left_margin)/picture_size+1,ev.button.y/picture_size+1,game_command,0);
          goto command;
        }
      case SDL_KEYDOWN:
        i=exec_key_binding(&ev,0,0,0,game_command,0);
      command:
        if(i==-1) exit(0);
        if(i==-2) {
          main_options['e']=1;
          SDL_SetTimer(0,0);
          return;
        }
        redraw_game();
        break;
    }
  }
}
