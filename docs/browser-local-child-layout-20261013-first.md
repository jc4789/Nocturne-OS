# 動的挿入と同期寸法flushの局所化：第一報

- 原境界：`compat-media-cadence-baike-article-20261013/serial.log` のTimer675は45,188ms、うち87回の同期layout flushが45,091ms。170寸法call、89挿入、style-nodes累積wall30,623ms、当該区間の媒体pump25秒を確認した。媒体cadence修理による実F12全高／上下wheel成立とは別の残失敗。
- 共通CSS修理：private selector索引にchild-listによる親状態と兄弟位置依存のsubjectを分離。通常挿入は接続した変更部分木と依存一致する兄弟だけをdirtyにし、親はdirty経路として通過する。:empty／:has／外側sibling効果、Shadow、focus path、fieldset/selectなどの意味境界は保守的に全子孫を再計算。正の:is/:whereは必要なstable subjectキーだけで候補を絞り、動的疑似の現在値や:not/:has witnessで旧一致を捨てない。
- 共通box修理：既存構造通知からparent/subtree queueへ接続。既存boxkindを変えるstyle、新sheet、資源、曖昧なトポロジーは全再建。通常block親の明示px top/leftを持つ独立out-of-flow blockだけ、既存genで短寿命の独立arenaに作り、接続・撤去する。サイト名・DOM名・script・font-probeの特例はない。
- 安全境界：auto inset/static position、percent/math/minmax依存、inline/flex/grid/table祖先、scroll-container/transform親、native atomic、list/counter/生成pseudo、Shadowや別文書ownerは元の全再建へ戻す。変更後の寸法は既存layout/overflowを同期実行し、固定戻り値やgeometry無効化の無視を使わない。
- 所有：新枝のgenはcommit前にstage。失敗はfresh node/anchor借用とarenaだけを撤去し、正常の全buildへfallback。撤去はhit/layer/abs借用を無効化、親の新chainから離し、cache/node借用をunpublish後に枝arenaを最後に回収。document破棄も旧treeから枝arenaを回収する。作者JSを途中へ再入させず、nativeキャンセルはstage前後で安全に扱う。
- 性能境界：queue重複／removalのpointer索引は短寿命で動的確保、各親適合と直接DOM chain合成は一回。単純queue再走査・候補ごとの predecessor探索による二乗反例を修理した。制限値で大量挿入を拒否しない。
- 支持：対象源freestanding構文／差分空白検査成功。既存hostの新local-box helper20条件0失敗。共有ABIを更新した旧placeholder／committed helper34条件も0失敗。音声供給hookはhostでstubであり、実性能・媒体受入ではない。
- 検査訂正履歴：新fixtureの未存在API名、overflow enum名、hostのmath型名を実装の既存名称へ訂正した。host型失敗ログは `build/browser-local-box-host-type-failure-20261013.log` に保全し、期待は弱めていない。
- 既存native `cssrelationaltest --child-list` へbody直下の巨大forest保持、末尾位置、logical位置、sibling :has、独立枝の実寸法とscroll extent、auto/percent fallback、32枝の一括insert／remove＋insert／removeを追加。原child-list・原geometry17期待は保持。新native／実記事性能はまだ未実行、源を凍結して主担当へ引き渡す。

