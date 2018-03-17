#if 0
gcc -s -O2 -o ./mbtofhm -Wno-unused-result mbtofhm.c
exit
#endif

// This program is part of Free Hero Mesh and is public domain.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "names.h"

#define fatal(...) do{ fprintf(stderr,__VA_ARGS__); exit(1); }while(0)

// Pictures
static unsigned char*pict;
static unsigned short*picalloc;
static int npic;
static unsigned short sizes[8];
static unsigned char*inpic;
static unsigned char*outpic;
static unsigned char*inpicstr;
static unsigned char*inpicabovestr;
static int outpiclen;
static char picavail[512];

// Classes
typedef struct {
  char*name;
  char*desc;
  unsigned char attr[62];
  short npic;
  unsigned short pic[128];
  short nvars;
  unsigned char*vars;
  short nsubslbl;
  unsigned char*subslbl;
  unsigned char*subscode;
  short nmsgslbl;
  unsigned char*msgslbl;
  unsigned short nmsgs;
  unsigned char**msgscode;
  short defpic;
} ClassInfo;
static int nusermsg;
static char**usermsg;
static ClassInfo*class[512];

// Levels
static int nlevels;
static unsigned short*levelid;

// Sounds
static int nsounds;
static char**sounds;
static unsigned short*soundid;

// Common
static char*basename;
static char*nam;
static unsigned long haroffs;

static void*alloc(unsigned int s) {
  void*o=malloc(s);
  if(s && !o) fatal("Allocation failed\n");
  return o;
}
static void*my_realloc(void*p,unsigned int s) {
  void*o=realloc(p,s);
  if(s && !o) fatal("Reallocation failed\n");
  return o;
}
#define Allocate(x,y) (x=alloc((y)*sizeof(*(x))))
#define Reallocate(x,y) (x=my_realloc((x),(y)*sizeof(*(x))))

static void hamarc_begin(FILE*fp) {
  fwrite(nam,1,strlen(nam)+1,fp);
  fwrite("\0\0\0",4,1,fp);
  haroffs=ftell(fp);
}

static void hamarc_end(FILE*fp) {
  unsigned long end=ftell(fp);
  fseek(fp,haroffs-4,SEEK_SET);
  fputc((end-haroffs)>>16,fp);
  fputc((end-haroffs)>>24,fp);
  fputc((end-haroffs)>>0,fp);
  fputc((end-haroffs)>>8,fp);
  fseek(fp,end,SEEK_SET);
}

static inline void read_header(void) {
  unsigned char buf[30];
  int i,s;
  fread(buf,1,30,stdin);
  if(memcmp("MESH",buf+2,4)) fatal("Unrecognized file format\n");
  for(i=0;i<buf[10];i++) sizes[i]=buf[i+i+12]|(buf[i+i+13]<<8);
  npic=buf[28]|(buf[29]<<8);
  Allocate(picalloc,npic);
  for(i=0;i<npic;i++) {
    fread(buf,1,2,stdin);
    picalloc[i]=buf[0]|(buf[1]<<8);
  }
  for(i=s=0;i<8;i++) s+=sizes[i]*sizes[i];
  Allocate(pict,i=s*npic*16);
  fread(pict,1,i,stdin);
  for(i=s=0;i<8;i++) if(s<sizes[i]) s=sizes[i];
  s*=s;
  Allocate(inpic,s);
  Allocate(outpic,s+256);
  Allocate(inpicstr,s+4);
  Allocate(inpicabovestr,s+4);
}

static void out_run(int ch,int am,int st,int le) {
  int n=am%85?:85;
  outpic[outpiclen++]=ch+n-1;
  if(le) memcpy(outpic+outpiclen,inpicstr+st,le<n?le:n);
  if(le) outpiclen+=le<n?le:n;
  if(ch) st+=n,le-=n;
  while(am-=n) {
    n=am>7225?7225:am;
    outpic[outpiclen++]=ch+n/85-1;
    if(le>0) memcpy(outpic+outpiclen,inpicstr+st,le<n?le:n);
    if(le>0) outpiclen+=le<n?le:n;
    if(ch) st+=n,le-=n;
  }
}

static int one_picture(int ps,int me) {
  int ms=ps*ps;
  int i=0;
  int w,x,y,z;
  int ca,homo,hetero;
  for(y=0;y<ps;y++) for(x=0;x<ps;x++) {
    if(me&1) x=ps-x-1;
    if(me&2) y=ps-y-1;
    inpicstr[i++]=inpic[me&4?x*ps+y:y*ps+x];
    if(me&1) x=ps-x-1;
    if(me&2) y=ps-y-1;
  }
  for(i=0;i<ms;i++) inpicabovestr[i]=i<ps?0:inpicstr[i-ps];
  inpicabovestr[i]=255; inpicstr[i++]=254;
  inpicabovestr[i]=253; inpicstr[i++]=252;
  outpiclen=i=0;
  while(i<ms) {
    ca=0;
    while(i+ca<ms && inpicstr[i+ca]==inpicabovestr[i+ca]) ca++;
    homo=1;
    while(i+homo<ms && inpicstr[i+homo]==inpicstr[i]) homo++;
    hetero=1;
    while(i+hetero<ms) {
      if(inpicstr[i+hetero]==inpicabovestr[i+hetero] && inpicstr[i+hetero+1]==inpicabovestr[i+hetero+1]) break;
      if(inpicstr[i+hetero]==inpicstr[i+hetero+1] && i+hetero==ms-1) break;
      if(inpicstr[i+hetero]==inpicstr[i+hetero+1] && inpicstr[i+hetero]==inpicstr[i+hetero+2]) break;
      hetero++;
    }
    if(ca>=homo && (ca>=hetero || homo>1)) {
      // Output a copy-above run
      out_run(170,ca,0,0);
      i+=ca;
    } else if(homo>1) {
      // Output a homogeneous run
      out_run(0,homo-1,i,1);
      i+=homo;
    } else {
      // Output a heterogeneous run
      if(hetero>85) hetero=85;
      out_run(85,hetero,i,hetero);
      i+=hetero;
    }
    if(outpiclen>=ms) return ms+1;
  }
  return outpiclen;
}

static inline void do_pictures(void) {
  FILE*fp;
  int i,j,k,s,t,x,y,z;
  char meth[8];
  int siz[8];
  sprintf(nam,"%s.xclass",basename);
  fp=fopen(nam,"w");
  if(!fp) fatal("Cannot open file '%s' for writing\n",nam);
  for(i=0;i<npic;i++) {
    for(j=0;j<16;j++) {
      if(picalloc[i]&(1<<j)) {
        fwrite(nam,sprintf(nam,"%d.IMG",i*16+j)+1,1,fp);
        for(z=k=t=0;z<8;z++) {
          x=sizes[z];
          if(!x) break;
          for(y=0;y<x;y++) memcpy(inpic+y*x,pict+k+(i*x+y)*x*16+j*x,x);
          meth[z]=15;
          siz[z]=x*x;
          for(y=0;y<8;y++) {
            s=one_picture(x,y);
            if(s<siz[z]) siz[z]=s,meth[z]=y;
          }
          t+=siz[z];
          k+=x*x*16*npic;
        }
        if(z!=8) meth[z]=15;
        t+=z+1+(z>>1);
        fputc(t>>16,fp); fputc(t>>24,fp); fputc(t,fp); fputc(t>>8,fp);
        fputc((*meth<<4)|z,fp);
        for(z=0;z<8;z++) {
          if(!sizes[z]) break;
          fputc(sizes[z],fp);
        }
        for(z=1;z<8;z+=2) {
          if(!sizes[z]) break;
          fputc(meth[z]|(z==7?0xF0:meth[z+1]<<4),fp);
        }
        for(z=k=0;z<8;z++) {
          x=sizes[z];
          if(!x) break;
          for(y=0;y<x;y++) memcpy(inpic+y*x,pict+k+(i*x+y)*x*16+j*x,x);
          if(meth[z]==15) fwrite(inpic,x,x,fp);
          else fwrite(outpic,1,one_picture(x,meth[z]),fp);
          k+=x*x*16*npic;
        }
      }
    }
  }
  fclose(fp);
}

static void unprefix_messages(const char*pre,int n) {
  int i,j;
  for(i=0;i<nusermsg;i++) {
    if(usermsg[i] && !strncmp(pre,usermsg[i],n)) {
      for(j=0;j<nusermsg;j++) if(j!=i && usermsg[j] && !strcmp(usermsg[i],usermsg[j])) goto skip;
      memmove(usermsg[i],usermsg[i]+n,strlen(usermsg[i])+1-n);
    }
    skip: ;
  }
}

static inline void do_user_messages(void) {
  int i,j;
  i=fgetc(stdin);
  nusermsg=i|(fgetc(stdin)<<8);
  Allocate(usermsg,nusermsg);
  for(i=0;i<nusermsg;i++) {
    j=fgetc(stdin);
    if(j) {
      Allocate(usermsg[i],j+1);
      fread(usermsg[i],1,j,stdin);
      usermsg[i][j]=0;
    } else {
      usermsg[i]=0;
    }
  }
  unprefix_messages("MSG_",4);
  unprefix_messages("USR_",4);
  unprefix_messages("WTP_",4);
  unprefix_messages("MSG_",4);
}

static void read_one_class(void) {
  unsigned char buf[4];
  int i,j,k,id;
  char*name;
  i=fgetc(stdin);
  Allocate(name,i+1);
  fread(name,1,i,stdin);
  name[i]=0;
  id=fgetc(stdin);
  id|=fgetc(stdin)<<8;
  if(id>=512) fatal("Class number %d out of range\n",id);
  if(class[id]) fatal("Duplicate class number %d\n",id);
  Allocate(class[id],1);
  class[id]->name=name;
  i=fgetc(stdin);
  if(i==255) {
    i=fgetc(stdin);
    i|=fgetc(stdin)<<8;
  }
  fread(Allocate(class[id]->desc,i+1),1,i,stdin);
  class[id]->desc[i]=0;
  fread(class[id]->attr,1,62,stdin);
  j=fgetc(stdin);
  j|=fgetc(stdin)<<8;
  class[id]->npic=j;
  class[id]->defpic=256;
  for(i=0;i<j;i++) {
    k=fgetc(stdin);
    k|=fgetc(stdin)<<8;
    if(i<128) {
      class[id]->pic[i]=k;
      if(k<512 && class[id]->defpic==256 && picavail[k]) class[id]->defpic=i;
    }
  }
  class[id]->defpic&=255;
  if(j>128) class[id]->npic=128;
  j=fgetc(stdin);
  j|=fgetc(stdin)<<8;
  class[id]->nvars=j;
  fread(Allocate(class[id]->vars,j<<3),j,8,stdin);
  j=fgetc(stdin);
  j|=fgetc(stdin)<<8;
  class[id]->nsubslbl=j;
  fread(Allocate(class[id]->subslbl,10*j),j,10,stdin);
  fread(buf,1,2,stdin);
  k=buf[0]|(buf[1]<<8);
  Allocate(class[id]->subscode,k<<1);
  class[id]->subscode[0]=i;
  class[id]->subscode[1]=j;
  fread(class[id]->subscode+2,2,k-1,stdin);
  j=fgetc(stdin);
  j|=fgetc(stdin)<<8;
  class[id]->nmsgslbl=j;
  fread(Allocate(class[id]->msgslbl,10*j),j,10,stdin);
  class[id]->nmsgs=0;
  class[id]->msgscode=0;
  for(;;) {
    fread(buf,1,4,stdin);
    k=buf[2]|(buf[3]<<8);
    if(!k) break;
    i=class[id]->nmsgs++;
    Allocate(Reallocate(class[id]->msgscode,class[id]->nmsgs)[i],k<<1);
    memcpy(class[id]->msgscode[i],buf,4);
    fread(class[id]->msgscode[i]+4,k-2,2,stdin);
  }
}

static inline void do_classes(void) {
  int n;
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  while(n--) read_one_class();
}

static void out_description(FILE*fp,const char*txt) {
  // CLASS section
  if(!strncmp("[CLASS]\r\n",txt,9)) txt+=9;
  while(txt[0]=='\r' && txt[1]=='\n') txt+=2;
  if(!strncmp("[LEVEL]\r\n",txt,9)) goto level_area;
  if(!strncmp("[GAME]\r\n",txt,8)) goto game_area;
  fprintf(fp,"  ; ");
  for(;;) {
    if(*txt=='\r') {
      if(!strncmp("\r\n[LEVEL]\r\n",txt,11)) {
        fputc('\n',fp);
        txt+=11;
        goto level_area;
      } else if(!strncmp("\r\n[GAME]\r\n",txt,10)) {
        fputc('\n',fp);
        txt+=10;
        goto game_area;
      } else {
        fprintf(fp,"\n  ; ");
        txt+=2;
      }
    } else if(!*txt) {
      fputc('\n',fp);
      return;
    } else {
      fputc(*txt++,fp);
    }
  }
  // LEVEL section
  level_area:
  if(!strncmp("[LEVEL]\r\n",txt,9)) txt+=9;
  while(txt[0]=='\r' && txt[1]=='\n') txt+=2;
  if(!strncmp("[GAME]\r\n",txt,8)) goto game_area;
  fprintf(fp,"  (EditorHelp\n    \"");
  for(;;) {
    if(*txt=='\r') {
      if(!strncmp("\r\n[GAME]\r\n",txt,10)) {
        fprintf(fp,"\"\n  )\n");
        txt+=10;
        goto game_area;
      } else {
        fprintf(fp,"\"\n    \"");
        txt+=2;
      }
    } else if(!*txt) {
      fprintf(fp,"\"\n  )\n");
      return;
    } else {
      if(*txt=='"') fputc('\\',fp);
      fputc(*txt++,fp);
    }
  }
  // GAME section
  game_area:
  if(!strncmp("[GAME]\r\n",txt,8)) txt+=8;
  while(txt[0]=='\r' && txt[1]=='\n') txt+=2;
  fprintf(fp,"  (Help\n    \"");
  for(;;) {
    if(*txt=='\r') {
      if(txt[1]=='\n' && !txt[2]) {
        fprintf(fp,"\"\n  )\n");
        return;
      } else {
        fprintf(fp,"\"\n    \"");
        txt+=2;
      }
    } else if(!*txt) {
      fprintf(fp,"\"\n  )\n");
      return;
    } else {
      if(*txt=='"') fputc('\\',fp);
      fputc(*txt++,fp);
    }
  }
}

static inline void out_classes(void) {
  FILE*fp;
  ClassInfo*c;
  int i,j,n;
  sprintf(nam,"%s.class",basename);
  fp=fopen(nam,"w");
  if(!fp) fatal("Cannot open file '%s' for writing\n",nam);
  fprintf(fp,"; Note: This file was automatically converted.\n; Remove this notice if you have corrected it.\n");
  for(n=0;n<512;n++) if(c=class[n]) {
    fprintf(fp,"\n($%s Compatible",c->name);
    if(c->attr[1]&1) fprintf(fp," Input");
    if(c->attr[1]&128) fprintf(fp," Player");
    if(!strcmp(c->name,"Quiz")) fprintf(fp," Quiz");
    fprintf(fp,"\n  (Image");
    for(j=i=0;i<c->npic;i++) {
      fprintf(fp," \"%d\"",c->pic[i]);
      if(picavail[c->pic[i]]) j++;
    }
    fprintf(fp,")\n");
    if(!j) {
      fprintf(fp,"  (DefaultImage ())\n");
    } else if(j!=c->npic) {
      fprintf(fp,"  (DefaultImage");
      for(i=0;i<c->npic;i++) if(picavail[c->pic[i]]) fprintf(fp," %d",i);
      fprintf(fp,")\n");
    }
    out_description(fp,c->desc);
    if(c->attr[2] || c->attr[3]) fprintf(fp,"  (Misc4 0x%04X)\n",c->attr[2]|(c->attr[3]<<8));
    if(c->attr[4] || c->attr[5]) fprintf(fp,"  (Misc5 0x%04X)\n",c->attr[4]|(c->attr[5]<<8));
    if(c->attr[6] || c->attr[7]) fprintf(fp,"  (Misc6 0x%04X)\n",c->attr[6]|(c->attr[7]<<8));
    if(c->attr[8] || c->attr[9]) fprintf(fp,"  (Misc7 0x%04X)\n",c->attr[8]|(c->attr[9]<<8));
    if(c->attr[50] || c->attr[51]) fprintf(fp,"  (Temperature %d)\n",c->attr[50]|(c->attr[51]<<8));
    if(c->attr[10]) {
      if(c->attr[10]==0x55) {
        fprintf(fp,"  Shovable\n");
      } else {
        fprintf(fp,"  (Shovable");
        if(c->attr[10]&0xAA) {
          // I don't know why, but sometimes this happens.
          fprintf(fp," 0x%02X",c->attr[10]);
        } else {
          if(c->attr[10]&0x01) fprintf(fp," E");
          if(c->attr[10]&0x04) fprintf(fp," N");
          if(c->attr[10]&0x10) fprintf(fp," W");
          if(c->attr[10]&0x40) fprintf(fp," S");
        }
        fprintf(fp,")\n");
      }
    }
    if(!c->attr[12] && !c->attr[13] && !c->attr[15] && !c->attr[16] && !(c->attr[14]&~4)) {
      if(c->attr[14]) fprintf(fp,"  (Arrivals InPlace)\n");
    } else {
      fprintf(fp,"  (Arrivals\n");
      for(i=0;i<5;i++) {
        fprintf(fp,"    ");
        for(j=0;j<5;j++) fprintf(fp," %c",(c->attr[i+12]<<j)&16?'1':'0');
        fputc('\n',fp);
      }
      fprintf(fp,"  )\n");
    }
    if(!c->attr[17] && !c->attr[18] && !c->attr[20] && !c->attr[21] && !(c->attr[19]&~4)) {
      if(c->attr[19]) fprintf(fp,"  (Departures InPlace)\n");
    } else {
      fprintf(fp,"  (Departures\n");
      for(i=0;i<5;i++) {
        fprintf(fp,"    ");
        for(j=0;j<5;j++) fprintf(fp," %c",(c->attr[i+17]<<j)&16?'1':'0');
        fputc('\n',fp);
      }
      fprintf(fp,"  )\n");
    }
    if(c->attr[22] || c->attr[23]) fprintf(fp,"  (Density %d)\n",c->attr[22]|(c->attr[23]<<8));
    if(c->attr[24] || c->attr[25]) fprintf(fp,"  (Volume %d)\n",c->attr[24]|(c->attr[25]<<8));
    if(c->attr[26] || c->attr[27]) fprintf(fp,"  (Strength %d)\n",c->attr[26]|(c->attr[27]<<8));
    if(c->attr[28] || c->attr[29]) fprintf(fp,"  (Weight %d)\n",c->attr[28]|(c->attr[29]<<8));
    if(c->attr[46] || c->attr[47]) fprintf(fp,"  (Height %d)\n",c->attr[46]|(c->attr[47]<<8));
    if(c->attr[48] || c->attr[49]) fprintf(fp,"  (Climb %d)\n",c->attr[48]|(c->attr[49]<<8));
    switch(i=c->attr[52]) {
      case 0x00:
        // Nothing
        break;
      case 0x55: case 0xAA: case 0xFF:
        fprintf(fp,"  (Shape %d)\n",i&3);
        break;
      default:
        fprintf(fp,"  (Shape");
        if(i&0x03) fprintf(fp," (E %d)",(i>>0)&3);
        if(i&0x0C) fprintf(fp," (N %d)",(i>>2)&3);
        if(i&0x30) fprintf(fp," (W %d)",(i>>4)&3);
        if(i&0xC0) fprintf(fp," (S %d)",(i>>6)&3);
        fprintf(fp,")\n");
    }
    if(memcmp(c->attr+30,"\0\0\0\0\0\0\0\0",8)) {
      if(memcmp(c->attr+30,c->attr+32,2) || memcmp(c->attr+30,c->attr+34,4)) {
        fprintf(fp,"  (Hard");
        if(c->attr[30] || c->attr[31]) fprintf(fp," (E %d)",c->attr[30]|(c->attr[31]<<8));
        if(c->attr[32] || c->attr[33]) fprintf(fp," (N %d)",c->attr[32]|(c->attr[33]<<8));
        if(c->attr[34] || c->attr[35]) fprintf(fp," (W %d)",c->attr[34]|(c->attr[35]<<8));
        if(c->attr[36] || c->attr[37]) fprintf(fp," (S %d)",c->attr[36]|(c->attr[37]<<8));
        fprintf(fp,")\n");
      } else {
        fprintf(fp,"  (Hard %d)\n",c->attr[30]|(c->attr[31]<<8));
      }
    }
    if(memcmp(c->attr+38,"\0\0\0\0\0\0\0\0",8)) {
      if(memcmp(c->attr+38,c->attr+40,2) || memcmp(c->attr+38,c->attr+42,4)) {
        fprintf(fp,"  (Sharp");
        if(c->attr[38] || c->attr[39]) fprintf(fp," (E %d)",c->attr[38]|(c->attr[39]<<8));
        if(c->attr[40] || c->attr[41]) fprintf(fp," (N %d)",c->attr[40]|(c->attr[41]<<8));
        if(c->attr[42] || c->attr[43]) fprintf(fp," (W %d)",c->attr[42]|(c->attr[43]<<8));
        if(c->attr[44] || c->attr[45]) fprintf(fp," (S %d)",c->attr[44]|(c->attr[45]<<8));
        fprintf(fp,")\n");
      } else {
        fprintf(fp,"  (Sharp %d)\n",c->attr[38]|(c->attr[39]<<8));
      }
    }
    //TODO: Class codes
    if(c->subscode[1] || c->subscode[0]>2) {
      fprintf(fp,"  (SUBS\n    ; Not implemented yet!\n  )\n");
    }
    for(i=0;i<c->nmsgs;i++) {
      fprintf(fp,"  (");
      j=c->msgscode[i][0]|(c->msgscode[i][1]<<8);
      if(j<20) fprintf(fp,"%s",standard_message_names[j]);
      else if(j-20<nusermsg && usermsg[j-20]) fprintf(fp,"#%s",usermsg[j-20]);
      else fprintf(fp,"#%d?",j);
      fprintf(fp,"\n    ; Not implemented yet!\n  )\n");
    }
    fprintf(fp,")\n");
  }
  fclose(fp);
}

static void one_level(FILE*fp,int ord) {
  unsigned char buf[16];
  unsigned char mru[32];
  int i,j,x,y,n,q,r;
  i=fgetc(stdin);
  i|=fgetc(stdin)<<8;
  levelid[ord]=i;
  sprintf(nam,"%d.LVL",i);
  hamarc_begin(fp);
  fputc(0,fp); fputc(0,fp); // Level version
  fputc(ord,fp); fputc(ord>>8,fp); // Level code
  fputc(28,fp); fputc(20,fp); // One less than width/height
  // (Width/height can each be up to 64 (stored as 63). Bit6 and bit7 of
  //  these numbers are extra header flags, currently unused.)
  i=fgetc(stdin);
  i|=fgetc(stdin)<<8;
  // fputc(17,fp); // Select proportional font
  while(i--) {
    switch(j=fgetc(stdin)) {
      case 14:
        i-=4;
        j=fgetc(stdin);
        j|=fgetc(stdin)<<8;
        if(j>=512 || !class[j]) {
          fgetc(stdin); fgetc(stdin);
          break;
        }
        fputc(14,fp);
        fputs(class[j]->name,fp);
        j=fgetc(stdin);
        j|=fgetc(stdin)<<8;
        fprintf(fp,":%d\\",j);
        break;
      default: fputc(j,fp);
    }
  }
  fread(mru,1,32,stdin); // Skip border colours
  fread(buf,1,2,stdin); // Skip border colours
  mru[0x04]=mru[0x14]=255;
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  x=q=r=0;
  y=1;
  // Free Hero Mesh level format for objects:
  //   * bit flags (or 0xFF for end):
  //     bit7 = MRU (omit everything but position)
  //     bit6 = Next position
  //     bit5 = New X position
  //     bit4 = New Y position
  //     bit3 = Has MiscVars (RLE in case of MRU)
  //     bit2-bit0 = LastDir (RLE in case of MRU)
  //   * new X if applicable
  //   * new Y if applicable
  //   * class (one-based; add 0x8000 for default image) (two bytes)
  //   * image (one byte)
  //   * data types (if has MiscVars):
  //     bit7-bit6 = How many (0=has Misc2 and Misc3, not Misc1)
  //     bit5-bit4 = Misc3 type
  //     bit3-bit2 = Misc2 type
  //     bit1-bit0 = Misc1 type
  //   * misc data (variable size)
  while(n--) {
    fread(buf,1,16,stdin);
    ++*buf;
    if(!*buf) ++buf[1];
    mru[0x06]=mru[0x16]=buf[6];
    mru[0x08]=mru[0x18]=buf[8];
    i=0;
    if(buf[6]==x+1) i|=0x40; else if(buf[6]!=x) i|=0x20;
    if(buf[8]!=y) i|=0x10;
    if(x==buf[6] && y==buf[8]) q+=16; else q=0;
    if(q<32 && !memcmp(mru+q,buf,16)) {
      i|=0x80;
    } else {
      i|=buf[4]&7;
      if(buf[5] || memcmp(buf+10,"\0\0\0\0\0\0",6)) i|=0x08;
      if(q<32) memcpy(mru+q,buf,16);
    }
    x=buf[6];
    y=buf[8];
    if(i==0xC0) {
      if(++r==16) fputc(r+0xBF,fp),r=0;
      continue;
    } else if(r) {
      fputc(r+0xBF,fp),r=0;
    }
    fputc(i,fp);
    if(i&0x20) fputc(x,fp);
    if(i&0x10) fputc(y,fp);
    if(i<0x80) {
      j=buf[0]|(buf[1]<<8);
      if(!j || j>512 || !class[j-1]) fatal("Object of invalid class number %d found in level\n",j);
      if(buf[2]==class[j-1]->defpic) {
        fputc(*buf,fp);
        fputc(buf[1]|0x80,fp);
      } else {
        fwrite(buf,1,3,fp);
      }
    }
    if(i&0x08) {
      i=buf[5]&0x3F;
      if(buf[14] || buf[15]) i|=0xC0;
      else if(buf[12] || buf[13]) i|=0x80;
      else i|=0x40;
      if(i>=0xC0 && !buf[10] && !buf[11]) i&=0x3F;
      if(i&0xC0) {
        if((i&0x03)==0x02 && (buf[11] || buf[10]>20)) {
          j=buf[10]|(buf[11]<<8);
          fputc(j+236,fp); fputc((j+236)>>8,fp);
        } else {
          fwrite(buf+10,1,2,fp);
        }
      }
      if((i&0xC0)!=0x40) {
        if((i&0x0C)==0x08 && (buf[13] || buf[12]>20)) {
          j=buf[12]|(buf[13]<<8);
          fputc(j+236,fp); fputc((j+236)>>8,fp);
        } else {
          fwrite(buf+12,1,2,fp);
        }
      }
      if(!(0xC0&~i) || !(i&0xC0)) {
        if((i&0x03)==0x02 && (buf[15] || buf[14]>20)) {
          j=buf[14]|(buf[15]<<8);
          fputc(j+236,fp); fputc((j+236)>>8,fp);
        } else {
          fwrite(buf+14,1,2,fp);
        }
      }
    }
  }
  if(r) fputc(r+0xBF,fp);
  fputc(255,fp); // End of objects
  //TODO: Long alternating sequences of 0xC0 0x80 are common, so we should
  // implement compression of this.
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  while(n--) {
    i=fgetc(stdin);
    i|=fgetc(stdin)<<8;
    // fputc(17,fp); // Select proportional font
    while(i--) {
      switch(j=fgetc(stdin)) {
        case 14:
          i-=4;
          j=fgetc(stdin);
          j|=fgetc(stdin)<<8;
          if(j>=512 || !class[j]) {
            fgetc(stdin); fgetc(stdin);
            break;
          }
          fputc(14,fp);
          fputs(class[j]->name,fp);
          j=fgetc(stdin);
          j|=fgetc(stdin)<<8;
          fprintf(fp,":%d\\",j);
          break;
        default: fputc(j,fp);
      }
    }
  }
  hamarc_end(fp);
}

static inline void do_levels(void) {
  int i,n;
  FILE*fp;
  sprintf(nam,"%s.level",basename);
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  fgetc(stdin); fgetc(stdin); // Unknown meaning
  if(!n) return;
  nlevels=n;
  Allocate(levelid,nlevels);
  fp=fopen(nam,"w");
  if(!fp) fatal("Cannot open file '%s' for writing\n",nam);
  strcpy(nam,"CLASS.DEF");
  hamarc_begin(fp);
  for(i=0;i<512;i++) if(class[i]) {
    fputc(i+1,fp); fputc((i+1)>>8,fp);
    fwrite(class[i]->name,1,strlen(class[i]->name)+1,fp);
  }
  fputc(0,fp); fputc(0,fp);
  for(i=0;i<nusermsg;i++) if(usermsg[i]) {
    fputc(i,fp); fputc((i+256)>>8,fp);
    fwrite(usermsg[i],1,strlen(usermsg[i])+1,fp);
  }
  hamarc_end(fp);
  for(i=0;i<n;i++) one_level(fp,i);
  strcpy(nam,"LEVEL.IDX");
  hamarc_begin(fp);
  for(i=0;i<n;i++) {
    fputc(levelid[i],fp);
    fputc(levelid[i]>>8,fp);
  }
  hamarc_end(fp);
  fclose(fp);
}

static void one_solution(FILE*fp) {
  char buf[9];
  int i,n;
  i=fgetc(stdin);
  i|=fgetc(stdin)<<8;
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  sprintf(nam,"%d.SOL",i);
  hamarc_begin(fp);
  fputc(0,fp); fputc(0,fp); // Level version
  fread(buf,1,9,stdin);
  if(*buf) {
    fputc(1,fp); // bit0=has user name, bit1=has timestamp
    for(i=0;i<9;i++) {
      fputc(buf[i],fp);
      if(!buf[i]) break;
    }
  } else {
    fputc(0,fp); // has neither user name nor timestamp
  }
  while(n--) fputc(fgetc(stdin),fp);
  hamarc_end(fp);
}

static inline void do_solutions(void) {
  int n;
  FILE*fp;
  sprintf(nam,"%s.solution",basename);
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  if(!n) return;
  fp=fopen(nam,"w");
  if(!fp) fatal("Cannot open file '%s' for writing\n",nam);
  while(n--) one_solution(fp);
  fclose(fp);
}

static inline void do_user_sounds(void) {
  int i,j,n;
  FILE*fp;
  n=fgetc(stdin);
  n|=fgetc(stdin)<<8;
  if(!n) return;
  nsounds=n;
  sprintf(nam,"%s.xclass",basename);
  fp=fopen(nam,"r+");
  if(!fp) fatal("Cannot open file '%s' for reading/writing\n",nam);
  fseek(fp,0,SEEK_END);
  Allocate(sounds,n);
  Allocate(soundid,n);
  while(n--) {
    i=fgetc(stdin);
    i|=fgetc(stdin)<<8;
    j=fgetc(stdin);
    fread(nam,1,j,stdin);
    nam[j]=0;
    sounds[n]=strdup(nam);
    if(!sounds[n]) fatal("String duplication failed\n");
    strcpy(nam+j,".WAV");
    soundid[n]=i;
    hamarc_begin(fp);
    i=fgetc(stdin);
    i|=fgetc(stdin)<<8;
    while(i--) fputc(fgetc(stdin),fp);
    hamarc_end(fp);
  }
  fclose(fp);
}

int main(int argc,char**argv) {
  if(argc!=2) fatal("usage: %s basename < inputfile\n",argv[0]);
  basename=argv[1];
  Allocate(nam,strlen(basename)+256);
  read_header();
  do_pictures();
  fread(picavail,1,512,stdin);
  do_user_messages();
  do_classes();
  do_levels();
  if(nlevels) do_solutions();
  do_user_sounds();
  out_classes();
  return 0;
}
