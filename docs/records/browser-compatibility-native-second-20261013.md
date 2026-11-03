# 長い native 処理：第二反例考察

第一報 `browser-compatibility-native-first-20261013.md` を不変で保全。最弱前提を「構造変更なら親の外側 sibling と ancestor 状態は変わらない」という誤った一般化として検討した。

## 外側 sibling を保持する反例

`.empty:empty + .after` は親へ子を追加／除去すると隣の `.after` の結果が変わる。親と全 descendant だけの再 style では不十分。この compound を既存 subject-key index へ登録し、mutation parent が stable keys に当たると祖父境界を維持した。

`:not(:empty)`／`:is` 等の論理 selector list 内の complex alternatives も、該当 nested compound を登録する。`:has` の内側はその既存 ancestor/witness invalidator を使い、子リスト用 index で代替しない。`.witness:has(> .flag) + .effect` の追加と除去では古い witness の一致も更新する。

同じ理由で focus-within／hover／active の descendant を移動／削除する場合を検討した。新 helper に mutation subtree を渡し、現在の native interaction target がその subtree 内なら旧保守境界へ戻す。DOM links だけを読む。割当や未公開 layout を観測しない。Shadow root 内や host／slot／foreign／fragment は今回の縮小対象から外した。

## 継承・可変データの反例

親の computed style が同じように見えても、子は explicit inherit や custom var、arena の借用を持ち得る。box-shadow は非継承プロパティだが `inherit` なら親のリストを借用するため、親の paint-only 更新を理由に子の再 style を飛ばしていない。親の全 descendant cascade と既存 retirement/join を残す設計で、この危険な仮定を採用しなかった。

## 限定 native 確認の新条件

既存 `tests/cssrelationaltest.c` に独立 `--child-list` を追加した。

- unrelated の 200 要素 forest がある状態で、別の小さな親へ child を追加。
- sheet に無関係な `+` selector があっても、unrelated style の identity とその子の identity を維持し、再 style visits を 20 未満に抑える。これは fixture のアルゴリズム確認値で、製品 node 数の上限ではない。
- 親の `first-child` と子同士の `+` を更新。
- 親 `:empty` による外側 sibling の追加／除去。
- `:has` の parent witness による外側 sibling の追加／除去。

この新 mode はまだ guest 実行待ち。原 WPT の代用ではない。既存 shadow/relational/WPT 成功を新 helper の未実行結果に流用しない。

## 診断時間の反例

sampling した `native_ms` を精密な全量時間と表示すると、時計 overhead を修理しても報告が誤る。実 sample 数と sampled estimate の表示を出し、task wall／layout flush の実計時と区別した。非 debug 性能は別条件で確認する。ユーザー提供画像の debug 有無は不明であり、sampling 修理だけで通常ブラウザーの 2 分ロードが解消したとは主張しない。

次は主担当が取得する最新 guest mode と同条件 Baike 実画面／操作の結果を読み、未達があれば製品修理へ戻る。
