# 角丸描画コストと背景画像関連付けの製品修理

## 対象と原症状

最新の依頼は、追加機能より性能回帰を先に扱い、百度百科で画像が一度表示された後に消え、マウス移動・スクロールで戻る回帰を修理するもの。主担当の同条件 QEMU／媒体管理に従い、この担当は新しい VM を起動していない。

私自身がユーザー原画像 `C:/Users/cesta/AppData/Local/Temp/codex-clipboard-86489425-082f-4fca-9a7e-664dd4faffca.png` と `C:/Users/cesta/AppData/Local/Temp/codex-clipboard-e04bf386-ccac-4bae-a44f-5dad297d20b0.png` を見た。前者では三つの主画像が祖先と同じ淡い多角形の模様になり、統計・青いボタンも欠ける。後者ではスクロール後に写真と青いボタンが戻る。主担当の後の旧媒体観察では消失を再現しなかったが、原症状を否定する根拠にはしない。

公開原 CSS／JS は `build/compat-baike-home-regression.{html,css,js}` に保存。主画像は `background-image` のブロックで、画像自身に角丸はなく、作者の動的 style は画像 URL を設定する。取得資料を製品へ注入したり、サイト名で分岐したりしていない。

## 製品修理

### 角丸マスクの不透明内部

`common/gfx.{h,c}` に行ごとの clip 境界と不透明区間の共有処理を統合した。`paint.c` の既存 radius cache は各行の A8 が実際に全て 255 である区間を、同じ所有者・寿命で保持する。祖先全ての境界とこの区間を一行一回で交差させ、角の部分被覆だけ従来の正確なマスク連鎖を使う。

gfx の fill／blend／blit、製品 `image_draw`、glyph、Web canvas、媒体 scaler の既存 consumer を接続。不透明内部では従来の直接行処理・memcpy・SSE2 を保ち、媒体の Q32 サンプリングやアルファ合成を変えない。未知の任意マスクには不透明区間を仮定しない。canvas 型本体は変えず、借用 mask descriptor の metadata を追加したため全 ABI ビルドが必要。

### 非継承画像の native handle

`css_style_init` は親の POD をコピー後、非継承プロパティを初期値へ戻す。旧 `copy_prop` は background／mask の文字列を戻す一方で native の `bg_img`／`mask_img` を戻さず、親の登録済み handle を残した。`doc.c` は handle が非零なら新 URL の登録を行わないため、部分 restyle で異なる画像の handle が結び付く製品不備が成立する。

`cssprop.c` で表現と handle を同時にコピーする。非継承初期値では零、明示的 inherit では対応する親 handle を保つ。URL 変更・none・gradient では旧 handle と競合する表現を無効化する。画像 cache を撤去して症状を隠したり、通信を毎 paint 再実行したりしない。

## 限定確認

- `build/css-compat-mask-span-correctness.log`: 製品 radius／image／gfx を使う host 20 件、0 失敗。既存の corner／center 条件を保ち、新しい不透明区間の pixel 一致を追加。
- `build/css-compat-mask-span-host-performance.log`: 同一 host、1920×1080、8 frame、4 祖先マスク。任意マスク fallback 228 ms と不透明区間 9 ms、全 pixel 一致、3 件、0 失敗。これは描画 microbenchmark であり OS／サイトの高速化結果ではない。
- `build/css-compat-mask-media-host.log`: 変更した実媒体 scaler のクリップ・resize／同寸法・不透明区間と fallback の比較を含む 76759 件、0 失敗。
- `build/css-compat-image-association-host.log`: 非継承・inherit・URL／none／gradient の handle 整合 9 件、0 失敗。
- 変更した製品 consumer／native fixture の freestanding 構文と CRLF 差分検査は成功。先の host 媒体試験の launcher 引数・header／link 不備は訂正してから成功し、失敗を製品成功に混ぜていない。

新 `imageassociation/partial-hover-background-mask-and-scroll` は、正常な native parser／resource／hover／layout／paint 経路で色の異なる SVG 背景を使用する 8 件。hover と scroll を経た画像 handle／pixel を厳密に確認する。主担当の次媒体で実行待ちであり、host 結果を guest 成功へ昇格しない。

## 受入境界

この担当の製品変更は凍結。主担当の flex 再配分・kernel 通信 pool・他担当の CSS／DOM 変更を保全。最新全媒体の同条件実サイト表示・hover 消失・実スクロール／入力応答・frame 時間は未確認。次は主担当の fresh build と実画面／関連 native 失敗だけを確認し、未変更の古い合格群を反復しない。
