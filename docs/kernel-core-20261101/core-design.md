# カーネルの中核の設計

この文書はカーネルの中核の今の形を書く。中核は次の部品からなる。

- CPU ごとの変数と横取り：`arch/percpu.h`・`arch/preempt.h`・`arch/cpuset.h`
- メモリー：`mm/bootmem`・`mm/pmm`・`mm/vmm`・`mm/vmalloc`・`mm/heap`
- 実行：`sys/thread`・`sys/process`・`sys/sched`
- 待ち：`sys/wait`・`sys/address_wait`・`sys/epoch`

要件は四つ。

1. GUI・GPU・合成が第一級。カーネルが原因で落ちるコマが無い。周期の違う画面が何枚あっても成り立つ。
2. CPU の数・メモリーの量を小さな固定の上限で縛らない。下限は Zen 4、上は 2 ソケットの EPYC（論理 CPU 1024、NUMA）。
3. shim・stub・決め打ちの周期と予算・一つの呼ぶ側のための包みを置かない。
4. ネイティブ ABI を迂回しない。VM だけの処理は `vm/vm.h` の後ろに置く。

## 1. CPU ごとの変数と横取り

### percpu（`arch/percpu.h`、実装は `arch/smp.c`）

- 責務：CPU ごとの状態の置き場所。
- 形：`PERCPU` の変数は `.percpu` 節（`kernel/linker.ld`）に置く。節は雛形である。`percpu_init` が CPU ごとに写しを作る。写しは、その CPU の home node のメモリーに置く。
- 各 CPU の GS base は、写しと雛形の距離である。`this_cpu(v)` は GS 相対の一命令になる。
- 各 CPU の TSC_AUX は CPU の番号である。NMI・#DB・#MC・#DF の入口（`arch/isr.asm`）は GS を信じない。入口は RDPID で番号を読み、`percpu_offsets` から距離を引く。
- 上限：CPU の数で配列を切らない。`percpu_offsets` は `topology.cpus` の長さで boot memory から取る。
- 10 倍のとき：写しは CPU ごとに一つなので、増えるのは CPU の数に比例するメモリーだけである。共有の行は無い。

### preempt（`arch/preempt.h`）

- 責務：今の CPU が横取りできるかどうかを数える。
- 形：CPU ごとの一つの数 `preempt_count` に三つの欄がある。`preempt_disable` と spinlock の数、割り込みの入れ子、NMI と #MC の入れ子である。
- 切り替えの点は四つ。`preempt_enable` で数が 0 に戻るとき、`irq_restore` で割り込みが開くとき、割り込みから核への出口（`sched_preempt_irq`）、ring 3 へ戻る道（`sched_user_return`）。
- 核の長い処理は横取りに任せる。自分で `schedule()` を呼んで譲らない。

### cpuset（`arch/cpuset.h`）

- 責務：CPU の集合。64 bit の語の列で、語の数は `topology.cpus` から決まる。
- 各語は原子的に変わる。CPU は自分を同時に足し引きできる。

## 2. メモリー

### 錠の順

先に取るもの → 後に取るもの：

```
アプリの配置の lock（process の space.lock、sys/syscall.c）
  → shared_lock（sys/syscall.c）
  → 頁表の lock（空間の vmm_space.lock、または kernel_space.lock。二つを同時に持たない）
  → CPU の頁の溜めの lock（pmm の cache）
  → node の頁の lock（pmm の node）
heap の depot の lock：葉。入れ子にせず、その下で頁を確保しない。
vmalloc の lock：葉。その下で他の lock を取らず、確保もしない。
```

TLB の撃ち落としは spinlock の下でも割り込みの処理の中でもしない。`shoot` がそれを ASSERT する。

### bootmem（`mm/bootmem`）

- 責務：頁の割り当て手ができる前のメモリー。topology・ACPI・pmm の記録が使う。
- 形：bootloader の usable の範囲を上から削る。`bootmem_by_node` が範囲を node の境で切る。`bootmem_alloc_node` で node に置ける。
- 最初の 1 MiB は渡さない。wake の code（`arch/wake.c`）のものである。
- 終わり：`bootmem_handoff` が渡さなかった範囲を pmm へ渡す。その後の確保は panic になる。

### pmm（`mm/pmm`）

- 責務：4 KiB の frame の割り当て。
- 記録：frame ごとに 32 byte の記録がある。記録は 1 GiB の section ごとに、section の node に置く。物理番地の穴は section の表の一項目だけを使う。
- node ごとの buddy：node ごとに、番地の bit 数の帯（band）ごとに、order ごとの空きの列を持つ。DMA の幅の制限は帯で選ぶ。幅を言わない確保は一番高い帯から取り、低いメモリーを装置に残す。
- CPU ごとの溜め：各 CPU は自分の home node の frame を溜める。一回に batch の数だけ node と出し入れする。ほとんどの確保と解放は、他の CPU が取る lock を取らない。溜めの合計は node のメモリーの 1/1024 までである。
- 近い順：node ごとに、SLIT の距離で並べた node の列を持つ。自分の node に無ければ近い node から取る。
- 10 倍のとき：node の lock は batch ごとに一回である。CPU が 10 倍なら、溜めの大きさは node のメモリー / (1024 × CPU の数) で自動で小さくなる。記録は RAM に比例する。メモリーが尽きたときだけ、`steal` と `drain_all` が全 CPU の溜めを見る。

### vmm（`mm/vmm`）

- 責務：頁表と TLB。核の半分（全空間で同じ）とアプリの空間（`struct vmm_space`）。
- direct map：RAM 全体を 1 GiB の頁で写す。端は 2 MiB、さらに端は 4 KiB で写す。Nocturne が動く CPU はすべて 1 GiB の頁を持つ（`arch/cpu.c` の必須の一覧）。memory map の framebuffer は WC（PAT 5）、reserved は UC（PAT 3）で写す。
- 型の変更：`vmm_direct_type` は RAM の頁の型を変える。GPU と共有の buffer を WC・UC にするときに使う。x86 は一つの頁のすべての写像で型が一致することを求める。大きな頁は先に割る。割ることは訳を変えないので、撃ち落としは要らない。割った後に葉を変え、一回だけ撃ち、clflush で古い行を出す。
- 核の半分の葉は global（CR4.PGE）である。核の番地を外したら必ず撃つ。核の番地を外す道は集め（`vmm_gather_init` → `vmm_kernel_unmap` → `vmm_gather_finish`）で、撃ち落としの後に frame を返す。
- 広げる変更（頁を足す、W を足す、X を足す）は撃たない。古い狭い訳で頁違反が起きる。`vmm_fault_spurious` がそれを流す。
- 狭める変更は lock の下で表を変え、lock を放してから撃つ。
- 撃ち落としの宛先：空間の `cpus`（今 CR3 にその空間を持つ CPU）だけに送る。核の半分は online の全 CPU に送る。
- 撃ち落としの形：送り手は自分の要求を一つ持つ。受け手は送り手ごとの bit の受け箱を持つ。答えは last-level cache ごとの数を減らす。一つの cache の CPU の答えは一つの行を分ける。全 CPU の答えが一つの行に集まらない。待つ送り手は自分の受け箱に答える。互いに待ち合う形にならない。
- PCID：Zen 4 の下限に PCID は無いので任意（`cpu_features.pcid`）である。在るときは CPU ごとに 8 つの空間の slot を持ち、空間の gen で撃ち落としの後かを見分ける。
- lazy：核の thread は前の空間を CR3 に残して走る。その空間の次の撃ち落としで離れる。
- 10 倍のとき：空間の撃ち落としは、その空間を持つ CPU の数に比例する。核の半分の撃ち落としは全 CPU に届く。vmalloc の解放は溜めてまとめて撃つので、その数は少ない。

### vmalloc（`mm/vmalloc`）

- 責務：核の番地の範囲。各範囲の後に守りの頁を置く。thread の核の stack が使う。
- `vfree` は頁表を変えない。範囲は retired の列で待つ。retired が RAM の 1/1024 を越えたら purge の thread が全部を一度に外し、一度だけ撃つ。
- `vmalloc_map_frames`・`vmalloc_map_phys` は既存の frame を新しい核の番地に写す。framebuffer の WC の写像（`dev/fb.c`）が使う。

### heap（`mm/heap`）

- 責務：任意の大きさの核の object。
- 頁より小さい object は大きさの級の slab から取る。slab は direct map で使う frame の run で、写像を作らないので撃ち落としが無い。
- 頁以上の object は frame の run で、run が無ければ vmalloc の頁を使う。
- CPU ごとに級ごとの三つの鎖（loaded、previous、away）を持つ。Bonwick の二つの magazine に、他の node の slab の object を返す away を足した形である。CPU は自分の鎖を割り込みを閉じて触り、lock を取らない。
- node ごとに級ごとの depot を持つ。鎖の長さは node のメモリーと CPU の数から決まる。
- 10 倍のとき：depot の lock は鎖一本ごとに一回である。鎖の長さは CPU の数で割って決まる。

### GPU・合成の共有の buffer

共有の buffer の持ち主は `sys/syscall.c` の shared の仕組みである。中核はその下の口を出す。

- `vmm_map_page`（`PTE_SHARED`）：窓の buffer をアプリの空間に写す（`gui/wm.c`）。
- `vmm_direct_type`：RAM の buffer を WC・UC にする。
- `vmalloc_map_frames`：buffer を核の番地に WC で写す。

## 3. 実行

### thread（`sys/thread`）

- 責務：thread の記録、tid、登録簿、核の thread とアプリの thread の始まり・join・detach・終わり・kill。
- 核の stack は vmalloc の 8 頁で、後ろに守りの頁がある。CPU ごとに終わった thread の stack を二つまで溜める。
- 錠：`thread_lock` が登録簿、process の thread の列、process の親子を守る。`thread_lock` は待ち行列の lock より先である。
- tid は正の int32 を回る。今ある tid を飛ばす。

### process（`sys/process`）

- 責務：アプリの handle の表、空間、親子、app の記録。
- 終わり：主 thread の終わりが process を終える。ほかの thread を全部終えてから、handle を閉じ、空間を外す。空間を外すのは `thread_lock` の下である。

### sched（`sys/sched`）

#### 級

級は二つ。締切の級（`SCHED_DEADLINE`）と公平の級（`SCHED_FAIR`）。

締切の級：

- thread は自分の装置の周期を言う（`sched_deadline(period_ns)`）。合成は画面の拍の周期、入力は割り込みの端点の service interval である。
- 起こされたとき、仕事（job）が出る。仕事の締切は、出た周期の終わりである。
- 締切の級の thread の中で、締切が一番早いものが走る（EDF）。締切の級は公平の級のすべてより先に走る。
- 各 thread は周期ごとに予約（reservation）を持つ。constant bandwidth server（Abeni・Buttazzo、1998）の規則で守る。
- 予約を使い切った thread は締切まで公平の級で走る（throttle）。締切で新しい周期が始まり、予約が満ちる。
- 早く起こされた thread は、残りの予約が締切までの時間に帯域で収まるときだけ締切を保つ。収まらなければ、新しい周期を始める。眠って帯域を貯めることはできない。
- 予約は仕事を追う。scheduler は仕事の使った時間を、仕事が終わったとき（thread が眠ったとき）と、走ったまま周期が終わったときに測る。予約は最近の頂点の二倍で、[周期 / 64, 周期 − 周期 / 16] に収める。頂点は仕事ごとに 1/32 ずつ下がる。
- 予約は取れる量の上限であり、取っておく量ではない。使わない予約の時間は他の thread へ行く。だから余裕は、thread が行儀よく走る間は何も失わない。
- 上限の周期 − 周期 / 16 により、締切の thread が走りすぎても、その CPU の公平の thread は周期の 1/16 を保つ。
- この形に決め打ちの周期と予算は無い。周期は装置から来る。予算は測った値から来る。定数は推定の速さと公平の級の最低の取り分だけである（`DL_LEAST_SHIFT`・`DL_SHARE_SHIFT`・`DL_DECAY_SHIFT`）。

公平の級：

- EEVDF（Stoica・Abdel-Wahab、1995）。重さに比例して CPU 時間を分ける。適格な thread の中で、要求の終わりが一番早いものが走る。
- 木は仮想時間で並べ、各節は部分木の一番早い仮想締切を持つ。O(log n) で選ぶ。

優先度の継承：

- mutex を待つ締切の thread は、自分の締切を持ち主に貸す。
- 持ち主は貸された締切の一番早いもので、締切の級で走る。その間は throttle されない。
- 持ち主が別の mutex を待てば、締切は連なりの先へ進む。
- 貸した締切は、mutex を手渡すときに戻る。待つ thread は眠っているので、貸した締切は待つ間変わらない。

#### 置き場所

- 起こされた公平の thread：前の CPU が idle ならそこ、同じ LLC の idle の CPU（core が丸ごと idle のものを先に）、それも無ければ前の CPU。
- 起こされた締切の thread：前の CPU が idle ならそこ、同じ LLC の idle の CPU、前の CPU か同じ LLC の CPU で締切の thread を持たないもの（そこの公平の thread を即座に横取りする）、同じ node とその先の idle の CPU、最後に前の CPU。締切の thread は互いの後ろに並ばず、CPU に散る。
- 手がかり：LLC・node・全体の三段の group が、idle・busy・free（締切の thread を持たない）の bit を持つ。各 CPU は自分の bit を自分の lock の下で立て下ろしする。group の bit は親に一つの bit で伝わる。全 CPU を見る道は無い。
- idle になる CPU は、core・LLC・node・全体の順に busy の bit から一つ引く（締切の thread を先に）。
- 忙しい CPU は slice の終わりに、idle の CPU か二つ少ない CPU へ一つ押す。core と LLC は毎回、node と全体はまれに見る。

#### 起こしてから走るまで

- 起こし手は相手の run queue の lock の下で `sched_resched` を立て、lock を放してから `cpu_kick` する。
- `cpu_kick` は、相手が MWAIT で自分の語を見ているなら、その語を書くだけで起こす（IPI 無し、`arch/idle.c`）。そうでなければ `VEC_RESCHED` の IPI を送る。
- 相手は割り込みからの出口で切り替える。核の中では横取りの次の点で切り替える。

#### tickless

- 周期の拍は無い。slice の alarm は、今の thread の後ろに待つ thread があるときだけ立てる。公平の thread なら要求の終わり、締切の thread なら予約の終わりである。alarm は早めの幅（1/16）を持ち、他の alarm と割り込みを分ける（`sys/alarm.c`）。
- throttle された thread の締切は、その thread の replenish の alarm が持つ。

#### 錠の順

```
thread_lock → 待ち行列の lock → thread の pi lock → run queue の lock → CPU の alarm の lock
二つの run queue の lock は CPU の番号の順に取る。
```

#### 10 倍のとき

- 締切の thread が 10 倍：各 CPU の EDF の木は O(log n)。置き場所は free の手がかりで散る。一つの CPU の締切の帯域の合計が 1 を越えるのは、締切の thread が CPU より多いときだけである。
- CPU が 10 倍：手がかりの bit は group ごとの語の列で、親は group ごとに一つの bit を見る。置き場所と引きは、bit の立った最初の member を辿る。
- 周期が 10 倍短い（600 Hz、1.67 ms）：予約の下限は周期 / 64 = 26 µs、上限は 1.56 ms。alarm は TSC-deadline の one-shot（`arch/lapic_timer.c`）で、周期の長さに依らない。

### 窓・GPU・音・入力がどの thread でどの級で走るか

| 仕事 | thread | 級 | 周期 |
|---|---|---|---|
| 合成（`gui/wm.c`） | `compositor`（核の thread 一本） | 締切 | `frame_period()`：表示中の mode の拍。変われば言い直す |
| 入力（`dev/xhci.c`・`dev/usbhid.c`） | `usb` | 締切（割り込みの端点が開くまでは公平） | 開いた割り込みの端点の service interval の一番短いもの（xHCI 6.2.3.6） |
| 音（`dev/audio.c`） | thread 無し。card の割り込みか alarm の中で混ぜる | — | — |
| 装置の irq の thread（`arch/irq.c`） | irq ごと | 公平、要求は最短 | — |
| GPU（`dev/pciuser.c`、NVIDIA は user の `nvrm`） | アプリの thread | 公平 | — |
| RDP の音・画面（`vm/rdp.c`、VM だけ） | `rdpsnd`・`rdp` | 締切 | 音の block（20 ms）、画面を見る間隔（10 ms） |

合成の構造（画面の数、flip、面の共有）は合成の持ち主が直す。中核は、画面ごとの合成の thread がそれぞれの周期を言えば、それぞれの締切で並べて予約で守る。

## 4. 待ち

### wait（`sys/wait`）

- 待ち行列：FIFO の item の列。thread の item と callback の item がある。排他の item は `wq_wake(wq, n)` で n 個だけ起こす。
- 待ちは条件の lock の下で入る（`wq_wait_locked`）。lock の無い語なら `wq_wait_unless`。
- 締切は thread の alarm が持つ。boot の時計の締切（眠りの間も進む）は別の列で、S3 から戻ると置き直す。
- kill：killable な待ちは kill で終わる。一つの kill は一つの待ちだけを終える。
- mutex：FIFO で手渡す。締切の待ち手を先に渡す。持ち主が別の CPU で走る間は回って待つ。持ち主は再び取れる（depth）。窓の管理（`wm_mutex`）と C: の呼出しが入れ子で取る。
- rwmutex：FIFO。書き手の後に来た読み手は書き手の後ろに並ぶ。

### address_wait（`sys/address_wait`）

- 責務：アプリのメモリーの語の待ち（`SYS_WAIT_ADDRESS`・`SYS_WAKE_ADDRESS`）と、事象の行列の語の見張り。
- 鍵は語の物理番地である。同じ共有頁を写すアプリが同じ待ちで会う。
- bucket の数は CPU の数 × 64 から決まる。

### epoch（`sys/epoch`）

- 責務：錠なしで読まれる記憶を、読み手が全員離れてから返す。handle の表が使う。
- 読む区間は横取りを止めるだけである。
- 猶予期間は LLC の group ごとに数える。group ごとに worker の thread があり、返す仕事はその thread で走る。
- 静かな点を長く通らない CPU は、期間の始まりから 10 ms で kick される。

## 5. ABI

中核の変更でネイティブ ABI は変わらない。アプリの thread が締切の級を求める口（周期の宣言）は今の ABI に無い。アプリの thread は公平の級で走る。
