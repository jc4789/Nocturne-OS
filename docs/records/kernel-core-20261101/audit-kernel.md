# 残りのカーネルの監査（中核と合成の道を除く）

## 依頼文（全文）

あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機）の監査役。読むだけ。コードを変えない。ビルドも QEMU も動かさない。別の書き手が同じ作業木でカーネルの中核（`kernel/src/mm/`、`arch/percpu.h`・`preempt.h`・`cpuset.h`、`sys/thread`・`process`・`sched`・`wait`・`epoch`、今は消えていて書き直し中）を書いている。別の監査役が窓・合成・GPU・入力・音の道（`gui/`、`dev/fb.c`、`dev/input.c`、`dev/usbhid.c`、`dev/audio.c`、`vm/virtio_gpu.c`）を読む。あなたはそれ以外のカーネルを読む。

# 持ち主の要件
GUI・GPU・合成が第一級。カーネルが原因のコマ落ち・かくつきがゼロ。規模の例：600 Hz の画面、3 枚の画面、ゲームと YouTube と Discord を同時に開いて、カーネルのせいで落ちるコマが無い。CPU・メモリー・装置・プログラムの数が増えても成り立つこと（上は 2 ソケットの EPYC、論理 CPU 1024、NUMA）。対象は今の PC（Zen 4 以上）。Linux・UNIX・POSIX を標準とも手本とも見なさない。shim（何もしない lock、偽の関数）・stub・決め打ちの値・特例の分岐・小さな固定の上限は欠陥。VM の判定が `kernel/src/vm/` の外にあれば欠陥。

# 読む範囲
`kernel/src/` の `arch/`（irq・apic・ioapic・lapic_timer・idt・isr.asm・smp・idle・fpu・cpu・topology）、`sys/`（syscall・object・channel・events・watch・clock・alarm・log・stream・prof・proc・keep・panic）、`fs/`、`dev/`（pci・pciuser・nvme・ahci・xhci・usb*・iommu*・acpi・pm・power・hibernate・blk・entropy・serial・refclock）、`net/`、`lib/`、`boot.c`・`init.c`。地図は `.claude/rules/map.md`。codegraph（MCP の `codegraph_explore`）と rg を使う。

# 問い
1. 割り込みと時間：割り込みを止める長い区間、割り込みの中の重い仕事、見回り（polling）や固定間隔の tick、回数で回る spin、timer の粒と遅れ。これらは合成の締切（1.67 ms）を落としうるか。
2. 錠：大域の錠が熱い道にあるか（syscall・事象・channel・block の I/O・網）。錠の順の約束はあるか。
3. 規模：O(N) の全走査（プロセス・thread・handle・窓・装置の数で）、固定の大きさの表、小さな上限、CPU の数で増える大域の仕事。
4. I/O：NVMe・xHCI の queue は CPU ごとか、割り込みは MSI-X で散るか、完了の待ちは眠るか spin か。
5. 中核（sched・mm・wait・thread・process・epoch・percpu・preempt）に何を求めているか：関数名・欄・呼び方の一覧（中核の書き手の参考になる形で）。
各欠陥に、ファイル:行、何が起き、規模の例でどう壊れるか、重さ（コマ落ちに直結／規模で詰まる／きれいさ）を書く。直し方の案は短く添えてよい。

# 報告
`docs/records/kernel-core-20261101/audit-kernel.md` へ日本語で全文（この依頼文の全文を先頭に）。返事にも要点を日本語で返す。サブエージェントをさらに出さない。この Windows の Python で書くなら `newline='\n'`。

---

## 範囲と証拠の種類

- 証拠は静的な読みだけ。ビルド・試験・QEMU は動かしていない。
- 行番号は監査の時点の作業木（`c5e0e7b6` の上の未追跡ファイルを含む）。
- 中核のファイル（`kernel/src/mm/`、`arch/percpu.h`・`preempt.h`・`cpuset.h`、`sys/thread`・`process`・`sched`・`wait`・`epoch`・`address_wait`）は監査の途中に書き手が置いた。中核の中身は監査していない。呼ぶ側の名前と中核の見出しの食い違いだけを五章に書く。
- 重さの印：
  - **［落］** コマ落ちに直結する。
  - **［詰］** 規模で詰まる。
  - **［清］** きれいさ（規則の違反、決め打ち、shim の類）。

## 要点

1. **USB の装置をつなぐ間、マウスとキーボードが 100 ms から 10 s 止まる。**［落］ xHCI はすべての制御器とすべての class driver を一本の "usb" thread で回す。その thread が port の reset・hub・大容量記憶の準備で眠る間、HID の report は処理されない。
2. **割り込みを止めたまま CPU の数や記憶の量に比例する仕事が三つある。**［落］［詰］ `entropy_get` が全 CPU の feed を畳む。block cache の `grow` が全 block を rehash する。AMD・Intel の IOMMU の `sync` が全 unit を最大 100 ms ずつ spin で待つ。
3. **syscall.c に mm の仕事と一本の大域 spinlock `shared_lock` がある。**［詰］ 共有メモリーの fault・program の text の fault・pager がこの錠を通り、錠の中で線形の list を歩き、frame を確保して 0 で埋め、page を map する。
4. **I/O の queue は CPU ごとでない。**［詰］ NVMe は制御器ごとに I/O の queue が一組で、命令は一度に一つ、128 KiB の bounce buffer を memcpy で通す。AHCI は slot 0 だけで NCQ が無い。xHCI は interrupter 0 の event ring が一つ。割り込みは MSI-X の vector を CPU に散らすが、`.node = -1` で NUMA を見ず、一度置いたら動かさない。
5. **網は一本の大域 mutex `net_lock` を syscall の入口から出口まで握る。**［詰］ adapter は一つ、RX の queue は 64、ARP は 16 件。
6. **時間の ABI は ms だけ。**［落］ 待ち・timer・sleep・uptime の単位が ms で、600 Hz の 1.67 ms の拍を表せない。カーネルの内側は ns の期限と TSC-deadline を持つので、ABI だけが粒を落としている。
7. **錠の順の検査の枠はあるが、ranked の錠が一つも無い。**［清］ `enum spin_rank` は `SPIN_UNRANKED` だけ。順はファイルごとの注釈にしか無い。
8. **規則の違反が二つある。**［清］ `arch/cpu.c` に VM の判定（`cpu_features.hypervisor`）が `vm/` の外にある。`net/rtl8125.c` の頭に、他の OS の driver の source に従ったという記述がある。
9. **中核の見出しと呼ぶ側が食い違う。** 呼ぶ側は `sched_realtime(SCHED_PRIORITY_IRQ/INPUT/COMPOSITOR/SOUND)` を使う。新しい `sys/sched.h` には `sched_fair` と `sched_deadline` しか無い。

良い所も書く。直すときに壊さないためである。

- 周期の tick が無い。期限は CPU ごとの pairing heap と LAPIC の TSC-deadline で、期限は [early, at] の幅で渡せる（`sys/alarm.c`、`arch/lapic_timer.c`）。
- log の書き手は CPU ごとの lock の無い ring（`sys/log.c`）。
- handle の表は epoch の下で lock なしに読む（`sys/object.c`）。
- channel の錠は link ごと（`sys/channel.c`）。
- 割り込みの vector の表は CPU ごとで、置き場所は使用の少ない CPU を選ぶ（`arch/irq.c` の `place()`）。
- カーネルは割り込みの戻りで preempt できる（`arch/isr.asm`、`preempt_count`・`sched_resched`）。

---

## 一　割り込みと時間

### 1.1 割り込みの入口で毎回 `entropy_add` を呼ぶ ［清］

- 場所：`arch/irq.c:221`（`isr_interrupt`、208 行から）。
- 起きること：装置の割り込みのたびに `entropy_add(rdtsc() ^ v << 56)` を呼ぶ。
- 規模の例：600 Hz の画面三枚の vblank、NVMe、NIC、USB の割り込みがすべて通る。`entropy_add` が CPU ごとの feed に書くだけなら費用は小さい。重いのは読み手の側（1.3）。
- 案：feed は CPU ごとのままにする。読み手が全 CPU を畳まない形にする（1.3）。

### 1.2 迷子の vector で割り込みの中から `kprintf` ［清］

- 場所：`arch/irq.c:240`。
- 起きること：持ち主の無い vector を受けると割り込みの中で `kprintf` を呼ぶ。log の書き込みは lock なしだが、1.5 の self-IPI を起こす。
- 規模の例：害は小さい。迷子が続く装置があると、割り込みのたびに log と IPI が一回ずつ増える。
- 案：数えるだけにし、報告は割り込みの外で一度にする。

### 1.3 `entropy_get` が割り込みを止めて全 CPU の feed を畳む ［落］［詰］

- 場所：`dev/entropy.c:80-97`（`entropy_get`）。呼び手は `sys/syscall.c:2756` の `sys_random`。
- 起きること：大域の `pool_lock` を握り、割り込みを止めて、すべての CPU の feed を `pool_add` で 4 回ずつ畳む。他の CPU の cache line を読む。`sys_random` は 256 byte ごとに一回呼ぶ。
- 規模の例：1024 CPU では一回でおよそ 4096 回の `pool_add` と 1024 本の遠い cache line になる。割り込みを止めた区間が 0.5 ms の桁になる。ブラウザーの TLS と `crypto.getRandomValues` が呼ぶたびに、その CPU の vblank と timer の割り込みが遅れる。複数の CPU が同時に呼ぶと `pool_lock` で並ぶ。
- 案：CPU ごとに種を持つ DRBG にする。全 CPU の feed を畳むのは種を入れ替える時だけにし、割り込みを開けたまま行う。

### 1.4 `alarm_interrupt` が期限の来た alarm を上限なしに一度に回す ［落］

- 場所：`sys/alarm.c:177`（`alarm_interrupt`）。
- 起きること：一回の割り込みの中で、期限の来た alarm をすべて取り出してその callback を呼ぶ。割り込みは止まったまま。数の上限が無い。
- 規模の例：Discord・YouTube・ゲームが細かい timer を多数持つと、同じ期限に重なった alarm の callback（多くは `wq_wake`）が一つの割り込みで連続する。callback は wait queue の lock を取り、起きる thread の数だけ仕事が増える。
- 案：一回の割り込みで回す数か時間に上限を置き、残りは次の期限で再び arm する。callback は起こすだけに限る。

### 1.5 `alarm_clock_changed` が全 CPU へ IPI ［詰］

- 場所：`sys/alarm.c:220`。
- 起きること：wall clock が変わると、すべての CPU に `cpu_alarm` を送る。
- 規模の例：1024 CPU で 1024 本の IPI。変わるのは稀なので、コマには効かない。
- 案：wall の期限を持つ CPU だけへ送る。

### 1.6 one-shot の LAPIC timer は早めに鳴り、arm し直す ［清］

- 場所：`arch/lapic_timer.c`。
- 起きること：TSC-deadline が無い CPU では、比の下限で数えるので早めに鳴り、残りを arm し直す。TSC-deadline のある CPU（Zen 4 以上の対象）は正確。
- 規模の例：対象の CPU では起きない。

### 1.7 log の self-IPI の中で serial と wake を回す ［落］

- 場所：`sys/log.c` の `tell()`、`told_isr`（252 行）、`tell_all`（602 行）。`dev/serial.c` の `fill`（213 行）と `serial_log_ready`（271 行）。
- 起きること：log を書くたびに自分へ IPI（`VEC_LOG`）を送る。その割り込みの中で `tell_all` が `serial_log_ready`、`wq_wake_all`、`stream_system_log_told`（`object_notify`）を呼ぶ。`serial_log_ready` は大域の `serial_lock` を取り、`fill` が `log_read` と port への `outb` を 16 回行う。`log_read` の一回りは `scan()`（`sys/log.c:372`）ですべての ring を見る。
- 規模の例：
  - 一回の log が、割り込みの中で 16 回の port I/O（UART の一回は 1 µs の桁）と全 CPU の ring の走査を起こす。
  - ブラウザーの `--log-console` や driver の多い log で、vblank の CPU が割り込みの中に居続ける。
  - 1024 CPU では `scan()` が 1024 個の ring を毎回見る。
  - すべての CPU が `serial_lock` を取りに来る。
- 案：割り込みは「書いた」の印を立てるだけにする。serial への吐き出しと wake は一本の低い優先度の thread に移す。読み手の round は、書かれた ring の bitmap だけを見る。

### 1.8 idle は MWAIT の C1 だけ ［清］

- 場所：`arch/idle.c:41`（`cpu_idle`）。
- 起きること：`_CST` を評価しないので、深い C state を使わない。起きる遅れは小さいので、コマには効かない。電力の面の欠け。
- `cpu_perf_set_policy`（114 行）は CPU ごとに kthread を一本ずつ作る。1024 CPU で 1024 本になる。

### 1.9 回数や時間で回る spin

| 場所 | 中身 | 重さ |
|---|---|---|
| `dev/iommu_amd.c:544`（`sync`）、536 行の `spin_take(&amd_lock)` | 全 unit に completion wait を送り、unit ごとに最大 100 ms spin。大域の錠を握り割り込みを止めたまま。呼び手は attach・block・`amd_flush`（626 行）。 | ［落］［詰］ |
| `dev/iommu_intel.c:972-980`・617 行 | 同じ形。`units_lock` の下。 | ［落］［詰］ |
| `dev/ahci.c:178` | COMRESET で `clock_spin` 2 ms。 | ［清］ |
| `dev/nvme.c` の `nvme_shutdown` | `pause` で spin。停止の時だけ。 | ［清］ |
| `dev/acpi.c` の stall | `clock_spin`。uACPI の stall は仕様どおり短い。 | ［清］ |
| `dev/acpi.c` の semaphore | 凍結中に spin。 | ［清］ |
| `net/rtl8125.c` | probe と電源の変更で `clock_spin`。 | ［清］ |

- IOMMU の規模の例：DMA の map を外すたび（GPU・NVMe・NIC の buffer の解放）に `sync` が走る。unit が多い EPYC（unit は数十）では、その全部を割り込みを止めて待つ。その CPU の合成は締切を落とす。
- IOMMU の案：invalidation の完了は unit ごとの queue で待ち、map の解放は batch にまとめて一度だけ待つ。待ちは眠る wait にし、大域の錠を unit ごとの錠に分ける。

### 1.10 USB の port・hub・大容量記憶の待ちが "usb" thread を眠らせる ［落］

- 場所：
  - `dev/xhci.c:1569`（"usb" thread の起動、すべての制御器に一本）。1453 行で優先度 `SCHED_PRIORITY_INPUT`。
  - `dev/xhci.c:1132`（接続の debounce 100 ms）、1108-1116 行（reset で最大 500 ms + 10 ms）、USB3 の link training で最大 1 s を 3 回。
  - `dev/usbhub.c:129`・176・204・213・215・229（hub の電源と reset の sleep）。
  - `dev/usbmsc.c:122-134`（TEST UNIT READY を 250 ms ずつ 40 回、最大 10 s）。
- 起きること：HID の report の callback は `hc_events` の中で同じ "usb" thread が回す。上の待ちはその thread を `sleep` で眠らせ、その間 event ring を読まない。
- 規模の例：USB の記憶装置や hub をつなぐと、その間マウスとキーボードが止まる。短くて 100 ms、記憶装置の準備で最大 10 s。ゲームの最中に headset をつなげば入力が凍る。入力のかくつきは持ち主の要件のコマ落ちに当たる。
- 案：
  - event ring の処理（HID の report を含む）は割り込み thread で回し、列挙の仕事から切り離す。
  - 列挙と class driver の準備は port ごとの仕事として別の worker に渡し、待ちは期限の alarm で再開する状態機械にする。
  - 制御器ごとに event の thread を持つ。

### 1.11 timer の粒は ABI で ms に落ちる ［落］

- 場所：
  - `sys/syscall.h:11-18` の `syscall_deadline` と `address_deadline`（ms から期限を作る）。
  - `sys/syscall.c` の `sys_events_timer`（2802 行あたり）、`SYS_SLEEP`（2841 行）、`SYS_UPTIME`（ms）。
- 起きること：カーネルの内側は ns の期限と幅つきの alarm を持つ。ABI の入口で ms に丸める。
- 規模の例：600 Hz の拍は 1.667 ms。ms の待ちでは 1 ms か 2 ms にしか合わせられず、毎拍 0.33 ms ずつずれる。合成・音・ゲームの frame pacing が拍を外す。`n_event` に時刻が無い（`sys/events.c:85-91`・419-421 行）ので、受け手は事象の起きた時刻を知れない。
- 案：ABI の待ちと timer を、時計の種類つきの絶対 ns の期限と許す幅（[early, at]）で受ける。`n_event` に起きた時刻（monotonic の ns）を載せる。`common/abi.h` と `docs/nocturne-native-abi.md` を同時に直す。

---

## 二　錠

### 2.1 錠の順の約束は注釈だけ ［清］

- 場所：`arch/spin.h:27-30` の `enum spin_rank { SPIN_UNRANKED, SPIN_RANKS };`。
- 起きること：MCS spinlock に rank の検査の枠がある。rank が `SPIN_UNRANKED` しか無いので、検査は何も見ない。
- 順が書いてあるのは注釈だけ：`sys/events.c:15-25`、`fs/document.h:1-14`、`net/net.c:11-19`、`sys/syscall.c` の `shared_lock` の注釈、`dev/pciuser.c`、`dev/xhci.c`。
- 規模の例：錠が増えるほど、注釈の順と実際の順の食い違いは見えなくなる。deadlock は負荷の高い時にだけ出る。
- 案：層ごとの rank を `enum spin_rank` に並べ、すべての spinlock に付ける。mutex と wait queue の lock にも同じ rank を使う。

### 2.2 `shared_lock`（syscall.c）は大域で、fault の道にある ［詰］［落］

- 場所：`sys/syscall.c:357`（`static struct spinlock shared_lock;`）。`shared_fault`（1777 行、錠は 1798 行あたり）、`owner->shared_mappings` の線形の walk（1800 行）、`shared_withdraw`（449-468 行）の大域 list `shared_all`。
- 起きること：
  - 共有メモリーの fault は大域の錠を割り込みを止めて握る。
  - 錠の中で、その process の共有の map を線形に歩く。
  - 錠の中で `shared_frame`（frame の確保と 0 埋め）と `vmm_map_page` を呼ぶ。
  - program の text の fault も同じ錠を通る。
- 規模の例：
  - ブラウザーの子 process（媒体・Worker・保存）、ゲーム、Discord が共有メモリーと text の fault を同時に起こすと、すべての CPU がこの一本の錠に並ぶ。
  - 4 KiB の 0 埋めは錠の中で 1 µs の桁、大きな共有の初回の触りでは連続する。
  - 割り込みが止まっているので、並んだ CPU の vblank と timer も遅れる。
  - 1024 CPU では、program の起動の嵐で錠の待ちが ms の桁に伸びる。
- 案：共有の object・pager・program の cache を `mm/` に移す。錠は object ごとと address space ごとに分ける。frame の確保と 0 埋めは錠の外で行う。共有の map の検索は区間の木（`lib/span`）にする。

### 2.3 block cache の錠は C: に一本 ［詰］［落］

- 場所：`fs/cache.c`（cache ごとに spinlock 一本。C: の cache は一つなので大域）。`grow`（145-169 行）。
- 起きること：
  - block の検索・挿入・追い出しがすべて一本の spinlock を通る。
  - `grow` は錠を握り割り込みを止めたまま、すべての block を rehash する。
  - cache は記憶の 1/16 を持つ（`fs/store.c:335`）。
- 規模の例：32 GiB の機械で block はおよそ 512K 個。一回の rehash で ms の桁、割り込みを止めた区間になる。1 TiB の EPYC では数百 ms になる。rehash の間はすべての CPU の file の読み書きも止まる。
- 案：hash の表を分割（CPU の数か、block の番号の高い bit で）し、錠を桶ごとにする。大きくするときは古い表と新しい表を並べて少しずつ移す。

### 2.4 `lib/idtable.c` の `id_table_room` も呼び手の錠の下で rehash ［詰］

- 場所：`lib/idtable.c:10-36`。
- 起きること：表が満ちると、呼び手が握った spinlock の下で全項目を移す。
- 規模の例：項目が多い表ほど、伸びる一回の区間が長い。
- 案：2.3 と同じく、少しずつ移す形にする。

### 2.5 store の cursor の表 ［詰］

- 場所：`fs/store.c:700-730`。
- 起きること：cursor の表は 2 × CPU の数の大きさ。大域の spinlock の下で線形に探し、`memmove` で詰め、古いものを追い出す。
- 規模の例：1024 CPU で 2048 項目の線形の探しと memmove を、file の操作のたびに一本の錠で行う。
- 案：CPU ごとか document ごとの cursor にする。

### 2.6 block の層の門 ［詰］

- 場所：`dev/blk.c`（`MAX_BLK 16`、大域の `gate_lock`）。
- 起きること：I/O ごとに大域の `gate_lock` を二回取る。API は同期だけで、queue も優先度も無い。
- 規模の例：ゲームの asset の読み込み、ブラウザーの cache、YouTube の buffer の書き込みが一本の門で並ぶ。読み込みの I/O が合成の process の page fault の読みと区別されない。
- 案：block の層を非同期の要求 queue（CPU ごとの提出の queue、完了の通知）にし、要求に優先度を持たせる。

### 2.7 網の `net_lock` ［詰］

- 場所：`net/net.c:40`（大域の mutex `net_lock`）。
- 起きること：網のすべての syscall が入口から出口まで `net_lock` を握る。網の仕事は一本の "net" thread で回し、adapter は一つ（`nif`）。
- 規模の例：Discord の音声、YouTube の動画の取得、ブラウザーの多数の TCP が一本の mutex に並ぶ。受信の処理が一本の thread なので、ゲームの UDP の遅れが他の流れの処理量に引きずられる。
- 案：socket ごとの錠、受信の queue ごとの処理（NIC の RSS の queue と CPU の対応）にする。

### 2.8 channel の名前の錠 ［清］

- 場所：`sys/channel.c` の `names_lock`。
- 起きること：大域だが、connect と listen の時だけ取る。熱い道ではない。

### 2.9 事象の queue の錠の下の仕事 ［詰］

- 場所：`sys/events.c:359-433`（`events_wait`）。
- 起きること：`q->lock` の下で、最大 64 個の `ready()` を呼び、arm し直す。level-triggered なので、待つたびに準備のできた源を問い直す。
- 規模の例：ブラウザーや Discord のように多数の handle を一つの queue で待つと、待つたびに 64 個ずつ問い直しが錠の中で走る。
- 案：源の側から ready の印を queue へ push し、待ちは印のある源だけを見る。

### 2.10 `object_notify` は wait queue の錠の下で全 callback ［詰］

- 場所：`sys/object.c:79`。
- 起きること：`wq_wake_all` が、wait queue の錠の下ですべての callback を呼ぶ。watcher の数に比例する。
- 規模の例：一つの object（窓、共有の buffer）を多数の queue が見ていると、通知のたびに錠の中が伸びる。割り込みの中から呼ばれる場合（`dev/pciuser.c` の `claim_isr`、1.7 の log）は、割り込みの中が伸びる。

### 2.11 `pciuser` の `claim_isr` ［落］

- 場所：`dev/pciuser.c:101`（`claim_isr`）。
- 起きること：割り込みの中で `irq_lock` を握り、`wq_wake_all` と `object_notify` を呼ぶ。
- 規模の例：アプリの driver（NVIDIA の `nvrm`）の割り込みは GPU の完了と vblank を運ぶ。600 Hz で三枚なら 1800 回／秒以上。通知の先が多いと、その割り込みの区間が伸びる。
- 案：割り込みは pending の bit を立てて一つの wait を起こすだけにする。

---

## 三　規模

### 3.1 線形の走査

| 場所 | 走査の対象 | 錠 | 重さ |
|---|---|---|---|
| `sys/syscall.c:222-249`（`sys_proclist`） | すべての thread | `thread_lock`、割り込み止め | ［詰］ |
| `sys/syscall.c:166-203`（`report_memory_exhaustion`） | すべての thread、process ごとに `kprintf` | `thread_lock` | ［詰］ |
| `sys/syscall.c:1800`（`shared_fault`） | process の共有の map | `shared_lock` | ［詰］ |
| `sys/syscall.c:449-468`（`shared_withdraw`） | 大域の `shared_all` | `shared_lock` | ［詰］ |
| `sys/syscall.c:1891-1958`（穴の確保） | address space の穴の連結 list（first-fit）と `space_ops` | process の space の錠 | ［詰］ |
| `sys/log.c:372`（`scan`） | すべての CPU の ring | なし（割り込みの中） | ［詰］ |
| `dev/entropy.c:80-97` | すべての CPU の feed | `pool_lock`、割り込み止め | ［落］［詰］ |
| `fs/store.c:700-730` | 2 × CPU の cursor | 大域の spinlock | ［詰］ |
| `net/net.c` の batch | `tcp_capacity` の全部 | `net_lock` | ［詰］ |
| `arch/irq.c:303`（`place`） | CPU × vector（最悪 O(CPU²)） | 要求の時だけ | ［清］ |

- 穴の確保について：`lib/span.h`（区間の AVL 木）があり、使う所が無い。ブラウザーと JS の JIT・WASM は map と unmap を繰り返し、穴が増えるほど first-fit が遅くなる。
- `sys_proclist` について：task manager の窓が一定の間隔で呼ぶと、そのたびに全 thread を割り込みを止めて歩く。thread が数千（ブラウザーの子 process と Worker）になると、その CPU の合成が遅れる。

### 3.2 固定の上限

| 場所 | 上限 | 規模の例での壊れ方 | 重さ |
|---|---|---|---|
| `dev/blk.c` | `MAX_BLK 16` | NVMe の名前空間と区画が多い機械で足りない。 | ［清］ |
| `dev/nvme.c` | `MAX_CTRL 4`、`MAX_NS 8` | EPYC は NVMe を 10 台以上持つ。5 台目から使えない。 | ［詰］ |
| `dev/ahci.c` | `MAX_HBA 4` | 同じ形。 | ［清］ |
| `dev/xhci.c` | `MAX_HC 8` | EPYC の板は xHCI を 8 個より多く持ちうる。 | ［清］ |
| `dev/pciuser.c:41` | `CLAIM_VECTORS 32` | ABI の 32 bit の pending の語に縛られる。GPU の MSI-X の vector を 32 より多く使えない。 | ［詰］ |
| `dev/pci.c` の注釈 16-20 行 | segment 0 だけ（`ecam_active[65536/8]`） | 2 ソケットの EPYC は PCI の segment を複数持ちうる。segment 1 以上の装置が見えない。 | ［詰］ |
| `net/net.c:248` 他 | `RXQ 64`、`ARP_N 16`、`PING_N 8`、`UDP_Q 8` | 受信の嵐で 64 を超えると落とす。UDP の queue が 8 で、ゲームと音声の UDP の packet が落ちる。 | ［詰］ |
| `sys/channel.c` | `CHANNEL_CREDIT` 1 MiB 固定 | 動画の frame を channel で送ると credit で止まる。 | ［清］ |
| `sys/stream.c` | console の ring 1024 byte | 小さい。 | ［清］ |
| `sys/channel.c:235-243`（`message_new`） | 上限なしの `kmalloc(n)` | 空の queue への message は必ず受け入れる（181 行あたりの注釈）。n はアプリが決めるので、一つのアプリがカーネルの heap を食い尽くせる。 | ［詰］ |

- `message_new` は「小さな固定の上限」の逆の欠陥である。上限は固定値でなく、受け手の credit と送り手の process の記憶の勘定で決める。

### 3.3 CPU の数で増える大域の仕事

- 1.3 の `entropy_get`、1.5 の `alarm_clock_changed`、1.7 の `scan`、1.8 の perf の kthread、2.5 の cursor の表。
- 割り込みの置き場所は一度決めたら動かない（`arch/irq.c:556` の `irq_set_cpu` に呼び手が無い）。負荷の偏りを直す道が無い。

---

## 四　I/O

### 4.1 NVMe ［詰］

- `dev/nvme.c:209-223`：I/O の queue は制御器ごとに一組。
- `dev/nvme.c:76`・285-300：命令は mutex の下で一度に一つ。
- `dev/nvme.c:298`：`blk_by_blocks` の `direct = false`。128 KiB の bounce buffer へ memcpy。
- `dev/nvme.c:374`：vector は一つ、`.node = -1`。
- `dev/nvme.c:166`：`command()` の完了は眠って待つ。割り込みが来なければ 10 ms で起きて見る。poll の時は 1 ms。
- 規模の例：
  - NVMe は queue を数十から数千持てる。一つの queue で一つの命令だけでは、装置の並列が使えない。
  - ゲームの asset の読み込みの間、他の process の page fault の読みと file の読みが一つずつ並ぶ。
  - bounce の memcpy は 7 GB/s の装置で CPU を一本占める。
  - 割り込みの来ない時の 10 ms の起床は、締切の 6 倍。
- 案：CPU ごと（少なくとも LLC ごと）の提出と完了の queue の組を作り、MSI-X の vector を queue ごとに、その CPU の node に置く。命令は queue の深さまで並べる。DMA は利用者の buffer を IOMMU で直接 map する。完了は割り込みで起こし、固定の起床の間隔を消す。

### 4.2 AHCI ［清］

- `dev/ahci.c`：port ごとに命令一つ（slot 0）。NCQ が無い。bounce buffer。
- 案：32 slot と NCQ を使う。

### 4.3 xHCI ［落］

- event ring は interrupter 0 の一つ。
- すべての制御器と class driver が一本の "usb" thread と一本の大域の `usb_lock` を共有する。
- 1.10 の通り、列挙の待ちが入力を止める。
- 案：制御器ごとに interrupter を一つ以上持ち、HID の端点の完了は割り込み thread で処理する。

### 4.4 割り込みの散らし方 ［詰］

- `dev/pci.c` の `msix_enable` は vector ごとに `IRQ_CPU_ANY` で要求する。`place()` が使用の少ない CPU を選ぶので、vector は散る。
- 欠け：
  - すべての `irq_spec` が `.node = -1`。`pci_device` に node と segment の欄が無い。
  - 一度置いたら動かさない（`irq_set_cpu` に呼び手が無い）。
  - 合成と入力の CPU から装置の割り込みを外す手段が無い。
- 規模の例：2 ソケットで、socket 0 の NVMe の割り込みが socket 1 の CPU に置かれると、完了のたびに socket をまたぐ。合成の thread の居る CPU に NVMe の割り込みが置かれると、I/O の嵐の間その CPU の合成が遅れる。
- 案：`pci_device` に segment と node（ACPI の `_PXM`・SRAT）を持たせ、MSI-X の vector を装置の node の CPU に置く。合成・入力・音の CPU を装置の割り込みから外す設定を持つ。

### 4.5 IOMMU ［落］［詰］

- 1.9 の `sync` の spin。
- `dev/iommu_amd.c`：page table は 4 KiB の page だけ。大きな buffer（GPU の VRAM の窓、動画の frame）で IOTLB が溢れる。
- `event_isr` は大域の `amd_lock` の下ですべての unit を drain する。
- `dev/iommu_intel.c` の `domain_used` は 64K bit で、仕様の domain の数どおり。欠陥ではない。

### 4.6 網の I/O ［詰］

- `net/rtl8125.c`：queue 0 だけ。RSS を使わない。
- 2.7 の `net_lock` と一本の "net" thread。

### 4.7 ACPI ［清］

- `dev/acpi.c`：
  - uACPI の unmap は何もしない（direct map の上なので害は無い）。
  - 仕事の FIFO は一本の acpi worker。GPE の仕事を CPU 0 に縛れない（`kthread_create_on` はある）。uACPI の GPE の扱いは、決められた CPU で回す前提を持つ。

---

## 五　規則の違反ときれいさ

### 5.1 VM の判定が `vm/` の外にある ［清］

- 場所：`arch/cpu.c:187`・192・227。
- 起きること：`cpu_features.hypervisor` が立つと、必須の機能の検査と TSC の P0 の検査を免除する。
- 規則：VM の処理は `kernel/src/vm/vm.h` の後ろに置き、共通コードに hypervisor の判定を書かない。
- 案：免除を消す。VM で足りない機能は VM の側で補うか、VM を試験の道具から外す。

### 5.2 TSC の P0 は AMD だけ ［清］

- 場所：`arch/cpu.c` の `tsc_p0`。
- 起きること：AMD の MSR だけを読む。Intel の CPU は起動で panic する。
- 案：周波数は CPUID の leaf 0x15・0x16（Intel）と、AMD の文書の手順で別々に読む。

### 5.3 rtl8125 が他の OS の driver を参照している ［清］

- 場所：`net/rtl8125.c` の頭。
- 起きること：register の手順が Realtek の FreeBSD の driver（`rtl_bsd_drv_v1102.01`）に従うと書き、BSD の告知を持つ。
- 規則：AGENTS.md は他の OS の source の参照を禁じる。
- 案：Realtek の data sheet だけから書き直す。持ち主の判断が要る。

### 5.4 syscall.c が mm の仕事を持つ ［清］［詰］

- `sys/syscall.c`（3114 行）が、共有メモリー、pager、program の cache、DMA、穴の確保、user copy を持つ。これらは `mm/` の責務である。2.2 と 3.1 の詰まりの源。
- `wm_syscall` の範囲もここから振り分ける。

### 5.5 起動の旗が内部を組む ［清］

- `arch/smp.c:303`（`nosmp`）、`dev/pciuser.c`（`noappdrivers`、`fbhold`、`appdrivers=any`）、`dev/acpi.c`（`noaml`）、`init.c`（`nogui`、`noiommu`、`noresume`、`fbsim10`、`wm*`）。
- AGENTS.md は「システムの内部を引数や旗の起動で組まない」とする。`nogui` は起動の項目として残る。それ以外は試験の道具か、設定の持ち主へ移す。

### 5.6 送りの非 blocking が無い ［清］

- 場所：`sys/syscall.c:2539`（`sys_channel_send`）は常に `CLOCK_NEVER` を渡す。
- 起きること：credit が尽きると送り手は無期限に眠る。合成や音の thread が channel で送ると、受け手の遅れに引きずられる。
- 案：期限つきの送りと、送れる時の事象を ABI に持つ。

### 5.7 GPU の syscall が一枚ごとに確保と二回の copy ［落］

- 場所：`sys/syscall.c:2956`・2976・2993（`SYS_GPU_RENDER*`・`BLIT`）。
- 起きること：呼ぶたびに一枚分を `kmalloc` し、二回 copy する。
- これは合成の道の監査役の範囲。境界として書く。

### 5.8 その他

- `arch/isr.asm:310` に日本語の注釈がある。注釈の言語の約束が無いので、欠陥とはしない。
- `sys/prof.c` は CPU ごとに 1 ms の alarm を持つ。`prof` の旗の時だけ動く。
- `sys/keep.c`、`sys/proc.c`、`dev/fbcon.c` は熱い道に無い。
- `init.c` の起動の順：acpi → pm → vm → pci → iommu → usb → net → audio → ahci → nvme → store → wm。

---

## 六　中核に求めること（呼ぶ側から引いた一覧）

呼ぶ側の使い方から引いた。中核の書き手はこの名前と呼び方を満たすか、呼ぶ側を同時に直す。

### 6.1 wait

- 型：`struct wait_queue { lock; ... }`、`struct wait_item { callback; key; ... }`。
- `wq_wait_locked(wq, lock)`：呼び手は spinlock を握り、割り込みを止めて呼ぶ。戻りでは錠を放し、割り込みは止まったまま。
- `wq_wait_locked_until(wq, lock, deadline)`、`wq_wait_locked_exclusive_until(...)`。
- `wq_wait_unless(wq, word *, seen, deadline)` は bool を返す。
- `wq_wake(wq, n)`、`wq_wake_all(wq)`：どの文脈からでも呼ぶ。hard IRQ の中と spinlock を握った中を含む。callback は wq の錠の下で走る。
- `wq_add`・`wq_remove`：呼び手が `wq->lock` を握る。
- `sleep_until(deadline)`、`sleep_until_boot(deadline)`。
- `mutex_lock`・`mutex_unlock`・`mutex_held`。再帰の `depth` を持つ（`net/net.c:116` が `depth == 1` を assert）。`mutex_wait(m, wq)`。
- `rwmutex_lock_read`・`rwmutex_lock_write`・`rwmutex_unlock`。
- `process->space` は wait queue で、その錠が address space の錠を兼ねる。

### 6.2 thread

- `thread_current()`。
- 欄：`tid`、`name`、`process`、`killed`、`kill_code`、`start_ns`、`user_stack`、`user_stack_size`、`sched.state`、`wait.item.key`、`all_next`、`process_next`。
- `THREAD_ZOMBIE`、`thread_lock`、`thread_list`、`thread_count`。
- `thread_find`、`thread_exit`、`thread_create(entry, stack, arg, ...)`、`thread_join`、`thread_detach`、`thread_enter_user`。
- `kthread_create(name, fn, arg)`、`kthread_create_on(name, fn, arg, cpu)`。

### 6.3 process

- `process_current()`。
- 欄：`pid`、`name`、`parent`、`main`、`threads`、`vmm_space`、`handles`、`shared_mappings`、`space`、`space_ops`、`mmap_holes`、`low_holes`、`mmap_next`、`low_next`、`space_ended`、`exiting`、`app`。
- `process_exit`、`process_kill`、`process_kill_tree`、`process_stop_all`、`process_new`、`process_start(p, parent, app)`、`process_discard`、`process_find`（`thread_lock` の下）、`process_put`。

### 6.4 sched

- `sched_set_policy`、`sched_yield`、`sched_thread_cpu_ns`、`sched_init`、`sched_idle`、`sched_get_stats`。
- CPU の出入り：`sched_cpu_prepare`、`sched_cpu_stack`、`sched_cpu_start`、`sched_cpu_leave`、`sched_cpu_empty`、`sched_make_ready`。
- `arch/isr.asm` から：`sched_user_return(regs *)`、`sched_preempt_irq()`、`sched_post_switch`（`context_switch` の後）。
- **食い違い**：呼ぶ側は `sched_realtime(SCHED_PRIORITY_IRQ)`（`arch/irq.c:114`）、`SCHED_PRIORITY_INPUT`（`dev/xhci.c:1453`）、`SCHED_PRIORITY_COMPOSITOR`（`gui/wm.c`）、`SCHED_PRIORITY_SOUND`、`vm/rdp.c` を使う。新しい `sys/sched.h` には `sched_fair` と `sched_deadline` しか無く、`sched_realtime` と `SCHED_PRIORITY_*` が無い。中核の形を決めてから、呼ぶ側を同時に直す。

### 6.5 arch/isr.asm と context の切り替え

- percpu：`preempt_count`（`PREEMPT_IRQ_ONE = 1 << 16`、`PREEMPT_IRQ_MASK`）、byte の `sched_resched`、`irq_stack_top`、`syscall_user_rsp`、`cpu_tss`。
- `context_switch(uint64_t *old_rsp, uint64_t new_rsp)`。
- `kthread_trampoline`（r12 = fn、r13 = arg）から `kthread_start(fn, arg)`。
- `user_trampoline` から `sched_user_return`。

### 6.6 mm

- heap：`kmalloc`、`kzalloc`、`krealloc`、`kfree`、`kappend(&arr, &n, &room, size)`、`heap_used_bytes`、`heap_cpu_gone`。
- 物理：`pmm_alloc`、`pmm_alloc_zeroed`、`pmm_alloc_contig(pages, addrbits)`（addrbits は 32 か 64）、`pmm_free`、`pmm_free_contig`、`pmm_free_pages`、`pmm_total_pages`、`pmm_ram_ranges`、`pmm_page_count`、`pmm_page_used`、`pmm_map_signature`、`pmm_low_range`、`pmm_claim`。
- 仮想：`vmm_map_mmio`、`vmm_map_phys`、`vmm_map_page`、`vmm_get_pte`、`vmm_translate`、`vmm_demand_fault`、`vmm_handle_user_fault`、`vmm_fault_spurious`、`vmm_user_alloc`・`free`・`reserve`・`protect`・`write_protect`・`pages`・`pages_split`、`vmm_copy_to_space`、`vmm_space_get`・`put`、`vmm_kernel_translate`、`vmm_direct_type`、`vmm_init_cpu`、`vmm_online`、`vmm_leave`、`vmm_tlb_poll`。
- `vmalloc`・`vfree`、`vmalloc_map_frames`、`vmalloc_purge_start`。
- `bootmem_alloc`、`bootmem_alloc_node`。
- 定数：`PTE_P`・`W`・`U`・`NX`・`SHARED`・`SHARED_RO`・`PWT`・`PCD`・`PAT4K`・`ADDR`、`VM_W`・`X`、`USER_TOP`・`STACK_TOP`・`STACK_MAX`・`MMAP_BASE`・`LOW_TOP`。
- `arch/idt.c` の page fault は `vmm_handle_user_fault` と `vmm_fault_spurious` へ行く。例外の道は `thread->tid`・`name`・`process`、`process->main`・`vmm_space`、`process_exit`、`stream_error_printf` を使う。

### 6.7 epoch・percpu・preempt・cpuset・address_wait

- epoch：`epoch_enter`、`epoch_leave`、`epoch_defer(struct epoch_item *, fn)`。
- percpu：`PERCPU`、`this_cpu`、`this_cpu_ptr`、`per_cpu`、`percpu_load`、`percpu_init`、`cpu_index`。percpu の変数の定義（`preempt_count`、`spin_nodes` など）は `arch/smp.c` にある。
- preempt：`preempt_disable`、`preempt_enable`、`in_nmi`、`PREEMPT_NMI_ONE`。
- cpuset：`each`、`add`、`remove`、`has`、`words`、`next`。
- address_wait：`address_wait`、`address_wake`、`address_word`、`address_watch_arm`、`address_watch_disarm`。

### 6.8 残りのカーネルが中核に求める新しい力

上の欠陥を直すために、中核が持つべきもの。

1. **ns の絶対期限の待ちと timer。** 時計の種類と許す幅（[early, at]）つき。ABI まで通す（1.11）。
2. **割り込み thread・入力・合成・音の時間の class。** 締切か実時間の優先度で、普通の thread に追い越されない（6.4 の食い違いを含む）。
3. **CPU への固定と、装置の割り込みを避ける CPU の指定。** 合成・入力・音の thread をその CPU に置き、装置の割り込みを他へ置く（4.4）。
4. **NUMA の node に沿った DMA の確保。** `pmm_alloc_contig` に node を渡す口（4.1、4.4）。
5. **非同期の I/O の完了。** block の層と NVMe の queue の完了を、thread を眠らせずに事象として返す（2.6、4.1）。
6. **address space の区間の木。** `lib/span` を使い、穴の確保と共有の map の検索を O(log N) にする（2.2、3.1）。
7. **mm の側の共有 object と pager。** syscall.c から移し、object ごとの錠にする（2.2、5.4）。
