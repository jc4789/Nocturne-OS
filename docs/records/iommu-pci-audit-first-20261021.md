# PCI追加監査修理：第一報告（2026-10-21）

## 範囲と保全

- 担当ID `pci`、共有treeの独立実装担当。開始時の差分は主担当の記録3ファイルのみで保全した。既存のPCI第一・第二・live第一・第二報告は上書きしていない。
- 指定skill、現在のmap、現行コード、一次抽出を読み、主担当の直列gate通知までcontroller・製品・試験・文書を変更しなかった。本人の `pulse resume → repo view → work check` が終了0、`ALLOW work` の後に編集を開始した。map viewは `build/iommu-pci-audit-map-view.log`。独立phase開始後に全tree gateを反復していない。
- 編集は `kernel/src/dev/pci.c`、`pci.h`、`pciuser.c`、`tests/pci_hosttest.c`、追加依頼の `tests/pcitest.c` と本報告。共通IOMMU、PM、scheduler、NVIDIA、公開ABIは編集していない。新しいsyscall・ABI迂回・外部実装・GPLv2-onlyコード・人工quota・推測期限を追加していない。他OS本体を参照していない。
- xhighの担当分担内で独立して反証した。新たな下位委任、重いbuild、QEMU、Hyper-Vは実行していない。

## 一次仕様と製品修理

一次根拠は `build/iommu-pci-spec-6.0-20261021.txt`（PCI Express Base 6.0）。規定値と装置固有・platform依存値を区別した。

### 1. CT不明・disabledでも、実際にTPが消える装置を回復できる

- §6.6.2、印刷pp.725–727、抽出40662–40753行：FLRは100ms、pendingの古いCompletionの待機とConfiguration-Readyは別。CT disabled時の100msは最低値であり、汎用の安全期限ではない。
- §7.5.3.16、印刷p.995、56153–56157行：CT Disableをclearしても、既存のoutstanding Requestsへtimeoutを適用することは任意。CTを有効にして既定値だけ待つ案は安全根拠にならず採用しなかった。
- `pci_function_reset` は、CTが不明ならBMEoffをreadbackで確認した後に、**FLR前のTP=0を実測するまで**取消・scheduler yield付きで待つ。その後だけFLRを発行する。CTの予約符号も架空の64秒等へ変換しない。
- BME stuckかつCT不明は依然拒否する。新要求の禁止を確認できず、TP=0の瞬間値も将来の古いCompletionを排除しない。既知のCTを持つBME stuck装置については、既存のFLRとCT待機・復元後BMEoff確認を維持した。
- CTを復元しても旧DMAページの安全性を推定しない。残存DMAの再利用条件 `dma_unsafe = !blocked`、全multi-memberのdirty化、個別FLR、未知peerの回復拒否は変更していない。

### 2. RTRなしのRRSを一度だけで拒否しない

- §6.6.2、抽出40715–40720行は100ms後も追加初期化のRRSを許す。§7.9.16、72395–72396・72584–72588行の有効RTRは広告時間として使うが、RTR不在のPFへ一律1秒期限を加える根拠はない。
- §2.3.1、12424–12431行のRRS Software Visibility（Vendor/Device IDのdwordは `FFFF0001h`）をnamed定義にした。
- 既存の仕様待機後はRRS中にVendor IDだけを取消可能に再照会し、同じ保存状態から復元する。装置不在・別ID・post-FLR pendingを黙って成功にしない。取消後の保存状態・開始時刻・残待機を維持し、再保存・二重FLRしない。

### 3. kill検査と長時間wait登録の間を原子的にする

- 旧経路には `killed` 検査後、`sleep_ms(remaining)` の登録前にkillされる余地があった。`sched.c:kill_locked` はその時点でBLOCKEDのtaskだけを起こし、`wq_wait_timeout` は登録前のkilledを再検査しない。既知CTの長い待機でwakeを取り逃す候補になる。
- 主担当と責務を確認し、共有schedulerを変更せず、既存 `pci.c` 内の `reset_wait` に `irq_save → killed再検査 → sleep_ms登録/schedule → irq_restore` を統合した。CT/FLR/RTR、TP、RRSのwaitが共用する。短い固定sliceや新しい期限では隠していない。
- この根拠は現在のBSP claim/kill前提。`schedule` は別taskのstackへ切り替わり、そのtask自身の `irq_restore`、新threadの `sti`、または割込み復帰を通る。PCI callerのIRQ guardのまま全PCをbusy waitさせない。`syscall_dispatch` はclaim呼出し前に `sti` する。これはソース確認であり、実IRQ・GUI応答の実測ではない。

### 4. named PCI責務への統合

- 重複していたBME clear/readbackを `pci_disable_busmaster` として `pci.c/h` に統合し、claim解放・回復・FLRで共用した。narrow Command writeと装置ID/BME readbackを保ち、DMA drain保証とは呼ばない。
- `PCIE_LNKCTL/LNKCTL2` とLink Disable・Enter Compliance・Enter Modified Complianceの仕様bitsを `pci.h` に定義し、保存復元配列と `guards_init` が共有する。旧 `pciuser.c:293–294` のraw offsetsを除いた。§7.5.3.7、§7.5.3.19に対応する。
- 実screen GPUの汎用FLRは追加していない。scanout再建の契約は未確定であり、primaryのnative回復責務とdirty display peerの拒否を維持した。

## 追加依頼：任意サイズDMAの実guest診断

`pcitest dma=268435456` のようなoptional分岐を既存 `tests/pcitest.c` に追加した。通常pcitestは変更していない。

1. 正の10進バイト数を乗加算overflow・nativeページ丸めoverflow込みで検証する。固定サイズ上限を設けない。PCI一覧は実件数に合わせて拡張しEDUを探す。
2. EDUの隔離claimを必須にし、`n_dma_create(pci, bytes, 0)`（CONTIGUOUS/32BITなし）だけを `uptime_ms` で計測する。資源不足などは既存のnative errorを表示し失敗扱いにする。
3. 全ページの `n_dma_addresses` 件数、非zero/整列、一覧終端を確認し、先頭末尾のアドレスを記録する。先頭・末尾のviewだけを写像し、zero初期化とCPU読書きを確認する。
4. 写像、view、DMA object、claimを順に解放する。時間の合否閾値、連続性/4GiB制約を設けない。これは実GPU転送・GPU性能・全体のGUI性能の証明ではない。

## 実行した検証

- 製品修理前に新しい進行反例を追加し、**6件失敗**を保存：`build/iommu-pci-audit-before.log`。CT disabled/予約値で後にTPが消える2条件と、RTRなしの100ms後RRS。失敗を除外せず修理した。
- 最終の実 `pci.c/pciuser.c` を含むホスト検査はUBSan trap有効、終了0・**失敗0**：`build/iommu-pci-audit-after.log`。全group dirty、BME停止拒否、MSI/MSI-X拒否、画面primary/peer、失敗時再遮断の既存対照に加え、TP/RRS進行中のgroup予約、RRS中Vendor ID以外のアクセス禁止、消失後の復元再開、killed-before-entry、kill/check/register境界、取消後state/残待機維持、named guardを確認した。
- 旧「未知CTなら直ちに拒否」「RRSは一回で戻る」のsleep件数は、新しい取消可能待機に合わせた。安全上の期待値（TP消失前FLR禁止、架空期限禁止、未準備復元禁止、二重FLR禁止）は維持し、永続pending/RRSでは実際に取消して失敗・状態保持を確認する。
- ホストのCF8/CFC、clock、task/IRQ guard、IOMMU attach/block、vnode等は模擬。kill境界はfixture注入であり、実競合の再現やIOMMU/PCIe orderingの証明ではない。
- 正規kernel相当 `--target=x86_64-unknown-none-elf -Wall -Wextra -Werror` で `pci.c/pciuser.c` の構文確認、正規user相当flagsで `tests/pcitest.c` の構文確認はいずれも終了0。担当5ファイルの `git diff --check` も終了0。

ホスト再現コマンド：

```text
tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-format -fsanitize=undefined -fsanitize-trap=undefined -Ikernel/src -Icommon tests/pci_hosttest.c "-Wl,--gc-sections" -o build/iommu-pci-audit-after.exe
build/iommu-pci-audit-after.exe
```

compilerは指定の既存実体を `require_escalated` で実行した。PowerShell cmdlet、編集代用スクリプト、全体buildは使っていない。

## 未確認と引継ぎ

- 最新製品媒体での単一QEMU通常pcitest/FLR/256MiB DMA診断は主担当の実行待ち。本担当は実QEMUを起動せず、画像を見た・操作したとは報告しない。
- 実機のDMA/IRQ、CTdisabled装置の実完了、BME stuck、実GPU/NVIDIA回復・scanout、S3/Hyper-V、実GUI応答・性能は未確認。永久pending/RRSには安全な汎用復旧保証はなく、ユーザー取消または装置/platform回復が必要。
- 実共通IOMMUのgroup/ACS probeはFLR再開前にもconfigを読む。今回のhost attachは模擬で、その全経路の無アクセスを証明しない。PCIe仕様が全FLRの一律config禁止を定めると捏造していない。
- 製品・試験を保存して静止する。主担当はこの差分・元反例・最終host証拠を独立確認し、map同期後の新しい第一報告登録、別委任による第二検討と実guest受入へ進む。
