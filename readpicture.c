#if 0
gcc -s -O2 -o ./readpicture -Wno-unused-result readpicture.c
exit
#endif

// This program is part of Free Hero Mesh and is public domain.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned char x[8];
} Color;

static unsigned char buf[32];
static int size,num,meth;
static Color pal[256];
static unsigned char*pic;

static inline void load_palette(void) {
  int i;
  FILE*fp=fopen("colorpalette","r");
  if(!fp) exit(1);
  for(i=0;i<256;i++) {
    fscanf(fp,"%2hhx%2hhx%2hhx",pal[i].x+0,pal[i].x+2,pal[i].x+4);
    pal[i].x[1]=pal[i].x[0];
    pal[i].x[3]=pal[i].x[2];
    pal[i].x[5]=pal[i].x[4];
    pal[i].x[7]=pal[i].x[6]=i?255:0;
  }
  fclose(fp);
}

static void load_picture(int id) {
  int c,n,t,x,y;
  unsigned char*p;
  meth=(id?buf[(*buf&15)+1+((id-1)>>1)]>>(id&1?0:4):*buf>>4)&15;
  size=buf[id+1];
  pic=realloc(pic,size*size);
  if(!pic) {
    fprintf(stderr,"Allocation failed\n");
    exit(1);
  }
  if(meth==15) {
    fread(pic,size,size,stdin);
    return;
  }
  p=pic;
  n=t=0;
  y=size*size;
  while(y--) {
    if(!n) {
      n=fgetc(stdin);
      if(n<85) {
        // Homogeneous run
        n++;
        x=fgetc(stdin);
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
    if(t==2) c=fgetc(stdin);
    if(t==3) c=p-pic>=size?p[-size]:0;
    *p++=c;
  }
}

static inline void out_picture(void) {
  int x,y;
  //fprintf(stderr,"%d\n",meth);
  if(meth==5 || meth==6) meth^=3;
  for(y=0;y<size;y++) for(x=0;x<size;x++) {
    if(meth&1) x=size-x-1;
    if(meth&2) y=size-y-1;
    fwrite(pal[pic[meth&4?x*size+y:y*size+x]].x,1,8,stdout);
    if(meth&1) x=size-x-1;
    if(meth&2) y=size-y-1;
  }
}

int main(int argc,char**argv) {
  int i;
  if(argc!=2) return 1;
  load_palette();
  num=strtol(argv[1],0,0);
  *buf=fgetc(stdin);
  i=*buf&15;
  fread(buf+1,1,i+(i>>1),stdin);
  for(i=0;i<=num;i++) load_picture(i);
  fwrite("farbfeld",1,8,stdout);
  putchar(0); putchar(0);
  putchar(size>>8); putchar(size);
  putchar(0); putchar(0);
  putchar(size>>8); putchar(size);
  out_picture();
  return 0;
}

