# IOMMU修理の引渡し範囲（2026-10-21）

目標を項目ごとに照合する。主対象の実装修理と模擬受入は以下の範囲で成立し、実機受入やNVIDIAの全残件まで完了とはしない。

- AMD/Intel IOMMU：一次仕様照合、DMA/IRQ・予約域・幅・queue/失敗寿命・電源遷移を修理した。
- PCI/NVIDIA接続：隔離必須、旧DMA保全、次claim FLR、group peer回復、回復状態のnative ABI通知を接続した。実GPU reset/scanoutは未解決・未受入。
- 既存ABI/差分：構造とsyscall番号を保全。元のAGENTS.md差分を未変更。他OSコード流用、GPLv2-only追加、人工quotaなし。
- 独立検討：担当の第一/第二検討を主担当が実ソース・反例・実行で確認。旧snapshotの報告を新ソースの証拠へ流用しない。
- 軽量確認：最終AMD113/0、Intel失敗0、PCI失敗0、共通84/0、実PIC/IOAPIC/PM51/0。モデル・構文成功を実機成功としない。
- 実製品受入：最新kernel/initrdの単一QEMUでAMDとIntelを順次実行。EDU DMA/IRQ/再claim、二機能FLR、native ABI、server検査は失敗0。主担当本人が両画面と実キー入力を確認した。
- 媒体：IMG/VHD/VHDX/ISO更新成功。kernel/initrdと関連native header/nvrmの内容一致を確認。Hyper-V使用コピーと通常dataは未操作。
- 片付け：両VM停止済み。専用scratchと展開コピーは回収、ログ・画像・最新媒体・既存データは保全。

残る実機検証は、AMD/Intel firmware引継ぎ・高位RAM・実DMA/IRQ・S3/hibernateと、ユーザー担当のHyper-V/実4070。nvrmのidle接続公平性、GPUクラッシュ後の実reset/画面復旧は未解決として引き渡す。無回帰・無損失・実機安全を全面保証しない。

詳細と根拠：[修理・受入記録](iommu-repair-20261021.md)。

J-space初回最終viewは、作業対象外の`hyperv/Nocturne-data.vhdx`だけの変化で停止した。その後の標準検査でinventory一致を確認したため、ユーザーのVM/データを操作せず最終読取りを行う。controllerの改変・skipは行わず、製品受入と手続きの結果を分離する。最終判定は`build/iommu-repair-20261021/ship-check.log`へ保存する。
