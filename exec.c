#if 0
gcc ${CFLAGS:--s -O2} -c exec.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"

Uint32 generation_number;
Object*objects;
Uint32 nobjects;
Value globals[0x800];

static jmp_buf my_env;
static int my_error;

int execute_turn(int key) {
  if(setjmp(my_env)) return my_error;
  
  return 0;
}
