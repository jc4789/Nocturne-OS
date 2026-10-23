# 角丸高速経路と背景関連付けの第二考察

第一記録は `docs/browser-css-mask-association-first-20261013.md`。同じ host 成功の再実行ではなく、別の所有権・性能・反例境界で再考察した。

1. **角丸内部を矩形と決めつけない。** 祖先の全 A8 を通す元経路を残し、実 cache の全 255 の連続区間だけを速い経路へ渡す。未知マスクでは区間なし、異なる祖先の位置／範囲では交差を取る。試験は corner の見た目だけでなく全画素を元 fallback と比較する。楕円や per-corner を uniform 半径へ縮める高速化はしない。
2. **速い合成でも source alpha を飛ばさない。** 証明するのは clip alpha だけであり画像／glyph／canvas の source alpha は元の合成で処理する。媒体ではサンプル位置を同じ Q32 で進め、clip 境界で SSE ペアが飛ばした座標を作らない。同寸法コピーも不透明 clip 区間だけに限定する。
3. **cache 寿命と借用 descriptor は別。** 不透明行 metadata は radius cache 自身が確保・破棄する。paint stack は cache の alpha と metadata を借用し、canvas をコピーしても所有権を移さない。checked allocation／checkpoint 失敗は古い cache を消してから作り直す方式ではなく、未完成の新 cache を破棄する。毎 frame の viewport scratch や pixel ごとの時計 syscall は増やさない。
4. **実画像の消失は radius と断定しない。** 公開原 CSS の主画像は角丸でなく背景ブロック。mask の導入前後という時間順だけで cause にせず、親 POD コピーと native image 登録の契約を追った。表示してはいけない親 handle が非継承画像へ残る source 不備を修理し、背景と mask の両方を明示 inherit／none／gradient の反例で確認する。実症状との因果は次媒体の hover／scroll 確認まで未確定。
5. **性能 microbenchmark を実ブラウザー成果にしない。** 同 pixel の 228→9 ms は大きな不透明内部の chain work を省いた支持証拠。実サイトでは通信・JS・CSS・layout・shadow／mask cache の構築時間もある。主担当が観察した旧約 38 ms／新約 270 ms は状態差があるため、私のマスクだけによる全 frame 回帰と断定しない。新媒体と同条件操作の比較を必要とする。
6. **host helper の盲点を guest に残さない。** host は exact production consumer を取り込むが native decoder／resource table／partial cascade 全体は別。新 fixture は通常 parser ticks と resource loaded を使い、hover による partial style 更新後の own-image handle と色を確認する。初期化の誤りを期待値弱化で隠さず、失敗時は元 log と修理理由を保存する。

`CSSStyleDeclaration` の null／undefined 変換には別の未完境界を静的点検で認めたが、作者の主画像 style は URL を与えており今回の画像 handle 不整合とは異なる。凍結した性能修理へ未確認の JS 変更を混入させていない。全面 CSSOM 対応・百度回帰解消・サイト性能改善・Hyper-V 成功はいずれも本記録の主張ではない。
