# DOM 担当・現在の製品修理

対象は指定4サイトの共通DOM／資源／幾何境界。既存性能実装を保持し、製品には
サイト名、作者DOM構造を狙った例外、容量上限、可視状態の偽装を追加していない。
実サイトと全体ビルド／VMは主担当の責務であり、以下の限定結果から受入成功へ
昇格しない。

## 製品変更
- `user/libc/web/js.c` の module map／native pending へ `modulepreload` を統合。
  linkの接続・rel/href変更・文書世代を扱い、同URLのlink/script/importを共有する。
  失敗したnetwork/MIME/SRI取得は恒久失敗としてcacheせず、最後の利用者で実解放する。
- `dom.c`／`js_nodes.h` の既存変更通知で索引を無効化する。通常のstyle書換えごとに
  全DOMのpreload探索を増やさない。旧文書の結果を新文書へ渡さない。
- root構文解析だけのpreloadを、QuickJSの最小局所flagで依存解決・作者評価から分離。
  通常evalの意味は変更せず、実利用時は既存 `JS_ResolveModule` で解決する。
  取得中の必要依存へのjoinは同author stackのnative pendingだけを借り、
  wait中に作者JS／`process_pending`／文書破棄へ再入しない。
- `js_fetch.js` の実SRI digest処理を共用し、relListの標準tokenと完了eventへ接続。
  module URLは要求URL、相対importのbaseは最終redirect URLとして保全する。
- IOのtarget矩形を既存CSSOM View fragment collectorへ接続。inlineのanchor／decoを
  doubleでunionし、explicit rootは普通DOM祖先ではなく実CSS containing-block鎖で判定。
  slotted・hidden・viewport-fixed・zero-areaの条件を保全する。
- `offsetLeft/Top` は同collectorの非transform／非scroll first border fragmentから
  offsetParentのfirst padding edgeを差し引く。`offsetWidth/Height` はprincipal fragment
  union。従来のlayout-only inline wrapper 0を使わず、整数IDLを保全する。
- `js_observers.js` の IOEntry V1 7属性を、prototype上の実brand付きgetterとprivate
  snapshotへ統合。V1 EntryInit constructorは必須属性、finite double、Element型、
  DOMRectInit copyを扱う。未実装のV2 visibility/occlusionを存在だけで広告しない。

## 確認済みの範囲
- 正規 `js_embed.py` 再生成、clang target/include構文検査、CRLF対応diff checkは成功。
- `build/browser-compat-repaired-guest-20261013/test-serial.log`：原modulepreload-SRI
  WPT 1ケース成功、独立native IO inline/slotted等の7外側検査成功。
- `build/browser-compat-flex-guest-20261013/test-serial.log`：修理したquery配送fixtureの
  `modulepreload-api` 6検査成功、原slotted-target WPT 1ケース成功。
- native modulepreloadのroot-only／依存join条件群は同じ前段ゲストで成功した。
  未変更の成功群を再実行する依頼はしていない。
- `build/browser-compat-integrated-guest-20261013/test-serial.log`：新IOEntryの
  10作者assertをnative実行し、外側7検査・失敗0。constructor／brand／snapshotは
  この実結果で確認した。原inlineは3ケース中2失敗を残す。

## 凍結と未確認
製品・生成物は凍結済み。offset修理後の原inline WPTは字体依存の小数差を残し、
原assert／IO精度を弱めて通していない。JSON/CSS/text型module、worker
destination、IO V2、CSP/referrer policyの全面拡張は今回の限定対応の保証外。
Reddit左nav／検索二重文字の実受入、全4サイトの表示・入力・上下scroll・性能比較は
未達／未確認を残しており、限定成功を全面成功へ置き換えない。
