# IOMMU・PCI実装修理と受入（2026-10-21）

## 製品の変更

- 既定のPCI claimは隔離必須。管理者の明示的な`appdrivers-unisolated`だけが固定的な隔離不能の例外を許す。無効化失敗、旧claimの遮断未確認、電源遷移失敗は例外設定でも迂回しない。
- 共通層で全unitの実IOVA/PA幅、ページ表深さ、leaf、オーバーフローを検査する。DMAとハードウェア用表の確保も実幅へ制約し、上位bitの切捨てaliasを作らない。
- IVMD/RMRRのaccess=0、重複の権限intersection、恒久予約の寿命を保持する。最終的に到達可能な予約片とOS所有RAMの交差を拒否し、DMA objectを予約範囲へ割り当てない。
- AMD-ViはExclusionBaseによる全体迂回を解除・readbackし、IVRSの長さ・alias・segment・能力情報を検証する。壊れた幅要約を推測上限にせず、取得できる各unitの実能力を検証・最小化する。実reserved値の拒否は維持。完了tokenの0 sentinel周回も修理した。
- VT-dは不正descriptorのskipとエラーclear反復を撤去し、queue/command失敗と未確定メモリーを保全する。全unitの停止前検査、drain能力、firmware QIの元の幅・長さ・所有メモリー、命令直列化、S3復帰を扱う。一時CFI有効化を要求せず、通常モードは全unitのCFI解除確認後にIRE/TEを有効化する。EIMEは仕様上それ自体が互換IRQを遮断する。
- 初期化はPIC/IOAPICをmaskしてからbackendと経路形式を切り替え、経路変換失敗を成功扱いしない。IRREQでは「停止確認」と「plain配送可能」を区別する。未確定状態からの新MSI/IOAPIC経路や解除成功、S3/hibernateを拒否する。
- S3はIOMMU確認後にIOAPIC→PIC→PCIを戻す。中止sleepではPICを再初期化せずmaskだけを戻し、保留edgeを消さない。IOMMU復旧失敗後にPCIを再開せず、休止復元ではlive tableを上書きしない。
- PCI終了処理から同期FLRと旧200ms待機を外した。保存configへBMEを戻さず、次claimで仕様由来のFLR/Completion Timeout/RTR時間と取消・未準備を扱う。保留FLRの保存状態はdeviceが所有する。
- 複数機能groupはclean終了でも全memberの旧状態を回復対象とする。新primaryだけのFLRでpeerを回復済みにせず、全dirty peerのMSI停止とFLRを確認してからhandleを返す。未回復peerは再遮断する。native回復の委任は新primaryだけで、`N_PCI_NEEDS_RECOVERY`で明示する。
- 非隔離claimの終了後は、BME off/pending=0だけで残存DMAページを再利用しない。FLR成功もDMA drainの代用にしない。

native ABIの構造・syscall番号は変更せず、既存flagsへ回復状態bitを追加した。NVIDIA adapterとABI説明も、隔離方針・回復責務・画面hold解除の意味へ接続した。開始時の`AGENTS.md`差分は変更していない。他OS本体の参照・コード流用やGPLv2-onlyコードの追加はしていない。

## 一次資料と独立確認

ユーザー指定のAMD 48882 Rev3.10、Intel VT-d Rev5.20、PCIe Base Rev6.0を使用した。過去指摘を一律に肯定せず、AMD event restartとPerfOptの元の正しい部分は維持した。担当ごとの反例・仕様節・旧失敗は各報告に保存している。

- AMD：[最終第二報告](iommu-amd-live-second-20261021.md)。欠損要約による拒否は修理前5失敗、修理後113判定で失敗0。
- Intel：[最終第一記録](iommu-intel-final-first-20261021.md)。CFI/EIMEの反例、IRREQ伝播、未確定queueの保全を確認。旧報告は旧snapshotとして保持。
- PCI：[新第一報告](iommu-pci-live-first-20261021.md)。二機能groupの回復漏れは修理前15失敗、修理後失敗0。
- 主担当も最新製品Cを直接含むAMD/Intel/PCIのモデルをMinGW＋UBSan trapで独立実行し、全て失敗0。共通層84判定、実PIC/IOAPIC/PM経路51判定も失敗0。kernel targetの厳密な構文確認と差分検査も成功した。

モデルの判定数は実機試験件数ではない。共有header変更前の担当実行と、その後の主担当の最終実行は区別している。

## 最新製品の実QEMU受入

正規の差分ビルド後、**一台ずつ**AMD→Intelの順で実行した。WHPX、4CPU、4096MiB、1920×1080、AHCI起動、専用boot/data snapshot。通常`build/data.img`とHyper-V使用媒体は接続・操作していない。

条件はQ35、EDU `dma_mask=0xffffffff`、root port配下の二つの`virtio-rng-pci` function（00.0/00.1）。AMDは`amd-iommu,intremap=on,dma-remap=on`、Intelは`intel-iommu,intremap=on,caching-mode=on`。

両backendで次を確認した。

- IOMMU起動、EDUの`isolated=yes`、DMA往復と内容一致、IRQ受信、claim/解除/再claim。
- `pcitest`、`nvrmtest`、二機能groupの`pcitest flr`は失敗0。
- `nativeabitest`は225判定・失敗0、`nvrm_servetest`は失敗0。
- claim外DMAへの負例ではEDU livenessを変更できない。Intelは対象IOVAの変換faultも記録した。AMDには遮断interrupt eventが記録されており、例外なしとは報告しない。
- revoke後アクセスを意図的にfaultさせる子のpage faultと、EDUを実NVIDIAとして初期化できないログは、成功条件から隠していない。

主担当自身が`amd-final-now.png`、`amd-final-input.png`、`intel-final-now.png`、`intel-final-input.png`を開き、デスクトップとTerminalを確認した。HMPの実キーボード入力で`help`を実行し、出力とプロンプトの復帰・再描画を両VMで目視した。ブラウザー、動画、性能、実機S3の回帰受入を行ったという意味ではない。

ログ・画像は`build/iommu-repair-20261021/`。元のAMD幅拒否とIntel CFI起動失敗のログも保全している。両VMはHMP quit後、終了0を確認した。

## 引渡し媒体と片付け

正規`image`で`build/nocturne.img`、`.vhd`、`.vhdx`、`.iso`を更新した。

- 起動したscratch、最終IMG、ISOのkernel/initrdを抽出しSHA-256一致を確認した。VHD/VHDXはIMGとの仮想ディスク比較で一致した。
- kernel: `4b0c5c68d17539844ed0ebf4a3f1a38f88fd11e83b04cf8053eadc603f96b7a1`
- initrd: `5d91438616d6d71df255af0f56a3d243fc261d5d30457080913a7145eb6531b6`
- initrd内`nocturne.h`と`nvrm`も生成元と一致。根拠は`media-content.log`、`app-content.log`、`vhd-compare.log`、`vhdx-compare.log`。
- mcopyのUnix出力先上書き確認で一度照合が停止した。所有mcopyだけを停止し、固有名の展開先に直して照合を完了した。製品の成功検査として数えていない。
- 停止確認後、この作業専用のscratch boot/data一組と照合用展開コピーだけを回収した。標準媒体、通常data、既存`nv-*`媒体、Hyper-Vのコピー、ログ・代表画像は保全した。scratchそのものは保存済みではない。片付け後の空き容量は約110GiB。

## 未解決・未確認

**実機に対する全面的な安全認定ではない。** 実NVIDIA/4070のクラッシュ後reset・scanout再建、実機DMA/IRQ、S3/hibernate、Hyper-V、性能は未受入。起動画面GPUの汎用FLRは実装しておらず、回復bitや画面hold解除を実回復成功とみなさない。

nvrmの同一appによるidle接続のthread占有と主体間公平性は未解決。127接続の処理と追加接続の安全な拒否は、公平性の証明ではない。根拠のない固定quotaは追加していない。

firmware/SMMとのsole-producer契約、probe前のfirmware queueメモリー寿命、実fabric drain、EIME/IRREQの実機経路、FLR中のACS/group probe全体の無アクセスは未証明。特に実QEMUのIntel ECAPはIRREQ=0であり、その1fieldの集約伝播は静的確認、共通必須IR失敗の拒否はモデル確認に留まる。

## J-space最終手続きの制限

最終repo sync後のroot viewは`Repository changed`で停止した。controllerと同じinventory比較で原因を一度調べたところ、差分は作業対象外の`hyperv/Nocturne-data.vhdx`だけで、branch変更はなかった。製品/試験/報告の依存hashは別に照合して一致した。

ユーザーのHyper-Vを停止したり、そのデータを移動・書換えしたりしてfingerprintを固定していない。標準controllerの内部関数差替えや検査skipも行っていない。後続の標準ship検査ではinventoryが一致し、失敗理由がrootの最終view receipt不足へ変わった。この静止区間に正規の最終読取りだけを行う。初回の停止は製品検査の失敗やJ-spaceの合格へ読み替えず、最終ツール判定は`build/iommu-repair-20261021/ship-check.log`へ保存する。
