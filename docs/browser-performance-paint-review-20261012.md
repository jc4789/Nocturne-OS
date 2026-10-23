# paint／hit 最適化の独立静的査読

主担当から追加依頼された read-only 査読。VM、既存検査の再実行、主担当所有ソースの編集はしていない。親から受け取った paintperformance 14／0、translate／alpha 成功を全面仕様の証明にはしていない。

査読時の Git content hash：`paint.c = d5f3a043d025f4ff658af238fe83c9edff865db5`、`box.c = 2194798a7f297648148b3334d9d1250904730e14`。

## 修理を要する静的反例

### outside marker の高速／fallback target 不一致

`paint.c:1278-1283` は `P->index_build` 限定で通常 list-item の outside marker に target primitive を追加する。通常 `M_HIT` walk では、border box 外の marker から target を設定するのは summary／既定 details legend だけで、普通の li に同じ処理がない。

例：普通の `<ul><li>項目</li></ul>` の li 左外 marker をヒットする点。最適化側は li を返せるが、fallback は ul 等の祖先になる。既存 paintperformance の HTML は普通の outside-marker li を含んでおらず、14／0 はこの分岐の反証にならない。これはソースから構成した反例で、実行して観測した回帰とは区別する。

marker を li target とする修理なら、同じ矩形・target 処理を通常 walk と BVH 収集に共通適用する必要がある。index だけに意味変更を置かない。主担当へ即時連絡済み。

## 取消・応答の残る境界

`collect_layers`、`sort_layers` の merge、`hit_partition` の bounds 全走査と階層ごとの native `qsort` に native checkpoint がない。primitive 収集中の 64 件ごとの checkpoint は、収集後の全体 sort／partition には効かない。実 `stdlib.c:198-224` の qsort も中断点を持たない二方向 quicksort である。

大規模初回 mousemove／stacking rebuild ではこの連続処理区間に取消と GUI service が届かない。実測したフリーズとは断定しないが、今回のネイティブ中断点要件に対する未対応境界である。batch 単位の checkpoint と、取消した layer／index を publish しない失敗伝播が必要。毎要素の時計 syscall は不要。主担当へ連絡済み。

## 反例を発見しなかった確認範囲

- merge は等しい z-index に左側を選択し、元の tree order を保つ。count は既存 pvec の int 上限内で、width doubling は count により終了する。
- layer vector は box 所有であり、通常 paint の借用終了時には解放しない。`box.c:unpublish_boxes` は古い published tree の全 box vector を回収し、`boxes_discard` は hit index を arena 解放前に破棄する。top-layer box／backdrop も root へ append されるので旧 tree cleanup の対象。
- BVH の rect は現在 clip との交差で、query は半開区間。branch は最大 order によって遅い paint-order primitive を選ぶ。透明な `pointer-events:auto` overlay も削除しない。
- 通常 text、pseudo text、既定 legend は `text_box` の node から実 run の node へ接続される。anonymous ellipsis は node と link が NULL であり、別の synthetic target を作らない。generated-run だけを理由とする追加反例は発見していない。
- dialog／fullscreen は最適化 lookup から通常 walk へ明示 fallback し、forced top-layer target を空間索引へ誤って昇格させない。
- layout／DOM／element scroll・marquee の visual revision、viewport position、font generation を再構築条件とし、href は index へ保存せず既存 `web_node_action`／URL 解決を通す。
- index 収集の allocation／checkpoint failure は valid=false のまま扱い、native cancel 時は入力結果を公開しない。

上記は独立ソース査読の範囲だけであり、実サイトの操作成功・性能改善・全組合せの hit 正当性の受入ではない。主担当の修理と新条件の確認結果を待つ。
