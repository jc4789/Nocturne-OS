# 時間の道の所見を原文の行で照らす（2026-10-30）

## 依頼文（全文）

> docs/kernel-audit-20261030 の 00-index.md と 01〜03 を読み、そこで「時間の道」（tick、clock、sched の時間の扱い、timer、TSC/HPET/LAPIC の基準、時間の単位）に関わる所見を全部拾って、今の `kernel/src/sys/sched.c`・`kernel/src/sys/clock.c`・`kernel/src/arch/` と `kernel/src/dev/` の該当ファイルの原文の行で一つずつ照らしてください。所見ごとに、原文の行（ファイル名と行番号と引用）、所見が当たっているか・外れているか、当たっているなら「決め打ちの数」「一つの CPU への寄り」「ポーリング」「VM の判定の混入」のどれか、を書く。結果は `docs/kernel-audit-20261030/07-timing-check.md` に全文で書いてください（要約で済ませない）。コードは一行も変えない。コミットもしない。作業木を作らない。

## 照らした状態と、読んだもの

- 照らしたのは 2026-10-30 21:27〜（EDT）の作業木：main の `5f2fabd9` に、未コミットの 13-2 の差分（00-index.md の「監査した状態」と同じ 22 ファイル）が載ったもの。行番号はすべてこの作業木のもの。コードは一行も変えていない。コミットもしていない。
- 監査の所在：`docs/kernel-audit-20261030/` には、21:27 と 21:35 に見た時点で `00-index.md` しか無かった（01〜06 の `.md` は無い）。起こしたセッションの記録に「監査の全文は docs ではなく D:\Programes の txt 六つにある」とあり、次の txt を読んだ。00-index.md の範囲の欄と、各 txt の書き出しの範囲で対応を決めた。
  - 01（fs・common/store・net）＝ `D:\Programes\Nocturne OS カーネル監査 fscommon store net.txt`（403 行）。時間に関わる行を `rg` で拾い、該当の所を読んだ。
  - 02（gui・vm と dev/fb.c）＝ `D:\Programes\gui vm audit .txt`（475 行）。時間に関わる行を `rg` で拾い、370〜475 行（(b) vm/ の外が vm/ に頼っている所、(c) の時計）を全部読んだ。
  - 03（横断）＝ `D:\Programes\カーネル監査.txt`（252 行）。全部読んだ。
  - 04（土台）・05（時間の道と sys）・06（dev）の txt も同じ所にある（`Nocturne カーネルの土台（…）監査の報告.txt`、`時間の道とカーネルの sys の監査.txt`、`dev audit.txt`）。依頼の範囲は 01〜03 なので、この文書では照らしていない（末尾の「照らしていないもの」）。
- 読んだコード（全体）：`kernel/src/sys/clock.c`、`kernel/src/sys/alarm.c`、`kernel/src/arch/apic.c`。部分：`kernel/src/sys/sched.c`（25〜124、200〜215、255〜365、540〜600、640〜700、760〜845）、`kernel/src/sys/clock.h`（1〜60）、`kernel/src/sys/kei.c`（30〜60、95〜125、160〜240）、`kernel/src/sys/kei.h`（rg）、`kernel/src/arch/smp.c`、`cpu.h`、`cpu.c`、`pstate.c`、`idt.c`、`kernel/src/dev/` の serial・fb・usbhid・audio・audio.h・nvme・ahci・pci・pm・acpi・input・power・usbhub・usbmsc・xhci・iommu_amd・iommu_intel、`kernel/src/sys/syscall.c`・`syscall.h`・`sched.h`・`events.c`・`prof.c`、`kernel/src/boot.c`、`kernel/src/mm/vmm.c`（48〜53）、`common/abi.h`（270〜290）、`user/libc/sys.c`（88〜128）。読んだ行は各所見の下に引いた。

## 判定の書き方

- **当たり**：所見の書く事実が原文にある。
- **一部当たり**：事実はあるが、所見の書く帰結（「何が詰まるか」）か行番号の一部が原文と合わない。合わない所を書く。
- **外れ**：所見の書く事実が原文に無い。
- 分類は依頼の四つ：**決め打ちの数**、**一つの CPU への寄り**、**ポーリング**、**VM の判定の混入**。四つのどれにも入らないもの（単位の不揃い、呼ばれない関数など）は「四つの外」と書き、何であるかを書く。
- 「直し方」は書かない（ユーザーの決まり：「Please do not suggest "patching" slop」）。

---

## 03（横断）の所見

### 03 (a) 固定の数のうち、時間に関わるもの

#### 03(a)-41 `sys/clock.c:26-28` MEASURE_SPANS 5、CLOCK_REMEASURE_S 64、COMPARE_ROUNDS 256

```
kernel/src/sys/clock.c:26	#define MEASURE_SPANS 5
kernel/src/sys/clock.c:27	#define CLOCK_REMEASURE_S 64
kernel/src/sys/clock.c:28	#define COMPARE_ROUNDS 256
```

使う所：

```
kernel/src/sys/clock.c:197	    uint64_t v[MEASURE_SPANS];
kernel/src/sys/clock.c:224	    if (++remeasure.seconds < CLOCK_REMEASURE_S) {
kernel/src/sys/clock.c:275	    for (unsigned i = 0; i < COMPARE_ROUNDS; i++) {
```

- 判定：**当たり**。TSC の速さの測りの回数（5）、測り直しの窓（64 秒）、CPU の TSC の比べの回数（256）は、どれも測った量や CPU の数から出ず、定数。
- 分類：**決め打ちの数**。

#### 03(a)-30 `dev/audio.c:17-21`・`dev/audio.h:21` 流れ 8、行列 170 ms、RUNS 32、null の塊 10 ms、AUDIO_RATE 48000 固定

```
kernel/src/dev/audio.c:17	#define MAX_STREAMS 8
kernel/src/dev/audio.c:18	#define QUEUE_BYTES (8192 * AUDIO_FRAME) /* 170 ms: what a writer may be ahead of the sink (SOUND_QUEUE) */
kernel/src/dev/audio.c:19	#define RING_BYTES  (2 * QUEUE_BYTES)    /* the queue, and the PCM a card may still give back (retained) */
kernel/src/dev/audio.c:20	#define RUNS        32                   /* a stream's runs in a sink's hands: an AC'97 card's whole ring */
kernel/src/dev/audio.c:21	#define NULL_BLOCK  (AUDIO_RATE / 100)   /* the sink of nothing plays 10 ms blocks, as a card its buffers */
kernel/src/dev/audio.h:21	#define AUDIO_RATE  48000
kernel/src/dev/audio.h:22	#define AUDIO_FRAME 4 /* bytes */
```

- 判定：**当たり**。時間に関わるのは行列の長さ（8192 frame＝48 kHz で 170 ms）、null の受け手の塊（10 ms）、rate 48000 の固定。RUNS 32 の注記は「an AC'97 card's whole ring」で、VM の装置（AC'97 は `kernel/src/vm/ac97.c`）の輪の大きさを共通の層の定数の理由にしている。
- 分類：**決め打ちの数**。RUNS の理由に VM の装置の形が入っている点は **VM の判定の混入**（判定の分岐ではなく、VM の装置の都合を共通の定数の根拠にしている）。

### 03 (b) 固定の時間（ms・µs・ns の定数）

#### 03(b)-55 `sys/sched.c:39` BALANCE_NS 4 ms

```
kernel/src/sys/sched.c:39	#define BALANCE_NS (4 * NS_PER_MS) /* how often a CPU with tasks waiting looks for a much idler one */
kernel/src/sys/sched.c:332	static void rebalance(unsigned self) {
kernel/src/sys/sched.c:333	    struct cpu_queue *q = &per_cpu(queue, self);
kernel/src/sys/sched.c:334	    uint64_t now = clock_now_ns();
kernel/src/sys/sched.c:335	    if (now - q->balanced_at < BALANCE_NS || !usable(self)) return;
```

- 判定：**当たり**。均しの間隔は 4 ms の定数。`rebalance` は `slice_fire`（364 行）からだけ呼ばれ、`slice_fire` は待つ task がある CPU の slice の alarm でしか来ない（323〜327 行）。だから「CPU の数に関係なく、偏りは 4 ms ごとにしか直らない」は、待つ task のある CPU については事実。待つ task の無い CPU は均しをしない（その代わり空いた CPU は `steal` する、528 行）。
- 分類：**決め打ちの数**。

#### 03(b)-56 `sys/sched.c:41-42,655` 切り替えの費用 10 µs の見積もりから slice を出し、0.5–4 ms に挟む

```
kernel/src/sys/sched.c:41	#define KEI_SWITCH_COST_NS 10000  /* a switch (XSAVE, CR3, the caches' recovery), estimated */
kernel/src/sys/sched.c:42	#define KEI_OVERHEAD_PPM 10000    /* aim at 1% lost to switching: the retention is about 99 times it */
kernel/src/sys/sched.c:655	    kei_retention_v = kei_retention_from_cost(KEI_SWITCH_COST_NS, KEI_OVERHEAD_PPM, 500000, 4000000);
```

```
kernel/src/sys/kei.c:226	uint64_t kei_retention_from_cost(uint64_t cost_ns, uint32_t ppm,
kernel/src/sys/kei.c:227	    uint64_t lower_v, uint64_t upper_v) {
kernel/src/sys/kei.c:228	    if (!ppm || ppm >= 1000000 || !lower_v || lower_v > upper_v) return 0;
kernel/src/sys/kei.c:229	    uint64_t m = 1000000 - ppm;
kernel/src/sys/kei.c:230	    uint64_t whole = cost_ns / ppm;
kernel/src/sys/kei.c:231	    if (whole > upper_v / m) return upper_v;
kernel/src/sys/kei.c:232	    uint64_t a = whole * m;
kernel/src/sys/kei.c:233	    uint64_t b = ((cost_ns % ppm) * m + ppm - 1) / ppm;
kernel/src/sys/kei.c:234	    uint64_t answer = b > upper_v - a ? upper_v : a + b;
kernel/src/sys/kei.c:235	    return answer < lower_v ? lower_v : answer;
kernel/src/sys/kei.c:236	}
```

- 計算：cost 10000、ppm 10000 で m＝990000、whole＝1、a＝990000、b＝0、答え 990000（下限 500000・上限 4000000 の内）。重さ `KEI_WEIGHT_ONE`（1024、`kei.h:13`）の task では `weighted_delta`（`kei.c:57-61`）が ns をそのまま仮想時間にするので、この retention は約 0.99 ms の実時間に当たる。全 task が `KEI_WEIGHT_ONE`（`sched.c:698`）なので、今はいつもこの値。
- 判定：**当たり**。名は「slice」ではなく retention（`kei_decide` の交替の閾、`kei.c:200`）だが、所見の事実（測っていない 10 µs の見積もりから出し、0.5〜4 ms に挟む）は原文のとおり。切り替えの費用を測る所は無い（`KEI_SWITCH_COST_NS` の注記が「estimated」）。
- 分類：**決め打ちの数**。

#### 03(b)-57 `sys/sched.c:43` KILLED_NAP_NS 10 ms

```
kernel/src/sys/sched.c:43	#define KILLED_NAP_NS (10 * NS_PER_MS) /* a killed task's waits end this soon, whether a waker comes or not */
kernel/src/sys/sched.c:785	            /* A kill after this finds it blocked and wakes it (kill_locked); one before is seen
kernel/src/sys/sched.c:786	               here, and the wait ends soon: the caller looks at killed again, or waits on. */
kernel/src/sys/sched.c:787	            if (__atomic_load_n(&t->killed, __ATOMIC_SEQ_CST)) deadline = MIN(deadline, clock_after(KILLED_NAP_NS));
```

- 判定：**一部当たり**。10 ms の定数はある。ただし所見の帰結「殺された thread の終わりが 10 ms 刻みになる」は言い過ぎ：眠りに入った後の kill は `kill_locked` がすぐ起こす（785 行の注記）。10 ms が効くのは、既に殺された task がさらに眠ろうとした回だけで、その回の眠りが最長 10 ms になる。
- 分類：**決め打ちの数**。待ちの側から見ると、殺された task は 10 ms ごとに起きて見直す形なので **ポーリング** の面もある。

#### 03(b)-58 `arch/apic.c:339` WAKE_LATE_SHIFT 13：timer の速さをわざと 1/8192 高く取る

```
kernel/src/arch/apic.c:339	#define WAKE_LATE_SHIFT 13 /* the rate taken 1/8192 high: under 0.013% late */
kernel/src/arch/apic.c:361	        wake_tsc = cpu_features.tsc_deadline && clock_tsc_at(clock_now_ns(), &tsc);
kernel/src/arch/apic.c:364	        if (!wake_tsc) {
kernel/src/arch/apic.c:374	            hz += hz >> WAKE_LATE_SHIFT;
kernel/src/arch/apic.c:375	            wake_mult = (hz << 32) / NS_PER_S;
```

- 判定：**一部当たり**。わざと遅らせる定数はある。ただし所見の帰結「すべての期限が遅れる側へずれる」は、一回数え（one-shot count）の道だけに当てはまる。TSC deadline の道（CPU が TSC deadline を持ち、時計の源が TSC のとき、361 行）では `WAKE_LATE_SHIFT` を使わず、`clock_tsc_at`（`clock.c:113`）が ns から TSC へ切り上げで換える。どちらの道を実機（7600X）が通るかは、7600X が CPUID の TSC deadline の bit を持つかで決まるが、その bit をこの照らしでは確かめていない（未確認）。時計の源が hypervisor のとき（`clock_tsc_at` が false、`clock.c:102`）は必ず一回数えの道になる。
- 分類：**決め打ちの数**。

#### 03(b)-59 `arch/apic.c:350` LAPIC timer の速さを 20 ms の窓で測る

```
kernel/src/arch/apic.c:345	/* The input clock's rate over 20 ms of the clock, with the timer masked. */
kernel/src/arch/apic.c:346	static uint64_t measure_lapic_hz(void) {
kernel/src/arch/apic.c:347	    lapic_write(LAPIC_TIMER_DIV, 0xB); /* divide by 1 */
kernel/src/arch/apic.c:348	    lapic_write(LAPIC_LVT_TIMER, LVT_UNUSED);
kernel/src/arch/apic.c:349	    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFF);
kernel/src/arch/apic.c:350	    uint64_t start = clock_now_ns(), until = start + 20 * NS_PER_MS, now;
kernel/src/arch/apic.c:351	    while ((now = clock_now_ns()) < until) pause();
```

- 判定：**当たり**。一回数えの道で、hypervisor が速さを言わないとき（366〜368 行）に、20 ms の固定の窓を spin で測る。測るのは最初に `lapic_wake_init` を呼んだ CPU で一度だけ（359 行 `if (!wake_vector)`）、その値を全 CPU が使う。
- 分類：**決め打ちの数**（20 ms）。測りの間 CPU を spin で塞ぐ点は **ポーリング**。

#### 03(b)-60 `arch/smp.c:24-25,95` AP の起動・停止の待ち 2000 ms ずつ、panic の NMI の待ち 100 ms

```
kernel/src/arch/smp.c:24	#define START_TIMEOUT_MS 2000
kernel/src/arch/smp.c:25	#define PARK_TIMEOUT_MS 2000
kernel/src/arch/smp.c:95	    uint64_t until = clock_after(100 * NS_PER_MS);
kernel/src/arch/smp.c:96	    while (__atomic_load_n(&panic_stopped, __ATOMIC_ACQUIRE) < others && clock_now_ns() < until) pause();
kernel/src/arch/smp.c:295	    uint64_t until = clock_after(START_TIMEOUT_MS * NS_PER_MS);
kernel/src/arch/smp.c:360	        uint64_t until = clock_after(PARK_TIMEOUT_MS * NS_PER_MS);
```

- 判定：**当たり**。起動の待ちは全 CPU で一つの締め切り（295 行）だが、停止の待ち（360 行）は CPU ごとに 2 秒で、356〜369 行の輪の中で一つずつ待つ（03(d)-128 を参照）。
- 分類：**決め打ちの数**。待ちはどれも spin なので **ポーリング** の面もある（03(c)-107）。

#### 03(b)-61 `arch/cpu.h:231-232` INIT の後 10 ms、STARTUP の後 200 µs

```
kernel/src/arch/cpu.h:227	/* INIT and STARTUP, sent and not waited for (SDM 9.4.4.1, Table 9-1): the caller waits
kernel/src/arch/cpu.h:228	   LAPIC_INIT_WAIT_NS after its INITs before any STARTUP, and sends STARTUP twice, waiting
kernel/src/arch/cpu.h:229	   LAPIC_STARTUP_WAIT_NS after each round, the second ignored by a CPU already running. One wait
kernel/src/arch/cpu.h:230	   serves every CPU sent to. */
kernel/src/arch/cpu.h:231	#define LAPIC_INIT_WAIT_NS 10000000ULL
kernel/src/arch/cpu.h:232	#define LAPIC_STARTUP_WAIT_NS 200000ULL
```

- 判定：**当たり**。注記のとおり SDM の表の値で、一つの待ちを全 CPU が共有する（CPU の数で伸びない）。使う所の一つ：`smp.c:376 clock_spin(LAPIC_INIT_WAIT_NS);`。
- 分類：**決め打ちの数**（仕様の値）。

#### 03(b)-62 `arch/pstate.c:83` 5 ms の spin

```
kernel/src/arch/pstate.c:79	static unsigned measure_mhz(void) {
kernel/src/arch/pstate.c:80	    uint64_t hz = clock_tsc_hz();
kernel/src/arch/pstate.c:81	    if (!have_aperf || hz < 1000000) return 0;
kernel/src/arch/pstate.c:82	    uint64_t a0 = rdmsr(MSR_APERF), m0 = rdmsr(MSR_MPERF);
kernel/src/arch/pstate.c:83	    for (uint64_t until = clock_after(5 * NS_PER_MS); clock_now_ns() < until;) pause();
kernel/src/arch/pstate.c:91	    unsigned before = measure_mhz();
kernel/src/arch/pstate.c:93	    unsigned after = measure_mhz();
```

- 判定：**当たり**。しかも `pstate_init_bsp` は前と後で二度呼ぶので（91、93 行）、起動の道で CPU 0 を 10 ms 塞ぐ。測った値は記録の一行（95 行）にしか使わない。
- 分類：**決め打ちの数**、**ポーリング**（spin）。CPU 0 だけで行う点は **一つの CPU への寄り**（起動の一度だけ）。

#### 03(b)-63 `mm/vmm.c:51` TLB_SLOW_NS 5 s

```
kernel/src/mm/vmm.c:51	#define TLB_SLOW_NS   (5 * NS_PER_S) /* a shootdown not answered for this long is reported */
```

- 判定：**当たり**（依頼の範囲の外の mm だが、一行なので照らした）。報せの線だけで、待ちの長さは変えない。
- 分類：**決め打ちの数**。

#### 03(b)-64 `sys/clock.c:169,182,225` 測りの窓 10 ms、1 s の alarm

```
kernel/src/sys/clock.c:169	    uint32_t span = PM_TIMER_HZ / 100, counted;
kernel/src/sys/clock.c:182	    uint16_t count = PIT_HZ / 100;
kernel/src/sys/clock.c:225	        alarm_set(a, a->at + NS_PER_S);
```

- 判定：**当たり**。PM timer と PIT の一回の測りはどちらも 10 ms（1/100 秒）、長い測り直しは 1 秒ごとの alarm で PM timer の桁あふれを拾う（210〜237 行）。
- 分類：**決め打ちの数**。測りは spin（166、170、189 行）で **ポーリング**。

#### 03(b)-65 `dev/serial.c:19` CHAR_NS 86806（115200 baud を前提）

```
kernel/src/dev/serial.c:19	#define CHAR_NS 86806 /* a character's 10 bits at 115200 baud */
kernel/src/dev/serial.c:33	    outb(COM1 + 3, 0x80); /* DLAB */
kernel/src/dev/serial.c:34	    outb(COM1 + 0, 0x01); /* 115200 baud */
```

- 判定：**一部当たり**。定数はあるが、同じファイルが除数を 1（115200 baud）に固定しているので、今は「baud が変わると送りの時間がずれる」ことは起きない。決め打ちは CHAR_NS ではなく、baud と COM1 の port（16 行 `#define COM1 0x3F8`）の方。
- 分類：**決め打ちの数**。

#### 03(b)-66 `dev/fb.c:208` DRAIN_POLL 100 µs

```
kernel/src/dev/fb.c:205	/* Returns once every present that began before it has ended. A present wakes no one (the
kernel/src/dev/fb.c:206	   console's come from any context, a log line under the scheduler's locks among them), so this
kernel/src/dev/fb.c:207	   looks again every DRAIN_POLL_NS: a present is one copy of at most the screen. */
kernel/src/dev/fb.c:208	#define DRAIN_POLL_NS (100 * NS_PER_US)
kernel/src/dev/fb.c:209	static void presents_drain(void) {
kernel/src/dev/fb.c:210	    while (__atomic_load_n(&presenting, __ATOMIC_SEQ_CST)) sleep_until(clock_after(DRAIN_POLL_NS));
```

- 判定：**当たり**。
- 分類：**決め打ちの数**、**ポーリング**（100 µs ごとに眠って見直す）。

#### 03(b)-67〜70 `gui/wm.c`・`gui/screenmode.c`・`common/display_abi.h` の拍と時間

- 判定：**照らしていない**（依頼の範囲は sched.c・clock.c・arch/・dev/ で、gui/ はその外）。所見の行：`gui/wm.c:117,127,208,213,1496,1933,2484,2584`、`gui/screenmode.c:36`、`common/display_abi.h:64`。

#### 03(b)-71 `dev/usbhid.c:15-16` 鍵の繰り返し 500 ms / 33 ms

```
kernel/src/dev/usbhid.c:15	#define REPEAT_DELAY (500 * NS_PER_MS)
kernel/src/dev/usbhid.c:16	#define REPEAT_RATE (33 * NS_PER_MS)
```

- 判定：**当たり**。利用者の設定に結ばれていない。繰り返しの拍は `usb_thread` の起きる時（`xhci.c:1495 next = usbhid_tick(now);`）で回る。
- 分類：**決め打ちの数**。

#### 03(b)-72 `dev/xhci.c` の固定の待ち（467,506,511,581,1056,1109,1114,1130,1153,1193,1275,1335,1343,1353,1367,1450,1497-1498,1519）

`rg -n "NS_PER_(MS|US|S)" kernel/src/dev/xhci.c` の結果（全部）：

```
467:        uint64_t look = now + (any_polled() ? NS_PER_MS : 10 * NS_PER_MS);
506:    if (!hc_wait(&hc->cmd_done, clock_after(3 * NS_PER_S))) {
511:        uint64_t deadline = clock_after(5 * NS_PER_S);
514:            sleep_until(clock_after(NS_PER_MS));
581:    if (!hc_wait(&xd->ctl_done, clock_after(2 * NS_PER_S))) {
838:    if (!hc_wait(&ep->sync_done, clock_after(timeout_ms * NS_PER_MS))) {
1031:    sleep_until(clock_after(2 * NS_PER_MS));
1056:        if (attempt) sleep_until(clock_after((100 * NS_PER_MS) << attempt));
1109:    for (uint64_t deadline = clock_after(500 * NS_PER_MS);
1111:        sleep_until(clock_after(NS_PER_MS));
1114:    sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
1130:        sleep_until(clock_after(100 * NS_PER_MS)); /* connect debounce */
1136:            for (uint64_t deadline = clock_after(NS_PER_S);
1140:                sleep_until(clock_after(NS_PER_MS));
1153:        sleep_until(clock_after(100 * NS_PER_MS));
1193:            ep->retry_at = now + ((10 * NS_PER_MS) << ep->fails);
1213:        sleep_until(clock_after(NS_PER_MS));
1251:                if (!wait_reg(hc->cap, x, 1u << 16, 0, NS_PER_S))
1270:    if (!wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
1275:    if (!wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, 50 * NS_PER_MS)) {
1280:    sleep_until(clock_after(2 * NS_PER_MS));
1281:    if (!wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S) || !wait_reg(hc->op, USBSTS, STS_CNR, 0, NS_PER_S)) {
1343:    if (!wait_reg(hc->op, USBSTS, STS_HCH, 0, 100 * NS_PER_MS)) {
1353:    sleep_until(clock_after(20 * NS_PER_MS));
1367:    wait_reg(hc->op, USBSTS, STS_HCH, STS_HCH, 50 * NS_PER_MS);
1369:    sleep_until(clock_after(2 * NS_PER_MS));
1370:    wait_reg(hc->op, USBCMD, 2, 0, NS_PER_S);
1450:    uint64_t settle_at = clock_after(500 * NS_PER_MS);
1497:        if (!usb_settled) next = MIN(next, now + 50 * NS_PER_MS);
1498:        next = MIN(next, now + (any_polled() ? 4 * NS_PER_MS : NS_PER_S));
1519:        for (uint64_t deadline = clock_after(20 * NS_PER_MS);
```

1335 行（IMOD）は単位が装置のもの：

```
kernel/src/dev/xhci.c:1335	    wr32(hc->rt, IMOD, 1000); /* at most one interrupt per 250 us */
```

- 判定：**当たり**。所見が挙げた行は全部ある。所見に無い固定の待ちもある：514、838（呼ぶ側の ms）、1031、1111、1136、1140、1213、1251、1270、1280、1281、1369、1370。
- 分類：**決め打ちの数**。1 ms・10 ms ごとに眠って見直す輪（467、514、1111、1140、1213）は **ポーリング**（03(c)-91〜94）。

#### 03(b)-73 `dev/usbhub.c:96,175-176,213-229`・`dev/usbmsc.c:134`

```
kernel/src/dev/usbhub.c:96	    h->power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;
kernel/src/dev/usbhub.c:129	    sleep_until(clock_after(h->power_ms * NS_PER_MS));
kernel/src/dev/usbhub.c:175	    for (uint64_t deadline = clock_after(500 * NS_PER_MS);;) {
kernel/src/dev/usbhub.c:176	        sleep_until(clock_after(10 * NS_PER_MS));
kernel/src/dev/usbhub.c:213	    if (!h->ss) sleep_until(clock_after(100 * NS_PER_MS));
kernel/src/dev/usbhub.c:215	        if (attempt) sleep_until(clock_after(100 * NS_PER_MS));
kernel/src/dev/usbhub.c:229	        sleep_until(clock_after(10 * NS_PER_MS)); /* reset recovery */
kernel/src/dev/usbmsc.c:134	        sleep_until(clock_after(250 * NS_PER_MS));
```

- 判定：**当たり**。96 行の 100 ms の床は hub の記述子の値（bPwrOn2PwrGood×2 ms）を下から押さえる定数。
- 分類：**決め打ちの数**。175〜176 行は **ポーリング**（03(c)-101）。

#### 03(b)-74 `dev/nvme.c:23-24,168,368` ADMIN 5 s・I/O 30 s、10 ms・1 ms の見直し、CAP.TO

```
kernel/src/dev/nvme.c:23	#define ADMIN_NS (5 * NS_PER_S)
kernel/src/dev/nvme.c:24	#define IO_NS (30 * NS_PER_S)
kernel/src/dev/nvme.c:167	        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
kernel/src/dev/nvme.c:168	        wq_wait_unless(&c->wq, &c->kicks, seen, clock_after(c->vector ? 10 * NS_PER_MS : NS_PER_MS));
kernel/src/dev/nvme.c:368	    c->ready_ns = MAX(to, 4u) * 500 * NS_PER_MS; /* 500 ms units, at least 2 s */
```

- 判定：**当たり**。368 行は装置の CAP.TO（500 ms の単位）から出し、床を 2 秒にする。
- 分類：**決め打ちの数**。168 行は **ポーリング**（03(c)-95）。

#### 03(b)-75 `dev/ahci.c:21-25,180,280,401-404`

```
kernel/src/dev/ahci.c:21	#define SPINUP_NS (50 * NS_PER_MS) /* links come up after the spin-up bit is set */
kernel/src/dev/ahci.c:22	#define LINK_NS NS_PER_S            /* after a COMRESET the link is up within this */
kernel/src/dev/ahci.c:23	#define READY_NS (10 * NS_PER_S)    /* a disk that was asleep or spinning up answers within this */
kernel/src/dev/ahci.c:24	#define STOP_NS (500 * NS_PER_MS)   /* a port's engines stop within this (10.1.2) */
kernel/src/dev/ahci.c:25	#define IO_NS (30 * NS_PER_S)
kernel/src/dev/ahci.c:180	    clock_spin(2 * NS_PER_MS); /* at least 1 ms, also where nothing may sleep */
kernel/src/dev/ahci.c:280	        else wq_wait_unless(&p->wq, &p->kicks, seen, clock_after(p->h->vector ? 10 * NS_PER_MS : NS_PER_MS));
kernel/src/dev/ahci.c:401	        uint64_t deadline = clock_after(25 * NS_PER_MS);
kernel/src/dev/ahci.c:402	        while (hba_rd(h, BOHC) & BOHC_BOS && clock_now_ns() < deadline) sleep_until(clock_after(NS_PER_MS));
kernel/src/dev/ahci.c:404	        if (hba_rd(h, BOHC) & BOHC_BB) sleep_until(clock_after(2 * NS_PER_S));
```

- 判定：**当たり**。
- 分類：**決め打ちの数**。180 行は割り込みの開閉によらない spin、280・402 行は **ポーリング**（03(c)-95・96）。

#### 03(b)-76 `dev/pci.c:695,697,717-718,1044-1046,1071,1191-1203`

```
kernel/src/dev/pci.c:695	#define COMPLETION_FLOOR_MS 1000
kernel/src/dev/pci.c:697	    static const unsigned top_ms[16] = {[0x0] = 50, [0x1] = 1, [0x2] = 10, [0x5] = 55, [0x6] = 210,
kernel/src/dev/pci.c:698	                                        [0x9] = 900, [0xA] = 3500, [0xD] = 13000, [0xE] = 64000};
kernel/src/dev/pci.c:717	#define FLR_DONE_NS (100 * NS_PER_MS)
kernel/src/dev/pci.c:718	#define READY_NS    NS_PER_S
kernel/src/dev/pci.c:1044	    if ((cap & PCIE_LNKCAP_SPEED) <= 2 || !(cap & PCIE_LNKCAP_DLL_REPORTING)) b->below = b->reset_end + 100 * NS_PER_MS;
kernel/src/dev/pci.c:1046	        b->below = clock_after(100 * NS_PER_MS);
kernel/src/dev/pci.c:1071	        for (uint64_t until = clock_after(4 * NS_PER_MS); clock_now_ns() < until;) wait_step(sleeps);
kernel/src/dev/pci.c:1191	    for (uint64_t until = clock_after(NS_PER_MS); clock_now_ns() < until;) wait_step(true); /* Trst */
kernel/src/dev/pci.c:1194	        for (uint64_t until = clock_after(NS_PER_S);
kernel/src/dev/pci.c:1203	    for (uint64_t until = clock_after(2 * NS_PER_MS); clock_now_ns() < until;) wait_step(true);
```

- 判定：**当たり**（定数はある）。ただしこれらは注記（690〜694、711〜716 行）が PCIe 仕様の節（6.6.1、6.6.2、7.9.16、5.10.1）から引いた時間で、仕様が要求する待ちである。「決め打ち」の中でも、根拠の無い定数とは性質が違う。`top_ms` は DEVCTL2 の Completion Timeout の値の表。
- 分類：**決め打ちの数**（仕様の値）。1071・1191・1203 行の輪は `wait_step`（1 ms 眠るか pause）で **ポーリング**（03(c)-97）。

#### 03(b)-77 `dev/pm.c:28-31,92,99,113`

```
kernel/src/dev/pm.c:28	#define GRACE_NS   (5 * NS_PER_S)  /* not from a menu: time the apps get to close */
kernel/src/dev/pm.c:29	#define STALL_NS   (15 * NS_PER_S) /* from a menu: no window closed for this long cancels */
kernel/src/dev/pm.c:30	#define FORCE_NS   (60 * NS_PER_S) /* from a menu, after a cancel: asking again closes them anyway */
kernel/src/dev/pm.c:31	#define STOP_NS    (3 * NS_PER_S)  /* stopped apps get this long to end */
kernel/src/dev/pm.c:92	        if (now - start >= NS_PER_S && (strcmp(title, shown) || now - told >= 3 * NS_PER_S)) {
kernel/src/dev/pm.c:99	        sleep_until(clock_after(50 * NS_PER_MS));
kernel/src/dev/pm.c:113	    while ((left = task_stop_apps()) && clock_now_ns() < end) sleep_until(clock_after(20 * NS_PER_MS));
```

- 判定：**当たり**。
- 分類：**決め打ちの数**。99・113 行は **ポーリング**（03(c)-99）。

#### 03(b)-78 `dev/acpi.c:261`・`dev/input.c:178`・`dev/audio.c:641`

```
kernel/src/dev/acpi.c:261	#define FROZEN_WAIT_NS (2 * NS_PER_S)
kernel/src/dev/input.c:178	#define PS2_WAIT_NS (100 * NS_PER_MS)
kernel/src/dev/audio.c:641	    uint64_t give_up = clock_after(NS_PER_S);
```

- 判定：**当たり**。
- 分類：**決め打ちの数**。

#### 03(b)-79 `dev/iommu_intel.c:430,451,544,646,1001,1009,1039,1094`・`dev/iommu_amd.c:433,495,593` 10–100 ms の spin の上限、割り込みを閉じたまま

```
kernel/src/dev/iommu_amd.c:433:    for (uint64_t t = clock_after(10 * NS_PER_MS); (rd(u, off) & bits) != want; pause())
kernel/src/dev/iommu_amd.c:495:    for (uint64_t t = clock_after(100 * NS_PER_MS); (rd(u, R_CMD_HEAD) & 0x7FFF0) == (u->cmd_tail + 16) % RING_BYTES;
kernel/src/dev/iommu_amd.c:593:        for (uint64_t t = clock_after(100 * NS_PER_MS); !u->lost && word[i] != token; pause()) {
kernel/src/dev/iommu_intel.c:430:    for (uint64_t t = clock_after(100 * NS_PER_MS);; pause()) {
kernel/src/dev/iommu_intel.c:451:    for (uint64_t t = clock_after(100 * NS_PER_MS); !!(rd32(u, R_GSTS) & bit) != completed; pause())
kernel/src/dev/iommu_intel.c:544:    for (uint64_t t = clock_after(100 * NS_PER_MS); (rd64(u, R_IQH) & (PAGE_SIZE - 16)) == (u->tail + 16) % PAGE_SIZE;
kernel/src/dev/iommu_intel.c:646:        for (uint64_t t = clock_after(100 * NS_PER_MS); u->waiting && word[i] != u->token; pause()) {
kernel/src/dev/iommu_intel.c:1001:        for (uint64_t t = clock_after(100 * NS_PER_MS); rd64(u, R_IQH) != u->tail; pause())
kernel/src/dev/iommu_intel.c:1009:    for (uint64_t t = clock_after(100 * NS_PER_MS); rd64(u, R_IQH) != tail; pause())
kernel/src/dev/iommu_intel.c:1039:    for (uint64_t t = clock_after(100 * NS_PER_MS); *word != u->handoff_token || rd64(u, R_IQH) != u->handoff_tail;
kernel/src/dev/iommu_intel.c:1094:    for (uint64_t t = clock_after(100 * NS_PER_MS); rd32(u, R_PMEN) & PMEN_PRS; pause())
```

「割り込みを閉じたまま」の確かめ（呼ぶ側が `spin_lock` を取ってから同期の spin に入る）：

```
kernel/src/dev/iommu_amd.c:161:static struct spinlock amd_lock;
kernel/src/dev/iommu_amd.c:635:    uint64_t f = spin_lock(&amd_lock);
kernel/src/dev/iommu_amd.c:637:    bool ok = sync();
kernel/src/dev/iommu_intel.c:194:static struct spinlock units_lock;
kernel/src/dev/iommu_intel.c:807:    uint64_t f = spin_lock(&units_lock);
kernel/src/dev/iommu_intel.c:817:        ok = sync();
```

`sync()`（`iommu_amd.c:581-601`）と `sync_units()`（`iommu_intel.c:632-654`）は unit ごとに 100 ms までの spin を一つずつ回す：

```
kernel/src/dev/iommu_amd.c:591	    for (size_t i = 0; i < nunits; i++) {
kernel/src/dev/iommu_amd.c:592	        struct unit *u = &units[i];
kernel/src/dev/iommu_amd.c:593	        for (uint64_t t = clock_after(100 * NS_PER_MS); !u->lost && word[i] != token; pause()) {
```

- 判定：**当たり**。`spin_lock` は割り込みを閉じる（`.claude/rules/kernel.md`：「呼ぶ側は `spin_lock` で割り込みも閉じる」）。unit が n 個なら最悪 n×100 ms を割り込みを閉じて spin しうる。
- 分類：**決め打ちの数**、**ポーリング**。

#### 03(b)-80・81・82・84 `fs/store.c`・`sys/keep.h`・`net/net.c`・`net/rtl8125.c`・`vm/` の固定の時間

- 判定：**照らしていない**（依頼の範囲の外：fs、net、vm）。所見の行：`fs/store.c:17-19,309`、`sys/keep.h:22`、`net/net.c:113,414-415,518,844,855,973,1210-1218,1287,2375-2377`、`net/rtl8125.c:196-298,547`、`vm/rdp.c`・`vm/hv/*`・`vm/virtio_gpu.c`・`vm/ac97.c`・`vm/tulip.c`・`vm/e1000.c` の行。

#### 03(b)-83 `sys/syscall.c:155`・`sys/prof.c:25,29` 報せの間隔 10 s、標本 1 ms、窓 10 s

```
kernel/src/sys/syscall.c:155	    if (last && now - last < 10 * NS_PER_S) return;
kernel/src/sys/prof.c:25	#define SAMPLE_NS NS_PER_MS
kernel/src/sys/prof.c:29	static unsigned period_ms = 10000, top_count = 400;
```

- 判定：**当たり**。prof の窓は起動の cmdline（`prof_ms=<n>`、26〜28 行の注記）で変えられる。
- 分類：**決め打ちの数**。

### 03 (c) 見回り（周期の thread、眠って見直す輪、回数で止める spin）

#### 03(c)-91 `dev/xhci.c:461-476` hc_wait が 1 ms か 10 ms ごとに見る

```
kernel/src/dev/xhci.c:459	/* Wait for a completion flag until the deadline, handling every controller's events meanwhile:
kernel/src/dev/xhci.c:460	   looked at again each millisecond while a controller is polled, else each 10 ms. */
kernel/src/dev/xhci.c:461	static bool hc_wait(volatile bool *flag, uint64_t deadline) {
kernel/src/dev/xhci.c:462	    for (;;) {
kernel/src/dev/xhci.c:463	        poll_all();
kernel/src/dev/xhci.c:464	        if (*flag) return true;
kernel/src/dev/xhci.c:465	        uint64_t now = clock_now_ns();
kernel/src/dev/xhci.c:466	        if (now >= deadline) return false;
kernel/src/dev/xhci.c:467	        uint64_t look = now + (any_polled() ? NS_PER_MS : 10 * NS_PER_MS);
kernel/src/dev/xhci.c:468	        uint64_t f = spin_lock(&usb_lock);
kernel/src/dev/xhci.c:469	        if (!usb_kick) {
kernel/src/dev/xhci.c:470	            wq_wait_locked_until(&usb_wq, &usb_lock, MIN(look, deadline));
```

割り込みの側（起こす）：

```
kernel/src/dev/xhci.c:488	    usb_kick = true;
kernel/src/dev/xhci.c:489	    wq_wake_all(&usb_wq);
```

- 判定：**一部当たり**。1 ms・10 ms の周期で起きる見回りはある。しかし帰結「終わりの知らせが最大 10 ms 遅れる」は、MSI が来る制御器では当たらない：`xhci_isr` が `usb_kick` を立てて `usb_wq` を起こすので、待ち手はすぐ起きる。10 ms 遅れるのは割り込みが失われたとき（または `any_polled()` の制御器が無く、割り込みの無い出来事を待つとき）。ただし `usb_kick` は全制御器・全待ち手で一つの旗で、`usb_thread` と入れ子の `hc_wait` が取り合う（1500 行の注記「usb_more: work arrived during a nested wait, which used up the kick」）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-92 `dev/xhci.c:1447-1507` usb_thread が 1 s・4 ms・50 ms ごとに見回り、usbhid_tick も回す

```
kernel/src/dev/xhci.c:1495	        uint64_t now = clock_now_ns(), next = usbhid_tick(now);
kernel/src/dev/xhci.c:1496	        next = MIN(next, usb_deadline);
kernel/src/dev/xhci.c:1497	        if (!usb_settled) next = MIN(next, now + 50 * NS_PER_MS);
kernel/src/dev/xhci.c:1498	        next = MIN(next, now + (any_polled() ? 4 * NS_PER_MS : NS_PER_S));
kernel/src/dev/xhci.c:1501	        if (!usb_more && !usb_kick && next > now) {
kernel/src/dev/xhci.c:1502	            wq_wait_locked_until(&usb_wq, &usb_lock, next);
```

- 判定：**当たり**。割り込みがあっても、最長 1 秒（1498 行）ごとに必ず起きて全制御器の状態を読む（1476 行 `rd32(hc->op, USBSTS)`）。鍵の繰り返し（usbhid）もこの thread の周期で回る。
- 分類：**ポーリング**、**決め打ちの数**。系に一本の thread（1562 行の生成は所見 03(d)-132）なので **一つの CPU への寄り** の面もある。

#### 03(c)-93 `dev/xhci.c:1465-1470` MSI が来なければ見回りに落ちる

```
kernel/src/dev/xhci.c:1465	            if (hc->running && hc->vector && !interrupts && hc->events >= 16) {
kernel/src/dev/xhci.c:1466	                /* events keep coming but no interrupt ever did: the message goes nowhere */
kernel/src/dev/xhci.c:1467	                kprintf("usb: controller %d interrupts never arrive, polling instead\n", hc->index);
kernel/src/dev/xhci.c:1468	                wr32(hc->op, USBCMD, rd32(hc->op, USBCMD) & ~CMD_INTE);
kernel/src/dev/xhci.c:1469	                wr32(hc->rt, IMAN, 1);
kernel/src/dev/xhci.c:1470	                hc_set_irq(hc, hc->running, 0);
```

- 判定：**当たり**。
- 分類：**ポーリング**。

#### 03(c)-94 `dev/xhci.c:512-515` 1 ms の見回り

```
kernel/src/dev/xhci.c:511	        uint64_t deadline = clock_after(5 * NS_PER_S);
kernel/src/dev/xhci.c:512	        while ((rd32(hc->op, CRCR) & CRCR_CRR) && clock_now_ns() < deadline) {
kernel/src/dev/xhci.c:513	            poll_all();
kernel/src/dev/xhci.c:514	            sleep_until(clock_after(NS_PER_MS));
kernel/src/dev/xhci.c:515	        }
```

- 判定：**当たり**（命令の中止の後の待ち）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-95 `dev/nvme.c:168`・`dev/ahci.c:280` 完了を 10 ms ごと（ベクタが無いと 1 ms）に見直す

```
kernel/src/dev/nvme.c:167	        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
kernel/src/dev/nvme.c:168	        wq_wait_unless(&c->wq, &c->kicks, seen, clock_after(c->vector ? 10 * NS_PER_MS : NS_PER_MS));
kernel/src/dev/ahci.c:278	        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
kernel/src/dev/ahci.c:279	        if (!sleeps) pause();
kernel/src/dev/ahci.c:280	        else wq_wait_unless(&p->wq, &p->kicks, seen, clock_after(p->h->vector ? 10 * NS_PER_MS : NS_PER_MS));
```

- 判定：**当たり**。注記自身が「the 10 ms only in case one is lost」と書く：割り込みを失っても 10 ms で拾う作りで、失ったことは表に出ない。ahci は眠れない文脈（279 行）では pause の spin になる。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-96 `dev/nvme.c:180`・`dev/ahci.c:139,402` wait_ready・wait_reg が 1 ms 眠って見直す

```
kernel/src/dev/nvme.c:180	        sleep_until(clock_after(NS_PER_MS));
kernel/src/dev/ahci.c:139	        if (sleeps) sleep_until(clock_after(NS_PER_MS));
kernel/src/dev/ahci.c:140	        else pause();
kernel/src/dev/ahci.c:402	        while (hba_rd(h, BOHC) & BOHC_BOS && clock_now_ns() < deadline) sleep_until(clock_after(NS_PER_MS));
```

- 判定：**当たり**。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-97 `dev/pci.c:163-165,1071,1191-1203,1340` wait_step（1 ms 眠るか pause）

```
kernel/src/dev/pci.c:162	/* A step of a polled wait: a millisecond asleep, or a spin where interrupts are off (boot, sleep). */
kernel/src/dev/pci.c:163	static void wait_step(bool sleeps) {
kernel/src/dev/pci.c:164	    if (sleeps) sleep_until(clock_after(NS_PER_MS));
kernel/src/dev/pci.c:165	    else pause();
kernel/src/dev/pci.c:1340	        if (watching) pause();
```

- 判定：**当たり**。1340 行は眠らない pause の輪（1336〜1339 行で全装置を回り、`below_ready` を見る）。
- 分類：**ポーリング**。

#### 03(c)-98 `dev/fb.c:210` presents_drain が 100 µs ごとに見る

- 原文と判定は 03(b)-66 と同じ（**当たり**、**ポーリング**）。

#### 03(c)-99 `dev/pm.c:99,113` 窓とアプリを 50 ms・20 ms ごとに見る

- 原文は 03(b)-77。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-100 `fs/store.c:309` ディスクを 100 ms ごとに探す

- 判定：**照らしていない**（fs は範囲の外）。

#### 03(c)-101 `dev/usbhub.c:175-176` 10 ms ごと

- 原文は 03(b)-73。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-102 `dev/serial.c:87,96` UART を alarm で、割り込みを閉じて吐く

```
kernel/src/dev/serial.c:86	    /* after the lock: alarm_set takes the alarms' own, which a log line under it would need */
kernel/src/dev/serial.c:87	    if (waits && !alarm_armed(&drain)) alarm_set(&drain, clock_after(fifo_depth * CHAR_NS));
kernel/src/dev/serial.c:91	static void drain_fire(struct alarm *a, struct regs *r) {
kernel/src/dev/serial.c:92	    spin_take(&serial_lock);
kernel/src/dev/serial.c:93	    pump();
kernel/src/dev/serial.c:94	    bool waits = head != tail && !seized;
kernel/src/dev/serial.c:95	    spin_give(&serial_lock);
kernel/src/dev/serial.c:96	    if (waits) alarm_set(a, clock_after(fifo_depth * CHAR_NS));
kernel/src/dev/serial.c:32	    outb(COM1 + 1, 0x00); /* no interrupts */
```

- 判定：**当たり**。UART の割り込みは切ってあり（32 行）、FIFO が空いたかは見ず、計算した時間（FIFO の深さ×一字の時間）の後に alarm の割り込み（割り込みを閉じた文脈）で吐く。
- 分類：**ポーリング**（時間で回す）。

#### 03(c)-103 `dev/serial.c:111` serial_flush が 100000 回回る

```
kernel/src/dev/serial.c:111	        while (!(inb(COM1 + 5) & LSR_THRE) && ++spin < 100000) pause();
kernel/src/dev/serial.c:112	        if (spin == 100000) break;
```

- 判定：**当たり**。時間でなく回数で止める。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-104 `net/net.c:113,2416`

- 判定：**照らしていない**（net は範囲の外）。

#### 03(c)-105 `sys/clock.c:158,189,309-316,364` PM timer の探り 100000 回、PIT の spin 5000 万回、比べの spin、CMOS の UIP の spin

```
kernel/src/sys/clock.c:156	    /* a port that does not count is no timer */
kernel/src/sys/clock.c:157	    uint32_t first = pm_read();
kernel/src/sys/clock.c:158	    for (unsigned i = 0; i < 100000; i++)
kernel/src/sys/clock.c:159	        if (pm_read() != first) return;
kernel/src/sys/clock.c:189	    while (!(inb(0x61) & 0x20) && ++spins <= 50000000) pause();
kernel/src/sys/clock.c:308	static void compare_wait(void) {
kernel/src/sys/clock.c:309	    while (__atomic_load_n(&compare.through, __ATOMIC_ACQUIRE) != compare.cpus) pause();
kernel/src/sys/clock.c:310	}
kernel/src/sys/clock.c:312	void clock_cpu_compare(void) {
kernel/src/sys/clock.c:313	    if (source != SOURCE_TSC) return;
kernel/src/sys/clock.c:314	    while (__atomic_load_n(&compare_pass, __ATOMIC_ACQUIRE) != 1) pause();
kernel/src/sys/clock.c:315	    compare_first();
kernel/src/sys/clock.c:316	    while (__atomic_load_n(&compare_pass, __ATOMIC_ACQUIRE) != 2) pause();
kernel/src/sys/clock.c:364	    while (cmos(0x0A) & 0x80) pause();
```

- 判定：**当たり**。加えて、364 行の CMOS の UIP の spin には上限が無い（回数も時間も）。309・314・316 行の比べの待ちにも上限が無い（来ない CPU があれば止まる。ただし比べに入るのは `start_wait` が「着いた」と数えた CPU だけ、`smp.c:312 clock_cpus_compare(arrived + 1);`）。
- 分類：**ポーリング**、**決め打ちの数**（100000、5000 万）。

#### 03(c)-106 `arch/apic.c:274-277` xapic_ipi_ready が 100 万回回る

```
kernel/src/arch/apic.c:273	static bool xapic_ipi_ready(void) {
kernel/src/arch/apic.c:274	    for (unsigned i = 0; i < 1000000; i++) {
kernel/src/arch/apic.c:275	        if (!(mmio_read32(lapic + 0x300 / 4) & (1u << 12))) return true;
kernel/src/arch/apic.c:276	        pause();
kernel/src/arch/apic.c:277	    }
```

- 判定：**当たり**。xAPIC の道だけ（x2APIC の道は 291 行の注記のとおり見回らない）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-107 `arch/smp.c:191-194,296,314-317,361-364` cpu_parallel_for の呼ぶ側が割り込みを閉じて spin、起動・停止の spin

```
kernel/src/arch/smp.c:190	        /* Items others claimed are running: they never wait for anything, so this is short. */
kernel/src/arch/smp.c:191	        while (__atomic_load_n(&batch.done, __ATOMIC_ACQUIRE) != count) {
kernel/src/arch/smp.c:192	            vmm_tlb_poll();
kernel/src/arch/smp.c:193	            pause();
kernel/src/arch/smp.c:194	        }
kernel/src/arch/smp.c:203	    irq_restore(flags);
kernel/src/arch/smp.c:296	    while (__atomic_load_n(&answered, __ATOMIC_ACQUIRE) < count && clock_now_ns() < until) pause();
kernel/src/arch/smp.c:314	    while (cpu_online_count() < before + arrived) {
kernel/src/arch/smp.c:315	        if (clock_now_ns() >= until) panic("smp: CPUs that arrived did not come online%s", after);
kernel/src/arch/smp.c:316	        pause();
kernel/src/arch/smp.c:317	    }
kernel/src/arch/smp.c:361	        while (!__atomic_load_n(&c->parked, __ATOMIC_ACQUIRE) && clock_now_ns() < until) {
kernel/src/arch/smp.c:362	            vmm_tlb_poll();
kernel/src/arch/smp.c:363	            pause();
kernel/src/arch/smp.c:364	        }
```

- 判定：**当たり**。191〜194 行は 203 行の `irq_restore` より前、つまり割り込みを閉じた間の spin。
- 分類：**ポーリング**。

#### 03(c)-108 `sys/sched.c:579-590`・`sys/alarm.c:125-128` off_cpu_wait・alarm_cancel の spin

```
kernel/src/sys/sched.c:579	static void off_cpu_wait(struct task *t) {
kernel/src/sys/sched.c:580	    for (;;) {
kernel/src/sys/sched.c:581	        uint64_t f = irq_save();
kernel/src/sys/sched.c:582	        unsigned cpu = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
kernel/src/sys/sched.c:583	        spin_take(&per_cpu(queue, cpu).lock);
kernel/src/sys/sched.c:584	        bool off = t->kei.state == KEI_DEAD || t->kei.state == KEI_NEW;
kernel/src/sys/sched.c:585	        spin_give(&per_cpu(queue, cpu).lock);
kernel/src/sys/sched.c:586	        irq_restore(f);
kernel/src/sys/sched.c:587	        if (off) return;
kernel/src/sys/sched.c:588	        pause();
kernel/src/sys/sched.c:589	    }
kernel/src/sys/sched.c:590	}
kernel/src/sys/alarm.c:125	    while ((firing = __atomic_load_n(&a->firing, __ATOMIC_ACQUIRE)) && firing != self) {
kernel/src/sys/alarm.c:126	        vmm_tlb_poll();
kernel/src/sys/alarm.c:127	        pause();
kernel/src/sys/alarm.c:128	    }
```

- 判定：**当たり**。off_cpu_wait は相手の実行キューの lock を毎回取り直して見る spin。alarm_cancel は割り込みを閉じたまま（122 行 `irq_save`）、別の CPU で走っている alarm の終わりを spin で待つ。
- 分類：**ポーリング**。

#### 03(c)-109 `dev/acpi.c:296,352` frozen の semaphore の spin、sci_wait_idle の spin

```
kernel/src/dev/acpi.c:291	        } else if (timeout != 0xFFFF || now - start < FROZEN_WAIT_NS) {
kernel/src/dev/acpi.c:294	            spin_give(&s->lock);
kernel/src/dev/acpi.c:295	            if (f & (1u << 9)) sti();
kernel/src/dev/acpi.c:296	            pause();
kernel/src/dev/acpi.c:297	            cli();
kernel/src/dev/acpi.c:351	static void sci_wait_idle(void) {
kernel/src/dev/acpi.c:352	    while (__atomic_load_n(&sci_running, __ATOMIC_SEQ_CST)) pause();
kernel/src/dev/acpi.c:353	}
```

- 判定：**当たり**。
- 分類：**ポーリング**。

#### 03(c)-110 `dev/input.c:179-185`・`dev/power.c:96,101` PS/2 の状態の spin、100000 × io_wait

```
kernel/src/dev/input.c:179	static bool wait_status(uint8_t bit, bool set) {
kernel/src/dev/input.c:180	    uint64_t deadline = clock_after(PS2_WAIT_NS);
kernel/src/dev/input.c:181	    for (;;) {
kernel/src/dev/input.c:182	        if (((inb(0x64) & bit) != 0) == set) return true;
kernel/src/dev/input.c:183	        if (clock_now_ns() >= deadline) return false;
kernel/src/dev/input.c:184	        pause();
kernel/src/dev/input.c:185	    }
kernel/src/dev/power.c:96	    if (acpi_reset()) for (int i = 0; i < 100000; i++) io_wait(); /* the reset register, any address space */
kernel/src/dev/power.c:99	        for (int i = 0; i < 100000; i++) io_wait();
kernel/src/dev/power.c:101	    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++) io_wait();
kernel/src/dev/power.c:103	    for (int i = 0; i < 1000000; i++) io_wait();
```

- 判定：**当たり**。input.c は時間で止め、power.c は port 0x80 の書き（`io_wait`）の回数で待つ（時計を使わない）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 03(c)-111・112 `mm/vmm.c:303-310`、`arch/spin.h:26-29`・`mm/vmm.c:224-238`（撃ち落としの spin）

- 判定：**照らしていない**。時間の道ではなく TLB の撃ち落としの待ちで、mm は依頼の範囲の外。

#### 03(c)-113 `common/store/latch.h:24-46`

- 判定：**照らしていない**（範囲の外、時間の道ではない）。

#### 03(c)-114 `sys/events.c:246-268` events_wait が timer と watch を全部、線形に見る

```
kernel/src/sys/events.c:246	        for (struct event_timer **at = &q->timers; *at;) {
kernel/src/sys/events.c:247	            struct event_timer *t = *at;
kernel/src/sys/events.c:248	            if (t->due <= now && n < max) {
kernel/src/sys/events.c:252	                t->due = t->due < CLOCK_NEVER - t->period ? t->due + t->period : CLOCK_NEVER;
kernel/src/sys/events.c:253	                if (t->due <= now) t->due = clock_after(t->period);
kernel/src/sys/events.c:255	            if (t->due < next_due) next_due = t->due;
kernel/src/sys/events.c:265	        for (struct event_watch *w = q->watches; w && n < max; w = w->queue_next) {
kernel/src/sys/events.c:266	            uint32_t ready = handle_ready(w->handle) & w->interest;
kernel/src/sys/events.c:276	        wq_wait_unless(&q->wait.wq, &q->wait.generation, seen, MIN(deadline, next_due));
```

- 判定：**当たり**（範囲の sys/ の中なので照らした）。アプリの timer は事象の行列ごとの連結の一覧で、待ちのたびに全部を線形に見て一番早い期限を探す。カーネルの alarm（CPU ごとの pairing heap、`alarm.c:1-7`）とは別の、もう一つの timer の仕組みになっている。
- 分類：四つの中では **ポーリング** に近い（level で全部を見直す）。数で詰まる点は「一つの持ち主に入っていない timer の二本目」で、四つの外の問題でもある。

#### 03(c)-115 `vm/` の spin と見回り

- 判定：**照らしていない**（vm は範囲の外）。

### 03 (d) 一つの CPU・一つの thread への偏りのうち、時間に関わるもの

#### 03(d)-127 `dev/pm.c:349` kthread_create_on("power", pm_thread, NULL, 0)

```
kernel/src/dev/pm.c:347	/* On the first CPU: the sleep it runs (smp_suspend, wake_sleep) comes back on the CPU the firmware
kernel/src/dev/pm.c:348	   wakes. */
kernel/src/dev/pm.c:349	void pm_init(void) { kthread_create_on("power", pm_thread, NULL, 0); }
```

- 判定：**当たり**。
- 分類：**一つの CPU への寄り**。

#### 03(d)-128 `arch/smp.c:345,380,349-369` smp_suspend・smp_resume が CPU 0 を ASSERT し、CPU を一つずつ止める

```
kernel/src/arch/smp.c:345	    ASSERT(cpu_current_index() == 0);
kernel/src/arch/smp.c:356	    for (unsigned i = 1; i < topology.cpus; i++) {
kernel/src/arch/smp.c:357	        struct cpu_slot *c = &per_cpu(slot, i);
kernel/src/arch/smp.c:358	        if (!c->asleep) continue;
kernel/src/arch/smp.c:359	        sched_cpu_leave(i); /* its tasks to CPU 0's queue; it sees parking once it has none */
kernel/src/arch/smp.c:360	        uint64_t until = clock_after(PARK_TIMEOUT_MS * NS_PER_MS);
kernel/src/arch/smp.c:361	        while (!__atomic_load_n(&c->parked, __ATOMIC_ACQUIRE) && clock_now_ns() < until) {
kernel/src/arch/smp.c:380	    ASSERT(cpu_current_index() == 0);
```

- 判定：**当たり**。止めるのは一つずつで、各 CPU に 2 秒の締め切り。CPU の数に比例して時間が延びる（起動の側は一斉で締め切り一つ、`smp.c:294-318`）。
- 分類：**一つの CPU への寄り**、**ポーリング**、**決め打ちの数**。

#### 03(d)-129 `sys/sched.c:549-563` sched_cpu_leave が task を全部 CPU 0 へ渡す

```
kernel/src/sys/sched.c:549	void sched_cpu_leave(unsigned cpu) {
kernel/src/sys/sched.c:550	    if (!cpu || cpu >= topology.cpus || !per_cpu(queue, cpu).prepared) return;
kernel/src/sys/sched.c:552	    lock2(0, cpu);
kernel/src/sys/sched.c:555	    while (q->rq.count) {
kernel/src/sys/sched.c:556	        struct kei_task *k = q->rq.heap[0];
kernel/src/sys/sched.c:557	        if (kei_migrate(&q->rq, &per_cpu(queue, 0).rq, k) != KEI_OK) panic("kei: cpu%u could not give its tasks back", cpu);
kernel/src/sys/sched.c:558	        __atomic_store_n(&KEI_OWNER(k)->cpu, 0, __ATOMIC_RELEASE);
```

- 判定：**当たり**。呼ぶのは `smp_suspend`（359 行）だけで、眠りの前に全 CPU の task が CPU 0 に集まる。
- 分類：**一つの CPU への寄り**。

#### 03(d)-130 `sys/alarm.c:159,168-186` alarm_cpu_leave が alarm を CPU 0 へ移す

```
kernel/src/sys/alarm.c:159	    if (!self) vector_register(VEC_TIMER, alarm_interrupt);
kernel/src/sys/alarm.c:168	void alarm_cpu_leave(void) {
kernel/src/sys/alarm.c:169	    unsigned self = cpu_current_index();
kernel/src/sys/alarm.c:170	    if (!self) return;
kernel/src/sys/alarm.c:171	    struct alarm_cpu *c = &per_cpu(alarms, self), *to = &per_cpu(alarms, 0);
kernel/src/sys/alarm.c:176	    while (c->root) {
kernel/src/sys/alarm.c:177	        struct alarm *a = c->root;
kernel/src/sys/alarm.c:178	        heap_remove(c, a);
kernel/src/sys/alarm.c:179	        heap_insert(to, a);
kernel/src/sys/alarm.c:180	        __atomic_store_n(&a->armed_on, 1, __ATOMIC_RELEASE);
kernel/src/sys/alarm.c:181	    }
kernel/src/sys/alarm.c:185	    if (sooner) cpu_alarm(0); /* CPU 0 sets its own wake device */
```

- 判定：**一部当たり**。168〜186 行は当たり（止まる CPU の alarm は全部 CPU 0 の heap へ一つずつ移る）。159 行は外れ：これは VEC_TIMER の処理を大域の表（`idt.c:49 static irq_handler_t vec_handlers[256];`）に一度だけ登録する行で、各 CPU の timer の割り込みはそれぞれの CPU で `alarm_interrupt` が自分の heap を処理する（`alarm.c:135-137`）。偏りではない。また、`alarm_resume`（188〜195 行）も CPU 0 の heap だけを置き直す（眠りの後、CPU 0 から戻るため）。
- 分類：**一つの CPU への寄り**（168〜186 行、188〜195 行）。

#### 03(d)-132 系に一つの thread（reaper、vmalloc、ACPI、USB、net、store、合成、keep、prof、記録、画面）

- 時間に関わる所だけ照らした。
  - `sys/prof.c:280`（prof）：一本の thread が窓ごとに起きる（`prof.c:256-260`、`sleep_until(at)`）。**当たり**、**一つの CPU への寄り**ではなく「一本の thread」。
  - `dev/xhci.c:1562`（USB）：`usb_thread` が鍵の繰り返しと全制御器の見回りの拍を持つ（03(c)-92）。**当たり**。
  - ほかは時間の道ではないので照らしていない。
- 分類：**一つの CPU への寄り**（正確には一本の thread への寄り）。

#### 03(d)-135 `sys/sched.c:266-275,306-318,332-348` busiest・choose・rebalance が全 CPU を走査し、NUMA を見ない

```
kernel/src/sys/sched.c:269	    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
kernel/src/sys/sched.c:312	    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
kernel/src/sys/sched.c:339	    for (unsigned cpu = 0; cpu < topology.cpus; cpu++) {
```

- 判定：**当たり**。三つとも `topology.cpus` までの線形の輪で、node・LLC（`topology.c` の `cpu_place`）を見ない。時間の面では、`rebalance` が待つ task のある各 CPU で 4 ms ごとに全 CPU を走査するので、全体の走査は CPU の数の二乗に比例する。
- 分類：四つのうちでは **決め打ちの数**（4 ms）。走査そのものは四つの外（規模の問題）。

### 03 (e) 系に一つの lock のうち、時間に関わるもの

#### 03(e)-143 `sys/sched.c:766,819-820` deadline_wait：sleep_until 全部が一つの待ち行列、wq_take_blocked が線形に歩く

```
kernel/src/sys/sched.c:764	/* The sleeps on a deadline alone: nothing wakes this queue, only the clock (or a kill) takes a
kernel/src/sys/sched.c:765	   sleeper out of it. */
kernel/src/sys/sched.c:766	static struct wait_queue deadline_wait;
kernel/src/sys/sched.c:775	    if (!wq) wq = &deadline_wait;
kernel/src/sys/sched.c:819	        struct task **pp = &wq->head;
kernel/src/sys/sched.c:820	        while (*pp && *pp != t) pp = &(*pp)->wq_next;
kernel/src/sys/sched.c:881	void sleep_until(uint64_t deadline) {
kernel/src/sys/sched.c:883	    wq_sleep(NULL, NULL, 0, NULL, deadline);
```

- 判定：**当たり**。`sleep_until` で眠る全 task（全 CPU の）が一つの `deadline_wait` に入り、その spinlock を取る。起きる時（`wake_fire`→`wq_take_blocked`）は一覧を線形に歩いて自分を外す。期限そのものは CPU ごとの alarm の heap にあるので、詰まるのはこの一覧と lock。上の (b)(c) の「1 ms 眠って見直す」輪（nvme・ahci・pci・xhci・usbhub・pm・fb）は全部この一つの行列を通る。
- 分類：**一つの CPU への寄り**ではなく「系に一つの lock と線形の一覧」。四つのうちでは最も近いのは **一つの CPU への寄り**（全 CPU が一つの資源に集まる）。

#### 03(e)-157 `dev/audio.c:57,322-327,342-347` null の受け手は alarm の割り込みの中で混ぜる

```
kernel/src/dev/audio.c:322	static void null_play(uint64_t now) {
kernel/src/dev/audio.c:323	    uint64_t heard = frames_in(now - null_start), mixed = sink_mixed - null_base;
kernel/src/dev/audio.c:324	    while (mixed <= heard && mix_locked(NULL, NULL_BLOCK, false)) mixed += NULL_BLOCK;
kernel/src/dev/audio.c:325	    played_locked(null_base + MIN(heard, mixed), now);
kernel/src/dev/audio.c:326	    if (mixed > heard) alarm_set(&null_alarm, null_start + ns_for(mixed));
kernel/src/dev/audio.c:342	static void null_fire(struct alarm *a, struct regs *r) {
kernel/src/dev/audio.c:345	    uint64_t f = spin_lock(&audio_lock);
kernel/src/dev/audio.c:346	    if (null_running) null_play(clock_now_ns());
kernel/src/dev/audio.c:347	    spin_unlock(&audio_lock, f);
```

- 判定：**当たり**。音の装置が無いとき（実機は今いつもそう、`audio.c:769` の注記）の受け手は、10 ms ごとの alarm の割り込みの中で `audio_lock` を持って混ぜる。
- 分類：**ポーリング**（時間で回す受け手）、**決め打ちの数**（10 ms の塊）。

### 03 (f) 空の実装・名だけの API のうち、時間に関わるもの

#### 03(f)-170 `sys/kei.c:98,117,165` kei_set_online・kei_charge_detached・kei_set_bonus を呼ぶ所が無い

```
kernel/src/sys/kei.c:98	enum kei_error kei_set_online(struct kei_rq *q, bool online) {
kernel/src/sys/kei.c:115	/* キューの外（BSPで借りて走らせた間）の実サービスを、本人の仮想実行量だけに
kernel/src/sys/kei.c:116	   加える。参照時計はそのCPUの供給ではないので進めない。起床時に先行分として残る。 */
kernel/src/sys/kei.c:117	enum kei_error kei_charge_detached(struct kei_task *t, uint64_t service_ns) {
kernel/src/sys/kei.c:165	enum kei_error kei_set_bonus(struct kei_rq *q, struct kei_task *t, uint64_t bonus_v) {
```

`rg -n "kei_set_online|kei_charge_detached|kei_set_bonus|kei_rq_init" kernel/src --glob "!kei.c" --glob "!kei.h"` の結果は `sched.c:208` の `kei_rq_init` だけで、三つの関数を呼ぶ所は無い。

- 判定：**当たり**。115〜116 行の注記は「BSP で借りて走らせた間」という、今は無い形（`.claude/rules/kernel.md` の「BSP へ回す道…は無い」）を書いている。
- 分類：四つの外（呼ばれない関数、古い注記）。

#### 03(f)-171 `sys/sched.c:208,698` bonus_cap 0、全 task が KEI_WEIGHT_ONE

```
kernel/src/sys/sched.c:208	        if (kei_rq_init(&q->rq, cpu, heap, cap, kei_retention_v, 0) != KEI_OK) panic("kei: queue %u setup failed", cpu);
kernel/src/sys/sched.c:698	    kei_task_init(&t->kei, (uint64_t)t->pid, KEI_WEIGHT_ONE, NULL);
```

- 判定：**当たり**。時間の面では、全 task が同じ重さなので、CPU 時間の配分は「一つの retention（約 0.99 ms）で交替する公平な割り当て」だけになる。音・表示・入力の thread を先にする道は無い。
- 分類：**決め打ちの数**（重さ・bonus が定数）。

#### 03(f)-177 `dev/audio.c:769,738` 実機の音の driver が無い、ABI に時刻の欄が「まだ」無い

```
kernel/src/dev/audio.c:736	/* How far ahead of the sink the stream is, and how much of it was played: its frames before the
kernel/src/dev/audio.c:737	   last ordinal its sink reported, heard at sink_played_at (the ABI has no field for that time
kernel/src/dev/audio.c:738	   yet). */
kernel/src/dev/audio.c:769	    vm_audio_init(); /* no driver for real sound hardware yet */
```

- 判定：**当たり**。音の再生の時刻（`sink_played_at`）が ABI で渡らないので、アプリは音の進みと時計を結べない。
- 分類：**VM の判定の混入**（769 行：共通の音の層の初期化が VM の入口だけを呼ぶ）。738 行は四つの外（ABI の欠け）。

#### 03(f)-184・247 `sys/clock.c:14` HPET は「使える」と書くだけで、実装が無い

```
kernel/src/sys/clock.c:13	   A TSC that is not invariant is used too, with a warning: a general machine without either is
kernel/src/sys/clock.c:14	   one this clock does not serve well yet (the HPET would). */
```

- 判定：**当たり**。`rg -n -i hpet kernel/src` の結果は 6 行で、どれも注記：`sys/clock.c:14`、`dev/iommu.h:163`、`dev/iommu_intel.c:245`、`dev/iommu_amd.c:9`、`dev/iommu_amd.c:205`、それと `dev/iommu.c:104` の記録の文字列 `lost("IOAPIC and HPET IDs")`。HPET を読む・時計や alarm の源にするコードは無い。
- 分類：四つの外（欠け）。

### 03 (g) vm/ の外にある VM の判定のうち、時間に関わるもの

#### 03(g)-190 `arch/apic.c:366` vm_lapic_hz

```
kernel/src/arch/apic.c:8	#include "vm/vm.h"
kernel/src/arch/apic.c:364	        if (!wake_tsc) {
kernel/src/arch/apic.c:365	            from = "the hypervisor's word";
kernel/src/arch/apic.c:366	            if (!(hz = vm_lapic_hz())) {
kernel/src/arch/apic.c:367	                from = "the clock";
kernel/src/arch/apic.c:368	                hz = measure_lapic_hz();
kernel/src/arch/apic.c:369	            }
```

- 判定：**当たり**。実機の LAPIC timer の速さの道の中で、まず hypervisor に聞く。`vm/vm.h` は「実機では即座に戻る」入口（`.claude/rules/map.md`）なので、実機では 0 が返り測りへ進むが、実機のコードの中に VM の段がある。
- 分類：**VM の判定の混入**。

#### 03(g)-191 `sys/clock.c:3-12,45,92-94,121-125,389-397,433,440` 核の時計の中心に hypervisor の参照

```
kernel/src/sys/clock.c:1	/* The clock (sys/clock.h). Two counters can hold it:
kernel/src/sys/clock.c:3	   - the hypervisor's reference time (vm/vm.h), which it keeps right for the guest across its own
kernel/src/sys/clock.c:4	     scheduling and migrations: in a virtual machine that offers one, it is the clock;
kernel/src/sys/clock.c:22	#include "vm/vm.h"
kernel/src/sys/clock.c:30	static enum { SOURCE_NONE, SOURCE_HYPERVISOR, SOURCE_TSC } source;
kernel/src/sys/clock.c:45	static uint64_t hv_base;
kernel/src/sys/clock.c:92	    case SOURCE_HYPERVISOR:
kernel/src/sys/clock.c:93	        if (!vm_clock_ns(&ns)) return 0;
kernel/src/sys/clock.c:94	        return (int64_t)(ns - hv_base) > 0 ? ns - hv_base : 0;
kernel/src/sys/clock.c:121	bool clock_hypervisor_at(uint64_t ns, uint64_t *reference_ns) {
kernel/src/sys/clock.c:122	    if (source != SOURCE_HYPERVISOR) return false;
kernel/src/sys/clock.c:389	    if (vm_clock_ns(&ns)) {
kernel/src/sys/clock.c:390	        source = SOURCE_HYPERVISOR;
kernel/src/sys/clock.c:391	        hv_base = ns;
kernel/src/sys/clock.c:392	        tsc_hz = vm_tsc_hz();
kernel/src/sys/clock.c:396	        uint64_t hz = vm_tsc_hz();
kernel/src/sys/clock.c:397	        rate_from = "the hypervisor's word";
kernel/src/sys/clock.c:433	    vm_clock_resume();
kernel/src/sys/clock.c:440	        if (vm_clock_ns(&ns)) hv_base = ns - stopped_ns;
```

- 判定：**当たり**。時計の源の列挙子に `SOURCE_HYPERVISOR` があり、`clock_now_ns`（毎回の読み、91〜95 行の `switch`）が源で分かれる。VM では TSC の道を通らない。実機でも `clock_init` と `clock_resume` は毎回 VM の入口を呼ぶ。
- 分類：**VM の判定の混入**。

#### 03(g)-192 `sys/clock.h:7-9,44-46` 時計の公開の面に VM の源がある

```
kernel/src/sys/clock.h:7	   The counter is the hypervisor's reference counter (vm/vm.h), else the TSC (every CPU's
kernel/src/sys/clock.h:8	   agreeing, checked as they start). Its rate comes from a
kernel/src/sys/clock.h:9	   reference of known frequency (the hypervisor's word, CPUID, the ACPI PM timer, the PIT, the
kernel/src/sys/clock.h:44	/* The hypervisor's reference time (ns, its own epoch) when the clock reads ns; false when the
kernel/src/sys/clock.h:45	   clock is not the hypervisor's. */
kernel/src/sys/clock.h:46	bool clock_hypervisor_at(uint64_t ns, uint64_t *reference_ns);
```

- 判定：**当たり**。
- 分類：**VM の判定の混入**。

#### 03(g)-193 `arch/pstate.c:40,44,74,94,105` vm_hosted で分ける

```
kernel/src/arch/pstate.c:31	static bool hypervisor;
kernel/src/arch/pstate.c:40	    hypervisor = vm_hosted();
kernel/src/arch/pstate.c:44	    if (hypervisor) method = PSTATE_NONE;
kernel/src/arch/pstate.c:74	        return hypervisor ? "hypervisor owns the clock" : "no P-state control found";
kernel/src/arch/pstate.c:94	    if (have_aperf && !hypervisor)
kernel/src/arch/pstate.c:105	bool pstate_sampling(void) { return have_aperf && !hypervisor; }
```

- 判定：**当たり**。時間の面では、APERF/MPERF の標本（`pstate_sample`、1 秒の窓、110 行 `if (s->tsc && now - s->at < NS_PER_S) return;`）を VM では止める判定が実機のファイルにある。
- 分類：**VM の判定の混入**、**決め打ちの数**（1 秒の窓、所見には無い）。

#### 03(g)-197 `sys/syscall.c:2272-2290,2904-2918`・`sys/sched.h:50` ABI に hypervisor の時計が出ている

```
kernel/src/sys/syscall.c:2272	/* The process's mapping of the hypervisor's clock page (read-only, a guard page after it), made
kernel/src/sys/syscall.c:2273	   at its first ask; 0 without one. */
kernel/src/sys/syscall.c:2274	static uint64_t clock_page_of(struct task *owner) {
kernel/src/sys/syscall.c:2277	    uint64_t reference = vm_clock_page();
kernel/src/sys/syscall.c:2902	    case SYS_CLOCK_INFO: {
kernel/src/sys/syscall.c:2906	        info.uptime_ms = clock_now_ns() / NS_PER_MS;
kernel/src/sys/syscall.c:2908	        if (clock_hypervisor_at(at, &reference_ns)) {
kernel/src/sys/syscall.c:2909	            if ((reference = clock_page_of(task_process()))) {
kernel/src/sys/syscall.c:2910	                info.flags = N_CLOCK_HV_REFERENCE;
kernel/src/sys/syscall.c:2911	                info.reference_page = reference;
kernel/src/sys/syscall.c:2912	                info.reference_sample_100ns = reference_ns / 100;
kernel/src/sys/syscall.c:2914	        } else if (clock_tsc_at(at, &info.tsc_sample)) {
kernel/src/sys/syscall.c:2915	            info.flags = N_CLOCK_USER_TSC;
kernel/src/sys/syscall.c:2916	            info.tsc_hz = clock_tsc_hz();
kernel/src/sys/sched.h:50	    uint64_t clock_page;      /* a process's mapping of the hypervisor clock page: set once under space's lock, read atomically */
```

ABI と libc の側：

```
common/abi.h:273	struct n_clockinfo {
common/abi.h:274	    uint32_t version, flags;
common/abi.h:275	    uint64_t tsc_hz, tsc_sample, uptime_ms;
common/abi.h:276	    uint64_t reference_page, reference_sample_100ns;
common/abi.h:277	};
common/abi.h:278	#define N_CLOCK_USER_TSC 1u
common/abi.h:279	#define N_CLOCK_HV_REFERENCE 2u
common/abi.h:280	/* Read-only Hyper-V reference page. Hypervisor updates use sequence protocol. */
common/abi.h:281	struct n_reference_tsc {
user/libc/sys.c:101	    if ((user_clock.flags & N_CLOCK_HV_REFERENCE) && user_clock.reference_page) {
user/libc/sys.c:102	        const struct n_reference_tsc *p = (const struct n_reference_tsc *)(uintptr_t)user_clock.reference_page;
```

- 判定：**当たり**。Hyper-V の参照の頁の形（`struct n_reference_tsc`）がネイティブ ABI の型として `common/abi.h` にあり、libc がそれを読む。task の記録にも欄がある。
- 分類：**VM の判定の混入**（ABI と libc まで）。

#### 03(g)-198 `arch/apic.c:3-4` Hyper-V Gen 2 の注釈

```
kernel/src/arch/apic.c:1	/* Local APIC. Legacy device IRQs still come from the 8259 PIC, routed through LINT0 (virtual wire
kernel/src/arch/apic.c:2	   mode); each CPU's alarms are woken by its LAPIC timer (sys/alarm.c), which every machine has,
kernel/src/arch/apic.c:3	   Hyper-V Generation 2 VMs included, which have no PIT. Vectors 0x30 and up are LAPIC-delivered
kernel/src/arch/apic.c:4	   (the timer, IPIs, and Hyper-V's SynIC) and need a LAPIC EOI. */
```

- 判定：**当たり**（注釈だけ）。
- 分類：**VM の判定の混入**（注釈の上で、実機の部品の説明に VM の事情を置く）。

#### 03(g)-195 のうち時計に関わる所：`arch/cpu.c:5` 「WHPX には PCID・MWAIT・不変 TSC が無い」

```
kernel/src/arch/cpu.c:4	   the floor is Zen 4) and stops at boot when one is missing. The optional features are the ones a
kernel/src/arch/cpu.c:5	   hypervisor or a model may lack (WHPX shows no PCID, MWAIT or invariant TSC bit): their users
kernel/src/arch/cpu.c:6	   choose a path by them. Leaf 0xD (the XSAVE layout) is arch/fpu.c's, the topology leaves (0xB,
```

- 判定：**当たり**。下限を Zen 4 としながら、不変 TSC を「任意の機能」に置く理由が WHPX である。そのため `clock.c` は不変でない TSC の道（13 行、413 行の警告）を持つ。
- 分類：**VM の判定の混入**（VM の欠けが実機の必須の機能の線を決めている）。

### 03 (h) 古い道のうち、時間に関わるもの

#### 03(h)-205 `sys/clock.c:141-194,363-384` PIT の測り、ACPI PM timer（port I/O）、CMOS RTC

```
kernel/src/sys/clock.c:141	static uint32_t pm_read(void) { return inl(pm_port) & pm_mask; }
kernel/src/sys/clock.c:150	        if (f->x_pm_tmr.space != 1) return; /* in memory space: not read here */
kernel/src/sys/clock.c:177	static uint64_t measure_pit(void) {
kernel/src/sys/clock.c:178	    uint8_t p61 = inb(0x61);
kernel/src/sys/clock.c:181	    outb(0x43, 0xB0);                 /* channel 2, low then high byte, mode 0 */
kernel/src/sys/clock.c:347	static uint8_t cmos(uint8_t reg) {
kernel/src/sys/clock.c:348	    outb(0x70, reg);
kernel/src/sys/clock.c:349	    return inb(0x71);
kernel/src/sys/clock.c:363	int64_t rtc_read_unix(void) {
kernel/src/sys/clock.c:364	    while (cmos(0x0A) & 0x80) pause();
```

- 判定：**当たり**。PM timer は I/O 空間だけを読み、記憶の空間の PM timer（150 行）は読まない。壁の時計は CMOS の秒から（415 行）。
- 分類：**決め打ちの数**（port 0x61・0x43・0x42・0x70・0x71）。spin は **ポーリング**（03(c)-105）。

#### 03(h)-211 `sys/clock.c:13,396-407,413` 不変 TSC が無いときの道、CPUID 15h・PIT の道

```
kernel/src/sys/clock.c:396	        uint64_t hz = vm_tsc_hz();
kernel/src/sys/clock.c:397	        rate_from = "the hypervisor's word";
kernel/src/sys/clock.c:398	        if (!hz && (hz = cpu_features.tsc_crystal_hz)) rate_from = "CPUID 15h";
kernel/src/sys/clock.c:399	        if (!hz) {
kernel/src/sys/clock.c:400	            pm_timer_find();
kernel/src/sys/clock.c:401	            if (pm_port && (hz = median_of(measure_pm))) {
kernel/src/sys/clock.c:406	        if (!hz && (hz = median_of(measure_pit))) rate_from = "the PIT";
kernel/src/sys/clock.c:407	        if (!hz) panic("clock: no TSC rate: no hypervisor, CPUID, PM timer or PIT gives one");
kernel/src/sys/clock.c:412	        kprintf("clock: TSC %lu kHz by %s%s\n", hz / 1000, rate_from,
kernel/src/sys/clock.c:413	                cpu_features.invariant_tsc ? "" : "; not invariant, so the clock drifts as its rate changes");
```

- 判定：**当たり**。TSC の速さの源は順に hypervisor → CPUID 15h → PM timer → PIT。CPUID 15h（TSC の結晶の周波数）は Intel の葉で、Zen 4 が答えるかはこの照らしでは確かめていない（未確認）。答えなければ実機（7600X）は PM timer で測る。
- 分類：**VM の判定の混入**（396〜397 行が先頭）、**決め打ちの数**（PM timer・PIT の速さの定数 24〜25 行）。

#### 03(h)-212 `arch/apic.c:361-376` TSC deadline が無いときの one-shot の数

- 原文は 03(b)-58・59・03(g)-190。判定：**当たり**。分類：**VM の判定の混入**（366 行）、**決め打ちの数**（339 行、350 行）。

#### 03(h)-210 のうち時間に関わる所：`arch/apic.c:273-279` ICR の busy-poll

- 原文は 03(c)-106。判定：**当たり**。分類：**ポーリング**。

### 03 (i) 時計の入口と単位

#### 03(i)-220 `sys/clock.c:88` clock_now_ns：ns。TSC × mult / 2^32、または hypervisor の参照

```
kernel/src/sys/clock.c:88	uint64_t clock_now_ns(void) {
kernel/src/sys/clock.c:89	    uint64_t ns;
kernel/src/sys/clock.c:90	    switch (source) {
kernel/src/sys/clock.c:91	    case SOURCE_TSC: return ns_of(tsc_now());
kernel/src/sys/clock.c:92	    case SOURCE_HYPERVISOR:
kernel/src/sys/clock.c:93	        if (!vm_clock_ns(&ns)) return 0;
kernel/src/sys/clock.c:94	        return (int64_t)(ns - hv_base) > 0 ? ns - hv_base : 0;
kernel/src/sys/clock.c:95	    default: return 0;
kernel/src/sys/clock.c:96	    }
kernel/src/sys/clock.c:97	}
```

- 判定：**当たり**。
- 分類：**VM の判定の混入**。

#### 03(i)-221 `sys/clock.c:101` clock_tsc_at：ns から TSC の数へ（「変換が二度入る」）

```
kernel/src/sys/clock.c:101	bool clock_tsc_at(uint64_t ns, uint64_t *tsc) {
kernel/src/sys/clock.c:113	        unsigned __int128 ticks = ((unsigned __int128)delta * inverse + ((1u << 24) - 1)) >> 24;
kernel/src/sys/clock.c:116	        if (__atomic_load_n(&tsc_offsets, __ATOMIC_ACQUIRE)) *tsc += (uint64_t)this_cpu(tsc_offset);
```

- 判定：**当たり**。alarm の期限は ns で持ち、TSC deadline に入れるたびに TSC の数へ換える（`apic.c:398`）。TSC を読むたびに ns へ換える（`clock.c:78`）。換えは掛け算一つずつで、切り上げ（113 行）で「早く来ない」を守る。
- 分類：四つの外（単位の設計）。

#### 03(i)-222 `sys/clock.c:121` clock_hypervisor_at

- 原文は 03(g)-191。判定：**当たり**。分類：**VM の判定の混入**。

#### 03(i)-223 `sys/clock.h:32-40` clock_after・clock_spin、clock_tsc_hz

```
kernel/src/sys/clock.h:32	static inline uint64_t clock_after(uint64_t ns) {
kernel/src/sys/clock.h:33	    uint64_t now = clock_now_ns();
kernel/src/sys/clock.h:34	    return ns >= CLOCK_NEVER - now ? CLOCK_NEVER : now + ns;
kernel/src/sys/clock.h:35	}
kernel/src/sys/clock.h:38	static inline void clock_spin(uint64_t ns) {
kernel/src/sys/clock.h:39	    for (uint64_t until = clock_after(ns); clock_now_ns() < until;) __asm__ volatile("pause");
kernel/src/sys/clock.h:40	}
```

- 判定：**当たり**（ns で揃っている。所見も問題を書いていない）。`clock_spin` は眠れない所の待ちの共通の口で、それ自体が **ポーリング** の部品。

#### 03(i)-224 `sys/clock.c:363-384` clock_wall_ns・clock_wall_seconds（ns と s）、rtc_read_unix（s）、time_to_parts

```
kernel/src/sys/clock.c:363	int64_t rtc_read_unix(void) {
kernel/src/sys/clock.c:415	    wall_offset_ns = rtc_read_unix() * (int64_t)NS_PER_S - (int64_t)clock_now_ns();
kernel/src/sys/clock.c:453	int64_t clock_wall_ns(void) { return (int64_t)clock_now_ns() + __atomic_load_n(&wall_offset_ns, __ATOMIC_RELAXED); }
kernel/src/sys/clock.c:455	int64_t clock_wall_seconds(void) {
kernel/src/sys/clock.c:464	void time_to_parts(int64_t t, struct tm_parts *p) {
```

- 判定：**一部当たり**。事実（壁の時計は CMOS の秒から、ns と s の口がある）は当たり。行番号は 363〜384 が `rtc_read_unix` だけで、`clock_wall_ns`・`clock_wall_seconds`・`time_to_parts` は 453〜483 行。起動時の壁の時刻は秒の粒（CMOS）で、ns の口でも起動の瞬間の 1 秒未満の端数は分からない。
- 分類：四つの外（単位）。

#### 03(i)-225 `arch/apic.c:394` lapic_wake_at：ns から TSC か、LAPIC の数へ

```
kernel/src/arch/apic.c:394	void lapic_wake_at(uint64_t ns) {
kernel/src/arch/apic.c:395	    if (wake_tsc) {
kernel/src/arch/apic.c:398	        wrmsr(IA32_TSC_DEADLINE, ns != CLOCK_NEVER && clock_tsc_at(ns, &tsc) && tsc ? tsc : 0);
kernel/src/arch/apic.c:401	    if (ns == CLOCK_NEVER) {
kernel/src/arch/apic.c:405	    uint64_t now = clock_now_ns(), delta = ns > now ? ns - now : 0;
kernel/src/arch/apic.c:406	    unsigned __int128 count = ((unsigned __int128)delta * wake_mult) >> 32;
kernel/src/arch/apic.c:407	    lapic_write(LAPIC_TIMER_INIT, count >= 0xFFFFFFFF ? 0xFFFFFFFF : count ? (uint32_t)count : 1);
```

- 判定：**当たり**。一回数えは 2^32−1 数えで頭打ち（407 行）で、それより先の alarm は途中で一度起きて置き直す（`alarm.c:5-7` の注記「It may come early … then nothing is due, and it is set again」）。
- 分類：**決め打ちの数**（0xFFFFFFFF は装置の幅）。

#### 03(i)-226 `arch/apic.c:339,350` 20 ms で測り、わざと 1/8192 遅らせる

- 03(b)-58・59 と同じ。判定：**一部当たり**（遅らせは一回数えの道だけ）。分類：**決め打ちの数**。

#### 03(i)-227・228 `sys/alarm.c`・`sys/sched.c` の ns の期限

```
kernel/src/sys/alarm.c:107	void alarm_set(struct alarm *a, uint64_t at) {
kernel/src/sys/sched.c:842	bool wq_wait_until(struct wait_queue *wq, uint64_t deadline) {
kernel/src/sys/sched.c:881	void sleep_until(uint64_t deadline) {
```

- 判定：**当たり**（ns で揃っている。所見も問題を書いていない）。

#### 03(i)-229 `sys/syscall.h:11-17` syscall_deadline(int64 timeout_ms)、address_deadline：ms を ns に換える

```
kernel/src/sys/syscall.h:8	/* An app's timeout (the ABI's milliseconds: negative for no limit, 0 for no wait) as the deadline
kernel/src/sys/syscall.h:11	static inline uint64_t syscall_deadline(int64_t timeout_ms) {
kernel/src/sys/syscall.h:12	    if (timeout_ms < 0 || (uint64_t)timeout_ms >= CLOCK_NEVER / NS_PER_MS) return CLOCK_NEVER;
kernel/src/sys/syscall.h:13	    return clock_after((uint64_t)timeout_ms * NS_PER_MS);
kernel/src/sys/syscall.h:16	static inline uint64_t address_deadline(unsigned timeout_ms) {
kernel/src/sys/syscall.h:17	    return syscall_deadline(timeout_ms == UINT32_MAX ? -1 : (int64_t)timeout_ms);
```

- 判定：**当たり**。アプリの待ちは ABI で ms の粒。
- 分類：**決め打ちの数**（ABI の粒が 1 ms）。

#### 03(i)-230 `sys/syscall.c:2845,2831,2667,1337,788,2882-2884` が ms で受ける

```
kernel/src/sys/syscall.c:2845	    case SYS_SLEEP: sleep_until(syscall_deadline((int64_t)a)); ret = 0; break;
kernel/src/sys/syscall.c:2831	    r = events_wait(queue, uout, max, syscall_deadline(timeout_ms));
kernel/src/sys/syscall.c:2667	    r = proc_app_result(app, syscall_deadline(timeout_ms), &status);
kernel/src/sys/syscall.c:1337	    uint64_t deadline=syscall_deadline(timeout_ms);
kernel/src/sys/syscall.c:788	    uint64_t deadline=syscall_deadline(timeout_ms);
kernel/src/sys/syscall.c:2882	        else if (r->rax == SYS_PCI_ACPI_NOTIFY_TAKE) ret = pci_user_acpi_notify_take(f, syscall_deadline((int64_t)b));
kernel/src/sys/syscall.c:2884	        else ret = pci_user_interrupt_take(f,syscall_deadline((int64_t)b));
```

- 判定：**当たり**。`gui/wm.c:3339` は範囲の外で照らしていない。所見に無いが同じ形：`SYS_UPTIME`（2846 行 `ret = (int64_t)(clock_now_ns() / NS_PER_MS);`）も ms で返す。
- 分類：**決め打ちの数**（ABI の粒）。

#### 03(i)-231 `net/net.c:2613,2658,2683` (int)d で期限を 32 bit に切る

- 判定：**照らしていない**（net は範囲の外）。

#### 03(i)-232 `sys/events.c:178` events_timer だけが ns で受ける

```
kernel/src/sys/events.c:178	int events_timer(struct handle *queue, uint64_t ns, bool repeat, uint64_t token) {
```

- 判定：**当たり**。同じ ABI の中で、timer の登録は ns、待ち（2831 行）は ms。
- 分類：四つの外（単位の不揃い）。

#### 03(i)-233 `sys/syscall.c:226,228,246,2906` uptime_ms、cpu_ms、start_ms：ms で出す

```
kernel/src/sys/syscall.c:226	        p->cpu_ms = sched_task_cpu_ns(t) / NS_PER_MS;
kernel/src/sys/syscall.c:228	        p->start_ms = t->start_ns / NS_PER_MS;
kernel/src/sys/syscall.c:246	    si->uptime_ms = clock_now_ns() / NS_PER_MS;
kernel/src/sys/syscall.c:2906	        info.uptime_ms = clock_now_ns() / NS_PER_MS;
```

- 判定：**当たり**。
- 分類：四つの外（単位）。

#### 03(i)-234 `gui/screenmode.c:382` trial_ms

- 判定：**照らしていない**（gui は範囲の外）。

#### 03(i)-235 `sys/syscall.c:2904-2918` SYS_CLOCK_INFO が uptime_ms、reference_sample_100ns、tsc_sample を混ぜる

- 原文は 03(g)-197。判定：**当たり**。一つの答えに ms、100 ns、TSC の数の三つの単位が入る。libc は ms の起点に 100 ns か TSC の差を足して µs を作る（`user/libc/sys.c:113`、`:124-125`）。
- 分類：**VM の判定の混入**（100 ns の単位は Hyper-V の参照の単位）、ほかは四つの外（単位）。

#### 03(i)-236 `dev/xhci.c:822,838,1335`・`dev/usbhub.c:96,129` usb_bulk の timeout_ms、power_ms、IMOD の単位

```
kernel/src/dev/xhci.c:822	int usb_bulk(struct usb_device *d, uint8_t address, void *buf, unsigned len, unsigned timeout_ms) {
kernel/src/dev/xhci.c:838	    if (!hc_wait(&ep->sync_done, clock_after(timeout_ms * NS_PER_MS))) {
kernel/src/dev/xhci.c:1335	    wr32(hc->rt, IMOD, 1000); /* at most one interrupt per 250 us */
kernel/src/dev/usbhub.c:96	    h->power_ms = desc[5] * 2u < 100 ? 100 : desc[5] * 2u;
kernel/src/dev/usbhub.c:129	    sleep_until(clock_after(h->power_ms * NS_PER_MS));
```

- 判定：**当たり**。核の中の口（`usb_bulk`）が ms を受ける。838 行の `timeout_ms * NS_PER_MS` は `unsigned` と `uint64_t` の掛け算で、`NS_PER_MS` が `uint64_t`（`clock.h:18`）なので桁あふれはしない。
- 分類：四つの外（単位の不揃い）。1335 行は **決め打ちの数**。

#### 03(i)-237 `dev/pci.c:695-707` top_ms、COMPLETION_FLOOR_MS

- 原文は 03(b)-76。判定：**当たり**（ms で持ち、708 行で ns に換える）。分類：四つの外（単位）、**決め打ちの数**（仕様の値）。

#### 03(i)-238 `dev/acpi.c:253-280` uacpi_kernel_stall（µs）、uacpi_kernel_sleep（ms）、semaphore の u16 の ms

```
kernel/src/dev/acpi.c:252	uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) { return clock_now_ns(); }
kernel/src/dev/acpi.c:253	void uacpi_kernel_stall(uacpi_u8 us) { clock_spin(us * NS_PER_US); }
kernel/src/dev/acpi.c:254	void uacpi_kernel_sleep(uacpi_u64 ms) {
kernel/src/dev/acpi.c:255	    uint64_t ns = MIN(ms, CLOCK_NEVER / NS_PER_MS) * NS_PER_MS;
kernel/src/dev/acpi.c:256	    if (!frozen) sleep_until(clock_after(ns));
kernel/src/dev/acpi.c:257	    else clock_spin(ns);
kernel/src/dev/acpi.c:278	uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle h, uacpi_u16 timeout) {
kernel/src/dev/acpi.c:280	    uint64_t start = clock_now_ns(), end = timeout == 0xFFFF ? CLOCK_NEVER : start + timeout * NS_PER_MS;
```

- 判定：**当たり**。単位は uACPI（上流）の口が決めるもので、核の側はそれを ns に換えて受けている。
- 分類：四つの外（上流の口の単位）。

#### 03(i)-239 `vm/hv/vmbus.h:125`・`vm/hv/vmbus.c:304-305` vmbus_wait(ms)

- 判定：**照らしていない**（vm は範囲の外）。

#### 03(i)-240 `arch/smp.c:207,295,360` cpu_max_job_us、START・PARK_TIMEOUT_MS × NS_PER_MS

```
kernel/src/arch/smp.c:207	uint64_t cpu_max_job_us(void) { return __atomic_load_n(&max_job_ns, __ATOMIC_RELAXED) / NS_PER_US; }
kernel/src/arch/smp.c:295	    uint64_t until = clock_after(START_TIMEOUT_MS * NS_PER_MS);
kernel/src/arch/smp.c:360	        uint64_t until = clock_after(PARK_TIMEOUT_MS * NS_PER_MS);
```

- 判定：**当たり**。
- 分類：四つの外（単位）、**決め打ちの数**（2000 ms）。

#### 03(i)-241 `sys/prof.c:259` period_ms

```
kernel/src/sys/prof.c:259	        at += period_ms * NS_PER_MS;
```

- 判定：**当たり**。分類：四つの外（単位）。

#### 03(i)-242 `dev/audio.c:315-317` frame と ns を 48 kHz 固定で換える

```
kernel/src/dev/audio.c:314	/* The frames that play in ns at AUDIO_RATE (rounded down), and the ns that frames take (up). */
kernel/src/dev/audio.c:315	static uint64_t frames_in(uint64_t ns) { return ns / NS_PER_S * AUDIO_RATE + ns % NS_PER_S * AUDIO_RATE / NS_PER_S; }
kernel/src/dev/audio.c:316	static uint64_t ns_for(uint64_t frames) {
kernel/src/dev/audio.c:317	    return frames / AUDIO_RATE * NS_PER_S + (frames % AUDIO_RATE * NS_PER_S + AUDIO_RATE - 1) / AUDIO_RATE;
```

- 判定：**当たり**。null の受け手の拍は核の時計（`clock_now_ns`）で、装置の拍ではない。装置の rate は 48000 に固定。
- 分類：**決め打ちの数**。

#### 03(i)-243 `gui/wm.c:2484` 周期を mHz から整数の割り算で出す

- 判定：**照らしていない**（gui は範囲の外）。

#### 03(i)-244 `dev/serial.c:19` CHAR_NS × fifo_depth

- 03(b)-65 と 03(c)-102 と同じ。判定：**一部当たり**（baud は同じファイルで固定なので、今はずれない）。分類：**決め打ちの数**。

#### 03(i)-245 `vm/e1000.c:47-48`

- 判定：**照らしていない**（vm は範囲の外）。

#### 03(i)-246 `dev/nvme.c:368,422` CAP.TO（500 ms の単位）、µs

```
kernel/src/dev/nvme.c:368	    c->ready_ns = MAX(to, 4u) * 500 * NS_PER_MS; /* 500 ms units, at least 2 s */
kernel/src/dev/nvme.c:421	        uint64_t us = c->rtd3e < 1000000 ? 1000000 : c->rtd3e > 10000000 ? 10000000 : c->rtd3e;
kernel/src/dev/nvme.c:422	        for (uint64_t deadline = clock_after(us * NS_PER_US);
kernel/src/dev/nvme.c:423	             (rd32(c, CSTS) >> 2 & 3) != 2 && clock_now_ns() < deadline;)
kernel/src/dev/nvme.c:424	            pause();
```

- 判定：**当たり**。421 行は装置の RTD3E（µs）を 1〜10 秒に挟む定数があり、422〜424 行はその間 pause で spin する（所見には無い）。
- 分類：四つの外（装置の単位）。421 行は **決め打ちの数**、422〜424 行は **ポーリング**。

---

## 02（gui・vm）の所見のうち、時間に関わるもの

02 の (b)「vm/ の外が vm/ に頼っている所」の「CPU」と「時計」の行、(c) の「時計」の節、gui の時間の行。

#### 02-382 `arch/apic.c:8`（include）、`:365-366`（LAPIC の拍の源に vm_lapic_hz）、`:3-4`（注記）

- 原文は 03(g)-190 と 03(g)-198。判定：**当たり**。分類：**VM の判定の混入**。

#### 02-383 `arch/pstate.c:16`（include）、`:31、40、44、74、94、105`（VM なら P-state と APERF の標本を止める）

- 原文は 03(g)-193（31 行 `static bool hypervisor;` を含む）。判定：**当たり**。分類：**VM の判定の混入**。

#### 02-387 `sys/clock.c:22`（include）、`:43-45`、`:93`、`:121`、`:389-397`、`:433`、`:440`

- 原文は 03(g)-191。43〜45 行：

```
kernel/src/sys/clock.c:43	/* The hypervisor's time (ns) at which the clock read 0, modulo 2^64: after a hibernation image is
kernel/src/sys/clock.c:44	   restored the hypervisor's time began again, below the clock's, and this is "negative". */
kernel/src/sys/clock.c:45	static uint64_t hv_base;
```

- 判定：**当たり**。分類：**VM の判定の混入**。

#### 02-388 `sys/clock.h:7-9`、`:44-46`

- 原文は 03(g)-192。判定：**当たり**。分類：**VM の判定の混入**。

#### 02-389・390・391・392 `sys/syscall.c:5,2272-2293,2908-2910`、`sys/sched.h:50`、`common/abi.h:273-286`、`user/libc/sys.c:91-127`

- 原文は 03(g)-197。`clock_page_of` は 2274〜2293 行（2277 行が `vm_clock_page()`）。判定：**当たり**。分類：**VM の判定の混入**（ABI と libc まで）。

#### 02-377 `boot.c:152`（clock_init の注記）

```
kernel/src/boot.c:152	    clock_init(); /* the hypervisor's reference time, or the TSC by the PM timer's measure */
```

- 判定：**当たり**（注記だけ）。注記は TSC の速さの源を「PM timer」と書くが、`clock.c:396-406` の順は hypervisor → CPUID 15h → PM timer → PIT で、注記は一部しか書いていない。
- 分類：**VM の判定の混入**（注記の上）。

#### 02-398 `dev/fb.c:146` と `:391-393`（WHPX の制約と測定で実機の既定を決める）、`:446`（vm_display_pm）

```
kernel/src/dev/fb.c:146	 * The framebuffer writes remain scalar: WHPX does not emulate SIMD MMIO. The 10 bpc simulation
kernel/src/dev/fb.c:391	    /* Real WHPX fbbench measured 2.4x slower with IPI/tile overhead. Keep
kernel/src/dev/fb.c:392	       the known-faster scalar default; explicit fbparallel/fbbench can still
kernel/src/dev/fb.c:393	       exercise the work on other CPUs without claiming a speedup. */
kernel/src/dev/fb.c:446	    vm_display_pm(&display_pm);
```

- 判定：**当たり**。時間の面では、画面への写しの速さの既定（直列の写し）を WHPX の上で測った値（2.4 倍遅い）で決めている。
- 分類：**VM の判定の混入**。

#### 02-229 `dev/fb.c:208-210` presents_drain が 100 µs ごとに見回る

- 原文は 03(b)-66。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 02-468・469 （c）の時計：「vm/ が無いと、clock.c は TSC を CPUID 15h、PM timer、PIT で測ります（396-407）」「WHPX は invariant TSC の bit を見せません（cpu.c:5 の注記）」

- 原文は 03(h)-211 と 03(g)-195。判定：**当たり**。ただし 396 行は `vm_tsc_hz()` が先頭で、「vm/ が無いと」の前提の下での話。
- 分類：**VM の判定の混入**。

#### 02-61・65・67・193・195 `gui/wm.c`・`gui/screenmode.c` の拍（60 Hz の代わりの値、EV_FRAME、FALLBACK_MHZ、試しの 15 秒）

- 判定：**照らしていない**（gui は範囲の外）。

#### 02-283〜312・332・349・355 `vm/` の中の時計（Hz、参照 TSC 頁、rdp の 30 ms・10 ms の見回り、ata の 200 万回、ac97 の 10 ms）

- 判定：**照らしていない**（vm は範囲の外）。

---

## 01（fs・common/store・net）の所見のうち、時間に関わるもの

#### 01-381 「NVMe は … MSI があっても 10 ms ごとに起きます（167-168）」

```
kernel/src/dev/nvme.c:167	        /* with MSI the interrupt wakes it, the 10 ms only in case one is lost; polled each ms */
kernel/src/dev/nvme.c:168	        wq_wait_unless(&c->wq, &c->kicks, seen, clock_after(c->vector ? 10 * NS_PER_MS : NS_PER_MS));
```

- 判定：**当たり**。命令を待つ間、MSI があっても 10 ms の期限を毎回置くので、長い命令の間は 10 ms ごとに起きる。
- 分類：**ポーリング**、**決め打ちの数**。

#### 01-20・31・298・305・324〜326・359 `fs/store.c` の 100 ms の見回り、時刻の単位、`net/net.c` の 2 ms・50 ms、TCP の alarm と RTO、link の 1 秒ごとの見回り

- 判定：**照らしていない**（fs・net は範囲の外）。01 自身は、fs の時刻（壁時計の ns と単調時計の `clock_after`）と net の時刻（clock の ns）を「揃っている」「良い作り」と書いている（01 の 31 行、324〜326 行）。

---

## 照らしている間に見えた、監査に無い事実（原文の行つき）

所見ではなく、この照らしで読んだ行に見えたこと。判定や直し方は書かない。

1. `kernel/src/sys/clock.c:364` の CMOS の UIP の待ちには上限が無い（03(c)-105 に含めたが、所見は「回数で止める」の列に置いている。実際は回数でも時間でも止まらない）。
   ```
   kernel/src/sys/clock.c:364	    while (cmos(0x0A) & 0x80) pause();
   ```
2. `kernel/src/sys/clock.c:309`・`:314`・`:316` の TSC の比べの待ちにも上限が無い（同上）。
3. `kernel/src/arch/apic.c:359` の `if (!wake_vector)`：LAPIC timer の速さ（一回数えの道）は最初の CPU が一度だけ測り、全 CPU がその値を使う。
4. `kernel/src/arch/idt.c:344-348`：LAPIC が届ける割り込み（VEC_TIMER を含む）は、処理（`alarm_interrupt`）より先に EOI を出す。
   ```
   kernel/src/arch/idt.c:344	    } else if (v < 0xFF) {
   kernel/src/arch/idt.c:345	        entropy_add(rdtsc() ^ v << 56);
   kernel/src/arch/idt.c:346	        lapic_eoi();
   kernel/src/arch/idt.c:347	        irq_handler_t h = __atomic_load_n(&vec_handlers[v], __ATOMIC_ACQUIRE);
   kernel/src/arch/idt.c:348	        if (h) h(r);
   ```
   04（土台）の txt の書き出しが「割り込みの終わりの合図（EOI）を処理の前に出している」を一番重い所として挙げている（04 はこの文書では照らしていない）。
5. `kernel/src/arch/pstate.c:110`：APERF/MPERF の標本は 1 秒の窓の定数。
6. `kernel/src/sys/sched.c:108-110`：「---- time ----」の見出しの下に空行が二つあり、中身が無い（消した跡と見える）。
7. 時計の仕組みが二本ある：カーネルの alarm（CPU ごとの pairing heap、`alarm.c`）と、アプリの事象の行列の timer（行列ごとの連結の一覧を待ちのたびに線形に見る、`events.c:246-257`）。
8. `kernel/src/dev/nvme.c:421-424`：眠りの前の NVMe の止めは、装置の RTD3E を 1〜10 秒に挟み、その間 pause で spin する。

## 照らしていないもの

- 範囲の外（gui・net・fs・vm・mm・common/store）にある時間の所見：上の各節で「照らしていない」と書いたもの。
- 04・05・06 は、二つ目の依頼で照らした（下の「二つ目の依頼：04・05・06 の所見」）。
- 7600X が CPUID の TSC deadline の bit と CPUID 15h の結晶の周波数を持つか（実機で時計と alarm がどの道を通るかを決める）。この照らしでは確かめていない。

## 数え

- 項目の見出しの数（`rg -c "^#### 03"` などで数えた）：03 が 99、02 が 11、01 が 2。同じ行を指す重複を含み、そのうち 15 は「照らしていない」（範囲の外）と判定した項目。
- 判定の内訳：外れは 03(d)-130 の 159 行の部分だけ。一部当たりは 03(b)-57、03(b)-58（＝03(i)-226）、03(b)-65（＝03(i)-244）、03(c)-91、03(d)-130、03(i)-224。ほかは当たり。
- 分類の傾向：決め打ちの数が最も多く（dev の待ちの時間のほぼ全部）、次にポーリング（1 ms・10 ms ごとに眠って見直す輪と、回数・時間で止める spin）、VM の判定の混入は時計の中心（clock.c・clock.h）、LAPIC timer の速さ（apic.c）、P-state（pstate.c）、ABI と libc（abi.h・syscall.c・sys.c）、画面の写し（fb.c）、音（audio.c）、CPU の機能の線（cpu.c）に及ぶ。一つの CPU への寄りは眠りの道（pm・smp・sched・alarm が CPU 0 へ集める）と、全 `sleep_until` が通る一つの `deadline_wait`。

---

# 二つ目の依頼：04・05・06 の所見

## 依頼文（全文、起こしたセッションから届いた二通）

> ユーザーから：監査の報告の全文が六つ、D:\Programes に txt で置かれた。今の題材に直接当たるのは "D:\Programes\時間の道とカーネルの sys の監査.txt"（05 に相当）。土台（arch・mm）は "D:\Programes\Nocturne カーネルの土台（arch・mm・lib・boot init・linker.ld・Makefile の核の規則）監査の報告.txt"、dev は "D:\Programes\dev audit.txt"。docs の 01〜03 に加えてこれらも所見の元として読み、同じ形で原文の行に照らしてください。コードは変えない、コミットしない、は変わらず。

> 01〜03 は docs には書かれていません。報告の全文はユーザーが D:\Programes に txt で置いています（六つ、合計 210 KB）。題名から見た対応：01 = "D:\Programes\Nocturne OS カーネル監査 fscommon store net.txt"、02 = "D:\Programes\gui vm audit .txt"、03（横断）= おそらく "D:\Programes\カーネル監査.txt"（頭を読んで確かめて）、04 = "D:\Programes\Nocturne カーネルの土台（arch・mm・lib・boot init・linker.ld・Makefile の核の規則）監査の報告.txt"、05 = "D:\Programes\時間の道とカーネルの sys の監査.txt"、06 = "D:\Programes\dev audit.txt"。これらを所見の元にして、時間の道に関わるものを今のコードの原文の行に照らしてください。docs に 01〜03 を書き写す必要はありません。

（03 の対応は、`カーネル監査.txt` の頭の「(a) 固定の数」〜「(i) 時計の入口と単位」の章立てが 00-index の「横断」の範囲と合うので、それで確かめた。）

## 読んだもの

- 05 `時間の道とカーネルの sys の監査.txt`（590 行）：全部読んだ。
- 04 `Nocturne カーネルの土台（…）監査の報告.txt`（348 行）：1〜30 行、72〜216 行、325〜349 行を読み、ほかは時間の語（時間・時計・timer・TSC・LAPIC・EOI・spin・ms など）で `rg` して拾った。
- 06 `dev audit.txt`（710 行）：40〜69 行（「G. 時間の扱い」）、168〜417 行、425〜684 行を読み、ほかは同じ語で `rg` して拾った。
- 新しく読んだコード：`kernel/src/sys/sched.c`（366〜470、505〜540、860〜920、1028〜1050）、`kernel/src/sys/kei.c`（125〜140、247・275 の定義の有無）、`kernel/src/sys/sched.h`（100〜116）、`kernel/src/arch/spin.h`（1〜50）、`kernel/src/arch/isr.asm`（140〜155）、`kernel/src/arch/smp.c`（105〜130、160〜175）、`kernel/src/arch/smp.h`（35〜46）、`kernel/src/arch/idt.c`（60〜70、260〜272）、`kernel/src/arch/cpu.c`（128〜140）、`kernel/src/sys/events.c`（228〜240）、`kernel/src/sys/object.c`（60〜74）、`kernel/src/sys/channel.c`（218〜265）、`kernel/src/sys/prof.c`（260〜266、45）、`kernel/src/sys/keep.h`（20〜24）、`kernel/src/sys/syscall.c`（2806〜2818、2938〜2953）、`common/abi.h`（50〜56、234〜242、467〜480、503〜533）、`user/libc/sys.c`（76〜90、128〜140）、`user/libc/threads.c`（148〜172）、`user/libc/time.c`（235〜263）、`ports/nvidia/os.c`（268〜314）、`docs/nocturne-native-abi.md`（308〜316、398〜412）、`kernel/src/dev/` の nvme（410〜440）・ahci（455〜475）・pm.h（20〜50）・entropy（108〜122）・iommu.c（183〜201）・usbmsc（118〜136）・input.h（15〜35）・acpi（298〜312）・pci（906〜916）・hibernate（290〜322）・fb（48〜74、394〜402）、`kernel/src/net/rtl8125.c`（545〜549）。
- `rg` で確かめたこと：`clock_set_wall(` の呼ぶ側、`arat`・`C0011029`・`DE_CFG`（AMD の LFENCE を直列にする MSR）の有無、`kei_choose_cpu`・`kei_validate` の呼ぶ側、`need_resched` の出る数、`vmm.c`・`idt.c` の `sti`。

01〜03 と同じ所見で、上で既に照らしたものは、その項の名（例：03(b)-58）を書いて引用を繰り返さない。

---

## 05（時間の道と sys）の所見

### 05 時間の道の段

#### 05-13・14 刻みの源：`clock.c:58-62` tsc_read（lfence; rdtsc）。AMD で LFENCE を直列にする MSR（C001_1029）の設定が核のどこにも無い

```
kernel/src/sys/clock.c:59	static inline uint64_t tsc_read(void) {
kernel/src/sys/clock.c:60	    uint32_t lo, hi;
kernel/src/sys/clock.c:61	    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
kernel/src/sys/clock.c:62	    return (uint64_t)hi << 32 | lo;
kernel/src/sys/clock.c:63	}
kernel/src/sys/clock.c:261	/* The TSC read once the loads before it are done: RDTSCP waits for every older instruction (SDM
kernel/src/sys/clock.c:262	   Vol. 2B RDTSCP, APM Vol. 3 RDTSCP), on either vendor, where LFENCE orders RDTSC on AMD only
kernel/src/sys/clock.c:263	   when made dispatch serializing. */
```

`rg -n -i "arat|C0011029|0xC0011029|DE_CFG" kernel/src` は、この MSR に当たる行を一つも返さなかった（当たったのは「separate」などの語の中の「arat」だけ）。

- 判定：**当たり**（行番号は 59〜63 で一つずれ）。clock.c 自身が 261〜263 行で「LFENCE が RDTSC を順に並べるのは AMD では dispatch serializing にしたときだけ」と書きながら、毎回の読み（`tsc_read`、`clock_now_ns` の道）は `lfence; rdtsc` で、その設定をする所は無い。比べ（`compare_readings`）だけ `rdtscp` を使う。Zen 4 で既定のままでも直列になるかは 05 と同じく未確認。
- 分類：四つの外（命令の順の保証の欠け）。

#### 05-15・16 速さを測る：PM timer の 10 ms の窓を 5 回、中央値。無ければ PIT。AMD 向けの CPUID 15h の道は無い

- 原文は 03(a)-41、03(b)-64、03(h)-211。CPUID 15h の読みは：

```
kernel/src/arch/cpu.c:134	    /* CPUID 15h: the TSC's ratio to the core crystal clock and, where it is given, the crystal's
kernel/src/arch/cpu.c:135	       frequency (Intel SDM vol. 2A, CPUID leaf 15h). */
kernel/src/arch/cpu.c:136	    if (max >= 0x15) {
kernel/src/arch/cpu.c:137	        cpuid(0x15, 0, &a, &b, &c, &d);
kernel/src/arch/cpu.c:138	        if (a && b && c) f->tsc_crystal_hz = (uint64_t)c * b / a;
```

- 判定：**当たり**。15h の読みは製造元で分けないが、葉の出所は Intel の SDM で、AMD の TSC の速さを得る別の道（AMD の資料による）は無い。Zen 4 が 15h を答えるかは未確認（03(h)-211 と同じ）。量子化 28 ppm（10 ms の窓で PM timer の 1 刻み＝1/35795 ≒ 28 ppm）は計算で合う。
- 分類：**決め打ちの数**（窓・回数）、**ポーリング**（測りの spin）。

#### 05-21〜24 速さを直す：64 秒目に一度だけ。壁時計を直す道は clock_set_wall だけで、呼ぶのは vm/hv/hv_util.c:136 だけ

```
kernel/src/sys/clock.c:224	    if (++remeasure.seconds < CLOCK_REMEASURE_S) {
kernel/src/sys/clock.c:229	    uint64_t hz = (tsc - remeasure.start_tsc) * PM_TIMER_HZ / remeasure.pm_total;
kernel/src/sys/clock.c:232	    conv_set(at, ns_of(at), hz);
kernel/src/sys/clock.c:460	void clock_set_wall(int64_t unix_ns) {
```

`rg -n "clock_set_wall\(" kernel/src` の結果：

```
kernel/src\vm\hv\hv_util.c:136:            clock_set_wall(want);
kernel/src\sys\clock.c:460:void clock_set_wall(int64_t unix_ns) {
kernel/src\sys\clock.h:57:void clock_set_wall(int64_t unix_ns);
```

- 判定：**当たり**。速さは起動の測りと、PM timer で測ったとき（`remeasuring`、401〜404 行）だけ 64 秒目に一度直る。CPUID 15h で決めた速さは一度も測り直さない。壁時計を動かすのは VM の部品だけで、実機では起動時の CMOS の秒から先は TSC の速さの誤差がそのまま壁時計の誤差になる。「50 ppm なら一日に約 4 秒」は計算で合う（86400 s × 50e-6 ＝ 4.32 s）。
- 分類：**決め打ちの数**（64 秒の一度）。壁時計を直す道が VM にしか無い点は **VM の判定の混入**。

#### 05-25〜27 CPU の間のずれ：offset を測る。利用者の道は offset を使わない

- 原文は 03(c)-105 と clock.c の 82〜86、116 行（上の全文）。利用者の側：

```
user/libc/sys.c:78	static uint64_t user_rdtsc(void) {
user/libc/sys.c:80	    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
user/libc/sys.c:119	    uint64_t now = user_rdtsc();
user/libc/sys.c:120	    if (now < user_clock.tsc_sample) return false;
```

- 判定：**当たり**。核の読みは `tsc_offset` を引くが（84、116 行）、libc の読み（`user_rdtsc`）と `ports/nvidia/os.c` の `read_tsc`（288〜292 行）は TSC をそのまま使い、どの CPU の上かを知らない。
- 分類：四つの外（核と利用者の時計の約束の食い違い）。

#### 05-28〜30 換算：tsc_hz だけは seq を閉じた後に書く（137）

```
kernel/src/sys/clock.c:135	    __atomic_store_n(&conv_seq, conv_seq + 1, __ATOMIC_RELEASE);
kernel/src/sys/clock.c:136	    __atomic_store_n(&tsc_hz, hz, __ATOMIC_RELAXED);
```

- 判定：**当たり**（行は 136）。`clock_tsc_hz()`（99 行）を読む側（`pstate.c:80`、`syscall.c:2916`）は、新しい換算と古い速さの組を取りうる。
- 分類：四つの外（読みの一貫性の欠け）。

#### 05-31〜39 壁時計と眠り：RTC は秒だけで二度読みを確かめない、S3 で単調時計は止まり、offset は 0 に戻し、壁時計に RTC の秒の差を足す。眠りを含む時計が無い

```
kernel/src/sys/clock.c:363	int64_t rtc_read_unix(void) {
kernel/src/sys/clock.c:364	    while (cmos(0x0A) & 0x80) pause();
kernel/src/sys/clock.c:365	    int sec = cmos(0x00), min = cmos(0x02), hour = cmos(0x04);
kernel/src/sys/clock.c:415	    wall_offset_ns = rtc_read_unix() * (int64_t)NS_PER_S - (int64_t)clock_now_ns();
kernel/src/sys/clock.c:436	        conv_set(tsc_read(), stopped_ns, tsc_hz);
kernel/src/sys/clock.c:443	    __atomic_store_n(&tsc_offsets, false, __ATOMIC_RELEASE);
kernel/src/sys/clock.c:444	    for (unsigned i = 0; i < topology.cpus; i++) per_cpu(tsc_offset, i) = 0;
kernel/src/sys/clock.c:445	    int64_t slept = rtc_read_unix() - rtc_before;
kernel/src/sys/clock.c:447	    __atomic_fetch_add(&wall_offset_ns, slept * (int64_t)NS_PER_S, __ATOMIC_RELAXED);
```

- 判定：**当たり**。UIP が消えるのを一度待ってから読むだけで、読む間に秒が繰り上がったかを二度読みで確かめない。`clock.h` の口（29〜57 行）に眠りを含む時計は無い。
- 分類：四つの外（時計の種類の欠け、壁時計の粒が秒）。364 行は **ポーリング**（上限なし、03(c)-105）。

#### 05-40〜45 VM の混ざり

- 原文は 03(g)-191・192・197。判定：**当たり**。分類：**VM の判定の混入**。

#### 05-46〜52 利用者の時計：process ごとに一度だけの写し、速さを更新しない、offset を引かない、S3 の後は ms の system call に落ちる、戻りそうな時は止めて隠す。nvidia に二つ目の実装

```
user/libc/sys.c:93	static bool user_clock_us(uint64_t *us) {
user/libc/sys.c:94	    if (__atomic_load_n(&clock_initialized, __ATOMIC_ACQUIRE) != 2) {
user/libc/sys.c:98	        if (clock_info(&user_clock) < 0) memset(&user_clock, 0, sizeof user_clock);
user/libc/sys.c:99	        __atomic_store_n(&clock_initialized, 2, __ATOMIC_RELEASE);
user/libc/sys.c:118	    if (!(user_clock.flags & N_CLOCK_USER_TSC) || user_clock.tsc_hz < 1000000) return false;
user/libc/sys.c:119	    uint64_t now = user_rdtsc();
user/libc/sys.c:120	    if (now < user_clock.tsc_sample) return false;
user/libc/sys.c:124	    *us = user_clock.uptime_ms * 1000 + cycles / user_clock.tsc_hz * 1000000 +
user/libc/sys.c:85	static uint64_t clock_publish(uint64_t *last, uint64_t value) {
user/libc/sys.c:86	    uint64_t previous = __atomic_load_n(last, __ATOMIC_RELAXED);
user/libc/sys.c:87	    while (value > previous && !__atomic_compare_exchange_n(last, &previous, value, false,
user/libc/sys.c:130	    return clock_publish(&clock_last, user_clock_us(&us) ? us / 1000 : (uint64_t)SC0(SYS_UPTIME));
user/libc/sys.c:135	    return clock_publish(&last, user_clock_us(&us) ? us : (uint64_t)SC0(SYS_UPTIME) * 1000);
ports/nvidia/os.c:282	static void clock_setup(void) {
ports/nvidia/os.c:284	    if (clock_info(&clock_base) >= 0 && (clock_base.flags & N_CLOCK_USER_TSC) && clock_base.tsc_hz >= 1000000)
ports/nvidia/os.c:295	    call_once(&clock_once, clock_setup);
ports/nvidia/os.c:298	    if (clock_tsc && (tsc = read_tsc()) >= clock_base.tsc_sample) {
ports/nvidia/os.c:304	        ns = n_clock_uptime_ms() * 1000000ull;
```

- 判定：**当たり**。写しは process の最初の読みで一度だけ取り、64 秒目の核の速さの直し（05-21）を知らない。TSC が写しの値より小さくなれば（S3 の後に TSC が 0 から数え直すとき、`clock.h:47-48` の注記）以後ずっと ms の `SYS_UPTIME` に落ちる。`clock_publish` は値が戻るときに前の値を返す（止めて隠す）。nvidia の移植に同じ形の二つ目の時計がある。
- 分類：四つの外（時計の持ち主が二つ三つに割れている。AGENTS.md の「一つの持ち主」に反する）。`N_CLOCK_HV_REFERENCE` の枝（101〜116 行）は **VM の判定の混入**。

#### 05-53〜60 ABI の単位：sleep・uptime は ms、time は秒、期限は ms の相対、events の timer は ms、番地の待ちも ms、docs も ms と秒、libc も ms

```
common/abi.h:53	    SYS_SLEEP = 19, SYS_UPTIME, SYS_TIME,
kernel/src/sys/syscall.c:2809	static int64_t sys_events_timer(int q, uint64_t ms, int repeat, uint64_t token) {
kernel/src/sys/syscall.c:2813	    r = events_timer(queue, ms < CLOCK_NEVER / NS_PER_MS ? ms * NS_PER_MS : CLOCK_NEVER, repeat != 0, token);
kernel/src/sys/syscall.c:2942	        else ret = task_wait_address((volatile void *)a, (uint32_t)b, false, address_deadline((unsigned)c));
docs/nocturne-native-abi.md:310	- タイマー：`n_events_timer(q, ms, repeat, token)`。`N_EVENT_TIMER` で届く。繰返しの
docs/nocturne-native-abi.md:407	- `n_clock_uptime_ms()`、`n_clock_wall()`（壁時計の秒）、`n_sleep_ms(ms)`（結果は無い。
user/libc/threads.c:152	    uint64_t ms = (uint64_t)duration->tv_sec * 1000u + (uint64_t)(duration->tv_nsec + 999999L) / 1000000u;
user/libc/time.c:237	/* ISO C timespec_get. The system's wall clock counts whole seconds; the
user/libc/time.c:257	    case TIME_MONOTONIC: ms = (int64_t)n_clock_uptime_ms(); break;
```

- 判定：**当たり**。03(i)-232 で「events の timer は ns で受ける」としたのは核の中の `events_timer`（`events.c:178`）で、system call の口（`syscall.c:2809`）は ms で受けて換える。ABI では timer も ms。ISO C の `timespec_get` は ms の粒（260〜261 行）。
- 分類：**決め打ちの数**（ABI の粒 1 ms）。

#### 05-61〜63 期限の器：alarm は正しい、sleep の期限は大域の deadline_wait 一つ、wq_take_blocked は線形

- 原文は 03(e)-143。判定：**当たり**。分類：03(e)-143 と同じ。

#### 05-64〜69 起こしの装置：TSC-deadline は CPUID.01H:ECX[24] の時だけ、LAPIC の速さは 20 ms で一度、64 秒目の直しの後も直さない、WAKE_LATE_SHIFT、ARAT を確かめない

```
kernel/src/arch/cpu.c:53	    OPTIONAL(L1, ECX, 24, "TSC deadline", tsc_deadline), OPTIONAL(L1, ECX, 31, "hypervisor", hypervisor),
```

ほかの原文は 03(b)-58・59、03(g)-190。ARAT は `rg -i arat` で一つも無い。

- 判定：**当たり**。LAPIC の速さ（`wake_mult`）は `lapic_wake_init` の最初の一度（359 行）だけ決め、`remeasure_fire`（clock.c:219〜237）はそれに触れない。「1 s 先の期限なら約 122 µs 遅れる」は 1 s/8192 ≒ 122 µs で合う。ただし 03(b)-58 のとおり、これは一回数えの道だけ。
- 分類：**決め打ちの数**。ARAT の欠けは四つの外（深い C 状態で LAPIC timer が止まる CPU の扱いの欠け。今は idle が HLT だけなので、どの C 状態に入るかは firmware 次第で未確認）。

#### 05-70〜73 発火：alarm.c:135-154 で割り込みを閉じたまま callback を全部その場で呼ぶ。idt.c:344-348 で EOI を先に送る

- 原文は alarm.c の 135〜154 行（全文を上で読んだ）と、「監査に無い事実」の 4（idt.c:344〜348）。判定：**当たり**。`wake_fire`（sched.c:831〜834）→ `sched_make_ready` → `enqueue` → `choose`（全 CPU の走査）が割り込みの中で走る。
- 分類：四つの外（hard IRQ の中の仕事の重さ）。choose の走査は 03(d)-135。

#### 05-74〜76 起こし：wake_fire → sched_make_ready（394-398）→ choose（306-318）→ IPI（smp.c:112-121）。xAPIC の送りの空回り

```
kernel/src/sys/sched.c:371	        unsigned from = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE), to = choose(t);
kernel/src/sys/sched.c:385	        if (kick) {
kernel/src/sys/sched.c:386	            if (to == cpu_current_index()) need_resched = true;
kernel/src/sys/sched.c:387	            else { STAT(kicks); cpu_kick(to); }
kernel/src/arch/smp.c:112	void cpu_kick(unsigned i) {
kernel/src/arch/smp.c:115	    if (!lapic_send_ipi(topology.cpu[i].apic_id, VEC_RESCHED)) panic("smp: IPI to cpu%u failed", i);
```

- 判定：**当たり**（394〜398 行は idle task の枝で、普通の task は 399〜413 行から `enqueue`（369〜391 行）へ入る）。`choose` は起きた task の前の CPU が空いていれば走査しない（310 行）が、そうでなければ全 CPU を見る。
- 分類：四つの外（O(CPU) の走査）、xAPIC の空回りは **ポーリング**（03(c)-106）。

#### 05-77〜84 走るまで：眠っていた分の貸しが無い、retention 990 µs、全 task 同じ重み、核は横取りされない、idle の輪は割り込みを閉じて仕事、cpu_parallel_for は割り込みを閉じる、spinlock は TAS で割り込みを閉じたまま回る

```
kernel/src/sys/kei.c:131	    kei_vtime lead = t->home && t->v > t->home->clock_v ? t->v - t->home->clock_v : 0;
kernel/src/sys/kei.c:133	    t->v = q->clock_v + lead; t->home = q;
kernel/src/sys/sched.h:110	   goes to an idle CPU first. Kernel code is not preempted: a CPU switches tasks when its task
kernel/src/sys/sched.h:111	   blocks, yields or goes back to ring 3. */
kernel/src/arch/isr.asm:147	    test byte [rsp + 8 + REGS_CS], 3
kernel/src/arch/isr.asm:148	    jz isr_exit
kernel/src/sys/sched.c:525	        cli();
kernel/src/sys/sched.c:526	        cpu_park_check();
kernel/src/sys/sched.c:527	        if (cpu_jobs_poll()) continue;
kernel/src/arch/smp.c:171	    uint64_t flags = irq_save(), since = clock_now_ns();
kernel/src/arch/spin.h:24	    while (__atomic_exchange_n(&l->taken, 1, __ATOMIC_ACQUIRE)) {
kernel/src/arch/spin.h:41	    uint64_t flags = irq_save();
kernel/src/arch/spin.h:42	    spin_take(l);
```

`rg -c need_resched kernel/src` の合計は 21 行（vm/ の 3 行を含む）。

- 判定：**当たり**。起きた task は前より先行していれば先行分を保ち（131 行）、遅れていれば `clock_v` の位置に置かれ、遅れ（負の lag）は捨てられる。核へ戻る割り込みは `isr_exit` へ直行し（147〜148 行）、横取りの点は利用者へ戻る道（`sched_user_return`、sched.c:509〜518）だけ。retention は 03(b)-56 の計算のとおり 990000 ns。
- 分類：**決め打ちの数**（retention、重さ）。spin と idle の輪は **ポーリング**。

#### 05-85〜88 事象の届け：events_wait は全 timer と全 watch を q->lock の下、割り込みを閉じて走査。object.c:63-72 と channel.c:221-236 は全員を起こす

```
kernel/src/sys/events.c:239	        uint64_t fl = spin_lock(&q->lock);
kernel/src/sys/object.c:63	void object_notify(struct object *o) {
kernel/src/sys/object.c:65	    uint64_t flags = spin_lock(&o->lock);
kernel/src/sys/object.c:66	    for (struct object_waiter *w = o->waiters; w; w = w->next) {
kernel/src/sys/object.c:69	        wq_wake_all(&w->wait->wq);
kernel/src/sys/channel.c:233	    wq_wake_all(&l->wait[i]);
kernel/src/sys/channel.c:234	    object_notify(&l->end[i]);
```

- 判定：**当たり**（走査の原文は 03(c)-114）。
- 分類：四つの外（走査と群れ起こし）。

#### 05-89〜96 時刻の印：n_event、gui_event、EV_FRAME、n_audio_status に時刻が無い。wm_vblank は system call が着いた時刻。核の log に時刻が無い

```
common/abi.h:236	struct n_event {
common/abi.h:237	    uint64_t token;
common/abi.h:238	    uint32_t events;
common/abi.h:239	    uint32_t reserved;
common/abi.h:240	};
common/abi.h:505	struct gui_event {
common/abi.h:506	    uint32_t type;
common/abi.h:507	    int32_t x, y;       /* mouse position in client coordinates / new size for EV_RESIZE */
common/abi.h:528	/* EV_FRAME: key = completed index, x = reusable index; completion means all
common/abi.h:529	   compositor reads have joined, not hardware vblank or physical scanout.
common/abi.h:530	   Every EV_FRAME has y = the display's frame period in µs: the next frame comes
common/abi.h:471	struct n_audio_status {
common/abi.h:472	    uint32_t queued_frames;
common/abi.h:473	    uint32_t device_pending_frames;
common/abi.h:474	    uint64_t played_frames;
common/abi.h:475	    uint64_t committed_frames;
common/abi.h:476	    uint32_t generation;
common/abi.h:477	    uint32_t flags;
common/abi.h:478	};
```

- 判定：**当たり**（ABI の四つ）。`n_audio_status` には時計の出所の旗（`N_AUDIO_CLOCK_DEVICE`・`N_AUDIO_CLOCK_ESTIMATED`、479〜480 行）はあるが時刻は無い。`gui/wm.c:2456-2460`（wm_vblank）と `wm.c:127` は gui で範囲の外のため照らしていない。核の log に時刻が無いことは `.claude/rules/kernel.md` に「カーネルの行には時刻が無い」とある。
- 分類：四つの外（ABI の欠け）。

### 05 持ち主の単位ごとの判定の根拠のうち、時間に関わるもの

#### 05-120・124 clock_spin を driver が ms の空回りに使う（rtl8125.c:547 で 10 ms、ahci.c:180 で 2 ms）

```
kernel/src/net/rtl8125.c:547	            clock_spin(10 * NS_PER_MS);
kernel/src/dev/ahci.c:180	    clock_spin(2 * NS_PER_MS); /* at least 1 ms, also where nothing may sleep */
```

- 判定：**当たり**（rtl8125.c は範囲の外の net だが一行なので照らした）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 05-122 CPU の比べは CPU ごとに 256 回の往復を起動時に順に行う

```
kernel/src/sys/clock.c:244	   behind then is a disagreement the offsets did not take away. All compare at once: the line of
kernel/src/sys/clock.c:245	   the highest reading crosses once a reading, so the time grows with the CPUs, not their pairs,
kernel/src/sys/clock.c:246	   and every CPU is compared with every other one that published while it read. */
kernel/src/arch/smp.c:312	    clock_cpus_compare(arrived + 1);
```

- 判定：**外れ**。比べは「順に」ではなく、着いた CPU が全部一度に行う（`clock_cpus_compare` は一度だけ、着いた数で呼ばれる。各 CPU は `clock_cpu_compare` で同時に `compare_readings` を回す、312〜318 行）。256 回は CPU ごとの読みの数で、往復ではない。ただし 244〜246 行の注記のとおり、`highest` の一語を全 CPU が取り合うので、時間は CPU の数に比例して延びる。
- 分類：（外れのため無し。比例する点は四つの外）。

#### 05-123 2 socket で offset が 0 でないと、offset を知らない利用者の時計が CPU を移るたびに戻ったり進んだりする

- 原文は 05-25〜27 と 05-46〜52。判定：**当たり**（コードの上で、利用者の時計は offset を引かない。戻る分は `clock_publish` が止めて隠す）。実機の 2 socket で offset が 0 でないかは未確認。
- 分類：四つの外。

#### 05-162〜164 alarm の契約：発火の間は armed_on が 0 なので、同じ alarm を二つの CPU が同時に立てると二つの heap に入る。dev/serial.c はこの契約を破っている

```
kernel/src/sys/alarm.c:107	void alarm_set(struct alarm *a, uint64_t at) {
kernel/src/sys/alarm.c:108	    uint64_t f = irq_save();
kernel/src/sys/alarm.c:109	    unlink(a);
kernel/src/sys/alarm.c:110	    unsigned self = cpu_current_index();
kernel/src/sys/alarm.c:111	    struct alarm_cpu *c = &per_cpu(alarms, self);
kernel/src/sys/alarm.c:112	    spin_take(&c->lock);
kernel/src/sys/alarm.c:113	    a->at = at;
kernel/src/sys/alarm.c:114	    heap_insert(c, a);
kernel/src/sys/alarm.c:115	    __atomic_store_n(&a->armed_on, self + 1, __ATOMIC_RELEASE);
kernel/src/sys/alarm.c:144	        __atomic_store_n(&a->firing, self + 1, __ATOMIC_RELAXED);
kernel/src/sys/alarm.c:145	        __atomic_store_n(&a->armed_on, 0, __ATOMIC_RELEASE);
kernel/src/sys/alarm.c:146	        spin_give(&c->lock);
kernel/src/sys/alarm.c:147	        a->fire(a, r);
kernel/src/dev/serial.c:87	    if (waits && !alarm_armed(&drain)) alarm_set(&drain, clock_after(fifo_depth * CHAR_NS));
kernel/src/dev/serial.c:96	    if (waits) alarm_set(a, clock_after(fifo_depth * CHAR_NS));
```

- 判定：**当たり**。`alarm_set` は「外す（109 行、`armed_on` が 0 なら何もしない）」と「自分の CPU の heap に入れる（112〜115 行）」の間に、二つの CPU を一つにする lock を持たない。二つの CPU が同時に `alarm_set` すると、どちらも 109 行で何も外さず、それぞれの heap に同じ `struct alarm` を入れる（`child`・`next`・`prev` の指を二つの heap で共有する）。serial.c では、どの CPU も記録の行を書き（26 行「any CPU logs」）、87 行は `alarm_armed` を見てから lock の外で `alarm_set` を呼ぶので、二つの CPU が同時に「立っていない」と見て両方とも立てうる。発火の間（145 行で `armed_on` が 0）の 96 行の立て直しと、別の CPU の 87 行も重なりうる。
- 分類：四つの外（契約の欠けと、それを破る呼ぶ側）。

#### 05-209〜214 sched の容量と均し：slice_fire は rebalance を IRQ の中で O(N)、slice の alarm を起こした側の CPU に立てる、idle の輪で割り込みを閉じて cpu_jobs_poll、FPU を毎回 eager に保存・復元、全 task 同じ重み

```
kernel/src/sys/sched.c:327	    alarm_set(&q->slice, clock_after(d.service_until_recheck_ns));
kernel/src/sys/sched.c:353	    if (cpu != cpu_current_index()) {
kernel/src/sys/sched.c:354	        cpu_kick(cpu);
kernel/src/sys/sched.c:355	        return;
kernel/src/sys/sched.c:465	    fpu_save(prev->fpu);
kernel/src/sys/sched.c:466	    fpu_restore(next->fpu);
```

- 判定：**当たり**。`slice_arm` は `enqueue`（381 行）から、起こした側の CPU で呼ばれうる。`alarm_set` は今の CPU の heap に置くので（03 の alarm.c の照らし）、slice の alarm は起こした CPU に立ち、来たときに持ち主の CPU を kick する（353〜356 行）：起こしの IPI が一つ余分に挟まる。rebalance は 03(b)-55・03(d)-135。
- 分類：**一つの CPU への寄り**ではなく、別の CPU に置かれた期限（四つの外）。rebalance は **決め打ちの数**。

#### 05-221〜223 待ち行列は先頭に入れる LIFO、起こしは全員起こしだけ、mutex は全員を起こし手渡しも優先度の継承も無い、番地の待ちは大域の一本

```
kernel/src/sys/sched.c:781	        t->wq_next = wq->head;
kernel/src/sys/sched.c:782	        wq->head = t;
kernel/src/sys/sched.c:863	static void wake_all_held(struct wait_queue *wq) {
kernel/src/sys/sched.c:899	void mutex_unlock(struct mutex *m) {
kernel/src/sys/sched.c:902	    if (!--m->depth) {
kernel/src/sys/sched.c:903	        m->owner = NULL;
kernel/src/sys/sched.c:904	        wake_all_held(&m->wait);
kernel/src/sys/sched.c:1032	static struct wait_queue address_wait;
kernel/src/sys/sched.c:1040	#define ADDRESS_BUCKETS 256
kernel/src/sys/sched.c:1041	static uint32_t address_waiters[ADDRESS_BUCKETS];
kernel/src/sys/sched.c:1049	static struct spinlock address_lock;
```

- 判定：**当たり**。時間の面では、mutex を放すたびに待つ全 task が起き、一つだけが取り、残りは眠り直す。待ち行列が LIFO なので、先に待った task が後になる。
- 分類：**決め打ちの数**（256 の桶）。全員起こしは四つの外。

#### 05-269〜276 kei：使われない部品（kei_charge_detached・kei_set_bonus・kei_choose_cpu・kei_validate・kei_set_online）

`rg -n "kei_choose_cpu|kei_validate" kernel/src` の結果は定義と宣言だけ：

```
kernel/src\sys\kei.c:247:size_t kei_choose_cpu(const struct kei_cpu_candidate *c, size_t count,
kernel/src\sys\kei.c:275:bool kei_validate(const struct kei_rq *q);
kernel/src\sys\kei.h:97:size_t kei_choose_cpu(const struct kei_cpu_candidate *c, size_t count,
kernel/src\sys\kei.h:102:bool kei_validate(const struct kei_rq *q);
```

（kei.c:275 の行は `bool kei_validate(const struct kei_rq *q) {`。ほかの三つは 03(f)-170。）

- 判定：**当たり**。
- 分類：四つの外（呼ばれない関数）。

#### 05-336〜340 events：timer は並んでいない list、遅れた繰り返しの timer は位相を作り直す（252-253）、EVENTS_BATCH 64、核の中は ns なのに system call は ms

```
kernel/src/sys/events.c:230	#define EVENTS_BATCH 64
kernel/src/sys/events.c:251	                /* keep the cadence; a late wait does not report the missed ticks */
kernel/src/sys/events.c:252	                t->due = t->due < CLOCK_NEVER - t->period ? t->due + t->period : CLOCK_NEVER;
kernel/src/sys/events.c:253	                if (t->due <= now) t->due = clock_after(t->period);
docs/nocturne-native-abi.md:310	- タイマー：`n_events_timer(q, ms, repeat, token)`。`N_EVENT_TIMER` で届く。繰返しの
docs/nocturne-native-abi.md:311	  タイマーは遅れた分をまとめて報告せず、周期を保つ。
```

- 判定：**一部当たり**。遅れが一周期より短ければ 252 行で位相を保つ。一周期以上遅れたとき（253 行）だけ、今から一周期後に置き直して位相を捨てる。docs の「周期を保つ」はこの場合に合わない。ほかは当たり。
- 分類：**決め打ちの数**（64）、ABI の単位の不揃いは四つの外。

#### 05-386〜390 channel：送り手は期限なしで待つ、受け手に timeout が無い

```
kernel/src/sys/channel.c:256	    while (l->open[peer] && message_queue_full(l, peer)) {
kernel/src/sys/channel.c:259	        wq_wait_locked(&l->send_wait[peer], &l->lock);
```

- 判定：**当たり**（送り手。受け手の `channel.h:14` は読んでいないので未確認）。
- 分類：四つの外（期限の欠け）。

#### 05-414 stream：log の行に時刻も CPU も無い

- 判定：**当たり**（`.claude/rules/kernel.md` の「カーネルの行には時刻が無い」。`sys/stream.c` の原文はこの照らしで読んでいない）。分類：四つの外。

#### 05-442 keep：KEEP_AFTER_NS 500 ms

```
kernel/src/sys/keep.h:22	#define KEEP_AFTER_NS (500 * NS_PER_MS) /* a change goes to C: once its part has stopped changing this long */
```

- 判定：**当たり**。分類：**決め打ちの数**。

#### 05-456・459 prof：SAMPLE_NS 1 ms の CPU ごとの alarm、enabled が素の bool

```
kernel/src/sys/prof.c:25	#define SAMPLE_NS NS_PER_MS
kernel/src/sys/prof.c:45	static bool enabled;
kernel/src/sys/prof.c:262	        enabled = false;
kernel/src/sys/prof.c:263	        dump();
kernel/src/sys/prof.c:264	        enabled = true;
```

- 判定：**当たり**（`enabled` を読む alarm の側は読んでいない。素の `bool` で、原子的な読み書きでも lock の下でもない書きであることは上の行で確か）。
- 分類：**決め打ちの数**（1 ms）、**ポーリング**（標本の拍）。

#### 05-535〜540 arch/spin.h：TAS、割り込みを閉じたまま回る

- 原文は 05-77〜84 の spin.h の行と、04-79・80（下）。判定：**当たり**。分類：**ポーリング**。

#### 05-541〜547 arch/idt.c：vector は 0x50-0xEF の 160 個、装置の割り込みは全部 CPU 0、EOI を先に送る

- EOI は「監査に無い事実」の 4。vector の範囲は 03 の (a) の所見で、時間の道ではないので照らしていない。判定：EOI は **当たり**。分類：四つの外（仕様の手順と違う順。04-115 を参照）。

#### 05-553〜557 cpu_parallel_for：一つの組みの間ずっと割り込みを閉じ、idle の全 CPU を O(N) で kick、呼ぶ側は空回り。fb.c の既定は WHPX で測った値

- 原文は 03(c)-107、02-398、05-77〜84 の smp.c:171 と、下の 04-185。判定：**当たり**。分類：**ポーリング**、**VM の判定の混入**（fb.c の既定）。

---

## 04（土台）の所見のうち、時間に関わるもの

#### 04-79・80 spin.h：割り込みを閉じてから待つ。待ちの輪の中で vmm_tlb_poll を呼び、VEC_RESCHED と VEC_TIMER は待ちが終わるまで届かない

```
kernel/src/arch/spin.h:4	   A CPU waiting for a lock has its interrupts off, so it cannot take a TLB shootdown's IPI; it
kernel/src/arch/spin.h:5	   serves the shootdowns waiting for it while it spins instead (vmm_tlb_poll). Holders never sleep
kernel/src/arch/spin.h:26	        while (__atomic_load_n(&l->taken, __ATOMIC_RELAXED)) {
kernel/src/arch/spin.h:27	            vmm_tlb_poll();
kernel/src/arch/spin.h:28	            pause();
kernel/src/arch/spin.h:41	    uint64_t flags = irq_save();
kernel/src/arch/spin.h:42	    spin_take(l);
```

- 判定：**当たり**。待つ CPU は割り込みを閉じているので、その CPU の alarm（VEC_TIMER）と起こし（VEC_RESCHED）は lock が取れるまで届かない。撃ち落としだけは 27 行で見回って答える。
- 分類：**ポーリング**。

#### 04-103 isr.asm：核へ戻る割り込みは isr_exit へ直行し、横取りの点が無い（146-148）

- 原文は 05-77〜84 の isr.asm:147〜148。判定：**当たり**。分類：四つの外（横取りの欠け）。

#### 04-115 処理の前に EOI を出す（LAPIC は idt.c:346、8259 は idt.c:338）

```
kernel/src/arch/idt.c:338	        pic_eoi(irq);
kernel/src/arch/idt.c:339	        for (unsigned i = 0; i < IRQ_SHARED; i++) {
kernel/src/arch/idt.c:340	            irq_handler_t h = __atomic_load_n(&irq_handlers[irq][i], __ATOMIC_ACQUIRE);
kernel/src/arch/idt.c:346	        lapic_eoi();
kernel/src/arch/idt.c:347	        irq_handler_t h = __atomic_load_n(&vec_handlers[v], __ATOMIC_ACQUIRE);
kernel/src/arch/idt.c:348	        if (h) h(r);
```

- 判定：**当たり**（コードの順）。SDM 第 3A 巻 13.8.5 の文言はこの照らしでは開いていない（未確認）。
- 分類：四つの外（手順の順）。

#### 04-116 全ゲートが割り込みゲート 0x8E、頁違反は割り込みを閉じたまま vmm_handle_user_fault に入る

```
kernel/src/arch/idt.c:66	    for (int i = 0; i < 256; i++) idt_set(i, isr_stub_table[i], 0x8E, 0);
kernel/src/arch/idt.c:268	        if ((r->cs & 3) == 3 ? vmm_handle_user_fault(cr2, r->error) : user_copy_fixup(r, cr2)) return;
```

`rg -n 'sti\(\)|irq_restore|sti;' kernel/src/mm/vmm.c kernel/src/arch/idt.c` の結果は `vmm.c:193`・`236`・`277` の `irq_restore`（その関数自身が閉じた分を戻す行）だけで、頁違反の道で割り込みを開く `sti` は無い。

- 判定：**当たり**。利用者の頁違反の処理（頁の確保と 0 埋め）は、全部割り込みを閉じた区間になる。`vmm.c:795`・`800-816`（mm）の中身は読んでいない。
- 分類：四つの外（割り込みを閉じた区間の長さ）。

#### 04-143・145〜148 apic.c：xAPIC の配達待ちは 100 万回、TSC-deadline の無い CPU では一回数え、速さは起動時に 20 ms 一回、1/8192 遅らせ、vm_lapic_hz、LVT は mask のまま、ARAT は見ない

```
kernel/src/arch/apic.c:115	    lapic_write(0x370, LVT_UNUSED);                       /* error */
kernel/src/arch/apic.c:119	    if (max_lvt >= 4) lapic_write(0x340, LVT_UNUSED);     /* performance counter */
kernel/src/arch/apic.c:120	    if (max_lvt >= 5) lapic_write(0x330, LVT_UNUSED);     /* thermal sensor */
```

ほかの原文は 03(b)-58・59、03(c)-106、03(g)-190。

- 判定：**当たり**。「Zen 3 の PPR 55898 では CPUID Fn0000_0001_ECX[24] が Reserved」は PPR を開いていないので未確認。
- 分類：**決め打ちの数**、**ポーリング**、**VM の判定の混入**（366 行）。

#### 04-177 smp_suspend は CPU を一つずつ待つ。最悪で 2 秒×CPU 数

- 原文は 03(d)-128。判定：**当たり**。分類：**一つの CPU への寄り**、**ポーリング**、**決め打ちの数**。

#### 04-185 cpu_parallel_for：呼び手は batch の間ずっと割り込みを閉じる、手伝う idle の CPU も割り込みを閉じて走る、fb.c:397-398 は 128 行ごとに割って逃げている

```
kernel/src/arch/smp.h:41	   a nested one, runs its batch alone). The callbacks run with interrupts off, on the idle task's
kernel/src/dev/fb.c:397	        /* The caller's interrupts stay off during a batch (cpu_parallel_for): batches of 128 rows
kernel/src/dev/fb.c:398	           let them in between, the alarms due on this CPU (sys/alarm.h) among them. */
kernel/src/dev/fb.c:399	        for (int first=0;first<h;first+=128) {
```

- 判定：**当たり**。fb.c は呼ぶ側の都合（alarm を遅らせない）を、128 行という決め打ちの区切りで受けている。ただし `present_parallel` は既定で切ってある（02-398、fb.c:391〜393）。
- 分類：**ポーリング**（呼ぶ側の空回り）、**決め打ちの数**（128 行）。

#### 04-208 idle は HLT だけ（sched.c:535）

```
kernel/src/sys/sched.c:535	        __asm__ volatile("sti; hlt" : : : "memory");
```

- 判定：**当たり**。
- 分類：四つの外（idle の方針の欠け）。

---

## 06（dev）の所見のうち、時間に関わるもの

#### 06-50 (1) 失った割り込みを隠す保険の poll：NVMe の 10 ms、xHCI の 10 ms と、4 ms の poll への退避

- 原文は 03(c)-91・92・93・95、01-381。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-51 (2) 時間でなく回数で数える待ち：power.c の io_wait の輪、serial.c の 100000 回、entropy.c の空の輪

```
kernel/src/dev/entropy.c:113	void entropy_init(void) {
kernel/src/dev/entropy.c:114	    for (int i = 0; i < 64; i++) {
kernel/src/dev/entropy.c:115	        entropy_add(rdtsc()); /* into this CPU's feed, which the first read folds in */
kernel/src/dev/entropy.c:116	        for (volatile int j = 0; j < (i & 7) * 50; j++) {
kernel/src/dev/entropy.c:117	        }
kernel/src/dev/entropy.c:118	    }
kernel/src/dev/entropy.c:119	    kprintf("random: entropy pool ready\n");
```

power.c と serial.c の原文は 03(c)-110・103。

- 判定：**当たり**。
- 分類：**ポーリング**、**決め打ちの数**。

#### 06-52〜55 (3) 割り込みを閉じたまま秒の単位で回る待ち：NVMe の停止は 1 台あたり最大 10 s、AHCI は 10 s、PCI の復帰は機能ごとに 1 s、IOMMU は lock を持ったまま 100 ms

```
kernel/src/dev/nvme.c:412	/* Normal shutdown notification before power goes away (section 7.6.2): the controller writes its
kernel/src/dev/nvme.c:413	   cache back and the drive does not count an unsafe shutdown. Interrupts are already off. */
kernel/src/dev/nvme.c:417	    for (int i = 0; i < nctrls; i++) {
kernel/src/dev/nvme.c:421	        uint64_t us = c->rtd3e < 1000000 ? 1000000 : c->rtd3e > 10000000 ? 10000000 : c->rtd3e;
kernel/src/dev/nvme.c:422	        for (uint64_t deadline = clock_after(us * NS_PER_US);
kernel/src/dev/nvme.c:424	            pause();
kernel/src/dev/ahci.c:457	   no DMA after this. Interrupts are already off. */
kernel/src/dev/ahci.c:468	            if ((p->cache && flush(p)) || command(p, fis, false, 0, READY_NS)) all = false;
kernel/src/dev/pci.c:913	        r->ready = r->since + (rtr ? rtr : READY_NS);
kernel/src/dev/pci.c:1340	        if (watching) pause();
kernel/src/dev/pm.h:25	   Quiesce callbacks run on one CPU with its interrupts off: they may only poll
kernel/src/dev/pm.h:26	   (io_wait, the clock: clock_spin and clock_now_ns, sys/clock.h), never sleep.
kernel/src/dev/pm.h:32	   Resume callbacks run on CPU 0 with interrupts on, every other CPU still
kernel/src/dev/pm.h:33	   parked and no other thread running (the AML of _WAK is frozen too,
kernel/src/dev/pm.h:34	   dev/acpi.h): they may poll the clock but must not sleep.
```

IOMMU は 03(b)-79。

- 判定：**一部当たり**。NVMe（制御器ごとに順に、最大 10 s を pause で）と AHCI（`command` を `false`＝眠らない形で、`READY_NS`＝10 s まで。眠らない形は 279 行の pause）は、pm.h:25〜26 のとおり割り込みを閉じた quiesce の中で回る：当たり。PCI の復帰は pm.h:32 のとおり「割り込みは開いている」で、閉じたままではない（眠らずに pause で回る点は当たり、1340 行）。IOMMU は当たり。
- 分類：**ポーリング**、**決め打ちの数**。復帰を CPU 0 だけで回す点（pm.h:32〜33）は **一つの CPU への寄り**。

#### 06-56〜59 (4) 事象に装置の時刻が付かない：入力の事象に時刻の欄が無い、音の位置には読んだ時の clock の時刻を付けるだけで ABI に出ない、GOP の画面の拍は EDID からの推測

```
kernel/src/dev/input.h:19	struct key_event {
kernel/src/dev/input.h:20	    uint16_t key;  /* KEY_* or character (lowercase/shifted as typed) */
kernel/src/dev/input.h:21	    uint8_t mods;
kernel/src/dev/input.h:22	    bool pressed;
kernel/src/dev/input.h:23	};
kernel/src/dev/input.h:27	struct mouse_event {
kernel/src/dev/input.h:28	    int dx, dy, wheel;
kernel/src/dev/fb.c:50	/* The firmware's monitor (fb.monitor): its ids, and the refresh of the firmware's mode: the first
kernel/src/dev/fb.c:51	   detailed timing of that size (the native one comes first), else the nominal rate nearest 60 Hz
kernel/src/dev/fb.c:52	   of a standard or established timing of that size, as firmware sets its other modes. */
kernel/src/dev/fb.c:72	    fb.refresh_mhz = best ? best->refresh_mhz : 0;
```

音は 03(f)-177。`vm/ac97.c:94,108` は範囲の外で照らしていない。

- 判定：**当たり**。GOP の拍は、firmware がどの mode を選んだかを EDID の表から推し量った値（fb.c:63〜71）で、装置から来る vblank ではない。
- 分類：四つの外（時刻の印の欠け）。fb.c:53 の 60 Hz を基準に選ぶ所は **決め打ちの数**。

#### 06-60 (5) 遅れを大きな緩衝で吸う：音の書き手は最大 170 ms 先行、無音の出口は 10 ms の塊

- 原文は 03(a)-30、03(e)-157。判定：**当たり**。分類：**決め打ちの数**。

#### 06-61 (6) 規格の値を丸める：NVMe の RTD3E を 10 s で切る

- 原文は 06-52〜55 の nvme.c:421。判定：**当たり**（下は 1 s で持ち上げ、上は 10 s で切る）。06 の 283〜284 行が引く NVMe Base 2.4 の文言はこの照らしでは開いていない（未確認）。
- 分類：**決め打ちの数**。

#### 06-175 acpi：凍結中の待ちは sti/pause/cli で回る（acpi.c:278-312）

```
kernel/src/dev/acpi.c:291	        } else if (timeout != 0xFFFF || now - start < FROZEN_WAIT_NS) {
kernel/src/dev/acpi.c:295	            if (f & (1u << 9)) sti();
kernel/src/dev/acpi.c:296	            pause();
kernel/src/dev/acpi.c:297	            cli();
kernel/src/dev/acpi.c:303	            sti(); /* let interrupts and other threads give the unit */
kernel/src/dev/acpi.c:304	            schedule();
kernel/src/dev/acpi.c:305	            cli();
```

- 判定：**当たり**。2 s（`FROZEN_WAIT_NS`）を過ぎた無期限の待ちは、凍結中でも `schedule()` で他の thread を走らせる（298〜306 行）。
- 分類：**ポーリング**、**決め打ちの数**。

#### 06-206・213・219 IOMMU：一つの lock の下で割り込みを閉じて、ユニットごとに最大 100 ms

- 原文は 03(b)-79。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-215 IOMMU の故障の報告は毎秒 8 件まで（iommu.c:185-200）

```
kernel/src/dev/iommu.c:189	    uint64_t now = clock_now_ns() / NS_PER_S;
kernel/src/dev/iommu.c:195	    bool more = shown == 8, show = shown < 8;
```

- 判定：**当たり**。分類：**決め打ちの数**。

#### 06-278・280 NVMe：MSI があっても 10 ms ごとに見る、MSI が無ければ 1 ms、I/O 30 s・admin 5 s、ready_ns は少なくとも 2 s

- 原文は 03(b)-74、03(c)-95。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-313・314 AHCI：10 ms と 1 ms の poll。停止では STANDBY IMMEDIATE を出し、割り込みを閉じて最大 10 s 回る（458-473）

- 原文は 03(b)-75、06-52〜55 の ahci.c:457・468。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-336〜350 USB の核：一本の usb thread、入れ子の待ち、キーの繰り返しは外側の輪だけ、IMOD 250 µs、4 ms の poll への退避、1 s ごとの見直し、時間の値（control 2 s、command 3 s＋abort 5 s、debounce 100 ms、reset 500 ms を 1 ms ごと、hub 10 ms ごと最大 500 ms、停止 20 ms を pause、落ち着き 500 ms）

- 原文は 03(b)-72・73、03(c)-91〜94・101。xhci.c の停止：

```
kernel/src/dev/xhci.c:1510	/* Power change (dev/pm.h): halt every controller, which ends its DMA (xHCI 4.24: within 16 ms).
kernel/src/dev/xhci.c:1511	   The disks behind it were flushed before. Interrupts are off: polled. */
kernel/src/dev/xhci.c:1519	        for (uint64_t deadline = clock_after(20 * NS_PER_MS);
kernel/src/dev/xhci.c:1521	            pause();
```

- 判定：**当たり**。06-352「BOT の転送を待つ間は、キーの繰り返しも hub の変化も止まる」：`usb_bulk` は `current_task != usb_task` なら断る（825 行）ので、転送は usb thread の上で `hc_wait` に入り、`hc_wait` は `poll_all()`（463 行）だけを回して `usbhid_tick`・`usbhub_work`（1485、1495 行）を回さない。当たり。
- 分類：**ポーリング**、**決め打ちの数**、一本の thread は **一つの CPU への寄り**（03(d)-132）。

#### 06-380 usbhid：キーの繰り返しはカーネルが作る。500 ms と 33 ms

- 原文は 03(b)-71。判定：**当たり**。分類：**決め打ちの数**。

#### 06-408 usbmsc：TEST UNIT READY を 250 ms × 40 回、合わせて 10 s まで

```
kernel/src/dev/usbmsc.c:122	    for (int tries = 0; tries < 40; tries++) {
kernel/src/dev/usbmsc.c:134	        sleep_until(clock_after(250 * NS_PER_MS));
```

- 判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-438・441・443・445〜447・450 音：実機の音の時計は無音の出口、alarm で 10 ms の塊、位置は clock_now_ns から、170 ms の先行、混ぜる仕事はカードの割り込みの中、ABI に時刻が出ない、流れを閉じるとき最大 1 s 待つ

- 原文は 03(a)-30、03(e)-157、03(f)-177、03(b)-78（audio.c:641）。`audio.c:47-57,184-238`（カードの割り込みの中の混ぜ）と `vm/ac97.c` はこの照らしでは読んでいない。
- 判定：**当たり**（読んだ範囲）。分類：**ポーリング**、**決め打ちの数**、**VM の判定の混入**（769 行、RUNS の根拠）。

#### 06-480〜483 画面：hold は presents_drain が 100 µs ごと、GOP の拍は EDID からの推測

- 原文は 03(b)-66、06-56〜59。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-536 PS/2：待ちは 1 回につき 100 ms までの pause の poll、復帰では十数個の命令をそれぞれ最大 100 ms

- 原文は 03(c)-110 の input.c:179〜185。`input.c:217-263,265-285`（復帰の命令の並び）は読んでいない。判定：**当たり**（読んだ範囲）。分類：**ポーリング**、**決め打ちの数**。

#### 06-575〜582・589〜590 pm：quiesce は割り込みを閉じて poll だけ、resume は CPU 0 で他の CPU を止めたまま眠れない、50 ms・20 ms ごとの poll、GRACE 5 s・STALL 15 s・FORCE 60 s・STOP 3 s

- 原文は 03(b)-77、06-52〜55 の pm.h:25〜34。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**、**一つの CPU への寄り**（CPU 0 で一つずつ）。

#### 06-616 power.c：待ちは時間でなく io_wait の回数

- 原文は 03(c)-110。判定：**当たり**。分類：**ポーリング**、**決め打ちの数**。

#### 06-626・633 休止：像は割り込みを閉じて他の CPU を止めた CPU 0 一つで取る

```
kernel/src/dev/hibernate.c:293	void hibernate_snapshot(void) {
kernel/src/dev/hibernate.c:301	    for (unsigned k = 0; k < nr; k++)
kernel/src/dev/hibernate.c:302	        for (uint64_t pfn = r[k].base / PAGE_SIZE; pfn < r[k].end / PAGE_SIZE; pfn++) {
kernel/src/dev/hibernate.c:311	            memcpy(h.raw + (size_t)fill * PAGE_SIZE, src, PAGE_SIZE);
```

- 判定：**一部当たり**（確かめた範囲）。一つの輪で全 RAM の頁を回ることは原文にある。割り込みを閉じて CPU 0 で呼ぶこと（06 が引く `pm.c:186-187`）は、その行を読んでいないので未確認。
- 分類：**一つの CPU への寄り**（未確認の部分を除く）。

#### 06-654 entropy：起動で rdtsc を 64 回と空の輪を回して「ready」と書く

- 原文は 06-51 の entropy.c:113〜119。判定：**当たり**。分類：**ポーリング**（回数の空回り）、**決め打ちの数**。

#### 06-675・676 serial：port と速さは手で決めてある、flush は 100000 回で打ち切る

- 原文は 03(b)-65、03(c)-103。判定：**当たり**。分類：**決め打ちの数**、**ポーリング**。

---

## 二つ目の依頼の数えと、照らしていないもの

- 判定の内訳（この節の項目）：**外れ**は 05-122（CPU の TSC の比べは「順に」ではなく一斉）。**一部当たり**は 05-336〜340（events の位相は一周期以上遅れたときだけ捨てる）、06-52〜55（PCI の復帰は割り込みを開いた文脈）、06-626・633（割り込みの状態は未確認）。ほかは当たり。
- 新しく原文で確かになった重いもの：
  - alarm の「同じ alarm を同時に立てる」契約の欠けと、それを破る serial.c（05-162〜164）。
  - 利用者の時計が process ごとの一度きりの写しで、核の 64 秒目の直しも CPU ごとの offset も知らず、nvidia の移植に二つ目がある（05-46〜52）。
  - 頁違反の処理が全部割り込みを閉じた区間（04-116）。
  - LFENCE を直列にする設定が無いまま `lfence; rdtsc` で時計を読む（05-13・14）。
- 照らしていないもの：gui（`wm.c:2456-2460` の wm_vblank、`wm.c:127`）、vm（`vm/ac97.c`）、mm（`vmm.c:795,800-816,861-997`）、`sys/stream.c`、`channel.h:14`、`audio.c:47-57,184-238`、`input.c:217-285`、`pm.c:186-187`、`entropy.c` の 113〜120 行以外、SDM・APM・PPR・NVMe Base・HDA の文言（どれも開いていない）。Zen 4 が TSC-deadline・CPUID 15h・ARAT を持つか、LFENCE が既定で直列かも未確認。
