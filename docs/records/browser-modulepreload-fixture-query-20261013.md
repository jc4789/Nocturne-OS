# modulepreload 回帰検査の資源配送不備

第二ゲストの `build/browser-compat-repaired-guest-20261013/test-serial.log` で、
原 WPT `preload/modulepreload-sri.html` は1ケース成功した。一方、
ネイティブ `jstest modulepreload` は17検査中2失敗した。
元の失敗は `preload-unexpected-error` と `preload-api-no-assertion-failures`。

原因は検査の資源配送境界にあった。`tests/jstest.c` の `lookup()` は query を
含むパスの完全一致で配送するが、検査で指定した `/dir/preload.mjs?sri` は
`assets[]` に存在しなかった。負例 preload の error は SRI 判定ではなく404、
続く同URLの module script も404で失敗していた。したがって、この元の error を
SRI の成功証拠として扱わない。

修理は exact query URL に module 本文を登録すること。製品の SRI 処理、
期待値、要求数3、失敗後の再取得／評価回数2という検査条件は変更していない。
失敗した条件群のみを再確認できる `modulepreload-api` selector を既存ファイル内で
分離した。既存 `modulepreload` selector は全3条件群を維持する。

修理後のホストclang構文検査と差分空白検査は成功。最新ゲストでの API 条件群の
再確認は主担当へ委ねており、この記録の時点では未確認。すでに成功した原WPT、
root-only構文解析、依存join、IO幾何を未変更のまま再検査する依頼はしていない。
