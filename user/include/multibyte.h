#pragma once
/* The types <uchar.h>, <wchar.h> and <wctype.h> share. The locale's multibyte
   characters are UTF-8 (<locale.h>) and a wide character is one UTF-32 code
   unit: wchar_t and wint_t are 32-bit int for both of Nocturne's compilers. */
#include <stdint.h>

/* A conversion's state between calls; a zeroed one is the initial state. */
typedef struct {
    uint32_t __value;              /* the character decoded so far, or being given out */
    unsigned char __need;          /* bytes still to come for it */
    unsigned char __low, __high;   /* the range the next of them must be in */
    unsigned char __give;          /* its code units still to give */
} mbstate_t;

typedef __WINT_TYPE__ wint_t;
#define WEOF ((wint_t)-1)
