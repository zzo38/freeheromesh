#if 0
gcc ${CFLAGS:--s -O2} -c edit.c `sdl-config --cflags`
exit
#endif

#include "SDL.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sqlite3.h"
#include "smallxrm.h"
#include "heromesh.h"

void run_editor(void) {
  fatal("[Editor not implemented yet]\n"); // so far just a stub
}
