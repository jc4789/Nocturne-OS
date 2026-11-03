# 層・CSS資源：原WPT失敗からの追加修理

先行の第一報／第二考察は変更しない。本記録は `build/browser-compat-ellipsis-shape-layer-guest-20261013/test-serial.log` を直接読み、後続の製品修理と分離した追加報である。

## 実行された条件

同ログの新native群はchild-list旧失敗だけ1件、層14件、import5件、named font/keyframes5件が成功。以前の未変更成功群を反復していない。CSSOMトップ階層動的操作11件は今回未実行であり、合格へ含めない。

原WPTは `D:/Programes/cloned repos/wpt/css/css-cascade/` の4原本と既存testharnessを使った。

- `layer-basic.html` 34件すべての最初の失敗は、期待 `rgb(0, 128, 0)` に対して実際 `rgba(0, 128, 0, 1)`。
- `layer-important.html` 9件も同じ不透明色の直列化失敗。
- `layer-media-query.html` 8件のうち6件は実色redの同じ直列化失敗。B1/B2は実色blackで、data CSS importがHTTP transportへ誤送出され `Invalid HTTP(S) URL` になった別の失敗。
- `layer-vs-inline-style.html` は `target` のWindow名前付き参照が見つからず、4作者条件まで進めない。DOM担当へ原因共有した。層の成功にも単なるインフラ成功にも数えない。

これらは最初のassertまでの結果である。直列化修理後の第二targetやmedia変更後の値まで正しいことは、旧失敗から推論できない。原ログを保全し、失敗原本だけ次の新製品媒体で再確認する。

## 後続製品修理と静的確認

`user/libc/web/js.c` のcomputed色、SVG paint色、animation underlying色を共通直列化helperへ接続。不透明色は [CSS Color4 §16.2.2](https://www.w3.org/TR/css-color-4/#serializing-sRGB-values) に従いrgb()を返す。既存の半透明色を不透明化していない。8-bit非不透明alphaの規定丸めは別の未修理として残る。

data/blob CSSは既存のローカル資源decoderとpending responseへ接続し、HTTP helperへ送らない。decoderのjs_malloc bufferからdoc CSS storageへコピーされ、pending_freeがjs_freeする。要求claim、文書世代、取消、OOM、redirect/import baseの責務を変えていない。CSS MIMEの適用可否は既存ネット経路にも不足があり、本修理の成功に混ぜない。

host構文確認とCRLF対応diff確認は成功。これら後続修理の新guest実行、実サイト表示・実GUI操作・同条件性能はまだ未確認。主担当が緊急nav修理の完全build／実画像を優先しており、source凍結を保つ。

## nav媒体確定後の追加修理（未実行）

主担当からnavの完全buildと実画像確認、source凍結解除の連絡を受けた。その後、8-bit非不透明alphaを [CSS Color4 §16.1.1](https://www.w3.org/TR/css-color-4/#serializing-alpha) の整数percentage round-tripと3桁丸めへ修理した。例128は0.5、237は0.93、236は0.925を返す。半透明を不透明化せず、0と1も区別する。

`tests/mediatest.c` の2箇所と `tests/js_semantic_elements_cases.js` の2箇所は、不透明色をrgba(...,1)とする旧仕様不適合期待を、同じRGB成分のrgb(...)期待へ訂正した。原失敗ログと原WPTは変更しない。新 `csscachetest --color-format` はlive DOMとgetComputedStyleを通して7alpha境界とSVG fill/strokeを確認する9件で、未実行。旧媒体の成功をこの後続sourceへ流用しない。
