# Nocturne の Brotli

Brotli 1.2.1（`c/common/version.h`、`CHANGELOG.md` の 1.2.1 の SECURITY 修正を含む）。
ライセンス: 同梱 `LICENSE`（MIT）。

Makefile は `c/common`・`c/dec`・`c/enc` をコンパイルする（`BROTLI_C`・`BROTLI_ENC_C`）。
CLI、各言語 binding、OS 固有の補助処理はリンクしない。
使う所は三つ：FreeType の WOFF2 の解読、HTTP の `br` の内容の符号
（`user/libc/http_coding.h`）、Compression Standard の `"brotli"`
（`user/libc/web/js_compression.c`）。FreeType の外からは
`user/libc/third_party_brotli.c`（宣言は `user/libc/brotli_codec.h`）だけが
brotli の見出しを読む。

上流からの局所修正: `c/common/platform.h` の `#include <sys/types.h>` は
ホスト環境（`__STDC_HOSTED__` が真）でだけ読む。Nocturne は freestanding で
`sys/types.h` を持たず、バイト順はコンパイラーのマクロから決まる。
上流を更新するときは、この一か所を新しい版へ当て直す。
