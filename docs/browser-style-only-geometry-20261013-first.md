# style-only 更新後の geometry 再使用・第一報

- 原失敗を保全：`build/browser-pseudo-body-native-20261013/test-serial.log` の新 pseudo-bodies 原 12 条件は、nongenerated 疑似色変更後の geometry 維持だけ失敗した。原 paint 12 と CSSOM 経路の成功でこの失敗を相殺しない。
- 原因：通常 div の class 更新は文書 layout を保守的に無効にする。cascade と box borrow 更新では生成しない body の色差を geometry に含めていないが、`web_layout` がその後無条件に layout_doc と layout_revision++ を実行していた。
- 製品修理：cascade/resource/font/borrow 同期を済ませた後、root の post-layout と cache が有効、root dirty なし、font 世代一致、post 寸法一致、viewport 同一、recovery 中でない場合だけ geometry を再使用する。文書・box の dirty を消して通す処理ではない。
- 再使用は通常の committed 出口へ合流する。子 frame viewport commit、find reveal と通常終了処理を飛ばさない。container 入力は既存 committed geometry のまま維持する。
- box restyle 対象の paint-layer cache を無効化し、visual 世代を更新する。DOM 世代の動かない native effect でも hit/paint を古い内容のまま再使用しない。
- 原 native 12 の期待を維持し、実 width/viewport/新生成 box の geometry 更新、生成した疑似背景・font の取得復活の 5 条件を追加した（合計 17）。まだ実行しておらず、主担当の修理後媒体確認を待つ。
- target syntax と diff check は成功。変更していない host 24 条件を再度走らせることはせず、今回変更した native 境界の確認へ進める。
- source は `doc.c` と `box.c` の既存責務内を修理し、試験は既存 `csscachetest.c --pseudo-bodies` に統合。source を凍結して主担当に引き渡した。
