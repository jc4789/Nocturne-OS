# computed pseudo body と box 生成の分離・第一報

- 主担当から、値衝突修理後の native placeholder paint 原 12 条件は全成功したが、追加 CSSOM 原 20 条件の input::before 作者色が不正との結果を受け取った。今回修理を前回の 12 成功で受入済みにしない。
- 原因：cascade が input 等では before/after の算出を省き、通常 element でも content:none/display:none では算出した body を保持しなかった。box の不存在と CSSOM resolved style の不存在を混同していた。
- 共通修理：作者宣言が一致した before/after の computed body を既存 node style slot に保持する。`before_generated` / `after_generated` の非継承 bool で box 生成を別管理する。別 cache や毎 query の cascade 追加はない。
- 生成条件は既存 box.c atomic_kind の意味から求める。AT_NONE と AT_INLINE_BLOCK は既存 children を生成するため許可し、button と object fallback を保全。replaced input/img/SVG 等の専用描画、content:none、display:none は body だけ保持する。
- nongenerated body を font wanted 選択・font refresh・背景 image 登録へ流さない。生成 body の font 取得は従来経路。layout 比較では生成 flag と生成された body だけを比較するので、nongenerated 作者色だけの更新は geometry を変更しない。
- 支持確認：target syntax と diff check 成功。既存 host の新生成反例を追加した結果は 24 条件・失敗 0（`build/browser-pseudo-generation-host-20261013.log`）。実媒体の原 paint 12 / CSSOM 20 は主担当担当。
- 新 native 支持入口 `tests/csscachetest.c --pseudo-bodies` は 12 条件。nongenerated body、button/object 維持、不要な背景/font 取得の不在、作者色変更で published geometry 維持を確認する。まだ未実行である。
- 根拠は CSSOM の getComputedStyle/resolved value 境界：https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle

追加編集は既存 `box.c` / `doc.c` / `css.c` / `cssprop.c` / `webi.h` と既存支持試験に統合。サイト例外なし。source を凍結して主担当へ引き渡した。
