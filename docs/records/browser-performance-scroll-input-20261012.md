# 共通の内部スクロール修理（2026-10-12）

## 製品差分

- `layout.c` の文書高さは通常フローの下端と、包含ブロックを通して伝播した root の scrolling overflow を合成する。絶対配置をもう一度グローバルに union する処理を除去し、内部 scrollport の切り取られた絶対配置を root 高さへ漏らさない。通常の border/margin、非 clip の絶対配置、clip 外の包含ブロックへ escape する絶対配置は既存の overflow 伝播を使う。
- `doc.c` / 公開 `web.h` に `web_scroll_input`。最寄り `overflow:auto/scroll` から残 delta を消費し、Home/End は最寄り縦 overflow がある scrollport で止める。hidden はネイティブ入力対象から除外し、既存 CSSOM scrolling は保全した。fractional CSSOM 境界では残整数 delta をゼロ方向へ丸め、誤った反対方向／余分な整数ピクセルを root へ送らない。
- native input は layout・Shadow DOM flush・作者 callback を呼ばない。文書／box の committed layout、dirty・取消・回復状態、node 接続、slot snapshot、現在の frame/context chain を先に検証する。
- iframe 内 target は child element → 現在の child viewport → frame element の親要素 → 呼出元 root の順に消費する。viewport の実寸法・現文書世代・親子接続を検証し、無関係な文書は拒否する。native 入力は origin による特例や JS wrapper 転送を使わない。
- native element 通知は node 所有の deduplicated FIFO、配送 batch は別 intrusive link。全 batch を切り離してから通常 tick で既存 `queueElementScroll` / WeakMap / postTask 経路へ渡す。callback 中の新 native input は次 tick の FIFO に残る。owner／接続／文書 alive を各配送前に再確認し、disabled/free 時は borrower を解放する。
- child viewport 通知は文書 pending flag を通常 tick の既存 document scroll dispatch へ渡す。native 通知と通常 source、element と viewport は交互に処理し、postTask の starvation を避ける。pending source を既存 deadline へ接続した。
- `doc_paint_full` は無分類の visual change を owner と frame 親全体の full damage にする。CSSOM/native element・child viewport・画像完了へ接続。動画等の既知矩形が先に pending でも、scroll 後にその古い矩形だけを present しない。CSSOM child viewport の view position / visual revision も更新した。
- 独立 review で、sibling iframe の allocation-owned node を別 sibling へ adopt して native pending にしたまま root 破棄すると、allocation owner が先に free され後続 sibling FIFO に borrower が残る反例を発見。製品凍結をこの修理だけ一度解除し、allocation-owned node の解放前に現在の pending owner FIFO から解除する共通 unlink を追加した。旧 owner pending 清掃とどちらの破棄順でも flag/link を解放する。新 fixture は実 sibling context と adoption、破棄前の pending borrower 条件を確認して通常 root 破棄を通る。guest の非 crash だけを寿命安全性の全証明にはしない。
- `js_html_elements.js` / `js_bootstrap.js` の private hook を追加。生成物は手編集せず、主担当の正規 all build の依存経路で更新する。

## 今回の限定確認

- doc/layout/新 fixture の native-target 構文確認成功。
- 最初の js 構文確認は Lexbor compatibility include 指定不足で `memory.h` を発見できず失敗。Makefile の正規 include `-Iports/lexbor/include` を追加した js の確認は成功。
- 最終 doc/js/新 fixture 構文確認と CRLF-aware `git diff --check` は成功。既存 sandbox unused と IndexedDB logical-parentheses 警告は残る。
- 新 `tests/paintperformancetest.c --scroll-input` だけを追加。nested 残 delta、Home/End、hidden/CSSOM、fractional、dirty/ detached/ unrelated、async trusted dedup、作者 callback 中の native 再入力、Shadow DOM slot、detach/adopt pending borrower、clip 内外 absolute extent、実 srcdoc child viewport、既知 damage 後の scroll を製品経路で扱う。**子担当は実行していない。** 古い paint/layout/network の同条件検査、VM、実サイト試験も再実行していない。

## 観測・未確認

- 子担当も既存の実 Shaka 初期／PageDown 画面を開いて見た。共通の約604px切断線と overflow ancestor の viewport 高さが root-only scroll で一緒に移動する静的反例を記録済み。既存画像・log は変更しない。
- 主担当が browser 入力接続・関連 full-damage 通知、正規 all build・梱包・新 guest 条件・実サイト再操作を所有する。本記録の構文成功や新 fixture の追加は、実画面修理・回帰なし・性能改善の証明ではない。
- 新製品 source の凍結後、主担当の fresh repository 同期／独立確認を受けて最終 binding を更新する。旧 snapshot の検査記録は保全する。

## 最初の新 guest 結果と extent の切分け

- 主担当が最新正規媒体で新条件を実行した記録を受領し、子も `build/browser-performance-scroll-guest-20261013/test-serial.log` の該当出力を読んだ。`scroll-input` は48条件中1失敗（`private absolute overflow does not inflate root document extent`）。その他47条件は同 snapshot で成功。既存 paintboundaries の prefix 訂正確認は17/0成功。子自身が VM を再実行した結果ではない。
- 唯一の失敗期待値は子が置いた `doc_h==200`。ただし既存 `layout_inner_used` の初期包含ブロックは viewport 高さを子の percentage-height reference に使い、自身の auto 高さは normal-flow content 高さになる（3348付近のコメント、child_cbh 設定）。40px flow の文書に200px最低文書高さを求めた根拠がない。
- 値を持たない元失敗を隠さず、`--scroll-extent` に失敗した同条件と abs-free40px control だけを分離した。doc/root/private/CB の実数値を出し、rootが350へ漏れているか、40pxの正しい短文書に対する fixture の誤前提かを切り分ける。旧47条件・paintboundariesは繰り返さない。専用分岐の syntax 成功、**guest数値はまだ未受領**。製品ソースはこの切分けでは変更しない。

## 専用対照の実 guest 結果と期待値訂正（2026-10-13）

- 主担当から専用分岐の4/0成功を受領し、子も `build/browser-performance-extent-guest-20261013/test-serial.log:65-67` を読んだ。control は `doc=40/root_h=40/root_scroll=40/viewport=200`。private 条件も `doc=40/root_h=40/root_scroll=40/clip_h=40/private=350/abs_y=300/abs_h=50/cb_is_clip=1`。
- 製品は private350px overflow を40px scrollport内に保持し、root はabs無しcontrolと同じ40pxだった。元48条件の1失敗は、子が書いた「viewportをroot最低高さとする」fixture期待値の不備と確定した。`--scroll-input` の当該期待だけを200から40へ訂正し、通常auto高さと専用対照を根拠としてコメントを付けた。製品ソースや他47条件・受入基準は変更しない。旧FAIL記録・画像・logは保全し、旧47条件も再実行していない。
- 主担当は最新実 Shaka の `scroll-inner-down.png` を目視し、card全体とfooterがviewport下まで表示され、旧約603pxの一斉切断が解消したと報告。これは主担当の観測報告であり、この更新時点では子は新画像をまだ開いていない。上へ戻る PageUp の結果は未受領。専用guest成功と実GUIの目視・操作・性能比較を混同しない。
