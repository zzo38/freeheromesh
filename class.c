#if 0
gcc ${CFLAGS:--s -O2} -c -Wno-unused-result class.c `sdl-config --cflags`
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

#define TF_COMMA 0x0001 // has a comma modifier
#define TF_EQUAL 0x0002 // has an equal sign modifier
#define TF_ABNORMAL 0x0004 // with TF_NAME, not directly an opcode
#define TF_COMPAT 0x0008 // add 1 to opcode in compatibility mode
#define TF_DIR 0x0010 // token is a direction
#define TF_DROP 0x0020 // add 0x2000 to opcode if followed by OP_DROP
#define TF_NAME 0x0080 // token is a name or opcode
#define TF_KEY 0x0100 // token is a Hero Mesh key name
#define TF_OPEN 0x0400 // token is (
#define TF_CLOSE 0x0800 // token is )
#define TF_INT 0x1000 // token is a number
#define TF_FUNCTION 0x2000 // token is a user function
#define TF_MACRO 0x4000 // token is a macro
#define TF_EOF 0x8000 // end of file
typedef struct {
  const char*txt;
  Uint32 num;
} Op_Names;
#include "instruc.h"
#define Tokenf(x) (tokent&(x))

Value globals[0x800];
Value initglobals[0x800];
Class*classes[0x4000];
const char*messages[0x4000];
Uint16 functions[0x4000];
int max_animation=32;
Sint32 max_volume=10000;
Uint8 back_color=1;
char**stringpool;

#define HASH_SIZE 8888
#define LOCAL_HASH_SIZE 5555
typedef struct {
  Uint16 id;
  char*txt;
} Hash;

typedef struct InputStack {
  FILE*classfp;
  int linenum;
  struct InputStack*next;
} InputStack;

typedef struct TokenList {
  Uint16 t;
  Uint32 v;
  char*str;
  Uint16 ref;
  struct TokenList*next;
} TokenList;

typedef struct MacroStack {
  TokenList*tok;
  Sint16 n;
  TokenList**args;
  struct MacroStack*next;
} MacroStack;

static FILE*classfp;
static Uint16 tokent;
static Uint32 tokenv;
static int linenum=1;
static char tokenstr[0x2000];
static int undef_class=1;
static int undef_message=0;
static int num_globals=0;
static int num_functions=0;
static int num_strings=0;
static char pushback=0;
static Hash*glohash;
static InputStack*inpstack;
static MacroStack*macstack;
static TokenList**macros;

#define ParseError(a,...) fatal("On line %d: " a,linenum,##__VA_ARGS__)

static const unsigned char chkind[256]={
  ['$']=1, ['!']=1, ['\'']=1, ['#']=1, ['@']=1, ['%']=1, ['&']=1, [':']=1,
  ['0'...'9']=2, ['-']=2, ['+']=2,
  ['A'...'Z']=3, ['a'...'z']=3, ['_']=3, ['?']=3, ['.']=3, ['*']=3, ['/']=3,
};

#define MIN_VERSION 0
#define MAX_VERSION 0

#define MAX_MACRO 0x3FC0

#define MAC_ADD 0xFFC0
#define MAC_SUB 0xFFC1
#define MAC_MUL 0xFFC2
#define MAC_DIV 0xFFC3
#define MAC_MOD 0xFFC4
#define MAC_BAND 0xFFC5
#define MAC_BOR 0xFFC6
#define MAC_BXOR 0xFFC7
#define MAC_BNOT 0xFFC8
#define MAC_CAT 0xFFC9
#define MAC_VERSION 0xFFE0
#define MAC_DEFINE 0xFFE1
#define MAC_INCLUDE 0xFFE2
#define MAC_CALL 0xFFE3
#define MAC_UNDEFINED 0xFFFF

static TokenList*add_macro(void) {
  TokenList*o=malloc(sizeof(TokenList));
  if(!o) fatal("Allocation failed\n");
  o->t=tokent;
  o->v=tokenv;
  o->str=0;
  o->ref=0;
  o->next=0;
  if(*tokenstr) {
    o->str=strdup(tokenstr);
    if(!o->str) fatal("Allocation failed\n");
  }
  return o;
}

static void ref_macro(TokenList*o) {
  while(o) {
    if(++o->ref==65000) ParseError("Reference count of macro token list overflowed\n");
    o=o->next;
  }
}

static void free_macro(TokenList*o) {
  TokenList*p;
  while(o && !o->ref--) {
    free(o->str);
    o=(p=o)->next;
    free(p);
  }
}

static inline TokenList*step_macro(TokenList*o) {
  TokenList*p=o->next;
  if(!o->ref--) {
    free(o->str);
    free(o);
  }
  return p;
}

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
  cl->uservars=cl->hard[0]=cl->hard[1]=cl->hard[2]=cl->hard[3]=cl->nmsg=0;
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

static Uint16 look_hash(Hash*tbl,int size,Uint16 minv,Uint16 maxv,Uint16 newv,const char*err) {
  int h,i,m;
  for(h=i=0;tokenstr[i];i++) h=(13*h+tokenstr[i])%size;
  m=h;
  for(;;) {
    if(tbl[h].id>=minv && tbl[h].id<=maxv && !strcmp(tokenstr,tbl[h].txt)) return tbl[h].id;
    if(!tbl[h].id) {
      if(newv>maxv) ParseError("Too many %s\n",err);
      tbl[h].txt=strdup(tokenstr);
      if(!tbl[h].txt) ParseError("Out of string space in hash table\n");
      tbl[h].id=newv;
      return 0;
    }
    h=(h+1)%size;
    if(h==m) ParseError("Hash table full\n");
  }
}

static Uint16 look_hash_mac(void) {
  int h,i,m;
  for(h=i=0;tokenstr[i];i++) h=(13*h+tokenstr[i])%HASH_SIZE;
  m=h;
  for(;;) {
    if(glohash[h].id>=0xC000 && !strcmp(tokenstr,glohash[h].txt)) return h;
    if(!glohash[h].id) {
      glohash[h].txt=strdup(tokenstr);
      if(!glohash[h].txt) ParseError("Out of string space in hash table\n");
      glohash[h].id=0xFFFF;
      return h;
    }
    h=(h+1)%HASH_SIZE;
    if(h==m) ParseError("Hash table full\n");
  }
}

#define ReturnToken(x,y) do{ tokent=x; tokenv=y; return; }while(0)

static void nxttok1(void) {
  int c,i,fl,n,pr;
  magain: if(macstack) {
    TokenList*tl=0;
    pr=0;
    tokent=macstack->tok->t;
    if(tokent&TF_EOF) ParseError("Unexpected end of file in macro expansion\n");
    tokenv=macstack->tok->v;
    *tokenstr=0;
    if(macstack->tok->str) strcpy(tokenstr,macstack->tok->str);
    if(tokent==TF_MACRO+TF_INT && macstack->n>=0) {
      if(tokenv&~0xFF) {
        tokenv-=0x100;
      } else {
        if(tokenv<macstack->n) tl=macstack->args[tokenv];
        if(tl) ref_macro(tl);
        pr=1;
      }
    }
    macstack->tok=step_macro(macstack->tok);
    if(!macstack->tok) {
      MacroStack*ms=macstack->next;
      for(i=0;i<macstack->n;i++) free_macro(macstack->args[i]);
      free(macstack->args);
      free(macstack);
      macstack=ms;
    }
    if(pr) {
      if(tl) {
        MacroStack*ms=malloc(sizeof(MacroStack));
        if(!ms) fatal("Allocation failed\n");
        ms->tok=tl;
        ms->n=-1;
        ms->args=0;
        ms->next=macstack;
        macstack=ms;
      }
      goto magain;
    }
    return;
  }
  fl=n=pr=0;
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
  if(c=='{') {
    c=fgetc(classfp);
    while(c==' ' || c=='\t' || c=='\r' || c=='\n') {
      if(c=='\n') ++linenum;
      c=fgetc(classfp);
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
    if(!n) fatal("Expected macro name\n");
    if(c!=EOF) ungetc(c,classfp);
    ReturnToken(TF_MACRO|TF_OPEN,look_hash_mac());
  }
  if(c=='}') ReturnToken(TF_MACRO|TF_CLOSE,0);
  if(c=='|') ReturnToken(TF_MACRO,1);
  if(c=='\\') {
    i=0;
    while((c=fgetc(classfp))=='\\') i+=0x100;
    tokenv=0;
    for(;c>='0' && c<='9';c=fgetc(classfp)) tokenv=10*tokenv+c-'0';
    --tokenv;
    if(tokenv&~255) ParseError("Macro parameter reference out of range\n");
    if(c!=EOF) ungetc(c,classfp);
    tokent=TF_MACRO|TF_INT;
    tokenv|=i;
    return;
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
      tokent=TF_NAME|TF_ABNORMAL|fl;
      tokenv=OP_LOCAL;
      break;
    case '@':
      if(fl&TF_COMMA) ParseError("Invalid use of , in token\n");
      tokent=TF_NAME|fl;
      tokenv=look_hash(glohash,HASH_SIZE,0x2800,0x2FFF,num_globals+0x2800,"user global variables")?:(num_globals++)+0x2800;
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
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_FUNCTION|TF_ABNORMAL;
      tokenv=look_hash(glohash,HASH_SIZE,0x8000,0xBFFF,num_functions,"user functions")?:(num_functions++)+0x8000;
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

static void define_macro(Uint16 name) {
  int i;
  TokenList**t;
  if(glohash[name].id<0xC000) fatal("Confusion\n");
  if(glohash[name].id<MAX_MACRO+0xC000) {
    free_macro(macros[i=glohash[name].id-0xC000]);
    macros[i]=0;
  } else {
    for(i=0;i<MAX_MACRO;i++) if(!macros[i]) break;
    if(i==MAX_MACRO) ParseError("Too many macro definitions\n");
  }
  glohash[name].id=i+0xC000;
  t=macros+i;
  i=1;
  for(;;) {
    nxttok1();
    if(tokent==TF_MACRO+TF_OPEN && ++i>65000) ParseError("Too much macro nesting\n");
    if(tokent==TF_MACRO+TF_CLOSE && !--i) break;
    *t=add_macro();
    t=&(*t)->next;
  }
}

static void begin_include_file(const char*name) {
  InputStack*nxt=inpstack;
  inpstack=malloc(sizeof(InputStack));
  if(!inpstack) fatal("Allocation failed\n");
  inpstack->classfp=classfp;
  inpstack->linenum=linenum;
  inpstack->next=nxt;
  linenum=1;
  //TODO: Use the correct directory to load the include file
  classfp=fopen(name,"r");
  if(!classfp) ParseError("Cannot open include file \"%s\": %m\n",name);
}

static void begin_macro(TokenList*mac) {
  MacroStack*ms=malloc(sizeof(MacroStack));
  TokenList**ap=0;
  int a=0;
  int b=0;
  int c=0;
  ref_macro(mac);
  if(!ms) fatal("Allocation failed\n");
  ms->tok=mac;
  ms->n=0;
  ms->args=0;
  for(;;) {
    nxttok1();
    if(tokent&TF_EOF) ParseError("Unexpected end of file in macro argument\n");
    if(tokent&TF_OPEN) {
      ++a;
      if(tokent&TF_MACRO) ++c;
    }
    if(tokent&TF_CLOSE) {
      --a;
      if(tokent&TF_MACRO) --c;
    }
    if(c==-1) {
      if(a!=-1 && !b) ParseError("Misnested macro argument\n");
      ms->next=macstack;
      macstack=ms;
      return;
    } else if(a==-1 && !b) {
      ParseError("Misnested macro argument\n");
    } else if(tokent==TF_MACRO && tokenv==1 && !a && !b) {
      if(c) ParseError("Misnested macro argument\n");
      b=1;
      ms->args=realloc(ms->args,++ms->n*sizeof(TokenList*));
      if(!ms->args) fatal("Allocation failed\n");
      ap=ms->args+ms->n-1;
      *ap=0;
    } else {
      if(!b && !(tokent&TF_CLOSE) && a==(tokent&TF_OPEN?1:0)) {
        ms->args=realloc(ms->args,++ms->n*sizeof(TokenList*));
        if(!ms->args) fatal("Allocation failed\n");
        ap=ms->args+ms->n-1;
        *ap=0;
      }
      *ap=add_macro();
      ap=&((*ap)->next);
    }
  }
}

static void nxttok(void) {
  if(pushback) {
    pushback=0;
    return;
  }
  again:
  nxttok1();
  if(tokent&TF_EOF) {
    if(inpstack) {
      InputStack s=*inpstack;
      free(inpstack);
      fclose(classfp);
      classfp=s.classfp;
      linenum=s.linenum;
      inpstack=s.next;
      goto again;
    }
  } else if(tokent&TF_MACRO) {
    Uint32 n;
    char*s;
    if(tokent&TF_OPEN) {
      call:
      switch(glohash[tokenv].id) {
        case 0xC000 ... MAX_MACRO+0xC000-1:
          begin_macro(macros[glohash[tokenv].id-0xC000]);
          goto again;
        case MAC_ADD:
          n=0;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n+=tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_SUB:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          n=tokenv;
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          n-=tokenv;
          nxttok();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_MUL:
          n=1;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n*=tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_DIV:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          n=tokenv;
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(!tokenv) ParseError("Division by zero\n");
          n/=tokenv;
          nxttok();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_MOD:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          n=tokenv;
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(!tokenv) ParseError("Division by zero\n");
          n%=tokenv;
          nxttok();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_BAND:
          n=-1;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n&=tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_BOR:
          n=0;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n|=tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_BXOR:
          n=0;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n^=tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_BNOT:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          n=~tokenv;
          nxttok();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_CAT:
          s=malloc(0x2000);
          if(!s) fatal("Allocation failed\n");
          n=0;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) {
              break;
            } else if(tokent==TF_INT) {
              sprintf(tokenstr,"%d",tokenv);
              goto isname;
            } else if(tokent&TF_NAME) {
              isname:
              if(strlen(tokenstr)+n>=0x1FFE) ParseError("Concatenated string too long\n");
              strcpy(s+n,tokenstr);
              n+=strlen(tokenstr);
            } else if(tokent!=TF_MACRO || tokenv!=1) {
              ParseError("Number or string or name expected\n");
            }
          }
          s[n]=0;
          strcpy(tokenstr,s);
          free(s);
          ReturnToken(TF_NAME|TF_ABNORMAL,OP_STRING);
        case MAC_VERSION:
          nxttok1();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv<MIN_VERSION || tokenv>MAX_VERSION) ParseError("Version number out of range\n");
          nxttok1();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          goto again;
        case MAC_DEFINE:
          if(!macros) {
            macros=calloc(MAX_MACRO,sizeof(TokenList*));
            if(!macros) fatal("Allocation failed\n");
          }
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          define_macro(look_hash_mac());
          goto again;
        case MAC_INCLUDE:
          if(macstack) ParseError("Cannot use {include} inside of a macro\n");
          nxttok1();
          if(tokent==TF_MACRO && tokenv==1) ReturnToken(TF_EOF,0);
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          n=look_hash_mac();
          nxttok1();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          begin_include_file(glohash[n].txt);
          goto again;
        case MAC_CALL:
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          tokenv=look_hash_mac();
          goto call;
        case MAC_UNDEFINED:
          ParseError("Undefined macro: {%s}\n",tokenstr);
          break;
        default:
          ParseError("Strange macro token: 0x%04X\n",glohash[tokenv].id);
      }
    }
  }
}

static int pool_string(const char*s) {
  int i;
  for(i=0;i<num_strings;i++) if(!strcmp(s,stringpool[i])) return i;
  i=num_strings++;
  if(i>32768) ParseError("Too many string literals\n");
  stringpool=realloc(stringpool,num_strings*sizeof(stringpool));
  if(!stringpool) fatal("Allocation failed\n");
  stringpool[i]=strdup(s);
  if(!stringpool[i]) fatal("Allocation failed\n");
  return i;
}

static Value parse_constant_value(void) {
  if(tokent==TF_INT) {
    return NVALUE(tokenv);
  } else if(Tokenf(TF_NAME)) {
    switch(tokenv) {
      case 0x0000 ... 0x00FF: return NVALUE(tokenv);
      case 0x0100 ... 0x01FF: return NVALUE(tokenv-0x0200);
      case 0x0200 ... 0x02FF: return MVALUE(tokenv&255);
      case 0x0300 ... 0x03FF: return UVALUE(0,TY_SOUND);
      case 0x0400 ... 0x04FF: return UVALUE(0,TY_USOUND);
      case 0x4000 ... 0x7FFF: return CVALUE(tokenv-0x4000);
      case OP_STRING: return UVALUE(pool_string(tokenstr),TY_STRING);
      case OP_BITCONSTANT ... OP_BITCONSTANT_LAST: return NVALUE(1<<(tokenv&31));
      case 0xC000 ... 0xFFFF: return MVALUE(tokenv-0xBF00);
    }
  }
  ParseError("Constant value expected\n");
}

#define AddInst(x) (cl->codes[ptr++]=(x),prflag=0)
#define AddInst2(x,y) (cl->codes[ptr++]=(x),cl->codes[ptr++]=(y),prflag=0,peep=ptr)
#define AddInstF(x,y) (cl->codes[ptr++]=(x),prflag=(y))
#define ChangeInst(x) (cl->codes[ptr-1]x,prflag=0)
#define InstFlag(x) (peep<ptr && (prflag&(x)))
#define Inst7bit() (peep<ptr && cl->codes[ptr-1]<0x0080)
#define Inst8bit() (peep<ptr && cl->codes[ptr-1]<0x0100)
#define AbbrevOp(x,y) case x: if(Inst7bit()) ChangeInst(+=0x1000|((y)<<4)); else AddInstF(x,tokent); break
static int parse_instructions(int cla,int ptr,Hash*hash) {
  int peep=ptr;
  int prflag=0;
  Class*cl=classes[cla];
  cl->codes=realloc(cl->codes,0x10000*sizeof(Uint16));
  if(!cl->codes) fatal("Allocation failed\n");
  for(;;) {
    nxttok();
    if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
    if(Tokenf(TF_INT)) {
      
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        AbbrevOp(OP_ADD,0x00);
        AbbrevOp(OP_SUB,0x08);
        AbbrevOp(OP_MUL,0x10);
        AbbrevOp(OP_DIV,0x18);
        AbbrevOp(OP_MOD,0x20);
        AbbrevOp(OP_MUL_C,0x28);
        AbbrevOp(OP_DIV_C,0x30);
        AbbrevOp(OP_MOD_C,0x38);
        AbbrevOp(OP_BAND,0x40);
        AbbrevOp(OP_BOR,0x48);
        AbbrevOp(OP_BXOR,0x50);
        AbbrevOp(OP_LSH,0x58);
        AbbrevOp(OP_RSH,0x60);
        AbbrevOp(OP_RSH_C,0x68);
        AbbrevOp(OP_EQ,0x70);
        AbbrevOp(OP_NE,0x78);
        AbbrevOp(OP_LT,0x80);
        AbbrevOp(OP_GT,0x88);
        AbbrevOp(OP_LE,0x90);
        AbbrevOp(OP_GE,0x98);
        AbbrevOp(OP_LT_C,0xA0);
        AbbrevOp(OP_GT_C,0xA8);
        AbbrevOp(OP_LE_C,0xB0);
        AbbrevOp(OP_GE_C,0xB8);
        case OP_DROP:
          if(InstFlag(TF_DROP)) ChangeInst(+=0x2000);
          else AddInst(OP_DROP);
          break;
        default:
          if(Tokenf(TF_ABNORMAL)) ParseError("Invalid instruction token\n");
          AddInstF(tokenv,tokent);
      }
    } else if(Tokenf(TF_FUNCTION)) {
      AddInst2(OP_FUNCTION,tokenv&0x3FFF);
    } else if(tokent==TF_OPEN) {
      nxttok();
      if(Tokenf(TF_MACRO) || !Tokenf(TF_NAME)) ParseError("Invalid parenthesized instruction\n");
      switch(tokenv) {
        
        default:
          ParseError("Invalid parenthesized instruction\n");
      }
    } else if(tokent==TF_CLOSE) {
      
      if(Inst8bit()) ChangeInst(+=0x1E00);
      else AddInst(OP_RET);
      break;
    } else {
      ParseError("Invalid instruction token\n");
    }
    if(ptr>=0xFFEF) ParseError("Out of code space\n");
  }
  cl->codes=realloc(cl->codes,ptr*sizeof(Uint16))?:cl->codes;
  return ptr;
}

void load_classes(void) {
  int i;
  int gloptr=0;
  Hash*glolocalhash;
  char*nam=sqlite3_mprintf("%s.class",basefilename);
  if(!nam) fatal("Allocation failed\n");
  classfp=fopen(nam,"r");
  sqlite3_free(nam);
  if(!classfp) fatal("Cannot open class file '%s': %m\n",nam);
  glohash=calloc(HASH_SIZE,sizeof(Hash));
  if(!glohash) fatal("Allocation failed\n");
  glolocalhash=calloc(LOCAL_HASH_SIZE,sizeof(Hash));
  if(!glolocalhash) fatal("Allocation failed\n");
  strcpy(tokenstr,"+"); glohash[look_hash_mac()].id=MAC_ADD;
  strcpy(tokenstr,"-"); glohash[look_hash_mac()].id=MAC_SUB;
  strcpy(tokenstr,"*"); glohash[look_hash_mac()].id=MAC_MUL;
  strcpy(tokenstr,"/"); glohash[look_hash_mac()].id=MAC_DIV;
  strcpy(tokenstr,"mod"); glohash[look_hash_mac()].id=MAC_MOD;
  strcpy(tokenstr,"band"); glohash[look_hash_mac()].id=MAC_BAND;
  strcpy(tokenstr,"bor"); glohash[look_hash_mac()].id=MAC_BOR;
  strcpy(tokenstr,"bxor"); glohash[look_hash_mac()].id=MAC_BXOR;
  strcpy(tokenstr,"bnot"); glohash[look_hash_mac()].id=MAC_BNOT;
  strcpy(tokenstr,"cat"); glohash[look_hash_mac()].id=MAC_CAT;
  strcpy(tokenstr,"version"); glohash[look_hash_mac()].id=MAC_VERSION;
  strcpy(tokenstr,"define"); glohash[look_hash_mac()].id=MAC_DEFINE;
  strcpy(tokenstr,"include"); glohash[look_hash_mac()].id=MAC_INCLUDE;
  strcpy(tokenstr,"call"); glohash[look_hash_mac()].id=MAC_CALL;
  if(main_options['L']) {
    for(;;) {
      nxttok();
      printf("** %5d %04X %08X \"",linenum,tokent,tokenv);
      for(i=0;tokenstr[i];i++) {
        if(tokenstr[i]<32 || tokenstr[i]>126) printf("<%02X>",tokenstr[i]&255);
        else putchar(tokenstr[i]);
      }
      printf("\"\n");
      if(Tokenf(TF_EOF)) goto done;
    }
  }
  *classes=malloc(sizeof(Class));
  if(!*classes) fatal("Allocation failed\n");
  classes[0]->name=classes[0]->edithelp=classes[0]->gamehelp=0;
  classes[0]->codes=classes[0]->messages=classes[0]->images=0;
  classes[0]->nmsg=0;
  memset(functions,-1,sizeof(functions));
  for(;;) {
    nxttok();
    if(tokent==TF_EOF) goto done;
    if(tokent!=TF_OPEN) ParseError("Expected open parenthesis\n");
    nxttok();
    if(Tokenf(TF_FUNCTION)) {
      
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        case 0x4000 ... 0x7FFF: // Class definition
          
          break;
        case 0x0200 ... 0x02FF: case 0xC000 ... 0xFFFF: // Default message handler
          
          break;
        case 0x2800 ... 0x2FFF: // Define initial values of global variables
          i=tokenv-0x2800;
          nxttok();
          if(tokent==TF_CLOSE) break;
          initglobals[i]=parse_constant_value();
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        case OP_BACKGROUND:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv&~255) ParseError("Background color out of range\n");
          back_color=tokenv;
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        case OP_ANIMATE:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv<1 || tokenv>256) ParseError("Number of max animation steps out of range\n");
          max_animation=tokenv;
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        case OP_VOLUME:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          max_volume=tokenv;
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        default:
          ParseError("Invalid top level definition: %s\n",tokenstr);
      }
    } else {
      ParseError("Invalid top level definition\n");
    }
  }
  done:
  fclose(classfp);
  if(main_options['H']) {
    for(i=0;i<HASH_SIZE;i++) if(glohash[i].id) printf("\"%s\": %04X\n",glohash[i].txt,glohash[i].id);
  }
  for(i=0;i<LOCAL_HASH_SIZE;i++) free(glolocalhash[i].txt);
  free(glolocalhash);
  for(i=0;i<num_functions;i++) if(functions[i]==0xFFFF) {
    int j;
    for(j=0;j<HASH_SIZE;j++) if(glohash[j].id==i+0x8000) fatal("Function &%s mentioned but not defined\n",glohash[j].txt);
    fatal("Function mentioned but not defined\n");
  }
  for(i=0;i<HASH_SIZE;i++) free(glohash[i].txt);
  free(glohash);
  for(i=1;i<undef_class;i++) if(classes[i] && (classes[i]->cflags&CF_NOCLASS1)) fatal("Class $%s mentioned but not defined\n",classes[i]->name);
  if(macros) for(i=0;i<MAX_MACRO;i++) if(macros[i]) free_macro(macros[i]);
  free(macros);
}
