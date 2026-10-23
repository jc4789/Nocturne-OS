# 実スクロール後の横一線クリップ・読取り診断

## 実際に見た画像

`build/nocturne-platform/browser-perf-after-shaka-20261012/after-player.png` と `after-player-down.png` をこの担当自身が画像閲覧で確認した。初期のvideo底は約573px、実PgDn後は約163pxで約410px上へ移動。card/image上端も約410px移動する一方、全cardが約603～604pxの同じ水平線で切れ、その下は白い。初期viewport底1015pxから約411pxを引いた位置と一致する。

既存画像/logは変更していない。VM操作・同検査の反復・製品編集もしていない。

## 現ソースと共通祖先の境界

- 既存serialのmedia祖先geometryはviewport 1908×947、depth 4のdivはheight 947・overflow `OV_AUTO`、depth 5はposition 2、すなわち `POS_ABSOLUTE`。viewport固定position 3ではない。
- `browser.c:458-479` はPgDn/wheelをdocument rootの `scroll_to` へ直接送る。
- `paint.c:1361-1363` のoverflow clipは `P.oy + ancestor visual y + height`。`web_paint` の `P.oy = TB - doc_y` なので、高さ947のscrollport自身をdocumentとともに約411px動かすと、祖先clipの下端は画面603～604pxになる。画像の共通切断線と整合する。
- 個々のcard height/relative cacheが同時に古いという説明より、共通のscrollportがroot scrollで移動した説明が強い。ただし今回の画像だけでcache導入の回帰原因を確定したわけではない。

## 製品境界の具体的反例

`layout_doc` は正しくclipping/actual cb/fixed除外を扱う `scroll_areas` の後、すべての `abs_boxes` を `box_abs_y(a) + a.height` で文書最下端へ無条件unionする。この別unionはoverflow祖先を考慮しない。

静的な反例は、root高さ100、内側clip box高さ40、そのclip boxをcbとするabsolute boxがy300・高さ50の状態。`scroll_area_publish` はclip boxのprivate overflowをrootへ出さず、root.scroll_hは100。旧absolute unionだけがdoc_hを350へ拡大し、scrollportから白い領域へのdocument scrollを許す。これはソースから導いた反例で、今回新fixtureを実行した結果ではない。

## 最小修理案

1. 文書の最下端は `max(root.height, root.scroll_h)` を主経路にし、clipを無視した全absolute別unionを除去する。absoluteが途中clipより外側のcbを持つ場合は、既存 `scroll_area_publish` が実cbへ直接publishするため、正当にescapeするoverflowを一律捨てる修理ではない。
2. extentだけ直すと、このようなOV_AUTO本文の続きをroot-only PgDn/wheelで読めなくなる。既存 `doc_element_scroll` を再利用し、実hit/focusから最寄りのOV_AUTO/OV_SCROLLへnative入力を送り、clampした残deltaだけ外側/rootへ渡す共通経路が必要。OV_HIDDENはprogrammaticスクロールと区別する。既存処理はvisual_revisionとpaint_dirtyを変え、layout/CSSを変えない。
3. scrollport高さを947から内容1457へ強制拡大する、overflow clipを外す、cache全無効化で画面を隠す、サイトDOMを特例化する修理は行わない。

root extentと入力routingの修理を主担当へ提案済み。この診断を実サイト再確認・性能無回帰の成功証拠にはしていない。
