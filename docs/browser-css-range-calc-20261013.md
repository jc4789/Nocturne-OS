# CSS range calc再考察

旧第一報・第二報は書き換えず、本記録を別artifactとして保存する。

## 仮説と反証

主担当の取得した `build/compat-github-primer-react.css` は `@media (width<=calc(48rem - .02px))` の内側でPageLayout contentの `flex-direction:column` を指定している。

基準 `mq_value` はliteral number/unitしか評価しないため、calc値は `MQ_UNKNOWN` になり、全queryがfalseである。したがってこの不足は767pxでcolumnにならない原因であり、1908pxで誤ってcolumnになる原因ではない。通常のcontentは `flex-wrap:wrap;flex:100%;max-width:100%;display:flex` なので、大viewport sidebar下落ちは別のflex item sizing/wrapping境界を調べる必要がある。仮説を確定原因へ昇格させていない。

## 製品修理

- `cssprop.c` の既存calc/min/max/clamp grammar、次元検査、foldを共用する `css_query_length` を追加。media lengthはproperty固有rangeclampやpercentage basisを持たないので、typedlengthかつpercentageを含まず有限値にfoldできることを確認する。
- `css.c:mq_value` はlength functionだけ新APIへ渡し、既存literal fastpathは維持。mediaのem/remは初期16pxで、作者`:root`のfont-sizeを誤用しない。container queryは既存em/remとviewport contextを渡す。
- calc-sumの二項 `+/-` に必要な両側空白を既存共用parserで検査。productが先に空白を消費してその判定を壊さないよう修理した。`*`/`/` は空白不要のまま。
- invalid/unknownのKleene論理を維持。無効calcを単にfalse扱いして `not` でtrueへ反転する変更はしていない。新たな容量／node／時間上限は追加しない。
- [CSS Values and Units Level 4](https://www.w3.org/TR/css-values-4/#calc-syntax) と [CSS Conditional Rules Level 3](https://www.w3.org/TR/css-conditional-3/) を確認した。全math functions対応とは主張しない。

## 限定確認

`python -X utf8 tests/check_cssperformance.py --media-calc`: **24 checks / 0 failures**。実production MQ関数をそのまま抽出し、実cssprop.cをコンパイルしたhostの算術・条件評価だけの確認である。

含む反例：原条件1908=false / 767=true / 768=false、min/max prefix、reverse/chainedrange、min/max/clamp、viewportunit、negative、mixednumber、percentage（0%も含む）、divide zero、nonfinite、binary空白、末尾garbage、unknown OR true、既存property calcの正常／無効spacing。

`tests/cssrelationaltest.c --media-calc` は同24入力に加え、実native document/cascadeで原条件1908=display:flex row、767=column、1908へresize=再rowを確認する独立mode。入力は `tests/cssmediacalc_cases.h` に一つだけ置き、host/nativeで共用する。まだguest実行待ちで、host結果をnative/style/実サイト成功へ流用しない。

最終userland ELF syntaxは css.c/cssprop.c/cssrelationaltest.cともexit0。CRLF-aware diffcheckも成功。関連WPT原本は `css/mediaqueries/mq-calc-001.html` と `mq-calc-003.html` を閲覧したが実行はまだである。既存と同条件の実サイト受入・速度比較は主担当の確認待ち。
