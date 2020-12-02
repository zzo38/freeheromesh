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

void run_game(void) {
  int i;
  SDL_Event ev;
  set_caption();
  begin_level(level_id);
  redraw_game();
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_VIDEOEXPOSE:
        redraw_game();
        break;
      case SDL_QUIT:
        exit(0);
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
          return;
        }
        redraw_game();
        break;
    }
  }
}
