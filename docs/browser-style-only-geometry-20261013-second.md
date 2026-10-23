# style-only 更新後の geometry 再使用・独立第二考察

- layout_valid を true へ戻すだけの案を採用しない。root post/cache、現在 font 世代、同じ viewport と post 寸法を条件にして、実際に geometry が committed のままかを判定する。新 box は post_valid を持たず、geometry/intrinsic/font 変更は root dirty へ伝播するので従来 layout を実行する。
- 画像の intrinsic 変化は `doc_image_loaded` が該当 box を layout_invalidate し、font 成功は layout_invalidate_fonts と世代更新を使う。viewport 更新は invalidate_layout_tree する。これらは再使用条件を満たさない。
- scroll extent、相対移動、ellipsis の font/overflow 入力は geometry-sensitive style 比較で layout dirty を立てる。nongenerated body 色だけは描画 geometry に参与しないため layout epoch を増やさない。
- 子 frame は通常 committed 出口で viewport を再確認する。frame 元の寸法変更は root dirty で従来 layout に進む。container の type/name/寸法変更も元の比較・invalidator を通る。geometry が同一なら container の寸法入力を再出版する必要はない。
- find は通常出口から従来どおり呼ぶ。beforematch 等で内容が露出・変更された場合はその新 dirty と style/box 要求により次の geometry が計算される。avmedia_layout_begin は committed raster を破棄しない既存 no-op 契約なので、再使用で媒体寿命を短絡しない。
- paint layers と hit index は geometry epoch 以外にも依存する。class 更新は DOM 世代を更新するが、CSS animation/native effect はそうとは限らないため、restyle 対象の layer validity を落とし visual epoch を更新した。paint-only 変更を geometry 変更へ昇格させずに正しく更新する。
- 元の geometry 失敗期待は弱めない。追加 5 反例が成功しても実サイトの表示・入力・性能の代わりにはならず、主担当の最新媒体の実確認が必要である。
