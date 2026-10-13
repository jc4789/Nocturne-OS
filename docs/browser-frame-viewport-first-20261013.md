# 子文書viewport同期：第一修理報

`build/browser-performance-native-20261013/test-serial.log` 316–342を直接確認した。原 `css/css-cascade/layer-media-query.html` の8条件は300pxでredまで成立し、iframe幅500px変更後の期待greenに対してredが残る。先行の不透明色・data CSS失敗は解消され、原basic34／important9、新native色9／owner event7／動的層11は成功した。これら成功群を反復しない。

原因はframe.cがpaint用surface準備時だけchild viewportを変え、getComputedStyle(child)がchildだけを旧幅でlayoutしていたこと。属性名や原WPTの値を狙う処理ではなく、[CSSOM Viewのviewport寸法](https://www.w3.org/TR/cssom-view-1/#dom-window-innerwidth)と[CSSOMのresolved style](https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle)が共有する製品境界を修理した。

- doc_viewport_updateは既存window resizeのlayout/CSS/source-selection無効化を共通化し、寸法をauthor callback無しで公開する。
- 親の成功layout commitとframe document commitで、実content boxから直下child viewportを公開。paint準備も同じhelperを使い、clip cropをCSS viewportにしない。
- JS共通flush_layoutはembedding ancestorを外側から内側へ同期。computed style、寸法、observerの既存consumerが同じ経路を使う。child innerWidth/innerHeight／matchMediaも同期する。
- 小さい通常chainは8ポインタの一時stack、大きなchainは動的確保。8は深度制限ではない。真OOMは取消可能なallocation-free traversalへフォールバックし、C再帰や全frame forestの寸法getter走査を追加しない。

新 `csscachetest --frame-viewport` は5条件。iframe属性は一切変更せず、parent CSSの幅300→500→300、child Window寸法／media／computed style、未変更親子layout revision維持を確認する。host構文とCRLF diff確認は成功、native実行はまだ未確認。原WPT8条件と別担当のWindow named修理後のinline4条件だけ、主担当の新全媒体で再確認する。

layout.cのauto-table、DOM担当のWindow named/native input、CSS担当のradius/paintは変更していない。新実site・入力・scroll・性能の成功にはまだ昇格しない。
