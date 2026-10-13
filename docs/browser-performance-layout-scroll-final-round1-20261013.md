# レイアウト・内部スクロール 最終参照 第1報（2026-10-13）

主担当の製品／試験ソース最終凍結後に、現在の型・所有権・描画失効・入力接続・生成元／生成物を再読して本資料を追加した。旧証拠は変更しない。今回は資料作成と静的再読だけであり、build・生成・guest・VM・旧検査の再実行はしていない。J-space report 登録は他担当資料を含めた主担当の最終 map 同期後に行う。

## 現在の全 material source

以下は今回読んだ実装・共有型・依存・生成・試験の対象ファイル全体を SHA256 で識別する。本文の再読は関連する責務境界に限定し、巨大な生成物全文を出力して切り捨ててはいない。生成物では private hook の実収録を確認した。

| 対象 | SHA256 |
|---|---|
| user/libc/web/layout.c | 34827fd363993a433afacb6b36f2e371d68969bf160b336d0df8588fd95e741e |
| user/libc/web/layout_text_cache.h | 1f87188fd92f3cbc0fe5fc368fc4394ede84bf3348ad3ecb905c30c41af6443b |
| user/libc/web/webi.h | 36b653fe38397e984f5af53a16f5abbc108b98484f2a9623bb1ddf93c3243aac |
| user/libc/web/doc.c | a257127b6f61f8dee79abd82cc346d3033e7c96f2a699742d44caea2f6391459 |
| user/libc/web/css.c | 26783ec36296fedfc3fab00f16c73106a637d157570c6ff97f012c8ae423c479 |
| user/libc/web/box.c | 40563900e7695ce10879a7b6e5e50740a0a137fd6ef6f951bd731a35fe9cd735 |
| user/libc/font.c | 0c90e8e303e3d4f545a66eb522b8e66918bed93b36cd09e7b090de62fd0cd91c |
| user/include/font.h | 94c88877d72e3676554fc925a2e1e004771e97707ffa1d7fa023080a8c2bee6d |
| user/include/web.h | ebe3acefcc7fcb67b00026e015456a4aeb59f07d8ab4975aeec1f0e9401741c7 |
| user/libc/web/js.c | 4809829623c37f74d81f0fba8558bb757f3a2406873c48d7baab5357b1070846 |
| user/libc/web/js_html_elements.js | 48071d97e48986722a6ec763a7405d9fe9dfd5b17902ba9f637b7d06e999a091 |
| user/libc/web/js_bootstrap.js | bab25fd89ba26f6c5be08c6259b3bea416e56191934d476202ca87176ccf286b |
| user/libc/web/js_bootstrap.inc | 3fca1ec4a135d007633b3664ad95e4bce1be7bd5fc638535ecebd2119657d65a |
| user/libc/web/js_embed.py | a3b42e8c197fdc520636f14e67cba699c0d29b953c8239e4dcb9cfaa32df73eb |
| user/libc/web/frame.c | 4f6cd2215edeb35244df3de69ade3a28302b6716cb883659d08b0cb915f59a31 |
| user/libc/web/frame.h | 0119e279bfcad483f5fb18587270910217d73270cde0ec9b817c592a9af170e6 |
| user/apps/browser.c | a0adbc51ea621f673548a2f880e8ed5b828f8b3faa434283e92995339908eda1 |
| user/libc/web/paint.c | 64cbda88b67dac92b75f0d602676ffcc70c5cd21b9a4f6410c06740565dd9fc9 |
| user/libc/webnet.c | 8c55823434826ee09f285daf44660e9cb9e045ee6a0ad000a35cbb8ad7af5007 |
| tests/paintperformancetest.c | 0c845ffdd62dcd2be11511f402a5c9c65cc52bcd3a7e97aeaaf68795afb273db |

## 現在の実装の接続

- layout は dirty 枝の prepare／intrinsic／inner／relative／overflow 再用、actual containing block の相対百分率、absolute 登録が必要な祖先の unsafe、run-owned descent、取消後 post 非公開を保持する。root extent は通常下端と root の伝播 overflow を合成し、内部 clip を無視した absolute 別 union はしない。
- 文書所有文字 cache は所有文字・font世代・字体選択の serialized 値・spacing で照合し、字体尾部を値共有/refcount、LRU、OOM rollback、font失効、再入、文書破棄で回収する。CSS current／previous slot と lazy tail、orphan pool の最終 slot 回収、box borrow 更新が同じ現 `webi.h` と共存する。
- `web_scroll_input` は committed layout／接続／slot snapshot／frame 世代と viewport を先検証し、native offset と pending 通知だけを変更する。closest auto/scroll → child viewport → frame 親要素 → root 残 delta の順。hidden 除外、CSSOM hidden 保全、Home/End 内部消費を保持する。
- element 通知の全 batch を private dispatch link へ切り離してから通常 tick に既存 WeakMap/postTask を呼び、再入力は別 FIFO に残す。owner／接続／alive 再確認、realm active guard、deadline、通常 task と viewport/element の公平性を保持する。
- current pending owner の清掃だけでなく allocation-owned node の破棄前にも queue unlink を呼び、sibling adoption 後の破棄順を保全する。
- `doc_paint_full` は owner と frame 親全体へ full damage を伝播する。native/CSSOM scroll、child viewport、画像完了、frame child の描画更新は先に known rectangle があっても古い小矩形へ縮退しない。CSSOM child viewport は view position と visual revision も更新する。
- browser の key/wheel/待機中入力は native API と残 delta を使用し、root 続行の加算を64bitで clampする。待機／active script 中は hit getter の再入を避ける。partial 描画/present と video copy/present の debug 集計は full 計数と分離されている。
- private `queueNativeElementScroll` は生成元と正規 `js_bootstrap.inc` の両方に収録されている。生成物を手編集したものではない。

## 証拠の分類

- 以前の layout 初期79件・追加28件、scheduler追加26件は各旧 snapshot の限定 host 証拠として元資料を保全する。現在の全 source の全面合格へ昇格しない。通信 source hash は追加26件時と同一で、origin cap は初回 request origin に限定される。
- 新 native scroll は元48条件中47成功／1失敗。唯一の失敗は子が置いた `doc_h==200` という viewport 最低高さの誤前提だった。元 FAIL を残し、abs無し40px flow control と同 clip 条件の専用4/0 guest 対照で root40/private350/absCBclip を確認。現在の fixture は根拠に従って当該期待だけ40へ訂正した。訂正後の全48は再実行しておらず、47成功＋誤期待の訂正＋専用4成功と分類する。
- 子は両 guest serial の該当出力を読んだ。guest を実行したのは主担当。prefix 訂正後 paintboundaries17/0、最新 network callback 境界0失敗も主担当の guest 証拠と区別する。
- 主担当は実 Shaka の PageDown で card全体／footerまで表示し、PageUp 後の最終画像で動画へ戻ったことを目視した。子が見たのは元切断の初期／下移動画像で、最新画像のこの受入は主担当観測として明記する。実動画の全操作・全体性能・音質・Hyper-V の成功には拡張しない。
- 現全VM/server/build終了、今回一時299 files回収と最新媒体／一baseline／画像・log保全は主担当の終了記録。削除した媒体をそのまま再実行可能な保存物として扱わない。

関連: `docs/browser-performance-scroll-input-20261012.md`、`docs/browser-performance-scroll-review-20261013.md`、`docs/browser-performance-root-review-20261013.md`、`docs/browser-performance-completion-20261013.md`。第2報ではこの現参照を使って失効・通知とallocation所有・生成接続の反例を再考する。
