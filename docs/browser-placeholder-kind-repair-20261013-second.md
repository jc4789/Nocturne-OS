# placeholder selector 値衝突・独立第二考察

- 「疑似要素を index が捨てた」という初回調査だけでは、新 kind を index から collection まで渡す契約を確認できていなかった。native の pseudo body 未作成と入力値の色変化という両症状を同時に説明する経路として、`PE_SLOTTED` 分岐を再読した。
- marker 値 5 の変換は CSS の仕様上の rollback ではない。assigned element に対して slotted ルールを普通 element 宣言として適用する正規経路だったため、placeholder と共有したことが直接の製品バグだった。
- slotted の既存値を変えて逃がす方法より、共有 enum に既存 marker を統合して名前空間を一元化する方を主担当指示に従い採用した。既存 before/after/backdrop と slotted の番号・振舞いを維持し、placeholder だけ非衝突値へ移した。
- 初回 native の 7 失敗を、固定入力の不備や期待値過剰として除外しない。原 paint 12 条件と原 CSSOM 20 条件で、作者 hint と通常 control style が独立することを修理後媒体で確認する必要がある。
- host 非衝突 1 条件の成功も、実際の CSS collection・入力描画・実サイト二重表示解消の代わりにはならない。latest native と実サイト画像が失敗するなら、次はその新しい実製品境界を調べる。
- 値変更に対して js.c は enum symbol を使用していることを主担当が確認している。root の CSSOM 疑似 mapping 修理も最新の全 build に入れる必要がある。媒体更新後に確認しないまま前回 host のみで完了としない。
