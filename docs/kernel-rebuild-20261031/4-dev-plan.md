# 波 4 の装置の層：設計の下書きの依頼と報告（2026-10-31）

Plan の型（Opus、読むだけ）のサブエージェントに、波 4 の装置の層の設計の下書きを頼んだ。報告は届いたら下の「報告」に全文を写す。本体がそれを照らして 4-design.md を書く。

## 依頼

あなたは Nocturne OS のカーネルの装置の層（`kernel/src/dev/`）を建て直す設計の下書きを書く。読むだけで、ファイルは変えない。報告がそのまま設計の材料になる。

### 背景
- Nocturne は独自の OS（UNIX でも POSIX でも Linux でもない）。対象は実機：Ryzen 5 7600X（Zen 4、AM5）と RTX 4070。下限は Zen 4、上は 2 ソケットの EPYC（論理 CPU 1024、NUMA、CXL）。QEMU と Hyper-V は試験の道具で、VM だけの処理は `kernel/src/vm/` の後ろ（`vm/vm.h` が唯一の入口）に置き、共通コードに入れない。Hyper-V 第 2 世代と拡張セッションは要件。
- 2026-10-30 の監査で、カーネルを持ち主の単位で消して建て直すと決めた。波 1（CPU の土台・割り込みの層・時間・記録）と波 2（process と thread の分離、横取りできる核と EEVDF・実時間の級、待ち行列と mutex の手渡し、優先度の継承）は main に入った。波 2 の object・handle・events・channel（2b-O、epoch の錠の無い handle の表）と波 3 の頁の確保（3-P）は書き手が走っている。波 3 の空間（3-V）・syscall の表（3-S）・時間の ABI（3-T）は設計だけある。
- 波 4 は装置・格納庫・網・窓。あなたの範囲は装置の層（dev/）だけ。格納庫（fs・common/store）・網（net/）・窓（gui/）は別の回。ただし装置の層の口がそれらとどう継がるか（block の層と格納庫、NIC と網、表示と窓、HID と窓、音と窓の音の口）は書く。

### 読むもの
- 決まり：`AGENTS.md`、`.claude/rules/kernel.md`、`.claude/rules/map.md`、`docs/kernel-rebuild-20261031/00-design.md`（§3 全員の決まり、§5 波 2 以降の要件の「装置」の行）。
- 監査：`docs/kernel-audit-20261030/06-devices.md`（dev/ の監査の全文）、`03-cross-cutting.md`（横断）、`05-time-and-execution.md` の装置に触れる所、`07-timing-check.md` の装置に触れる所。
- 先の波の口：`kernel/src/arch/irq.h`（割り込みの層）、`kernel/src/sys/alarm.h`、`kernel/src/sys/wait.h`、`kernel/src/sys/clock.h`、`kernel/src/sys/sched.h`（`kthread_create_on` など）、`docs/kernel-rebuild-20261031/3-design.md`（mm の口、時間の ABI の事象の時刻 §4.5）。
- 今のコード：`kernel/src/dev/` の全部、`kernel/src/vm/`（vm の装置が装置の層の口に乗る形を見るため）、`kernel/src/dev/pciuser.c` と `user/apps/init.c`（アプリのドライバーの道。NVIDIA の表示はアプリ `nvrm` が持つ）。
- 仕様：`D:\Programes\OS DEV INFO` の PCIe・NVMe（2.4 と 1.4c）・xHCI・USB 2.0／3.2・HID 1.11 と HUT・HDA・AHCI・ACPI 6.5・AMD の IOMMU（48882）・PPR 56713（Zen 4）。記憶でなくここで照らす。`rtl_bsd_drv_v1102.01/` は BSD の driver のソースなので読まない。Linux・BSD・NT ほか OS のソースは読まない（公開された設計の説明は可）。

### 書いてほしいこと
1. 今の dev/ の持ち主ごとの判定：消して建て直す／形を保って中を直す／そのまま。理由は監査とコードの行で。
2. 装置の層の全体の設計：装置の木と driver の照合（PCI・ACPI・USB の装置が一つの木に乗るか）、driver の寿命（照合・始め・眠り・起き・外れ）、割り込み（MSI-X を CPU ごとのキューへ、波 1 の `irq_request_msi` の上で）、DMA と IOMMU、電源（装置の木の順に並列で眠る文脈）。
3. 各部品の設計：block の層（非同期の要求、CPU ごとの送り口）、NVMe（CPU ごとのキュー対、写しなしの PRP・SGL）、AHCI（残すか消すか、理由）、xHCI（非同期の転送、複数の interrupter、isochronous）、USB の hub・HID（usage のまま時刻つきで窓へ、鍵の繰り返しは窓の側）・mass storage・網、音（HDA の driver、割り込みの外の混ぜ手、時刻つきの位置）、表示（connector・CRTC・plane の模型と vblank の時刻、起動の画面 fb・fbcon、アプリのドライバーへの渡し）、ACPI（uACPI の上で）、乱数（CPU ごとの生成器）、serial、refclock、休止（hibernate）。
4. 各部品で：責務と持ち主、数が十倍になった時に詰まる所、錠と眠れる文脈、時間の扱い（期限・時刻の源）、vm/ の装置との境。
5. Linux と NT の公開された設計と軸ごとに並べた表（劣る軸が残るならそう書く）。
6. 書き手への割り方：並べて走らせられる段（ファイルが重ならない）、順に走らせる段、各段の持つファイルと触ってよい所、各段の確かめ（QEMU の q35・UEFI・NVMe・xHCI・e1000 の機械で何を見るか、実機でしか見えない物）。
7. 仕様で確かめられなかったことは「未確認」と書く。

### 守ること
- 決め打ちの上限、間隔の見回り、回数の spin、錠の下の眠り、shim（何もしない関数・渡すだけの包み）を案に入れない。
- 古い装置（PS/2・PIT・8259・xAPIC・BIOS 起動）のための道を作らない。
- 報告は日本語。コードの名・パス・仕様の節の番号は元の表記。

## 報告

（Plan の型の書き手の最後のメッセージ、全文）

# 波 4 装置の層（kernel/src/dev/）の設計の下書き

**読んだもの**
- 決まり：`AGENTS.md`、`.claude/rules/kernel.md`、`.claude/rules/map.md`、`00-design.md`（§3、§4、§5）。
- 監査：06-devices.md の全文、03・05・07 の装置に触れる所。
- 先の波の口：`arch/irq.h`、`sys/alarm.h`・`wait.h`・`clock.h`・`sched.h`・`thread.h`、3-design.md、2-design.md、2b-O.md の冒頭。
- コード：dev/ の全部と vm/vm.h・vm.c・devices.h。vm/ の他のファイルは、dev の口を呼ぶ所だけ grep で読んだ。`user/apps/init.c` の driver の起動の部分。
- 仕様：`D:\Programes\OS DEV INFO` を pdftotext と grep で照らした。節の番号は各所に書く。
- 読んでいないもの：Linux・BSD・NT のソースと `rtl_bsd_drv_v1102.01/`。

**前置き：監査のうち、今のコードでは古くなった記述**
- 装置の割り込みは、もう CPU 0 に縛られていない。
  - `pci.c:464-500` は `irq_request_msi(&specs[n], IRQ_CPU_ANY)`。
  - SCI は `acpi.c:340-379` で `irq_request_gsi`。
  - `pciuser.c:1068-1094` は `.node=-1`。
- PS/2 は消えた。
- serial は 1-D（SPCR、THRE の割り込み）、refclock は 1-B2 が建て直した。fbcon は 1-D で記録の読み手になった。
- QEMU の試験の機械は NVMe から起動する（`scripts/test.py:525-557`）。AHCI は起動の道ではない。

---

## 1. 今の dev/ の持ち主ごとの判定

| 持ち主（ファイル） | 判定 | 理由（監査とコードの行） |
|---|---|---|
| PCI の核 `dev/pci.{h,c}` | 消して建て直す | **形の欠陥**<br>- segment 0 だけ（pci.c:16-20・31、pci.h:46-47）。<br>- CF8/CFC を全体の `cf8_lock` で（45-87）。<br>- 固定の `ecam_active[65536/8]`（26）。<br>- 256×32×8 の総当たりで、各点で `ready_id`（207-249）。上流との結びは O(n²)（251-254）。<br>- `pci_find`・`pci_find_class`・`pci_find_function` が線形（257-299）。<br>- driver の照合が無く、各 driver が `pci_at` を class で回る（nvme.c:440-450、xhci.c:1559-1567）。<br>- `_OSC` が無い。<br>**待ちと錠の欠陥**<br>- `wait_step` は 1 ms の眠りか pause（160-164）。`ready_id` は見回り（174-183）。<br>- `COMPLETION_FLOOR_MS 1000`（759）。<br>- 全体の `reset_mutex`（821）。<br>- `links_retrain` は 1 s の見回り（870-888）。<br>- `reset_lock` の spinlock の下で線形の `pci_find_function`（1050-1068）。<br>- `pci_resume` は Root Port を `pause()` で回り（1396-1403）、関数ごとに順に `ready_id(..., sleeps=false)`（1404-1433）。<br>**新しい核へ写す知識**<br>- 設定の保存と戻し（ReBAR・ACS を含む：573-668）。<br>- FLR・RRS・SBR の手順の順序と仕様の時間（670-1310）。<br>コードではなく、手順として写す。 |
| アプリのドライバーの道 `dev/pciuser.{h,c}` | 消して建て直す。claim は装置の木の結びに（`dev/claim.*`）。画面の部分は表示の核に。 | **欠陥**<br>- CLAIM_VECTORS・CLAIM_GUARDS・CLAIM_ACPI_NOTES が 32 の決め打ち（41-43）。<br>- 全体の `claim_mutex`（90）。<br>- 画面の保持・fbhold の見張りの thread・give_screen・scanout・mode・image が PCI のファイルの中にある（146-840）。<br>- `pci_user_screen_vblank` が driver の時計の時刻を `wm_vblank` へ渡す（759-765）。<br>- claim は bus・dev・func で、segment が無い（636-663、user/apps/init.c:135-136 の `n_pci_claim(bus, dev, func)`）。<br>- Notify の輪が上書きする（903-913）。<br>**残す形**<br>- 短い `claim_isr` から `object_notify`（101-112）。<br>- ACPI の method を拒む表（859-867）。<br>- 設定の空間の読みを自分・上流・host bridge に限ること（966-1001）。 |
| ACPI の表（`dev/acpi.c:1-160` と `acpi.h` の表の形） | そのまま | 検査和、firmware の範囲、XSDT→RSDT、有効の bitmap、`acpi_find`、`acpi_entry_next`。監査も残すと判定した。 |
| ACPI の AML の継ぎ（`dev/acpi.c` の 161 行以降） | 消して建て直す（`dev/acpi_aml.c` へ分ける） | - `uacpi_kernel_unmap` が何もしない（183：shim）。<br>- `pci_device_open` が segment 0 だけ（191-192）。<br>- `uacpi_kernel_sleep` が凍った時に spin（260-264）。<br>- semaphore の待ちが凍った時に sti/pause/cli の spin。FROZEN_WAIT 2 s の後は `sched_yield`（266-318）。<br>- `sci_wait_idle` が pause の spin（358-360）。<br>- 一つの "acpi" の thread。注釈の「CPU 0 に置けない」は古い（388-452）。<br>- PNP0C0F の `LINKS_MAX 32`（517-543）。<br>- PCI の節を _BBN/_ADR の線形で探し、`PCI_DEPTH_MAX 32`（595-651）。<br>- _OSC・EC・熱・_PRW・名前空間の装置の列挙が無い。 |
| `dev/power.c` | 消す。S5 と reset は uACPI の口へ。 | - io_wait を回数で回す（78・83・87・96・99・101・103）。<br>- 8042 による reset（101-102）。<br>- DSDT のバイトを探す（45-48）。<br>- triple fault（104）。<br>- `vm_power_off`（89）。 |
| IOMMU `dev/iommu.{h,c}`・`dev/iommu_amd.c` | 消して建て直す | **素通し**<br>- 核の装置・IOAPIC・HPET は DTE が 0 で素通し（iommu_amd.c:1-13）。<br>- domain は claim の関数にだけ付く（iommu.h:3-16）。<br>**iommu_amd.c の待ちと錠**<br>- 全体の `amd_lock`（160）。`RING_BYTES 4096`（85）。<br>- `register_wait` は 10 ms の pause の spin（394-398）。<br>- `command` は錠の下で割り込みを閉じ、100 ms の pause の spin（455-476）。<br>- `send` は全ユニットへ送る（478-480）。`domain_flush` は domain の全部を無効にする（483-486）。<br>- `event_isr` は `amd_lock` の下で読む（534-540）。<br>- `sync` はユニットごとに 100 ms の spin（544-572）。<br>**iommu.c**<br>- 全体の `domains_lock`（38）と `attach_mutex`（41）。<br>- 故障の報告を毎秒 8 に間引く（177-190）。<br>- ACS の群を `pci_count()` の輪で計算（542-628）。<br>- `domain_id_new` が線形（752）。 |
| `dev/iommu_intel.c` | 消す（建て直さない） | 下限は Zen 4、上は EPYC（依頼の背景）。「Intel には手間をかけない」というユーザーの言葉もある（盤）。`struct iommu_hw` の口は AMD の実装一つで残す。**本体の確認が要る。** |
| block の層 `dev/blk.{h,c}` | 消して建て直す | - read・write・persist が同期の関数の指（blk.h）。<br>- `MAX_BLK 16`（blk.c:9）。<br>- 要求ごとに全体の `gate_lock` と inflight（37-57）。<br>- `blk_by_blocks` の写し（84-111）。<br>- blk.h:1 の注釈が "Hyper-V SCSI" の名を出す。 |
| `dev/nvme.c` | 消して建て直す | - MAX_CTRL 4・MAX_NS 8・ADMIN_ENTRIES 32・IO_ENTRIES 64・XFER_PAGES 32・ADMIN 5 s・IO 30 s（18-24）。<br>- 制御器ごとに mutex 一つで、一度に一つの命令（76）。<br>- 保険の見回り `wq_wait_unless(..., clock_after(c->irq ? 10*NS_PER_MS : NS_PER_MS))`（165-166、監査 07 の 01-381）。<br>- `wait_ready` が 1 ms の見回り（170-180）。<br>- I/O のキューが一つ（208-210）。PRP の写し（240-251）。<br>- 期限が切れると `enable` から全部やり直す（258-276）。<br>- ready_ns は 2 s 以上（366）。irq の node は -1（374）。<br>- `nvme_shutdown` が RTD3E を 1〜10 s に挟み、pause の spin（410-425）。 |
| `dev/ahci.c` | 消す | - MAX_HBA 4、slot 0、128 KiB の写し、SPINUP 50 ms・LINK 1 s・READY 10 s・STOP 500 ms・IO 30 s（18-25）。<br>- 1 ms の眠りか pause の見回り（139-140・277）。BOHC で 2 s（400-402）。<br>- 起動の道は NVMe で、disk は一つ（AGENTS.md）。<br>- 7600X の PPR（56713 Table 13）には SATA の制御器が無い。<br>- 実機の C: が SATA なら §3.4 の形で建てる（未確認）。 |
| USB の核と xHCI `dev/usb.h`・`dev/xhci.c` | 消して建て直す | **形の欠陥**<br>- 「すべて一つの usb の thread で」（usb.h:3-4）。<br>- `usb_bulk` は usb の thread だけで 64 KiB まで（usb.h:82、xhci.c:827）。stream は 16 まで（usb.h:79）。<br>- MAX_HC 8・RING_TRBS 256・EV_TRBS 1024・EP_XFERS 16・SYNC_BYTES 0x10000（17-20・92-93）。<br>- 全体の `usb_lock`（188）。isochronous が無い（716）。<br>- `usb_init` が線形（1559-1571）。<br>**見回り**<br>- `poll_all`（451-453）。<br>- `hc_wait` は 1/10 ms（461-478）。<br>- `hc_command` は 3 s＋5 s を 1 ms の見回りで（503-523）。<br>- `usb_thread` は割り込みが無いと見回りへ落ちる。次の起きは MIN(usbhid_tick, 50 ms, 4 ms, 1 s)（1451-1515）。<br>- `xhci_quiesce` は 20 ms の pause の spin（1519-1532）。<br>**眠りと起き**<br>- 起きで全部を数え直す（1537-1549）。<br>- USB の disk があると眠りを断る（1551-1554）。 |
| `dev/usbhub.c` | 消して建て直す | - usb の thread の上で動く。<br>- `HUB_PORTS 15`（11）は route string の 4 bit という仕様の限りで、決め打ちではない（残す）。 |
| `dev/usbhid.c` | 消して建て直す | - scancode set 1 へ訳す（2-3・56-75）。<br>- 繰り返し 500/33 ms を核で行う（15-16）。<br>- 全体の caps_lock と leds_dirty（54）。<br>- `e.buttons = buttons & 7`（196）。<br>- `usages[16]` と `nkeys < 16`（244・275）。 |
| `dev/usbmsc.c` | 消して建て直す | - BOT だけ（48）。<br>- XFER 0x10000・`MAX_LUNS 8`（14-15）。Get Max LUN は 0〜15 を返しうる。<br>- TEST UNIT READY を 40×250 ms（122-134）。<br>- `usb_run`（210-238）。 |
| `dev/usbnet.c` | 消す。建て直しは後の段（任意）。 | - 全体で一つの `adapter`（44-45）。<br>- `TX_SLOTS 16`（25）。<br>- usb の thread と mutex。 |
| 入力 `dev/input.{h,c}` | 消して建て直す（usage の router へ） | - 内の符号が scancode set 1 で、US の表を持つ（20-35）。<br>- 一つの `input_lock` の下で、割り込みを閉じたまま sink を呼ぶ（8-17・37-41）。<br>- 全体の mods・caps・stream_e0（19-32）。<br>- 時刻が無い。ボタンは 3 つ（29）。<br>- `input_unicode` は 0x80 以上を KEY_UNKNOWN にする（137）。 |
| 音 `dev/audio.{h,c}` | 消して建て直す | **固定の数**<br>- `AUDIO_RATE 48000`・`AUDIO_FRAME 4`（audio.h:21-22）。<br>- MAX_STREAMS 8、QUEUE_BYTES 8192 frames、RUNS 32（"an AC'97 card's whole ring"）、NULL_BLOCK 10 ms（audio.c:18-22）。<br>**錠と割り込みの中の混ぜ**<br>- card_lock→audio_lock の spinlock（52-58）。<br>- `mix_locked` が audio_lock の下で混ぜる（185-233）。<br>- card の `fill()` は割り込みを閉じて card_lock の下（audio.h:67-68）。<br>- null の sink が alarm の fire の中で混ぜる（311-349）。<br>**VM の漏れと跡**<br>- remote の sink の特別な口（audio.h:87-110）。<br>- AC'97 の跡（audio.h:128-135、audio.c:89-175）。<br>- `vm/vm.h` を include する（16）。<br>**その他**<br>- `stream_close` が最大 1 s 待つ（640-659）。<br>- 状態に時刻が無い（737-747）。<br>- 実機の driver が無い（770 `vm_audio_init(); /* no driver for real sound hardware yet */`）。 |
| 起動の画面の書き手 `dev/fb.{h,c}` | 消して建て直す（表示の核の「起動の提供者」へ） | **欠陥**<br>- `static struct scanout scanouts[3]`（fb.c:38）。<br>- `presents_drain` が 100 µs ごとに見る（205-207 `DRAIN_POLL_NS`）。<br>- vblank も時刻も無く、拍は EDID から推す（48-71）。<br>- 口の形が pciuser に合わせてある（fb.h:35-68）。<br>- `vm_display_pm`（427）。<br>**残す形**<br>- LUT の変換（41-46・119-180）。<br>- panic を「示されうる面」すべてへ描く（`alsos`、310-361）。<br>- hold の意味：時間で hold を終えない（182-234）。 |
| `dev/fbcon.{h,c}` | 形を保って中を直す | 1-D の記録の読み手。描く先を `fb_present`・`fb_present_last`（fbcon.c:62・389）から、表示の核の「起動の面」と「panic の面」の口へ替える。 |
| `dev/gpu.h` | 消す | - 実装は vm/virtio_gpu.c だけ（virtio_gpu.c:6）。<br>- gui/wm.c:2761 が `gpu_init()` を直に呼ぶ。vm/vm.h を通らない VM の漏れ。<br>- syscall の `gpu_render*`（sys/syscall.c:2948-2996）も消す。 |
| 電源 `dev/pm.{h,c}` | 消して建て直す | **欠陥**<br>- quiesce は、一つの CPU で割り込みを閉じて見回るだけの約束（pm.h:25-37）。<br>- 木ではなく IO・STORAGE・BUS の三段（pm.h:41-46）。<br>- 起きは CPU 0 で、他の CPU を止めたまま。眠れない（pm.h:32-34、pm.c:193-250）。<br>- 窓が閉じるのを 50 ms ごとに見る（pm.c:104）。`stop_apps` は 20 ms ごと（118）。<br>- GRACE/STALL/FORCE/STOP の決め打ち（33-36）。<br>- app が PCI を持つと眠りを断る（256-257、`pci_count` の線形）。<br>**残す形**<br>- 錠の短い一つの電源の thread。<br>- `pm_request` はどの文脈からでも呼べ、眠らない（66-78）。<br>- `kthread_create_on("power", …, 0)`（352）。 |
| 休止 `dev/hibernate.{h,c}` | 消して建て直す | - `RESERVE 16384` frames、`BLOCK_PAGES 16`、`STAGE_BYTES` の決め打ち（40-42）。<br>- FNV の hash（43）は改ざんを見ない。<br>- 暗号化が無い（hibernate.h:20-21）。<br>- 8259 の注釈（691）。<br>- blk が同期。 |
| 乱数 `dev/entropy.{h,c}` | 消して建て直す（`dev/random.*` へ） | - 全体の `pool_lock`（14）の下で全 CPU の feed を畳む。読みが O(CPU)（85-97）。<br>- `rdrand64` の 10 回の再試行（49-56）は回数の spin。<br>- pool の混ぜ（30-38）は暗号の hash ではない。<br>- `entropy_init` の空の輪（113-118、監査 07）。<br>- RDSEED を使わない。Zen 4 は RDRAND・RDSEED とも Fixed,1（PPR 56713：CPUID Fn0000_0001_ECX[30]、Fn0000_0007_EBX[18]）。 |
| `dev/serial.{h,c}` | そのまま | 1-D の作。SPCR が 16550 を言えばその port、無ければ COM1。THRE の割り込みで送る。 |
| `dev/refclock.{h,c}` | そのまま | 1-B2 の作。<br>- HPET を速さの基準とし、自分の alarm で読む（124）。<br>- CMOS RTC は FADT の IAPC_BOOT_ARCH bit 5 で有無を見る（ACPI 6.5 §5.2.9.3）。 |

**vm/ の側（範囲の外だが、境のために判定する）**
- 消す：
  - `vm/ata.c`（IDE。Hyper-V 第 1 世代と QEMU の "pc" 用）。
  - `vm/tulip.c`（第 1 世代の旧式の網）。
  - 理由：媒体は UEFI・q35・第 2 世代だけになった（盤）。
- `vm/ac97.c`：試験の機械を HDA に替えたら消す（§6）。残すなら、音の装置の口に乗せる。
- 新しい口に乗せ替える（§2.7）：`vm/e1000.c`、`vm/virtio_gpu.c`（2D だけ）、`vm/vbe.c`、`vm/hv/storvsc.c`、`vm/hv/netvsc.c`、`vm/hv/hv_input.c`、`vm/rdp.c`。

---

## 2. 装置の層の全体の設計

### 2.1 装置の木と driver の照合

**一つの木に乗る**
- PCI・ACPI・USB の装置は一つの木に乗る。
- 根の下：
  - ACPI の名前空間の装置（`_HID`/`_CID` を持ち、`_STA` で居るもの）。例：PNP0C0C 電源の button、PNP0C09 EC、PNP0103 HPET、PNP0B00 RTC、PNP0501 serial、熱の zone。
  - ACPI の host bridge（PNP0A08/PNP0A03。`_SEG`・`_BBN`・`_CRS` が bus の範囲を言う）。その下に PCI の階層。
- xHCI の関数の下：root hub の port、USB の装置、USB の interface。
- VMBus：vm/ が置く bus の節（VM の中でだけ）。
- PCI の関数の ACPI の相棒（companion）：親の bridge と `_ADR` で結び、節の欄に置く。電源（`_PRW`・`_S0W`・`_PRx`・`_SxD`）と Notify は相棒から引く。

**節 `struct device`（新しい `dev/device.h`）**
- 親と子の鎖、bus の型、bus ごとの識別。
- NUMA の node：`_PXM`（ACPI 6.5 §6.2.14）。無ければ親から。
- 資源：BAR と MMIO、割り込みの能力（MSI-X の項の数、MSI、INTx）、DMA の幅、IOMMU の群。
- 状態（§2.2）、結ばれた driver か app の claim、依存（`_DEP` と、driver が言う他の節）。
- 節は 2b-O の object：handle で持て、外れは epoch で見える。

**bus の型 `struct bus_type`**：照合の関数と列挙を持つ。
- PCI：vendor・device・subsystem・class の mask。
- USB：interface の class・subclass・protocol、または vid/pid。
- ACPI：`_HID`/`_CID` の文字列。
- VMBus：GUID。vm/ の中だけ。

**driver `struct driver`**
- bus と照合の表。
- `probe`：照合の直後。短く、眠ってよい。
- `start`：資源を取り、装置を動かす。非同期。
- `suspend`・`resume`：眠れる文脈。
- `shutdown`。
- `remove`：驚きの外れを含む。

**照合は出来事で行う**
- 節が足された時と、driver が登録された時に、その bus の照合を一度だけ行う。
- 表の最も細かい一致が勝つ（ID の完全一致 > subsystem > class）。
- driver が木を回って装置を探すことは無い。今の `pci_find`・`pci_at` を回る形は消える。

**引き**（線形の探しは無い）
- (segment, bus, devfn)：segment ごとの bus の表。bus 番号は仕様で 8 bit。
- ACPI の道。
- handle の id：2b-O の idtable。

**アプリへ**
- 木の一覧と、出来事（足された・結ばれた・外れた）を 2b-O の events で出す。
- init は一覧と出来事から GPU の関数を選び、節の handle で claim する。segment も含まれる。今の `n_pci_claim(bus, dev, func)` を替える。

### 2.2 driver の寿命（照合・始め・眠り・起き・外れ）

**状態**
- ADDED → BOUND（probe 済み）→ STARTING → STARTED。
- STARTED ⇄ SUSPENDING → SUSPENDED → RESUMING → STARTED。
- どこからでも REMOVING → REMOVED。

**始め**
- 非同期。probe の後、start を「装置の仕事の池」で走らせる。
  - 池は NUMA の node ごとに、その node の CPU と同じ数だけ、縛った kthread を置く（`kthread_create_on`）。数は topology から決め、決め打ちしない。
- 兄弟は並んで始まる。子の照合は、親が STARTED になってから。

**列挙の静まり**
- STARTING の節が 0 になった、という出来事。
- 格納庫は C: を、「静まり」と「blk の到着」の出来事で待つ。待ちの期限は決め打ちしない。

**外れ**
- 子から先に外す。
- driver の `remove` の手順：
  1. 新しい要求を断る。
  2. 行きの要求を `-N_ERR_GONE` で終える。
  3. 割り込みと DMA を止める。
  4. 資源を返す。
- handle を持つ者は epoch で外れを見る。
- 同じ道を通るもの：USB の抜き、claim の終わり、後の段の PCIe hot-plug と DPC。

**眠りと起き**：§2.5。

**錠**
- 木の形は rwmutex で守る。足し・外しは書き、歩きは読み。
- driver の呼び戻しは木の錠を持たずに呼ぶ（節の参照を取ってから呼ぶ）。
- 装置の完了を待つ間は、どの錠も持たない。

### 2.3 割り込み（MSI-X を CPU ごとのキューへ）

**MSI-X の項の取り方**
- 波 1 の `irq_request_msi(spec, cpu)` の上に、PCI の核が `pci_vectors(pdev, wanted, specs, cpu_of)` を置く。
- MSI-X の Table Size と driver の欲しい数の小さい方だけ項を取る。
- 項 i は `irq_request_msi(&specs[i], cpu_of(i))` でその CPU に向け、`irq_msi_message` で項を書く。
- `irq_set_cpu` で CPU が移ったら、`irq_msi_writer` で項を書き直す。今の `message_rewrite`（pci.c:435-449）の考えを残す。

**キューと CPU**
- キューの数は、装置の能力と CPU の数の小さい方で決める。能力とは：NVMe の Number of Queues の答え、xHCI の MaxIntrs、MSI-X の項の数。
- CPU は topology の群（LLC → node）ごとにキューへ割る。各キューの割り込みは、その群の CPU へ向ける。`irq_spec.node` は装置の node。
- CPU がキューより多い時は、群の CPU が一つのキューを分け合う。キューの spinlock（波 1 の MCS）を持つのは、送りの尾を書く間だけ。

**MSI-X の無い装置**
- MSI だけの装置：`irq_request_msi_block`。MSI の block は一つの番地なので、一つの CPU に向く。
- MSI も無い装置（QEMU の e1000＝82540EM、AC'97）：`_PRT`（今の `acpi_pci_gsi`）と `irq_request_gsi`。8259 の道は無い。

**handler**
- hard handler は短い。完了の行列を読み、要求の完了の呼び戻しを呼ぶ。眠らない。
- 長い仕事は `IRQ_WAKE_THREAD` で、縛った irq の thread（`SCHED_PRIORITY_IRQ`）へ。

**割り込みの時刻**
- hard handler の入りで `clock_now_ns()` を一度読み、そのキューの完了に付ける。HID の報告、網の受け、音の位置の組がこれを使う。

**集め**
- 使う機構：NVMe の Interrupt Coalescing（FID 08h：NVMe 2.4 §5.2.30.2.1、Figure 543）、xHCI の IMOD（§4.17.2）。
- 低い遅れの要る道（HID、音）は、集めないキューか interrupter に置く。

**CPU が外れる時**
- 眠りの時は、その CPU の vector を `irq_set_cpu` で群の他の CPU へ移す。

### 2.4 DMA と IOMMU

**持ち主**
- DMA の型・IOMMU の domain・装置の pin は mm（3-V の契約）。
- dev/iommu は、mm の domain を hardware で実行する：頁表の更新、無効化、割り込みの再写像（IR）、故障。

**すべての bus master に domain を付ける**
- 核の装置にも付け、素通しは無い。
- 核の装置の domain は「遅い無効化」：unmap した IOVA は、集めた無効化と、それに続く COMPLETION_WAIT（48882 §2.4.1）が終わるまで使い直さない。
- app の claim の domain は「厳しい無効化」：unmap は無効化の完了を待ってから返る。

**IOVA**
- domain ごとの範囲の木と、CPU ごとの小さな溜め。溜めの長さは CPU の数と要求の大きさの分布から決める。

**無効化の範囲**
- `INVALIDATE_IOMMU_PAGES`（§2.4.3）で範囲だけを無効にする。
- 送り先は、その domain の装置を訳すユニットだけ（domain ごとにユニットの集合を持つ）。
- `INVALIDATE_IOMMU_ALL`（§2.4.8）は起きの時だけ。

**完了の待ち（spin は無い）**
- COMPLETION_WAIT に store（s）と割り込み（i）を立てる。割り込みは `ComWaitIntEn`（MMIO 0018h bit 4）で許し、状態は MMIO 2020h の `ComWaitInt`。
- 待つ者はユニットの wait queue で、store された列の番号が自分の番号に届くまで眠る。

**命令の輪**
- ユニットごとに輪を持つ。ユニットの spinlock を持つのは、尾を書く間だけ。
- 輪の長さは、ユニットの能力（ComLen）と CPU の数から決める。今の 4096 の決め打ちを消す。

**IR**
- APIC ID が 255 を超える機械（2 ソケットの EPYC）で要る（`iommu_irq_wide`）。
- 7600X（12 論理 CPU）では要らない。

**故障**（event log：§2.5 の各 event）
- event log の割り込みで、装置ごとに数え、持ち主へ知らせる。核の driver には呼び戻し、app の claim には events。
- 毎秒 8 の間引きは消す。記録には、装置ごとの「初めの一つ」と「数」を出す。

**群（ACS）**
- 木の上で、上流の ACS の能力から作る。群の中の関数は一つの domain を分け合う。

**IOMMU の無い機械**（今の QEMU の試験の機械）
- DMA の幅だけを守る。32 bit の装置は mm の帯（`pmm_alloc_contig(count, bits)`）から取る。

### 2.5 電源（装置の木の順に並べて、眠れる文脈で）

一つの電源の thread（CPU 0 に縛る）が段の順を持つ。装置の段は、木の歩きを装置の仕事の池へ流す。

**眠り**：葉から進む。
- 各節は子の残りの数を持ち、0 になると池へ入る。兄弟は並ぶ。
- driver の `suspend` は眠れる：
  1. 新しい要求を門で止める。
  2. 行きの要求の完了を割り込みで待つ（wait queue）。
  3. 装置へ眠りを告げる。NVMe は shutdown、xHCI は Save State（§4.23.2）。
  4. 終わったら親の数を減らす。
- PCI の核は driver の後で設定を保存し、D3hot へ入れる。
- 依存も数に足す。例：`_DEP`。IOMMU のユニットは、それが訳すすべての装置の後に眠る。

**起き**
1. arch の段で CPU・割り込み・alarm を戻す。
2. 他の CPU を起こす。今は CPU 0 で他を止めたまま装置を起こしている（pm.c:193-250）。これを替える。
3. 根から並べて `resume` を呼ぶ。眠れる文脈で、割り込みも使える。

**終わり（電源断・再起動）**
- 同じ木の順で `shutdown`（書き戻し、DMA の停止）を呼ぶ。
- 割り込みを閉じて回る quiesce は無い。

**時間**
- driver の中の待ちは §2.6 に従う。
- 各節の suspend には、driver が言う最悪の時間（NVMe の RTD3E など）から期限を付ける。越えたら記録し、眠りを取りやめる。電源断なら進める。

**app のドライバー**
- claim に、眠りと起きの要求を channel（2b-O）で送り、期限つきで答えを待つ。
- 今は claim があると眠りを断っている（pm.c:256-257）。これを替える。

**窓と app**
- EV_CLOSE の後、窓の数の変わりと process の終わりを wait queue で待つ。今の 50 ms と 20 ms の見回り（pm.c:104・118）を消す。
- 猶予の時間（GRACE など）は設定の値にする。

### 2.6 通知の無い完了の待ち（全部品の決まり）

**1. 通知がある所は通知で待つ**
- PCIe：
  - DRS（PCIe 6.0 §6.22.1。Link Control 15:14 の DRS Signaling Control で割り込みを選ぶ）。
  - FRS（§6.22.2。Root Port の FRS Queuing Extended Capability、§7.8.10）。
  - retrain の終わりは Link Bandwidth Management Interrupt（Link Control bit 10）。
  - hot-plug の Data Link Layer State Changed。
- xHCI：Port Status Change Event（PRC）と Command Completion Event。
- NVMe：完了の行列。
- HDA：RIRB の割り込みと STATESTS による起こし。
- AMD IOMMU：`ComWaitInt`。

**2. 仕様が最小と最大を言う所は二点で読む**
- 最小の時に一度（alarm 一つ）読む。まだなら、最大の時にもう一度読む。それでも駄目なら故障とする。輪は回さない。
- 例：
  - PCIe の reset の後は 100 ms と 1.0 s（RRS、PCIe 6.0 §6.6、§2.3.1 の RRS の決まり）。
  - Readiness Time Reporting（§7.9.16）があれば、その時間で読む。この能力を持つ関数は、言った時間の後に RRS を返してはならない。
  - Status の Immediate Readiness（bit 0）が立っていれば待たない。
  - USB 2.0：TATTDB ≥100 ms（§7.1.7.3）、TRSTRCY 10 ms（§7.1.7.5）、TDSETADDR 50 ms（§9.2.6.3）。root port の reset の長さ TDRSTR は xHC が数え、終わりを PRC で知らせる。
  - xHCI の HCH は、R/S を消してから 16 ms 以内（§5.4.1/5.4.2）。16 ms の時に一度だけ読む。

**3. 仕様が最大だけを言う所**
- 対象：
  - NVMe の CSTS.RDY：CAP.TO か CRTO.CRIMT/CRWMT（NVMe 2.4 §3.5.4、Figure 57）。
  - NVMe の CSTS.SHST：RTD3E（§3.6。RTD3E が 0 なら 1 s 以上）。
  - xHCI の CNR、Save/Restore State の SSS/RSS。
- ここは読むほかない。
- 案：最大を期限として、間を倍に延ばしながら読む（読む数は log(最大/初め)）。
  - 始めと reset の時だけで、定常の道には無い。
  - 装置の start の文脈で走るので、他の装置と並ぶ。
- この案は「間隔の見回り」の字に当たる。**本体の判断が要る。**
- 別の案：最大の時に一度だけ読む。字には合うが、CAP.TO が数十秒の装置では起動が遅くなる。

**4. µs の仕様の待ちは `clock_spin`**
- 例：HDA の CRST の後の 521 µs。sys/clock.h の約束どおり。

### 2.7 vm/ との境

**共通の口に乗せる**
- vm/ の driver は共通の口に乗る：装置の木の driver、blk の提供者、netdev、音の装置、表示の提供者、入力の router。
- 共通コードは VM を判定しない。

**`vm/vm.h` の口の行き先**
- 残す：`vm_detect`、`vm_machine`/`vm_hosted`、`vm_session_init`。
- `vm_init`：vm の bus（VMBus）と vm の driver（e1000・virtio_gpu・vbe）を、装置の木に一度に登録する。実機では何もしない。
- 消える（照合が代わる）：`vm_storage_init`、`vm_net_probe`、`vm_audio_init`、`vm_display_pm`。
- 消す：`vm_power_off`（vm.c:84-91）。q35 は ACPI の S5（uACPI）で止まる。
- 替える：`vm_suspend`・`vm_resume`・`vm_hibernate_blocker` は、VMBus の bus の節の `suspend`・`resume` と「眠れない理由」にする。

**入力**
- vm/hv/hv_input.c と vm/rdp.c は、set 1 の符号を vm/ の中の表で HID の usage に訳し、router へ渡す。
- RDP の Unicode は router の「文字」の出来事（`input_text`）。

**音**
- RDP の音は、音の装置の一つとして登録する。`audio_remote*` の特別な口は消す。

**表示**
- virtio_gpu の 3D の描き（gpu.h）は消し、2D の scanout だけを表示の提供者にする。

### 2.8 他の層との継ぎ目

**block ↔ 格納庫**
- blk は装置の到着を出来事で知らせる。識別：model、serial、NGUID/EUI64。
- 要求は非同期：read、write、flush、discard、write zeroes。旗に FUA。
- 限りを言う：論理と物理の block、最大の転送、segment の数と境、整列、write cache の有無、原子の書きの単位。
- 格納庫の書きの順序は、flush と FUA で言う。
- 同期の呼び手には `blk_wait`：送って、完了で眠る。
- 分割（GPT）は格納庫の持ち物とする。今の持ち主は未確認。

**NIC ↔ 網**
- netdev の口は net/ の持ち物。装置の側から要るもの：
  - TX と RX のキューの数（CPU の群へ割る）、RSS。
  - 受けの時刻：割り込みの時、装置に時計があればそれ。
  - checksum と TSO の能力、link の出来事。
  - 受けの頁は mm の頁の池から（写しなし）。
- 実機の NIC の driver は、dev/ の部品として後の段で建てる（板の chip は未確認）。

**表示 ↔ 窓**
- wm は、表示の核の唯一の desktop の客。
- 窓に見せるもの：connector（EDID、mode、状態）、CRTC（mode、vblank の番号と時刻）、plane（形式、buffer）。
- commit には目標の vblank を付ける。完了の出来事（vblank の時刻と番号）が EV_FRAME の `time` と番号になる（3-T §4.5）。
- gui/screenmode.c は connector の EDID を使う。

**HID ↔ 窓**
- router は usage の束（装置の id、時刻、usage と値の列）を窓の seat へ渡す。
- keymap、繰り返し（窓の alarm）、caps lock と LED の状態は窓の側に置く。
- LED は、窓が router を通して各 keyboard へ送る（Set_Report）。

**音 ↔ 窓の音の口**
- app の stream（syscall の持ち主は音）。played_frames と played_time（3-T §4.5）。
- 窓の側の方針（窓ごとの音量など）は、stream の持ち主の process から引く。

**アプリのドライバー**
- claim は節の handle。
- BAR は mm の range の shared_pages、DMA は mm の domain、割り込みは events、ACPI の eval と Notify は相棒から、眠りは channel で。画面は表示の提供者として登録する。

---

## 3. 各部品の設計

### 3.1 PCI の核（`dev/pci.{h,c}`、新しい `dev/pci_power.c`）

**設定の空間**
- ECAM だけ使う（MCFG の segment と bus の範囲）。CF8/CFC は消す。
- ECAM は host bridge の bus の範囲ごとに写す。
- 読みは錠なし。同じ関数の読み書き替え（RMW）は節の錠で。

**列挙**
- host bridge ごとに並べて行う。bridge の secondary/subordinate の範囲だけを読む。
- Header Type の bit 7 が無ければ関数 0 だけ。ARI なら 256 関数。
- bus 番号と BAR は firmware の振りを使う（振り直さない）。
- 能力の鎖を一度読んで節に置く：MSI-X、PM、PCIe、ACS、ReBAR、Readiness Time、DRS/FRS、LTR、L1 PM Substates。

**_OSC**（ACPI 6.5 §6.2.11）
- native の hot-plug・PME・AER・LTR・DPC の制御を求め、得たものだけを使う。
- bit の形は PCI Firmware 仕様にあり、手元に無い（未確認）。

**DMA**
- busmaster は、IOMMU の domain が付いてから立てる（`pci_dma_enable`）。
- 今の `pci_enable_busmaster`（pci.c:269-274）は消す。

**reset と準備**（§2.6）
- FLR：Initiate FLR の後、Readiness Time の FLR Time か 100 ms と 1.0 s の二点で読む。
- SBR：親の bridge で行う。
  - Trst（1 ms）は alarm で待つ。
  - link の上がりは DLLSC か LBMS の割り込みで知る。無ければ二点で読む。
- reset の錠は関数か bridge ごと。全体の `reset_mutex` は消す。

**保存と戻し**
- 今の手順（573-668）を写す。
- 起きでは関数ごとに並べる。今の順に回る `ready_id` は消す。

**十倍**
- EPYC の数千の関数でも、列挙は bridge の数で並び、引きは表で済む。SR-IOV の VF は後の段。

### 3.2 block の層（`dev/blk.{h,c}`）

**`struct blk_device`**
- 限り（§2.8）、hardware のキューの数、CPU → キューの表（start の時に topology から作る）、識別、ops の `submit(queue, req)`。

**`struct blk_request`**
- op、開始の block。
- segment の列：物理の frame・offset・長さ、または mm の DMA の写像。
- 旗：FUA、前の flush。
- 期限：出し手が付ける（任意）。
- 完了の呼び戻し：割り込みの文脈で呼ばれる。眠らない。`wait_item` の呼び戻しと同じ約束。
- 状態。

**送り**
- `blk_submit(dev, req)` は this CPU のキューへ送る。全体の錠は無い。
- 限りを越える要求（MDTS など）は、層が割る。
- キューが満ちていたら CPU ごとの待ちの列に置き、完了の時に送り直す。見回りは無い。

**写し（bounce）は無い**
- 例外は、装置が届かない番地の時だけ（IOMMU の無い機械の 32 bit の装置）。その時は mm の帯の頁を使う。

**門（hold）**
- 装置ごとに、CPU ごとの inflight の数と門の旗を持つ。
- `blk_hold` は旗を立て、inflight が 0 になるのを wait queue で待つ。完了が旗を見て起こす。
- 旗の見え方は 2b-O の epoch で揃える。

**期限**
- キューごとに期限の heap と alarm を一つ持つ。
- 期限が来たら、まず装置の abort（NVMe の Abort §5.2.1）。駄目なら制御器の reset。

**同期の道**
- `blk_wait(req)`：送って、完了の wait queue で眠る。hibernate と格納庫の同期の道が使う。

**耐久**
- write cache のある装置には、flush を装置へ送る。
- persist の数（今の 116-128）は、装置ごとの flush の完了の列番号として出す。

**その他**
- I/O の順序を決める scheduler は持たない（NVMe）。

### 3.3 NVMe（`dev/nvme.c`）

**始め**
1. CC.EN を立て、CSTS.RDY を待つ（§2.6 の 3）。
2. admin のキュー、Identify。
3. Number of Queues（FID 07h、§5.2.30.1.5）で、I/O の SQ/CQ を CPU の群の数だけ求める。
4. 答えと MSI-X の項の数から、キューの数を決める。CQ ごとに項を一つ、その群の CPU へ向ける。admin の CQ は項 0。

**大きさ**
- キューの深さは CAP.MQES+1。命令の id の表はキューの深さで作る。決め打ちは無い。
- メモリーは装置の node から取る。

**写しなし**
- segment の列から、PRP（§4.3.1）を直に作る。PRP の list の頁はキューごとの池から取る。池の長さは MDTS と深さから。
- 整列の合わない要求と大きな要求には、SGL（§4.3.2）を使う。Identify Controller の SGLS が言う時だけ。

**完了**
- CQ の割り込みの hard handler が phase の bit で読み、呼び戻しを呼ぶ。保険の見回りは無い。

**期限と回復**
- 出し手の期限 → Abort → 駄目なら制御器の reset。reset ではキューを作り直し、行きの要求を送り直すか失敗にする。

**電源**
- shutdown は CC.SHN を立て、CSTS.SHST を RTD3E の期限で読む（§3.6。2.4 の本文で、RTD3E が 0 なら 1 s 以上待つ）。
- 起きは reset から始める。
- APST（FID 0Ch、§5.2.30.1.7）と HMB は後の段。

**名前空間**
- active NS list から作り、数の決め打ちは無い。
- 名前空間の出来事は Asynchronous Event Request（§5.2.2）で受ける。

### 3.4 AHCI

**判定：消す**
- 起動の道は NVMe で、disk は一つ。
- 今のコードは slot 0、写し、見回り（§1）。
- 7600X の SoC の PCI の表（PPR 56713 Table 13）に、SATA の制御器は無い。AM5 の chipset の仕様は手元に無い（未確認）。

**実機の C: が SATA の時に建てる形**
- port ごとに一つの hardware のキュー。NCQ の 32 slot（CAP.NCS、CAP.SNCQ）。
- port ごとの割り込み。MSI の複数の message が無ければ、一つの vector を IS の bit で振り分ける。
- PRDT で写しなし。
- COMRESET と spin-up の待ちは §2.6。

### 3.5 xHCI と USB の核（`dev/xhci.c`、`dev/usb.{h,c}`）

**usb の thread は無い。転送は非同期**
- `struct usb_transfer`：endpoint、segment の列、長さ、完了の呼び戻し、完了の時刻。isochronous なら frame の列も持つ。
- `usb_submit`：どの CPU のどの文脈からでも呼べ、眠らない。control・bulk・interrupt・isochronous を扱う。
- 同期の呼び手には `usb_wait`。

**錠**
- ring ごとの spinlock（送りの間だけ）、slot ごとの状態の錠、command ring の錠。全体の `usb_lock` は無い。

**interrupter**
- 数は MaxIntrs（HCSPARAMS1、§5.3.3）と MSI-X の項の小さい方。MSI だけなら 32 まで（§4.17、"Interrupters and PCI Interrupt Mechanisms"）。
- Primary Interrupter（0）は Command Completion と Port Status Change を受ける。どちらも固定（§4.17.4、"Fixed and always the Primary Interrupter"）。
- HID と音の endpoint は、IMOD を 0 にした低遅れの interrupter へ。bulk は CPU の群ごとの interrupter へ置き、IMOD をかける。
- 行き先は TRB と Slot Context の Interrupter Target で決める（§4.17.4）。
- event ring の segment の数と大きさは、ERST Max と負荷から決める。

**isochronous**
- Isoch TRB（Figure 6-12、Table 6-32〜6-34）、Frame ID（§4.11.2.5）と SIA、周期の帯域（§4.14.2）。
- MFINDEX（§5.5.1）と割り込みの時刻で、frame と単調の時刻を結ぶ。
- USB Audio（手元に Audio2・Audio4 の仕様がある）の土台になる。

**命令**
- Command Completion Event を wait queue で待つ。
- 期限が来たら Command Abort（§4.6.1.2）。期限の値は仕様に見当たらない（未確認）。

**port**
- Port Status Change Event で知る。debounce と回復の待ちは §2.6 の二点。

**眠り**
- Save State（USBCMD.CSS）と Restore State（CRS）で装置の文脈を保つ（§4.23.2、§5.4.1）。数え直しは失敗した時だけ。
- USB の disk があっても眠る。MSC の suspend が行きの要求を終える。

**外れ**
- 抜けた装置は子から remove する。

### 3.6 hub（`dev/usbhub.c`）

- hub は USB の class driver の一つ。
- 状態の変わりは、hub の interrupt endpoint への非同期の転送で知る。
- port の reset・電源・debounce は §2.6。
- `HUB_PORTS 15`（route string の 4 bit）と tier の深さは仕様の限り。
- 高速 hub の下の全速・低速は、Slot Context の TT の欄で扱う。

### 3.7 HID と入力の道（`dev/usbhid.c`、新しい `dev/hid.c`、`dev/input.{h,c}`）

**report**
- report descriptor（HID 1.11 §6.2.2）を一般に読む（hid.c）。
- report protocol だけを使う。boot protocol（Appendix B）は使わない。
- keyboard には Set_Idle(0)（§7.2.4）を送る。
- 報告の時刻は interrupter の割り込みの時（§2.3）。

**router（input.c）**
- `input_report(dev, time, usages, n)`：usage page・usage・値・相対か絶対か・配列の差。
- `input_text(dev, time, codepoint)`：RDP などの文字。
- 装置ごとに輪を持つ。書き手は一つ（その装置の完了の文脈）。router は輪に書いて、窓の入力の thread（`SCHED_PRIORITY_INPUT`）を起こすだけ。錠の下で sink を呼ばない。
- 輪の長さは、装置の報告の速さ（bInterval）と消費者の遅れから決める。
- 溢れたら、相対の軸を足し合わせて一つにし、落とした数を付ける。

**usage**
- ボタンと軸は descriptor が言う数だけ扱う（`& 7` を消す）。
- 使う page：HUT 1.7 の Keyboard/Keypad（第 10 章、0x07）、LED（第 11 章、0x08）、Generic Desktop（第 4 章）、Consumer（第 15 章）、Digitizers（第 16 章）。

**窓の側**
- keymap、繰り返し（alarm）、caps lock の状態、LED の要求を持つ。

### 3.8 mass storage（`dev/usbmsc.c`）

- BOT（usbmassbulk_10）を非同期の転送で行い、blk の提供者になる。
- LUN は Get Max LUN の答え（0〜15）の数だけ。
- TEST UNIT READY の 40 回の輪は消す。準備は SCSI の sense（NOT READY）で見て、期限は §2.6。
- UAS は仕様が手元に無い（未確認）。
- 眠りでは、行きの要求を終えてから門を閉じる。

### 3.9 USB の網（`dev/usbnet.c`、後の段、任意）

- CDC ECM・NCM（CDC 1.2、NCM 1.1）の class driver を、netdev の提供者として建てる。
- adapter の数は決め打ちしない。各 adapter が一つの NIC。
- RX は bulk の転送を並べる。数は NTB の大きさと遅れから決める。時刻は完了の時。

### 3.10 音（`dev/audio.{h,c}`、新しい `dev/hda.c`）

**HDA の制御器と codec**
- CORB/RIRB は RIRB の割り込みで受ける（見回りなし）。
- codec は STATESTS の起こしで見つける。
- widget の木（§7.3.4 の parameter）から、pin → converter の道を作る。
- jack の抜き差しは Unsolicited Response（§7.3.3.14）で知る。

**stream**
- BDL（2〜256 の項、§3.6.2）を周期ごとに分け、各項に IOC を立てる。
- 周期の長さは、stream が求める遅れの最小から決める。
- 周期を変える時は RUN を 0 にして BDL を作り直す（BDL は RUN の間変えない、§3.6.2）。

**位置と時刻**
- 位置は DMA Position Buffer（§3.6.1。DPLBASE/DPUBASE は §3.3.32・§3.3.33）から読む。
- 時刻は WALCLK（§3.3.16）。名目 24 MHz の 32 bit の数で、約 179 s で一周する。
- IOC の hard handler で、(位置, WALCLK, `clock_now_ns`) の組を取る。
  - WALCLK と単調の時計の速さの比を直し続ける。
  - stream が動く間は一周より短い間隔で組が来るので、64 bit へ延ばせる。
- codec の遅れ（Audio Widget Capabilities の Delay）を足して played_time にする。

**混ぜ手**
- 出力の装置ごとに、実時間の thread（`SCHED_PRIORITY_SOUND`）を一つ置く。IOC で起き、次の周期の分を混ぜて DMA の buffer へ書く。
- stream は app と混ぜ手の間の輪で、書き手も読み手も一つ（錠なし）。
- 混ぜは錠の外、割り込みの外で行う。
- 装置の速さは codec の能力から選ぶ（48 kHz を好む）。stream の速さが違えば、stream ごとに速さを変えて混ぜる。`AUDIO_RATE` の決め打ちを消す。

**状態**
- `n_audio_status` の played_frames と played_time（3-T §4.5）。

**7600X**
- SoC に HD Audio Controller 1022:15E3、ACP 1022:15E2、Display HD Audio がある（PPR 56713 Table 13）。
- 板の codec がどれに付くかは未確認。RTX 4070 の HDA の関数は §7。

**vm**
- RDP と AC'97 は音の装置の口に乗る（§2.7）。

### 3.11 表示（新しい `dev/display.{h,c}`、`dev/display_boot.c`、`dev/fbcon.c`）

**模型**
- `display_device` の下に三つの物を置く：
  - connector：EDID、mode の一覧、接続の状態。
  - CRTC：今の mode、vblank の番号と時刻。
  - plane：primary・cursor・overlay、形式、buffer。
- wm は atomic な commit（plane → buffer、CRTC → mode、目標の vblank）を出し、完了の出来事（vblank の時刻と番号）を受ける。

**提供者**
- 起動の画面（GOP の面、`display_boot.c`）：
  - 今の fb.c の LUT の変換と CPU の写しを使う。
  - vblank は無いので、時刻は「推定」の旗を付けた写しの終わりの時。
- app のドライバー（nvrm）：
  - claim の上で提供者として登録する。
  - vblank の時刻は、driver が時計の頁で読んだ単調の ns。
- vm：virtio_gpu の 2D、vbe。

**渡し**（起動の提供者 → app の提供者）
- app が hold を求めると、核は起動の面への写しを止め、行きの写しが終わるのを wait queue で待つ。
  - 100 µs の見回りは消す。
  - 写しは thread からだけ来る（fbcon は 1-D から自分の thread で描く。fbcon.c:311）ので、wait queue で起こせる。
- hold は時間で終えない（今の意味を残す）。
- app の提供者が登録したら、wm の desktop はそちらへ移る。
- claim が終わった時：起動の面がまだ有効なら戻す。GPU が作り替えて無効なら、表示なしで panic の面だけを残す。

**panic の面**
- 各提供者は「今示している buffer」と「示されうる app の image」（今の `alsos`）を登録する。
- panic はそのすべてへ描く（fb.c:310-361 の形）。

**fbcon**
- 起動の面へ描き、wm が画面を取ったら止まり、panic で戻る。形は保つ。

### 3.12 アプリのドライバーの道（新しい `dev/claim.{h,c}`）

**claim**
- claim は装置の節の結びの一つで、driver の代わりに app が結ばれる。群（ACS）の関数をまとめて持つ。
- BAR の写像は mm の range の shared_pages。
- DMA は mm の domain で、厳しい無効化。
- 割り込みは MSI-X の項ごとに events（時刻つき）。数は装置の Table Size で決め、32 の決め打ちを消す。

**錠と設定の空間**
- 錠は claim ごとの mutex。確保と写しをまたぐ。
- 割り込みの hard handler は、claim の spinlock の下で events に知らせるだけ。
- 設定の空間の読み書きは、claim の関数と、上流の bridge の決まった欄だけ。

**ACPI**
- eval は相棒の節の method を、拒む表で絞る。
- Notify は events で渡す。上書きの輪を消す。

**眠り**：§2.5。**画面**：§3.11。

**終わり**
- app が死んだら、装置を止め（FLR か SBR、§3.1）、domain を外し、hold を返す。

### 3.13 ACPI（`dev/acpi.c` の表は残す。新しい `dev/acpi_aml.c`）

**uACPI の口**
- map と unmap は参照の数を持つ。unmap は本当に外す。
- sleep は `sleep_until`。AML の Stall（µs）は `clock_spin`。
- mutex と event は wait queue と期限で。
- PCI の設定は、すべての segment の ECAM で。
- port の I/O は、名前空間が言う範囲だけ。

**仕事**
- GPE の実行と Notify の配りに、それぞれ縛らない thread を一つずつ置く（uACPI の work の型に合わせる）。
- AML は uACPI の中で順に走る見込み（未確認）。

**凍り（`acpi_freeze`）は無くなる**
- 起きで CPU と thread が先に戻るので、AML はいつも眠れる。

**列挙と出来事**
- 名前空間の列挙で、木に節を足す（§2.1）。PNP0C0F の link の装置も節にする（LINKS_MAX を消す）。
- SCI → GPE と固定の出来事。電源の button は `pm_request` へ。
- Notify は節の driver か claim へ。
- EC（PNP0C09）の driver を置く。uACPI が EC の address space を持つかは未確認。
- 熱の zone は Notify で受ける。_TZP の見回りを求める zone は扱わない（§7）。

**眠りの入口**
- `_PTS`・`_WAK`、S3・S5・reset を uACPI の口で行う。power.c を消す。
- `_PRW`・`_S0W`・`_SxD`・`_DSW`・`_PRx`（§7.3）を、節の電源に使う。

### 3.14 IOMMU（AMD）（`dev/iommu.{h,c}`、`dev/iommu_amd.c`）

- §2.4 のとおり。
- IVRS からユニットと、それが訳す装置の範囲を読む。
- ユニットごとに、命令の輪と錠、event log の割り込み、COMPLETION_WAIT の割り込みを持つ。
- 起き：ユニットを戻し、`INVALIDATE_IOMMU_ALL` を一度送る。IR の表を戻してから APIC を x2APIC へ入れる。この順は今のまま残す（pm.c:213-219 の注釈）。

### 3.15 乱数（`dev/random.{h,c}`）

**CPU ごとの生成器**
- ChaCha20 の鍵・番号・出力の残りを CPU ごとに持つ。
- 読みは横取りを止め、自分の CPU の生成器から出す。出した後に鍵を作り替える（fast key erasure）。全体の錠は無い。

**種の溜め**
- CPU ごとの feed（割り込みの時刻を混ぜる、今の形）を、その CPU が種を足す時に、自分の分だけ溜めへ畳む。O(1)。
- 溜めの混ぜは暗号の hash（例：BLAKE2s）で行う。溜めの錠を持つのは畳む間だけ。

**取り直し**
- CPU の生成器は、溜めの世代が進み、かつ前の取り直しの後に使われていたら、次の読みで種を取り直す。時間の見回りは無い。
- RDSEED は取り直しの時に、RDRAND は起動の時に使う。どちらも失敗しても再試行の輪は回さない。

**起動**
- 最初の種は RDSEED から。空の輪は消す。

### 3.16 serial（そのまま）

- 変えない。記録の読み手の口（serial.h）もそのまま。

### 3.17 refclock（そのまま）

- 変えない。

### 3.18 休止（`dev/hibernate.{h,c}`）

- 写しは CPU 0 で、他の CPU を止めて行う。機械を静かにするために要る。
- 書きは装置が起きた後、非同期の blk で並べる。圧縮も全 CPU で並べる。
- 予備の頁の数は、RAM と圧縮の見込みから決める（RESERVE の決め打ちを消す）。
- 完全さは暗号の hash で見る（FNV を消す）。
- 暗号化は鍵の源を決めてから（§7）。
- 8259 の注釈を消す。

### 3.19 電源の持ち主（`dev/pm.{h,c}`）

- §2.5。
- 電源の thread は段の順だけを持つ。装置の段は木の歩きとして池へ流す。
- `pm_request` の約束（どの文脈からでも呼べ、眠らない）は残す。

---

## 4. 各部品：責務と持ち主、十倍で詰まる所、錠と眠れる文脈、時間、vm/ との境

各部品を、次の五つで書く：
- **責務と持ち主**
- **十倍で詰まる所**
- **錠と文脈**
- **時間**
- **vm/ との境**

**装置の核（`dev/device.c`）**
- 責務と持ち主：木、照合、寿命、装置の仕事の池、電源の木の歩き。
- 十倍：節は EPYC で数千になる。照合は出来事ごとに bus の表を引く。池は node ごと。眠りの並びの長さは、数ではなく深さで決まる。
- 錠と文脈：木の形は rwmutex、節の状態は節の spinlock。呼び戻しはすべて、眠れる thread の文脈で、木の錠を持たずに呼ぶ。
- 時間：自分では期限を持たない。driver の期限を記録する。
- vm/：VMBus の bus の型と節は vm/ が登録する。

**PCI（`dev/pci.c`、`dev/pci_power.c`）**
- 責務と持ち主：設定の空間、列挙、能力、MSI-X、reset と準備、保存と戻し。
- 十倍：関数が増えても表で引く。列挙は bridge ごとに並ぶ。segment ごとに表を持つ。
- 錠と文脈：ECAM の読みは錠なし。関数ごとの錠（RMW と reset）。reset は眠れる文脈でだけ行う。
- 時間：§2.6（DRS/FRS、Readiness Time、二点）。期限は単調の時計。
- vm/：QEMU の装置も同じ道を通る。Hyper-V 第 2 世代に PCI は無い（VMBus だけ）。

**claim（`dev/claim.c`）**
- 責務と持ち主：app のドライバーへ装置を貸す。
- 十倍：claim の数も項の数も、claim ごとの錠と、装置の能力の大きさで扱う。
- 錠と文脈：claim の mutex（mm の口と写しをまたぐ）。hard handler は spinlock の下で events に知らせるだけ。
- 時間：割り込みの時刻を events に付ける。眠りの答えに期限。
- vm/：無し。

**ACPI（`dev/acpi.c`・`dev/acpi_aml.c`）**
- 責務と持ち主：表、AML、名前空間の節、出来事、眠りの入口。
- 十倍：Notify や GPE の嵐では、AML が uACPI の中で順に走るので詰まりが残る。AML の本性で、§5 の劣る所に書く。名前空間の列挙は一度だけ。
- 錠と文脈：uACPI の mutex は wait queue。SCI の hard handler は短く、GPE の実行は thread で。
- 時間：AML の Sleep・Wait・Acquire の期限は単調の ns。Stall は `clock_spin`。
- vm/：無し。QEMU も Hyper-V も ACPI は同じ道。

**IOMMU（`dev/iommu.c`・`dev/iommu_amd.c`）**
- 責務と持ち主：domain の実行、無効化、IR、故障。domain の物と IOVA は mm の持ち物。
- 十倍：
  - NVMe の百万 IOPS では unmap が多い。遅い無効化で集め、IOVA は CPU ごとの溜めから取る。
  - EPYC のようにユニットが多くても、無効化は domain ごとのユニットの集合へだけ送る。
- 錠と文脈：ユニットの命令の輪の spinlock（尾を書く間）。domain の頁表は範囲の木の錠。完了の待ちは wait queue で眠る。
- 時間：完了は割り込みで知る。ユニットが答えない時の期限の値は仕様に無い（未確認）。出し手の要求の期限に従わせる案。
- vm/：無し。

**blk（`dev/blk.c`）**
- 責務と持ち主：要求の口、割り、門、期限。
- 十倍：IOPS は CPU ごとの送りと完了で受ける。装置の数は表で（MAX_BLK を消す）。
- 錠と文脈：送りの spinlock は提供者のキューごと。CPU ごとの待ちの列は横取りを止めて触る。完了の呼び戻しは割り込みの文脈。`blk_wait` は眠る。
- 時間：出し手の期限を、キューごとの heap と alarm で見る。
- vm/：storvsc が提供者になり、VMBus の subchannel を CPU の群へ割る。ata は消す。

**NVMe（`dev/nvme.c`）**
- 責務と持ち主：NVMe の制御器と名前空間。blk の提供者。
- 十倍：キューは装置の能力まで作り、CPU がそれより多ければ群で分ける。深さは CAP.MQES。
- 錠と文脈：SQ の尾の spinlock はキューごと。CQ は割り込みの CPU だけが読む。admin の命令は admin のキューの錠。
- 時間：CRTO・CAP.TO、RTD3E、出し手の期限。
- vm/：QEMU の NVMe は実機と同じ道。

**xHCI と USB の核（`dev/xhci.c`・`dev/usb.c`）**
- 責務と持ち主：HC、転送、列挙、class の照合。
- 十倍：
  - 装置が増えても、slot ごとの錠と event ring の segment で受ける。
  - 8 kHz の mouse は低遅れの interrupter に置き、HID の輪で合わせる。
- 錠と文脈：ring の spinlock、command ring の錠、slot の錠。完了の呼び戻しは割り込みの文脈。列挙（Address Device など）は眠れる仕事の文脈。
- 時間：割り込みの時刻、MFINDEX と時刻の組、USB 2.0 の時間（§2.6）、命令の期限（値は未確認）。
- vm/：qemu-xhci は実機と同じ道。Hyper-V 第 2 世代に USB は無く、hv_input が代わる。

**hub（`dev/usbhub.c`）**
- 責務と持ち主：port の状態、reset、TT。
- 十倍：hub の数と深さは、仕様の tier で限られる。
- 錠と文脈：hub ごとの mutex（眠れる仕事の文脈）。
- 時間：TATTDB、TRSTRCY。
- vm/：無し。

**HID と入力（`dev/usbhid.c`・`dev/hid.c`・`dev/input.c`）**
- 責務と持ち主：report を usage に読み、時刻つきで窓へ渡す。
- 十倍：装置の数と報告の速さは、装置ごとの輪と相対の軸の合わせで受ける。
- 錠と文脈：輪は書き手が一つで錠なし。読むのは窓の入力の thread。
- 時間：報告の時刻は割り込みの時。繰り返しは窓の alarm。
- vm/：hv_input と rdp は、vm/ で usage へ訳して router へ渡す。

**mass storage（`dev/usbmsc.c`）**
- 責務と持ち主：BOT。blk の提供者。
- 十倍：LUN と装置の数は、装置ごとの状態で扱う。
- 錠と文脈：装置ごとの mutex（命令の順）。完了は呼び戻しで。
- 時間：sense と §2.6。
- vm/：無し。

**USB の網（`dev/usbnet.c`、後の段）**
- 責務と持ち主：CDC ECM・NCM。netdev の提供者。
- 十倍：adapter ごとに NIC を一つ。
- 錠と文脈：adapter ごとの錠。
- 時間：受けの時刻は完了の時。
- vm/：無し。

**音（`dev/audio.c`・`dev/hda.c`）**
- 責務と持ち主：装置、stream、混ぜ手、位置と時刻。
- 十倍：
  - 混ぜの費用は stream の数 × frame の数。混ぜ手を出力の装置ごとに分け、stream の輪は錠なしにする。
  - 装置の数は、装置ごとの thread で受ける。
- 錠と文脈：stream の輪は錠なし。装置の設定（音量と経路）は装置の mutex。IOC の hard handler は、組を取って thread を起こすだけ。
- 時間：WALCLK と位置の組。単調の ns。
- vm/：AC'97（残すなら）と RDP は、音の装置として登録する。

**表示（`dev/display.c`・`dev/display_boot.c`・`dev/fbcon.c`）**
- 責務と持ち主：模型、commit、vblank、提供者、hold、panic の面。
- 十倍：connector と CRTC は提供者が言う数だけ。vblank の出来事は CRTC ごと。
- 錠と文脈：
  - commit は表示の装置の mutex。vblank の完了は、錠を持たずに出来事で待つ。
  - panic の面の一覧は錠なし（今の atomic の鎖）。
- 時間：vblank の時刻（提供者が読んだ単調の ns）。起動の面は推定の旗。
- vm/：
  - virtio_gpu と vbe は提供者。
  - Hyper-V 第 2 世代は、GOP の面（起動の提供者）と RDP（窓の側の `wm_remote_*`）。

**乱数（`dev/random.c`）**
- 責務と持ち主：CPU ごとの生成器と、種の溜め。
- 十倍：読み手は CPU ごとに読むので、全体の錠は無い。
- 錠と文脈：溜めの spinlock は畳む間だけ。
- 時間：無し。
- vm/：無し。

**serial（そのまま）**
- 責務と持ち主：記録の出口（1-D）。
- 十倍：記録の量は 1-D の CPU ごとの輪で受ける。
- 錠と文脈：今のまま。
- 時間：今のまま。
- vm/：無し。

**refclock（そのまま）**
- 責務と持ち主：HPET による速さの基準と RTC（1-B2）。
- 十倍：無し。
- 錠と文脈：今のまま。
- 時間：時計そのもの。
- vm/：無し。

**休止（`dev/hibernate.c`）**
- 責務と持ち主：RAM の写しと、書きと、戻し。
- 十倍：RAM が大きくなっても、圧縮と書きを並べる。
- 錠と文脈：写しは割り込みを閉じた CPU 0。書きは眠れる thread。
- 時間：blk の期限。
- vm/：VMBus の bus の節が眠れない時に理由を言う。

**電源（`dev/pm.c`）**
- 責務と持ち主：段の順と、木の歩き。
- 十倍：装置が増えても並べるので、長さは深さで決まる。
- 錠と文脈：`pm_lock`（短い）、木の歩きの数（atomic）と wait queue。
- 時間：driver の期限。猶予は設定の値。
- vm/：VMBus の bus の節が `suspend` と `resume` を持つ。

---

## 5. Linux・NT の公開の設計との表

| 軸 | Linux | NT | Nocturne の案 | 劣る所 |
|---|---|---|---|---|
| 装置の模型 | driver core（bus・device・driver、sysfs、非同期の probe、deferred probe） | PnP manager（PDO・FDO と filter の device stack、IRP_MN_START_DEVICE） | 一つの木、表の照合、非同期の start、静まりの出来事 | filter driver は持たない（要らない）。PCIe の hot-plug・AER・DPC・SR-IOV は後の段で、それまで劣る |
| 割り込み | MSI-X、管理された affinity（blk-mq のキューと CPU の対） | MSI-X、processor の affinity の方針（IoConnectInterruptEx） | 同じ形（波 1 の irq.h） | 無し |
| block | blk-mq（CPU ごとの software のキュー → hardware の文脈、tag set、I/O scheduler、io_uring の polling） | storport（processor ごとのキュー、stornvme） | CPU ごとの送り、装置ごとの tag、scheduler なし | polling の I/O は決まりで持たないので、極端な低遅れでは Linux の IOPOLL に劣る。I/O の QoS も無い |
| NVMe | nvme-pci（CPU ごとのキュー、PRP/SGL、APST、HMB） | stornvme（同様） | 同じ形 | APST と HMB は後の段まで劣る |
| DMA と IOMMU | dma-iommu（群ごとの既定の domain、lazy と strict、IOVA の CPU ごとの溜め） | Kernel DMA Protection（外の口の装置を再写像、driver の対応の宣言） | すべての装置に domain。核は遅い無効化、app は厳しい無効化 | Linux と同じ。NT の細かい既定は未確認 |
| USB | 非同期の URB。公開の説明では、xHCI は主に一つの interrupter（細部は未確認） | USB 3 stack（UCX・USBXHCI、非同期） | 非同期の転送、複数の interrupter、isochronous | UAS が無い間は、USB の SSD で劣る |
| HID と入力 | input（evdev の時刻つきの出来事。繰り返しは核にも Wayland の client の側にもある） | HID class → kbdhid・mouhid → win32k、raw input。メッセージの時刻は ms | usage のまま ns の時刻、繰り返しは窓 | 無し |
| 音 | ALSA の HDA。位置と時刻（audio_tstamp：link・wallclock）、時間で動かす混ぜ（PipeWire） | WaveRT（位置の register の写像、QPC の時刻）、audiodg の混ぜ | IOC で起きる実時間の混ぜ手、WALCLK の時刻 | DSP の offload と経路の UI は無い（範囲の外） |
| 表示 | DRM/KMS（connector・CRTC・plane、atomic、vblank の時刻、simpledrm からの渡し） | WDDM（VidPN の source・target、Basic Display Driver からの渡し、DWM） | KMS に似た模型を、app のドライバーへ仲立ちする | GPU の driver が app にあるので、panic の面は提供者の登録に頼る。GPU の時間の割り（NT の hardware scheduling）は無い |
| 電源 | dpm（木の順、async suspend、親と子の待ち、noirq の段、runtime PM） | 電源 IRP（system と device の電源状態、PnP の関係の順、runtime の D-state、Modern Standby） | 木の順に並べ、眠れる文脈で、段は一つ | runtime PM と S0ix は、無い間劣る |
| ACPI | ACPICA と自前の継ぎ | acpi.sys（自前の解釈器） | uACPI | EC と熱の zone は、段が入るまで劣る。AML の並びは uACPI の造りに従う（未確認） |
| 乱数 | random.c（CPU ごとの ChaCha20、BLAKE2s の溜め、fast key erasure） | CNG（processor ごとの DRBG、公開の白書） | CPU ごとの ChaCha20、暗号の hash の溜め | 無し |
| 休止 | swsusp（swap へ、圧縮、暗号化は user の側） | hiberfil.sys（圧縮、BitLocker の下） | C: へ、並べた圧縮と書き | 暗号化の鍵の源が決まるまで劣る |
| 通知の無い状態の bit | 読む（NVMe の RDY など） | 読む | §2.6 | 本体の判断次第で同じか、遅い |
| VM の分け | hv_* などは普通の driver | 同様 | vm/ の後ろに置き、照合で乗せる | 無し |

---

## 6. 書き手への割り方

**前提**
- 波 3（3-V、3-S、3-T）が入ってから走らせる。要るもの：
  - 3-V の DMA の型と domain の口。
  - 3-S の syscall の持ち主のファイル。
  - 3-T の時刻の欄（`gui_event.time`、EV_FRAME の番号、`played_time`）。
- 同時に走らせる書き手は 5 未満（盤）。

**試験の機械**
- 基本：q35、edk2 の UEFI、`-device nvme`、e1000、qemu-xhci、usb-kbd、usb-tablet、QMP の指。
- 段 S で `-device ich9-intel-hda -device hda-output` を足し、AC'97 を外す（`scripts/test.py` と nocturne-project の手順書を直す）。

### 段 D0（順、一人）：装置の核と PCI の核

- 持つファイル：
  - 新しい `dev/device.{h,c}`。
  - `dev/pci.{h,c}`、新しい `dev/pci_power.c`。
  - `dev/pm.c` の装置の段の歩き：木の順にし、driver の `suspend`・`resume` を呼ぶ。今の pm の枠（CPU 0、他は止める）の中で、一つの CPU で順に回す。並べるのは段 P。
- 触ってよい所：
  - 各 PCI driver の入口を照合の登録へ替える。中身は変えない：`dev/nvme.c`・`dev/xhci.c`・`vm/e1000.c`・`vm/virtio_gpu.c`・`vm/vbe.c`・`vm/ac97.c`。
  - 今の `pm_register` の quiesce・resume を、driver の `suspend`・`resume` に名を替える。
  - `dev/pciuser.c` の引きを節で行う。
  - `dev/iommu.c` の群の計算を木の歩きへ替える。
  - `vm/vm.{h,c}`：`vm_init` に vm の driver の登録を入れ、消える口を消す。
  - 起動の順（`kernel/src/init.c` か boot の呼び手）。
- 消す：
  - `dev/ahci.c`、`dev/usbnet.c`。
  - `dev/gpu.h`、`vm/virtio_gpu.c` の 3D の部分、`gui/wm.c` の `gpu_init` の一行、syscall の `gpu_*` の手続き。
  - `vm/ata.c`、`vm/tulip.c`。
  - nocturne-project の SATA の試し方の記述。
- QEMU で確かめる：
  - 起動の行に、木の節（`seg:bus:dev.fn` と結ばれた driver）が出る。
  - `store: C: "…" on nvme0`。デスクトップが出る。
  - USB の指とキー、網が動く。
  - `prof` で、割り込みがどの CPU に来るか。
  - 休止と戻しが通る（起きの道を試せる）。
  - S3 の入り（QMP の suspended）。
- 実機だけで見えるもの：7600X の PCI の木（xHCI 1022:15B6/15B7、HDA 1022:15E3、GPU、chipset の bridge）、DRS・FRS・Readiness Time の有無。

### 一組目（並べる、4 人、D0 の後）

**A：ACPI**
- 持つファイル：`dev/acpi.{h,c}`（表は残し、AML の部分を `dev/acpi_aml.c` へ）。
- 触ってよい所：
  - `dev/power.c` を消す。
  - `vm/vm.{h,c}` の `vm_power_off` を消す。
  - pm.c の `acpi_*` を呼ぶ所（名前の替えだけ）。
- QEMU で確かめる：
  - menu からの電源断で QEMU が終わる（S5）。再起動で QEMU が再起動する（FADT の reset の register）。
  - QMP の `system_powerdown` が電源の button として `pm_request` に届く。
  - 名前空間の節（PNP0A08、PNP0103、PNP0B00、PNP0501）が木に出る。
  - _OSC の結果の行が出る。
- 実機だけ：EC、熱、電源の button、_OSC で得た制御、S3 の `_PTS`・`_WAK`。

**I：IOMMU**
- 持つファイル：`dev/iommu.{h,c}`、`dev/iommu_amd.c`。`dev/iommu_intel.c` を消す。
- 触ってよい所：装置の核の bind の hook（D0 が置く関数の中身）、3-V の mm の domain の口の呼び手。
- QEMU で確かめる：
  - IOMMU の無い機械で、何も変わらないこと。
  - `-device amd-iommu` が WHPX で使えるなら：各装置に domain が付き、NVMe・xHCI・e1000 が動く。完了の割り込みの数を記録する。
- 実機だけ：AMD-Vi の domain・無効化・故障の記録、nvrm の claim の domain。

**B：block と NVMe**
- 持つファイル：`dev/blk.{h,c}`、`dev/nvme.c`。
- 触ってよい所：
  - `fs/store.c` の blk の呼び手（`blk_wait`）。
  - `vm/hv/storvsc.c`（提供者）。
  - `dev/hibernate.c` と `dev/pm.c` の blk の呼び手（名前の替え）。
- QEMU で確かめる：
  - `-smp 4` で `nvme0: N queues` が出て、MSI-X の項が各 CPU へ向く。
  - C: が動く。ファイルの写しの速さを前と比べる。
  - `prof` で、完了が出した CPU に来る。
  - 電源断で flush が出る。
  - 保険の見回りが無いこと（`prof` の起きの数）。
- 実機だけ：実機の SSD のキューの数、RTD3E、SGL。Hyper-V の storvsc はユーザーが確かめる。

**U：USB と入力**
- 持つファイル：`dev/usb.{h,c}`、`dev/xhci.c`、`dev/usbhub.c`、`dev/usbhid.c`、新しい `dev/hid.c`、`dev/input.{h,c}`。
- 触ってよい所：
  - `gui/wm.c` の入力の sink の部分。
  - 新しい `gui/keyboard.c`（keymap と繰り返し）。窓の回の書き手と分担を合わせる。
  - `vm/hv/hv_input.c`。
  - `vm/rdp.c` の入力の部分。
- QEMU で確かめる：
  - QMP の指（abs）と HMP の `sendkey` が届く。`sendkey` の hold_time で、繰り返しが窓の側で起きる。
  - `gui_event.time` が割り込みの時になる。
  - `-device usb-hub` の下の装置。
  - QMP の `device_del` と `device_add` で、usb-kbd の抜き差し。
  - usb の thread が無いこと。interrupter の数の行。
- 実機だけ：CPU と chipset の xHCI、高い報告の速さの mouse、USB 3 の hub。

### 二組目（並べる、一組目の該当の段の後）

**S：音**
- 持つファイル：`dev/audio.{h,c}`、新しい `dev/hda.c`。
- 触ってよい所：
  - `vm/rdp.c` の音の部分（U の後）。
  - `vm/ac97.c`（消すか、乗せ替える）。
  - 音の syscall の持ち主のファイル（3-S の後の配置）。
  - `scripts/test.py` と手順書の機械の行。
- QEMU で確かめる：
  - QEMU の HDA で、codec の木の行が出る。
  - 試しの app で、played_frames と played_time が進む。
  - underrun の記録が無い。
  - 混ぜ手が実時間の thread で走る（`prof`）。
- 実機だけ：板の codec、jack の検知、WALCLK の実の速さ、RTX 4070 の HDA の関数。

**D：表示**
- 持つファイル：新しい `dev/display.{h,c}`、新しい `dev/display_boot.c`（fb.c から）、`dev/fbcon.c`。
- 触ってよい所：
  - `gui/wm.c` の表示の客の部分（U の後）、`gui/screenmode.c`。
  - `vm/virtio_gpu.c` と `vm/vbe.c`（提供者として）。
- QEMU で確かめる：
  - 起動の画面から wm への渡し。
  - EV_FRAME の時刻（std VGA では推定の旗）。
  - mode の変え。
  - panic の画面（試しの panic）。
- 実機だけ：GOP の面と EDID の拍。

**C：claim と app の提供者**
- 持つファイル：`dev/pciuser.*` を新しい `dev/claim.*` へ。
- 触ってよい所：
  - `user/apps/init.c`（節の handle で claim）。
  - nvrm の app の提供者の口の側。
  - claim の syscall の持ち主のファイル。
- 前の段：D の後。表示の提供者の口を使うため。
- QEMU で確かめる：init の claim の道が、nvrm の無い媒体で skip になる。claim の試しの app があればそれで。
- 実機だけ：RTX 4070 で nvrm の渡し、hold、vblank の時刻、claim の終わりで画面が戻るか。

**M：mass storage**
- 持つファイル：`dev/usbmsc.c`。
- 前の段：B と U の後。
- QEMU で確かめる：`-device usb-storage` が blk の装置として出て、読める。抜くと要求が `-N_ERR_GONE` で終わる。
- 実機だけ：実の USB の disk。

**R：乱数**
- 持つファイル：`dev/entropy.*` を `dev/random.*` へ。乱数の syscall の持ち主の部分。
- 前の段：小さいので、同時に 5 未満に収まるならどこでもよい。
- QEMU で確かめる：SYS_RANDOM の出力、TLS（ブラウザーで https の頁）、全体の錠が無いこと。
- 実機だけ：RDSEED の失敗の記録の有無。

### 三組目（順）

**P：電源**
- 持つファイル：`dev/pm.{h,c}`。
- 触ってよい所：
  - arch の眠りの呼び順（CPU を先に戻す）。
  - `gui/wm.c` の窓の数の出来事の口（窓の回と合わせる）。
  - process の終わりの出来事の口。
  - claim の眠りの channel。
- 前の段：全 driver の新しい `suspend`・`resume` が入った後。
- QEMU で確かめる：
  - 窓を開いたままの電源断と再起動（見回りなしで EV_CLOSE が流れる）。
  - menu からの S3 の入り（QMP の suspended）。
  - 休止と戻しで、起きが並ぶ（記録の時刻）。
  - WHPX は reset の欠陥で、S3 からの起きを試せない。
- 実機だけ：S3 からの復帰、並べた起きの時間、nvrm を持ったままの眠り。

**H：休止**
- 持つファイル：`dev/hibernate.{h,c}`。
- 前の段：B と P の後。
- QEMU で確かめる：C: への休止、次の起動での戻し、デスクトップが戻る、像の hash の検査。
- 実機だけ：大きな RAM での時間。

### 後の段（任意）

- N：USB の網。
- AHCI：実機の C: が SATA の時。
- APST・HMB、runtime PM。
- PCIe の hot-plug・AER・DPC、SR-IOV。
- UAS、USB Audio。
- 実機の NIC。

---

## 7. 仕様で確かめられなかったこと（未確認）

**手元に仕様が無いもの**
- PCI Firmware 仕様：_OSC の PCI の bit の形、MCFG の形。今のコードは MCFG を読んでいる。
- UAS の仕様。
- NVMe の NVM Command Set 仕様：Interrupt Coalescing の欄の詳しい形。Base 2.4 は参照するだけ。
- AM5 の chipset（Promontory 21）の xHCI と SATA の詳しい形。
- CXL の仕様：CXL の装置の列挙は扱っていない。

**実機で見るほかないもの**
- 7600X と AM5 の板：
  - root port が DRS・FRS・Readiness Time Reporting・Immediate Readiness を持つか。
  - firmware に S3 があるか（Low Power S0 Idle だけではないか）。
  - SPCR と HPET の有無。
- 板の音の codec の型と、それが HDA 1022:15E3 に付くか。ACP 1022:15E2 の役目。
- RTX 4070 の HDA の関数が、関数 0 と別の IOMMU の群になるか（ACS）。nvrm が関数 0 を持つ間に、核の HDA で HDMI の音が出せるか。
- ユーザーの実機の C: が NVMe か SATA か。AHCI の判定がこれで決まる。
- Zen 4 の机上の機械にある IOMMU のユニットの数。

**QEMU の模した装置の細部**
- intel-hda の WALCLK と DMA Position Buffer。
- qemu-xhci の interrupter の数と MSI-X。
- NVMe の SGL とキューの数、その property の名前。
- amd-iommu が WHPX で動くか。usb-audio と usb-net の造り。

**仕様の中で値を見つけられなかったもの**
- xHCI の命令の期限の値。§4.6.1.2 は Abort の手順だけ。
- xHCI の CNR と SSS/RSS の最大の時間。
- IOMMU の COMPLETION_WAIT が終わらない時の期限。
- HDA の codec の答えの時間の節の番号。
- xHCI の HCH の 16 ms は確かめた（§5.4.1/5.4.2）。

**uACPI**
- 仕事の型と AML の並び方、EC の address space を自分で持つか。uACPI の資料は仕様ではないので、照らしていない。

**ACPI の熱**
- _TZP が 0 でない熱の zone は、仕様の上で OS に見回りを求める。この案は扱わない。AM5 の板が _TZP を使うかは未確認。

**本体の判断が要るもの**
- §2.6 の 3：仕様が最大だけを言う状態の bit を、どう読むか。
- Intel の IOMMU を消すこと。
- 休止の像の暗号化の鍵の源。
- 分割（GPT）の今の持ち主。

**NT と Linux の公開の設計の細部**
- NT の Kernel DMA Protection の既定、USB の interrupter の使い方。
- Linux の xHCI の interrupter の使い方。

### Critical Files for Implementation
- D:\Programes\llm vibe slop os\kernel\src\dev\pci.c
- D:\Programes\llm vibe slop os\kernel\src\dev\pm.c
- D:\Programes\llm vibe slop os\kernel\src\dev\pciuser.c
- D:\Programes\llm vibe slop os\kernel\src\dev\xhci.c
- D:\Programes\llm vibe slop os\kernel\src\dev\blk.c
