#if 0
gcc ${CFLAGS:--s -O2} -c exec.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"
#include "instruc.h"

Uint32 max_objects;
Uint32 generation_number;
Object**objects;
Uint32 nobjects;
Value globals[0x800];
Uint32 firstobj=VOIDLINK;
Uint32 lastobj=VOIDLINK;
Uint32 playfield[64*64];
Uint8 pfwidth,pfheight;

typedef struct {
  Uint16 msg;
  Uint32 from;
  Value arg1,arg2,arg3;
} MessageVars;

static jmp_buf my_env;
static const char*my_error;
static MessageVars msgvars;
static char lastimage_processing;

#define Throw(x) (my_error=(x),longjmp(my_env,1))
#define StackReq(x,y)

void pfunlink(Uint32 n) {
  Object*o=objects[n];
  if(o->down==VOIDLINK) playfield[o->x+o->y*64-65]=o->up;
  else objects[o->down]->up=o->up;
  if(o->up!=VOIDLINK) objects[o->up]->down=o->down;
  o->down=o->up=VOIDLINK;
}

void pflink(Uint32 n) {
  Object*o=objects[n];
  int p=o->x+o->y*64-65;
  if(p<0) return;
  if(playfield[p]==VOIDLINK) {
    playfield[p]=n;
  } else {
    Sint32 d=o->density;
    Uint32 m=playfield[p];
    Uint32 t=VOIDLINK;
    while(m!=VOIDLINK && objects[m]->density>=d) t=m,m=objects[m]->up;
    o->down=t;
    o->up=m;
    if(t!=VOIDLINK) objects[t]->up=n; else playfield[p]=n;
    if(m!=VOIDLINK) objects[m]->down=n;
  }
}

#define OBJECT_ARRAY_BLOCK 256
Uint32 objalloc(Uint16 c) {
  // Allocates a new object of the given class; links into the event list but not into the playfield.
  // Does not send any messages or otherwise notify anyone that it has been created.
  // Returns VOIDLINK if object cannot be created.
  Uint32 n;
  Class*cl=classes[c];
  Object*o=calloc(1,sizeof(Object)+cl->uservars*sizeof(Value));
  if(!c || !cl || cl->cflags&(CF_GROUP|CF_NOCLASS1|CF_NOCLASS2)) goto bad;
  if(!o) fatal("Allocation failed\n");
  o->class=c;
  o->generation=generation_number;
#define C(x) o->x=cl->x;
  C(height) C(weight) C(climb) C(density) C(volume) C(strength) C(arrivals) C(departures) C(temperature)
  C(shape) C(shovable) C(oflags)
  C(sharp[0]) C(sharp[1]) C(sharp[2]) C(sharp[3])
  C(hard[0]) C(hard[1]) C(hard[2]) C(hard[3])
#undef C
  o->misc4=NVALUE(cl->misc4);
  o->misc5=NVALUE(cl->misc5);
  o->misc6=NVALUE(cl->misc6);
  o->misc7=NVALUE(cl->misc7);
  if(nobjects) for(n=nobjects-1;;n--) {
    if(!objects[n]) goto found;
    if(!n) break;
  }
  if(nobjects>=max_objects) goto bad;
  objects=realloc(objects,(nobjects+OBJECT_ARRAY_BLOCK)*sizeof(Object*));
  if(!objects) fatal("Allocation failed\n");
  for(n=nobjects;n<nobjects+OBJECT_ARRAY_BLOCK;n++) objects[n]=0;
  n=nobjects;
  nobjects+=OBJECT_ARRAY_BLOCK;
  found:
  o->up=o->down=o->prev=o->next=VOIDLINK;
  if(cl->nmsg || classes[0]->nmsg) {
    o->prev=lastobj;
    if(lastobj!=VOIDLINK) objects[lastobj]->next=n;
    lastobj=n;
    if(firstobj==VOIDLINK) firstobj=n;
  }
  objects[n]=o;
  return n;
  bad:
  free(o);
  return VOIDLINK;
}

static void execute_program(Uint16*code,int ptr,Uint32 obj) {
  Object*o=objects[obj];
  if(StackProtection()) Throw("Call stack overflow");
  for(;;) switch(code[ptr++]) {
    case OP_GOTO: ptr=code[ptr]; break;
    case OP_CALLSUB: execute_program(code,code[ptr++],obj); break;
    case OP_RET: return;
    default: Throw("Internal error: Unrecognized opcode");
  }
}

static Value send_message(Uint32 from,Uint32 to,Uint16 msg,Value arg1,Value arg2,Value arg3) {
  MessageVars saved=msgvars;
  Uint16 c=objects[to]->class;
  Uint16 p=get_message_ptr(c,msg);
  Uint16*code;
  if(p==0xFFFF) {
    p=get_message_ptr(0,msg);
    if(!p) return NVALUE(0);
    code=classes[0]->codes;
  } else {
    code=classes[c]->codes;
  }
  
  msgvars=(MessageVars){msg,from,arg1,arg2,arg3};
  execute_program(code,p,to);
  msgvars=saved;
  
}

void annihilate(void) {
  Uint32 i;
  if(!objects) return;
  for(i=0;i<nobjects;i++) free(objects[i]);
  nobjects=0;
  free(objects);
  objects=0;
  firstobj=lastobj=VOIDLINK;
  for(i=0;i<64*64;i++) playfield[i]=VOIDLINK;
}

const char*execute_turn(int key) {
  if(setjmp(my_env)) return my_error;
  lastimage_processing=0;
  
  return 0;
}
