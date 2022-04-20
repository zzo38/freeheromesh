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
#include <time.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"
#include "quarks.h"
#include "cursorshapes.h"
#include "names.h"

Uint8*replay_list;
Uint16 replay_size,replay_count,replay_pos,replay_mark;
Uint8 solution_replay=255;

static volatile Uint8 timerflag;
static int exam_scroll;
static Uint8*inputs;
static int inputs_size,inputs_count;
static Uint8 side_mode=255;
static Uint8 should_record_solution;
static Uint8 replay_speed;
static Uint8 replay_time;
static Uint8 solved;
static Uint8 inserting,saved_inserting;
static sqlite3_stmt*autowin;

static void record_solution(void);

static void setup_game(void) {
  const char*v;
  optionquery[1]=Q_showInventory;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  side_mode=boolxrm(v,1);
  optionquery[1]=Q_replaySpeed;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  replay_speed=strtol(v,0,10)?:16;
  if(main_options['r']) {
    should_record_solution=0;
  } else {
    optionquery[1]=Q_saveSolutions;
    v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
    should_record_solution=boolxrm(v,0);
  }
  solution_replay=0;
  optionquery[1]=Q_autoWin;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(v && *v) sqlite3_prepare_v3(userdb,v,-1,SQLITE_PREPARE_PERSISTENT,&autowin,0);
}

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
    draw_text(0,0,buf,0xF0,solution_replay?0xFE:solved?0xFA:0xFC);
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
    draw_text(16,0,buf,0xF0,solution_replay?0xFE:solved?0xFA:0xFC);
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
  if(!gameover) {
    snprintf(buf,8,"%2dx%2d",pfwidth,pfheight);
    draw_text(8,32,buf,0xF0,0xFD);
    draw_text(24,32,"x",0xF0,0xF5);
  } else if(gameover<0) {
    draw_text(4,32,"*LOSE*",0xF4,0xFC);
  } else {
    draw_text(4,32,"*WIN*",0xF2,0xFA);
  }
  x=x>=left_margin?(x-left_margin)/picture_size+1:0;
  y=y/picture_size+1;
  if(x>0 && y>0 && x<=pfwidth && y<=pfheight) snprintf(buf,8,"(%2d,%2d)",x,y);
  else strcpy(buf,"       ");
  draw_text(0,40,buf,0xF0,0xF1);
  if(side_mode) {
    // Inventory
    x=20-(left_margin-picture_size)/8;
    if(x>19) x=19;
    if(x<0) x=0;
    for(y=0;y<ninventory;y++) {
      if(y*picture_size+60>=screen->h) break;
      snprintf(buf,22,"%20d",inventory[y].value);
      draw_text(picture_size,y*picture_size+52,buf+x,0xF8,0xFE);
    }
    SDL_UnlockSurface(screen);
    r.x=0; r.y=52; r.w=picture_size; r.h=screen->h-52;
    SDL_FillRect(screen,&r,inv_back_color);
    for(y=0;y<ninventory;y++) {
      if(y*picture_size+60>=screen->h) break;
      if(classes[inventory[y].class]->nimages<inventory[y].image) continue;
      draw_picture(0,y*picture_size+52,classes[inventory[y].class]->images[inventory[y].image]&0x7FFF);
    }
  } else {
    // Move list
    snprintf(buf,8,"%5d",replay_pos);
    draw_text(8,52,buf,0xF0,0xF9);
    snprintf(buf,8,"%5d",replay_count);
    draw_text(8,screen->h-8,buf,0xF0,solution_replay?0xFA:0xFC);
    for(y=44,x=replay_pos-(screen->h-68)/32;;x++) {
      y+=16;
      if(y+24>screen->h) break;
      if(x>=0 && x<replay_count) draw_key(16,y,replay_list[x],0xF8,0xFB);
      if(x==replay_count) draw_key(16,y,1,0xF0,0xF8);
      if(x==replay_pos) draw_text(0,y,inserting?"I~":"~~",0xF0,0xFE);
      if(x==replay_mark) draw_text(32,y,"~~",0xF0,0xFD);
    }
    SDL_UnlockSurface(screen);
  }
  if(quiz_text) draw_popup(quiz_text);
  SDL_Flip(screen);
  set_cursor(XC_arrow);
}

static void continue_animation(void) {
  Uint32 n=firstobj;
  Object*o;
  Animation*a;
  DeadAnimation*d;
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
  if(ndeadanim) {
    for(i=0;i<ndeadanim;i++) {
      d=deadanim+i;
      draw_cell(d->x,d->y);
      if(!d->s.flag) continue;
      if(d->delay) {
        --d->delay;
        continue;
      }
      if(d->vimage<classes[d->class]->nimages)
       draw_picture((d->x-1)*picture_size+left_margin,(d->y-1)*picture_size,classes[d->class]->images[d->vimage]&0x7FFF);
      if(++d->vtime>=d->s.speed) {
        if(d->vimage==d->s.end) d->s.flag=0;
        if(d->s.end>=d->s.start) ++d->vimage; else --d->vimage;
        d->vtime=0;
      }
    }
    for(i=0;i<ndeadanim;i++) while(i<ndeadanim && !deadanim[i].s.flag) {
      draw_cell(deadanim[i].x,deadanim[i].y);
      if(i<ndeadanim-1) deadanim[i]=deadanim[ndeadanim-1];
      --ndeadanim;
    }
  }
  SDL_Flip(screen);
}

static void show_mouse_xy(SDL_Event*ev) {
  char buf[32];
  int x,y;
  x=(ev->motion.x-left_margin)/picture_size+1;
  y=ev->motion.y/picture_size+1;
  if(ev->motion.x<left_margin) {
    if(ev->button.y<48) {
      strcpy(buf,"       ");
    } else if(side_mode) {
      // Inventory
      x=(ev->button.y-52)/picture_size;
      if(x<0 || x>=ninventory) strcpy(buf,"       "); else snprintf(buf,8,"%7d",inventory[x].value);
    } else {
      // Move list
      x=replay_pos+(ev->button.y+4)/16-(screen->h-68)/32-4;
      if(x<0 || x>replay_count) strcpy(buf,"       "); else snprintf(buf,8,"%c%6d",x<replay_pos?0xAE:x>replay_pos?0xAF:0xFE,x);
    }
  } else {
    if(x>0 && y>0 && x<=pfwidth && y<=pfheight) snprintf(buf,8,"(%2d,%2d)",x,y);
    else strcpy(buf,"       ");
  }
  SDL_LockSurface(screen);
  draw_text(0,40,buf,0xF0,0xF1);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
}

static void save_replay(void) {
  long sz=replay_size;
  if(solution_replay || !replay_list) return;
  if(sz<replay_count+6) {
    replay_list=realloc(replay_list,sz=replay_count+6);
    if(!replay_list) fatal("Allocation failed\n");
    replay_size=(sz>0xFFFF?0xFFFF:sz);
  }
  if(gameover==1) solved=1;
  sz=replay_count+6;
  replay_list[sz-6]=replay_mark>>8;
  replay_list[sz-5]=replay_mark;
  replay_list[sz-4]=(level_version+solved-1)>>8;
  replay_list[sz-3]=level_version+solved-1;
  replay_list[sz-2]=replay_count>>8;
  replay_list[sz-1]=replay_count;
  write_userstate(FIL_LEVEL,level_id,sz,replay_list);
}

static void load_replay(void) {
  long sz;
  int i;
  free(replay_list);
  if(solution_replay) {
    replay_list=read_lump(FIL_SOLUTION,level_id,&sz);
    // Solution format: version (16-bits), flag (8-bits), user name (null-terminated), timestamp (64-bits), move list
    if(sz>3) {
      i=replay_list[0]|(replay_list[1]<<8);
      if(i!=level_version) goto notfound;
      i=3;
      if(replay_list[2]&1) {
        while(i<sz && replay_list[i]) i++;
        i++;
      }
      if(replay_list[2]&2) i+=8;
      if(i>=sz || sz-i>0xFFFF) goto notfound;
      replay_size=(sz>0xFFFF?0xFFFF:sz);
      memmove(replay_list,replay_list+i,replay_count=sz-i);
      replay_mark=0;
    } else {
      goto notfound;
    }
  } else {
    replay_list=read_userstate(FIL_LEVEL,level_id,&sz);
    if(sz>=2) {
      replay_size=(sz>0xFFFF?0xFFFF:sz);
      replay_count=(replay_list[sz-2]<<8)|replay_list[sz-1];
      if(sz-replay_count>=4) replay_mark=(replay_list[replay_count]<<8)|replay_list[replay_count+1]; else replay_mark=0;
      if(sz-replay_count>=6) {
        i=(replay_list[replay_count+2]<<8)|replay_list[replay_count+3];
        if(i==level_version) solved=1;
      }
    } else {
      notfound:
      replay_count=replay_mark=replay_size=0;
      free(replay_list);
      replay_list=0;
    }
  }
}

static void begin_level(int id) {
  const char*t;
  replay_time=0;
  if(replay_count) save_replay();
  inputs_count=0;
  replay_pos=0;
  solved=0;
  inserting=0;
  t=load_level(id)?:init_level();
  load_replay();
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
    case TY_MARK:
      draw_text(200,y,"<Mark>",0xF0,0xF3);
      break;
    case TY_ARRAY:
      draw_text(200,y,"<Array>",0xF0,0xF9);
      snprintf(buf,255,"0x%08lX",(long)v.u);
      draw_text(264,y,buf,0xF0,0xFE);
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
  draw_text(200,y,"--- --- --- --- --- --- --- --- --- --- --- --- --- ---",0xF0,0xF8);
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
  if(v&OF_CONNECTION) draw_text(552,y,"Con",0xF0,0xFF);
  if(v&OF_MOVING) draw_text(584,y,"Mvi",0xF0,0xFF);
  if(v&OF_ORDERED) draw_text(616,y,"Ord",0xF0,0xFF);
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
        case SDLK_F1: case SDLK_g: if(classes[o->class]->gamehelp) modal_draw_popup(classes[o->class]->gamehelp); break;
        case SDLK_F2: case SDLK_h: if(classes[o->class]->edithelp) modal_draw_popup(classes[o->class]->edithelp); break;
        case SDLK_1: case SDLK_4: if(o->misc1.t==TY_LEVELSTRING) modal_draw_popup(value_string_ptr(o->misc1)); break;
        case SDLK_2: case SDLK_5: if(o->misc2.t==TY_LEVELSTRING) modal_draw_popup(value_string_ptr(o->misc2)); break;
        case SDLK_3: case SDLK_6: if(o->misc3.t==TY_LEVELSTRING) modal_draw_popup(value_string_ptr(o->misc3)); break;
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

static void global_examine(void) {
  sqlite3_stmt*st;
  SDL_Event ev;
  SDL_Rect r;
  int i,y;
  y=0;
  i=sqlite3_prepare_v2(userdb,"SELECT '@'||`NAME`,`ID` FROM `VARIABLES` WHERE `ID` BETWEEN 0x0000 AND 0xFFFF ORDER BY `ID`",-1,&st,0);
  if(i) fatal("SQL error (%d): %s",i,sqlite3_errmsg(userdb));
  exam_scroll=0;
  redraw:
  set_cursor(XC_arrow);
  r.x=r.y=0;
  r.w=screen->w;
  r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  SDL_LockSurface(screen);
  exam_value("MoveNumber:",0,NVALUE(move_number));
  exam_value("LevelStrings:",1,NVALUE(nlevelstrings));
  exam_value("Generation:",2,NVALUE(generation_number));
  while(sqlite3_step(st)==SQLITE_ROW) {
    i=sqlite3_column_int(st,1);
    exam_value(sqlite3_column_text(st,0),i+4,globals[i]);
  }
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

static void list_objects_at(int xy,Uint32*pf,const char*s) {
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
  n=pf[xy];
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
  snprintf(buf,255," %d %sobjects at (%d,%d): ",count,s,(xy&63)+1,(xy/64)+1);
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
  while(n!=VOIDLINK && !classes[objects[n]->class]->gamehelp) n=objects[n]->down;
  if(n==VOIDLINK) return;
  if(classes[objects[n]->class]->gamehelp[0]==16 && !classes[objects[n]->class]->gamehelp[1]) {
    if(objects[n]->misc1.t!=TY_LEVELSTRING || objects[n]->misc1.u>=nlevelstrings) return;
    modal_draw_popup(levelstrings[objects[n]->misc1.u]);
    return;
  }
  s=sqlite3_mprintf("\x0C\x0E%s:%d\\ %s\x0B\x0F%s",classes[objects[n]->class]->name,objects[n]->image,classes[objects[n]->class]->name,classes[objects[n]->class]->gamehelp);
  if(!s) fatal("Allocation failed\n");
  modal_draw_popup(s);
  sqlite3_free(s);
}

static void describe_inventory(int n) {
  unsigned char*s;
  if(n<0 || n>=ninventory) return;
  if(!classes[inventory[n].class]->gamehelp) return;
  if(classes[inventory[n].class]->gamehelp[0]==16) return;
  s=sqlite3_mprintf("\x0C\x0E%s:%d\\ %s\x0B\x0F%s",classes[inventory[n].class]->name,inventory[n].image,classes[inventory[n].class]->name,classes[inventory[n].class]->gamehelp);
  if(!s) fatal("Allocation failed\n");
  modal_draw_popup(s);
  sqlite3_free(s);
}

static void do_import_moves(const char*arg) {
  FILE*fp;
  int i;
  if(!arg || !arg[strspn(arg," \t")]) return;
  fp=popen(arg,"r");
  if(!fp) {
    screen_message("Unable to open pipe for reading");
    return;
  }
  replay_list=realloc(replay_list,0x10000);
  if(!replay_list) fatal("Allocation failed");
  replay_mark=0;
  replay_size=0xFFFF;
  i=fread(replay_list,1,0xFFFD,fp);
  if(i&~0xFFFF) i=0;
  replay_count=i;
  pclose(fp);
}

static void do_export_moves(const char*arg) {
  FILE*fp;
  int i;
  if(!arg || !arg[strspn(arg," \t")]) return;
  fp=popen(arg,"w");
  if(!fp) {
    screen_message("Unable to open pipe for writing");
    return;
  }
  if(replay_count) fwrite(replay_list,1,replay_count,fp);
  pclose(fp);
}

static void do_load_moves(sqlite3_stmt*st) {
  int i=sqlite3_column_bytes(st,1);
  if(i&~0xFFFF) return;
  if(replay_size<i) replay_list=realloc(replay_list,replay_size=i);
  if(!replay_list) fatal("Allocation failed");
  replay_count=i;
  if(i) memcpy(replay_list,sqlite3_column_blob(st,1),i);
}

static int copy_text_to_plain(unsigned char*out,int maxlen,const unsigned char*in) {
  int at=0;
  if(!in) {
    *out=0;
    return 0;
  }
  while(*in && at<maxlen) switch(*in) {
    case 10: if(at && out[at-1]!=32) out[at++]=32; in++; break;
    case 14: case 30: in=strchrnul(in,'\\'); if(*in) in++; break;
    case 31: in++; if(*in) out[at++]=*in++; break;
    case 32 ... 255: out[at++]=*in++; break;
    default: in++;
  }
  out[at]=0;
  return at;
}

static inline void levels_column(int x,int y,int n,int bg,sqlite3_stmt*st,char*buf) {
  const DisplayColumn*dc=ll_disp+n;
  Uint8 co=dc->color;
  int w=dc->width;
  int nc=dc->data+(dc->flag&4?0:8);
  int t=sqlite3_column_type(st,nc);
  int a=0;
  int i;
  const char*p;
  sqlite3_int64 v;
  if(t==SQLITE_NULL) return;
  if(dc->flag&1) w=255;
  if(t==SQLITE_BLOB || t==SQLITE_TEXT) {
    blob:
    i=snprintf(buf,w,"%s",sqlite3_column_text(st,nc));
    if(dc->form[0]=='R' && i<w) a=w-i;
    if(dc->flag&2) co=0xFF;
  } else {
    // This implementation does not check that the format is necessarily valid.
    // You should not rely on the use of any undocumented format.
    v=sqlite3_column_int64(st,nc);
    if(dc->flag&2) {
      co=0xFF;
      for(i=0;i<dc->color;i++) if(ll_code[dc->ptr+i]>=0xFF00 || v+128<=(ll_code[dc->ptr+i]>>8)) {
        co=ll_code[dc->ptr+i]&0xFF;
        break;
      }
    }
    switch(dc->form[0]) {
      case 'L': case 'R':
        if(dc->form[1]) {
          if(v<0 || v>w) v=w;
          memset(buf,dc->form[1],v);
          buf[v]=0;
          if(dc->form[0]=='R') a=w-v;
        } else {
          goto blob;
        }
        break;
      case 'd':
        snprintf(buf,w+1,"%*lld",w,(long long)(dc->form[1] && v<0?-v:v));
        if(dc->form[1]=='-') *buf=v<0?'-':' ';
        if(dc->form[1]=='+') *buf=v<0?'-':'+';
        break;
      case 'o':
        snprintf(buf,w+1,dc->form[1]?"%0*llo":"%*llo",w,(unsigned long long)v);
        break;
      case 'u':
        snprintf(buf,w+1,dc->form[1]?"%0*llu":"%*llu",w,(unsigned long long)v);
        break;
      case 'x':
        snprintf(buf,w+1,dc->form[1]?"%0*llx":"%*llx",w,(unsigned long long)v);
        break;
      case 'X':
        snprintf(buf,w+1,dc->form[1]?"%0*llX":"%*llX",w,(unsigned long long)v);
        break;
      default: *buf=0;
    }
  }
  draw_text(a*8+x,y,buf,bg,co);
}

static int list_levels(void) {
  static Sint8 mo=-1;
  static Uint8 columns=0;
  static int scroll=0;
  static Uint16 divmin,divmax; // 1 less than level order number
  sqlite3_stmt*st;
  SDL_Event ev;
  SDL_Rect r;
  const char*v;
  int scrmax=0;
  int sel=level_ord-1;
  int b,i,j,x,y;
  char buf[256];
  char rescroll=2;
  Uint16 ndiv;
  if(mo<0) {
    optionquery[1]=Q_listMode;
    v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
    mo=strtol(v,0,16);
    if(mo<0) mo=0;
  }
  if(!columns) {
    optionquery[1]=Q_listColumns;
    v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
    i=strtol(v,0,10);
    j=(screen->w-16)/070;
    if(i<1) i=j;
    if(i>j) i=j;
    if(i>255) i=255;
    columns=i;
  }
  if(!ll_head) ll_head="\xB3W \xB3H \xB3 TITLE";
  // ID, ORD, CODE, WIDTH, HEIGHT, TITLE, SOLVED, SOLVABLE, ...
  prepare:
  if(sqlite3_prepare_v2(userdb,mo&4
   ?"WITH D(F,L,H,R) AS (SELECT `FIRST`, LEAD(`FIRST`,1,MAX_LEVEL()+1) OVER (ORDER BY `FIRST`,_ROWID_), `HEADING`, _ROWID_ FROM `DIVISIONS`)"
    " SELECT F, L, H, PRINTF('(%d/%d)',(SELECT COUNT() FROM `LEVELS` WHERE `ORD` >= F AND `ORD` < L AND `SOLVED`),L-F) FROM D WHERE F <> L ORDER BY F, R;"
   :"SELECT * FROM `LEVELS` WHERE `ORD` NOT NULL AND `ORD` >= ?1 AND `ORD` < ?2 ORDER BY `ORD`;"
   ,-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  if(mo&4) {
    ndiv=0;
    while(sqlite3_step(st)==SQLITE_ROW) ndiv++;
    if(!ndiv) {
      mo&=3;
      sqlite3_finalize(st);
      goto prepare;
    }
  }
  set_cursor(XC_arrow);
  redraw:
  if(sel<0) sel=0;
  if(sel>=level_nindex) sel=level_nindex-1;
  if((mo&8) && sel>=divmax-divmin) sel=divmax-divmin-1;
  SDL_FillRect(screen,0,0x02);
  r.x=r.y=0;
  r.w=screen->w;
  r.h=24;
  SDL_FillRect(screen,&r,0xF7);
  SDL_LockSurface(screen);
  draw_text(0,0,"<LMB/\x18\x19\x1A\x1B> Select  <MMB/SP> Title  <RMB/RET> Play  <0-9> Find  <ESC> Cancel",0xF7,0xF0);
  draw_text(0,8,"<F1> Wide/Tall  <F2> ID/Ord  <F3> Divisions  <F4> All",0xF7,0xF0);
  if(mo&8) draw_text(0,16,"\x15",0xF7,0xF0);
  reset:
  sqlite3_reset(st);
  sqlite3_bind_int(st,2,mo&8?divmax:level_nindex+1);
  if(mo&4) {
    scrmax=ndiv;
    for(i=0;i<scroll;i++) if(sqlite3_step(st)!=SQLITE_ROW) sqlite3_reset(st),scroll=i-1;
    for(j=0,y=24;y<screen->h-7;y+=8,j++) {
      i=sqlite3_step(st);
      if(i!=SQLITE_ROW) break;
      if(rescroll==2 && sel>=sqlite3_column_int(st,0) && sel<sqlite3_column_int(st,1)) {
        found:
        sel=j;
        rescroll=1;
        goto reset;
      }
      if(j==sel) {
        divmin=sqlite3_column_int(st,0);
        divmax=sqlite3_column_int(st,1);
        r.x=16; r.y=y;
        r.w=screen->w-16; r.h=8;
        SDL_FillRect(screen,&r,0xF8);
        draw_text(2*8,y,"\x10",b=0xF8,0xFE);
        snprintf(buf,100,"#%d  (%d - %d)",sel+1,divmin,divmax-1);
        draw_text(80,16,buf,0xF7,0xFE);
      } else {
        draw_text(2*8,y,"\xB3",b=0x02,0xF8);
      }
      if(v=sqlite3_column_text(st,2)) {
        x=sqlite3_column_bytes(st,2);
        draw_text(4*8,y,v,b,0xFF);
        if(v=sqlite3_column_text(st,3)) draw_text((x+5)*8,y,v,b,0xFB);
      }
    }
    if(rescroll) {
      if(rescroll==2 && i==SQLITE_ROW) {
        while((i=sqlite3_step(st))==SQLITE_ROW) {
          j++;
          if(sel>=sqlite3_column_int(st,0) && sel<sqlite3_column_int(st,1)) goto found;
        }
      }
      if(sel<scroll) scroll=sel;
      if(sel>=scroll+screen->h/8-3) scroll=sel+4-screen->h/8;
      rescroll=0;
    }
    if(y<screen->h-7 && sel>=j) {
      sel=j-1;
      goto reset;
    }
  } else if(mo&1) {
    scrmax=mo&8?(divmax-divmin):level_nindex;
    draw_text(16+060,16,ll_head,0xF7,0xF1);
    draw_text(16,16,mo&2?"\xB3 ID  ":"\xB3 ORD ",0xF7,0xF1);
    if(rescroll) {
      if(sel<scroll) scroll=sel;
      if(sel>=scroll+screen->h/8-3) scroll=sel+4-screen->h/8;
      rescroll=0;
    }
    sqlite3_bind_int(st,1,scroll+(mo&8?divmin:1));
    for(y=24;y<screen->h-7;y+=8) {
      if(sqlite3_step(st)!=SQLITE_ROW) break;
      i=sqlite3_column_int(st,1);
      if(mo&8) i-=divmin-1;
      if(i-1==sel) {
        r.x=16; r.y=y;
        r.w=screen->w-16; r.h=8;
        SDL_FillRect(screen,&r,0xF8);
        draw_text(2*8,y,"\x10",b=0xF8,0xFE);
      } else {
        draw_text(2*8,y,"\xB3",b=0x02,0xF8);
      }
      if(mo&8) i+=divmin-1;
      snprintf(buf,6,"%5u",mo&2?sqlite3_column_int(st,0):i);
      draw_text(3*8,y,buf,b,sqlite3_column_int(st,6)?0xFA:0xFC);
      draw_text(8*8,y,"\xB3",b,b^0xFA);
      if(ll_ndisp) {
        x=9*8;
        for(i=0;i<ll_ndisp && x<screen->w;i++) {
          levels_column(x,y,i,b,st,buf);
          x+=ll_disp[i].width*8+8;
          if(x<screen->w && !(ll_disp[i].flag&1)) draw_text(x-8,y,"\xB3",b,b^0xFA);
        }
      } else {
        snprintf(buf,6,"%2u %2u",sqlite3_column_int(st,3),sqlite3_column_int(st,4));
        draw_text(9*8,y,buf,b,0xFF);
        draw_text(11*8,y,"\xB3",b,b^0xFA);
        draw_text(14*8,y,"\xB3",b,b^0xFA);
        copy_text_to_plain(buf,255,sqlite3_column_text(st,5));
        draw_text(15*8,y,buf,b,0xFF);
      }
    }
  } else {
    scrmax=((mo&8?divmax-divmin:level_nindex)+columns-1)/columns;
    draw_text(8,16,mo&2?"(ID)":"(Ord)",0xF7,0xF1);
    if(rescroll) {
      if(sel<scroll*columns) scroll=sel/columns;
      if(sel>(scroll+screen->h/8-3)*columns) scroll=(sel+3-screen->h/8)/columns;
      rescroll=0;
    }
    sqlite3_bind_int(st,1,scroll*columns+(mo&8?divmin:1));
    for(y=24;y<screen->h-7;y+=8) for(x=0;x<columns;x++) {
      if(sqlite3_step(st)!=SQLITE_ROW) goto done;
      i=sqlite3_column_int(st,1);
      if(mo&8) i-=divmin-1;
      if(i-1==sel) {
        draw_text(x*070+16,y,"\x10",b=0xF8,0xFE);
        snprintf(buf,128,"[ ID=%u Ord=%u Code=%u Size=%ux%u ]",sqlite3_column_int(st,0),i,sqlite3_column_int(st,2),sqlite3_column_int(st,3),sqlite3_column_int(st,4));
        draw_text(80,16,buf,0xF7,0xFE);
      } else {
        draw_text(x*070+16,y," ",b=0x02,0x02);
      }
      if(mo&8) i+=divmin-1;
      snprintf(buf,7,"%5u ",mo&2?sqlite3_column_int(st,0):i);
      draw_text(x*070+24,y,buf,b,sqlite3_column_int(st,6)?0xFA:0xFC);
      if(!sqlite3_column_int(st,7)) draw_text(x*070+64,y,"\a",b,b==0xF8?0xF1:0xF9);
    }
  }
  done:
  SDL_UnlockSurface(screen);
  r.x=r.w=0; r.y=24; r.h=screen->h-24;
  scrollbar(&scroll,screen->h/8-3,scrmax,0,&r);
  SDL_Flip(screen);
  sqlite3_reset(st);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE && scrollbar(&scroll,screen->h/8-3,scrmax,&ev,&r)) goto redraw;
    switch(ev.type) {
      case SDL_MOUSEMOTION:
        set_cursor(XC_arrow);
        break;
      case SDL_KEYDOWN:
        if((ev.key.keysym.mod&KMOD_NUM) && ev.key.keysym.sym>=SDLK_KP0 && ev.key.keysym.sym<=SDLK_KP9) goto digit;
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: i=0; goto final;
          //TODO: Change the scroll to approximately the middle, instead of zero
          case SDLK_F1: scroll=0; mo^=1; rescroll=1; goto redraw;
          case SDLK_F2: mo^=2; goto redraw;
          case SDLK_F3:
            mo&=~8;
            if(mo&4) break;
            mo|=4;
            rescroll=2;
            sqlite3_finalize(st);
            goto prepare;
          case SDLK_F4:
            if(mo&8) {
              sel+=divmin-1;
              divmin=0;
            }
            if(mo&4) {
              sqlite3_finalize(st);
              mo&=3;
              sel=level_ord-1;
              goto prepare;
            }
            mo&=3;
            goto redraw;
          case SDLK_SPACE: title:
            if(mo&4) break;
            sqlite3_reset(st);
            sqlite3_bind_int(st,1,sel+1);
            if(sqlite3_step(st)==SQLITE_ROW) {
              v=sqlite3_column_text(st,5); // not Unicode, but add the null terminator
              if(v) modal_draw_popup(v);
            }
            goto redraw;
          case SDLK_0 ... SDLK_9: digit:
            if(mo&4) break;
            SDL_PushEvent(&ev);
            if(v=screen_prompt("Find?")) {
              if(!*v) goto redraw;
              i=strtol(v,0,10);
              if(mo&2) {
                for(j=0;j<level_nindex;j++) if(level_index[j]==i) sel=j;
              } else {
                if(i>0 && i<=level_nindex) sel=i-1;
              }
              rescroll=1;
            }
            goto redraw;
          case SDLK_DOWN: case SDLK_KP2: case SDLK_j:
            sel+=mo&5?1:columns;
            rescroll=1; goto redraw;
          case SDLK_UP: case SDLK_KP8: case SDLK_k:
            sel-=mo&5?1:columns;
            rescroll=1; goto redraw;
          case SDLK_LEFT: case SDLK_KP4: case SDLK_h:
            sel--;
            rescroll=1; goto redraw;
          case SDLK_RIGHT: case SDLK_KP6: case SDLK_l:
            sel++;
            rescroll=1; goto redraw;
          case SDLK_PAGEUP: case SDLK_KP9:
            sel-=(screen->h/8-3)*(mo&5?1:columns);
            scroll-=(screen->h/8-3)*(mo&5?1:columns);
            rescroll=1; goto redraw;
          case SDLK_PAGEDOWN: case SDLK_KP3:
            sel+=(screen->h/8-3)*(mo&5?1:columns);
            scroll+=(screen->h/8-3)*(mo&5?1:columns);
            rescroll=1; goto redraw;
          case SDLK_HOME: case SDLK_KP7: sel=scroll=0; rescroll=1; goto redraw;
          case SDLK_END:
            sel=mo&8?divmax-divmin-1:level_nindex-1;
            rescroll=1; goto redraw;
          case SDLK_RETURN: case SDLK_KP_ENTER: play:
            if(mo&4) {
              mo=(mo|8)&~4;
              sel=0;
              sqlite3_finalize(st);
              goto prepare;
            }
            i=1; goto final;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x>=16 && ev.button.y>=24) {
          if(mo&1) i=scroll+ev.button.y/8-3;
          else i=(scroll+ev.button.y/8-3)*columns+(ev.button.x-16)/070;
          if(i>=0 && i<level_nindex) sel=i;
          if(ev.button.button==2) goto title;
          if(ev.button.button==3) goto play;
          goto redraw;
        }
        break;
      case SDL_VIDEOEXPOSE: goto redraw;
      case SDL_QUIT: exit(0); break;
    }
  }
  i=-1;
  final:
  sqlite3_finalize(st);
  if(i==1) begin_level(mo&8?-sel-divmin:~sel);
  return i;
}

static int game_command(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux) {
  switch(cmd) {
    case '\' ': // Play a move
      if(replay_time) {
        replay_time=0;
        return -3;
      }
      if(solution_replay) {
        screen_message("You cannot play your own moves during the solution replay");
        return -3;
      }
      if(inputs_count>=inputs_size) {
        inputs=realloc(inputs,inputs_size+=32);
        if(!inputs) fatal("Allocation failed\n");
      }
      inputs[inputs_count++]=number;
      return 0;
    case '+ ': replay: // Replay
      saved_inserting=inserting; inserting=0;
      replay_time=0;
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
      saved_inserting=inserting;
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
      no_dead_anim=1;
      return 1;
    case '^<': // Rewind to mark
      number=replay_mark;
      goto restart;
    case '^>': // Replay to mark
      inputs_count=0;
      number=replay_mark-replay_pos;
      goto replay;
    case '^-': // Delete move
      inputs_count=0;
      if(solution_replay) {
        screen_message("You cannot delete moves during the solution replay");
        return -3;
      }
      if(replay_pos==replay_count) return 0;
      memmove(replay_list+replay_pos,replay_list+replay_pos+1,replay_count-replay_pos-1);
      replay_count--;
      if(replay_mark>replay_pos) replay_mark--;
      return 0;
    case '^+': // Insert moves
      if(solution_replay) return 0;
      inputs_count=0;
      inserting^=1;
      return 0;
    case '^E': // Edit
      return main_options['r']?1:-2;
    case '^I': // Toggle inventory display
      side_mode^=1;
      return prev;
    case '^L': // List levels
      return list_levels();
    case '^M': // Mark replay position
      replay_mark=replay_pos+inputs_count;
      return prev;
    case '^Q': // Quit
      return -1;
    case '^S': // Save solution
      if(gameover==1) record_solution();
      return 1;
    case '^T': // Show title
      modal_draw_popup(level_title);
      return prev;
    case '^Y': // Sound test
      sound_test();
      return prev;
    case '^d': // Describe object
      describe_at(number-65);
      return prev;
    case '^g': // Display global variables
      global_examine();
      return prev;
    case '^n': // List objects (bizarro)
      list_objects_at(number-65,bizplayfield,"bizarro ");
      return prev;
    case '^o': // List objects
      list_objects_at(number-65,playfield,"");
      return prev;
    case '^p': // Slow replay
      replay_time=replay_time?0:1;
      return 0;
    case '^s': // Toggle solution replay
      inserting=0;
      if(replay_count) save_replay();
      solution_replay^=1;
      if(replay_count) replay_count=0,begin_level(level_id); else load_replay();
      return 1;
    case '^x': // Cancel dead animation
      ndeadanim=0;
      return prev;
    case 'go': // Select level
      begin_level(number);
      return 1;
    case 'lo': // Locate me
      locate_me(number&63?:64,number/64?:64);
      return prev;
    case 'mi': // Move list import
      if(argc<2 || solution_replay) break;
      if(replay_pos) begin_level(level_id);
      do_import_moves(sqlite3_column_text(args,1));
      return 1;
    case 'ml': // Move list load
      if(argc<2 || solution_replay) break;
      if(replay_pos) begin_level(level_id);
      do_load_moves(args);
      return 1;
    case 'mx': // Move list export
      if(argc<2) break;
      do_export_moves(sqlite3_column_text(args,1));
      return 0;
    case 'rs': // Replay speed
      number+=replay_speed;
      if(number<1) number=1; else if(number>255) number=255;
      replay_speed=number;
      return prev;
    default:
      return prev;
  }
}

static void do_autowin(void) {
  const char*name;
  int i,j,k;
  int prev=0;
  sqlite3_reset(autowin);
  if(sqlite3_bind_parameter_count(autowin)) {
    for(i=sqlite3_bind_parameter_count(autowin);i;--i) if(name=sqlite3_bind_parameter_name(autowin,i)) {
      if(*name=='$') {
        if(!sqlite3_stricmp(name+1,"LEVEL")) {
          sqlite3_bind_int(autowin,i,level_ord);
        } else if(!sqlite3_stricmp(name+1,"LEVEL_ID")) {
          sqlite3_bind_int(autowin,i,level_id);
        }
      }
    }
  }
  while((i=sqlite3_step(autowin))==SQLITE_ROW) {
    if(i=sqlite3_data_count(autowin)) {
      j=(i>1&&sqlite3_column_type(autowin,1)!=SQLITE_NULL)?sqlite3_column_int(autowin,1):0;
      if((name=sqlite3_column_text(autowin,0)) && *name) {
        if(name[0]==':') {
          switch(name[1]) {
            case '!': if(i>1) i=system(sqlite3_column_text(autowin,1)?:(const unsigned char*)"# "); break;
            case ';': goto done;
            case '?': if(i>1) puts(sqlite3_column_text(autowin,1)?:(const unsigned char*)"(null)"); break;
            case 'm': if(i>1) screen_message(sqlite3_column_text(autowin,1)?:(const unsigned char*)"(null)"); break;
          }
        } else {
          k=name[0]*'\1\0'+name[1]*'\0\1';
          while(i && sqlite3_column_type(autowin,i-1)==SQLITE_NULL) i--;
          prev=game_command(prev,k,j,i,autowin,0);
          if(prev<0) goto done;
        }
      }
    }
  }
  if(i!=SQLITE_DONE) screen_message("SQL error");
  done:
  sqlite3_reset(autowin);
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
    if(inserting) {
      if(replay_pos>=0xFFFE || replay_pos==replay_count) {
        inserting=0;
      } else {
        if(replay_count>=0xFFFE) replay_count=0xFFFD;
        if(replay_size<0xFFFF) {
          replay_list=realloc(replay_list,replay_size=0xFFFF);
          if(!replay_list) fatal("Allocation failed\n");
        }
        memmove(replay_list+replay_pos+1,replay_list+replay_pos,replay_count-replay_pos);
        replay_count++;
      }
    }
    if(replay_pos>=replay_size) {
      if(replay_size>0xFDFF) replay_size=0xFDFF;
      if(replay_size+0x200<=replay_pos) fatal("Confusion in input_move function\n");
      replay_list=realloc(replay_list,replay_size+=0x200);
      if(!replay_list) fatal("Allocation failed\n");
    }
    replay_list[replay_pos++]=k;
    if(replay_pos>replay_count) replay_count=replay_pos;
  }
}

static void record_solution(void) {
  const char*v;
  const char*com;
  Uint8*data;
  Uint8*p;
  long sz;
  if(solution_replay) return;
  if(data=read_lump(FIL_SOLUTION,level_id,&sz)) {
    if(sz<3 || (data[0]|(data[1]<<8))!=level_version || (data[2]&~3)) goto dontkeep;
    sz-=3;
    if(data[2]&1) sz-=strnlen(data+3,sz);
    if(data[2]&2) sz-=8;
    if(sz<=0 || sz>replay_pos) goto dontkeep;
    free(data);
    return;
    dontkeep:
    free(data);
  }
  optionquery[1]=Q_solutionComment;
  com=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(com && !*com) com=0;
  optionquery[1]=Q_solutionTimestamp;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  data=malloc(sz=replay_pos+(boolxrm(v,0)?8:0)+(com?strlen(com)+1:0)+3);
  if(!data) fatal("Allocation failed\n");
  data[0]=level_version&255;
  data[1]=level_version>>8;
  data[2]=(boolxrm(v,0)?2:0)|(com?1:0);
  p=data+3;
  if(com) {
    strcpy(p,com);
    p+=strlen(com)+1;
  }
  if(data[2]&2) {
    time_t t=time(0);
    p[0]=t>>000; p[1]=t>>010; p[2]=t>>020; p[3]=t>>030;
    p[4]=t>>040; p[5]=t>>050; p[6]=t>>060; p[7]=t>>070;
    p+=8;
  }
  memcpy(p,replay_list,replay_pos);
  write_lump(FIL_SOLUTION,level_id,sz,data);
  free(data);
  sqlite3_exec(userdb,"UPDATE `LEVELS` SET `SOLVABLE` = 1 WHERE `ID` = LEVEL_ID();",0,0,0);
}

void run_game(void) {
  int i;
  SDL_Event ev;
  set_caption();
  replay_count=0;
  replay_time=0;
  if(side_mode==255) setup_game();
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
        goto quit;
        break;
      case SDL_MOUSEMOTION:
        show_mouse_xy(&ev);
        break;
      case SDL_USEREVENT:
        if(!gameover && !quiz_text) continue_animation();
        timerflag=0;
        if(replay_time && !--replay_time && !game_command(1,'+ ',1,0,0,0)) {
          replay_time=replay_speed;
          goto replay;
        }
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x<left_margin) {
          if(ev.button.y<48) break;
          if(side_mode) {
            // Inventory
            describe_inventory((ev.button.y-52)/picture_size);
            timerflag=0;
            redraw_game();
          } else {
            // Move list
            i=(ev.button.y+4)/16-(screen->h-68)/32-4;
            if(i<0) game_command(0,'- ',-i,0,0,0); else if(i>0) game_command(0,'+ ',i,0,0,0);
            goto replay;
          }
          break;
        } else {
          i=exec_key_binding(&ev,0,(ev.button.x-left_margin)/picture_size+1,ev.button.y/picture_size+1,game_command,0);
          goto command;
        }
      case SDL_KEYDOWN:
        i=exec_key_binding(&ev,0,0,0,game_command,0);
      command:
        if(i==-1) goto quit;
        if(i==-2) {
          main_options['e']=1;
          SDL_SetTimer(0,0);
          save_replay();
          return;
        }
      replay:
        if(inputs_count) {
          for(i=0;i<inputs_count && !gameover;i++) if(inputs[i]) input_move(inputs[i]);
          inputs_count=0;
          if(saved_inserting) inserting=1,saved_inserting=0;
          no_dead_anim=0;
          if(gameover==1) {
            if(should_record_solution) record_solution();
            if(!solution_replay && !solved) sqlite3_exec(userdb,"UPDATE `LEVELS` SET `SOLVED` = 1 WHERE `ID` = LEVEL_ID();",0,0,0);
            if(autowin) do_autowin();
          }
        }
        redraw_game();
        timerflag=0; // ensure we have not missed a timer event
        break;
    }
  }
  quit:
  SDL_SetTimer(0,0);
  save_replay();
  exit(0);
}

void run_auto_test(void) {
  Uint8 rc=0;
  int lvl,pro,i,n;
  const char*t;
  no_dead_anim=1;
  setbuf(stdout,0);
  solution_replay=1;
  optionquery[1]=Q_progress;
  t=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  pro=t?strtol(t,0,10):0;
  if(main_options['t']) pro=0;
  if(main_options['+']) {
    lvl=1;
    goto start2;
  }
  optionquery[1]=Q_level;
  t=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(n=lvl=t?strtol(t,0,10):0) goto start;
  for(n=1;n<=level_nindex;n++) {
    if(lvl) break;
    start:
    if(pro<0) sqlite3_sleep(-pro);
    if(main_options['t']) printf("*** Level %d\n",n); else printf("Level %d",n);
    if(t=load_level(-n)) {
      printf(": Error during loading: %s\n",t);
      rc=1; continue;
    }
    start2:
    load_replay();
    if(!replay_count) {
      printf(": Solution is absent, invalid, or for wrong version of this level\n");
      rc=1; continue;
    }
    if(t=init_level()) {
      printf(": Error during initialization: %s\n",t);
      rc=1; continue;
    }
    if(gameover==-1) {
      printf(": Lose during initialization\n");
      rc=1; continue;
    }
    for(i=0;i<replay_count;i++) {
      if(gameover) {
        printf(": Premature termination on move %d\n",i);
        rc=1; goto cont;
      }
      if(pro>0 && !(i%pro)) putchar('.');
      if(t=execute_turn(replay_list[i])) {
        printf(": Error on move %d: %s\n",i+1,t);
        rc=1; goto cont;
      }
      if(gameover==-1) {
        printf(": Game loss on move %d\n",i+1);
        rc=1; goto cont;
      }
    }
    if(gameover<=0) {
      printf(": Game not terminated after %d moves\n",replay_count);
      rc=1; continue;
    }
    printf(": OK\n");
    cont: ;
  }
  exit(rc);
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
