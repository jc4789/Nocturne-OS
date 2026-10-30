---
paths:
  - "kernel/**"
  - "common/**"
  - "boot/**"
  - "host/**"
---
# カーネルの領域に触れたとき

- **対称のカーネル**（2026-10-29 から）。CPU に役割の差は無い。どの CPU も自分の実行キュー（`sys/sched.c`、`kei`）と idle task を持ち、user の thread も核の thread（`kthread_create`）もどの CPU でも走り、CPU の間を移る。syscall（`SYSCALL` → `syscall_entry`）・例外（`isr_exception`）・割り込みと IPI（`isr_interrupt`、CPU ごとの割り込みの stack の上）は起きた CPU で処理する（`arch/isr.asm`・`arch/idt.c`）。BSP へ回す道、AP の写し・読み取り区間・門は無い（消した名：`cpu_require_bsp`・`sched_enter_bsp`・`cpu_is_runner`・`ap_read_*`・`handle_peek`・`handle_install_locked`・`handle_take_locked`・`handle_table_find/reserve/add`・`events_watch_locked`・`syscall_ap`・`syscall_anywhere`・`isr_dispatch_remote`・`sched_quiesce_space`・`vmm_ap_*`・`net_ap_*`・`wm_state_ap`・`proc_app_result_ap`）。戻さない。
- **CPU が自分で行う手順は、その CPU に縛った thread で**（`kthread_create_on`：task の `allowed` を kei の盗みと移しが守り、`choose` も守る）。眠り（`smp_suspend`・`wake_sleep`）は最初の CPU で走る：pm の thread は CPU 0 に縛ってある。
- **CPU ごとの状態は CPU ごとの変数**（`arch/percpu.h`）。CPU の数で配列を切らない（上限の定数は無い）。自分の CPU のものは `this_cpu`／`this_cpu_ptr`、ほかの CPU のものは `per_cpu(v, cpu)`。CPU を回る輪は `topology.cpus` まで。`this_cpu_ptr` で取った指は、`schedule()` をまたぐと別の CPU のものになりうる。
- **核は横取りされない**が、ほかの CPU と同時に走る。`irq_save`／`cli` が閉じるのはその CPU の割り込みだけで、ほかの CPU との排他にはならない。共有の状態は持ち主の `struct spinlock`（`arch/spin.h`、`spin_lock`／`spin_unlock`、割り込みを閉じてあれば `spin_take`／`spin_give`）か `struct mutex`（`sys/sched.h`、眠れる。確保・写し・待ち・disk をまたぐ状態）で守る。spinlock を持つ間は眠らない・写さない・`mutex_lock` しない。長い核の処理は lock を持たない所で `if (need_resched) schedule();`。
- **待ちは条件の lock の下で入る**。条件を変える者が持つ lock を取り、条件を見て、`wq_wait_locked(&wq, &lock)`（戻ったとき lock は持っていない、割り込みは閉じたまま）。`irq_save` の後の `wq_wait` は、ほかの CPU が条件を変えると起こしを失う。lock の無い語なら `wq_wait_unless`。
- **装置の割り込みは CPU 0 に来る**（IOAPIC・MSI の宛先）。割り込みの処理と、どの CPU でも走る呼ぶ側が同じ状態に触れるなら、両方が同じ spinlock を取る（呼ぶ側は `spin_lock` で割り込みも閉じる）。周期の拍は無い（`dev/timer.c` は消した）。時間の仕事は、その持ち主が自分の alarm（`sys/alarm.h`、CPU ごとの heap）を立てる。
- **利用者のメモリーは写しだけで触る**（SMAP が止める：`stac` するのは `arch/isr.asm` の `user_copy` だけで、ほかの所の触れは panic）。同じアプリの別の thread がほかの CPU でいつでも頁を外せる。`copy_from_user`／`copy_to_user`・`user_str`・`user_string`（`sys/syscall.h`）は頁違反を戻り値に換え、無い頁を作るので眠ることがある：spinlock の下で呼ばない。`user_ok`／`user_ok_w` は長い仕事の前の確かめだけで、確かめた後に利用者の指を直に読み書きしない（`memcpy` で写さない）。要求の構造体は一度核へ写してから見る。数の多い一覧は核の中に作ってから一度に写す。番地の待ちは物理の番地で読む（`sys/sched.c` の `address_read`）。
- lock の順（先に取るもの → 後）：`task_lock` → 待ち行列の lock → 実行キューの lock。`watchers_lock` → 事象の行列の lock（`sys/events.c`）。`heap_lock` → `kmap_lock` → CPU の頁の溜めの lock → node の頁の lock（`mm/pmm.c`）。頁表は `kmap_lock`（核の半分）と、空間ごとの縞の lock（`mm/vmm.c` の `tables_of`）。TLB の撃ち落としはどの CPU からでもよく、spinlock を待つ CPU は待ちの間に撃ち落としに答える（`vmm_tlb_poll`）。
- handle の表はアプリごと（その process の task の `handles_lock`）。`handle_get` が参照を取り、`handle_install` が入れる（失敗なら閉じる）。`task_list` と親子のつながりは `task_lock`、空間の付け替えも `task_lock` の下（`task_exit`）なので、`task_lock` の下で読んだ `pml4` の表はその間は消えない。
- ゲストが止まって見えたら、QEMU の HMP で `info registers -a` を取り、CPU0 の RIP と RBP の鎖（`x /2gx`）を `build/kernel.elf` で llvm-symbolizer に引く。
- カーネルの標本は、起動の cmdline に `prof`（または `prof_ms=<n>`）で有効になり、serial に窓（既定 10 秒）ごとに出る：`prof: cpu%u user … kernel … idle …`、`prof: task pid …`、`prof: <標本数> <rip> pid …`、`prof: switches … wakes … steals …`（実行キューの間の動き）、`prof: TLB …`。起動の cmdline に入れるのは limine の起動の項目を手で編むか、`boot/limine.conf` を変えずに QEMU の起動の画面で。
- 起動の行：`memory: N MiB usable, M MiB free`、`video: monitor … at … Hz`（EDID が決める）、`store: C: "…" on sata0, …`。メモリー切れは `[memory] pid … no pages for …`（10 秒に 1 回まで）。カーネルの行には時刻が無い。
- VM だけの処理（QEMU・Hyper-V の装置、hypervisor の判定）は一か所の入口の後ろに置き、共通コードに書かない。VM の測定値で実機の既定を決めない。
- ABI は `common/abi.h`（`SYS_*` の番号）・`common/display_abi.h`・`common/gpu_abi.h`。仕様は `docs/nocturne-native-abi.md`。番号で引く呼出しは codegraph の索引に出ないので、呼ぶ側は自分で読む。
- 解像度と拍は EDID とカーネル・表示経路（`kernel/src/dev/fb.c`、`kernel/src/gui/screenmode.c`、`kernel/src/gui/wm.c`）が決める。limine.conf で固定しない。
- 目録（`common/store/catalog.c`）の枝の item 0 の鍵は何も区切らない。目録を変えたら `store tree-test <seed> 200000` を幾つかの種で通す（覚え書き）。
- 実機の記録は写真だけ（USB は起動用の一本、Windows から読める）。iGPU・二台目のディスク・BIOS の変更へ逃げない。
