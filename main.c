#if 0
gcc -s -O2 -o ~/bin/heromesh main.c smallxrm.o sqlite3.o `sdl-config --cflags --libs`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define _BSD_SOURCE
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
#include "pcfont.h"

static const char schema[]=
  "PRAGMA APPLICATION_ID(1296388936);"
  "PRAGMA RECURSIVE_TRIGGERS(1);"
  "CREATE TABLE IF NOT EXISTS `USERCACHEINDEX`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `LVLTIME` INT, `SOLTIME` INT, `VERSION` INT);"
  "CREATE TEMPORARY TABLE `PICTURES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `OFFSET` INT);"
;

static sqlite3*userdb;
static SDL_Cursor*cursor[77];
static xrm_db*resourcedb;
static SDL_Surface*screen;
static const char*basefilename;
static const char*globalclassname;
static FILE*levelfp;
static FILE*solutionfp;
static xrm_quark optionquery[16];
static FILE*hamarc_fp;
static long hamarc_pos;
static Uint16 picture_size;
static SDL_Surface*picture_surface;

#define fatal(...) do{ fprintf(stderr,__VA_ARGS__); exit(1); }while(0)
#define boolxrm(a,b) (*a=='1'||*a=='y'||*a=='t'||*a=='Y'||*a=='T'?1:*a=='0'||*a=='n'||*a=='f'||*a=='N'||*a=='F'?0:b)

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
  v=getenv("HOME")?:".";
  s=sqlite3_mprintf("%s%s.heromeshsession",v,v[strlen(v)-1]=='/'?"":"/");
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

static void draw_picture(int x,int y,Uint16 img) {
  // To be called only when screen is unlocked!
  SDL_Rect src={(img&15)*picture_size,(img>>4)*picture_size,picture_size,picture_size};
  SDL_Rect dst={x,y,picture_size,picture_size};
  SDL_BlitSurface(picture_surface,&src,screen,&dst);
}

static void draw_text(int x,int y,const unsigned char*t,int bg,int fg) {
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
    for(yy=0;yy<8;yy++) {
      for(xx=0;xx<8;xx++) p[xx]=(*f<<xx)&128?fg:bg;
      p+=pitch;
      ++f;
    }
    t++;
    if(!--len) return;
  }
}

static Uint16 decide_picture_size(int nwantsize,const Uint8*wantsize,const Uint16*havesize) {
  
}

static void load_pictures(void) {
  sqlite3_stmt*st=0;
  FILE*fp;
  Uint8 wantsize[32];
  Uint8 nwantsize=0;
  Uint8 altImage;
  Uint16 havesize[256];
  char*nam=sqlite3_mprintf("%s.xclass",basefilename);
  const char*v;
  int i,j,n;
  if(!nam) fatal("Allocation failed\n");
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
  sqlite3_exec(userdb,"BEGIN;",0,0,0);
  if(sqlite3_prepare(userdb,"INSERT INTO `PICTURES`(`ID`,`NAME`,`OFFSET`) VALUES(?1,?2,?3);",-1,&st,0))
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
    if(n++==32768) fatal("Too many pictures\n");
    sqlite3_reset(st);
    sqlite3_bind_int(st,1,n);
    sqlite3_bind_text(st,2,nam,i,SQLITE_TRANSIENT);
    sqlite3_bind_int64(st,3,ftell(fp)+4);
    while((i=sqlite3_step(st))==SQLITE_ROW);
    if(i!=SQLITE_DONE) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
    i=fgetc(fp)<<16;
    i|=fgetc(fp)<<24;
    i|=fgetc(fp)<<0;
    i|=fgetc(fp)<<8;
    i-=j=fgetc(fp)&15;
    while(j--) ++havesize[fgetc(fp)&255];
    fseek(fp,i-1,SEEK_CUR);
  }
nomore1:
  free(nam);
  sqlite3_finalize(st);
  rewind(fp);
  for(i=0;i<256;i++) havesize[i]=(havesize[i]==n)?1:0;
  picture_size=decide_picture_size(nwantsize,wantsize,havesize);
  
  fclose(fp);
  sqlite3_exec(userdb,"COMMIT;",0,0,0);
}

static void set_cursor(int id) {
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

int main(int argc,char**argv) {
  if(argc<2) fatal("usage: %s basename [options...]\n",argc?argv[0]:"heromesh");
  if(xrm_init(realloc)) fatal("Failed to initialize resource manager\n");
  if(xrm_init_quarks(global_quarks)) fatal("Failed to initialize resource manager\n");
  resourcedb=xrm_create();
  if(!resourcedb) fatal("Allocation of resource database failed\n");
  basefilename=argv[1];
  globalclassname=basefilename; //TODO: Fix this
  load_options();
  if(argc>2) read_options(argc-2,argv+2);
  *optionquery=xrm_make_quark(globalclassname,0)?:xrm_anyq;
  init_sql();
  
  set_cursor(XC_arrow);
  
  atexit(SDL_Quit);
  
  return 0;
}
