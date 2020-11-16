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

#ifndef VSTACKSIZE
#define VSTACKSIZE 0x400
#endif

Uint32 max_objects;
Uint32 generation_number;
Object**objects;
Uint32 nobjects;
Value globals[0x800];
Uint32 firstobj=VOIDLINK;
Uint32 lastobj=VOIDLINK;
Uint32 playfield[64*64];
Uint8 pfwidth,pfheight;
Sint8 gameover,key_ignored;

typedef struct {
  Uint16 msg;
  Uint32 from;
  Value arg1,arg2,arg3;
} MessageVars;

static jmp_buf my_env;
static const char*my_error;
static MessageVars msgvars;
static char lastimage_processing,changed;
static Value vstack[VSTACKSIZE];
static int vstackptr;

#define Throw(x) (my_error=(x),longjmp(my_env,1))
#define StackReq(x,y) do{ if(vstackptr<(x)) Throw("Stack underflow"); if(vstackptr-(x)+(y)>=VSTACKSIZE) Throw("Stack overflow"); }while(0)
#define Push(x) (vstack[vstackptr++]=(x))
#define Pop() (vstack[--vstackptr])

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
  // All fields are initialized by the class or to zero.
  // Does not send any messages or otherwise notify anyone that it has been created.
  // Returns VOIDLINK if object cannot be created.
  Uint32 n;
  Class*cl=classes[c];
  Object*o=0;
  if(!c || !cl || cl->cflags&(CF_GROUP|CF_NOCLASS2)) goto bad;
  o=calloc(1,sizeof(Object)+cl->uservars*sizeof(Value));
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

static inline int v_bool(Value v) {
  switch(v.t) {
    case TY_NUMBER: return v.u!=0;
    case TY_SOUND: case TY_USOUND: Throw("Cannot convert sound to boolean");
    default: return 1;
  }
}

static Uint32 v_object(Value v) {
  if(v.t==TY_NUMBER) {
    if(v.u) Throw("Cannot convert number to object");
    return VOIDLINK;
  } else if(v.t>TY_MAXTYPE) {
    if(v.u>=nobjects || !objects[v.u]) Throw("Attempt to use a nonexistent object");
    if(objects[v.u]->generation!=v.t) Throw("Attempt to use a nonexistent object");
    return v.u;
  } else {
    Throw("Cannot convert non-object to object");
  }
}

// Here is where the execution of a Free Hero Mesh bytecode subroutine is executed.
#define NoIgnore() do{ changed=1; }while(0)
#define GetVariableOf(a,b) (i=v_object(Pop()),i==VOIDLINK?NVALUE(0):b(objects[i]->a))
static void execute_program(Uint16*code,int ptr,Uint32 obj) {
  Uint32 i;
  Object*o=objects[obj];
  Value t1,t2;
  if(StackProtection()) Throw("Call stack overflow");
  for(;;) switch(code[ptr++]) {
    case 0x0000 ... 0x00FF: StackReq(0,1); Push(NVALUE(code[ptr-1])); break;
    case 0x0100 ... 0x01FF: StackReq(0,1); Push(NVALUE(code[ptr-1]-0x200)); break;
    case 0x0200 ... 0x02FF: StackReq(0,1); Push(MVALUE(code[ptr-1]&255)); break;
    case 0x0300 ... 0x03FF: StackReq(0,1); Push(UVALUE(code[ptr-1]&255,TY_SOUND)); break;
    case 0x0400 ... 0x04FF: StackReq(0,1); Push(UVALUE(code[ptr-1]&255,TY_USOUND)); break;
    case 0x2000 ... 0x2FFF: StackReq(0,1); Push(o->uservars[code[ptr-1]&0xFFF]); break;
    case 0x3000 ... 0x3FFF: NoIgnore(); StackReq(1,0); o->uservars[code[ptr-1]&0xFFF]=Pop(); break;
    case 0x4000 ... 0x7FFF: StackReq(0,1); Push(CVALUE(code[ptr-1]-0x4000)); break;
    case 0x87E8 ... 0x87FF: StackReq(0,1); Push(NVALUE(1UL<<(code[ptr-1]&31))); break;
    case 0xC000 ... 0xFFFF: StackReq(0,1); Push(NVALUE((code[ptr-1]&0x3FFF)+256)); break;
    case OP_CALLSUB: execute_program(code,code[ptr++],obj); break;
    case OP_CLASS: StackReq(0,1); Push(CVALUE(o->class)); break;
    case OP_CLASS_C: StackReq(1,1); Push(GetVariableOf(class,CVALUE)); break;
    case OP_DROP: StackReq(1,0); Pop(); break;
    case OP_DROP_D: StackReq(2,0); Pop(); Pop(); break;
    case OP_DUP: StackReq(1,2); t1=Pop(); Push(t1); Push(t1); break;
    case OP_GOTO: ptr=code[ptr]; break;
    case OP_IF: StackReq(1,0); if(v_bool(Pop())) ptr=code[ptr]; else ptr++; break;
    case OP_INT16: StackReq(0,1); Push(NVALUE(code[ptr++])); break;
    case OP_INT32: StackReq(0,1); t1=UVALUE(code[ptr++]<<16,TY_NUMBER); t1.u|=code[ptr++]; Push(t1); break;
    case OP_LAND: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) && v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LNOT: StackReq(1,1); if(v_bool(Pop())) Push(NVALUE(0)); else Push(NVALUE(1)); break;
    case OP_LOR: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) || v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LOSELEVEL: gameover=-1; Throw(0); break;
    case OP_NIP: StackReq(2,1); t1=Pop(); Pop(); Push(t1); break;
    case OP_RET: return;
    case OP_SWAP: StackReq(2,2); t1=Pop(); t2=Pop(); Push(t1); Push(t2); break;
    case OP_WINLEVEL: key_ignored=0; gameover=1; Throw(0); break;
    default: Throw("Internal error: Unrecognized opcode");
  }
}

static Value send_message(Uint32 from,Uint32 to,Uint16 msg,Value arg1,Value arg2,Value arg3) {
  MessageVars saved=msgvars;
  Uint16 c=objects[to]->class;
  Uint16 p=get_message_ptr(c,msg);
  Uint16*code;
  int stackptr=vstackptr;
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
  if(vstackptr<stackptr) Throw("Message code used up too much data from stack");
  if(vstackptr>stackptr+1) Throw("Message code left too much data on stack");
  if(vstackptr==stackptr) {
    return NVALUE(0);
  } else {
    return Pop();
  }
}

void annihilate(void) {
  Uint32 i;
  for(i=0;i<64*64;i++) playfield[i]=VOIDLINK;
  firstobj=lastobj=VOIDLINK;
  if(!objects) return;
  for(i=0;i<nobjects;i++) free(objects[i]);
  nobjects=0;
  free(objects);
  objects=0;
  gameover=0;
}

const char*execute_turn(int key) {
  // Set key=0 for initialization
  if(setjmp(my_env)) return my_error;
  changed=0;
  key_ignored=0;
  lastimage_processing=0;
  vstackptr=0;
  
  if(key_ignored && changed) return "Invalid use of IgnoreKey";
  
  if(key_ignored && changed) return "Invalid use of IgnoreKey";
  return 0;
}
