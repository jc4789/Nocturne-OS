# CSS共通修理の第一報

## 対象と観測

- 前回性能変更を含む commit `0519429` を基準にし、`css.c` の selector解析・matcher・index・dirty のみを編集した。font src の2関数は別担当との明示分担である。
- ユーザーの比較12画像を自分で閲覧した。DDGでは検索欄の輪郭と影がなく、Firefox比較ではある。Redditでは左navigationが空、投稿画像のfitも異なる。GitHubでは初期unstyledから段階的style適用、最終でもAboutが本文下へ流れる。画像だけでparser原因とは断定しない。
- 取得したDDGの `s.8695f6b482ed323670fc.css` / `r.5ce53de3523331a1dced.css` / `wpl.main.47eab9025cbaf32c764b.css` とGitHubのprimer/repository/code-view CSSで使用機能を調べた。実CSSは `:has()` を使用するのに、基準native parserはその引数を解析せず `PC_NEVER` を返す。製品の共通未実装境界である。
- DDG検索欄の標準 `.search,.search--adv` は通常の半透明border/box-shadow宣言である。`:has` 修理だけでこの輪郭が直るとはまだ証明していない。

## 製品修理

- `:has()` はstrict relative selector listを解析し、descendant/child/adjacent/general siblingと連続combinatorをnative DOMで照合する。禁止nested `:has()` / pseudo-elementはinvalid。`:is()` / `:where()` 内のforgiving recoveryは維持する。specificityはargument listの最大値。
- relative内部anchorとDOM queryの `:scope` referenceを別の明示contextで渡す。Shadow scopeはさらに独立しており、`:has` はShadow/flat tree/generated boxへ横断しない。
- 旧selector list 64件・compound 32個の黙切捨てを動的arena配列に置換。DOM queryだけにあった65536字・1MiB arena quotaも除去し、実確保失敗は従来のtrapで失敗する。
- stylesheet indexにrelational subjectのid/class/tag索引を追加。mutationの祖先と、必要な場合だけ先行siblingの候補subjectをdirtyにする。単にCSSに `:has` があるだけで全DOMをrestyleする従来経路は除去。subject配下のinheritance/外側combinatorは従来のsubtreedirtyとsibling境界で保全する。
- `CSS.supports(selector(:has(...)))` と実 `@supports` が同じnative parserへ接続したまま実機能を報告する。支持偽装やサイト専用条件を追加していない。

## 規格

- [CSS Conditional Rules Level 3](https://www.w3.org/TR/css-conditional-3/): true条件のみその場所で適用、false条件は非適用、supportは実機能に基づく。
- [Selectors Level 4 relational / specificity / relative selectors](https://www.w3.org/TR/selectors-4/): anchorとの関係、nestedhas/pseudo禁止、strict引数、argument最大specificity。

## 第一段階の確認と未確認

- 正規userland targetの freestanding syntax確認は `css.c` と `tests/cssrelationaltest.c` とも exit 0。
- 旧性能の実C限定 `python -X utf8 tests/check_cssperformance.py`: 22 checks / 0 failures。style pool・decl cache・AST ownershipだけの証拠であり、今回のmatcherや実サイト受入ではない。
- Windows host targetの最初のsyntax commandは `nocturne.h` とhost `ssize_t` ABI重複で失敗した。userland ELF targetへ訂正した結果を採用し、host成功を主張しない。
- 新native `tests/cssrelationaltest.c` は全4combinator、`:scope`、複合relative、禁則、変異・reparent、無関係style再用、長いlist/compoundを製品経路で検査する。まだguest実行前。
- 関連WPT原本候補は `css/selectors/has-relative-argument.html`、`has-argument-with-explicit-scope.html`、`parsing/parse-has-disallow-nesting-has-inside-has.html`。前2頁はWindow named propertyを使うため、その基盤の未対応でassertion前失敗ならselector合否と区別する。原本の書換え・alias注入・skipは禁止。
- 実サイト最終表示、入力、上下scroll、初期ロード速度、Hyper-Vはこの第一報では未検証。基準3VM・受入は主担当の責務で、こちらはVMを起動していない。

## 次

主担当の一括guest結果を受け、mutation/Shadow/failure反例を別第二報で検討し、同3実サイトの最終表示と結び付ける。
