# Intel VT-d 最終統合の独立第二記録（2026-10-21）

## 範囲・履歴・実施の区別

- 既存担当ID `intel`、J-space xhighの最終round 2。主担当からの別委任として、旧first/second、live-first/live-second/live-handoff、`iommu-intel-final-first-20261021.md` を全文実読した。六報告は上書きしていない。新第一記録のSHA-256は `aeed8b022ee5a885031222676435d44da4995902a850c83b5565e9961afc8e0c`。
- 開始時の共有差分とAGENTS.mdのnative ABI保全追記を確認し、保全した。主担当の全体記録 `iommu-repair-20261021.md`、completion、意味mapも実読した。PCIの第二記録と主担当の同期通知までは、文書・controllerを含め変更せず読取りだけで再考した。
- 最終窓で自身の `pulse resume`、`read --agent intel`、`repo view --agent intel`、`check --stage work --agent intel` を一度実施した。終了0、`ALLOW work` を主担当へ即通知した。巨大なview出力は `build/iommu-repair-20261021/intel-final-second-view.log` に保存した。
- 今回の編集は本記録だけ。製品・tests・意味map・旧報告・証拠ログは未変更。追加build、host試験再実行、QEMU、媒体照合、Hyper-V、通常data操作、追加子委任はしていない。他OS本体・Linux・QEMU実装・外部NVIDIA `uvm_common.c` は参照していない。コード、GPLv2-only素材、人工上限、ABI迂回を追加していない。

## 第一記録を疑った境界と独立照合

一次資料はユーザー許可済みIntel VT-d Rev.5.20の既存抽出 `build/iommu-intel-spec-20261021.txt`。SHA-256は `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。今回は原PDFを新たに開いたとは主張せず、抽出の本文と現ソースを照合した。

### 1. 「停止できたからplainへ戻せる」はIRREQでも成り立つか

§5.1.4、抽出4623–4628行は、IRES=0のときIRREQ=1なら割込みを遮断し、IRREQ=0なら互換形式で処理すると明記する。したがってshutdown成功だけではplain配送可能を証明しない。

- `iommu_intel_probe()` は全unitのECAP.IRREQをOR集約し、正常返却前に `intel.interrupt_remapping_required = irreq` を設定する。この一field伝播を現ソースで追った。
- `iommu_init()` は `stopped && !hw->interrupt_remapping_required` のときだけplainへのrollbackを行う。IRREQ必須、停止未確認、rollback失敗は `restore_failed` を残す。
- probeがNULLでもDMAR/IVRS宣言が存在すれば未確認状態を保持するので、fieldを返せなかった早期失敗を「IOMMUなし」と取り違えない。宣言自体がない通常の不在や、停止を確認できた非必須IRの未使用とは区別する。
- 新MSI、MSI clear、IOAPIC変換、claim、電源移行がこのlatchを迂回しないことを呼出境界で再読した。

共通モデルは「start失敗＋shutdown成功＋必須IR」でplainを拒否する対照を持つ。一方、Intelの `width_probe_case()` は幅を判定するだけで、IRREQ集約値の明示assertを持たない。**実probeから共通層までのIRREQ=1動作を実行で確認したとはしない。** この欠けた証拠を新たな確定製品不具合とも断定しない。

### 2. 一時CFI=1や全modeでのCFIS=0は必要か

§5.1.4の4648–4653行、§11.4.4.1の15495–15507行、§11.4.4.2の15634–15645行と、IRTAの17572–17580行を再読した。

- 一時CFI=1をIR開始前提にする根拠はない。永久0を拒否した旧回帰は旧live報告の失敗として保存されており、現製品にその必須化はない。
- EIMEはそれ自体で互換形式IRQを遮断する。現 `units_on()` のCFI解除条件 `remap && !x2` は、EIMEで不要なCFIS=0条件を課さない。
- xAPICでは全unitのCFI解除確認後に、どのunitのIRE/TEも有効化する。SIRTP→必要な無効化/sync→CFI/EIMEに応じた処理→IRE/TEの順序を追った。EIMEはSIRTPで有効となる契約に接続している。
- 「適用外なので準拠実機が任意CFISを返し、CFI書込みを無視してよい」という仕様反例は依然未立証。固定1モデルは不要条件を調べる意図的入力であり、準拠実機再現へ昇格しない。

### 3. 失敗後の再試行やfirmware QI停止で不確実性を消していないか

§6.5.2、§6.5.4、§6.9と現 `queue_check()`、`queue_quiet()`、`intel_shutdown()`、`intel_resume()`、`dma_draining()` を突き合わせた。

- IQE/ICE/ITEのdescriptor skip、queue異常のclear、`failed` / `command_failed` 解除はない。FSTS.PFOや通常fault recordの解放を、queueエラーの回復と混同しない。
- 全unitの異常検査と終端wait/empty確認が保護解除より先。semaphore未確保でactive QIを停止成功へ変えず、元IQA/QS/descriptor幅とin-flight slotの寿命を保持する。
- major1でDRD/DWD不足ならapp domain公開前にattachを拒否し、unmap/block成功を偽らない。major2の必須drainと、peer DMA/ATSまで証明できるという主張は区別する。
- S3後は旧 `queue_on` を現hardware所有権の証拠にせず、現在activeなqueueを再確認する。ただしfirmware/SMMのsole-producer引継ぎと、probe前にPMMがqueueを再利用しない保証は、この静的確認でも未証明のまま。

この再考で、追加の根拠ある重大反例は見つからなかった。これは全実機への安全認定ではなく、確認した境界について製品追加修理を要求する確定根拠が得られなかったという結論である。

## 共通IRQ・PMと既存証拠の再読

- 初期化はPIC mask保存→全PIC/IOAPIC mask→backend start→一回のbool `ioapic_remap()`→PIC maskのみ復帰。Intel backendに形式変換の二重所有はない。
- `ioapic_suspend()` は保存後mask、wake後にも再maskし、実際に眠った場合だけIOMMUを復帰する。その後IOAPIC→PIC→PCI。中止sleepではPICのICW再送を避けmaskだけを戻す。IOMMU復帰失敗は後続復帰前に停止し、hibernateはshutdown未確認のlive tableを上書きしない。実edge無損失・実S3を証明したとはしない。
- 主担当実行の保存ログ `intel-final-hosttest.log`（失敗0）、`common-live-hosttest.log`（84判定/失敗0）、`pm-live-hosttest.log`（51判定/失敗0）を実読した。自分が今回実行した試験ではない。
- `intel-final-serial.log` のVT-d起動、EDU隔離yes、禁止DMAの変換fault、pcitest/nvrmtest/group FLR/server試験の失敗0、native ABI 225判定/失敗0を読んだ。revoke後の子page fault、実NVIDIAではないEDU、thread limitでの33接続拒否も消していない。
- 同ログのECAPは `f00f4a` でIRREQ(bit62)=0。QEMU成功からIRREQ=1、EIME、実4070、S3、Hyper-Vの成功を推定しない。主担当のWHPX/4CPU/4096MiB/1080p/AHCI条件での画面目視、実キー入力、媒体一致、VM停止・scratch回収は主担当の実施。本担当は画像を開いたりVMを操作したりしておらず、自身の目視・実操作へ置き換えない。

## 現スナップショット・残件・返却

| 対象 | SHA-256 |
|---|---|
| `kernel/src/dev/iommu_intel.c` | `73b743817c2471bf01df7e27acda3ecc62dea5d9034f22121aea087f93b83a14` |
| `tests/iommu_intel_hosttest.c` | `eef44563da3f0dd8af5a8bd0719b10a1c92a54a549430ec6fec0318db630d1bb` |
| `kernel/src/dev/iommu.c` | `f3dea5f4f8de569fdbfda76d1af0c0e5bfc01458ff466d3fd0c6deee8187c66e` |
| `kernel/src/dev/iommu.h` | `622b99536fef07e7abb91443ad64eca654f4e9297d8b077a05a2c6bb08d818d5` |

主担当の全体記録にはIRREQ伝播の静的確認とIRREQ=0のQEMU限界が明記されている。全体記録/completionとも、実機未受入、nvrm同一appのidle thread占有、実GPU reset/scanout未解決を保持していることを確認した。firmware/SMM引継ぎ、初期queue寿命、実fabric drain、実edge、高位RAM、EIME/IRREQ実機、S3/hibernate、性能、FLR中のACS/group probe全体の無アクセスも今回解決済みにはしない。

本記録を新しい証拠として、現製品・モデルソース・一次抽出・保存ログへの依存付きround 2を一度提出する。次は主担当が本記録を独立受入し、最終map同期・必要な読取りreceiptと引渡し境界を整える。本担当の製品作業・試験は再開しない。
