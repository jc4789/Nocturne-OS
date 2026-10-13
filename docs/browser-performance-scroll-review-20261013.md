# スクロール修理の独立静的確認

2026-10-13。担当 `style_perf`。製品・fixtureは主担当／layout担当の所有で、この確認では変更していない。以前のCSS・paintレビュー記録も変更していない。

## 発見した境界と修理の再確認

- **子frameの入力漏れ**：`web_node_at`は子文書のnodeを返す。最初のroot owner限定APIでは、そのdeltaが親viewportへ漏れた。最新の`web_scroll_input`は接続済みframe/container/owner・実viewport寸法・全文書のcommit状態を変更前に検証し、子element→子viewport→frame祖先→root残deltaへ渡す。別originでもnative入力だけを移送し、JS wrapperの権限を貸していない。
- **既知damageの後のスクロール**：video矩形damageの後にscrollが発生すると、その矩形だけの再paintに誤縮小できた。共通`doc_paint_full`でnative/CSSOM element、子viewport、画像完了、marquee、子frame paintのownerとframe祖先を全面damageに昇格する修理を再読した。
- **確保元と現在ownerの差**：兄弟frame Aで確保したnodeをBへadoptしてBの通知をpendingにしたまま、Aから破棄するとB queueが解放済みnodeを読む可能性があった。最新の`element_scroll_input_unqueue`は、enqueue移送とallocation-owned nodeの破棄前（`owned_nodes`／`free_values`）で他owner listからも外す。A→B、B→Aいずれの破棄順でもpending borrowerを残さない。
- root残deltaのsigned int加算は、最新browser実装が`int64_t`加算→実root scroll limitへclamp→int化するため、合法の大きな文書で末尾から先頭へoverflowしない。

## 再入・イベント・焦点

- native commitはlayoutやauthor JSを呼ばない。未commit layout、dirty style／shadow assignment、native recovery／layout処理中はdeltaを保持したまま拒否する。
- 通知batchのFIFO linkを全て外してから専用dispatch linkへ移し、各hook前に現在ownerと接続rootを再確認する。callback中の新入力・adoptionは新FIFOへ入り、古いbatchの次nodeを上書きしない。
- 通常tickはactiveな同realm・親／兄弟runtimeへ通知を再入しない。既存の`queueElementScroll`のWeakMap重複排除・trusted nonbubbling event・`postTask`を再利用し、入力sourceとposted taskを交互に実行する。pending通知はdeadlineにも反映される。
- viewport通知はdispatch前にpendingを消すため、callback中のscrollは後続tickへ残る。disabled realmはJSを呼ばずborrowを解除する。
- browserの通常wheelはauthorの取消を尊重した後でnative既定動作を行う。編集・select・activation処理はscroll keyより先で、既存textbox／textareaのカーソルキー処理を迂回していない。native waitではauthor callbackとhit-tree構築を避け、直前のhover targetをcoherence検証して利用する。
- sourceframeのdetach／navigationは`web_frame_retire`がnative arenaを親の破棄まで保持する。active author中にnative arenaを同期破棄しない既存UI所有保証は必要であり、任意の外部callerによる同期`web_free`まで安全と証明したものではない。

## 実行範囲と残件

- 追加の`--scroll-input`fixtureを読んだ。実layout、trusted event、callback中のnative入力、shadow assignment、detach／adoption、damage順序、子viewport、兄弟frameのallocation-owner先破棄を対象とし、既存14／17条件を再実行する入口ではない。
- この担当はVM・全build・guest試験を実行していない。主担当の新guest結果とShaka実マウス／キー操作は別の受入証拠で、ここでは未受領。spinner／controlsと実カード見切れの解消を静的確認で合格扱いしない。
- 通常`git diff --check`は既存CRLFをtrailing whitespaceとして誤報した。`git -c core.whitespace=cr-at-eol diff --check`の対象差分確認は終了値0。これを動作確認へ昇格しない。

## 読了時のsource内容hash

Git blob hash。後続編集には流用しない。

|対象|hash|
|---|---|
|`user/libc/web/doc.c`|`bbeb26ef73768df122dd6c57971c47bbd61d9dee`|
|`user/libc/web/webi.h`|`56ee0a9d493269ec7de21bbe37164c0e3e1db54d`|
|`user/libc/web/js.c`|`4f61c58e3caf6194941d8d90789cbc2de57ea4f9`|
|`user/libc/web/js_bootstrap.js`|`0f15295d4906235a048cd5409bab1de4134305fe`|
|`user/libc/web/js_html_elements.js`|`92f5b2506e9ea34e1be4f7f1dc230bd6fc7433a0`|
|`user/libc/web/layout.c`|`fbcca02f3eebbd9cba7441e658fc49903d0ac0d3`|
|`user/libc/web/frame.c`|`979b13d6d1ba295db7eef464f8ed4ee68c344023`|
|`user/libc/web/marquee.c`|`85af7823b42f682e380851498a9ab19395f4858f`|
|`user/apps/browser.c`|`9d79a1386fff2a92399ecaa454a2b64c3e7284b9`|
|`tests/paintperformancetest.c`|`ca8a7d83021931f7d06529a4bc1cd45bba49942b`|
