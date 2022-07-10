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
Uint32 bizplayfield[64*64]; // bizarro world
Uint8 pfwidth,pfheight;
Sint8 gameover,key_ignored;
Uint8 generation_number_inc;
Uint32 move_number;
unsigned char*quiz_text;
Inventory*inventory;
Uint32 ninventory;
unsigned char**levelstrings;
Uint16 nlevelstrings;
Value*array_data;
Uint16 ndeadanim;
DeadAnimation*deadanim;
Uint8 no_dead_anim;
Uint32 max_trigger;
Uint8 conn_option;
Sint32 gameover_score;

typedef struct {
  Uint16 msg;
  Uint32 from;
  Value arg1,arg2,arg3;
} MessageVars;

typedef struct {
  Uint32 obj;
  Uint16 id;
  Uint8 visual;
} Connection;

static jmp_buf my_env;
static const char*my_error;
static MessageVars msgvars;
static Uint8 lastimage_processing,changed,all_flushed;
static Value vstack[VSTACKSIZE];
static int vstackptr;
static const char*traceprefix;
static Uint8 current_key;
static Value quiz_obj;
static Value traced_obj;
static Uint32 control_obj=VOIDLINK;
static Connection conn[VSTACKSIZE];
static int nconn,pconn;
static Uint8 conn_dir;

#define Throw(x) (my_error=(x),longjmp(my_env,1))
#define StackReq(x,y) do{ if(vstackptr<(x)) Throw("Stack underflow"); if(vstackptr-(x)+(y)>=VSTACKSIZE) Throw("Stack overflow"); }while(0)
#define Push(x) (vstack[vstackptr++]=(x))
#define Pop() (vstack[--vstackptr])

// For arrival/departure masks
#define Xbit(a) ((a)%5-2)
#define Ybit(a) (2-(a)/5)

static void execute_program(Uint16*code,int ptr,Uint32 obj);
static Value send_message(Uint32 from,Uint32 to,Uint16 msg,Value arg1,Value arg2,Value arg3);
static Uint32 broadcast(Uint32 from,int c,Uint16 msg,Value arg1,Value arg2,Value arg3,Uint8 s);
static Value destroy(Uint32 from,Uint32 to,Uint32 why);

static const Sint8 x_delta[8]={1,1,0,-1,-1,-1,0,1};
static const Sint8 y_delta[8]={0,-1,-1,-1,0,1,1,1};

const unsigned char*value_string_ptr(Value v) {
  switch(v.t) {
    case TY_STRING: return stringpool[v.u];
    case TY_LEVELSTRING: return v.u<nlevelstrings?levelstrings[v.u]:(unsigned char*)"<Invalid level string>";
    default: fatal("Internal confusion: Trying to get string pointer for a non-string\n");
  }
}

void pfunlink(Uint32 n) {
  Object*o=objects[n];
  Uint32*pf=(o->oflags&OF_BIZARRO?bizplayfield:playfield);
  if(o->down==VOIDLINK) pf[o->x+o->y*64-65]=o->up;
  else objects[o->down]->up=o->up;
  if(o->up!=VOIDLINK) objects[o->up]->down=o->down;
  o->down=o->up=VOIDLINK;
}

void pflink(Uint32 n) {
  Object*o=objects[n];
  Uint32*pf=(o->oflags&OF_BIZARRO?bizplayfield:playfield);
  int p=o->x+o->y*64-65;
  if(p<0) return;
  if(pf[p]==VOIDLINK) {
    pf[p]=n;
  } else {
    Sint32 d=o->density;
    Uint32 m=pf[p];
    Uint32 t=VOIDLINK;
    while(m!=VOIDLINK && objects[m]->density>=d) t=m,m=objects[m]->up;
    o->down=t;
    o->up=m;
    if(t!=VOIDLINK) objects[t]->up=n; else pf[p]=n;
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

static void set_dead_animation(const Object*o) {
  DeadAnimation*d;
  if(no_dead_anim) return;
  deadanim=realloc(deadanim,(ndeadanim+1)*sizeof(DeadAnimation));
  if(!deadanim) fatal("Allocation failed\n");
  d=deadanim+ndeadanim++;
  d->class=o->class;
  d->x=o->x;
  d->y=o->y;
  d->s=o->anim->step[o->anim->vstep];
  d->vtime=o->anim->vtime;
  d->vimage=o->anim->vimage;
  d->delay=0;
}

static void v_animate_dead(Value x,Value y,Value c,Value s,Value e,Value z) {
  DeadAnimation*d;
  if(no_dead_anim || ndeadanim>=0x1000) return;
  if(x.t || y.t || s.t || e.t || z.t || c.t!=TY_CLASS) return;
  if(!z.u || (z.u&~255) || x.u<1 || x.u>pfwidth || y.u<1 || y.u>pfheight) return;
  if(!classes[c.u] || !classes[c.u]->nimages || (classes[c.u]->cflags&(CF_GROUP|CF_NOCLASS2))) return;
  deadanim=realloc(deadanim,(ndeadanim+1)*sizeof(DeadAnimation));
  if(!deadanim) fatal("Allocation failed\n");
  d=deadanim+ndeadanim++;
  d->class=c.u;
  d->x=x.u;
  d->y=y.u;
  d->s.flag=ANI_ONCE;
  d->s.start=s.u;
  d->s.end=e.u;
  d->s.speed=z.u;
  d->vtime=0;
  d->vimage=s.u;
  d->delay=0;
}

void objtrash(Uint32 n) {
  Object*o=objects[n];
  if(!o) return;
  if(!main_options['e']) {
    if(n==traced_obj.u && o->generation==traced_obj.t && main_options['t']) {
      printf("# Object traced at (%d,%d)\n",o->x,o->y);
      Throw("Object traced");
    } else if(ndeadanim<0x1000 && o->anim && o->anim->status==ANISTAT_VISUAL && !(o->oflags&OF_INVISIBLE)) {
      if(o->up==VOIDLINK || (objects[o->up]->oflags&OF_DESTROYED)) set_dead_animation(o);
    }
  }
  animfree(o->anim);
  if(o->down==VOIDLINK) (o->oflags&OF_BIZARRO?bizplayfield:playfield)[o->x+o->y*64-65]=o->up;
  else objects[o->down]->up=o->up;
  if(o->up!=VOIDLINK) objects[o->up]->down=o->down;
  if(!(o->oflags&OF_DESTROYED)) {
    if(firstobj==n) firstobj=o->next;
    if(lastobj==n) lastobj=o->prev;
    if(o->prev!=VOIDLINK) objects[o->prev]->next=o->next;
    if(o->next!=VOIDLINK) objects[o->next]->prev=o->prev;
  }
  free(o);
  objects[n]=0;
  generation_number_inc=1;
}

static inline void clear_inventory(void) {
  free(inventory);
  inventory=0;
  ninventory=0;
}

static inline Uint8 resolve_dir(Uint32 n,Uint16 d) {
  return d<8?d:(objects[n]->dir+d)&7;
}

/*
  Working of animation: There are two separate animations, being logical
  and visual animations. New animations replace the current logical
  animation; if there isn't one, but there is a visual animation, then a
  new logical animation is added at the end. The logical animation is
  always at the same step or ahead of the visual animation.

  - lstep - Step number of the logical animation; this cannot equal or
  exceed max_animation. Only a single logical step is present at once.

  - vstep - Step number of the visual animation; this cannot equal or
  exceed max_animation. The tail of the visual animation is the same as
  the head of the logical animation, and the buffer is circular.

  - status - Contains bit flags. If the ANISTAT_LOGICAL flag is set, then
  it means a logical animation is active. If the ANISTAT_VISUAL flag is
  set, then it means a visual animation is active. ANISTAT_SYNCHRONIZED
  is set only for synchronized animations, which are visual only.

  - ltime - Amount of logical time passed

  - vtime - Number of centiseconds elapsed.

  - vimage - If the status field has the ANISTAT_VISUAL bit set, then this
  will decide the picture to display rather than using the object's image.

  - count - Number of logical animation steps so far this turn. If it is
  equal to max_animation then no more animations can be added this turn.

  Synchronized animations only use vstep, status, and vimage.
*/

static Animation*animalloc(void) {
  Animation*a=malloc(sizeof(Animation)+max_animation*sizeof(AnimationStep));
  if(!a) fatal("Allocation failed\n");
  a->lstep=a->vstep=a->ltime=a->vtime=a->status=a->count=0;
  return a;
}

static void animate(Uint32 n,Uint32 f,Uint32 a0,Uint32 a1,Uint32 t) {
  Animation*an=objects[n]->anim;
  objects[n]->image=a0;
  f&=0x0B;
  if(!an) an=objects[n]->anim=animalloc();
  if(an->status&ANISTAT_SYNCHRONIZED) an->status=0;
  if(an->count==max_animation) f=ANI_STOP;
  if(!an->count && (an->status&ANISTAT_VISUAL)) an->status=0;
  if(f&(ANI_ONCE|ANI_LOOP)) {
    switch(an->status) {
      case 0:
        an->vtime=an->lstep=an->vstep=0;
        an->vimage=a0;
        break;
      case ANISTAT_LOGICAL:
        an->vstep=an->lstep;
        an->vtime=0;
        an->vimage=a0;
        break;
      case ANISTAT_VISUAL:
        an->lstep++;
        if(an->lstep>=max_animation) an->lstep=0;
        if(an->lstep==an->vstep && max_animation>1) {
          an->vstep++;
          if(an->vstep==max_animation) an->vstep=0;
          an->vimage=an->step[an->vstep].start;
          an->vtime=0;
        }
        break;
      case ANISTAT_VISUAL|ANISTAT_LOGICAL:
        if(an->lstep==an->vstep) {
          an->vimage=a0;
          an->vtime=0;
        }
        break;
    }
    an->step[an->lstep].flag=f;
    an->step[an->lstep].start=a0;
    an->step[an->lstep].end=a1;
    an->step[an->lstep].speed=t;
    an->ltime=0;
    an->status=ANISTAT_VISUAL|ANISTAT_LOGICAL;
    an->count++;
  } else if(an->lstep==an->vstep) {
    if(an->status&ANISTAT_LOGICAL) an->status=0;
  } else if(an->status&ANISTAT_LOGICAL) {
    an->lstep=(an->lstep?:max_animation)-1;
    an->status&=~ANISTAT_LOGICAL;
  }
}

static void animate_ext(Uint32 n,Uint32 f,Uint32 a0,Uint32 a1,Uint32 t) {
  Animation*an=objects[n]->anim;
  if(!an) an=objects[n]->anim=animalloc();
  an->lstep=an->vstep=an->count=an->ltime=an->vtime=an->status=0;
  an->step->start=a0;
  an->step->end=a1;
  an->step->speed=t;
  an->vimage=(f&0x08?objects[n]->image:a0);
  if(f&0x10) an->ltime=an->vtime=t/2;
  switch(f&0x07) {
    case 0: an->status=0; an->step->flag=0; break;
    case 1: an->status=ANISTAT_LOGICAL; an->step->flag=ANI_ONCE; break;
    case 2: an->status=ANISTAT_VISUAL|ANISTAT_LOGICAL; an->step->flag=ANI_ONCE; break;
    case 3: an->status=ANISTAT_VISUAL; an->step->flag=ANI_ONCE; break;
    case 4: an->status=ANISTAT_VISUAL|ANISTAT_LOGICAL; an->step->flag=ANI_LOOP; break;
    case 5: an->status=ANISTAT_VISUAL|ANISTAT_LOGICAL; an->step->flag=ANI_LOOP|ANI_OSC; break;
    case 6: an->status=ANISTAT_VISUAL|ANISTAT_SYNCHRONIZED; an->step->flag=ANI_LOOP|ANI_SYNC; an->step->slot=t&7; break;
    case 7: an->status=ANISTAT_LOGICAL; an->step->flag=ANI_ONCE; objects[n]->image=a0; break;
  }
}

static void animate_sync(Uint32 n,Uint32 sl,Uint32 a0) {
  Animation*an=objects[n]->anim;
  objects[n]->image=a0;
  sl&=7;
  if(!an) an=objects[n]->anim=animalloc();
  an->status=ANISTAT_VISUAL|ANISTAT_SYNCHRONIZED;
  an->vimage=a0+anim_slot[sl].frame;
  an->lstep=an->vstep=0;
  an->step->flag=ANI_LOOP|ANI_SYNC;
  an->step->start=a0;
  an->step->slot=sl;
}

static Sint32 height_at(Uint32 x,Uint32 y) {
  Sint32 r=0;
  Object*o;
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 0;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    o=objects[i];
    if(r<o->height && !(o->oflags&(OF_DESTROYED|OF_VISUALONLY))) r=o->height;
    i=o->up;
  }
  return r;
}

static Sint32 volume_at(Uint32 x,Uint32 y) {
  Sint32 r=0;
  Object*o;
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 0;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    o=objects[i];
    if(r<o->volume && !(o->oflags&(OF_DESTROYED|OF_VISUALONLY))) r=o->volume;
    i=o->up;
  }
  return r;
}

static Uint32 obj_seek(Uint32 aa,Uint32 bb) {
  Object*a;
  Object*b;
  if(aa==VOIDLINK || bb==VOIDLINK) Throw("Cannot seek to zero object");
  a=objects[aa]; b=objects[bb];
  if(a->x==b->x) {
    if(a->y==b->y) return 8;
    return a->y<b->y?6:2;
  } else if(a->x<b->x) {
    if(a->y==b->y) return 0;
    return a->y<b->y?7:1;
  } else {
    if(a->y==b->y) return 4;
    return a->y<b->y?5:3;
  }
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

static Uint32 obj_bottom_at(Uint32 x,Uint32 y) {
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return VOIDLINK;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    if(!(objects[i]->oflags&(OF_DESTROYED|OF_VISUALONLY))) return i;
    i=objects[i]->up;
  }
  return VOIDLINK;
}

static Uint32 obj_class_at(Uint32 c,Uint16 x,Uint16 y) {
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return VOIDLINK;
  i=playfield[x+y*64-65];
  if(c && (classes[c]->cflags&CF_GROUP)) {
    Uint16 k;
    while(i!=VOIDLINK) {
      if(!(objects[i]->oflags&OF_DESTROYED)) {
        k=objects[i]->class;
        while(k!=c && classes[k]->codes && classes[k]->codes[0]==OP_SUPER) k=classes[k]->codes[1];
      }
      i=objects[i]->up;
    }
    return VOIDLINK;
  }
  while(i!=VOIDLINK) {
    if(objects[i]->class==c && !(objects[i]->oflags&OF_DESTROYED)) return i;
    i=objects[i]->up;
  }
  return VOIDLINK;
}

static Uint32 obj_layer_at(Uint8 b,Uint32 x,Uint32 y) {
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return VOIDLINK;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    if(!(objects[i]->oflags&OF_DESTROYED) && (classes[objects[i]->class]->collisionLayers&b)) return i;
    i=objects[i]->up;
  }
  return VOIDLINK;
}

static int connection_collision(Uint8 b,Uint32 x,Uint32 y) {
  Uint32 i;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 1;
  i=playfield[x+y*64-65];
  while(i!=VOIDLINK) {
    if(!(objects[i]->oflags&OF_DESTROYED) && (classes[objects[i]->class]->collisionLayers&b)) return !!((OF_CONNECTION|OF_MOVING)&~objects[i]->oflags);
    i=objects[i]->up;
  }
  return 0;
}

static Uint32 obj_top_at(Uint32 x,Uint32 y) {
  Uint32 i,r;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return VOIDLINK;
  i=playfield[x+y*64-65];
  r=VOIDLINK;
  while(i!=VOIDLINK) {
    if(!(objects[i]->oflags&(OF_DESTROYED|OF_VISUALONLY))) r=i;
    i=objects[i]->up;
  }
  return r;
}

static Uint32 obj_dir(Uint32 n,Uint32 d) {
  if(n==VOIDLINK) return VOIDLINK;
  d=resolve_dir(n,d);
  return obj_top_at(objects[n]->x+x_delta[d],objects[n]->y+y_delta[d]);
}

static Sint16 new_x(Sint16 n,Uint8 d) {
  n+=x_delta[d&7];
  return n<0?0:n>pfwidth?pfwidth+1:n;
}

static Sint16 new_y(Sint16 n,Uint8 d) {
  n+=y_delta[d&7];
  return n<0?0:n>pfheight?pfheight+1:n;
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
  int i=o->x+o->y*64-65;
  Value v;
  p->up=o->up;
  o->down=p->down;
  o->up=y;
  p->down=x;
  if(o->down==VOIDLINK) playfield[i]=x; else objects[o->down]->up=x;
  if(p->up!=VOIDLINK) objects[p->up]->down=y;
  v=send_message(x,y,MSG_FLOATED,NVALUE(0),NVALUE(0),NVALUE(0));
  if(!((o->oflags|p->oflags)&(OF_VISUALONLY|OF_DESTROYED))) send_message(y,x,MSG_SUNK,NVALUE(0),NVALUE(0),v);
}

static void change_density(Uint32 n,Sint32 v) {
  Object*o=objects[n];
  Uint32 i;
  if(o->oflags&OF_BIZARRO) return;
  if(n==control_obj) {
    o->density=v;
    return;
  }
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
    if(v.u>=nobjects || !objects[v.u] || objects[v.u]->generation!=v.t) {
      if(main_options['t']) printf("[Object %lu in generation %lu does not exist]\n",(long)v.u,(long)v.t);
      Throw("Attempt to use a nonexistent object");
    }
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

static Uint32 v_is(Value x,Value y) {
  Uint32 n;
  if(x.t==TY_NUMBER && !x.u) {
    return 0;
  } else if(x.t>TY_MAXTYPE && y.t==TY_CLASS) {
    n=v_object(x);
    if(classes[y.u]->cflags&CF_GROUP) {
      n=objects[n]->class;
      while(n!=y.u && classes[n]->codes && classes[n]->codes[0]==OP_SUPER) n=classes[n]->codes[1];
      return (n==y.u)?1:0;
    }
    return (objects[n]->class==y.u)?1:0;
  } else if((x.t>TY_MAXTYPE || x.t==TY_CLASS) && y.t==TY_NUMBER && !y.u) {
    return 1;
  } else if(x.t==TY_CLASS && y.t==TY_CLASS) {
    if(classes[y.u]->cflags&CF_GROUP) {
      n=x.u;
      while(n!=y.u && classes[n]->codes && classes[n]->codes[0]==OP_SUPER) n=classes[n]->codes[1];
      return (n==y.u)?1:0;
    }
    return (x.u==y.u)?1:0;
    //TODO: CF_GROUP
  } else {
    Throw("Type mismatch");
  }
}

static Uint32 v_destroyed(Value v) {
  if(v.t==TY_NUMBER) {
    if(v.u) Throw("Cannot convert non-zero number to object");
    return 0;
  } else if(v.t>TY_MAXTYPE) {
    if(v.u>=nobjects || !objects[v.u] || objects[v.u]->generation!=v.t) return 1;
    return objects[v.u]->oflags&OF_DESTROYED?1:0;
  } else {
    Throw("Cannot convert non-object to object");
  }
}

static Uint8 collisions_at(Uint32 x,Uint32 y) {
  Uint8 c=0;
  Uint32 n;
  if(x<1 || y<1 || x>pfwidth || y>pfheight) return 0;
  n=playfield[x+y*64-65];
  while(n!=VOIDLINK) {
    if(!(objects[n]->oflags&OF_DESTROYED)) c|=classes[objects[n]->class]->collisionLayers;
    n=objects[n]->up;
  }
  return c;
}

static Uint8 collide_with(Uint8 b,Uint32 n,Uint8 x,Uint8 y,Uint16 c) {
  int i,j;
  Uint8 r=0;
  Uint32 e[8]={VOIDLINK,VOIDLINK,VOIDLINK,VOIDLINK,VOIDLINK,VOIDLINK,VOIDLINK,VOIDLINK};
  Uint8 re[8]={0,0,0,0,0,0,0,0};
  Value v;
  if(StackProtection()) Throw("Call stack overflow");
  for(i=0;i<8;i++) if(b&(1<<i)) e[i]=obj_layer_at(1<<i,x,y);
  for(i=0;i<7;i++) for(j=i+1;j<8;j++) if(e[i]==e[j]) e[j]=VOIDLINK;
  if(n!=VOIDLINK) {
    v=send_message(VOIDLINK,n,MSG_COLLIDE,NVALUE(x),NVALUE(y),NVALUE(b));
    if(v.t) Throw("Type mismatch in COLLIDE");
    r=v.u;
  }
  for(i=0;i<8;i++) if(e[i]!=VOIDLINK && !(r&0x02)) {
    v=send_message(n,e[i],MSG_COLLIDEBY,NVALUE(n==VOIDLINK?0:objects[n]->x),NVALUE(n==VOIDLINK?0:objects[n]->y),CVALUE(c));
    if(v.t) Throw("Type mismatch in COLLIDEBY");
    r|=re[i]=v.u;
  }
  if(!(r&0x01)) {
    // See if we can destroy some objects to make room
    b=classes[c]->collisionLayers;
    if(obj_layer_at(b,x,y)==VOIDLINK) return r;
    n=playfield[x+y*64-65];
    while(n!=VOIDLINK) {
      if(!(objects[n]->oflags&OF_DESTROYED)) {
        if(i=classes[objects[n]->class]->collisionLayers) {
          i=__builtin_ctz(i);
          if(e[i]!=n || !(re[i]&0x10)) return r|0x01;
        }
      }
      n=objects[n]->up;
    }
    for(i=0;i<8;i++) {
      if(e[i]!=VOIDLINK && (re[i]&0x10) && objects[e[i]]->x==x && objects[e[i]]->y==y && !(objects[e[i]]->oflags&OF_DESTROYED)) {
        destroy(n,e[i],3);
      }
    }
    if(obj_layer_at(b,x,y)!=VOIDLINK) r|=0x01;
  }
  return r;
}

static void set_order(Uint32 obj) {
  // To avoid confusing order of execution at the wrong time,
  // calling this function is limited to only certain times.
  Object*o=objects[obj];
  Uint8 ord=classes[o->class]->order;
  Uint8 u;
  Sint32 v0,v1;
  Uint16 p;
  Uint32 n=firstobj;
  Value v;
  for(;;) {
    if(n==obj || n==VOIDLINK) goto notfound;
    u=classes[objects[n]->class]->order;
    if(u<ord || !(objects[n]->oflags&OF_ORDERED)) goto found;
    if(u==ord) {
      p=orders[ord]+1;
      criteria: switch(orders[p]) {
        case OP_RET: goto found;
        case OP_DENSITY: v0=o->density; v1=objects[n]->density; goto compare;
        case OP_DENSITY_C: v1=o->density; v0=objects[n]->density; goto compare;
        case OP_IMAGE: v0=o->image; v1=objects[n]->image; goto compare;
        case OP_IMAGE_C: v1=o->image; v0=objects[n]->image; goto compare;
        case OP_MISC1:
          if(o->misc1.t || objects[n]->misc1.t) Throw("Type mismatch in order criteria");
          v0=o->misc1.s; v1=objects[n]->misc1.s; goto compare;
        case OP_MISC1_C:
          if(o->misc1.t || objects[n]->misc1.t) Throw("Type mismatch in order criteria");
          v1=o->misc1.s; v0=objects[n]->misc1.s; goto compare;
        case OP_MISC2:
          if(o->misc2.t || objects[n]->misc2.t) Throw("Type mismatch in order criteria");
          v0=o->misc2.s; v1=objects[n]->misc2.s; goto compare;
        case OP_MISC2_C:
          if(o->misc2.t || objects[n]->misc2.t) Throw("Type mismatch in order criteria");
          v1=o->misc2.s; v0=objects[n]->misc2.s; goto compare;
        case OP_MISC3:
          if(o->misc3.t || objects[n]->misc3.t) Throw("Type mismatch in order criteria");
          v0=o->misc3.s; v1=objects[n]->misc3.s; goto compare;
        case OP_MISC3_C:
          if(o->misc3.t || objects[n]->misc3.t) Throw("Type mismatch in order criteria");
          v1=o->misc3.s; v0=objects[n]->misc3.s; goto compare;
        case OP_MISC4:
          if(o->misc4.t || objects[n]->misc4.t) Throw("Type mismatch in order criteria");
          v0=o->misc4.s; v1=objects[n]->misc4.s; goto compare;
        case OP_MISC4_C:
          if(o->misc4.t || objects[n]->misc4.t) Throw("Type mismatch in order criteria");
          v1=o->misc4.s; v0=objects[n]->misc4.s; goto compare;
        case OP_MISC5:
          if(o->misc5.t || objects[n]->misc5.t) Throw("Type mismatch in order criteria");
          v0=o->misc5.s; v1=objects[n]->misc5.s; goto compare;
        case OP_MISC5_C:
          if(o->misc5.t || objects[n]->misc5.t) Throw("Type mismatch in order criteria");
          v1=o->misc5.s; v0=objects[n]->misc5.s; goto compare;
        case OP_MISC6:
          if(o->misc6.t || objects[n]->misc6.t) Throw("Type mismatch in order criteria");
          v0=o->misc6.s; v1=objects[n]->misc6.s; goto compare;
        case OP_MISC6_C:
          if(o->misc6.t || objects[n]->misc6.t) Throw("Type mismatch in order criteria");
          v1=o->misc6.s; v0=objects[n]->misc6.s; goto compare;
        case OP_MISC7:
          if(o->misc7.t || objects[n]->misc7.t) Throw("Type mismatch in order criteria");
          v0=o->misc7.s; v1=objects[n]->misc7.s; goto compare;
        case OP_MISC7_C:
          if(o->misc7.t || objects[n]->misc7.t) Throw("Type mismatch in order criteria");
          v1=o->misc7.s; v0=objects[n]->misc7.s; goto compare;
        case OP_TEMPERATURE: v0=o->temperature; v1=objects[n]->temperature; goto compare;
        case OP_TEMPERATURE_C: v1=o->temperature; v0=objects[n]->temperature; goto compare;
        case OP_XLOC: v0=o->x; v1=objects[n]->x; goto compare;
        case OP_XLOC_C: v1=o->x; v0=objects[n]->x; goto compare;
        case OP_YLOC: v0=o->y; v1=objects[n]->y; goto compare;
        case OP_YLOC_C: v1=o->y; v0=objects[n]->y; goto compare;
        case 0xC000 ... 0xFFFF: // message
          changed=0;
          v=send_message(obj,n,(orders[p]&0x3FFF)+256,NVALUE(o->x),NVALUE(o->y),NVALUE(0));
          if(changed) Throw("State-changing is not allowed during ordering");
          changed=1;
          if((o->oflags|objects[n]->oflags)&OF_DESTROYED) Throw("Destruction during ordering");
          if(v.t) Throw("Type mismatch in order criteria");
          v0=0; v1=v.s; goto compare;
        compare:
          if(v0==v1) {
            p++;
            goto criteria;
          }
          if(v0>v1) goto found;
          break;
        default: fatal("Internal confusion: Invalid order criteria (%d)\n",orders[p]);
      }
    }
    n=objects[n]->next;
  }
  found:
  // Now it has been found; insert this object previous to the found object, removing from its existing slot.
  // (Objects are executed in reverse order, so previous in the linked list means executed next)
  if(o->prev==VOIDLINK) firstobj=o->next; else objects[o->prev]->next=o->next;
  if(o->next==VOIDLINK) lastobj=o->prev; else objects[o->next]->prev=o->prev;
  o->next=n;
  o->prev=objects[n]->prev;
  objects[n]->prev=obj;
  if(o->prev==VOIDLINK) firstobj=obj; else objects[o->prev]->next=obj;
  notfound:
  objects[obj]->oflags|=OF_ORDERED;
}

static Uint32 create(Uint32 from,Uint16 c,Uint32 x,Uint32 y,Uint32 im,Uint32 d) {
  Uint32 m,n;
  int i,xx,yy;
  Object*o;
  Object*p;
  Value v;
  if(d>7) d=0;
  if(x<1 || y<1 || x>pfwidth || y>pfheight || c==control_class) return VOIDLINK;
  if(!(classes[c]->oflags&OF_BIZARRO) && (i=classes[c]->collisionLayers) && (xx=collisions_at(x,y)&i)) {
    if(collide_with(xx,VOIDLINK,x,y,c)&0x01) return VOIDLINK;
  }
  n=objalloc(c);
  if(n==VOIDLINK) Throw("Error creating object");
  o=objects[n];
  o->x=x;
  o->y=y;
  o->image=im;
  o->dir=d;
  pflink(n);
  v=send_message(from,n,MSG_CREATE,NVALUE(0),NVALUE(0),NVALUE(0));
  if(o->oflags&OF_DESTROYED) return VOIDLINK;
  if(o->oflags&OF_BIZARRO) {
    if(classes[objects[n]->class]->order) set_order(n);
    return n;
  }
  for(y=0;y<5;y++) for(x=0;x<5;x++) {
    xx=o->x+x-2; yy=o->y+y-2;
    if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
    i=x+5*(4-y);
    m=playfield[xx+yy*64-65];
    while(m!=VOIDLINK) {
      p=objects[m];
      if(p->arrivals&(1<<i)) if(m!=n) send_message(n,m,MSG_CREATED,NVALUE(o->x),NVALUE(o->y),v);
      m=p->up;
    }
  }
  if(o->oflags&OF_DESTROYED) return VOIDLINK;
  if(classes[objects[n]->class]->order) set_order(n);
  m=objects[n]->up;
  if(m!=VOIDLINK) {
    v=send_message(VOIDLINK,n,MSG_SUNK,NVALUE(0),NVALUE(0),v);
    while(m!=VOIDLINK) {
      send_message(n,m,MSG_FLOATED,NVALUE(0),NVALUE(0),v);
      m=objects[m]->up;
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
  if(to==VOIDLINK || to==control_obj) return NVALUE(0);
  o=objects[to];
  // EKS Hero Mesh doesn't check if it already destroyed.
  v=why==8?NVALUE(0):send_message(from,to,MSG_DESTROY,NVALUE(0),NVALUE(0),NVALUE(why));
  if(!v_bool(v)) {
    if(!(o->oflags&OF_DESTROYED)) {
      if(firstobj==to) firstobj=o->next;
      if(lastobj==to) lastobj=o->prev;
      if(o->prev!=VOIDLINK) objects[o->prev]->next=o->next;
      if(o->next!=VOIDLINK) objects[o->next]->prev=o->prev;
      // This object is not itself removed from the linked list, since it may be destroyed while enumerating all objects
    }
    o->oflags|=OF_DESTROYED;
    if(why!=8 && !(o->oflags&(OF_VISUALONLY|OF_BIZARRO))) {
      // Not checking for stealth; stealth only applies to movement, not destruction
      for(yy=0;yy<5;yy++) for(xx=0;xx<5;xx++) {
        x=objects[to]->x+xx-2; y=objects[to]->y+yy-2;
        if(x<1 || x>pfwidth || y<1 || y>pfheight) continue;
        i=xx+5*(4-yy);
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

static int move_to(Uint32 from,Uint32 n,Uint32 x,Uint32 y) {
  Uint32 m;
  int i,xx,yy;
  Object*o;
  Object*p;
  Value v;
  if(n==VOIDLINK || (objects[n]->oflags&OF_DESTROYED) || n==control_obj) return 0;
  o=objects[n];
  if(lastimage_processing) Throw("Can't move during animation processing");
  if(x<1 || y<1 || x>pfwidth || y>pfheight) return 0;
  if(v_bool(send_message(from,n,MSG_MOVING,NVALUE(x),NVALUE(y),NVALUE(0)))) return 0;
  if(classes[o->class]->cflags&CF_PLAYER) {
    m=lastobj;
    while(m!=VOIDLINK) {
      if(v_bool(send_message(n,m,MSG_PLAYERMOVING,NVALUE(x),NVALUE(y),OVALUE(from)))) return 0;
      m=objects[m]->prev;
    }
  }
  if(!(o->oflags&OF_BIZARRO) && (i=classes[o->class]->collisionLayers) && (xx=collisions_at(x,y)&i)) {
    if((i=collide_with(xx,n,x,y,o->class))&0x01) return i&0x04?1:0;
  }
  pfunlink(n);
  if(!(o->oflags&((classes[o->class]->cflags&CF_COMPATIBLE?OF_VISUALONLY:0)|OF_STEALTHY|OF_BIZARRO))) {
    for(i=25;i>=0;i--) {
      xx=o->x+Xbit(i); yy=o->y+Ybit(i);
      if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
      m=playfield[xx+yy*64-65];
      while(m!=VOIDLINK) {
        p=objects[m];
        if(p->departures&(1<<i)) p->departed|=1<<i;
        m=p->up;
      }
    }
  }
  o->distance+=abs(x-o->x)+abs(y-o->y);
  o->x=x;
  o->y=y;
  if(!(o->oflags&((classes[o->class]->cflags&CF_COMPATIBLE?OF_VISUALONLY:0)|OF_STEALTHY|OF_BIZARRO))) {
    for(i=25;i>=0;i--) {
      xx=x+Xbit(i); yy=y+Ybit(i);
      if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
      m=playfield[xx+yy*64-65];
      while(m!=VOIDLINK) {
        p=objects[m];
        if(p->arrivals&(1<<i)) p->arrived|=1<<i;
        m=p->up;
      }
    }
  }
  pflink(n);
  if(!(o->oflags&(OF_VISUALONLY|OF_BIZARRO))) {
    m=objects[n]->up;
    if(m!=VOIDLINK) {
      v=send_message(VOIDLINK,n,MSG_SUNK,NVALUE(0),NVALUE(0),NVALUE(0));
      while(m!=VOIDLINK) {
        send_message(n,m,MSG_FLOATED,NVALUE(0),NVALUE(0),v);
        m=objects[m]->up;
      }
    }
  }
  // The OF_MOVED flag is set elsewhere, not here
  o->oflags&=~OF_DONE;
  return 1;
}

static inline int slide_test(Object*o,Object*oF,Uint32 d1,Uint32 d2,Uint32 vol) {
  // Checks if sliding is possible, except the height of the target location.
  Uint8 m=(o->shape&(1<<d1))|(oF->shape&(1<<(4^d1)));
  if(!(m&(m-1))) return 0;
  if(!(o->shovable&(1<<d2))) return 0;
  if(height_at(o->x+x_delta[d2],o->y+y_delta[d2])<=o->climb) return 1;
  return vol+volume_at(o->x+x_delta[d2],o->y+y_delta[d2])<=max_volume;
}

static int move_dir(Uint32 from,Uint32 obj,Uint32 dir);

static void add_connection(Uint32 obj) {
  Object*o=objects[obj];
  if(nconn==VSTACKSIZE) Throw("Connection limit overflow");
  if((o->oflags^OF_CONNECTION)&(OF_MOVING|OF_DESTROYED|OF_CONNECTION|OF_BIZARRO)) return;
  conn[nconn].obj=obj;
  conn[nconn].id=nconn; // used for stable sorting
  conn[nconn].visual=(o->oflags&OF_VISUALONLY)?1:0;
  o->oflags|=OF_MOVING|OF_VISUALONLY;
  nconn++;
}

static void add_connection_shov(Object*o) {
  int i,x,y;
  Uint8 b=classes[o->class]->collisionLayers;
  Uint32 n;
  for(i=0;i<8;i++) if(o->shovable&(0x100<<i)) {
    x=o->x+x_delta[i]; y=o->y+y_delta[i];
    if(x<1 || x>pfwidth || y<1 || y>pfheight) continue;
    n=playfield[x+y*64-65];
    while(n!=VOIDLINK) {
      if(classes[objects[n]->class]->collisionLayers==b && (objects[n]->shovable&(0x100<<(4^i)))) add_connection(n);
      n=objects[n]->up;
    }
  }
}

static int fake_move_dir(Uint32 obj,Uint32 dir,Uint8 no) {
  // This is similar to move_dir, but specialized for the HIT/HITBY processing in connected_move.
  // Note that this may result in calling the real move_dir due to shoving.
  Object*o;
  Object*oE;
  Object*oF;
  Uint32 objE,objF,objLF,objRF;
  Uint32 hit=0;
  Uint32 vol;
  Value v;
  if(StackProtection()) Throw("Call stack overflow during movement");
  if(obj==VOIDLINK) return 0;
  o=objects[obj];
  restart:
  if(hit&0x100000) dir=o->dir;
  objF=obj_dir(obj,dir);
  if(objF==VOIDLINK) goto fail;
  if(hit) hit=0x800|(hit&0x10000000);
  oF=objects[objF];
  objLF=obj_dir(obj,(dir+1)&7);
  objRF=obj_dir(obj,(dir-1)&7);
  if(height_at(oF->x,oF->y)<=o->climb) hit|=0x200000;
  if(no) {
    if((vol=classes[oF->class]->collisionLayers) && connection_collision(vol,oF->x,oF->y)) goto fail;
    hit|=0x4066;
  }
  if(dir&1) {
    // Diagonal movement
    hit|=0x80000;
    vol=o->volume;
    if(objLF!=VOIDLINK) vol+=volume_at(objects[objLF]->x,objects[objLF]->y);
    if(objRF!=VOIDLINK) vol+=volume_at(objects[objRF]->x,objects[objRF]->y);
    if(vol<=max_volume) {
      objE=objF;
      while(objE!=VOIDLINK) {
        if(o->oflags&OF_DESTROYED) break;
        oE=objects[objE];
        if(oE->height>0) {
          hit&=0xFC287040;
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit|0x81));
          if(v.t==TY_MARK) goto sticky;
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u;
          if(hit&8) goto fail;
          if(!(hit&0x11)) {
            v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(hit|0x81));
            if(v.t==TY_MARK) goto sticky;
            if(v.t) Throw("Type mismatch in HIT/HITBY");
            hit|=v.u;
            if(hit&8) goto fail;
          }
        }
        // Shoving
        if(!(hit&0x44) && (oE->shovable&(1<<dir)) && o->inertia>=oE->weight && !(oE->oflags&OF_VISUALONLY)) {
          oE->inertia=o->inertia;
          if(move_dir(obj,objE,dir)) {
            if(!(oE->oflags&OF_DESTROYED)) o->inertia=oE->inertia;
            hit|=0x8000;
            if(hit&0x800000) goto restart;
          }
        }
        if((oE->oflags&OF_CRUSH) && !(hit&0x22) && oE->x==o->x+x_delta[dir] && oE->y==o->y+y_delta[dir]) {
          if(!v_bool(destroy(obj,objE,4))) hit|=0x8000;
        }
        objE=obj_below(objE);
      }
      if((hit&0x48000)==0x8000) goto restart;
      if((hit&0x200000) && !(hit&0x402008)) {
        if((hit&0x20000) || oF) goto success; else goto fail;
      }
    } else {
      // Volume is too much; hit the objects it won't go between
      if(o->oflags&OF_DESTROYED) goto fail;
      objE=objLF;
      while(objE!=VOIDLINK) {
        oE=objects[objE];
        if(oE->height>0) {
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(0x80089));
          if(v.t==TY_MARK) goto sticky;
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          if(!(v.u&1)) v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(v.u|0x80089));
          if(v.t==TY_MARK) goto sticky;
          if(v.t==TY_SOUND || v.t==TY_USOUND) Throw("Type mismatch in HIT/HITBY");
        }
        objE=obj_below(objE);
      }
      if(o->oflags&OF_DESTROYED) goto fail;
      objE=objRF;
      while(objE!=VOIDLINK) {
        oE=objects[objE];
        if(oE->height>0) {
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(0x80089));
          if(v.t==TY_MARK) goto sticky;
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          if(!(v.u&1)) v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(v.u|0x80089));
          if(v.t==TY_MARK) goto sticky;
          if(v.t==TY_SOUND || v.t==TY_USOUND) Throw("Type mismatch in HIT/HITBY");
        }
        objE=obj_below(objE);
      }
    }
  } else {
    // Orthogonal movement
    if(!oF) goto fail;
    objE=objF;
    while(objE!=VOIDLINK) {
      if(o->oflags&OF_DESTROYED) break;
      oE=objects[objE];
      if(oE->height>0) {
        hit&=~7;
        // HIT/HITBY messages
        v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit|0x81));
        if(v.t==TY_MARK) goto sticky;
        if(v.t) Throw("Type mismatch in HIT/HITBY");
        hit|=v.u;
        if(hit&8) goto fail;
        if(!(hit&0x11)) {
          v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(hit|0x81));
          if(v.t==TY_MARK) goto sticky;
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u;
        }
        if(hit&0x108) goto fail;
        // Hardness/sharpness
        if(!(hit&0x22)) {
          if(o->sharp[dir>>1]>oE->hard[(dir^4)>>1] && !v_bool(destroy(obj,objE,2))) hit|=0x8004;
        }
        if(hit&0x200) goto fail;
      }
      // Shoving
      if(!(hit&0x44) && (oE->shovable&(1<<dir)) && o->inertia>=oE->weight && !(oE->oflags&OF_VISUALONLY)) {
        oE->inertia=o->inertia;
        if(move_dir(obj,objE,dir)) {
          if(!(oE->oflags&OF_DESTROYED)) o->inertia=oE->inertia;
          hit|=0x8000;
          if(hit&0x800000) goto restart;
        }
      }
      if((oE->oflags&OF_CRUSH) && !(hit&0x22) && oE->x==o->x+x_delta[dir] && oE->y==o->y+y_delta[dir]) {
        if(!v_bool(destroy(obj,objE,4))) hit|=0x8000;
      }
      if(hit&0x400) goto fail;
      objE=obj_below(objE);
    }
    if(hit&0x2008) goto fail;
    if((hit&0x48000)==0x8000) goto restart;
    if((hit&0x200000) && !(hit&0x400000)) goto success;
  }
  fail: if(hit&0x1000) goto success; o->inertia=0; return 0;
  success: return 1;
  sticky: add_connection(objE); return 2;
}

static int compare_connection(const void*v1,const void*v2) {
  // Ensure that the sorting is stable.
  const Connection*c1=v1;
  const Connection*c2=v2;
  Object*o1=objects[c1->obj];
  Object*o2=objects[c2->obj];
  switch(conn_dir) {
    case 0: case 1: return (o2->x-o1->x)?:(o1->y-o2->y)?:c2->id-c1->id; // E, NE
    case 2: case 3: return (o1->y-o2->y)?:(o1->x-o2->x)?:c2->id-c1->id; // N, NW
    case 4: case 5: return (o1->x-o2->x)?:(o2->y-o1->y)?:c2->id-c1->id; // W, SW
    case 6: case 7: return (o2->y-o1->y)?:(o2->x-o1->x)?:c2->id-c1->id; // S, SE
  }
}

static int connected_move(Uint32 obj,Uint8 dir) {
  Object*o;
  Uint32 n;
  Sint32 inertia=objects[obj]->inertia;
  Sint32 weight=0;
  char bad;
  int first=nconn;
  int last;
  int i,j;
  if(nconn!=pconn) Throw("Improper nested connected move");
  // Find and add all connections
  add_connection(obj);
  for(i=first;i<nconn;i++) {
    loop1:
    o=objects[n=conn[i].obj];
    o->inertia=inertia;
    if(v_bool(send_message(obj,n,MSG_CONNECT,NVALUE(i-first),NVALUE(dir),NVALUE(weight)))) goto fail;
    weight+=o->weight;
    if(o->shovable&0xFF00) add_connection_shov(o);
  }
  if(conn_option&0x01) {
    for(i=first;i<nconn;i++) {
      o=objects[conn[i].obj];
      if(o->inertia<weight) goto fail;
      inertia-=o->weight;
    }
  } else {
    weight=objects[conn[first].obj]->weight;
  }
  last=pconn=nconn;
  bad=0;
  // Check HIT/HITBY; may shove other objects
  conn_dir=dir;
  qsort(conn+first,last-first,sizeof(Connection),compare_connection);
  if(conn_option&0x02) {
    for(i=first;i<last;i++) {
      o=objects[n=conn[i].obj];
      j=fake_move_dir(n,o->dir=dir,1);
      if(!j) goto fail;
      if(j==2) {
        if(nconn==pconn) Throw("This object cannot be sticky");
        i=pconn;
        goto loop1;
      }
    }
  }
  for(i=first;i<last;i++) {
    o=objects[n=conn[i].obj];
    if(conn_option&0x01) o->inertia=inertia;
    j=fake_move_dir(n,o->dir=dir,0);
    if(!j) bad=1;
    if((conn_option&0x01) && !(o->oflags&OF_DESTROYED)) inertia=o->inertia;
    if(j==2) {
      if(nconn==pconn) Throw("This object cannot be sticky");
      i=pconn;
      goto loop1;
    }
  }
  if(bad) goto fail;
  // Check if each object in the group is movable
  for(i=first;i<last;i++) {
    o=objects[n=conn[i].obj];
    if(v_bool(send_message(VOIDLINK,n,MSG_MOVING,NVALUE(o->x+x_delta[dir]),NVALUE(o->y+y_delta[dir]),NVALUE(0)))) goto fail;
    if(classes[o->class]->cflags&CF_PLAYER) {
      n=lastobj;
      while(n!=VOIDLINK) {
        if(v_bool(send_message(conn[i].obj,n,MSG_PLAYERMOVING,NVALUE(o->x+x_delta[dir]),NVALUE(o->y+y_delta[dir]),NVALUE(0)))) goto fail;
        n=objects[n]->prev;
      }
    }
    if(j=classes[o->class]->collisionLayers) {
      if(connection_collision(j,o->x+x_delta[dir],o->y+y_delta[dir])) goto fail;
    }
  }
  // Move everything in the group
  for(i=first;i<last;i++) {
    o=objects[n=conn[i].obj];
    if(!conn[i].visual) o->oflags&=~OF_VISUALONLY;
    if(move_to(obj,n,o->x+x_delta[dir],o->y+y_delta[dir])) o->oflags|=OF_MOVED;
  }
  // Done
  j=1; goto done;
  fail: j=0;
  done:
  for(i=first;i<nconn;i++) {
    o=objects[conn[i].obj];
    o->oflags&=~OF_MOVING;
    if(conn[i].visual) o->oflags|=OF_VISUALONLY; else o->oflags&=~OF_VISUALONLY;
  }
  pconn=nconn=first;
  return j;
}

static int move_dir(Uint32 from,Uint32 obj,Uint32 dir) {
  // This function is complicated, and there may be mistakes.
  Object*o;
  Object*oE;
  Object*oF;
  Object*oW;
  Uint32 objE,objF,objLF,objRF,objW;
  Uint32 hit=0;
  Uint32 vol;
  Value v;
  if(StackProtection()) Throw("Call stack overflow during movement");
  if(obj==VOIDLINK) return 0;
  oW=o=objects[objW=obj];
  o->dir=dir=resolve_dir(obj,dir);
  if(o->weight>o->inertia) goto fail;
  if(o->oflags&OF_CONNECTION) return connected_move(obj,dir);
  o->inertia-=o->weight;
  restart:
  if(hit&0x100000) dir=o->dir;
  objF=obj_dir(objW,dir);
  if(objF==VOIDLINK) goto fail;
  if(hit) hit=0x800|(hit&0x10000000);
  oF=objects[objF];
  objLF=obj_dir(obj,(dir+1)&7);
  objRF=obj_dir(obj,(dir-1)&7);
  if(height_at(oF->x,oF->y)<=o->climb) hit|=0x200000;
  if(dir&1) {
    // Diagonal movement
    hit|=0x80000;
    vol=o->volume;
    if(objLF!=VOIDLINK) vol+=volume_at(objects[objLF]->x,objects[objLF]->y);
    if(objRF!=VOIDLINK) vol+=volume_at(objects[objRF]->x,objects[objRF]->y);
    if(vol<=max_volume) {
      objE=objF;
      while(objE!=VOIDLINK) {
        if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) break;
        oE=objects[objE];
        if(oE->height>0) {
          hit&=0xFC287040;
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit));
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u&(classes[o->class]->cflags&CF_COMPATIBLE?0xC0090F7F:-1L);
          if(hit&8) goto fail;
          if(!(hit&0x11)) {
            v=send_message(obj,objE,MSG_HITBY,NVALUE(oW->x),NVALUE(oW->y),NVALUE(hit));
            if(v.t>TY_MAXTYPE) goto warp;
            if(v.t) Throw("Type mismatch in HIT/HITBY");
            hit|=v.u&(classes[oE->class]->cflags&CF_COMPATIBLE?0xC0090F7F:-1L);
            if(hit&8) goto fail;
          }
        }
        // Shoving
        if(!(classes[o->class]->cflags&CF_COMPATIBLE)) {
          if(!(hit&0x44) && (oE->shovable&(1<<dir)) && o->inertia>=oE->weight && !(oE->oflags&OF_VISUALONLY)) {
            oE->inertia=o->inertia;
            if(move_dir(obj,objE,dir)) {
              if(!(oE->oflags&OF_DESTROYED)) o->inertia=oE->inertia;
              hit|=0x8000;
              if(hit&0x800000) goto restart;
            }
          }
          if((oE->oflags&OF_CRUSH) && !(hit&0x22) && obj==objW && oE->x==o->x+x_delta[dir] && oE->y==o->y+y_delta[dir]) {
            if(!v_bool(destroy(obj,objE,4))) hit|=0x8000;
          }
        }
        objE=obj_below(objE);
      }
      if((hit&0x48000)==0x8000) goto restart;
      if((hit&0x200000) && !(hit&0x402008)) {
        if(hit&0x20000) goto success;
        if(!oF) goto fail;
        if(move_to(from,obj,oF->x,oF->y)) goto success; else goto fail;
      }
    } else {
      // Volume is too much; hit the objects it won't go between
      if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) goto fail;
      objE=objLF;
      while(objE!=VOIDLINK) {
        oE=objects[objE];
        if(oE->height>0) {
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(0x80008));
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          if(!(v.u&1)) v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(v.u|0x80008));
        }
        objE=obj_below(objE);
      }
      if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) goto fail;
      objE=objRF;
      while(objE!=VOIDLINK) {
        oE=objects[objE];
        if(oE->height>0) {
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(0x80008));
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          if(!(v.u&1)) v=send_message(obj,objE,MSG_HITBY,NVALUE(o->x),NVALUE(o->y),NVALUE(v.u|0x80008));
        }
        objE=obj_below(objE);
      }
    }
  } else {
    // Orthogonal movement
    if(!oF) goto fail;
    if(o->shape) {
      // Check if attributes allow sliding
      vol=o->volume+volume_at(oF->x,oF->y);
      if(objLF!=VOIDLINK && slide_test(o,oF,dir,(dir+2)&7,vol)) {
        hit|=0x4000000;
        oE=objects[objLF];
        if(height_at(oE->x,oE->y)<=o->climb) hit|=0x1000000;
      }
      if(objRF!=VOIDLINK && slide_test(o,oF,dir|1,(dir-2)&7,vol)) {
        hit|=0x8000000;
        oE=objects[objRF];
        if(height_at(oE->x,oE->y)<=o->climb) hit|=0x2000000;
      }
    }
    objE=objF;
    while(objE!=VOIDLINK) {
      if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) break;
      oE=objects[objE];
      if(oE->height>0) {
        hit&=~7;
        // HIT/HITBY messages
        v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit));
        if(v.t) Throw("Type mismatch in HIT/HITBY");
        hit|=v.u&(classes[o->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1L);
        if(hit&8) goto fail;
        if(!(hit&0x11)) {
          v=send_message(obj,objE,MSG_HITBY,NVALUE(oW->x),NVALUE(oW->y),NVALUE(hit));
          if(v.t>TY_MAXTYPE) {
            warp:
            oW=objects[objW=v_object(v)];
            dir=oW->dir;
            hit=0x10000000;
            goto restart;
          }
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u&(classes[oE->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1L);
        }
        if(hit&0x108) goto fail;
        // Hardness/sharpness
        if(!(hit&0x22)) {
          if(o->sharp[dir>>1]>oE->hard[(dir^4)>>1] && !v_bool(destroy(obj,objE,2))) hit|=0x8004;
          if(oE->sharp[(dir^4)>>1]>o->hard[dir>>1] && !v_bool(destroy(objE,obj,1))) hit|=0x4C;
        }
        if(hit&0x200) goto fail;
      }
      // Shoving
      if(!(hit&0x44) && (oE->shovable&(1<<dir)) && o->inertia>=oE->weight && !(oE->oflags&OF_VISUALONLY)) {
        oE->inertia=o->inertia;
        if(move_dir(obj,objE,dir)) {
          if(!(oE->oflags&OF_DESTROYED)) o->inertia=oE->inertia;
          hit|=0x8000;
          if(hit&0x800000) goto restart;
        }
      }
      if((oE->oflags&OF_CRUSH) && !(hit&0x22) && obj==objW && oE->x==o->x+x_delta[dir] && oE->y==o->y+y_delta[dir]) {
        if(!v_bool(destroy(obj,objE,4))) hit|=0x8000;
      }
      if(hit&0x400) goto fail;
      objE=obj_below(objE);
    }
    if(hit&0x2008) goto fail;
    if((hit&0x48000)==0x8000) goto restart;
    if((hit&0x200000) && !(hit&0x400000)) {
      if(hit&0x20000) goto success;
      if(move_to(from,obj,oF->x,oF->y)) goto success; else goto fail;
    }
    // Sliding
    if(hit&0x80) goto fail;
    hit|=0x10000;
    if(hit&0x8000000) {
      hit&=0xF010000;
      objE=objRF;
      while(objE!=VOIDLINK) {
        if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) break;
        oE=objects[objE];
        if(oE->height>0) {
          hit&=~7;
          // HIT/HITBY messages
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit));
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u&(classes[o->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1L);
          if(hit&8) goto otherside;
          if(!(hit&0x11)) {
            v=send_message(obj,objE,MSG_HITBY,NVALUE(oW->x),NVALUE(oW->y),NVALUE(hit));
            if(v.t) Throw("Type mismatch in HIT/HITBY");
            hit|=v.u&(classes[oE->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1L);
          }
          if(hit&0x108) goto otherside;
          // Hardness/sharpness
          if(!(hit&0x22)) {
            if(o->sharp[dir>>1]>oE->hard[(dir^4)>>1] && !v_bool(destroy(obj,objE,2))) hit|=0x8004;
            if(oE->sharp[(dir^4)>>1]>o->hard[dir>>1] && !v_bool(destroy(objE,obj,1))) hit|=0x4C;
          }
        }
        if(hit&0x600) goto otherside;
        objE=obj_below(objE);
      }
      if((hit&0x48000)==0x8000) goto restart;
      if((hit&0x2000000) && !(hit&0x400080)) {
        if(hit&0x20000) goto success;
        if(move_to(from,obj,objects[objRF]->x,objects[objRF]->y)) goto success;
      }
    }
    otherside:
    if(hit&0x4000000) {
      hit&=0xF010000;
      objE=objLF;
      while(objE!=VOIDLINK) {
        if(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) break;
        oE=objects[objE];
        if(oE->height>0) {
          hit&=~7;
          // HIT/HITBY messages
          v=send_message(objE,obj,MSG_HIT,NVALUE(oE->x),NVALUE(oE->y),NVALUE(hit));
          if(v.t) Throw("Type mismatch in HIT/HITBY");
          hit|=v.u&(classes[o->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1);
          if(hit&8) goto fail;
          if(!(hit&0x11)) {
            v=send_message(obj,objE,MSG_HITBY,NVALUE(oW->x),NVALUE(oW->y),NVALUE(hit));
            if(v.t) Throw("Type mismatch in HIT/HITBY");
            hit|=v.u&(classes[oE->class]->cflags&CF_COMPATIBLE?0xC0098F7F:-1);
          }
          if(hit&0x108) goto fail;
          // Hardness/sharpness
          if(!(hit&0x22)) {
            if(o->sharp[dir>>1]>oE->hard[(dir^4)>>1] && !v_bool(destroy(obj,objE,2))) hit|=0x8004;
            if(oE->sharp[(dir^4)>>1]>o->hard[dir>>1] && !v_bool(destroy(objE,obj,1))) hit|=0x4C;
          }
        }
        if(hit&0x600) goto fail;
        objE=obj_below(objE);
      }
      if((hit&0x48000)==0x8000) goto restart;
      if((hit&0x1000000) && !(hit&0x400080)) {
        if(hit&0x20000) goto success;
        if(move_to(from,obj,objects[objLF]->x,objects[objLF]->y)) goto success;
      }
    }
  }
  fail:
  if(hit&0x1000) goto success;
  if(hit&0x10000000) {
    v=send_message(obj,objW,MSG_NEXTWARP,NVALUE(dir),NVALUE(0),NVALUE(hit));
    if(v.t || v.u) goto warp;
  }
  o->inertia=0; return 0;
  success: if(!(hit&0x4000)) o->oflags|=OF_MOVED; if(hit&0x10000000) o->dir=dir; return 1;
}

static int jump_to(Uint32 from,Uint32 n,Uint32 x,Uint32 y) {
  int xx,yy;
  if(n==VOIDLINK) return 0;
  xx=objects[n]->x;
  yy=objects[n]->y;
  if(!move_to(from,n,x,y)) return 0;
  send_message(VOIDLINK,n,MSG_JUMPED,NVALUE(xx),NVALUE(yy),OVALUE(from));
  return 1;
}

static int defer_move(Uint32 obj,Uint32 dir,Uint8 plus) {
  Object*o;
  Object*q;
  Uint32 n;
  Uint8 x,y;
  Value v;
  if(StackProtection()) Throw("Call stack overflow during movement");
  if(obj==VOIDLINK || obj==control_obj) return 0;
  o=objects[obj];
  if(o->oflags&(OF_DESTROYED|OF_BIZARRO|OF_CONNECTION)) return 0;
  dir=resolve_dir(obj,dir);
  x=o->x+x_delta[dir]; y=o->y+y_delta[dir];
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 0;
  if(plus) {
    if(o->oflags&OF_MOVING) {
      if(o->dir==dir) return 1;
      v=send_message(VOIDLINK,obj,MSG_CONFLICT,NVALUE(dir),NVALUE(0),NVALUE(0));
      if(!v_bool(v)) return 0;
    }
    o->oflags|=OF_MOVING;
    o->dir=dir;
  } else {
    if(!(o->oflags&OF_MOVING)) return 0;
    if(o->dir!=dir) return 0;
    o->oflags&=~OF_MOVING;
  }
  n=playfield[x+y*64-65];
  while(n!=VOIDLINK) {
    q=objects[n];
    if(!(q->oflags&(OF_DESTROYED|OF_VISUALONLY|OF_MOVING)) && ((q->height>0 && q->height>=o->climb) || (classes[q->class]->collisionLayers&classes[o->class]->collisionLayers))) {
      v=send_message(obj,n,MSG_BLOCKED,NVALUE(o->x),NVALUE(o->y),NVALUE(plus));
      if(v.t) Throw("Type mismatch");
      if(v.u&1) o->oflags&=~OF_MOVING;
      if(v.u&2) break;
    }
    n=q->up;
  }
  return plus?(o->oflags&OF_MOVING?1:0):1;
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

static void trace_stack_list(int n) {
  int b;
  if(!main_options['t']) return;
  if(!traceprefix) {
    optionquery[1]=Q_tracePrefix;
    traceprefix=xrm_get_resource(resourcedb,optionquery,optionquery,2);
    if(!traceprefix) traceprefix="";
  }
  b=vstackptr-n;
  if(b<0) b=0;
  n=vstackptr;
  while(n-->b) printf("%s<%d> : %u %u\n",traceprefix,n,vstack[n].t,vstack[n].u);
}

static void flush_object(Uint32 n) {
  Object*o=objects[n];
  o->arrived=o->departed=0;
  o->oflags&=~(OF_MOVED|OF_BUSY|OF_USERSIGNAL|OF_MOVING);
  o->inertia=0;
}

static void flush_class(Uint16 c) {
  Uint32 n,p;
  Object*o;
  if(lastobj==VOIDLINK) return;
  n=lastobj;
  while(o=objects[n]) {
    p=o->prev;
    if(!c || o->class==c) {
      o->arrived=o->departed=0;
      o->oflags&=~(OF_MOVED|OF_BUSY|OF_USERSIGNAL|OF_MOVING);
      o->inertia=0;
    }
    if(p==VOIDLINK) break;
    n=p;
  }
}

static inline void flush_all(void) {
  if(current_key) all_flushed=255;
  flush_class(0);
}

static void set_bizarro(Uint32 n,Uint32 v) {
  Object*o;
  Uint8 b;
  if(n==VOIDLINK || n==control_obj) return;
  o=objects[n];
  if(o->oflags&OF_DESTROYED) return;
  if(v) {
    if(o->oflags&OF_BIZARRO) return;
    pfunlink(n);
    o->oflags|=OF_BIZARRO;
    pflink(n);
  } else {
    if(!(o->oflags&OF_BIZARRO)) return;
    if(b=classes[o->class]->collisionLayers) {
      Uint32 m=obj_layer_at(b,o->x,o->y);
      if(m!=VOIDLINK && (collide_with(b,n,o->x,o->y,o->class)&0x01)) return;
    }
    pfunlink(n);
    o->oflags&=~OF_BIZARRO;
    pflink(n);
  }
}

void swap_world(void) {
  Uint32 n;
  int i,j,b;
  if(!main_options['e']) for(i=0;i<64*pfheight;i++) {
    n=playfield[i];
    for(b=0;n!=VOIDLINK;) {
      j=classes[objects[n]->class]->collisionLayers;
      if(b&j) Throw("SwapWorld attempted while collisions are present");
      b|=j;
      n=objects[n]->up;
    }
  }
  for(i=0;i<64*pfheight;i++) {
    n=playfield[i];
    playfield[i]=bizplayfield[i];
    bizplayfield[i]=n;
  }
  for(n=0;n<nobjects;n++) if(objects[n]) objects[n]->oflags^=OF_BIZARRO;
}

static Uint32 v_bizarro_swap(Value x,Value y) {
  Uint32 k,m,n;
  Uint8 b,c;
  if(x.t==TY_NUMBER && y.t==TY_NUMBER) {
    if(x.u<1 || x.u>pfwidth || y.u<1 || y.u>pfheight) return 0x100;
    b=c=0;
    n=bizplayfield[x.u+y.u*64-65];
    while(n!=VOIDLINK) {
      c|=b&classes[objects[n]->class]->collisionLayers;
      b|=classes[objects[n]->class]->collisionLayers;
      n=objects[n]->up;
    }
    if(c) return c;
    n=playfield[x.u+y.u*64-65];
    while(n!=VOIDLINK) objects[n]->oflags|=OF_BIZARRO,n=objects[n]->up;
    m=playfield[x.u+y.u*64-65]=bizplayfield[x.u+y.u*64-65];
    bizplayfield[x.u+y.u*64-65]=n;
    while(m!=VOIDLINK) objects[n]->oflags&=~OF_BIZARRO,m=objects[m]->up;
    return 0;
  } else {
    m=v_object(x);
    n=v_object(y);
    if(m==VOIDLINK || n==VOIDLINK || m==n) return 0x100;
    if(objects[m]->x!=objects[n]->x || objects[m]->y!=objects[n]->y) return 0x100;
    if((objects[m]->oflags|objects[n]->oflags)&OF_DESTROYED) return 0x100;
    if(!((objects[m]->oflags^objects[n]->oflags)&OF_BIZARRO)) return 0x100;
    if(objects[n]->oflags&OF_BIZARRO) k=m,m=n,n=k;
    if(b=classes[objects[m]->class]->collisionLayers) {
      k=obj_layer_at(b,objects[m]->x,objects[m]->y);
      if(k!=VOIDLINK && k!=n) {
        c=collisions_at(objects[m]->x,objects[m]->y)&b;
        if(collide_with(c,m,objects[m]->x,objects[m]->y,objects[m]->class)&0x01) return c;
      }
    }
    pfunlink(m);
    pfunlink(n);
    objects[m]->oflags^=OF_BIZARRO;
    objects[n]->oflags^=OF_BIZARRO;
    pflink(m);
    pflink(n);
    return 0;
  }
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

static int v_for(Uint16*code,int ptr,Value v,Value xv,Value yv) {
  int k=code[ptr];
  Uint32 n;
  if(xv.t || yv.t) Throw("Type mismatch");
  globals[k].t=TY_FOR;
  if(xv.u<1 || xv.u>pfwidth || yv.u<1 || yv.u>pfheight) {
    globals[k].s=-1;
    xv.u=yv.u=0;
  } else {
    globals[k].u=xv.u+yv.u*64-65;
    if(v_bool(v)) globals[k].u|=0x10000;
  }
  if(xv.u || yv.u) {
    n=playfield[globals[k].u&0xFFFF];
    while(n!=VOIDLINK) {
      objects[n]->oflags&=~OF_DONE;
      n=objects[n]->up;
    }
  }
  return code[ptr+1]-2; // This will cause "next" to be executed next
}

static int v_next(Uint16*code,int ptr) {
  int k=code[code[ptr]-1];
  Uint32 n;
  Uint32 r=VOIDLINK;
  if(globals[k].t!=TY_FOR) Throw("Uninitialized for/next loop");
  if(globals[k].s==-1) return ptr+1;
  n=playfield[globals[k].u&0xFFFF];
  while(n!=VOIDLINK) {
    if(!(objects[n]->oflags&(OF_DONE|OF_DESTROYED|OF_VISUALONLY))) {
      r=n;
      if(globals[k].u&0x10000) break;
    }
    n=objects[n]->up;
  }
  if(r==VOIDLINK) return ptr+1;
  objects[r]->oflags|=OF_DONE;
  Push(OVALUE(r));
  return code[ptr]+1;
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

static void v_delete_inventory(Value cl,Value im) {
  int i;
  if(!cl.t && !cl.u) return;
  if(cl.t!=TY_CLASS || im.t!=TY_NUMBER) Throw("Type mismatch");
  for(i=0;i<ninventory;i++) if(inventory[i].class==cl.u && inventory[i].image==im.u) {
    --ninventory;
    if(i!=ninventory) inventory[i]=inventory[ninventory];
    return;
  }
}

static Value v_get_inventory(Value cl,Value im) {
  int i;
  if(cl.t!=TY_CLASS || im.t!=TY_NUMBER) Throw("Type mismatch");
  for(i=0;i<ninventory;i++) if(inventory[i].class==cl.u && inventory[i].image==im.u) {
    Push(NVALUE(inventory[i].value));
    Push(NVALUE(1));
    return;
  }
  Push(NVALUE(0));
}

static void v_set_inventory(Value cl,Value im,Value va) {
  int i;
  if(!cl.t && !cl.u) return;
  if(cl.t!=TY_CLASS || im.t!=TY_NUMBER || va.t!=TY_NUMBER) Throw("Type mismatch");
  im.u&=0xFF;
  va.u&=0xFFFF;
  if(ninventory>=max_objects) Throw("Inventory exceeds max_objects");
  for(i=0;i<ninventory;i++) if(inventory[i].class==cl.u && inventory[i].image==im.u) {
    inventory[i].value=va.u;
    return;
  }
  inventory=realloc(inventory,++ninventory*sizeof(Inventory));
  if(!inventory) fatal("Allocation failed\n");
  inventory[i].class=cl.u;
  inventory[i].image=im.u;
  inventory[i].value=va.u;
}

static void v_init_array(Value ar,Value v) {
  Uint32 s,n;
  if(ar.t!=TY_ARRAY) Throw("Type mismatch");
  s=ar.u&0xFFFF;
  n=((ar.u>>16)&0x3FF)+1;
  n*=((ar.u>>26)&0x3F)+1;
  while(n--) array_data[s++]=v;
}

static Value v_get_array(Value ar,Uint32 x,Uint32 y) {
  Uint32 s;
  if(ar.t!=TY_ARRAY) Throw("Type mismatch");
  s=ar.u&0xFFFF;
  if(y>((ar.u>>16)&0x3FF) || x>((ar.u>>26)&0x3F)) Throw("Array index out of bounds");
  s+=x+y+((ar.u>>26)&0x3F)*y;
  return array_data[s];
}

static void v_set_array(Value ar,Uint32 x,Uint32 y,Value v) {
  Uint32 s;
  if(ar.t!=TY_ARRAY) Throw("Type mismatch");
  s=ar.u&0xFFFF;
  if(y>((ar.u>>16)&0x3FF) || x>((ar.u>>26)&0x3F)) Throw("Array index out of bounds");
  s+=x+y+((ar.u>>26)&0x3F)*y;
  array_data[s]=v;
}

static Value v_array_cell(Value ar,Uint32 x,Uint32 y) {
  Uint32 s;
  if(ar.t!=TY_ARRAY) Throw("Type mismatch");
  s=ar.u&0xFFFF;
  if(y>((ar.u>>16)&0x3FF) || x>((ar.u>>26)&0x3F)) Throw("Array index out of bounds");
  s+=x+y+((ar.u>>26)&0x3F)*y;
  return UVALUE(s,TY_ARRAY);
}

static Value v_array_slice(Value ar,Uint32 x,Uint32 y) {
  // Is this correct? I am not sure.
  if(ar.t!=TY_ARRAY) Throw("Type mismatch");
  if((x|y)&~0xFFFF) Throw("Slice out of range");
  if(x+y>((ar.u>>16)&0x3FF)-1) Throw("Slice out of range");
  ar.u+=((ar.u>>26)&0x3F)*x-((x+y)<<16);
  return ar;
}

static void v_copy_array(Value a1,Value a2) {
  Uint32 n;
  if(!a2.t && !a2.u) return;
  if(a1.t!=TY_ARRAY || a2.t!=TY_ARRAY) Throw("Type mismatch");
  if(a1.u==a2.u) return;
  if((a1.u^a2.u)>>16) Throw("Dimension mismatch");
  n=((a1.u>>16)&0x3FF)+1;
  n*=((a1.u>>26)&0x3F)+1;
  memmove(array_data+(a2.u&0xFFFF),array_data+(a1.u&0xFFFF),n*sizeof(Value));
}

static Value v_dot_product(Value a1,Value a2) {
  Uint32 r=0;
  Uint32 s1,s2,n;
  Value v1,v2;
  if(a1.t!=TY_ARRAY || a2.t!=TY_ARRAY) Throw("Type mismatch");
  if((a1.u^a2.u)>>16) Throw("Dimension mismatch");
  s1=a1.u&0xFFFF;
  s2=a2.u&0xFFFF;
  n=((a1.u>>16)&0x3FF)+1;
  n*=((a1.u>>26)&0x3F)+1;
  while(n--) {
    v1=array_data[s1++];
    v2=array_data[s2++];
    if((v1.t==TY_NUMBER && !v1.u) || (v2.t==TY_NUMBER && !v2.u)) continue;
    if(v1.t==TY_NUMBER && v2.t==TY_NUMBER) {
      r+=v1.u*v2.u;
    } else if(v1.t==TY_NUMBER && (v2.t==TY_CLASS || v2.t==TY_MESSAGE || v2.t>TY_MAXTYPE)) {
      return v2;
    } else if(v2.t==TY_NUMBER && (v1.t==TY_CLASS || v1.t==TY_MESSAGE || v1.t>TY_MAXTYPE)) {
      return v1;
    } else if(v1.t==TY_MESSAGE && v2.t>TY_MAXTYPE) {
      v1=send_message(VOIDLINK,v_object(v2),v1.u,NVALUE(0),NVALUE(0),NVALUE(0));
      mess:
      if(v1.t==TY_SOUND || v1.t==TY_USOUND) Throw("Type mismatch");
      if(v1.t!=TY_NUMBER) return v1;
      r+=v1.u;
    } else if(v2.t==TY_MESSAGE && v1.t>TY_MAXTYPE) {
      v1=send_message(VOIDLINK,v_object(v1),v2.u,NVALUE(0),NVALUE(0),NVALUE(0));
      goto mess;
    } else {
      Throw("Type mismatch");
    }
  }
  return NVALUE(r);
}

static void v_set_popup(Uint32 from,int argc) {
  const unsigned char*t;
  const unsigned char*u;
  sqlite3_str*s;
  Value v;
  int argi=1;
  if(argc>32 || argc<0) Throw("Too many arguments");
  vstackptr-=argc;
  v=Pop();
  if(quiz_text) return;
  if(argc) {
    argc++;
    if(v.t!=TY_STRING && v.t!=TY_LEVELSTRING) Throw("Type mismatch");
    s=sqlite3_str_new(userdb);
    t=value_string_ptr(v);
    while(*t) {
      if(u=strchr(t,'%')) {
        sqlite3_str_append(s,t,u-t);
        t=u+2;
        switch(u[1]) {
          case 0:
            t=u+1;
            break;
          case 'c':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) {
              sqlite3_str_appendchar(s,1,31);
              sqlite3_str_appendchar(s,1,v.u&255?:255);
            }
            break;
          case 'C':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) sqlite3_str_appendchar(s,1,(v.u&7)+1);
            break;
          case 'd':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) sqlite3_str_appendf(s,"%d",(signed int)v.s);
            break;
          case 'i':
            if(argi>=argc-1) break;
            if(vstack[vstackptr+argi].t==TY_CLASS && vstack[vstackptr+argi+1].t==TY_NUMBER) {
              Class*c=classes[vstack[vstackptr+argi].u];
              int n=vstack[vstackptr+argi+1].u&255;
              if(n<c->nimages) sqlite3_str_appendf(s,"\x0E\x07:%d\\",c->images[n]&0x7FFF);
            }
            argi+=2;
            break;
          case 'R':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) {
              static const char*const r1000[10]={"","M","MM","MMM","MMMM","MMMMM","MMMMMM","MMMMMMM","MMMMMMMM","MMMMMMMMM"};
              static const char*const r100[10]={"","C","CC","CCC","CD","D","DC","DCC","DCCC","CM"};
              static const char*const r10[10]={"","X","XX","XXX","XL","L","LX","LXX","LXXX","XC"};
              static const char*const r1[10]={"","I","II","III","IV","V","VI","VII","VIII","IX"};
              if(v.u) sqlite3_str_appendf(s,"%s%s%s%s",r1000[(v.u/1000)%10],r100[(v.u/100)%10],r10[(v.u/10)%10],r1[v.u%10]);
              else sqlite3_str_appendchar(s,1,'N');
            }
            break;
          case 's':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            switch(v.t) {
              case TY_STRING: case TY_LEVELSTRING:
                sqlite3_str_appendf(s,"%s",value_string_ptr(v));
                break;
              case TY_NUMBER:
                sqlite3_str_appendf(s,"%llu",(sqlite3_int64)v.u);
                break;
              case TY_CLASS:
                sqlite3_str_appendf(s,"%s",classes[v.u]->name);
                break;
              case TY_MESSAGE:
                sqlite3_str_appendf(s,"%s",v.u<256?standard_message_names[v.u]:messages[v.u-256]);
                break;
            }
            break;
          case 'u':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) sqlite3_str_appendf(s,"%u",(unsigned int)v.u);
            break;
          case 'x':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) sqlite3_str_appendf(s,"%x",(unsigned int)v.u);
            break;
          case 'X':
            if(argi==argc) break;
            v=vstack[vstackptr+argi++];
            if(v.t==TY_NUMBER) sqlite3_str_appendf(s,"%X",(unsigned int)v.u);
            break;
          case '%':
            sqlite3_str_appendchar(s,1,'%');
            break;
        }
      } else {
        sqlite3_str_appendall(s,t);
        break;
      }
    }
    sqlite3_str_appendchar(s,1,0);
    quiz_text=sqlite3_str_finish(s);
  } else {
    switch(v.t) {
      case TY_STRING: case TY_LEVELSTRING:
        quiz_text=sqlite3_mprintf("%s",value_string_ptr(v));
        break;
      case TY_NUMBER:
        quiz_text=sqlite3_mprintf("%llu",(sqlite3_int64)v.u);
        break;
      case TY_CLASS:
        quiz_text=sqlite3_mprintf("%s",classes[v.u]->name);
        break;
      case TY_MESSAGE:
        quiz_text=sqlite3_mprintf("%s",v.u<256?standard_message_names[v.u]:messages[v.u-256]);
        break;
      default:
        Throw("Type mismatch");
    }
  }
  if(!quiz_text) fatal("Allocation failed\n");
  t=quiz_text;
  while(*t) {
    if(*t==16) {
      quiz_obj.t=objects[from]->generation?:1;
      quiz_obj.u=from;
    }
    if(*t==31 && t[1]) t+=2; else t+=1;
  }
}

static int v_dispatch(const Uint16*code) {
  int i=msgvars.arg1.u;
  if(msgvars.arg1.t!=TY_NUMBER) Throw("Type mismatch");
  if(!i || (msgvars.arg1.u&~0xFF) || !code[i]) {
    StackReq(0,1);
    if(!code[256]) Push(msgvars.arg2);
  }
  if(msgvars.arg1.u&~0xFF) {
    if(current_key && !v_bool(msgvars.arg3)) key_ignored=all_flushed=1;
    return 0;
  }
  if(!i) return 0;
  if(current_key && !v_bool(msgvars.arg3) && !(keymask[i>>3]&(1<<(i&7)))) key_ignored=all_flushed=1;
  if(!code[i] && !msgvars.arg3.t && !msgvars.arg3.u && !key_ignored) i=256;
  return code[i];
}

static int v_in(void) {
  int p=vstackptr;
  Value v;
  while(vstackptr-- && vstack[vstackptr].t!=TY_MARK);
  if(!vstackptr) Throw("No mark");
  v=Pop();
  while(--p>vstackptr) if(v_equal(v,vstack[p])) return 1;
  return 0;
}

static Uint32 v_chain(Value v,Uint16 c) {
  if(v.t==TY_SOUND || v.t==TY_USOUND) Throw("Cannot convert sound to object");
  if(v.t<=TY_MAXTYPE) return VOIDLINK;
  if(v.u>=nobjects || !objects[v.u] || objects[v.u]->generation!=v.t) return VOIDLINK;
  if(objects[v.u]->class!=c) return VOIDLINK;
  return v.u;
}

static inline Uint32 manhattan(Uint32 a,Uint32 b) {
  if(a==VOIDLINK || b==VOIDLINK) return 0;
  return abs(objects[a]->x-objects[b]->x)+abs(objects[a]->y-objects[b]->y);
}

static inline Uint32 chebyshev(Uint32 a,Uint32 b) {
  Uint32 i,j;
  if(a==VOIDLINK || b==VOIDLINK) return 0;
  i=abs(objects[a]->x-objects[b]->x);
  j=abs(objects[a]->y-objects[b]->y);
  return i>j?i:j;
}

static inline void v_flip(void) {
  int p=vstackptr;
  int n;
  Value v;
  while(p>0 && vstack[p-1].t!=TY_MARK) p--;
  if(!p) Throw("No mark");
  p++;
  for(n=0;n<(vstackptr-p)/2;n++) {
    v=vstack[p+n];
    vstack[p+n]=vstack[vstackptr-n-1];
    vstack[vstackptr-n-1]=v;
  }
}

static int v_count(void) {
  int p=vstackptr;
  while(p>0 && vstack[p-1].t!=TY_MARK) p--;
  if(!p) Throw("No mark");
  return vstackptr-p;
}

static int v_uniq(Value v) {
  int p=vstackptr;
  if(v.t==TY_SOUND || v.t==TY_USOUND) Throw("Cannot compare sounds");
  while(p>0) {
    p--;
    if(vstack[p].t==TY_MARK) return 1;
    if(vstack[p].t==TY_SOUND || vstack[p].t==TY_USOUND) Throw("Cannot compare sounds");
    if(v.t==vstack[p].t && v.u==vstack[p].u) return 0;
    if((v.t==TY_STRING || v.t==TY_LEVELSTRING) && (vstack[p].t==TY_STRING || vstack[p].t==TY_LEVELSTRING)) {
      if(!strcmp(value_string_ptr(v),value_string_ptr(vstack[p]))) return 1;
    }
  }
  Throw("No mark");
}

static inline void v_obj_moving_to(Uint32 x,Uint32 y) {
  int d;
  Uint32 n,xx,yy;
  Object*o;
  for(d=0;d<8;d++) {
    xx=x-x_delta[d];
    yy=y-y_delta[d];
    if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
    n=playfield[xx+yy*64-65];
    while(n!=VOIDLINK) {
      o=objects[n];
      if((o->oflags&OF_MOVING) && !(o->oflags&(OF_DESTROYED|OF_VISUALONLY)) && o->dir==d) {
        if(vstackptr>=VSTACKSIZE-2) Throw("Stack overflow");
        Push(OVALUE(n));
      }
      n=o->up;
    }
  }
}

static inline int v_target(Uint32 n) {
  if(n==VOIDLINK) return 0;
  if(msgvars.arg1.t || msgvars.arg2.t) Throw("Type mismatch");
  return (objects[n]->x==msgvars.arg1.u && objects[n]->y==msgvars.arg2.u);
}

static int colocation(Uint32 a,Uint32 b) {
  if(a==VOIDLINK || b==VOIDLINK) return 0;
  if((objects[a]->oflags|objects[b]->oflags)&OF_DESTROYED) return 0;
  if((objects[a]->oflags^objects[b]->oflags)&OF_BIZARRO) return 0;
  return (objects[a]->x==objects[b]->x && objects[a]->y==objects[b]->y);
}

static Uint32 pat_find_at(int xy,Uint32 m4,Uint32 m5,Uint32 m6,Uint32 m7,Uint8 cl) {
  Uint32 n=playfield[xy];
  Object*o;
  while(n!=VOIDLINK) {
    o=objects[n];
    if(!(o->oflags&(OF_DESTROYED|OF_VISUALONLY))) {
      if(o->misc4.t==TY_SOUND || o->misc4.t==TY_USOUND) Throw("Cannot convert sound to type");
      if(o->misc5.t==TY_SOUND || o->misc5.t==TY_USOUND) Throw("Cannot convert sound to type");
      if(o->misc6.t==TY_SOUND || o->misc6.t==TY_USOUND) Throw("Cannot convert sound to type");
      if(o->misc7.t==TY_SOUND || o->misc7.t==TY_USOUND) Throw("Cannot convert sound to type");
      if(o->misc4.t==TY_NUMBER && (o->misc4.u&m4)) return n;
      if(o->misc5.t==TY_NUMBER && (o->misc5.u&m5)) return n;
      if(o->misc6.t==TY_NUMBER && (o->misc6.u&m6)) return n;
      if(o->misc7.t==TY_NUMBER && (o->misc7.u&m7)) return n;
      if(classes[o->class]->collisionLayers&cl) return n;
    }
    n=o->up;
  }
  return VOIDLINK;
}

typedef struct {
  Uint16 ptr,depth;
  Uint8 x,y,dir,t;
} ChoicePoint;

#define MAXCHOICE 80
static Uint32 v_pattern(Uint16*code,int ptr,Uint32 obj,char all) {
  Uint8 x=objects[obj]->x;
  Uint8 y=objects[obj]->y;
  Uint8 d=objects[obj]->dir;
  Uint32 n=VOIDLINK;
  Uint32 m;
  Uint16 g;
  Value v;
  static ChoicePoint cp[MAXCHOICE];
  Uint8 cpi=0;
  if(!x) return VOIDLINK;
  cp->depth=vstackptr;
  again: switch(code[ptr++]) {
    case 0 ... 7:
      n=VOIDLINK;
      x+=x_delta[code[ptr-1]];
      y+=y_delta[code[ptr-1]];
      if(x<1 || x>pfwidth || y<1 || y>pfheight) goto fail;
      break;
    case 8 ... 15:
      n=VOIDLINK;
      x+=x_delta[(code[ptr-1]+d)&7];
      y+=y_delta[(code[ptr-1]+d)&7];
      if(x<1 || x>pfwidth || y<1 || y>pfheight) goto fail;
      break;
    case 0x0200 ... 0x02FF:
      g=code[ptr-1]&255;
      goto message;
    case 0x1000 ... 0x11FF:
      g=code[ptr-1]&255;
      if(g<0x20) m=pat_find_at(x+y*64-65,1UL<<(g&31),0,0,0,0);
      else if(g<0x40) m=pat_find_at(x+y*64-65,0,1UL<<(g&31),0,0,0);
      else if(g<0x60) m=pat_find_at(x+y*64-65,0,0,1UL<<(g&31),0,0);
      else if(g<0x80) m=pat_find_at(x+y*64-65,0,0,0,1UL<<(g&31),0);
      else m=pat_find_at(x+y*64-65,0,0,0,0,1U<<(g&7));
      if(code[ptr-1]&0x0100) {
        if(m!=VOIDLINK) goto fail;
      } else {
        n=m;
        if(n==VOIDLINK) goto fail;
      }
      break;
    case 0x4000 ... 0x7FFF:
      n=obj_class_at(code[ptr-1]&0x3FFF,x,y);
      if(n==VOIDLINK) goto fail;
      break;
    case 0xC000 ... 0xFFFF:
      g=(code[ptr-1]&0x3FFF)+256;
    message:
      m=playfield[x+y*64-65];
      while(m!=VOIDLINK) {
        v=send_message(obj,m,g,NVALUE(d),OVALUE(n),NVALUE(0));
        if(v.t==TY_NUMBER) {
          if(v.u==1) goto fail;
          else if(v.u==2) break;
          else if(v.u) Throw("Invalid return value from message in pattern matching");
        } else if(v.t>TY_MAXTYPE) {
          n=v_object(v);
          x=objects[n]->x;
          y=objects[n]->y;
          if(!x) return VOIDLINK;
        } else {
          Throw("Type mismatch");
        }
        m=objects[m]->up;
      }
      break;
    case OP_ADD:
      if(vstackptr>=VSTACKSIZE-1) Throw("Stack overflow");
      m=(n==VOIDLINK?obj_bottom_at(x,y):n);
      Push(OVALUE(m));
      break;
    case OP_AGAIN:
      if(cp[cpi].ptr!=ptr+1) {
        if(cpi>=MAXCHOICE-1) Throw("Choice overflow");
        cpi++;
        cp[cpi].ptr=ptr+1;
      }
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].dir=d;
      cp[cpi].depth=vstackptr;
      ptr=code[ptr]+2;
      break;
    case OP_BEGIN:
      ptr++;
      break;
    case OP_BISHOP:
      if(cpi>=MAXCHOICE-4) Throw("Choice overflow");
      d=1;
      cpi+=3;
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].depth=vstackptr;
      cp[cpi].ptr=ptr;
      cp[cpi-1]=cp[cpi-2]=cp[cpi];
      cp[cpi].dir=3;
      cp[cpi-1].dir=5;
      cp[cpi-2].dir=7;
      break;
    case OP_CLIMB:
      if(playfield[x+y*64-65]==VOIDLINK || height_at(x,y)>objects[obj]->climb) goto fail;
      break;
    case OP_CLIMB_C:
      g=code[ptr++];
      if(playfield[x+y*64-65]==VOIDLINK || height_at(x,y)>g) goto fail;
      break;
    case OP_CUT:
      if(cpi) cpi--;
      break;
    case OP_DIR:
      d=objects[obj]->dir;
      break;
    case OP_DIR_C:
      if(n==VOIDLINK) Throw("No object specified in pattern");
      d=objects[n]->dir;
      break;
    case OP_DIR_E:
      changed=1;
      objects[obj]->dir=d;
      break;
    case OP_DIR_EC:
      if(n==VOIDLINK) Throw("No object specified in pattern");
      changed=1;
      objects[n]->dir=d;
      break;
    case OP_ELSE:
      ptr--;
      while(code[ptr]==OP_ELSE) ptr=code[ptr+2];
      break;
    case OP_FUNCTION:
      StackReq(0,2);
      Push(OVALUE(n));
      Push(NVALUE(d));
      execute_program(classes[0]->codes,functions[code[ptr++]],obj);
      StackReq(1,0);
      v=Pop();
      if(v.t==TY_NUMBER) {
        d=v.u&7;
      } else if(v.t==TY_MARK) {
        goto fail;
      } else if(v.t>TY_MAXTYPE) {
        n=v_object(v);
        x=objects[n]->x;
        y=objects[n]->y;
        if(!x) return VOIDLINK;
      } else {
        Throw("Type mismatch");
      }
      break;
    case OP_HEIGHT:
      if(playfield[x+y*64-65]==VOIDLINK || height_at(x,y)<=objects[obj]->climb) goto fail;
      break;
    case OP_HEIGHT_C:
      g=code[ptr++];
      if(playfield[x+y*64-65]==VOIDLINK || height_at(x,y)<=g) goto fail;
      break;
    case OP_IF:
      if(cpi>=MAXCHOICE-1) Throw("Pattern stack overflow");
      cpi++;
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].dir=d;
      cp[cpi].depth=vstackptr;
      cp[cpi].ptr=code[ptr++];
      break;
    case OP_LOC:
      if(vstackptr>=VSTACKSIZE-2) Throw("Stack overflow");
      Push(NVALUE(x));
      Push(NVALUE(y));
      break;
    case OP_MARK:
      if(vstackptr>=VSTACKSIZE-1) Throw("Stack overflow");
      Push(UVALUE(0,TY_MARK));
      break;
    case OP_MUL:
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].dir=d;
      break;
    case OP_NEXT:
      goto fail;
    case OP_OBJABOVE:
      if(n==VOIDLINK) Throw("No object specified in pattern");
      n=obj_above(n);
      break;
    case OP_OBJBELOW:
      if(n==VOIDLINK) Throw("No object specified in pattern");
      n=obj_below(n);
      break;
    case OP_OBJBOTTOMAT:
      n=obj_bottom_at(x,y);
      break;
    case OP_OBJTOPAT:
      n=obj_top_at(x,y);
      break;
    case OP_QUEEN:
      if(cpi>=MAXCHOICE-8) Throw("Choice overflow");
      d=0;
      cpi+=7;
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].depth=vstackptr;
      cp[cpi].ptr=ptr;
      cp[cpi-1]=cp[cpi-2]=cp[cpi-3]=cp[cpi-4]=cp[cpi-5]=cp[cpi-6]=cp[cpi];
      cp[cpi].dir=1;
      cp[cpi-1].dir=2;
      cp[cpi-2].dir=3;
      cp[cpi-3].dir=4;
      cp[cpi-4].dir=5;
      cp[cpi-5].dir=6;
      cp[cpi-6].dir=7;
      break;
    case OP_RET:
      if(all) {
        if(vstackptr>=VSTACKSIZE-1) Throw("Stack overflow");
        if(n==VOIDLINK) n=obj_bottom_at(x,y);
        Push(OVALUE(n));
        goto fail;
      } else {
        return n==VOIDLINK?obj_bottom_at(x,y):n;
      }
      break;
    case OP_ROOK:
      if(cpi>=MAXCHOICE-4) Throw("Choice overflow");
      d=0;
      cpi+=3;
      cp[cpi].x=x;
      cp[cpi].y=y;
      cp[cpi].depth=vstackptr;
      cp[cpi].ptr=ptr;
      cp[cpi-1]=cp[cpi-2]=cp[cpi];
      cp[cpi].dir=2;
      cp[cpi-1].dir=4;
      cp[cpi-2].dir=6;
      break;
    case OP_SUB:
      if(vstackptr>=VSTACKSIZE-1) Throw("Stack overflow");
      Push(NVALUE(0));
      break;
    case OP_THEN:
      // This opcode does nothing
      break;
    case OP_TRACE:
      if(main_options['t']) {
        printf("ptr=%d cpi=%d x=%d y=%d dir=%d obj=%lu ",ptr,cpi,x,y,d,(long)n);
        printf("[ptr=%d x=%d y=%d dir=%d]\n",cp[cpi].ptr,cp[cpi].x,cp[cpi].y,cp[cpi].dir);
      }
      break;
    default:
      fprintf(stderr,"Unrecognized opcode 0x%04X at 0x%04X in pattern\n",code[ptr-1],ptr-1);
      Throw("Internal error: Unimplemented opcode in pattern");
  }
  goto again;
  fail:
  if(!all) {
    if(vstackptr<cp->depth) Throw("Stack underflow in pattern matching");
    vstackptr=cp[cpi].depth;
  }
  if(!cpi) return VOIDLINK;
  x=cp[cpi].x;
  y=cp[cpi].y;
  d=cp[cpi].dir;
  ptr=cp[cpi].ptr;
  n=VOIDLINK;
  cpi--;
  goto again;
}

static Uint32 v_pattern_anywhere(Uint16*code,int ptr,char all) {
  int i;
  Uint32 n;
  for(i=0;i<pfheight*64;i++) {
    n=playfield[i];
    if(n!=VOIDLINK) {
      n=v_pattern(code,ptr,n,all);
      if(n!=VOIDLINK && !all) return n;
    }
  }
  return VOIDLINK;
}

static int v_case(Uint16*code,int ptr,Value v) {
  int n=(code[ptr]&255)+1;
  int t0=(code[ptr]>>12)&15;
  int t1=(code[ptr]>>8)&15;
  int i,j,v0,v1;
  if(v.t==TY_SOUND || v.t==TY_USOUND) Throw("Cannot use sound in case block");
  for(i=0;i<n;i++) {
    v0=code[ptr+i+i+1];
    v1=code[ptr+i+i+2];
    if(!v0 && v.t==TY_NUMBER && !v.u) goto found;
    if(v.t==t0) switch(t0) {
      case TY_NUMBER:
        if(!((v0^v.u)&0xFFFF)) goto found;
        break;
      case TY_CLASS:
        if(v.u==v0) goto found;
        break;
      case TY_MESSAGE:
        if(v.u+1==v0) goto found;
        break;
    }
  }
  v1=code[ptr+n+n+1];
  found:
  switch(t1) {
    case TY_NUMBER:
      Push(NVALUE(v1));
      break;
    case TY_CLASS:
      if(v1) Push(CVALUE(v1)); else Push(NVALUE(0));
      break;
    case TY_MESSAGE:
      if(v1) Push(MVALUE(v1-1)); else Push(NVALUE(0));
      break;
    case TY_STRING:
      if(v1) Push(UVALUE(v1-1,TY_STRING)); else Push(NVALUE(0));
      break;
    case TY_CODE:
      return v1;
  }
  return ptr+n+n+2;
}

static void v_data(Value v1,Value v2) {
  const unsigned char*s;
  int i;
  Value v;
  if(v1.t!=TY_STRING && v1.t!=TY_LEVELSTRING) Throw("Type mismatch");
  s=value_string_ptr(v1);
  if(v2.t==TY_NUMBER) {
    for(;;) {
      s+=strcspn(s,"\x1F\x1E");
      if(!*s) return;
      if(*s==31) {
        s++;
        if(*s) s++;
      } else {
        s++;
        if(!v2.u) break;
        v2.u--;
      }
    }
    while(s && *s && *s!='\\') {
      if(*s=='_') {
        v.t=TY_MARK;
        v.u=0;
        s++;
      } else if(*s=='$') {
        s++;
        v.t=TY_CLASS;
        i=strcspn(s,";\\");
        for(v.u=1;v.u<0x4000;v.u++) {
          if(classes[v.u] && !(classes[v.u]->cflags&(CF_NOCLASS2|CF_GROUP)) && i==strlen(classes[v.u]->name) && !strncmp(s,classes[v.u]->name,i)) {
            s+=i;
            break;
          }
        }
      } else {
        v.t=TY_NUMBER;
        v.u=strtol(s,(char**)&s,10);
      }
      StackReq(0,1);
      Push(v);
      if(*s=='\\') break;
      if(*s!=';' && *s!='\\') Throw("Invalid string data");
      if(*s==';') s++; else break;
    }
  } else if(v2.t==TY_MESSAGE && v2.u==MSG_KEY) {
    v.t=TY_NUMBER;
    v.u=0;
    for(;;) {
      s+=strcspn(s,"\x1F\x10");
      if(!*s) return;
      if(*s==31) {
        s++;
        if(*s) s++;
      } else {
        s++;
        StackReq(0,1);
        if(*s) v.u=*s++;
        Push(v);
      }
    }
  } else {
    Throw("Type mismatch");
  }
}

static void v_trigger(Uint32 from,Uint32 to,Value v) {
  Object*o;
  Uint32 n;
  if(v.t!=TY_MESSAGE) Throw("Type mismatch");
  o=objects[to];
  if(classes[o->class]->cflags&CF_COMPATIBLE) return;
  switch(v.u) {
    case MSG_MOVED:
      if(o->oflags&OF_MOVED2) {
        o->oflags&=~OF_MOVED2;
        send_message(from,to,MSG_MOVED,NVALUE(0),msgvars.arg2,msgvars.arg3);
      }
      break;
    case MSG_DEPARTED:
      if(o->departed2) {
        n=o->departed2;
        o->departed2=0;
        send_message(from,to,MSG_DEPARTED,NVALUE(n),msgvars.arg2,msgvars.arg3);
      }
      break;
    case MSG_ARRIVED:
      if(o->arrived2) {
        n=o->arrived2;
        o->arrived2=0;
        send_message(from,to,MSG_ARRIVED,NVALUE(n),msgvars.arg2,msgvars.arg3);
      }
      break;
    default: Throw("Invalid message for Trigger");
  }
}

static void v_trigger_at(Uint32 from,Uint32 x,Uint32 y,Value v) {
  Uint32 m,n;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return;
  n=playfield[x+y*64-65];
  while(n!=VOIDLINK) {
    m=objects[n]->up;
    v_trigger(from,n,v);
    n=m;
  }
}

static void v_sweep(Uint32 from,Value arg3) {
  Value arg2=Pop();
  Value arg1=Pop();
  Value v=Pop();
  Uint8 hv=v_bool(Pop());
  Uint16 msg=v.u;
  Uint8 x0,y0,x1,y1,x,y;
  Uint32 m,n;
  if(v.t!=TY_MESSAGE) Throw("Type mismatch");
  v=Pop(); if(v.t) Throw("Type mismatch"); y1=(v.s<0?0:v.s>pfheight?pfheight+1:v.s);
  v=Pop(); if(v.t) Throw("Type mismatch"); x1=(v.s<0?0:v.s>pfheight?pfheight+1:v.s);
  v=Pop(); if(v.t) Throw("Type mismatch"); y0=(v.s<0?0:v.s>pfheight?pfheight+1:v.s);
  v=Pop(); if(v.t) Throw("Type mismatch"); x0=(v.s<0?0:v.s>pfheight?pfheight+1:v.s);
  if((!x0 && !x1) || (!y0 && !y1) || (x0>pfwidth && x1>pfwidth) || (y0>pfheight && y1>pfheight)) return;
  if(!x0) x0=1;
  if(!x1) x1=1;
  if(!y0) y0=1;
  if(!y1) y1=1;
  if(x0>pfwidth) x0=pfwidth;
  if(y0>pfheight) y0=pfheight;
  if(x1>pfwidth) x1=pfwidth;
  if(y1>pfheight) y1=pfheight;
  for(x=x0,y=y0;;) {
    n=playfield[x+y*64-65];
    while(n!=VOIDLINK) {
      m=objects[n]->up;
      send_message(from,n,msg,arg1,arg2,arg3);
      n=m;
    }
    if(x==x1 && y==y1) break;
    if(hv) {
      if(y==y1) y=y0,x+=(x1>x0?1:-1); else y+=(y1>y0?1:-1);
    } else {
      if(x==x1) x=x0,y+=(y1>y0?1:-1); else x+=(x1>x0?1:-1);
    }
  }
}

static Uint32 v_hitme(Uint32 objE,Uint16 dir) {
  Object*o;
  Object*oE=objects[objE];
  Uint32 hit=msgvars.arg3.u&~0x8000;
  if(msgvars.from==VOIDLINK) return 0;
  if(msgvars.arg3.t) Throw("Type mismatch in HitMe");
  o=objects[msgvars.from];
  if(dir>7) dir=(o->dir+dir)&7;
  if(!(dir&1) && !(hit&0x122)) {
    if(o->sharp[dir>>1]>oE->hard[(dir^4)>>1] && !v_bool(destroy(msgvars.from,objE,2))) hit|=0x8004;
    if(oE->sharp[(dir^4)>>1]>o->hard[dir>>1] && !v_bool(destroy(objE,msgvars.from,1))) hit|=0x4C;
  }
  if(!(hit&0x344) && (oE->shovable&(1<<dir)) && o->inertia>=oE->weight) {
    oE->inertia=o->inertia;
    if(move_dir(msgvars.from,objE,dir)) {
      if(!(oE->oflags&OF_DESTROYED)) o->inertia=oE->inertia;
      hit|=0x8000;
    }
  }
  msgvars.arg3.u=hit|7;
  return (hit&0x8008)==0x8000?1:0;
}

static Uint32 convert_link(Value v,Uint32 obj,Uint16*code) {
  int i;
  switch(v.t) {
    case TY_NUMBER:
      if(v.u) Throw("Type mismatch");
      return 0xFFFF;
    case TY_MESSAGE:
      i=get_message_ptr(objects[obj]->class,v.u);
      return i==0xFFFF?get_message_ptr(0,v.u):(i|(objects[obj]->class<<16));
    case TY_CODE:
      i=v.u>>16;
      if(i==objects[obj]->class || !i || code==classes[i]->codes) return v.u;
      Throw("Link mismatch");
    default: Throw("Type mismatch");
  }
}

static Uint32 morton(Uint32 x) {
  x&=0xFFFF;
  x|=x<<8; x&=0x00FF00FF;
  x|=x<<4; x&=0x0F0F0F0F;
  x|=x<<2; x&=0x33333333;
  x|=x<<1; x&=0x55555555;
  return x;
}

static Value v_find_connection(Uint32 xobj,Uint32 lnk,Uint32 obj,Uint16*code) {
  Object*o=objects[obj];
  Value v=NVALUE(0);
  Uint32 n;
  int i;
  int first=nconn;
  int last;
  if(nconn!=pconn) Throw("Improper nested connected move");
  if((o->oflags^OF_CONNECTION)&(OF_CONNECTION|OF_MOVING|OF_DESTROYED|OF_BIZARRO)) return NVALUE(0);
  add_connection(obj);
  for(i=first;i<nconn;i++) {
    o=objects[n=conn[i].obj];
    v=send_message(obj,n,MSG_CONNECT,NVALUE(i-first),NVALUE(objects[obj]->dir),UVALUE(0,TY_MARK));
    if(v_bool(v)) goto done;
    if(o->shovable&0xFF00) add_connection_shov(o);
  }
  last=pconn=nconn;
  for(i=first;i<last;i++) {
    StackReq(0,1);
    Push(OVALUE(conn[i].obj));
    if(lnk!=0xFFFF) execute_program(classes[lnk>>16]->codes,lnk&0xFFFF,xobj);
  }
  done:
  for(i=first;i<nconn;i++) {
    o=objects[conn[i].obj];
    o->oflags&=~OF_MOVING;
    if(conn[i].visual) o->oflags|=OF_VISUALONLY; else o->oflags&=~OF_VISUALONLY;
  }
  pconn=nconn=first;
  StackReq(0,1);
  return v;
}

static Uint32 v_walkable(Uint32 obj,Uint32 x,Uint32 y) {
  Uint32 r=0;
  Object*o;
  Uint32 n;
  Uint8 lay;
  if(obj==VOIDLINK) return 0;
  o=objects[obj];
  if(o->oflags&OF_DESTROYED) return 0;
  lay=classes[o->class]->collisionLayers;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 0;
  n=playfield[x+y*64-65];
  while(n!=VOIDLINK) {
    if(n!=obj) {
      if(lay&classes[objects[n]->class]->collisionLayers) return 0;
      if(!(objects[n]->oflags&(OF_VISUALONLY|OF_DESTROYED))) {
        if(o->climb<objects[n]->height) return 0;
        r=1;
      }
    }
    n=objects[n]->up;
  }
  return r;
}

static Uint32 v_walkable_c(Uint32 c,Uint32 x,Uint32 y) {
  Class*cl=classes[c];
  Uint32 r=0;
  Uint8 lay=cl->collisionLayers;
  Uint32 n;
  if(cl->cflags&CF_NOCLASS2) Throw("Invalid class number");
  if(x<1 || x>pfwidth || y<1 || y>pfheight) return 0;
  n=playfield[x+y*64-65];
  while(n!=VOIDLINK) {
    if(lay&classes[objects[n]->class]->collisionLayers) return 0;
    if(!(objects[n]->oflags&(OF_VISUALONLY|OF_DESTROYED))) {
      if(cl->climb<objects[n]->height) return 0;
      r=1;
    }
    n=objects[n]->up;
  }
  return r;
}

static Value v_collision_layers(Value v) {
  Uint32 i;
  if(v.t==TY_CLASS) {
    return NVALUE(classes[v.u]->collisionLayers);
  } else {
    i=v_object(v);
    if(i==VOIDLINK) return NVALUE(0); else return NVALUE(classes[objects[i]->class]->collisionLayers);
  }
}

// Here is where the execution of a Free Hero Mesh bytecode subroutine is executed.
#define NoIgnore() do{ changed=1; }while(0)
#define GetVariableOf(a,b) (i=v_object(Pop()),i==VOIDLINK?NVALUE(0):b(objects[i]->a))
#define GetVariableOrAttributeOf(a,b) (t2=Pop(),t2.t==TY_CLASS?NVALUE(classes[t2.u]->a):(i=v_object(t2),i==VOIDLINK?NVALUE(0):b(objects[i]->a)))
#define Numeric(a) do{ if((a).t!=TY_NUMBER) Throw("Type mismatch"); }while(0)
#define DivideBy(a) do{ Numeric(a); if(!(a).u) Throw("Division by zero"); }while(0)
#define GetFlagOf(a) t2=Pop(),Push(t2.t==TY_CLASS?NVALUE(classes[t2.u]->oflags&a?1:0):(i=v_object(t2),i==VOIDLINK?NVALUE(0):NVALUE(objects[i]->oflags&a?1:0)))
#define GetClassFlagOf(a) t2=Pop(),Push(t2.t==TY_CLASS?NVALUE(classes[t2.u]->cflags&a?1:0):(i=v_object(t2),i==VOIDLINK?NVALUE(0):NVALUE(classes[objects[i]->class]->cflags&a?1:0)))
#define SetFlagOf(a) do{ t2=Pop(); i=v_object(Pop()); if(i!=VOIDLINK) { if(v_bool(t2)) objects[i]->oflags|=a; else objects[i]->oflags&=~a; } }while(0)
#define NotSound(a) do{ if((a).t==TY_SOUND || (a).t==TY_USOUND) Throw("Cannot convert sound to type"); }while(0)
static void execute_program(Uint16*code,int ptr,Uint32 obj) {
  Uint32 i,j;
  Object*o=objects[obj];
  Value t1,t2;
  static Value t3,t4,t5,t6;
  if(StackProtection()) Throw("Call stack overflow");
  // Note about bit shifting: At least when running Hero Mesh in DOSBOX, out of range bit shifts produce zero.
  // I don't know if this is true on all computers that Hero Mesh runs on, though. (Some documents suggest that x86 doesn't work this way)
  // The below code assumes that signed right shifting is available on the computer that Free Hero Mesh runs on.
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
    case 0x1F00 ... 0x1F1F: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u&(1L<<(code[ptr-1]&31))?1:0)); break; // tbit
    case 0x1F20 ... 0x1F3F: StackReq(2,1); t2=Pop(); t1=Pop(); Numeric(t2); i=code[ptr-1]&31; if(v_bool(t1)) t2.u|=1L<<i; else t2.u&=~(1L<<i); Push(t2); break; // sbit
    case 0x2000 ... 0x27FF: StackReq(0,1); Push(o->uservars[code[ptr-1]&0x7FF]); break;
    case 0x2800 ... 0x28FF: StackReq(0,1); Push(globals[code[ptr-1]&0x7FF]); break;
    case 0x3000 ... 0x37FF: NoIgnore(); StackReq(1,0); o->uservars[code[ptr-1]&0x7FF]=Pop(); break;
    case 0x3800 ... 0x38FF: NoIgnore(); StackReq(1,0); globals[code[ptr-1]&0x7FF]=Pop(); break;
    case 0x4000 ... 0x7FFF: StackReq(0,1); Push(CVALUE(code[ptr-1]-0x4000)); break;
    case 0x87E8 ... 0x87FF: StackReq(0,1); Push(NVALUE(1UL<<(code[ptr-1]&31))); break;
    case 0xC000 ... 0xFFFF: StackReq(0,1); Push(MVALUE((code[ptr-1]&0x3FFF)+256)); break;
    case OP_ADD: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u+t2.u)); break;
    case OP_AND: StackReq(1,0); t1=Pop(); if(!v_bool(t1)) { Push(t1); ptr=code[ptr]; } else { ptr++; } break;
    case OP_ANIMATE: NoIgnore(); StackReq(4,0); t4=Pop(); Numeric(t4); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); animate(obj,t1.u,t2.u,t3.u,t4.u); break;
    case OP_ANIMATE_E: NoIgnore(); StackReq(4,0); t4=Pop(); Numeric(t4); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); animate_ext(obj,t1.u,t2.u,t3.u,t4.u); break;
    case OP_ANIMATEDEAD: StackReq(6,0); t6=Pop(); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_animate_dead(t1,t2,t3,t4,t5,t6); break;
    case OP_ANIMATEDEAD_E: StackReq(1,0); t1=Pop(); if(ndeadanim && !t1.t) deadanim[ndeadanim-1].delay=t1.u; break;
    case OP_ARG1: StackReq(0,1); Push(msgvars.arg1); break;
    case OP_ARG1_E: StackReq(1,0); msgvars.arg1=Pop(); break;
    case OP_ARG2: StackReq(0,1); Push(msgvars.arg2); break;
    case OP_ARG2_E: StackReq(1,0); msgvars.arg2=Pop(); break;
    case OP_ARG3: StackReq(0,1); Push(msgvars.arg3); break;
    case OP_ARG3_E: StackReq(1,0); msgvars.arg3=Pop(); break;
    case OP_ARRAYCELL: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Push(v_array_cell(t1,t2.u,t3.u)); break;
    case OP_ARRAYCELL_C: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Push(v_array_cell(t1,t2.u-1,t3.u-1)); break;
    case OP_ARRAYSLICE: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Push(v_array_slice(t1,t2.u,t3.u)); break;
    case OP_ARRIVALS: StackReq(0,1); Push(NVALUE(o->arrivals&0x1FFFFFF)); break;
    case OP_ARRIVALS_C: StackReq(1,1); Push(GetVariableOrAttributeOf(arrivals&0x1FFFFFF,NVALUE)); break;
    case OP_ARRIVALS_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->arrivals=t1.u; break;
    case OP_ARRIVALS_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->arrivals=t1.u; break;
    case OP_ARRIVED: StackReq(0,1); Push(NVALUE(o->arrived&0x1FFFFFF)); break;
    case OP_ARRIVED_C: StackReq(1,1); Push(GetVariableOf(arrived&0x1FFFFFF,NVALUE)); break;
    case OP_ARRIVED_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->arrived=t1.u; break;
    case OP_ARRIVED_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->arrived=t1.u; break;
    case OP_ASSASSINATE: NoIgnore(); destroy(obj,obj,8); break;
    case OP_ASSASSINATE_C: NoIgnore(); StackReq(1,0); i=v_object(Pop()); destroy(obj,i,8); break;
    case OP_BAND: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u&t2.u)); break;
    case OP_BIZARRO: StackReq(0,1); if(o->oflags&OF_BIZARRO) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_BIZARRO_C: StackReq(1,1); GetFlagOf(OF_BIZARRO); break;
    case OP_BIZARRO_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); set_bizarro(obj,t1.u); break;
    case OP_BIZARRO_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); set_bizarro(v_object(Pop()),t1.u); break;
    case OP_BIZARROSWAP: NoIgnore(); StackReq(2,1); t2=Pop(); t1=Pop(); i=v_bizarro_swap(t1,t2); Push(NVALUE(i)); break;
    case OP_BIZARROSWAP_D: NoIgnore(); StackReq(2,0); t2=Pop(); t1=Pop(); v_bizarro_swap(t1,t2); break;
    case OP_BNOT: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(~t1.u)); break;
    case OP_BOR: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u|t2.u)); break;
    case OP_BROADCAST: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),0)); break;
    case OP_BROADCAST_D: StackReq(4,0); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),0); break;
    case OP_BROADCASTAND: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),2)); break;
    case OP_BROADCASTANDEX: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,t5,2)); break;
    case OP_BROADCASTCLASS: StackReq(3,1); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_broadcast(obj,CVALUE(code[ptr++]),t2,t3,t4,NVALUE(0),0)); break;
    case OP_BROADCASTEX: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,t5,0)); break;
    case OP_BROADCASTEX_D: StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,t5,0); break;
    case OP_BROADCASTLIST: StackReq(4,0); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),3); break;
    case OP_BROADCASTLISTEX: StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_broadcast(obj,t1,t2,t3,t4,t5,3); break;
    case OP_BROADCASTSUM: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,NVALUE(0),1)); break;
    case OP_BROADCASTSUMEX: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_broadcast(obj,t1,t2,t3,t4,t5,1)); break;
    case OP_BUSY: StackReq(0,1); if(o->oflags&OF_BUSY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_BUSY_C: StackReq(1,1); GetFlagOf(OF_BUSY); break;
    case OP_BUSY_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_BUSY; else o->oflags&=~OF_BUSY; break;
    case OP_BUSY_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_BUSY); break;
    case OP_BXOR: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u^t2.u)); break;
    case OP_CALLSUB: execute_program(code,code[ptr++],obj); break;
    case OP_CASE: StackReq(1,1); t1=Pop(); ptr=v_case(code,ptr,t1); break;
    case OP_CHAIN: StackReq(1,1); t1=Pop(); i=v_chain(t1,o->class); if(i==VOIDLINK) { Push(NVALUE(1)); } else { o=objects[obj=i]; Push(NVALUE(0)); } break;
    case OP_CHEBYSHEV: StackReq(1,1); t1=Pop(); i=chebyshev(obj,v_object(t1)); Push(NVALUE(i)); break;
    case OP_CHEBYSHEV_C: StackReq(2,1); t2=Pop(); t1=Pop(); i=chebyshev(v_object(t1),v_object(t2)); Push(NVALUE(i)); break;
    case OP_CLASS: StackReq(0,1); Push(CVALUE(o->class)); break;
    case OP_CLASS_C: StackReq(1,1); Push(GetVariableOf(class,CVALUE)); break;
    case OP_CLEAR: t1.t=TY_NUMBER; while(vstackptr>0) { t1=Pop(); if(t1.t==TY_MARK) break; } if(t1.t!=TY_MARK) Throw("No mark"); break;
    case OP_CLIMB: StackReq(0,1); Push(NVALUE(o->climb)); break;
    case OP_CLIMB_C: StackReq(1,1); Push(GetVariableOrAttributeOf(climb,NVALUE)); break;
    case OP_CLIMB_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->climb=t1.u; break;
    case OP_CLIMB_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->climb=t1.u&0xFFFF; break;
    case OP_CLIMB_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->climb=t1.u; break;
    case OP_CLIMB_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) o->climb=t1.u&0xFFFF; break;
    case OP_COLLISIONLAYERS: StackReq(0,1); Push(NVALUE(classes[o->class]->collisionLayers)); break;
    case OP_COLLISIONLAYERS_C: StackReq(1,1); t1=Pop(); Push(v_collision_layers(t1)); break;
    case OP_COLOC: StackReq(1,1); t1=Pop(); i=colocation(obj,v_object(t1)); Push(NVALUE(i)); break;
    case OP_COLOC_C: StackReq(2,1); t1=Pop(); t2=Pop(); i=colocation(v_object(t1),v_object(t2)); Push(NVALUE(i)); break;
    case OP_COMPATIBLE: StackReq(0,1); if(classes[o->class]->cflags&CF_COMPATIBLE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_COMPATIBLE_C: StackReq(1,1); GetClassFlagOf(CF_COMPATIBLE); break;
    case OP_CONNECT: NoIgnore(); add_connection(obj); break;
    case OP_CONNECT_C: NoIgnore(); StackReq(1,0); i=v_object(Pop()); if(i!=VOIDLINK) add_connection(i); break;
    case OP_CONNECTION: StackReq(0,1); if(o->oflags&OF_CONNECTION) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_CONNECTION_C: StackReq(1,1); GetFlagOf(OF_CONNECTION); break;
    case OP_CONNECTION_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_CONNECTION; else o->oflags&=~OF_CONNECTION; break;
    case OP_CONNECTION_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_CONNECTION); break;
    case OP_CONTROL: StackReq(0,1); Push(OVALUE(control_obj)); break;
    case OP_COPYARRAY: NoIgnore(); StackReq(2,0); t2=Pop(); t1=Pop(); v_copy_array(t1,t2); break;
    case OP_COUNT: StackReq(1,2); i=v_count(); Push(NVALUE(i)); break;
    case OP_CREATE: NoIgnore(); StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_create(obj,t1,t2,t3,t4,t5)); break;
    case OP_CREATE_D: NoIgnore(); StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_create(obj,t1,t2,t3,t4,t5); break;
    case OP_CRUSH: StackReq(0,1); if(o->oflags&OF_CRUSH) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_CRUSH_C: StackReq(1,1); GetFlagOf(OF_CRUSH); break;
    case OP_CRUSH_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_CRUSH; else o->oflags&=~OF_CRUSH; break;
    case OP_CRUSH_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_CRUSH); break;
    case OP_DATA: StackReq(2,1); t2=Pop(); t1=Pop(); v_data(t1,t2); break;
    case OP_DELINVENTORY: StackReq(2,0); t2=Pop(); t1=Pop(); v_delete_inventory(t1,t2); break;
    case OP_DELTA: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u>t2.u?t1.u-t2.u:t2.u-t1.u)); break;
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
    case OP_DESTROY: NoIgnore(); StackReq(0,1); Push(destroy(obj,obj,0)); break;
    case OP_DESTROY_C: NoIgnore(); StackReq(1,1); i=v_object(Pop()); Push(destroy(obj,i,0)); break;
    case OP_DESTROY_D: NoIgnore(); destroy(obj,obj,0); break;
    case OP_DESTROY_CD: NoIgnore(); StackReq(1,0); i=v_object(Pop()); destroy(obj,i,0); break;
    case OP_DESTROYED: StackReq(0,1); if(o->oflags&OF_DESTROYED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_DESTROYED_C: StackReq(1,1); t1=Pop(); Push(NVALUE(v_destroyed(t1))); break;
    case OP_DIR: StackReq(0,1); Push(NVALUE(o->dir)); break;
    case OP_DIR_C: StackReq(1,1); Push(GetVariableOf(dir,NVALUE)); break;
    case OP_DIR_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->dir=resolve_dir(obj,t1.u); break;
    case OP_DIR_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->dir=resolve_dir(i,t1.u); break;
    case OP_DISPATCH: ptr=v_dispatch(code+ptr-1); if(!ptr) return; break;
    case OP_DISTANCE: StackReq(0,1); Push(NVALUE(o->distance)); break;
    case OP_DISTANCE_C: StackReq(1,1); Push(GetVariableOf(distance,NVALUE)); break;
    case OP_DISTANCE_E: StackReq(1,0); t1=Pop(); Numeric(t1); o->distance=t1.u; break;
    case OP_DISTANCE_EC: StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->distance=t1.u; break;
    case OP_DIV: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u/t2.u)); break;
    case OP_DIV_C: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s/t2.s)); break;
    case OP_DONE: StackReq(0,1); if(o->oflags&OF_DONE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_DONE_C: StackReq(1,1); GetFlagOf(OF_DONE); break;
    case OP_DONE_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_DONE; else o->oflags&=~OF_DONE; break;
    case OP_DONE_EC: StackReq(2,0); SetFlagOf(OF_DONE); break;
    case OP_DOTPRODUCT: StackReq(2,1); t2=Pop(); t1=Pop(); Push(v_dot_product(t1,t2)); break;
    case OP_DROP: StackReq(1,0); Pop(); break;
    case OP_DROP_D: StackReq(2,0); Pop(); Pop(); break;
    case OP_DUP: StackReq(1,2); t1=Pop(); Push(t1); Push(t1); break;
    case OP_EQ: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_equal(t1,t2)?1:0)); break;
    case OP_EQ2: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(NVALUE(v_equal(t1,t3)?(v_equal(t2,t4)?1:0):0)); break;
    case OP_EXEC: StackReq(1,0); t1=Pop(); i=convert_link(t1,obj,code); if(i==0xFFFF) return; code=classes[i>>16]->codes; ptr=i&0xFFFF; break;
    case OP_EXEC_C: StackReq(1,0); t1=Pop(); i=convert_link(t1,obj,code); if(i!=0xFFFF) execute_program(classes[i>>16]->codes,i&0xFFFF,obj); break;
    case OP_FAKEMOVE: NoIgnore(); StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(fake_move_dir(obj,resolve_dir(obj,t1.u),0))); break;
    case OP_FAKEMOVE_C: NoIgnore(); StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(fake_move_dir(i,resolve_dir(i,t1.u),0))); break;
    case OP_FINDCONNECTION: NoIgnore(); StackReq(1,1); t1=Pop(); t2=v_find_connection(obj,convert_link(t1,obj,code),obj,code); Push(t2); break;
    case OP_FINDCONNECTION_C: NoIgnore(); StackReq(2,1); t1=Pop(); i=v_object(Pop()); t2=(i==VOIDLINK)?NVALUE(0):v_find_connection(obj,convert_link(t1,obj,code),i,code); Push(t2); break;
    case OP_FINISHED: StackReq(0,1); Push(NVALUE(all_flushed)); break;
    case OP_FINISHED_E: StackReq(1,0); t1=Pop(); Numeric(t1); all_flushed=t1.u; break;
    case OP_FLIP: v_flip(); break;
    case OP_FLUSHCLASS: NoIgnore(); StackReq(1,0); t1=Pop(); if(t1.t==TY_CLASS) flush_class(t1.u); else if(t1.t==TY_NUMBER && t1.s==-1) flush_all(); else if(t1.t) Throw("Type mismatch"); break;
    case OP_FLUSHOBJ: NoIgnore(); flush_object(obj); break;
    case OP_FLUSHOBJ_C: NoIgnore(); StackReq(1,0); i=v_object(Pop()); if(i!=VOIDLINK) flush_object(i); break;
    case OP_FOR: NoIgnore(); StackReq(3,1); t3=Pop(); t2=Pop(); t1=Pop(); ptr=v_for(code,ptr,t1,t2,t3); break;
    case OP_FORK: execute_program(code,ptr+1,obj); ptr=code[ptr]; break;
    case OP_FROM: StackReq(0,1); Push(OVALUE(msgvars.from)); break;
    case OP_FUNCTION: execute_program(classes[0]->codes,functions[code[ptr++]],obj); break;
    case OP_GE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t2,t1)?0:1)); break;
    case OP_GE_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t2,t1)?0:1)); break;
    case OP_GETARRAY: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Push(v_get_array(t1,t2.u,t3.u)); break;
    case OP_GETARRAY_C: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Push(v_get_array(t1,t2.u-1,t3.u-1)); break;
    case OP_GETINVENTORY: StackReq(2,2); t2=Pop(); t1=Pop(); v_get_inventory(t1,t2); break;
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
    case OP_HEIGHT_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->height=t1.u; break;
    case OP_HEIGHT_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->height=t1.u&0xFFFF; break;
    case OP_HEIGHTAT: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(height_at(t1.u,t2.u))); break;
    case OP_HITME: NoIgnore(); StackReq(1,1); t1=Pop(); Numeric(t1); i=v_hitme(obj,t1.u); break;
    case OP_IF: StackReq(1,0); if(v_bool(Pop())) ptr++; else ptr=code[ptr]; break;
    case OP_IGNOREKEY: if(current_key) key_ignored=all_flushed=1; break;
    case OP_IMAGE: StackReq(0,1); Push(NVALUE(o->image)); break;
    case OP_IMAGE_C: StackReq(1,1); Push(GetVariableOf(image,NVALUE)); break;
    case OP_IMAGE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->image=t1.u; break;
    case OP_IMAGE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->image=t1.u; break;
    case OP_IN: StackReq(2,1); i=v_in(); Push(NVALUE(i?1:0)); break;
    case OP_INERTIA: StackReq(0,1); Push(NVALUE(o->inertia)); break;
    case OP_INERTIA_C: StackReq(1,1); Push(GetVariableOf(inertia,NVALUE)); break;
    case OP_INERTIA_E: StackReq(1,0); t1=Pop(); Numeric(t1); o->inertia=t1.u; break;
    case OP_INERTIA_E16: StackReq(1,0); t1=Pop(); Numeric(t1); o->inertia=t1.u&0xFFFF; break;
    case OP_INERTIA_EC: StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->inertia=t1.u; break;
    case OP_INERTIA_EC16: StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->inertia=t1.u&0xFFFF; break;
    case OP_INITARRAY: NoIgnore(); StackReq(2,0); t2=Pop(); t1=Pop(); v_init_array(t1,t2); break;
    case OP_INT16: StackReq(0,1); Push(NVALUE(code[ptr++])); break;
    case OP_INT32: StackReq(0,1); t1=UVALUE(code[ptr++]<<16,TY_NUMBER); t1.u|=code[ptr++]; Push(t1); break;
    case OP_INTMOVE: NoIgnore(); StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(move_dir(obj,obj,t1.u))); break;
    case OP_INTMOVE_C: NoIgnore(); StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(move_dir(obj,i,t1.u))); break;
    case OP_INTMOVE_D: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); move_dir(obj,obj,t1.u); break;
    case OP_INTMOVE_CD: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) move_dir(obj,i,t1.u); break;
    case OP_INVISIBLE: StackReq(0,1); if(o->oflags&OF_INVISIBLE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_INVISIBLE_C: StackReq(1,1); GetFlagOf(OF_INVISIBLE); break;
    case OP_INVISIBLE_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_INVISIBLE; else o->oflags&=~OF_INVISIBLE; break;
    case OP_INVISIBLE_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_INVISIBLE); break;
    case OP_IS: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_is(t1,t2))); break;
    case OP_JUMPTO: NoIgnore(); StackReq(2,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); Push(NVALUE(jump_to(obj,obj,t2.u,t3.u))); break;
    case OP_JUMPTO_C: NoIgnore(); StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); i=v_object(Pop()); Push(NVALUE(jump_to(obj,i,t2.u,t3.u))); break;
    case OP_JUMPTO_D: NoIgnore(); StackReq(2,0); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); jump_to(obj,obj,t2.u,t3.u); break;
    case OP_JUMPTO_CD: NoIgnore(); StackReq(3,0); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); i=v_object(Pop()); jump_to(obj,i,t2.u,t3.u); break;
    case OP_KEY: StackReq(0,1); Push(NVALUE(current_key)); break;
    case OP_KEYCLEARED: StackReq(0,1); if(o->oflags&OF_KEYCLEARED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_KEYCLEARED_C: StackReq(1,1); GetFlagOf(OF_KEYCLEARED); break;
    case OP_KEYCLEARED_E: StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_KEYCLEARED; else o->oflags&=~OF_KEYCLEARED; break;
    case OP_KEYCLEARED_EC: StackReq(2,0); SetFlagOf(OF_KEYCLEARED); break;
    case OP_LAND: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) && v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t1,t2)?0:1)); break;
    case OP_LE_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t1,t2)?0:1)); break;
    case OP_LEVEL: StackReq(0,1); Push(NVALUE(level_code)); break;
    case OP_LINK: StackReq(0,1); Push(UVALUE((ptr+2)|(code[ptr+1]<<16),TY_CODE)); ptr=code[ptr]; break;
    case OP_LNOT: StackReq(1,1); if(v_bool(Pop())) Push(NVALUE(0)); else Push(NVALUE(1)); break;
    case OP_LOC: StackReq(0,2); Push(NVALUE(o->x)); Push(NVALUE(o->y)); break;
    case OP_LOC_C: StackReq(1,2); i=v_object(Pop()); Push(NVALUE(i==VOIDLINK?0:objects[i]->x)); Push(NVALUE(i==VOIDLINK?0:objects[i]->y)); break;
    case OP_LOCATEME: locate_me(o->x,o->y); break;
    case OP_LOCATEME_C: StackReq(1,0); i=v_object(Pop()); if(i!=VOIDLINK) locate_me(objects[i]->x,objects[i]->y); break;
    case OP_LOR: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1) || v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_LOSELEVEL: gameover=-1; Throw(0); break;
    case OP_LSH: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t2.u&~31?0:t1.u<<t2.u)); break;
    case OP_LT: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_unsigned_greater(t2,t1)?1:0)); break;
    case OP_LT_C: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_signed_greater(t2,t1)?1:0)); break;
    case OP_LXOR: StackReq(2,1); t1=Pop(); t2=Pop(); if(v_bool(t1)?!v_bool(t2):v_bool(t2)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_MANHATTAN: StackReq(1,1); t1=Pop(); i=manhattan(obj,v_object(t1)); Push(NVALUE(i)); break;
    case OP_MANHATTAN_C: StackReq(2,1); t2=Pop(); t1=Pop(); i=manhattan(v_object(t1),v_object(t2)); Push(NVALUE(i)); break;
    case OP_MARK: StackReq(0,1); Push(UVALUE(0,TY_MARK)); break;
    case OP_MAX: StackReq(2,1); t1=Pop(); Numeric(t1); t2=Pop(); Numeric(t2); if(t1.u>t2.u) Push(t1); else Push(t2); break;
    case OP_MAX_C: StackReq(2,1); t1=Pop(); Numeric(t1); t2=Pop(); Numeric(t2); if(t1.s>t2.s) Push(t1); else Push(t2); break;
    case OP_MAXINVENTORY: StackReq(1,0); t1=Pop(); Numeric(t1); if(ninventory>t1.u) Throw("Inventory overflow"); break;
    case OP_MIN: StackReq(2,1); t1=Pop(); Numeric(t1); t2=Pop(); Numeric(t2); if(t1.u<t2.u) Push(t1); else Push(t2); break;
    case OP_MIN_C: StackReq(2,1); t1=Pop(); Numeric(t1); t2=Pop(); Numeric(t2); if(t1.s<t2.s) Push(t1); else Push(t2); break;
    case OP_MINUSMOVE: StackReq(1,1); t1=Pop(); Numeric(t1); i=defer_move(obj,t1.u,0); Push(NVALUE(i)); break;
    case OP_MINUSMOVE_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); i=defer_move(i,t1.u,0); Push(NVALUE(i)); break;
    case OP_MINUSMOVE_D: StackReq(1,0); t1=Pop(); Numeric(t1); defer_move(obj,t1.u,0); break;
    case OP_MINUSMOVE_CD: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); defer_move(i,t1.u,0); break;
    case OP_MOD: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u%t2.u)); break;
    case OP_MOD_C: StackReq(2,1); t2=Pop(); DivideBy(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s%t2.s)); break;
    case OP_MORTON: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(morton(t1.u))); break;
    case OP_MORTON_C: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(morton(t2.u)|(morton(t1.u)<<1))); break;
    case OP_MOVE: NoIgnore(); StackReq(1,1); t1=Pop(); Numeric(t1); o->inertia=o->strength; Push(NVALUE(move_dir(obj,obj,t1.u))); break;
    case OP_MOVE_C: NoIgnore(); StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else { objects[i]->inertia=o->strength; Push(NVALUE(move_dir(obj,i,t1.u))); } break;
    case OP_MOVE_D: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->inertia=o->strength; move_dir(obj,obj,t1.u); break;
    case OP_MOVE_CD: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) { objects[i]->inertia=o->strength; move_dir(obj,i,t1.u); } break;
    case OP_MOVED: StackReq(0,1); if(o->oflags&OF_MOVED) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_MOVED_C: StackReq(1,1); GetFlagOf(OF_MOVED); break;
    case OP_MOVED_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_MOVED; else o->oflags&=~OF_MOVED; break;
    case OP_MOVED_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_MOVED); break;
    case OP_MOVENUMBER: StackReq(0,1); Push(NVALUE(move_number)); break;
    case OP_MOVENUMBER_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); move_number=t1.u; break;
    case OP_MOVEPLUS: NoIgnore(); StackReq(1,1); t1=Pop(); Numeric(t1); o->inertia+=o->strength; Push(NVALUE(move_dir(obj,obj,t1.u))); break;
    case OP_MOVEPLUS_C: NoIgnore(); StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else {objects[i]->inertia+=o->strength;Push(NVALUE(move_dir(obj,i,t1.u)));} break;
    case OP_MOVEPLUS_D: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->inertia+=o->strength; move_dir(obj,obj,t1.u); break;
    case OP_MOVEPLUS_CD: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) { objects[i]->inertia+=o->strength; move_dir(obj,i,t1.u); } break;
    case OP_MOVETO: NoIgnore(); StackReq(2,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); Push(NVALUE(move_to(obj,obj,t2.u,t3.u))); break;
    case OP_MOVETO_C: NoIgnore(); StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); i=v_object(Pop()); Push(NVALUE(move_to(obj,i,t2.u,t3.u))); break;
    case OP_MOVETO_D: NoIgnore(); StackReq(2,0); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); move_to(obj,obj,t2.u,t3.u); break;
    case OP_MOVETO_CD: NoIgnore(); StackReq(3,0); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); i=v_object(Pop()); move_to(obj,i,t2.u,t3.u); break;
    case OP_MOVING: StackReq(0,1); if(o->oflags&OF_MOVING) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_MOVING_C: StackReq(1,1); GetFlagOf(OF_MOVING); break;
    case OP_MOVING_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_MOVING; else o->oflags&=~OF_MOVING; break;
    case OP_MOVING_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_MOVING); break;
    case OP_MSG: StackReq(0,1); Push(MVALUE(msgvars.msg)); break;
    case OP_MUL: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u*t2.u)); break;
    case OP_MUL_C: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.s*t2.s)); break;
    case OP_NE: StackReq(2,1); t2=Pop(); t1=Pop(); Push(NVALUE(v_equal(t1,t2)?0:1)); break;
    case OP_NEG: StackReq(1,1); t1=Pop(); Numeric(t1); t1.s=-t1.s; Push(t1); break;
    case OP_NEWX: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(new_x(t1.u,t2.u))); break;
    case OP_NEWXY: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(new_x(t1.u,t3.u))); Push(NVALUE(new_y(t2.u,t3.u))); break;
    case OP_NEWY: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(new_y(t1.u,t2.u))); break;
    case OP_NEXT: StackReq(0,1); ptr=v_next(code,ptr); break;
    case OP_NIN: StackReq(2,1); i=v_in(); Push(NVALUE(i?0:1)); break;
    case OP_NIP: StackReq(2,1); t1=Pop(); Pop(); Push(t1); break;
    case OP_OBJABOVE: StackReq(0,1); i=obj_above(obj); Push(OVALUE(i)); break;
    case OP_OBJABOVE_C: StackReq(1,1); i=obj_above(v_object(Pop())); Push(OVALUE(i)); break;
    case OP_OBJBELOW: StackReq(0,1); i=obj_below(obj); Push(OVALUE(i)); break;
    case OP_OBJBELOW_C: StackReq(1,1); i=obj_below(v_object(Pop())); Push(OVALUE(i)); break;
    case OP_OBJBOTTOMAT: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); i=obj_bottom_at(t1.u,t2.u); Push(OVALUE(i)); break;
    case OP_OBJCLASSAT: StackReq(3,1); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_obj_class_at(t1,t2,t3)); break;
    case OP_OBJDIR: StackReq(1,1); t2=Pop(); Numeric(t2); i=obj_dir(obj,t2.u); Push(OVALUE(i)); break;
    case OP_OBJDIR_C: StackReq(2,1); t2=Pop(); Numeric(t2); i=obj_dir(v_object(Pop()),t2.u); Push(OVALUE(i)); break;
    case OP_OBJLAYERAT: StackReq(3,1); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); i=obj_layer_at(t1.u,t2.u,t3.u); Push(OVALUE(i)); break;
    case OP_OBJMOVINGTO: StackReq(2,0); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); v_obj_moving_to(t1.u,t2.u); break;
    case OP_OBJTOPAT: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); i=obj_top_at(t1.u,t2.u); Push(OVALUE(i)); break;
    case OP_OR: StackReq(1,0); t1=Pop(); if(v_bool(t1)) { Push(t1); ptr=code[ptr]; } else { ptr++; } break;
    case OP_OVER: StackReq(2,3); t2=Pop(); t1=Pop(); Push(t1); Push(t2); Push(t1); break;
    case OP_PATTERN: StackReq(0,1); i=code[ptr++]; j=v_pattern(code,ptr,obj,0); ptr=i; Push(OVALUE(j)); break;
    case OP_PATTERN_C: StackReq(1,1); i=code[ptr++]; j=v_object(Pop()); if(j!=VOIDLINK) j=v_pattern(code,ptr,j,0); ptr=i; Push(OVALUE(j)); break;
    case OP_PATTERN_E: StackReq(0,1); i=code[ptr++]; j=v_pattern_anywhere(code,ptr,0); ptr=i; Push(OVALUE(j)); break;
    case OP_PATTERNS: StackReq(0,1); i=code[ptr++]; v_pattern(code,ptr,obj,1); ptr=i; break;
    case OP_PATTERNS_C: StackReq(1,1); i=code[ptr++]; j=v_object(Pop()); if(j!=VOIDLINK) v_pattern(code,ptr,j,1); ptr=i; break;
    case OP_PATTERNS_E: StackReq(0,1); i=code[ptr++]; v_pattern_anywhere(code,ptr,1); ptr=i; break;
    case OP_PICK: StackReq(0,1); t1=Pop(); Numeric(t1); if(t1.u>=vstackptr) Throw("Stack index out of range"); t1=vstack[vstackptr-t1.u-1]; Push(t1); break;
    case OP_PLAYER: StackReq(0,1); if(classes[o->class]->cflags&CF_PLAYER) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_PLAYER_C: StackReq(1,1); GetClassFlagOf(CF_PLAYER); break;
    case OP_PLUSMOVE: StackReq(1,1); t1=Pop(); Numeric(t1); i=defer_move(obj,t1.u,1); Push(NVALUE(i)); break;
    case OP_PLUSMOVE_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); i=defer_move(i,t1.u,1); Push(NVALUE(i)); break;
    case OP_PLUSMOVE_D: StackReq(1,0); t1=Pop(); Numeric(t1); defer_move(obj,t1.u,1); break;
    case OP_PLUSMOVE_CD: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); defer_move(i,t1.u,1); break;
    case OP_POPUP: StackReq(1,0); v_set_popup(obj,0); break;
    case OP_POPUPARGS: i=code[ptr++]; StackReq(i+1,0); v_set_popup(obj,i); break;
    case OP_QA: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_ARRAY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QC: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_CLASS) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QCZ: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_CLASS || (t1.t==TY_NUMBER && !t1.u)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QM: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_MESSAGE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QN: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_NUMBER) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QO: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t>TY_MAXTYPE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QOZ: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t>TY_MAXTYPE || (t1.t==TY_NUMBER && !t1.u)) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QS: StackReq(1,1); t1=Pop(); NotSound(t1); if(t1.t==TY_STRING || t1.t==TY_LEVELSTRING) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_QUEEN: StackReq(0,1); Numeric(msgvars.arg1); i="\x06\x01\x07\x05\x03\x04\x02\x00"[msgvars.arg1.u&7]; Push(NVALUE(i)); break;
    case OP_REL: StackReq(1,1); t1=Pop(); Numeric(t1); i=resolve_dir(obj,t1.u); Push(NVALUE(i)); break;
    case OP_REL_C: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); i=v_object(t1); i=(i==VOIDLINK?t2.u:resolve_dir(i,t2.u)); Push(NVALUE(i)); break;
    case OP_RET: return;
    case OP_ROT: StackReq(3,3); t3=Pop(); t2=Pop(); t1=Pop(); Push(t2); Push(t3); Push(t1); break;
    case OP_ROTBACK: StackReq(3,3); t3=Pop(); t2=Pop(); t1=Pop(); Push(t3); Push(t1); Push(t2); break;
    case OP_RSH: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t2.u&~31?0:t1.u>>t2.u)); break;
    case OP_RSH_C: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t2.u&~31?(t1.s<0?-1:0):t1.s>>t2.u)); break;
    case OP_SEEK: StackReq(1,1); t1=Pop(); i=obj_seek(obj,v_object(t1)); Push(NVALUE(i)); break;
    case OP_SEEK_C: StackReq(2,1); t2=Pop(); t1=Pop(); i=obj_seek(v_object(t1),v_object(t2)); Push(NVALUE(i)); break;
    case OP_SELF: StackReq(0,1); Push(OVALUE(obj)); break;
    case OP_SEND: StackReq(3,1); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_send_self(obj,t2,t3,t4,NVALUE(0))); break;
    case OP_SEND_C: StackReq(4,1); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_send_message(obj,t1,t2,t3,t4,NVALUE(0))); break;
    case OP_SEND_D: StackReq(3,0); t4=Pop(); t3=Pop(); t2=Pop(); v_send_self(obj,t2,t3,t4,NVALUE(0)); break;
    case OP_SEND_CD: StackReq(4,0); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_send_message(obj,t1,t2,t3,t4,NVALUE(0)); break;
    case OP_SENDEX: StackReq(4,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); Push(v_send_self(obj,t2,t3,t4,t5)); break;
    case OP_SENDEX_C: StackReq(5,1); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); Push(v_send_message(obj,t1,t2,t3,t4,t5)); break;
    case OP_SENDEX_D: StackReq(4,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); v_send_self(obj,t2,t3,t4,t5); break;
    case OP_SENDEX_CD: StackReq(5,0); t5=Pop(); t4=Pop(); t3=Pop(); t2=Pop(); t1=Pop(); v_send_message(obj,t1,t2,t3,t4,t5); break;
    case OP_SETARRAY: NoIgnore(); StackReq(4,0); t4=Pop(); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); v_set_array(t1,t2.u,t3.u,t4); break;
    case OP_SETARRAY_C: NoIgnore(); StackReq(4,0); t4=Pop(); t3=Pop(); Numeric(t3); t2=Pop(); Numeric(t2); t1=Pop(); v_set_array(t1,t2.u-1,t3.u-1,t4); break;
    case OP_SETINVENTORY: StackReq(3,0); t3=Pop(); t2=Pop(); t1=Pop(); v_set_inventory(t1,t2,t3); break;
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
    case OP_STEALTHY_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_STEALTHY; else o->oflags&=~OF_STEALTHY; break;
    case OP_STEALTHY_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_STEALTHY); break;
    case OP_STRENGTH: StackReq(0,1); Push(NVALUE(o->strength)); break;
    case OP_STRENGTH_C: StackReq(1,1); Push(GetVariableOrAttributeOf(strength,NVALUE)); break;
    case OP_STRENGTH_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->strength=t1.u; break;
    case OP_STRENGTH_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->strength=t1.u&0xFFFF; break;
    case OP_STRENGTH_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->strength=t1.u; break;
    case OP_STRENGTH_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->strength=t1.u&0xFFFF; break;
    case OP_STRING: StackReq(0,1); Push(UVALUE(code[ptr++],TY_STRING)); break;
    case OP_SOUND: StackReq(2,0); t2=Pop(); t1=Pop(); set_sound_effect(t1,t2); break;
    case OP_SUB: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(t1.u-t2.u)); break;
    case OP_SUPER: i=code[1]; code=classes[i]->codes; ptr=get_message_ptr(i,msgvars.msg); if(ptr==0xFFFF) break; break;
    case OP_SUPER_C: i=code[1]; j=get_message_ptr(i,msgvars.msg); if(j!=0xFFFF) execute_program(classes[i]->codes,j,obj); break;
    case OP_SWAP: StackReq(2,2); t1=Pop(); t2=Pop(); Push(t1); Push(t2); break;
    case OP_SWAPWORLD: NoIgnore(); swap_world(); break;
    case OP_SWEEP: StackReq(8,0); v_sweep(obj,NVALUE(0)); break;
    case OP_SWEEPEX: StackReq(9,0); t1=Pop(); v_sweep(obj,t1); break;
    case OP_SYNCHRONIZE: StackReq(2,0); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); animate_sync(obj,t1.u,t2.u); break;
    case OP_TARGET: StackReq(0,1); Push(NVALUE(v_target(obj)?1:0)); break;
    case OP_TARGET_C: StackReq(1,1); i=v_object(Pop()); Push(NVALUE(v_target(i)?1:0)); break;
    case OP_TEMPERATURE: StackReq(0,1); Push(NVALUE(o->temperature)); break;
    case OP_TEMPERATURE_C: StackReq(1,1); Push(GetVariableOrAttributeOf(temperature,NVALUE)); break;
    case OP_TEMPERATURE_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->temperature=t1.u; break;
    case OP_TEMPERATURE_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->temperature=t1.u&0xFFFF; break;
    case OP_TEMPERATURE_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->temperature=t1.u; break;
    case OP_TEMPERATURE_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->temperature=t1.u&0xFFFF; break;
    case OP_TMARK: StackReq(1,2); t1=Pop(); if(t1.t==TY_MARK) { Push(NVALUE(0)); } else { Push(t1); Push(NVALUE(1)); } break;
    case OP_TRACE: StackReq(3,0); trace_stack(obj); break;
    case OP_TRACESTACK: trace_stack_list(vstackptr); break;
    case OP_TRACESTACK_C: StackReq(1,0); t1=Pop(); Numeric(t1); trace_stack_list(t1.u); break;
    case OP_TRIGGER: NoIgnore(); StackReq(2,0); t1=Pop(); i=v_object(Pop()); if(i!=VOIDLINK) v_trigger(obj,i,t1); break;
    case OP_TRIGGERAT: NoIgnore(); StackReq(3,0); t3=Pop(); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); v_trigger_at(obj,t1.u,t2.u,t3); break;
    case OP_TUCK: StackReq(2,3); t2=Pop(); t1=Pop(); Push(t2); Push(t1); Push(t2); break;
    case OP_UNIQ: StackReq(2,3); t1=Pop(); i=v_uniq(t1); if(i) Push(t1); Push(NVALUE(i)); break;
    case OP_USERSIGNAL: StackReq(0,1); if(o->oflags&OF_USERSIGNAL) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_USERSIGNAL_C: StackReq(1,1); GetFlagOf(OF_USERSIGNAL); break;
    case OP_USERSIGNAL_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_USERSIGNAL; else o->oflags&=~OF_USERSIGNAL; break;
    case OP_USERSIGNAL_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_USERSIGNAL); break;
    case OP_USERSTATE: StackReq(0,1); if(o->oflags&OF_USERSTATE) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_USERSTATE_C: StackReq(1,1); GetFlagOf(OF_USERSTATE); break;
    case OP_USERSTATE_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_USERSTATE; else o->oflags&=~OF_USERSTATE; break;
    case OP_USERSTATE_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_USERSTATE); break;
    case OP_VISUALONLY: StackReq(0,1); if(o->oflags&OF_VISUALONLY) Push(NVALUE(1)); else Push(NVALUE(0)); break;
    case OP_VISUALONLY_C: StackReq(1,1); GetFlagOf(OF_VISUALONLY); break;
    case OP_VISUALONLY_E: NoIgnore(); StackReq(1,0); if(v_bool(Pop())) o->oflags|=OF_VISUALONLY; else o->oflags&=~OF_VISUALONLY; break;
    case OP_VISUALONLY_EC: NoIgnore(); StackReq(2,0); SetFlagOf(OF_VISUALONLY); break;
    case OP_VOLUME: StackReq(0,1); Push(NVALUE(o->volume)); break;
    case OP_VOLUME_C: StackReq(1,1); Push(GetVariableOrAttributeOf(volume,NVALUE)); break;
    case OP_VOLUME_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->volume=t1.u; break;
    case OP_VOLUME_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->volume=t1.u&0xFFFF; break;
    case OP_VOLUME_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->volume=t1.u; break;
    case OP_VOLUME_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->volume=t1.u&0xFFFF; break;
    case OP_VOLUMEAT: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); Push(NVALUE(volume_at(t1.u,t2.u))); break;
    case OP_WALKABLE: StackReq(2,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); i=v_walkable(obj,t1.u,t2.u); Push(NVALUE(i)); break;
    case OP_WALKABLE_C: StackReq(3,1); t2=Pop(); Numeric(t2); t1=Pop(); Numeric(t1); t3=Pop(); i=(t3.t==TY_CLASS?v_walkable_c(t3.u,t1.u,t2.u):v_walkable(v_object(t3),t1.u,t2.u)); Push(NVALUE(i)); break;
    case OP_WEIGHT: StackReq(0,1); Push(NVALUE(o->weight)); break;
    case OP_WEIGHT_C: StackReq(1,1); Push(GetVariableOrAttributeOf(weight,NVALUE)); break;
    case OP_WEIGHT_E: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->weight=t1.u; break;
    case OP_WEIGHT_E16: NoIgnore(); StackReq(1,0); t1=Pop(); Numeric(t1); o->weight=t1.u&0xFFFF; break;
    case OP_WEIGHT_EC: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->weight=t1.u; break;
    case OP_WEIGHT_EC16: NoIgnore(); StackReq(2,0); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i!=VOIDLINK) objects[i]->weight=t1.u&0xFFFF; break;
    case OP_WINLEVEL: key_ignored=0; gameover=1; gameover_score=NO_SCORE; Throw(0); break;
    case OP_WINLEVEL_C: StackReq(1,0); t1=Pop(); Numeric(t1); key_ignored=0; gameover=1; gameover_score=t1.s; Throw(0); break;
    case OP_XDIR: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(o->x+x_delta[resolve_dir(obj,t1.u)])); break;
    case OP_XDIR_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(objects[i]->x+x_delta[resolve_dir(i,t1.u)])); break;
    case OP_XLOC: StackReq(0,1); Push(NVALUE(o->x)); break;
    case OP_XLOC_C: StackReq(1,1); Push(GetVariableOf(x,NVALUE)); break;
    case OP_XSTEP: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(x_delta[resolve_dir(obj,t1.u)])); break;
    case OP_XSTEP_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(x_delta[resolve_dir(i,t1.u)])); break;
    case OP_XYDIR: StackReq(1,2); t1=Pop(); Numeric(t1); i=resolve_dir(obj,t1.u); Push(NVALUE(o->x+x_delta[i])); Push(NVALUE(o->y+y_delta[i])); break;
    case OP_YDIR: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(o->y+y_delta[resolve_dir(obj,t1.u)])); break;
    case OP_YDIR_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(objects[i]->y+y_delta[resolve_dir(i,t1.u)])); break;
    case OP_YLOC: StackReq(0,1); Push(NVALUE(o->y)); break;
    case OP_YLOC_C: StackReq(1,1); Push(GetVariableOf(y,NVALUE)); break;
    case OP_YSTEP: StackReq(1,1); t1=Pop(); Numeric(t1); Push(NVALUE(y_delta[resolve_dir(obj,t1.u)])); break;
    case OP_YSTEP_C: StackReq(2,1); t1=Pop(); Numeric(t1); i=v_object(Pop()); if(i==VOIDLINK) Push(NVALUE(0)); else Push(NVALUE(y_delta[resolve_dir(i,t1.u)])); break;
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
  printf("%s%d : %s%s : %d : %u %u",traceprefix,move_number,msgvars.msg<256?"":"#",s,vstackptr,o?o->generation:0,msgvars.from);
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

static Uint32 broadcast(Uint32 from,int c,Uint16 msg,Value arg1,Value arg2,Value arg3,Uint8 s) {
  Uint32 t=0;
  Uint32 n;
  Object*o;
  Value v;
  if(s==2) t=1;
  if(lastobj==VOIDLINK) return t;
  n=lastobj;
  if(c && (classes[c]->cflags&CF_GROUP)) Throw("Broadcast for abstract classes is not implemented yet");
  while(o=objects[n]) {
    if(!c || o->class==c) {
      v=send_message(from,n,msg,arg1,arg2,arg3);
      switch(s) {
        case 0:
          t++;
          break;
        case 1:
          switch(v.t) {
            case TY_NUMBER: t+=v.u; break;
            case TY_CLASS: t++; break;
            default:
              if(v.t<=TY_MAXTYPE) Throw("Invalid return type for BroadcastSum");
              t++;
          }
          break;
        case 2:
          switch(v.t) {
            case TY_NUMBER: if(!v.u) return 0; break;
            case TY_SOUND: case TY_USOUND: Throw("Invalid return type for BroadcastAnd");
          }
          break;
        case 3:
          StackReq(0,1);
          if(v.t!=TY_MARK) Push(v);
          break;
      }
    }
    n=o->prev;
    if(n==VOIDLINK) break;
  }
  return t;
}

void annihilate(void) {
  Uint32 i;
  for(i=0;i<64*64;i++) playfield[i]=bizplayfield[i]=VOIDLINK;
  firstobj=lastobj=VOIDLINK;
  if(quiz_text) {
    sqlite3_free(quiz_text);
    quiz_text=0;
  }
  if(!objects) return;
  for(i=0;i<nobjects;i++) if(objects[i]) {
    animfree(objects[i]->anim);
    free(objects[i]);
  }
  nobjects=0;
  free(objects);
  objects=0;
  gameover=0;
  clear_inventory();
  for(i=0;i<nlevelstrings;i++) free(levelstrings[i]);
  nlevelstrings=0;
  free(deadanim);
  deadanim=0;
  ndeadanim=0;
  control_obj=VOIDLINK;
  nconn=pconn=0;
}

static inline int try_sharp(Uint32 n1,Uint32 n2) {
  Object*o=objects[n1];
  Object*p=objects[n2];
  if((o->oflags|p->oflags)&OF_DESTROYED) return 0;
  if(o->dir&1) return 0;
  if(p->x!=o->x+x_delta[o->dir] || p->y!=o->y+y_delta[o->dir]) return 0;
  if(p->hard[(o->dir^4)>>1]<=o->sharp[o->dir>>1]) return 0;
  return !v_bool(destroy(n1,n2,o->oflags&OF_MOVING?2:1));
}

static Uint32 handle_colliding(Uint32 n1,Uint32 n2,Uint8 r1,Uint8 r2) {
  // n1 = the object trying to move
  // n2 = the object it is colliding with (which might also be trying to move, or might not)
  Value v;
  Uint32 h;
  v=send_message(n1,n2,MSG_COLLIDING,NVALUE(objects[n1]->x),NVALUE(objects[n1]->y),NVALUE(r1));
  if(v.t) Throw("Type mismatch");
  h=(v.u>>16)|(v.u<<16);
  v=send_message(n2,n1,MSG_COLLIDING,NVALUE(objects[n2]->x),NVALUE(objects[n2]->y),NVALUE(r2));
  if(v.t) Throw("Type mismatch");
  h|=v.u;
  if((h&0x0002) && try_sharp(n1,n2)) h|=0x0004;
  if((h&0x00020000) && try_sharp(n2,n1)) h|=0x00040000;
  return h;
}

static inline Uint8 find_deferred_movement_loop(Uint32 obj,Uint8 xx,Uint8 yy) {
  // At this point, it is necessary to check for a loop, which may be like the diagram below:
  //     >>>>>>>v
  //        ^<<<<
  // The loop may also have a different shape.
  // If a loop is found, call the handle_colliding function to determine what to do next.
  // If there is no loop, allow the move to be retried later, trying objE's location next.
  // Since there may be multiple objects in a location, which move in different directions,
  // a variable "ch" mentions the next object to try, if it is VOIDLINK. If not, then once
  // it reaches the end without finding a loop, it can try the next one, resetting ch to
  // VOIDLINK. This will find not only loops, but also if there is a branch that is later
  // merged; due to what this program needs to do, and how they will be dealt with after
  // the potential loop is found, that isn't a problem. This algorithm also will not find
  // some loops with multiple branches, but that is also OK, since the main handling of
  // deferred movements will eventually try every object as an origin, and this will cause
  // the loop to eventually be found.
  static sqlite3_uint64 board[64]; // bit board for locations with possible collisions
  Object*o=objects[obj];
  Uint32 ch=VOIDLINK; // first object found in same place as other where it must be checked
  Uint8 x=o->x;
  Uint8 y=o->y;
  Uint32 n,nn;
  memset(board,0,sizeof(board));
  begin:
  if(x==xx && y==yy) return 1;
  if(x<1 || x>pfwidth || y<1 || y>pfheight) goto notfound;
  if(board[y-1]&(1ULL<<(x-1))) return 0;
  board[y-1]|=1ULL<<(x-1);
  n=playfield[x+y*64-65];
  nn=VOIDLINK;
  while(n!=VOIDLINK) {
    o=objects[n];
    if((o->oflags&(OF_MOVING|OF_VISUALONLY|OF_DESTROYED))==OF_MOVING && o->height>0) {
      if(nn==VOIDLINK) {
        nn=n;
        if(ch!=VOIDLINK) goto found;
      } else if(ch==VOIDLINK) {
        ch=n;
        goto found;
      }
    }
    up:
    n=o->up;
  }
  if(nn==VOIDLINK) {
    notfound:
    if(ch==VOIDLINK) return 0;
    o=objects[n=ch];
    x=o->x;
    y=o->y;
    ch=VOIDLINK;
    goto up;
  }
  found:
  o=objects[nn];
  x+=x_delta[o->dir];
  y+=y_delta[o->dir];
  goto begin;
}

static Uint32 deferred_colliding(Uint32 obj,int x,int y) {
  Object*o=objects[obj];
  Object*oE;
  Uint32 h=0xFFFFFFFF;
  Uint32 objE=playfield[x+y*64-65];
  Uint8 d;
  int xx,yy;
  while(objE!=VOIDLINK) {
    oE=objects[objE];
    if(!(oE->oflags&(OF_DESTROYED|OF_VISUALONLY))) {
      if((oE->oflags&OF_MOVING) && oE->height>0) {
        if(find_deferred_movement_loop(objE,objects[obj]->x,objects[obj]->y)) {
          h&=handle_colliding(obj,objE,3,3);
        } else {
          return h&4;
        }
      } else if(oE->height>o->climb || (classes[o->class]->collisionLayers&classes[oE->class]->collisionLayers)) {
        h&=handle_colliding(obj,objE,1,0);
      }
    }
    objE=oE->up;
  }
  // Find other objects trying to move to the same place
  if(h&8) for(d=0;d<8;d++) {
    xx=x-x_delta[d];
    yy=y-y_delta[d];
    if(xx<1 || xx>pfwidth || yy<1 || yy>pfheight) continue;
    objE=playfield[xx+yy*64-65];
    while(objE!=VOIDLINK) {
      oE=objects[objE];
      if(obj!=objE && (oE->oflags&OF_MOVING) && oE->dir==d && !(oE->oflags&OF_DESTROYED)) {
        if(o->height>oE->climb || oE->height>o->climb || (classes[o->class]->collisionLayers&classes[oE->class]->collisionLayers)) {
          h&=handle_colliding(obj,objE,2,2);
        }
      }
      objE=oE->up;
    }
  }
  return h;
}

static void do_deferred_moves(void) {
  Object*o;
  Object*p;
  Uint32 h,n;
  int i,x,y;
  Uint8 re;
  restart:
  re=0;
  for(i=0;i<64*pfheight;i++) {
    retry:
    n=playfield[i];
    while(n!=VOIDLINK) {
      o=objects[n];
      if((o->oflags&(OF_MOVING|OF_DESTROYED))==OF_MOVING) {
        x=o->x+x_delta[o->dir];
        y=o->y+y_delta[o->dir];
        if(x<1 || x>pfwidth || y<1 || y>pfheight || playfield[x+y*64-65]==VOIDLINK) goto stop;
        h=deferred_colliding(n,x,y);
        if((h&1) && (o->oflags&OF_MOVING) && move_to(VOIDLINK,n,x,y)) {
          re=1;
          o->oflags=(o->oflags|OF_MOVED)&~OF_MOVING;
          i-=65;
          if(i<0) i=0;
          goto retry;
        }
        if((h&5)!=4) stop: o->oflags&=~OF_MOVING;
      }
      skip:
      n=o->up;
    }
  }
  if(re) goto restart;
}

static void execute_animation(Uint32 obj) {
  Object*o=objects[obj];
  Animation*a=o->anim;
  if(!(a->step[a->lstep].flag&ANI_ONCE)) return;
  if(a->ltime>=a->step[a->lstep].speed) {
    a->ltime=0;
    if(o->image==a->step[a->lstep].end) {
      a->status&=~ANISTAT_LOGICAL;
      if(classes[o->class]->cflags&CF_COMPATIBLE) lastimage_processing=1;
      send_message(VOIDLINK,obj,MSG_LASTIMAGE,NVALUE(0),NVALUE(0),NVALUE(0));
      lastimage_processing=0;
    } else {
      if(a->step[a->lstep].start>a->step[a->lstep].end) --o->image; else ++o->image;
    }
  }
}

const char*execute_turn(int key) {
  Uint8 busy,clock,x,y;
  Uint32 m,n,turn,tc;
  Object*o;
  Value v;
  int i;
  tc=0;
  if(!key) {
    // This is part of initialization; if anything triggered, it must be executed now
    all_flushed=1;
    goto trig;
  }
  if(setjmp(my_env)) return my_error;
  if(quiz_text) {
    if(key>256 && !key_ignored) return 0;
    sqlite3_free(quiz_text);
    quiz_text=0;
    if(key_ignored) {
      if(quiz_obj.t) quiz_obj=NVALUE(0); else return 0;
    } else if(!quiz_obj.t) {
      move_number++;
      return 0;
    }
  }
  changed=0;
  key_ignored=0;
  all_flushed=0;
  lastimage_processing=0;
  vstackptr=0;
  for(n=0;n<nobjects;n++) if(o=objects[n]) {
    o->distance=0;
    o->oflags&=~(OF_KEYCLEARED|OF_DONE|OF_MOVING);
    if(o->anim) {
      o->anim->count=0;
      if(o->anim->status==ANISTAT_VISUAL) o->anim->status=0;
    }
  }
  // Input phase
  m=VOIDLINK;
  v=NVALUE(0);
  if(key>=8 && key<256) {
    current_key=key;
    if(!quiz_obj.t) {
      n=lastobj;
      while(n!=VOIDLINK) {
        i=classes[objects[n]->class]->cflags;
        if(i&CF_INPUT) v=send_message(VOIDLINK,n,MSG_KEY,NVALUE(key),v,NVALUE(0));
        if(i&CF_PLAYER) m=n;
        n=objects[n]->prev;
      }
    } else {
      n=quiz_obj.u;
      if(objects[n]->generation!=quiz_obj.t) n=VOIDLINK;
      quiz_obj=NVALUE(0);
      if(classes[objects[n]->class]->cflags&CF_COMPATIBLE) all_flushed=1;
      v=send_message(VOIDLINK,n,MSG_KEY,NVALUE(key),NVALUE(0),NVALUE(1));
    }
  } else if(has_xy_input && key>=0x8000 && key<=0x8FFF) {
    x=((key>>6)&63)+1; y=(key&63)+1;
    if(x>pfwidth || y>pfheight) return "Illegal move";
    current_key=KEY_XY;
    if(control_obj!=VOIDLINK) {
      quiz_obj=NVALUE(0);
      v=send_message(VOIDLINK,control_obj,MSG_CLICK,NVALUE(x),NVALUE(y),NVALUE(0));
      if(key_ignored) goto ignored;
    }
    m=n=playfield[x+y*64-65];
    while(m!=VOIDLINK) {
      m=objects[m]->up;
      if(m!=VOIDLINK) n=m;
    }
    while(n!=VOIDLINK) {
      if(objects[n]->x!=x || objects[n]->y!=y) break;
      if(v_bool(send_message(VOIDLINK,n,MSG_CLICK,NVALUE(x),NVALUE(y),v))) break;
      n=objects[n]->down;
    }
  } else {
    return "Illegal move";
  }
  current_key=0;
  if(key_ignored) {
    ignored:
    quiz_obj=NVALUE(0);
    return changed?"Invalid use of IgnoreKey":0;
  }
  move_number++;
  // Beginning phase
  if(!all_flushed) {
    if(m==VOIDLINK) x=0,y=0; else x=objects[m]->x,y=objects[m]->y;
    broadcast(m,0,MSG_BEGIN_TURN,NVALUE(x),NVALUE(y),v,0);
  }
  turn=0;
  // Trigger phase
  trig:
  if(max_trigger) {
    if(tc++>=max_trigger) return "Turn looped too many times";
  }
  busy=0;
  clock=255;
  for(i=0;i<64*pfheight;i++) {
    n=playfield[i];
    while(n!=VOIDLINK) {
      o=objects[n];
      m=o->up;
      if(o->oflags&OF_DESTROYED) {
        objtrash(n);
      } else if(classes[o->class]->cflags&CF_COMPATIBLE) {
        if((o->oflags&(OF_MOVED|OF_MOVED2))==OF_MOVED) {
          o->oflags=(o->oflags|OF_MOVED2)&~OF_MOVED;
          send_message(VOIDLINK,n,MSG_MOVED,NVALUE(0),NVALUE(0),NVALUE(turn));
          busy=1;
        }
        if(o->departed && !o->departed2) {
          o->departed2=1;
          send_message(VOIDLINK,n,MSG_DEPARTED,NVALUE(0),NVALUE(0),NVALUE(turn));
          o->departed=0;
          busy=1;
        }
        if(o->arrived && !o->arrived2) {
          o->arrived2=1;
          send_message(VOIDLINK,n,MSG_ARRIVED,NVALUE(0),NVALUE(0),NVALUE(turn));
          o->arrived=0;
          busy=1;
        }
        if(o->anim && (o->anim->status&ANISTAT_LOGICAL)) execute_animation(n);
        if(o->oflags&(OF_BUSY|OF_USERSIGNAL)) busy=1;
      } else {
        if(o->oflags&OF_MOVING) {
          do_deferred_moves();
          goto trig;
        }
        o->departed2|=o->departed;
        o->arrived2|=o->arrived;
        o->departed=o->arrived=0;
        if(o->oflags&OF_MOVED) o->oflags=(o->oflags|OF_MOVED2)&~OF_MOVED;
      }
      n=m;
    }
  }
  n=lastobj;
  while(n!=VOIDLINK) {
    o=objects[n];
    if(!(classes[o->class]->cflags&CF_COMPATIBLE)) {
      if(o->oflags&OF_MOVED2) send_message(VOIDLINK,n,MSG_MOVED,NVALUE(0),NVALUE(0),NVALUE(turn)),busy=1;
      if(o->departed2) send_message(VOIDLINK,n,MSG_DEPARTED,NVALUE(o->departed2),NVALUE(0),NVALUE(turn)),busy=1;
      if(o->arrived2) send_message(VOIDLINK,n,MSG_ARRIVED,NVALUE(o->arrived2),NVALUE(0),NVALUE(turn)),busy=1;
      if(o->anim && (o->anim->status&ANISTAT_LOGICAL)) execute_animation(n);
      if(o->oflags&(OF_BUSY|OF_USERSIGNAL)) busy=1;
    }
    o->arrived2=o->departed2=0;
    o->oflags&=~OF_MOVED2;
    n=o->prev;
  }
  // Ending phase
  if(!busy && !all_flushed) {
    n=lastobj;
    while(n!=VOIDLINK) {
      v=send_message(VOIDLINK,n,MSG_END_TURN,NVALUE(turn),NVALUE(0),NVALUE(0));
      if(v_bool(v) || objects[n]->arrived || objects[n]->departed) busy=1;
      if(objects[n]->oflags&(OF_BUSY|OF_USERSIGNAL|OF_MOVED|OF_MOVING)) busy=1;
      n=objects[n]->prev;
    }
    turn++;
    if(!busy) all_flushed=1;
  }
  // Clock phase
  n=lastobj;
  while(n!=VOIDLINK) {
    o=objects[n];
    if(o->oflags&OF_BIZARRO) {
      o->arrived=o->departed=0;
      o->oflags&=~(OF_MOVING|OF_MOVED);
    } else {
      if(o->arrived || o->departed) goto trig;
      if(o->oflags&OF_MOVED) goto trig;
      if(o->anim && (o->anim->status&ANISTAT_LOGICAL) && (o->anim->step[o->anim->lstep].flag&ANI_ONCE)) {
        if(o->anim->ltime>=o->anim->step[o->anim->lstep].speed) goto trig;
        if(clock>o->anim->step[o->anim->lstep].speed-o->anim->ltime) clock=o->anim->step[o->anim->lstep].speed-o->anim->ltime;
      }
    }
    n=o->prev;
  }
  n=lastobj;
  while(n!=VOIDLINK) {
    o=objects[n];
    if(o->oflags&(OF_BUSY|OF_USERSIGNAL|OF_MOVED|OF_MOVING)) busy=1;
    if(o->arrived || o->departed) busy=1;
    if(o->anim && (o->anim->status&ANISTAT_LOGICAL) && (o->anim->step[o->anim->lstep].flag&ANI_ONCE)) {
      i=o->anim->ltime+clock;
      o->anim->ltime=i>255?255:i;
      busy=1;
    }
    n=o->prev;
  }
  if(busy) goto trig;
  // Cleanup phase
  for(n=0;n<nobjects;n++) if(objects[n] && (objects[n]->oflags&OF_DESTROYED)) objtrash(n);
  if(generation_number<=TY_MAXTYPE) return "Too many generations of objects";
  if(firstobj==VOIDLINK) return "Game cannot continue with no objects";
  // Finished
  return 0;
}

const char*init_level(void) {
  Uint32 n,m;
  if(setjmp(my_env)) return my_error;
  clear_inventory();
  if(main_options['t']) {
    printf("[Level %d restarted]\n",level_id);
    if(!traced_obj.t) {
      const char*s;
      optionquery[1]=Q_traceObject;
      if(s=xrm_get_resource(resourcedb,optionquery,optionquery,2)) {
        traced_obj.u=strtol(s,(char**)&s,10);
        if(*s==':') traced_obj.t=strtol(s+1,0,10);
        printf("Tracing object <%ld:%ld>\n",(long)traced_obj.u,(long)traced_obj.t);
      } else {
        traced_obj.t=TY_FOR;
      }
    }
  }
  if(array_size) memset(array_data,0,array_size*sizeof(Value));
  memcpy(globals,initglobals,sizeof(globals));
  quiz_obj=NVALUE(0);
  gameover=0;
  changed=0;
  key_ignored=0;
  all_flushed=0;
  lastimage_processing=0;
  vstackptr=0;
  move_number=0;
  current_key=0;
  if(control_class) {
    control_obj=objalloc(control_class);
    if(control_obj==VOIDLINK) Throw("Error creating object");
  }
  n=lastobj;
  while(n!=VOIDLINK && !(objects[n]->oflags&OF_ORDERED)) {
    send_message(VOIDLINK,n,MSG_INIT,NVALUE(0),NVALUE(0),NVALUE(0));
    m=objects[n]->prev;
    if(classes[objects[n]->class]->order && !(objects[n]->oflags&OF_DESTROYED)) set_order(n);
    n=m;
  }
  broadcast(VOIDLINK,0,MSG_POSTINIT,NVALUE(0),NVALUE(0),NVALUE(0),0);
  if(gameover) return 0;
  return execute_turn(0);
}
