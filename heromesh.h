/*
  This file is part of Free Hero Mesh and is public domain.
*/

// == main ==

#define fatal(...) do{ fprintf(stderr,__VA_ARGS__); exit(1); }while(0)
#define boolxrm(a,b) (*a=='1'||*a=='y'||*a=='t'||*a=='Y'||*a=='T'?1:*a=='0'||*a=='n'||*a=='f'||*a=='N'||*a=='F'?0:b)

#define TY_NUMBER 0
#define TY_CLASS 1
#define TY_MESSAGE 2
#define TY_SOUND 3
#define TY_USOUND 4
#define TY_STRING 5
#define TY_LEVELSTRING 6
#define TY_MAXTYPE 15

typedef struct {
  union {
    Sint32 s;
    Uint32 u;
  };
  Uint32 t; // Type 0-15, or a generation_number of an object
} Value;

extern sqlite3*userdb;
extern xrm_db*resourcedb;
extern const char*basefilename;
extern xrm_quark optionquery[16];
extern Uint32 generation_number;

void set_cursor(int id);

// == picture ==

extern SDL_Surface*screen;
extern Uint16 picture_size;

void draw_picture(int x,int y,Uint16 img);
void draw_text(int x,int y,const unsigned char*t,int bg,int fg);
void load_pictures(void);

// == class ==

#define CF_PLAYER 0x01
#define CF_INPUT 0x02
#define CF_COMPATIBLE 0x04
#define CF_QUIZ 0x08
#define CF_GROUP 0x10 // this is a group of classes; you can't create an object of this class
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

typedef struct {
  const char*name;
  const char*edithelp; // not present if CF_GROUP
  const char*gamehelp; // not present if CF_GROUP
  Uint16*codes; // if this is CF_GROUP, then instead a zero-terminated list of classes
  Uint16*messages; // use 0xFFFF if no such message block; not present if CF_GROUP
  Uint16*images; // high bit is set if available to editor; not present if CF_GROUP
  Sint32 height,weight,climb,density,volume,strength,arrivals,departures;
  Sint32 temperature,misc4,misc5,misc6,misc7;
  Uint16 uservars,oflags;
  Uint16 sharp[4];
  Uint16 hard[4];
  Uint8 cflags,shape,shovable;
} Class;

extern Class*classes[0x4000]; // 0 isn't used
extern const char*messages[0x4000]; // index is 256 less than message number
extern int max_animation; // max steps in animation queue
extern Sint32 max_volume; // max total volume to allow moving diagonally (default 10000)

