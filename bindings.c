#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-multichar bindings.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define HEROMESH_BINDINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "SDL.h"
#include "sqlite3.h"
#include "smallxrm.h"
#include "quarks.h"
#include "heromesh.h"

#define MOD_SHIFT 1
#define MOD_CTRL 2
#define MOD_ALT 4
#define MOD_META 8
#define MOD_NUMLOCK 14
typedef struct {
  UserCommand m[16];
} KeyBinding;

static KeyBinding*editor_bindings[SDLK_LAST];
static KeyBinding*game_bindings[SDLK_LAST];
static KeyBinding*editor_mouse_bindings[4];
static KeyBinding*game_mouse_bindings[4];
static int cur_modifiers,loose_modifiers;

static void set_binding(KeyBinding**pkb,int mod,const char*txt) {
  int i;
  KeyBinding*kb=*pkb;
  UserCommand*uc;
  if(!*txt) return;
  if(!kb) kb=*pkb=calloc(1,sizeof(KeyBinding));
  if(!kb) fatal("Allocation failed\n");
  uc=kb->m+mod;
  if(uc->cmd) return;
  switch(*txt) {
    case '^': // Miscellaneous
      uc->cmd='^';
      uc->n=txt[1];
      break;
    case '=': case '-': case '+': // Restart, rewind, advance
      uc->cmd=*txt;
      uc->n=strtol(txt+1,0,0);
      break;
    case '\'': // Input move
      uc->cmd='\'';
      for(i=1;i<256;i++) if(heromesh_key_names[i] && !strcmp(txt+1,heromesh_key_names[i])) {
        uc->n=i;
        return;
      }
      fatal("Error in key binding:  %s\nInvalid Hero Mesh key name\n",txt);
      break;
    case '!': // System command
      uc->cmd='!';
      uc->txt=txt+1;
      break;
    case 'A' ... 'Z': case 'a' ... 'z': // Execute SQL statement
      uc->cmd='s';
      if(i=sqlite3_prepare_v3(userdb,txt,-1,SQLITE_PREPARE_PERSISTENT,&uc->stmt,0)) fatal("Error in key binding:  %s\n%s\n",txt,sqlite3_errmsg(userdb));
      break;
    default:
      fatal("Error in key binding:  %s\nUnrecognized character\n",txt);
  }
}

#define BINDING_MODIFIER(a,b) \
  if(q==a && !(cur_modifiers&b)) { \
    cur_modifiers|=b; \
    xrm_enumerate(sub,cb_2,usr); \
    cur_modifiers&=~b; \
    if(loose) --loose_modifiers; \
    return 0; \
  }

static void*cb_2(xrm_db*db,void*usr,int loose,xrm_quark q) {
  KeyBinding**kb=usr;
  const char*txt;
  xrm_db*sub=xrm_sub(db,loose,q);
  if(loose) ++loose_modifiers;
  BINDING_MODIFIER(Q_shift,MOD_SHIFT);
  BINDING_MODIFIER(Q_ctrl,MOD_CTRL);
  BINDING_MODIFIER(Q_alt,MOD_ALT);
  BINDING_MODIFIER(Q_meta,MOD_META);
  BINDING_MODIFIER(Q_numLock,MOD_NUMLOCK);
  if(txt=xrm_get(sub)) {
    if(usr==editor_mouse_bindings || usr==game_mouse_bindings) {
      switch(q) {
        case Q_left: kb+=SDL_BUTTON_LEFT; break;
        case Q_middle: kb+=SDL_BUTTON_MIDDLE; break;
        case Q_right: kb+=SDL_BUTTON_RIGHT; break;
        default: goto stop;
      }
    } else {
      if(q>=FirstKeyQuark && q<=LastKeyQuark) kb+=quark_to_key[q-FirstKeyQuark]; else goto stop;
    }
    set_binding(kb,cur_modifiers,txt);
    if(loose_modifiers) {
      int i;
      for(i=0;i<16;i++) if((i&cur_modifiers)==cur_modifiers && !kb[0]->m[i].cmd) kb[0]->m[i]=kb[0]->m[cur_modifiers];
    }
  }
stop:
  if(loose) --loose_modifiers;
  return 0;
}

static void*cb_1(xrm_db*db,void*usr) {
  xrm_enumerate(db,cb_2,usr);
  return 0;
}

void load_key_bindings(void) {
  fprintf(stderr,"Loading key bindings...\n");
  cur_modifiers=loose_modifiers=0;
  optionquery[1]=Q_editKey;
  xrm_search(resourcedb,optionquery,optionquery,2,cb_1,editor_bindings);
  optionquery[1]=Q_gameKey;
  xrm_search(resourcedb,optionquery,optionquery,2,cb_1,game_bindings);
  optionquery[1]=Q_editClick;
  xrm_search(resourcedb,optionquery,optionquery,2,cb_1,editor_mouse_bindings);
  optionquery[1]=Q_gameClick;
  xrm_search(resourcedb,optionquery,optionquery,2,cb_1,game_mouse_bindings);
  fprintf(stderr,"Done\n");
}

const UserCommand*find_key_binding(SDL_Event*ev,int editing) {
  KeyBinding*kb;
  static const UserCommand nul={cmd:0};
  SDLMod m;
  int i;
  if(ev->type==SDL_KEYDOWN) {
    m=ev->key.keysym.mod;
    if(ev->key.keysym.sym>=SDLK_LAST) return &nul;
    kb=(editing?editor_bindings:game_bindings)[ev->key.keysym.sym];
  } else if(ev->type==SDL_MOUSEBUTTONDOWN) {
    m=SDL_GetModState();
    if(ev->button.button>3) return &nul;
    kb=(editing?editor_mouse_bindings:game_mouse_bindings)[ev->button.button];
  } else {
    return &nul;
  }
  if(!kb) return &nul;
  i=0;
  if(m&KMOD_CTRL) i|=MOD_CTRL;
  if(m&KMOD_SHIFT) i|=MOD_SHIFT;
  if(m&KMOD_ALT) i|=MOD_ALT;
  if(m&KMOD_META) i|=MOD_META;
  if((m&KMOD_NUM) && kb->m[i|MOD_NUMLOCK].cmd) i|=MOD_NUMLOCK;
  return kb->m+i;
}

int exec_key_binding(SDL_Event*ev,int editing,int x,int y,int(*cb)(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux),void*aux) {
  const UserCommand*cmd=find_key_binding(ev,editing);
  int prev=0;
  int i,j,k;
  const char*name;
  if(ev->type==SDL_MOUSEBUTTONDOWN && !x && !y && ev->button.x>=left_margin) {
    x=(ev->button.x-left_margin)/picture_size+1;
    y=ev->button.y/picture_size+1;
    if(x<1 || y<1 || x>pfwidth || y>pfheight) return 0;
  }
  switch(cmd->cmd) {
    case 0:
      return 0;
    case '^':
      return cb(0,cmd->n*'\0\1'+'^\0',y*64+x,0,0,aux);
    case '=': case '-': case '+':
      return cb(0,cmd->cmd*'\1\0'+'\0 ',cmd->n,0,0,aux);
    case '\'':
      return cb(0,'\' ',cmd->n,0,0,aux);
    case '!':
      i=system(cmd->txt);
      return 0;
    case 's':
      sqlite3_reset(cmd->stmt);
      sqlite3_clear_bindings(cmd->stmt);
      if(sqlite3_bind_parameter_count(cmd->stmt)) {
        for(i=sqlite3_bind_parameter_count(cmd->stmt);i;--i) if(name=sqlite3_bind_parameter_name(cmd->stmt,i)) {
          if(*name=='$') {
            if(!sqlite3_stricmp(name+1,"X")) {
              if(x) sqlite3_bind_int(cmd->stmt,i,x);
            } else if(!sqlite3_stricmp(name+1,"Y")) {
              if(y) sqlite3_bind_int(cmd->stmt,i,y);
            } else if(!sqlite3_stricmp(name+1,"LEVEL")) {
              sqlite3_bind_int(cmd->stmt,i,level_ord);
            } else if(!sqlite3_stricmp(name+1,"LEVEL_ID")) {
              sqlite3_bind_int(cmd->stmt,i,level_id);
            }
          } else if(*name==':') {
            name=screen_prompt(name);
            if(!name) return 0;
            sqlite3_bind_text(cmd->stmt,i,name,-1,SQLITE_TRANSIENT);
          }
        }
      }
      while((i=sqlite3_step(cmd->stmt))==SQLITE_ROW) {
        if(i=sqlite3_data_count(cmd->stmt)) {
          j=(i>1&&sqlite3_column_type(cmd->stmt,1)!=SQLITE_NULL)?sqlite3_column_int(cmd->stmt,1):y*64+x;
          if((name=sqlite3_column_text(cmd->stmt,0)) && *name) {
            k=name[0]*'\1\0'+name[1]*'\0\1';
            while(i && sqlite3_column_type(cmd->stmt,i-1)==SQLITE_NULL) i--;
            prev=cb(prev,k,j,i,cmd->stmt,aux);
            if(prev<0) {
              i=SQLITE_DONE;
              break;
            }
          }
        }
      }
      sqlite3_reset(cmd->stmt);
      sqlite3_clear_bindings(cmd->stmt);
      if(i!=SQLITE_DONE) fprintf(stderr,"SQL error in key binding: %s\n",sqlite3_errmsg(userdb)?:"unknown error");
      return prev;
    default:
      fprintf(stderr,"Confusion in exec_key_binding()\n");
      return 0;
  }
}
