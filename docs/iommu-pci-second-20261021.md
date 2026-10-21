# PCI・FLR修理：第二検討（2026-10-21）

## 範囲と差分の保全

- 担当 `pci`。第一報告 `docs/iommu-pci-first-20261021.md` を全文読み、変更せず保全した。第一報告の依存ソースが今回の修理で変わることは履歴であり、過去の確認を最新ソースの受入へ流用しない。
- 今回の追加製品変更は `kernel/src/dev/pciuser.c` の解放時DMA保全だけ。追加検査は既存の `tests/pci_hosttest.c` へ統合した。共有ABI、syscall番号、NVIDIA実装、他担当のバックエンドは編集していない。
- 開始時の共有差分と既存 `AGENTS.md` 差分を保全。mapの旧factを主担当へ通知した。訂正は主担当が行い、こちらの競合した編集は適用されていない。全担当の短い同期窓で `repo view → read → check --stage work` が終了0・ALLOWになってから製品を編集した。
- 実機が対象。他OS本体・Linux・外部 `uvm_common.c` は参照していない。新しい固定quotaや人工的な待機期限、GPLv2-onlyコードは追加していない。重いビルド、QEMU、Hyper-Vは実行していない。

## 一次仕様で覆った仮定と追加修理

指定の PCI Express Base Specification Revision 6.0 を抽出した `build/iommu-pci-spec-6.0-20261021.txt` で再照合した。

- §6.4、印刷p.717：Transactions Pendingはnon-posted要求の完了を示す。TC0のhost readによるflushを、他TC・Relaxed Ordering・ID-Based Orderingへ無条件に拡張できない。P2Pのposted到達にも装置・driver固有の同期が必要。
- §7.5.3.5 表7-22、pp.965–966：pending=0はposted DMA全体のdrain証明ではない。FLR自体もこのビットをクリアする。
- §7.5.1.1.3 表7-4、pp.918–919：BME=0は新しいMemory/I/O要求を禁止するが、先に発行された全posted writeの到達確認を提供しない。

このため第一修理の `dma_unsafe = running && !blocked` には反例がある。driverがBMEを切り、pending=0でclean closeしても、汎用カーネルは全posted要求の到達を確認していない。その状態を安全とみなして残存DMAページを再利用していた。

`claim_release` を **`dma_unsafe = !blocked`** へ修理した。確認済みIOMMU遮断だけが、claimを超えて残るDMAページの再利用を許す。非隔離または遮断未確認ならclean closeでも保全し、理由をログに残す。回復要否の `needs_reset` 判定とは分離し、clean closeへFLRや新しい待機を追加していない。生存中claimでdriver自身が明示的に破棄するDMAオブジェクトの契約を、新たに万能のdrain保証とはしていない。

主担当へ旧文書の200ms・1秒・解放内FLRとscanout保証の不一致を通知した。主担当がnative ABI文書、公開header、NVIDIAコメントを訂正した。GPUをgeneric resetした、画面を再建した、という結論は出していない。

## 保留FLR・取消・再claimの反証検討

- `pci.c` の列挙配列は `pci_init` 中だけ伸び、その後のdeviceポインターは安定する。保留 `pci_reset` は列挙済みdeviceが所有し、取消されたappのstackやclaimの寿命に依存しない。
- FLR前の保存状態を `d->reset` に公開し、開始時刻と仕様上の必要待機を保持する。再呼出しは保存状態を再取得せず、FLRを再発行せず、残り時間だけ待つ。RRS、取消、復元後BME残留では保存状態を失わない。
- 呼出側はFLRのsleep中に `d->claimed` を予約する。失敗時は新しいhandleを返さず、新domainを遮断・解放へ回し、保存状態だけをdeviceへ残す。ここは呼出経路の静的確認であり、複数taskの競合を実行した証拠ではない。
- 保存CommandからBMEを除外し、復元後もBMEを読戻す。IDが一致してもreset後pendingが残ればBAR等を復元しない。
- §6.6.2、pp.725–727では、FLRの100ms、RRSで延長される装置固有初期化、stale Completionの待機を別に扱う。post-reset pending=0はFLRが消しただけでも成立するため、旧要求やposted DMAのdrain証明には使わない。
- §7.5.3.16 表7-34、p.995の定義値だけをCompletion Timeoutとして使う。64秒は定義された1110bに限る。無効・予約値でpendingがある場合に64秒を捏造しない。
- §7.9.16 表7-280、p.1267の有効なRTRだけを使う。広告のないPFへ一律1秒の制限を課さない。未準備なら保存状態を残して戻る。

以上のFLR製品実装は第一修理を維持した。今回加えた反例では、IDは準備済みでもpost-reset pendingを残し、復元保留、BAR未復元、次回の同一保存状態からの復元、FLR回数が増えないことを確認した。

## 最新の共通DMA/native境界（読み取り確認）

- `sys_dma_create → pci_user_dma_limit → iommu_dma_limit` はnative syscall経路で接続されている。後者はbackendの実IOVA/PA幅、domainのindex幅、PTEの物理フィールド幅の最小から排他的上限を返す。`N_DMA_32BIT` はその上限をさらに狭める。割当後に上限を照合し、切り詰めて別の物理番地へ写さない。
- 連続領域と幅制限のある1ページ割当は低番地から検索する既存PMM経路へ接続し、共通hardware table割当は通常の `pmm_alloc` を使ってから、実幅に合わない時だけ低番地検索へ戻る。
- `iommu_map` は全入力の幅を先に確認し、app DMA objectとfirmware予約の重複を拒否する。予約範囲の最終access区間についてのみOS所有RAMとの交差を判定する。leafをtableとしてたどる経路も防護している。割当失敗が成功・非隔離へ化ける `r=0` 経路は修理済みの現行ソースを読んだ。
- `shared_release` はunmap未確認または `pci_user_dma_reusable=false` なら物理ページをPMMへ返さず、claim参照を解放する。今回の保全方針が実際のページ寿命の境界へ届くことを確認した。
- `start=false` 後は `shutdown` の停止確認を行い、未停止をlatchする。S3再開失敗ではPCI/IOAPIC再開前に停止し、休止復元のshutdown失敗ではlive tableへ上書きせず再起動する。未知TSCの周波数推測は撤去され、cache flush単位はCPUID由来になっている。
- 最新 `iommu.h` のstart説明は、falseでも停止保証せずcallerがshutdown確認を行う契約へ訂正された版を読んだ。headerの最後の変更は説明だけであり、下記のPCI実行検査後に追加された動作変更としては扱わない。共通/backendの実行結果は主担当の独立確認であり、このPCI担当のホスト検査範囲には含めない。

## 実行した確認と失敗の記録

1. `tests/pci_hosttest.c` は実 `pci.c` と実 `pciuser.c` をincludeする。CF8/CFC、clock、割込み禁止、IOMMU遮断の応答、BAR取消しの呼出境界を模擬した。非隔離・遮断失敗・遮断成功のclean closeを比較し、前二者で再利用不可、成功時だけ再利用可、sleep/FLRなし、BAR取消し、claim返却を確認した。実PTE取消しやhardware drainは模擬していない。
2. 保存BMEの再enable拒否、復元後BME残留、RRS、取消済み開始拒否、取消後の残り待機、未知Completion Timeout、RTR、post-reset pending反例も実行。最終MinGWホスト検査は終了0、**失敗0**。`build/iommu-pci-second-hosttest.log`。
3. `clang --target=x86_64-unknown-none-elf` と正規kernel相当flagsで `pci.c/pciuser.c`、正規user相当flagsで `tests/pcitest.c` の構文確認：終了0。`build/iommu-pci-second-target-syntax.log`。担当ファイル限定 `git diff --check`：終了0。
4. ホストcompilerのsandboxアクセス拒否を、許可された `require_escalated` の既存toolchain実行へ切替えた。`pciuser.c` 追加後の未使用経路のlink依存で一度失敗し、LTOでも解消しなかった。未検査経路を成功値で代用せず `abort` するstubを補い、通常の非LTOコマンドで成功した。これらは検査基盤の失敗である。ホストのLLP64 format差だけを `-Wno-format` とし、製品targetのformat警告は有効のまま確認した。

## 未解決・受入の限界

- `nvrm_serve.c` はHELLO認証前に接続ごとthreadを作り、idle接続でもthreadを保持する。同一appが接続を増やしてdaemonの実thread資源を占有できる問題は未解決。principalごと単一threadへ直すと、別connectionでのfence解除を必要とするRM待機とdeadlockし得る。
- 改善の候補はidle接続をnative eventsのreactorへ統合し、RM実行時だけ実資源に基づくworkerへ渡すこと。長期待機の取消・進行保証と主体別公平性も必要で、reactorだけで全DoS解消とはしない。根拠のない固定per-client quotaは追加していない。NVIDIAは本担当の編集範囲外。
- 実QEMU・EDU/virtio、実機のPCIe ordering/IOMMU/NVIDIA回復、scanout、GUI応答、Hyper-Vは本担当では未検証。ホスト検査の成功はその代用ではない。主担当の最新媒体による受入へ引き渡す。
- 保留resetが再claimされなければ保存状態をdevice lifetimeで保持する。隔離未確認のDMAページも意図的に保全する。これは回収済みとの報告ではない。
- 主担当はこの報告と差分を独立確認し、実QEMUで失敗が出た場合は新しい修理cycleで対処する。提出後は本担当から製品を編集しない。
