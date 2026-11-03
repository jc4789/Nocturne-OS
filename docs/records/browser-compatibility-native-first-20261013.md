# 長い native 処理：第一切り分けと実装修理

主担当から百度百科の長い tick を修理する追加担当を受領。`build/nocturne-platform/compat-after2-baidu-20261013/serial.log` を範囲指定で読んだ。

## 別々の長時間境界

- 211–212行付近：posted task 21785 ms、native 300015 calls / 19905 ms、metadata 2 syncs / 15040 visits / 1 ms、resource 3 scans / 8 ms。
- 1761–1763行付近：timer 658 は 22304 ms、layout 22063 ms / 87 flushes、DOM revisions 268、89 inserts、resource 1 scan / 3 ms、script 87 scans / 1018857 visits / 39 ms。
- 1807–1808行付近：wall 23270 ms、ticks 14 / 22744 ms / max 22528、style-nodes の累積 wall 71423 ms。

最初は大量 native 呼出し、後者は変更と寸法読出しの交互処理であり、通信待ち／全資源走査へ一括帰属しない。累積並列 stage wall と一 task の時間を加算していない。提供画像の実行が debug 条件だったかは不明。

## 診断処理自身の費用を修理

`native_dom_magic` の debug 分岐は、すべての outer native 呼出しの前後で `uptime_ms()` を呼んでいた。`user/libc/sys.c` には安全な user TSC／Hyper-V reference 経路があるが、どちらも使えない環境は毎回 SYS_UPTIME へ戻る。実 guest の clock_info flags はまだ確認していないため、時計費用だけを 2 分ロード全体の確定原因としない。

今回の変更は、安価な xorshift で outer 呼出しの約 1/64 を計時する。周期的な作者の getter パターンとの 64-call 位相一致を避ける。全 invocation の `native_calls` は従来どおり正確に数え、`native_samples` も正確に記録。`native_ms` はサンプル時間を 64 倍した推定値であり、overflow は飽和処理。Timer／Posted のログへ「native time sampled estimate, ~1/64」と実 sample 数を明記した。値は正確な inclusive 全量計時ではなく、少数標本や分布次第で誤差があり、wall より大きくなる場合もある。

通常の非 debug 分岐には時計読出しも sampling 演算も入らない。DOM の変換、反応、例外、結果、所有権、watchdog、中断、media supply は変更しない。時計 ABI を危険な TSC 前提へ切り替えたり、期限を延長して成功に見せたりしていない。

## 構造変更の余計な広域再カスケードを修理

DOM の child-list 通知は既に sibling 集合の「親」を渡している。旧 `css_mark_dirty` は sheet のどこかに `+` / `~` があるだけで、さらに祖父へ昇格した。挿入と寸法読出しの交互処理で無関係な forest まで再 style する余分な一段を削除した。

- `css_mark_child_list_dirty` と `changed_geometry` の構造 reason を既存 DOM/CSS 層へ接続。
- 親とその **全 descendant** の再 style は維持。`cascade_node` の `c->full=redo` を省略していない。通常の継承、非継承プロパティへの明示 inherit、custom vars、box-shadow の retirement 借用、祖先 combinator と positional selector を保持する。
- 親の `:empty`／状態が外側 sibling に作用する compound は、既存 CSS index に subject-key として登録。該当候補は旧広域境界へ戻す。判定は stable tag/id/class だけで絞り、動的 predicate の新しい結果だけで「古い一致」を見失わない。
- `:has` の ancestor／previous-sibling witness invalidator は維持。
- Shadow tree／host／slot／foreign／fragment、および移動 subtree に focus／hover／active がある場合は旧保守経路。変異途中の box を読まず、slot 割当をここで flush しない。

CSS 担当から index/dirty/cascade 部分と `cssrelationaltest.c` の単一 writer 合意を取得。DOM 担当の modulepreload、主担当の style_access／table-layout、CSS 担当の box-shadow parser／paint は触らない。

## 確認範囲

`css.c/dom.c/js.c` と新 fixture は host 構文確認成功、CRLF-aware diff check 成功。native guest と同条件の実サイト速度は主担当の最新媒体待ち。新 `--child-list` モードだけを依頼し、既存成功群の無変更反復を求めていない。

次は、祖父境界を省略しても外側 sibling の古い一致を保持する反例、および正確な時間と sampled estimate の区別を第二考察する。
