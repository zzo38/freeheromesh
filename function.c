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
  Uint16 arg[4];
} Cursor;

static void*bizarro_vtab;

static void find_first_usable_image(const Class*cl,sqlite3_context*cxt) {
  int i;
  if(cl->cflags&CF_GROUP) return;
  if(!cl->images) return;
  for(i=0;i<cl->nimages;i++) {
    if(cl->images[i]&0x8000) {
      sqlite3_result_int(cxt,i);
      return;
    }
  }
}

static void fn_basename(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_text(cxt,basefilename,-1,SQLITE_STATIC);
}

static void fn_cacheid(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int64(cxt,*(sqlite3_int64*)sqlite3_user_data(cxt));
}

static void fn_cl(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int a;
  const char*s=sqlite3_value_text(*argv);
  if(!s) return;
  for(a=1;a<0x4000;a++) {
    if(classes[a] && !(classes[a]->cflags&CF_NOCLASS2) && !strcmp(s,classes[a]->name)) goto found;
  }
  return;
  found: sqlite3_result_int(cxt,a);
}

static void fn_class_data(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int id=sqlite3_value_int(argv[0])&0xFFFF;
  Class*cl;
  if(id<0 || id>=0x4000 || !classes[id]) return;
  cl=classes[id];
  switch(sqlite3_value_int(argv[1])&255) {
    case 0: sqlite3_result_int(cxt,id); break;
    case 1: sqlite3_result_int64(cxt,cl->temperature); break;
    case 2: sqlite3_result_int(cxt,cl->shape); break;
    case 7: find_first_usable_image(cl,cxt); break;
    case 12: sqlite3_result_int64(cxt,cl->misc4&0xFFFFFFFFULL); break;
    case 13: sqlite3_result_int64(cxt,cl->misc5&0xFFFFFFFFULL); break;
    case 14: sqlite3_result_int64(cxt,cl->misc6&0xFFFFFFFFULL); break;
    case 15: sqlite3_result_int64(cxt,cl->misc7&0xFFFFFFFFULL); break;
    case 18: sqlite3_result_int64(cxt,cl->arrivals); break;
    case 19: sqlite3_result_int64(cxt,cl->departures); break;
    case 32: sqlite3_result_int(cxt,cl->oflags&OF_BUSY?1:0); break;
    case 33: sqlite3_result_int(cxt,cl->oflags&OF_INVISIBLE?1:0); break;
    case 34: sqlite3_result_int(cxt,cl->oflags&OF_USERSIGNAL?1:0); break;
    case 35: sqlite3_result_int(cxt,cl->oflags&OF_USERSTATE?1:0); break;
    case 36: sqlite3_result_int(cxt,cl->oflags&OF_KEYCLEARED?1:0); break;
    case 37: sqlite3_result_int(cxt,cl->cflags&CF_PLAYER?1:0); break;
    case 38: sqlite3_result_int(cxt,cl->oflags&OF_DESTROYED?1:0); break;
    case 39: sqlite3_result_int(cxt,cl->oflags&OF_STEALTHY?1:0); break;
    case 40: sqlite3_result_int(cxt,cl->oflags&OF_VISUALONLY?1:0); break;
    case 64: sqlite3_result_int64(cxt,cl->density); break;
    case 65: sqlite3_result_int64(cxt,cl->volume); break;
    case 66: sqlite3_result_int64(cxt,cl->strength); break;
    case 67: sqlite3_result_int64(cxt,cl->weight); break;
    case 69: sqlite3_result_int64(cxt,cl->height); break;
    case 70: sqlite3_result_int64(cxt,cl->climb); break;
    case 72: sqlite3_result_int(cxt,cl->hard[0]); break;
    case 73: sqlite3_result_int(cxt,cl->hard[1]); break;
    case 74: sqlite3_result_int(cxt,cl->hard[2]); break;
    case 75: sqlite3_result_int(cxt,cl->hard[3]); break;
    case 76: sqlite3_result_int(cxt,cl->sharp[0]); break;
    case 77: sqlite3_result_int(cxt,cl->sharp[1]); break;
    case 78: sqlite3_result_int(cxt,cl->sharp[2]); break;
    case 79: sqlite3_result_int(cxt,cl->sharp[3]); break;
    case 80: sqlite3_result_int(cxt,(cl->shape>>0)&3); break;
    case 81: sqlite3_result_int(cxt,(cl->shape>>2)&3); break;
    case 82: sqlite3_result_int(cxt,(cl->shape>>4)&3); break;
    case 83: sqlite3_result_int(cxt,(cl->shape>>6)&3); break;
    case 84: sqlite3_result_int(cxt,cl->shovable); break;
    case 128: sqlite3_result_double(cxt,cl->volume/(double)max_volume); break;
    case 129: sqlite3_result_int(cxt,cl->uservars); break;
    case 130: sqlite3_result_int(cxt,cl->collisionLayers); break;
    case 132: sqlite3_result_int(cxt,cl->cflags&CF_COMPATIBLE?1:0); break;
  }
}

static void fn_cvalue(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int a;
  if(sqlite3_value_type(*argv)==SQLITE_NULL) return;
  if(sqlite3_value_type(*argv)==SQLITE_TEXT || sqlite3_value_type(*argv)==SQLITE_BLOB) {
    const char*s=sqlite3_value_text(*argv);
    for(a=1;a<0x4000;a++) {
      if(classes[a] && !(classes[a]->cflags&CF_NOCLASS2) && !strcmp(s,classes[a]->name)) goto found;;
    }
    return;
  } else {
    a=sqlite3_value_int(*argv)&0xFFFF;
    if(!a || (a&~0x3FFF) || !classes[a] || (classes[a]->cflags&CF_NOCLASS2)) return;
  }
  found: sqlite3_result_int64(cxt,a|((sqlite3_int64)TY_CLASS<<32));
}

static void fn_heromesh_escape(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const unsigned char*u=sqlite3_value_blob(*argv);
  int un=sqlite3_value_bytes(*argv);
  char*e;
  int en=0;
  int i=0;
  int c;
  int isimg=0;
  if(!u) return;
  if(!un) {
    sqlite3_result_zeroblob(cxt,0);
    return;
  }
  e=sqlite3_malloc((un<<2)+1);
  if(!e) {
    sqlite3_result_error_nomem(cxt);
    return;
  }
  while(i<un) switch(c=u[i++]) {
    case 1 ... 8: e[en++]='\\'; e[en++]=c+'0'-1; break;
    case 10: e[en++]='\\'; e[en++]='n'; break;
    case 11: e[en++]='\\'; e[en++]='l'; break;
    case 12: e[en++]='\\'; e[en++]='c'; break;
    case 14: e[en++]='\\'; e[en++]='i'; isimg=1; break;
    case 15: e[en++]='\\'; e[en++]='b'; break;
    case 16: e[en++]='\\'; e[en++]='q'; break;
    case 30: e[en++]='\\'; e[en++]='d'; isimg=1; break;
    case 31:
      if(i==un) break;
      c=u[i++];
      e[en++]='\\';
      e[en++]='x';
      e[en++]=(c>>4)<10?(c>>4)+'0':(c>>4)+'A'-10;
      e[en++]=c<10?c+'0':c+'A'-10;
      break;
    case 32 ... 127:
      if(c=='\\') {
        if(!isimg) e[en++]=c;
        isimg=0;
      } else if(c=='"') {
        e[en++]='\\';
      }
      e[en++]=c;
      break;
    default:
      e[en++]='\\';
      e[en++]='x';
      e[en++]=(c>>4)<10?(c>>4)+'0':(c>>4)+'A'-10;
      e[en++]=c<10?c+'0':c+'A'-10;
  }
  sqlite3_result_text(cxt,sqlite3_realloc(e,en+1)?:e,en,sqlite3_free);
}

static void fn_heromesh_type(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  static const char*const n[16]={
    [TY_NUMBER]="number",
    [TY_CLASS]="class",
    [TY_MESSAGE]="message",
    [TY_LEVELSTRING]="string",
    [TY_STRING]="string",
    [TY_SOUND]="sound",
    [TY_USOUND]="sound",
    [TY_MARK]="mark",
  };
  int i;
  if(sqlite3_value_type(*argv)!=SQLITE_INTEGER) return;
  i=sqlite3_value_int64(*argv)>>32;
  sqlite3_result_text(cxt,i<0||i>TY_MAXTYPE?"object":n[i]?:"???",-1,SQLITE_STATIC);
}

static void fn_heromesh_unescape(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const unsigned char*e=sqlite3_value_text(*argv);
  int en=sqlite3_value_bytes(*argv);
  char*u;
  int un=0;
  int c,n;
  int isimg=0;
  if(!e) return;
  if(!*e) {
    sqlite3_result_text(cxt,"",0,SQLITE_STATIC);
    return;
  }
  u=sqlite3_malloc(en+1);
  if(!u) {
    sqlite3_result_error_nomem(cxt);
    return;
  }
  while(c=*e++) {
    if(c=='\\') {
      if(isimg) {
        u[un++]=c;
        isimg=0;
      } else switch(c=*e++) {
        case '0' ... '7': u[un++]=c-'0'+1; break;
        case 'b': u[un++]=15; break;
        case 'c': u[un++]=12; break;
        case 'd': u[un++]=30; isimg=1; break;
        case 'i': u[un++]=14; isimg=1; break;
        case 'l': u[un++]=11; break;
        case 'n': u[un++]=10; break;
        case 'q': u[un++]=16; break;
        case 'x':
          c=*e++;
          if(c>='0' && c<='9') n=c-'0';
          else if(c>='A' && c<='F') n=c+10-'A';
          else if(c>='a' && c<='f') n=c+10-'a';
          else break;
          n<<=4;
          c=*e++;
          if(c>='0' && c<='9') n|=c-'0';
          else if(c>='A' && c<='F') n|=c+10-'A';
          else if(c>='a' && c<='f') n|=c+10-'a';
          else break;
          if(n<32) u[un++]=31;
          u[un++]=n?:255;
          break;
        default: u[un++]=c;
      }
    } else {
      u[un++]=c;
    }
  }
  done:
  sqlite3_result_blob(cxt,u,un,sqlite3_free);
}

static void fn_inrect(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int x=sqlite3_value_int(argv[0]);
  int y=sqlite3_value_int(argv[1]);
  if(!editrect.x0 || !editrect.y0) return;
  sqlite3_result_int(cxt,(x>=editrect.x0 && x<=editrect.x1 && y>=editrect.y0 && y<=editrect.y1));
}

static void fn_level(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,*(Uint16*)sqlite3_user_data(cxt));
}

static void fn_level_title(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  if(level_title) sqlite3_result_blob(cxt,level_title,strlen(level_title),SQLITE_TRANSIENT);
}

static void fn_load_level(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const char*s;
  if(!main_options['x']) {
    sqlite3_result_error(cxt,"LOAD_LEVEL function requires -x switch",-1);
    return;
  }
  if(sqlite3_value_type(*argv)==SQLITE_NULL) return;
  s=load_level(sqlite3_value_int(*argv));
  if(s) sqlite3_result_error(cxt,s,-1);
}

static void fn_max_level(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,level_nindex);
}

static void fn_modstate(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,SDL_GetModState());
}

static void fn_move_list(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  if(replay_count) sqlite3_result_blob(cxt,replay_list,replay_count,SQLITE_TRANSIENT);
}

static void fn_movenumber(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,replay_pos);
}

static void fn_mvalue(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  int a;
  if(sqlite3_value_type(*argv)==SQLITE_NULL) return;
  a=sqlite3_value_int(*argv)&0xFFFF;
  sqlite3_result_int64(cxt,a|((sqlite3_int64)TY_MESSAGE<<32LL));
}

static void fn_ovalue(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  Uint32 a;
  if(sqlite3_value_type(*argv)==SQLITE_NULL) {
    sqlite3_result_int(cxt,0);
    return;
  }
  a=sqlite3_value_int64(*argv)&0xFFFFFFFF;
  if(a>=nobjects || !objects[a] || !objects[a]->generation) return; // result is null if object does not exist
  sqlite3_result_int64(cxt,a|((sqlite3_int64)objects[a]->generation<<32));
}

static void fn_pfsize(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,*(Uint8*)sqlite3_user_data(cxt));
}

static void fn_picture_size(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,picture_size);
}

static void fn_pipe(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const char*name=sqlite3_value_text(*argv);
  FILE*fp;
  int i;
  char buf[512];
  sqlite3_str*str;
  if(!name) return;
  fp=popen(name,"r");
  if(!fp) {
    sqlite3_result_error(cxt,"Cannot open pipe",-1);
    return;
  }
  str=sqlite3_str_new(userdb);
  for(;;) {
    i=fread(buf,1,512,fp);
    if(i<=0) break;
    sqlite3_str_append(str,buf,i);
  }
  pclose(fp);
  i=sqlite3_str_errcode(str);
  if(i==SQLITE_NOMEM) {
    sqlite3_result_error_nomem(cxt);
  } else if(i==SQLITE_TOOBIG) {
    sqlite3_result_error_toobig(cxt);
  } else if(i) {
    sqlite3_result_error(cxt,"Unknown error",-1);
  }
  if(i) {
    sqlite3_free(sqlite3_str_finish(str));
    return;
  }
  if(i=sqlite3_str_length(str)) {
    sqlite3_result_blob(cxt,sqlite3_str_finish(str),i,sqlite3_free);
  } else {
    sqlite3_free(sqlite3_str_finish(str));
    sqlite3_result_zeroblob(cxt,0);
  }
}

static void fn_read_lump_at(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_int64 of=sqlite3_value_int64(argv[0]);
  FILE*fp=sqlite3_value_pointer(argv[1],"http://zzo38computer.org/fossil/heromesh.ui#FILE_ptr");
  long sz;
  Uint8*buf;
  if(!fp) return;
  rewind(fp);
  fseek(fp,of-4,SEEK_SET);
  sz=fgetc(fp)<<16; sz|=fgetc(fp)<<24; sz|=fgetc(fp); sz|=fgetc(fp)<<8;
  if(sz>0) {
    buf=malloc(sz);
    if(!buf) fatal("Allocation failed\n");
    if(fread(buf,1,sz,fp)<=0) fatal("I/O error in READ_LUMP_AT() function\n");
    sqlite3_result_blob64(cxt,buf,sz,free);
  } else {
    sqlite3_result_zeroblob64(cxt,0);
  }
}

static void fn_rect_x0(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_result_int(cxt,*(Uint8*)sqlite3_user_data(cxt));
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

static void fn_trace_on(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const char*v=sqlite3_user_data(cxt);
  if(main_options['t']=*v) puts("[Tracing enabled]"); else puts("[Tracing disabled]");
}

static void fn_xy(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  if(sqlite3_value_type(argv[0])==SQLITE_NULL || sqlite3_value_type(argv[1])==SQLITE_NULL) return;
  sqlite3_result_int(cxt,sqlite3_value_int(argv[0])+sqlite3_value_int(argv[1])*64);
}

static void fn_zero_extend(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  sqlite3_int64 a;
  if(sqlite3_value_type(*argv)==SQLITE_NULL) return;
  a=sqlite3_value_int64(*argv)&0xFFFFFFFF;
  sqlite3_result_int64(cxt,a);
}

static int vt0_close(sqlite3_vtab_cursor*cur) {
  sqlite3_free(cur);
  return SQLITE_OK;
}

static int vt0_connect(sqlite3*db,void*aux,int argc,const char*const*argv,sqlite3_vtab**vt,char**err) {
  sqlite3_declare_vtab(db,aux);
  *vt=sqlite3_malloc(sizeof(sqlite3_vtab));
  if(*vt && argv[0][0]=='B') bizarro_vtab=*vt;
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
  if(info->nOrderBy==1 && (info->aOrderBy->iColumn==-1 || !info->aOrderBy->iColumn) && !info->aOrderBy->desc) info->orderByConsumed=1;
  for(i=0;i<info->nConstraint;i++) {
    if((info->aConstraint[i].iColumn==-1 || !info->aConstraint[i].iColumn) && info->aConstraint[i].op==SQLITE_INDEX_CONSTRAINT_EQ && info->aConstraint[i].usable) {
      info->aConstraintUsage[i].argvIndex=1;
      info->aConstraintUsage[i].omit=1;
      info->idxFlags=SQLITE_INDEX_SCAN_UNIQUE;
      info->estimatedCost=1.0;
      info->estimatedRows=1;
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

static int vt1_classes_column(sqlite3_vtab_cursor*pcur,sqlite3_context*cxt,int n) {
  Cursor*cur=(void*)pcur;
  switch(n) {
    case 0: // ID
      sqlite3_result_int64(cxt,cur->rowid);
      break;
    case 1: // NAME
      if(sqlite3_vtab_nochange(cxt)) return SQLITE_OK;
      sqlite3_result_text(cxt,classes[cur->rowid]->name,-1,0);
      break;
    case 2: // EDITORHELP
      if(sqlite3_vtab_nochange(cxt)) return SQLITE_OK;
      sqlite3_result_text(cxt,classes[cur->rowid]->edithelp,-1,0);
      break;
    case 3: // HELP
      if(sqlite3_vtab_nochange(cxt)) return SQLITE_OK;
      sqlite3_result_text(cxt,classes[cur->rowid]->gamehelp,-1,0);
      break;
    case 4: // INPUT
      sqlite3_result_int(cxt,classes[cur->rowid]->cflags&CF_INPUT?1:0);
      break;
    case 5: // QUIZ
      sqlite3_result_int(cxt,classes[cur->rowid]->cflags&CF_QUIZ?1:0);
      break;
    case 6: // TRACEIN
      sqlite3_result_int(cxt,classes[cur->rowid]->cflags&CF_TRACEIN?1:0);
      break;
    case 7: // TRACEOUT
      sqlite3_result_int(cxt,classes[cur->rowid]->cflags&CF_TRACEOUT?1:0);
      break;
    case 8: // GROUP
#if 0
      if(sqlite3_vtab_nochange(cxt)) return SQLITE_OK;
      if(classes[cur->rowid]->cflags&CF_GROUP) {
        char*s=sqlite3_mprintf(" ");
        if(!s) return SQLITE_NOMEM;
        for(n=0;classes[cur->rowid]->codes[n];n++) {
          s=sqlite3_mprintf("%z%s ",s,classes[classes[cur->rowid]->codes[n]]->name);
          if(!s) return SQLITE_NOMEM;
        }
        sqlite3_result_text(cxt,s,-1,sqlite3_free);
      } else {
        sqlite3_result_null(cxt);
      }
#endif
      break;
    case 9: // PLAYER
      sqlite3_result_int(cxt,classes[cur->rowid]->cflags&CF_PLAYER?1:0);
      break;
  }
  return SQLITE_OK;
}

static int vt1_classes_filter(sqlite3_vtab_cursor*pcur,int idxNum,const char*idxStr,int argc,sqlite3_value**argv) {
  Cursor*cur=(void*)pcur;
  cur->eof=0;
  if(argc) {
    cur->rowid=sqlite3_value_int64(*argv);
    cur->unique=1;
    if(cur->rowid<=0 || cur->rowid>=0x4000 || !classes[cur->rowid] || (classes[cur->rowid]->cflags&CF_NOCLASS2)) cur->eof=1;
  } else {
    cur->unique=0;
    cur->rowid=1;
    while(cur->rowid<0x4000 && (!classes[cur->rowid] || (classes[cur->rowid]->cflags&CF_NOCLASS2))) ++cur->rowid;
    if(cur->rowid>=0x4000) cur->eof=1;
  }
  return SQLITE_OK;
}

static int vt1_classes_next(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  if(cur->unique) {
    cur->eof=1;
  } else {
    ++cur->rowid;
    while(cur->rowid<0x4000 && (!classes[cur->rowid] || (classes[cur->rowid]->cflags&CF_NOCLASS2))) ++cur->rowid;
    if(cur->rowid>=0x4000) cur->eof=1;
  }
  return SQLITE_OK;
}

static int vt1_classes_update(sqlite3_vtab*vt,int argc,sqlite3_value**argv,sqlite3_int64*rowid) {
  sqlite3_int64 id;
  int v[3];
  if(argc<2 || sqlite3_value_type(*argv)!=SQLITE_INTEGER) return SQLITE_CONSTRAINT_VTAB;
  id=sqlite3_value_int64(*argv);
  if(id!=sqlite3_value_int64(argv[1])) return SQLITE_CONSTRAINT_VTAB;
  if(id<=0 || id>=0x4000 || !classes[id]) return SQLITE_INTERNAL;
  v[0]=sqlite3_value_int(argv[7]);
  v[1]=sqlite3_value_int(argv[8]);
  v[2]=sqlite3_value_int(argv[9]);
  if((v[0]|v[1]|v[2])&~1) return SQLITE_CONSTRAINT_CHECK;
  if(v[0]) classes[id]->cflags|=CF_QUIZ; else classes[id]->cflags&=~CF_QUIZ;
  classes[id]->cflags&=~(CF_TRACEIN|CF_TRACEOUT);
  classes[id]->cflags|=(v[1]?CF_TRACEIN:0)|(v[2]?CF_TRACEOUT:0);
  return SQLITE_OK;
}

Module(vt_classes,
  .xColumn=vt1_classes_column,
  .xFilter=vt1_classes_filter,
  .xNext=vt1_classes_next,
  .xUpdate=vt1_classes_update,
);

static int vt1_objects_column(sqlite3_vtab_cursor*pcur,sqlite3_context*cxt,int n) {
  Cursor*cur=(void*)pcur;
  switch(n) {
    case 0: // ID
      sqlite3_result_int64(cxt,cur->rowid);
      break;
    case 1: // CLASS
      sqlite3_result_int(cxt,objects[cur->rowid]->class);
      break;
    case 2: // MISC1
      sqlite3_result_int64(cxt,ValueTo64(objects[cur->rowid]->misc1));
      break;
    case 3: // MISC2
      sqlite3_result_int64(cxt,ValueTo64(objects[cur->rowid]->misc2));
      break;
    case 4: // MISC3
      sqlite3_result_int64(cxt,ValueTo64(objects[cur->rowid]->misc3));
      break;
    case 5: // IMAGE
      sqlite3_result_int(cxt,objects[cur->rowid]->image);
      break;
    case 6: // DIR
      sqlite3_result_int(cxt,objects[cur->rowid]->dir&7);
      break;
    case 7: // X
      sqlite3_result_int(cxt,objects[cur->rowid]->x);
      break;
    case 8: // Y
      sqlite3_result_int(cxt,objects[cur->rowid]->y);
      break;
    case 9: // UP
      if(objects[cur->rowid]->up!=VOIDLINK) sqlite3_result_int64(cxt,objects[cur->rowid]->up);
      break;
    case 10: // DOWN
      if(objects[cur->rowid]->down!=VOIDLINK) sqlite3_result_int64(cxt,objects[cur->rowid]->down);
      break;
    case 11: // DENSITY
      sqlite3_result_int(cxt,objects[cur->rowid]->density);
      break;
    case 12: // BIZARRO
      sqlite3_result_int(cxt,objects[cur->rowid]->oflags&OF_BIZARRO?1:0);
      break;
  }
  return SQLITE_OK;
}

static int vt1_objects_next(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  if(cur->unique) {
    eof:
    cur->eof=1;
    return SQLITE_OK;
  }
  again:
  if(cur->arg[0]&0x0008) {
    // Ordered by ID
    if(cur->arg[1] || !objects[cur->rowid]) {
      for(;;) {
        if(++cur->rowid>=nobjects) {
          cur->eof=1;
          break;
        }
        if(objects[cur->rowid]) break;
      }
    }
    cur->arg[1]=1;
  } else if((cur->arg[0]&0x0066) && !cur->arg[1]) {
    // Find top/bottom at location
    cur->arg[1]=1;
    atxy:
    cur->rowid=(cur->pVtab==bizarro_vtab?bizplayfield:playfield)[cur->arg[2]+cur->arg[3]*64-65];
    if(cur->rowid==VOIDLINK) goto nextxy;
    if(cur->arg[0]&0x0020) {
      while(cur->rowid!=VOIDLINK && objects[cur->rowid]->up!=VOIDLINK) cur->rowid=objects[cur->rowid]->up;
    }
  } else if(cur->arg[0]&0x0020) {
    // Go down
    cur->rowid=objects[cur->rowid]->down;
    if(cur->rowid==VOIDLINK) goto nextxy;
  } else if(cur->arg[0]&0x0040) {
    // Go up
    cur->rowid=objects[cur->rowid]->up;
    if(cur->rowid==VOIDLINK) {
      nextxy:
      if(cur->arg[0]&0x1000) {
        cur->arg[2]+=cur->arg[0]&0x0400?-1:1;
        if(cur->arg[0]&0x0002) goto eof;
        if(cur->arg[2]<1 || cur->arg[2]>pfwidth) {
          if(cur->arg[0]&0x0004) goto eof;
          cur->arg[2]=cur->arg[0]&0x0400?pfwidth:1;
          cur->arg[3]+=cur->arg[0]&0x4000?-1:1;
          if(cur->arg[3]<1 || cur->arg[3]>pfheight) goto eof;
        }
      } else {
        cur->arg[3]+=cur->arg[0]&0x4000?-1:1;
        if(cur->arg[0]&0x0004) goto eof;
        if(cur->arg[3]<1 || cur->arg[3]>pfheight) {
          if(cur->arg[0]&0x0002) goto eof;
          cur->arg[3]=cur->arg[0]&0x4000?pfheight:1;
          cur->arg[2]+=cur->arg[0]&0x0400?-1:1;
          if(cur->arg[2]<1 || cur->arg[2]>pfwidth) goto eof;
        }
      }
      goto atxy;
    }
  } else {
    // This shouldn't happen
    return SQLITE_INTERNAL;
  }
  if(!cur->eof) {
    if(!objects[cur->rowid]->generation) goto again;
    if((objects[cur->rowid]->oflags&OF_BIZARRO)!=(cur->pVtab==bizarro_vtab?OF_BIZARRO:0)) goto again;
  }
  return SQLITE_OK;
}

static int vt1_objects_filter(sqlite3_vtab_cursor*pcur,int idxNum,const char*idxStr,int argc,sqlite3_value**argv) {
  Cursor*cur=(void*)pcur;
  int i;
  Uint32 t;
  cur->rowid=-1;
  cur->unique=0;
  cur->eof=0;
  cur->arg[0]=idxNum;
  cur->arg[1]=0;
  cur->arg[2]=cur->arg[3]=1;
  if(!nobjects) {
    cur->eof=1;
    return SQLITE_OK;
  }
  if(idxStr) for(i=0;idxStr[i];i++) switch(idxStr[i]) {
    case 'a':
      cur->rowid=sqlite3_value_int64(argv[i]);
      cur->unique=1;
      break;
    case 'b':
      if(cur->rowid<sqlite3_value_int64(argv[i])) {
        cur->rowid=sqlite3_value_int64(argv[i]);
        cur->arg[1]=1;
        if(cur->unique) cur->eof=1;
      }
      break;
    case 'c':
      if(cur->rowid<=sqlite3_value_int64(argv[i])) {
        cur->rowid=sqlite3_value_int64(argv[i]);
        cur->arg[1]=0;
        if(cur->unique) cur->eof=1;
      }
      break;
    case 'd':
      cur->arg[2]=sqlite3_value_int(argv[i]);
      if(cur->arg[2]<1 || cur->arg[2]>64) cur->eof=1;
      break;
    case 'e':
      cur->arg[3]=sqlite3_value_int(argv[i]);
      if(cur->arg[2]<1 || cur->arg[2]>64) cur->eof=1;
      break;
    case 'f':
      if(sqlite3_value_type(argv[i])==SQLITE_NULL) {
        cur->arg[0]=idxNum&=~0x0010;
      } else {
        t=sqlite3_value_int64(argv[i]);
        if(t>=nobjects || t<0 || !objects[t]) {
          cur->eof=1;
          break;
        }
        cur->rowid=objects[t]->down;
        if(cur->rowid==VOIDLINK) cur->eof=1;
        cur->unique=1;
      }
      break;
    case 'g':
      if(sqlite3_value_type(argv[i])==SQLITE_NULL) {
        cur->arg[0]=idxNum&=~0x0010;
      } else {
        t=sqlite3_value_int64(argv[i]);
        if(t>=nobjects || t<0 || !objects[t]) {
          cur->eof=1;
          break;
        }
        cur->rowid=objects[t]->up;
        if(cur->rowid==VOIDLINK) cur->eof=1;
        cur->unique=1;
      }
      break;
  }
  if(cur->unique) {
    if(cur->rowid<0 || cur->rowid>=nobjects || !objects[cur->rowid]) cur->eof=1;
    return SQLITE_OK;
  }
  if(cur->eof) return SQLITE_OK;
  if(idxNum&0x0400) cur->arg[2]=pfwidth;
  if(idxNum&0x4000) cur->arg[3]=pfheight;
  if(cur->rowid==-1) cur->rowid=0;
  return vt1_objects_next(pcur);
}

static int vt1_objects_index(sqlite3_vtab*vt,sqlite3_index_info*info) {
  sqlite3_str*str=sqlite3_str_new(0);
  int arg=0;
  int i,j;
  info->estimatedCost=100000.0;
  for(i=0;i<info->nConstraint;i++) if(info->aConstraint[i].usable) {
    j=info->aConstraint[i].op;
    switch(info->aConstraint[i].iColumn) {
      case -1: case 0: // ID
        if(info->idxNum&0x0017) break;
        if(j==SQLITE_INDEX_CONSTRAINT_EQ || j==SQLITE_INDEX_CONSTRAINT_IS) {
          info->idxNum|=0x0001;
          info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          info->idxFlags=SQLITE_INDEX_SCAN_UNIQUE;
          sqlite3_str_appendchar(str,1,'a');
          info->estimatedCost/=10000.0;
        } else if(j==SQLITE_INDEX_CONSTRAINT_GT || j==SQLITE_INDEX_CONSTRAINT_GE) {
          info->idxNum|=0x0008;
          //info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          sqlite3_str_appendchar(str,1,j==SQLITE_INDEX_CONSTRAINT_GT?'b':'c');
          info->estimatedCost/=1200.0;
        }
        break;
      case 7: // X
        if(info->idxNum&0x0013) break;
        if(j==SQLITE_INDEX_CONSTRAINT_EQ || j==SQLITE_INDEX_CONSTRAINT_IS) {
          info->idxNum|=0x0002;
          info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          sqlite3_str_appendchar(str,1,'d');
          info->estimatedCost/=64.0;
        }
        break;
      case 8: // Y
        if(info->idxNum&0x0015) break;
        if(j==SQLITE_INDEX_CONSTRAINT_EQ || j==SQLITE_INDEX_CONSTRAINT_IS) {
          info->idxNum|=0x0004;
          info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          sqlite3_str_appendchar(str,1,'e');
          info->estimatedCost/=64.0;
        }
        break;
      case 9: // UP
        if(info->idxNum&0x0019) break;
        if(j==SQLITE_INDEX_CONSTRAINT_IS && !(info->idxNum&0x000E)) {
          info->idxNum|=0x0010;
          info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          sqlite3_str_appendchar(str,1,'f');
          info->estimatedCost/=80.0;
        }
        break;
      case 10: // DOWN
        if(info->idxNum&0x0019) break;
        if(j==SQLITE_INDEX_CONSTRAINT_IS && !(info->idxNum&0x000E)) {
          info->idxNum|=0x0010;
          info->aConstraintUsage[i].omit=1;
          info->aConstraintUsage[i].argvIndex=++arg;
          sqlite3_str_appendchar(str,1,'g');
          info->estimatedCost/=80.0;
        }
        break;
    }
  }
  if(info->nOrderBy==1) {
    if(info->aOrderBy->iColumn==0 && !info->aOrderBy->desc && !(info->idxNum&~0x0008)) info->orderByConsumed=1;
    if(info->aOrderBy->iColumn==-1 && !info->aOrderBy->desc && !(info->idxNum&~0x0008)) info->orderByConsumed=1;
    if(info->aOrderBy->iColumn==7 && !(info->idxNum&~0x0004)) info->orderByConsumed=1,info->idxNum|=info->aOrderBy->desc?0x0400:0x0800;
    if(info->aOrderBy->iColumn==8 && !(info->idxNum&~0x0002)) info->orderByConsumed=1,info->idxNum|=info->aOrderBy->desc?0x4000:0x8000;
    if(info->aOrderBy->iColumn==11 && info->idxNum==0x0006) info->orderByConsumed=1,info->idxNum|=info->aOrderBy->desc?0x0040:0x0020;
  } else if(info->nOrderBy==2 && !(info->idxNum&~0x0066)) {
    if(info->aOrderBy[0].iColumn==7 && info->aOrderBy[1].iColumn==8) {
      info->orderByConsumed=1;
      info->idxNum|=info->aOrderBy[0].desc?0x0400:0x0800;
      info->idxNum|=info->aOrderBy[1].desc?0x4000:0x8000;
    } else if(info->aOrderBy[0].iColumn==8 && info->aOrderBy[1].iColumn==7) {
      info->orderByConsumed=1;
      info->idxNum|=info->aOrderBy[0].desc?0x4000:0x8000;
      info->idxNum|=info->aOrderBy[1].desc?0x0400:0x0800;
      info->idxNum|=0x1000;
    }
  } else if(info->nOrderBy==3 && info->aOrderBy[2].iColumn==11 && !(info->idxNum&~0x0006)) {
    if(info->aOrderBy[0].iColumn==7 && info->aOrderBy[1].iColumn==8) {
      info->orderByConsumed=1;
      info->idxNum|=info->aOrderBy[0].desc?0x0400:0x0800;
      info->idxNum|=info->aOrderBy[1].desc?0x4000:0x8000;
      info->idxNum|=info->aOrderBy[2].desc?0x0040:0x0020;
    } else if(info->aOrderBy[0].iColumn==8 && info->aOrderBy[1].iColumn==7) {
      info->orderByConsumed=1;
      info->idxNum|=info->aOrderBy[0].desc?0x4000:0x8000;
      info->idxNum|=info->aOrderBy[1].desc?0x0400:0x0800;
      info->idxNum|=0x1000;
      info->idxNum|=info->aOrderBy[2].desc?0x0040:0x0020;
    }
  }
  if(info->orderByConsumed) info->estimatedCost-=0.5;
  switch(info->idxNum&0x0006) {
    case 0x0002: info->idxNum&=~0x1000; break;
    case 0x0004: info->idxNum|=0x1000; break;
  }
  if(!info->idxNum) info->idxNum=0x0008;
  if((info->idxNum&0x0006) && !(info->idxNum&0x0060)) info->idxNum|=0x0040;
  if((info->idxNum&0xCC00) && !(info->idxNum&0x0069)) info->idxNum|=0x0040;
  arg=sqlite3_str_errcode(str);
  info->idxStr=sqlite3_str_finish(str);
  info->needToFreeIdxStr=1;
  return arg;
}

static int vt1_objects_update(sqlite3_vtab*vt,int argc,sqlite3_value**argv,sqlite3_int64*rowid) {
  sqlite3_int64 id;
  int x,y,z;
  if(!main_options['e']) return SQLITE_LOCKED;
  if(argc==1) {
    // DELETE
    id=sqlite3_value_int64(argv[0]);
    objtrash(id);
  } else if(sqlite3_value_type(argv[0])==SQLITE_NULL) {
    // INSERT
    if(sqlite3_value_type(argv[1])!=SQLITE_NULL || sqlite3_value_type(argv[2])!=SQLITE_NULL) return SQLITE_CONSTRAINT_VTAB;
    if(sqlite3_value_type(argv[3])==SQLITE_NULL) return SQLITE_CONSTRAINT_NOTNULL;
    if(sqlite3_value_type(argv[7])==SQLITE_NULL) return SQLITE_CONSTRAINT_NOTNULL;
    if(sqlite3_value_type(argv[11])!=SQLITE_NULL) return SQLITE_CONSTRAINT_VTAB;
    if(sqlite3_value_type(argv[12])!=SQLITE_NULL) return SQLITE_CONSTRAINT_VTAB;
    x=sqlite3_value_int(argv[9]);
    y=sqlite3_value_int(argv[10]);
    if(x<1 || x>pfwidth || y<1 || y>pfheight) return SQLITE_CONSTRAINT_CHECK;
    z=sqlite3_value_int(argv[3]);
    if(z<=0 || z>=0x4000 || !classes[z]) return SQLITE_CONSTRAINT_FOREIGNKEY;
    if(sqlite3_value_int(argv[7])<0 || sqlite3_value_int(argv[7])>=classes[z]->nimages) return SQLITE_CONSTRAINT_CHECK;
    id=objalloc(z);
    if(id==VOIDLINK) return SQLITE_CONSTRAINT_VTAB;
    if(vt==bizarro_vtab) objects[id]->oflags|=OF_BIZARRO; else objects[id]->oflags&=~OF_BIZARRO;
    goto update;
  } else {
    // UPDATE
    id=sqlite3_value_int64(argv[0]);
    if(id!=sqlite3_value_int64(argv[1]) || id!=sqlite3_value_int64(argv[2])) return SQLITE_CONSTRAINT_VTAB;
    if(sqlite3_value_type(argv[3])==SQLITE_NULL) return SQLITE_CONSTRAINT_NOTNULL;
    if(sqlite3_value_type(argv[7])==SQLITE_NULL) return SQLITE_CONSTRAINT_NOTNULL;
    x=sqlite3_value_int(argv[9]);
    y=sqlite3_value_int(argv[10]);
    if(x<1 || x>pfwidth || y<1 || y>pfheight) return SQLITE_CONSTRAINT_CHECK;
    if(sqlite3_value_int(argv[3])!=objects[id]->class) return SQLITE_CONSTRAINT_VTAB;
    if(sqlite3_value_int(argv[13])!=objects[id]->density) return SQLITE_CONSTRAINT_VTAB;
    z=objects[id]->class;
    if(sqlite3_value_int(argv[7])<0 || sqlite3_value_int(argv[7])>=classes[z]->nimages) return SQLITE_CONSTRAINT_CHECK;
    if(x!=objects[id]->x || y!=objects[id]->y) {
      pfunlink(id);
      update:
      objects[id]->x=x;
      objects[id]->y=y;
      pflink(id);
    }
    objects[id]->image=sqlite3_value_int(argv[7]);
    objects[id]->dir=sqlite3_value_int(argv[8])&7;
    objects[id]->misc1.u=sqlite3_value_int64(argv[4])&0xFFFF;
    objects[id]->misc1.t=sqlite3_value_int64(argv[4])>>32;
    if(objects[id]->misc1.t&~3) objects[id]->misc1=NVALUE(0);
    objects[id]->misc2.u=sqlite3_value_int64(argv[5])&0xFFFF;
    objects[id]->misc2.t=sqlite3_value_int64(argv[5])>>32;
    if(objects[id]->misc2.t&~3) objects[id]->misc2=NVALUE(0);
    objects[id]->misc3.u=sqlite3_value_int64(argv[6])&0xFFFF;
    objects[id]->misc3.t=sqlite3_value_int64(argv[6])>>32;
    if(objects[id]->misc3.t&~3) objects[id]->misc3=NVALUE(0);
  }
  level_changed=1;
  return SQLITE_OK;
}

Module(vt_objects,
  .xBestIndex=vt1_objects_index,
  .xColumn=vt1_objects_column,
  .xFilter=vt1_objects_filter,
  .xNext=vt1_objects_next,
  .xUpdate=vt1_objects_update,
);

static int vt1_inventory_index(sqlite3_vtab*vt,sqlite3_index_info*info) {
  int i;
  info->estimatedCost=16.0;
  info->estimatedRows=16;
  for(i=0;i<info->nConstraint;i++) if(info->aConstraint[i].usable && info->aConstraint[i].op==SQLITE_INDEX_CONSTRAINT_EQ && !info->aConstraint[i].iColumn) {
    info->aConstraintUsage[i].omit=1;
    info->aConstraintUsage[i].argvIndex=1;
    info->idxFlags=SQLITE_INDEX_SCAN_UNIQUE;
    info->estimatedCost=1.0;
    info->estimatedRows=1;
    break;
  }
  return SQLITE_OK;
}

static int vt1_inventory_next(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  ++cur->rowid;
  if(cur->unique || cur->rowid>=ninventory) cur->eof=1;
  return SQLITE_OK;
}

static int vt1_inventory_filter(sqlite3_vtab_cursor*pcur,int idxNum,const char*idxStr,int argc,sqlite3_value**argv) {
  Cursor*cur=(void*)pcur;
  if(argc) {
    if(sqlite3_value_type(*argv)==SQLITE_NULL) {
      eof:
      cur->eof=1;
      return SQLITE_OK;
    }
    cur->rowid=sqlite3_value_int(*argv);
    if(cur->rowid>=ninventory || cur->rowid<0) goto eof;
    cur->unique=1;
    cur->eof=0;
    return SQLITE_OK;
  } else {
    cur->rowid=-1;
    cur->unique=0;
    cur->eof=0;
  }
  return vt1_inventory_next(pcur);
}

static int vt1_inventory_column(sqlite3_vtab_cursor*pcur,sqlite3_context*cxt,int n) {
  Cursor*cur=(void*)pcur;
  switch(n) {
    case 0: sqlite3_result_int64(cxt,cur->rowid); break;
    case 1: sqlite3_result_int(cxt,inventory[cur->rowid].class); break;
    case 2: sqlite3_result_int(cxt,inventory[cur->rowid].image); break;
    case 3: sqlite3_result_int(cxt,inventory[cur->rowid].value); break;
  }
  return SQLITE_OK;
}

Module(vt_inventory,
  .xBestIndex=vt1_inventory_index,
  .xColumn=vt1_inventory_column,
  .xFilter=vt1_inventory_filter,
  .xNext=vt1_inventory_next,
);

static int vt1_playfield_index(sqlite3_vtab*vt,sqlite3_index_info*info) {
  int i;
  info->estimatedCost=32*32;
  info->estimatedRows=32*64;
  return SQLITE_OK;
}

static int vt1_playfield_next(sqlite3_vtab_cursor*pcur) {
  Cursor*cur=(void*)pcur;
  ++cur->rowid;
  if((cur->rowid&63)>=pfwidth) cur->rowid=(cur->rowid&~63)+64;
  if(cur->rowid/64>=pfheight) cur->eof=1;
  return SQLITE_OK;
}

static int vt1_playfield_filter(sqlite3_vtab_cursor*pcur,int idxNum,const char*idxStr,int argc,sqlite3_value**argv) {
  Cursor*cur=(void*)pcur;
  cur->rowid=0;
  cur->eof=0;
  return SQLITE_OK;
}

static int vt1_playfield_column(sqlite3_vtab_cursor*pcur,sqlite3_context*cxt,int n) {
  Cursor*cur=(void*)pcur;
  switch(n) {
    case 0: sqlite3_result_int(cxt,(cur->rowid&63)+1); break;
    case 1: sqlite3_result_int(cxt,(cur->rowid/64)+1); break;
    case 2: if(playfield[cur->rowid]!=VOIDLINK) sqlite3_result_int64(cxt,playfield[cur->rowid]); break;
    case 3: if(bizplayfield[cur->rowid]!=VOIDLINK) sqlite3_result_int64(cxt,bizplayfield[cur->rowid]); break;
  }
  return SQLITE_OK;
}

Module(vt_playfield,
  .xBestIndex=vt1_playfield_index,
  .xColumn=vt1_playfield_column,
  .xFilter=vt1_playfield_filter,
  .xNext=vt1_playfield_next,
);

void init_sql_functions(sqlite3_int64*ptr0,sqlite3_int64*ptr1) {
  sqlite3_create_function(userdb,"BASENAME",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_basename,0,0);
  sqlite3_create_function(userdb,"CL",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_cl,0,0);
  sqlite3_create_function(userdb,"CLASS_DATA",2,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_class_data,0,0);
  sqlite3_create_function(userdb,"CVALUE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_cvalue,0,0);
  sqlite3_create_function(userdb,"HEROMESH_ESCAPE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_heromesh_escape,0,0);
  sqlite3_create_function(userdb,"HEROMESH_TYPE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_heromesh_type,0,0);
  sqlite3_create_function(userdb,"HEROMESH_UNESCAPE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_heromesh_unescape,0,0);
  sqlite3_create_function(userdb,"INRECT",2,SQLITE_UTF8,0,fn_inrect,0,0);
  sqlite3_create_function(userdb,"LEVEL",0,SQLITE_UTF8,&level_ord,fn_level,0,0);
  sqlite3_create_function(userdb,"LEVEL_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr0,fn_cacheid,0,0);
  sqlite3_create_function(userdb,"LEVEL_ID",0,SQLITE_UTF8,&level_id,fn_level,0,0);
  sqlite3_create_function(userdb,"LEVEL_TITLE",0,SQLITE_UTF8,0,fn_level_title,0,0);
  sqlite3_create_function(userdb,"LOAD_LEVEL",1,SQLITE_UTF8,0,fn_load_level,0,0);
  sqlite3_create_function(userdb,"MAX_LEVEL",0,SQLITE_UTF8,0,fn_max_level,0,0);
  sqlite3_create_function(userdb,"MODSTATE",0,SQLITE_UTF8,0,fn_modstate,0,0);
  sqlite3_create_function(userdb,"MOVE_LIST",0,SQLITE_UTF8,0,fn_move_list,0,0);
  sqlite3_create_function(userdb,"MOVENUMBER",0,SQLITE_UTF8,0,fn_movenumber,0,0);
  sqlite3_create_function(userdb,"MVALUE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_mvalue,0,0);
  sqlite3_create_function(userdb,"NVALUE",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_zero_extend,0,0);
  sqlite3_create_function(userdb,"OVALUE",1,SQLITE_UTF8,0,fn_ovalue,0,0);
  sqlite3_create_function(userdb,"PFHEIGHT",0,SQLITE_UTF8,&pfheight,fn_pfsize,0,0);
  sqlite3_create_function(userdb,"PFWIDTH",0,SQLITE_UTF8,&pfwidth,fn_pfsize,0,0);
  sqlite3_create_function(userdb,"PICTURE_SIZE",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_picture_size,0,0);
  sqlite3_create_function(userdb,"PIPE",1,SQLITE_UTF8,0,fn_pipe,0,0);
  sqlite3_create_function(userdb,"READ_LUMP_AT",2,SQLITE_UTF8,0,fn_read_lump_at,0,0);
  sqlite3_create_function(userdb,"RECT_X0",0,SQLITE_UTF8,&editrect.x0,fn_rect_x0,0,0);
  sqlite3_create_function(userdb,"RECT_X1",0,SQLITE_UTF8,&editrect.x1,fn_rect_x0,0,0);
  sqlite3_create_function(userdb,"RECT_Y0",0,SQLITE_UTF8,&editrect.y0,fn_rect_x0,0,0);
  sqlite3_create_function(userdb,"RECT_Y1",0,SQLITE_UTF8,&editrect.y1,fn_rect_x0,0,0);
  sqlite3_create_function(userdb,"RESOURCE",-1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_resource,0,0);
  sqlite3_create_function(userdb,"SIGN_EXTEND",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_sign_extend,0,0);
  sqlite3_create_function(userdb,"SOLUTION_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr1,fn_cacheid,0,0);
  sqlite3_create_function(userdb,"TRACE_OFF",0,SQLITE_UTF8,"",fn_trace_on,0,0);
  sqlite3_create_function(userdb,"TRACE_ON",0,SQLITE_UTF8,"\x01",fn_trace_on,0,0);
  sqlite3_create_function(userdb,"XY",2,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_xy,0,0);
  sqlite3_create_function(userdb,"ZERO_EXTEND",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_zero_extend,0,0);
  bizarro_vtab=""; // ensure that it is not null
  sqlite3_create_module(userdb,"CLASSES",&vt_classes,"CREATE TABLE `CLASSES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `EDITORHELP` TEXT, `HELP` TEXT,"
   "`INPUT` INT, `QUIZ` INT, `TRACEIN` INT, `TRACEOUT` INT, `GROUP` TEXT, `PLAYER` INT);");
  sqlite3_create_module(userdb,"MESSAGES",&vt_messages,"CREATE TABLE `MESSAGES`(`ID` INTEGER PRIMARY KEY, `NAME` TEXT, `TRACE` INT);");
  sqlite3_create_module(userdb,"OBJECTS",&vt_objects,"CREATE TABLE `OBJECTS`(`ID` INTEGER PRIMARY KEY, `CLASS` INT, `MISC1` INT, `MISC2` INT, `MISC3` INT,"
   "`IMAGE` INT, `DIR` INT, `X` INT, `Y` INT, `UP` INT, `DOWN` INT, `DENSITY` INT HIDDEN, `BIZARRO` INT HIDDEN);");
  sqlite3_create_module(userdb,"BIZARRO_OBJECTS",&vt_objects,"CREATE TABLE `OBJECTS`(`ID` INTEGER PRIMARY KEY, `CLASS` INT, `MISC1` INT, `MISC2` INT, `MISC3` INT,"
   "`IMAGE` INT, `DIR` INT, `X` INT, `Y` INT, `UP` INT, `DOWN` INT, `DENSITY` INT HIDDEN, `BIZARRO` INT HIDDEN);");
  sqlite3_create_module(userdb,"INVENTORY",&vt_inventory,"CREATE TABLE `INVENTORY`(`ID` INTEGER PRIMARY KEY, `CLASS` INT, `IMAGE` INT, `VALUE` INT);");
  sqlite3_create_module(userdb,"PLAYFIELD",&vt_playfield,"CREATE TABLE `PLAYFIELD`(`X` INT, `Y` INT, `OBJ` INT, `BIZARRO_OBJ` INT);");
}
