# 書記素境界：別境界からの考察

## 反例を検討

1. **単runの冒頭へ毎回sotを置くと壊れる。**
   結合記号、emoji ZWJ、Indic linker、RI奇偶はrunを跨ぐ。
   `wf_grapheme_break` は直前状態を持ち越す。原853行の各コード点を
   別runとみなした検査は公式境界と一致。next の局所sotは
   「既知の書記素境界からのnext」だけの契約で、任意byte offsetには使わない。
2. **ASCIIの直後に非ASCII結合記号が来る。**
   next のASCII高速返却は次byteもASCII／制御文字／実末尾だけに限定。
   `a + combining mark` を1byteで切らず、CRLFの後に結合記号が続いても
   LF制御境界を保つ。原検査にこの規則の反例が含まれる。
3. **書記素と字形は同じではない。**
   `fi` の ligature は複数書記素になり得る。helperは UAX29 境界だけを返し、
   実幅測定／shaping／paintの代用をせず、幅を丸めたり固定幅にしない。
4. **版を混ぜるとIndicの境界が変わる。**
   current18規則を同版18原UCDと検査へ固定。既存QuickJS17の
   Grapheme_Extendだけから GCB／InCB を推測せず、QuickJS全体も更新しない。
5. **非NUL runの末尾で UTF8 decoder が読む。**
   必要byte数と n を先に比較し、許可範囲の直後を PAGE_NOACCESS にした
   host検査で truncated／invalid を実Cへ渡した。オーバーリードなし。

## 残る受入

paint側が論理text順に状態を保持し、atomic inline／block等の分断を適切に
区切るかは root の統合査読が必要。host公式検査合格は run位置・Bidi・
ellipsis幅・全glyph描画の実製品受入に昇格させない。変更前後の
同条件性能測定も未実施。主担当自身の実画面と操作確認を残す。
