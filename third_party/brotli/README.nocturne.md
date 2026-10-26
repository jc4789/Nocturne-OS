# Nocturne の WOFF2 用 Brotli

Brotli 1.2.1（`c/common/version.h`、`CHANGELOG.md` の 1.2.1 の SECURITY 修正を含む）。
ライセンス: 同梱 `LICENSE`（MIT）。

Makefile は `c/common` と `c/dec` のみをコンパイルする。
encoder、CLI、各言語 binding、OS 固有の補助処理はリンクしない。
フリースタンディング C の portable decoder として FreeType が呼び出す。

上流からの局所修正: `c/common/platform.h` の `#include <sys/types.h>` は
ホスト環境（`__STDC_HOSTED__` が真）でだけ読む。Nocturne は freestanding で
`sys/types.h` を持たず、バイト順はコンパイラーのマクロから決まる。
上流を更新するときは、この一か所を新しい版へ当て直す。
