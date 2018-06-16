#if 0
gcc ${CFLAGS:--s -O2} -o ${EXE:-~/bin/heromesh} main.c class.o picture.o bindings.o function.o smallxrm.o sqlite3.o `sdl-config --cflags --libs` -ldl -lpthread
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define _BSD_SOURCE
#define HEROMESH_MAIN
#include "SDL.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
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
  "CREATE TABLE IF NOT EXISTS `USERCACHEDATA`(`ID` INTEGER PRIMARY KEY, `FILE` INT, `LEVEL` INT, `NAME` TEXT COLLATE NOCASE, `OFFSET` INT, `DATA` BLOB, `USERSTATE` BLOB);"
  "CREATE UNIQUE INDEX IF NOT EXISTS `USERCACHEDATA_I1` ON `USERCACHEDATA`(`FILE`, `LEVEL`);"
  "CREATE TEMPORARY TABLE `PICTURES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT COLLATE NOCASE, `OFFSET` INT);"
  "CREATE TEMPORARY TABLE `VARIABLES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT);"
  "COMMIT;"
;

sqlite3*userdb;
xrm_db*resourcedb;
const char*basefilename;
xrm_quark optionquery[16];
Uint32 generation_number;
char main_options[128];
Uint8 message_trace[0x4100/8];

static const char*globalclassname;
static SDL_Cursor*cursor[77];
static FILE*levelfp;
static FILE*solutionfp;
static sqlite3_int64 leveluc,solutionuc;
static sqlite3_stmt*readusercachest;

static sqlite3_int64 reset_usercache(FILE*fp,const char*nam,struct stat*stats,const char*suffix) {
  sqlite3_stmt*st;
  sqlite3_int64 t,id;
  char buf[128];
  int i,z;
  if(z=sqlite3_prepare_v2(userdb,"DELETE FROM `USERCACHEDATA` WHERE `FILE` = (SELECT `ID` FROM `USERCACHEINDEX` WHERE `NAME` = ?1);",-1,&st,0)) {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  sqlite3_bind_text(st,1,nam,-1,0);
  while((z=sqlite3_step(st))==SQLITE_ROW);
  if(z!=SQLITE_DONE) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
  if(z=sqlite3_prepare_v2(userdb,"DELETE FROM `USERCACHEINDEX` WHERE `NAME` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  sqlite3_bind_text(st,1,nam,-1,0);
  while((z=sqlite3_step(st))==SQLITE_ROW);
  if(z!=SQLITE_DONE) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
  t=stats->st_mtime;
  if(stats->st_ctime>t) t=stats->st_ctime;
  if(z=sqlite3_prepare_v2(userdb,"INSERT INTO `USERCACHEINDEX`(`NAME`,`TIME`) VALUES(?1,?2);",-1,&st,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  sqlite3_bind_text(st,1,nam,-1,0);
  sqlite3_bind_int64(st,2,t);
  while((z=sqlite3_step(st))==SQLITE_ROW);
  if(z!=SQLITE_DONE) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  id=sqlite3_last_insert_rowid(userdb);
  sqlite3_finalize(st);
  if(z=sqlite3_prepare_v2(userdb,"INSERT INTO `USERCACHEDATA`(`FILE`,`LEVEL`,`NAME`,`OFFSET`) VALUES(?1,?2,?3,?4);",-1,&st,0)) {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  sqlite3_bind_int64(st,1,id);
  rewind(fp);
  for(;;) {
    sqlite3_reset(st);
    sqlite3_bind_null(st,3);
    i=0;
    for(;;) {
      z=fgetc(fp);
      if(z==EOF) goto done;
      buf[i]=z;
      if(!z) break;
      ++i;
      if(i==127) fatal("Found a long lump name; maybe this is not a real Hamster archive\n");
    }
    t=fgetc(fp)<<16; t|=fgetc(fp)<<24; t|=fgetc(fp); t|=fgetc(fp)<<8;
    if(feof(fp)) fatal("Invalid Hamster archive\n");
    if(t<0) fatal("Invalid lump size\n");
    sqlite3_bind_text(st,3,buf,i,0);
    sqlite3_bind_int64(st,4,ftell(fp));
    if(i>4 && i<10 && !sqlite3_stricmp(buf+i-4,suffix)) {
      for(z=0;z<i-4;z++) if(buf[z]<'0' || buf[z]>'9') goto nomatch;
      if(*buf=='0' && i!=5) goto nomatch;
      sqlite3_bind_int(st,2,strtol(buf,0,10));
    } else if(i==9 && suffix[1]=='L' && !sqlite3_stricmp(buf,"CLASS.DEF")) {
      sqlite3_bind_int(st,2,LUMP_CLASS_DEF);
    } else if(i==9 && suffix[1]=='L' && !sqlite3_stricmp(buf,"LEVEL.IDX")) {
      sqlite3_bind_int(st,2,LUMP_LEVEL_IDX);
    } else {
      nomatch: sqlite3_bind_null(st,2);
    }
    while((z=sqlite3_step(st))==SQLITE_ROW);
    if(z!=SQLITE_DONE) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
    fseek(fp,t,SEEK_CUR);
  }
  done:
  sqlite3_finalize(st);
  return id;
}

unsigned char*read_lump(int sol,int lvl,long*sz,sqlite3_value**us) {
  // Returns a pointer to the data; must be freed using free().
  // If there is no data, returns null and sets *sz and *us to zero.
  // Third argument is a pointer to a variable to store the data size (must be not null).
  // Fourth argument may be null, and is user state (use sqlite3_value_free() to free it).
  unsigned char*buf=0;
  sqlite3_reset(readusercachest);
  sqlite3_bind_int64(readusercachest,1,sol?solutionuc:leveluc);
  sqlite3_bind_int(readusercachest,2,lvl);
  if(sqlite3_step(readusercachest)==SQLITE_ROW) {
    if(us) *us=sqlite3_value_dup(sqlite3_column_value(readusercachest,6));
    if(sqlite3_column_type(readusercachest,5)==SQLITE_BLOB) {
      const unsigned char*con=sqlite3_column_blob(readusercachest,5);
      *sz=sqlite3_column_bytes(readusercachest,5);
      buf=malloc(*sz);
      if(*sz && !buf) fatal("Allocation failed");
      memcpy(buf,con,*sz);
    } else {
      FILE*fp=sol?solutionfp:levelfp;
      rewind(fp);
      fseek(fp,sqlite3_column_int64(readusercachest,4)-4,SEEK_SET);
      *sz=fgetc(fp)<<16; *sz|=fgetc(fp)<<24; *sz|=fgetc(fp); *sz|=fgetc(fp)<<8;
      if(feof(fp) || *sz<0) fatal("Invalid Hamster archive\n");
      buf=malloc(*sz);
      if(!buf) fatal("Allocation failed\n");
      if(!fread(buf,1,*sz,fp)) fatal("Unable to read data\n");
      rewind(fp);
    }
  } else {
    *sz=0;
    if(us) *us=0;
  }
  sqlite3_reset(readusercachest);
  return buf;
}

void write_lump(int sol,int lvl,long sz,const unsigned char*data) {
  // Writes a lump to the user cache.
  // The actual Hamster archive files will be updated when the program terminates.
  sqlite3_stmt*st;
  int e;
  if(e=sqlite3_prepare_v2(userdb,"INSERT INTO `USERCACHEDATA`(`FILE`,`LEVEL`,`NAME`,`DATA`) VALUES(?1,?2,CASE WHEN ?2 < 0 THEN ?3 ELSE ?2 || ?3 END,?4)"
   " ON CONFLICT(`FILE`,`LEVEL`) DO UPDATE SET `DATA` = ?4;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,sol?solutionuc:leveluc);
  sqlite3_bind_int(st,2,lvl);
  sqlite3_bind_text(st,3,lvl==LUMP_CLASS_DEF?"CLASS.DEF":lvl==LUMP_LEVEL_IDX?"LEVEL.IDX":sol?".SOL":".LVL",-1,SQLITE_STATIC);
  sqlite3_bind_blob64(st,4,data,sz,0);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
}

static void flush_usercache_1(int sol) {
  sqlite3_int64 uc_id=sol?solutionuc:leveluc;
  FILE*fp=sol?solutionfp:levelfp;
  int fd=fileno(fp);
  sqlite3_stmt*st;
  sqlite3_int64 of,of2;
  sqlite3_int64*ofs;
  int e,i,j,c;
  if(fd<0) fatal("Unable to flush user cache. Expected file descriptor number; found %d\n",fd);
  if(e=sqlite3_prepare_v2(userdb,"SELECT `OFFSET`, `OFFSET`-LENGTH(`NAME`)-5 FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `DATA` NOT NULL ORDER BY `OFFSET` LIMIT 1;",-1,&st,0))
   fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  e=sqlite3_step(st);
  if(e==SQLITE_ROW) {
    of=sqlite3_column_int64(st,0);
    of2=sqlite3_column_int64(st,1);
  } else if(e==SQLITE_DONE) {
    // There is nothing to do; nothing has been changed.
    sqlite3_finalize(st);
    return;
  } else {
    fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  }
  sqlite3_finalize(st);
  if(e=sqlite3_prepare_v2(userdb,"UPDATE `USERCACHEDATA` SET `DATA` = READ_LUMP_AT(`OFFSET`,?2) WHERE `FILE` = ?1 AND `OFFSET` > ?3 AND `DATA` IS NULL;",-1,&st,0))
   fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  sqlite3_bind_pointer(st,2,fp,"http://zzo38computer.org/fossil/heromesh.ui#FILE_ptr",0);
  sqlite3_bind_int64(st,3,of);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
  if(e=sqlite3_prepare_v2(userdb,"SELECT COUNT() FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `DATA` NOT NULL;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  if((e=sqlite3_step(st))!=SQLITE_ROW) fatal("SQL error (%d): %s\n",e,e==SQLITE_DONE?"Expected a result row; got nothing":sqlite3_errmsg(userdb));
  ofs=malloc((c=sqlite3_column_int(st,0)<<1)*sizeof(sqlite3_int64));
  sqlite3_finalize(st);
  if(c<=0) fatal("Unexpected error: COUNT() returned zero or negative even though there used to be some data\n");
  if(!ofs) fatal("Allocation failed\n");
  if(e=sqlite3_prepare_v2(userdb,"SELECT `ID`, `NAME`, `DATA` FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `DATA` NOT NULL ORDER BY `ID`;",-1,&st,0))
   fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  rewind(fp);
  fseek(fp,of2,SEEK_SET);
  i=0;
  while((e=sqlite3_step(st))==SQLITE_ROW) {
    ofs[i++]=sqlite3_column_int64(st,0);
    if(sqlite3_column_type(st,1)!=SQLITE_TEXT || sqlite3_column_type(st,2)!=SQLITE_BLOB) fatal("Corrupted user cache database (NAME and DATA columns have wrong types)\n");
    fwrite(sqlite3_column_text(st,1),1,sqlite3_column_bytes(st,1)+1,fp);
    j=sqlite3_column_bytes(st,2);
    fputc(j>>16,fp); fputc(j>>24,fp); fputc(j,fp); fputc(j>>8,fp);
    ofs[i++]=ftell(fp);
    fwrite(sqlite3_column_blob(st,2),1,j,fp);
  }
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  if(ftruncate(fd,ftell(fp))) fatal("I/O error: %m\n");
  sqlite3_finalize(st);
  if(e=sqlite3_prepare_v2(userdb,"UPDATE `USERCACHEDATA` SET `OFFSET` = ?2 WHERE `ID` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  i=0;
  while(i<c) {
    sqlite3_reset(st);
    sqlite3_bind_int64(st,1,ofs[i++]);
    sqlite3_bind_int64(st,2,ofs[i++]);
    while((e=sqlite3_step(st))==SQLITE_ROW);
    if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  }
  sqlite3_finalize(st);
  free(ofs);
  if(e=sqlite3_prepare_v2(userdb,"UPDATE `USERCACHEDATA` SET `DATA` = NULL WHERE `FILE` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
  if(e=sqlite3_prepare_v2(userdb,"UPDATE `USERCACHEINDEX` SET `TIME` = STRFTIME('%s')+1 WHERE `ID` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,uc_id);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
}

static void flush_usercache(void) {
  int e;
  fprintf(stderr,"Flushing user cache...\n");
  if(e=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  flush_usercache_1(FIL_LEVEL);
  flush_usercache_1(FIL_SOLUTION);
  if(e=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  fprintf(stderr,"Done\n");
}

static void init_usercache(void) {
  sqlite3_stmt*st;
  int z;
  sqlite3_int64 t1,t2;
  char*nam1;
  char*nam2;
  char*nam3;
  struct stat fst;
  fprintf(stderr,"Initializing user cache...\n");
  if(z=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  if(z=sqlite3_prepare_v2(userdb,"SELECT `ID`, `TIME` FROM `USERCACHEINDEX` WHERE `NAME` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  nam1=sqlite3_mprintf("%s.level",basefilename);
  if(!nam1) fatal("Allocation failed\n");
  nam2=realpath(nam1,0);
  if(!nam2) fatal("Cannot find real path of '%s': %m\n",nam1);
  levelfp=fopen(nam2,"r");
  if(!levelfp) fatal("Cannot open '%s' for reading: %m\n",nam2);
  sqlite3_free(nam1);
  sqlite3_bind_text(st,1,nam2,-1,0);
  z=sqlite3_step(st);
  if(z==SQLITE_ROW) {
    leveluc=sqlite3_column_int64(st,0);
    t1=sqlite3_column_int64(st,1);
  } else if(z==SQLITE_DONE) {
    leveluc=t1=-1;
  } else {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  sqlite3_reset(st);
  nam1=sqlite3_mprintf("%s.solution",basefilename);
  if(!nam1) fatal("Allocation failed\n");
  nam3=realpath(nam1,0);
  if(!nam3) fatal("Cannot find real path of '%s': %m\n",nam1);
  if(!strcmp(nam2,nam3)) fatal("Level and solution files seem to be the same file\n");
  solutionfp=fopen(nam3,"r");
  if(!solutionfp) fatal("Cannot open '%s' for reading: %m\n",nam3);
  sqlite3_free(nam1);
  sqlite3_bind_text(st,1,nam3,-1,0);
  z=sqlite3_step(st);
  if(z==SQLITE_ROW) {
    solutionuc=sqlite3_column_int64(st,0);
    t2=sqlite3_column_int64(st,1);
  } else if(z==SQLITE_DONE) {
    solutionuc=t2=-1;
  } else {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  sqlite3_finalize(st);
  if(stat(nam2,&fst)) fatal("Unable to stat '%s': %m\n",nam2);
  if(!fst.st_size) fatal("File '%s' has zero size\n",nam2);
  if(fst.st_mtime>t1 || fst.st_ctime>t1) leveluc=reset_usercache(levelfp,nam2,&fst,".LVL");
  if(stat(nam3,&fst)) fatal("Unable to stat '%s': %m\n",nam3);
  if(!fst.st_size) fatal("File '%s' has zero size\n",nam2);
  if(fst.st_mtime>t2 || fst.st_ctime>t2) solutionuc=reset_usercache(solutionfp,nam3,&fst,".SOL");
  free(nam2);
  free(nam3);
  if(z=sqlite3_prepare_v3(userdb,"SELECT * FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `LEVEL` = ?2;",-1,SQLITE_PREPARE_PERSISTENT,&readusercachest,0)) {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  if(z=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  fprintf(stderr,"Done\n");
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
  init_sql_functions(&leveluc,&solutionuc);
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

static void do_sql_mode(void) {
  int m=sqlite3_limit(userdb,SQLITE_LIMIT_SQL_LENGTH,-1);
  char*txt=malloc(m);
  int n=0;
  int c;
  int bail=1;
  if(m>1000000) m=1000000;
  txt=malloc(m+2);
  if(!txt) fatal("Allocation failed\n");
  for(;;) {
    c=fgetc(stdin);
    if(c=='\n' || c==EOF) {
      if(!n) continue;
      if(*txt=='#') {
        n=0;
      } else if(*txt=='.') {
        txt[n]=0;
        n=0;
        switch(txt[1]) {
          case 'b': bail=strtol(txt+2,0,0); break;
          case 'f': sqlite3_db_cacheflush(userdb); sqlite3_db_release_memory(userdb); break;
          case 'i': puts(sqlite3_db_filename(userdb,"main")); break;
          case 'q': exit(0); break;
          case 'u': flush_usercache(); break;
          case 'x': sqlite3_enable_load_extension(userdb,strtol(txt+2,0,0)); break;
          default: fatal("Invalid dot command .%c\n",txt[1]);
        }
      } else {
        txt[n]=0;
        if(sqlite3_complete(txt)) {
          n=sqlite3_exec(userdb,txt,test_sql_callback,0,0);
          if(bail && n) fatal("SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
          n=0;
        } else {
          txt[n++]='\n';
        }
      }
      if(c==EOF) break;
    } else {
      txt[n++]=c;
    }
    if(n>=m) fatal("Too long SQL statement\n");
  }
  if(n) fatal("Unterminated SQL statement\n");
  free(txt);
}

int main(int argc,char**argv) {
  int optind=1;
  while(argc>optind && argv[optind][0]=='-') {
    int i;
    const char*s=argv[optind++];
    if(s[1]=='-' && !s[2]) break;
    for(i=1;s[i];i++) main_options[s[i]&127]=1;
  }
  if(!main_options['c']) fprintf(stderr,"FREE HERO MESH\n");
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
  if(main_options['c']) {
    load_classes();
    return 0;
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
  init_usercache();
  load_classes();
  if(main_options['x']) {
    fprintf(stderr,"Ready for executing SQL statements.\n");
    do_sql_mode();
    return 0;
  }
  
  return 0;
}
