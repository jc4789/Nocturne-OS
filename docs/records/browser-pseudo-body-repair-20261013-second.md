# computed pseudo body と box 生成の分離・独立第二考察

- 「before/after pointer が非 NULL なら生成する」という旧暗黙契約を全て読み直した。box children、背景登録、font refresh、geometry equality/paint-only compatibility がこの前提を共有していたため、CSSOM だけへ body を残す修理では、空 box・不要取得・layout 回帰が生じる。
- generation bool を originating style に置き、body は既存 style slot の current/previous 世代と一緒に回収する。初期化で pointer と bool を非継承に戻す。animation base の疑似 body にも同じ生成 flag を持たせる。
- 原子 box の全排除を採用しない。既存 native button の AT_INLINE_BLOCK は children を生成する。flex/grid button は AT_NONE、resource-less object も AT_NONE で fallback を生成する。SVG の専用描画と入力 control は、その専用 box 内容を維持する。
- 反例は replaced input の content 定義あり、普通 div の content:none、display:none の after、空文字 content の生成、native/flex button、object fallback。作者色しか変えない nongenerated style は、body の pointer address や font 記述によって layout を壊さない。
- background/font 条件は node/body が nongenerated の場合だけ抑制する。body の URL や font-family の computed value 自体は保存し、作者宣言を無視したり固定値を返すことで CSSOM 試験を通していない。
- native と実サイトの結果は host 24 条件では代替できない。特に GitHub の nav/button/SVG は最新統合媒体で主担当が確認する。失敗した原 CSSOM の期待は弱めず、同じ入力を製品修理後に確認する。
- 調査中の一度の rg 呼出しで options を `--` の後へ置いてしまい、生成 JS include を大量出力した。以後は options を pattern より前に置き、対象 C/H と出力範囲を限定した。この CLI 失敗を製品の検証成功として数えない。
