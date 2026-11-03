# GPU修理の最終受入と目標照合

2026-10-20、主担当root。`repair-20261020.md`の結果と最新製品差分を照合した。ユーザー依頼はP1優先・P2修理と可能なQEMU edu検証であり、未実装のNVIDIA modeset/NVDECやHyper-V実機試験へ範囲を拡張しない。

## 依頼項目の照合

- **P1**：接続別RM主体、RO不可逆権利と取消、BAR1 drain→GPPut→doorbell順、virglのscheduler待機・取消/寿命を実装した。親は関連差分と呼出し境界を読み、eduの実権利/IRQとvirglの描画を独立実行した。BAR1実装置到達と実NVIDIA間の隔離は未実機受入。
- **P2**：BAR列挙/cache、RMエラー、timeline回収、既存context保持付き新GPU選択、caveat、headless依存、BDF endpoint、firmware依存/格納照合、装置/資源に基づく可変描画を製品へ接続した。対応表13項目を省略していない。
- **既存機能保全**：既存ABI番号、1936-byte batch、48-byte Protocol1、旧サービスalias、CPU fallbackを保持。既存開始差分3ファイルを保全。最終GPU55/0、CPU44/0、native契約0失敗、実ブラウザーCanvas/WebGL/入力/上下scrollを確認した。全アプリ・実サイト・全GPU世代の無回帰は証明していない。
- **禁止事項**：POSIX互換層、人工的な資源上限、GPLv2-onlyコードを追加しない。`uvm_common.c`は参照/使用しない。独立MIT virglrendererのwire/座標契約を参照したが他OS本体やソース転記はない。
- **委任**：GPT-6.1 SOL/xhighを3担当に使用。各担当の第一・第二検討、統合追補2段階を親が読み、重要契約を実差分とhost/実QEMU結果で確認した。全員の合意だけで成功判定していない。
- **実検証**：QEMUは一台ずつWHPX/4CPU/4GiB/1080p、AHCI、専用snapshot。edu PCI/DMA/IRQ/取消試験、virgl、CPU fallback、native WSI/softpipe、ブラウザー実操作を確認し主担当自身が画像を開いた。
- **引渡し**：正規増分build終了0、IMG/VHD/VHDX/ISOを更新しpayloadを照合した。QEMU終了、今回の使い捨てboot/dataだけ回収。通常data・ユーザーVMには触れていない。

## 担当別の独立レビュー

| 担当・最新第二報 | 親が独立に受け入れた根拠 | 判定と境界 |
|---|---|---|
| gpu_rm / `repair-rm-final-second.md` | principalのrequest/cleanup TLS、Protocol1のHELLO union、RO伝播、精密map tokenとvendor寿命hook、BAR順序/cacheを差分で追跡。親の反例で暗黙FREE・旧client互換を修理。実7TU構文/host3群ログ、正規RM link、edu拒否・claim再取得を照合 | 実装・検査済み範囲を受入。実NVIDIAのGSP/共有/DMA/cacheは未確認 |
| gpu_nvk / `repair-nvk-final-second.md` | channelのdrain page寿命とsubmit順、timelineの旧seqno、CPU/GPU screen分離、WSI広告/初期化を差分で追跡。親がnative instance/surfaceと2contextを実QEMUで実行し、ブラウザー画素保持も実目視。firmwareの独立反例を親が修理 | 実装・softpipe/headless契約を受入。実NVIDIAの到達順・ready昇格・複数GPU/性能は未確認 |
| gpu_kernel / `repair-kernel-final-second.md` | native shared scope/権利/失効、BME停止確認とquarantine、waiter起床、GPU snapshot/copyout、chunk/resource寿命、最終原点を差分で追跡。親が実QEMUでRO/raw/child fault/IRQ、元の末端pixelと四隅/拡縮/上下、GPU55/0・CPU44/0・GUIを確認 | 実装と対象QEMU操作を受入。意図的な遅いfence中GUI latency、全OOM/実PCIe消失競合は未実測 |

RM/NVKの最終追補に記録された「親の末端修理待ち」は当時の正しい状態である。後の`origin-after-serial.log`と最終画面を親が統合し、旧追補を書き換えず解決した。先のCOPY仮説失敗やRO試験契約誤りも保存し、成功だけを選別していない。

## 残る検証境界

NVIDIA実機のGSP起動、物理BAR/cache/posted-write、IOMMU前提、複数GPU、実GPUへのcontext昇格、異常終了・復帰、Hyper-V実機、転送/同期込み性能は未確認。eduのDMA quarantineは安全側の隔離であり、停止完了証明/隔離解除/実NVIDIA支援の証拠ではない。これらを合格に含めず、実装引渡し後の実機検証課題として残す。

最終J-space map refresh/ship gateはこの受入後に実行する。これは実製品の試験の代わりではなく、上記ソース・報告・証拠の整合確認である。
