#if 0
gcc ${CFLAGS:--s -O2} -c -fwrapv exec.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "quarks.h"
#include "heromesh.h"
#include "instruc.h"
#include "names.h"

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
Uint32 playfield[64*64]; // bottom-most object per cell
Uint8 pfwidth,pfheight;
Sint8 gameover,key_ignored;
Uint8 generation_number_inc;
Uint32 move_number;

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
static const char*traceprefix;
static Uint8 current_key;

#define Throw(x) (my_error=(x),longjmp(my_env,1))
#define StackReq(x,y) do{ if(vstackptr<(x)) Throw("Stack underflow"); if(vstackptr-(x)+(y)>=VSTACKSIZE) Throw("Stack overflow"); }while(0)
#define Push(x) (vstack[vstackptr++]=(x))
#define Pop() (vstack[--vstackptr])

// For arrival/departure masks
#define Xbit(a) ((a)%5-2)
#define Ybit(a) (2-(a)/5)

static Value send_message(Uint32 from,Uint32 to,Uint16 msg,Value arg1,Value arg2,Value arg3);
static Uint32 broadcast(Uint32 from,int c,Uint16 msg,Value arg1,Value arg2,Value arg3,int s);

const char*value_string_ptr(Value v) {
  switch(v.t) {
    case TY_STRING: return stringpool[v.u];
    //TODO: Level strings
    default: fatal("Internal confusion: Trying to get string pointer for a non-string\n");
  }
}

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
  if(generation_number_inc) {
    generation_number_inc=0;
    ++generation_number;
    if(generation_number<=TY_MAXTYPE) goto bad;
  }
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

#define animfree free

void objtrash(Uint32 n) {
  Object*o=objects[n];
  if(!o) return;
  if(o->anim && !(o->oflags&(OF_INVISIBLE|OF_BIZARRO))) {
    //TODO: Check that the animation is actually playing
    // If there is an animation, continue to display it, but let it have no effect on the game behaviour
    // This may happen if the LASTIMAGE message destroys the object.
    o->oflags=OF_DESTROYED|OF_STEALTHY|OF_VISUALONLY;
    o->generation=0;
    //TODO
    return;
  }
  animfree(o->anim);
  if(o->down==VOIDLINK) playfield[o->x+o->y*64-65]=o->up;
  else objects[o->down]->up=o->up;
  if(o->up!=VOIDLINK) objects[o->up]->down=o->down;
  o->down=o->up=VOIDLINK;
  if(firstobj==n) firstobj=o->next;
  if(lastobj==n) lastobj=o->prev;
  if(o->prev!=VOIDLINK) objects[o->prev]->next=o->next;
  if(o->next!=VOIDLINK) objects[o->next]->prev=o->prev;
  free(o);
  objects[n]=0;
  generation_number_inc=1;
}

static inline Uint8 resolve_dir(Uint32 n,Uint32 d) {
  return d<8?d:(objects[n]->dir+d)&7;
}

static void animate(Uint32 n,Uint32 f,Uint32 a0,Uint32 a1,Uint32 t) {
  objects[n]->image=a0;
  //TODO
}

static Uint32 obj_above(Uint32 i) {
  Object*o;
  if(i==VOIDLINK) return VOIDLINK;
  o=objects[i];
  i=o->up;
  while(i!=VOIDLINK) {
    o=objects[i];
    if(!(o->oflags&(OF_DESTROYED|OF_VISUALONLY))) return i;
    i=o->up;
  }
  return VOIDLINK;
}

static Uint32 obj_below(Uint32 i) {
  Object*o;
  if(i==VOIDLINK) return VOIDLINK;
  o=objects[i];
  i=o->down;
  while(i!=VOIDLINK) {
    o=objects[i];
    if(!(o->oflags&(OF_DESTROYED|OF_VISUALONLY))) return i;
    i=o->down;
  }
  return VOIDLINK;
}

static Uint32 obj_class_at(Uint32 c,Uint32 x,Uint32 y) {
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return VOIDLINK;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    if(objects[i]->class==c && !(objects[i]->oflags&OF_DESTROYED)) return i;
    i=objects[i]->up;
  }
  return VOIDLINK;
}

static void change_shape(Uint32 n,int d,int v) {
  v&=3;
  v<<=d+d;
  v|=objects[n]->shape&~(3<<(d+d));
  objects[n]->shape=v;
}

static void sink(Uint32 x,Uint32 y) {
  // Before, x(o) was above y(p); now make y(p) above x(o)
  Object*o=objects[x];
  Object*p=objects[y];
  Value v;
  p->up=o->up;
  o->down=p->down;
  o->up=y;
  p->down=x;
  if(!(o->oflags&p->oflags&OF_VISUALONLY)) v=send_message(x,y,MSG_FLOATED,NVALUE(0),NVALUE(0),NVALUE(0));
  if(!(o->oflags&p->oflags&OF_VISUALONLY)) send_message(y,x,MSG_SUNK,NVALUE(0),NVALUE(0),v);
}

static void change_density(Uint32 n,Sint32 v) {
  Object*o=objects[n];
  Uint32 i;
  if(v<o->density) {
    o->density=v;
    for(;;) {
      i=o->up;
      if(i==VOIDLINK || objects[i]->density<=v) return;
      sink(i,n);
    }
  } else if(v>o->density) {
    o->density=v;
    for(;;) {
      i=o->down;
      if(i==VOIDLINK || objects[i]->density>=v) return;
      sink(n,i);
    }
  }
}

static inline int v_bool(Value v) {
  switch(v.t) {
    case TY_NUMBER: return v.u!=0;
    case TY_SOUND: case TY_USOUND: Throw("Cannot convert sound to boolean");
    default: return 1;
  }
}

static inline int v_equal(Value x,Value y) {
  if(x.t==TY_SOUND || y.t==TY_SOUND || x.t==TY_USOUND || y.t==TY_USOUND) Throw("Cannot compare sounds");
  if(x.t==y.t && x.u==y.u) return 1;
  if((x.t==TY_STRING || x.t==TY_LEVELSTRING) && (y.t==TY_STRING || y.t==TY_LEVELSTRING)) {
    if(!strcmp(value_string_ptr(x),value_string_ptr(y))) return 1;
  }
  return 0;
}

static Uint32 v_object(Value v) {
  if(v.t==TY_NUMBER) {
    if(v.u) Throw("Cannot convert non-zero number to object");
    return VOIDLINK;
  } else if(v.t>TY_MAXTYPE) {
    if(v.u>=nobjects || !objects[v.u]) Throw("Attempt to use a nonexistent object");
    if(objects[v.u]->generation!=v.t) Throw("Attempt to use a nonexistent object");
    return v.u;
  } else {
    Throw("Cannot convert non-object to object");
  }
}

static inline int v_sh_dir(Value x) {
  if(x.t!=TY_NUMBER) Throw("Type mismatch");
  if(x.u&~6) Throw("Direction must be E, N, W, or S");
  return x.u>>1;
}

static inline int v_signed_greater(Value x,Value y) {
  if(x.t!=TY_NUMBER || y.t!=TY_NUMBER) Throw("Type mismatch");
  return x.s>y.s;
}

static inline int v_unsigned_greater(Value x,Value y) {
  if(x.t!=TY_NUMBER || y.t!=TY_NUMBER) Throw("Type mismatch");
  return x.u>y.u;
}

static Uint32 create(Uint32 from,Uint16 c,Uint32 x,Uint32 y,Uint32 im,Uint32 d) {
  Uint32 m,n;
  int i,xx,yy;
  Object*o;
  Object*p;
  Value v;
  if(d>7) d=0;
  if(x<1 || y<1 || x>pfwidth || y>pfheight) return VOIDLINK;
  //TODO: Handle collisions
  n=objalloc(c);
  if(n==VOIDLINK) return VOIDLINK;
  o=objects[n];
  o->x=x;
  o->y=y;
  o->image=im;
  o->dir=d;
  pflink(n);
  v=send_message(from,n,MSG_CREATE,NVALUE(0),NVALUE(0),NVALUE(0));
  if(o->oflags&OF_DESTROYED) return VOIDLINK;
  for(i=25;i>=0;i--) {
    xx=x+Xbit(i); yy=y+Ybit(i);
    if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
    m=playfield[xx+yy*64-65];
    while(m!=VOIDLINK) {
      p=objects[m];
      if(p->arrivals&(1<<i)) send_message(n,m,MSG_CREATED,NVALUE(x),NVALUE(y),v);
      m=p->up;
    }
  }
  if(o->oflags&OF_DESTROYED) return VOIDLINK;
  m=obj_above(n);
  if(m!=VOIDLINK) {
    v=send_message(VOIDLINK,n,MSG_SUNK,NVALUE(0),NVALUE(0),v);
    while(m!=VOIDLINK) {
      send_message(n,m,MSG_FLOATED,NVALUE(0),NVALUE(0),v);
      m=obj_above(m);
    }
  }
  if(o->oflags&OF_DESTROYED) return VOIDLINK;
  return n;
}

static Value destroy(Uint32 from,Uint32 to,Uint32 why) {
  Object*o;
  Value v;
  int i,x,y,xx,yy;
  Uint32 n;
  if(n==VOIDLINK) return NVALUE(0);
  o=objects[to];
  // Not checking if it is already destroyed, since EKS Hero Mesh doesn't check.
  v=send_message(from,to,MSG_DESTROY,NVALUE(0),NVALUE(0),NVALUE(why));
  if(!v_bool(v)) {
    o->oflags|=OF_DESTROYED;
    if(firstobj==to) firstobj=o->next;
    if(lastobj==to) lastobj=o->prev;
    if(o->prev!=VOIDLINK) objects[o->prev]->next=o->next;
    if(o->next!=VOIDLINK) objects[o->next]->prev=o->prev;
    if(!(o->oflags&OF_VISUALONLY)) {
      // Not checking for stealth; stealth only applies to movement, not destruction
      xx=o->x; yy=o->y;
      for(i=25;i>=0;i--) {
        x=xx+Xbit(i); y=yy+Ybit(i);
        if(x<1 || x>pfwidth || y<1 || y>pfheight) continue;
        n=playfield[x+y*64-65];
        while(n!=VOIDLINK) {
          o=objects[n];
          if(o->departures&(1<<i)) send_message(to,n,MSG_DESTROYED,NVALUE(0),NVALUE(0),NVALUE(why));
          n=o->up;
        }
      }
    }
  }
  return v;
}

static void trace_stack(Uint32 obj) {
  Value t2=Pop();
  Value t1=Pop();
  Value t0=Pop();
  Object*o;
  if(!main_options['t']) return;
  if(!traceprefix) {
    optionquery[1]=Q_tracePrefix;
    traceprefix=xrm_get_resource(resourcedb,optionquery,optionquery,2);
    if(!traceprefix) traceprefix="";
  }
  printf("%s%d : Trace : %d : %u %u",traceprefix,move_number,vstackptr,t0.t,t0.u);
  if(t0.t>TY_MAXTYPE && t0.u<nobjects && objects[t0.u] && objects[t0.u]->generation==t0.t) {
    o=objects[t0.u];
    printf(" [$%s %d %d]",classes[o->class]->name,o->x,o->y);
  }
  o=objects[obj];
  printf(" : %u %u [$%s %d %d]",o->generation,obj,classes[o->class]->name,o->x,o->y);
  printf(" : %u %u : %u %u\n",t1.t,t1.u,t2.t,t2.u);
}

static inline Value v_broadcast(Uint32 from,Value c,Value msg,Value arg1,Value arg2,Value arg3,int s) {
  if(msg.t!=TY_MESSAGE) Throw("Type mismatch");
  if(c.t!=TY_CLASS && (c.t!=TY_NUMBER || c.u)) Throw("Type mismatch");
  return NVALUE(broadcast(from,c.u,msg.u,arg1,arg2,arg3,s));
}

static inline Value v_create(Uint32 from,Value cl,Value x,Value y,Value im,Value d) {
  Uint32 n;
  if(!cl.t && !cl.u) return NVALUE(0);
  if(cl.t!=TY_CLASS || x.t || x.t || y.t || im.t || d.t) Throw("Type mismatch");
  n=create(from,cl.u,x.u,y.u,im.u,d.u);
  return OVALUE(n);
}

static inline Value v_obj_class_at(Value c,Value x,Value y) {
  Uint32 i;
  if(c.t==TY_NUMBER && !c.u) return NVALUE(0);
  if(c.t!=TY_CLASS || x.t!=TY_NUMBER || y.t!=TY_NUMBER) Throw("Type mismatch");
  i=obj_class_at(c.u,x.u,y.u);
  return OVALUE(i);
}

static inline Value v_send_message(Uint32 from,Value to,Value msg,Value arg1,Value arg2,Value arg3) {
  if(msg.t!=TY_MESSAGE) Throw("Type mismatch");
  if(!to.t && !to.u) return NVALUE(0);
  return send_message(from,v_object(to),msg.u,arg1,arg2,arg3);
}

static inline Value v_send_self(Uint32 from,Value msg,Value arg1,Value arg2,Value arg3) {
  if(msg.t!=TY_MESSAGE) Throw("Type mismatch");
  return send_message(from,from,msg.u,arg1,arg2,arg3);
}

// Here is where the execution of a Free Hero Mesh bytecode subroutine is executed.
#define NoIgnore() do{ changed=1; }while(0)
#define GetVariableOf(a,b) (i=v_object(Pop()),i==VOIDLINK?NVALUE(0):b(objects[i]->a))
#define GetVariableOrAttributeOf(a,b) (t2=Pop(),t2.t==TY_CLASS?NVALUE(classes[t2.u]->a):(i=v_object(t2),i==VOIDLINK?NVALUE(0):b(objects[i]->a)))
#define Numeric(a) do{ if((a).t!=TY_NUMBER) Throw("Type mismatch"); }while(0)
#define DivideBy(a) do{ Numeric(a); if(!(a).u) Throw("Division by zero"); }while(0)
#define GetFlagOf(a) t2=Pop(),Push(t2.t==TY_CLASS?NVALUE(classes[t2.u]->oflags&a?1:0):(i=v_object(t2),i==VOIDLINK?NVALUE(0):NVALUE(objects[i]->oflags&a?1:0)))
#define GetClassFlagOf(a) t2=Pop(),Push(t2.t==TY_CLASS?NVALUE(classes[t2.u]->cflags&a?1:0):(i=v_object(t2),i==VOIDLINK?NVALUE(0):NVALUE(classes[objects[i]->class]->cflags&a?1:0)))
#define SetFlagOf(a) do { t2=Pop(); i=v_object(Pop()); if(i!=VOIDLINK) { if(v_bool(t2)) objects[i]->oflags|=a; else objects[i]->oflags&=~a; } }while(0)
static void execute_program(Uint16*code,int ptr,Uint32 obj) {
  Uint32 i,j;
  Object*o=objects[obj];
  Value t1,t2;
  static Value t3,t4,t5;
  if(StackProtection()) Throw("Call stack overflow");
  // Note about bit shifting: At least when running Hero Mesh in DOSBOX, out of range bit shifts produce zero.
  // I don't know if this is true on all computers that Hero Mesh runs on, though. (Some documents suggest that x86 doesn't work this way)
  // The below code assumes that signed right shifting is available.
  for(;;) switch(code[ptr++]) {
    case 0x0000 ... 0x00FF: StackReq(0,1); Push(NVALUE(code[ptr-1])); break;
    case 0x0100 ... 0x01FF: StackReq(0,1); Push(NVALUE(code[ptr-1]-0x200)); break;
    case 0x0200 ... 0x02FF: StackReq(0,1); Push(MVALUE(code[ptr-1]&255)); break;
    case 0x0300 ... 0x03FF: StackReq(0,1); Push(UVALUE(code[ptr-1]&255,TY_SOUND)); break;
    case 0x0400 ... 0x04FF: StackReq(0,1); Push(UVALUE(code[ptr-1]&255,TY_USOUND)); break;
    case 0x1000 ... 0x107F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u+=code[ptr-1]&0x7F; Push(t1); break; // +
    case 0x1080 ... 0x10FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u-=code[ptr-1]&0x7F; Push(t1); break; // -
    case 0x1100 ... 0x117F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u*=code[ptr-1]&0x7F; Push(t1); break; // *
    case 0x1181 ... 0x11FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u/=code[ptr-1]&0x7F; Push(t1); break; // /
    case 0x1201 ... 0x127F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u%=code[ptr-1]&0x7F; Push(t1); break; // mod
    case 0x1280 ... 0x12FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s*=code[ptr-1]&0x7F; Push(t1); break; // ,*
    case 0x1301 ... 0x137F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s/=code[ptr-1]&0x7F; Push(t1); break; // ,/
    case 0x1381 ... 0x13FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s%=code[ptr-1]&0x7F; Push(t1); break; // ,mod
    case 0x1400 ... 0x147F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u&=code[ptr-1]&0x7F; Push(t1); break; // band
    case 0x1480 ... 0x14FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u|=code[ptr-1]&0x7F; Push(t1); break; // bor
    case 0x1500 ... 0x157F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u^=code[ptr-1]&0x7F; Push(t1); break; // bxor
    case 0x1580 ... 0x15FF: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u=code[ptr-1]&0x60?0:t1.u<<(code[ptr-1]&31); Push(t1); break; // lsh
    case 0x1600 ... 0x167F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.u=code[ptr-1]&0x60?0:t1.u>>(code[ptr-1]&31); Push(t1); break; // rsh
    case 0x1680 ... 0x168F: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s=code[ptr-1]&0x60?(t1.s<0?-1:0):t1.s>>(code[ptr-1]&31); Push(t1); break; // ,rsh
    case 0x1700 ... 0x177F: StackReq(1,1); t1=Pop(); Push(NVALUE(t1.t?0:t1.u==(code[ptr-1]&0x7F)?1:0)); break; // eq
    case 0x1780 ... 0x17FF: StackReq(1,1); t1=Pop(); Push(NVALUE(t1.t?1:t1.u==(code[ptr-1]&0x7F)?0:1)); break; // ne
    case 0x1800 ... 0x187F: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u<(code[ptr-1]&0x7F)?1:0)); break; // lt
    case 0x1880 ... 0x18FF: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u>(code[ptr-1]&0x7F)?1:0)); break; // gt
    case 0x1900 ... 0x197F: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u<=(code[ptr-1]&0x7F)?1:0)); break; // le
    case 0x1980 ... 0x19FF: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u>=(code[ptr-1]&0x7F)?1:0)); break; // ge
    case 0x1A00 ... 0x1A7F: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s<(code[ptr-1]&0x7F)?1:0)); break; // ,lt
    case 0x1A80 ... 0x1AFF: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s>(code[ptr-1]&0x7F)?1:0)); break; // ,gt
    case 0x1B00 ... 0x1B7F: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s<=(code[ptr-1]&0x7F)?1:0)); break; // ,le
    case 0x1B80 ... 0x1BFF: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s>=(code[ptr-1]&0x7F)?1:0)); break; // ,ge
    case 0x1E00 ... 0x1EFF: StackReq(0,1); Push(NVALUE(code[ptr-1]&0xFF)); return; // ret
    case 0x2000 ... 0x27FF: StackReq(0,1); Push(o->uservars[code[ptr-1]&0x7FF]); break;
    case 0x2800 ... 0x28FF: StackReq(0,1); Push(globals[code[ptr-1]&0x7FF]); break;
    case 0x3000 ... 0x37FF: NoIgnore(); StackReq(1,0); o->uservars[code[ptr-1]&0x7FF]=Pop(); break;
    case 0x3800 ... 0x38FF: NoIgnore(); StackReq(1,0); globals[code[ptr-1]&0x7FF]=Pop(); break;
    case 0x4000 ... 0x7FFF: StackReq(0,1); Push(CVALUE(code[ptr-1]-0x4000)); break;
    case 0x87E8 ... 0x87FF: StackReq(0,1); Push(NVALUE(1UL<<(code[ptr-1]&31))); break;
    case 0xC000 ... 0xFFFF: StackReq(0,1); Push(MVALUE((code[ptr-1]&0x3FFF)+256)); break;
    case OP_ADD: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u+t2.u)); break;
    case OP_ANIMATE: StackReq(4,0); t4=Pop(); Numeric(t4); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); animate(obj,t1.u,t2.u,t3.u,t4.u); break;
    case OP_ARG1: StackReq(0,1); Push(msgvars.arg1); break;
    case OP_ARG1_E: StackReq(1,0); msgvars.arg1=Pop(); break;
    case OP_ARG2: StackReq(0,1); Push(msgvars.arg2); break;
    case OP_ARG2_E: StackReq(1,0); msgvars.arg2=Pop(); break;
    case OP_ARG3: StackReq(0,1); Push(msgvars.arg3); break;
    case OP_ARG3_E: StackReq(1,0); msgvars.arg3=Pop(); break;
    case OP_ARRIVALS: StackReq(0,1); Push(NVALUE(o->arrivals&0x1FFFFFF)); break;
    case OP_ARRIVALS_C: StackReq(1,1); Push(GetVariableOrAttributeOf(arrivals&0x1FFFFFF,NVALUE)); break;
    case OP_ARRIVALS_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->arrivals=t1.u; break;
    case OP_ARRIVALS_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->arrivals=t1.u; break;
    case OP_ARRIVED: StackReq(0,1); Push(NVALUE(o->arrived&0x1FFFFFF)); break;
    case OP_ARRIVED_C: StackReq(1,1); Push(GetVariableOf(arrived&0x1FFFFFF,NVALUE)); break;
    case OP_ARRIVED_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->arrived=t1.u; break;
    case OP_ARRIVED_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->arrived=t1.u; break;
    case OP_BAND: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u&t2.u)); break;
    case OP_BNOT: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(~t1.u)); break;
    case OP_BOR: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u|t2.u)); break;
    case OP_BROADCAST: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),0)); break;
    case OP_BROADCAST_D: StackReq(4,0); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),0); break;
    case OP_BROADCASTCLASS: StackReq(3,1); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_broadcast(obj,CVALUE(code[ptr++]),t2,t3,t4,NVALUE(0),0)); break;
    case OP_BROADCASTEX: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,t5,0)); break;
    case OP_BROADCASTEX_D: StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,t5,0); break;
    case OP_BROADCASTSUM: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),1)); break;
    case OP_BROADCASTSUMEX: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,t5,1)); break;
    case OP_BUSY: StackReq(0,1); if(o->oflags&OF_BUSY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_BUSY_C: StackReq(1,1); GetFlagOf(OF_BUSY); break;
    case OP_BUSY_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_BUSY; else o->oflags&=~OF_BUSY; break;
    case OP_BUSY_EC: StackReq(2,0); SetFlagOf(OF_BUSY); break;
    case OP_BXOR: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u^t2.u)); break;
    case OP_CALLSUB: execute_program(code,code[ptr++],obj); break;
    case OP_CLASS: StackReq(0,1); Push(CVALUE(o->class)); break;
    case OP_CLASS_C: StackReq(1,1); Push(GetVariableOf(class,CVALUE)); break;
    case OP_CLIMB: StackReq(0,1); Push(NVALUE(o->climb)); break;
    case OP_CLIMB_C: StackReq(1,1); Push(GetVariableOrAttributeOf(climb,NVALUE)); break;
    case OP_CLIMB_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->climb=t1.u; break;
    case OP_CLIMB_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->climb=t1.u&0xFFFF; break;
    case OP_CLIMB_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->climb=t1.u; break;
    case OP_CLIMB_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->climb=t1.u; break;
    case OP_COMPATIBLE: StackReq(0,1); if(classes[o->class]->cflags&CF_COMPATIBLE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_COMPATIBLE_C: StackReq(1,1); GetClassFlagOf(CF_COMPATIBLE); break;
    case OP_CREATE: NoIgnore(); StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_create(obj,t1,t2,t3,t4,t5)); break;
    case OP_DENSITY: StackReq(0,1); Push(NVALUE(o->density)); break;
    case OP_DENSITY_C: StackReq(1,1); Push(GetVariableOrAttributeOf(density,NVALUE)); break;
    case OP_DENSITY_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); change_density(obj,t1.s); break;
    case OP_DENSITY_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); change_density(obj,t1.s&0xFFFF); break;
    case OP_DENSITY_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); change_density(v_object(Pop()),t1.s); break;
    case OP_DENSITY_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); change_density(v_object(Pop()),t1.s&0xFFFF); break;
    case OP_DEPARTED: StackReq(0,1); Push(NVALUE(o->departed&0x1FFFFFF)); break;
    case OP_DEPARTED_C: StackReq(1,1); Push(GetVariableOf(departed&0x1FFFFFF,NVALUE)); break;
    case OP_DEPARTED_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->departed=t1.u; break;
    case OP_DEPARTED_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->departed=t1.u; break;
    case OP_DEPARTURES: StackReq(0,1); Push(NVALUE(o->departures&0x1FFFFFF)); break;
    case OP_DEPARTURES_C: StackReq(1,1); Push(GetVariableOrAttributeOf(departures&0x1FFFFFF,NVALUE)); break;
    case OP_DEPARTURES_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->departures=t1.u; break;
    case OP_DEPARTURES_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->departures=t1.u; break;
    case OP_DESTROYED: StackReq(0,1); if(o->oflags&OF_DESTROYED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_DESTROYED_C: StackReq(1,1); GetFlagOf(OF_DESTROYED); break;
    case OP_DIR: StackReq(0,1); Push(NVALUE(o->dir)); break;
    case OP_DIR_C: StackReq(1,1); Push(GetVariableOf(dir,NVALUE)); break;
    case OP_DIR_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->dir=resolve_dir(obj,t1.u); break;
    case OP_DIR_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->dir=resolve_dir(i,t1.u); break;
    case OP_DISTANCE: StackReq(0,1); Push(NVALUE(o->distance)); break;
    case OP_DISTANCE_C: StackReq(1,1); Push(GetVariableOf(distance,NVALUE)); break;
    case OP_DISTANCE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->distance=t1.u; break;
    case OP_DISTANCE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->distance=t1.u; break;
    case OP_DIV: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u/t2.u)); break;
    case OP_DIV_C: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s/t2.s)); break;
    case OP_DROP: StackReq(1,0); Pop(); break;
    case OP_DROP_D: StackReq(2,0); Pop(); Pop(); break;
    case OP_DUP: StackReq(1,2); t1=Pop(); Push(t1); Push(t1); break;
    case OP_EQ: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_equal(t1,t2)?1:0)); break;
    case OP_FROM: StackReq(0,1); Push(OVALUE(msgvars.from)); break;
    case OP_GE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t2,t1)?0:1)); break;
    case OP_GE_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t2,t1)?0:1)); break;
    case OP_GOTO: ptr=code[ptr]; break;
    case OP_GT: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t1,t2)?1:0)); break;
    case OP_GT_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t1,t2)?1:0)); break;
    case OP_HARD: StackReq(1,1); j=v_sh_dir(Pop()); Push(NVALUE(o->hard[j])); break;
    case OP_HARD_C: StackReq(2,1); j=v_sh_dir(Pop()); Push(GetVariableOrAttributeOf(hard[j],NVALUE)); break;
    case OP_HARD_E: NoIgnore(); StackReq(2,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); o->hard[j]=t1.u; break;
    case OP_HARD_EC: NoIgnore(); StackReq(3,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->hard[j]=t1.u; break;
    case OP_HEIGHT: StackReq(0,1); Push(NVALUE(o->height)); break;
    case OP_HEIGHT_C: StackReq(1,1); Push(GetVariableOrAttributeOf(height,NVALUE)); break;
    case OP_HEIGHT_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->height=t1.u; break;
    case OP_HEIGHT_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->height=t1.u&0xFFFF; break;
    case OP_HEIGHT_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->height=t1.u; break;
    case OP_HEIGHT_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->height=t1.u; break;
    case OP_IF: StackReq(1,0); if(v_bool(Pop())) ptr++; else ptr=code[ptr]; break;
    case OP_IMAGE: StackReq(0,1); Push(NVALUE(o->image)); break;
    case OP_IMAGE_C: StackReq(1,1); Push(GetVariableOf(image,NVALUE)); break;
    case OP_IMAGE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->image=t1.u; break;
    case OP_IMAGE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->image=t1.u; break;
    case OP_INT16: StackReq(0,1); Push(NVALUE(code[ptr++])); break;
    case OP_INT32: StackReq(0,1); t1=UVALUE(code[ptr++]<<16,TY_NUMBER); t1.u|=code[ptr++]; Push(t1); break;
    case OP_INVISIBLE: StackReq(0,1); if(o->oflags&OF_INVISIBLE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_INVISIBLE_C: StackReq(1,1); GetFlagOf(OF_INVISIBLE); break;
    case OP_INVISIBLE_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_INVISIBLE; else o->oflags&=~OF_INVISIBLE; break;
    case OP_INVISIBLE_EC: StackReq(2,0); SetFlagOf(OF_INVISIBLE); break;
    case OP_KEY: StackReq(0,1); Push(NVALUE(current_key)); break;
    case OP_KEYCLEARED: StackReq(0,1); if(o->oflags&OF_KEYCLEARED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_KEYCLEARED_C: StackReq(1,1); GetFlagOf(OF_KEYCLEARED); break;
    case OP_KEYCLEARED_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_KEYCLEARED; else o->oflags&=~OF_KEYCLEARED; break;
    case OP_KEYCLEARED_EC: StackReq(2,0); SetFlagOf(OF_KEYCLEARED); break;
    case OP_LAND: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) && v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t1,t2)?0:1)); break;
    case OP_LE_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t1,t2)?0:1)); break;
    case OP_LNOT: StackReq(1,1); if(v_bool(Pop())) Push(NVALUE(0)); else Push(NVALUE(1)); break;
    case OP_LOC: StackReq(0,2); Push(NVALUE(o->x)); Push(NVALUE(o->y)); break;
    case OP_LOR: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) || v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LOSELEVEL: gameover=-1; Throw(0); break;
    case OP_LT: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t2,t1)?1:0)); break;
    case OP_LT_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t2,t1)?1:0)); break;
    case OP_LXOR: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1)?!v_bool(t2):v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_MOD: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u%t2.u)); break;
    case OP_MOD_C: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s%t2.s)); break;
    case OP_MOVED: StackReq(0,1); if(o->oflags&OF_MOVED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_MOVED_C: StackReq(1,1); GetFlagOf(OF_MOVED); break;
    case OP_MOVED_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_MOVED; else o->oflags&=~OF_MOVED; break;
    case OP_MOVED_EC: StackReq(2,0); SetFlagOf(OF_MOVED); break;
    case OP_MSG: StackReq(0,1); Push(MVALUE(msgvars.msg)); break;
    case OP_MUL: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u*t2.u)); break;
    case OP_MUL_C: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s*t2.s)); break;
    case OP_NE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_equal(t1,t2)?0:1)); break;
    case OP_NEG: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s=-t1.s; Push(t1); break;
    case OP_NIP: StackReq(2,1); t1=Pop(); Pop(); Push(t1); break;
    case OP_OBJABOVE: StackReq(0,1); i=obj_above(obj); Push(OVALUE(i)); break;
    case OP_OBJABOVE_C: StackReq(1,1); i=obj_above(v_object(Pop())); Push(OVALUE(i)); break;
    case OP_OBJBELOW: StackReq(0,1); i=obj_below(obj); Push(OVALUE(i)); break;
    case OP_OBJBELOW_C: StackReq(1,1); i=obj_below(v_object(Pop())); Push(OVALUE(i)); break;
    case OP_OBJCLASSAT: StackReq(3,1); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_obj_class_at(t1,t2,t3)); break;
    case OP_PLAYER: StackReq(0,1); if(classes[o->class]->cflags&CF_PLAYER) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_PLAYER_C: StackReq(1,1); GetClassFlagOf(CF_PLAYER); break;
    case OP_RET: return;
    case OP_SELF: StackReq(0,1); Push(OVALUE(obj)); break;
    case OP_SEND: StackReq(3,1); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_send_self(obj,t2,t3,t4,NVALUE(0))); break;
    case OP_SEND_C: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_send_message(obj,t1,t2,t3,t4,NVALUE(0))); break;
    case OP_SEND_D: StackReq(3,0); t4=Pop(); t3=Pop(); t2=Pop(); v_send_self(obj,t2,t3,t4,NVALUE(0)); break;
    case OP_SEND_CD: StackReq(4,0); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_send_message(obj,t1,t2,t3,t4,NVALUE(0)); break;
    case OP_SENDEX: StackReq(4,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_send_self(obj,t2,t3,t4,t5)); break;
    case OP_SENDEX_C: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_send_message(obj,t1,t2,t3,t4,t5)); break;
    case OP_SENDEX_D: StackReq(4,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); v_send_self(obj,t2,t3,t4,t5); break;
    case OP_SENDEX_CD: StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_send_message(obj,t1,t2,t3,t4,t5); break;
    case OP_SHAPE: StackReq(0,1); Push(NVALUE(o->shape)); break;
    case OP_SHAPE_C: StackReq(1,1); Push(GetVariableOrAttributeOf(shape,NVALUE)); break;
    case OP_SHAPE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->shape=t1.u; break;
    case OP_SHAPE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->shape=t1.u; break;
    case OP_SHAPEDIR: StackReq(1,1); j=v_sh_dir(Pop()); Push(NVALUE((o->shape>>(j+j))&3)); break;
    case OP_SHAPEDIR_C: StackReq(2,1); j=v_sh_dir(Pop()); t1=GetVariableOrAttributeOf(sharp[j],NVALUE); t1.u=(t1.u>>(j+j))&3; Push(t1); break;
    case OP_SHAPEDIR_E: NoIgnore(); StackReq(2,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); change_shape(obj,j,t1.u); break;
    case OP_SHAPEDIR_EC: NoIgnore(); StackReq(3,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) change_shape(i,j,t1.u); break;
    case OP_SHARP: StackReq(1,1); j=v_sh_dir(Pop()); Push(NVALUE(o->sharp[j])); break;
    case OP_SHARP_C: StackReq(2,1); j=v_sh_dir(Pop()); Push(GetVariableOrAttributeOf(sharp[j],NVALUE)); break;
    case OP_SHARP_E: NoIgnore(); StackReq(2,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); o->sharp[j]=t1.u; break;
    case OP_SHARP_EC: NoIgnore(); StackReq(3,0); j=v_sh_dir(Pop()); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->sharp[j]=t1.u; break;
    case OP_SHOVABLE: StackReq(0,1); Push(NVALUE(o->shovable)); break;
    case OP_SHOVABLE_C: StackReq(1,1); Push(GetVariableOrAttributeOf(shovable,NVALUE)); break;
    case OP_SHOVABLE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->shovable=t1.u; break;
    case OP_SHOVABLE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->shovable=t1.u; break;
    case OP_STEALTHY: StackReq(0,1); if(o->oflags&OF_STEALTHY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_STEALTHY_C: StackReq(1,1); GetFlagOf(OF_STEALTHY); break;
    case OP_STEALTHY_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_STEALTHY; else o->oflags&=~OF_STEALTHY; break;
    case OP_STEALTHY_EC: StackReq(2,0); SetFlagOf(OF_STEALTHY); break;
    case OP_STRENGTH: StackReq(0,1); Push(NVALUE(o->strength)); break;
    case OP_STRENGTH_C: StackReq(1,1); Push(GetVariableOrAttributeOf(strength,NVALUE)); break;
    case OP_STRENGTH_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->strength=t1.u; break;
    case OP_STRENGTH_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->strength=t1.u&0xFFFF; break;
    case OP_STRENGTH_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->strength=t1.u; break;
    case OP_STRENGTH_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->strength=t1.u; break;
    case OP_SOUND: StackReq(2,0); t2=Pop(); t1=Pop(); break; // Sound not implemented at this time
    case OP_SUB: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u-t2.u)); break;
    case OP_SWAP: StackReq(2,2); t1=Pop(); t2=Pop(); Push(t1); Push(t2); break;
    case OP_TRACE: StackReq(3,0); trace_stack(obj); break;
    case OP_USERSTATE: StackReq(0,1); if(o->oflags&OF_USERSTATE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_USERSTATE_C: StackReq(1,1); GetFlagOf(OF_USERSTATE); break;
    case OP_USERSTATE_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_USERSTATE; else o->oflags&=~OF_USERSTATE; break;
    case OP_USERSTATE_EC: StackReq(2,0); SetFlagOf(OF_USERSTATE); break;
    case OP_USERSIGNAL: StackReq(0,1); if(o->oflags&OF_USERSIGNAL) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_USERSIGNAL_C: StackReq(1,1); GetFlagOf(OF_USERSIGNAL); break;
    case OP_USERSIGNAL_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_USERSIGNAL; else o->oflags&=~OF_USERSIGNAL; break;
    case OP_USERSIGNAL_EC: StackReq(2,0); SetFlagOf(OF_USERSIGNAL); break;
    case OP_VISUALONLY: StackReq(0,1); if(o->oflags&OF_VISUALONLY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_VISUALONLY_C: StackReq(1,1); GetFlagOf(OF_VISUALONLY); break;
    case OP_VISUALONLY_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_VISUALONLY; else o->oflags&=~OF_VISUALONLY; break;
    case OP_VISUALONLY_EC: StackReq(2,0); SetFlagOf(OF_VISUALONLY); break;
    case OP_VOLUME: StackReq(0,1); Push(NVALUE(o->volume)); break;
    case OP_VOLUME_C: StackReq(1,1); Push(GetVariableOrAttributeOf(volume,NVALUE)); break;
    case OP_VOLUME_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->volume=t1.u; break;
    case OP_VOLUME_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->volume=t1.u&0xFFFF; break;
    case OP_VOLUME_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->volume=t1.u; break;
    case OP_VOLUME_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->volume=t1.u; break;
    case OP_WEIGHT: StackReq(0,1); Push(NVALUE(o->weight)); break;
    case OP_WEIGHT_C: StackReq(1,1); Push(GetVariableOrAttributeOf(weight,NVALUE)); break;
    case OP_WEIGHT_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->weight=t1.u; break;
    case OP_WEIGHT_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->weight=t1.u&0xFFFF; break;
    case OP_WEIGHT_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->weight=t1.u; break;
    case OP_WEIGHT_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->weight=t1.u; break;
    case OP_WINLEVEL: key_ignored=0; gameover=1; Throw(0); break;
    case OP_XLOC: StackReq(0,1); Push(NVALUE(o->x)); break;
    case OP_YLOC: StackReq(0,1); Push(NVALUE(o->y)); break;
#define MiscVar(a,b) \
    case a: StackReq(0,1); Push(o->b); break; \
    case a+0x0800: StackReq(1,1); Push(GetVariableOf(b,)); break; \
    case a+0x1000: NoIgnore(); StackReq(1,0); o->b=Pop(); break; \
    case a+0x1001: NoIgnore(); StackReq(1,0); t1=Pop(); if(!t1.t) t1.u&=0xFFFF; o->b=t1; break; \
    case a+0x1800: NoIgnore(); StackReq(2,0); t1=Pop(); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->b=t1; break; \
    case a+0x1801: NoIgnore(); StackReq(2,0); t1=Pop(); if(!t1.t) t1.u&=0xFFFF; i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->b=t1; break;
    MiscVar(OP_MISC1,misc1) MiscVar(OP_MISC2,misc2) MiscVar(OP_MISC3,misc3)
#undef GetVariableOf
#define GetVariableOf(a,b) GetVariableOrAttributeOf(a,b)
    MiscVar(OP_MISC4,misc4) MiscVar(OP_MISC5,misc5) MiscVar(OP_MISC6,misc6) MiscVar(OP_MISC7,misc7)
#undef MiscVar
#undef GetVariableOf
    default: fprintf(stderr,"Unrecognized opcode 0x%04X at 0x%04X in $%s\n",code[ptr-1],ptr-1,classes[o->class]->name); Throw("Internal error: Unrecognized opcode");
  }
}

static void trace_message(Uint32 obj) {
  Object*o;
  const char*s;
  if(!traceprefix) {
    optionquery[1]=Q_tracePrefix;
    traceprefix=xrm_get_resource(resourcedb,optionquery,optionquery,2);
    if(!traceprefix) traceprefix="";
  }
  if(msgvars.msg<256) s=standard_message_names[msgvars.msg]; else s=messages[msgvars.msg-256];
  o=msgvars.from==VOIDLINK?0:objects[msgvars.from];
  printf("%s%d : %s%s : %d : %u %u",traceprefix,move_number,msgvars.msg<256?"":"#",s,vstackptr,o?o->generation:0,o?msgvars.from:0);
  if(o) printf(" [$%s %d %d]",classes[o->class]->name,o->x,o->y);
  o=objects[obj];
  printf(" : %u %u [$%s %d %d]",o->generation,obj,classes[o->class]->name,o->x,o->y);
  printf(" : %u %u : %u %u : %u %u\n",msgvars.arg1.t,msgvars.arg1.u,msgvars.arg2.t,msgvars.arg2.u,msgvars.arg3.t,msgvars.arg3.u);
}

static Value send_message(Uint32 from,Uint32 to,Uint16 msg,Value arg1,Value arg2,Value arg3) {
  MessageVars saved=msgvars;
  Uint16 c=objects[to]->class;
  Uint16 p=get_message_ptr(c,msg);
  Uint16*code;
  int stackptr=vstackptr;
  if(objects[to]->oflags&OF_DESTROYED) return NVALUE(0);
  if(p==0xFFFF) {
    p=get_message_ptr(0,msg);
    if(p==0xFFFF) return NVALUE(0);
    code=classes[0]->codes;
  } else {
    code=classes[c]->codes;
  }
  msgvars=(MessageVars){msg,from,arg1,arg2,arg3};
  if(main_options['t']) {
    if(from==VOIDLINK || (classes[objects[from]->class]->cflags&CF_TRACEOUT)) {
      if((classes[c]->cflags&CF_TRACEIN) && (message_trace[msg>>3]&(1<<(msg&7)))) trace_message(to);
    }
  }
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

static Uint32 broadcast(Uint32 from,int c,Uint16 msg,Value arg1,Value arg2,Value arg3,int s) {
  Uint32 t=0;
  Uint32 n,p;
  Object*o;
  Value v;
  if(lastobj==VOIDLINK) return;
  n=lastobj;
  while(o=objects[n]) {
    p=o->prev;
    if(!c || o->class==c) {
      v=send_message(from,n,msg,arg1,arg2,arg3);
      if(s>0) {
        switch(v.t) {
          case TY_NUMBER: t+=v.u; break;
          case TY_CLASS: t++; break;
          default:
            if(v.t<=TY_MAXTYPE) Throw("Invalid return type for BroadcastSum");
            t++;
        }
      } else {
        if(s<0) arg2=v;
        t++;
      }
    }
    if(p==VOIDLINK) break;
    n=p;
  }
  return t;
}

void annihilate(void) {
  Uint32 i;
  for(i=0;i<64*64;i++) playfield[i]=VOIDLINK;
  firstobj=lastobj=VOIDLINK;
  if(!objects) return;
  for(i=0;i<nobjects;i++) if(objects[i]) {
    animfree(objects[i]->anim);
    free(objects[i]);
  }
  nobjects=0;
  free(objects);
  objects=0;
  gameover=0;
}

const char*execute_turn(int key) {
  Uint32 n;
  if(setjmp(my_env)) return my_error;
  changed=0;
  key_ignored=0;
  lastimage_processing=0;
  vstackptr=0;
  current_key=key;
  for(n=0;n<nobjects;n++) if(objects[n]) {
    objects[n]->distance=0;
    objects[n]->oflags&=~(OF_KEYCLEARED|OF_DONE);
  }
  
  if(key_ignored && changed) return "Invalid use of IgnoreKey";
  current_key=0;
  
  if(key_ignored && changed) return "Invalid use of IgnoreKey";
  if(generation_number<=TY_MAXTYPE) return "Too many generations of objects";
  return 0;
}

const char*init_level(void) {
  if(setjmp(my_env)) return my_error;
  if(main_options['t']) printf("[Level %d restarted]\n",level_id);
  gameover=0;
  changed=0;
  key_ignored=0;
  lastimage_processing=0;
  vstackptr=0;
  move_number=0;
  current_key=0;
  broadcast(VOIDLINK,0,MSG_INIT,NVALUE(0),NVALUE(0),NVALUE(0),0);
  broadcast(VOIDLINK,0,MSG_POSTINIT,NVALUE(0),NVALUE(0),NVALUE(0),0);
  if(generation_number<=TY_MAXTYPE) return "Too many generations of objects";
  return 0;
}
