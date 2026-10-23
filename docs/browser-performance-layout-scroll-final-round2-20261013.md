# レイアウト・内部スクロール 最終参照 第2報（2026-10-13）

第1報作成後、同じ現在の material source を使って、旧最終報には含まれていなかった内部／frame入力・通知borrower・unknown damageと生成接続を再考した。製品修正、検査・build・生成・VM再実行はしていない。material source の全対象と SHA256 は `docs/browser-performance-layout-scroll-final-round1-20261013.md` の表に対応する。

## 反例境界の再考

1. **root 高さと private absolute overflow。** 文書は viewport 最低高さではなく normal-flow auto 高さを返す。root下端とroot scrolling overflowのunionは、clip内absoluteのprivate範囲を独立に加算しない。一方、非clip absoluteとclip外CBへescapeするabsoluteは実CBへpublishされる。元48中1失敗の `doc_h==200` を製品都合で受入弱化したのではなく、abs-free control と同 private 条件の数値4/0対照が双方root40/private350を証明したため、誤前提を厳密な40へ訂正した。他47条件は再実行していない。
2. **dirty と subtree reuse。** prepareはdirty無し、innerは幅/cbh/usedh/edge/font世代、postはinner有効と幅/高さ/content_dyを要求する。相対自身のactualCB更新をsubtree早期returnより前に行う。absolute再登録のunsafeは祖先へ伝播し、float exportをsharedBFC再用から除外する。新native入力はroot post有効／dirty無し、pending style/font/shadow無し、取消／回復／layout stack無しを要求し、作者実行中にlayout/Shadow反応を同期flushしない。
3. **child と root の文書世代。** targetのownerから呼出元まで、frame containerの現在document、親、element、接続、実viewportと現box寸法を先検証してからoffsetをcommitする。順序はchild element→child viewport→frame親element→root残delta。native入力はcross-originでも作者権限を借りない。Home/Endは最寄り縦overflowを消費し、既に端でもrootを動かさない。root残deltaの加算はbrowser側64bit clamp。fractional位置では残整数の丸め限界を製品コメントと限定fixtureで明記している。
4. **作者callback中の再入力。** node FIFOを全件独立のdispatch linkへ移してpending owner/nextを消してからJSへ渡す。後続batch nodeのadopt／再queueもcurrent FIFO linkを使い、既存batchを書き換えない。各notify前にowner/root/aliveを再確認する。callback中の新native inputは次tickに残り、native通知と通常postTask・viewportとelementは交互に進む。active親／兄弟realm中の配送をguardし、pending sourceはdeadlineに反映する。
5. **allocation-owner-firstの破棄。** 文書自分のFIFO清掃だけでは、別siblingにadoptされたallocation-owned nodeが後続sibling queueに残る。現実装はowned_nodes解放前とlegacy解放経路でpending owner FIFOからunlinkし、逆の破棄順ではowner文書のFIFO清掃がflagを先に消す。helper／siblingのnativearena寿命とcurrent DOM ownerを混同しない。新native fixtureは実sibling adoption/pending/通常root破棄を通ったが、非crashだけを任意callback中の全寿命保証へ拡張しない。既存frame retirementはauthorからのnavigation/detachを即arena破棄せず保留する責務を維持する。
6. **known rectangle→unknown visual change。** CSSOM/native elementとchild viewportは `doc_paint_full` でownerとframe親全てをfull dirtyにする。frame tickのchild paint伝播も同helperを使用する。先に動画矩形がpendingでもoldrectだけをconsumeしない。CSSOM child viewportはview position／visual revisionも更新し、native viewport eventだけをpending flagから通常tickへ送る。既存JS programmatic dedup/postTaskの責務をnative通知へ無理に置換していない。
7. **style retirementとowned測定キー。** node current/previous固定slotはtail arenaと別に退役し、孤立したcascade owner poolは最終slot返却まで保持する。box borrowを交換してから旧tailを回収する。文字cacheはCSS pointer/hashだけで同一視せず、所有文字・serialized選択値/range・世代・spacingを比較し、共有尾部refsをOOM/LRU/freeで戻す。web_freeはcacheをfont resourceより先に解放し、font_closeもbackend世代を進めるため、別文書で同addressを再利用しても古いmeasurementをvalidにしない。
8. **生成元・生成物と計数。** private queue hookは `js_html_elements.js` のexport→`js_bootstrap.js` のhook→現 `js_bootstrap.inc` に接続する。`js_embed.py` は正規include展開とUTF-8生成を所有し、子は生成物を手編集していない。browserのpartial draw/presentとvideo copy/presentのdebug計数が別々になっていることを読んだが、その存在を全体速度改善の実測へ昇格しない。

## 判定と残る範囲

この現在の型・失効・所有権・生成境界の再読では、新しい未修理の反例は見つからなかった。元失敗と対照、限定構文とhost、主担当guest、主担当の実Shaka下／上スクロール目視は第1報の分類を保つ。全48再実行による全面PASS、全site速度向上、WPT全適合、実動画全操作、音質、Hyper-V成功とはしない。

旧2026-10-12最終roundpairは旧型への参照として保全し、この新roundpairがdoc/header/JS/frame/browser/paint/現style/font測定cacheへのfresh binding資料となる。現在は資料作成まで。J-space登録は全担当資料を含めた主担当の最終map同期合図を待つ。
