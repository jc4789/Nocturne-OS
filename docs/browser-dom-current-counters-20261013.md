# DOM 担当・独立した反例と残る境界

第一報 `docs/browser-dom-current-implementation-20261013.md` の弱い前提を、
以下の別証拠で確認した。過去の第一・第二報も変更せず保全している。

## 依存解決と負例の取り違え
QuickJSの元COMPILE_ONLYはroot解析の後に依存解決する。原ソースのこの反例から
parse-only局所flagを追加し、利用時に既存resolverへ戻す修理へ訂正した。
root-onlyと必要依存joinのnative条件群で、この分離を確認した。

第二ゲストのmodulepreload 17検査中2失敗は、SRI用query URLをfixtureが404に
していたものだった。負例errorをSRI成功証拠にせず、exact query assetを追加した。
元失敗と理由は `docs/browser-modulepreload-fixture-query-20261013.md` に保全。
修理後の失敗条件群だけをnative再確認し、6検査成功した。

## slot／placeholder／prototypeの別境界
Redditのslot割当は成立している。SLOTがdisplay:contents・0寸法だからDOMが欠損した、
という仮説は棄却した。native原作者 `faceplate-partial` の公開クラスsourceは、lazy時に
子をDocumentFragmentへ退避し、1px placeholderを置くことを示す。1s高さ1→30s高さ0は
この状態遷移と整合するが、開始しない原原因をそれだけで確定しない。

実作者のpolyfill loaderはIOEntry.prototype.intersectionRatioの存在を検査する。
従来のown値だけのEntryでは、この検査が失敗し、不要なpolyfill経路へ入り得る。
修理は実snapshot＋brand getterであり、存在だけのstubではない。
一方V2のisVisibleを公開すると未実装trackingを広告する反例があるため、
主担当の第二査読でV2公開を外し、V1の実属性へ限定した。

新Redditゲスト `compat-entry-reddit-20261013` はCSS到着直後にRIP 0x4131dfで
不正freeが疑われるGP faultに達した。NAV_BASEは未取得。この起動失敗は無視しない。
CSS担当がinline fragment cache未初期化を修理し、主担当が別recoveryゲストを起動。
原NAV_BASEを `compat-recovery-reddit-20261013/serial.log:309` で実読した。
`readyMark` の非空時はPromiseを待ち、その後lazy controller `Yt` を作る。
同サイト左欄はなお空で、controller型／原sourceの次の読取が必要。
observerには原superclass sourceを最初の定義済み1/30/90sに1回だけ読む補助を置いた。
作者DOM／style／関数を差し替えない。

## 原inline WPTの整数仮定を保全
原 `intersection-observer/inline-client-rect.html` は3ケース中2失敗。
修理前はexpected308／actual320.480010986、scroll後218／230.480010986だった。
offset getterのinline wrapper 0は製品不備として修理した。

原頁は整数offset getterから空白幅を算出し、fractional IO矩形へepsilon0で比較する。
bundled `NotoSerifLiving-Regular.ttf` のspace hmtxをfontToolsで読み、260/1000em、
16pxで4.16pxと確認した。デフォルトserif選択はcssprop.c／font_familyへ接続する。
修理後に字体依存の小数差が残り得るが、原assert／整数IDL／IO精度を弱めて通さない。
新native原結果は `browser-compat-integrated-guest-20261013/test-serial.log` で
expected323／actual320.480010986、scroll後233／230.480010986の2失敗を残した。
offset wrapper不備の修理と、原頁の字体依存を分離する。新IOEntryは独立nativeで
10作者assert／7外側検査・失敗0となり、未実装V2を広告せず実brandを確認した。

## 診断資料の扱い
旧NAV_MODULEがdata:コードを出していた診断不備をHTTP/S限定へ修理した。
今回所有logの該当3行はapply_patchで赤化し、元行番号、1372／2919／35381文字の
長さとSHA256だけを残した。値を成果物へ転載せず、ユーザー資料は変更しない。
公開concatの取得は異なるquery encodingでもhost404で、成功証拠にはしていない。

途中で主担当がモデル容量エラーに達したが、同モデルで復帰した。その間、
主担当のVM／allbuild所有を勝手に引き継がず、凍結ソースと残件を保全した。
次はlazy controllerの原sourceと検索caption原境界を、実画像の不具合へ接続する。
