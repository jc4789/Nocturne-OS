# 丸画像と computed CSSOM の原 guest 失敗修理

原ログは `build/browser-compat-ellipsis-shape-layer-guest-20261013/test-serial.log`。shadow damage 6 件は成功したが、radius 13 件中 decoded image curved content edge が 1 失敗、WPT radius valid 23/invalid 11 は成功、computed 14 は全失敗。成功した項目で失敗を相殺せず、元ログと前の第一/第二報は不変保全した。

## 製品原因と修理

1. `user/libc/image.c` の `image_draw` は decoded bitmap を直接 `canvas->px` に描き、`mask_clip` を無視していた。共通 gfx と glyph/media/canvas consumer だけの修理ではこの consumer が抜けていた。mask 有りのときだけ既存 `gfx_blend_pixel` で描き、source alpha と chain alpha を結合する分岐を追加。mask 無しの元 direct-row fast path はそのまま保った。Reddit の四角 avatar と native SVG image fixture の同一境界を共通で修理した。期待値変更や画像 CSS 注入はない。
2. 原 WPT computed 14 の失敗は全て最初の `property in getComputedStyle(target)` が false。原 helper は正常取得・hash 一致して実行され、値比較へ未到達だった。製品 `getComputedStyle` の Proxy が get だけで、CSS の named attributes を公開していなかった。
3. `css_prop_name` を既存 native property table へ追加し、private `host.cssProperties` が実名前/longhand metadata を返す。既存 `StyleDeclaration` 責務へ `CSSStyleDeclaration` / `CSSStyleProperties` を統合。実 table の名前だけ prototype accessor を公開し、method と属性は同じ native computed/style に接続する。unknown property への無条件 true `has` や固定 computed 値は使わない。
4. computed object は同じ node の現在値を読み、実 brand を private WeakMap で保持する。readonly setter/method/cssText は `NoModificationAllowedError`、非 element と借用不正 receiver は TypeError。numeric item/indices/iterator と length は実 longhand metadata と custom declarations を列挙し、standard names は vendor-prefixed names より先、同群は lexicographic 順。
5. 列挙で custom name だけ公開して値取得が空になる偽対応を避け、現在 computed `style_t.vars` の実値を読む。override/guaranteed-invalid の最初の head を保持し、invalid な local value が古い inherited 同名を列挙しない。局所 linked declarations のみを扱い、全 DOM/全 stylesheets の追加走査はない。

仕様境界は [CSSOM declaration interface](https://drafts.csswg.org/cssom/#cssstyledeclaration) と [getComputedStyle](https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle)。既存 inline cssText の raw declaration/list grammar、computed pseudo-element 選択など本修理外の未完全部分まで完成と称さない。

## 再確認の対象

- host 新反例は common `gfx_blit_alpha` のみでなく exact production `image_draw` を test extraction へ接続。`build/css-compat-radius-image-host.log` は radius 18 件 / 0 失敗。これは native decoder/VM の代用品ではない。
- `boxshadowtest --radius` は元の pixel assert を不変に保ち、実 decoder/native request/paint/hit を再確認する。
- WPT 新 `--radius-computed` / group `wptradiuscomputed` は失敗した原 computed 14 件だけを実行。元 valid/invalid の成功 34 件は再実行しない。
- 新 authored `jstest computed-style-declaration` / group `computedstyle` は actual property presence、unsupported 反例、readonly 例外、live radius/custom、indices/order、brand、detach を 24 assert で確認。原 WPT を置換せず、旧 cssom 54 件も未変更反復しない。
- 新源の JS syntax、C syntax、diff check 成功。生成元 JS の変更は正規 `js_embed.py` を DOM 担当の単 writer で再生成した。初回『生成完了』と最後 tiny 更新が交差したため、inc の actual read で stale と判断し再生成を依頼。その後私自身も source/inc の vendor sort・custom list・CSSStyleDeclaration 継承の actual lines 一致を確認した。旧 inc を最終確認としない。

主担当の nav button flex 差分は保全。優先実 GitHub 原画の受入を WPT 完走待ちへ遅らせず、今回 snapshot の後に次 batch として修理を入れた。新 ABI/full build・guest computed 14/画像・新実サイト avatar の成功、性能比較はまだ未確認。
