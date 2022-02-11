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
Uint16 array_size;
Uint16*orders;
Uint8 norders;
Uint16 control_class;

char*ll_head;
DisplayColumn*ll_disp;
Uint8 ll_ndisp;
DataColumn*ll_data;
Uint8 ll_ndata;
Uint8 ll_naggregate;
Uint16*ll_code;

#define HASH_SIZE 8888
#define LOCAL_HASH_SIZE 5555
typedef struct {
  Uint16 id;
  char*txt;
} Hash;

/*
  Global hash:
    1000-10FF = User flags
    2800-2FFF = Variables
    8000-BFFF = Functions
    C000-FFFF = Macros
  Local hash:
    2000-27FF = Variables
    8000-FFFF = Labels
*/

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
  ['$']=1, ['!']=1, ['\'']=1, ['#']=1, ['@']=1, ['%']=1, ['&']=1, [':']=1, ['^']=1,
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
#define MAC_MAKE 0xFFCB
#define MAC_VERSION 0xFFE0
#define MAC_DEFINE 0xFFE1
#define MAC_INCLUDE 0xFFE2
#define MAC_CALL 0xFFE3
#define MAC_APPEND 0xFFE4
#define MAC_EDIT 0xFFE5
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
        case 'd': tokenstr[i++]=30; isimg=1; break;
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
  cl->shape=cl->shovable=cl->collisionLayers=cl->nimages=cl->order=0;
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
    if(tokent!=(TF_MACRO|TF_CLOSE)) *tokenstr=0;
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
        if(e && *e) goto norm;
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
      if(fl) ParseError("Invalid use of , and = in token\n");
      tokent=TF_NAME;
      tokenv=find_user_sound(tokenstr);
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
      if(fl&TF_EQUAL) tokenv+=0x1000;
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
    case '^':
      tokent=TF_NAME|TF_ABNORMAL|fl;
      tokenv=OP_USERFLAG;
      break;
  }
}

static void define_macro(Uint16 name,Uint8 q) {
  int i;
  TokenList**t;
  if(main_options['M']) printf("M< %04X %04X \"%s\" %d\n",name,glohash[name].id,glohash[name].txt,q);
  if(glohash[name].id<0xC000) fatal("Confusion\n");
  if(glohash[name].id<MAX_MACRO+0xC000) {
    i=glohash[name].id-0xC000;
    if(q) {
      free_macro(macros[i]);
      macros[i]=0;
    }
  } else {
    q=1;
    for(i=0;i<MAX_MACRO;i++) if(!macros[i]) break;
    if(i==MAX_MACRO) ParseError("Too many macro definitions\n");
  }
  glohash[name].id=i+0xC000;
  t=macros+i;
  if(!q) while(*t) t=&(*t)->next;
  i=1;
  for(;;) {
    nxttok1();
    if(main_options['L'] && main_options['M']) {
      int j;
      printf("*: %5d %04X %08X \"",i,tokent,tokenv);
      for(j=0;tokenstr[j];j++) {
        if(tokenstr[j]<32 || tokenstr[j]>126) printf("<%02X>",tokenstr[j]&255);
        else putchar(tokenstr[j]);
      }
      printf("\"\n");
    }
    if(tokent==TF_MACRO+TF_OPEN && ++i>65000) ParseError("Too much macro nesting\n");
    if(tokent==TF_MACRO+TF_CLOSE && !--i) break;
    *t=add_macro();
    t=&(*t)->next;
  }
  if(main_options['M']) printf("M> %04X %04X %p\n",name,glohash[name].id,macros[glohash[name].id-0xC000]);
}

static void begin_include_file(const char*name) {
  InputStack*nxt=inpstack;
  inpstack=malloc(sizeof(InputStack));
  if(!inpstack) fatal("Allocation failed\n");
  inpstack->classfp=classfp;
  inpstack->linenum=linenum;
  inpstack->next=nxt;
  linenum=1;
  if(*name=='.' || *name=='_' || *name=='-' || !*name || name[strspn(name,"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ_-")])
   ParseError("Improper name of include file \"%s\"\n",name);
  if(main_options['z']) {
    if(strlen(name)<9 || memcmp(name-strlen(name),".include",8)) ParseError("Include file name doesn't end with .include\n");
    classfp=composite_slice(name,0)?:fopen(name,"r");
  } else {
    classfp=fopen(name,"r");
  }
  if(!classfp) ParseError("Cannot open include file \"%s\": %m\n",name);
}

static void begin_macro(TokenList*mac) {
  MacroStack*ms=malloc(sizeof(MacroStack));
  TokenList**ap=0;
  int a=0;
  int b=0;
  int c=0;
  if(StackProtection()) fatal("Stack overflow\n");
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
            if(tokenv<32) n|=1<<tokenv;
          }
          tokent=TF_INT;
          tokenv=n;
          break;
        case MAC_MAKE:
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          s=tokenstr;
          n=0;
          while(*s) switch(*s++) {
            case '=': n|=0x0001; break;
            case ',': n|=0x0002; break;
            case ':': n|=0x0004; break;
            case '@': n|=0x0008; break;
            case '%': n|=0x0010; break;
            case '#': n|=0x0020; break;
            case '$': n|=0x0040; break;
            case '&': n|=0x0080; break;
            case '^': n|=0x0100; break;
            case '\'': n|=0x0400; break;
            case 'D': n|=0x0800; break;
            case '+': n|=0x1000; break;
            case 'C': n|=0x2000; break;
            case 'A': n|=0x4000; break;
            case '!': n|=0x8000; break;
            default: ParseError("Incorrect mode of {make}\n");
          }
          nxttok();
          if(tokent==TF_INT || ((tokent&TF_NAME) && !(tokenv&~255))) {
            n|=0x10000;
            snprintf(tokenstr,32,"%llu",(long long)tokenv);
          } else if(!(tokent&TF_NAME) || tokenv!=OP_STRING) {
            ParseError("Numeric or string literal expected\n");
          }
          s=strdup(tokenstr);
          if(!s) fatal("Allocation failed\n");
          nxttok();
          strcpy(tokenstr,s);
          free(s);
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          if(n&0x10000) {
            n&=0xFFFF;
            tokent=TF_INT;
            tokenv=strtol(tokenstr,0,10);
          }
          switch(n) {
            case 0x0004: // :
              ReturnToken(TF_NAME|TF_ABNORMAL,OP_LABEL);
            case 0x0005: // =:
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_EQUAL,OP_LABEL);
            case 0x0006: // ,:
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_COMMA,OP_LABEL);
            case 0x0008: // @
              tokent=TF_NAME;
              tokenv=look_hash(glohash,HASH_SIZE,0x2800,0x2FFF,num_globals+0x2800,"user global variables")?:(num_globals++)+0x2800;
              return;
            case 0x0009: // =@
              tokent=TF_NAME|TF_EQUAL;
              tokenv=look_hash(glohash,HASH_SIZE,0x2800,0x2FFF,num_globals+0x2800,"user global variables")?:(num_globals++)+0x2800;
              return;
            case 0x0010: // %
              ReturnToken(TF_NAME|TF_ABNORMAL,OP_LOCAL);
            case 0x0011: // =%
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_EQUAL,OP_LOCAL);
            case 0x0020: // #
              tokent=TF_NAME;
              tokenv=look_message_name()+0xC000;
              return;
            case 0x0040: // $
              tokent=TF_NAME;
              tokenv=look_class_name()+0x4000;
              return;
            case 0x0080: // &
              tokent=TF_FUNCTION|TF_ABNORMAL;
              tokenv=look_hash(glohash,HASH_SIZE,0x8000,0xBFFF,num_functions+0x8000,"user functions")?:(num_functions++)+0x8000;
              return;
            case 0x0100: // ^
              ReturnToken(TF_NAME|TF_ABNORMAL,OP_USERFLAG);
            case 0x0101: // =^
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_EQUAL,OP_USERFLAG);
            case 0x0102: // ,^
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_COMMA,OP_USERFLAG);
            case 0x0103: // =,^
              ReturnToken(TF_NAME|TF_ABNORMAL|TF_EQUAL|TF_COMMA,OP_USERFLAG);
            case 0x0400: // '
              if(tokent==TF_INT) {
                n=tokenv;
              } else {
                for(n=8;n<256;n++) if(heromesh_key_names[n] && !strcmp(heromesh_key_names[n],tokenstr)) break;
              }
              if(!heromesh_key_names[n&=255]) ParseError("Invalid key token (%d)\n",n);
              ReturnToken(TF_NAME|TF_KEY,n);
            case 0x0800: // D
              if(tokent==TF_INT) ReturnToken(TF_NAME|TF_DIR,tokenv&15);
              else ReturnToken(TF_NAME|TF_DIR,strtol(tokenstr,0,10)&15);
            case 0x1000: // +
              ReturnToken(TF_INT,strtol(tokenstr,0,10));
            case 0x1001: // =+
              ReturnToken(TF_INT,strtol(tokenstr,0,2));
            case 0x1002: // ,+
              ReturnToken(TF_INT,strtol(tokenstr,0,16));
            case 0x1003: // =,+
              ReturnToken(TF_INT,strtol(tokenstr,0,8));
            case 0x2000: // C
              if(tokent!=TF_INT) tokenv=strtol(tokenstr,0,10);
              *tokenstr=n=tokenv&255;
              if(n<32) tokenstr[0]=31,tokenstr[1]=n,tokenstr[2]=0; else tokenstr[1]=0;
              ReturnToken(TF_NAME|TF_ABNORMAL,OP_STRING);
            case 0x2002: // ,C
              if(tokent!=TF_INT) tokenv=strtol(tokenstr,0,10);
              if(n<32) {
                *tokenstr=0;
              } else {
                tokenstr[0]=16;
                tokenstr[1]=n;
                tokenstr[2]=0;
              }
              ReturnToken(TF_NAME|TF_ABNORMAL,OP_STRING);
            case 0x4000: // A
              ReturnToken(TF_INT,*tokenstr&255);
            default: ParseError("Incorrect mode of {make}\n");
          }
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
          define_macro(look_hash_mac(),1);
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
        case MAC_APPEND:
          if(!macros) {
            macros=calloc(MAX_MACRO,sizeof(TokenList*));
            if(!macros) fatal("Allocation failed\n");
          }
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          define_macro(look_hash_mac(),0);
          goto again;
        case MAC_EDIT:
          if(!macros) ParseError("Cannot edit nonexistent macro\n");
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          n=glohash[look_hash_mac()].id;
          if(n<0xC000 || n>MAX_MACRO+0xC000-1 || !macros[n-0xC000]) ParseError("Undefined macro: {%s}\n",tokenstr);
          nxttok();
          n-=0xC000;
          if(macros[n]->t&(TF_MACRO|TF_EOF)) ParseError("Invalid edit token\n");
          free(macros[n]->str);
          macros[n]->t=tokent;
          macros[n]->v=tokenv;
          macros[n]->str=0;
          if(*tokenstr) {
            macros[n]->str=strdup(tokenstr);
            if(!macros[n]->str) fatal("Allocation failed\n");
          }
          if(main_options['M']) {
            int i;
            printf("M= @%4d %04X %08X \"",linenum,tokent,tokenv);
            for(i=0;tokenstr[i];i++) {
              if(tokenstr[i]<32 || tokenstr[i]>126) printf("<%02X>",tokenstr[i]&255);
              else putchar(tokenstr[i]);
            }
            printf("\"\n");
          }
          nxttok();
          if(tokent!=TF_MACRO+TF_CLOSE) ParseError("Too many macro arguments\n");
          goto again;
        case MAC_UNDEFINED:
          ParseError("Undefined macro: {%s}\n",tokenstr);
          break;
        default:
          ParseError("Strange macro token: 0x%04X\n",glohash[tokenv].id);
      }
      if(main_options['M']) printf("M- %5d %04X %08X\n",linenum,tokent,tokenv);
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
      case 0x0300 ... 0x03FF: return UVALUE(tokenv&255,TY_SOUND);
      case 0x0400 ... 0x04FF: return UVALUE(tokenv&255,TY_USOUND);
      case 0x4000 ... 0x7FFF: return CVALUE(tokenv-0x4000);
      case OP_STRING: return UVALUE(pool_string(tokenstr),TY_STRING);
      case OP_BITCONSTANT ... OP_BITCONSTANT_LAST: return NVALUE(1<<(tokenv&31));
      case 0xC000 ... 0xFFFF: return MVALUE(tokenv-0xBF00);
    }
  }
  ParseError("Constant value expected\n");
}

static Uint32 parse_array(void) {
  Uint32 x,y,z;
  nxttok();
  if(tokent!=TF_INT) ParseError("Number expected\n");
  x=tokenv;
  nxttok();
  if(tokent!=TF_INT) ParseError("Number expected\n");
  y=tokenv;
  if(x<1 || x>64 || y<1 || y>1024) ParseError("Array dimension out of range\n");
  z=array_size;
  if(z+x*y>0xFFFE) ParseError("Out of array memory\n");
  array_size+=x*y;
  return z|((y-1)<<16)|((x-1)<<26);
}

static void define_user_flags(Uint16 ca,Uint16 cb) {
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) break;
    if(!Tokenf(TF_NAME) || tokenv!=OP_USERFLAG) ParseError("User flag or close parenthesis expected\n");
    if(ca>cb) ParseError("Too many user flags of this kind\n");
    if(look_hash(glohash,HASH_SIZE,0x1000,0x10FF,ca++,"user flags")) ParseError("Duplicate definition of ^%s\n",tokenstr);
  }
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

static int parse_pattern(int cla,int ptr,Hash*hash) {
  Class*cl=classes[cla];
  Uint8 depth=0;
  Uint16 nest[32];
  Uint16 nest0[32];
  int x,y;
  for(;;) {
    nxttok();
    if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
    if(Tokenf(TF_DIR)) {
      cl->codes[ptr++]=tokenv&15;
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        case OP_ADD: case OP_CLIMB: case OP_HEIGHT:
        case OP_LOC: case OP_MARK: case OP_SUB:
        case OP_QUEEN: case OP_ROOK: case OP_BISHOP:
        case OP_DIR: case OP_DIR_C: case OP_DIR_E: case OP_DIR_EC:
        case OP_OBJTOPAT: case OP_OBJBOTTOMAT: case OP_CUT: case OP_MUL:
        case OP_OBJABOVE: case OP_OBJBELOW: case OP_TRACE: case OP_NEXT:
        case 0x0200 ... 0x02FF: // message
        case 0x4000 ... 0x7FFF: // class
        case 0xC000 ... 0xFFFF: // message
          cl->codes[ptr++]=tokenv;
          break;
        case OP_BEGIN: case OP_IF:
          if(depth==31) ParseError("Too much pattern nesting\n");
          nest0[depth]=nest[depth]=ptr;
          cl->codes[ptr++]=tokenv;
          cl->codes[ptr++]=0;
          depth++;
          break;
        case OP_ELSE:
          if(!depth) ParseError("Premature end of subpattern\n");
          cl->codes[nest[depth-1]+1]=ptr+1;
          cl->codes[ptr++]=OP_ELSE;
          cl->codes[ptr++]=cl->codes[nest[depth-1]];
          cl->codes[nest[depth-1]]=OP_IF;
          cl->codes[ptr++]=0;
          nest[depth-1]=ptr-2;
          break;
        case OP_THEN:
          if(!depth) ParseError("Premature end of subpattern\n");
          cl->codes[nest[--depth]+1]=ptr;
          cl->codes[ptr++]=OP_THEN;
          break;
        case OP_AGAIN:
          if(!depth) ParseError("Premature end of subpattern\n");
          cl->codes[ptr++]=OP_AGAIN;
          cl->codes[ptr++]=nest0[depth-1];
          cl->codes[nest[--depth]+1]=ptr;
          cl->codes[ptr++]=OP_THEN;
          break;
        case OP_USERFLAG:
          x=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
          if(!x) ParseError("User flag ^%s not defined\n",tokenstr);
          if(Tokenf(TF_COMMA)) x+=0x100;
          if(Tokenf(TF_EQUAL)) ParseError("Improper token in pattern\n");
          cl->codes[ptr++]=x;
          break;
        default: ParseError("Improper token in pattern\n");
      }
    } else if(Tokenf(TF_FUNCTION)) {
      cl->codes[ptr++]=OP_FUNCTION;
      cl->codes[ptr++]=tokenv&0x3FFF;
    } else if(tokent==TF_OPEN) {
      nxttok();
      if(Tokenf(TF_MACRO) || !Tokenf(TF_NAME)) ParseError("Improper token in pattern\n");
      switch(tokenv) {
        case OP_HEIGHT: case OP_CLIMB:
          cl->codes[ptr++]=tokenv+0x0800; // OP_HEIGHT_C or OP_CLIMB_C
          nxttok();
          if(tokent!=TF_INT) ParseError("Number expected\n");
          if(tokenv&~0xFFFF) ParseError("Number out of range\n");
          cl->codes[ptr++]=tokenv;
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Close parentheses expected\n");
          break;
        default: ParseError("Improper token in pattern\n");
      }
    } else if(tokent==TF_CLOSE) {
      if(depth) ParseError("Premature end of pattern\n");
      cl->codes[ptr++]=OP_RET;
      return ptr;
    } else if(Tokenf(TF_EOF)) {
      ParseError("Unexpected end of file\n");
    } else {
      ParseError("Improper token in pattern\n");
    }
    if(ptr>=0xFFEF) ParseError("Out of code space\n");
  }
}

#define CaseT0(t) do{ if(t0!=-1 && t0!=t) ParseError("Type mismatch in case block\n"); t0=t; }while(0)
#define CaseT1(t) do{ if(t1!=-1 && t1!=t) ParseError("Type mismatch in case block\n"); t1=t; }while(0)
static int case_block(int cla,int ptr,Hash*hash) {
  Class*cl=classes[cla];
  int sptr=ptr++;
  Sint8 t0=-1;
  Sint8 t1=-1;
  Uint16 n=0;
  Uint8 e=1;
  Uint8 z=0;
  for(;;) {
    nxttok();
    if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
    if(tokent==TF_OPEN) {
      nxttok();
      if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
      if(n>=256 && tokenv!=OP_ELSE && !Tokenf(TF_NAME)) ParseError("Too many cases\n");
      if(!e) ParseError("Cannot add more cases after the default block\n");
      n++;
      if(Tokenf(TF_INT) || Tokenf(TF_DIR)) {
        if(tokenv) CaseT0(TY_NUMBER);
        cl->codes[ptr++]=tokenv;
      } else if(Tokenf(TF_NAME)) {
        switch(tokenv) {
          case OP_ELSE:
            e=0;
            n--;
            break;
          case 0x0200 ... 0x02FF:
            CaseT0(TY_MESSAGE);
            cl->codes[ptr++]=tokenv+1-0x0200;
            break;
          case 0x4000 ... 0x7FFF:
            CaseT0(TY_CLASS);
            cl->codes[ptr++]=tokenv-0x4000;
            break;
          case 0xC000 ... 0xFFFF:
            CaseT0(TY_MESSAGE);
            cl->codes[ptr++]=(tokenv&0x3FFF)+257;
            break;
          default:
            ParseError("Improper token in case block\n");
            break;
        }
      } else if(Tokenf(TF_EOF)) {
        ParseError("Unexpected end of file\n");
      } else {
        ParseError("Improper token in case block\n");
      }
      nxttok();
      if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
      if(Tokenf(TF_INT) || Tokenf(TF_DIR)) {
        if(tokenv) CaseT1(TY_NUMBER); else z=1;
        if(tokenv&~0xFFFF) ParseError("Number in case block out of range\n");
        cl->codes[ptr++]=tokenv;
      } else if(Tokenf(TF_NAME)) {
        switch(tokenv) {
          case OP_LABEL:
            CaseT1(TY_CODE);
            if(Tokenf(TF_COMMA|TF_EQUAL)) ParseError("Improper token in case block\n");
            tokenv=look_hash(hash,LOCAL_HASH_SIZE,0x8000,0xFFFF,*labelptr,"labels");
            if(!tokenv) tokenv=*labelptr,++*labelptr;
            tokenv-=0x8000;
            cl->codes[ptr++]=labelptr[tokenv];
            if(labelptr[tokenv]==0xFFFF) {
            LabelStack*s=malloc(sizeof(LabelStack));
              if(!s) fatal("Allocation failed\n");
              s->id=tokenv|0x8000;
              s->addr=ptr-1;
              s->next=labelstack;
              labelstack=s;
            }
            break;
          case OP_STRING:
            CaseT1(TY_STRING);
            cl->codes[ptr++]=pool_string(tokenstr)+1;
            break;
          case 0x0200 ... 0x02FF:
            CaseT1(TY_MESSAGE);
            cl->codes[ptr++]=tokenv+1-0x0200;
            break;
          case 0x4000 ... 0x7FFF:
            CaseT1(TY_CLASS);
            cl->codes[ptr++]=tokenv-0x4000;
            break;
          case 0xC000 ... 0xFFFF:
            CaseT1(TY_MESSAGE);
            cl->codes[ptr++]=(tokenv&0x3FFF)+257;
            break;
          default:
            ParseError("Improper token in case block\n");
            break;
        }
      } else if(Tokenf(TF_EOF)) {
        ParseError("Unexpected end of file\n");
      } else {
        ParseError("Improper token in case block\n");
      }
      nxttok();
    } else if(tokent==TF_CLOSE) {
      if(!n) ParseError("Empty case block\n");
      if(z && t1==TY_CODE) ParseError("Type mismatch in case block\n");
      if(e) {
        cl->codes[ptr]=(t1==TY_CODE?ptr+1:0);
        ptr++;
      }
      if(t0==-1) t0=TY_NUMBER;
      if(t1==-1) t1=TY_NUMBER;
      cl->codes[sptr]=(t0<<12)|(t1<<8)|(n-1);
      return ptr;
    } else if(Tokenf(TF_EOF)) {
      ParseError("Unexpected end of file\n");
    } else {
      ParseError("Improper token in case block\n");
    }
    if(ptr>=0xFFEF) ParseError("Out of code space\n");
  }
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
#define FlowPop(x) do{ if(!flowdepth || flowop[--flowdepth]!=(x)) ParseError("Flow control mismatch\n"); prflag=0; }while(0)
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
        case OP_IF: case OP_OR: case OP_AND: case OP_FORK:
          AddInst(tokenv);
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
        case OP_USERFLAG:
          // Opcodes 0x1F00-0x1F1F read a bit; opcodes 0x1F20-0x1F3F set/clear a bit
          x=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
          if(!x) ParseError("User flag ^%s not defined\n",tokenstr);
          if(x<0x1020) y=OP_MISC4;
          else if(x<0x1040) y=OP_MISC5;
          else if(x<0x1060) y=OP_MISC6;
          else if(x<0x1080) y=OP_MISC7;
          else y=OP_COLLISIONLAYERS;
          if(Tokenf(TF_EQUAL)) {
            if(x>=0x1080) ParseError("Flag ^%s is read-only\n",tokenstr);
            if(Tokenf(TF_COMMA)) {
              AddInst(OP_OVER);
              AddInst(y+0x0800);
              AddInst(0x1F20|(x&0x1F));
              AddInst(y+0x1800);
            } else {
              AddInst(y);
              AddInst(0x1F20|(x&0x1F));
              AddInst(y+0x1000);
            }
          } else if(Tokenf(TF_COMMA)) {
            AddInst(y+0x0800);
            AddInst(0x1F00|(x&0x1F));
          } else {
            AddInst(y);
            AddInst(0x1F00|(x&0x1F));
          }
          break;
        case OP_SUPER:
        case OP_SUPER_C:
          if(!ptr || cl->codes[0]!=OP_SUPER) ParseError("Use of Super in a class with no parent class\n");
          AddInst(tokenv);
          break;
        case OP_LINK:
          AddInst(OP_LINK);
          FlowPush(OP_IF);
          cl->codes[++ptr]=cla;
          peep=++ptr;
          break;
        case OP_RTN:
          AddInst(OP_RET);
          FlowPop(OP_IF);
          cl->codes[flowptr[flowdepth]]=peep=ptr;
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
        case OP_PATTERN: case OP_PATTERNS:
        case OP_PATTERN_C: case OP_PATTERNS_C:
        case OP_PATTERN_E: case OP_PATTERNS_E:
          AddInst(tokenv);
          cl->codes[ptr]=peep=parse_pattern(cla,ptr+1,hash);
          ptr=peep;
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
        case OP_CASE:
          cl->codes[ptr++]=OP_CASE;
          ptr=peep=case_block(cla,ptr,hash);
          break;
        case OP_MISC4: y=0x000; goto uflags;
        case OP_MISC4_C: y=0x100; goto uflags;
        case OP_MISC5: y=0x020; goto uflags;
        case OP_MISC5_C: y=0x120; goto uflags;
        case OP_MISC6: y=0x040; goto uflags;
        case OP_MISC6_C: y=0x140; goto uflags;
        case OP_MISC7: y=0x060; goto uflags;
        case OP_MISC7_C: y=0x160; goto uflags;
        case OP_COLLISIONLAYERS: y=0x080; goto uflags;
        case OP_COLLISIONLAYERS_C: y=0x180; goto uflags;
        uflags:
          x=0;
          for(;;) {
            nxttok();
            if(tokent==TF_CLOSE) break;
            if(Tokenf(TF_MACRO) || !Tokenf(TF_NAME) || tokenv!=OP_USERFLAG) ParseError("User flag or close parenthesis expected\n");
            tokenv=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
            if(!tokenv) ParseError("User flag ^%s not defined\n",tokenstr);
            if((tokenv^y)&0xE0) {
              if(y&0x100) ParseError("User flag ^%s belongs to the wrong attribute\n",tokenstr);
            } else {
              if(Tokenf(TF_COMMA)) x&=~(1<<(tokenv&31)); else x|=1<<(tokenv&31);
            }
          }
          tokenv=x;
          goto numeric;
        default:
          ParseError("Invalid parenthesized instruction\n");
      }
    } else if(tokent==TF_CLOSE) {
      if(flowdepth) ParseError("Unterminated flow control structure\n");
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
  if(!ptr) cl->codes=0;
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
    if(!Tokenf(TF_DIR) || tokenv>7) ParseError("Expected absolute direction\n");
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
  if(!txt) fatal("Allocation failed\n");
  for(;;) {
    nxttok();
    if(tokent==TF_CLOSE) break;
    if(Tokenf(TF_NAME) && tokenv==OP_MISC1 && !n) {
      txt[0]=16;
      n=1;
      nxttok();
      if(tokent==TF_CLOSE) break;
      ParseError("Close parenthesis expected\n");
    }
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

static void class_user_flag(Class*cl) {
  int x=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
  if(!x) ParseError("User flag ^%s not defined\n",tokenstr);
  if(x<0x1020) cl->misc4|=1L<<(x&0x1F);
  else if(x<0x1040) cl->misc5|=1L<<(x&0x1F);
  else if(x<0x1060) cl->misc6|=1L<<(x&0x1F);
  else if(x<0x1080) cl->misc7|=1L<<(x&0x1F);
  else cl->collisionLayers|=1<<(x&0x1F);
}

static void set_super_class(Class*cl,int ptr) {
  Class*su=classes[tokenv&0x3FFF];
  int i;
  if(ptr || cl->nmsg) ParseError("Cannot specify superclasses before program blocks\n");
  if(!su || !(su->cflags&CF_GROUP)) ParseError("Cannot inherit from non-abstract class\n");
  cl->codes=malloc(8);
  if(!cl->codes) fatal("Allocation failed\n");
  cl->codes[0]=OP_SUPER;
  cl->codes[1]=tokenv&0x3FFF;
  cl->height=su->height;
  cl->weight=su->weight;
  cl->climb=su->climb;
  cl->density=su->density;
  cl->volume=su->volume;
  cl->strength=su->strength;
  cl->arrivals|=su->arrivals;
  cl->departures|=su->departures;
  cl->temperature=su->temperature;
  cl->misc4|=su->misc4;
  cl->misc5|=su->misc5;
  cl->misc6|=su->misc6;
  cl->misc7|=su->misc7;
  cl->uservars+=su->uservars;
  cl->oflags|=su->oflags;
  for(i=0;i<4;i++) {
    cl->sharp[i]=su->sharp[i];
    cl->hard[i]=su->hard[i];
  }
  cl->cflags|=su->cflags&(CF_PLAYER|CF_INPUT|CF_COMPATIBLE|CF_QUIZ);
  cl->shape=su->shape;
  cl->shovable=su->shovable;
  cl->collisionLayers|=su->collisionLayers;
  if(cl->nmsg=su->nmsg) {
    cl->messages=malloc(cl->nmsg*sizeof(Uint16));
    if(!cl->messages) fatal("Allocation failed\n");
    for(i=0;i<cl->nmsg;i++) cl->messages[i]=(su->messages[i]==0xFFFF)?0xFFFF:0x0000;
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
          if(ptr>0xFDFE) ParseError("Out of code space\n");
          disp=1;
          set_message_ptr(cla,MSG_KEY,ptr);
          cl->codes[ptr]=OP_DISPATCH;
          for(i=1;i<257;i++) cl->codes[ptr+i]=0;
          ptr+=257;
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
          case OP_OTHERS:
            if(!disp) ParseError("Others block without key dispatch block\n");
            if(!(cl->cflags&CF_INPUT)) ParseError("Others block without Input flag\n");
            cl->codes[cl->messages[MSG_KEY]+256]=ptr;
            ptr=parse_instructions(cla,ptr,hash,compat);
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
        case OP_BIZARRO: cl->oflags|=OF_BIZARRO; break;
        case OP_SHOVABLE: cl->shovable=0x55; break;
        case OP_USERFLAG: class_user_flag(cl); break;
        case OP_ABSTRACT: cl->cflags|=CF_GROUP; break;
        case OP_CONNECTION: cl->oflags|=OF_CONNECTION; break;
        case 0x4000 ... 0x7FFF: set_super_class(cl,ptr); ptr=2; break;
        default: ParseError("Invalid directly inside of a class definition\n");
      }
    } else if(Tokenf(TF_CLOSE)) {
      break;
    } else {
      ParseError("Invalid directly inside of a class definition\n");
    }
  }
  end_label_stack(cl->codes,hash);
  if(!cl->nimages && !(cl->cflags&CF_GROUP)) cl->oflags|=OF_INVISIBLE;
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
  if(cl->cflags&CF_GROUP) {
    if(cl->edithelp || cl->gamehelp || cl->nimages) ParseError("Invalid in abstract classes");
  }
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
    if(n>=0x4000) fatal("Malformed CLASS.DEF lump (invalid class number %d)\n",n);
    i+=2;
    p=data+i;
    while(i<size && data[i++]);
    if(i==size && data[i-1]) fatal("Malformed CLASS.DEF lump\n");
    initialize_class(n,CF_NOCLASS2,p);
  }
  i+=2;
  for(;i<size-3;) {
    n=data[i]|(data[i+1]<<8);
    if(n<256 || n>=0x4100) fatal("Malformed CLASS.DEF lump (invalid message number %d)\n",n);
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

static void parse_order_block(void) {
  // OP_MISC1, OP_MISC1_C, etc = properties (_C=reverse)
  // 0x1000...0x10FF = Have flag
  // OP_RET = end of block
  Uint16 beg,ptr;
  ParseError("(Not implemented yet)\n"); //TODO: remove this when it is implemented in exec.c too
  orders=malloc(0x4000*sizeof(Uint16));
  if(!orders) fatal("Allocation failed\n");
  nxttok();
  if(tokent==TF_INT) {
    if(tokenv<1 || tokenv>254) ParseError("Order number out of range\n");
    beg=ptr=tokenv;
    nxttok();
  } else {
    beg=ptr=128;
  }
  while(tokent!=TF_CLOSE) {
    if(tokent!=TF_OPEN) ParseError("Open or close parenthesis expected\n");
    if(ptr>=0x3FFD) ParseError("Out of order memory\n");
    nxttok();
    if(Tokenf(TF_MACRO|TF_COMMA|TF_EQUAL) || !Tokenf(TF_NAME)) ParseError("Unexpected token in (Order) block\n");
    orders[++norders]=ptr;
    if(norders==beg) ParseError("Too many orders\n");
    switch(tokenv) {
      case OP_INPUT: case OP_PLAYER: case OP_CONTROL:
        orders[ptr++]=tokenv;
        break;
      case OP_USERFLAG:
        tokenv=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
        if(!tokenv) ParseError("User flag ^%s not defined\n",tokenstr);
        orders[ptr++]=tokenv;
        break;
      default: ParseError("Unexpected token in (Order) block\n");
    }
    nxttok();
    while(tokent!=TF_CLOSE) {
      if(Tokenf(TF_MACRO|TF_EQUAL) || !Tokenf(TF_NAME)) ParseError("Unexpected token in (Order) block\n");
      if(ptr>=0x3FFE) ParseError("Out of order memory\n");
      switch(tokenv) {
        case OP_MISC1: case OP_MISC1_C:
        case OP_MISC2: case OP_MISC2_C:
        case OP_MISC3: case OP_MISC3_C:
        case OP_MISC4: case OP_MISC4_C:
        case OP_MISC5: case OP_MISC5_C:
        case OP_MISC6: case OP_MISC6_C:
        case OP_MISC7: case OP_MISC7_C:
        case OP_TEMPERATURE: case OP_TEMPERATURE_C:
        case OP_DENSITY: case OP_DENSITY_C:
        case OP_XLOC: case OP_XLOC_C:
        case OP_YLOC: case OP_YLOC_C:
        case OP_IMAGE: case OP_IMAGE_C:
          orders[ptr++]=tokenv;
          break;
      }
      nxttok();
    }
    orders[ptr++]=OP_RET;
  }
  if(!norders) ParseError("Empty (Order) block\n");
  orders=realloc(orders,ptr*sizeof(Uint16))?:orders;
}

static void set_class_orders(void) {
  int i,j,k;
  for(i=1;i<undef_class;i++) if(classes[i] && (classes[i]->nmsg || classes[0]->nmsg) || !(classes[i]->cflags&(CF_GROUP|CF_NOCLASS2))) {
    for(j=1;j<norders;j++) {
      k=orders[orders[j]];
      switch(k) {
        case 0x1000 ... 0x101F: if(classes[i]->misc4&(1UL<<(k&0x1F))) goto found; break;
        case 0x1020 ... 0x103F: if(classes[i]->misc5&(1UL<<(k&0x1F))) goto found; break;
        case 0x1040 ... 0x105F: if(classes[i]->misc6&(1UL<<(k&0x1F))) goto found; break;
        case 0x1060 ... 0x107F: if(classes[i]->misc7&(1UL<<(k&0x1F))) goto found; break;
        case 0x1080 ... 0x1087: if(classes[i]->collisionLayers&(1L<<(k&0x07))) goto found; break;
        case OP_PLAYER: if(classes[i]->cflags&CF_PLAYER) goto found; break;
        case OP_INPUT: if(classes[i]->cflags&CF_INPUT) goto found; break;
        case OP_CONTROL: if(i==control_class) goto found; break;
      }
      continue;
      found: classes[i]->order=j; break;
    }
  }
}

static int level_table_code(int ptr,Hash*hash) {
  int flowdepth=0;
  Uint16 flowptr[64];
  int x;
  for(;;) {
    if(ptr>=0xFFFA) ParseError("Out of memory\n");
    nxttok();
    if(Tokenf(TF_MACRO)) ParseError("Unexpected macro\n");
    if(tokent==TF_CLOSE) {
      ll_code[ptr++]=OP_RET;
      return ptr;
    } else if(Tokenf(TF_INT)) {
      if(!(tokenv&~0xFFL)) ll_code[ptr++]=tokenv;
      else if(!(tokenv&~0xFFFFL)) ll_code[ptr++]=OP_INT16,ll_code[ptr++]=tokenv;
      else ll_code[ptr++]=OP_INT32,ll_code[ptr++]=OP_INT32,ll_code[ptr++]=tokenv>>16,ll_code[ptr++]=tokenv;
    } else if(Tokenf(TF_NAME)) {
      switch(tokenv) {
        case OP_IF:
          if(flowdepth==64) ParseError("Too much flow control nesting\n");
          ll_code[ptr++]=OP_IF;
          flowptr[flowdepth++]=ptr++;
          break;
        case OP_ELSE:
          if(!flowdepth) ParseError("Flow control mismatch\n");
          ll_code[flowptr[flowdepth-1]]=ptr+2;
          ll_code[ptr++]=OP_GOTO;
          flowptr[flowdepth-1]=ptr++;
          break;
        case OP_THEN:
          if(!flowdepth) ParseError("Flow control mismatch\n");
          ll_code[flowptr[--flowdepth]]=ptr;
          break;
        case OP_STRING:
          ll_code[ptr++]=OP_STRING;
          ll_code[ptr++]=pool_string(tokenstr);
          break;
        case OP_LOCAL:
          x=look_hash(hash,LOCAL_HASH_SIZE,0x200,0x23F,ll_naggregate+0x200,"aggregates")?:(ll_naggregate++)+0x200;
          ll_code[ptr++]=OP_LOCAL;
          ll_code[ptr++]=x-0x200;
          break;
        case OP_USERFLAG:
          if(Tokenf(TF_COMMA|TF_EQUAL)) ParseError("Invalid instruction token\n");
          x=look_hash(glohash,HASH_SIZE,0x1000,0x10FF,0,"user flags");
          if(!x) ParseError("User flag ^%s not defined\n",tokenstr);
          ll_code[ptr++]=OP_USERFLAG;
          ll_code[ptr++]=x;
          break;
        default:
          if(Tokenf(TF_ABNORMAL)) ParseError("Invalid instruction token\n");
          ll_code[ptr++]=tokenv;
      }
    } else {
      ParseError("Invalid instruction token\n");
    }
  }
}

static void level_table_definition(void) {
  sqlite3_str*str=sqlite3_str_new(0);
  unsigned char buf[0x2000];
  Hash*hash=calloc(LOCAL_HASH_SIZE,sizeof(Hash));
  DataColumn*datac=calloc(0x41,sizeof(DataColumn));
  DataColumn*aggrc=calloc(0x41,sizeof(DataColumn));
  DisplayColumn*dispc=calloc(0x41,sizeof(DisplayColumn));
  int ptr=0;
  int last=0;
  int i;
  if(ll_naggregate || ll_ndata || ll_ndisp || ll_head || ll_code) ParseError("LevelTable block is already defined\n");
  ll_code=malloc(0x10000*sizeof(Uint16));
  if(!hash || !ll_code || !datac || !aggrc || !dispc) fatal("Allocation failed\n");
  sqlite3_str_appendchar(str,1,0xB3);
  for(;;) {
    nxttok();
    if(tokent==TF_EOF) ParseError("Unexpected end of file\n");
    if(tokent==TF_CLOSE) break;
    if(tokent!=TF_OPEN) ParseError("Expected ( or )\n");
    nxttok();
    if(!Tokenf(TF_NAME)) ParseError("Unexpected token in (LevelTable) block\n");
    switch(tokenv) {
      case OP_LABEL:
        i=look_hash(hash,LOCAL_HASH_SIZE,0x100,0x13F,ll_ndata+0x100,"data columns")?:(ll_ndata++)+0x100;
        i-=0x100;
        if(datac[i].name) ParseError("Duplicate definition\n");
        datac[i].name=strdup(tokenstr);
        if(!datac[i].name) fatal("Allocation failed\n");
        datac[i].ptr=ptr;
        if(Tokenf(TF_COMMA)) datac[i].sgn=1;
        ptr=level_table_code(ptr,hash);
        break;
      case OP_STRING:
        if(last) ParseError("Extra columns after fill column\n");
        for(i=0;tokenstr[i];i++) if(!(tokenstr[i]&~31)) ParseError("Improper column heading\n");
        strcpy(buf,tokenstr);
        // Data
        nxttok();
        if(Tokenf(TF_NAME) && tokenv==OP_LABEL) {
          i=look_hash(hash,LOCAL_HASH_SIZE,0x100,0x13F,ll_ndata+0x100,"display columns")?:(ll_ndata++)+0x100;
          dispc[ll_ndisp].data=i-0x100;
        } else {
          ParseError("Syntax error\n");
        }
        // Width
        nxttok();
        if(tokent==TF_INT && tokenv>0 && tokenv<=255) {
          dispc[ll_ndisp].width=tokenv;
        } else if(Tokenf(TF_NAME) && tokenv==OP_MUL) {
          dispc[ll_ndisp].width=255;
          dispc[ll_ndisp].flag|=1;
        } else {
          ParseError("Syntax error\n");
        }
        // Format
        nxttok();
        if(!Tokenf(TF_NAME) || tokenv!=OP_STRING || !*tokenstr) ParseError("Syntax error\n");
        if(*tokenstr<32 || *tokenstr>126) ParseError("Syntax error\n");
        if(tokenstr[1]>0 && tokenstr[1]<31) ParseError("Syntax error\n");
        if(tokenstr[1]==31) tokenstr[1]=tokenstr[2],tokenstr[2]=tokenstr[3];
        if(tokenstr[1] && tokenstr[2]) ParseError("Syntax error\n");
        memcpy(dispc[ll_ndisp].form,tokenstr,2);
        // Colour
        nxttok();
        if(tokent==TF_INT) {
          if(tokenv&~255) ParseError("Color out of range\n");
          dispc[ll_ndisp].color=tokenv;
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Syntax error\n");
        } else if(tokent==TF_OPEN) {
          dispc[ll_ndisp].flag|=2;
          dispc[ll_ndisp].ptr=ptr;
          for(;;) {
            if(++dispc[ll_ndisp].color==255) ParseError("Too many colors\n");
            nxttok();
            if(tokent!=TF_INT) ParseError("Number expected\n");
            i=tokenv;
            if(i<-127 || i>127) ParseError("Number out of range\n");
            nxttok();
            if(tokent!=TF_INT) ParseError("Number expected\n");
            if(ptr>=0xFFFE) ParseError("Out of memory\n");
            if(tokenv&~255) ParseError("Color out of range\n");
            ll_code[ptr++]=tokenv|((i+128)<<8);
            nxttok();
            if(tokent!=TF_CLOSE) ParseError("Close parenthesis expected\n");
            nxttok();
            if(tokent==TF_CLOSE) break;
            if(tokent!=TF_OPEN) ParseError("Syntax error\n");
          };
        } else if(tokent==TF_CLOSE) {
          dispc[ll_ndisp].color=0xFF;
        }
        // End
        if(dispc[ll_ndisp].flag&1) {
          last=1;
          sqlite3_str_appendall(str,buf);
        } else {
          i=dispc[ll_ndisp].width;
          sqlite3_str_appendf(str,"%-*.*s\xB3",i,i,buf);
        }
        ll_ndisp++;
        break;
      case OP_LOCAL:
        i=look_hash(hash,LOCAL_HASH_SIZE,0x200,0x23F,ll_naggregate+0x200,"aggregates")?:(ll_naggregate++)+0x200;
        i-=0x200;
        if(aggrc[i].ag) ParseError("Duplicate definition\n");
        nxttok();
        if(!Tokenf(TF_NAME)) ParseError("Improper aggregate\n");
        switch(tokenv) {
          case OP_ADD: aggrc[i].ag=1; break;
          case OP_MIN_C: aggrc[i].sgn=1; case OP_MIN: aggrc[i].ag=2; break;
          case OP_MAX_C: aggrc[i].sgn=1; case OP_MAX: aggrc[i].ag=3; break;
          default: ParseError("Improper aggregate\n");
        }
        aggrc[i].ptr=ptr;
        ptr=level_table_code(ptr,hash);
        break;
      default: ParseError("Unexpected token in (LevelTable) block\n");
    }
  }
  ll_head=sqlite3_str_finish(str);
  if(!ll_head) fatal("Allocation failed\n");
  for(i=0;i<ll_ndata;i++) if(!datac[i].name) ParseError("Undefined data column\n");
  for(i=0;i<ll_naggregate;i++) if(!aggrc[i].ag) ParseError("Undefined aggregate\n");
  ll_data=realloc(datac,(ll_ndata+ll_naggregate)*sizeof(DataColumn));
  if(!ll_data) fatal("Allocation failed\n");
  for(i=0;i<ll_naggregate;i++) ll_data[i+ll_ndata]=aggrc[i];
  free(aggrc);
  ll_disp=realloc(dispc,(ll_ndisp)*sizeof(DisplayColumn))?:dispc;
  // The next line will result in an invalid pointer if ptr is zero,
  // but this is harmless, since ll_code is never accessed if ptr is zero.
  ll_code=realloc(ll_code,ptr*sizeof(Uint16))?:ll_code;
  for(i=0;i<LOCAL_HASH_SIZE;i++) free(hash[i].txt);
  free(hash);
  if(main_options['C']) {
    printf("<<<LevelTable>>>\n  Data/Aggregate:\n");
    for(i=0;i<ll_ndata+ll_naggregate;i++) {
      printf("    %d: ",i);
      if(i<ll_ndata) printf(" \"%s\"",ll_data[i].name); else printf(" (%d)",ll_data[i].ag);
      printf(" 0x%04X %c\n",ll_data[i].ptr,ll_data[i].sgn?'s':'u');
    }
    printf("  Display:\n");
    for(i=0;i<ll_ndisp;i++) printf("    %d: data=%d width=%d format=%c%c color=%d flag=%d ptr=0x%04X\n",i
     ,ll_disp[i].data,ll_disp[i].width,ll_disp[i].form[0]?:' ',ll_disp[i].form[1]?:' ',ll_disp[i].color,ll_disp[i].flag,ll_disp[i].ptr);
    printf("  Codes:");
    for(i=0;i<ptr;i++) {
      if(!(i&15)) printf("\n    [%04X]",i);
      printf(" %04X",ll_code[i]);
    }
    putchar('\n');
    printf("---\n\n");
  }
}

void load_classes(void) {
  int i;
  int gloptr=0;
  Hash*glolocalhash;
  char*nam=sqlite3_mprintf("%s.class",basefilename);
  sqlite3_stmt*vst=0;
  fprintf(stderr,"Loading class definitions...\n");
  if(!nam) fatal("Allocation failed\n");
  classfp=main_options['z']?composite_slice(".class",1):fopen(nam,"r");
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
  strcpy(tokenstr,"make"); glohash[look_hash_mac()].id=MAC_MAKE;
  strcpy(tokenstr,"version"); glohash[look_hash_mac()].id=MAC_VERSION;
  strcpy(tokenstr,"define"); glohash[look_hash_mac()].id=MAC_DEFINE;
  strcpy(tokenstr,"include"); glohash[look_hash_mac()].id=MAC_INCLUDE;
  strcpy(tokenstr,"call"); glohash[look_hash_mac()].id=MAC_CALL;
  strcpy(tokenstr,"append"); glohash[look_hash_mac()].id=MAC_APPEND;
  strcpy(tokenstr,"edit"); glohash[look_hash_mac()].id=MAC_EDIT;
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
          if(Tokenf(TF_NAME) && tokenv==OP_ARRAY) {
            initglobals[i].t=TY_ARRAY;
            initglobals[i].u=parse_array();
          } else {
            initglobals[i]=parse_constant_value();
          }
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
        case OP_MISC4: define_user_flags(0x1000,0x101F); break;
        case OP_MISC5: define_user_flags(0x1020,0x103F); break;
        case OP_MISC6: define_user_flags(0x1040,0x105F); break;
        case OP_MISC7: define_user_flags(0x1060,0x107F); break;
        case OP_COLLISIONLAYERS: define_user_flags(0x1080,0x1087); break;
        case OP_CODEPAGE:
          nxttok();
          if(tokent!=TF_INT || tokenv<1 || tokenv>0x7FFFFF) ParseError("Number from 1 to 8388607 expected\n");
          set_code_page(tokenv);
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        case OP_ORDER:
          if(norders) ParseError("Extra (Order) block\n");
          parse_order_block();
          break;
        case OP_CONTROL:
          if(control_class) ParseError("Extra (Control) block\n");
          strcpy(tokenstr,"(Control)");
          control_class=look_class_name();
          if(!(classes[control_class]->cflags&CF_NOCLASS1)) ParseError("Conflicting definition of (Control) class\n");
          class_definition(control_class,vst);
          break;
        case OP_CONNECTION:
          nxttok();
          if(!(tokent&TF_NAME) || tokenv!=OP_STRING) ParseError("String literal expected\n");
          for(i=0;tokenstr[i];i++) switch(tokenstr[i]) {
            case 't': conn_option|=0x02; break;
            case 'w': conn_option|=0x01; break;
            default: ParseError("Unrecognized (Connection) option\n");
          }
          nxttok();
          if(tokent!=TF_CLOSE) ParseError("Expected close parenthesis\n");
          break;
        case OP_LEVELTABLE:
          level_table_definition();
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
  if(control_class) {
    if(classes[control_class]->nimages) fatal("Images are not allowed in Control class");
    if(classes[control_class]->cflags&CF_GROUP) fatal("Control class is not allowed to be Abstract");
  }
  if(macros) for(i=0;i<MAX_MACRO;i++) if(macros[i]) free_macro(macros[i]);
  free(macros);
  if(array_size) {
    array_data=malloc(array_size*sizeof(Value));
    if(!array_data) fatal("Array allocation failed\n");
  }
  if(norders) set_class_orders();
  fprintf(stderr,"Done\n");
}
