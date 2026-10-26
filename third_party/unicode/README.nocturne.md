# Unicode 拡張書記素プロパティ

ブラウザーの書記素切断は Unicode 18.0.0、UAX #29 revision 49 の
**extended grapheme cluster** 規則を使う。QuickJS の Unicode 17.0.0
正規表現／文字正規化テーブルは変更しない。両者の版を同一と称しない。

公式公開原データ（2026-10-13 取得）:

- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakProperty.txt
- https://www.unicode.org/Public/18.0.0/ucd/DerivedCoreProperties.txt
- https://www.unicode.org/Public/18.0.0/ucd/emoji/emoji-data.txt
- https://www.unicode.org/Public/18.0.0/ucd/auxiliary/GraphemeBreakTest.txt
- https://www.unicode.org/Public/18.0.0/ucd/UnicodeData.txt（2026-10-23 取得、libc の
  `wctype.h` の分類と大文字小文字変換）
- https://www.unicode.org/Public/18.0.0/ucd/PropList.txt（2026-10-23 取得、同じく
  White_Space）
- https://www.unicode.org/license.txt
- 規則 https://www.unicode.org/reports/tr29/tr29-49.html

原データは `18.0.0/` に保全、ライセンスは `LICENSE`（Unicode License V3）。
`python -X utf8 scripts/unicode_grapheme.py` が必要な GCB・InCB・
Extended_Pictographic のみを `user/libc/web/unicode_grapheme_data.h` へ
生成する。生成物には各原データの SHA256 が入り、`--check` で原データとの
一致を確認できる。Hangul LV/LVT の音節は UAX #29 の算式で分類する。
`python -X utf8 scripts/unicode_wctype.py` は UnicodeData・PropList・DerivedCoreProperties から
libc の `wctype.h` の分類と単純大文字小文字変換を `user/libc/wctype_data.h` へ同じ流儀で生成する。
`python -X utf8 scripts/unicode_idna.py` は UnicodeData の Bidi_Class と General_Category=Mark を
Lexbor の IDNA（UTS #46 の CheckBidi と「ラベルを結合記号で始めない」）用に
`third_party/lexbor/source/lexbor/unicode/idna_validity_res.h` へ生成する。
三つの生成器は原データの読み方・二段の表・`--check` を `scripts/ucd.py` で共有する。
ZWNJ の文脈（RFC 5892 付録 A.1 の Joining_Type）に要る ArabicShaping.txt はまだ無い。

同版の公式 `GraphemeBreakTest.txt` はブラウザーの実 C helper に対して
`python -X utf8 tests/check_grapheme.py` で検査する。ホスト分離検査であり、
実サイトの ellipsis 描画・操作・性能の受入を代用しない。
