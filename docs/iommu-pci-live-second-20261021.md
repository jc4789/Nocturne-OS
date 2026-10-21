# PCI寿命と割込み移行：独立第二検討（2026-10-21）

## 範囲・保全・結論

- 担当 `pci`、既存の新しい検討周期の round 2。別の委任による独立再照合であり、第一検討の実行を自分の実行として記載しない。
- 旧 `iommu-pci-first-20261021.md`、`iommu-pci-second-20261021.md`、新 `iommu-pci-live-first-20261021.md` を全文読み、上書きしなかった。SHA-256は順に `e9d001e8e90cf138a5e439f260981c27b7a7145fb681d3eae6e02ed50599d016`、`0a31ca55a4114d5da66124a668d31bf8236e5e333431458799b27f414c3afbd3`、`c5691e2621a9da61f6acd2b64d48a22d2bafbf4dd562d8cb72b85e896e0a62a4` のまま確認した。
- 開始時の共有差分と `AGENTS.md` のnative ABI追記を確認して保全した。製品・試験・ABI・受入条件は変更していない。今回の追加は本報告と担当のcontroller記録だけ。
- 主担当の同期通知までは読取専用で再照合した。通知後、Python 3.14.6と指定controllerで、自分の `pulse --event resume`、`repo view`、`read`、`check --stage work` を一度実行し、全て終了0・`ALLOW work` を確認して主担当へ即通知した。巨大inventoryは `build/iommu-pci-live-second-map-view.log` に退避し、同期済みmapの契約・根拠・未確認を読んだ。
- **今回再照合した範囲に追加の確定反例はない。** 以下の反証候補を最新ソース・一次仕様・保存された失敗と成功ログに照らして退けた。ただし実機安全認定、全config無アクセス、性能無回帰を意味しない。新しい製品修理や、無変更の試験反復は行っていない。

## 反証候補とソースによる判定

### 1. clean closeならpeerの回復やDMA遮断は不要ではないか

`pciuser.c:claim_release` を `iommu.c:iommu_block/iommu_domain_free`、`syscall.c:shared_release` まで追跡した。

- 複数memberならprimaryのclean判定によらず全memberをdirtyにする。peerのBME停止・readbackも行うが、解放にsleepやFLRを追加していない。primaryのpending確認も `pci_requests_done(d, 0)` である。
- 残存DMAページの再利用条件は `dma_unsafe = !blocked`。BMEoff、pending=0、後のFLR成功を遮断確認の代用にしない。実ページ解放側もunmap未確認または再利用不可ならPMMへ返さず、claim参照の寿命とページ保全を分離している。
- 遮断未確認domainは共通層のheldリストに残り、groupの新claimを拒否する。claim記録だけを捨てて新ownerへ割り当て直す経路ではない。

一次PCIe 6.0 §6.4、印刷p.717、抽出txt 40184–40246行はnon-posted完了、TC0のhost read、他TC/RO/IDO、P2P同期を区別する。したがってclean closeだけで汎用posted DMA drainを立証できないという第二修理の根拠を維持した。

### 2. 新primaryだけFLRすれば旧group全体を再利用できるのではないか

一次§6.6.2、p.724、40600行は対象Functionだけに作用すると定める。`claim_recover` の全member処理を読み、primary交代とdirty非primaryを別々に追った。

- fresh peerにもBMEoff/readbackが必要。停止未確認のfresh peerはdirty化してそのclaimを失敗にする。既にdirtyなら、BMEclearが効かなくても仕様準拠FLR後の停止を確認できる経路は保つ。
- dirty memberはMSI/MSI-X Enable無効化のreadbackとIOMMU側clearを確認してからFLRへ進む。`pci_msi_disable` は装置がEnableを拒んだ場合もclearを試みるが、成功とは返さない。旧EnableをFLR保存状態へ持ち込むことや、未確認vectorの再利用を許す根拠にはならない。
- dirty display peer・FLR不能peerにはnative例外を適用しない。失敗・未準備・取消ではhandleを公開せず、domain解放経路でgroupを再遮断する。
- boot display/無FLRのnative回復委任は新primaryだけで、そこでもBMEoffが必須。`N_PCI_NEEDS_RECOVERY` を保持する。generic FLRでscanoutを再建したという契約にはなっていない。

### 3. 取消後のFLRで保存状態を取り直す、または予約を落とすのではないか

`pci.c:pci_function_reset` とclaimの呼出順を再読した。

- 保存状態・開始時刻・必要待機はdevice所有の `d->reset` に置く。再開は同じ保存状態から残待機を行い、二度目のFLRや保存し直しを行わない。保存CommandのBMEとInitiate FLRを除き、復元後のBMEも読戻す。
- memberに保留resetがあればBME/MSI操作より先に再開する。`guards_init` は回復後。primaryの `claimed` とattached domainによる全member予約は回復sleepからfile確保まで維持する。共通の `iommu_in_use/group_check` がpeerの別claimも拒否することを確認した。
- readiness/pending不成立ではBAR等を復元せず状態を残す。Completion Timeoutは表7-34の定義値、RTRはValidと表7-280の範囲を使う。予約値に64秒を捏造せず、PF全体へ一律1秒を課していない。
- 一次§6.6.2、pp.725–727、40662–40753行は100ms、stale Completion、FRS、RRSとRequest破棄を分けている。実装例の同期手順を、全FLRの全configアクセスを一律禁止する新しいmustへ読み替えなかった。

**残る境界：** 実 `iommu_attach` はFLR再開より前にgroup/ACSを調べ、能力・controlを読める。`acs_set` には必要時のcontrol書込みもある。PCI fixtureのattachは模擬なので、全共通probeの無アクセスは証明しない。この限界を一律config禁止や無関係deviceの待機で隠していない。

### 4. 初期化/復帰失敗からplain IRQやPCI DMAを開き直すのではないか

共通 `iommu.c/h`、IntelのIRREQ集約、`pm.c`、実 `idt.c`、`ioapic.c` と該当モデルを再読した。

- 初期化はPIC/IOAPICをmaskしてbackendと経路を変更する。IOAPIC変換はboolで失敗を伝播する。停止確認後のplain rollbackも `!interrupt_remapping_required` が必要。
- `restore_failed` はclaim、新MSI message、MSI clear、IOAPIC変換の成功を拒否する。Intel probeが実ECAP.IRREQを共通契約へ集約することも読んだ。一次VT-d §5.1.4、抽出4623–4628行はIRREQ=1/IRES=0のinterruptをblockすると定めるため、停止確認だけでplain配送可能とはしない。
- 実sleepの復帰はPIC/IOAPICを再maskし、IOMMU確認後にIOAPIC→PIC→PCIを戻す。IOMMU復帰失敗ではPCIを再開しない。中止sleepは `pic_restore_mask` だけで、ICW1を再発行しない。
- `ioapic_remap` は形式移行失敗時の元要求と、新route失敗時のmasked要求を区別する。sleepの保存RTEとmaskは実値を保持する。ここから全実機edge・全register bank・pending FLRを含む実S3成功までは推定しない。

## 既存実行証拠の独立再読

今回自分でcompiler、host test、build、QEMU、Hyper-Vは実行していない。以下は保存ログとfixtureの照合であり、自分の再実行結果ではない。

- `tests/pci_hosttest.c` は実 `pci.c/pciuser.c` を含み、二機能CF8/CFC、clock、IOMMU応答等を模擬する。修理前 `iommu-pci-live-before.log` の15失敗、修理後 `iommu-pci-live-after.log` と主担当の `pci-final-hosttest.log` の失敗0を全文読んだ。
- `iommu-pci-live-fixture-order-failure.log` の1失敗も残っている。現fixtureは判定後に次caseを初期化しており、期待値を弱めていない。保留FLR、MSI/MSI-X拒否、primary交代、予約、native例外、file確保失敗をソースで照合した。IOMMU attach/block、実PCIe ordering、実scheduler競合は模擬の範囲外。
- 主担当の `common-live-hosttest.log` は84判定、`pm-live-hosttest.log` は51判定で失敗0。後者は実PIC/IOAPIC/PMコードを使うが、PIC/IRRと一つのRTE windowのモデルであり実配送ではない。
- 主担当のAMD/Intel `*-final-serial.log` を再読し、両方のisolated=yes、`pcitest`/`pcitest flr`/`nvrmtest`/`nvrm_servetest`失敗0、native ABI 225判定・失敗0、二機能groupのログを確認した。EDU禁止DMAの内容不変と、Intelの対象IOVA fault、AMDの遮断interrupt eventを区別する。AMDを無例外と書き換えない。
- 主担当が撮影・目視・実PS2入力を行った画像の存在や説明を、本担当の操作・目視へ昇格させない。実QEMU成功は実RTX 4070の回復や実機IOMMUの代用ではない。
- `media-content.log`、`app-content.log`、`vhd-compare.log`、`vhdx-compare.log` を読んだ。主担当の記録ではIMG/ISO/試験scratchのkernel/initrd SHAが一致し、VHD/VHDXの内容もIMGと一致する。照合済みscratchと展開コピーは回収済みであり、保存済み・再実行可能とは報告しない。

## 引渡しと未解決

本第二検討では修理追加を要求する確定反例は得ていない。旧報告・元失敗を保全したまま、最新依存ソースを付けたround 2を一度提出し、主担当の独立受入へ渡す。

実機DMA/IRQ、実fabric drain、firmware/SMMの所有権、実NVIDIA/4070のreset・scanout、S3/hibernate、Hyper-V、EIME/IRREQ実機、性能は未受入。nvrmの同一appによるidle接続/thread占有と公平性は未解決であり、127接続処理とthread限界での拒否成功はその解決ではない。根拠のないquotaや追加の人工上限は入れていない。他OS本体・Linux・外部NVIDIA `uvm_common.c` は参照せず、GPLv2-onlyコードも追加していない。
