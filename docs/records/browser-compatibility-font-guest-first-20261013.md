# フォント担当：最初の統合 guest と失敗切り分け

主担当の `build/browser-compat-guest-20261013/test-serial.log` を直接読んだ。

- `--parallel-css`：12 checks / 0 failures。全候補発行、重複・rescan・逆順完了、redirect/import、同 URL の stream 取消／新要求／旧完了拒否が製品 native 経路で成功。
- 主担当 `--render`：11 checks / 0 failures。自分のホスト検査や実サイト成功とは区別。
- 自分の `--font-sources`：10 checks / 1 failure。「failed candidate does not remeasure unchanged font metrics」だけ失敗。他 9 件は成功。元結果と元 fixture 条件は原ログ／先行 artifact／Git 差分から保全する。

## 条件の切り分け

旧基準 `0519429` と現在の `css_refresh_font_selection` は、ともに未消費 resource／style／index の dirty があると保守的に cascade へ戻り、quiet で名前付き face が NULL → NULL なら次候補 wanted だけを更新して計測を再用する。同じ保守分岐を今回のフォント修理が新しく追加したわけではない。

元 fixture は `web_parse` の静的文書だった。計測再用の assertion に必要な「実 live document で pending resource scan を消費し、style/index/viewport が quiet」という前提を保証していなかった。フォント失敗の副作用と元からの dirty の消費を区別できないため、これだけから実ブラウザーの再計測回帰とは断定しない。

成功期待を弱める代わりに、同じ既存 `tests/csscachetest.c` へ独立 `--font-failure-noop` を追加した。実 live parser と tick、未完了 font host 要求を保持する条件で quiet を assertion に含める。無範囲 font の失敗 → 次 wanted、root box、layout revision、`font_metrics_generation` を一つの assertion で確認し、前後の数値を診断として出力する。

これは原 WPT ではない。失敗した一つの前提だけを区別する新条件であり、既存の他 9 件、parallel、render を同じ条件で再実行する依頼ではない。主担当の次の限定 guest で判定する。現時点でその結果は未確認、製品 source は変更していない。

主担当の指示で、repository の元 `--font-sources` も正しい live 条件へ訂正した。`web_live` の parser／tick で初期 resource/style work を通常経路で消費し、元の計測不変 assertion に quiet 前提を追加して強くした。他 9 件の期待は不変。resource／dirty の直接消去や製品 hook の迂回はない。訂正後の 10 件モードはまだ再実行しておらず、旧 9 成功を新条件の合格へ昇格させない。既知の不適切条件を恒久 fixture に残すことと、元失敗証拠の保全を区別した。
