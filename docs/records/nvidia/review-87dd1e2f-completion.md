# 再監査修理の依頼照合

- **妥当な指摘を実装修理**：A1/A2を優先し、A4〜A9の契約境界とA10の共有寿命も修理した。A3は誤った永久取消を除き、未対応を状態無変更で返す。RTD3完成とはしない。
- **既存機能を保全**：旧ABI番号/Protocol1/診断接続、RO上限・通常取消、CPU context/fallbackを保持。native/edu・virgl・局所GUIの実確認を主担当が行った。全機能無回帰は未証明。
- **制約**：新規人工上限/POSIX/GPLv2-only追加なし。uvm_common.c参照/使用なし。virgl本体未変更。既存native event batch契約は維持し、誤っていた試験の順序/一回全数期待を正した。
- **委任と独立受入**：指定モデルの三担当による二段階検討。親がsourceと反例を独立確認し、ready/probed混同、構築中取消、partial handle回収を追加修理へつなげた。
- **検証**：host二コマンドの各三群0失敗、実pcitest0、native ABI222/0、RM edu拒否0、virgl55/0、通信最終333/0。初回の試験不備と失敗も保存。実GUIの再描画・入力・送信・上下scrollを目視。
- **媒体**：正規増分でIMG/VHD/VHDX/ISO更新。kernel/initrdと主要app/native ABI一致。Hyper-Vや通常dataを操作しない。
- **残る境界**：実NVIDIAのGSP・DMA/cache/VRAM・物理順序・複数GPU・実RM共有・任意競合・性能、Hyper-V実機は未確認。詳細な根拠と後片付けは `review-87dd1e2f-acceptance.md`。

これは実装の引渡し確認であり、実NVIDIAでの全面成功証明ではない。最終map同期と全担当の再読後、J-spaceの整合gateを実行する。
