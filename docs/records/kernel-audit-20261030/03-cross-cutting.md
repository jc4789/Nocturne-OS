# {"agentType": "Explore", "description": "カーネル全体の横断の洗い出し", "toolUseId": "toolu_01W6DsKeuJSEHTMgB8vBp9iw", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log を読むのはよい）。Linux や他の OS のソースを開かない。third_party は読まない。作業木には未コミットの変更があり、今のファイルをそのまま読む。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。あなたの仕事は、カーネル全体を横断して、下の八つの型に当たる所を漏れなく洗い出すこと。範囲は kernel/src/ の全部（arch・mm・lib・sys・fs・dev・net・gui・vm・boot.c・init.c・kernel.h）と common/store/、kernel/linker.ld。grep・rg と codegraph で候補を集め、各候補はその行の前後を読んで当たるかを確かめてから載せる（名前だけで決めない）。直し方は書かない。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・メモリー・装置・プログラム・利用者の数が増えても成り立つ形。対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）。CPU の下限は Ryzen 5 7600X（Zen 4）、上は 2 ソケットの EPYC 9996（論理 CPU 1024、NUMA、CXL のメモリーだけの node）。
- 後ろ向きの互換は要らない。数を決め打ちしない（ユーザー：「As many as the physical cpu / threads has. do not hard code slop」）。
- ユーザーは時間の扱いが全体として根から誤っていると言っている（「our timing is still off. the tolerances youve allowed are way too big.」「THIS IS NOT JUST ABOUT SOUND.. THIS IS ABOUT EVERYTHING」）。

## 洗い出す八つの型
(a) 決め打ちの数：固定の大きさの配列・表、#define の上限（MAX・COUNT・N など）、決め打ちの回数の輪。何の数を縛っているか。
(b) 時間の決め打ち：ms・µs・ns の定数（締め切り・待ち・周期・猶予・時間切れ）。何のための値か、その値が何を決めるか。
(c) ポーリング：周期で見回る thread や輪、sleep を挟んで状態を見直す輪、割り込みでなく見回りで装置を見る所、回数で回る spin。
(d) 一つの CPU・一つの thread への寄り：CPU 0 の判定、特定の CPU に縛る thread、装置の割り込みの宛先、系に一つの worker thread。
(e) 系に一つの lock・mutex（大域の lock）と、その下で行う重い仕事（記憶の確保、IPI・TLB の撃ち落とし、装置の待ち、全走査）。
(f) stub・空の実装・固定値を返す関数・名前だけの API・TODO・FIXME・未実装の分岐・「後で」の注釈。
(g) VM・hypervisor の判定や VM の装置への依存が kernel/src/vm/ の外にある所（vm_hosted() などの分岐、hv_・hyperv の名前）。
(h) 古い CPU・古い装置・後ろ向きの互換の道（8259 PIC、PIT、FXSAVE、xAPIC だけの道、レガシーの装置、Zen 4 より前の CPU のための分岐、CPUID の機能が無いときの代わりの道）。
加えて (i) 時間の単位と時計の口の一覧：カーネルの中で時刻を読む関数・時計の源（TSC・HPET・APIC timer・hypervisor の参照）・締め切りを受ける口と、その単位（ms・ns・拍）。単位の混在と変換の所を file:line で。

## 報告の形（日本語、平文）
型ごとに表：| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |。各行は確かめたものだけ。数が多い型は全部載せる（省略するなら「ほかに N か所」と書き、その grep の式を書く）。最後に、型を通して見えた全体の傾向を五行まで。

## reply

# Nocturne OS カーネル監査（読むだけ）

path はすべて `D:\Programes\llm vibe slop os\kernel\src\` からです。`common/` で始まるものは `D:\Programes\llm vibe slop os\common\` からです。どの行も、前後を読んで当てはまることを確かめてあります。直し方は書いていません。

## (a) 固定の数（配列・表・上限・固定の回数）

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| sys/sched.c:933 | `if (count >= 128)` で、アプリ一つの thread を 128 本で断る | 1024 CPU の機械で、一つのアプリが CPU の数だけ thread を持てない |
| sys/sched.c:161,198 | 実行キューの heap を「系の全 task 数」の大きさで全 CPU に取る（64 から倍に広げる） | 記憶が CPU 数 × task 数で増える。広げる間は capacity_lock の下で全キューを止める |
| sys/sched.c:40 | STEAL_PROBES 8：空いた CPU が覗く相手は 8 つだけ | 1024 CPU では仕事を見つけられない。node も LLC も見ない |
| sys/sched.c:79 | KSTACK_KEPT 2：CPU ごとに取っておく核の stack が 2 本 | thread が多く終わる負荷では毎回確保に戻る |
| sys/sched.c:1040-1041 | ADDRESS_BUCKETS 256 の番地待ちの表（address_waiters[256]） | 待ち手が増えても桶は増えない。起こしは全部を走査する（(e) を参照） |
| arch/idt.c:47-49 | irq_handlers[16][4]（ISA の 16 本 × 共有 4）、vec_handlers[256] | 5 つ目の共有は入らない。線は 16 本で頭打ち |
| arch/idt.c:132-134 | 装置のベクタの溜め 0x50–0xEF（160 本）が系に一つ。vec_owned[256] | IDT が CPU ごとでないので、全装置の MSI-X が合わせて 160 本で尽きる |
| dev/xhci.c:19 / dev/nvme.c:19 / net/rtl8125.c:50 | 型ごとに固定のベクタ（xHCI 0x40、NVMe 0x48–0x4B、RTL 0x4C） | 同じ型の二台目と、キューごとの割り込みを持てない |
| arch/ioapic.c:11,32,178 | IOAPIC_MAX 8、given[8][256]、saved_id[8] | IOAPIC が 8 個を超える板では、残りを使えない |
| mm/vmm.c:52 | PCID_SLOTS 8 | 9 個目の空間からは、CR3 の切り替えで TLB を捨てる |
| mm/vmm.c:50 | TLB_RANGE_MAX 32 | 32 頁を超える外しは、全消しの撃ち落としになる |
| mm/vmm.h:30,37-38 | 利用者の stack は 8 MiB まで。vmalloc の窓は 16 TiB 固定。頁表は 4 段だけ | 窓の大きさを機械の記憶でなく定数が決める。LA57 は使わない |
| mm/heap.c:47,50,51 / mm/pmm.c:33,35 | ORDER_MOST 3、CHAIN_MOST 64、CACHE_SHARE 1024、BATCH_MOST 64 | CPU 数で割るので規模では詰まらない。ただし割合は測った値でなく定数 |
| mm/heap.c:544 | 全 node の depot の表を cpu[0].home の記憶に置く | 2 ソケットでは、半分の CPU が depot の表を遠い node から読む |
| mm/vmalloc.c:46 | retired_most = 頁の総数 / 1024 | 撃ち落としを待つ番地の量が、定数の割合で決まる |
| kernel.h:60 | KLOG_SIZE 64 KiB の核の記録 | 1024 CPU の起動の行で、前の行を失う |
| dev/serial.c:18 | 送りの輪が 64 KiB | 書き込みが多いと記録を落とす |
| dev/blk.c:8 | MAX_BLK 16 | 17 台目のディスクを捨てる |
| fs/store.c:290 | `kept = i < 32`（looked は 32 bit） | 33 台目以降のディスクを見ない |
| dev/nvme.c:17-22 | MAX_CTRL 4・MAX_NS 8・admin 32・I/O 64。I/O の対は一つで、命令は一度に一つ。bounce は 128 KiB | CPU が増えても NVMe の I/O は一本で、深さは 1 |
| dev/ahci.c:18-22 | MAX_HBA 4・MAX_PORTS 32。slot 0 だけを使い、bounce を通す | NCQ を使わず一度に一命令。5 つ目の HBA は捨てる |
| dev/xhci.c:15,16,91,1442 | MAX_HC 8、輪 256 TRB、EP_XFERS 16、port_pending[8] | 9 つ目の制御器と、深い転送の行列を持てない |
| dev/usbhub.c:11 / dev/usbmsc.c:15 / dev/usbnet.c:25 | HUB_PORTS 15、MAX_LUNS 8、TX_SLOTS 16 | 固定の上限を越えた port・LUN・送りを持てない |
| dev/audio.c:17-21 / dev/audio.h:21 | 流れ 8、行列 170 ms、RUNS 32、null の塊 10 ms、AUDIO_RATE 48000 固定 | 9 つ目の流れを持てない。装置の rate に合わせない |
| dev/pciuser.c:39-41,87 | CLAIM_VECTORS 32、GUARDS 32、ACPI_NOTES 32、claim_of_vector[256] | 利用者のドライバーが 33 本目のベクタを持てない |
| dev/iommu_intel.c:131,133 / dev/iommu_amd.c:85,88 | IRT 4096、IOAPIC 256、AMD の輪 4096 bytes、IRT_LOG2 5（装置ごとに 32） | AMD では一装置の割り込みの付け替えが 32 本で頭打ち |
| dev/acpi.c:507 | LINKS_MAX 32 | PCI の link が 32 を超えると扱えない |
| dev/fb.c:40 / boot.c:125 | scanouts[3] を持つが、使うのは framebuffers[0] だけ | 画面は一つ |
| gui/wm.c:41-42,175,223,608,1494,1915,1927 | MAX_WIN 64、EVQ 128、damage 16 と 32、受け 16、wm_work_budget[4]（512×1280 の定数）、INQ 256、TRACE 256 | 65 個目の窓を作れない。合成の予算が定数 |
| gui/screenmode.c:35 | CHOICES_MAX 8。示すのは画面一つだけ | 二つ目の画面がない |
| net/net.c:56,231-233,413,517,588,1184,1191,1206 | 口は一つ（nif・link_mac）。受けは 64 × 1536 bytes。ARP 16、ping 8、UDP 8、TCP は 16 から、MSS 1460 固定、順外れ 8 | 二つ目の NIC と jumbo フレームを持てない |
| net/rtl8125.c:56,78,139-140 | 送り・受けとも queue 0 だけ。輪は一頁（256 記述子） | RSS がなく、受けは一 CPU |
| sys/events.c:230 / sys/channel.c:29,169 / sys/stream.c:123 | EVENTS_BATCH 64、CHANNEL_BUCKETS 64、MESSAGE_QUEUE_LIMIT 1 MiB、CONSOLE_INPUT 1024 | 桶が固定で、数が増えると鎖が延びる |
| sys/prof.c:24,174 | PROF_SLOTS、PIDS 64 | 65 個目のアプリは標本に出ない |
| sys/clock.c:26-28 | MEASURE_SPANS 5、CLOCK_REMEASURE_S 64、COMPARE_ROUNDS 256 | 時計の正しさを固定の回数と窓が決める |
| arch/smp.c:415-419 | 起動の試しの配列 257（value・sse・avx_ok・cpu） | 試しの大きさが CPU 数に関係ない |
| arch/smp.c:215-216 | smp_get_info が N_CPU_SSE2\|N_CPU_AVX の固定と、worker_cpus = online - 1 を返す | 実の CPUID を返さない。「一つは別」という古い形が残っている |
| arch/fpu.c:21 | USER_COMPONENTS 0xE7 固定 | CPU が持つ XSAVE の部品でなく、定数で決める |
| arch/pstate.c:56-59 | CPPC の方針が固定（max = highest、min = nominal、EPP 0） | 負荷や電力に合わせない |
| common/store/latch.h:17-20 | LATCH_READERS 0xFFFF、待ち手 15 bit | 読み手と待ち手の数が bit の幅で頭打ち |
| common/store/namespace.c:7 | READ_BLOCKS 64 | 一度に読む量が固定 |
| vm/hv/vmbus.c:106, vm/hv/hvsock.c:48,57, vm/virtio_gpu.c:21, vm/ata.c:27,29, vm/hv/storvsc.c:64, vm/hv/netvsc.c:44-46 | MAX_RELID 2048、listen 4、保留 16、QSIZE 8、disks[4]、channels[2]、bounce 16 頁、受けの予算 | VM だけの層の固定 |

ほかに 64 行あります（vm/ を除く。表に無い分はこの式で）：
`grep -rnE '^#define [A-Z_]*(MAX|MOST|COUNT|_N|SLOTS|ENTRIES|RING|QUEUE|BUCKETS)[A-Z_]* +[0-9]' kernel/src common/store`

## (b) 固定の時間（ms・µs・ns の定数）

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| sys/sched.c:39 | BALANCE_NS 4 ms の均しの間隔 | CPU の数に関係なく、偏りは 4 ms ごとにしか直らない |
| sys/sched.c:41-42,655 | 切り替えの費用を 10 µs と見積もった定数（KEI_SWITCH_COST_NS・KEI_OVERHEAD_PPM）から slice を出し、0.5–4 ms に挟む | slice は測った値でなく見積もりで、CPU の速さにも task 数にも合わない |
| sys/sched.c:43 | KILLED_NAP_NS 10 ms | 殺された thread の終わりが 10 ms 刻みになる |
| arch/apic.c:339 | WAKE_LATE_SHIFT 13：timer の速さをわざと 1/8192 高く取る | すべての期限が遅れる側へずれる。誤差を設計で入れている |
| arch/apic.c:350 | LAPIC timer の速さを 20 ms の窓で測る | 起こしの精度が固定の窓で決まる |
| arch/smp.c:24-25,95 | AP の起動・停止の待ち 2000 ms ずつ、panic の NMI の待ち 100 ms | 上限が CPU の数や状態に関係ない |
| arch/cpu.h:231-232 | INIT の後 10 ms、STARTUP の後 200 µs | 古い MP 手順の待ちを固定で持つ |
| arch/pstate.c:83 | 5 ms の spin | 起動の道で CPU を 5 ms 塞ぐ |
| mm/vmm.c:51 | TLB_SLOW_NS 5 s（遅い撃ち落としの報せ） | 報せの線が固定 |
| sys/clock.c:169,182,225 | 測りの窓 10 ms、1 s の alarm | 時計の正しさが固定の窓で決まる |
| dev/serial.c:19 | CHAR_NS 86806（115200 baud を前提） | baud が変わると送りの時間がずれる |
| dev/fb.c:208 | DRAIN_POLL 100 µs | 表示の吐き出しを 100 µs 刻みで見る |
| gui/wm.c:117,127,208,213,1496,1933 | 拍が無いとき 60 Hz、VBLANK_LOST 100 ms、二度押し 450 ms、toast 4 s、JOIN_SHRINK 1 ms、trace 20 s | 画面の拍を見失うと 60 Hz の決め打ちに戻る |
| gui/wm.c:2484 | 周期 = NS_PER_S*1000/mHz を整数で割る | 割り切れない rate では、コマごとに切り捨ての誤差がたまる |
| gui/wm.c:2584 | 分の拍 | 時計の表示を周期の拍で回す |
| gui/screenmode.c:36 / common/display_abi.h:64 | 拍が無いとき 60 Hz、SCREEN_MODE_TRIAL_MS 15000 | 固定 |
| dev/usbhid.c:15-16 | 鍵の繰り返し 500 ms / 33 ms | 固定 |
| dev/xhci.c:467,506,511,581,1056,1109,1114,1130,1153,1193,1275,1335,1343,1353,1367,1450,1497-1498,1519 | 1・10 ms の見、3 s、5 s、2 s、100 ms<<試した回数、500 ms、10 ms、100 ms、退き、50・100 ms、IMOD 250 µs、20 ms、settle 500 ms、50 ms・4 ms・1 s、20 ms | USB の手順の全部が固定の待ちで進む |
| dev/usbhub.c:96,175-176,213-229 / dev/usbmsc.c:134 | power_ms の床 100 ms、500 ms・10 ms、250 ms | 固定 |
| dev/nvme.c:23-24,168,368 | ADMIN 5 s・I/O 30 s、10 ms・1 ms の見直し、CAP.TO | 終わりを固定の刻みで見る |
| dev/ahci.c:21-25,180,280,401-404 | SPINUP 50 ms、LINK 1 s、READY 10 s、STOP 500 ms、I/O 30 s、2 ms、25 ms・2 s | 固定 |
| dev/pci.c:695,697,717-718,1044-1046,1071,1191-1203 | COMPLETION_FLOOR_MS 1000、top_ms の表、FLR 100 ms・READY 1 s、100 ms、4 ms | 固定 |
| dev/pm.c:28-31,92,99,113 | GRACE 5 s、STALL 15 s、FORCE 60 s、STOP 3 s、1 s・3 s、50 ms、20 ms | 眠りの手順が固定の窓で決まる |
| dev/acpi.c:261 / dev/input.c:178 / dev/audio.c:641 | FROZEN_WAIT 2 s、PS2_WAIT 100 ms、give_up 1 s | 固定 |
| dev/iommu_intel.c:430,451,544,646,1001,1009,1039,1094 / dev/iommu_amd.c:433,495,593 | 10–100 ms の spin の上限 | 割り込みを閉じたまま、最大 100 ms CPU を塞ぐ |
| fs/store.c:17-19,309 / sys/keep.h:22 | WRITE_DELAY 5 s、FIND 10 s、FIND_MOST 60 s、100 ms、KEEP_AFTER 500 ms | 書きの遅らせが固定 |
| net/net.c:113,414-415,518,844,855,973,1210-1218,1287,2375-2377 | 50 ms、ARP 600 s・1 s、ping 1 s、DHCP、DNS 2 s、RTO 1 s・200 ms・16 s、TIME_WAIT 2 s、LINGER 20 s、CONNECT 15 s、FAULT_HOLD 200 ms、窓 2 ms・番の待ち 2 ms | 通信の時間がすべて定数 |
| net/rtl8125.c:196-298,547 | µs・ms の spin が多数 | 固定 |
| sys/syscall.c:155 / sys/prof.c:25,29 | 報せの間隔 10 s、標本 1 ms、窓 10 s | 固定 |
| vm/rdp.c:1163,1241-1243,1604,1702-1703,1716-1742, vm/hv/vmbus.c:335, vm/hv/hv_util.c:83, vm/hv/netvsc.c:46-47, vm/hv/hvsock.c:24, vm/hv/storvsc.c:147, vm/virtio_gpu.c:109,164, vm/ac97.c:217,229, vm/tulip.c:121-123, vm/e1000.c:47-48,205-207 | VM の層の固定の時間 | VM だけの層 |

ほかに 166 行（vm/ を除く）と、vm/ に 30 行あります：
`grep -rnE '[0-9]+ \* NS_PER_(MS|US|S)|NS_PER_(MS|US|S)\)' kernel/src`

## (c) 見回り（周期の thread、眠って見直す輪、回数で止める spin）

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| dev/xhci.c:461-476 | hc_wait が 1 ms か 10 ms ごとに見る | 終わりの知らせが最大 10 ms 遅れる |
| dev/xhci.c:1447-1507 | usb_thread が 1 s・4 ms・50 ms ごとに見回り、usbhid_tick も回す | 割り込みがあっても周期で起きる |
| dev/xhci.c:1465-1470 | MSI が来なければ見回りに落ちる | 割り込みを失っても見回りが隠す |
| dev/xhci.c:512-515 | 1 ms の見回り | 固定 |
| dev/nvme.c:168 / dev/ahci.c:280 | 完了を 10 ms ごと（ベクタが無いと 1 ms）に見直す | 割り込みを失っても 10 ms で拾う作りで、遅れが隠れる |
| dev/nvme.c:180 / dev/ahci.c:139,402 | wait_ready・wait_reg が 1 ms 眠って見直す | 固定の刻み |
| dev/pci.c:163-165,1071,1191-1203,1340 | wait_step（1 ms 眠るか pause） | 固定の刻み |
| dev/fb.c:210 | presents_drain が 100 µs ごとに見る | 表示の終わりを見回りで待つ |
| dev/pm.c:99,113 | 窓とアプリを 50 ms・20 ms ごとに見る | 眠りの手順が見回り |
| fs/store.c:309 | ディスクを 100 ms ごとに探す | 見回り |
| dev/usbhub.c:175-176 | 10 ms ごと | 見回り |
| dev/serial.c:87,96 | UART を alarm で、割り込みを閉じて吐く | 送りが割り込みでなく alarm で回る |
| dev/serial.c:111 | serial_flush が 100000 回回る | 回数で止める |
| net/net.c:113,2416 | 50 ms の見直し、NET_TURN_WAIT | 通信の thread が時間で起きる |
| sys/clock.c:158,189,309-316,364 | PM timer の探り 100000 回、PIT の spin 5000 万回、比べの spin、CMOS の UIP の spin | 回数で止める |
| arch/apic.c:274-277 | xapic_ipi_ready が 100 万回回る | 回数で止める |
| arch/smp.c:191-194,296,314-317,361-364 | cpu_parallel_for の呼ぶ側が割り込みを閉じて spin。起動・停止の spin | 割り込みを閉じて待つ |
| sys/sched.c:579-590 / sys/alarm.c:125-128 | off_cpu_wait・alarm_cancel の spin | 相手の CPU を spin で待つ |
| dev/acpi.c:296,352 | frozen の semaphore の spin、sci_wait_idle の spin | spin で待つ |
| dev/input.c:179-185 / dev/power.c:96,101 | PS/2 の状態の spin、100000 × io_wait | 回数で止める |
| mm/vmm.c:303-310 | 撃ち落としの送り手が spin で待つ | 1024 CPU の答えを spin で待つ |
| arch/spin.h:26-29 / mm/vmm.c:224-238 | spin の一回ごとに vmm_tlb_poll が cpuset の語を全部回る | spin の費用が CPU の数に比例する |
| common/store/latch.h:24-46 | latch が純粋な spin の読み書き lock | 待ちが眠らない |
| sys/events.c:246-268 | events_wait が timer と watch を全部、線形に見る（level で判定） | 待つ物の数に比例する |
| vm/ata.c:50-67, vm/tulip.c:88, vm/e1000.c:98, vm/virtio_gpu.c:113-114,167, vm/hv/vmbus.c:146,339, vm/rdp.c:1738 | 200 万回・100 万回の spin、30 ms・10 ms の見回り | VM だけの層 |

ほかに 70 行あります（vm/ を除く）：
`grep -rnE 'pause\(\)|sleep_until\(clock_after' kernel/src`

## (d) 一つの CPU・一つの thread への偏り

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| dev/pci.c:200 | `interrupt_dest = lapic_current_id();` で、全 MSI が BSP に向く | 装置の割り込みを全部 CPU 0 が受ける |
| dev/pci.c:495-501 | pci_msi_enable が宛先 > 0xFF を断り、MSI-X は項目 0 だけ使う。MSI-X より MSI を先に選ぶ | キューごと・CPU ごとの割り込みを持てない |
| dev/pciuser.c:1079 | `dest = pci_interrupt_dest(); /* CPU 0 … */` | 利用者のドライバーの割り込みも CPU 0 |
| dev/iommu_intel.c:186 | fault_cpu が CPU 0 | 固定 |
| dev/xhci.c:478 / dev/nvme.c:67 / dev/acpi.c:334 | 装置の ISR が CPU 0 で走る | 1024 CPU でも I/O の完了は一つの CPU |
| dev/pm.c:349 | `kthread_create_on("power", pm_thread, NULL, 0)` | 眠りが CPU 0 に縛られる |
| arch/smp.c:345,380,349-369 | smp_suspend・smp_resume が CPU 0 を ASSERT し、CPU を一つずつ止める | 1024 CPU を一列に止める |
| sys/sched.c:549-563 | sched_cpu_leave が task を全部 CPU 0 へ渡す | 偏りが CPU 0 に集まる |
| sys/alarm.c:159,168-186 | alarm_cpu_leave が alarm を CPU 0 へ移す | 同上 |
| arch/idt.c:330 / arch/apic.c:123-126 | 8259 の線は CPU 0 だけに届く。LINT0 ExtINT・LINT1 NMI を BSP だけに置く | 古い線が CPU 0 に縛られる |
| sys/sched.c:670, mm/vmalloc.c:239, dev/acpi.c:478, dev/xhci.c:1562, net/net.c:2472,2512, fs/store.c:440, gui/wm.c:2886, sys/keep.c:331, sys/prof.c:280, sys/stream.c:115, dev/pciuser.c:204, gui/screenmode.c:419 | 系に一つの thread（reaper、vmalloc の掃除、ACPI、USB、net、store の書き手、合成、keep、prof、記録、画面の保持、画面の型） | その仕事は CPU が増えても一本 |
| vm/ の hvsock・hv-util・rdp・rdpsnd | VM の層の単一の thread | VM だけの層 |
| arch/smp.c:134-141,187-188 | cpu_parallel_for は系に一つの束だけを許し、空いた CPU を全部起こす | 並列の仕事が同時に一つ |
| sys/sched.c:266-275,306-318,332-348 | busiest・choose・rebalance が全 CPU を走査し、NUMA を見ない | 1024 CPU では走査が重く、node をまたいで移す |

## (e) 系に一つの lock と、その下の重い仕事

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| sys/sched.c:30 | task_lock と task_list の線形の鎖 | 全 task の操作が一つの lock |
| sys/sched.c:930-932,970,1256,1282,1337,1389,1421,1437,1452 | task_lock の下で task_list を線形に走査する | task 数に比例する |
| sys/syscall.c:160-185 | report_memory_exhaustion：task_lock の下で O(n²) の走査、頁表の歩き、kprintf | 記憶が切れたとき系が止まる |
| sys/syscall.c:211-232 | sys_proclist：task_lock の下で各実行キューの lock を取り、頁表を歩く | 一覧一つで全 CPU を止める |
| sys/sched.c:65,170-183 | capacity_lock の下で、全キューの lock を取って heap を広げる | 広げる間、全 CPU が待つ |
| sys/sched.c:766,819-820 | deadline_wait：sleep_until 全部が一つの待ち行列。wq_take_blocked が線形に歩く | 眠る thread の数に比例する |
| sys/sched.c:1032,1049,1112-1143,1190,1240 | address_lock と address_wait が系に一つ。address_wake が待ち手と watch を毎回全部走査 | futex 相当が一つの lock で、全走査 |
| mm/vmm.c:473,498,530,557 | kernel_space.lock が核の半分全部を守り、その下で頁表を確保する | 核の写像が一列 |
| mm/vmm.c:279 | 核の半分の撃ち落としは全 online CPU が宛先で、送り手は spin | CPU の数に比例する |
| mm/vmalloc.c:34,196-201 | vmalloc の空間全体で一つの lock。vunmap がすぐ全 CPU へ撃つ | 確保と解放が一列で、撃ち落としが全 CPU |
| mm/pmm.c:355-376 | 盗みと drain_all が全 CPU を歩く | CPU の数に比例する |
| sys/events.c:31 / sys/object.c:50 | watchers_lock が系に一つで、handle の最後の close ごとに取る | close が一列 |
| sys/syscall.c:347,439-458 | shared_lock が系に一つ（使う所は 147）。shared_withdraw が shared_all を全部走査し、その下で vmm を呼ぶ | 共有記憶の操作が一列 |
| lib/printf.c:138 / dev/serial.c:26 / dev/fbcon.c:13,34-36,242-250 | kprintf ごとに klog・serial・fbcon の三つの lock を、割り込みを閉じて取る。fbcon_write は lock の下で fb_present | 記録の一行が画面の書き出しを待ち、全 CPU が並ぶ |
| gui/wm.c:162,2550-2612 | wm_mutex が入力・composite()・窓の状態の全部を囲む | 入力と合成が一列 |
| net/net.c:37,2433-2436 | net_lock の mutex が stack 全体に一つ。束ごとに全 tcb を走査 | 接続の数に比例し、一列 |
| dev/xhci.c:186 | usb_lock が全制御器に一つ | 一列 |
| dev/blk.c:36,40-56 | gate_lock を I/O ごとに取る | 全ディスクの I/O が一つの lock |
| dev/iommu_amd.c:161,581-603 / dev/iommu_intel.c:194,636-660 | 系に一つの spinlock。その下で、割り込みを閉じたまま unit ごとに最大 100 ms spin | DMA の写像の変更が一列で、CPU を塞ぐ |
| dev/audio.c:57,322-327,342-347 | audio_lock。null の受け手は alarm の割り込みの中で混ぜる | 割り込みの中で重い仕事 |
| dev/pciuser.c:81,86,248 | claim_mutex（claim は一度に一つ）、claim_irq_lock、screen_lock | 一列 |
| fs/store.c:445-480 | store.gate の spinlock が系の全変更を数える。store.committing は一つの mutex | 書きが一列 |
| common/store/catalog.c:357 | 巻の全体で一つの木の latch | 目録の操作が一列 |
| dev/input.c:18 | input_lock の下で受け手を呼ぶ | 受け手が遅いと入力が止まる |
| arch/spin.h | test-and-set の spinlock（行列にしない） | 1024 CPU で cache の線を取り合い、公平でない |
| arch/idt.c:52 / arch/ioapic.c:37 / dev/acpi.c:391 / dev/pci.c:50,611,757 | vectors_lock、ioapic_lock、work_lock、cf8_lock、reset_lock、reset_mutex | 系に一つ |

ほかに 47 行あります（vm/ を除く。表に無い分はこの式で）：
`grep -rnE '^static struct (spinlock|mutex) ' kernel/src common/store`

## (f) 空の実装・固定の値を返す関数・名だけの API・「まだ」

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| sys/kei.c:98,117,165 | kei_set_online・kei_charge_detached（注釈は前の BSP の設計のまま）・kei_set_bonus を呼ぶ所が無い | 名だけの API |
| sys/sched.c:208,698 | bonus_cap 0、全 task が KEI_WEIGHT_ONE | 優先度が無い |
| arch/cpu.c:52-57 / arch/cpu.h:91 | MWAIT・INVLPGB・invlpgb_pages を調べるだけで使わない | Zen 4 の撃ち落としの命令を使わず IPI に頼る |
| dev/acpi.c:186 | `if (a.segment) return UACPI_STATUS_UNIMPLEMENTED;` | PCI segment 0 だけ |
| dev/acpi.c:357 | SCI の irq ≥ 16 は UNIMPLEMENTED | 板によっては ACPI の事象が来ない |
| dev/acpi.c:383 | 「まだ thread をそこに留める道が無い」の注釈（kthread_create_on はある） | 注釈が古い |
| dev/pci.c:15,33,207 | segment 0 だけ | 2 ソケットの多い segment を見ない |
| dev/audio.c:769,738 | `vm_audio_init(); /* no driver for real sound hardware yet */`、ABI に時刻の欄が「まだ」無い | 実機の音が無い |
| dev/fb.c:437 | 実機の表示を眠りから戻す処理が無く、眠りを出さない | 実機で眠れない |
| dev/pm.c:251 | 利用者の PCI ドライバーに眠りを「まだ」知らせない | 同上 |
| dev/xhci.c:714 | isochronous が「まだ」無い | USB の音・映像が無い |
| net/rtl8125.c:556 / net/net.c:2511 | RTL8125B だけ。ほかの NIC は無い | 実機の NIC が一種 |
| dev/input.c:318 | 鍵の番号に「まだ」余地が無い | 同上 |
| arch/topology.c:193 | CPU の hot-add が無い | 同上 |
| sys/clock.c:14 | HPET は「使える」と書くだけで、実装が無い | 同上 |
| arch/smp.c:215-216 | smp_get_info が固定の値を返す | (a) と同じ |
| dev/pciuser.c:1084 | アプリの legacy INTx が無い | 同上 |

## (g) vm/ の外にある VM の判定と、VM の装置への依存

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| boot.c:128 / init.c:89,98,110 | vm_detect、vm_init、vm_storage_init、vm_session_init を共通の起動から呼ぶ | 実機の起動の道に VM の段がある |
| arch/apic.c:366 | vm_lapic_hz | 実機の timer の速さに VM の値が入る道 |
| sys/clock.c:3-12,45,92-94,121-125,389-397,433,440 | hv_base、SOURCE_HYPERVISOR、clock_hypervisor_at、vm_clock_ns、vm_tsc_hz、vm_clock_resume | 核の時計の中心に hypervisor の参照が組み込まれている |
| sys/clock.h:7-9,44-46 | 時計の公開の面に VM の源がある | 同上 |
| arch/pstate.c:40,44,74,94,105 | vm_hosted で分ける | 実機の電力の道に VM の判定 |
| dev/fb.c:146,391-393,105,446 | 「WHPX does not emulate SIMD MMIO」、WHPX の測りで並列の present を既定で切る、vm_display_pm | VM で測った値が実機の既定を決めている |
| arch/cpu.h:14-18 / arch/fpu.c:4-6 / arch/cpu.c:5 | WHPX の emulator のための MMIO の書き方、WHPX への回避、「WHPX には PCID・MWAIT・不変 TSC が無い」 | 実機のコードの形を VM が決めている |
| dev/audio.c:769 / dev/hibernate.c:146,637 / dev/pm.c:220 / dev/power.c:89 / net/net.c:2506 / dev/input.c:206 | vm_audio_init、hibernate の VM の道、vm_resume、vm_power_off、vm_net_probe、vm_ps2_flips_y | 共通のコードが VM の入口を直に呼ぶ |
| sys/syscall.c:2272-2290,2904-2918 / sys/sched.h:50 | clock_page_of・vm_clock_page、SYS_CLOCK_INFO の N_CLOCK_HV_REFERENCE・reference_sample_100ns、task の clock_page の欄 | ABI に hypervisor の時計が出ている |
| arch/apic.c:3-4 / gui/wm.c:2352 / dev/input.c:290 | Hyper-V Gen 2、Hyper-V の tablet pointer の注釈 | 注釈だけ |

## (h) 古い道・後方互換・CPUID の欠けへの退き道

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| arch/idt.c:175-233,330-343 / boot.c:148 | 8259 PIC（pic_init、ELCR）とその振り分け | 線が 16 本で CPU 0 だけ |
| arch/apic.c:1-2,123-126 | virtual wire（LINT0 ExtINT） | 同上 |
| dev/acpi.c:361-362 / dev/input.c:298-302 / net/rtl8125.c:609-612 | SCI・PS/2（i8042、IRQ 1・12・2）・RTL の INTx の退き道が PIC に乗る | 同上 |
| dev/pm.c:203-218 / dev/hibernate.c:630 | PIC を保存して戻す、PIC を閉じる | 同上 |
| sys/clock.c:141-194,363-384 | PIT の測り、ACPI PM timer（port I/O）、CMOS RTC | 時計の速さを古い装置で決める |
| dev/serial.c | COM1 0x3F8 | 固定の port |
| dev/pci.c:50-88 | CF8・CFC の設定の読み書き | ECAM でない道と、それ用の lock |
| arch/cpu.h:12 | io_wait が port 0x80 | 古い待ち |
| dev/power.c:101-109 | i8042 の 0xFE の reset と triple fault | 古い再起動 |
| arch/apic.c:48-54,82-98,143-154,195-201,273-279 | xAPIC の MMIO の道、AMD の 4 bit の ID、x2apic_leave、ICR の busy-poll | Zen 4 では要らない道 |
| sys/clock.c:13,396-407,413 | 不変 TSC が無いときの道、CPUID 15h・PIT の道 | Zen 4 では要らない |
| arch/apic.c:361-376 | TSC deadline が無いときの one-shot の数 | 同上 |
| mm/vmm.c:621 | PCID が無いときの道（nopcid） | 同上 |
| arch/pstate.c:63-71 / arch/apic.c:167-179 | Intel の HWP・CMCI・TME の枝 | 床は Zen 4 なのに Intel の道 |
| arch/topology.c:38-44 | 0xB の葉への退き道 | 同上 |
| dev/pci.c:501 | MSI-X より MSI を選ぶ | 同上 |
| vm/ata.c, vm/vbe.c, vm/tulip.c, vm/e1000.c, vm/ac97.c | IDE の PIO、Bochs VBE、古い NIC と音 | VM だけの層 |

## (i) 時計の入口と単位

| file:line | 何か（一行） | 規模・正しさで何が詰まるか（一行） |
|---|---|---|
| sys/clock.c:88 | clock_now_ns：ns。TSC × mult / 2^32、または hypervisor の参照 | 源が二つで、VM の源が中心にある |
| sys/clock.c:101 | clock_tsc_at：ns から TSC の数へ（TSC deadline に使う） | 変換が二度入る |
| sys/clock.c:121 | clock_hypervisor_at | VM の源 |
| sys/clock.h:32-40 | clock_after・clock_spin、clock_tsc_hz | ns |
| sys/clock.c:363-384 | clock_wall_ns・clock_wall_seconds（ns と s）、rtc_read_unix（s）、time_to_parts | 壁の時計は CMOS の秒から |
| arch/apic.c:394 | lapic_wake_at：ns から TSC か、LAPIC の数へ（wake_mult = Hz / 2^32） | 源ごとに道が違う |
| arch/apic.c:339,350 | lapic_wake_init が 20 ms で測り、わざと 1/8192 遅らせる | 起こしの誤差を設計で入れている |
| sys/alarm.c | alarm_set・alarm_interrupt：ns | — |
| sys/sched.c | sleep_until、wq_wait_until、wq_wait_unless、wq_wait_locked_until：ns の期限 | — |
| sys/syscall.h:11-17 | syscall_deadline(int64 timeout_ms)、address_deadline：ms を ns に換える | ABI の待ちが ms の粒 |
| sys/syscall.c:2845,2831,2667,1337,788,2882-2884 / gui/wm.c:3339 | SYS_SLEEP、events_wait、app_result、pager_take、pci_screen_state、pci_user_interrupt_take・acpi_notify、win_receive が ms で受ける | 1 ms より細かく待てない |
| net/net.c:2613,2658,2683 | `(int)d` で期限を 32 bit に切る | 長い期限が化ける |
| sys/events.c:178 | events_timer だけが ns で受ける | ABI の中で単位が揃わない |
| sys/syscall.c:226,228,246,2906 | uptime_ms、cpu_ms、start_ms：ms で出す | 出力は ms に切り捨てる |
| gui/screenmode.c:382 | trial_ms | ms |
| sys/syscall.c:2904-2918 | SYS_CLOCK_INFO が uptime_ms（ms に切り捨て）、reference_sample_100ns（100 ns）、tsc_sample（TSC）を混ぜる | 一つの答えに三つの単位 |
| dev/xhci.c:822,838,1335 / dev/usbhub.c:96,129 | usb_bulk の timeout_ms、power_ms、IMOD の単位 | ms と装置の単位 |
| dev/pci.c:695-707 | top_ms、COMPLETION_FLOOR_MS | ms |
| dev/acpi.c:253-280 | uacpi_kernel_stall（µs）、uacpi_kernel_sleep（ms）、semaphore の u16 の ms | firmware の µs・ms |
| vm/hv/vmbus.h:125 / vm/hv/vmbus.c:304-305 | vmbus_wait(ms) | ms |
| arch/smp.c:207,295,360 | cpu_max_job_us、START・PARK_TIMEOUT_MS × NS_PER_MS | µs・ms から ns へ |
| sys/prof.c:259 | period_ms | ms |
| dev/audio.c:315-317 | frame と ns を 48 kHz 固定で換える | 装置の rate に合わない |
| gui/wm.c:2484 | 周期を mHz から整数の割り算で出す | 切り捨ての誤差 |
| dev/serial.c:19 | CHAR_NS × fifo_depth | baud が固定 |
| vm/e1000.c:47-48 | ITR を 256 ns の単位で | VM |
| dev/nvme.c:368,422 | CAP.TO（500 ms の単位）、µs | 装置の単位 |
| — | HPET は使わない（sys/clock.c:14 は注釈だけ） | 源の選び肢が無い |

## 全体の傾向（五行）

1. 装置の割り込みは全部 CPU 0 に来ます。subsystem はそれぞれ「一つの thread と一つの lock」の形です（net、wm、usb、store、iommu、記録、task_list、address_wait、shared）。CPU の数が増えても、仕事の道は一本のままです。
2. 固定の配列と上限が、装置・窓・流れ・ベクタ・IOAPIC・NIC・ディスク・thread の数を区切っています。理由は測りでなく定数です。
3. 待ちの多くは、割り込みを待つ形でなく、ms の固定の周期で見直す見回りや、回数で止める spin です。見回りが割り込みを失ったときの後ろ盾も兼ねていて、遅れが表に出ずに隠れます。
4. 時間の単位が揃っていません。ABI は ms、内部は ns、firmware は µs と ms、ほかに 100 ns と TSC の数が混ざります。timer はわざと遅らせてあり、HPET はなく、hypervisor の時計が核の時計の中心に入っています。
5. PIC・PIT・PM timer・CMOS・i8042・CF8・COM1・xAPIC の古い道と、VM（WHPX・Hyper-V）で測った既定が、共通のコードに残っています。
