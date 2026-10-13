# DOM・modulepreload 第二反例確認（2026-10-13）

第一記録 `docs/browser-compat-dom-first-20261013.md` は変更せず保全した。以下は別のソース読解と反例から得た訂正・追加修理である。guest・実サイトの未確認を合格へ置き換えない。

## 1. COMPILE_ONLY の反例
第一実装は COMPILE_ONLY を「root解析だけ」と考えていたが、実際の `third_party/quickjs/quickjs.c` は module の関数作成後に `js_resolve_module` を呼んでから返す。このままでは preload のload前に依存を同期取得し、A/B循環ではnative pendingへの再joinまで起こり得る。

- 既存QuickJSの `JS_EVAL_FLAG_MODULE_PARSE_ONLY` を追加した。MODULEとCOMPILE_ONLYの両方を指定するnative呼出しだけで依存解決を保留し、通常eval・通常compileの動作を変えない。
- preload rootはこのflagで解析して保持。利用時の `load_module` が `JS_ResolveModule` を呼ぶ。`linked`／`resolving` とGC可視の保持JSValueにより循環と解析済みmoduleを保全する。
- 独立native fixtureに「root preloadだけでloadが来て、依存取得・同期load・作者評価が始まらない」を追加した。必要childを先行取得中の別graphでは、同じpendingへnative-onlyjoinして追加HTTP取得をしないことも確認する。
- この訂正前の前提・構文成功を、訂正後の動作成功へ流用しない。

## 2. 失敗・取消・解放の反例
- 原WPT `preload/modulepreload-sri.html` は、SRI失敗を恒久的なmodule map失敗へcacheしてはいけない反例である。ネットワーク／MIME／SRI失敗はmap検索対象から除外し、後の取得を許可した。
- 失敗recordは現在のnative pendingとlinkの参照だけが保持し、最後の利用者で実メモリを解放する。失敗URLを永久に溜めない。
- 文書input stream置換時は旧pendingを取消し、link索引を解放。encoding restartはpending→module所有順で破棄する。root wait callbackは同じauthor stackのpendingを借り、作者JS・文書破棄・process_pendingへ再入しない。
- 原WPTの最初の実行は1case失敗、基盤失敗2。native runnerにmodule同期hostが無く、実import先をHTTP0拒否したため、製品modulepreloadの合否は未判定。rootがrunnerの実native transportを修理し、失敗原ページだけを再確認する。
- 最初の `compat/modulepreload` も `/bin/jstest` 不在のexit127で未実行。正規TinyCCの `/tests/jstest.c` 選択経路が必要。製品不具合として隠したり、成功と数えたりしない。

## 3. Redditのslot仮説を棄却
- rootの実GUI console経由read-only観測により、host defaultslotがlight DIV `#flex-left-nav-container` へ正しく割り当てられることを確認した。
- `build/nocturne-platform/compat-after-reddit-20261013/serial.log` の `NAV_INNER_BOX` は、左欄のlight subtreeが存在し、`#flex-left-nav-contents`／FACEPLATE-PARTIALがdisplay:block・幅271・高さ0・text空であることを示す。
- SLOTのdisplay:contents・0寸法だけでDOM欠損とした仮説は棄却。部分資源とlazy-loadingの作者lifecycleを次の境界とし、定義状態・srcのqueryを除いた形・childrenの型だけを追加read-only観測する。サイトDOMを差し替えない。

## 4. IntersectionObserverの実装不備と修理
[Intersection Observer](https://w3c.github.io/IntersectionObserver/#update-intersection-observations-algo) は targetのboundingClientRectをCSSOM Viewで取得し、explicit rootをCSS containing-block chainで扱う。

現ソースは、inline要素について実deco fragmentを無視してlayout-only wrapperの座標・寸法を使い、explicitrootをordinary DOM parent鎖で判定していた。これはinline lazy targetとslotted targetの正しい交差を失う共通の境界不備である。Reddit固有原因と断定はしない。

- `observer_geometry` と DOM_rect は既存 `collect_client_rects` の同じdouble精度bounding unionを共用する。
- 現実のCSS containing-block鎖でexplicit rootを判定し、slotted shadow contentを扱う。普通DOMの祖先だけではviewport-fixed要素を含めない。
- 実fragmentが無いdisplay:none／display:contentsを可視と偽装しない。ResizeObserverのinline content/border size 0、clip chain、zero-areaのedge-inclusive交差とratio規則は保持する。
- `tests/js_observer_cases.js` の独立 `runIntersectionInlineCases` と `jstest intersection-inline` に10作者assertを追加した。inlineの非同期・実位置・CSSOM一致・root内交差・hidden、slottedの論理親／割当親・root交差、fixed除外、zero-area ratio1を扱う。
- 原WPTの関連候補は `intersection-observer/inline-client-rect.html` と `slotted-target.html` の2頁2case。原HTML／assertion／scriptは変更しない。

## 現時点の確認と残件
- 正規 `js_embed.py` とCRLF対応 `git diff --check` はexit0。
- IO追加後のjs.c／jstest.c target/includeを正規Makefileへ合わせたclang構文確認はexit0。既存IndexedDB括弧warningのみ。
- rootが既存QuickJS／js.oを正規媒体で更新し、module第一群を実guest実行中。IO追加はその第一結果へ流用せず、修理後の別媒体・独立native群で確認する。
- JavaScript destination以外のJSON/CSS/text型moduleやscript-like worker destinationは今回未実装。modulepreload全適合とはしない。referrer policyやCSP全体の機能拡張をこの限定実装で保証しない。
- 実サイトの速度・表示・操作・無回帰はrootの最新画像／操作／同条件比較を待つ。第一の構文確認、他担当のCSS WPT47成功、取得資源数は、その代わりにならない。
