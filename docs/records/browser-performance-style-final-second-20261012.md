# CSS 担当・凍結製品の最終再考察

最終第一報とは別 artifact。追加試験を反復せず、最新共有型／文書破棄経路と、独立 paint 査読で発見した反例の処置を照合した。

## 現在の共有 lifetime を再確認

- `webi.h` の `style_slots`／`style_previous_slots` と `style_declarations` は style の current／retired ownership であり、box cache／文書の measurement cache と同じ field を兼用していない。`style_pool` の destructor は allocation-owned node destructor 後に呼ばれる。
- `doc.c` の通常 restyle は、worker join 後に box borrows を `boxes_restyle` で更新してから `css_styles_release` を呼ぶ。box topology が変わる場合は `boxes_discard` が先である。
- 途中 native cancel は box discard と全 style unpublish 後に退役世代を返却する。旧画面へ transactional rollback するという未実装の保証は加えず、既存 coherent-DOM recovery の方式を維持する。
- したがって、過去の追加 11／0 が確認した pseudo／underlying chain、旧親 variable borrow、未公開部分 slot の回収、slab／cache 確保失敗の境界と、現在の文書接続に矛盾は発見していない。今回この 11 件を再実行したという意味ではない。

## paint 独立査読の反例を実証側へ接続

第一査読で発見した通常 li marker の高速／fallback 差と sorting 中断点不足は、主担当の製品修理と第二査読で source 上の処置を確認した。自分で追加した `--boundaries` は、主担当の実 guest serial で 17／0 を確認した。

ただし `build/browser-performance-boundaries-guest-20261012/test-serial.log:64` の wrapper 総括 FAIL は消さない。実 17／0、`tests/runtests.c` の prefix 訂正、prefix 訂正後の未再実行を別々に扱う。既存 paint 14／0 は第一統合媒体の結果として保持する。

## 判定範囲

凍結製品について、CSS 固定部共有／宣言 AST／退役 lifetime の実装と、参照した限定 C・native guest・独立静的査読の範囲には既知の未修理反例が残っていない。これはブラウザー全体の回帰なし、実サイト性能の全面達成、Hyper-V の受入を意味しない。最新実サイトの最終結果と map 同期は主担当へ引き渡す。
