# フォント・CSS 資源供給の第二報

第一報 `browser-compatibility-font-first-20261013.md` は変更せず保全。第二考察で、形式選択だけでは GitHub 初期ロードの直列待ちを修理しないことを確認し、主担当の追加依頼により既存 CSS 供給経路を修理した。

## 直列供給の製品修理

旧 `request_styles` は `web_pending_stylesheet` の先頭 URL 一つと `pending_kind(P_CSS)` 全体ゲートにより、通信層に複数 worker があっても CSS を一つしか渡さなかった。

- `cached_css.loading_id`：`pending_css` の走査 snapshot と独立した、文書寿命の要求 identity。
- `doc_css_request_next(d,&cursor)`：作者順に全候補を列挙し、既に完了／要求中の URL を除外。cache 所有を安全な calloc／文字列／fallible vector で作成する。資源数の新しい固定上限はない。
- `js_pending.css`：安定 cache 記録への借用。送信前に id を claim し、毎 tick と duplicate link／import の二重供給を防止する。資源 rescan は `pending_css` を作り直すが claim は失わない。
- 完了と解放は id 一致でのみ処理。既存の document stream generation の検査・取消を残し、古い要求の解放が新要求の id を消さない。
- `doc_css_loaded`：要求 URL は cache identity、redirect の最終 URL は import の相対解決 base。最終 base の確保失敗時は古い base で bytes を公開しない。作者の stylesheet 順序／import child-before-parent は既存 scan／AST インスタンス責務を維持する。
- 通信の実並列／per-origin／RAM 予算は既存 `webnet` に委任。全資源待ちでページを隠す処理、サイト例外、資源容量・取得期限の変更はない。

単一 writer は `dom_compat` と合意。変更は `user/libc/web/js.c` の CSS pending／request 部分、`doc.c` の CSS cache helper／completion、`webi.h` の cache/API。modulepreload 修理は DOM 担当が同じ既存ファイルの別領域を所有する。

## 反例と検証区分

第一報の形式選択 26 checks と原 WOFF2 4 checks、元 container 34 checks の結果はその範囲でのみ使う。今回の並列供給実装の動作証拠へ昇格させない。

`tests/csscachetest.c` の独立 `--font-sources` は実 `web_parse`／computed style／`doc_font_loaded`／layout を通し、無ヒント失敗候補から次候補への選択、失敗時の計測再用、実 Inter bytes の face 公開を確認する。

同じ既存試験の `--parallel-css` は実 `web_live`／native pending の host request capture を通し、次を確認する設計である。

1. 応答ゼロのまま三つの CSS を供給し、同 URL link は一要求。
2. 途中の resource rescan 後も重複発行なし。
3. 逆順の完了でも作者 DOM source order を保持。
4. redirect base から import URL を解決し、child-before-parent を保持。
5. document.open で旧要求を取消、新同 URL に別 id を払い出し、旧 completion は新 stream へ入らない。

この二つは主担当の統合 guest バッチ待ちで、現在の本報では成功と数えない。host request capture は実 socket 性能ではなく、原 WPT でもない。製品 `doc.c/js.c` と fixture の host 構文確認は成功。最初の構文確認は MinGW の ssize_t ABI 二重定義で失敗し、既存 host decoder と同じ `_SSIZE_T_DEFINED=1` で host 条件を正した。製品を変更してそのエラーを隠していない。

## 他担当修理との独立 seam review

主担当の head stylesheet 初期 render gate を読み、import pending は root sheet の order を持つこと、scope/private／inactive media の除外、cache.done による解除、取得を取消しない fallback と照合した。

期限 API の確定反例を発見した。新 render 期限が相対 `deadline-now` の値だったが、`web_deadline` と browser の consumer は絶対 uptime を使うため、通常 uptime では即時 wake の busy loop になる。主担当へ報告し、現在の source は `(int64_t)d->initial_render_deadline` に修理されたことを再読した。自分で VM／新試験を起動して確認したという意味ではない。

## 残件と次

フォント／CSS queue source と自分の fixture 編集は凍結し主担当へ引き渡した。容量エラー後の「新追加試験なし」に従い、追加 VM・server・全 build・検査は起動していない。Baike を含む 4 サイト、同条件の実画面・上下 scroll・入力、関連 WPT 原本結果は主担当が担当する。原 WPT の FontFace src CSSOM 表現や Window named access を native 列挙成功から推測しない。

次は主担当がまとめて行う最新媒体／guest 結果を読み、実 source binding と独立 review に更新する。全サイト表示完了・Chrome より速い・性能回帰なしは本報の主張ではない。
