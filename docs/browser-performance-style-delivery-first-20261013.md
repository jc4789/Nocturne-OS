# CSS担当・最終引渡し第一報

2026-10-13、担当`style_perf`。最終製品／fixture凍結後、現在の共有型、文書破棄、生成済みJS、paint／scroll接続を読み直した新artifactである。既存の第一／第二報、guest失敗、独立査読evidenceは変更していない。今回は文書作成とread-only確認だけで、新しい試験・build・VM・serverを実行していない。J-spaceの最終登録は主担当のmap同期合図後に行う。

## 最終実装への接続

- computed `style_t`固定本体は64-slot文書slab、variable tailだけ512-byte初期chunkのlazy arena。current／previous chainにpseudo／underlying／backdropも含め、既存cascade join後のborrow交換まで退役世代を保持する。
- immutable inline／WAAPI ASTは所有bytes・length・baseを照合し、同pointerで内容が変わる場合も失効する。computed custom-property名／値はtailへ所有コピーし、AST更新と継承borrowの寿命を分離する。native CSS keyframeの既存sheet ASTは維持し、値が実際に変わるsampleの再解析までゼロとは主張しない。
- 現在の`web_free`はpending通知を外し、box／measurement cache／retired style borrow、frame／JS／helper document、allocation-owned nodeの順に既存所有を処理し、その後`css_style_doc_free`で文書pool所有を外す。adoptされたslotが残ればorphan poolは最後の返却まで存続する。
- 独立paint査読の通常LI marker差、未中断sort、未完成cache公開は修理済みsourceを再読した。stable同順位順序、box-owned layer vector解放、generation付きBVH、dialog／fullscreen exact fallbackを保持している。
- 独立scroll査読のchild owner違い、known damageの後のunknown visual、兄弟frameの確保元違いpending borrower、root int加算overflowも最新sourceで修理を確認した。詳細は`docs/browser-performance-scroll-review-20261013.md`。この文書の古いfixture hashは履歴として保持し、現在のfixtureは下表へ結び直す。
- `js_bootstrap.js`の`queueNativeElementScroll`と`js_html_elements.js`の既存`queueElementScroll`exportが、現在の生成物`js_bootstrap.inc`にも存在する。Makefileの正規`js_embed.py`依存と`js.c`の生成物includeを確認した。生成物全体の再生成／全byte照合をこの担当が今回実行したという意味ではない。

## 実行証拠を分離した最終状態

|証拠|結果／扱い|
|---|---|
|自担当の過去host CSS確認|初回22／0、別の追加境界11／0。再実行していない。4096 plain nodeでtail未確保、固定body保持量5,724,680 bytesは限定fixtureの測定で実サイト速度ではない。|
|第一統合guest|`build/browser-performance-guest-20261012/test-serial.log`のstyle150／0、paint14／0、共有25／0。全体14成功／2失敗も保持し、現在の全面PASSへ流用しない。|
|paint追加境界の元guest|`build/browser-performance-boundaries-guest-20261012/test-serial.log`実17／0とprefix総括FAILの両方を保持。|
|最新paint追加境界|`build/browser-performance-scroll-guest-20261013/test-serial.log:64-65`のprefix訂正後17／0、総括PASSを今回読んだ。旧報の「未再実行」は当時の事実であり、旧artifactは書き換えない。|
|新scroll入力|同log:66-68は48中47成功／1失敗。40px flowのrootにviewport200pxを期待したfixture誤りで、元FAILを保存。期待値訂正後の48全件成功とは報告しない。|
|専用extent対照|`build/browser-performance-extent-guest-20261013/test-serial.log:64-67`は4／0。absなしflowのdoc／root=40と、同clipのprivate=350、root=40を対照した。現在fixtureの40px期待を再読したが、他47件を反復していない。|
|原本WPT限定guest|`build/browser-performance-wpt-guest-20261013/test-serial.log:65-98`は3page／6case中5成功・1失敗、基盤失敗0。`container`未定義で最初のhit assertionに未到達。alias注入や原assert改変で通していない。|

最新三guestのmetadataは`boot_source=build/nocturne.img`、`process_stopped=true`を記録する。主担当の主報／completionは全VM／server終了、引渡しIMG／VHD／VHDX／ISO保全、使い捨て299ファイル回収を記録している。この担当が全process／媒体を再検査したとは主張しない。

## 現在のmaterial source内容hash

Git blob hash。J-space登録時の依存にも同じmaterial sourceを含める。後続編集へ流用しない。

|対象|hash|
|---|---|
|`user/libc/web/css.c`|`1f6d92e614e3a500e97c18eda473b44d63b3ebc8`|
|`user/libc/web/webi.h`|`56ee0a9d493269ec7de21bbe37164c0e3e1db54d`|
|`user/libc/web/doc.c`|`bbeb26ef73768df122dd6c57971c47bbd61d9dee`|
|`user/libc/web/cssprop.c`|`ea3746387add4e3106f8c74a2ff821074dafdc07`|
|`user/libc/web/util.c`|`afd1ae02b30163215140c10d2afcaa77fb4e9859`|
|`user/libc/web/paint.c`|`65f2df9ef619aab1f944d651b77d8d3d9e7af29e`|
|`user/libc/web/box.c`|`f1008fc2fd173eddeea09e2805840ba6eeba15ca`|
|`user/libc/web/layout.c`|`fbcca02f3eebbd9cba7441e658fc49903d0ac0d3`|
|`user/libc/web/frame.c`|`979b13d6d1ba295db7eef464f8ed4ee68c344023`|
|`user/libc/web/marquee.c`|`85af7823b42f682e380851498a9ab19395f4858f`|
|`user/libc/web/js.c`|`4f61c58e3caf6194941d8d90789cbc2de57ea4f9`|
|`user/libc/web/js_bootstrap.js`|`0f15295d4906235a048cd5409bab1de4134305fe`|
|`user/libc/web/js_html_elements.js`|`92f5b2506e9ea34e1be4f7f1dc230bd6fc7433a0`|
|`user/libc/web/js_bootstrap.inc`|`39cdb73f03964ea777f2c67161ae4bfff8b96cb9`|
|`user/libc/web/js_embed.py`|`4e0c5d3da47ab9b4e527992ea7d2e0616809c161`|
|`user/include/web.h`|`77749fb9e39caea4f479bf028fbbf1c799806610`|
|`user/apps/browser.c`|`9d79a1386fff2a92399ecaa454a2b64c3e7284b9`|
|`tests/check_cssperformance.py`|`99487ba801faa339e303d2449fd82a65848df429`|
|`tests/cssperformancetest.c`|`aba7a187ef8d8f68ba6cc84699937ab58e94b0c6`|
|`tests/styleincrementaltest.c`|`69a4e4ed1c39aa9a8a3a7c8099cc23989a40545e`|
|`tests/paintperformancetest.c`|`6510d0e4f8a408565381e24d5bd873385b8acd6d`|
|`tests/runtests.c`|`a3c63c9b0a1fc7f5cf02a83366c373f3bcf243d8`|
|`tests/wptperformancetest.c`|`21703bde9f8fe07846fb8728ee07b05d1d5e79c5`|

## 判定の限界

現在sourceのCSS共有固定部／宣言cache／退役所有と、独立paint／scrollの修理済み境界に追加の未修理反例は発見していない。ただし実全体速度・全サイト／全WPT適合・動画pause／seek／再open・聴取音質・Hyper-V実機の達成を意味しない。実Shakaでのfooterまでの下移動と動画へ戻る操作は主担当の原画／実操作記録として扱い、この担当の静的確認や旧style150／0で実受入を代替しない。
