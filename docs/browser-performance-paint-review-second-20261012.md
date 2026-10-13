# paint／hit 修理後の独立再査読

第一査読の二つの境界を主担当が修理した後の静的再査読。製品ソースは read-only、VM は起動せず、旧 paintperformance 14 検査は繰り返していない。

査読 snapshot：`paint.c = 2d0a18f7bf87527fee90a552c65975d0c734a9b6`、`box.c = f1008fc2fd173eddeea09e2805840ba6eeba15ca`。

## 第一査読の反例への処置

- index-only outside marker primitive の追加を summary／既定 details legend に限定している。通常 li で高速側だけ新しい target を作る分岐はなくなり、第一査読の普通 li 静的反例はソース上で解消した。
- native qsort は typed stable merge に置換され、layer collection／merge と BVH bounds／partition／merge には native checkpoint が接続された。確保失敗・取消は false／SIZE_MAX で上へ伝播し、未完成 index は valid として公開されない。
- `prepare_paint_order` は root／stacking context／top-layer に必要な layer cache を先に構築する。`web_paint` の canvas clip／fill と媒体 snapshot 開始より前に実行するので、sorting cancellation は既存 window pixel を書き換える前に戻る。
- 文書の同一 layout／DOM revision では準備全木走査を skip する。`boxes_discard` は文書 order valid も外し、旧 box cache と hit index の破棄との整合を保つ。
- merge の tie は左を選び、index order を失わない。途中の scratch／部分 branch は valid=false のままで、取消 query を不完全構造へ進めない。読んだ範囲で追加の製品反例は発見していない。

## 新条件だけの native 統合 fixture

許可された `tests/paintperformancetest.c` に `--boundaries` 分岐を追加した。この分岐は旧 14 検査を実行しない。

- 普通 li の正のページ座標にある outside marker の BVH target／action と通常 walk の一致。
- summary outside marker の target／action 一致。
- 実 `::before` generated run の target／link action 一致。run がなければ skip 合格にせず失敗にする。
- 実 DOM／CSS／layout から 4096 positioned layers を構築し、collection が全件終わって未公開の sort 区間で host callback が取消を要求する。
- その取消が native flag と未公開 order に到達し、以前の coherent window を表す sentinel pixels が不変であることを要求する。

この fixture の freestanding Nocturne target `clang -fsyntax-only` と担当範囲 `git diff --check` は成功。担当はゲスト実行していない。主担当へ `/bin/paintperformancetest --boundaries` の限定実行を引き渡した。

fixture content hash（引渡し時）：`8ff7a31623d6c021fca4e11b25f03ab2e36d375f`。

## 限界

これは修理後の独立静的再査読と、新条件 fixture の構文確認までの証拠。取消実行・pixel 不変・実サイト操作の合格はまだ担当自身の観測ではない。主担当の新条件実行と実サイト受入結果を区別して取り込む必要がある。
