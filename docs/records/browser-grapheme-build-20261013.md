# 書記素生成物のビルド依存

主担当のwriter許可後、Makefile の既存埋込み生成rule隣に
3原UCD／`scripts/unicode_grapheme.py` → 生成表 → layout.o の
明示依存を追加。guestのlinkは保全された表を使い Python は要求しない。
`make -n user/libc/web/unicode_grapheme_data.h` は up to date、
CRLF許容 `git diff --check` は exit 0。全ビルド・guest受入は主担当が実行する。
先の第一／第二考察文書は保全、Makefile追加後のsource再bindが必要。
