#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-multichar picedit.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"
#include "quarks.h"
#include "cursorshapes.h"

typedef struct {
  Uint8 size,meth;
  Uint8 data[0]; // the first row is all 0, since the compression algorithm requires this
} Picture;

static int load_picture_file(void) {
  sqlite3_stmt*st=0;
  FILE*fp;
  char*nam;
  char*buf=0;
  int r=0;
  int i,j;
  i=sqlite3_exec(userdb,
    "BEGIN;"
    "CREATE TEMPORARY TABLE `PICEDIT`(`ID` INTEGER PRIMARY KEY,`NAME` TEXT NOT NULL COLLATE NOCASE,`TYPE` INT,`DATA` BLOB);"
    "CREATE INDEX `PICEDIT_I1` ON `PICEDIT`(`NAME`,`TYPE`);"
  ,0,0,0);
  if(i) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  nam=sqlite3_mprintf("%s.xclass",basefilename);
  if(!nam) fatal("Allocation failed\n");
  fprintf(stderr,"Loading pictures...\n");
  fp=fopen(nam,"r");
  sqlite3_free(nam);
  if(!fp) {
    nam=0;
    fprintf(stderr,"%m\n");
    goto done;
  }
  nam=malloc(256);
  if(!nam) fatal("Allocation failed\n");
  i=sqlite3_prepare_v2(userdb,"INSERT INTO `PICEDIT`(`NAME`,`TYPE`,`DATA`) VALUES(?1,?2,?3);",-1,&st,0);
  if(i) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  while(!feof(fp)) {
    i=0;
    while(j=fgetc(fp)) {
      if(j==EOF) goto done;
      if(i<255) nam[i++]=j;
    }
    nam[i]=0;
    sqlite3_reset(st);
    sqlite3_bind_text(st,1,nam,i,SQLITE_TRANSIENT);
    sqlite3_bind_int(st,2,j=(i>4 && !memcmp(".IMG",nam+i-4,4)?1:0));
    r+=j;
    i=fgetc(fp)<<16;
    i|=fgetc(fp)<<24;
    i|=fgetc(fp)<<0;
    i|=fgetc(fp)<<8;
    if(!i) continue;
    buf=realloc(buf,i);
    if(!buf) fatal("Allocation failed\n");
    fread(buf,1,i,fp);
    sqlite3_bind_blob(st,3,buf,i,SQLITE_TRANSIENT);
    while((i=sqlite3_step(st))==SQLITE_ROW);
    if(i!=SQLITE_DONE) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  }
done:
  if(st) sqlite3_finalize(st);
  if(fp) fclose(fp);
  free(nam);
  free(buf);
  fprintf(stderr,"Done\n");
  return r;
}

static void save_picture_file(void) {
  sqlite3_stmt*st;
  FILE*fp;
  char*s=sqlite3_mprintf("%s.xclass",basefilename);
  int i;
  const char*nam;
  const char*buf;
  if(!s) fatal("Allocation failed\n");
  fprintf(stderr,"Saving pictures...\n");
  fp=fopen(s,"w");
  if(!fp) fatal("Cannot open picture file for writing: %m\n");
  sqlite3_free(s);
  i=sqlite3_prepare_v2(userdb,"SELECT `NAME`,`DATA` FROM `PICEDIT`;",-1,&st,0);
  if(i) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  while((i=sqlite3_step(st))==SQLITE_ROW) {
    nam=sqlite3_column_text(st,0);
    buf=sqlite3_column_blob(st,1);
    i=sqlite3_column_bytes(st,1);
    if(!nam) continue;
    fwrite(nam,1,strlen(nam)+1,fp);
    fputc(i>>16,fp);
    fputc(i>>24,fp);
    fputc(i>>0,fp);
    fputc(i>>8,fp);
    if(i) fwrite(buf,1,i,fp);
  }
  if(i!=SQLITE_DONE) fprintf(stderr,"SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
done:
  if(st) sqlite3_finalize(st);
  if(fp) fclose(fp);
  fprintf(stderr,"Done\n");
}

static sqlite3_int64 ask_picture_id(const char*t) {
  sqlite3_stmt*st;
  const char*r=screen_prompt(t);
  int i;
  sqlite3_int64 id=0;
  if(!r || !*r) return 0;
  i=sqlite3_prepare_v2(userdb,"SELECT `ID` FROM `PICEDIT` WHERE `NAME` = (?1 || '.IMG');",-1,&st,0);
  if(i) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  sqlite3_bind_text(st,1,r,-1,0);
  i=sqlite3_step(st);
  if(i==SQLITE_ROW) id=sqlite3_column_int64(st,0);
  if(i==SQLITE_DONE) screen_message("Picture not found");
  sqlite3_finalize(st);
  return id;
}

static void uncompress_picture(FILE*fp,Picture*pic) {
  Uint8*p=pic->data+pic->size;
  int c,n,t,x,y;
  n=t=0;
  y=pic->size*pic->size;
  while(y--) {
    if(!n) {
      n=fgetc(fp);
      if(n<85) {
        // Homogeneous run
        n++;
        x=fgetc(fp);
        if(t==1 && x==c) n*=85; else n++;
        c=x;
        t=1;
      } else if(n<170) {
        // Heterogeneous run
        n-=84;
        t=2;
      } else {
        // Copy-above run
        n-=169;
        if(t==3) n*=85;
        t=3;
      }
    }
    n--;
    if(t==2) c=fgetc(fp);
    if(t==3) c=p[-pic->size];
    *p++=c;
  }
}

static void load_picture_lump(const unsigned char*data,int len,Picture**pict) {
  Uint8 buf[32];
  FILE*fp;
  int i,j,n;
  if(!len) return;
  fp=fmemopen((unsigned char*)data,len,"r");
  if(!fp) fatal("Failed to open in-memory stream of size %d\n",len);
  *buf=fgetc(fp);
  n=*buf&15;
  fread(buf+1,1,n+(n>>1),fp);
  for(i=0;i<n;i++) {
    j=buf[i+1];
    pict[i]=malloc(sizeof(Picture)+(j+1)*j);
    if(!pict[i]) fatal("Allocation failed\n");
    memset(pict[i]->data,0,pict[i]->size=j);
    j=(i?buf[n+1+((i-1)>>1)]>>(i&1?0:4):*buf>>4);
    pict[i]->meth=j^((j==5 || j==6)?3:0);
  }
  for(i=0;i<n;i++) {
    j=pict[i]->size;
    if(pict[i]->meth==15) fread(pict[i]->data+j,j,j,fp),pict[i]->meth=0; else uncompress_picture(fp,pict[i]);
    // Rotation
    
  }
  fclose(fp);
}

static inline void edit_picture_1(Picture**pict,const char*name) {
  static Picture*pclip=0;
  Uint8 sel=0;
  SDL_Rect r;
  SDL_Event ev;
  int i,j,x;
  unsigned char buf[256];
  set_cursor(XC_arrow);
  redraw:
  SDL_LockSurface(screen);
  r.x=r.y=0; r.w=screen->w; r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  draw_text(0,0,name,0xF0,0xF5);
  x=strlen(name)+1;
  for(i=0;i<16;i++) if(pict[i]) {
    j=snprintf(buf,255,"%c%d%c",i==sel?'<':' ',pict[i]->size,i==sel?'>':' ');
    draw_text(x<<3,0,buf,0xF0,i==sel?0xFF:0xF8);
    x+=j;
  }
  
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        exit(0);
        return;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: return;
        }
        break;
      case SDL_VIDEOEXPOSE:
        goto redraw;
    }
  }
}

static void edit_picture(sqlite3_int64 id) {
  sqlite3_stmt*st;
  Picture*pict[16]={0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0};
  char*name;
  const unsigned char*data;
  int i;
  if(i=sqlite3_prepare_v2(userdb,"SELECT SUBSTR(`NAME`,1,LENGTH(`NAME`)-4),`DATA` FROM `PICEDIT` WHERE `ID`=?1;",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return;
  }
  sqlite3_bind_int64(st,1,id);
  i=sqlite3_step(st);
  if(i!=SQLITE_ROW) {
    screen_message(i==SQLITE_DONE?"No such ID":sqlite3_errmsg(userdb));
    sqlite3_finalize(st);
    return;
  }
  data=sqlite3_column_blob(st,1);
  i=sqlite3_column_bytes(st,1);
  load_picture_lump(data,i,pict);
  name=strdup(sqlite3_column_text(st,0)?:(const unsigned char*)"???");
  if(!name) fatal("Allocation failed\n");
  sqlite3_finalize(st);
  if(!*pict) {
    i=picture_size;
    *pict=malloc(sizeof(Picture)+(i+1)*i);
    if(!*pict) fatal("Allocation failed");
    pict[0]->size=i;
    memset(pict[0]->data,0,(i+1)*i);
  }
  edit_picture_1(pict,name);
  free(name);
  for(i=0;i<16;i++) {
    
    free(pict[i]);
  }
  
}

static void set_caption(void) {
  char buf[256];
  snprintf(buf,255,"Free Hero Mesh - %s - Picture",basefilename);
  SDL_WM_SetCaption(buf,buf);
}

void run_picture_editor(void) {
  sqlite3_int64*ids;
  SDL_Event ev;
  SDL_Rect r;
  sqlite3_stmt*st;
  int sc=0;
  int max=load_picture_file();
  int i,n;
  init_palette();
  optionquery[1]=Q_imageSize;
  picture_size=strtol(xrm_get_resource(resourcedb,optionquery,optionquery,2)?:"16",0,10);
  set_cursor(XC_arrow);
  set_caption();
  i=sqlite3_prepare_v3(userdb,"SELECT `ID`,SUBSTR(`NAME`,1,LENGTH(`NAME`)-4),`TYPE` FROM `PICEDIT` WHERE `TYPE` ORDER BY `NAME` LIMIT ?1 OFFSET ?2;",-1,SQLITE_PREPARE_PERSISTENT,&st,0);
  if(i) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  ids=calloc(screen->h/8,sizeof(sqlite3_int64));
  if(!ids) fatal("Allocation failed\n");
  redraw:
  sqlite3_reset(st);
  if(sc>max-screen->h/8) sc=max-screen->h/8;
  if(sc<0) sc=0;
  sqlite3_bind_int(st,1,screen->h/8-1);
  sqlite3_bind_int(st,2,sc);
  SDL_LockSurface(screen);
  r.x=r.y=0; r.w=screen->w; r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  draw_text(0,0,"<ESC> Save/Quit  <F1> Add  <F2> Delete  <F3> Edit",0xF0,0xFB);
  n=0;
  while((i=sqlite3_step(st))==SQLITE_ROW) {
    ids[n++]=sqlite3_column_int64(st,0);
    draw_text(16,8*n,sqlite3_column_text(st,1),0xF0,0xF7);
    if(8*n+8>screen->h-8) break;
  }
  SDL_UnlockSurface(screen);
  sqlite3_reset(st);
  r.y=8; r.h-=8;
  scrollbar(&sc,r.h/8,max,0,&r);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    if(ev.type!=SDL_VIDEOEXPOSE && scrollbar(&sc,screen->h/8,max,&ev,&r)) goto redraw;
    switch(ev.type) {
      case SDL_QUIT:
        return;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE:
            if(!(ev.key.keysym.mod&(KMOD_SHIFT|KMOD_CTRL))) save_picture_file();
            return;
          case SDLK_q:
            if(!(ev.key.keysym.mod&KMOD_CTRL)) break;
            if(!(ev.key.keysym.mod&KMOD_SHIFT)) save_picture_file();
            return;
          case SDLK_HOME: sc=0; goto redraw;
          case SDLK_UP: if(sc) --sc; goto redraw;
          case SDLK_DOWN: ++sc; goto redraw;
          case SDLK_END: sc=max-screen->h/8; goto redraw;
          case SDLK_PAGEUP: sc-=screen->h/8-1; goto redraw;
          case SDLK_PAGEDOWN: sc+=screen->h/8-1; goto redraw;
          case SDLK_F3:
            *ids=ask_picture_id("Edit:");
            if(*ids) edit_picture(*ids);
            goto redraw;
        }
        break;
      case SDL_MOUSEMOTION:
        set_cursor(XC_arrow);
        break;
      case SDL_VIDEOEXPOSE:
        goto redraw;
    }
  }
}
