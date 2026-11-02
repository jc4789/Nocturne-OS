#pragma once
#include <stddef.h>
#include <cdefs.h>
NOCTURNE_BEGIN_DECLS
/* ISO C <locale.h>. A program starts in "C". setlocale(category, "") selects the user's language and
   region (nocturne_language.h), by the name of its tag, for that category. "C.UTF-8" is "C". Multibyte
   characters are UTF-8 in every locale (MB_CUR_MAX is 4). Another language's name is not known: NULL.
   A selection takes the values of its moment. Asked with NULL, a mix of categories gives the names joined
   by ';', which setlocale(LC_ALL, ...) takes back. */
#define LC_ALL 0
#define LC_COLLATE 1
#define LC_CTYPE 2
#define LC_MONETARY 3
#define LC_NUMERIC 4
#define LC_TIME 5
struct lconv {
    char *decimal_point, *thousands_sep, *grouping;
    char *mon_decimal_point, *mon_thousands_sep, *mon_grouping;
    char *positive_sign, *negative_sign;
    char *currency_symbol, *int_curr_symbol;
    char frac_digits, p_cs_precedes, n_cs_precedes, p_sep_by_space, n_sep_by_space, p_sign_posn, n_sign_posn;
    char int_frac_digits, int_p_cs_precedes, int_n_cs_precedes, int_p_sep_by_space, int_n_sep_by_space,
        int_p_sign_posn, int_n_sign_posn;
};
char *setlocale(int category, const char *locale);
struct lconv *localeconv(void);
NOCTURNE_END_DECLS
