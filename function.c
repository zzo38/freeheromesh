#if 0
gcc ${CFLAGS:--s -O2} -c -fplan9-extensions function.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"

typedef struct {
  struct sqlite3_vtab_cursor;
  sqlite3_int64 rowid;
  char unique,eof;
} Cursor;

static void fn_basename(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_text(cxt,basefilename,-1,SQLITE_STATIC);
}

static void fn_cacheid(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int64(cxt,*(sqlite3_int64*)sqlite3_user_data(cxt));
}

static void fn_modstate(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,SDL_GetModState());
}

static void fn_picture_size(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,picture_size);
}

static void fn_resource(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int i;
  if(argc>14 || argc<1) {
    sqlite3_result_error(cxt,"Invalid number of XRM resource components",-1);
  } else {
    for(i=0;i<argc;i++) optionquery[i+1]=xrm_make_quark(sqlite3_value_text(argv[i])?:(const unsigned char*)"?",0)?:xrm_anyq;
    sqlite3_result_text(cxt,xrm_get_resource(resourcedb,optionquery,optionquery,argc+1),-1,SQLITE_TRANSIENT);
  }
}

static void fn_sign_extend(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_int64 a;
  if(sqlite3_value_type(*argv)==SQLITE_NULL) return;
  a=sqlite3_value_int64(*argv)&0xFFFFFFFF;
  sqlite3_result_int64(cxt,a-(a&0x80000000?0x100000000LL:0));
}

static int vt0_close(sqlite3_vtab_cursor*cur) {
  sqlite3_free(cur);
  return SQLITE_OK;
}

static int vt0_connect(sqlite3*db,void*aux,int argc,const char*const*argv,sqlite3_vtab**vt,char**err) {
  sqlite3_declare_vtab(db,aux);
  *vt=sqlite3_malloc(sizeof(sqlite3_vtab));
  return *vt?SQLITE_OK:SQLITE_NOMEM;
}

static int vt0_disconnect(sqlite3_vtab*vt) {
  sqlite3_free(vt);
  return SQLITE_OK;
}

static int vt0_eof(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  return cur->eof;
}

static int vt0_index(sqlite3_vtab*vt,sqlite3_index_info*info) {
  int i;
  if(info->nOrderBy==1 && info->aOrderBy->iColumn==-1 && !info->aOrderBy->desc) info->orderByConsumed=1;
  for(i=0;i<info->nConstraint;i++) {
    if(info->aConstraint[i].iColumn==-1 && info->aConstraint[i].op==SQLITE_INDEX_CONSTRAINT_EQ && info->aConstraint[i].usable) {
      info->aConstraintUsage[i].argvIndex=1;
      info->aConstraintUsage[i].omit=1;
      info->idxFlags=SQLITE_INDEX_SCAN_UNIQUE;
      break;
    }
  }
  return SQLITE_OK;
}

static int vt0_open(sqlite3_vtab*vt,sqlite3_vtab_cursor**cur) {
  *cur=sqlite3_malloc(sizeof(Cursor));
  return *cur?SQLITE_OK:SQLITE_NOMEM;
}

static int vt0_rename(sqlite3_vtab*vt,const char*z) {
  return SQLITE_ERROR;
}

static int vt0_rowid(sqlite3_vtab_cursor*pcur,sqlite3_int64*rowid) {
  Cursor*cur=(void*)pcur;
  *rowid=cur->rowid;
  return SQLITE_OK;
}

#define Module(a,...) static const sqlite3_module a={ \
  .xBestIndex=vt0_index, .xClose=vt0_close, .xConnect=vt0_connect, .xDisconnect=vt0_disconnect, \
  .xEof=vt0_eof, .xOpen=vt0_open, .xRename=vt0_rename, .xRowid=vt0_rowid, __VA_ARGS__ };

static int vt1_messages_column(sqlite3_vtab_cursor*pcur,sqlite3_context*cxt,int n) {
  Cursor*cur=(void*)pcur;
  switch(n) {
    case 0: // ID
      sqlite3_result_int64(cxt,cur->rowid);
      break;
    case 1: // NAME
      if(sqlite3_vtab_nochange(cxt)) return SQLITE_OK;
      if(cur->rowid<256) sqlite3_result_text(cxt,standard_message_names[cur->rowid],-1,0);
      else sqlite3_result_text(cxt,sqlite3_mprintf("#%s",messages[cur->rowid-256]),-1,sqlite3_free);
      break;
    case 2: // TRACE
      if(cur->rowid>=0 && cur->rowid<0x4100) sqlite3_result_int(cxt,message_trace[cur->rowid>>3]&(1<<(cur->rowid&7))?1:0);
      break;
  }
  return SQLITE_OK;
}

static int vt1_messages_filter(sqlite3_vtab_cursor*pcur,int idxNum,const char*idxStr,int argc,sqlite3_value**argv) {
  Cursor*cur=(void*)pcur;
  if(argc) {
    cur->rowid=sqlite3_value_int64(*argv);
    cur->unique=1;
    if(cur->rowid>=0x4100 || cur->rowid<0) cur->eof=1;
    else if(cur->rowid>=N_MESSAGES || cur->rowid<256) cur->eof=1;
    else cur->eof=messages[cur->rowid-256]?0:1;
  } else {
    cur->rowid=0;
    cur->unique=0;
    cur->eof=0;
  }
  return SQLITE_OK;
}

static int vt1_messages_next(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  if(cur->unique) {
    cur->eof=1;
  } else if(cur->rowid<256) {
    ++cur->rowid;
    if(cur->rowid==N_MESSAGES) {
      cur->rowid=256;
      if(!*messages) goto next_user_msg;
    }
  } else {
    next_user_msg:
    for(;;) {
      if(++cur->rowid==0x4100) {
        cur->eof=1;
        break;
      }
      if(messages[cur->rowid-256]) break;
    }
  }
  return SQLITE_OK;
}

static int vt1_messages_update(sqlite3_vtab*vt,int argc,sqlite3_value**argv,sqlite3_int64*rowid) {
  sqlite3_int64 id;
  int v;
  if(argc!=5 || sqlite3_value_type(*argv)!=SQLITE_INTEGER) return SQLITE_CONSTRAINT_VTAB;
  id=sqlite3_value_int64(*argv);
  if(id!=sqlite3_value_int64(argv[1])) return SQLITE_CONSTRAINT_VTAB;
  if(sqlite3_value_type(argv[4])!=SQLITE_INTEGER) return SQLITE_MISMATCH;
  v=sqlite3_value_int(argv[4]);
  if(v&~1) return SQLITE_CONSTRAINT_CHECK;
  if(id<0 || id>=0x4100) return SQLITE_INTERNAL;
  if(v) message_trace[id>>3]|=1<<(id&7);
  else message_trace[id>>3]&=~(1<<(id&7));
  return SQLITE_OK;
}

Module(vt_messages,
  .xColumn=vt1_messages_column,
  .xFilter=vt1_messages_filter,
  .xNext=vt1_messages_next,
  .xUpdate=vt1_messages_update,
);

void init_sql_functions(sqlite3_int64*ptr0,sqlite3_int64*ptr1) {
  sqlite3_create_function(userdb,"BASENAME",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_basename,0,0);
  sqlite3_create_function(userdb,"LEVEL_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr0,fn_cacheid,0,0);
  sqlite3_create_function(userdb,"MODSTATE",0,SQLITE_UTF8,0,fn_modstate,0,0);
  sqlite3_create_function(userdb,"PICTURE_SIZE",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_picture_size,0,0);
  sqlite3_create_function(userdb,"RESOURCE",-1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_resource,0,0);
  sqlite3_create_function(userdb,"SIGN_EXTEND",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_sign_extend,0,0);
  sqlite3_create_function(userdb,"SOLUTION_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr1,fn_cacheid,0,0);
  sqlite3_create_module(userdb,"MESSAGES",&vt_messages,"CREATE TABLE `MESSAGES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `TRACE` INT);");
}
