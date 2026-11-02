/* 言語と地域（user/libc/language.c）。値はサービス settings の領域 N_DOMAIN_LANGUAGE が持つ。読むのは呼ぶ側の
   利用者の値で、見張りが今の値に保つ。サービスが無ければ既定（日本語、日本）。

   文の表はプログラムごとに持つ：`static const char *const words[][N_LANGUAGE_COUNT]` を n_language_now() で引く。 */
#pragma once
#include <nocturne.h>
#include <settings_abi.h>
#include <cdefs.h>
#include <time.h>
NOCTURNE_BEGIN_DECLS

struct n_locale {
    uint32_t language;   /* enum n_language */
    uint32_t region;     /* enum n_region */
    uint32_t date_order; /* N_DATE_YMD・N_DATE_MDY・N_DATE_DMY。地域に従うを解いた値 */
    bool clock_12;
    char point;          /* 小数点 */
    char group;          /* 桁の区切り */
    const char *tag;     /* BCP 47 の言語のタグ："ja"、"en-US"、"en-GB"、"en" */
};

/* 今の値。 */
void n_locale_now(struct n_locale *out);
/* 今の言語。enum n_language。 */
uint32_t n_language_now(void);
/* 言語の変更を聞く：呼ぶ度に、前の呼出しの後に変わったか。 */
bool n_language_changed(uint64_t *seen);
/* 既定の優先言語の一覧："ja" や "en-US,en"。長さを返す。 */
size_t n_locale_languages(char *out, size_t cap);
/* Intl の既定の言語："ja" か "en"。 */
const char *n_locale_intl(void);

/* 書式：地域と設定に従う文字列を out へ。書いたバイト数（終端を除く）を返す。 */
size_t n_locale_date(char *out, size_t cap, const struct tm *tm);
size_t n_locale_time(char *out, size_t cap, const struct tm *tm, bool seconds);
/* "11月2日（月）" や "Mon, Nov 2" のような短い日付。 */
size_t n_locale_day(char *out, size_t cap, const struct tm *tm);
size_t n_locale_number(char *out, size_t cap, long long value);
/* 今の言語の名。index が範囲の外なら ""。 */
const char *n_locale_weekday(int index, bool abbreviated);
const char *n_locale_month(int index, bool abbreviated);

NOCTURNE_END_DECLS
