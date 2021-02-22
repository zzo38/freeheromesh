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
#include "names.h"

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

Value initglobals[0x800];
Class*classes[0x4000];
const char*messages[0x4000];
Uint16 functions[0x4000];
int max_animation=32;
Sint32 max_volume=10000;
Uint8 back_color=1;
Uint8 inv_back_color=9;
char**stringpool;
AnimationSlot anim_slot[8];
Uint8 keymask[256/8];

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

typedef struct LabelStack {
  Uint16 id;
  Uint16 addr;
  struct LabelStack*next;
} LabelStack;

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
static LabelStack*labelstack;
static Uint16*labelptr;

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
#define MAC_BIT 0xFFCA
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
          tokenstr[i++]=n?:255;
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

Uint16 get_message_ptr(int c,int m) {
  if(!classes[c]) return 0xFFFF;
  if(classes[c]->nmsg<=m) return 0xFFFF;
  return classes[c]->messages[m];
}

static void set_message_ptr(int c,int m,int p) {
  if(m>=classes[c]->nmsg) {
    classes[c]->messages=realloc(classes[c]->messages,(m+1)*sizeof(Uint16));
    if(!classes[c]->messages) fatal("Allocation failed\n");
    while(m>=classes[c]->nmsg) classes[c]->messages[classes[c]->nmsg++]=0xFFFF;
  }
  classes[c]->messages[m]=p;
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
    if(main_options['M']) {
      printf("M* Macro %04X %08X \"",tokent,tokenv);
      for(i=0;tokenstr[i];i++) {
        if(tokenstr[i]<32 || tokenstr[i]>126) printf("<%02X>",tokenstr[i]&255);
        else putchar(tokenstr[i]);
      }
      printf("\"\n");
    }
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
      if(main_options['M']) printf("M.\n");
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
      if(main_options['M']) printf("M^ %d\n",tl?1:0);
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
      tokenv=look_hash(glohash,HASH_SIZE,0x8000,0xBFFF,num_functions+0x8000,"user functions")?:(num_functions++)+0x8000;
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
    if(main_options['M']) {
      int i;
      printf("M= %c%4d %04X %08X \"",b?'|':a?'^':' ',ms->n,tokent,tokenv);
      for(i=0;tokenstr[i];i++) {
        if(tokenstr[i]<32 || tokenstr[i]>126) printf("<%02X>",tokenstr[i]&255);
        else putchar(tokenstr[i]);
      }
      printf("\"\n");
    }
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
  if(StackProtection()) fatal("Stack overflow\n");
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
      if(main_options['M']) printf("M+ {%s}\n",glohash[tokenv].txt);
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
        case MAC_BIT:
          n=0;
          for(;;) {
            nxttok();
            if(tokent==TF_MACRO+TF_CLOSE) break;
            if(tokent!=TF_INT) ParseError("Number expected\n");
            n|=1<<tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
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
      if(main_options['M']) printf("M-\n");
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

static void begin_label_stack(void) {
  labelstack=0;
  labelptr=malloc(0x8000*sizeof(Uint16));
  if(!labelptr) fatal("Allocation failed\n");
  memset(labelptr,255,0x8000*sizeof(Uint16));
  *labelptr=0x8001;
}

static void end_label_stack(Uint16*codes,Hash*hash) {
  LabelStack*s;
  while(labelstack) {
    s=labelstack;
    if(labelptr[s->id-0x8000]==0xFFFF) {
      int i;
      for(i=0;i<LOCAL_HASH_SIZE;i++) {
        if(hash[i].id==s->id) ParseError("Label :%s mentioned but never defined\n",hash[i].txt);
      }
      ParseError("Label mentioned but never defined\n");
    }
    codes[s->addr]=labelptr[s->id-0x8000];
    labelstack=s->next;
    free(s);
  }
  free(labelptr);
  labelstack=0;
  labelptr=0;
}

#define AddInst(x) (cl->codes[ptr++]=(x),prflag=0)
#define AddInst2(x,y) (cl->codes[ptr++]=(x),cl->codes[ptr++]=(y),prflag=0,peep=ptr)
#define AddInstF(x,y) (cl->codes[ptr++]=(x),prflag=(y))
#define ChangeInst(x) (cl->codes[ptr-1]x,prflag=0)
#define InstFlag(x) (peep<ptr && (prflag&(x)))
#define Inst7bit() (peep<ptr && cl->codes[ptr-1]<0x0080)
#define Inst8bit() (peep<ptr && cl->codes[ptr-1]<0x0100)
#define AbbrevOp(x,y) case x: if(Inst7bit()) ChangeInst(+=0x1000|((y)<<4)); else AddInstF(x,tokent); break
#define FlowPush(x) do{ if(flowdepth==64) ParseError("Too much flow control nesting\n"); flowop[flowdepth]=x; flowptr[flowdepth++]=ptr; }while(0)
#define FlowPop(x) do{ if(!flowdepth || flowop[--flowdepth]!=(x)) ParseError("Flow control mismatch\n"); }while(0)
static int parse_instructions(int cla,int ptr,Hash*hash,int compat) {
  int peep=ptr;
  int prflag=0;
  Class*cl=classes[cla];
  int flowdepth=0;
  Uint16 flowop[64];
  Uint16 flowptr[64];
  int x,y;
  cl->codes=realloc(cl->codes,0x10000*sizeof(Uint16));
  if(!cl->codes) fatal("Allocation failed\n");
  for(;;) {
    nxttok();
    if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
    if(Tokenf(TF_INT)) {
      numeric:
      if(!(tokenv&~0xFFL)) AddInst(tokenv);
      else if(!((tokenv-1)&tokenv)) AddInst(0x87E0+__builtin_ctz(tokenv));
      else if(!(tokenv&~0xFFFFL)) AddInst2(OP_INT16,tokenv);
      else if((tokenv&0x80000000L) && -256<(Sint32)tokenv) AddInst(tokenv&0x01FF);
      else AddInst(OP_INT32),AddInst2(tokenv>>16,tokenv);
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
        AbbrevOp(OP_RET,0xE0);
        case OP_DROP:
          if(InstFlag(TF_DROP)) ChangeInst(+=0x2000);
          else AddInst(OP_DROP);
          break;
        case OP_LOCAL:
          if(!cla) ParseError("Cannot use local variable in a global definition\n");
          tokenv=look_hash(hash,LOCAL_HASH_SIZE,0x2000,0x27FF,cl->uservars+0x2000,"user local variables")?:(cl->uservars++)+0x2000;
          if(Tokenf(TF_EQUAL)) tokenv+=0x1000;
          AddInst(tokenv);
          break;
        case OP_LABEL:
          tokenv=look_hash(hash,LOCAL_HASH_SIZE,0x8000,0xFFFF,*labelptr,"labels");
          if(!tokenv) tokenv=*labelptr,++*labelptr;
          tokenv-=0x8000;
          if(Tokenf(TF_COMMA)) {
            AddInst2(OP_CALLSUB,labelptr[tokenv]);
          } else if(Tokenf(TF_EQUAL)) {
            AddInst2(OP_GOTO,labelptr[tokenv]);
          } else {
            if(labelptr[tokenv]!=0xFFFF) ParseError("Duplicate definition of label :%s\n",tokenstr);
            labelptr[tokenv]=ptr;
            peep=ptr;
            break;
          }
          if(labelptr[tokenv]==0xFFFF) {
            LabelStack*s=malloc(sizeof(LabelStack));
            if(!s) fatal("Allocation failed\n");
            s->id=tokenv|0x8000;
            s->addr=ptr-1;
            s->next=labelstack;
            labelstack=s;
          }
          break;
        case OP_IF:
          AddInst(OP_IF);
          FlowPush(OP_IF);
          peep=++ptr;
          break;
        case OP_THEN:
          FlowPop(OP_IF);
          cl->codes[flowptr[flowdepth]]=peep=ptr;
          break;
        case OP_ELSE:
          FlowPop(OP_IF);
          x=flowptr[flowdepth];
          AddInst(OP_GOTO);
          FlowPush(OP_IF);
          cl->codes[x]=peep=++ptr;
          break;
        case OP_EL:
          AddInst(OP_GOTO);
          y=++ptr;
          FlowPop(OP_IF);
          x=flowptr[flowdepth];
          AddInst(OP_GOTO);
          FlowPush(OP_IF);
          cl->codes[x]=peep=++ptr;
          flowptr[flowdepth-1]=y;
          break;
        case OP_BEGIN:
          FlowPush(OP_BEGIN);
          peep=ptr;
          break;
        case OP_AGAIN:
          FlowPop(OP_BEGIN);
          AddInst2(OP_GOTO,flowptr[flowdepth]);
          break;
        case OP_UNTIL:
          FlowPop(OP_BEGIN);
          AddInst2(OP_IF,flowptr[flowdepth]);
          break;
        case OP_WHILE:
          AddInst(OP_IF);
          FlowPush(OP_WHILE);
          peep=++ptr;
          break;
        case OP_REPEAT:
          FlowPop(OP_WHILE);
          x=flowptr[flowdepth];
          FlowPop(OP_BEGIN);
          AddInst2(OP_GOTO,flowptr[flowdepth]);
          cl->codes[x]=ptr;
          break;
        case OP_FOR:
          if(num_globals>=0x07FF) ParseError("Too many user variables\n");
          x=num_globals++;
          AddInst2(OP_FOR,x);
          FlowPush(OP_FOR);
          peep=++ptr;
          break;
        case OP_NEXT:
          FlowPop(OP_FOR);
          AddInst2(OP_NEXT,flowptr[flowdepth]);
          cl->codes[flowptr[flowdepth]]=peep=ptr;
          break;
        case OP_STRING:
          AddInst2(OP_STRING,pool_string(tokenstr));
          break;
        case OP_MBEGIN:
          FlowPush(OP_BEGIN);
          AddInst(OP_TMARK);
          AddInst(OP_IF);
          FlowPush(OP_WHILE);
          peep=++ptr;
          break;
        default:
          if(Tokenf(TF_ABNORMAL)) ParseError("Invalid instruction token\n");
          if(compat && Tokenf(TF_COMPAT) && Tokenf(TF_EQUAL)) ++tokenv;
          AddInstF(tokenv,tokent);
      }
    } else if(Tokenf(TF_FUNCTION)) {
      AddInst2(OP_FUNCTION,tokenv&0x3FFF);
    } else if(tokent==TF_OPEN) {
      nxttok();
      if(Tokenf(TF_MACRO) || !Tokenf(TF_NAME)) ParseError("Invalid parenthesized instruction\n");
      switch(tokenv) {
        case OP_POPUP:
          nxttok();
          if(tokent!=TF_INT || tokenv<0 || tokenv>32) ParseError("Expected number from 0 to 32");
          if(tokenv) AddInst2(OP_POPUPARGS,tokenv); else AddInst(OP_POPUP);
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Unterminated (PopUp)\n");
          break;
        case OP_BROADCAST:
          nxttok();
          if(Tokenf(TF_MACRO) || !Tokenf(TF_NAME) || tokenv<0x4000 || tokenv>0x7FFF) ParseError("Class name expected\n");
          AddInst2(OP_BROADCASTCLASS,tokenv-0x4000);
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Unterminated (Broadcast)\n");
          break;
        case OP_BIT:
          x=0;
          for(;;) {
            nxttok();
            if(tokent==TF_CLOSE) break;
            if(Tokenf(TF_MACRO) || !Tokenf(TF_INT)) ParseError("Number or close parenthesis expected\n");
            x|=1<<tokenv;
          }
          tokenv=x;
          goto numeric;
        default:
          ParseError("Invalid parenthesized instruction\n");
      }
    } else if(tokent==TF_CLOSE) {
      if(peep<ptr && cl->codes[ptr-1]==OP_RET) break;
      if(peep<ptr && (cl->codes[ptr-1]&0xFF00)==0x1E00) break;
      if(Inst8bit()) ChangeInst(+=0x1E00);
      else AddInst(OP_RET);
      break;
    } else if(Tokenf(TF_EOF)) {
      ParseError("Unexpected end of file\n");
    } else {
      ParseError("Invalid instruction token\n");
    }
    if(ptr>=0xFFEF) ParseError("Out of code space\n");
  }
  if(flowdepth) ParseError("Unterminated flow control blocks (%d levels)\n",flowdepth);
  cl->codes=realloc(cl->codes,ptr*sizeof(Uint16))?:cl->codes;
  return ptr;
}

static void dump_class(int cla,int endptr,const Hash*hash) {
  const Class*cl=classes[cla];
  int i,j;
  if(!cl) return;
  printf("<<<Class %d>>>\n",cla);
  printf("  Name: %c%s\n",cla?'$':'(',cla?cl->name:"Global)");
  printf("  Flags: 0x%02X 0x%04X\n",cl->cflags,cl->oflags);
  printf("  Ht=%d Wt=%d Clm=%d Den=%d Vol=%d Str=%d Arr=%d Dep=%d\n",cl->height,cl->weight,cl->climb,cl->density,cl->volume,cl->strength,cl->arrivals,cl->departures);
  printf("  Temp=%d M4=%d M5=%d M6=%d M7=%d Shape=%d Shov=%d Coll=%d Img=%d\n",cl->temperature,cl->misc4,cl->misc5,cl->misc6,cl->misc7,cl->shape,cl->shovable,cl->collisionLayers,cl->nimages);
  printf("  Hard: %d %d %d %d\n",cl->hard[0],cl->hard[1],cl->hard[2],cl->hard[3]);
  printf("  Sharp: %d %d %d %d\n",cl->sharp[0],cl->sharp[1],cl->sharp[2],cl->sharp[3]);
  if(cl->nimages && cl->images) {
    printf("  Images:");
    for(i=0;i<cl->nimages;i++) printf(" %d%c",cl->images[i]&0x7FFF,cl->images[i]&0x8000?'*':'.');
    putchar('\n');
  }
  if(cl->nmsg && cl->messages) {
    printf("  Messages:\n");
    for(i=0;i<cl->nmsg;i++) {
      if(cl->messages[i]!=0xFFFF) {
        if(i<256) printf("    %s: 0x%04X\n",standard_message_names[i],cl->messages[i]);
        else printf("    #%s: 0x%04X\n",messages[i-256],cl->messages[i]);
      }
    }
  }
  if(hash && cl->uservars) {
    printf("  Variables:\n");
    for(i=0;i<LOCAL_HASH_SIZE;i++) {
      if(hash[i].id>=0x2000 && hash[i].id<0x3000) printf("    %%%s = 0x%04X\n",hash[i].txt,hash[i].id);
    }
  }
  if(endptr && cl->codes) {
    printf("  Codes:");
    for(i=0;i<endptr;i++) {
      if(!(i&15)) printf("\n    [%04X]",i);
      printf(" %04X",cl->codes[i]);
    }
    putchar('\n');
  }
  printf("---\n\n");
}

static inline Uint32 class_def_number(void) {
  Uint32 n;
  nxttok();
  if(!Tokenf(TF_INT)) fatal("Number expected\n");
  n=tokenv;
  nxttok();
  if(tokent!=TF_CLOSE) fatal("Close parentheses expected\n");
  return n;
}

static Uint32 class_def_misc(void) {
  Uint32 n=0;
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) return n;
    if(Tokenf(TF_INT)) {
      n|=tokenv;
    } else if(Tokenf(TF_NAME) && !(tokenv&~255)) {
      n|=tokenv;
    } else if(Tokenf(TF_NAME) && tokenv>=OP_BITCONSTANT && tokenv<=OP_BITCONSTANT_LAST) {
      n|=1L<<(tokenv&31);
    } else {
      fatal("Number expected");
    }
  }
}

static Uint32 class_def_arrivals(void) {
  Uint32 n=0;
  int i;
  nxttok();
  if(Tokenf(TF_NAME) && tokenv==OP_INPLACE) {
    nxttok();
    if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
    return 1<<12;
  }
  for(i=0;i<25;i++) {
    if(!Tokenf(TF_INT) || (tokenv&~1)) ParseError("Expected 0 or 1\n");
    if(tokenv) n|=1<<(i+4-2*(i%5));
    nxttok();
  }
  if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
  return n;
}

static void class_def_hard(Uint16*data) {
  int i;
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) {
      return;
    } else if(tokent==TF_OPEN) {
      nxttok();
      if(!Tokenf(TF_DIR) || tokenv>7 || (tokenv&1)) ParseError("Expected even absolute direction\n");
      i=tokenv>>1;
      nxttok();
      if(tokent!=TF_INT || (tokenv&~0xFFFF)) ParseError("Hardness/sharpness must be a 16-bit number\n");
      data[i]=tokenv;
      nxttok();
      if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
    } else if(tokent==TF_INT) {
      if(tokenv&~0xFFFF) ParseError("Hardness/sharpness must be a 16-bit number\n");
      data[0]=data[1]=data[2]=data[3]=tokenv;
    } else {
      ParseError("Expected ( or ) or number\n");
    }
  }
}

static inline Uint8 class_def_shovable(void) {
  Uint8 n=0;
  nxttok();
  if(tokent==TF_INT) {
    n=tokenv;
    nxttok();
    if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
    return n;
  }
  for(;;) {
    if(tokent==TF_CLOSE) return n;
    if(!Tokenf(TF_DIR) || tokenv>7 || (tokenv&1)) ParseError("Expected even absolute direction\n");
    n|=1<<tokenv;
    nxttok();
  }
}

static inline Uint8 class_def_shape(void) {
  Uint8 n=0;
  int i;
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) {
      return n;
    } else if(tokent==TF_OPEN) {
      nxttok();
      if(!Tokenf(TF_DIR) || tokenv>7 || (tokenv&1)) ParseError("Expected even absolute direction\n");
      i=tokenv;
      n&=~(3<<i);
      nxttok();
      if(tokent!=TF_INT || (tokenv&~3)) ParseError("Shape must be 0, 1, 2, or 3\n");
      n|=tokenv<<i;
      nxttok();
      if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
    } else if(tokent==TF_INT) {
      if(tokenv&~3) ParseError("Shape must be 0, 1, 2, or 3\n");
      n=tokenv*0x55;
    } else {
      ParseError("Expected ( or ) or number\n");
    }
  }
}

static char*class_def_help(void) {
  char*txt=malloc(0x3000);
  int n=0;
  int i;
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) break;
    if(!Tokenf(TF_NAME) || tokenv!=OP_STRING) ParseError("String expected\n");
    i=strlen(tokenstr);
    if(i+n>=0x2FFA) ParseError("Help text is too long\n");
    strcpy(txt+n,tokenstr);
    n+=i;
    txt[n++]=10;
    txt[n]=0;
  }
  if(!n) {
    free(txt);
    return 0;
  }
  txt[n]=0;
  return realloc(txt,n+1)?:txt;
}

static void class_def_image(int cla) {
  Class*cl=classes[cla];
  Uint16*img=malloc(256*sizeof(Uint16));
  sqlite3_stmt*st;
  if(cl->nimages || cl->images) ParseError("Duplicate (Image) block\n");
  if(!img) fatal("Allocation failed\n");
  if(userdb && sqlite3_prepare_v2(userdb,"SELECT `ID` FROM `PICTURES` WHERE `NAME` = ?1;",-1,&st,0)) fatal("SQL error: %s\n",sqlite3_errmsg(userdb));
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) break;
    if(cl->nimages==255) ParseError("Too many images in class\n");
    if(!Tokenf(TF_NAME) || tokenv!=OP_STRING) ParseError("String expected\n");
    if(userdb) {
      sqlite3_bind_text(st,1,tokenstr,-1,0);
      if(sqlite3_step(st)==SQLITE_ROW) {
        img[cl->nimages++]=sqlite3_column_int(st,0)|0x8000;
      } else {
        ParseError("A picture named \"%s\" does not exist\n",tokenstr);
      }
      sqlite3_reset(st);
      sqlite3_clear_bindings(st);
    } else {
      img[cl->nimages++]=0;
    }
  }
  if(userdb) sqlite3_finalize(st);
  if(!cl->nimages) {
    free(img);
    return;
  }
  cl->images=realloc(img,cl->nimages*sizeof(Uint16))?:img;
}

static void class_def_defaultimage(int cla) {
  Class*cl=classes[cla];
  int i;
  for(i=0;i<cl->nimages;i++) cl->images[i]&=0x7FFF;
  for(;;) {
    nxttok();
    if(tokent==TF_OPEN) {
      nxttok();
      if(tokent==TF_INT) {
        if(tokenv<0 || tokenv>=cl->nimages) ParseError("Image number out of range\n");
        i=tokenv;
        nxttok();
        if(tokent!=TF_INT) ParseError("Number expected\n");
        if(tokenv<i) ParseError("Backward range in (DefaultImage) block\n");
        if(tokenv>=cl->nimages) ParseError("Image number out of range\n");
        while(i<=tokenv) cl->images[i++]|=0x8000;
      } else if(tokent!=TF_CLOSE) {
        ParseError("Number expected\n");
      }
    } else if(tokent==TF_CLOSE) {
      break;
    } else if(tokent==TF_INT) {
      if(tokenv<0 || tokenv>=cl->nimages) ParseError("Image number out of range\n");
      cl->images[tokenv]|=0x8000;
    } else {
      ParseError("Expected ( or ) or number\n");
    }
  }
}

static void class_definition(int cla,sqlite3_stmt*vst) {
  Hash*hash=calloc(LOCAL_HASH_SIZE,sizeof(Hash));
  Class*cl=classes[cla];
  int ptr=0;
  int compat=0;
  int i;
  char disp=0;
  if(!hash) fatal("Allocation failed\n");
  if(!cl) fatal("Confusion of class definition somehow\n");
  if(cl->cflags&(CF_NOCLASS1|CF_NOCLASS2)) {
    cl->cflags=0;
  } else {
    ParseError("Duplicate definition of class $%s\n",cl->name);
  }
  begin_label_stack();
  for(;;) {
    nxttok();
    if(Tokenf(TF_EOF)) {
      ParseError("Unexpected end of file\n");
    } else if(Tokenf(TF_MACRO)) {
      ParseError("Unexpected macro token\n");
    } else if(Tokenf(TF_OPEN)) {
      nxttok();
      if(Tokenf(TF_KEY)) {
        if(!disp) {
          cl->codes=realloc(cl->codes,0x10000*sizeof(Uint16));
          if(!cl->codes) fatal("Allocation failed\n");
          if(get_message_ptr(cla,MSG_KEY)!=0xFFFF) ParseError("Class $%s has a KEY message already\n",cl->name);
          if(ptr>0xFDFF) ParseError("Out of code space\n");
          disp=1;
          set_message_ptr(cla,MSG_KEY,ptr);
          cl->codes[ptr]=OP_DISPATCH;
          for(i=1;i<256;i++) cl->codes[ptr+i]=0;
          ptr+=256;
        }
        i=tokenv&255;
        cl->codes[cl->messages[MSG_KEY]+i]=ptr;
        if(cl->cflags&CF_INPUT) {
          nxttok();
          if(tokent!=TF_NAME || tokenv!=OP_IGNOREKEY) keymask[i>>3]|=1<<(i&7);
          pushback=1;
        }
        ptr=parse_instructions(cla,ptr,hash,compat);
      } else if(Tokenf(TF_NAME)) {
        switch(tokenv) {
          case OP_IMAGE:
            class_def_image(cla);
            break;
          case OP_DEFAULTIMAGE:
            class_def_defaultimage(cla);
            break;
          case OP_HELP:
            if(cl->gamehelp) ParseError("Duplicate (Help) block\n");
            cl->gamehelp=class_def_help();
            break;
          case OP_EDITORHELP:
            if(cl->edithelp) ParseError("Duplicate (EditorHelp) block\n");
            cl->edithelp=class_def_help();
            break;
          case OP_HEIGHT:
            cl->height=class_def_number();
            break;
          case OP_WEIGHT:
            cl->weight=class_def_number();
            break;
          case OP_CLIMB:
            cl->climb=class_def_number();
            break;
          case OP_DENSITY:
            cl->density=class_def_number();
            break;
          case OP_VOLUME:
            cl->volume=class_def_number();
            break;
          case OP_STRENGTH:
            cl->strength=class_def_number();
            break;
          case OP_TEMPERATURE:
            cl->temperature=class_def_number();
            break;
          case OP_MISC4:
            cl->misc4=class_def_misc();
            break;
          case OP_MISC5:
            cl->misc5=class_def_misc();
            break;
          case OP_MISC6:
            cl->misc6=class_def_misc();
            break;
          case OP_MISC7:
            cl->misc7=class_def_misc();
            break;
          case OP_ARRIVALS:
            cl->arrivals=class_def_arrivals();
            break;
          case OP_DEPARTURES:
            cl->departures=class_def_arrivals();
            break;
          case OP_HARD:
            class_def_hard(cl->hard);
            break;
          case OP_SHARP:
            class_def_hard(cl->sharp);
            break;
          case OP_SHAPE:
            cl->shape=class_def_shape();
            break;
          case OP_SHOVABLE:
            cl->shovable=class_def_shovable();
            break;
          case OP_SUBS:
            ptr=parse_instructions(cla,ptr,hash,compat);
            break;
          case OP_LABEL:
            pushback=1;
            ptr=parse_instructions(cla,ptr,hash,compat);
            break;
          case OP_COLLISIONLAYERS:
            cl->collisionLayers=i=class_def_misc();
            if(i&~255) ParseError("CollisionLayers out of range\n");
            break;
          case 0x0200 ... 0x02FF:
            set_message_ptr(cla,tokenv&255,ptr);
            ptr=parse_instructions(cla,ptr,hash,compat);
            break;
          case 0xC000 ... 0xFFFF:
            set_message_ptr(cla,tokenv+256-0xC000,ptr);
            ptr=parse_instructions(cla,ptr,hash,compat);
            break;
          default: ParseError("Invalid directly inside of a class definition\n");
        }
      } else {
        ParseError("Invalid directly inside of a class definition\n");
      }
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        case OP_PLAYER: cl->cflags|=CF_PLAYER; break;
        case OP_INPUT: cl->cflags|=CF_INPUT; break;
        case OP_COMPATIBLE: cl->cflags|=CF_COMPATIBLE; compat=1; break;
        case OP_COMPATIBLE_C: cl->cflags|=CF_COMPATIBLE; break;
        case OP_QUIZ: cl->cflags|=CF_QUIZ; break;
        case OP_INVISIBLE: cl->oflags|=OF_INVISIBLE; break;
        case OP_VISUALONLY: cl->oflags|=OF_VISUALONLY; break;
        case OP_STEALTHY: cl->oflags|=OF_STEALTHY; break;
        case OP_USERSTATE: cl->oflags|=OF_USERSTATE; break;
        case OP_SHOVABLE: cl->shovable=0x55; break;
        default: ParseError("Invalid directly inside of a class definition\n");
      }
    } else if(Tokenf(TF_CLOSE)) {
      break;
    } else {
      ParseError("Invalid directly inside of a class definition\n");
    }
  }
  end_label_stack(cl->codes,hash);
  if(!cl->nimages) cl->oflags|=OF_INVISIBLE;
  if(main_options['C']) dump_class(cla,ptr,hash);
  if(main_options['H']) {
    for(i=0;i<LOCAL_HASH_SIZE;i++) if(hash[i].id) printf(" \"%s\": %04X\n",hash[i].txt,hash[i].id);
  }
  for(i=0;i<LOCAL_HASH_SIZE;i++) {
    if(vst && hash[i].id>=0x2000 && hash[i].id<0x2800) {
      sqlite3_reset(vst);
      sqlite3_bind_int(vst,1,(hash[i].id&0x07FF)|(cla<<16));
      sqlite3_bind_text(vst,2,hash[i].txt,-1,free);
      while(sqlite3_step(vst)==SQLITE_ROW);
    } else {
      free(hash[i].txt);
    }
  }
  free(hash);
}

static void load_class_numbers(void) {
  int i,n;
  long size=0;
  unsigned char*data=read_lump(FIL_LEVEL,LUMP_CLASS_DEF,&size);
  unsigned char*p;
  if(!data) return;
  for(i=0;i<size-3;) {
    n=data[i]|(data[i+1]<<8);
    if(!n) break;
    if(n>=0x4000) fatal("Malformed CLASS.DEF lump\n");
    i+=2;
    p=data+i;
    while(i<size && data[i++]);
    if(i==size && data[i-1]) fatal("Malformed CLASS.DEF lump\n");
    initialize_class(n,CF_NOCLASS2,p);
  }
  i+=2;
  for(;i<size-3;) {
    n=data[i]|(data[i+1]<<8);
    if(n<256 || n>=0x4100) fatal("Malformed CLASS.DEF lump\n");
    n-=256;
    i+=2;
    p=data+i;
    while(i<size && data[i++]);
    if(i==size && data[i-1]) fatal("Malformed CLASS.DEF lump\n");
    if(messages[n]) fatal("Duplicate message number %d\n",n+256);
    messages[n]=strdup(p);
    if(!messages[n]) fatal("Allocation failed\n");
  }
  free(data);
}

void load_classes(void) {
  int i;
  int gloptr=0;
  Hash*glolocalhash;
  char*nam=sqlite3_mprintf("%s.class",basefilename);
  sqlite3_stmt*vst=0;
  fprintf(stderr,"Loading class definitions...\n");
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
  strcpy(tokenstr,"bit"); glohash[look_hash_mac()].id=MAC_BIT;
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
  load_class_numbers();
  if(userdb && (i=sqlite3_prepare_v2(userdb,"INSERT INTO `VARIABLES`(`ID`,`NAME`) VALUES(?1,?2);",-1,&vst,0))) fatal("SQL error (%d): %s\n",i,sqlite3_errmsg(userdb));
  for(;;) {
    nxttok();
    if(tokent==TF_EOF) goto done;
    if(tokent!=TF_OPEN) ParseError("Expected open parenthesis\n");
    nxttok();
    if(Tokenf(TF_FUNCTION)) {
      functions[tokenv&0x3FFF]=gloptr;
      begin_label_stack();
      gloptr=parse_instructions(0,gloptr,glolocalhash,0);
      end_label_stack(classes[0]->codes,glolocalhash);
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        case 0x4000 ... 0x7FFF: // Class definition
          class_definition(tokenv-0x4000,vst);
          break;
        case 0x0200 ... 0x02FF: case 0xC000 ... 0xFFFF: // Default message handler
          begin_label_stack();
          set_message_ptr(0,tokenv&0x8000?(tokenv&0x3FFF)+256:tokenv-0x0200,gloptr);
          gloptr=parse_instructions(0,gloptr,glolocalhash,0);
          end_label_stack(classes[0]->codes,glolocalhash);
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
          back_color=inv_back_color=tokenv;
          nxttok();
          if(tokent==TF_INT) {
            if(tokenv&~255) ParseError("Background color out of range\n");
            inv_back_color=tokenv;
            nxttok();
          }
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
        case OP_SYNCHRONIZE:
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          i=tokenv;
          if(i&~7) ParseError("Animation slot number out of range\n");
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv<1 || tokenv>255) ParseError("Length of synchronized animation out of range\n");
          anim_slot[i].length=tokenv;
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv<1 || tokenv>255) ParseError("Synchronized animation speed out of range\n");
          anim_slot[i].speed=tokenv;
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
  if(main_options['C']) dump_class(0,gloptr,glolocalhash);
  if(main_options['H']) {
    for(i=0;i<HASH_SIZE;i++) if(glohash[i].id) printf("\"%s\": %04X\n",glohash[i].txt,glohash[i].id);
    for(i=0;i<LOCAL_HASH_SIZE;i++) if(glolocalhash[i].id) printf(" \"%s\": %04X\n",glolocalhash[i].txt,glolocalhash[i].id);
  }
  for(i=0;i<LOCAL_HASH_SIZE;i++) free(glolocalhash[i].txt);
  free(glolocalhash);
  for(i=0;i<num_functions;i++) if(functions[i]==0xFFFF) {
    int j;
    for(j=0;j<HASH_SIZE;j++) if(glohash[j].id==i+0x8000) fatal("Function &%s mentioned but not defined\n",glohash[j].txt);
    fatal("Function mentioned but not defined\n");
  }
  for(i=0;i<HASH_SIZE;i++) {
    if(vst && glohash[i].id>=0x2800 && glohash[i].id<0x3000) {
      sqlite3_reset(vst);
      sqlite3_bind_int(vst,1,glohash[i].id&0x07FF);
      sqlite3_bind_text(vst,2,glohash[i].txt,-1,free);
      while(sqlite3_step(vst)==SQLITE_ROW);
    } else {
      free(glohash[i].txt);
    }
  }
  if(vst) sqlite3_finalize(vst);
  free(glohash);
  for(i=1;i<undef_class;i++) if(classes[i] && (classes[i]->cflags&CF_NOCLASS1)) fatal("Class $%s mentioned but not defined\n",classes[i]->name);
  if(macros) for(i=0;i<MAX_MACRO;i++) if(macros[i]) free_macro(macros[i]);
  free(macros);
  fprintf(stderr,"Done\n");
}
