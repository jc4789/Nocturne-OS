# 波 2 の設計（波 1 を取り込んだ main 8eb59b91 に合わせた版）

波 2 は「実行の単位と待ち」の層：thread と process、run queue と級、横取り、待ちの部品、kernel object と handle。監査の判定は `sys/sched.c`・`kei.c`・`object.c`・`events.c`・`channel.c` が「消して建て直す」、`watch.c` が「残せる」（`D:\Programes\時間の道とカーネルの sys の監査.txt` の【sys/sched.c】〜【sys/channel.c】）。

## 0. 波 1 から使う口（今の main にある物）

- 横取りの数：`arch/preempt.h` の `preempt_count`（bit 0〜15 は `preempt_disable` と持っている spinlock、16〜23 は割り込みの深さ、24〜27 は NMI・#MC の深さ）、`preempt_disable`／`preempt_enable`、`in_interrupt()`、`in_nmi()`、`preemptible()`。今は数を保つだけで、横取りはまだしない（注記のとおり）。
- 錠：`arch/spin.h` の MCS の spinlock（`spin_lock`／`spin_unlock` は割り込みも閉じる、`spin_take`／`spin_give` は閉じてある時、rank の照らし）。待つ間は TLB の受け箱を見ない。
- 割り込み：`arch/irq.h`。hard handler は割り込みを閉じて短く、`IRQ_WAKE_THREAD` は irq の thread（`kthread_create_on` でその irq の CPU に縛った核の thread）を起こす。isr.asm の出口：利用者へ戻るときだけ `sched_user_return`。
- 起こし：`cpu_kick(i)`（相手が MWAIT で眠っていれば語を書く `cpu_idle_wake`、でなければ `VEC_RESCHED` の IPI）、`cpu_alarm(i)`、`cpu_idle()`（`arch/idle.h`）。
- 時間：`sys/clock.h`（monotonic・boot・wall、どこからでも読める）、`sys/alarm.h`（CPU ごとの heap、`alarm_set_range(a, early, latest)` で近い期限をまとめる、fire は割り込みの中）。
- 記録：`sys/log.h`（どこからでも書ける。書き手は錠を取らない）。

## 1. 順

- **2a（一人、単独で）**：process と thread を分け、`sched.c` を持ち主ごとのファイルへ割る。`struct task` はほぼ全部のファイルが触るので、ほかの書き手と並べない。
- **2b（三人、並べて、作業木で）**：S（run queue・級・横取り・均し）、W（待ちの部品・mutex・読み書きの mutex・番地の待ち・眠り）、O（object・handle・events・channel、epoch の回収）。
- **2c（本体）**：優先度の継承（W の mutex・番地の待ちが S の継承の口を呼ぶ）を、S と W を取り込んだ後につなぐ。

## 2. 2a：process と thread（持ち主 T）

今の `struct task` は sched の entity、待ち、核の stack、FPU、handle の表、アプリの空間の配置（mmap の前線と穴、共有の写像）、家族、アプリの記録を一つに持つ（`sys/sched.h`）。

- `struct process`（`sys/process.h`・`process.c`）：pid、handle の表、`vmm_space` の参照、空間の配置（波 3 で mm へ移すまでここに置き、持ち主の注記を付ける）、親子、アプリの記録、終わりの値、thread の一覧と数、子の待ち。
- `struct thread`（`sys/thread.h`・`thread.c`）：tid、持ち主の process、核の stack、FPU の領域、FS・GS の基、利用者の stack、sched の entity（中身は S の `struct sched_entity`。2a では今の欄をそのまま入れた入れ物）、待ちの欄（中身は W の）、眠りの alarm、CPU の時間。
- kernel thread（`kthread_create`・`kthread_create_on`、irq の thread、pm の thread など）が process を持つか：呼ぶ側を読んで決め、注記する。handle の表を使う核の thread が無ければ process は NULL。
- ID：pid と tid は使い回す割り当てで、上限の定数を置かない（伸びる表。今の `lib/idtable` が合わなければ ID の割り当ての部品を lib に建てる）。使い回しの前に間を置くかを決めて注記。`task_find` は O(1)。
- thread の数の上限（今の 128）を消す。
- 終わり：process の最後の thread が終わると、handle の表を閉じる。各持ち主の後片付けはその object の close（`pci_user_is_claim` の特例、`wm_process_exit`・`net_process_exit`・`net_task_exit`・`sys_shared_cleanup` の直の呼出しを消し、それぞれの object の close へ）。sched は他の持ち主を呼ばない。window・PCI の claim・接続が handle でない物を持つなら、その持ち主に handle の object を持たせる。
- `sched.c` を割る：run queue と schedule と idle（`sys/sched.c`）、thread と process の一生（`thread.c`・`process.c`）、待ち（`sys/wait.c`・`wait.h`）、番地の待ち（`sys/address_wait.c`・`address_wait.h`）。各ファイルは使う見出しを直に include する（まとめて渡すだけの見出しを作らない）。2a では待ちと番地の待ちの中身は変えない（W が建て直す）。
- `task_printf_stderr` は持ち主（console の stream、`sys/stream.c`）へ。

## 3. 2b-S：run queue・級・横取り・均し

- **横取り**：`preempt_enable()` で数が 0 になり `need_resched` が立ち、割り込みが開いていれば `schedule()`。割り込みから核へ戻る出口（isr.asm の `.back`、核へ戻る側）でも、数が 0 で、戻る先の RFLAGS.IF が立ち、`need_resched` なら横取りする。核の thread も横取りされる。spinlock の間（数が 0 でない）は横取りしない。`preempt.h` の「まだ横取りしない」の注記をこの段で消す。
- **級は二つ**：
  - 実時間：固定の優先度、同じ優先度の中は FIFO。予算（周期ごとの走る時間）を持ち、使い切ったら周期の残りは公平の級で走る（飢えを防ぐ）。irq の thread（`arch/irq.c` の thread を作る所が級を与える）、合成・入力・音・表示の thread が使う。
  - 公平：EEVDF。仮想時間、lag（起こしで保つ）、資格（lag ≥ 0）、締め切り（資格の時刻 + 要求 / 重み）。資格のある中で締め切りの一番早いものを選ぶ。起きた thread が資格を持ち締め切りが今のものより早ければ横取りする。thread ごとの要求（短い slice＝遅れを取る、長い＝量を取る）と重み。
  - 核の口：`sched_set_policy(thread, …)`（級・優先度・重み・要求）。アプリへの口（ABI）は波 3。
- **queue の木は自分の thread だけ**（今の「全 queue が全 task を持てる」容量と `capacity_reserve` を消す）。
- **slice の期限は自分の CPU の alarm**（`alarm_set_range`：早めてよい幅を slice の小さな割合にして、ほかの期限と割り込みをまとめる）。ほかの CPU の thread を起こしたら、相手の `need_resched` を立てて `cpu_kick`。
- **均し**：topology（`arch/topology.c`）の階層（SMT の兄弟 → LLC → node → 全体）。起こしの CPU 選びは、前の CPU の LLC の中の idle の集合（LLC ごとの `cpuset`）から。idle に入る CPU が近い階層から引く。全部が忙しいときの偏りは、slice の期限が来た時（runnable が 2 以上の時だけ立つ）にその CPU が自分の階層の兄弟を見る（上の階層ほど間を空ける）。熱い道に O(CPU) の走査を置かない。決まった間隔の見回りを作らない。
- **FPU**：利用者の thread から離れる時に XSAVES、利用者へ戻る時にその CPU に載っている状態がこの thread のものでなければ XRSTORS（核の thread への切り替えでは保存も復元もしない）。lazy（#NM）にはしない。
- **CPU の出入り**（S3・休止）：出る CPU の thread を移すのは sched の持ち物（今の `sched_cpu_leave`）。
- **idle**：割り込みを閉じて仕事をしない。`cpu_idle()` を呼ぶだけ。
- `kei.c`・`kei.h` は消す（公平の級の中身は新しく建てる）。

## 4. 2b-W：待ちの部品

- **待ち行列**：FIFO の項目の列。項目は thread か callback（O の events が ready の列へ入れるのに使う）で、排他の印を持つ。`wq_wake(n)` は排他でない項目を全部と、排他の項目を n 個起こす。`wq_wait_locked` の約束（条件の錠の下で入る）は保つ。
- **眠りの期限**：thread ごとの alarm、絶対の ns と時計（monotonic か boot。boot の期限は眠りから覚めた時に計り直す）。大域の `deadline_wait` を消す。
- **mutex**：持ち主と FIFO の待ち手。放す時は先頭の待ち手へ手渡す（後から来た者が割り込まない）。持ち主が他の CPU で走っている間だけ回ってよい（回数でなく持ち主の状態で止める）。入れ子（今の depth）が要る所を数え、要らなければ消す。
- **読み書きの mutex**：読み手は並び、書き手は FIFO の順で、書き手を飢えさせない。
- **番地の待ち**（`SYS_WAIT_ADDRESS` の中身）：物理の番地の hash、bucket ごとの錠、bucket の数は CPU の数から。期限は絶対の ns。起こしは bucket の中の該当だけ。events の番地の見張り（今の `address_watch_*`）も同じ bucket に並ぶ。
- 優先度の継承は 2c（S の口を呼ぶ）。

## 5. 2b-O：object・handle・events・channel

- **読みの錠なし**：handle の表の読みは錠を取らない。表の差し替えと handle の解放は、全 CPU が静かな点（切り替え・idle・利用者の空間）を一度通った後に返す（epoch の回収。`sys/epoch.c`・`epoch.h`、O の持ち物。口は `epoch_enter`／`epoch_leave`、後で返す `epoch_defer`、待つ `epoch_wait`、静かな点 `epoch_quiet`。静かな点の呼出しは S の `schedule`・idle・利用者への戻りに一行ずつ）。読む側は横取りを止める（`preempt_disable`）だけ。
- **handle の値**：表の番号と世代（使い回した番号が古い値で別の object を指さない）。`n_handle` は非負の int32 のまま（ABI §1）。
- **権利**：mask（読む・書く・待つ・渡す…）。今の `writable` を置き換える。ABI に権利を見せるかは波 3。
- **notify**：`object_notify` は登録された待ちの callback を呼ぶ。events の待ち行列は、callback で watch を自分の ready の列へ入れ、待ち手を一人起こす。`events_wait` の全 watch の走査を消す。
- **timer**：一つずつ alarm。期限は絶対 ns、周期は ns で位相を保つ（遅れても周期の格子に戻す）。ABI の ms は波 3 で ns へ。
- **事象に時刻**（単調 ns）を付ける欄を `struct n_event` に足すのは波 3（ABI）。核の中では今から持つ。
- **channel**：channel ごとの錠。名前の空間は伸びる hash（`lib/idtable` か同じ形）。容量は受け手の信用（決め打ちの 1 MiB を消す）。起こすのは一人。送りも受けも絶対期限で待てる（核の口。ABI は波 3）。
- `watch.c` は残し、新しい notify に合わせる。

## 6. 継ぎ目（2b の書き手の間）

| 口 | 持ち主 | 使う側 |
|---|---|---|
| `struct sched_entity`、`sched_make_ready`、`schedule`、`sched_set_policy`、横取りの点（`preempt_enable`・isr.asm の核への出口） | S | W、T、O、irq.c |
| 待ち行列の項目（thread・callback・排他）、`wq_wake`、`sleep_until`、mutex、読み書きの mutex、番地の待ち | W | S（idle の起こしは使わない）、O、全部 |
| `epoch_enter`／`epoch_leave`、`epoch_defer`、`epoch_wait`、静かな点 `epoch_quiet` | O | S（静かな点を呼ぶ）、mm（波 3） |
| `struct process`・`struct thread`、ID | T（2a） | 全部 |

2b の三人は同じ時に `sys/sched.h`（2a の後は `thread.h`）の欄を足すので、各人は自分の欄を自分の型（`struct sched_entity`、待ちの欄の型、epoch の欄）にまとめ、thread には一つの欄として入れる。取り込みは S → W → O の順に本体が行う。

O の notify と events は W の待ち行列の callback の項目の上に建つので、O は二つの段で書く。一段目（S・W と同じ時）：epoch、handle の表（読みの錠なし・世代・権利）、channel の錠と名前の空間と信用、timer。待ちは 2a の口のまま使う。二段目（本体が S と W を取り込んだ後、同じ書き手へ続きを頼む）：main へ載せ直し、`object_notify` と events を W の callback の項目へ移し、静かな点の三行を S の `schedule`・idle・利用者への戻りへ入れる。
