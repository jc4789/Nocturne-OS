# フォント・CSS供給・長native境界：現ソースの第一引渡し

2026-10-13。担当 `font_load` の製品変更を凍結した。親の最新状態はモデル容量エラーであり、この担当が VM／server／全build を代わりに起動したことはない。ここは担当範囲の引渡しで、四実サイトの受入完了ではない。先行第一／第二報と原ログは変更せず保全する。

## 製品へ入れた修理

- `css.c` の font `src` 選択：圧縮を含む EOT 形式全体を支持していない native decoder に対する `embedded-opentype` hint の誤広告を除去。hint 無しの EOTLite／XOR と既存 FreeType／WOFF2 を維持。全 unsupported／invalid の後続 `src` 宣言で先行する有効宣言を消さない。
- `doc.c`／`js.c`／`webi.h` の CSS 供給：一件ずつの全体 gate を廃止し、DOM scan 順に全 pending 候補を送出する。文書寿命の cache record と `loading_id` により rescan 重複、同URL新旧要求、逆順完了、取消、redirect の相対 import base を保全。並列輸送の admission は既存 webnet に委ねる。
- `js.c` の診断：debug native 全呼出しの前後時計を outer 呼出しの約 1/64 sampling へ変更。call／sample 件数は正確、native 時間は64倍した推定値と明記。非 debug に sampling／追加時計はない。
- `dom.c`／`css.c` の child-list dirty：既に sibling 親を指す変更で、無関係な `+`／`~` の存在だけを理由に祖父まで再 style していた余計な昇格を縮小。親の全 descendant cascade は維持。親の `:empty`／状態を外側 sibling に使う候補を stable key index で保守的に判定し、`:has` の既存 ancestor／旧 witness 無効化を保つ。shadow／slot／foreign／fragment／interaction subtree は旧境界を保つ。

サイト名や特定 DOM で分岐する製品処理、偽 UA、author script 差替え、resource 上限追加、期限の最大値化は入れていない。

## 原資料・実行した限定確認

DDG 原 CSS と font は通常 HTTPS 取得した。`build/compat-ddg-font-20261013.eot` の flags `0x4` は圧縮容器で、native decode は拒否した。原 `build/compat-ddg-font-20261013.woff2` は同じ製品 decoder の host fixture で decode／字形／raster／advance 4件成功。raw EOT の1件失敗は非対応容器の証拠として保全した。元 fixture へ WOFF2 を raw TTF として渡した32件中1失敗も、期待値を弱めず原記録に分離した。正しい既存 TTF／WOFF／WOFF2／CFF 入力は34件成功。

- font source parser：`build/compat-font-sources-20261013.log`、26件／失敗0。
- CSS並列 native：`build/browser-compat-guest-20261013/test-serial.log`、12件／失敗0。
- 親 render mode：同原ログ11件／失敗0。他担当の検証範囲として区別する。
- 元 font sources native：同原ログ10件／失敗1。静的文書の未消費 dirty がある条件で「失敗candidateは計測を更新しない」と期待した前提不備。
- その境界だけの live quiet one-shot：`build/browser-compat-repaired-guest-20261013/test-serial.log` 87–89行。`quiet=1`、layout revision `1/1`、font generation `2/2`、raw `csscache-font-noop: 1 checks, 0 failures`。wrapper が異なる prefix を期待して FAIL したことと製品結果を区別する。wrapper は親が訂正済み。元10件 mode の quiet 前提も正常 live parser／tick 経路で強めたが、訂正後10件は未再実行。

`css.c`／`dom.c`／`js.c` と新 child-list fixture の host syntax、CRLF-aware diff check は成功。原 WPT font-src format／list を読んだが、WPT原本の実行成功とは主張しない。

## 長いtickの根拠と未確認

`build/nocturne-platform/compat-after2-baidu-20261013/serial.log` には posted 21785ms／native 300015calls、別timerの layout 22063ms／87flushes、tick最大22528msがある。native call timing自身と mutation／寸法読出し反復を別境界として修理した。累積並列style wallをtask時間へ加算していない。

新 `/home/cssrelationaltest --child-list` は未実行。親の全build／最新媒体での実サイト、同条件 debug／非debug性能比較、実入力と上下scrollは未完了。構造変更時の `need_boxes` →全 `boxes_build` も残るため、この修理だけで2分ロード解消や回帰なしとは言えない。容量エラーから親が復帰した後、まず新 native 条件と実サイト結果を読み、失敗境界を修理する。
