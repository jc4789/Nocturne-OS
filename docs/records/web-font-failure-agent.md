# Webフォント失敗時の候補更新

失敗だけで全cascadeを予約すると、SVGの`style_box_compatible()`が常にfalseになるため、未変更のページでもbox tree全体が再生成される。

`css_refresh_font_selection()`は公開済みのstyleと現CSS scopeを使い、本体・疑似要素・animation baseのフォント候補を再選択する。次候補が未読込の`NULL→NULL`でもリソースの`wanted`を進めるが、style／layout dirtyを追加しない。実際にfaceが変わった要素のみ既存の`css_mark_dirty()`へ渡す。

現cascadeと同じflat treeを既存の`container_first()`／`container_next()`／`doc_flat_parent()`で非再帰に走査し、Shadow、割当済みslot、別文書からadoptされたノードも扱う。未割当light DOMや非表示のslot fallbackは走査しない。global font cacheを利用せず、family解析の一時arenaはノードごとに解放する。OOMはtrapで回収して通常cascadeへ戻す。既にcascade待ちならその経路へ任せ、CSS snapshotが無い・古い場合も通常cascadeへ戻す。既存のdirty状態は消去しない。

主担当は`doc_font_loaded()`の失敗で`font_selection_dirty`を予約し、`web_layout()`で一括消費する。pending cascadeがある場合は予約を保持し、公開後に候補を更新する。候補変更でpartial cascadeが必要になった場合も再予約し、partial cascadeで消えた無変更要素の次srcの`wanted`を公開後に復元する。API宣言は`webi.h`のCSS API群、フラグは`web_doc`のstyle dirty群に追加した。

統合差分の独立再読では、OOM trap後のarena解放、公開後のstyle借用、次srcの再予約、cancel時の従来破棄経路を確認した。raw DOM走査とcascade対象の不一致は、非cascade要素に古いstyleが残る場合の反復条件になり得るため、主担当の指示でflat tree走査へ修理した。通常のslot変更は既存の全style消去で古いstyleを防いでおり、この条件を実GUIで再現済みとは扱わない。新iterator版の製品試験は主担当が担当する。

独立レビュー対象は、Shadowとadoptの走査、擬似要素の次src進行、既読込faceへ切り替わる場合の再計算、OOM時のarena寿命、既存CSS／DOM dirty状態との共存。製品試験と実QEMU確認は主担当が実施する。
