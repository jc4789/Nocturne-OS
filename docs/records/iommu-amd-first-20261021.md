# AMD IOMMU 第一検討・修理（2026-10-21）

## 範囲と根拠

- 担当ID: `amd`。独立した第一検討。製品編集は `kernel/src/dev/iommu_amd.c` のみ、検査追加は `tests/iommu_amd_hosttest.c`。
- 開始時の既存変更 `AGENTS.md` を変更していない。共通層・Intel・PCI・ABIは各担当の所有。全体ビルド、QEMU、Hyper-V、通常data媒体を操作していない。
- 一次資料: `D:/Programes/OS DEV INFO/48882-3.10.pdf`（AMD 48882、改訂3.10、2025年2月）。既存Calibreの `pdftotext.exe -layout -enc UTF-8` で `build/iommu-amd-spec-48882-3.10.txt` に抽出して照合。他OS・QEMUのソースは参照していない。
- J-spaceは指定のxhigh。resume、repo view、work gateを実施。並行担当の変更によるstaleとpending broadcastで一時停止し、root同期と自身のpulse後に `ALLOW work` を確認して独立編集phaseへ入った。独断のmap同期はしていない。

## 一次仕様による判定と製品修理

1. **ファームウェア除外範囲の全体迂回を修理。** §3.4、MMIO 0020h（p217）のExEn/AllowはControlとは独立。旧処理のControl=0ではAllow=1が残り、対象範囲のDMAはclaimed/blocked DTEを迂回する。新処理は再有効化前にExclusionBaseを0にし、readbackで無効化を確認する。解除できなければ新テーブルを設定せず失敗を返す。予約IVMDは共通層のidentity mappingで扱い、全体bypassへ置き換えない。
2. **提示されたevent-log再開指摘は不成立。** §2.5.1（p141）を照合。現行はEventLogRun=0を待ち、EventLogEn=0、ポインター、Overflow W1C、EventLogEn=1の順であり維持した。停止確認前にtailを書かないこと、停止したcommand processorを付随的に再始動しないことを検査した。
3. **PerfOptのoffset/bit/gate指摘も不成立。** §3.4.9（p237）とEFR（p220）では016Ch、32-bit bit13、EFR bit45が正しい。指定自体は維持。予約bitを再書込みしない0書込みとreadbackを追加し、解除不能なら失敗する。
4. **実機アドレス幅と変換モード。** §3.2（pp203–204）のVAsize/PAsize、IVinfo（Table 86）、EFR.HATS（p221）を照合。全unitの最小幅を `iova_bits`/`phys_bits`、最小対応host depthを `levels` に設定する。予約値・HATDisは拒否。4階層固定を除き、5/6階層対応機の高位RAMを切り捨てない。DTE/command/event/semaphore/IRTの確保はrootの `iommu_alloc_pages` へ統合した。NextLevel=0/7をleafと判定するcallbackを追加した（§2.2.3、pp79–80）。
5. **IVRSの完全性と所有境界。** §5.2.2、Tables 88–111を照合。短いIVHD11/40、壊れたentry/UID長、未終端・逆転range、IVMDの加算/丸めoverflowを途中成功にしない。非zero PCI segmentを両passとper-device IVMDで除外。IVHD11/40のEFR imageとHATDisを使用し、legacy MMIO EFRより詳細な記述を優先する。aliasの実requesterもcoverageへ加え、ACPI名付きplatform requesterとIOMMU自身のIDをPCI claimから保護する。
6. **IVMDの権限。** §5.2.2.2（pp302–304）はIR/IW=0を非mappingとし、Unity=0は任意の変換を許す。以前捨てていたUnity=0のIR/IW、およびaccess=0を共通層へ渡す。permission intersection・DMA再有効化禁止はroot側の修理と接続する。
7. **複数unitを含むfunctionのevent MSI-X。** §3.1、Capability+10h、Table 91を照合。共通functionのMSI-X tableは一度だけ初期化し、各unitのMSInumへ配送する。IRQ routeができる前にEventIntEnを立てない。MSIのみの既存経路は維持する。
8. **回復と寿命。** §2.4/§2.4.1（pp122–125）、§2.4.2/3/5/8、§3.4.16を照合。DTE変更後のDTE/IRT/domain invalidation、全domainのPDE+S invalidation、COMPLETION_WAIT、command停止時のそのsync失敗を維持・検査した。resume/shutdownはboolで全unit確認を返し、途中失敗でもテーブルを解放しない。復旧失敗後のPCI再開阻止・failure latchはroot担当へ接続した。

## 実施した軽量確認

- 製品用targetの `clang --target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror ... -fsyntax-only kernel/src/dev/iommu_amd.c`: 終了0。
- `clang -std=gnu11 -O1 -g -Wall -Wextra -Wno-format -Wno-unused-function -Wno-unused-parameter -Ikernel/src -Icommon tests/iommu_amd_hosttest.c -o build/iommu-amd-hosttest.exe && build/iommu-amd-hosttest.exe`: **57判定、失敗0、終了0**。
- hosttestは製品Cを直接includeし、MMIO/PCI/メモリー境界のみをモデル化する。旧Control=0の負の対照、除外/PerfOptの解除・lock拒否、event restart順と停止未確認、alias/F0/IVMDの正負例、IVHD feature/width、DTEとcommand encoding、command fault後の失敗と再発行、shutdown/resume失敗時の所有維持、共有MSI-Xを確認した。
- 最初のnative host compileはWindows LLP64とNocturne LP64の差で既存 `%lx`/uint64_t に17警告が出た（終了0、52判定は成功）。hostモデルのkprintfはformatを実行しないため最終host compileだけ `-Wno-format` を指定。製品targetはformatを含む `-Werror` で別に確認した。製品の警告を抑えて合格にしたものではない。
- `git diff --check -- kernel/src/dev/iommu_amd.c tests/iommu_amd_hosttest.c`: 終了0。
- 最終ログ保存時に同一条件の検査を一度再実行した。これは追加の独立した検証ではなく、同じ証拠として扱う。その後の製品変更はIVRS解析コメント2箇所の訂正だけで、実行結果を新しい動作確認へ水増ししない。

## 未確認・次の受入

- host MMIOモデルは、実DMAの停止、実fabricのin-flight drain、実CPU cache coherency、実MSI-X、S3/hibernate、NVIDIAを証明しない。
- QEMU EDUの実製品経路、最新媒体への格納、単一VMでの実画面、実機・Hyper-Vは未実施。重い確認はroot専任。
- rootの共通層とPCI修理を統合した実build、共通幅・予約権限・PM fail-stopの独立確認が必要。SMIはDTEでは遮断できないという既存仕様上の境界を変更していない（§2.1.5、Table 10）。
- 次はrootの差分確認・統合検証と、別委任による第二検討。本第一報告で編集phaseを終了する。
