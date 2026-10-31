# カーネルの建て直し：設計（2026-10-31）

監査（`D:\Programes\` の六つの txt と `docs/kernel-audit-20261030/07-timing-check.md`）の判定に従い、持ち主の単位で消して建て直す。この文書は、建て直しの順と、各部品の責務・口・継ぎ目を決める。書き手（サブエージェント）はこの文書の自分の節と「全員の決まり」を契約として書く。契約を変える必要が出たら、書く前に報告で知らせる。

## 1. 前提

- 的は実機：下限 Zen 4（Ryzen 5 7600X）、上は 2 ソケットの EPYC（論理 CPU 1024、NUMA、CXL）。x86-64-v4・x2APIC・不変 TSC・PCID・INVPCID・SMEP・SMAP・FSGSBASE・XSAVES を前提にしてよい。
- QEMU（WHPX）と Hyper-V 第 2 世代（拡張セッション）は試験の道具で、`kernel/src/vm/` は残る。vm/ の装置は共通の口に登録する driver であり、共通コードは hypervisor を知らない。hypervisor が隠す機能（不変 TSC の印、PCID、MWAIT など）は「無ければ使わない」で扱い、無いことを理由に拒まない。
- 古い道（8259 の配り、xAPIC、PIT、CF8、不変 TSC が無いときの道、PCID が無いときの道）は消す。
- 時間の正しさは土台の性質であって、部品ごとの許容の定数で埋めない。どの層も誤差と遅れの上限を設計で持つ。

## 2. 波（依存の順）

| 波 | 部品 | 依存 |
|---|---|---|
| 0 | 下準備：`cpu.h` の寄せ集めの口を持ち主の見出しへ、LAPIC の timer を `arch/lapic_timer.c` へ（動きは変えない） | — |
| 1 | A1：CPU の状態と錠と横取りの数、CPU の起こし・止め、idle と性能の方針、起動の順。A2：割り込みの層。B：時間（核の側）。D：記録 | 0 |
| 2 | sched（横取りできる核、級、待ちの部品、mutex・rwsem・futex、task の一生）、object・handle・events・channel | 1 |
| 3 | mm（pmm・vmm の空間の記述・頁違反・vmalloc・heap の大きな物）、syscall の分割、時間の ABI（ns・絶対期限・時計の頁・時刻の印）と libc | 2 |
| 4 | 装置の模型（bus と driver の照合、装置木、電源の順）、PCI、IOMMU、block の層、NVMe、USB（xHCI・HID・大容量記憶）、入力の層、音（HDA）、表示の部分系、pm・休止、乱数 | 3 |
| 5 | 格納庫（fs・common/store）、網（net）、窓（wm とシェルの分離） | 4 |

各波の終わりに、本体の作業木へ一つずつ取り込み、組み、QEMU（窓付き一台）で起動して確かめ、コミットする。波の中の書き手は互いのファイルを書かない（下の各節の「持つファイル」）。

## 3. 全員の決まり

- Linux・BSD・NT ほか OS のソースを読まない・写さない。GPLv2 だけのコードを入れない。照らすのは `D:\Programes\OS DEV INFO` の仕様（SDM `325462-093`、AMD APM `24593`/`40332`/vol2、PPR `55898`、ACPI 6.5、PCIe、NVMe、xHCI、HDA など）と公開の論文。仕様に無いことは「未確認」と書く。
- 持ち主の単位で消してから建てる。古い道を残したまま新しい道を足さない。shim（何もしない関数、渡すだけの包み、偽の環境）を作らない。
- 決め打ちの上限を置かない。表の大きさは実際の数（CPU・node・IOAPIC・装置）から。ハードウェアが決める数（x86 の vector 256、MSR の幅）は定数でよく、その出典を注記に書く。
- 時間の定数を置くなら、仕様の値（出典を注記）か、測った値を使う。「許容」を足して誤差を隠さない。
- 待ちは割り込みか事象で起こされる形にする。決まった間隔で見に行く輪を作らない。回数で止める spin を作らない。
- 錠の下で眠らない・写さない。割り込みを閉じる区間は短く、上限が言える形にする。
- コードの注記は周りに合わせて英語。コミットのメッセージと報告は日本語。
- 作業木の中で `python -X utf8 scripts/build.py -j 8 kernel` が通ること（出力は本体の `build/wt/<作業木の名>/`）。QEMU は起こさない（本体の会話が取り込み後に起動して確かめる）。
- 自分の作業木の枝にコミットしてよい（「段 1-A1：…」のような日本語の題）。main へは取り込まない。

## 4. 波 1 の契約

### 4.1 A1：CPU の状態・錠・横取りの数・CPU の起こしと止め・idle・起動の順

持つファイル：`arch/cpu.c`・`cpu.h`、`arch/spin.h`、新しい `arch/preempt.h`、`arch/smp.c`・`smp.h`・`smp.asm`、`arch/wake.c`・`wake.asm`・`wake.h`、`arch/pstate.c`・`pstate.h`（消して idle と性能の持ち主を建てる）、`arch/pat.c`、`arch/fpu.c`・`fpu.h`、`arch/gdt.c`・`gdt.h`、`kernel/src/boot.c`・`init.c`。

錠（`arch/spin.h`）：
- 口の名と意味は今のまま：`spin_lock(l)`→flags（割り込みを閉じる）／`spin_unlock(l, flags)`、`spin_take`／`spin_give`（割り込みは既に閉じている）、`spin_held`、`spin_seize`（panic）。呼ぶ側は変えない。
- 中身は列に並ぶ公平な錠（MCS の待ち行列。待ち手は自分の node の語の上で回る）。CPU ごとの node は入れ子の深さぶん持つ（割り込みを閉じて待つので、入れ子は NMI だけ。余裕を持たせて 4）。同じ CPU が持っている錠を取ったら止める検査は残す。
- 待ちの輪の中で `vmm_tlb_poll` を呼ばない。撃ち落としの約束は「送り手は割り込みを開き、spinlock を持たずに待つ」（今の `shoot` の ASSERT のとおり）なので、錠の待ち手が答えなくても進む。`spins_held` は残す（撃ち落としの ASSERT が読む）。
- 錠の順の検査：`struct spinlock` に `rank`（0 は検査しない）。CPU ごとに持っている rank の列を持ち、持っている最上位の rank 以下の錠を取ったら panic。波 1 の書き手は自分の錠に rank を付ける。rank の表は `arch/spin.h` の一か所に名前つきで置く。

横取りの数（`arch/preempt.h`）：
- `extern PERCPU uint32_t preempt_count;` 下位 16 bit が横取りを止める深さ（`preempt_disable`、spinlock を持つ間）、bit 16〜23 が割り込みの処理の深さ（今の `irq_nesting` の代わり。A2 が `isr.asm` と `isr_interrupt` で増減する）、bit 24〜27 が NMI の深さ。
- `preempt_disable()`／`preempt_enable()`、`in_interrupt()`、`preemptible()`（数が 0 で割り込みが開いている）。`spin_take` は数を一つ増やし、`spin_give` は減らす。
- 波 1 では数を正しく保つだけで、横取りそのもの（`preempt_enable` と割り込みの出口から scheduler を呼ぶこと）は波 2 の sched が入れる。

CPU の状態を作る手順（`arch/cpu.c`）：
- 一つの手順 `cpu_setup()` を、起動の CPU 0、AP、S3 の後の全 CPU が同じように通る。保存した CR・MSR の写しを書き戻す道（`wake.c` の写しの表）は消す。
- 手順が立てるもの：CR0（WP・NE・MP、CD・NW は落とす）、CR4（PGE・PCIDE・FSGSBASE・SMEP・SMAP・UMIP・OSXSAVE・OSFXSR・OSXMMEXCPT・MCE）、EFER（SCE・NXE、CPUID 8000_0021h EAX の AutoIBRS があれば AIBRSE）、PAT、XCR0 と XSS（`fpu`）、TSC_AUX と GS（`percpu_load`）、SYSCALL の MSR（A2 の `entry_load` を呼ぶ）。
- AMD では LFENCE を命令の直列化にする（APM：DE_CFG MSR C001_1029 の bit 1）。B の時計はこれを前提に `lfence; rdtsc` で読む。立てられなかったら起動の行に書く。
- 機械検査（MCA）：MCG_CAP から bank の数を読み（決め打ちしない）、全 bank の CTL を開き、起動時の STATUS を記録してから消す。`#MC` は bank を読んで記録し、訂正済みなら続け、訂正できない誤りは panic。AMD の threshold の割り込み（LVT）は A2 の vector の口で受け、決まった間隔で見に行かない。NMI は panic で止める道（他の CPU の panic）と、それ以外（記録して戻る）を分ける。
- 投機の対策：APM と PPR に書かれている範囲で立てる（AutoIBRS、アプリの空間を替えるときの IBPB を `cpu_ibpb()` として mm に出す）。手元に資料の無い対策（SRSO など）は未確認として報告に書く。
- MTRR：全 CPU が同じであることを確かめ、違うときだけ SDM の手順で揃える。

CPU の起こし・止め（`arch/smp.c`・`wake.*`）：
- 起こしの道は一つ：自前の INIT-SIPI の trampoline（今の `wake.asm` の道）を起動にも S3 の後にも使う。Limine の `goto_address` の道は消す。x2APIC だけ（`wake.asm` の xAPIC の ID の道を消す）。全 AP を一斉に起こし、締め切りは一つ。
- 止める道（S3・休止の前）も全 CPU 一斉で、締め切りは一つ。CPU 0 へ task と alarm を寄せる今の形は、波 2 の sched が CPU の出入りを持つまで残してよいが、一つずつ順に待つ輪は消す。
- 休止の写しの小片（`wake.asm` の後半、`wake.c` の休止の部分）は `dev/hibernate.c` へ移す（中身は波 4 で建て直す）。
- `cpu_parallel_for`・`cpu_jobs_poll`・`cpu_parallel_jobs`・`cpu_max_job_us`・起動の自己試験・`fpu_kernel_begin/end` は消す。呼ぶ側（`gui/wm.c`・`dev/fb.c`）はその場で順に回す形にする（表示と合成は波 4・5 で建て直す）。
- `smp_get_info` は実の値を返す（CPUID の機能、online の数）。ABI の欄の意味が変わるなら `common/abi.h` と `docs/nocturne-native-abi.md` を同じ変更で直す。
- `cpu_kick`・`cpu_alarm` は A2 の IPI の口の上に作る。

idle と性能の方針（`pstate.c` を消して `arch/idle.c` に建てる）：
- `void cpu_idle(void);` sched の idle の輪が割り込みを閉じて呼び、起こされたら割り込みを閉じたまま戻る。MWAIT があれば CPU ごとの起こしの語を MONITOR して待ち（sched はその語に書くだけで IPI を省ける。口は `cpu_idle_wake(cpu)`）、無ければ `sti; hlt`。深い C 状態は、LAPIC timer が止まらない（ARAT、B が `lapic_timer_always_running()` で出す）ときだけ。
- 性能：CPPC（AMD）があれば EPP を「性能寄りの均衡」の既定に置き、min・max は CPPC の値から。既定は一か所の名前つきの値で、後で設定の画面から変えられる口（`cpu_perf_set_policy`）を持つ。hypervisor の判定はしない（MSR が無ければ使わない）。

起動の順（`boot.c`・`init.c`）：
- 土台の依存の順に並べ直す：像と受け渡し → 機能の検出と `cpu_setup` → topology → 起動の記憶 → CPU ごとの領域 → 頁 → 核の頁表 → 入口と割り込み → 時計 → 核の仮想の番地と heap → AP → 装置。端末の鍵の訳と cmdline の読みは `init.c` から持ち主へ出す。何もしない呼出し（`boot.c:141`）は消す。vm/ の入口は装置の段で `vm/vm.h` を通して呼ぶ。

### 4.2 A2：割り込みの層

持つファイル：`arch/idt.c`、`arch/irq.h`、`arch/apic.c`・`apic.h`（timer を除く）、`arch/ioapic.c`・`ioapic.h`、`arch/isr.asm`（入口と出口、割り込みの配り）。口を変えるために触ってよい呼ぶ側：`dev/pci.c` の MSI・MSI-X の部分、`dev/pciuser.c` の割り込みの部分、`dev/xhci.c`・`dev/nvme.c`・`net/rtl8125.c`・`dev/acpi.c`（SCI）・`dev/iommu*.c`（故障の割り込み）・`dev/input.c`・`dev/serial.c` の割り込みの登録の行、`vm/` の割り込みの登録の行。

- x2APIC だけ。xAPIC の MMIO の道、AMD の 4 bit の ID、TME の KeyID、xAPIC へ戻る遷移、ICR の空回りを消す。
- IPI：`apic_send_ipi(cpu, vector)`、`apic_send_ipi_set(const uint64_t *cpuset, vector)`（x2APIC の論理 cluster で一回の ICR に最大 16 CPU、SDM 第 3A 巻）、`apic_send_ipi_others(vector)`、`apic_send_nmi(cpu)`。宛先は topology の CPU の番号で受ける。
- vector は CPU ごと：各 CPU に vector→`struct irq` の表（x86 の 256 の vector）。系の vector（spurious、APIC error、`VEC_TIMER`、`VEC_RESCHED`、`VEC_TLB_SHOOTDOWN`、cross call）は上の固定の範囲。装置の vector はそれより下を CPU ごとに割り当てる。driver ごとの決め打ちの vector（xHCI・NVMe・RTL8125・VMBus）は全部消す。
- 口（`arch/irq.h`）：
  ```c
  enum irq_result { IRQ_NONE, IRQ_HANDLED, IRQ_WAKE_THREAD };
  struct irq_spec {
      const char *name;
      enum irq_result (*handler)(void *context);   /* hard: short, interrupts off */
      void (*thread)(void *context);               /* optional: runs in the irq's own kernel thread */
      void *context;
      int node;                                    /* the device's NUMA node, -1 when unknown */
  };
  struct irq *irq_request_msi(const struct irq_spec *spec, unsigned cpu_hint); /* the caller writes the message */
  void irq_msi_message(const struct irq *irq, uint64_t *address, uint32_t *data);
  struct irq *irq_request_gsi(const struct irq_spec *spec, unsigned gsi);     /* trigger and polarity from MADT/ACPI */
  struct irq *irq_request_system(const struct irq_spec *spec, unsigned vector);/* LVT sources: threshold, error */
  void irq_free(struct irq *irq);              /* returns once no handler or thread of it runs on any CPU */
  bool irq_set_cpu(struct irq *irq, unsigned cpu);
  ```
  thread は、その irq の宛先の CPU に縛った核の thread（`kthread_create_on`）で走る。level の線は thread が終わるまで mask しておく。割り当ての CPU は device の node の中で vector の少ない CPU を選ぶ。
- EOI は処理の終わり（hard の handler の後、IRET の前。SDM 第 3A 巻 13.8.5）。
- 例外：`#PF` は CR2 を読んだ後、割り込まれた文脈の IF が 1 なら割り込みを開いて処理する（`vmm.h` の約束「割り込みは開いている」を実際にする）。user_copy の頁違反も同じ。NMI は A1 の決めたとおり。`#MC` は A1 の MCA へ渡す。
- `irq_nesting` は A1 の `preempt_count` の割り込みの深さへ置き換える。
- IOAPIC：MADT の全 IOAPIC を持つ（上限の定数なし）。GSI の唯一の持ち主。MADT の ISO（極性・引き金）、SCI を IOAPIC に載せる。宛先は 8 bit（RTE の形）なので、APIC ID が 255 を超える CPU へは IOMMU の割り込みの付け替え（IR）がある時だけ向ける。無ければ 255 以下の CPU へ。S3 の後の戻しも持つ。
- 8259：起動で番号を上へ移して全部 mask するだけ。配り・EOI・ELCR の保存と戻しは消す。PCI の INTx の 8259 への後戻り（`rtl8125.c` など）は消す。
- PS/2（`dev/input.c` の i8042 の部分）は消す。実機の鍵とマウスは USB、Hyper-V は合成の入力（vm/）、QEMU は xHCI の `usb-kbd`・`usb-tablet` で試す。
- MSI・MSI-X（`pci.c`）：MSI-X を先に、vector を複数、vector ごとの宛先。APIC ID が 255 を超える CPU は IR がある時だけ。driver はキューの数だけ irq を取れる。
- vm/ の装置も同じ口を使う（VMBus の SynIC の vector も `irq_request_system` か CPU ごとの割り当てで）。vm/ の古い装置の線（8259 の線を使っていたもの）は、PCI の INTx を ACPI の `_PRT` で GSI に引いて `irq_request_gsi` で受ける（`_PRT` の読みが `acpi.c` にあればそれを使う）。

### 4.3 B：時間（核の側）

持つファイル：`sys/clock.c`・`clock.h`、`sys/alarm.c`・`alarm.h`、`arch/lapic_timer.c`・`lapic_timer.h`、新しい基準の時計の driver（HPET・ACPI PM timer。置き場所は B が決め、持ち主は一つ）。口を変えるために触ってよい所：`sys/syscall.c` の時計の部分（`clock_page_of`、`SYS_CLOCK_INFO`）、`sys/sched.h` の `clock_page` の欄、`common/abi.h` の hypervisor の時計の型（`N_CLOCK_HV_REFERENCE`・`struct n_reference_tsc`）、`user/libc/sys.c` の hypervisor の時計の読み、`vm/hyperv.c`・`vm/vm.c` の時計の部分、`arch/apic.c` の `vm_lapic_hz` の行。

- 時計の源は TSC だけ。hypervisor の参照の時計（`SOURCE_HYPERVISOR`、`clock_hypervisor_at`、`vm_clock_ns`）を核の時計から消す。ABI の `n_reference_tsc` と libc の読みも消す。
- TSC の速さの源（順に）：CPUID 15h の正確な値、登録された速さの源（`clock_register_tsc_hz(hz, source_name)`。vm/hyperv がその MSR の値を登録する。核は hypervisor を知らない）、基準の時計での測り（HPET があれば HPET、無ければ ACPI PM timer。PIT は消す）。
- 速さは起動の短い測りで始め、その後も alarm で基準の時計と比べ続け、窓を伸ばしながら滑らかに直す（換算の傾きを少しずつ動かし、時計を戻さず跳ばさない）。64 秒目の一度だけの段は消す。直す幅と収束は報告に数で書く。
- 時計は三つ：単調（S3 の間は進まない）、起動から（S3 の間も進む：RTC の差を足す）、壁（RTC を更新中の印を見て秒の境で読み、以後は単調に乗る。後で利用者の時刻の service が滑らかに直す口 `clock_adjust_wall(offset_ns, slew_ppm)` を持つ）。
- CPU の間の TSC：起動で比べる今の考え（往復の下限）は持ち越す。ずれが測れたら、核の読みと利用者の読みが同じ表（CPU の番号→offset、`RDTSCP` の TSC_AUX が番号）を使う形にする。
- 時計の頁：核が書き、全アプリが読む頁（seqlock、換算・三つの時計の基・CPU ごとの offset。CPU の数から頁の数を決める）。波 1 では核の側（頁を持ち、速さが変わるたびに書く、`clock_data_pages()` で frame を渡す）まで。アプリの空間へ写すのと ABI は波 3。
- `clock_tsc_hz` は seqlock の中で読める形にする（新しい換算と古い速さを組み合わせない）。
- 起こしの装置（`lapic_timer.c`）：TSC-deadline があれば期限は TSC の値そのもの（偏りなし）。無ければ LAPIC の one-shot を CPU ごとに TSC と比べて測り、時計の速さが変わるたびに測り直す。`WAKE_LATE_SHIFT` を消す。早く来た割り込みは alarm の側が読み直して立て直す。ARAT を見て `lapic_timer_always_running()` を出す。VM の値（`vm_lapic_hz`）は読まない。
- alarm：同じ alarm を二つの CPU が同時に立てても一つの heap にしか入らないことを alarm 自身が守る（今の `dev/serial.c` の使い方でも正しくなる）。期限の幅（`alarm_set_range(a, earliest, latest)`：電力のためにまとめてよい幅）を持つ。発火は割り込みの中のまま（callback は短く、重い仕事は thread へ渡す）。
- `clock_spin` は仕様が求める短い待ち（µs）のためだけの口と注記に書き、ms の空回りに使う driver（`rtl8125.c`・`ahci.c`）の直しは波 4。

### 4.4 D：記録

持つファイル：`lib/printf.c`（記録と panic の部分。書式の `kvsnprintf` は残す）、`kernel.h` の `KLOG_SIZE`、`dev/serial.c`、`dev/fbcon.c`・`fbcon.h`、`sys/stream.c`・`stream.h` の system log の部分。

- 記録の行は CPU ごとの、錠の無い輪に書く。行ごとに単調 ns（`clock_now_ns`）・CPU・task の ID・重さ（level）・文字。輪の大きさは RAM と CPU の数から決める。
- 読み手は三つ：serial、起動の画面（`fbcon`：WM が画面を取るまで。実機の記録は画面の写真だけなので、起動の行を画面に出すことは要る）、system log の stream（アプリが読む）。どれも輪から読み、書き手を待たせない。serial は UART の送りの割り込み（A2 の `irq_request_gsi`）で吐き、alarm と回数の spin を消す。port は ACPI の SPCR があればそれ、無ければ COM1 を scratch register で確かめて使う。
- panic は錠を取らず、確保もせず、今出ている画面（GOP の framebuffer）と serial へ直に書く。他の CPU は A1 の NMI の道で止める。
- 核が記録を C: に書く道（`log_keep`）は消す。保存は利用者側（`user/apps/init.c` が system log の stream を読んで `C:\Nocturne` の記録へ書く）。
- 起動の旗の文（「a hobby OS dreamt up overnight」）は消す。

### 4.5 継ぎ目の表（波 1 の書き手の間）

| 口 | 持ち主 | 使う側 |
|---|---|---|
| `preempt_count`・`preempt_disable/enable`・`spin_*` の rank | A1 | A2（割り込みの深さ）、B、D |
| `cpu_setup`、DE_CFG の LFENCE | A1 | B（`lfence; rdtsc`） |
| `entry_load`、`irq_request_*`、`apic_send_ipi*` | A2 | A1（`cpu_setup`、`cpu_kick`、MCA の threshold）、B（VEC_TIMER）、D（UART の割り込み） |
| `clock_now_ns`、`alarm_*`、`lapic_timer_always_running` | B | A1（idle、起こしの締め切り）、D（行の時刻） |
| `klog`・`kprintf`・`panic` | D | 全員 |

## 5. 波 2 以降（要件だけ。各波の始めに詳しく書く）

- sched：横取りできる核（`preempt_enable` と割り込みの出口で）。級は二つ：実時間（予算つき、合成・入力・音・表示の thread）と公平（EEVDF：起こしで lag を保ち、資格があれば横取り、task ごとの要求と重み）。queue の heap は自分の task だけ。均しは topology（SMT・LLC・node）の階層で、熱い道に O(CPU) の走査を置かない。待ち行列は FIFO と排他の起こし。mutex は手渡し、rwsem、futex は番地の hash と bucket ごとの錠で絶対期限。pid・tid は使い回す割り当て。task の終わりは各持ち主が object の close で片付ける（sched は pci・wm・net を呼ばない）。
- object・handle：読みは錠なし、handle の値に世代、権利は mask。events は object の notify の callback で ready の列へ入れ、走査しない。channel は channel ごとの錠、容量は受け手の信用、送りも受けも絶対期限。
- mm：pmm は多 order の CPU ごとの溜め・遠い解放のまとめ・node ごとの並列の初期化・記憶の圧の口。vmm は空間の記述（範囲の木）を mm に一つ、頁表は触れた所だけ作り外せば返す、範囲か頁表の単位の錠、眠れる頁違反、大きな頁、撃ち落としは集合へ一度に。vmalloc は CPU ごとの塊と、足りないときの掃除。syscall.c の共有記憶・pager・program の cache・空間の予約は mm へ移す。
- 時間の ABI：時間はすべて ns、期限は絶対で時計の id を持つ。時計の頁をアプリへ写し、libc は頁を読む一つの関数だけ。事象・入力・表示（vblank の番号と表示の時刻）・音（位置とその時刻）に単調 ns の印。`ports/nvidia/os.c` の二つ目の時計は消す。
- 装置：装置木と driver の照合、MSI-X を CPU ごとのキューへ、block の層は非同期の要求と CPU ごとの送り口、NVMe は CPU ごとのキュー対と写しなしの PRP・SGL、xHCI は非同期の転送と複数の interrupter と isochronous、HID は usage のまま時刻つきで、鍵の繰り返しは GUI 側、音は HDA の driver と割り込みの外の混ぜ手と時刻つきの位置、表示は connector・CRTC・plane と装置の vblank の時刻、pm は装置木の順に並列で眠れる文脈、乱数は CPU ごとの生成器。AHCI を消すか（監査は「消すだけ」）は block の層の時に決める（今の QEMU の試験は AHCI から起動している）。
- 格納庫・網・窓：監査 01・02 の要件のとおり。
