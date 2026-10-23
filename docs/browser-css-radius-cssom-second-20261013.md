# 丸画像・CSSOM 修理の第二考察

第一記録は `docs/browser-css-radius-cssom-guest-repair-20261013.md`。元 guest の画像 1/13 失敗と computed 14/14 失敗を保全し、独立な consumer/IDL/所有境界で再考察した。

- **host 成功の盲点:** 前回 host の「image alpha blit」は gfx 関数を直接呼んでいたため、製品 image_draw が独自 direct row を使う事実を検出できなかった。今回は exact production image_draw 本体を取り込み、同じ retained mask の corner 白/center 青を assert。18 件 / 0 失敗はこの consumer を含むが、decoder/native request は元 native fixture の同じ厳しい assert で別に再確認する。旧 fixture の期待を弱めず、根拠のない shader/OS 成功に昇格しない。
- **fake `has` の反例:** computed の property presence を Proxy の常時 true で直す案は採らない。native cssprop.c の table 名を取り出し、実 supported 属性に accessor を作る。`nocturne-unimplemented-property in style` が false という作者反例を追加。CSS.supports、style getter、computed getter は元の実 parser/serializer を使い、固定値を新たに返さない。
- **read-only と live の両立:** plain object を返すまま名前だけ追加すると setter が expando を書き、後続 read が native 値ではなくなる。private WeakMap brand の owner node を変えず、CSS named setter と set/remove/cssText は readonly 例外。保持済み computed object に対して作者が inline radius/custom を変更したあとも現在 native 値を読む。この仕様を 24 assert の別 native mode に固定した。
- **列挙の整合:** property table の ASCII 順だけだと vendor prefix が先頭に来る。computed standard longhand は初期 1 回 vendor-last lexicographic 順へ整理する。custom declarations は current style の局所リストで、先行 invalid local override を seen に記録して古い inherited value を誤列挙しない。値取得も同じ最初の current entry を使う。全 DOM/全 CSS 走査や新しい固定ノード上限は追加しない。
- **generated source の反例:** source 最終 tiny 修理と別担当の js_embed 完了 message が交差し、実 inc に古い sort/継承/custom list が残った。message を信用して済ませず actual read で drift を確認し、単 writer の正規再生成後に source/inc の 3 境界一致を私自身が確認した。

root の nav button flex 実装・Window named access・font の opaque color/data CSS 修理を保全して統合した。glyph/media mask とは別の image consumer 失敗を本当に修理したが、mask 有りの実サイトコスト、新画像の見た目・操作、computed 原 WPT の値比較成功はまだ未確認。旧 passing parser 34 件と shadow damage 6 件を新成功と取り違えない。次は失敗した native radius / 原 computed 14 件と新 24 assert だけをまとめて製品媒体で確認し、指定サイト原画へ戻る。
