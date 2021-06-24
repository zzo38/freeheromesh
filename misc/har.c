#if 0
gcc -s -O2 -o ~/bin/har -Wno-unused-result har.c
exit
#endif

/*
  Hamster archiver
  Public domain

  c = Create
  d = Delete
  l = Lowercase
  o = Extract to stdout
  r = Rename
  s = Safe
  t = List
  u = Uppercase
  x = Extract
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define fatal(...) do { fprintf(stderr,__VA_ARGS__); putchar('\n'); exit(1); } while(0)

static const char safe[128]={
  [0]=1,
  ['0'...'9']=1,
  ['A'...'Z']=1,
  ['a'...'z']=1,
  ['-']=1,
  ['_']=1,
  ['.']=1,
  ['/']=1,
  ['+']=1,
};

static char opt[128];
static char name[256];
static unsigned long size;
static int argc;
static char**argv;

static void process_lump(int a) {
  int n=0;
  int d=1;
  int c,o;
  FILE*fp;
  FILE*in;
  if(opt['c']) {
    strcpy(name,argv[a]);
    in=fopen(argv[a+opt['r']],"r");
    if(!in) {
      perror(argv[a+opt['r']]);
      fatal("Cannot open file for input");
    }
    if(opt['u']) {
      for(n=0;name[n];n++) if(name[n]>='a' && name[n]<='z') name[n]+='A'-'a';
    } else if(opt['l']) {
      for(n=0;name[n];n++) if(name[n]>='A' && name[n]<='Z') name[n]+='a'-'A';
    }
    fseek(in,0,SEEK_END);
    size=ftell(in);
    fseek(in,0,SEEK_SET);
    o=1;
  } else {
    in=stdin;
    for(;;) {
      c=fgetc(stdin);
      if(c==EOF) exit(0);
      if(opt['u'] && c>='a' && c<='z') c+='A'-'a'; else if(opt['l'] && c>='A' && c<='Z') c+='a'-'A';
      if(opt['s']) {
        if(c=='/') d=1; else if(c && c!='.') d=0;
        if((c&~127) || !safe[c] || (c=='.' && n && name[n-1]=='.') || (c=='/' && d)) c='_';
      }
      name[n++]=c;
      if(!c) break;
      if(n==254) fatal("Name too long: %s",name);
    }
    if(opt['s']) {
      if(!n) name[1]=0,n=1;
      if(d) name[n-1]='_';
    }
    size=fgetc(stdin)<<16;
    size|=fgetc(stdin)<<24;
    size|=fgetc(stdin);
    size|=fgetc(stdin)<<8;
    if(argc<3) {
      o=1;
    } else {
      o=0;
      for(n=2;n<argc;n++) {
        if(!strcmp(name,argv[n])) {
          o=1;
          if(opt['r'] && ++n<argc) strcpy(name,argv[n]);
          break;
        }
        if(opt['r']) n++;
      }
    }
    if(opt['d']) o=!o;
  }
  if(o && opt['t']) {
    puts(name);
    o=0;
  }
  if(o) {
    if(opt['x']) {
      fp=fopen(name,"w");
      if(!fp) {
        perror(name);
        fatal("Cannot open file for output");
      }
    } else {
      fp=stdout;
      if(!opt['o']) {
        fwrite(name,1,strlen(name)+1,stdout);
        putchar(size>>16);
        putchar(size>>24);
        putchar(size);
        putchar(size>>8);
      }
    }
    while(size--) fputc(fgetc(in),fp);
    if(opt['x']) fclose(fp);
  } else {
    while(size--) fgetc(in);
  }
  if(opt['c']) fclose(in);
}

int main(int Argc,char**Argv) {
  char*t;
  int a;
  argc=Argc;
  argv=Argv;
  if(argc>1) for(t=argv[1];*t;) opt[*t++&127]=1;
  if(opt['c']) {
    for(a=2;a<argc;a+=opt['r']+1) process_lump(a);
  } else {
    for(;;) process_lump(0);
  }
  return 0;
}
