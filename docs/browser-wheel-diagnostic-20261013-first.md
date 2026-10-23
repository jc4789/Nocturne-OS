# 記事 wheel 未達・第一報

- 実画像 `build/nocturne-platform/compat-tls-restored-baike-home-20261013/article-body-wheel-down.png` を担当自身も見た。記事本文・画像・動画は表示されているが、主担当の実 wheel 上下操作で本文位置が変わらない。home の同操作成功を記事へ流用しない。
- serial の native media 祖先では root 高が約 13894px。短い本文による scroll 範囲ゼロと即断せず、wheel cancellation / root viewport max / 後続 offset 復元を区別する必要がある。
- 源の通常 default は `web_dispatch` の allowed に従う。passive fastpath が子 frame readiness で拒否されても、main loop は queued event を通常 dispatcher へ渡す。web_scroll_input が coherence 不成立で false を返すだけなら、残差は browser の root viewport fallback に渡る。ここだけを推測で変えない。
- 既存 doc.c に debug/profile 時の最小数値診断を追加：wheel allowed/cancelable、delta/tag、window blocking count、coherence/ready、document/viewport height、view offset、style/box 状態。native fastpath の cancellation ancestor と scroll-chain 不整合・残差も出す。作者 URL、token、DOM 文字列、コールバック本文は出さない。
- 通常時の時計・全 CSS 走査・新共有 ABI は追加しない。診断は wheel/default/scroll commit の既存境界で、profile guard に限定する。browser の viewport 前後・max 診断は主担当が別担当区間と調整して追加した。
- geometry 再使用条件は同じまま既存 doc.c の private helper へまとめ、既存 host に実 C の 10 反例を追加。全 34 条件・失敗 0（`build/browser-committed-geometry-host-20261013.log`）。原 native 17 条件の受入には置き換えない。
- source syntax/diff check 成功。all build / 実 wheel 再観察は主担当所有。記事 wheel の製品原因はまだ未確定で、修復済みとは報告しない。
