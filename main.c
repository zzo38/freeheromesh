#if 0
gcc ${CFLAGS:--s -O2} -o ${EXE:-~/bin/heromesh} -Wno-multichar main.c class.o picture.o bindings.o function.o exec.o game.o edit.o picedit.o sound.o smallxrm.o hash.o sqlite3.o `sdl-config --cflags --libs` -ldl -lpthread -lm
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

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

typedef struct {
  char a[(N_MESSAGES==sizeof(standard_message_names)/sizeof(*standard_message_names))?1:-9];
  char b[('\1\0'*'x'+'\0\1'*'y'=='xy')?1:-9];
  char c[(N_STANDARD_SOUNDS==sizeof(standard_sound_names)/sizeof(*standard_sound_names))?1:-9];
} ASSERTION;

#ifndef CONFIG_APPLICATION_ID
#define CONFIG_APPLICATION_ID "1296388936"
#endif
static const char schema[]=
  "BEGIN;"
  "PRAGMA APPLICATION_ID("CONFIG_APPLICATION_ID");"
  "PRAGMA RECURSIVE_TRIGGERS(1);"
  "CREATE TABLE IF NOT EXISTS `USERCACHEINDEX`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `TIME` INT);"
  "CREATE TABLE IF NOT EXISTS `USERCACHEDATA`(`ID` INTEGER PRIMARY KEY, `FILE` INT, `LEVEL` INT, `NAME` TEXT COLLATE NOCASE, `OFFSET` INT, `DATA` BLOB, `USERSTATE` BLOB);"
  "CREATE UNIQUE INDEX IF NOT EXISTS `USERCACHEDATA_I1` ON `USERCACHEDATA`(`FILE`, `LEVEL`);"
  "CREATE TRIGGER IF NOT EXISTS `USERCACHEINDEX_DELETION` AFTER DELETE ON `USERCACHEINDEX` BEGIN DELETE FROM `USERCACHEDATA` WHERE `FILE` = OLD.`ID`; END;"
  "CREATE TEMPORARY TABLE `PICTURES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT COLLATE NOCASE, `OFFSET` INT, `DEPENDENT` INT, `MISC` BLOB);"
  "CREATE TEMPORARY TABLE `VARIABLES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT);"
  "CREATE TEMPORARY TABLE `DIVISIONS`(`HEADING` BLOB NOT NULL, `FIRST` INT NOT NULL);"
  "COMMIT;"
;

const char jispunct[0x60]=
  "  ,.,. :;?!  '` "
  "^~_         ---/"
  "\\~||  ''\"\"()  []"
  "{}<>        +-  "
  " = <>       '\" \\"
  "$ #%#&*@        "
;

sqlite3*userdb;
xrm_db*resourcedb;
const char*basefilename;
xrm_quark optionquery[16];
char main_options[128];
Uint8 message_trace[0x4100/8];
Uint16 level_id,level_ord,level_version,level_code;
unsigned char*level_title;
Uint16*level_index;
int level_nindex;
char level_changed;
FILE*levelfp;
FILE*solutionfp;

#ifdef CONFIG_WITH_STACK_PROTECTION
char stack_protect_mode=0;
void*stack_protect_mark;
void*stack_protect_low;
void*stack_protect_high;
#endif

static const char*globalclassname;
static SDL_Cursor*cursor[77];
static FILE*compositefp;
static sqlite3_int64 leveluc,solutionuc;
static sqlite3_stmt*readusercachest;
static char*hpath;

typedef struct {
  FILE*fp;
  sqlite3_uint64 start,length,offset;
} SliceCookie;

static ssize_t slice_read(void*cookie,char*buf,size_t size) {
  SliceCookie*d=cookie;
  fseek(d->fp,d->start+d->offset,SEEK_SET);
  if(size>d->length-d->offset) size=d->length-d->offset;
  d->offset+=size=fread(buf,1,size,d->fp);
  return size;
}

static int slice_seek(void*cookie,off64_t*offset,int whence) {
  SliceCookie*d=cookie;
  switch(whence) {
    case SEEK_SET: d->offset=*offset; break;
    case SEEK_CUR: d->offset+=*offset; break;
    case SEEK_END: d->offset=d->length+*offset; break;
  }
  if(d->offset<0 || d->offset>d->length) return -1;
  return fseek(d->fp,d->start+(*offset=d->offset),SEEK_SET);
}

static int slice_close(void*cookie) {
  free(cookie);
  return 0;
}

FILE*composite_slice(const char*suffix,char isfatal) {
  FILE*fp;
  SliceCookie*d;
  int c,n;
  sqlite3_int64 t;
  rewind(compositefp);
  look:
  n=0;
  if(*suffix=='.') for(;;) {
    c=fgetc(compositefp);
    if(c==EOF) goto notfound;
    if(!c) goto skip;
    if(c=='.') goto name;
  }
  for(;;) {
    c=fgetc(compositefp);
    if(c==EOF) goto notfound;
    name:
    if(!c) {
      if(suffix[n]) goto skip; else goto found;
    }
    if(c==suffix[n]) {
      n++;
    } else {
      do c=fgetc(compositefp); while(c>0);
      goto skip;
    }
  }
  skip:
  t=fgetc(compositefp)<<16; t|=fgetc(compositefp)<<24;
  t|=fgetc(compositefp); t|=fgetc(compositefp)<<8;
  fseek(compositefp,t,SEEK_CUR);
  goto look;
  found:
  t=fgetc(compositefp)<<16; t|=fgetc(compositefp)<<24;
  t|=fgetc(compositefp); t|=fgetc(compositefp)<<8;
  d=malloc(sizeof(SliceCookie));
  if(!d) fatal("Allocation failed\n");
  d->fp=compositefp;
  d->start=ftell(compositefp);
  d->length=t;
  d->offset=0;
  fp=fopencookie(d,"r",(cookie_io_functions_t){.read=slice_read,.seek=slice_seek,.close=slice_close});
  if(!fp) fatal("Allocation failed\n");
  return fp;
  notfound:
  if(isfatal) fatal("Cannot find '%s' lump in composite puzzle set file\n",suffix);
  return 0;
}

static sqlite3_int64 reset_usercache(FILE*fp,const char*nam,struct stat*stats,const char*suffix) {
  sqlite3_stmt*st;
  sqlite3_int64 t,id;
  char buf[128];
  int i,z;
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
    } else if(i==12 && suffix[1]=='L' && !sqlite3_stricmp(buf,"DIVISION.IDX")) {
      sqlite3_bind_int(st,2,LUMP_DIVISION_IDX);
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

unsigned char*read_lump_or_userstate(int sol,int lvl,long*sz,char us) {
  // Returns a pointer to the data; must be freed using free().
  // If there is no data, returns null and sets *sz to zero.
  // Third argument is a pointer to a variable to store the data size (must be not null).
  // Fourth argument is 1 for user state or 0 for lump data.
  unsigned char*buf=0;
  sqlite3_reset(readusercachest);
  sqlite3_bind_int64(readusercachest,1,sol?solutionuc:leveluc);
  sqlite3_bind_int(readusercachest,2,lvl);
  sqlite3_bind_int(readusercachest,3,us);
  if(sqlite3_step(readusercachest)==SQLITE_ROW) {
    if(sqlite3_column_type(readusercachest,1)==SQLITE_BLOB) {
      const unsigned char*con=sqlite3_column_blob(readusercachest,1);
      *sz=sqlite3_column_bytes(readusercachest,1);
      buf=malloc(*sz);
      if(*sz && !buf) fatal("Allocation failed\n");
      memcpy(buf,con,*sz);
    } else if(us) {
      *sz=0;
    } else {
      FILE*fp=sol?solutionfp:levelfp;
      rewind(fp);
      fseek(fp,sqlite3_column_int64(readusercachest,0)-4,SEEK_SET);
      *sz=fgetc(fp)<<16; *sz|=fgetc(fp)<<24; *sz|=fgetc(fp); *sz|=fgetc(fp)<<8;
      if(feof(fp) || *sz<0) fatal("Invalid Hamster archive\n");
      buf=malloc(*sz);
      if(!buf) fatal("Allocation failed\n");
      if(!fread(buf,1,*sz,fp)) fatal("Unable to read data\n");
      rewind(fp);
    }
  } else {
    *sz=0;
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
  sqlite3_bind_text(st,3,lvl==LUMP_CLASS_DEF?"CLASS.DEF":lvl==LUMP_LEVEL_IDX?"LEVEL.IDX":lvl==LUMP_DIVISION_IDX?"DIVISION.IDX":sol?".SOL":".LVL",-1,SQLITE_STATIC);
  sqlite3_bind_blob64(st,4,data,sz,0);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
}

void write_userstate(int sol,int lvl,long sz,const unsigned char*data) {
  sqlite3_stmt*st;
  int e;
  if(e=sqlite3_prepare_v2(userdb,"UPDATE `USERCACHEDATA` SET `USERSTATE` = ?3"
   " WHERE `FILE` = ?1 AND `LEVEL` = ?2;",-1,&st,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_bind_int64(st,1,sol?solutionuc:leveluc);
  sqlite3_bind_int(st,2,lvl);
  sqlite3_bind_blob64(st,3,data,sz,0);
  while((e=sqlite3_step(st))==SQLITE_ROW);
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
}

static void load_level_index(void) {
  long sz;
  sqlite3_stmt*st;
  int i;
  unsigned char*data=read_lump(FIL_LEVEL,LUMP_LEVEL_IDX,&sz);
  unsigned char*p;
  if(!data) return;
  if(sz>65536) fatal("Too many levels\n");
  level_index=malloc((level_nindex=sz>>1)*sizeof(Uint16));
  if(!level_index) fatal("Allocation failed\n");
  for(i=0;i<level_nindex;i++) level_index[i]=data[i+i]|(data[i+i+1]<<8);
  free(data);
  // Load divisions
  data=read_lump(FIL_LEVEL,LUMP_DIVISION_IDX,&sz);
  if(!data || sz<3) {
    free(data);
    return;
  }
  if(i=sqlite3_prepare_v2(userdb,"INSERT INTO `DIVISIONS`(`HEADING`,`FIRST`) VALUES(?1,?2);",-1,&st,0))
   fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  p=data;
  while(p<data+sz-3) {
    sqlite3_reset(st);
    sqlite3_bind_int(st,2,p[0]|(p[1]<<8));
    p+=2;
    sqlite3_bind_blob(st,1,p,i=strnlen(p,data+sz-p),0);
    p+=i;
    if(p<data+sz) p++;
    while(sqlite3_step(st)==SQLITE_ROW);
  }
  sqlite3_finalize(st);
  free(data);
}

const char*load_level(int lvl) {
  // Load level by ID. Returns null pointer if successful, or an error message if it failed.
  long sz=0;
  Uint16 of=0;
  unsigned char*buf=lvl>=0?read_lump(FIL_LEVEL,lvl,&sz):0;
  unsigned char*p=buf;
  unsigned char*end=buf+sz;
  unsigned char*q;
  int i,n,x,y,z;
  Uint16 lo=0;
  Uint32 o;
  Uint32 mru[2];
  if(lvl<0 && level_index && -lvl<=level_nindex) {
    lo=-lvl;
    lvl=level_index[~lvl];
    p=buf=read_lump(FIL_LEVEL,lvl,&sz);
    end=buf+sz;
  }
  if(lvl<0) return "Invalid level ID";
  if(!buf) return "Cannot find level";
  level_changed=1;
  free(level_title);
  level_title=0;
  annihilate();
  generation_number=TY_MAXTYPE+1;
  generation_number_inc=0;
  level_version=p[0]|(p[1]<<8);
  level_code=p[2]|(p[3]<<8);
  p+=4;
  pfwidth=(*p++&63)+1;
  pfheight=(*p++&63)+1;
  while(*p && p<end) p++; // skip text for now
  p++; // skip null terminator
  if(p>=end) goto bad1;
  level_title=strdup(buf+6);
  if(!level_title) fatal("Allocation failed\n");
  mru[0]=mru[1]=VOIDLINK;
  restart:
  x=0;
  y=1;
  n=0;
  for(;;) {
    if(n) {
      o=objalloc(objects[*mru]->class);
      if(o==VOIDLINK) goto bad3;
      objects[o]->oflags=(objects[o]->oflags&~OF_BIZARRO)|of;
      objects[o]->image=objects[*mru]->image;
      objects[o]->misc1=objects[*mru]->misc1;
      objects[o]->misc2=objects[*mru]->misc2;
      objects[o]->misc3=objects[*mru]->misc3;
      objects[o]->dir=objects[*mru]->dir;
      objects[o]->x=++x;
      objects[o]->y=y;
      if(x>pfwidth) goto bad2;
      pflink(o);
      --n;
    } else {
      if(p>=end) goto bad1;
      z=*p++;
      if(z==0xFF) break;
      if(z==0xFE) {
        of^=OF_BIZARRO;
        goto restart;
      }
      if(z&0x20) x=*p++;
      if(z&0x10) y=*p++;
      if(z&0x40) x++;
      if(!x || !y || x>pfwidth || y>pfheight) goto bad2;
      n=z&0x70?0:1;
      if(z&0x80) {
        // MRU
        if(mru[n]==VOIDLINK) goto bad1;
        o=objalloc(objects[mru[n]]->class);
        if(o==VOIDLINK) goto bad3;
        objects[o]->oflags=(objects[o]->oflags&~OF_BIZARRO)|of;
        objects[o]->image=objects[mru[n]]->image;
        objects[o]->misc1=objects[mru[n]]->misc1;
        objects[o]->misc2=objects[mru[n]]->misc2;
        objects[o]->misc3=objects[mru[n]]->misc3;
        objects[o]->dir=objects[mru[n]]->dir;
        objects[o]->x=x;
        objects[o]->y=y;
        pflink(o);
        n=z&15;
      } else {
        // Not MRU
        i=*p++;
        i|=*p++<<8;
        o=objalloc(i&0x3FFF);
        if(o==VOIDLINK) goto bad3;
        objects[o]->oflags=(objects[o]->oflags&~OF_BIZARRO)|of;
        if(n!=2) mru[n]=o;
        if(i&0x8000) {
          n=objects[o]->class;
          for(i=0;i<classes[n]->nimages;i++) {
            if(classes[n]->images[i]&0x8000) {
              objects[o]->image=i;
              break;
            }
          }
        } else {
          objects[o]->image=*p++;
        }
        objects[o]->dir=z&7;
        if(z&0x08) {
          z=*p++;
          if(z&0xC0) {
            // Misc1
            objects[o]->misc1=UVALUE(p[0]|(p[1]<<8),(z>>0)&3);
            p+=2;
          }
          if((z&0xC0)!=0x40) {
            // Misc2
            objects[o]->misc2=UVALUE(p[0]|(p[1]<<8),(z>>2)&3);
            p+=2;
          }
          if(!((z&0xC0)%0xC0)) {
            // Misc3
            objects[o]->misc3=UVALUE(p[0]|(p[1]<<8),(z>>4)&3);
            p+=2;
          }
        }
        objects[o]->x=x;
        objects[o]->y=y;
        pflink(o);
        n=0;
      }
    }
  }
  // Level strings
  i=0;
  while(p<end) {
    if(i>=max_objects) goto bad3;
    q=memchr(p,0,end-p);
    if(!q) goto bad1;
    levelstrings=realloc(levelstrings,(i+1)*sizeof(unsigned char*));
    if(!levelstrings) fatal("Allocation failed\n");
    levelstrings[i]=strdup(p);
    if(!levelstrings[i]) fatal("Allocation failed\n");
    p=q+1;
    i++;
  }
  nlevelstrings=i;
  if(p>end) goto bad1;
  free(buf);
  level_id=lvl;
  level_ord=lo;
  if(level_index && !lo) {
    for(i=0;i<level_nindex;i++) if(level_index[i]==lvl) {
      level_ord=i+1;
      break;
    }
  }
  level_changed=0;
  return 0;
bad1:
  free(buf);
  return "Corrupted level data";
bad2:
  free(buf);
  return "Object out of bounds";
bad3:
  free(buf);
  return "Bad object in level";
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
    if(ferror(fp)) fatal("I/O error: %m\n");
  }
  if(e!=SQLITE_DONE) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  if(ftruncate(fd,ftell(fp))) fatal("I/O error: %m\n");
  rewind(fp);
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
  if(main_options['r']) return;
  printStatus("Flushing user cache...\n");
  if(e=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  flush_usercache_1(FIL_LEVEL);
  flush_usercache_1(FIL_SOLUTION);
  if(e=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",e,sqlite3_errmsg(userdb));
  printStatus("Done\n");
}

static void init_composite(void) {
  FILE*fp=compositefp=fopen(basefilename,"r");
  sqlite3_stmt*st;
  sqlite3_int64 t1,t2;
  int z;
  struct stat fst;
  if(!fp) fatal("Cannot open '%s' for reading: %m\n",basefilename);
  printStatus("Loading puzzle set...\n");
  if(z=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  if(z=sqlite3_prepare_v2(userdb,"SELECT `ID`, `TIME` FROM `USERCACHEINDEX` WHERE `NAME` = CHAR(?2)||'//'||?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  basefilename=realpath(basefilename,0);
  if(!basefilename) fatal("Cannot find real path of puzzle set: %m\n");
  sqlite3_bind_text(st,1,basefilename,-1,0);
  levelfp=composite_slice(".level",1);
  solutionfp=composite_slice(".solution",1);
  sqlite3_bind_int(st,2,'L');
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
  sqlite3_bind_int(st,2,'S');
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
  if(stat(basefilename,&fst)) fatal("Unable to stat '%s': %m\n",basefilename);
  if(!fst.st_size) fatal("File '%s' has zero size\n",basefilename);
  if(fst.st_mtime>t1 || fst.st_ctime>t1) {
    char*p=sqlite3_mprintf("L//%s",basefilename);
    if(!p) fatal("Allocation failed\n");
    leveluc=reset_usercache(levelfp,p,&fst,".LVL");
    *p='S';
    solutionuc=reset_usercache(solutionfp,p,&fst,".SOL");
    sqlite3_free(p);
  }
  if(z=sqlite3_prepare_v3(userdb,"SELECT `OFFSET`, CASE WHEN ?3 THEN `USERSTATE` ELSE `DATA` END "
   "FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `LEVEL` = ?2;",-1,SQLITE_PREPARE_PERSISTENT,&readusercachest,0)) {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  if(z=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  printStatus("Done\n");
}

static void init_usercache(void) {
  sqlite3_stmt*st;
  int z;
  sqlite3_int64 t1,t2;
  char*nam1;
  char*nam2;
  char*nam3;
  struct stat fst;
  printStatus("Initializing user cache...\n");
  if(z=sqlite3_exec(userdb,"BEGIN;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  if(z=sqlite3_prepare_v2(userdb,"SELECT `ID`, `TIME` FROM `USERCACHEINDEX` WHERE `NAME` = ?1;",-1,&st,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  nam1=sqlite3_mprintf("%s.level",basefilename);
  if(!nam1) fatal("Allocation failed\n");
  nam2=realpath(nam1,0);
  if(!nam2) fatal("Cannot find real path of '%s': %m\n",nam1);
  levelfp=fopen(nam2,main_options['r']?"r":"r+");
  if(!levelfp) fatal("Cannot open '%s' for reading%s: %m\n",nam2,main_options['r']?"":"/writing");
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
  solutionfp=fopen(nam3,main_options['r']?"r":"r+");
  if(!solutionfp) fatal("Cannot open '%s' for reading%s: %m\n",nam3,main_options['r']?"":"/writing");
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
  if(fst.st_mtime>t2 || fst.st_ctime>t2) solutionuc=reset_usercache(solutionfp,nam3,&fst,".SOL");
  free(nam2);
  free(nam3);
  if(z=sqlite3_prepare_v3(userdb,"SELECT `OFFSET`, CASE WHEN ?3 THEN `USERSTATE` ELSE `DATA` END "
   "FROM `USERCACHEDATA` WHERE `FILE` = ?1 AND `LEVEL` = ?2;",-1,SQLITE_PREPARE_PERSISTENT,&readusercachest,0)) {
    fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  }
  if(z=sqlite3_exec(userdb,"COMMIT;",0,0,0)) fatal("SQL error (%d): %s\n",z,sqlite3_errmsg(userdb));
  printStatus("Done\n");
}

static void new_puzzle_set(void) {
  char*nam1;
  char*nam2;
  nam1=sqlite3_mprintf("%s.level",basefilename);
  if(!nam1) fatal("Allocation failed\n");
  levelfp=fopen(nam1,"w+x");
  if(!levelfp) fatal("Cannot open '%s' for reading/writing: %m\n",nam1);
  write_empty_level_set(levelfp);
  nam2=sqlite3_mprintf("%s.solution",basefilename);
  if(!nam2) fatal("Allocation failed\n");
  solutionfp=fopen(nam2,"w+x");
  if(!solutionfp) fatal("Cannot open '%s' for reading/writing: %m\n",nam2);
  fclose(levelfp);
  fclose(solutionfp);
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
  if(!v || !*v) strcpy(hpath+strlen(hpath)-2,"session"),v=hpath;
  if(z=sqlite3_open(v,&userdb)) fatal("Failed to open user database %s (%s)\n",v,userdb?sqlite3_errmsg(userdb):sqlite3_errstr(z));
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

Uint32*tron8to32(const Uint8*si) {
  // This function does not interpret the internal-only 0xFF code, which will be implemented in picture.c instead.
  Uint16 cs=0;
  int n=0;
  Uint32*so=0;
  int k=0;
  if(has_mbcs && *si==0xFE) {
    // TRON
    for(k=0;si[k];k++) {
      if(si[k]==31 && si[k+1]) {
        n+=2;
        k++;
      } else if(si[k]==14 || si[k]==30) {
        while(si[k] && si[k]!='\\') {
          n+=2;
          k++;
        }
        n+=2;
      } else if(si[k]==0xFE) {
        n--;
        while(si[k+1]==0xFE) k++;
      } else {
        n++;
        if(si[k]<=0x20) n++;
      }
    }
    so=calloc(sizeof(Uint32),(n>>=1)+2);
    if(!so) fatal("Allocation failed\n");
    n++; k=0;
    while(*si && k<n) {
      if(*si==0xFE) {
        cs=-0x100;
        while(*si==0xFE) cs+=0x100,si++;
        if(*si) cs|=*si++;
      } else if((*si==16 || *si==31 || *si==127) && si[1]) {
        so[k++]=(si[0]<<8)|si[1];
        si+=2;
      } else if(*si==14 || *si==30) {
        while(*si && *si!='\\' && *si!=0xFE) so[k++]=*si++;
        if(*si=='\\') so[k++]=*si++;
      } else if(*si<=0x20) {
        so[k++]=*si++;
      } else if(*si<0xFE && si[1]<0xFE && si[1]>0x20) {
        so[k++]=(si[0]<<8)|si[1]|(cs<<16);
        si+=2;
      } else {
        si++;
      }
    }
  } else {
    // ASCII
    so=calloc(sizeof(Uint32),(n=strlen(si))+1);
    if(!so) fatal("Allocation failed\n");
    while(*si) {
      if(*si==16 || *si==31 || *si==127) {
        if(!si[1]) break;
        so[k++]=(si[0]<<8)|si[1];
        si+=2;
      } else {
        so[k++]=*si++;
      }
    }
  }
  return so;
}

Uint8*tron32to8(const Uint32*si) {
  Uint16 cs=0;
  int i;
  Uint8*so=0;
  size_t sz=0;
  FILE*f=open_memstream((char**)&so,&sz);
  if(!f) fatal("Allocation failed\n");
  for(i=0;si[i];i++) if(si[i]>0x200000) goto nonascii;
  while(*si) {
    if(*si<0x7F) {
      fputc(*si++,f);
    } else if(*si>=0x1000 && *si<=0x10FF) {
      fputc(16,f);
      fputc((*si++&0xFF)?:0x20,f);
    } else if(*si>=0x1F00 && *si<=0x1FFF) {
      fputc(31,f);
      fputc((*si++&0xFF)?:0xFF,f);
    } else if(*si>=0x7F20 && *si<0x7F7F) {
      fputc('%',f);
      fputc(*si++&0xFF,f);
    }
  }
  nonascii:
  while(*si) {
    if(*si>0x200000) {
      if(cs!=(*si>>16)) {
        cs=*si>>16;
        for(i=cs>>8;i>=0;i--) fputc(0xFE,f);
        fputc(cs,f);
      }
      fputc(*si>>8,f);
      fputc(*si++,f);
    } else if(*si<0x21) {
      fputc(i=*si++,f);
      if(i==14 || i==30) while(*si) {
        if(*si=='\\') {
          fputc(*si++,f);
          break;
        } else if(*si<0xFE || (*si>=0x702221 && *si<0x70227F) || (*si>=0x212330 && *si<0x21237B)) {
          fputc(*si++,f);
        } else if(*si>=0x212121 && *si<0x21217F) {
          fputc(jispunct[*si++-0x212120],f);
        } else if(*si<0xFE) {
          fputc(*si++,f);
        } else {
          fputc(' ',f);
          si++;
        }
      }
    } else if(*si<0x7F) {
      if(cs!=0x70) {
        cs=0x70;
        fputc(0xFE,f);
        fputc(0x70,f);
      }
      fputc(0x22,f);
      fputc(*si++,f);
    } else if(*si>=0x1000 && *si<=0x10FF) {
      fputc(16,f);
      fputc(*si++?:0x20,f);
    } else if(*si>=0x1F00 && *si<=0x1FFF) {
      fputc(31,f);
      fputc(*si++?:0xFF,f);
    } else if(*si>=0x7F20 && *si<0x7F7F) {
      fputc(127,f);
      fputc(*si++,f);
    } else {
      si++;
    }
  }
  fputc(0,f);
  fclose(f);
  if(!so) fatal("Allocation failed\n");
  return so;
}

static void set_path(const char*arg) {
  const char*s;
  if(main_options['h']) goto home;
  if((s=getenv("HEROMESH_PREFIX")) && *s) {
    hpath=malloc(strlen(s)+32);
    if(!hpath) fatal("Allocation failed\n");
    sprintf(hpath,"%s.heromeshrc",s);
    return;
  }
#ifndef CONFIG_NO_PORTABLE
#ifndef CONFIG_USING_APPIMAGE
  if(s=strrchr(arg,'/')) {
    hpath=malloc(s+64-arg);
    if(!hpath) fatal("Allocation failed\n");
    sprintf(hpath,"%.*s/current.heromeshrc",(int)(s-arg),arg);
    return;
  }
#endif
#endif
  home:
  s=getenv("HOME")?:".";
  hpath=malloc(strlen(s)+32);
  if(!hpath) fatal("Allocation failed\n");
  sprintf(hpath,"%s%s.heromeshrc",s,s[strlen(s)-1]=='/'?"":"/");
}

static void load_options(void) {
  FILE*fp=fopen(hpath,"r");
#ifdef CONFIG_DEFAULT_RESOURCES
  if(!fp) fp=fopen(CONFIG_DEFAULT_RESOURCES,"r");
#endif
  if(!fp) fatal("Failed to open %s (%m)\n",hpath);
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
  int i,j;
  set_cursor(XC_tcross);
  SDL_LockSurface(screen);
  draw_text(0,0,"Hello, World!",0xF0,0xFF);
  buf[16]=0;
  for(i=0;i<16;i++) {
    for(n=0;n<16;n++) buf[n]=(i<<4)+n?:255;
    draw_text(4,(i<<3)+12,buf,0xF0,0xF7);
  }
  n=0;
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      switch(ev.key.keysym.sym) {
        case SDLK_BACKSPACE:
          n/=10;
          goto setnum;
        case SDLK_SPACE:
          n=0;
          SDL_WM_SetCaption("0","0");
          break;
        case SDLK_0 ... SDLK_9:
          n=10*n+ev.key.keysym.sym-SDLK_0;
        setnum:
          snprintf(buf,30,"%u",n);
          SDL_WM_SetCaption(buf,buf);
          break;
        case SDLK_KP_MINUS:
          if(n) n--;
          goto setnum;
        case SDLK_KP_PLUS:
          n++;
          goto setnum;
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
        case SDLK_i:
          SDL_FillRect(screen,0,5);
          for(i=j=0;;i++) {
            if(picture_size*(i+1)>screen->w) i=0,j++;
            if(picture_size*(j+1)>screen->h) break;
            draw_picture(picture_size*i,picture_size*j,n++);
          }
          SDL_Flip(screen);
          break;
        case SDLK_o:
          if(n<0x4000 && classes[n] && classes[n]->gamehelp) {
            Uint32*a=tron8to32(classes[n]->gamehelp);
            Uint8*b=tron32to8(a);
            for(i=0;classes[n]->gamehelp[i];i++) printf("%02X ",classes[n]->gamehelp[i]&0xFF);
            puts(";");
            for(i=0;a[i];i++) printf("%08X ",a[i]);
            puts(";");
            for(i=0;b[i];i++) printf("%02X ",b[i]);
            puts(".");
            free(a);
            free(b);
          }
          break;
        case SDLK_p:
          sqlite3_exec(userdb,"SELECT * FROM `PICTURES`;",test_sql_callback,0,0);
          break;
        case SDLK_q:
          exit(0);
          break;
        case SDLK_s:
          goto scrolltest;
        case SDLK_t:
          puts(screen_prompt("Testing screen_prompt()")?:"No output.");
          break;
        case SDLK_u:
          set_sound_effect(UVALUE(n,TY_USOUND),NVALUE(0));
          break;
        case SDLK_w:
          sound_test();
          SDL_FillRect(screen,0,0);
          SDL_Flip(screen);
          break;
        case SDLK_z:
          set_sound_effect(NVALUE(0),NVALUE(1));
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
  fatal("An error occurred waiting for events.\n");
scrolltest:
  SDL_FillRect(screen,0,0xF2);
  i=0;
  scrollbar(&i,10,n,0,0);
  draw_picture(16,screen->h/10,1);
  draw_picture(16,(2*screen->h)/10,2);
  draw_picture(16,(3*screen->h)/10,3);
  SDL_Flip(screen);
  set_cursor(XC_arrow);
  while(SDL_WaitEvent(&ev)) switch(ev.type) {
    case SDL_KEYDOWN:
      if(ev.key.keysym.sym==SDLK_SPACE) exit(0);
      break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
      scrollbar(&i,10,n,&ev,0);
      printf("scroll %d %d\n",i,n);
      break;
    case SDL_MOUSEMOTION:
      if(!scrollbar(&i,10,n,&ev,0)) set_cursor(XC_arrow);
      break;
    case SDL_VIDEOEXPOSE:
      SDL_FillRect(screen,0,0xF2);
      scrollbar(&i,10,n,&ev,0);
      draw_picture(16,screen->h/10,1);
      draw_picture(16,(2*screen->h)/10,2);
      draw_picture(16,(3*screen->h)/10,3);
      SDL_Flip(screen);
      break;
    case SDL_QUIT:
      exit(0);
      break;
  }
  fatal("An error occurred waiting for events.\n");
}

static void do_sql_mode(void) {
  int m=sqlite3_limit(userdb,SQLITE_LIMIT_SQL_LENGTH,-1);
  char*txt;
  int n=0;
  int c;
  int bail=1;
  if(m>1000000) m=1000000;
  txt=malloc(m+2);
  if(!txt) fatal("Allocation failed\n");
  for(;;) {
    c=fgetc(stdin);
    if(c=='\n' || c==EOF) {
      if(!n && c==EOF) break;
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
        fflush(stdout);
      } else {
        txt[n]=0;
        if(sqlite3_complete(txt)) {
          n=sqlite3_exec(userdb,txt,test_sql_callback,0,0);
          if(bail && n) fatal("SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
          else if(n) fprintf(stderr,"SQL error (%d): %s\n",n,sqlite3_errmsg(userdb));
          n=0;
          fflush(stdout);
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

#ifdef CONFIG_WITH_STACK_PROTECTION
static void test_stack_protection(void) {
  fprintf(stderr,"Stack protection final values: %p %p %p\n",stack_protect_mark,stack_protect_low,stack_protect_high);
}

static void set_stack_protection(void) {
  const char*v;
  optionquery[1]=Q_stackProtection;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2);
  if(!v || !*v) return;
  stack_protect_mode=*v;
  if(*v=='?') {
    fprintf(stderr,"Stack protection test mode: %p\n",stack_protect_mark);
    stack_protect_low=stack_protect_high=stack_protect_mark;
    atexit(test_stack_protection);
  } else if(*v=='H') {
    v=malloc(strtoll(v+1,0,0));
    fatal("%p %p :: %lld\n",v,stack_protect_mark,(long long)(((char*)stack_protect_mark)-v));
  }
  if(v[1]) stack_protect_mark=((char*)stack_protect_mark)+strtoll(v+1,0,0);
}
#endif

const char*log_if_error(const char*t) {
  if(t && main_options['v']) fprintf(stderr,"!! %s\n",t);
  return t;
}

static void set_tracing(void) {
  const char*v;
  int i;
  optionquery[1]=Q_traceAll;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  if(boolxrm(v,0)) {
    memset(message_trace,255,sizeof(message_trace));
    for(i=0;i<0x4000;i++) if(classes[i]) classes[i]->cflags|=CF_TRACEIN|CF_TRACEOUT;
  }
}

static inline void set_autosave(void) {
  const char*v;
  optionquery[1]=Q_autoSave;
  v=xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"";
  if(boolxrm(v,0)) atexit(flush_usercache);
}

int main(int argc,char**argv) {
  int optind=1;
  while(argc>optind && argv[optind][0]=='-') {
    int i;
    const char*s=argv[optind++];
    if(s[1]=='-' && !s[2]) break;
    for(i=1;s[i];i++) main_options[s[i]&127]=1;
  }
  setbuf(stderr,0);
  if(!main_options['c']) printStatus("FREE HERO MESH\n");
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
  if(main_options['z']) {
    if(main_options['p'] || main_options['f'] || main_options['e']) fatal("Switches are conflicting with -z\n");
    main_options['r']=1;
  }
  if(main_options['n'] && main_options['i']) fatal("Switches -n and -i are conflicting\n");
  if(main_options['n'] || main_options['i']) {
    if(main_options['r']) fatal("Switches -r and -%c are conflicting\n",main_options['i']?'i':'n');
    main_options['x']=1;
  }
  if(main_options['a'] || main_options['O'] || main_options['S']) main_options['r']=main_options['x']=1;
  if(main_options['p']) main_options['r']=1;
  if(main_options['f']) main_options['x']=1;
  if(!main_options['c']) {
    set_path(argv[0]);
    load_options();
  }
  if(argc>optind) read_options(argc-optind,argv+optind);
  *optionquery=xrm_make_quark(globalclassname,0)?:xrm_anyq;
#ifdef CONFIG_WITH_STACK_PROTECTION
  stack_protect_mark=__builtin_frame_address(0);
  set_stack_protection();
#endif
  if(main_options['c']) {
    load_classes();
    return 0;
  }
  init_sql();
  init_screen();
  if(main_options['p']) {
    run_picture_editor();
    return 0;
  }
  if(main_options['z']) init_composite();
  load_pictures();
  if(main_options['T']) {
    printf("argv[0] = %s\n",argv[0]);
    init_sound();
    if(main_options['+']) load_classes();
    test_mode();
    return 0;
  }
  if(main_options['n']) {
    new_puzzle_set();
    return 0;
  }
  if(!main_options['z']) init_usercache();
  if(screen) init_sound();
  load_classes();
  load_key_bindings();
  load_level_index();
  optionquery[1]=Q_maxObjects;
  max_objects=strtoll(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"",0,0)?:0xFFFF0000L;
  set_tracing();
  annihilate();
  optionquery[1]=Q_level;
  if(main_options['+']) {
    level_id=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"",0,10);
    if(log_if_error(load_level(level_id))) fatal("Cannot load level with specified ID\n");
  } else if(level_ord=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"",0,10)) {
    if(level_ord>level_nindex) level_ord=level_nindex;
    log_if_error(load_level(-level_ord));
  }
  optionquery[1]=Q_maxTrigger;
  max_trigger=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"",0,10);
  if(main_options['a']) run_auto_test();
  if(main_options['O']) export_private_solutions();
  if(main_options['S']) make_level_hashes();
  if(main_options['x']) {
    if(main_options['i']) {
      batch_import();
      if(main_options['f']) flush_usercache();
      return 0;
    } else if(main_options['f']) {
      if(main_options['r']) fatal("Cannot flush user cache; puzzle set is read-only\n");
      flush_usercache();
      return 0;
    }
    printStatus("Ready for executing SQL statements.\n");
    no_dead_anim=1;
    do_sql_mode();
    return 0;
  }
  set_autosave();
  for(;;) { if(main_options['e']) run_editor(); else run_game(); }
}
