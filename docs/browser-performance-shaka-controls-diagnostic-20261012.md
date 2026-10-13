# 実 Shaka の controls／spinner／下部切断の切り分け

主担当から追加された read-only 原因調査。既存査読／最終報告 artifacts は変更していない。VM 起動・操作・検査反復・製品修理はこの担当では行っていない。

## 自分で見た実画像

- 変更後 `build/nocturne-platform/browser-perf-after-shaka-20261012/after-controls.png`、`after-controls-current.png`：異なる動画 frame が表示され、同じ中央 spinner が残り、controls が表示されない。
- 同 `after-player-down.png`：全 card が画面 y 約603-604 にそろって切れ、その下が白い。
- 基準 `build/nocturne-platform/browser-perf-base-native-shaka-20261012/base-player.png`／`base-player-hover.png`：停止中の controls がある。
- 同 `base-playing.png`／`screen-final.png`：動画進行中は controls がないが spinner もない。基準で同じ動画内外 pointer 移動や player PgDn 後を確認した画像ではないため、再表示不能・下部切断の回帰を肯定も否定もしない。

## 下部切断は実 ancestor overflow clip と数値一致

変更後 serial 4196 行の動画 paint は `rect=527,-357,854,492 clip=0,40,1908,576`。window の上側28pxを足すと clip 底604pxとなり、実画像の card 切断線と一致する。scroll は動画 y の54→-357から411px。

同 log の祖先では depth4 div が height947px／height100%／overflow3、depth3 main が内容高約1457.87px／overflow3。947＋native toolbar40−scroll411＝576 の clip で、内部 scroll-container の固定高が文書全体 scroll と一緒に上へ移動した結果で説明できる。

基準 serial 1373-1377 行にも同じ main／height947px div／overflow3 があり、祖先clipを作る通常 `paint_box`／`paint_stacked_layer` のこの境界は今回の paint cache 修理で置換されていない。

したがって snapshot の video 矩形破損より、browser の wheel／PageDown が文書 viewport へ送られる一方、ページが内部 main/container scroll を使う境界が有力。`browser.c:445-485` と wheel default `2339-2351` は文書 scroll であり、nearest scrollable ancestor への既定 scroll 配送がない。別サイト専用例外を作らず、既定scroll対象と overflowを共通境界で扱うべきである。ただし基準同操作が未観測のため「既存不具合と確認済み」とは言わない。

新しい snapshot は video 内矩形だけを保持・更新し、card 全幅の604px clipを直接生成しない。BVH は入力側の矩形索引であり、この描画底線を作らない。

## controls と spinner は次の分岐を残す

- `js_bootstrap.js:1077` は同じ target でも毎 mousemove を queue する。browser の native mousemove→`web_hover`→`flush_dom_layout`→dirty relayout／redraw は現ソースに接続されている。今回の BVH に controls target を消す具体的静的反例はここでは発見していない。
- snapshot は現在 committed overlay pixel を保持するので、overlay state の DOM／CSS dirty 通知が届かなければ spinner が固定しうる。ただし正しい通知があり普通 paint を行えば再構築するため、snapshot を原因と断定しない。
- native の実画像進行・`starved=0` と、著者側 waiting→canplay／playing の task dispatch は別。`avmedia.c:507-532` の MSE ready-state 判定と `js_avmedia.js:39-60` の ready-event transition が次の共通切り分け境界。
- 次の観測を1つ選ぶなら、実 pointer 移動時の native target／event ancestry と UI の native ready-state を読み取り専用に確認し、「入力が届かない」「state event が更新されない」「dirty／paint通知が失われる」を分ける。同じ再生試験を繰り返すだけでは判別できない。

実サイトの失敗は未解消。この担当の静的検査や以前の17／0で、実 controls 再表示・スクロール成功を上書きしていない。
