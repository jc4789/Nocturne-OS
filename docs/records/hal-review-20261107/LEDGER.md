# HAL の見直しの台帳

目的：HAL を、汎用の OS にふさわしい形にする。新しい装置と新しい種類の装置を、実装（ドライバー）さえあれば HAL を変えずに扱えること。ドライバーを書くか移すとき、HAL を変えないこと。
この台帳は作業の正本。区切りごとに読み、所見・決定・状態を更新する。

## 0. 前提

- 読んだもの：`docs/hal.md`、`kernel/src/hal/*`、`common/hal_abi.h`、`common/hal_buses.h`、`kernel/src/dev/pci.c`（HAL の部分）、`pci.h`、`pciuser.c`、`acpi_devices.c`、`acpi.h`、`pm.c`、`iommu.h`、`arch/irq.h`、`usb.h`、`user/apps/services.c`、ABI の §9、`init.c`。カーネルの実装ごとの使い方は §2 の表。
- 照らした原文：ACPI 6.5（16.1.6、7.1〜7.4）、PCIe 6.0（5.8、5.9、7.5.2）。`D:\Programes\OS DEV INFO`。
- Gemini の資料（`D:\Programes\Grounding HAL Info By Gemini 4 Argon.txt`）は HAL の範囲の目安にだけ使う。事実は原文とコード。

## 1. 所見

番号は H。状態：未・直・外（範囲の外、`docs/open.md` へ）。

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
| H20 | detach の後に記録と thread を返さない実装（`nvme`、`ahci`、`storvsc`）。抜き差しの無いバスなので今は害が無い | §2 | 外 |
| H21 | レジスターを期限つきで待つ関数を実装ごとに写している（9 か所） | §2 | 外 |
| H22 | ACPI の接続の資源（`GpioInt`、`GpioIo`、`I2cSerialBus`、`SpiSerialBus`、`UartSerialBus`）を HAL が表さない。GPIO の割り込みを出す実装（割り込みの提供者）の口も無い。I2C の HID（ノートの内蔵のキーボードとタッチパッド）を作るときに、HAL へ資源の種類と提供者の口を足す要がある | `kernel/src/dev/acpi_devices.c:125` | 外 |
| H23 | 起こしの元（`_PRW`、`_DSW`、GPE、PCI の PME）を扱わない。起こせるのは電源のボタンだけ。設定の窓の項目と組で作るもの | — | 外 |
| H24 | 動作中の装置の電源の管理（使わない装置を D3 へ）が無い | — | 外 |
| H25 | 固まった装置をバスでリセットして実装を結び直す口（カーネルの実装の立て直し）が HAL に無い。PCI の FLR と SBR は claim の道（`pciuser.c`）だけが使う | `kernel/src/dev/pci.h:229`、`:264` | 外 |

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
| D9 | H20〜H25 はこの仕事の範囲の外。`docs/open.md` へ | 範囲。H22〜H24 は端から端まで（制御器の実装、設定の窓）通さないと、口だけで呼ぶ側が無い |

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

## 4. 段

| 段 | 中身 | 状態 |
|---|---|---|
| A | 機械の装置（D4）、木（D8）、H10、H11 | 済：宣言 `struct hal_machine`（`.hal_machine`）、firmware の節 `struct firmware_node`、ACPI の二度の出し方、QEMU で木を確かめた |
| B | firmware の口（D3）、claim の渡し方（D5）、台帳の見張り（D6） | 済：`dev/acpi_claim.c`（`SYS_HAL_ACPI_EVAL`・`_NOTIFY_TAKE`、`N_ACPI_PARENT`）、`SYS_SCREEN_HOLD`、`n_hal_claim` の実装の名、管理者が claim を取って `N_SERVICE_HANDLE_DEVICE` で渡す、台帳の見張りで実例を作り・消す、見張りの相手へ一覧の全部。確かめは下の 5 |
| C | 電源（D1、D2、H3、H12、H14） | 済：`hal/power.c` の walk、`dev/acpi_power.c`、PCI の `power`、`dev/pm.c` の機械の順、claim の段、実装ごとの suspend・resume（3.2）。確かめは下の 5 |
| D | DMA（D7）、H19 | 未 |
| E | 文書（`docs/hal.md`、ABI の §9、地図、`docs/open.md`）と全体の確かめ | 未 |

## 5. 確かめの記録

**段 B（コミット 6babecc2 の媒体、QEMU 窓付き、WHPX、q35、`intel-iommu,intremap=off,caching-mode=on`、`edu,dma_mask=0xffffffff`）**
- `nvrmtest`：0 failed。試験が edu を `nvrm` の名で claim し、`N_SERVICE_HANDLE_DEVICE` で nvrm に渡す。nvrm は動き、終わると claim が終わり、試験の名で取り直せる。
- `pcitest`（起動の画面を `bochs-display` にした回）：ACPI の口（`N_ACPI_SELF`・`N_ACPI_PARENT`・`N_ACPI_CHILD` の評価と断り）と `n_screen_hold` の場合に失敗は無い。5 件は画面に描かれた印の数の確かめで落ちる（refresh の分からない画面での合成の振る舞い。HAL の範囲の外、`docs/open.md` へ）。
- 前提の食い違い：`-device VGA` ではカーネルの `vbe` が VGA を持つので、pcitest の VGA の場合は claim できない（busy）。AMD の IOMMU は QEMU で x2APIC に要る形式（XTSup）を持たず使われない。WHPX は split の irqchip を持たないので、Intel の IOMMU は割り込みの再割り当てなしで使う。
- `C:\Tests` で tcc で組んだプログラムは公開の呼出ししか持たず、claim の能力を引けない。pcitest と nvrmtest はホストで組み（`build/tests/pcitest`、`build/tests/nvrmtest`）、試験の媒体の `C:\Nocturne\System` に置く。
- 確かめていないこと：管理者が台帳の見張りで装置ごとの実例を作り・消す道（QEMU に宣言に合う装置が無い）。

**段 C（コミット 88c1d6fc の媒体、xHCI の実装の前。QEMU 窓付き、WHPX、q35、`intel-iommu,intremap=off,caching-mode=on`、NVMe の起動、e1000、`intel-hda`＋`hda-duplex`、`edu` 二つ、試験の SSDT（作業場の `power_ssdt.py`）、`-device VGA`。xHCI は外した：まだ実装の電源の段が無く、眠りを拒む）**
- 取りやめの walk（`powertest`、作業場）：0 failed。NOCT0001 と edu 1 は SUSPEND に止まったと答え、edu 2 は止まらないと答える。同じ段の全部（NVMe、AHCI、e1000、HDA、vbe、三つの claim）が SLEEP で止まり、取りやめで戻る。NOCT0001 の記録は "3abBA0"：`_PS3`、資源を順の大きい方から `_OFF`（共有の PRC0 は残る）、小さい方から `_ON`、`_PS0`。edu 1 は D3hot から D0 と構成の戻しの後に答える。止まらないと答えた edu 2 も RESUME を受ける。戻った後、AHCI は port を見直し、NVMe の C: は書ける（電源断の段まで失敗の行が無い）。
- S3 の入り（`powertest sleep`）：QMP の `query-status` が `suspended`。walk の全部の装置が止まり D3 になり、`_PTS`、CPU の停止、入りまで通った。WHPX は S3 から戻れない：戻りは確かめていない。
- 休止の全部の輪（`powertest hibernate`、作業場の qcow2 の overlay）：一度目は IMAGE の walk、写し（30 MiB を 152 ms）、戻る walk（AHCI の見直し）、OFF の walk、電源断。二度目の起動は写しへ戻り「awake after 30 s」「the image is removed」、続く電源断も通る。
- 見つけて直したこと：止まらないと答えた装置に resume が来なかった（`hal/power.c`、3811a86a）。眠る前から使えない装置が眠りのたびに「戻らなかった」と言う形（7f414941）。止まらない suspend が自分で戻す二つ目の道（88c1d6fc）。
- xHCI の取り込みの後（コミット 0aadf56b の媒体、`qemu-xhci`＋`usb-kbd`＋`usb-tablet` を足した）：取りやめの walk は 0 failed で、xHCI と USB の実装の失敗は無い。S3 の入りは QMP の `suspended`。休止の全部の輪では、写しから戻ると制御器が状態を失ったと分かり（起動のカーネルの電源断が HCRST した）、装置を外して立て直し、port 5 と 6 の装置を数え直す。普通の起動で、タブレットで欄を押しキーボードで打てる。
- 試験の側の注意：`n_power_request` の眠りと休止はすぐ戻る（pm の thread が後で行う）。試験の終わりの電源断の頼みが先に来ると、そちらが行われる。
- 確かめていないこと：実機と Hyper-V。tulip、ata、rtl8125、ac97、usbmsc、usbnet の walk（QEMU のこの構成に無い）。S3 からの戻り（WHPX）。
