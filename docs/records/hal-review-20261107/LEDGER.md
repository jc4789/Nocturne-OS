# HAL の見直しの台帳

目的：汎用の OS の HAL と装置の作りを、混成（hybrid）の形で建てる。全部が ring 0 で動き、層の境は厳しく、実装（ドライバー）は版の付いた ABI で組む別の像。どんな装置もどんなバスも、実装さえあれば HAL と核を変えずに扱えること。ドライバーを書くか移すとき、HAL と核を変えないこと。範囲は OS の全部で、今のコードは正しいと示されるまで残す理由にならない。
この台帳は作業の正本。区切りごとに読み、所見・決定・状態を更新する。

## 0. 前提

- 読んだもの：`docs/hal.md`、`kernel/src/hal/*`、`common/hal_abi.h`、`common/hal_buses.h`、`kernel/src/dev/pci.c`（HAL の部分）、`pci.h`、`pciuser.c`、`acpi_devices.c`、`acpi.h`、`pm.c`、`iommu.h`、`arch/irq.h`、`usb.h`、`user/apps/services.c`、ABI の §9、`init.c`。カーネルの実装ごとの使い方は §2 の表。
- 照らした原文：ACPI 6.5（16.1.6、7.1〜7.4）、PCIe 6.0（5.8、5.9、7.5.2）。`D:\Programes\OS DEV INFO`。
- Gemini の資料（`D:\Programes\Grounding HAL Info By Gemini 4 Argon.txt`）は HAL の範囲の目安にだけ使う。事実は原文とコード。

## 1. 所見

番号は H。状態：未・直。範囲の外は無い：見つけたものは全部この仕事で直す。

| # | 所見 | 場所 | 状態 |
|---|---|---|---|
| H1 | 実装が装置を止める道（`quiesce`）は、割り込みを切り、ほかの CPU を止めた後にだけ走る。実装は眠れず、自分の装置の完了の割り込みも待てない。電源を切る道（`finish`）も同じ。眠り・休止・電源断の装置の段取りが、割り込みの無い一つの段に押し込まれている | `kernel/src/dev/pm.c:177-189`、`:258-264`、`kernel/src/hal/power.c:51-62`、`hal.h:15` | 直 |
| H2 | 眠りの前に装置を D 状態へ移さない。ACPI 6.5 16.1.6 の 4（起こしに使わない装置は D3）、7.3 の `_PSx`・`_PRx`（電源資源）、PCIe 6.0 5.9 の遷移の時間のどれも行わない。HAL に装置の電源の状態という考えが無い。`rtl8125` だけが自分で PMCSR を読んで D0 にする（バスの仕事をドライバーが持つ） | `kernel/src/hal/*`、`kernel/src/net/rtl8125.c:581` | 直 |
| H3 | `\_TTS` を呼ばない（ACPI 6.5 7.4.4：眠りの始めに Sx、終わりと取りやめで S0）。uACPI の `uacpi_prepare_for_sleep_state` は `_PTS` と `_SST` だけ | `kernel/src/dev/acpi.c:885`、`third_party/uacpi/source/sleep.c:364` | 直 |
| H4 | 装置の firmware の節（ACPI の node）が HAL の考えに無い。評価と Notify はプロセスの実装の PCI のバスの呼出し（`N_PCI_CALL_ACPI_EVAL`・`_NOTIFY_TAKE`）だけにあり、ACPI の装置や他のバスの装置のドライバーは使えない。画面の保持（`N_PCI_CALL_HOLD_SCREEN`）も PCI のバスの呼出しにある（監査 K4） | `common/hal_buses.h:34-36`、`kernel/src/dev/pciuser.c:342-437`、`:632-637` | 直 |
| H5 | ACPI の「system」実装は何もしない attach で、電源のボタン・蓋・割り込みの link・PCI のホスト・HPET・RTC・シリアルを「持つ」ように台帳に見せる。本当の持ち主（`dev/acpi.c`、`dev/pci.c`、`dev/refclock.c`、`dev/serial.c`）は HAL を通らずに自分で見つける（shim） | `kernel/src/dev/acpi_devices.c:224-236` | 直 |
| H6 | PCI の機能が ACPI のホストブリッジ（PNP0A03・PNP0A08）の下でなく、根の下に並ぶ。木の順の眠りにホストブリッジが入らず、機能と firmware の節のつながりが木に無い | `kernel/src/dev/pci.c:269` | 直 |
| H7 | サービスの管理者は、起動のときの台帳の写し一回だけでプロセスの実装の実例を作る。後から出た装置（USB、カーネルの実装が手放した装置、抜き差し）にはプロセスの実装が付かない。装置が去っても実例が残る | `user/apps/services.c:288-294` | 直 |
| H8 | DMA の契約に「写す・外す」が無い。装置へ渡す番地は「物理番地＋`dma_offset`」と決め打ちで、IOMMU がカーネルの装置を訳せない（`docs/open.md` の既知の欠陥の根）。装置が届く番地の幅は装置の性質でなく、呼ぶたびの `bits`。データの道の番地の幅は実装ごとに守るか守らない | `kernel/src/hal/hal.h:140-145`、§2 | 未 |
| H9 | DMA のメモリーを HAL の外で取る実装がある（`pmm_*` を直接、`hhdm_virt_to_phys`）。HAL の「止まったと確かめてから返す」の約束が届かない。`ahci` は `dma_offset` を直接読む | §2 | 未 |
| H10 | `struct hal_depend_entry` が `device.c` と `power.c` に二つある。`hal_gate_close`・`hal_gate_open` の宣言が `power.c` の中にある | `kernel/src/hal/device.c:12`、`power.c:8-14` | 直 |
| H11 | 出す前に捨てる装置の記録を、バスが HAL の中身（`props`・`prop_bytes`）を直接 `kfree` して捨てる | `kernel/src/dev/acpi_devices.c:175-179` | 直 |
| H12 | `pm_sleep_check` は「全部の実装が登録した」と書くが、結びつけは装置ごとの thread で、その時に終わっている保証は無い | `kernel/src/init.c:55` | 直 |
| H13 | claim の能力（`N_CAP_HAL_CLAIM`）は装置を指さない。持つプロセスは実装の無いどの装置でも claim できる（TPM、EC、GPIO の制御器など）。「ドライバーは自分の装置だけを claim する」はプロセスの約束で、カーネルは確かめない。H5 の shim はこの穴を台帳の上で塞ぐためにある | `kernel/src/sys/syscall.c:235`、`kernel/src/hal/claim.c:635`、`ports/nvidia/nvrm.c:398` | 直 |
| H14 | 眠りの手順の順が ACPI 6.5 16.1.6 と違う。`_PTS`（5）が装置を止める前（4 の前）に走る | `kernel/src/dev/pm.c:368-371` | 直 |
| H15 | IOMMU の実装 `iommu_driver` も、attach が 0 を返し resume が true を返すだけの shim（H5 と同じ形）。AMD の IOMMU の PCI の機能を「持つ」ように見せる | `kernel/src/dev/iommu.c:1171-1189` | 直 |
| H16 | `dma_offset` を書く所が無い（いつも 0）。読み手（`ahci`・`xhci` の `bus - dma_offset`）だけがある | `kernel/src/hal/hal.h:80` | 未 |
| H17 | `hda` が自分の DMA の頁を IOMMU の domain へ `iommu_map` で直に写す（guest の道）。装置が domain に入る・出ることを HAL が知らないので、実装が IOMMU を知っている | `kernel/src/dev/hda.c:566`、`:601`、`:828` | 未 |
| H18 | `xhci` は AC64 の無い制御器を断る（32 bit の DMA の装置を HAL が扱えないので、実装が諦める） | `kernel/src/dev/xhci.c:2193-2196` | 未 |
| H19 | `virtio_gpu` が `vmm_map_mmio` と `pci_read32` を直に使う。`ata` は `hal_port` を使わず決まったポートを調べる（どちらも VM の実装） | `kernel/src/vm/virtio_gpu.c:128-142`、`kernel/src/vm/ata.c:132-135` | 未 |
| H20 | detach の後に記録と thread を返さない実装（`nvme`、`ahci`、`storvsc`）。抜き差しの無いバスなので今は害が無い | §2 | 未 |
| H21 | レジスターを期限つきで待つ関数を実装ごとに写している（9 か所） | §2 | 未 |
| H22 | ACPI の接続の資源（`GpioInt`、`GpioIo`、`I2cSerialBus`、`SpiSerialBus`、`UartSerialBus`）を HAL が表さない。GPIO の割り込みを出す実装（割り込みの提供者）の口も無い。I2C の HID（ノートの内蔵のキーボードとタッチパッド）を作るときに、HAL へ資源の種類と提供者の口を足す要がある | `kernel/src/dev/acpi_devices.c:125` | 未 |
| H23 | 起こしの元（`_PRW`、`_DSW`、GPE、PCI の PME）を扱わない。起こせるのは電源のボタンだけ。設定の窓の項目と組で作るもの | — | 未 |
| H24 | 動作中の装置の電源の管理（使わない装置を D3 へ）が無い | — | 未 |
| H25 | 固まった装置をバスでリセットして実装を結び直す口（カーネルの実装の立て直し）が HAL に無い。PCI の FLR と SBR は claim の道（`pciuser.c`）だけが使う | `kernel/src/dev/pci.h:229`、`:264` | 未 |
| H26 | 実装に ABI が無い。実装 21 個とバス 4 個は、カーネルの約 50 の部品から 300 を超える記号を直に取る：スケジューラーと CPU ごとの中身（`preempt_count`、`spin_nodes`、`percpu_offsets`、`sched_*`）、`pmm_*`、`vmalloc_*`、`hhdm_offset`、割り込みの vector と MSI（`irq_msi_rewrite`、`irq_request_msi_block`）、`iommu_*`、`uacpi_*`、`topology`、PCI の内部（`pci_device_of` と 25 の関数）。中身の構造体（`struct hal_device` の欄、`struct pci_device`）も読む。核の中を変えると実装が壊れ、実装はカーネルのソースなしに組めない | `build/k` の object の `llvm-nm -u`（作業場の `imports.py`） | 未 |
| H27 | 実装はリンカーの節 `.hal_drivers` でカーネルの像に焼き込まれる。読み込める実装の像が無く、実装を足すにはカーネルを組み直す | `kernel/linker.ld:34`、`hal.h` の `HAL_DRIVER` | 未 |
| H28 | 機械の部品（ローカル APIC、IOAPIC、vector と MSI、LAPIC の timer、TSC の測り、HPET・PM timer・RTC、SMP と IPI、起こしの入口、idle、IOMMU、ACPI の表、PCI の ECAM、電源断とリセット、console のシリアル）が `arch/` と `dev/` に散らばり、一つの口が無い。核も実装も直に呼ぶ。`hal/` と呼ぶものは装置の木と結びつけ（NT の PnP と電源の管理者）で、機械の HAL ではない | `kernel/src/arch/*`、`kernel/src/dev/{acpi,refclock,serial,iommu*,pci,power,pm}.c` | 未 |
| H29 | USB と VMBus の装置は `ops` が NULL で出る。HAL を通る割り込み・DMA・電源・リセット・claim が無い。USB の class の実装は xHCI の実装の口（`dev/usb.h` の `usb_*`、xhci.c が定義）を直に呼ぶ：制御器の実装がバスの口を兼ねる | `kernel/src/dev/xhci.c:1714`、`:1743`、`kernel/src/hal/claim.c:589` | 未 |
| H30 | `dev/power.c` の電源断とリセットに「emulator の代わりの道」と、AML が読めないときに DSDT から `_S5_` のバイトを探す代わりの道がある | `kernel/src/dev/power.c:1-3` | 未 |
| H31 | DMA の番地が物理番地に決め打ち（`hal_dma_reach` の `*bus = phys`、`hal/dma.c`）。IOMMU の領域は同じ番地の写しだけで、カーネルの実装の装置は外向きの port（Thunderbolt など）でも隔離されない。幅の外の緩衝は block だけにあり、網・音・USB は持てない | `kernel/src/hal/hal.h`、`kernel/src/hal/dma.c` | 未 |
| H32 | 眠りの穴：USB のディスク（`usbmsc`）を挿している間は眠りも休止もしない（戻った制御器が数え直すとディスクが新しい装置になる）。`nvrm` が `n_hal_power_wait` を呼ばず、NVIDIA の GPU の機械は眠らない。`firmware-screen` は claim の GPU ドライバーが画面 0 を持っても結ばれたままで S3 を拒む。PCI の列挙は firmware が D3cold で渡した機能を D0 にしない | `kernel/src/dev/usbmsc.c`、`ports/nvidia/nvrm.c`、`kernel/src/dev/fb.c:642`、`kernel/src/dev/pci.c` | 未 |
| H33 | 起動のたびに QEMU が「Ignoring request for interrupt vector 0」と言う。serial の最初の行（UEFI の BdsDxe）より前に出る：Nocturne はまだ走っていない。QEMU か firmware の事で、Nocturne の欠陥ではない | 作業場の `when.py`、`serial-h33*.log` | 直 |
| H34 | 利用者の ABI と文書が装置の管理者を「HAL」と呼ぶ（`n_hal_*`、`SYS_HAL_*`、`common/hal_abi.h`）。機械の HAL を分けると、名前が持ち主に着かない | `common/hal_abi.h`、`common/abi.h` | 未 |
| H35 | `pcitest` の画面の印の数の確かめが 5 件落ちる（refresh の分からない画面での合成の振る舞い）。段 B で範囲の外とした | §5 の段 B | 未 |
| H36 | 機械の検査：訂正の誤りを拾う道は UC と PCC の bank を飛ばす。#MC の道と分けた理由が SDM と照らしてない | `kernel/src/arch/mca.c:43`、`:85` | 未 |
| H37 | TSC：AMD は family（19h、1Ah）だけで P-state の欄を読む。`HWCR[LockTscToCurrentP0]` を読まずに 0 と仮定する | `kernel/src/arch/tsc_rate.h:24-45` | 未 |
| H38 | HD Audio：link を落とす手順の結果を捨てて DMA の頁を返す。段 D の後、hda は DMA の頁を自分で返さない：装置の管理者の `io_release` が、PCI のバスの stop（Bus Master Enable を 0 にし、Device Status の Transactions Pending が 0、PCIe 6.0 7.5.1.1.3・7.5.3.5）が止まりを確かめた時だけ返す | `kernel/src/dev/hda.c`、`kernel/src/dev/pci.c` の `bus_stop`、`kernel/src/io/resource.c` | 直 |
| H39 | 共通の CPU のコードに hypervisor の判定がある（`vm_cpu_guest`）。VM の処理は `vm/` の後ろに置く約束 | `kernel/src/arch/cpu.c:137`、`:163` | 未 |
| H40 | IBPB：`cpu_ibpb` に呼ぶ者が無いのに、起動の行が「IBPB between apps」と書く | `kernel/src/arch/cpu.h:164`、`kernel/src/arch/cpu.c:261` | 未 |
| H41 | PCI に抜き差し（PCIe の Hot-Plug、ACPI の Notify）と AER が無い | `kernel/src/dev/pci.c` | 未 |
| H42 | 製品のカーネルで virgl が結ばれない：`virtio_gpu` の attach と要求が試験の旗（`gpu-enable`、`nogpu`、`gpu-slow-test`、`gpu-timing`）で製品の分岐を変える。製品の `test_flag` は常に false | `kernel/src/vm/virtio_gpu.c:296`、`:324`、`:707`、`:813`、`kernel/src/kernel.h:76` | 未 |
| H43 | GPU のドライバー（claim）が動作中に落ちると、画面が既定の道（起動の画面）へ戻らない。装置の管理者が claim の死で結び直さない | `docs/records/audit-gpu-generic-20261102.md` の F1 | 未 |
| H44 | `nvrm` の帰線とフリップの thread が fair 級。プロセスの実装が期限の級を使う口が無い（`n_thread_schedule` が `ports/nvidia` に無い） | `ports/nvidia` | 未 |
| H45 | USB の実装の状態が `void *`（`dev/usb.h` の `hub`、`state[]`）。監査の K5 | `kernel/src/dev/usb.h` | 未 |
| H46 | 電源でカーネルが郵便箱になる（`SYS_POWER_SAY`）。監査の K9 | `kernel/src/sys/syscall.c` | 未 |
| H47 | DMA の確保に中断点が無い：`hal_dma_alloc` は 0 埋めを一度の `memset` で行う。`virtio_gpu` の絵の backing（1 GiB まで）で、前の頁ごとの中断点（`gpu_checkpoint`）が消えた | `kernel/src/hal/dma.c`、`kernel/src/vm/virtio_gpu.c` | 未 |
| H48 | 写しの口：`hal_map` は BAR を丸ごと写し、外す口が無い（virtio 1.2 の 4.1.4 は要る部分だけを写すのを勧める）。割り込みの形（メッセージか線か）を言わず、`virtio_gpu` は `pci_device_of(d)->intx` を読む。物理番地を要る装置（ACCESS_PLATFORM の無い virtio）を DMA の口で言えない | `kernel/src/hal/resource.c`、`kernel/src/vm/virtio_gpu.c` | 未 |
| H49 | `pci_find_cap` は同じ ID の最初の capability だけを返す。virtio は ID 9 の全部を手の walk で辿る。PCI のバスの口に capability の列挙が要る | `kernel/src/dev/pci.c`、`kernel/src/vm/virtio_gpu.c` | 未 |
| H50 | 一つの装置の状態が file の static にある（`virtio_gpu`、`ata` の `disks[]`・`channels[]`、二度目の attach を断る門）。二つ目の装置を持てない | `kernel/src/vm/virtio_gpu.c`、`kernel/src/vm/ata.c` | 未 |
| H51 | 固定の節 "ide-ports" の slot の並び（channel c の命令の block が 2c、制御が 2c+1）を書くのは `ata.c` だけで、出す側（`vm/vm.c` の `publish_ide_ports`）に無い | `kernel/src/vm/vm.c` | 未 |
| H52 | 光学ドライブ（ATAPI）を扱わない：AHCI は「port 2: an ATAPI drive, skipped」と言って飛ばす。SCSI の MMC の命令、読むだけの block、媒体の入れ替えが無い。q35 の CD は台帳にも出ない | `kernel/src/dev/ahci.c` | 未 |
| H53 | SMBus の制御器（q35 の 8086:2930）に実装が無い。設定の窓は PCI の class だけで種類を言い、SMBus（0Ch の 05h）を「USB などのバス」と書く | `kernel/src/dev/`、`user/apps/settings.c` の `hardware_kind` | 未 |
| H54 | usbhid の起動の行の文が崩れる（「is a absolute pointer」） | `kernel/src/dev/usbhid.c` | 直 |
| H55 | 利用者に英語が見える：端末の窓の題と案内の行、メモ帳（「untitled - Text Editor」、New・Open・Save・Save as）、ファイルの窓の保存（Up・Home・New folder・Save・Cancel・Name・Size・Type）、音の出口の名「line out」（カーネルの codec の pin の内部の値を窓が出す） | `user/apps/`、`kernel/src/dev/hda_codec.c`、`user/apps/settings.c` | 未 |
| H56 | `audiotest` の「1 s の音が 948 ms で鳴り終わった」（下限 950 ms）。段 D の媒体で四回のうち一回（948、971、1032、972 ms）。`n_audio_close` は出口の位置（HDA の LPIB から `user/apps/audio.c` の heard）が流れの終わりを過ぎるまで待つ。段 D の hda の差分は位置の道に触れていない（整形だけ）。前の記録も前の写しも無く、原因は確かめていない | `tests/audiotest.c:148`、`user/apps/audio.c`、`kernel/src/dev/hda.c` | 未 |
| H57 | ブラウザー：ja.wikipedia.org の頁で `getItem`・`removeItem` を undefined から読む TypeError と、jqueryMsg の解析の誤り（F12 の console）。F12 で `localStorage` を読むと「UnknownError: Native storage read/write failed」。`AppData\Browser` に `storage` が無く、`AppData\Temp\Browser\storage` に同じ名の `.0`・`.2`・`.3`・`.4` がある。新しい C: の最初の頁でも起きる | `user/libc/web/`、`user/libc/webstorage.c` | 未 |

## 2. 実装ごとの事実

サブエージェント（Opus 5.5、読むだけ）の報告から。file:line は 2026-11-07 の main。

- DMA の取り方：`hal_dma_alloc` は tulip、e1000、ac97（BDL）、rtl8125、hda、nvme、ahci。pmm を直に：virtio_gpu、xhci（`dma_page`・`dma_block`）、vmbus、netvsc、storvsc、hv.c。`shared_kernel_contig`：ac97、hda の流れの輪。
- 装置へ渡す番地：`hal_dma_address` は virtio_gpu、nvme、ac97、hda、ahci。`bus - dma_offset` は ahci:1192/1284、xhci:290。`hhdm_virt_to_phys` は hda:872。
- 32 bit の守り方：bits=32 で取る（tulip、ac97、hda）、S64A で選ぶ（ahci、低い緩衝 1283）、AC64 が無ければ断る（xhci）。
- quiesce と resume：その場で立て直す（vbe、e1000、rtl8125、ac97、hda）、thread に回す（nvme、ahci、xhci）、true を返すだけ（USB の class、ata、iommu、acpi の system）、無い（virtio_gpu、VMBus の実装）。
- detach が無い：vbe、ata、tulip、e1000、ac97、virtio_gpu、rtl8125、hda、iommu（どれも抜き差しの無いバス）。
- HAL の下へ届く所：`pci_device_of` は PCI の実装の全部。`pci_find_cap`・`pci_read32`・`pci_write32`（rtl8125 の D0）、`pci_disable_busmaster`（nvme、ahci、xhci、hda）、`pci_function_reset`・`pci_reset_left`（nvme）、`iommu_map`・`iommu_unmap`・`iommu_cache_flush`（hda）。

## 3. 決めたこと

| # | 決定 | 理由 |
|---|---|---|
| D1 | 実装の電源の段は、電源の thread で割り込みを開けたまま走る `suspend(d, 目標)` と `resume(d)`。割り込みを切った段で実装を呼ばない。`quiesce` を消す | H1。眠れない段に押し込むと、実装は完了の割り込みを待てず、NVMe の shutdown のような秒の単位の手順を回して待つしかない |
| D2 | 装置の電源の状態（D0・D3）を HAL が持つ。眠り（S3）では、実装の suspend → バスの電源（PCI：構成の保存、PMCSR を D3hot）→ firmware の電源（ACPI：`_PS3`、参照の無くなった電源資源の `_OFF`）を子から。戻りは逆。休止の写しと電源断では D 状態を変えない | H2、ACPI 6.5 16.1.6 の 4、7.3、PCIe 6.0 5.9。16.1.7（S5）は D 状態を求めない |
| D3 | 装置の firmware の節を HAL の考えにする（`hal_device` の firmware の口）。評価と Notify はバスを問わない claim の呼出しにし、画面の保持は画面の呼出しへ移す | H4、K4 |
| D4 | 機械の核が直に使う装置（HPET、RTC、割り込みの link、PCI のホスト、電源のボタンと蓋、シリアルの console、IOMMU）は、持ち主の部品が宣言する「機械の装置」にする。偽の attach を消す。機械の装置は結ばず、claim できず、電源の段は持ち主のもの | H5、H15 |
| D5 | claim はサービスの管理者が装置ごとに作り、実装のプロセスへ handle で渡す。実装のプロセスは `N_CAP_HAL_CLAIM` を持たない | H13 |
| D6 | サービスの管理者は台帳を見張り、装置が来たら実例を作り、去ったら止めて消す | H7 |
| D7 | DMA の契約：装置の番地の幅を実装が申告し（`hal_dma_bits`）、長く使うメモリーは `hal_dma_alloc`、既にあるメモリーは `hal_dma_map`・`hal_dma_unmap`。幅を超えれば HAL が予約した緩衝を通す。装置が domain に入っていれば、バスがその domain に写す。`dma_offset` と `hal_dma_address` を消す | H8、H9、H16、H17、H18 |
| D8 | PCI の根の機能は ACPI のホストブリッジの下に出す。AML が無ければ根の下 | H6 |
| D9 | 範囲の外を作らない。H20〜H25 と H26 以降は全部この仕事で直す。`docs/open.md` へ回さない | 目的は「どんな装置も、実装さえあれば HAL を変えずに」。H22〜H25 はそれに直に当たる |
| D10 | 混成の層：核の CPU（`arch/`）、機械の HAL（`hal/`）、核、装置の管理者（`io/`）、実装の ABI（`ddk/`）、実装の像（`drivers/`）、プロセスの実装（claim）。全部 ring 0（プロセスの実装を除く）、境は組みが強いる | H26〜H28。Gemini の資料の hybrid の形（NT、XNU）：性能のため ring 0、層の境は厳しく、実装の口は版の付いた二進の口 |
| D11 | 機械の HAL はカーネルの像の中の別の部品（NT の Windows 10 2004 以降の形）。口は `hal/hal.h` 一つ。核と装置の管理者は機械にこれだけで触る。`hal/` の外は機械の部品の見出しを include できない（`tests/check_boundaries.py`） | 機械は x86-64 の UEFI と ACPI の一種で、起動のごく初めから要る部品を別の像にする益が小さい。実装は別の像なので、移す益はそちらで得る |
| D12 | 実装は Nocturne の像の形式（`docs/open.md` にあった決めたこと：我々の形式を作り、ELF は組みの中間物と Limine が読むカーネルの像にだけ残す。導入の形式もドライバーを同じ形で入れる）。組みが ELF の ET_DYN から変える。プログラムと利用者の空間の像（`C:\Nocturne\Drivers` の GPU の像）も同じ形式を読む。カーネルは輸出の表（名と番地と版）を持ち、読み込みが再配置と輸入を解く。組みは輸出の一覧から作る名ばかりの共有 object に `-z defs` で結ぶ：ABI の外の記号を使う実装は組めない。規則は像の注記の節にあり、像を走らせずに読む。起動に要る像（PCI、ACPI、NVMe、AHCI、xHCI と USB とディスク、VMBus と storvsc、画面）は起動の区画から Limine の module で、ほかは `C:\Nocturne\Drivers` から、合う装置が出たときに読む | H27。実装を足し・移すのにカーネルを組み直さない |
| D13 | 実装の ABI（DDK）は中身を見せない handle と、大きさの欄を持つ構造体だけ。核のサービス（メモリー、錠、待ち、thread、仕事の列、時計と期限、ログ、乱数、CPU と node の数）、装置の口（性質、資源、写し、ポート、割り込み、DMA、電源、子を出す、口の提供と引き）、種類の口（block、網、音、入力、画面）、バスの口（PCI の構成、USB の転送、I2C・SPI・UART の転送、GPIO の線と割り込み）。版は major と minor：同じ major の大きい minor のカーネルが読む | H26。核の中が変わっても実装が壊れない |
| D14 | バスも実装の像：PCI（ECAM は機械の HAL から）、ACPI（uACPI、名前空間の列挙、firmware の電源、評価と Notify、`_PRT` の割り込みの道、電源のボタンと蓋と GPE、眠りの値を電源の管理者へ）、USB（xHCI の host の実装と、USB の核と hub の実装）、VMBus、I2C・SPI・GPIO・UART の制御器。バスの装置はどれも本物のバスの操作（割り込み、DMA、電源、リセット、claim）を持つ | H29。ACPI は NT の acpi.sys と同じく firmware のバス |
| D15 | 装置の間の口：提供者が自分の装置に型つきの口を置き、使う側が引く（GPIO、I2C、SPI、UART、USB の host、HDA の codec のバス、firmware の割り込みの道）。資源に接続の種類（提供者の装置と記述）を足す：ACPI の `GpioInt`・`GpioIo`・`I2cSerialBus`・`SpiSerialBus`・`UartSerialBus`・`CSI2Bus`。GPIO の割り込みは提供者を通して普通の割り込みと同じ口で渡る | H22。NT の resource hub と SPB に当たる |
| D16 | DMA の層（`io/dma.c`）：装置へ渡す番地はバスが決めて返す。長く使うメモリー（取る・返す）と、要求ごとの流れの写し（区切りの並びと向きを写し、終わりに外す）。幅の外は DMA の層の緩衝（全部の種類が使える）。訳の領域（IOMMU）は IOVA を割り当てて写す。外向きの port の装置（ACPI の外向きの印、DMAR の SATC・`external`、IVRS の印）は既定で隔離し、内向きは素通り（方針は設定で選べる）。キャッシュを覗かない写しは DMA の層が書き出す。`hal_dma_reach` と block の緩衝を消す | H31。NT の DMA adapter（GetScatterGatherList）と Kernel DMA Protection の形 |
| D17 | 電源と寿命：系の walk（段 C）に加え、動作中の電源の管理（使わない装置を D3、起こしの道つき）、起こしの元（`_PRW`、`_DSW`、GPE、PCI の PME、USB の remote wake）と設定の窓の項目、固まった装置のリセットと結び直し（バスのリセットの段：FLR、SBR、port のリセット）、取り外しで記録と thread を全部返す、レジスターを期限つきで待つ口を DDK に一つ。USB のディスクは同じ装置と分かれば続けて使う。`nvrm` と `firmware-screen` も walk に加わる | H20、H21、H23〜H25、H32 |
| D18 | 利用者の ABI の名を持ち主に合わせる：`n_hal_*` → `n_device_*`、`SYS_HAL_*` → `SYS_DEVICE_*`、`common/hal_abi.h` → `common/device_abi.h`。プロセスの実装の口を USB・I2C・SPI・UART・GPIO へ広げる（ABI §9） | H34、H29 |
| D19 | 確かめ：段ごとに `nocturne-project` で、ログインの先のデスクトップで装置を使い、serial.log を全部読む。ゲストの試験（`tests/` と、Nocturne の中で clang で組む試験）と、試験用の実装の像（ソフトの GPIO・I2C の提供者と、`-acpitable` の SSDT）で接続の資源の道を確かめる。QEMU に無い制御器（AMD・Intel の GPIO、DesignWare の I2C、I2C の HID）は、ソフトの提供者の試験と実機の記録に分けて書く | AGENTS.md の「確かめ方」。ログイン画面は何の証拠にもならない |

## 3.1 段 C の設計

照らした原文：ACPI 6.5 の 7.1〜7.3（電源資源、`_PSx`、`_PRx`）、7.4.1（`_PTS`）、7.4.4（`_TTS`）、7.5、16.1.3、16.1.6、16.1.7。PCIe 6.0 の 5.3.1（D 状態）、5.9（表 5-14）、7.5.2（PMC、PMCSR）。

**実装の電源の段（D1）**
- `struct hal_driver` の電源の欄：`bool (*suspend)(d, enum hal_power_target)`、`bool (*resume)(d)`、`refuse`、`enum hal_after_sleep after_sleep`。
  - `after_sleep`：`LOST`（既定。戻せない：結んでいる間、眠りも休止もしない）、`BOOT`（次の起動の firmware が戻す：休止だけを許す）、`RESUME`（suspend・resume で加わる。無い方は、することが無い）。
  - 目標：`SLEEP`（S3。この後、装置は電源を失う）、`IMAGE`（休止の写し。電源は残り、resume が続く）、`OFF`（電源断、再起動、休止の写しへ渡す。resume は来ない）。
  - 文脈：電源の walk の thread。割り込みは開き、全部の CPU が動いている。眠ってよく、自分の割り込みを待ってよい。実装は自分のデータの道（送信、要求、描画）を自分の錠で閉じる。実装の thread の `hal_busy_begin` は walk の間待つ。
- walk：動いている装置（カーネルの実装か claim を持ち、機械の装置でない）だけを数えた段。段は木の親と依存先を辿った深さ。止めるのは深い段から、戻すのは浅い段から。同じ段の装置は装置ごとの thread で同時に走らせる（NVMe の shutdown のような秒の単位の手順を足し算にしない）。claim も同じ walk に入る（`N_HAL_POWER_SUSPEND` を渡して答えを待ち、戻りは `N_HAL_POWER_RESUME` を渡す）。
- 止まらない装置があれば、その段の後で walk を止める。戻す walk は、止まった印と電源を落とした印の付いた装置だけを戻す。`OFF` は止まらない装置があっても全部を回る。
- 消すもの：`quiesce`、`cold_ok`、`hal_quiesce_all`、`hal_resume_all`、`hal_claims_suspend`、`hal_claims_resume`、各実装の quiesce と resume（割り込みを切った文脈のための回る待ち、錠を取らない道、`RFLAGS.IF` を読む分かれ道）。

**装置の電源の状態（D2）**
- `SLEEP` だけ、各装置の suspend の後に二つを行う。
  - バスの電源（`hal_bus_ops.power`）。PCI は構成を保存して PMCSR を D3hot にし、10 ms 待つ（表 5-14）。
  - firmware の電源（`struct hal_firmware_ops`、firmware の持ち主が置く）。ACPI は `_PS3` を呼び、`_PR0` の参照を放し、参照の無くなった電源資源を順の値の大きい方から一つずつ `_OFF` する（7.2、7.3.11）。firmware の段は、段ごとに walk の thread でまとめて一つずつ行う。
- 戻りは段ごとに、firmware → バス → 実装の resume の順。
  - firmware：`_PR0` の資源を参照し、切れていたものを順の値の小さい方から `_ON` し、その後 `_PS0`（7.3.8）。
  - バス：準備の待ち、D0、10 ms（Immediate_Readiness_on_Return_to_D0 なら待たない、7.5.2.1）、構成を戻す。
- 結ぶ前（attach、claim）にも装置を D0 にする（firmware が D3 で渡した装置のため）。`rtl8125` の PMCSR の手書きを消す。
- ACPI の電源資源の記録：AML の初期化の後、名前空間の全部の Device の `_PR0` を読み、資源の `_STA` から D0 にある装置の参照を数える。実装の無い装置の参照も数え、共有の資源を切らない。S3 の後は、参照のある資源を PCI の構成を戻す前に `_ON` し直す（7.2.3：繰り返してよい）。
- PCI：HAL が D3 にした機能は、`pci_suspend` が保存しない（D3hot ではメモリー空間に届かない、5.3.1.4.1）。`pci_resume` も戻さない（HAL の戻りが firmware の後で戻す）。claim の PMCSR の守りは「D 状態は HAL のもの」。

**機械の順（H3、H14）**
- S3：
  1. 拒みの確かめ。
  2. `_TTS(3)`（7.4.4）。
  3. store_sync、blk_hold、flush。
  4. `SLEEP` の walk（16.1.6 の 4）。
  5. `_PTS(3)`（16.1.6 の 5）。
  6. ほかの CPU を止める（16.1.6 の 6）。
  7. PCI の構成を保存し、AML を凍らせ、割り込みを切り、入る。
  8. 起きたら：時計、IOMMU、APIC、割り込みの線、ACPI の link、電源資源、PCI の構成、`_WAK`（7.5 の 9）。
  9. ほかの CPU を戻し、凍結を解く。
  10. 戻す walk（7.5 の 10）。
  11. `_TTS(0)`（7.5 の 11）。
  - 取りやめでも `_TTS(0)` を呼ぶ。`_PTS` の後の取りやめは `_WAK` も呼ぶ（7.4.1）。
- 休止の写し：拒み → store_sync、blk_hold、flush → `IMAGE` の walk → CPU を止める → 写し → 戻す → 写しを書く → 電源断。
- 電源断：`_TTS(5)` → `_PTS(5)`（16.1.7 の 1）→ blk_hold、flush（16.1.7 の 2）→ `OFF` の walk → 切る。再起動は `_TTS`・`_PTS` なし。
- 休止からの戻し（起動した側のカーネル）：`OFF` の walk → CPU を止める → 写しへ跳ぶ。
- H12：`pm_sleep_check` は起動で一度、機械の部分（IOMMU、S3、起こしの頁、休止の置き場）だけを決める。文を事実に直す。

**書くときに決めたこと**
- 電源の状態は三つ：`HAL_D0`、`HAL_D3HOT`、`HAL_D3COLD`。バスの `power(d, to)` の D3COLD は「電源が切れた」の知らせで、PCI は D0 へ戻すときに link と準備を待つ（PCIe 6.0 6.6.1、5.3.1.4 の No_Soft_Reset が無ければ構成は失われている）。
- firmware の口は節ごと：`hal_firmware_ops.power(node, to, &cold)`。cold は「この節の電源資源を切った（または入れた）」。walk はそれでバスへ D3COLD を知らせる。ACPI の持ち主は一つの mutex で `_PSx` と資源の評価を並べる（7.3.15 の `_IRC` の順を壊さない）。
- 段の中の並びは仕事の列：walk の thread が段の装置を列に置き、助けの thread（`kthread_create` が失敗しても並びが減るだけ）と一緒に取る。段の終わりで全部を待ち、失敗があれば次の段へ進まない（`OFF` を除く）。
- 結ぶ前の D0：`bind_thread` の attach と `claim_begin` の前に `hal_power_up`（firmware の D0 → cold なら D3COLD の知らせ → バスの D0）。D0 に戻らない装置は結ばない（`-N_ERR_IO`）。
- claim は門の中：`hal_claim_open` と claim の終わりは `hal_busy_begin` の中で走り、walk と重ならない。walk の中の claim は `hal_claim_suspend` が SUSPEND を渡して答えを待ち、`hal_claim_resume` が RESUME を渡す。
- 眠れるかの問い：`hal_power_cannot(hibernate)` が結んでいる実装の `after_sleep` と claim の加わりを見る。`pm_can_sleep`・`pm_can_hibernate` は機械の部分とこれの両方。

**確かめ方**
- WHPX は S3 から戻れない（`qemu-visual.md`）。TCG で必須の CPU の機能が揃うかを見る。揃わなければ、S3 の戻りは未確認と書く。
- QEMU の表には電源資源が無い。試験の SSDT（`-acpitable`）で電源資源と `_PS0`・`_PS3` を持つ装置を作り、試験のプログラムが claim する。浅い段の claim が断ると取りやめになるので、深い段の装置の D3 と D0、同じ段の NVMe・e1000・xHCI の D3hot から D0 と構成の戻りを、眠らずに見られる。
- 休止の全部の輪、電源断、再起動を QEMU で見る。

## 3.2 実装ごとの電源の段

サブエージェント四つ（Opus 5.5、作業木ごと）が書き、差分を読んでから取り込んだ。どれも `after_sleep` は `RESUME`（`usbmsc` を除く）。

| 実装 | suspend | resume |
|---|---|---|
| `nvme` | `asleep` で道を閉じ、I/O の命令の終わりを待ち、I/O の提出の queue、続けて完了の queue を消す（3.6.1、5.3.3、5.3.4）。SLEEP と OFF は CC.SHN の正常の shutdown と CSTS.SHST の完了を待つ | Controller Reset から 3.5.1 の順で立ち上げ、Identify で同じ装置かを見る。眠りの間の要求は retry の列から出る |
| `ahci` | port ごとに `asleep`、命令の終わりを待つ。SLEEP と OFF は FLUSH CACHE、STANDBY IMMEDIATE、Slumber、engine を止めて listen（10.10.3） | adapter を 10.1.2 の順で立ち上げ、円盤が居うる port を見直す |
| `xhci` | 装置を深い方から：端点を閉じ、SP つきの Stop Endpoint、SuperSpeed の hub の port を U3。root port を U3、制御器を止める。SLEEP は Save State（4.23.2）。OFF は HCRST | 状態を持つか Restore State で戻せれば R/S、port を U0（4.15.2）、浅い装置から端点を開ける。戻らない装置と状態を失った制御器は、門が開いた後に worker が数え直す |
| `usbhid`、`usbnet` | 押されたキーを離し報告を捨てる／送ることを閉じる | 受け直す／開けて `net_rx_notify` |
| `usbhub` | 無し（木の順は `xhci` が辿り、`usbhub_link_suspend`・`_resume` を呼ぶ） | 無し |
| `usbmsc` | OFF だけ（`LOST`：眠りと休止は USB のディスクを挿している間しない） | — |
| `hda` | stream を止め、codec の function group を D3、link を reset（4.5.8.2） | link を reset から上げ、thread が codec を初めから戻す（4.5.8.1） |
| `rtl8125` | 道を閉じて MAC を reset。SLEEP と OFF は D3 の前の設定と PHY の電源断（Realtek の FreeBSD の driver の手順） | `hw_init` から立て直し、道を開く |
| `e1000`、`tulip`、`ac97`、`vbe`、`ata` | 送受を止め reset ／過程を止め reset ／RPBM を落とす ／フリップを覚える ／channel の錠で要求を待たせる | 同じ初期化の関数で立て直す ／`ata` は IDENTIFY で同じ drive かを見る |

## 3.3 段 D の設計

照らしたもの：PCI の DMA の番地の幅（32 bit は全部の機能が持ち、64 bit は機能ごとの申告：NVMe 1.4 の PRP と SGL は 64 bit、AHCI 1.3.1 の CAP.S64A、xHCI 1.2 の HCCPARAMS1.AC64）、`dev/iommu.h` の domain と guest の約束、`dev/blk.h` の要求の形。

**持ち主**
- 装置が届く番地の幅は実装の申告（装置の register が言う）。HAL が持ち、`hal_dma_alloc` と `hal_dma_reach` が使う。既定は 32（PCI の床）。
- 装置へ渡す番地を決めるのは HAL とバス。実装は物理番地から番地を作らない（`dma_offset`、`hal_dma_address`、`bus - dma_offset`、`hhdm_virt_to_phys` を消す）。
- 訳の領域（IOMMU の domain）に装置を入れるのはバス。HAL は装置の DMA のメモリーの一覧を持ち、装置が領域に入るとバスにその全部を写させる。実装は IOMMU を知らない。
- 区切りの DMA（block の要求）は `blk.c` が一か所で扱う。届かない区切りは `blk.c` の緩衝（今ある `bounce_io`）を通す。HAL に二つ目の緩衝を作らない（D7 の「HAL が予約した緩衝」はこれに替える：同じ責務を二つ持たない）。

**口（`hal/hal.h`）**
- `void hal_dma_bits(struct hal_device *d, unsigned bits)`：attach の中、DMA のメモリーを取る前に。
- `void *hal_dma_alloc(struct hal_device *d, size_t bytes, uint64_t *bus)`・`void hal_dma_free(struct hal_device *d, void *memory)`：長く使うメモリー（輪、記述子、装置の文脈、送受の緩衝）。幅の中、装置の node の近く、0 埋め。装置が領域にいればそこに写る。眠ってよい thread から。装置が止まったと確かめられない限り、片づけで返さない（今のとおり）。
- `bool hal_dma_map(struct hal_device *d, uint64_t phys, uint64_t pages, uint64_t *bus)`・`void hal_dma_unmap(struct hal_device *d, uint64_t phys, uint64_t pages)`：ほかの持ち主のメモリー（共有 object の音の輪）を長く装置へ渡す。幅の外なら false（呼ぶ側は幅の中で取り直す）。眠ってよい thread から。
- `bool hal_dma_reach(const struct hal_device *d, uint64_t phys, uint64_t bytes, uint64_t *bus)`：データの道（どの文脈でも）。そのままで届く区間なら番地。領域にいる装置は、写した区間の中だけ。
- バスの方法：`hal_bus_ops.dma_map(d, phys, pages)`・`dma_unmap`。PCI は装置が domain にいればそこへ同じ番地で写す（`iommu_map`）。いなければ何もしない。
- 道が変わる知らせ：HAL の `hal_dma_pause(d)`・`hal_dma_restart(d, translated)`（`hal/dma.c`）が、実装の `struct hal_driver` の `dma_pause(d)`・`dma_restart(d)` を呼ぶ。pause は実装を止め（何も送らず、割り込みを止める。眠ってよい）、バスの `stop` で止めを確かめ、領域から外す。restart は一覧の全部を新しい道へ写してから実装を始める。両方を持つ実装だけが claim の group に入れる（`dev/iommu.c` の `iommu_guests_stop`・`iommu_guests_start`。claim が立てば `no_messages` と `claim_domain` を立てて restart）。止めた装置は電源の walk が飛ばす。
- CPU のキャッシュの書き出しは `arch/cpu.h` の `cpu_cache_flush`（IOMMU の表と、キャッシュを覗かない装置の両方が使う）。

**block**
- `struct blkdev` に `struct hal_device *dma`（データを DMA で動かす装置。CPU で動かす実装は NULL：`usbmsc`、`ata`）。
- `struct blk_segment` に `bus`。`blk.c` が区切りを作るときに `hal_dma_reach` で埋める。届かない区切りを持つ要求は `bounce_io` へ行き、緩衝は `hal_dma_alloc` で取る。実装は `bus` だけを読む。
- `ahci` の低い緩衝（`p->low`）と、S64A の無い道の写しを消す。

**実装**
- `nvme`：64。PRP と SGL は `segments[i].bus`。輪と一覧は `hal_dma_alloc`。
- `ahci`：S64A なら 64、無ければ 32。
- `xhci`：AC64 なら 64、無ければ 32（断りを消す、H18）。`dma_page`・`dma_block`・`pmm_*` を `hal_dma_alloc`・`hal_dma_free` に（装置の文脈と輪は USB の装置ごとに取り、返す）。
- `hda`：64BIT（GCAP の 64OK）で幅。CORB・RIRB・BDL は `hal_dma_alloc`、流れの輪は `hal_dma_map`。`iommu_map`・`iommu_unmap`・guest の欄を消し、`dma_pause`・`dma_restart` にする。キャッシュを覗かない流れの書き出しは `cpu_cache_flush`。
- `ac97`、`e1000`、`tulip`、`rtl8125`：幅の申告と `hal_dma_alloc`。`ac97` の流れの輪は `hal_dma_map`。
- `virtio_gpu`：`pmm_*` を `hal_dma_alloc` に、`vmm_map_mmio` を `hal_map` に、`pci_read32` を PCI の口に（H19）。`ata`：`hal_port` で（H19）。
- VMBus の実装（`netvsc`、`storvsc`）の GPADL の頁：VMBus は訳の無いバス。`hal_dma_alloc` に揃えるかは読んで決める。

## 3.4 混成の設計

**層と持ち主**

| 層 | 場所 | 持つもの | 使ってよい口 |
|---|---|---|---|
| 核の CPU | `kernel/src/arch/` | GDT・IDT・TSS、入口、文脈の切り替え、FPU、頁表、CPUID の機能、CPU ごとの区画、錠の命令 | — |
| 機械の HAL | `kernel/src/hal/` | 割り込みの制御器（ローカル APIC と x2APIC、IOAPIC、MSI の文の組み立て、vector の割り当て、再割り当て）、割り込みの物、時計と timer（TSC、LAPIC の timer、HPET、PM timer、RTC）、SMP と IPI と NMI、IOMMU（訳の領域と IOVA）、firmware の静的な表（MADT、SRAT・SLIT・HMAT・CEDT、DMAR・IVRS、MCFG、FADT、HPET）と topology、PCI の構成の空間の読み書き（ECAM）、眠りの入口（CPU の保存と起こしの頁、PM1 の書き込み）、リセットと電源断の register、キャッシュの書き出し、idle と性能の要求、MCA、console のシリアル、機械の装置の宣言 | `arch/`、核のメモリー |
| 核 | `kernel/src/{sys,mm,fs,net,gui}/` | スケジューラー、メモリー、object と IPC、syscall、C: の store、網の stack、窓の管理、電源の管理者（機械の眠りの順、休止） | `hal/hal.h`、`io/io.h`、種類の口 |
| 種類の層 | `kernel/src/class/` | block、網の adapter、音の出口、入力、画面と面。実装の側の口は DDK、使う側の口は核の中 | `hal/hal.h`、`io/io.h` |
| 装置の管理者 | `kernel/src/io/` | 装置の木、性質と規則、実装の一覧と読み込み、輸出の表、結びつけ、資源、割り込みと DMA の層、装置の間の口、電源（系の walk、動作中の管理、起こし）、リセットと結び直し、取り外し、claim、台帳 | `hal/hal.h` |
| 実装の ABI | `ddk/` | 版の付いた見出しと輸出の一覧。SDK に入る | — |
| 実装の像 | `drivers/<名>/` | バスと装置の実装。`ddk/` だけを include し、輸出だけを呼ぶ | DDK |
| プロセスの実装 | `user/`、`ports/` | claim で持つ装置の実装（GPU の `nvrm` など） | 利用者の ABI |

**実装の像**
- 形：Nocturne の像の形式（頭、区画と権限、再配置、輸入と輸出の表、注記、署名の場所）。組みは位置に依らない ELF64 の ET_DYN（`-fPIC`、見せる記号は入口だけ）を作り、ホストの道具が形式へ変える。カーネルの実装、プログラム、利用者の空間の像を、頭の種類で分ける。読む口は `common/` の一つで、カーネルの読み込みとプロセスの起動が使う。節の権限は text が RX、rodata が R、data が RW（NX）。
- 輸入：`GLOB_DAT`・`JUMP_SLOT` を輸出の表の名で解く。`RELATIVE` と `64` を当てる。解けない名があれば読まず、台帳に理由を出す。
- 注記：`.note.nocturne`（実装の名、DDK の版、規則の表、起動に要るか）。装置の管理者は像を写さずに注記だけを読み、規則の一覧を作る。合う装置が出たときに像を写して入口を呼ぶ。
- 入口：`n_driver_entry(module)` が一つ以上の実装を登録する。像を外すのは、結んだ装置が無く参照が 0 のとき。
- 組み：`ddk/exports.list` から、名だけを輸出する共有 object（`build/ddk/libnocturne_kernel.so`）と、カーネルの中の輸出の表（生成した C）を作る。実装はこれに `-z defs` で結ぶ。
- 起動：`scripts/layout.py` が起動に要る像を起動の区画の `boot/drivers/` へ置き、`limine.conf` に module として書く。C: の `C:\Nocturne\Drivers` に全部を置く。C: が開いた後、残りの規則を読む。

**DDK の口（形）**
- 装置：`n_device`（handle）。性質を読む、資源（メモリー、ポート、割り込み、接続）を引く、写す、ポートを読み書きする、割り込みを立てる（処理と、CPU の近さの望み）、DMA（幅、取る・返す、流れの写し）、電源の段、子を出す（バスの操作の表と一緒）、口を置く・引く、失敗と回復を言う、レジスターを期限つきで待つ。
- 実装の表：`struct n_driver`（大きさの欄、名、`attach`・`detach`・`suspend`・`resume`・`refuse`・`after_sleep`・`dma_pause`・`dma_restart`・動作中の電源の段・リセットの段）。
- バスの表：`struct n_bus_ops`（大きさの欄、割り込み、DMA の写し、電源、リセット、claim の口、子の身元）。
- 核のサービス：`n_alloc`（node つき）、`n_spin_*`・`n_mutex_*`（大きさの決まった不透明な物）、待ちと事象、thread、仕事の列、期限の callback、時計、ログ、乱数、CPU と node の数と近さ。
- 種類の口：`n_block_*`、`n_netif_*`、`n_audio_output_*`、`n_input_*`、`n_screen_*`。
- バスの口：`n_pci_config_*`・`n_pci_reset`、`n_usb_*`（端点、制御・bulk・interrupt・isochronous の転送）、`n_i2c_transfer`、`n_spi_transfer`、`n_uart_*`、`n_gpio_*`（線の読み書き、割り込み）。

**ここから決めること（段ごとの設計で書く）**
- 割り込みの物の形（共有の線、MSI の塊、CPU の近さ、再割り当て）と、HAL と装置の管理者の分け目。
- 輸出の表の版の上げ方と、互換を持たない範囲（同じ major の中だけ）。
- 種類の層の口（block の要求と DMA の写し、網の送受の輪）。
- ACPI の像と電源の管理者の口（`_TTS`・`_PTS`・`_WAK`・SLP_TYP、GPE と SCI）。

## 3.5 段 F の設計

**像の形式**（`common/image_format.h`）
- プログラム、利用者の像、カーネルの実装の像は一つの形式で、頭の kind で分ける。表は区画、基底の再配置、輸入、型つきの注記、名の文字列。輸出の表は像に無い：どの像も入口を一つ持ち、カーネルの輸出の表は DDK の一覧から生成する。
- 区画の中身はファイルの頁の境から：読み取り専用で slot の無い区画は文書の頁を共有して写す（今の `program_map`）。
- 利用者の像の再配置はカーネルが写すときに当てる。`user/libc/driver_image.c` は TLS（`struct n_image` で受ける）と構築子だけを行う。
- 変換が ELF の再配置を解く：`RELATIVE` と自分の記号への `64` は基底の再配置、未定義の記号への `GLOB_DAT`・`JUMP_SLOT`・`64` は輸入（実装の像だけ）、`DTPMOD64` は 1（C ライブラリーの写し一つが TLS の module 一つ）、`DTPOFF64` は addend。知らない型、ほかの kind の未定義の記号は変換の失敗。
- 規則は実装の C の表（`.nocturne.rules` の区画）に書き、変換が再配置で指を辿って `N_IMAGE_NOTE_RULE` にする。装置の管理者は像を写さずに注記だけを読む。

**変換と道具の鎖**
- `common/image_write.c`：ELF64 の ET_EXEC（プログラム、base は link の番地）と ET_DYN（像、base 0）から像を書く。ホストの道具 `host/image/` とゲストのプログラム `image` が同じ部品を組む。
- ホストの組み：Makefile のプログラム、試験、利用者の像、実装の像の link のあとに変換。`build/programs/` と `build/drivers/` に入るのは像だけ。
- ゲストの clang（`ports/llvm/nocturne.patch` の Nocturne の toolchain）はリンクのあとに変換の段を持ち、`-o` の名に像を書く。tcc は出力の段で変換の部品を呼ぶ。Go と Rust の移植の link も同じ。
- カーネルは ELF を読まない：`proc_launch`・`proc_image_map` の ELF の読み手と、tcc の古い組みを許す道（「entry point is not executable」）を消す。ELF が残るのは組みの中間物と、Limine が読む `kernel.elf` だけ。

**DDK**（`ddk/`）
- 見出し：`ddk.h`（版 `N_DDK_MAJOR`・`N_DDK_MINOR`、入口 `n_driver_entry`、規則の表の宣言）、`core.h`（メモリー、錠、待ち、thread、仕事の列、期限、時計、ログ、乱数、CPU と node）、`device.h`（装置、性質、資源、写し、ポート、割り込み、DMA、電源の段、子を出す、装置の間の口、失敗と回復、レジスターを期限つきで待つ）、バスの口（`pci.h`、`usb.h`、`acpi.h`）、種類の口（`block.h`、`net.h`、`audio.h`、`input.h`、`screen.h`）。形は 3.4 の「DDK の口」。
- 型：中身を見せない handle と、頭に大きさの欄を持つ構造体。錠と待ちは大きさを決めた不透明な物で、カーネルの側が `_Static_assert` で大きさを合わせる。実装はカーネルの見出しを include しない（`check_boundaries` が `drivers/` の include を `ddk/` だけに縛る）。
- 輸出の一覧 `ddk/exports.list`（名、一行に一つ）が宣言。`scripts/build_ddk.py` が `build/ddk/exports.c`（カーネルの表、名で並べ、DDK の見出しを include して `&名` を取る：一覧にあって見出しに無い名は組みの誤り）と `build/ddk/libnocturne_kernel.so`（名だけを持つ共有 object：見出しにあって一覧に無い名は、実装の `-z defs` の link の誤り）を作る。
- カーネルの側：DDK の関数は装置の管理者（`io/ddk_*.c`）と核の上に置く。DDK は ABI の境なので本物の継ぎ目で、包みの禁止に当たらない。
- 版：同じ major で、像の minor がカーネルの minor 以下なら読む。major を上げたら全部の実装を組み直す。

**読み込み**（`kernel/src/io/load.c`）
- 起動の束（`N_IMAGE_BUNDLE_MAGIC`、Limine の module `drivers`、`limine.conf` に一行）：装置の管理者が始まるときに全部の注記を読み、規則を登録する。束を作るのは `scripts/layout.py`（`N_IMAGE_DRIVER_BOOT` の像を集める）。
- C: が開いたら `C:\Nocturne\Drivers` の実装の像の注記を読み、残りの規則を登録する。同じ名の像は C: の方が新しい版なら替える。
- 合う装置が出たら：像を核の番地（`mm/vmalloc.c`）へ写し、再配置と輸入を当て、区画に権限を付け（text は RX、rodata は R、data は RW で NX）、`n_driver_entry` を呼ぶ。像は結んだ装置が無く参照が 0 で外す。読めない像（版、解けない名、壊れた表）は台帳に理由を出す。
- 起動の束に入る像：起動の媒体を読むまでの全部（PCI、ACPI、NVMe、AHCI、xHCI と USB の核と hub と usbmsc、VMBus と storvsc、画面）。

**組みの置き場所**
- 実装の像のソースは `drivers/<名>/`。`ddk/` だけを include し、`-fPIC -fvisibility=hidden`、`-shared -z defs -z now -Bsymbolic` で `libnocturne_kernel.so` に結ぶ。
- バスも像（D14）：PCI は構成の空間を機械の HAL から DDK の `n_platform_*`（バスの実装だけが使う部分）で受ける。ACPI の像は uACPI を持ち、名前空間・評価・Notify・電源の資源・眠りの値・GPE と SCI を、電源の管理者と装置の管理者へ型つきの口で渡す。機械の HAL は静的な表（MADT など）を自分で読む。VMBus の像は Hyper-V の機械の装置が置く口（hypercall、SynIC）を引く。

**段 F の分け方**
- F1：像の形式の読み手、変換、道具の鎖（ホスト、ゲストの clang・tcc、Go、Rust）、カーネルのプログラムと利用者の像の読み込み。ELF の読み手を消す。
- F2：DDK の見出しと輸出の一覧と生成、カーネルの側の DDK、`io/load.c`、起動の束、`C:\Nocturne\Drivers` の走査。`struct io_driver` と区画 `.io_drivers` を消す。
- F3：実装を `drivers/<名>/` の像に（block、網、音、入力と USB の装置、画面、VM の装置）。
- F4：バスを像に（PCI、VMBus、ACPI）。USB の核は段 G。
- F5：導入の形式（宣言と中身。実装の像、任意の機能、アプリを同じ形で入れる）。
- F1 と E は別のファイルを触るので並べる。F2 は E の後。F3 と F4 は F2 の後に実装の組ごとに並べる。

## 4. 段

| 段 | 中身 | 状態 |
|---|---|---|
| A | 機械の装置（D4）、木（D8）、H10、H11 | 済：宣言 `struct hal_machine`（`.hal_machine`）、firmware の節 `struct firmware_node`、ACPI の二度の出し方、QEMU で木を確かめた |
| B | firmware の口（D3）、claim の渡し方（D5）、台帳の見張り（D6） | 済：`dev/acpi_claim.c`（`SYS_HAL_ACPI_EVAL`・`_NOTIFY_TAKE`、`N_ACPI_PARENT`）、`SYS_SCREEN_HOLD`、`n_hal_claim` の実装の名、管理者が claim を取って `N_SERVICE_HANDLE_DEVICE` で渡す、台帳の見張りで実例を作り・消す、見張りの相手へ一覧の全部。確かめは下の 5 |
| C | 電源（D1、D2、H3、H12、H14） | 済：`hal/power.c` の walk、`dev/acpi_power.c`、PCI の `power`、`dev/pm.c` の機械の順、claim の段、実装ごとの suspend・resume（3.2）。確かめは下の 5 |
| D | DMA の長く使うメモリー（D7）、H19 | 済：口（`hal/dma.c`）、全部の実装（xHCI、virtio_gpu、ata、block と NVMe と AHCI、HDA、AC'97、e1000、tulip、rtl8125、netvsc、storvsc）、VMBus のバスの stop（host が持つかもしれない GPADL を覚える）、`check_boundaries` の規則 7。確かめは下の 5 |
| E | 機械の HAL を分ける（D10、D11）：`hal/` と `io/`、H28、H30、H33、H36、H37、H40、H41 | 未 |
| F | 実装の ABI と像（D12、D13、D16）：像の形式、DDK、輸出、読み込み、組み、起動の module（NVMe、SATA、USB のどれからでも起動する）、DMA の層、全部の実装とバスを像に（D14）、プログラムも同じ形式に、導入の形式（宣言と中身。ドライバーと任意の機能とアプリを同じ形で入れる）。H26、H27、H31、H39、H42、H47〜H51 | 未 |
| G | USB を本物のバスに（D14、H29、H45）、USB のプロセスの実装 | 未 |
| H | 接続の資源と提供者（D15、H22）、GPIO・I2C・SMBus の制御器の実装、I2C の HID、i8042、光学ドライブ（H52、H53） | 未 |
| I | 電源と寿命（D17、H20、H21、H23〜H25、H32、H43、H44、H46）、H35、H56 | 未 |
| J | 利用者の ABI の名（D18、H34）、文書（`docs/hal.md`、ABI の §9、地図）、利用者に見える英語（H55）、H57、全体の確かめ、台帳を閉じる | 未 |

## 5. 確かめの記録

**段 B（コミット 6babecc2 の媒体、QEMU 窓付き、WHPX、q35、`intel-iommu,intremap=off,caching-mode=on`、`edu,dma_mask=0xffffffff`）**
- `nvrmtest`：0 failed。試験が edu を `nvrm` の名で claim し、`N_SERVICE_HANDLE_DEVICE` で nvrm に渡す。nvrm は動き、終わると claim が終わり、試験の名で取り直せる。
- `pcitest`（起動の画面を `bochs-display` にした回）：ACPI の口（`N_ACPI_SELF`・`N_ACPI_PARENT`・`N_ACPI_CHILD` の評価と断り）と `n_screen_hold` の場合に失敗は無い。5 件は画面に描かれた印の数の確かめで落ちる（refresh の分からない画面での合成の振る舞い。H35）。
- 前提の食い違い：`-device VGA` ではカーネルの `vbe` が VGA を持つので、pcitest の VGA の場合は claim できない（busy）。AMD の IOMMU は QEMU で x2APIC に要る形式（XTSup）を持たず使われない。WHPX は split の irqchip を持たないので、Intel の IOMMU は割り込みの再割り当てなしで使う。
- `C:\Tests` で tcc で組んだプログラムは公開の呼出ししか持たず、claim の能力を引けない。pcitest と nvrmtest はホストで組み（`build/tests/pcitest`、`build/tests/nvrmtest`）、試験の媒体の `C:\Nocturne\System` に置く。
- 確かめていないこと：管理者が台帳の見張りで装置ごとの実例を作り・消す道（QEMU に宣言に合う装置が無い）。

**段 C（コミット 88c1d6fc の媒体、xHCI の実装の前。QEMU 窓付き、WHPX、q35、`intel-iommu,intremap=off,caching-mode=on`、NVMe の起動、e1000、`intel-hda`＋`hda-duplex`、`edu` 二つ、試験の SSDT（作業場の `power_ssdt.py`）、`-device VGA`。xHCI は外した：まだ実装の電源の段が無く、眠りを拒む）**
- 取りやめの walk（`powertest`、作業場）：0 failed。NOCT0001 と edu 1 は SUSPEND に止まったと答え、edu 2 は止まらないと答える。同じ段の全部（NVMe、AHCI、e1000、HDA、vbe、三つの claim）が SLEEP で止まり、取りやめで戻る。NOCT0001 の記録は "3abBA0"：`_PS3`、資源を順の大きい方から `_OFF`（共有の PRC0 は残る）、小さい方から `_ON`、`_PS0`。edu 1 は D3hot から D0 と構成の戻しの後に答える。止まらないと答えた edu 2 も RESUME を受ける。戻った後、AHCI は port を見直し、NVMe の C: は書ける（電源断の段まで失敗の行が無い）。
- S3 の入り（`powertest sleep`）：QMP の `query-status` が `suspended`。walk の全部の装置が止まり D3 になり、`_PTS`、CPU の停止、入りまで通った。WHPX は S3 から戻れない：戻りは確かめていない。
- 休止の全部の輪（`powertest hibernate`、作業場の qcow2 の overlay）：一度目は IMAGE の walk、写し（30 MiB を 152 ms）、戻る walk（AHCI の見直し）、OFF の walk、電源断。二度目の起動は写しへ戻り「awake after 30 s」「the image is removed」、続く電源断も通る。
- 見つけて直したこと：止まらないと答えた装置に resume が来なかった（`hal/power.c`、3811a86a）。眠る前から使えない装置が眠りのたびに「戻らなかった」と言う形（7f414941）。止まらない suspend が自分で戻す二つ目の道（88c1d6fc）。
- xHCI の取り込みの後（コミット 0aadf56b の媒体、`qemu-xhci`＋`usb-kbd`＋`usb-tablet` を足した）：取りやめの walk は 0 failed で、xHCI と USB の実装の失敗は無い。S3 の入りは QMP の `suspended`。休止の全部の輪では、写しから戻ると制御器が状態を失ったと分かり（起動のカーネルの電源断が HCRST した）、装置を外して立て直し、port 5 と 6 の装置を数え直す。「普通の起動で、タブレットで欄を押しキーボードで打てる」は、最初の利用者を作る画面だけを見た記録で、確かめになっていない。
- 試験の側の注意：`n_power_request` の眠りと休止はすぐ戻る（pm の thread が後で行う）。試験の終わりの電源断の頼みが先に来ると、そちらが行われる。
- 確かめていないこと：実機と Hyper-V。tulip、ata、rtl8125、ac97、usbmsc、usbnet の walk（QEMU のこの構成に無い）。S3 からの戻り（WHPX）。
- 段 B と段 C の確かめは足りない：ログインの先のデスクトップで装置を使っていない（ファイルの保存と再起動、音、網、開始メニューからの眠りと休止とその後の装置）。serial.log の全部と QEMU の stderr（H33）を読んでいない。段 D の媒体で、この手順の全部をやり直す。

**段 D（コミット 71a74e77 の媒体、QEMU 窓付き、WHPX、q35、`intel-iommu,intremap=off,caching-mode=on`、e1000、`qemu-xhci`＋`usb-kbd`＋`usb-tablet`、`intel-hda`＋`hda-duplex`、qcow2 の overlay で書込みが残る）**
- NVMe の起動：最初の利用者を作り、デスクトップへ。起動の行は段 C の媒体と同じで、違いは xHCI の「64-bit DMA」だけ（作業場の `serial-d1.log` と `serial-c1.log`）。
- 端末で試験の源をホストから `fetch`（e1000）し、ゲストの clang と tcc で組んだ。`dmadisktest write` と `check`（13 の文書、35 MB、block の端と blk.c の緩衝の端と MiB の単位、1 MiB の塊と頁の中の半端な位置の塊）：0 failed。`fstest`：0 failed。`audiotest`：四回のうち三回 0 failed、一回 H56。
- 設定の窓：音の頁（HDA の出口、48000 Hz）と装置の頁（PCI の木、USB の装置、nvme・e1000・xhci・usbhid・hda・ahci が動かす。SMBus は実装なし：H53）。
- ブラウザー：`browser --maximized` で ja.wikipedia.org の「月」を開き、本文と画像が出て、wheel で送れる。F12 の console は H57。
- 開始メニューの「電源を切る」の後に起動し直し、冷えた cache で `dmadisktest check`：35 MB を 399 ms（cache からは 17 ms）、0 failed。作った利用者でログインできる。
- 開始メニューの「休止」：写し 99 MiB を 402 ms で書いて電源断。起動し直すと写しへ戻り、端末の窓と文が戻る。xHCI は状態を失ったと分かって数え直し、キーボードとタブレットが効く。戻った後に `dmadisktest check`、`fetch`、`audiotest` は 0 failed。
- 開始メニューの「スリープ」：QMP の `query-status` が `suspended`。WHPX は S3 から戻れない。
- AHCI の起動（NVMe を外し `-device ahci` に disk）：C: は `sata0.0`。`dmadisktest write` と `check`、電源断の後の冷えた `check`（551 ms）：0 failed。この構成の QEMU は CD を作らない（`info block`）ので、ATAPI の行が出ないのは構成の事。
- serial の行は全部読んだ。説明の要る行：「nvme0: namespace 1 is not ready yet」は NVMe 2.4 の Figure 346 の NSTAT（byte 14）の NRDY が 0 のときの知らせで、続く命令は通る。「cpu: LFENCE is not known to be dispatch serializing」は WHPX の CPUID 80000021h が 0 のため。
- 前の写し `build/mae.img` は段 C の試験の媒体（autorun と powertest を C: に入れたもの）で、起動するとすぐ眠る。H56 と H57 の「前」には使えない。
- 確かめていないこと：実機と Hyper-V（netvsc、storvsc、VMBus の stop は Hyper-V でしか通らない）。virtio_gpu、ata、AC'97、tulip、rtl8125、usbmsc、usbnet（この構成に無い）。聞こえる音。S3 からの戻り（WHPX）。
