# Nocturne の WOFF2 用 Brotli

Brotli v1.2.0（上流タグ `v1.2.0`）。
取得元: https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
ライセンス: 同梱 `LICENSE`（MIT）。

Makefile は `c/common` と `c/dec` のみをコンパイルする。
encoder、CLI、各言語 binding、OS 固有の補助処理はリンクしない。
フリースタンディング C の portable decoder として FreeType が呼び出す。
