# Intel VT-d 実検証回帰の第一報告（2026-10-21）

## 開始点と範囲

- 担当ID `intel` を継続。旧第一・第二報告は変更していない。旧第一のSHA-256は `8e520bf50d59bd56df4c42009ee69cd47c28896f4d0def15de0b485042f22d44`、旧第二は `74b128443d7c011681c24f14904c895d8d30ed4244783813a9e46ab6f470324e`。
- 主担当の最新媒体・実QEMU Intel VT-d/EDUの `build/iommu-repair-20261021/intel-serial.log` で、`did not turn on compatibility-format interrupts (CFI)` と `not used` が観測された。これは今回追加したCFI一時有効化必須条件による回帰。EDUはclaimできず、ABIの別検査成功をIOMMU成功とみなさない。
- 製品所有は `kernel/src/dev/iommu_intel.c`、検査所有は `tests/iommu_intel_hosttest.c`。共有ABI・AMD・PCI・PM・IOAPICは上書きせず、主担当と移行契約を調整した。重いビルド、QEMU、媒体、Hyper-Vは操作していない。
- AGENTS.md、指定スキル、既存map、現在差分を確認。phase開始gateは同時controller更新と別担当の変更で一度ずつ止まり、主担当の同期窓で `ALLOW work` を確認した。その後は独立所有編集のたびに全tree gateを反復していない。

## 仕様と反例

一次資料はIntel VT-d Rev.5.20、抽出 `build/iommu-intel-spec-20261021.txt`。他OS・Linux・QEMU実装・NVIDIAの `uvm_common.c` は参照していない。

- §5.1.4（抽出4623–4653行）、§11.4.4.1（15329–15339、15495–15507行）では、CFIは互換形式IRQをIR tableから迂回させる制御であり、IR開始のために1を経由する要件はない。永久0で互換形式を遮断する動作を、隔離不能と判定するのは誤り。
- 一方CFI解除が未確認なら、翻訳なしのIR-onlyへ緩和して成功させない。command timeoutの永続 `command_failed` を解除して再試行することもしていない。
- 旧前提「初期IOAPICは全pin masked」「ioapic_suspendがmaskする」は実ソースと一致しない。初期化は既存の非mask ExtINTを残し、suspendは保存だけ。IRE後のplain pinは互換形式として遮断され、§5.1.4.1の非回復faultになる可能性があり、後からの再配送は保証されない。
- 読取り専用の独立仕様レビューも同じ反例を提示した。主担当へ通知し、PIC/IOAPIC停止と変換の失敗伝播を共通責務へ戻した。独立レビューの同意を実機証明にしていない。

## 製品変更

1. `units_on(bool cfi)` を `units_on(void)` へ統合。root/IR tableと必要な無効化の完了後、**全unit**のCFI解除を確認してから、どのunitのIRE/TEも有効化する。
2. 一時CFI1、起動時と復帰時の方針分岐、後置clear、非翻訳ならCFI解除失敗を成功扱いする分岐を撤去。失敗latch、未知queueの保持、旧DMA drain修理は維持する。
3. 最新の主担当契約では、IOAPIC変換は `iommu_init()` がbackend start直後に一回だけ行い、boolの失敗を伝播する。Intelの `units_start()` はbackendの表/CFI/IRE/TEのみを担当し、IOAPIC変換呼出しを持たない。
4. 主担当は初期化時のPIC mask保存・全PIC mask・IOAPIC mask、S3保存後と復帰直後のmask、IOMMU復帰→RTE復帰→PIC復帰の順序を共有層で修理中。maskだけ戻す処理とPIC再初期化を区別する。これらはIntel所有の変更ではなく、最終ソースの独立再読が必要。

## 検証と限界

- CFI応答を可変・永久0・永久1として模擬し、旧製品で22失敗を観測した。永久0の拒否、IRE中の互換形式bypass、IR-only時のCFI解除失敗の緩和を含む。試験結果を弱めるためのskipは追加していない。
- 修正版に、firmware初期CFI1/own0/永久0、全unit解除順、後方unit解除失敗、解除失敗後のresume/shutdown、x2APIC、条件付きmasked pinのS3、非mask ExtINTの反例を追加。主担当の共通責務への統合後も実行し、`build/iommu-intel-live-common-boundary.log` は失敗0・終了0。
- 検査のS3は「callerがpinをmaskし、backend成功後に保存済みremappable RTEを復帰する」条件の模擬。実際のIOAPIC/PIC書込み、固定edgeの無損失、実機S3を証明しない。実ExtINT反例は隠さず、rootの共有修理で取り扱う。
- 初期修正版のtarget freestanding構文検査と `git diff --check` は終了0。共通境界を最終化した後の最終検査は第二報告へ分離する。
- Windows clang 18.1.8、UB trap付きの製品C直接include試験であり、実機DMA・実割込み・QEMU EDU成功へ昇格させない。

次はCFISの適用条件とx2APICの独立反証、最終共有ソースの再読、最終focused検査。その後、主担当が増分ビルド・最新媒体・単一QEMU EDUで元回帰を確認する。
