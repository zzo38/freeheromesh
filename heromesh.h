/*
  This file is part of Free Hero Mesh and is public domain.
*/

// main.c

extern sqlite3*userdb;
extern xrm_db*resourcedb;
extern const char*basefilename;
extern xrm_quark optionquery[16];

// picture.c

extern SDL_Surface*screen;
extern Uint16 picture_size;

void draw_picture(int x,int y,Uint16 img);
void draw_text(int x,int y,const unsigned char*t,int bg,int fg);
void load_pictures(void);

// class.c

#define CF_PLAYER 0x01
#define CF_INPUT 0x02
#define CF_COMPATIBLE 0x04
#define CF_QUIZ 0x08
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
  const char*edithelp;
  const char*gamehelp;
  Uint16*codes;
  Uint16*messages; // use 0xFFFF if no such message block
  Uint16*images; // high bit is set if available to editor
  Sint32 height,weight,climb,density,volume,strength,arrivals,departures;
  Sint32 temperature,misc4,misc5,misc6,misc7;
  Uint16 uservars,oflags;
  Uint16 sharp[4];
  Uint16 hard[4];
  Uint8 cflags,shape,shovable;
} Class;

extern Class*classes[0x4000]; // 0 isn't used
extern const char*messages[0x4000];
extern int max_animation;
extern Sint32 max_volume;

