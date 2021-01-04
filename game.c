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
#include "names.h"

Uint8*replay_list;
Uint16 replay_size,replay_count,replay_pos,replay_mark;

static volatile Uint8 timerflag;
static int exam_scroll;
static Uint8*inputs;
static int inputs_size,inputs_count;

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
  if(quiz_text) draw_popup(quiz_text);
  SDL_Flip(screen);
  set_cursor(XC_arrow);
}

static void continue_animation(void) {
  Uint32 n=firstobj;
  Object*o;
  Animation*a;
  int i;
  for(i=0;i<8;i++) if(anim_slot[i].length && ++anim_slot[i].vtime==anim_slot[i].speed && (anim_slot[i].vtime=0,++anim_slot[i].frame==anim_slot[i].length)) anim_slot[i].frame=0;
  while(n!=VOIDLINK) {
    o=objects[n];
    if((a=o->anim) && (a->status&ANISTAT_VISUAL)) {
      i=a->vstep;
      if(a->step[i].flag&ANI_SYNC) {
        i=anim_slot[a->step[i].slot].frame+a->step[i].start;
        if(i!=a->vimage) {
          a->vimage=i;
          draw_cell(o->x,o->y);
        }
      } else if(++a->vtime>=a->step[i].speed) {
        a->vtime=0;
        if(a->vimage==a->step[i].end) {
          if(a->step[i].flag&ANI_ONCE) {
            if(a->vstep==a->lstep) {
              a->status&=~ANISTAT_VISUAL;
            } else {
              if(++a->vstep==max_animation) a->vstep=0;
              a->vimage=a->step[a->vstep].start;
            }
          } else if(a->step[i].flag&ANI_OSC) {
            a->step[i].end=a->step[i].start;
            a->step[i].start=a->vimage;
            goto advance;
          } else {
            a->vimage=a->step[i].start;
          }
        } else {
          advance:
          if(a->step[i].end>=a->step[i].start) ++a->vimage; else --a->vimage;
        }
        draw_cell(o->x,o->y);
      }
    }
    n=o->next;
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
  draw_text(0,40,buf,0xF0,0xF1);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
}

static void begin_level(int id) {
  const char*t;
  inputs_count=0;
  replay_pos=0;
  t=load_level(id)?:init_level();
  if(t) {
    gameover=-1;
    screen_message(t);
  } else {
    gameover=0;
  }
  timerflag=0;
}

static inline void exam_value(const char*t,int y,Value v) {
  char buf[256];
  int i;
  y=(y-exam_scroll)*8;
  if(y<0 || y>screen->h-8) return;
  draw_text(0,y,t,0xF0,0xF7);
  switch(v.t) {
    case TY_NUMBER:
      snprintf(buf,255,"%12lu  0x%08lX  %ld",(long)v.u,(long)v.u,(long)v.s);
      draw_text(200,y,buf,0xF0,0xFE);
      break;
    case TY_CLASS:
      draw_text(200,y,"$",0xF0,0xFB);
      draw_text(208,y,classes[v.u]->name,0xF0,0xFB);
      break;
    case TY_MESSAGE:
      snprintf(buf,255,"%s%s",v.u<256?"":"#",v.u<256?standard_message_names[v.u]:messages[v.u-256]);
      draw_text(200,y,buf,0xF0,0xFD);
      break;
    case TY_LEVELSTRING: case TY_STRING:
      draw_text(200,y,"<String>",0xF0,0xF9);
      break;
    case TY_SOUND: case TY_USOUND:
      draw_text(200,y,"<Sound>",0xF0,0xF6);
      break;
    default:
      snprintf(buf,80,"<%lu:%lu>",(long)v.u,(long)v.t);
      draw_text(200,y,buf,0xF0,0xFA);
      i=strlen(buf)*8+208;
      if(v.u<nobjects && objects[v.u] && objects[v.u]->generation==v.t) {
        snprintf(buf,80,"@ (%d,%d)",objects[v.u]->x,objects[v.u]->y);
        draw_text(i,y,buf,0xF0,0xF2);
      } else {
        draw_text(i,y,"(dead)",0xF0,0xF4);
      }
      break;
  }
}

static inline void exam_flags(int y,Uint16 v) {
  y=(y-exam_scroll)*8;
  if(y<0 || y>screen->h-8) return;
  draw_text(0,y,"Flags:",0xF0,0xF7);
  draw_text(200,y,"--- --- --- --- --- --- --- --- --- --- ---",0xF0,0xF8);
  if(v&OF_INVISIBLE) draw_text(200,y,"Inv",0xF0,0xFF);
  if(v&OF_VISUALONLY) draw_text(232,y,"Vis",0xF0,0xFF);
  if(v&OF_STEALTHY) draw_text(264,y,"Stl",0xF0,0xFF);
  if(v&OF_BUSY) draw_text(296,y,"Bus",0xF0,0xFF);
  if(v&OF_USERSTATE) draw_text(328,y,"Ust",0xF0,0xFF);
  if(v&OF_USERSIGNAL) draw_text(360,y,"Usg",0xF0,0xFF);
  if(v&OF_MOVED) draw_text(392,y,"Mov",0xF0,0xFF);
  if(v&OF_DONE) draw_text(424,y,"Don",0xF0,0xFF);
  if(v&OF_KEYCLEARED) draw_text(456,y,"Key",0xF0,0xFF);
  if(v&OF_DESTROYED) draw_text(488,y,"Des",0xF0,0xFF);
  if(v&OF_BIZARRO) draw_text(520,y,"Biz",0xF0,0xFF);
}

static inline void exam_hardsharp(const char*t,int y,Uint16*v) {
  int i;
  char buf[16];
  y=(y-exam_scroll)*8;
  if(y<0 || y>screen->h-8) return;
  draw_text(0,y,t,0xF0,0xF7);
  for(i=0;i<4;i++) {
    snprintf(buf,8,"%5u",v[i]);
    draw_text(200+i*56,y,buf,0xF0,v[i]?0xFF:0xF8);
  }
}

static void draw_back_line(int y,int c) {
  unsigned char*p=screen->pixels;
  int i;
  p+=screen->pitch*y;
  for(i=0;i<screen->w;i++) if(p[i]==0xF0 || p[i]==0xF1) p[i]=i&1?c:0xF0;
}

static void examine(Uint32 n) {
  sqlite3_stmt*st;
  SDL_Event ev;
  SDL_Rect r;
  Object*o;
  int i,y;
  y=0;
  i=sqlite3_prepare_v2(userdb,"SELECT '%'||`NAME`,`ID`&0xFFFF FROM `VARIABLES` WHERE `ID` BETWEEN ?1 AND (?1|0xFFFF) ORDER BY `ID`",-1,&st,0);
  if(i) fatal("SQL error (%d): %s",i,sqlite3_errmsg(userdb));
  object:
  if(n==VOIDLINK) return;
  o=objects[n];
  if(!o) return;
  sqlite3_bind_int(st,1,o->class<<16);
  exam_scroll=0;
  redraw:
  set_cursor(XC_arrow);
  r.x=r.y=0;
  r.w=screen->w;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  SDL_LockSurface(screen);
  exam_value("Self:",0,OVALUE(n));
  exam_value("Class:",1,CVALUE(o->class));
  exam_value("Image:",2,NVALUE(o->image));
  if(classes[o->class]->cflags&CF_QUIZ) goto quiz;
  exam_value("Dir:",3,NVALUE(o->dir));
  exam_value("Misc1:",4,o->misc1);
  exam_value("Misc2:",5,o->misc2);
  exam_value("Misc3:",6,o->misc3);
  exam_value("Misc4:",7,o->misc4);
  exam_value("Misc5:",8,o->misc5);
  exam_value("Misc6:",9,o->misc6);
  exam_value("Misc7:",10,o->misc7);
  exam_value("Temperature:",11,NVALUE(o->temperature));
  exam_flags(12,o->oflags);
  exam_value("Density:",13,NVALUE(o->density));
  exam_value("Volume:",14,NVALUE(o->volume));
  exam_value("Strength:",15,NVALUE(o->strength));
  exam_value("Weight:",16,NVALUE(o->weight));
  exam_value("Climb:",17,NVALUE(o->climb));
  exam_value("Height:",18,NVALUE(o->height));
  exam_value("Arrivals:",19,NVALUE(o->arrivals));
  exam_value("Departures:",20,NVALUE(o->departures));
  exam_value("Shape:",21,NVALUE(o->shape));
  exam_value("Shovable:",22,NVALUE(o->shovable));
  exam_value("Distance:",23,NVALUE(o->distance));
  exam_value("Inertia:",24,NVALUE(o->inertia));
  exam_hardsharp("Hardness:",25,o->hard);
  exam_hardsharp("Sharpness:",26,o->sharp);
  while(sqlite3_step(st)==SQLITE_ROW) {
    i=sqlite3_column_int(st,1);
    exam_value(sqlite3_column_text(st,0),i+28,o->uservars[i]);
  }
  quiz:
  sqlite3_reset(st);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      switch(ev.key.keysym.sym) {
        case SDLK_ESCAPE: case SDLK_RETURN: case SDLK_KP_ENTER: sqlite3_finalize(st); return;
        case SDLK_UP: if(exam_scroll) exam_scroll--; break;
        case SDLK_DOWN: exam_scroll++; break;
        case SDLK_HOME: exam_scroll=0; break;
        case SDLK_PAGEUP: exam_scroll-=screen->h/8; if(exam_scroll<0) exam_scroll=0; break;
        case SDLK_PAGEDOWN: exam_scroll+=screen->h/8; break;
      }
      goto redraw;
    case SDL_MOUSEMOTION:
      if(ev.motion.y!=y && ev.motion.y<screen->h) {
        SDL_LockSurface(screen);
        draw_back_line(y,0xF0);
        draw_back_line(y=ev.motion.y,0xF1);
        SDL_UnlockSurface(screen);
        SDL_Flip(screen);
      }
      break;
    case SDL_VIDEOEXPOSE:
      goto redraw;
    case SDL_QUIT:
      exit(0);
      break;
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
    snprintf(buf,255," %8d: %-14.14s %3d %s",n,classes[o->class]->name,o->image,classes[o->class]->cflags&CF_QUIZ?"":dirs[o->dir&7]);
    draw_text(24,r.y,buf,0xF1,o->generation?(classes[o->class]->cflags&CF_PLAYER?0xFE:0xFF):0xF8);
    n=o->down;
    r.y+=8;
  }
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE) {
      r.h=screen->h-8;
      r.x=0;
      r.y=8;
      if(scrollbar(&scroll,r.h/8,count,&ev,&r)) goto redraw;
    }
    switch(ev.type) {
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.button!=1 || ev.button.y<8) break;
        j=ev.button.y/8-scroll-1;
        if(j>=count) break;
        n=t;
        for(i=0;i<j;i++) n=objects[n]->down;
        examine(n);
        set_cursor(XC_draft_small);
        goto redraw;
      case SDL_MOUSEMOTION:
        set_cursor(XC_draft_small);
        break;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: case SDLK_RETURN: case SDLK_KP_ENTER: return;
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

static void describe_at(int xy) {
  unsigned char*s;
  Uint32 n;
  if(xy<0 || xy>=64*64) return;
  n=playfield[xy];
  if(n==VOIDLINK) return;
  while(n!=VOIDLINK && objects[n]->up!=VOIDLINK) n=objects[n]->up;
  if(!classes[objects[n]->class]->gamehelp) return;
  s=sqlite3_mprintf("\x0C\x0E%s:%d\\ %s\x0B\x0F%s",classes[objects[n]->class]->name,objects[n]->image,classes[objects[n]->class]->name,classes[objects[n]->class]->gamehelp);
  if(!s) fatal("Allocation failed\n");
  modal_draw_popup(s);
  sqlite3_free(s);
}

static int game_command(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux) {
  switch(cmd) {
    case '\' ': // Play a move
      if(inputs_count>=inputs_size) {
        inputs=realloc(inputs,inputs_size+=32);
        if(!inputs) fatal("Allocation failed\n");
      }
      inputs[inputs_count++]=number;
      return 0;
    case '+ ': replay: // Replay
      if(number>replay_count-replay_pos) number=replay_count-replay_pos;
      if(number<=0) return prev;
      if(inputs_count+number>=inputs_size) {
        inputs=realloc(inputs,inputs_size+=number+1);
        if(!inputs) fatal("Allocation failed\n");
      }
      memcpy(inputs+inputs_count,replay_list+replay_pos,number);
      inputs_count+=number;
      return 0;
    case '- ': // Rewind
      number=replay_pos-number;
      if(number<0) number=0;
      //fallthru
    case '= ': restart: // Restart
      begin_level(level_id);
      if(!number) return 1;
      if(number>replay_count) number=replay_count;
      if(number>=inputs_size) {
        inputs=realloc(inputs,inputs_size=number+1);
        if(!inputs) fatal("Allocation failed\n");
      }
      memcpy(inputs,replay_list,inputs_count=number);
      return 1;
    case '^<': // Rewind to mark
      number=replay_mark;
      goto restart;
    case '^>': // Replay to mark
      inputs_count=0;
      number=replay_mark-replay_pos;
      goto replay;
    case '^E': // Edit
      return -2;
    case '^M': // Mark replay position
      replay_mark=replay_pos+inputs_count;
      return prev;
    case '^Q': // Quit
      return -1;
    case '^T': // Show title
      modal_draw_popup(level_title);
      return prev;
    case '^d': // Describe object
      describe_at(number-65);
      return prev;
    case '^o': // List objects
      list_objects_at(number-65);
      return prev;
    case 'go': // Select level
      begin_level(number);
      return 1;
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

static inline void input_move(Uint8 k) {
  const char*t=execute_turn(k);
  if(replay_pos==65534 && !gameover) t="Too many moves played";
  if(t) {
    screen_message(t);
    gameover=-1;
    return;
  }
  if(!key_ignored) {
    if(replay_pos>=replay_size) {
      replay_list=realloc(replay_list,replay_size+=0x200);
      if(!replay_list) fatal("Allocation failed\n");
    }
    replay_list[replay_pos++]=k;
    if(replay_pos>replay_count) replay_count=replay_pos;
  }
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
        if(!gameover && !quiz_text) continue_animation();
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
        if(inputs_count) {
          //TODO: Check for solution replay
          for(i=0;i<inputs_count && !gameover;i++) if(inputs[i]) input_move(inputs[i]);
          inputs_count=0;
        }
        redraw_game();
        timerflag=0; // ensure we have not missed a timer event
        break;
    }
  }
}

void locate_me(int x,int y) {
  Uint8 c=7;
  SDL_Rect r,rh,rv;
  SDL_Event ev;
  if(!screen) return;
  redraw_game();
  r.x=(x-1)*picture_size+left_margin;
  r.y=(y-1)*picture_size;
  r.w=r.h=picture_size;
  rh.x=0;
  rh.y=r.y+picture_size/2;
  rh.w=screen->w;
  rh.h=1;
  rv.x=r.x+picture_size/2;
  rv.y=0;
  rv.w=1;
  rv.h=screen->h;
  show:
  timerflag=0;
  SDL_FillRect(screen,&rh,c+45);
  SDL_FillRect(screen,&rv,c+67);
  SDL_FillRect(screen,&r,c);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_USEREVENT:
      if(c>240) return;
      c+=15;
      goto show;
    case SDL_KEYDOWN: case SDL_QUIT: case SDL_MOUSEBUTTONDOWN:
      SDL_PushEvent(&ev);
      return;
  }
}
