#if 0
gcc -s -O2 -c -Wno-unused-result class.c `sdl-config --cflags`
exit
#endif

/*
  This program is part of Free Hero Mesh and is public domain.
*/

#define HEROMESH_CLASS
#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"

#define TF_COMMA 0x0001
#define TF_EQUAL 0x0002
#define TF_ABNORMAL 0x0004
#define TF_COMPAT 0x0008
#define TF_DIR 0x0010
#define TF_DROP 0x0020
#define TF_NAME 0x0080
#define TF_KEY 0x0100
#define TF_OPEN 0x0400
#define TF_CLOSE 0x0800
#define TF_INT 0x1000
#define TF_MACRO 0x4000
#define TF_EOF 0x8000
typedef struct {
  const char*txt;
  Uint32 num;
} Op_Names;
#include "instruc.h"
#define Tokenf(x) (tokent&(x))

Class*classes[0x4000];
const char*messages[0x4000];
int max_animation=32;
Sint32 max_volume=10000;

static FILE*classfp;
static Uint16 tokent;
static Uint32 tokenv;
static int linenum=1;
static char tokenstr[0x2000];
static int undef_class=1;
static int undef_message=0;

#define ParseError(a,...) fatal("On line %d: " a,linenum,##__VA_ARGS__)

static const unsigned char chkind[256]={
  ['$']=1, ['!']=1, ['\'']=1, ['#']=1, ['@']=1, ['%']=1, ['&']=1, [':']=1,
  ['0'...'9']=2, ['-']=2, ['+']=2,
  ['A'...'Z']=3, ['a'...'z']=3, ['_']=3, ['?']=3, ['.']=3, ['*']=3, ['/']=3,
};

static void read_quoted_string(void) {
  int c,i,n;
  int isimg=0;
  for(i=0;i<0x1FFD;) {
    c=fgetc(classfp);
    if(c==EOF) {
      ParseError("Unterminated string literal\n");
    } else if(c=='\\') {
      if(isimg) {
        isimg=0;
        tokenstr[i++]=c;
      } else switch(c=fgetc(classfp)) {
        case '"': case '\\': tokenstr[i++]=c; break;
        case '0' ... '7': tokenstr[i++]=c+1-'0'; break;
        case 'b': tokenstr[i++]=15; break;
        case 'c': tokenstr[i++]=12; break;
        case 'i': tokenstr[i++]=14; isimg=1; break;
        case 'l': tokenstr[i++]=11; break;
        case 'n': tokenstr[i++]=10; break;
        case 'q': tokenstr[i++]=16; break;
        case 'x':
          c=fgetc(classfp);
          if(c>='0' && c<='9') n=c-'0';
          else if(c>='A' && c<='F') n=c+10-'A';
          else if(c>='a' && c<='f') n=c+10-'a';
          else ParseError("Invalid string escape: \\x%c\n",c);
          n<<=4;
          c=fgetc(classfp);
          if(c>='0' && c<='9') n|=c-'0';
          else if(c>='A' && c<='F') n|=c+10-'A';
          else if(c>='a' && c<='f') n|=c+10-'a';
          else ParseError("Invalid string escape: \\x%X%c\n",n>>4,c);
          if(n<32) tokenstr[i++]=31;
          tokenstr[i++]=n;
          break;
        default: ParseError("Invalid string escape: \\%c\n",c);
      }
    } else if(c=='"') {
      if(isimg) ParseError("Unterminated \\i escape in string literal\n");
      tokenstr[i]=0;
      return;
    } else if(c!='\r') {
      tokenstr[i++]=c;
      if(c=='\n') ++linenum;
    }
  }
  ParseError("Too long string literal\n");
}

static int name_compar(const void*a,const void*b) {
  const Op_Names*x=a;
  const Op_Names*y=b;
  return strcmp(x->txt,y->txt);
}

static Class*initialize_class(int n,int f,const char*name) {
  Class*cl;
  if(classes[n]) fatal("Duplicate class number %d\n",n);
  cl=classes[n]=malloc(sizeof(Class));
  if(!cl) fatal("Allocation failed\n");
  cl->name=strdup(name);
  if(!cl->name) fatal("Allocation failed\n");
  cl->edithelp=cl->gamehelp=0;
  cl->codes=cl->messages=cl->images=0;
  cl->height=cl->weight=cl->climb=cl->density=cl->volume=cl->strength=cl->arrivals=cl->departures=0;
  cl->temperature=cl->misc4=cl->misc5=cl->misc6=cl->misc7=0;
  cl->uservars=cl->hard[0]=cl->hard[1]=cl->hard[2]=cl->hard[3]=0;
  cl->oflags=cl->sharp[0]=cl->sharp[1]=cl->sharp[2]=cl->sharp[3]=0;
  cl->shape=cl->shovable=cl->collisionLayers=cl->nimages=0;
  cl->cflags=f;
  if(undef_class<=n) undef_class=n+1;
}

static int look_class_name(void) {
  int i;
  int u=undef_class;
  for(i=1;i<undef_class;i++) {
    if(classes[i]) {
      if(!strcmp(classes[i]->name,tokenstr)) {
        if(classes[i]->cflags&CF_NOCLASS2) classes[i]->cflags|=CF_NOCLASS1;
        return i;
      }
    } else {
      u=i;
    }
  }
  if(u==0x4000) ParseError("Too many class names; cannot add $%s\n",tokenstr);
  initialize_class(u,CF_NOCLASS1,tokenstr);
  return u;
}

static int look_message_name(void) {
  int i;
  int u=undef_message;
  for(i=0;i<undef_message;i++) {
    if(messages[i]) {
      if(!strcmp(messages[i],tokenstr)) return i;
    } else {
      u=i;
    }
  }
  if(u==0x4000) ParseError("Too many message names; cannot add #%s",tokenstr);
  if(u==undef_message) ++undef_message;
  messages[u]=strdup(tokenstr);
  if(!messages[u]) fatal("Allocation failed\n");
  return u;
}

#define ReturnToken(x,y) do{ tokent=x; tokenv=y; return; }while(0)
static void nxttok(void) {
  int c,i;
  int fl=0;
  int n=0;
  int pr=0;
  tokent=tokenv=0;
  *tokenstr=0;
again:
  c=fgetc(classfp);
  while(c==' ' || c=='\t' || c=='\r' || c=='\n') {
    if(c=='\n') ++linenum;
    c=fgetc(classfp);
  }
  if(c==EOF) ReturnToken(TF_EOF,0);
  if(c==';') {
    while(c!=EOF && c!='\n') c=fgetc(classfp);
    ++linenum;
    goto again;
  }
  if(c=='(') ReturnToken(TF_OPEN,0);
  if(c==')') ReturnToken(TF_CLOSE,0);
  if(c=='"') {
    tokent=TF_NAME|TF_ABNORMAL;
    tokenv=OP_STRING;
    read_quoted_string();
    return;
  }
  if(c=='=') {
    fl|=TF_EQUAL;
    c=fgetc(classfp);
  }
  if(c==',') {
    fl|=TF_COMMA;
    c=fgetc(classfp);
  }
  if(c==EOF) ParseError("Unexpected end of file\n");
  if(chkind[c]==1) {
    pr=c;
    c=fgetc(classfp);
    if(c==EOF) ParseError("Unexpected end of file\n");
  }
  while(c>0 && (chkind[c]&2)) {
    if(n==256) {
      tokenstr[256]=0;
      ParseError("Identifier too long: %s\n",tokenstr);
    }
    tokenstr[n++]=c;
    c=fgetc(classfp);
  }
  tokenstr[n]=0;
  if(!n) fatal("Expected identifier\n");
  if(c!=EOF) ungetc(c,classfp);
  switch(pr) {
    case 0:
      if(chkind[c=*tokenstr]==2) {
        char*e;
        if(c=='-' || c=='+') n=1; else n=0;
        if(n && !tokenstr[1]) goto norm;
        tokent=TF_INT;
        if(fl) ParseError("Invalid use of , and = in token\n");
        if(c=='0' && tokenstr[1]=='x') {
          tokenv=strtol(tokenstr+2,&e,16);
        } else if(c=='0' && tokenstr[1]=='o') {
          tokenv=strtol(tokenstr+2,&e,8);
        } else {
          tokenv=strtol(tokenstr+n,&e,10);
        }
        if(e && *e) ParseError("Invalid token: %s\n",tokenstr);
        if(c=='-') tokenv=-tokenv;
      } else {
        static Op_Names key={tokenstr};
        const Op_Names*op;
        norm:
        op=bsearch(&key,op_names,N_OP_NAMES,sizeof(Op_Names),name_compar);
        if(!op) ParseError("Invalid token: %s\n",tokenstr);
        tokenv=op->num&0xFFFF;
        tokent=op->num>>16;
        if(fl&~tokent) ParseError("Invalid use of , and = in token: %s\n",tokenstr);
        if(fl&TF_COMMA) tokenv+=0x0800;
        if(fl&TF_EQUAL) tokenv+=0x1000;
        tokent&=fl|~(TF_COMMA|TF_EQUAL);
      }
      break;
    case '$':
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_NAME;
      tokenv=look_class_name()+0x4000;
      break;
    case '!':
      // Just ignore sounds for now
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_NAME;
      tokenv=0x0400;
      break;
    case '%':
      if(fl&TF_COMMA) ParseError("Invalid use of , in token\n");
      tokent=TF_NAME|fl;
      tokenv=OP_LOCAL;
      break;
    case '@':
      if(fl&TF_COMMA) ParseError("Invalid use of , in token\n");
      tokent=TF_NAME|fl;
      
      break;
    case '#':
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_NAME;
      tokenv=look_message_name()+0xC000;
      break;
    case ':':
      tokent=TF_NAME|TF_ABNORMAL|fl;
      tokenv=OP_LABEL;
      break;
    case '&':
      
      break;
    case '\'':
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_NAME|TF_KEY;
      for(i=8;i<256;i++) {
        if(heromesh_key_names[i] && !strcmp(heromesh_key_names[i],tokenstr)) {
          tokenv=i;
          return;
        }
      }
      ParseError("Invalid Hero Mesh key name: %s\n",tokenstr);
      break;
  }
}

void load_classes(void) {
  int i;
  
  for(i=1;i<undef_class;i++) if(classes[i] && (classes[i]->cflags&CF_NOCLASS1)) fatal("Class $%s mentioned but not defined",classes[i]->name);
}
