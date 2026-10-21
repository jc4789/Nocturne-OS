# AMD IOMMU 第二検討・追加修理（2026-10-21）

## 目的・保全・範囲

- 担当IDは既存の `amd` を継続。第一報告 `docs/iommu-amd-first-20261021.md` の全文と製品差分を読み、第一報告の最も弱い前提である「完了待機の再試行」「部分起動失敗時の所有」「共通予約権限」を反証する第二検討とした。
- 第一報告は上書きしていない。同報告のSHA-256は `000f2a775f06a15f6177601bb1f3a159ca84f5b0119979ef41ab9297d33f5a8b`。第一報告が依存する製品ソースは今回変わったため、その古い検証結果を最新ソースの証明として再利用しない。
- 製品の追加編集は `kernel/src/dev/iommu_amd.c`、追加検査は `tests/iommu_amd_hosttest.c` のみ。共通層は独立した読取り検討・rootへの修理提案に留めた。既存の `AGENTS.md`、別担当の製品差分、ABI、生成媒体には触れていない。
- J-spaceは既存xhighの責務分担を継続。自身のpulse/read、現行mapのrepo view、`ALLOW work` を確認してから編集した。独立所有内の並行編集を理由にgateを繰り返していない。map同期、重いbuild、QEMU、Hyper-V、通常data媒体はrootの担当のまま。
- 一次資料はAMD 48882-PUB、Rev 3.10、2025年2月。ユーザー許可の資料から抽出済みの `build/iommu-amd-spec-48882-3.10.txt` を再照合した。他OS、Linux、QEMU本体のソース、NVIDIAの `uvm_common.c` は参照していない。

## 1. 完了待機の反証と追加修理

根拠は§2.4/§2.4.1、Figure 44/Table 34（pp123–125）と§2.4.11（pp138–139）。command headが進んだことは、並列実行され得るcommandの完了を証明しない。完了storeは64-bitでcoherentに実行されるため、Device Tableの非coherent設定と同一視して別のcache操作を足していない。

製品Cを直接含む既存hostモデルへ、完了storeを保留し、前回のstoreだけを次の待機中に配送する条件を追加した。

1. 今回のstoreが未実行なら、headが進んでも `sync()` は失敗する。
2. timeout後の前回tokenのstoreが次のwordへ到着しても、今回tokenと異なるため `sync()` は成功しない。
3. その後、今回tokenのstoreが実行されれば成功する。
4. 一方、`token == UINT64_MAX` からの増分では0へwrapし、未完了wordの初期値0と一致して、store未実行でも成功していた。

4の境界反例は修理前の追加モデルで再現し、**64判定中1失敗、終了1**だった（`build/iommu-amd-second-before.log`）。これは実機で2^64回の同期を観測したという報告ではない。有限幅フィールドと未完了sentinelの衝突を強制入力で検証したもの。

追加修理は `sync()` のtoken更新で0だけを飛ばす処理。新たな回数上限、期限、固定容量を設けていない。遅延storeの所有を捨てる、queue headを勝手に進める、失敗を成功へ変換する修理でもない。最終モデルでは上記すべての条件を成功した。

`f=0`のCOMPLETION_WAITでは後続の独立commandが先行して進むことや、前の完了storeが後から見える可能性は仕様上残る。その古いstoreは今回tokenではないため誤成功にはならないが、遅延に伴う追加のtimeoutは起こり得る。確認できない同期を成功にする変更はしていない。実fabricの遅延・drainと継続的な性能は未測定。

## 2. 部分program・再開・停止とテーブル寿命

§2.4のcommand停止手順、§2.5.1のevent再開手順、Control/Status（pp215–216、250）を再照合した。

- 別々のMMIO領域を持つ二unitをモデル化し、第一unitは起動できるが第二unitのIommuEnが解除不能という反例条件を追加した。
- `amd_start(true)` の途中失敗と `units_off()` の一部失敗では、Device Table、semaphore、両unitのcommand/event領域の解放通知は0で、実装上の所有を保持した。
- その状態のresumeは、一方のprogram成功で他方の失敗を打ち消さずfalseを返した。shutdownも同様にfalseを返した。
- 解除不能条件を取り除き、全unitの停止を確認した後だけ `tables_free()` を実行し、製品定義から計算される全pageの解放と所有フィールドのクリアを確認した。
- 新たな製品修理は不要だった。boolの結果が共通層の `restore_failed` とPMのPCI再開阻止へ接続されることもソースで確認した。ただしPM/hibernateや実DMAの試験を実施したという意味ではない。

最終共通層では、初期 `start()` のfalseも「全停止済み」とは扱わず、追加の `shutdown()` が全停止を確認できないと `restore_failed` を保持する。後続の非隔離opt-in、resume、hibernate上書きでこの状態を忘れないことを再読し、共通hostモデルの失敗/全停止対照も実行した。headerに残っていた「falseで停止済み」という説明の矛盾をrootへ指摘し、callerがshutdownで確認する最終コメントへの訂正も確認した。共有headerは本担当では編集していない。

一方、probe自体がNULLを返し、既知hardwareの状態を確定できない初期引継ぎの境界は今回解消していない。start後の失敗latchの保証を、probe以前の未知状態まで拡張しない。

停止statusの確認は、実fabric全体のDMA停止をhostモデルで証明したものではない。実機のデバイスquiesce、in-flight処理、S3/hibernate復帰は引き続きrootの統合と実機受入が必要。

## 3. IVMDと共通予約権限の独立再検討

§5.2.2.2、Tables109–111（pp302–304）を改めて読んだ。

- IR=IW=0の範囲はdevice address spaceへmappingしてはならない。Unity=0はIR/IWを無効にする意味ではなく、任意のvirtual-to-physical変換を許す意味。
- ExclusionRange=1のときだけIR/IW/Unityを無視する。全体bypassを残す代わりに、claimのdomainへ必要なidentity mappingを置く第一修理とは矛盾しない。
- 共通層のaccess=0保持、完全一致重複のintersection、開始・終了点で区切った部分重複のintersectionは、禁止権限を足し戻さない。複数requester IDを共有domainへ集める場合も、該当する予約全体のintersectionとなる。
- 初期の共通修理が行っていた、制限予約のRW DMA割当による上書き拒否と、unmapでfirmware予約を消さない処理は権限上妥当。ただし恒久RW予約がallocatorのRAMを再露出する所有問題は別である。
- 最新差分ではrootが全予約へのapp DMA割当を拒否し、恒久mappingとOS-owned RAMの重なりを拒否する処理も追加した。権限があることとOS RAMの所有権があることを区別する方向は妥当。これらはAMD担当自身の編集・実行結果ではない。

**共通層への追加反例候補をrootへ通知し、修理を再読した。** 生のRW予約をRAMと比較するだけでは、NONE予約とのintersectionで最終mappingが消える部分まで拒否する。例としてRW `[0,8p)`、NONE `[2p,4p)`、OS RAM `[2p,4p)` では、最終mappingはRAMを含まない。安全性はraw予約ではなく、最終cut片のaccessとRAMの交差で判定する提案を渡した。最新共通ソースは `reserved_map()` の有効片（access>0）にだけ `reserved_piece_safe()` を適用しており、この過剰拒否の反例候補はソース上解決したと確認した。boolからerrnoへの結果変更とattach側の伝播も追跡した。

共通hosttestに対して、部分重複、複数requester ID交差、level6の入力も独立受入で加えるよう提案した。rootが追加したその試験本文も自分で読んだ。NONE片にのみOS RAMが重なる成功例、RO片へのRAM重なり拒否、不完全RAM一覧の拒否を含む。所有外の試験ファイルを勝手に編集していない。

## 4. PTE・アドレス幅とlegacy IVRS

§2.2.3（pp79–84）、Capability+10h（pp203–204）、EFR.HATS（p221）、IVinfo/Table86とIVHD10/Tables89–92を照合した。

- NextLevel=0/7をleafとするAMD callbackは仕様に一致する。共通treeの解放はleafを下位tableとして辿らない。
- level6の実際のshiftは57。`shift_of(levels)+9` は66になるが、その値で64-bit shiftを行わず、ハードウェアのiova/physical幅との最小値を取り、さらにPTE物理fieldの52bit以内にするので、この入力で未定義のshiftは発生しない。
- Table/leaf addressのbits51:12、AMD physical幅40/48/52、共通のaddress preflightと確保後幅確認は整合する。アドレスをmaskして別pageへ化けさせる処理を認めていない。
- 初回読取り時の `entry()` は中間leafを検査せず辿る余地があった。ただしclaimは4KiBのみ、Intel identityは一度だけ作成するため、既存利用経路での反例までは示していない。rootへ防護を提案し、最新差分ではlarge-leaf domainで中間leafを拒否する追加を確認した。
- 最終共通層の単page確保は通常の `pmm_alloc()` を使い、実physical幅を外れた場合だけ低位contiguous探索へfallbackする。全page確保ごとの低位全走査を増やさず、幅の切捨ても行わないことをソースで確認した。
- legacy IVHD10、EFRSup=0、IVinfo 48/48、Capability+10h 48/48は正規入力としてprobe成功した。Capability側が40/40なら共通幅との最小値40/40を使い、予約physical幅41は拒否した。新しい厳格parserが有効なlegacy記述を一律に拒否するという仮説には、この条件で反例を得た。
- これはQEMUの実IVRS/Capability値を取得した検証ではない。QEMU本体ソースから値を推測していない。現実のQEMUの公開値と実EDU DMAの確認はrootの単一VM検証に委ねる。

## 5. event再開・PerfOpt・SMIの再判定

- event overflowの再開順は、EventLogRun=0確認、EventLogEn=0、pointer設定、Overflow W1C、EventLogEn=1で§2.5.1に一致する。停止を確認できない場合のpointer再設定や、event再開に伴う停止command processorの勝手な再開をしない既存検査を維持した。元のevent再開指摘を無条件に成立扱いしない。
- PerfOptはMMIO016Ch、32-bit bit13、EFR bit45であり、§3.4.9の指定に一致する。offset/bit/gateを変更する根拠は得られなかった。予約bitを0で書き、解除をreadbackする第一修理を維持した。
- SMIは§2.1.5/Table10の別境界で、DTE/IRTによる通常DMA・IRQ隔離をSMI遮断の証明へ拡張しない。firmware用のSMI filterを無根拠に書き換えていない。
- 最終共通層は未知TSCを10GHz等と推測せず、readyを確認できないpollを失敗にする。共通モデルで未知TSCの判定を確認した。cache flushの固定64-byte走査はCPUIDが報告するline幅へ変わり、取得できなければ全体writeback/invalidateを使う。ただしこのhostモデルはcoherent backendなので、その実cache命令経路は実行していない。

## 最終確認・限界

- host compile: `clang -std=gnu11 -O1 -g -Wall -Wextra -Wno-format -Wno-unused-function -Wno-unused-parameter -Ikernel/src -Icommon tests/iommu_amd_hosttest.c -o build/iommu-amd-hosttest.exe`。
- 通常hostモデル実行は **79判定、失敗0、終了0**。結果は `build/iommu-amd-second-after.log`。64判定の修理前失敗ログを上書きせず残した。79判定には既存57判定を含み、79件の独立した実機試験と数えない。
- 最終の独立実行は、root指定の `./tools/msys64/ucrt64/bin/clang.exe` に `-std=gnu11 -O1 -g -ffunction-sections -fdata-sections -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon` と `-Wl,--gc-sections` を使用。AMDは **79判定、失敗0、終了0**（`build/iommu-amd-second-mingw.log`）、最終共通層は **61判定、失敗0、終了0**（`build/iommu-amd-second-common.log`）。両compileログに警告なし。共通層の部分overlap/level6/複数ID/allocator失敗/初期start失敗latchの正負例を本担当自身が実行した。
- 製品用targetはMakefileのkernel flagsに対応する `clang --target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror ... -fsyntax-only kernel/src/dev/iommu_amd.c` で終了0。ログは `build/iommu-amd-second-syntax.log`。ホストLLP64向けのformat警告抑制を製品検査へ持ち込んでいない。
- 最新編集後の `git diff --check -- kernel/src/dev/iommu_amd.c tests/iommu_amd_hosttest.c` は終了0。
- 途中の失敗も保持する。最初の通常host clangによるcommon検査は `tsc_hz` / `timer_ticks` の未定義参照（LNK2019/LNK1120）でlink失敗。rootがfixtureへstubを補った。さらに通常host clang/MSVC linkerはgc-sectionsをLNK4044で無視した（`build/iommu-amd-second-ubsan-build.log`）。指定MinGWへの切替初回はsandboxのアクセス拒否で未実行だった。許可された `require_escalated` で同コンパイラを実行し、上記最終の警告なし・失敗0を得た。これらを製品targetの不具合や未実行の成功へ取り違えない。
- mockはCPU/hardwareの実coherency、実DMA、PCI fabric drain、実MSI-X、性能、S3/hibernate、NVIDIAを証明しない。実build、媒体格納、QEMUの実画面/実EDU、実機・Hyper-Vは本担当では未実施。
- 次の一手はrootによる第二報告の独立差分確認、増分build・媒体・単一QEMU EDUでの統合確認。QEMU成功を実機成功へ昇格させない。本報告提出後の製品編集は終了し、実QEMUで新たな失敗が出た場合は別の修理cycleとする。
