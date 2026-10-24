#pragma once
/* ISO C <wctype.h>: Unicode's classification of wide characters (UTF-32,
   <multibyte.h>), in the compatibility properties of Unicode Technical
   Standard #18 that agree with ISO C's rules:
     alpha  Alphabetic, and the decimal digits other than 0-9
     upper  Uppercase           lower  Lowercase
     digit  0-9                 xdigit 0-9, A-F, a-f
     space  White_Space         blank  White_Space that does not end a line
     cntrl  general category Cc
     graph  everything but White_Space, Cc, Cs and unassigned code points
     print  graph and blank but not cntrl
     punct  graph but not alnum
   towupper and towlower are Unicode's simple case mappings. The tables come
   from the Unicode Character Database (third_party/unicode). */
#include <multibyte.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS

typedef int wctype_t;   /* 0 for none, else the class's number */
typedef int wctrans_t;  /* 0 for none, 1 tolower, 2 toupper */

int iswalnum(wint_t c);
int iswalpha(wint_t c);
int iswblank(wint_t c);
int iswcntrl(wint_t c);
int iswdigit(wint_t c);
int iswgraph(wint_t c);
int iswlower(wint_t c);
int iswprint(wint_t c);
int iswpunct(wint_t c);
int iswspace(wint_t c);
int iswupper(wint_t c);
int iswxdigit(wint_t c);
int iswctype(wint_t c, wctype_t type);
wctype_t wctype(const char *name);
wint_t towlower(wint_t c);
wint_t towupper(wint_t c);
wint_t towctrans(wint_t c, wctrans_t trans);
wctrans_t wctrans(const char *name);

NOCTURNE_END_DECLS
