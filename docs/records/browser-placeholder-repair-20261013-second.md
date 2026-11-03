# placeholder 共通修理・独立第二考察

- 初回仮説を再検討した：二重表示だけで作者 label を削除するのは誤り。作者が透明な native hint と別 label を併用する場合もあるため、作者 CSS を正規 cascade と入力 paint へ接続する必要がある。実サイトの宣言・修正後の最終画面確認は未達のまま残す。
- 透明と opacity 0 を同一の原因と決めつけず、alpha の乗算を native helper で別々に確認する。入力値があるときは元 control style を使うので疑似色に巻き込まない。空 placeholder は空文字として paint される。textarea にも同じ境界を適用する。
- 疑似スタイルの親は originating control。explicit inherit/currentColor は既存 cascade に委ね、標準 UA 灰色は author 宣言がない時と rollback 後に維持する。疑似 pointer は子 element に継承せず、旧世代の style slot は publication join まで保持する。
- paint は font と spacing の読出しだけを cached body に変更し、caret/selection の色・入力値・選択範囲は元 control の既存経路を維持する。placeholder の opacity は元 control の group opacity と別に一度だけ色 alpha に掛ける。
- 擬似 font を変えても control の正常な box/scroll/hit geometry を変更しない。placeholder pointer は layout 比較の address identity に含めないため、paint-only 色変更で不要な全 layout を増やさない。
- `getComputedStyle` の疑似 mapping は既存 js.c に before/after しかないことを主担当に伝えた。担当外 js.c は触らず、computed default と animation-base 選択の修理を主担当が調整する。
- 限界：この変更を ::placeholder の全 CSS property、Web Animations、複雑な背景・装飾までの完全対応とは報告しない。今回の共通修理は実際の hint font/color/opacity と寿命・paint 境界。実サイトの二重表示解消は最新媒体での主担当目視が必要。
