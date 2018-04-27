#if 0
gcc -s -O2 -o ~/bin/heromesh main.c class.o picture.o bindings.o smallxrm.o sqlite3.o `sdl-config --cflags --libs` -ldl -lpthread
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define _BSD_SOURCE
#define HEROMESH_MAIN
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "names.h"
#include "quarks.h"
#include "cursorshapes.h"
#include "heromesh.h"

static const char schema[]=
  "BEGIN;"
  "PRAGMA APPLICATION_ID(1296388936);"
  "PRAGMA RECURSIVE_TRIGGERS(1);"
  "CREATE TABLE IF NOT EXISTS `USERCACHEINDEX`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `TIME` INT);"
  "CREATE TABLE IF NOT EXISTS `USERCACHEDATA`(`ID` INTEGER PRIMARY KEY, `FILE` INT, `LEVEL` INT, `NAME` TEXT, `OFFSET` INT, `DATA` BLOB, `USERSTATE` BLOB);"
  "CREATE INDEX IF NOT EXISTS `USERCACHEDATA_I1` ON `USERCACHEDATA`(`FILE`, `LEVEL`) WHERE `LEVEL` IS NOT NULL;"
  "CREATE TEMPORARY TABLE `PICTURES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `OFFSET` INT);"
  "COMMIT;"
;

sqlite3*userdb;
xrm_db*resourcedb;
const char*basefilename;
xrm_quark optionquery[16];
Uint32 generation_number;
char main_options[128];

static const char*globalclassname;
static SDL_Cursor*cursor[77];
static FILE*levelfp;
static FILE*solutionfp;
static FILE*hamarc_fp;
static long hamarc_pos;

static void hamarc_begin(FILE*fp,const char*name) {
  while(*name) fputc(*name++,fp);
  fwrite("\0\0\0\0",1,5,hamarc_fp=fp);
  hamarc_pos=ftell(fp);
}

static long hamarc_end(void) {
  long end=ftell(hamarc_fp);
  long len=end-hamarc_pos;
  fseek(hamarc_fp,hamarc_pos-4,SEEK_SET);
  fputc(len>>16,hamarc_fp);
  fputc(len>>24,hamarc_fp);
  fputc(len>>0,hamarc_fp);
  fputc(len>>8,hamarc_fp);
  fseek(hamarc_fp,end,SEEK_SET);
}

static void init_sql(void) {
  char*s;
  char*p;
  const char*v;
  int z;
  sqlite3_config(SQLITE_CONFIG_URI,0);
  optionquery[1]=Q_sqlMemStatus;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  sqlite3_config(SQLITE_CONFIG_MEMSTATUS,(int)boolxrm(v,0));
  optionquery[1]=Q_sqlSmallAllocations;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  sqlite3_config(SQLITE_CONFIG_SMALL_MALLOC,(int)boolxrm(v,0));
  optionquery[1]=Q_sqlCoveringIndexScan;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  sqlite3_config(SQLITE_CONFIG_COVERING_INDEX_SCAN,(int)boolxrm(v,1));
  if(sqlite3_initialize()) fatal("Failure to initialize SQLite.\n");
  optionquery[1]=Q_sqlFile;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(v && *v) {
    s=sqlite3_mprintf("%s",v);
  } else {
    v=getenv("HOME")?:".";
    s=sqlite3_mprintf("%s%s.heromeshsession",v,v[strlen(v)-1]=='/'?"":"/");
  }
  if(!s) fatal("Allocation failed\n");
  if(z=sqlite3_open(s,&userdb)) fatal("Failed to open user database %s (%s)\n",s,userdb?sqlite3_errmsg(userdb):sqlite3_errstr(z));
  sqlite3_free(s);
  optionquery[1]=Q_sqlExtensions;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  sqlite3_db_config(userdb,SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION,*v?1:0,&z);
  if(*v) {
    p=s=strdup(v);
    if(!s) fatal("Allocation failed\n");
    while(*v) {
      if(*v==' ') {
        *p=0;
        if(*s) {
          p=0;
          if(sqlite3_load_extension(userdb,s,0,&p)) fatal("Failed to load extension '%s' (%s)\n",s,p?:"unknown error");
          p=s;
        }
        v++;
      } else {
        *p++=*v++;
      }
    }
    *p=0;
    p=0;
    if(*s && sqlite3_load_extension(userdb,s,0,&p)) fatal("Failed to load extension '%s' (%s)\n",s,p?:"unknown error");
    free(s);
  }
  if(sqlite3_exec(userdb,schema,0,0,&s)) fatal("Failed to initialize database schema (%s)\n",s?:"unknown error");
  optionquery[1]=Q_sqlInit;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(v && sqlite3_exec(userdb,v,0,0,&s)) fatal("Failed to execute user-defined SQL statements (%s)\n",s?:"unknown error");
}

void set_cursor(int id) {
  id>>=1;
  if(!cursor[id]) cursor[id]=SDL_CreateCursor((void*)cursorimg+(id<<6),(void*)cursorimg+(id<<6)+32,16,16,cursorhot[id]>>4,cursorhot[id]&15);
  SDL_SetCursor(cursor[id]);
}

static void load_options(void) {
  const char*home=getenv("HOME")?:".";
  char*nam=malloc(strlen(home)+16);
  FILE*fp;
  sprintf(nam,"%s%s.heromeshrc",home,home[strlen(home)-1]=='/'?"":"/");
  fp=fopen(nam,"r");
  if(!fp) fatal("Failed to open %s (%m)\n",nam);
  free(nam);
  if(xrm_load(resourcedb,fp,1)) fatal("Error while loading .heromeshrc\n");
  fclose(fp);
}

static void read_options(int argc,char**argv) {
  xrm_db*db=xrm_sub(resourcedb,0,xrm_make_quark(globalclassname,0)?:xrm_anyq);
  while(argc--) xrm_load_line(db,*argv++,1);
}

static int find_globalclassname(void) {
  char*s=malloc(strlen(basefilename)+7);
  FILE*fp;
  if(!s) fatal("Allocation failed\n");
  sprintf(s,"%s.name",basefilename);
  fp=fopen(s,"r");
  free(s);
  if(!fp) return 1;
  s=malloc(256);
  if(!s) fatal("Allocation failed\n");
  if(fscanf(fp," %255s",s)!=1) fatal("Unable to scan name of class set\n");
  globalclassname=s;
  return !*s;
}

static int test_sql_callback(void*usr,int argc,char**argv,char**name) {
  int i;
  if(argc) printf("%s",*argv);
  for(i=1;i<argc;i++) printf("|%s",argv[i]);
  putchar('\n');
  return 0;
}

static void test_mode(void) {
  Uint32 n=0;
  SDLKey k;
  SDL_Event ev;
  char buf[32];
  const UserCommand*uc;
  set_cursor(XC_tcross);
  SDL_LockSurface(screen);
  draw_text(0,0,"Hello, World!",0xF0,0xFF);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      switch(ev.key.keysym.sym) {
        case SDLK_BACKSPACE:
          n/=10;
          snprintf(buf,30,"%u",n);
          SDL_WM_SetCaption(buf,buf);
          break;
        case SDLK_SPACE:
          n=0;
          SDL_WM_SetCaption("0","0");
          break;
        case SDLK_0 ... SDLK_9:
          n=10*n+ev.key.keysym.sym-SDLK_0;
          snprintf(buf,30,"%u",n);
          SDL_WM_SetCaption(buf,buf);
          break;
        case SDLK_c:
          SDL_FillRect(screen,0,n);
          SDL_Flip(screen);
          break;
        case SDLK_e:
          n=1;
          goto keytest;
        case SDLK_g:
          n=0;
          goto keytest;
        case SDLK_p:
          sqlite3_exec(userdb,"SELECT * FROM `PICTURES`;",test_sql_callback,0,0);
          break;
        case SDLK_q:
          exit(0);
          break;
      }
      break;
    case SDL_MOUSEBUTTONDOWN:
      draw_picture(ev.button.x,ev.button.y,n);
      SDL_Flip(screen);
      break;
    case SDL_QUIT:
      exit(0);
      break;
  }
  fatal("An error occurred waiting for events.\n");
keytest:
  SDL_FillRect(screen,0,0xF0);
  SDL_LockSurface(screen);
  draw_text(1,5,n?"Edit Key":"Game Key",0xF1,0xF7);
  SDL_UnlockSurface(screen);
  SDL_EnableUNICODE(1);
  SDL_Flip(screen);
  set_cursor(XC_arrow);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      printf("[%d %d %d %d] ",ev.key.keysym.scancode,ev.key.keysym.sym,ev.key.keysym.mod,ev.key.keysym.unicode);
      goto bindingtest;
    case SDL_MOUSEBUTTONDOWN:
      printf("[%d %d %d] ",ev.button.x,ev.button.y,ev.button.button);
    bindingtest:
      uc=find_key_binding(&ev,n);
      switch(uc->cmd) {
        case 0: printf("<Unbound>\n"); break;
        case '^': printf("<Misc> %c\n",uc->n); break;
        case '=': printf("<Reset> %d\n",uc->n); break;
        case '-': printf("<Rewind> %d\n",uc->n); break;
        case '+': printf("<Advance> %d\n",uc->n); break;
        case '\'': printf("<Play> %s (%d)\n",heromesh_key_names[uc->n],uc->n); break;
        case '!': printf("<System> %s",uc->txt); break;
        case 's': printf("<SQL> %s",sqlite3_sql(uc->stmt)); break;
        default: printf("<Unknown>\n");
      }
      break;
    case SDL_QUIT:
      exit(0);
      break;
  }
}

int main(int argc,char**argv) {
  int optind=1;
  fprintf(stderr,"FREE HERO MESH\n");
  while(argc>optind && argv[optind][0]=='-') {
    int i;
    const char*s=argv[optind++];
    if(s[1]=='-' && !s[2]) break;
    for(i=1;s[i];i++) main_options[s[i]&127]=1;
  }
  if(argc<=optind) fatal("usage: %s [switches] [--] basename [options...]\n",argc?argv[0]:"heromesh");
  if(xrm_init(realloc)) fatal("Failed to initialize resource manager\n");
  if(xrm_init_quarks(global_quarks)) fatal("Failed to initialize resource manager\n");
  resourcedb=xrm_create();
  if(!resourcedb) fatal("Allocation of resource database failed\n");
  basefilename=argv[optind++];
  if(argc>optind && argv[1][0]=='=') {
    globalclassname=argv[optind++]+1;
  } else if(find_globalclassname()) {
    globalclassname=strrchr(basefilename,'/');
    globalclassname=globalclassname?globalclassname+1:basefilename;
  }
  load_options();
  if(argc>optind) read_options(argc-optind,argv+optind);
  *optionquery=xrm_make_quark(globalclassname,0)?:xrm_anyq;
  init_sql();
  load_key_bindings();
  init_screen();
  load_pictures();
  if(main_options['T']) {
    test_mode();
    return 0;
  }
  load_classes();
  
  return 0;
}
