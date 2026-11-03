# フォント・CSS供給・長native境界：別の第二考察

第一報 `browser-compatibility-font-current-first-20261013.md` を変更せず、成功の最弱前提を反例から検討した。

## 形式hintを消すだけでフォント成功とは限らない

EOT の拡張子そのものを拒否すると、hint 無しの実 decode や対応済み EOTLite／XOR が壊れる。製品変更は `format(embedded-opentype)` の誤った全形式支持に限定した。次候補は作者順で選び、WOFF2 をサイト用に優先していない。先行有効 `src` を unsupported 後続で消す別の失敗を、CSS Fonts 4 の declaration parse-error の境界で修理した。

原 WOFF2 の字形確認は選択した実 binary の支持を示すが、DDGの検索欄枠・影・日本語fallback・最終レイアウトを示さない。WPT font-src serialization の原本実行も別境界のまま残す。

## 全候補発行だけでは並列CSSにならない

rescan は pending リストを交換し、document.open は同URLの新 stream 要求を作り得る。単に gate を消すと毎 tick 再発行や古い完了の混入が起きる。文書寿命 cache record を request の所有者とし、id一致した response／free だけが loading 状態を更新する。OOM は未claim状態で再試行可能、redirect base の確保失敗で誤った import base を公開しない。

12件の native 確認は逆順・重複・import・取消を通ったが、実 network worker の同時利用率／総ロード時間は計測していない。初期描画の3秒 fallback は別の親修理であり、全資源取得の取消でも成功隠蔽でもない。

## no-op失敗の前提と訂正

旧10件中1失敗を削除して報告していない。一方、quiet前提が未成立の無効fixtureを永久維持することも正しくない。元assertへquiet条件を追加し、live parser／tick／実未完了font要求で成立させた。dirtyを直接消去する迂回はない。新one-shot raw1件成功とwrapper prefix FAILは分離し、旧9成功を未再実行の新10件へ流用しない。

## 局所dirtyが壊し得る古い一致

`.empty:empty + .after` は子追加だけで外側 sibling が変わる。新結果の `:empty` がfalseだから無効化しないという判定は古い一致を残す。stable tag／id／class のみで候補化し、`:not(:empty)`や論理list内も保守的に扱う。`:has` は既存 ancestor／previous sibling witness invalidator に委ねる。

親の computed styleがほぼ同じでも、子は非継承propertyへの明示inherit、custom var、退役arena内のbox-shadow listを借用し得る。親全 descendantの再cascadeとretirementを維持し、`c->full=redo`を削っていない。interaction subtreeやshadow/slotは既存境界を保つ。新 child-list fixtureは、この外側 empty／has 反例とunrelated forest再styleの減少を一緒に確認する設計だが、guest未実行である。

## 診断costと通常性能を混同しない

sampling推定値には分散があり、task wallより大きい値もあり得る。sample件数と推定表示を付けたが、精密なinclusive時間には戻さない。呼出し回数は正確で、非debugにsampling演算を増やしていない。提供画面のdebug有無とguest時計fastpathは未確定だから、debug時計修理だけでユーザーの2分報告を全面説明しない。

この担当の製品sourceは凍結、独自VM等は起動していない。親のモデル容量エラーを完了やユーザー停止とは扱わず、復帰後の新guest／同条件実サイト測定と独立reviewへ引き継ぐ。
