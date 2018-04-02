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

