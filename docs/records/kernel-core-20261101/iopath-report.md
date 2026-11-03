# iopath の報告

枝は `worktree-agent-ab71346e813f10323`。土台は main の 8837d5bd。

## 依頼文の全文

あなたの名は「iopath」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の c7d91d6e（b6388a38 の上に Rust の std の直し）。QEMU のポートは HMP 45468・QMP 45469。作業木での組みの欠陥（`scripts/build_cxx.py`・`ports/mesa/generate.py` が `BUILD` を受けず出力先を `<木の根>/build/` に決め打ち、`ports/mesa/nvk.mk` が `tools/rust/sysroot` を決め打ち）がある。前の書き手は作業木の `tools` と `build` を本体への junction にして越えた（終わりに外すこと）。この三つの決め打ちを直すのもあなたの仕事に含める（`BUILD` を受ける形に。空の build/ から組んで通ることを読んで確かめる）。

# 仕事：割り込みを止める長い区間と、規模で詰まる I/O の道を直す
監査 `audit-kernel.md` の要点 2・3・4・5（xHCI 以外）・8・9 を読むこと。持ち主の部品と、必ず入れる形：
- `dev/entropy.c` の `entropy_get`：全 CPU の feed を大域の錠の下・割り込みを止めて畳む（1024 CPU で 0.5 ms の桁、`sys_random` が 256 byte ごとに呼ぶ）。CPU の数に比例する仕事を熱い道から外す。
- `fs/cache.c` の `grow`：全 block を割り込みを止めて rehash（32 GiB で ms、1 TiB で数百 ms）。段ごとに移す形か、伸ばしの要らない構造に。
- `dev/iommu_amd.c`・`iommu_intel.c` の `sync`：大域の錠の下で全 unit を最大 100 ms ずつ spin。完了は装置の印で待ち、錠を unit ごとに。
- `sys/log.c`・`dev/serial.c`：log を書くたびの self-IPI と、その割り込みの中の大域 `serial_lock`・`outb` 16 回・全 CPU の ring の走査・全部の wake。log の書き手は lock なしのまま（良い所）、吐き出しを割り込みの外の流れへ。
- `sys/syscall.c` の `shared_lock`（共有メモリーと program の text の fault が、大域 spinlock を割り込みを止めて握り、線形の list を歩き、錠の中で frame を確保・0 埋め・map、穴の確保は first-fit の連結 list）。共有の object と区間の木（`lib/span`）で mm の持ち物として作り直す。中核の `mm/vmm` の口を使う。syscall.c から mm の責務を出す。
- `dev/nvme.c`：制御器ごとに queue が一組で命令が一度に一つ、128 KiB の bounce の memcpy、割り込みが来ないとき 10 ms で起きて見る。CPU ごと（または NUMA の node・LLC ごと）の I/O queue の対、MSI-X の vector を queue ごと、複数の命令を同時に、PRP・SGL で直に DMA（IOMMU の口 `dev/iommu.h` を通す）、完了は割り込みで。仕様は `D:\Programes\OS DEV INFO` の NVMe Base Specification。
- `dev/ahci.c`：slot 0 だけ・NCQ なし。NCQ で複数の slot（AHCI 1.3.1）。
- `arch/irq.c`：MSI-X の vector の置き場所が NUMA を見ない（`.node = -1`）、一度置いたら動かない（`irq_set_cpu` に呼び手なし）。装置の node に近い CPU へ置く。合成・入力の締切の仕事が走る CPU を割り込みで乱さないための口（割り込みを避ける CPU の集合など）を考え、要るなら入れる。
- 固定の上限：`MAX_CTRL 4`、`MAX_NS 8`、`CLAIM_VECTORS 32`、`MAX_BLK 16`、`channel.c` の `message_new` の上限なしの `kmalloc`（アプリが大きさを決める：handle の信用・資源の量で縛る形に）。`sys_proclist`・`report_memory_exhaustion` が割り込みを止めて全 thread を歩く。`fs/store.c` の cursor の表の線形の探し。
- `arch/spin.h` の錠の rank：枠はあるのに `SPIN_UNRANKED` しか無い。錠の順を rank で書き、検査が働くようにする（debug の組みだけで検査するなら、そう書く）。
- `user/libc/sys.c` の今のフォルダーと設定の表が lock なし（`setting_set` が `realloc` する間に別の thread の `getenv` が読む）。直す。`tests/navigatortest.c` が消えた `setenv` を呼んでいる：ネイティブの口に直す。

範囲の外：合成・窓・表示（「compositor」）、USB・入力（「usbinput」）、時間の ABI（「timeabi」）は別の書き手が並べて書いている。`net/` の大域の `net_lock` は今回は触らない。`common/abi.h` を変えるなら自分の部分だけを変え、ほかの行を並べ替えない。

確かめ：組み（変数なしで `kernel`・`qemu-image`）。QEMU で起動、C: の読み書き（Terminal でファイルを作り読む、ブラウザーで実サイトを開いて cache が書かれる）、NVMe の道と SATA の道（AHCI の起動の仕方は nocturne-project の手順書）の両方で起動する。`prof:` の行と起動の時間を前（c7d91d6e の媒体の serial の行）と並べて報告に書く。

## 結論

依頼の項目のうち、`shared_lock` の作り直しだけが一部で止まっている。大域の錠は残り、共有メモリーのコードは `sys/syscall.c` に残っている。詳しくは「残った欠陥」の 1 に書く。

ほかの項目は入れた。合成・入力の CPU を割り込みから守る口は入れていない。理由は「残った欠陥」の 2 に書く。

## 設計

### 組み

- `scripts/build.py` は作業木から組むとき、出力を本体の `build/wt/<作業木の名>/` に置き、本体の `tools` と `local` を使う。
- `build_cxx.py`・`layout.py`・`generate.py`・`mkimage.sh`・`mksdk.sh` は出力先を受ける。
- `nvk.mk` の sysroot は `RUST_SYSROOT` で、Makefile の頭で決める。
- Mesa の Rust の crate は std の rlib に依存する。std を組み直せば crate も組み直す。
- tsc の出力先は `$(BUILD)/apps-ts`。
- `third_party/wabt/src/tools/wasm2c.cc` を git に入れた。`.gitignore` の `tools/` が外していた。
- 作業木の `tools`・`build` の junction は無い。

### 乱数（`dev/entropy.c`）

- 種は entropy の thread が畳み、seqlock で出す。
- 各 CPU は自分の生成器を持つ。種の代が変わったときか、1 MiB を出したときに作り直す。
- 熱い道は自分の CPU の鍵を進めるだけで、CPU の数に比例する仕事をしない。

### block cache（`fs/cache.c`）

- `grow` は新しい表を作って移しを始めるだけにした。
- 以後の各操作が、lock の下で決まった数の bucket を移す。
- 移しの間は、移していない bucket を古い表で引く。
- 空になった古い表は lock を放してから返す。

### IOMMU（`dev/iommu_amd.c`・`iommu_intel.c`）

- 錠は unit ごと。命令と完了の待ちの命令を入れるのは錠の下で、待つのは錠の外。
- 各 unit は自分の token を数え、完了の語が token に届いたら済む。
- AMD-Vi は COMPLETION_WAIT の割り込みで眠りから起きる。命令の列が止まったら事象の割り込みで起こす。
- VT-d は状態の書き込みを錠の外で見て待つ。
- 割り込みの表と `msi_owner` は `tables_lock` が守る。

### log と serial（`sys/log.c`・`dev/serial.c`）

- 書き手は今も錠なしで自分の輪に書く。
- 書いた輪の印を立て、流しが借りになっていなければ log の thread を起こす。spinlock を持つ書き手と NMI だけが自分への IPI を使う。
- 読み手の cursor は印の立った輪だけを見る。
- serial の吐き出しは log の thread だけが行う。
- FIFO を一回満たしたら、FIFO の線の時間（115200 baud の 16 byte で 1.39 ms）が過ぎるまで次を満たさない。
- UART の送信の割り込みは使わない。割り込みの handler、SPCR の割り込みの読み取り、`log_kick` の公開を消した。

線の時間で歩調を取る理由は、次の観察である。

- 送信の割り込みで歩調を取った版では、QEMU の UART が FIFO をすぐ空にするので、PIO が切れ目なく続いた。
- その版の NVMe の起動で、`smp: cpu2 did not start; excluded`・`smp: cpu3 did not start; excluded`・`smp: 2/4 CPUs online` が出た（`serial-t8-nvme.log`）。
- log の thread の重みを 1/64 に下げた版は、`clock: TSC` から `TSCs agree` までの段を縮めなかった（136 ms、`serial-t7-sata.log`）。この変更は外した。
- 一回満たすごとに 1.4 ms 眠る試しの版は、同じ段を 12 ms にした（`serial-t6-exp.log`）。
- 実機の UART は同じ速さで THRE を立てる。したがって実機の送りの速さは変わらない。

### NVMe（`dev/nvme.c`）

- `MAX_CTRL`・`MAX_NS` を消した。
- I/O queue は制御器が許す数まで CPU に一つ。CPU はその順で queue に分かれ、cache と node の近い CPU がまとまる。
- queue の記憶はその CPU の node から取る。
- MSI-X の message は queue ごとで、`irq_set_cpu` でその queue の CPU に置く。message が足りなければ queue が分け合う。
- 一つの queue は entries - 1 の命令を同時に持つ。命令ごとの slot で待ち、完了の割り込みが起こす。
- 固定の起床の間隔は無い。割り込みが無い制御器を見るときだけ 1 ms で見る。
- データは PRP で呼ぶ側の記憶へ直に動く。bounce は block の一部だけの要求か、4 byte に揃わない記憶のときだけ。
- 時間切れと致命の状態は制御器を止める。最初の呼ぶ側が reset し、残りの命令は失敗して再び出される。

### AHCI（`dev/ahci.c`）・`dev/blk.c`

- CAP.SNCQ と IDENTIFY の語 76 bit 8 があり、48 bit の番地を持つ disk は READ/WRITE FPDMA QUEUED を使う。深さは CAP.NCS と語 75 の小さい方。
- 完了は PxSACT と PxCI の両方から消えた slot で判る。
- slot ごとの command table に PRD を置き、呼ぶ側の記憶へ直に DMA する。
- adapter が 64 bit の番地を持たないときだけ、4 GiB より下の bounce を使う。
- adapter の数に上限は無い。`hbas` は list。
- `MAX_BLK` を消し、`blk` の装置の表は 8 から倍に伸びる。読み手は錠なしで表を読む。

### 割り込みの置き場所（`dev/pci.c`・`dev/acpi.c`）

- `pci_device` に `node` を持たせた。ACPI の `_PXM` を機能の節から host bridge まで探し、topology の node に換える。
- PCI の装置の割り込みは `irq_spec.node` にそれを渡す。割り込みの層が装置の node の CPU に置く。
- node が一つの機械では `_PXM` を探さない。機能の ACPI の節を探すと隣の `_ADR` を評価するので、選ぶものの無い機械で払わない。

### 固定の上限

- `CLAIM_VECTORS` を消した。割り込みの本数は PCIe の MSI-X の上限 2048 まで。pending は 64 bit の語の配列。
- `n_pci_interrupt_take(pci, bits, words, deadline)` が呼ぶ側の語の配列へ書く。仕様・`nocturne.h`・`ports/nvidia/nvrm.c`・`tests/pcitest.c` を合わせた。
- アプリの channel のメッセージは送り手の分として数え、合計を空きメモリーの半分までにする。超える送信は `N_ERR_NO_MEMORY`。カーネルの post は数えない。仕様の第 4 節に書いた。
- `thread_list` を消した。`thread_walk` が tid の表を逆二進の順で歩き、`thread_lock` を持つのは一段に 32 thread まで。`sys_proclist`・`report_memory_exhaustion`・`process_stop_all` がこれを使う。
- メモリー切れの報告は大きい 8 個のアプリだけ頁を分けて数え、pid で引き直して空間の lock の下で数える。
- store の一覧の cursor は CPU ごとの shard に分けた。大域の錠と線形の探しは無い。

### 錠の順位（`arch/spin.h`）

順位は低い順に次のとおり。検査は組みによらず常に効く。順を破った道でその場で止まる。

`SPIN_THREAD` → `SPIN_SHARED` → `SPIN_ADDRESS_SPACE` → `SPIN_PAGE_CACHE` → `SPIN_PAGE_NODE` → `SPIN_HEAP_DEPOT` → `SPIN_VMALLOC`

run queue の錠は二つを CPU の順で同時に持つので、順位を持たない。wait queue の錠はほとんどの錠の下で取るので、順位を持たない。

### 共有の写像と穴（`sys/syscall.c`・`sys/process.h`）

- アプリの共有の写像を線形の list から `lib/span` の木へ移した。fault と重なりの検査は O(log n)。
- 穴（`mmap_holes`・`low_holes`）を first-fit の連結 list から `lib/span` の木へ移した。当てはめと解放は O(log n)。

### libc（`user/libc/sys.c`）・`tests/navigatortest.c`

- 自分の設定は消えない項目の列で、値は丸ごと差し替える。読み手は錠なしで読み、`getenv` が渡した指は残る。
- 書き手どうしは番地の待ちの lock で並ぶ。
- `navigatortest` は試験の利用者のフォルダーを起動の設定 `USERFOLDER` に渡して自分を起こし直す。

## 変えたファイル

`git diff --stat 8837d5bd 9f480e75`：57 ファイル。追加 2585 行、削除 1111 行。主なものは次のとおり。

| 層 | ファイル |
|---|---|
| 組み | `Makefile`、`.gitignore`、`scripts/build.py`・`build_cxx.py`・`layout.py`・`mkimage.sh`・`mksdk.sh`、`ports/llvm/llvm.mk`、`ports/mesa/generate.py`・`llvmpipe.mk`・`mesa.mk`・`nvk.mk`、`ports/rust/rust.mk`、`third_party/wabt/src/tools/wasm2c.cc` |
| 乱数・log | `kernel/src/dev/entropy.c`・`entropy.h`、`kernel/src/sys/log.c`・`log.h`、`kernel/src/dev/serial.c`・`serial.h`、`kernel/src/arch/irq.h`、`kernel/src/boot.c` |
| 格納 | `kernel/src/fs/cache.c`・`store.c`、`kernel/src/dev/nvme.c`・`ahci.c`・`blk.c` |
| IOMMU・PCI | `kernel/src/dev/iommu.h`・`iommu_amd.c`・`iommu_intel.c`・`pci.c`・`pci.h`・`acpi.c`・`acpi.h`・`pciuser.c`・`pciuser.h`、`kernel/src/net/rtl8125.c` |
| 中核 | `kernel/src/arch/spin.h`、`kernel/src/lib/idtable.c`・`idtable.h`、`kernel/src/mm/heap.c`・`pmm.c`・`vmalloc.c`・`vmm.c`・`vmm.h`、`kernel/src/sys/channel.c`・`process.c`・`process.h`・`syscall.c`・`thread.c`・`thread.h` |
| ABI と利用者 | `docs/nocturne-native-abi.md`、`user/include/nocturne.h`、`user/libc/sys.c`、`ports/nvidia/nvrm.c`、`tests/pcitest.c`・`navigatortest.c` |

## 消したもの

- `MAX_CTRL`、`MAX_NS`、`MAX_BLK`、`CLAIM_VECTORS`。
- NVMe の一組だけの queue、128 KiB の常の bounce、10 ms の起床。
- AHCI の slot 0 だけの道。
- `entropy_get` の大域の錠と全 CPU の畳み込み。
- `cache.c` の割り込みを止めた全 block の rehash。
- IOMMU の大域の錠と全 unit の spin。
- serial の送信の割り込みと `serial_isr`、log を書くたびの self-IPI。
- `thread_list` と `all_next`・`all_prev`。
- store の `cursors_init` と cursor の大域の表。
- 共有の写像の線形の list、穴の連結 list。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：変数なしで通り、警告は 0。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：変数なしで通る。増分の組みの警告は 0。
- libc の全部を組み直すと `user/libc/font.c`、`web/css.c`、`web/cssmath.c`、`web/cssprop.c`、`web/layout.c` に `-Wmissing-field-initializers` が 52 個出る。これらのファイルは main と同じで、私は変えていない。
- `tests/pcitest.c`・`tests/navigatortest.c` はゲストの tcc で組む試験である。ホストの clang の `-fsyntax-only` で型だけを照らし、通った。試験は走らせていない。

## QEMU で見たもの

条件はどの回も q35、UEFI、4 CPU、4 GiB、`-accel whpx`、`-cpu host`、2560×1440、窓付き。画像と log は次のフォルダーにある。

`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\iopath\`

前の媒体は c7d91d6e。最後の媒体は 9f480e75 の kernel で、NVMe は `t11`、SATA は `t10`。

### 画面

| 道 | 見たもの | 画像の絶対 path |
|---|---|---|
| NVMe | Terminal で `echo iopath-io > io.txt` と `cat io.txt` の後、Wikipedia の Moon | `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\iopath\t11-wiki.png` |
| NVMe | `echo final-nvme > fin.txt` の結果が `cat` で `final-nvme` と読め、`ls AppData/Browser` が `storage`・`cookies` を出す。媒体は 9f480e75 と注釈 1 行だけ違う t9 | `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\iopath\t9-ls.png` |
| SATA | `echo final-sata > fin.txt` の結果が `cat` で `final-sata` と読め、後ろに Wikipedia の Moon | `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\iopath\t10-sata.png` |
| 前の NVMe | Wikipedia の Moon | `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\iopath\base-wiki.png` |

`webfetch` の HTTP cache はメモリーの上にあり、C: に書かない。依頼の「cache が書かれる」は、ブラウザーの保存（`storage`・`cookies`）が C: に書かれたことで確かめた。

t10・t11 の serial.log に `panic`・`lost`・重さ E の行は 0 個。

### 装置の行

```
前 NVMe  nvme0: QEMU NVMe Ctrl (1b36:0010), version 10400, 256 namespaces, MSI, write cache
今 NVMe  nvme0: QEMU NVMe Ctrl (1b36:0010), version 10400, 256 namespaces, 4 I/O queues of 63, 4 MSI-X vectors, write cache
前 SATA  ahci0: 8086:2922, version 10000, ports 3f, MSI
今 SATA  ahci0: 8086:2922, version 10000, ports 3f, 32 slots, queued commands, MSI
今 SATA  ahci0: port 0: QEMU HARDDISK, 32 queued commands at once
```

### 起動の時間

`clock: TSC` の行から `store: C:` の行までの実時間を比べる。

| 媒体 | 道 | clock → TSCs agree | clock → store | log |
|---|---|---|---|---|
| c7d91d6e | NVMe、prof | 48.3 ms | 165.0 ms | `serial-base-nvme.log` |
| c7d91d6e | SATA | 45.5 ms | 166.5 ms | `serial-base-sata.log` |
| 割り込みの歩調の版 | NVMe、prof | 98.0 ms | 241.2 ms | `serial-t3-nvme.log` |
| 割り込みの歩調の版 | SATA | 109.7 ms | 278.6 ms | `serial-t4-sata.log` |
| 9f480e75 の注釈前 | NVMe | 24.2 ms | 238.7 ms | `serial-t9-nvme.log` |
| 9f480e75 | SATA | 15.1 ms | 137.9 ms | `serial-t10-sata.log` |
| 9f480e75 | NVMe、prof | 17.6 ms | 120.8 ms | `serial-t11-nvme.log` |

- CPU の起動の段は、前の 45〜48 ms に対して今は 15〜24 ms である。比べたのは上の表の前の 2 回と今の 3 回で、同じ機械の QEMU である。
- clock → store は、前の 165〜167 ms に対して今は 121〜239 ms である。同じ NVMe の道で 121 ms と 239 ms が出たので、この幅の中の一回の差で速くなったとは言わない。
- t9 の 239 ms の内訳は、PCI の最後の行から `iommu:` の行までが 139 ms（前は 73 ms）、`ahci0` の行から `nvme0` の行までが 34 ms（前は 4 ms）である。t11 では同じ段が 75 ms と 4 ms である。この幅の原因は分からない。測った間、ほかの書き手のビルドと QEMU が同じ機械で動いていた。
- 実機の起動の時間は測っていない。

### prof の行

ブラウザーの読み込みの窓を並べる。4 CPU の合計で、窓は 10 秒、単位は標本（1 ms）。

| 窓 | user | kernel | switches | TLB shootdowns | IPIs | late jobs |
|---|---|---|---|---|---|---|
| 前 234 秒 | 987 | 695 | 9284 | 86 | 106 | 1 |
| 今 78 秒 | 1108 | 744 | 9809 | 90 | 101 | 0 |
| 前 244 秒 | 5163 | 351 | 8159 | 89 | 84 | 0 |
| 今 88 秒 | 4692 | 447 | 8904 | 89 | 105 | 0 |

前は `serial-base-nvme.log`、今は `serial-t11-nvme.log`。

読み込みの後、動きの無い窓を並べる。

| 窓 | user | kernel | switches | late jobs |
|---|---|---|---|---|
| 前 264 秒 | 0 | 4 | 314 | 3 |
| 前 274 秒 | 0 | 4 | 314 | 2 |
| 今 118 秒 | 0 | 5 | 378 | 0 |
| 今 128 秒 | 0 | 6 | 366 | 0 |

- 今の 98 秒の窓は kernel 444 で、そのうち 443 標本が `serial_drain` である。prof 自身の行の吐き出しで、その窓の switches は 4953。
- 前の版は同じ吐き出しを serial の割り込みの中で行い、標本に出ていなかった。前の 254 秒の窓は kernel 7 だが、cpu3 の合計は 7267 標本で、10000 に約 2700 足りない。今はこの費用が `serial_drain` として見える。
- 今の読み込みの窓で一番多い kernel の番地は `vmm_demand_fault` の `spin_unlock` で、78 秒に 433、88 秒に 147 である。

## 残った欠陥と未確認

1. **`shared_lock` の作り直しは一部だけである。** 入れたのは写像と穴の区間の木と、錠の順位。次は入っていない。
   - `shared_lock` は大域のままで、割り込みを止めて握る。
   - 共有の fault は今も錠の中で frame を確保し、0 で埋め、map する（`shared_frame`、`demand_slot`、`paged_slot`）。
   - revoke は大域の `shared_all` の list を歩く。
   - コードは `sys/syscall.c` の約 1500 行に残り、mm へ出していない。

   この部分は object、scope、compose、pager、DMA の型の変更が一つの錠に頼っている。錠を object ごとに分けるには、compose の部分どうしと scope の木の順を決める設計が要る。`shared_scanout_open` は画面の scanout の道（`dev/pciuser.c`）から呼ばれる。compositor の差分が main に載った後に行えば、衝突が少ない。

2. **合成・入力の CPU を割り込みから守る口は入れていない。** 守る CPU を決める呼ぶ側がまだ無いからである。合成の thread を CPU に縛る形が compositor の差分で決まってから入れる。

3. **`demand_fill` は割り込みを止めた空間の錠の下で、最大 16 個の frame を 0 で埋める。** 上の prof で kernel の標本の最大がこの区間である。これは依頼の項目の外で、c7d91d6e から変えていない。前の 244 秒の窓の最大は 183 標本（`0xffffffff8006a911`）で、前の `kernel.elf` が残っていないので関数は引けなかった。

4. **`id_table_room` は `thread_lock` の下で表を rehash する。** thread の数が増えると、この区間が伸びる。

5. **AHCI の NCQ のエラーは READ LOG EXT 10h を使わない。** エラーで出ている命令を全部失敗にし、port を COMRESET し、呼ぶ側が出し直す。

6. **serial の送りは QEMU でも線の速さになった。** 11.5 KB/s を超える log は輪に溜まって遅れ、輪が回れば cursor が失った分を言う。`--debug-js` の長い log での遅れは測っていない。t11 の log に、失った分を言う行は無い。

7. **`.claude/rules/kernel.md` に `thread_list` の記述が残っている。** 規則のファイルなのでここでは変えていない。地図と一緒に直す必要がある。

8. **実機と Hyper-V は未確認。** AMD-Vi と VT-d の新しい待ちは、IOMMU の無い QEMU の条件では通っていない（`iommu: none described`）。NVMe の複数の node での queue の置き方と `_PXM` も、QEMU の node 一つの条件では通っていない。NCQ のエラーの道も通っていない。

9. **ゲストの試験（`scripts/test.py`）と WPT は走らせていない。**

## コミット

```
e397bfee 組み：BUILD・TOOLS・LOCAL を受け、作業木から空の出力先へ組める
8f7e9c03 wabt の wasm2c.cc を git に入れる
42d85694 組み：tsc の出力先を BUILD の下に
1ca9c21e 乱数：種の畳み込みを専用の thread へ移す
4ad82190 記録：serial への流しを log の thread へ移す
905def04 block cache：hash の表を少しずつ移す
37b8fb3b libc：自分の設定の表を錠なしで読めるようにする
ca28d7ab IOMMU：unit ごとの lock と、完了の印での待ち
7437b2da PCI：機能ごとの NUMA の node
ba50bfdf NVMe：CPU ごとの I/O queue、queue ごとの割り込み、並ぶ命令、直の DMA
cdb91913 AHCI：NCQ と複数の slot、直の DMA、adapter の数に上限なし。blk：装置の表を伸ばす
74fa6da9 組み：Mesa の crate を Rust の std に依存させ、std を組み直したら組み直す
b75fbccc channel：アプリのメッセージを送り手の分として数え、空きメモリーの半分までにする
9097cec1 thread の一覧を段ごとに歩く：proclist・メモリー切れの報告・全停止
f62a3b70 store：一覧の cursor を CPU ごとの shard に分け、大域の錠と線形の探しを消す
0063de55 pciuser：アプリのドライバーの割り込みを機能が出せる本数まで開く
f950df3f spinlock の順位を mm と sys の錠に付け、共有の写像と穴を区間の木にする
9f480e75 serial：FIFO を線の速さで満たし、送信の割り込みを消す。PCI：node が一つなら _PXM を探さない
```
