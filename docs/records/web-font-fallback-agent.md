# Webフォント欠字補完の担当記録

- 原因：`font_open_memory()`で検証したWeb faceは欠字補完が無効で、`named_font`が選ばれた本文では同梱CJKフォントを調べず豆腐を描いていた。
- 修正：`font.c`のWeb faceだけ既存Sans補完群を有効にした。`font_has`・advance・width・drawが共有する`glyph_face`で実フォントを選ぶため、測定と描画が同じ字形を使う。Web faceの元bytesと主face metrics、単体`font_open()`、既存cacheの所有権・解放経路は維持。
- 仕様：字体選択の関連仕様は[CSS Fonts 4のfont matching](https://www.w3.org/TR/css-fonts-4/#font-matching-algorithm)と[文字の扱い](https://www.w3.org/TR/css-fonts-4/#character-handling)。PUAの私用文字ではWeb faceからgeneric/installed補完を行わない。
- 回帰確認：既存`tests/fonttest.c`へ、実Inter bytesのWeb face、Latin保持、BMP/補助面CJK、実同梱CFFの幅・ラスタ一致、混在測定/描画一致、不在文字、PUA抑止、Web face解放後の同梱face健全性を追加。既存単体faceの欠字確認は変更していない。
- 未検証：この担当はOS試験を起動していない。主担当が最新媒体で既存`web/fonts`と実サイトの目視・操作をまとめて確認する。CSSの全family stack順序とcluster shapingを完全実装した変更ではない。読込失敗時のlayout invalidationは主担当の別修正。
