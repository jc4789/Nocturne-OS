# CSS担当・最終引渡し第二考察

2026-10-13。第一報`docs/browser-performance-style-delivery-first-20261013.md`とは別artifact。製品／fixtureは凍結のまま、新しい試験をせず、確保元と現在owner、未公開cascade、継承tail、生成JSとpaint／scrollの所有境界を再考察した。旧reportevidenceは変更していない。

## CSSの所有を現在の共有文書経路に結び直す

1. slotは確保poolを自身に保持し、nodeのcurrent／previous chainで管理される。adoption後のcascade owner変更をallocation owner変更と同一視しない。`css_style_doc_free`は文書所有だけを外し、live slotがあればorphan poolを残す。最後のslot返却でpool／slabを回収する。
2. `retire_style`は旧variable arenaと旧slot chainを一緒にpreviousへ移す。worker join後、topology維持時は`boxes_restyle`で借用pointerを交換してから、変更時は`boxes_discard`で借用を外してから`css_styles_release`する。別世代のtailだけを先に解放しない。
3. native cancelはbox discardとstyle unpublishの後で退役世代を回収する。部分確保されたcurrent slotは次の再cascade／node破棄で返す。過去追加11／0のOOM／未公開slot／pseudo chain条件と現在の接続に矛盾は見つからないが、今回その検査を再実行したものではない。
4. AST cacheの所有snapshotを交換しても、computed custom-property名／値はtailの所有コピーなので、子／pseudoが継承している旧親tailはjoinまで有効である。可変尾部がなくても固定bodyはnode専用4KiB arenaを持たない。
5. 最新`web_free`のpending notification cleanupはCSS pool destructorと責務を混ぜていない。allocation-owned node破棄前に別ownerのintrusive queueをunlinkし、その後style slotを返す。兄弟frame確保元先破棄の反例は、root queueだけを先にclearするhelper文書ケースと区別して修理されている。

## 独立paint／scroll査読の最終再考察

- stable layer sortの同順位は左側優先。cached orderはlayout／DOM世代に結び、全contextの準備をcanvas書込み前に行う。typed layer／BVH sortとbounds／partitionには中断点があり、途中indexはvalid公開されない。box破棄はowned layer vector／文書index／order validを回収する。汎用raster layer cache全体を実装したとは解釈しない。
- targetは原walkから集めるprimitiveのpaint order／clipによって選ぶ。ordinary LI outside markerのindex-only targetを除去し、summary/default-detailsの従来targetを維持する。generated run、透明overlay、dialog fallbackを以前の限定fixture／source査読の範囲で保持する。
- native scrollはcommit状態の全文書／frame chainを先に検証する。active作者JSの間にlayoutやShadow DOM flushを再入せず、通常tickで既存trusted scroll postTaskへ渡す。batchとnew-input FIFOのlinkを分離し、採用／detach後のownerとconnected rootを通知前に再確認する。
- known rectの後のscroll／marquee／子frame visual updateは`doc_paint_full`で全面damageへ格上げする。旧video矩形へ誤縮小してscrollportの外を残す反例を防ぐ。root残deltaの64bit加算と実limitへのclampは巨大文書のsigned overflowを避ける。
- `js_bootstrap.inc`のexport／hookも再読した。現`webi.h`／`doc.c`／`js.c`／generated JSを含むmaterial sourceにこの第一／第二報を同時にbindする。第一報のhash表が今回の内容snapshotであり、旧共有header／doc hashへ流用しない。

## 失敗と証拠の再解釈を禁止する

- style150／0は旧第一統合snapshotの結果である。現CSS本文が同hashでも、現在のshared struct・全helper・全siteをその結果だけで動作確認済みとはしない。
- 最新paint17／0の総括PASSは、元prefix FAILを消すのではなく別媒体の追加証拠である。
- scroll47成功／1誤期待と専用extent対照4／0は別結果。現在の40px期待はabsなしflowとの対照で訂正されたが、48件全体を再実行してPASSにしたものではない。
- WPT原本の5成功／1失敗を保持する。未定義`container`はhit assertion前の失敗なので、そのケースのhit機能は未検証である。「製品のhitが誤った」とも「hitが合格した」とも読み替えない。
- 過去自担当の目視診断と、最終実Shaka下／上移動の主担当観測は別の証拠。spinner／controls、YouTube遷移後の旧題名、長いnative／JS処理、未実施動画操作や音質を、限定CSS／paint成功で相殺しない。

凍結sourceを読んだ範囲では追加の未修理反例は発見していない。これは無回帰・全体速度・全適合の保証ではない。artifact作成後は主担当のmap同期／登録合図を待ち、新しい試験や製品修正は行わない。
