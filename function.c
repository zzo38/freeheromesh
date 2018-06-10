#if 0
gcc ${CFLAGS:--s -O2} -c function.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"

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

void init_sql_functions(sqlite3_int64*ptr0,sqlite3_int64*ptr1) {
  sqlite3_create_function(userdb,"BASENAME",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_basename,0,0);
  sqlite3_create_function(userdb,"LEVEL_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr0,fn_cacheid,0,0);
  sqlite3_create_function(userdb,"MODSTATE",0,SQLITE_UTF8,0,fn_modstate,0,0);
  sqlite3_create_function(userdb,"PICTURE_SIZE",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_picture_size,0,0);
  sqlite3_create_function(userdb,"RESOURCE",-1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_resource,0,0);
  sqlite3_create_function(userdb,"SIGN_EXTEND",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_sign_extend,0,0);
  sqlite3_create_function(userdb,"SOLUTION_CACHEID",0,SQLITE_UTF8|SQLITE_DETERMINISTIC,ptr1,fn_cacheid,0,0);
}
