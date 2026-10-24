#pragma once
/* ISO C <inttypes.h> for the types of Nocturne's <stdint.h> (LP64): the 8-,
   16- and 32-bit types are signed char, short and int, the 64-bit ones long;
   int_fast8_t is int8_t and the other fast types, intmax_t and intptr_t are
   long. printf's arguments of the narrower types are promoted to int, so they
   need no length modifier; scanf's need the exact one. */
#include <stdint.h>
#include <cdefs.h>

#define __NOCTURNE_PRI8 ""
#define __NOCTURNE_PRI16 ""
#define __NOCTURNE_PRI32 ""
#define __NOCTURNE_PRI64 "l"
#define __NOCTURNE_SCN8 "hh"
#define __NOCTURNE_SCN16 "h"
#define __NOCTURNE_SCN32 ""
#define __NOCTURNE_SCN64 "l"
#define __NOCTURNE_FAST8 __NOCTURNE_SCN8
#define __NOCTURNE_FAST "l"

#define PRId8 __NOCTURNE_PRI8 "d"
#define PRId16 __NOCTURNE_PRI16 "d"
#define PRId32 __NOCTURNE_PRI32 "d"
#define PRId64 __NOCTURNE_PRI64 "d"
#define PRIdLEAST8 PRId8
#define PRIdLEAST16 PRId16
#define PRIdLEAST32 PRId32
#define PRIdLEAST64 PRId64
#define PRIdFAST8 PRId8
#define PRIdFAST16 __NOCTURNE_FAST "d"
#define PRIdFAST32 __NOCTURNE_FAST "d"
#define PRIdFAST64 __NOCTURNE_FAST "d"
#define PRIdMAX "ld"
#define PRIdPTR "ld"

#define PRIi8 __NOCTURNE_PRI8 "i"
#define PRIi16 __NOCTURNE_PRI16 "i"
#define PRIi32 __NOCTURNE_PRI32 "i"
#define PRIi64 __NOCTURNE_PRI64 "i"
#define PRIiLEAST8 PRIi8
#define PRIiLEAST16 PRIi16
#define PRIiLEAST32 PRIi32
#define PRIiLEAST64 PRIi64
#define PRIiFAST8 PRIi8
#define PRIiFAST16 __NOCTURNE_FAST "i"
#define PRIiFAST32 __NOCTURNE_FAST "i"
#define PRIiFAST64 __NOCTURNE_FAST "i"
#define PRIiMAX "li"
#define PRIiPTR "li"

#define PRIo8 __NOCTURNE_PRI8 "o"
#define PRIo16 __NOCTURNE_PRI16 "o"
#define PRIo32 __NOCTURNE_PRI32 "o"
#define PRIo64 __NOCTURNE_PRI64 "o"
#define PRIoLEAST8 PRIo8
#define PRIoLEAST16 PRIo16
#define PRIoLEAST32 PRIo32
#define PRIoLEAST64 PRIo64
#define PRIoFAST8 PRIo8
#define PRIoFAST16 __NOCTURNE_FAST "o"
#define PRIoFAST32 __NOCTURNE_FAST "o"
#define PRIoFAST64 __NOCTURNE_FAST "o"
#define PRIoMAX "lo"
#define PRIoPTR "lo"

#define PRIu8 __NOCTURNE_PRI8 "u"
#define PRIu16 __NOCTURNE_PRI16 "u"
#define PRIu32 __NOCTURNE_PRI32 "u"
#define PRIu64 __NOCTURNE_PRI64 "u"
#define PRIuLEAST8 PRIu8
#define PRIuLEAST16 PRIu16
#define PRIuLEAST32 PRIu32
#define PRIuLEAST64 PRIu64
#define PRIuFAST8 PRIu8
#define PRIuFAST16 __NOCTURNE_FAST "u"
#define PRIuFAST32 __NOCTURNE_FAST "u"
#define PRIuFAST64 __NOCTURNE_FAST "u"
#define PRIuMAX "lu"
#define PRIuPTR "lu"

#define PRIx8 __NOCTURNE_PRI8 "x"
#define PRIx16 __NOCTURNE_PRI16 "x"
#define PRIx32 __NOCTURNE_PRI32 "x"
#define PRIx64 __NOCTURNE_PRI64 "x"
#define PRIxLEAST8 PRIx8
#define PRIxLEAST16 PRIx16
#define PRIxLEAST32 PRIx32
#define PRIxLEAST64 PRIx64
#define PRIxFAST8 PRIx8
#define PRIxFAST16 __NOCTURNE_FAST "x"
#define PRIxFAST32 __NOCTURNE_FAST "x"
#define PRIxFAST64 __NOCTURNE_FAST "x"
#define PRIxMAX "lx"
#define PRIxPTR "lx"

#define PRIX8 __NOCTURNE_PRI8 "X"
#define PRIX16 __NOCTURNE_PRI16 "X"
#define PRIX32 __NOCTURNE_PRI32 "X"
#define PRIX64 __NOCTURNE_PRI64 "X"
#define PRIXLEAST8 PRIX8
#define PRIXLEAST16 PRIX16
#define PRIXLEAST32 PRIX32
#define PRIXLEAST64 PRIX64
#define PRIXFAST8 PRIX8
#define PRIXFAST16 __NOCTURNE_FAST "X"
#define PRIXFAST32 __NOCTURNE_FAST "X"
#define PRIXFAST64 __NOCTURNE_FAST "X"
#define PRIXMAX "lX"
#define PRIXPTR "lX"

#define SCNd8 __NOCTURNE_SCN8 "d"
#define SCNd16 __NOCTURNE_SCN16 "d"
#define SCNd32 __NOCTURNE_SCN32 "d"
#define SCNd64 __NOCTURNE_SCN64 "d"
#define SCNdLEAST8 SCNd8
#define SCNdLEAST16 SCNd16
#define SCNdLEAST32 SCNd32
#define SCNdLEAST64 SCNd64
#define SCNdFAST8 __NOCTURNE_FAST8 "d"
#define SCNdFAST16 __NOCTURNE_FAST "d"
#define SCNdFAST32 __NOCTURNE_FAST "d"
#define SCNdFAST64 __NOCTURNE_FAST "d"
#define SCNdMAX "ld"
#define SCNdPTR "ld"

#define SCNi8 __NOCTURNE_SCN8 "i"
#define SCNi16 __NOCTURNE_SCN16 "i"
#define SCNi32 __NOCTURNE_SCN32 "i"
#define SCNi64 __NOCTURNE_SCN64 "i"
#define SCNiLEAST8 SCNi8
#define SCNiLEAST16 SCNi16
#define SCNiLEAST32 SCNi32
#define SCNiLEAST64 SCNi64
#define SCNiFAST8 __NOCTURNE_FAST8 "i"
#define SCNiFAST16 __NOCTURNE_FAST "i"
#define SCNiFAST32 __NOCTURNE_FAST "i"
#define SCNiFAST64 __NOCTURNE_FAST "i"
#define SCNiMAX "li"
#define SCNiPTR "li"

#define SCNo8 __NOCTURNE_SCN8 "o"
#define SCNo16 __NOCTURNE_SCN16 "o"
#define SCNo32 __NOCTURNE_SCN32 "o"
#define SCNo64 __NOCTURNE_SCN64 "o"
#define SCNoLEAST8 SCNo8
#define SCNoLEAST16 SCNo16
#define SCNoLEAST32 SCNo32
#define SCNoLEAST64 SCNo64
#define SCNoFAST8 __NOCTURNE_FAST8 "o"
#define SCNoFAST16 __NOCTURNE_FAST "o"
#define SCNoFAST32 __NOCTURNE_FAST "o"
#define SCNoFAST64 __NOCTURNE_FAST "o"
#define SCNoMAX "lo"
#define SCNoPTR "lo"

#define SCNu8 __NOCTURNE_SCN8 "u"
#define SCNu16 __NOCTURNE_SCN16 "u"
#define SCNu32 __NOCTURNE_SCN32 "u"
#define SCNu64 __NOCTURNE_SCN64 "u"
#define SCNuLEAST8 SCNu8
#define SCNuLEAST16 SCNu16
#define SCNuLEAST32 SCNu32
#define SCNuLEAST64 SCNu64
#define SCNuFAST8 __NOCTURNE_FAST8 "u"
#define SCNuFAST16 __NOCTURNE_FAST "u"
#define SCNuFAST32 __NOCTURNE_FAST "u"
#define SCNuFAST64 __NOCTURNE_FAST "u"
#define SCNuMAX "lu"
#define SCNuPTR "lu"

#define SCNx8 __NOCTURNE_SCN8 "x"
#define SCNx16 __NOCTURNE_SCN16 "x"
#define SCNx32 __NOCTURNE_SCN32 "x"
#define SCNx64 __NOCTURNE_SCN64 "x"
#define SCNxLEAST8 SCNx8
#define SCNxLEAST16 SCNx16
#define SCNxLEAST32 SCNx32
#define SCNxLEAST64 SCNx64
#define SCNxFAST8 __NOCTURNE_FAST8 "x"
#define SCNxFAST16 __NOCTURNE_FAST "x"
#define SCNxFAST32 __NOCTURNE_FAST "x"
#define SCNxFAST64 __NOCTURNE_FAST "x"
#define SCNxMAX "lx"
#define SCNxPTR "lx"

typedef struct {
    intmax_t quot, rem;
} imaxdiv_t;

/* wchar_t without the rest of <stddef.h>. */
#ifdef __cplusplus
typedef wchar_t __nocturne_wchar_t;
#else
typedef __WCHAR_TYPE__ __nocturne_wchar_t;
#endif

NOCTURNE_BEGIN_DECLS
intmax_t imaxabs(intmax_t j);
imaxdiv_t imaxdiv(intmax_t numer, intmax_t denom);
intmax_t strtoimax(const char *s, char **end, int base);
uintmax_t strtoumax(const char *s, char **end, int base);
intmax_t wcstoimax(const __nocturne_wchar_t *s, __nocturne_wchar_t **end, int base);
uintmax_t wcstoumax(const __nocturne_wchar_t *s, __nocturne_wchar_t **end, int base);
NOCTURNE_END_DECLS
