# 波 3 の設計：mm・syscall の分割・時間の ABI（2026-10-31）

main `ca7a80c3`（段 2b-S・2b-W を取り込んだ後）のコードと、監査 04（土台：`docs/kernel-audit-20261030/04-foundation.md` の [19]〜[24]）・03（横断）・05（時間と実行）で書いた。00-design.md §5 の mm と時間の ABI の要件を、段に割る。

## 1. 段と依存

| 段 | 中身 | 持つファイル | 待つもの |
|---|---|---|---|
| 3-P | 頁の確保（pmm・bootmem）、記憶の圧の口、vmalloc、heap の大きな物 | `mm/pmm.*`・`mm/bootmem.*`・`mm/vmalloc.*`・`mm/heap.*` | なし（2b-O と重ならない） |
| 3-V | アプリの空間（空間の記述・頁表・頁違反・大きな頁・撃ち落とし）、syscall.c の配置・共有記憶・pager・program の cache・空間の予約を mm へ | `mm/vmm.*`・新しい `mm/space.*` など・`sys/syscall.c` の該当の部分 | 2b-O の取り込み（syscall.c）、3-P |
| 3-S | syscall の表と持ち主ごとの手続き、64 bit の番号と錠の無い登録、起動の塊、`n_cpuinfo` の掃除 | `sys/syscall.c`・新しい `sys/syscalls.h`・`sys/usercopy.c`・各持ち主の手続き・`common/abi.h`・`crt0.asm` | 3-V（syscall.c） |
| 3-T | 時間の ABI（ns・絶対期限・時計の頁をアプリへ・事象の時刻）と libc・移植 | `common/abi.h` の時間の部分・時計の頁の見出し・`user/libc/` の時間の部分・使い手 | 3-S |

3-P は今走らせる。3-V は 2b-O の二段目と 3-P を取り込んでから。3-S・3-T は §4 のとおり 3-V の後に順に。

## 2. 3-P の契約

### 2.1 頁の確保（pmm）
監査 [20] の判定は「消して建て直す」。良い点（1 GiB の節ごとに node の上の 32 byte の記録、帯で DMA の幅を選ぶ、`pmm_mark` の印）は形として残してよい。

- **CPU ごとの溜めは多 order。** 今は 1 頁だけで、多頁と 2 MiB は毎回 node の錠を取る。溜める order は、多く確保される大きさ（1 頁、核の stack の大きさ、2 MiB：3-V の大きな頁と直写像）から決め、注記に理由を書く。溜めの長さは node の記憶と CPU の数から決める（決め打ちの数ではなく式。heap.c の鎖の長さの決め方と同じ考え）。
- **錠の区間を短く。** buddy の錠は node（と帯）ごと。溜めの補充と吐き出しは一度に束で行い、錠の下では束を繋ぎ替えるだけ。
- **遠い解放はまとめる。** 他の node の frame の解放は、CPU ごとの node ごとの束に貯め、束ごとに一度だけ相手の錠を取る（今は頁ごと）。
- **足りないときに CPU の数に比例する走査をしない。** 今の steal と drain_all（全 CPU を回る）を消す。他の CPU の溜めを空にさせたいなら、その CPU に知らせて自分で返させる形（IPI か、CPU ごとの印を CPU が自分の点で見る）で、送り手は全 CPU を回らない。
- **記憶の圧の口。** 回収できる物を持つ部品（vmalloc の退いた番地と frame、heap の depot、核の stack の溜め、後で格納庫の塊の溜め `fs/cache.c`）が登録する口と、node ごとの水位（min・low・high、node の記憶から式で決める）。空きが low を下回ったら、その node の回収の thread（node の CPU に縛った核の thread、待ちで起こされる）が登録された部品に返させて high まで戻す。眠れる確保の呼び手のための直接の回収の口（`pmm` 自身は眠らない契約のまま、眠れる呼び手が呼ぶ別の口）も置く。登録する部品は 3-P の中で全部つなぐ（名前だけの口にしない）：vmalloc・heap・`sys/thread.c` の核の stack の溜め。`fs/cache.c` はこの段では触れず、報告に「次につなぐ所」として書く。
- **記録の初期化は node ごとに並列。** 今は CPU 0 一つで RAM の量に比例する。起動に要る分（各 node の最初の部分）だけ CPU 0 が作り、残りは CPU が起きた後に各 node の CPU が自分の node の節を作る。作り終わっていない節の frame は buddy に入っていない。確保が足りないときに作り終わりを待つ道（眠れる呼び手）と、待てない呼び手が 0 を受ける道を決めて書く。
- **ブートローダーの解放可能領域を返す。** limine の応答（memmap・cmdline・module など）を核が自分の記憶へ写し終えた後に、BOOTLOADER_RECLAIMABLE の範囲を空きへ。写すのは誰か（boot.c・init.c の持ち主）を読んで、最小の行で。
- **範囲を足す口は一つ。** 起動の空き（bootmem の handoff）も、後の hot-add（ACPI の装置の通知は波 4）も同じ口で節を作って空きに入れる。起動の道がこの口の呼び手なので、名前だけの口にならない。SRAT の hot-pluggable の印と記憶だけの node（CXL の記憶はこう見える）は topology が持つ（`arch/topology.c` の事実を読む）。node の階層（距離）で、home に無いときに次に近い node から取る今の順は残す。
- **数は累積と生存を分ける。** 空き・使用中・溜めの中・作り終わっていない、を別々に数え、`pmm_free_pages` などの呼び手（約 18 か所）が何を読みたいかを見て口を決める。

### 2.2 起動の記憶（bootmem）
監査 [19] の判定は「残せる」。事実の傷を直す：指定の node に入らないとき、最も高い番地ではなく距離の近い順の node から取る（`bootmem.c` の `bootmem_alloc_node` の落ち先）。CPU ごとの写しは今は home を渡している（`arch/smp.c` の `percpu_init`、監査の時の傷は直っている）。

### 2.3 vmalloc
監査 [22] の判定は「消して建て直す」。
- 全体で一つの錠と、頁ごとに核の半分の錠を取る形を消す。小さな確保（核の stack と、それ以下の大きさ）は CPU ごとの塊（その CPU の home の node の番地の池から取った大きな範囲を、CPU が錠なしで切る）から。大きな確保は span の木から（錠は短く）。頁表は一度の確保で一度に作る（頁ごとに錠を取らない）。そのために vmm に範囲の口が要るなら、`mm/vmm.c` に最小の口を足してよい（3-V が後で建て直す。足した口を報告に書く）。
- 掃除は量の閾だけでなく、番地か記憶が足りない時（確保の失敗の時に一度掃除してからやり直す）と、記憶の圧の口からも。撃ち落としは掃除の束ごとに一度。
- `vunmap` が毎回全 CPU を撃つ形をやめる。frame を持ち主に返す前に訳が消えていることは守る（契約は今の vmalloc.h の通り）。
- 核の stack の溜め（`sys/thread.c` の `kstack_stash`、CPU ごとに 2 つ）は、vmalloc の CPU ごとの塊で足りるなら消し、足りないなら残して記憶の圧の口に登録する。どちらにしたかを書く。

### 2.4 heap の大きな物
監査 [23b] の判定は「消して建て直す」：heap.h は「NMI と #MC 以外のどこでも」と約束するのに、連続の頁が無いと vmalloc へ逃げ、vmalloc は撃ち落としを待つ（割り込みの中と spinlock の下で禁じられている）。
- `kmalloc` は vmalloc へ逃げない。大きな物は home の続いた frame から、無ければ NULL。契約は「どこでも、眠らない、他の CPU を待たない」に揃える。
- 連続でなくてよい大きな記憶が要る呼び手は、自分で vmalloc を呼ぶ（その呼び手が眠れる文脈か読んで決める）。今 `kmalloc` で大きな物を取っている呼び手を探し（`rg` と codegraph）、連続が要らず眠れるものは vmalloc へ、連続が要るものはそのまま、どちらでもないものは報告に書く。
- 小さな物（slab・CPU ごとの鎖・depot）は「残せる」。傷の二つ（二重解放の検査が先頭だけ、`CACHE_SHARE` を pmm と別々に定義）は直す。depot は記憶の圧の口に登録する。

### 2.5 触ってよい所
`mm/` の四つの部品（pmm・bootmem・vmalloc・heap）は持ち物。`mm/vmm.c` は vmalloc の範囲の口のための最小の行だけ。`sys/thread.c` の核の stack の溜め、limine の応答を写し終える所（boot.c か init.c）、大きな `kmalloc` の呼び手の書き換え。それ以外は触らずに報告。

### 2.6 2b-O との境
2b-O の二段目が同時に `sys/object.c`・`events.c`・`channel.c`・`watch.c`・`net/net.c`・`sys/sched.c`（静かな点）・`sys/wait.c`・`sys/syscall.c` を書いている。3-P はこれらに触れない。大きな `kmalloc` の呼び手がこの中にあれば、書き換えずに報告に書く（本体が取り込みの時に直す）。

## 3. 3-V の契約（2b-O と 3-P を取り込んでから走らせる）

監査 [21] の判定は「消して建て直す」。今の形（main `c3931aa2` で読んだ）：アプリの空間の記述が二つの持ち主に割れている。頁表と予約の印（葉の `PTE_DEMAND`、範囲の全部の頁表を先に作る）は `mm/vmm.c`、配置の一覧（`struct shared_mapping` の線形の鎖、`mmap_hole` の穴の鎖、`space_op` の作業中の範囲、`mmap_next`・`low_next` の前線）は `sys/syscall.c` と `struct process` の欄。共有記憶の物（`struct shared_pages`：RAM・demand・range（BAR・連続の DMA）・view・composite・paged）、pager、program の cache も syscall.c の 3110 行の中。頁違反は `arch/idt.c` から割り込みを閉じたまま `vmm_handle_user_fault` に入り、空間ごとの一つの spinlock の下で 0 埋めをする。主 stack は 8 MiB の決め打ち（`USER_STACK_MAX`）、ASLR は無い。アプリは非 PIE（`-fno-pie`）。

### 3.1 空間の記述は mm に一つ
- 空間（今の `struct vmm_space` を土台に、持ち主は mm）が範囲の木を持つ：各範囲は始めと終わり、権利、種類（無名の予約・確定、program の像、stack、共有記憶の物の写し）、写す物と offset、`n_memory_protect` の頁ごとの権利の表。空きの範囲を探すのは `lib/span` の最長の空きの要約で O(log n)。
- `struct process` の配置の欄（`space`・`space_ops`・`space_ended`・`mmap_next`・`low_next`・`mmap_holes`・`low_holes`・`shared_mappings`）と syscall.c の `mmap_hole`・`space_op`・`shared_mapping` の鎖を消し、範囲の木へ。
- 錠：範囲の木は読み書きの mutex（段 2b-W の `rwmutex`）。頁違反は読む側で入り、眠れる（frame の確保、pager の待ち、記憶の圧の直接の回収）。写す・外す・権利を変えるのは書く側。頁表は頁表の頁ごとの錠（頁表の frame の記録に置くか、空間の中の頁表の階層ごとか、3-P の frame の記録の形を見て決める）で、二つの頁違反が同じ空間の別の所で並ぶ。一つの大きな spinlock を残さない。
- 頁表は触れた所だけ作り、外して空になった頁表の頁は撃ち落としの後に返す（gather に頁表の frame も入れる）。予約は範囲の木の印で、葉を先に作らない。

### 3.2 頁違反
- `arch/idt.c` は利用者の頁違反（ring 3 と `user_copy` の中）で割り込みを開いてから mm を呼ぶ（触ってよい最小の行）。核の番地の違反は今と同じく閉じたまま。
- 範囲の種類で解く：無名（0 の frame、範囲と揃いが許せば 2 MiB の大きな頁）、物の写し（共有記憶の物が frame を返す：RAM・demand・range・view・composite・paged・program の segment）、stack（下へ伸びる範囲と守りの頁）。pager の待ちは範囲の木の錠を放して眠り、起きたら引き直す。

### 3.3 共有記憶の物・pager・program を mm へ
- `struct shared_pages` の種類と寿命（scope の取り消し、DMA の型と IOMMU の domain、device の pin）、pager、program の cache を mm の部品（例 `mm/memory.c`、名は持ち主の責務から）へ移し、syscall.c には ABI の手続き（引数の写しと mm の口を呼ぶこと）だけを残す。`shared_lock` の一つの大きな錠は、物ごとの錠と範囲の木の錠に割れるか読んで決め、割れないものは理由を書く。
- 監査 01（fs）・06（dev）が見る `pciuser`・IOMMU との継ぎ目は口だけを保ち、中身は波 4。

### 3.4 TLB
- 撃ち落としは、その空間を載せたことのある CPU の集合（空間ごとの `cpuset`、`vmm_switch` で印を足し、全部を消した時に外す）へ一度に。宛先ごとに一つの IPI を送る今の形をやめ、集合へまとめる。
- Zen 4 の事実：PCID は Fixed,0（PPR 56713）。INVLPGB・TLBSYNC の有無と、PCID を使わない時（CR4.PCIDE=0、全空間が PCID 0）の INVLPGB の意味を APM vol3 と PPR 56713 で照らし、使えるなら IPI の代わりに使う。今の CPU ごとの PCID の席の仕組みは、的の CPU で走らない道になる：残すなら、PCID を持つ CPU の上で INVLPGB と両立する形と、その CPU の資料を示す。示せないなら消す。
- 直写像は 1 GiB の頁を使う（今は 2 MiB まで）。

### 3.5 配置
- 主 stack の大きさは決め打ちをやめる（program の像か起動の引数が決め、範囲の木の stack の範囲として伸びる上限を持つ）。今の ABI の振舞いを変えるなら、`docs/nocturne-native-abi.md` を同じコミットで直す。
- ASLR：無名の写しの領域の基と stack の位置を、核の乱数（`random` の持ち主）で毎回変える。像の位置は非 PIE なので変えられない：PIE にするかは 3-T（ABI と道具）で決める、と報告に書く。

### 3.6 持つファイルと触ってよい所
`mm/vmm.*`、mm の新しい部品、`sys/syscall.c` の共有記憶・pager・program・配置・頁違反の部分（ABI の手続きは残す）、`sys/process.*` の配置の欄、`arch/idt.c` の頁違反の入口、`sys/proc.c` の像の写し（範囲の木を使う所）。3-P の口（記憶の圧、多 order の溜め）を使う。

## 4. 3-S と 3-T（3-V の後に、この順で）

00-design.md §5「時間の ABI」と盤の「ABI の段」を二つの段に割る。どちらも `common/abi.h` と syscall の手続きを書くので並べない。3-S は形（syscall の表、番号、起動の塊）、3-T は時間。socket を handle にするのは網の建て直し（波 4、監査 02）と一緒にする：網の作り直しの前に今の `net.c` の TCP の番号の空間を handle へ移すと、同じ所を二度書く。time-design.md の T2 はそれを待つ。

### 4.1 今の形（main `c7ca2661` で読んだ）

- 期限は相対の ms で、15 個ほどの syscall に散る：`SYS_SLEEP`（ms）、`SYS_WIN_RECEIVE`・`SYS_EVENTS_WAIT`・`SYS_APP_RESULT`・`SYS_PAGER_TAKE`・`SYS_PCI_INTERRUPT_TAKE`・`SYS_PCI_ACPI_NOTIFY_TAKE`・`SYS_PCI_SCREEN_STATE`（int64 の ms、負は無限、0 は待たない）、`SYS_WAIT_ADDRESS`・`SYS_WAIT_ADDRESS64`（unsigned の ms、`UINT32_MAX` が無限）、`SYS_UDP_RECV`・`SYS_TCP_RECV`・`SYS_TCP_ACCEPT`・`SYS_TCP_CONNECT`・`SYS_NET_PING`（int の ms）、`SYS_TCP_SEND_UNTIL`（絶対の `uptime_ms`）、`SYS_EVENTS_TIMER`（相対の ms と繰り返しの旗）。核の側は `sys/syscall.h` の `syscall_deadline`・`address_deadline` で入口ごとに換える。
- 時刻を読む口：`SYS_UPTIME`（ms）、`SYS_TIME`（秒）、`SYS_CLOCK_INFO`（ms の境とそこの TSC と速さを一度だけ渡し、libc の `user_clock_us` はそこから自分で数え続ける。速さの直しも CPU ごとの TSC の差も見えない）。libc には `uptime_ms`・`uptime_us`・`n_clock_uptime_ms`（同じ時計が三つの名）と `n_clock_wall`（秒）。`ports/nvidia/os.c`・Go の runtime も `SYS_CLOCK_INFO` の標本から自分の時計を作る。核の時計の頁（`struct clock_data`、`clock_data_pages`）はあるが、アプリに写していない。
- 時刻を持たない記録：`struct gui_event`（入力も `EV_FRAME` も）、`struct n_event`、`struct n_audio_status`（位置はあるが、その位置がいつの物か無い）。ms の欄：`n_procinfo` の `cpu_ms`・`start_ms`、`n_sysinfo` と `n_clockinfo` の `uptime_ms`。ns なのは `SYS_PCI_SCREEN_VBLANK` の `timestamp_ns`（ただし「driver の時計」）と `n_entry` の `modified_ns`。
- 番号：thread の番号は `int`（`lib/idtable`、2^31 を巡ると使い回す）、pid は主 thread の tid。登録は大域の `thread_lock`。
- syscall の振り分け：`syscall_dispatch` の 3110 行の `switch`、wm と網は番号の範囲で `wm_syscall`・`net_syscall` へ。手続きの多くが syscall.c にある（63 個の `sys_*`）。
- 入口：`crt0.asm` は rdi に argc、rsi に argv、rdx に起動の設定の列を受ける。ほかに渡す物が増えるたびにレジスターが要る。
- `n_cpuinfo` の `worker_cpus`・`parallel_jobs`・`worker_chunks` と `N_CPU_AP_WORKERS` は常に 0、`N_CPU_SSE2`・`N_CPU_AVX`・`N_CPU_USER_THREADS` は下限の CPU（x86-64-v4）で常に立つ。

### 4.2 3-S：syscall の表・番号・起動の塊

- **表。**`sys/syscall.c` に残すのは入口（`syscall_dispatch`：kill の確かめ、割り込みを開く、表を引く、呼ぶ）と、番号から手続きへの一つの表（`[SYS_X] = sys_x` の指定の初期化子の const の配列。穴は NULL で `N_ERR_UNSUPPORTED`）だけ。手続きの型は一つ（六つの 64 bit の引数、r9 まで読む）。手続きは持ち主のファイルに置く：process・app（`sys/process.c`・`proc.c`）、thread、wait・address_wait、events、channel、object、stream・log、fs（document・entry・folder）、wm、net、pciuser・dma、音、random、mm（3-V が移した共有記憶・pager・配置）、時計、電源。番号の範囲で別の振り分けへ渡す `wm_syscall`・`net_syscall` は消す。手続きの宣言は一つの見出し（`sys/syscalls.h`、核の中の ABI の目録）に並べる。利用者の記憶の写し（`copy_from_user` など）は `sys/usercopy.c` へ。
- **番号。**変わらない呼出しは今の番号のまま。消した呼出しの番号は空けたまま、新しい呼出しは新しい番号（abi.h の今の決まり）。Go と Rust の移植の差分が番号を写しているので、写しは `common/abi.h` から生成する（ホストの道具で Go・Rust の定数のファイルを作る。生成物は生成元から）。今ある `SYS_THREAD_TLS` のような消えた番号を移植が持っていれば、ここで直す。
- **64 bit の番号。**thread と app の番号は 64 bit で、1 から数え上げ、使い回さない（2^64 は巡らない）。番号から物を引く登録は 2b-O の epoch で読む錠の無い表（2c の残り「`thread_lock` を錠の無い登録へ」をここで閉じる）。`thread_lock` の残りの役（process の thread の一覧、親子のつながり）は process ごとの錠へ。ps の一覧は登録を epoch の下で歩く。ABI の `SYS_THREAD_ID`・`SYS_APP_ID`・`n_procinfo` の `pid`・`ppid` を 64 bit に。
- **起動の塊。**入口は rdi に一つの指 `const struct n_start *`：版と大きさ、argc・argv、起動の設定の列、時計の頁の番地（3-T）、主 stack の範囲（3-V が決め打ちを消した後の値）。新しい欄は末尾に足す。`crt0.asm`・`__libc_start`・`proc.c` の積み方と、Go・Rust の入口を直す。
- **`n_cpuinfo`。**常に 0 の欄と常に立つ旗を欄ごと消す（`worker_cpus`・`parallel_jobs`・`worker_chunks`・`N_CPU_AP_WORKERS`・`N_CPU_SSE2`・`N_CPU_AVX`・`N_CPU_USER_THREADS`）。使い手：`tests/cpuinfotest.c`・`tests/mlfqtest.c`・`user/apps/sysmon.c`・`tests/check_navigator_native.py`・`ports/rust` の差分・`docs/nocturne-native-abi.md` §9。

### 4.3 3-T：時計の id と時計の頁

- **時計は三つ**：`N_CLOCK_MONOTONIC`（起きている間だけ進む、起動で 0）、`N_CLOCK_BOOT`（眠りも数える）、`N_CLOCK_WALL`（UTC の Unix 時刻の ns、閏秒を数えない。time-design.md §3.1）。核の三つの時計（`clock_now_ns`・`clock_boot_ns`・`clock_wall_ns`）と同じ物。
- **時計の頁をアプリへ。**`struct clock_data` の形を ABI にする：定義は `common/` の一つの見出しへ移し（版の欄を先頭に）、核と libc が同じ定義を組む。核は起動の時に各アプリの空間へ読むだけで写し（範囲の木の一つの範囲、位置は 3-V の ASLR）、番地は起動の塊で渡す。書くのは核だけ、seq の決まりは今の注釈のまま。CPU の番号は RDTSCP の TSC_AUX（`offset[]` の添字）。
- **libc は頁を読む一つの関数** `n_clock_now(clock)`（ns）だけを持つ。`uptime_ms`・`uptime_us`・`n_clock_uptime_ms`・`n_clock_wall`・`clock_info`・`user_clock_us` を消し、使い手（user・tests・ports で約 980 か所、機械的な書き換え）を `n_clock_now` の ns に直す。ISO C の `timespec_get`（`TIME_UTC` は壁、C23 の `TIME_MONOTONIC` は単調）・`time`・`thrd_sleep` もこの関数で。
- **消す syscall**：`SYS_UPTIME`・`SYS_TIME`・`SYS_CLOCK_INFO`（と `struct n_clockinfo`・`N_CLOCK_USER_TSC`）。`n_sysinfo` の `uptime_ms` は欄ごと消す。
- **移植**：Go の runtime（`nanotime1`・`walltime`）、Rust の std（`Instant`・`SystemTime`）、`ports/nvidia/os.c`（二つ目の時計を消す）が頁を読む。読み方を三つ書かないよう、移植の言語で書く物は libc の関数と同じ式を一つの注釈に結ぶ（Go・Rust は libc を link しないので、式の写しは避けられない。式の持ち主は `common/` の見出しの注釈）。

### 4.4 3-T：期限

- **待つ呼出しの期限はどれも絶対の `N_CLOCK_MONOTONIC` の ns**（`uint64_t deadline`）。0 は待たない（見るだけ）、`N_NEVER`（`UINT64_MAX`）は限りなし。期限が来たら `N_ERR_TIMEOUT`。入口で換えない：核の待ち（`sys/wait.h` の期限は単調の ns）へそのまま渡し、`syscall_deadline`・`address_deadline` を消す。相対の時間を足すのは呼ぶ側（libc で一度）で、割り込まれて呼び直しても期限は動かない。
- 対象：`SYS_WIN_RECEIVE`・`SYS_EVENTS_WAIT`・`SYS_APP_RESULT`・`SYS_PAGER_TAKE`・`SYS_PCI_INTERRUPT_TAKE`・`SYS_PCI_ACPI_NOTIFY_TAKE`・`SYS_PCI_SCREEN_STATE`・`SYS_WAIT_ADDRESS`・`SYS_WAIT_ADDRESS64`、網の `SYS_UDP_RECV`・`SYS_TCP_RECV`・`SYS_TCP_ACCEPT`・`SYS_TCP_CONNECT`・`SYS_NET_PING`（往復の時間も ns で返す）。`SYS_TCP_SEND_UNTIL` は期限が ns になった `SYS_TCP_SEND` 一つにまとめる。網の呼出しは波 4 で作り直すが、期限の形はここで揃える（網の作り直しが同じ形を受け継ぐ）。
- **時計を選ぶのは眠りと timer だけ**：`SYS_SLEEP_UNTIL`（clock, deadline）と `SYS_EVENTS_TIMER`（queue, clock, 最初の期限, 周期の ns（0 は一度）, token）。起きている時計や壁の時計で待ちたい呼出しは、事象の行列に timer を置いて行列で待つ。待つ呼出しの全部に時計の id を足さないのは、引数のレジスターが足りない呼出し（channel の受け取りは五つ使う）があり、時計の選びを一つの物（timer）に置けば呼ぶ側の形が一つで済むから。
- **核の側の時計つきの期限**：今の `sleep_until_boot` の起きている時計の眠りは大域の `boot_lock` の一本の鎖（`boot_sleeps`）。ここに壁の時計の期限（壁の時計が跳んだ時・`clock_adjust_wall` の時に引き直す）が加わるので、鎖を CPU ごと（その期限の alarm の CPU）に割り、時計が動いた時は各 CPU が自分の鎖を引き直す。眠りの前後と壁の時計の跳びはまれなので、引き直しは全部を歩いてよいが、一つの大域の錠を待ちの入りと出に置かない。

### 4.5 3-T：事象の時刻

- `struct gui_event` に `uint64_t time`（単調の ns）：入力は装置が報告した時（xHCI の割り込みの時を HID の報告に付け、wm を通して窓の行列まで運ぶ。VM の合成装置は vm/ の受け取りの時）。`EV_FRAME` は表示の時（vblank の時刻）と表示の番号（vblank の数、新しい欄）。ブラウザーの `Event.timeStamp` と `requestAnimationFrame` の時刻はこの値から。
- `struct n_event` の `reserved` を使い、`uint64_t time`（物が用意できた時。timer はその期限）と timer の回数（前に受け取ってから期限が来た数）を持たせる。
- `struct n_audio_status` に `uint64_t played_time`：`played_frames` がその位置だった単調の ns（HDA は位置の読みとその時、推定の装置は推定の時。旗 `N_AUDIO_CLOCK_*` が今のまま源を言う）。Web Audio の出力の時刻と、動画の音と絵の合わせはこの組から。
- `SYS_PCI_SCREEN_VBLANK` の `timestamp_ns` は `N_CLOCK_MONOTONIC`（driver は時計の頁を読む）。
- `n_procinfo` の `cpu_ms`・`start_ms` を `cpu_ns`・`start`（起きている時計の ns）に。`n_entry` の `modified`（壁の秒）と `modified_ns` は形を変えない：精度は ns で、格納庫の書式の範囲（int64 の ns の 1678〜2262 年より広い）をそのまま渡すため。

### 4.6 Linux・NT と軸ごとに（公開された設計だけ）

| 軸 | Linux | NT | 我々（3-S・3-T の後） |
|---|---|---|---|
| syscall なしで時刻を読む | vDSO と vvar の頁、TSC が揃わない機械では syscall へ落ちる | `KUSER_SHARED_DATA` と QPC（TSC） | 時計の頁、CPU ごとの TSC の差も頁に持ち、落ちる道が無い |
| 期限 | 相対の ms（poll・epoll）と絶対の timespec（`clock_nanosleep`、futex の時計の旗）が混ざる | 負は相対、正は絶対の壁の時刻 | 待つ呼出しは全部絶対の単調 ns、時計の選びは眠りと timer に一つ |
| 事象の時刻 | evdev は timeval、DRM の vblank と ALSA は単調の時刻 | 入力の時刻は ms、DXGI の表示の統計は QPC | 入力・表示・音・事象の全部に単調の ns |
| thread・process の番号 | 32 bit、使い回す | 32 bit、使い回す | 64 bit、使い回さない（使い回しの取り違えが無い） |
| syscall の表 | 番号で引く表、手続きは各部品 | SSDT、手続きは各部品 | 番号で引く const の表、手続きは各持ち主 |

劣る軸は無い。費用：番号の 8 byte、事象の記録が大きくなる（`gui_event` は 32 → 48 byte ほど）。

### 4.7 確かめ

q35 の QEMU（窓付き一台）：デスクトップ、Terminal の `uptime`・`ps`（64 bit の番号）、ブラウザーの `performance.now()` と `requestAnimationFrame` の時刻が進む、動画が時間どおりに進み音が出る、事象の行列の timer（起きている時計と壁の時計）、`tests/runtests.c` の時間と thread の群。移植（Go・Rust）は道具があれば組んで動かす。眠りをまたぐ期限は WHPX の reset の欠陥で起こしが確かめられないので未確認と書く。
