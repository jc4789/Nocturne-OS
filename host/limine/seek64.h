/* Put before Limine's installer, tools/limine/limine.c, when the Makefile builds it: the installer
   moves through the disk with fseek and 32-bit positions, which Windows' C library refuses past
   2 GiB, and the boot disk is larger. _fseeki64 takes 64-bit ones. */
#include <stdio.h>
#define fseek _fseeki64
