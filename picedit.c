#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-multichar picedit.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#include "SDL.h"
#include <math.h>
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

static void fn_valid_name(sqlite3_context*cxt,int argc,sqlite3_value**argv) {
  const char*s=sqlite3_value_text(*argv);
  if(!s || !*s || s[strspn(s,"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz_-0123456789")]) {
    sqlite3_result_error(cxt,"Invalid name",-1);
    return;
  }
  sqlite3_result_value(cxt,*argv);
}

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
  sqlite3_exec(userdb,
    "CREATE TRIGGER `PICEDIT_T1` BEFORE INSERT ON `PICEDIT` BEGIN"
    "  SELECT RAISE(FAIL,'Duplicate name') FROM `PICEDIT` WHERE `NAME`=NEW.`NAME`;"
    "END;"
    "CREATE TRIGGER `PICEDIT_T2` BEFORE UPDATE OF `NAME` ON `PICEDIT` BEGIN"
    "  SELECT RAISE(FAIL,'Duplicate name') FROM `PICEDIT` WHERE `NAME`=NEW.`NAME`;"
    "END;"
  ,0,0,0);
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

static void load_rotate(Picture*pic) {
  int x,y;
  int m=pic->meth;
  int s=pic->size;
  Uint8*d=pic->data+s;
  static Uint8*buf;
  Uint8*p;
  if(!buf) {
    buf=malloc(255*255);
    if(!buf) fatal("Allocation failed\n");
  }
  p=buf;
  for(y=0;y<s;y++) for(x=0;x<s;x++) {
    if(m&1) x=s-x-1;
    if(m&2) y=s-y-1;
    *p++=d[m&4?x*s+y:y*s+x];
    if(m&1) x=s-x-1;
    if(m&2) y=s-y-1;
  }
  memcpy(d,buf,s*s);
}

static void out_run(FILE*fp,int ch,int am,const Uint8*d,int le) {
  int n=am%85?:85;
  fputc(ch+n-1,fp);
  if(le) fwrite(d,1,le<n?le:n,fp);
  if(ch) d+=n,le-=n;
  while(am-=n) {
    n=am>7225?7225:am;
    fputc(ch+n/85-1,fp);
    if(le>0) fwrite(d,1,le<n?le:n,fp);
    if(ch) d+=n,le-=n;
  }
}

static void compress_picture_1(FILE*fp,Picture*pic) {
  int ps=pic->size;
  int ms=ps*ps;
  const Uint8*d=pic->data+ps;
  int i=0;
  int ca,homo,hetero;
  while(i<ms) {
    ca=0;
    while(i+ca<ms && d[i+ca]==d[i+ca-ps]) ca++;
    homo=1;
    while(i+homo<ms && d[i+homo]==d[i]) homo++;
    hetero=1;
    while(i+hetero<ms) {
      if(d[i+hetero]==d[i+hetero-ps] && d[i+hetero+1]==d[i+hetero+1-ps]) break;
      if(d[i+hetero]==d[i+hetero+1] && i+hetero==ms-1) break;
      if(d[i+hetero]==d[i+hetero+1] && d[i+hetero]==d[i+hetero+2]) break;
      hetero++;
    }
    if(ca>=homo && (ca>=hetero || homo>1)) {
      out_run(fp,170,ca,0,0);
      i+=ca;
    } else if(homo>1) {
      out_run(fp,0,homo-1,d+i,1);
      i+=homo;
    } else {
      if(hetero>85) hetero=85;
      out_run(fp,85,hetero,d+i,hetero);
      i+=hetero;
    }
  }
}

static void compress_picture(FILE*out,Picture*pic) {
  int bm=15;
  int bs=pic->size*pic->size;
  FILE*fp=fmemopen(0,bs,"w");
  int i,j;
  if(!fp) fatal("Error with fmemopen");
  for(i=0;i<8;i++) {
    compress_picture_1(fp,pic);
    if(!ferror(fp)) {
      j=ftell(fp);
      if(j>0 && j<bs) bs=j,bm=i;
    }
    rewind(fp);
    pic->meth="\1\3\1\7\1\3\1\7"[i];
    load_rotate(pic);
  }
  pic->meth=bm;
  if(bm && bm!=15) load_rotate(pic);
  fclose(fp);
  if(bm==15) fwrite(pic->data+pic->size,pic->size,pic->size,out); else compress_picture_1(out,pic);
}

static void block_rotate(Uint8*d,Uint8 s,Uint8 w,Uint8 h,Uint8 m) {
  Uint8*b=malloc(w*h);
  Uint8*p=b;
  int x,y;
  if(!b) fatal("Allocation failed\n");
  if(w!=h && (m&4)) goto end;
  for(y=0;y<h;y++) for(x=0;x<w;x++) {
    if(m&1) x=w-x-1;
    if(m&2) y=h-y-1;
    *p++=d[m&4?x*s+y:y*s+x];
    if(m&1) x=w-x-1;
    if(m&2) y=h-y-1;
  }
  p=b;
  for(y=0;y<h;y++) {
    memcpy(d,p,w);
    p+=w;
    d+=s;
  }
  end: free(b);
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
    j=(i?buf[n+1+((i-1)>>1)]>>(i&1?0:4):*buf>>4)&15;
    pict[i]->meth=j^((j==5 || j==6)?3:0);
  }
  for(i=0;i<n;i++) {
    j=pict[i]->size;
    if(pict[i]->meth==15) fread(pict[i]->data+j,j,j,fp),pict[i]->meth=0; else uncompress_picture(fp,pict[i]);
    if(pict[i]->meth) load_rotate(pict[i]);
  }
  fclose(fp);
}

static inline void show_cursor_xy(int x,int y,int xx,int yy) {
  char buf[64];
  if(x>=0 && xx>=0) snprintf(buf,64,"[%d,%d]:(%d,%d) %+d%+d%63s",xx,yy,x,y,x-xx,y-yy,"");
  else if(x>=0) snprintf(buf,64,"(%d,%d)%63s",x,y,"");
  else if(xx>=0) snprintf(buf,64,"[%d,%d]%63s",xx,yy,"");
  else snprintf(buf,64,"%63s","");
  SDL_LockSurface(screen);
  draw_text(0,32,buf,0xF0,0xF9);
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
}

static void draw_line(Uint8*p,Uint8 s,Uint8 x0,Uint8 y0,Uint8 x1,Uint8 y1,Uint8 c) {
  int dx=abs(x1-x0);
  int sx=x0<x1?1:-1;
  int dy=-abs(y1-y0);
  int sy=y0<y1?1:-1;
  int e=dx+dy;
  int e2;
  for(;;) {
    p[y0*s+x0]=c;
    if(x0==x1 && y0==y1) break;
    e2=e+e;
    if(e2>=dy) e+=dy,x0+=sx;
    if(e2<=dx) e+=dx,y0+=sy;
  }
}

static void draw_circle(Uint8*p,Uint8 s,Uint8 x0,Uint8 y0,Uint32 r,Uint8 c) {
  int i,j,x,y;
  for(i=0;i<s && r>=0;i++) {
    j=sqrt(r);
    r-=i+i+1;
    x=x0+i,y=y0+j; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0-i,y=y0+j; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0+i,y=y0-j; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0-i,y=y0-j; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0+j,y=y0+i; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0-j,y=y0+i; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0+j,y=y0-i; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
    x=x0-j,y=y0-i; if(x>=0 && x<s && y>=0 && y<s) p[y*s+x]=c;
  }
}

static void fill_circle(Uint8*p,Uint8 s,Uint8 x0,Uint8 y0,Uint32 r,Uint8 c) {
  int x,y;
  for(y=0;y<s;y++) for(x=0;x<s;x++) {
    if((x-x0)*(x-x0)+(y-y0)*(y-y0)<=r) p[y*s+x]=c;
  }
}

static void draw_ellipse(Uint8*p,Uint8 s,Uint8 x0,Uint8 y0,Uint8 x1,Uint8 y1,Uint8 c) {
  double cx,cy,rx,ry,v;
  int x,y;
  if(x1<x0) x=x1,x1=x0,x0=x;
  if(y1<y0) y=y1,y1=y0,y0=y;
  cx=0.5*(x1+x0);
  cy=0.5*(y1+y0);
  rx=0.5*(x1-x0);
  ry=0.5*(y1-y0);
  for(y=y0;y<=y1;y++) {
    v=(y-cy)/ry; v*=v;
    if(v>1.0) continue;
    v=rx*sqrt(1.0-v);
    x=round(cx-v); if(x>=0 && x<s) p[y*s+x]=c;
    x=round(cx+v); if(x>=0 && x<s) p[y*s+x]=c;
  }
  for(x=x0;x<=x1;x++) {
    v=(x-cx)/rx; v*=v;
    if(v>1.0) continue;
    v=ry*sqrt(1.0-v);
    y=round(cy-v); if(y>=0 && y<s) p[y*s+x]=c;
    y=round(cy+v); if(y>=0 && y<s) p[y*s+x]=c;
  }
}

static void fill_ellipse(Uint8*p,Uint8 s,Uint8 x0,Uint8 y0,Uint8 x1,Uint8 y1,Uint8 c) {
  double cx,cy,rx,ry;
  int x,y;
  if(x1<x0) x=x1,x1=x0,x0=x;
  if(y1<y0) y=y1,y1=y0,y0=y;
  cx=0.5*(x1+x0);
  cy=0.5*(y1+y0);
  rx=0.5*(x1-x0);
  ry=0.5*(y1-y0);
  for(y=y0;y<=y1;y++) for(x=x0;x<=x1;x++) {
    if(pow((x-cx)/rx,2.0)+pow((y-cy)/ry,2.0)<=1.0) p[y*s+x]=c;
  }
}

static Picture*do_import(Picture*pic) {
  SDL_Color*pal=screen->format->palette->colors;
  int a,b,i,j,x,y;
  Uint8 buf[16];
  Uint8*q;
  const char*cmd=screen_prompt("Import?");
  FILE*fp;
  if(!cmd || !*cmd) return pic;
  fp=popen(cmd,"r");
  if(!fp) {
    screen_message("Import failed");
    return pic;
  }
  if(fread(buf,1,16,fp)!=16 || memcmp("farbfeld",buf,8)) {
    screen_message("Invalid format");
    goto end;
  }
  if(buf[8] || buf[9] || buf[10] || buf[12] || buf[13] || buf[14] || !buf[15] || buf[11]!=buf[15]) {
    screen_message("Invalid size");
    goto end;
  }
  if(pic->size!=buf[15]) {
    pic=realloc(pic,sizeof(Picture)+(buf[15]+1)*buf[15]);
    if(!pic) fatal("Allocation failed\n");
    pic->size=buf[15];
    memset(pic->data+pic->size,0,pic->size);
  }
  q=pic->data+pic->size;
  for(y=0;y<pic->size;y++) for(x=0;x<pic->size;x++) {
    fread(buf,1,8,fp);
    if(buf[6]&0x80) {
      for(i=1;i<255;i++) if(buf[0]==pal[i].r && buf[1]==pal[i].g && buf[2]==pal[i].b) goto found;
      i=1;
      a=0x300000;
      for(j=1;j<255;j++) {
        b=abs(buf[0]-pal[j].r)+abs(buf[2]-pal[j].g)+abs(buf[4]-pal[j].b);
        if(b<=a) a=b,i=j;
      }
      found: *q++=i;
    } else {
      *q++=0;
    }
  }
  end:
  pclose(fp);
  return pic;
}

static void do_export(Picture*pic) {
  SDL_Color*pal=screen->format->palette->colors;
  Uint8*q=pic->data+pic->size;
  int c,x,y;
  const char*cmd=screen_prompt("Export?");
  FILE*fp;
  if(!cmd || !*cmd) return;
  fp=popen(cmd,"w");
  fwrite("farbfeld\0\0\0",1,11,fp); fputc(pic->size,fp);
  fwrite("\0\0\0",1,3,fp); fputc(pic->size,fp);
  for(y=0;y<pic->size;y++) for(x=0;x<pic->size;x++) {
    c=*q++;
    fputc(pal[c].r,fp); fputc(pal[c].r,fp);
    fputc(pal[c].g,fp); fputc(pal[c].g,fp);
    fputc(pal[c].b,fp); fputc(pal[c].b,fp);
    fputc(c?255:0,fp); fputc(c?255:0,fp);
  }
  pclose(fp);
}

static inline void edit_picture_1(Picture**pict,const char*name) {
  static const Uint8 shade[64]=
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0A\x0B\x0C\x0D\x0E\x0F"
  ;
  static const Uint8 gridlines[32]=
    "\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF"
    "\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF\xF8\xFF"
  ;
  static const char*const tool[10]={
    "Draw",
    "Mark",
    "Pick",
    "Line",
    "Rect",
    "Fillrect",
    "Circle",
    "fIllcirc",
    "Ellipse",
    "fillellipSe",
  };
  static Picture*pclip=0;
  Uint8*p;
  Uint8*q;
  Uint8 sel=0;
  Uint8 cc=0;
  Uint8 t=2;
  SDL_Rect r,m;
  SDL_Event ev;
  int i,j,x,y,z;
  int xx=-1;
  int yy=-1;
  unsigned char buf[256];
  m.x=m.y=m.w=m.h=0;
  set_cursor(XC_arrow);
  redraw:
  if((sel&~15) || !pict[sel]) sel=0;
  z=screen->w-pict[sel]->size-169;
  if(z>screen->h-49) z=screen->h-49;
  z/=pict[sel]->size;
  if(z<3) return;
  if(z>32) z=32;
  SDL_LockSurface(screen);
  p=screen->pixels;
  r.x=r.y=0; r.w=screen->w; r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  draw_text(0,0,name,0xF0,0xF5);
  x=strlen(name)+1;
  for(i=0;i<16;i++) if(pict[i]) {
    j=snprintf(buf,255,"%c%d%c",i==sel?'<':' ',pict[i]->size,i==sel?'>':' ');
    draw_text(x<<3,0,buf,0xF0,i==sel?0xFF:0xF8);
    x+=j;
  }
  draw_text(0,8,"<ESC> Exit  <1-9> Sel  <SP> Unmark  <RET> Copy  <INS> Paste  <DEL> Erase",0xF0,0xFB);
  draw_text(0,16,"<F1> CW  <F2> CCW  <F3> \x12  <F4> \x1D  <F5> Size  <F6> Add  <F7> Del  <F8> Mark  <F9> In  <F10> Out",0xF0,0xFB);
  x=0;
  for(i=0;i<10;i++) {
    j=snprintf(buf,255,"%c%s%c",i==t?'<':' ',tool[i],i==t?'>':' ');
    draw_text(x<<3,24,buf,0xF0,i==t?0xFE:0xF8);
    x+=j;
  }
  p=screen->pixels+40*screen->pitch;
  q=pict[sel]->data+pict[sel]->size;
  for(y=0;y<pict[sel]->size;y++) {
    memcpy(p,q,pict[sel]->size);
    p+=screen->pitch;
    q+=pict[sel]->size;
  }
  p=screen->pixels+40*screen->pitch-screen->w-161;
  for(x=0;x<256;x++) {
    for(y=0;y<10;y++) memset(p+y*screen->pitch,x,10);
    if(x==cc) {
      memset(p+1,0xF0,8);
      for(y=1;y<9;y++) {
        p[y*screen->pitch]=0xF0;
        p[y*screen->pitch+(y&1)+4]=(y&1)+0xF7;
        p[y*screen->pitch+9]=0xFF;
      }
      memset(p+9*screen->pitch+1,0xFF,8);
    }
    if(15&~x) p+=10; else p+=10*screen->pitch-150;
  }
  p=screen->pixels+41*screen->pitch+pict[sel]->size+2;
  q=pict[sel]->data+pict[sel]->size;
  for(y=0;y<pict[sel]->size;y++) {
    for(x=0;x<pict[sel]->size;x++) {
      memcpy(p,gridlines,z);
      for(i=1;i<z;i++) {
        p[i*screen->pitch]=i&1?0xF8:0xFF;
        if(*q) memset(p+i*screen->pitch+1,*q,z-1);
        else memcpy(p+i*screen->pitch+1,shade+(i&15),z-1);
      }
      if(xx==x && yy==y) {
        memset(p+(z/2)*screen->pitch,0xFA,z);
        memset(p+(z/2+1)*screen->pitch,0xF2,z);
        for(i=1;i<z;i++) memcpy(p+i*screen->pitch+z/2,"\xF2\xFA",2);
      }
      p+=z;
      q++;
    }
    for(i=1;i<z;i++) p[i*screen->pitch]=i&1?0xF8:0xFF;
    p+=z*(screen->pitch-pict[sel]->size);
  }
  for(x=0;x<pict[sel]->size;x++) {
    memcpy(p,gridlines,z);
    p+=z;
  }
  if(m.h) {
    r.x=m.x*z+pict[sel]->size+4;
    r.y=m.y*z+43;
    r.w=m.w*z-4;
    r.h=m.h*z-4;
    p=screen->pixels+r.y*screen->pitch+r.x;
    for(i=0;i<r.w;i++) p[i]=i&1?0xFE:0xF6;
    for(i=0;i<r.h;i++) {
      *p=p[r.w]=i&1?0xF6:0xFE;
      p+=screen->pitch;
    }
    for(i=0;i<r.w;i++) p[i]=i&1?0xFE:0xF6;
  }
  SDL_UnlockSurface(screen);
  SDL_Flip(screen);
  while(SDL_WaitEvent(&ev)) {
    switch(ev.type) {
      case SDL_QUIT:
        exit(0);
        return;
      case SDL_MOUSEMOTION:
        if(ev.motion.x<pict[sel]->size+2 || ev.motion.x>=pict[sel]->size*(z+1)+2 || ev.motion.y<41 || ev.motion.y>=z*pict[sel]->size+41) {
          x=-1;
          set_cursor(XC_arrow);
        } else {
          x=(ev.motion.x-pict[sel]->size-2)/z;
          y=(ev.motion.y-41)/z;
          set_cursor(XC_tcross);
          if(ev.motion.state && !t) {
            if(ev.motion.state&SDL_BUTTON(2)) break;
            pict[sel]->data[(y+1)*pict[sel]->size+x]=(ev.motion.state&SDL_BUTTON(1)?cc:0);
            goto redraw;
          }
        }
        show_cursor_xy(x,y,xx,yy);
        break;
      case SDL_MOUSEBUTTONDOWN:
        if(ev.button.x>=screen->w-161 && ev.button.x<screen->w-1 && ev.button.y>=40 && ev.button.y<200) {
          x=(ev.button.x+161-screen->w)/10;
          y=(ev.button.y-40)/10;
          i=y*16+x;
          pick:
          switch(ev.button.button) {
            case 1: cc=i; break;
            case 2:
              for(x=(m.y+1)*pict[sel]->size+m.x;x<pict[sel]->size*(pict[sel]->size+1);x++) {
                if(pict[sel]->data[x]==cc) pict[sel]->data[x]=i;
                if(m.w && x%pict[sel]->size==m.x+m.w-1) x+=pict[sel]->size-m.w;
                if(m.h && x/pict[sel]->size>m.h+m.y) break;
              }
              break;
            case 3:
              for(x=(m.y+1)*pict[sel]->size+m.x;x<pict[sel]->size*(pict[sel]->size+1);x++) {
                if(pict[sel]->data[x]==cc) pict[sel]->data[x]=i;
                else if(pict[sel]->data[x]==i) pict[sel]->data[x]=cc;
                if(m.w && x%pict[sel]->size==m.x+m.w-1) x+=pict[sel]->size-m.w;
                if(m.h && x/pict[sel]->size>m.h+m.y) break;
              }
              break;
          }
          goto redraw;
        } else if(ev.button.x>=pict[sel]->size+2 && ev.button.x<pict[sel]->size*(z+1)+2 && ev.button.y>=41 && ev.button.y<z*pict[sel]->size+41) {
          x=(ev.button.x-pict[sel]->size-2)/z;
          y=(ev.button.y-41)/z;
          i=ev.button.button;
          if(i>3) break;
          switch(t) {
            case 0: // Draw
              if(i==2) cc=pict[sel]->data[(y+1)*pict[sel]->size+x];
              else pict[sel]->data[(y+1)*pict[sel]->size+x]=(i==1?cc:0);
              break;
            case 1: // Mark
              if(i==1) {
                xx=x; yy=y;
              } else if(i==3) {
                m.x=(x<xx?x:xx); m.y=(y<yy?y:yy);
                m.w=abs(x-xx)+1; m.h=abs(y-yy)+1;
              }
              break;
            case 2: // Pick
              i=pict[sel]->data[(y+1)*pict[sel]->size+x];
              goto pick;
            case 3: // Line
              if((i&2) && xx!=-1) draw_line(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,xx,yy,cc);
              if(i&1) xx=x,yy=y;
              break;
            case 4: // Rectangle
              if(i==1) {
                xx=x; yy=y;
              } else if(xx!=-1) {
                p=pict[sel]->data+(j=pict[sel]->size);
                if(xx<x) i=x,x=xx,xx=i;
                if(yy<y) i=y,y=yy,yy=i;
                memset(p+y*j+x,cc,xx+1-x);
                memset(p+yy*j+x,cc,xx+1-x);
                while(y<yy) p[y*j+x]=p[y*j+xx]=cc,y++;
                xx=yy=-1;
              }
              break;
            case 5: // Fill rectangle
              if(i==1) {
                xx=x; yy=y;
              } else if(xx!=-1) {
                p=pict[sel]->data+(j=pict[sel]->size);
                if(xx<x) i=x,x=xx,xx=i;
                if(yy<y) i=y,y=yy,yy=i;
                while(y<yy) memset(p+y*j+x,cc,xx+1-x),y++;
                xx=yy=-1;
              }
              break;
            case 6: // Circle
              if(i==1) {
                xx=x; yy=y;
              } else if(i==2) {
                draw_circle(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,(x-xx)*(x-xx)+(y-yy)*(y-yy),cc);
              } else if(i==3) {
                draw_circle(pict[sel]->data+pict[sel]->size,pict[sel]->size,xx,yy,(x-xx)*(x-xx)+(y-yy)*(y-yy),cc);
              }
              break;
            case 7: // Fill circle
              if(i==1) {
                xx=x; yy=y;
              } else if(i==2) {
                fill_circle(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,(x-xx)*(x-xx)+(y-yy)*(y-yy),cc);
              } else if(i==3) {
                fill_circle(pict[sel]->data+pict[sel]->size,pict[sel]->size,xx,yy,(x-xx)*(x-xx)+(y-yy)*(y-yy),cc);
              }
              break;
            case 8: // Ellipse
              if(i==1) {
                xx=x; yy=y;
              } else if(xx!=-1) {
                draw_ellipse(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,xx,yy,cc);
              }
              break;
            case 9: // Fill ellipse
              if(i==1) {
                xx=x; yy=y;
              } else if(xx!=-1) {
                fill_ellipse(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,xx,yy,cc);
                draw_ellipse(pict[sel]->data+pict[sel]->size,pict[sel]->size,x,y,xx,yy,cc);
              }
              break;
          }
          goto redraw;
        }
        break;
      case SDL_KEYDOWN:
        switch(ev.key.keysym.sym) {
          case SDLK_ESCAPE: return;
          case SDLK_LEFTBRACKET: case SDLK_LEFTPAREN: --sel; m.x=m.y=m.w=m.h=0; goto redraw;
          case SDLK_RIGHTBRACKET: case SDLK_RIGHTPAREN: ++sel; m.x=m.y=m.w=m.h=0; goto redraw;
          case SDLK_1 ... SDLK_9: sel=ev.key.keysym.sym-SDLK_1; m.x=m.y=m.w=m.h=0; goto redraw;
          case SDLK_c: t=6; xx=yy=-1; goto redraw;
          case SDLK_d: t=0; xx=yy=-1; goto redraw;
          case SDLK_e: t=8; xx=yy=-1; goto redraw;
          case SDLK_f: t=5; xx=yy=-1; goto redraw;
          case SDLK_i: t=7; xx=yy=-1; goto redraw;
          case SDLK_l: t=3; xx=yy=-1; goto redraw;
          case SDLK_m: t=1; xx=yy=-1; goto redraw;
          case SDLK_p: t=2; xx=yy=-1; goto redraw;
          case SDLK_r: t=4; xx=yy=-1; goto redraw;
          case SDLK_s: t=9; xx=yy=-1; goto redraw;
          case SDLK_TAB: if(ev.key.keysym.mod&KMOD_SHIFT) --sel; else ++sel; m.x=m.y=m.w=m.h=0; goto redraw;
          case SDLK_SPACE:
            m.x=m.y=m.w=m.h=0;
            xx=yy=-1;
            goto redraw;
          case SDLK_RETURN: case SDLK_KP_ENTER:
            if(!m.w) m.w=m.h=pict[sel]->size;
            free(pclip);
            pclip=malloc(sizeof(Picture)+(m.h+1)*m.w);
            if(!pclip) fatal("Allocation failed\n");
            pclip->meth=m.w;
            pclip->size=m.h;
            memset(pclip->data,0,m.w);
            p=pict[sel]->data+(m.y+1)*pict[sel]->size+m.x;
            q=pclip->data+m.w;
            for(y=0;y<m.h;y++) {
              memcpy(q,p,m.w);
              p+=pict[sel]->size;
              q+=m.w;
            }
            goto redraw;
          case SDLK_INSERT: case SDLK_KP0: paste:
            if(!pclip) break;
            if(!m.w) {
              if(xx!=-1) {
                if(xx+pclip->meth>pict[sel]->size || yy+pclip->size>pict[sel]->size) break;
                m.x=xx;
                m.y=yy;
              }
              m.w=pclip->meth;
              m.h=pclip->size;
              if(m.w>pict[sel]->size) m.w=pict[sel]->size;
              if(m.h>pict[sel]->size) m.h=pict[sel]->size;
            }
            p=pclip->data+pclip->meth;
            q=pict[sel]->data+(m.y+1)*pict[sel]->size+m.x;
            for(y=0;y<m.h;y++) {
              for(x=0;x<m.w;x++) {
                i=(x*pclip->meth)/m.w;
                j=(y*pclip->size)/m.h;
                q[x]=p[j*pclip->meth+i];
              }
              q+=pict[sel]->size;
            }
            goto redraw;
          case SDLK_DELETE: case SDLK_KP_PERIOD:
            if(!m.w) break;
            p=pict[sel]->data+(m.y+1)*pict[sel]->size+m.x;
            for(y=0;y<m.h;y++) memset(p,0,m.w),p+=pict[sel]->size;
            goto redraw;
          case SDLK_F1:
            if(!m.w) m.w=m.h=pict[sel]->size;
            block_rotate(pict[sel]->data+(m.y+1)*pict[sel]->size+m.x,pict[sel]->size,m.w,m.h,5);
            goto redraw;
          case SDLK_F2:
            if(!m.w) m.w=m.h=pict[sel]->size;
            block_rotate(pict[sel]->data+(m.y+1)*pict[sel]->size+m.x,pict[sel]->size,m.w,m.h,6);
            goto redraw;
          case SDLK_F3:
            if(!m.w) m.w=m.h=pict[sel]->size;
            block_rotate(pict[sel]->data+(m.y+1)*pict[sel]->size+m.x,pict[sel]->size,m.w,m.h,2);
            goto redraw;
          case SDLK_F4:
            if(!m.w) m.w=m.h=pict[sel]->size;
            block_rotate(pict[sel]->data+(m.y+1)*pict[sel]->size+m.x,pict[sel]->size,m.w,m.h,1);
            goto redraw;
          case SDLK_F5: resize:
            m.x=m.y=m.w=m.h=0; xx=yy=-1;
            p=(Uint8*)screen_prompt("Size? (1 to 255, or P to paste)");
            if(!p || !*p) goto redraw;
            if(*p=='p' || *p=='P') {
              if(!pclip || pclip->meth!=pclip->size) goto redraw;
              i=pclip->size;
            } else {
              i=strtol(p,0,10);
            }
            if(i<1 || i>255) goto redraw;
            free(pict[sel]);
            pict[sel]=malloc(sizeof(Picture)+(i+1)*i);
            if(!pict[sel]) fatal("Allocation failed\n");
            pict[sel]->size=i;
            if(*p=='p' || *p=='P') memcpy(pict[sel]->data,pclip->data,(i+1)*i);
            else memset(pict[sel]->data,0,(i+1)*i);
            goto redraw;
          case SDLK_F6:
            if(pict[14]) break;
            for(sel=0;sel<15;sel++) if(!pict[sel]) break;
            goto resize;
          case SDLK_F7:
            if(!sel && !pict[1]) break;
            free(pict[sel]);
            for(i=sel;i<15;i++) pict[i]=pict[i+1];
            pict[15]=0;
            goto redraw;
          case SDLK_F8: case SDLK_KP_MULTIPLY:
            m.x=m.y=0;
            m.w=m.h=pict[sel]->size;
            goto redraw;
          case SDLK_F9:
            xx=yy=-1;
            m.x=m.y=m.w=m.h=0;
            pict[sel]=do_import(pict[sel]);
            goto redraw;
          case SDLK_F10:
            do_export(pict[sel]);
            goto redraw;
          case SDLK_LEFT: case SDLK_KP4: --cc; goto redraw;
          case SDLK_RIGHT: case SDLK_KP6: ++cc; goto redraw;
          case SDLK_UP: case SDLK_KP8: cc-=16; goto redraw;
          case SDLK_DOWN: case SDLK_KP2: cc+=16; goto redraw;
          case SDLK_HOME: case SDLK_KP7: cc=0; goto redraw;
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
  Uint8*buf=0;
  size_t size=0;
  FILE*fp;
  int i,n;
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
  fp=open_memstream((char**)&buf,&size);
  if(!fp) fatal("Cannot open memory stream\n");
  for(i=n=0;i<16;i++) if(pict[i]) n++;
  fputc(n,fp);
  for(i=0;i<n;i++) fputc(pict[i]->size,fp);
  for(i=0;i<n>>1;i++) fputc(0,fp);
  for(i=0;i<n;i++) compress_picture(fp,pict[i]);
  fclose(fp);
  if(!buf || !size) fatal("Allocation failed\n");
  *buf|=pict[0]->meth<<4;
  for(i=1;i<n;i++) buf[n+1+((i-1)>>1)]|=pict[i]->meth<<(i&1?0:4);
  for(i=0;i<16;i++) free(pict[i]);
  if(i=sqlite3_prepare_v2(userdb,"UPDATE `PICEDIT` SET `DATA`=?2 WHERE ID=?1;",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    free(buf);
    return;
  }
  sqlite3_bind_int64(st,1,id);
  sqlite3_bind_blob(st,2,buf,size,free);
  i=sqlite3_step(st);
  if(i!=SQLITE_DONE) screen_message(sqlite3_errmsg(userdb));
  sqlite3_finalize(st);
}

static int add_picture(int t) {
  sqlite3_stmt*st;
  const char*s=screen_prompt("Enter name of new picture:");
  int i;
  if(!s || !*s) return 0;
  if(sqlite3_prepare_v2(userdb,"INSERT INTO `PICEDIT`(`NAME`,`TYPE`,`DATA`) SELECT VALID_NAME(?1)||'.IMG',1,X'';",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  sqlite3_bind_text(st,1,s,-1,0);
  i=sqlite3_step(st);
  sqlite3_finalize(st);
  if(i!=SQLITE_DONE) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  edit_picture(sqlite3_last_insert_rowid(userdb));
  return 1;
}

static int delete_picture(void) {
  sqlite3_stmt*st;
  const char*s=screen_prompt("Enter name of picture to delete:");
  int i;
  if(!s || !*s) return 0;
  if(sqlite3_prepare_v2(userdb,"DELETE FROM `PICEDIT` WHERE `NAME`=?1||'.IMG';",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  sqlite3_bind_text(st,1,s,-1,0);
  i=sqlite3_step(st);
  sqlite3_finalize(st);
  if(i!=SQLITE_DONE) {
    screen_message(sqlite3_errmsg(userdb));
    return 0;
  }
  return 1;
}

static void rename_picture(void) {
  sqlite3_stmt*st;
  const char*s=screen_prompt("Old name:");
  int i;
  if(!s || !*s) return;
  if(sqlite3_prepare_v2(userdb,"UPDATE `PICEDIT` SET `NAME`=VALID_NAME(?2)||'.IMG' WHERE `NAME`=?1||'.IMG';",-1,&st,0)) {
    screen_message(sqlite3_errmsg(userdb));
    return;
  }
  sqlite3_bind_text(st,1,s,-1,SQLITE_TRANSIENT);
  s=screen_prompt("New name:");
  if(!s || !*s) {
    sqlite3_finalize(st);
    return;
  }
  sqlite3_bind_text(st,2,s,-1,SQLITE_TRANSIENT);
  i=sqlite3_step(st);
  sqlite3_finalize(st);
  if(i!=SQLITE_DONE) screen_message(sqlite3_errmsg(userdb));
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
  sqlite3_create_function(userdb,"VALID_NAME",1,SQLITE_UTF8|SQLITE_DETERMINISTIC,0,fn_valid_name,0,0);
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
  if(sc>max-screen->h/8+1) sc=max-screen->h/8+1;
  if(sc<0) sc=0;
  sqlite3_bind_int(st,1,screen->h/8-1);
  sqlite3_bind_int(st,2,sc);
  SDL_LockSurface(screen);
  r.x=r.y=0; r.w=screen->w; r.h=screen->h;
  SDL_FillRect(screen,&r,0xF0);
  draw_text(0,0,"<ESC> Save/Quit  <F1> Add  <F2> Delete  <F3> Edit  <F4> Rename",0xF0,0xFB);
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
          case SDLK_HOME: case SDLK_KP7: sc=0; goto redraw;
          case SDLK_UP: case SDLK_KP8: if(sc) --sc; goto redraw;
          case SDLK_DOWN: case SDLK_KP2: ++sc; goto redraw;
          case SDLK_END: case SDLK_KP1: sc=max-screen->h/8+1; goto redraw;
          case SDLK_PAGEUP: case SDLK_KP9: sc-=screen->h/8-1; goto redraw;
          case SDLK_PAGEDOWN: case SDLK_KP3: sc+=screen->h/8-1; goto redraw;
          case SDLK_F1:
            if(max<65535) max+=add_picture(1);
            else screen_message("Too many pictures");
            goto redraw;
          case SDLK_F2:
            max-=delete_picture();
            goto redraw;
          case SDLK_F3:
            *ids=ask_picture_id("Edit:");
            if(*ids) edit_picture(*ids);
            goto redraw;
          case SDLK_F4:
            rename_picture();
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
