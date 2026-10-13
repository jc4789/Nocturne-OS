# CSS 担当・凍結製品の最終第一報

過去の第一報／第二報／paint 査読 evidence は変更せず、最終 `webi.h`／`doc.c` の実装を再読して現在 source へ binding を更新する。この段階では製品編集、build、VM 起動、同じ検査の再実行はしていない。

## 現在の責務と実装

- 固定 computed style 本体は文書の共有 slab、可変 tail は要素の遅延 arena。current／previous slot chain と variable arena の retirement を既存 publication join に接続している。
- parsed inline／WAAPI 宣言は immutable snapshot で、所有する bytes／length／base を比較する。computed custom-property 名と値は variable tail の所有コピーで、cache 交換から継承 borrow を分離している。
- 文書より allocation-owned node が長生きする場合、pool は最後の slot 返却まで保持される。pseudo／underlying の全 body も chain で回収される。
- 最新共有 header は style pool、文書 text measurement cache、paint order／hit index を別の field／所有責務として保持する。最新 `web_free` は旧 box borrower と retirement を先に破棄し、helper document と owned node の cleanup 後に style pool 所有を外す。
- 自分で実行した限定 C は初回 22／0 と追加境界 11／0。これを再実行せず、過去の限定証拠として保持する。初回 plain 4096 node の固定 body 保持量は退役余裕込み 5,724,680 バイトで per-node tail 未確保。これは実サイト速度の測定ではない。

## 主担当が実行し、この担当も serial を読んだ結果

### 第一統合媒体

`build/browser-performance-guest-20261012/test-serial.log`：styleincremental は 150 checks／0 failures（166-173 行）、既存 paint は 14 checks／0 failed（174-175 行）。共有ページは 25／0、translate／alpha も成功しているが、同じ媒体の CSS animation と通信の失敗を全体成功へ上書きしない。

### 修理後の追加 paint 境界

`build/browser-performance-boundaries-guest-20261012/test-serial.log:64-65` に実 `paintperformance boundaries: 17 checks, 0 failed` が残る。この 17 件は通常 li marker／summary marker／generated run／4096 layer sort 取消／非公開 order／旧 window pixel 保持を含む、新条件だけの native 実行である。

同じ log の総括は、期待 prefix が古い `paintperformance: ` だったため FAIL になっている。主担当は `tests/runtests.c:144-145` の prefix を実出力の `paintperformance boundaries: ` へ訂正した。製品と 17 検査を変えて合格にしたものではなく、元の総括 FAIL と実 17／0 の両方を保存している。prefix 訂正後の再実行を行ったとは報告しない。

### 修理した別境界

`build/browser-performance-repaired-guest-20261012/test-serial.log:64-66` は CSS animation 94／0、worker-network の PASS を記録する。今回の CSS pool の成功を、通信回帰が初めから存在しなかったという説明に使わない。

## 保存した静的 snapshot

Git content hash：CSS `1f6d92e614e3a500e97c18eda473b44d63b3ebc8`、共有 header `3d3d866bee89e49f0064f64a1c4d99d667d4c224`、文書 `2b754e72fbb45c673f9853fa544560850fc4ffd9`、style fixture `69a4e4ed1c39aa9a8a3a7c8099cc23989a40545e`、paint 境界 fixture `8ff7a31623d6c021fca4e11b25f03ab2e36d375f`。

## 未確認

実サイトの最新二 VM は主担当が操作している。この担当の code／host／native serial 証拠を、実サイト表示・スクロール・動画速度の全面成功、Hyper-V 成功へ昇格させない。次は主担当の独立 review と最終 map 同期。
