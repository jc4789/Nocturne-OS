# Cascade layer／文字省略：第一修理報

2026-10-13。親の GitHub 回復画面は表の重なりが消えた一方、About の見出しが24pxだった。原 `build/compat-github.html` には `data-variant=small`、原 `build/compat-github-primer-react.css` は small の `font:var(...,600 1rem/1.5 ...)` を持つ。既存 font parser をそのまま使う host 二段計算は原fallbackとslash空白条件10件成功だった。

原因は原 primer CSS の `@layer primer-css-base` 内の h2 24px と、原 React CSS の `@layer primer-react` 内の specificity 0 の `:where` 宣言を、既存 parser が透明groupとして平坦化していたこと。固定字号・字体やサイト名分岐を入れず、[CSS Cascade 5](https://www.w3.org/TR/css-cascade-5/#layering) の層優先を既存層へ接続した。

- 共有／adopted ASTは名前、親、syntax identityとencounterイベントだけを保持。順位と対応表は文書・style scope別の `css_ctx` 所有。
- 名前付き／匿名／nested／dotted、statementの先行順、同名再開を保持。通常は後の層、importantは逆順。implicit unlayered／parent direct、inline、encapsulation、animationの順位を区別。
- import metadataとdoc-owned sheet instance topologyにより、statement→import→parentのencounter順を保持。逆順HTTP応答を層順にしない。`layer()`／anonymous `layer`、supports／mediaを扱い、文書全体条件とelement-sensitive条件を分離。
- `revert-layer`／`revert` を選択候補のrollbackとして処理し、shorthandは既存 longhand expansionを使う。`all` のCSS-wide展開はdirection／unicode-bidi／custom propertiesを除外する。通常の宣言はrollback配列の初期化・mask走査を通らない。

先行child-list guestは `build/browser-compat-integrated-guest-20261013/test-serial.log` の10件中1失敗。`.empty:not(:empty)` のargumentをuniversal subjectとして独立登録したため、無関係な親まで候補になっていた。rightmost argumentは同一subjectなのでouter stable keysへ畳込み、complex alternativeの左compoundだけ独立登録する修理をした。旧期待値と原ログは維持し、新 `--child-list-locality` は旧失敗と同じHTML／通常tickの1assertだけ。

追加依頼のtext-overflowは親と[CSS Overflow 3](https://www.w3.org/TR/css-overflow-3/#text-overflow)の安定定義を照合し、一値 `clip|ellipsis` に限定した。非継承 `uint8_t` field／共通keyword parser／computed serializerへ接続。親がline retentionと実fontを使う描画本体を担当するため、field変化はlayout比較から除外しない。Level4二値／stringをstubで広告していない。

## 実行範囲

製品Cと新native fixtureのhost syntax、CRLF-aware diff checkは成功。text-overflow parser／default／明示inherit／invalid保持40件成功：`build/compat-text-overflow-parser-fixed-20261013.log`。最初のhost抽出build失敗はrollback helperまで不要に拾った基盤不備で、原 `build/compat-text-overflow-parser-20261013.log` に保全。抽出境界修理後の結果と分離する。

新 native `--layers` 14件、`--layers-import` 5件、`--child-list-locality` 1件は親の最新媒体での実行待ち。原WPT layers／important／inline／media、GitHub見出しの実画像、text-overflow描画／GUI入力と同条件性能は未確認。CSSOM layer専用class／insertRule整序、font-face／keyframes同名overrideもこの修理だけの成功に含めない。
