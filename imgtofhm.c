#if 0
gcc -s -O2 -o ./imgtofhm -Wno-unused-result -fwrapv imgtofhm.c
exit
#endif

#define _GNU_SOURCE
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  unsigned char size,meth;
  unsigned char data[0xFFFE]; // the first row is all 0, since the compression algorithm requires this
} Picture;

typedef struct {
  unsigned char x[8];
} Color;

static const char default_palette[]=
  "C020FF "
  "000000 222222 333333 444444 555555 666666 777777 888888 999999 AAAAAA BBBBBB CCCCCC DDDDDD EEEEEE FFFFFF "
  "281400 412300 5F3200 842100 A05000 C35F14 E1731E FF8232 FF9141 FFA050 FFAF5F FFBE73 FFD282 FFE191 FFF0A0 "
  "321E1E 412220 5F2830 823040 A03A4C BE4658 E15464 FF6670 FF7F7B FF8E7F FF9F7F FFAF7F FFBF7F FFCF7F FFDF7F "
  "280D0D 401515 602020 802A2A A03535 C04040 E04A4A FF5555 FF6764 FF6F64 FF7584 FF849D FF94B7 FF9FD1 FFAEEA "
  "901400 A02000 B03000 C04000 D05000 E06000 F07000 FF8000 FF9000 FFA000 FFB000 FFC000 FFD000 FFE000 FFF000 "
  "280000 400000 600000 800000 A00000 C00000 E00000 FF0000 FF2828 FF4040 FF6060 FF8080 FFA0A0 FFC0C0 FFE0E0 "
  "280028 400040 600060 800080 A000A0 C000C0 E000E0 FF00FF FF28FF FF40FF FF60FF FF80FF FFA0FF FFC0FF FFE0FF "
  "281428 402040 603060 804080 A050A0 C060C0 E070E0 FF7CFF FF8CFF FF9CFF FFACFF FFBCFF FFCCFF FFDCFF FFECFF "
  "280050 350566 420A7C 4F0F92 5C14A8 6919BE 761ED4 8323EA 9028FF A040FF B060FF C080FF D0A0FF E0C0FF F0E0FF "
  "000028 000040 000060 000080 0000A0 0000C0 0000E0 0000FF 0A28FF 284AFF 466AFF 678AFF 87AAFF A7CAFF C7EBFF "
  "0F1E1E 142323 193232 1E4141 285050 325F5F 377373 418282 469191 50A0A0 5AAFAF 5FC3C3 69D2D2 73E1E1 78F0F0 "
  "002828 004040 006060 008080 00A0A0 00C0C0 00E0E0 00FFFF 28FFFF 40FFFF 60FFFF 80FFFF A0FFFF C0FFFF E0FFFF "
  "002800 004000 006000 008000 00A000 00C000 00E000 00FF00 28FF28 40FF40 60FF60 80FF80 A0FFA0 C0FFC0 E0FFE0 "
  "002110 234123 325F32 418241 50A050 5FC35F 73E173 85FF7A 91FF6E A0FF5F B4FF50 C3FF41 D2FF32 E1FF23 F0FF0F "
  "282800 404000 606000 808000 A0A000 C0C000 E0E000 FFFF00 FFFF28 FFFF40 FFFF60 FFFF80 FFFFA0 FFFFC0 FFFFE0 "
  "442100 00FF55 0055FF FF5500 55FF00 FF0055 5500FF CA8B25 F078F0 F0F078 FF7F00 DD6D01 7AFF00 111111 "
  "000000 0000AA 00AA00 00AAAA AA0000 AA00AA AAAA00 AAAAAA "
  "555555 5555FF 55FF55 55FFFF FF5555 FF55FF FFFF55 FFFFFF "
;

static unsigned char head[16];
static int maxpic,size;
static const char*prefix;
static Color pal[256];
static Picture pic;
static FILE*sizer;
static int total=0;

static ssize_t cookie_write(void*cookie,const char*buf,size_t size) {
  total+=size;
  return size;
}

static int cookie_seek(void*cookie,off64_t*offset,int whence) {
  if(whence==SEEK_SET) total=*offset;
  else if(whence==SEEK_CUR) total+=*offset;
  else return -1;
  *offset=total;
  return 0;
}

static void load_palette(void) {
  int i;
  for(i=0;i<256;i++) {
    sscanf(default_palette+i*7,"%2hhX%2hhX%2hhX ",pal[i].x+0,pal[i].x+2,pal[i].x+4);
    pal[i].x[1]=pal[i].x[0];
    pal[i].x[3]=pal[i].x[2];
    pal[i].x[5]=pal[i].x[4];
    pal[i].x[7]=pal[i].x[6]=i?255:0;
  }
}

static void set_transparency(const char*s) {
  if(*s) switch(strlen(s)) {
    case 6:
      sscanf(s,"%2hhX%2hhX%2hhX",pal->x+0,pal->x+2,pal->x+4);
      pal->x[1]=pal->x[0];
      pal->x[3]=pal->x[2];
      pal->x[5]=pal->x[4];
      pal->x[7]=pal->x[6]=255;
      break;
    case 12:
      sscanf(s,"%2hhX%2hhX%2hhX%2hhX%2hhX%2hhX",pal->x+0,pal->x+1,pal->x+2,pal->x+3,pal->x+4,pal->x+5);
      pal->x[7]=pal->x[6]=255;
      break;
    default: errx(1,"Incorrect transparency specification");
  }
}

static void load_rotate(void) {
  int x,y;
  int m=pic.meth;
  unsigned char*d=pic.data+size;
  static unsigned char buf[255*255];
  unsigned char*p;
  p=buf;
  for(y=0;y<size;y++) for(x=0;x<size;x++) {
    if(m&1) x=size-x-1;
    if(m&2) y=size-y-1;
    *p++=d[m&4?x*size+y:y*size+x];
    if(m&1) x=size-x-1;
    if(m&2) y=size-y-1;
  }
  memcpy(d,buf,size*size);
}

static void out_run(FILE*fp,int ch,int am,const unsigned char*d,int le) {
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

static void compress_picture(FILE*fp) {
  int ps=pic.size;
  int ms=ps*ps;
  const unsigned char*d=pic.data+ps;
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

static void import_one(int numb) {
  int a,b,i,j,m,s,x,y;
  unsigned char buf[8];
  unsigned char*q;
  // Load picture data
  pic.size=size;
  pic.meth=15;
  memset(pic.data,0,255);
  q=pic.data+pic.size;
  for(y=0;y<size;y++) for(x=0;x<size;x++) {
    if(fread(buf,1,8,stdin)<=0) err(1,"I/O error");
    if(buf[6]&0x80) {
      for(i=0;i<255;i++) if(!memcmp(buf,pal[i].x,8)) goto found;
      i=1;
      a=0x300000;
      for(j=1;j<255;j++) {
        b=abs(buf[0]-pal[j].x[0])+abs(buf[2]-pal[j].x[2])+abs(buf[4]-pal[j].x[4]);
        if(b<=a) a=b,i=j;
      }
      found: *q++=i;
    } else {
      *q++=0;
    }
  }
  // Try compression
  m=15;
  s=size*size;
  rewind(sizer);
  for(i=0;i<8;i++) {
    compress_picture(sizer);
    j=ftell(sizer);
    if(j>0 && j<s) s=j,m=i;
    rewind(sizer);
    pic.meth="\1\3\1\7\1\3\1\7"[i];
    load_rotate();
  }
  // Write header
  s+=2;
  printf("%s%d.IMG",prefix,numb);
  putchar(0);
  putchar(s>>020);
  putchar(s>>030);
  putchar(s>>000);
  putchar(s>>010);
  // Write compressed data
  putchar((m<<4)|1);
  putchar(size);
  pic.meth=m;
  load_rotate();
  if(m==15) fwrite(pic.data+pic.size,pic.size,pic.size,stdout); else compress_picture(stdout);
}

int main(int argc,char**argv) {
  int i;
  if(argc!=3 && argc!=4) errx(1,"Incorrect number of arguments");
  maxpic=strtol(argv[1],0,10);
  prefix=argv[2];
  load_palette();
  if(argc==4) set_transparency(argv[3]);
  sizer=fopencookie("","w+",(cookie_io_functions_t){.write=cookie_write,.seek=cookie_seek});
  if(!sizer) err(1,"Allocation failed");
  fread(head,1,16,stdin);
  if(memcmp(head,"farbfeld",8)) errx(1,"Unrecognized input format");
  if(head[8]|head[9]|head[10]) errx(1,"Picture is too big");
  size=head[11];
  if(!size) errx(1,"Wrong horizontal size");
  i=(head[12]<<030)|(head[13]<<020)|(head[14]<<010)|(head[15]<<000);
  if(!i) errx(1,"Wrong vertical size");
  if(maxpic>i/size || maxpic<=0) {
    maxpic=i/size;
    if(i%size) errx(1,"Wrong vertical size");
  }
  for(i=0;i<maxpic;i++) import_one(i);
  return 0;
}
