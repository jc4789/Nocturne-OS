# CSS影・別境界の第二考察

第一報の「宣言が解析される」を再試験するのではなく、別artifact `tests/boxshadow_native_cases.c` で実raster/cache境界を検討した。

## 別反例

20件のhost確認は alpha spread輪郭、透明内側knockout、角丸、geometry cache再使用、tintだけ更新、inset穴、複数影順、Gaussian半平面のalpha/遠方falloff、100万px高boxのviewport cropと新scrollslice、実malloc失敗、取消時旧cache保全、明示解放を扱う。host dispatcherは直列でありSMP・製品DOM・実画面の証明ではない。

初回20件/2失敗を隠していない。曲線edgeに「完全緑」を期待していたが、実coverage253の抗aliasのため `0x02ff02` だった。期待は幾何の計算結果に訂正し、色だけ更新時同じmaskである要求は維持した。もう1件は完全offscreenの空cropが構築rowのcheckpointを通らず取消が伝播しなかった。製品cache list列挙/構築の64層ごとのcheckpointを追加し、空maskでも取消で新cacheをpublishしないよう修理した。後続の関連修理（座標cast前clamp、合成中追加checkpoint除去）後の20件は0失敗。

maskの重い構築はcanvas書込前に完結する。普通の影ページに新しい合成row checkpointを足すと、既存alpha group以外にはviewport rollback bufferがないため半描画を増やす。この新checkpointは撤去し、構築のyieldと既存walk境界を維持した。全viewportを毎回malloc/copyする新経路は入れない。

## 実製品で見つかった回帰と最小修理

rootの修理前新媒体:

- `build/nocturne-platform/compat-table-github-20261013/serial.log:83`: cpu3、rip `0x4131fa`、不正アドレス `0xffefd658` のpage fault。
- `build/nocturne-platform/compat-entry-reddit-20261013/serial.log:83`: cpu1、rip `0x4131df` のgeneral protection fault。

rootの逆引きはheap free/coalesce。実sourceを追うと `layout.c:add_deco` は `struct deco d;` に旧fieldを個別代入し、新しいshadow_cacheだけ未初期化だった。sbufがstack garbageを保存し、shadowなしfragmentのpreflightでもcachefreeがそれをfreeしていた。私の新增所有fieldが製品へ接続したときの初期化漏れであり、CSS/JS解析成功や以前のPASSから退役安全を推測したことが誤りだった。

修理は `struct deco d={0};` とした。shadow cache機能を撤去せず、clampでndecosを縮める2経路でもdiscarded tail masksを縮小前に実freeする。layout arena退役後はdecos=NULL/ndecos=0としてframeset早returnの旧pointer再読を防ぐ。`box.c:nbox` は `ar_alloc` が全確保byteをmemsetゼロする別経路で、未初期化仮説を反証した。list nodeも designated initializerからnext=NULLになる。

新native fixtureは普通inlineの複数fragmentを初paint→repeatlayout→影追加→幅変更reflow→影除去→文書廃棄へ通し、NULL所有者/独立mask所有者を実箱から確認する。既存blockだけのfixtureではこの生成境界を通らず漏れた。修理後構文/diff checkは成功、native/同2サイト再起動はroot担当でこの第二報時点では未確認。元crashログを保全する。

## 所有と残件

computed listは自身のarenaから確保し、cacheはstyle arena pointerを保存せず値をcopyする。非継承の通常子は初期noneへ戻す。explicit inheritは計算済み親listを借りる既存property所有契約を維持し、親currentColorを子の色へ書き換えない（親finishは親cascade時点で解決済み）。親変更時子を再cascadeする契約を破る最適化は別担当と禁止合意済み。

第一報のsource表と同じ `b68460fc...` 修理後sourceを束縛材料とする。paint/js/layout等が以後変われば報告の結果をその新sourceへ自動流用しない。実site視覚、上下scroll、DDG細部、Reddit検索重複、Hyper-V、同条件性能比較は未完了。回帰解消の実結果が得られるまで完了扱いにしない。
