# CSS 性能担当・第二考察

親担当から指定された弱い境界（allocation owner と cascade owner、途中取消、継承 variable tail、pseudo／underlying の寿命、確保失敗）だけを新しく検証した。第一報の 22 検査は繰り返していない。

## 新しい反証条件と結果

`python -X utf8 tests/check_cssperformance.py --second`：11 検査、失敗 0。

- before／after／backdrop と animation underlying、その before／after を合わせた旧 7 slot＋可変 tail を実 production の retirement 関数で保持。新 4 slot の publication 前後で生存 slot が 11→4 となり、旧 tail も解放される。現在の 4 slot も node cleanup で返却される。
- 実 `css_style_init` による子の parent custom-variable tail borrow を確認。親を retire して未公開 replacement slot を確保後、実 arena の quota failure による longjmp を発生させた。取消 join までは旧子が旧親の値を読め、tree unpublish と retirement release の後は公開 style／stack trap／旧 slot／旧 tail が残らない。
- coherent DOM からの回復に相当する新世代確保で、未公開だった途中 replacement slot と旧子 slot も返却され、生存 2 slot に戻る。
- slab の実 `malloc` 呼出しをホスト補助で 1 回失敗させた。pool lock が解放され、live 数 64 が保たれ、未完成 slot は公開されない。同じ pool の次の確保は成功する。
- cache 本体の実 `calloc` 呼出しを補助で 1 回失敗させた。旧完成 AST の `color:red` と `arena.trap==NULL` が保たれる。

## 独立した source 再検討

- `web_free` は最初に box borrowers と retired styles を破棄し、helper allocation document の cleanup と現在 node の cleanup が済んでから、その cascade 文書の pool 所有を外す。slot は自身の pool を指し、allocation document と pool 文書が異なっても最後の slot 返却まで pool が保持される。
- 本体を shared pool に置いても、名前・値・font choices 等の可変 tail の親子 borrow は従来の同一世代 retirement 単位を維持している。特に cached inline declarations の名前と値は computed tail にコピーされ、AST 交換が退役 style を壊さない。
- 取消は「旧画面に transaction rollback」する設計ではない。既存の `web_layout` は半端な tree を box discard／style unpublish で公開から外し、次の recovery で coherent DOM から構築し直す。本変更はこの取消方針を変えていない。
- `css_style_init` が固定本体を全体初期化し、子に `before`／`after`／`backdrop` を引き継がないことも新反証条件で確認した。

## 実製品統合用に加えた確認

`tests/styleincrementaltest.c` の既存統合検査に、実 native inline cache＋custom-variable＋child／before／after、同 source 再 cascade、source 変更、pseudo WAAPI／underlying の独立所有、detach／reinsert を追加した。更新 fixture の freestanding target syntax 検査は成功。この担当ではゲスト実行しておらず、主担当の統合 guest 検証に引き渡した。

担当範囲の `git diff --check` は成功。全体 diff check は並行編集中の `browser.c` の CRLF／trailing whitespace で失敗したため、主担当へ箇所を連絡した。別担当の変更は編集していない。

## 残る区別

この第二考察も限定実 C／静的証拠であり、実サイトの表示・入力・スクロール・映像の性能改善、SMP での競合費用、Hyper-V、配布媒体は証明していない。新しい製品修理の必要となる反例はこの担当の範囲では出なかったが、主担当の同条件実サイト確認と独立 review は引き続き必要。
