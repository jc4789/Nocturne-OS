# レイアウト・通信独立担当 最終参照更新 第1報

製品凍結後、主担当の依頼により最新 `webi.h` / `doc.c`、レイアウトcache、CSS値比較/box restyle、媒体寸法失効と通信スケジューラーを再読した。製品編集、検査再実行、VM操作は行っていない。

## 現在の参照

- `layout.c`: `6cbd0e0537b7dfb06bdd87e17f2a979389779f7772d2926c9cd44f21aecf3ed0`
- `layout_text_cache.h`: `1f87188fd92f3cbc0fe5fc368fc4394ede84bf3348ad3ecb905c30c41af6443b`
- `webi.h`: `79c76650260013a64d4a84d7ef569b6d32b14ee3cfcb8e149fea49baa0bde81a`
- `doc.c`: `eecfab14af5fb77c4840964d956a76d53fe4a470c0d855769d82f03154d200e2`
- `webnet.c`: `8c55823434826ee09f285daf44660e9cb9e045ee6a0ad000a35cbb8ad7af5007`

## 保存済み実装と証拠

dirty枝prepare、安全なinner/post再利用、相対offset自身のcb寸法更新、overflow/最下端の統合集計、run-owned descent、font世代intrinsic失効、文書owned文字cache、fontchoice値共有/refcount/LRU/OOM/reentry/freeは現在のソースに保持されている。

- 初期実装の79件: `docs/browser-performance-layout-20261012.md`。この初期snapshotの証拠を現在の全面受入へ流用しない。
- interningとpost追加28境界: `docs/browser-performance-layout-round2-20261012.md`。
- 通信の独立反例3件と既存callback寿命境界: `docs/browser-performance-network-review-20261012.md`。
- 修理後scheduler追加26境界の実製品関数抽出/成功/範囲: `docs/browser-performance-network-review-second-20261012.md` と小さい実行log。元の証拠を上書きせず保持。
- 主担当から、最新正規build後のnative `networkboundaries` が6079ms、失敗0、callback generation取消loan/nested guardも成功と受領した。この担当自身のVM実行ではないため、主担当のguest証拠と区別する。

## 統合寿命の再読

- `web_free` は旧box借用を解除後に `layout_cache_free` を呼び、フォントresourceを閉じる前に文書cacheを回収する。
- 新しいCSS共有slotと文書paint/hit定義が同じ `webi.h` に共存。layout cache pointer/prototype/box準備・post・intrinsic世代・run descentは失われていない。
- box restyleは値が変わったboxを失効し、旧style arenaを回収する前にrun/deco借用を新styleへ更新する。字体range/face値とtranslationをCSS値比較に含む。
- 媒体natural size変更は `layout_invalidate(node->box)` を通知し、画素だけの変更はdamage通知へ分離されている。

## 限界

通信origin capは初回request originのみ。CPU policyは最低8を含む `MAX(8, CPU*4)`。redirect先のsocket数6、実メモリ圧力下の性能、実動画/実サイト/GUI全体の無回帰はこの担当の限定host検査では証明していない。最新二サイトは主担当が操作中。

次は現共有定義での失効漏れ・解放順・callback loanを静的に再考した第2報を提出し、主担当の独立受入へ渡す。
