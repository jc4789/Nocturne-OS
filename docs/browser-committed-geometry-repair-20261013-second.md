# committed geometry と内部 layout cache の区別・独立第二考察

- 当初の「内部 cache がない場合は geometry も使えない」を診断が反証した。初回に public layout_valid と post_valid が成立し、根の font/寸法が同じなのに内部 cache だけ false だった。保守条件の意味を混同し、追加 layout 一回を要求していた。
- 内部 cache_valid を false→true に書き換える修理はしない。内部 formatting の入力キー・安全性は既存 layout.c が管理し、必要な実 layout 時には引き続き元の条件で判定される。
- 修理で緩めたのは内部 cache の有無と公開済み geometry の関連だけ。新 box には post_valid がなく、実寸法・font・intrinsic 画像・viewport 変更は dirty や世代不一致を使う。原 5 反例の期待を変えず、これらを再使用扱いにしていないことを確認する。
- 子 frame/find は共通 committed 出口へ合流する既存修理を保全。paint layer と hit index は restyle 対象の validity/visual 世代で更新し、geometry epoch を維持しても古い描画意味を残さない。
- 原 diagnostic の一失敗は原因確定の証拠であり、新実装の合格ではない。主担当が最新全媒体で原 17 条件と実サイト操作を確認するまで、geometry 回帰解消とは報告しない。
