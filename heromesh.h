/*
  This file is part of Free Hero Mesh and is public domain.
*/

#include "comconfig.h"

// == main ==

#ifdef CONFIG_NO_STATUS
#define printStatus(...) do{ if(main_options['v']) fprintf(stderr,__VA_ARGS__); }while(0)
#else
#define printStatus(...) fprintf(stderr,__VA_ARGS__)
#endif

#define fatal(...) do{ fprintf(stderr,"FATAL: " __VA_ARGS__); exit(1); }while(0)
#define boolxrm(a,b) (*a=='1'||*a=='y'||*a=='t'||*a=='Y'||*a=='T'?1:*a=='0'||*a=='n'||*a=='f'||*a=='N'||*a=='F'?0:b)

#define TY_NUMBER 0
#define TY_CLASS 1
#define TY_MESSAGE 2
#define TY_LEVELSTRING 3
#define TY_STRING 4
#define TY_SOUND 5
#define TY_USOUND 6
#define TY_FOR 7
#define TY_MARK 8
#define TY_ARRAY 9
#define TY_CODE 10
#define TY_MAXTYPE 15
// The level file format requires type codes 0 to 3 to be as is; other codes may change.

typedef struct {
  union {
    Sint32 s;
    Uint32 u;
  };
  Uint32 t; // Type 0-15, or a generation_number of an object
} Value;
#define UVALUE(x,y) ((Value){.u=x,.t=y})
#define SVALUE(x,y) ((Value){.s=x,.t=y})
#define NVALUE(x) SVALUE(x,TY_NUMBER)
#define CVALUE(x) UVALUE(x,TY_CLASS)
#define MVALUE(x) UVALUE(x,TY_MESSAGE)
#define ZVALUE(x) UVALUE(x,TY_STRING)
#define OVALUE(x) ((x)==VOIDLINK?NVALUE(0):UVALUE(x,objects[x]->generation))
#define ValueTo64(v) (((sqlite3_int64)((v).u))|(((sqlite3_int64)((v).t))<<32))
#define ValueEq(x,y) ((x).t==(y).t && (x).u==(y).u)

#define N_MESSAGES 30
#define N_STANDARD_SOUNDS 49
extern const char*const standard_message_names[];
extern const char*const standard_sound_names[];
extern const char*const heromesh_key_names[256];
extern const char jispunct[];

extern sqlite3*userdb;
extern xrm_db*resourcedb;
extern const char*basefilename;
extern xrm_quark optionquery[16];
extern char main_options[128];
extern Uint8 message_trace[0x4100/8];
extern Uint16 level_id,level_ord,level_version,level_code;
extern unsigned char*level_title;
extern Uint16*level_index;
extern int level_nindex;
extern char level_changed; // 1 if solution is potentially invalidated by edits
extern FILE*levelfp;
extern FILE*solutionfp;

#ifdef CONFIG_WITH_STACK_PROTECTION
extern char stack_protect_mode;
extern void*stack_protect_mark;
extern void*stack_protect_low;
extern void*stack_protect_high;
#define StackProtection() (stack_protect_mode && ( \
  stack_protect_mode=='<' ? (__builtin_frame_address(0)<stack_protect_mark) : \
  stack_protect_mode=='>' ? (__builtin_frame_address(0)>stack_protect_mark) : \
  stack_protect_mode=='?' ? ({ \
    if(__builtin_frame_address(0)<stack_protect_low) stack_protect_low=__builtin_frame_address(0); \
    if(__builtin_frame_address(0)>stack_protect_high) stack_protect_high=__builtin_frame_address(0); \
    0; \
  }) : \
  stack_protect_mode=='!' ? 1 : \
0))
#else
#define StackProtection() 0
#endif

FILE*composite_slice(const char*suffix,char isfatal);
unsigned char*read_lump_or_userstate(int sol,int lvl,long*sz,char us);
void write_lump(int sol,int lvl,long sz,const unsigned char*data);
void write_userstate(int sol,int lvl,long sz,const unsigned char*data);
const char*load_level(int lvl);
void set_cursor(int id);
Uint32*tron8to32(const Uint8*);
Uint8*tron32to8(const Uint32*);
const char*log_if_error(const char*t);

#define FIL_SOLUTION 1
#define FIL_LEVEL 0
#define LUMP_LEVEL_IDX (-1)
#define LUMP_CLASS_DEF (-2)
#define LUMP_DIVISION_IDX (-3)

#define read_lump(a,b,c) read_lump_or_userstate(a,b,c,0)
#define read_userstate(a,b,c) read_lump_or_userstate(a,b,c,1)

// == picture ==

extern SDL_Surface*screen;
extern Uint16 picture_size;
extern int left_margin;
extern Uint32 codepage;

void init_palette(void);
void init_screen(void);
void load_pictures(void);
void set_code_page(Uint32 n);

// Use only when screen is unlocked
void draw_picture(int x,int y,Uint16 img);
void draw_cell(int x,int y);

// Use only when screen is locked
void draw_text(int x,int y,const unsigned char*t,int bg,int fg);
int draw_text_line(int x,int y,unsigned char*t,int cur,Uint8**cp);
void draw_key(int x,int y,int k,int bg,int fg);
void draw_selection_rectangle(void);

const char*screen_prompt(const char*txt);
int screen_message(const char*txt);

int scrollbar(int*cur,int page,int max,SDL_Event*ev,SDL_Rect*re);

void draw_popup(const unsigned char*txt);
int modal_draw_popup(const unsigned char*txt);

void draw_lines(int x,int y);

// == class ==

#define CF_PLAYER 0x01
#define CF_INPUT 0x02
#define CF_COMPATIBLE 0x04
#define CF_QUIZ 0x08
#define CF_GROUP 0x10 // this is a group of classes; you can't create an object of this class
#define CF_TRACEIN 0x20
#define CF_TRACEOUT 0x40 // same as CF_NOCLASS1 (no problem since CF_NOCLASS1 not used after class loading)
#define CF_NOCLASS1 0x40 // if only the name has been loaded so far, from the .class file
#define CF_NOCLASS2 0x80 // if only the name has been loaded so far, from the CLASS.DEF lump

#define OF_INVISIBLE 0x0001
#define OF_VISUALONLY 0x0002
#define OF_STEALTHY 0x0004
#define OF_BUSY 0x0008
#define OF_USERSTATE 0x0010
#define OF_USERSIGNAL 0x0020
#define OF_MOVED 0x0040
#define OF_DONE 0x0080
#define OF_KEYCLEARED 0x0100
#define OF_DESTROYED 0x0200
#define OF_BIZARRO 0x0400
#define OF_MOVED2 0x0800
#define OF_MOVING 0x1000
#define OF_ORDERED 0x2000
#define OF_CONNECTION 0x4000
#define OF_CRUSH 0x8000

typedef struct {
  const char*name;
  const char*edithelp; // not present if CF_GROUP
  const char*gamehelp; // not present if CF_GROUP
  Uint16*codes;
  Uint16*messages; // use 0xFFFF if no such message block
  Uint16*images; // high bit is set if available to editor; not present if CF_GROUP
  Sint32 height,weight,climb,density,volume,strength,arrivals,departures;
  Sint32 temperature,misc4,misc5,misc6,misc7;
  Uint16 uservars,oflags,nmsg;
  Uint16 sharp[4];
  Uint16 hard[4];
  Uint8 cflags,shape,shovable,collisionLayers,nimages,order;
} Class;

typedef struct {
  Uint8 length,speed,vtime,frame;
} AnimationSlot;

extern Value initglobals[0x800];
extern Class*classes[0x4000]; // 0 isn't a real class
extern const char*messages[0x4000]; // index is 256 less than message number
extern Uint16 functions[0x4000];
extern int max_animation; // max steps in animation queue (default 32)
extern Sint32 max_volume; // max total volume to allow moving diagonally (default 10000)
extern Uint8 back_color,inv_back_color;
extern char**stringpool;
extern AnimationSlot anim_slot[8];
extern Uint8 keymask[256/8];
extern Uint16 array_size;
extern Uint16*orders;
extern Uint8 norders;
extern Uint16 control_class;
extern Uint8 has_xy_input; // zero if not, nonzero if it has
extern Uint8 has_mbcs; // nonzero if multibyte character encoding is in use

typedef struct {
  // Flags: 1=fill-width, 2=multi-colours, 4=built-in-data
  Uint8 width,data,color,flag;
  Uint8 form[2];
  Uint16 ptr;
} DisplayColumn;

typedef struct {
  union {
    char*name; // free when loading table
    Uint8 ag; // aggregate type
  };
  Uint16 ptr;
  Uint8 sgn; // 0=unsigned, 1=signed
} DataColumn;

extern char*ll_head;
extern DisplayColumn*ll_disp;
extern Uint8 ll_ndisp;
extern DataColumn*ll_data;
extern Uint8 ll_ndata;
extern Uint8 ll_naggregate; // Aggregates will be listed in ll_data after the data columns
extern Uint16*ll_code;

Uint16 get_message_ptr(int c,int m);
void load_classes(void);

// == bindings ==

typedef struct {
  char cmd;
  union {
    int n;
    sqlite3_stmt*stmt;
    const char*txt;
  };
} UserCommand;

void load_key_bindings(void);
const UserCommand*find_key_binding(SDL_Event*ev,int editing);
int exec_key_binding(SDL_Event*ev,int editing,int x,int y,int(*cb)(int prev,int cmd,int number,int argc,sqlite3_stmt*args,void*aux),void*aux);

// == function ==

void init_sql_functions(sqlite3_int64*ptr0,sqlite3_int64*ptr1);

// == exec ==

#define VOIDLINK ((Uint32)(-1))
#define NO_SCORE ((Sint32)(0x80000000))

#define ANI_STOP 0x00
#define ANI_ONCE 0x01
#define ANI_LOOP 0x02
#define ANI_OSC 0x08
#define ANI_SYNC 0x80

// Special key codes; used in encoded move lists and in some cases also values for Key
// Only numbers 1 to 7 can be used in this way.
#define KEY_XY 1

typedef struct {
  Uint8 flag,start,end;
  union {
    Uint8 speed; // unsynchronized
    Uint8 slot; // synchronized
  };
} AnimationStep;

typedef struct {
  Uint8 x,y,vtime,vimage;
  Uint16 class,delay;
  AnimationStep s;
} DeadAnimation;

#define ANISTAT_LOGICAL 0x01
#define ANISTAT_VISUAL 0x02
#define ANISTAT_SYNCHRONIZED 0x80

typedef struct {
  Uint8 lstep,vstep,status,ltime,vtime,vimage,count;
  AnimationStep step[0];
} Animation;

typedef struct {
  Sint32 height,weight,climb,density,volume,strength,arrivals,departures,temperature,inertia;
  Uint32 arrived,departed,arrived2,departed2,generation;
  Uint32 up,down,prev,next; // links to other objects
  Uint16 class,oflags,distance,shape,shovable,image;
  Uint16 sharp[4];
  Uint16 hard[4];
  Uint8 x,y,dir;
  Animation*anim;
  Value misc1,misc2,misc3,misc4,misc5,misc6,misc7,replacement;
  Value uservars[0];
} Object;

typedef struct {
  Uint16 class,value;
  Uint8 image;
} Inventory;

extern Uint32 max_objects;
extern Uint32 generation_number;
extern Object**objects;
extern Uint32 nobjects;
extern Value globals[0x800];
extern Uint32 firstobj,lastobj;
extern Uint32 playfield[64*64];
extern Uint32 bizplayfield[64*64];
extern Uint8 pfwidth,pfheight;
extern Sint8 gameover,key_ignored;
extern Uint8 generation_number_inc;
extern Uint32 move_number;
extern unsigned char*quiz_text;
extern Inventory*inventory;
extern Uint32 ninventory;
extern unsigned char**levelstrings;
extern Uint16 nlevelstrings;
extern Value*array_data;
extern Uint16 ndeadanim;
extern DeadAnimation*deadanim;
extern Uint8 no_dead_anim;
extern Uint32 max_trigger;
extern Uint8 conn_option;
extern Sint32 gameover_score;

const unsigned char*value_string_ptr(Value v);
void pfunlink(Uint32 n);
void pflink(Uint32 n);
Uint32 objalloc(Uint16 c);
void objtrash(Uint32 n);
void annihilate(void);
const char*execute_turn(int key);
const char*init_level(void);
void swap_world(void);

// == game ==

typedef Uint16 MoveItem;

extern MoveItem*replay_list;
extern size_t replay_size;
extern Uint16 replay_count,replay_pos,replay_mark;
extern Uint8 solution_replay;
extern char*best_list;
extern Sint32 best_score;

int encode_move(FILE*fp,MoveItem v);
int encode_move_list(FILE*fp);
MoveItem decode_move(FILE*fp);
int decode_move_list(FILE*fp);

void run_game(void);
void run_auto_test(void);
void export_private_solutions(void);
void locate_me(int x,int y);

// == edit ==

typedef struct {
  Uint8 x0,y0,x1,y1;
} EditorRect;

extern EditorRect editrect;

void run_editor(void);
void write_empty_level_set(FILE*);
void batch_import(void);
void make_level_hashes(void);

// == picedit ==

void run_picture_editor(void);

// == sound ==

void init_sound(void);
void set_sound_effect(Value v1,Value v2);
Uint16 find_user_sound(const char*name);
void set_sound_on(int on);
void sound_test(void);

