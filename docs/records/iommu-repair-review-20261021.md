# 主担当による最終独立受入（2026-10-21）

## 受入対象と判定

実機の全面認定ではなく、今回のIOMMU・PCI修理と明記した模擬受入の範囲で受け入れる。担当のPASSをそのまま承認せず、最新差分・反例・一次仕様の接続を読み、主担当自身でモデルと最新製品を実行した。

- **AMD**：[live第二報告](iommu-amd-live-second-20261021.md)を全文実読。壊れた要約を実reserved能力の代用にしないこと、全unitの最小化、欠損functionの拒否、旧tokenの誤成功を防ぐ境界を確認した。主担当の最新113判定は失敗0。実QEMUで元の幅拒否が解消し、隔離DMA/IRQと二機能FLRが成立した。要約VA0を仕様適合値へ変更したという解釈は受け入れない。
- **Intel**：[final第二報告](iommu-intel-final-second-20261021.md)を全文実読。一時CFI1必須、EIMEへの不要CFIS条件、IRREQを失ったplain rollbackを除去した接続と、queue/command失敗・全unit quiet・DMA drainの保全を確認した。主担当の最終hostは失敗0、実QEMUで旧CFI起動回帰は解消した。QEMUはIRREQ0であり、実probeのIRREQ集約伝播は静的確認、共通拒否はモデル確認という留保を維持する。
- **PCI**：[live第二報告](iommu-pci-live-second-20261021.md)を全文実読。対象FunctionのFLRをgroup回復と混同せず、clean旧primaryのpeer化、MSI停止、取消後の保存FLR、native例外のBME確認、handle公開前の予約を追跡した。主担当の最新製品Cを含む二機能モデルは失敗0。実QEMUの二機能virtio groupでもFLR再claimが成立した。ACS/group probe全体のconfig無アクセスは証明していない。

上記の旧第一・第二報告や修理前失敗は履歴として保全し、旧hashの受入を現在へ流用していない。最終報告の依存が現在の製品/証拠と一致することも確認する。

## 共通層・実製品・媒体

主担当は共通84判定と実PIC/IOAPIC/PMコードを含む51判定を実行した。allocator失敗時の誤claim、予約交差、アドレス幅/leaf、未確認IRQ経路、IRREQ停止後のplain拒否、中止sleepでのIRR保全、復帰失敗後のPCI停止は失敗0だった。全register bank、実edgeや実S3の検査ではない。

最新kernel/initrdのAMD/Intel実QEMUを順番に操作し、EDU DMA往復、IRQ、claim/再claim、group FLR、native ABI/server受入を確認した。主担当本人が両方のデスクトップと実キー入力後の`help`出力を見た。禁止DMA、意図的な子アクセスfault、EDUの実NVIDIA初期化拒否、thread資源での接続拒否を隠していない。

正規増分buildとIMG/VHD/VHDX/ISO生成は終了0。起動scratchとIMG/ISOのkernel/initrd、initrd内native header/nvrm、VHD/VHDXの内容一致を確認した。両VMと停止した照合用mcopyは終了済み。専用scratch・展開コピーは回収済み。

## 受け入れない拡大解釈

実4070のクラッシュ後reset/scanout再建、nvrm idle接続公平性は未解決。実機DMA/IRQ、高位RAM、EIME/IRREQ実機、SMM/firmware ownership、probe前queue寿命、実fabric drain、S3/hibernate、Hyper-V、性能は未受入。QEMU/モデル成功をこれらの成功や「無回帰」「割込み無損失」「実機に絶対安全」へ拡大しない。

この境界を保持した三担当の最終第二報告を、controllerの独立reviewへ登録する。

最終全repoの初回viewは、作業対象外`hyperv/Nocturne-data.vhdx`の変化で停止した。同じinventoryの差分はこの一ファイルだけ、全担当の最新製品/報告依存hashは一致していた。後続の標準検査でinventory一致となったため、ユーザーの媒体やcontrollerを操作せず正規の最終読取りを行う。最終view/ship結果と上記の製品・証拠の範囲付き独立受入は区別し、前者は`ship-check.log`に残す。
