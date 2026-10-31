# 段 2b-S：run queue・級・横取り・均し（依頼文と報告）

## 依頼文

あなたは Nocturne OS（独自の OS。UNIX でも POSIX でも Linux でもない。ユーザーはその三つを嫌う）のカーネルを建て直す、波 2b の書き手の一人（S）です。git の作業木（worktree）の中で働きます。同じ時に二人が別の作業木で書いています：W（待ちの部品）と O（object・handle・events・channel）。取り込みは本体の会話が S → W → O の順に行います。

### 必ず最初に読むもの
本体の作業木の絶対パスで読んでください。あなたの作業木の同じ名のファイルは古いことがあります。
- `D:\Programes\llm vibe slop os\AGENTS.md`：作業の決まり。特に「製品のコード」。
- `D:\Programes\llm vibe slop os\.claude\rules\kernel.md`：カーネルの約束（対称のカーネル、CPU ごとの変数、lock の順、撃ち落とし）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\00-design.md`：全体の設計。§1〜§3（全員の決まり）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2-design.md`：波 2 の設計。§0（波 1 の口）、§1、§3（あなたの節：これが契約）、§6（継ぎ目）。§4・§5 は W と O の節で、あなたの形がそれを妨げないことだけ見てください。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2a.md` の「報告」：2a が作った `struct process`・`struct thread` とファイルの割り方、§5 の口の名。
- 監査 `D:\Programes\時間の道とカーネルの sys の監査.txt` の【sys/sched.c・sched.h】【sys/kei.c・kei.h】。行番号は少し前のもので、ずれています。

書き換えるコードは、あなたの作業木の中のもの（2a を取り込んだ main と同じ）です。

### 頼むこと（2-design.md §3 のとおり）
1. 横取り：`preempt_enable()` で数が 0 になり `need_resched` が立ち、割り込みが開いていれば `schedule()`。割り込みから核へ戻る出口（`arch/isr.asm`）でも、数が 0、戻る先の RFLAGS.IF が立つ、`need_resched` のとき横取りする。核の thread も横取りされる。`arch/preempt.h` の「まだ横取りしない」の注記を消す。
2. 横取りを入れると、「核は横取りされない」を前提にしたコードが壊れる。`this_cpu`・`this_cpu_ptr` の使いは今 19 ファイルに 78 件ある。全部を読み、横取りできる所で CPU ごとの物に触れるなら `preempt_disable`／`preempt_enable` の区間に入れるか、割り込みを閉じた区間・spinlock の下に限られていることを確かめる。`need_resched` を見て `schedule()` を呼ぶ所（今 23 件）は、横取りがあれば要らない所を消す。数えた結果と直した所を報告に書く。`kernel.md` の「核は横取りされない」の項は本体が直すので、あなたは直さない。
3. 級は二つ（§3 の本文のとおり）：実時間（固定の優先度、同じ優先度の中は FIFO、周期ごとの予算、使い切ったら周期の残りは公平の級）と公平（EEVDF：仮想時間、lag、資格、締め切り、要求と重み、起きた thread の横取り）。核の口 `sched_set_policy(thread, …)`。irq の thread を作る所（`arch/irq.c`）が実時間の級を与える。合成・入力・音・表示の thread も実時間にする（どの thread かは作る所を読んで決め、報告に一覧を書く）。アプリへの口（ABI）は波 3 なので作らない。
4. queue の木は自分の thread だけ。今の「全 queue が全 task を持てる」容量と `capacity_reserve` を消す。
5. slice の期限は自分の CPU の alarm（`alarm_set_range`、早めてよい幅は slice の小さな割合）。ほかの CPU の thread を起こしたら相手の `need_resched` を立てて `cpu_kick`。
6. 均し：`arch/topology.c` の階層（SMT の兄弟 → LLC → node → 全体）。起こしの CPU 選びは前の CPU の LLC の idle の集合（LLC ごとの `cpuset`）から。idle に入る CPU が近い階層から引く。全部が忙しいときは slice の期限（runnable が 2 以上の時だけ立つ）にその CPU が自分の階層の兄弟を見る。熱い道に O(CPU) の走査を置かない。決まった間隔の見回りを作らない。
7. FPU：利用者の thread から離れる時に XSAVES、利用者へ戻る時にその CPU に載る状態がこの thread のものでなければ XRSTORS。核の thread への切り替えでは保存も復元もしない。lazy（#NM）にしない。
8. CPU の出入り（S3・休止）で出る CPU の thread を移すのは sched の持ち物。idle は割り込みを閉じて仕事をせず `cpu_idle()` を呼ぶだけ。
9. `sys/kei.c`・`kei.h` を消す（公平の級は新しく建てる）。
10. thread が CPU を離れるのを待つ所（`sched_thread_remove` の中の pause の待ち、旧の `off_cpu_wait`）は回数でなく事象で終わる形にする。終わった thread の解放は今は大域の reaper の thread 一つ（`sys/thread.c` の `reaper`）が受け持つ：一つの CPU に寄せる仕事なので、切り替えの後（その CPU が終わった thread から離れた後、`sched_post_switch` の辺り）に、その CPU が自分で解放するか CPU ごとの列へ渡す形にし、reaper を消す。解放の中身（`thread_release`・`process_put`）は T（`sys/thread.c`・`process.c`）の口を呼ぶ。

### 決まり
- 持ち主の単位で消してから建てる。古い名を残す別名・包み・渡すだけの関数を作らない。名前だけの口を作らない。
- 決め打ちの上限を置かない（CPU の数、thread の数、優先度の段の数は表の大きさを実際の数から）。待ちは割り込みか事象で起こされる形で、決まった間隔の見回りや回数の spin を作らない。
- 待ち行列・mutex・番地の待ち（`sys/wait.c`・`address_wait.c`）は W の持ち物で、中身を変えない。呼ぶ口（起こされた thread を run queue へ入れる `sched_make_ready` など）は §6 の表のとおりあなたが持つ。W はあなたの口の名を 2-design.md §6 で知っているので、名を変えるなら報告に書く。
- 静かな点（O の `epoch_quiet`）は O の二段目で本体があなたの `schedule`・idle・利用者への戻りに一行ずつ入れる。その三か所が一行で入る形にしておく。
- thread への欄は `struct sched_entity` 一つにまとめ、`struct thread` には一つの欄として入れる（W と O も同じ時に自分の欄を足すので）。
- ABI（`common/abi.h`）は変えない。アプリから見える振舞いを変えない。
- 的は実機の Zen 4（Ryzen 5 7600X）以上。PCID は Zen 4 に無い（PPR 56713 で CPUID 1 ECX[17] は Fixed,0）。QEMU の都合で設計しない。VM だけの処理は `kernel/src/vm/vm.h` の後ろ。
- Linux・BSD・NT ほか OS のソースを読まない・写さない（patch のメールも）。GPLv2 だけのコードを入れない。公開された論文と仕様（EEVDF の原論文など）はよい。
- コードの注記は周りに合わせて英語、今あるコードの書き方（名、注記の濃さ）に合わせる。
- 編集は Edit・Write で。シェルや Python でファイルを書き換えない（この Windows の Python は LF を CRLF に変える）。シェルは Bash（Git Bash）。探すのは `rg`。
- 組んで確かめる：作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`（出力は本体の `build/wt/<作業木の名>/`）。警告を増やさない。QEMU は起こさない。媒体は作らない（本体の会話が取り込んだ後に起動して確かめる）。
- 作業木の枝にコミットする（題は日本語で「段 2b-S：何をしたか」、本文の最後に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。main へ取り込まない。push しない。

### 報告（最後のメッセージ。日本語。本体の会話がそのまま下の「報告」へ写す）
1. 消したもの（ファイル・関数・定数・欄）。
2. 建てたもの：`struct sched_entity`、二つの級の算法と口、横取りの点、slice の alarm、均しの形、FPU の切り替え。
3. 横取りの監査：`this_cpu` の 78 件と `need_resched` の 23 件をどう扱ったか（ファイルごと）。CPU を離れる待ちと reaper をどう替えたか。
4. 実時間の級にした thread の一覧と、その理由。
5. 2-design.md §3 から外れた所と、その理由。
6. 自分の持ち物の外に触れた所（ファイルと関数）。W と O の書き手の欄・ファイルと重なりうる所。
7. 確かめたこと（ビルドの最後の数行）と、確かめていないこと。
8. 作業木の枝の名と、コミットの一覧。

## 報告（書き手の最後のメッセージ、全文）

## 段 2b-S の報告（書き手 S）

結論：依頼文の 1〜10 を全部入れ、作業木の枝 `worktree-agent-a9a0494c32ce1b49c` にコミット `67e35a26` で載せました。カーネルは警告なしで組めます。QEMU と実機ではまだ動かしていません（依頼文の決まりのとおり）。

### 1. 消したもの
- **ファイル**：`kernel/src/sys/kei.c`、`kernel/src/sys/kei.h`（git rm）。
- **sched.h と sched.c**
  - 古い sched.c の中身を全部建て直しました。全 queue が全 task を持てる容量、`capacity_reserve`、kei の選択核、`off_cpu_wait` に当たる pause の待ちが消えています。
  - `need_resched` のマクロと `sched_cpu_idle` を消しました。
  - `sched_thread_add`・`sched_thread_bind` は失敗を返さない形（void）にしました。
- **thread.c と thread.h**
  - 大域の reaper を消しました：`dead`、`dead_wait`、`reaper()`、`thread_init()`、`thread_reap()`。
  - `struct thread` の欄 `dead_next` を消しました。
  - boot.c の `thread_init();` の呼出しを消しました。
- **need_resched を見る所**
  - 各所の `if (need_resched) schedule();` を消しました（数は 3 にファイルごと）。
  - net.c の `net_yield()` を消しました。
  - channel.c の `COPY_PIECE` と、区切って写す輪を消しました。
  - fbcon.c の `draw_some` の途中で止める道を消しました。
  - document.c の `YIELD_PAGES` は `STOP_PAGES` に名を変え、kill を見るだけにしました。
- **README**：「継」の行と、kei_sched の使用許諾の行を消しました。

### 2. 建てたもの

**struct sched_entity（sys/sched.h、struct thread の欄 `sched` 一つ）**
- 欄は次のとおりです：`state`、`idle`、`on_cpu`、`queued`、`throttled`、`cpu`、`bound`（CPU 番号＋1）、`fpu_cpu`、`cpu_ns`、`policy`、`node`（AVL）、`seq`、`vruntime`、`deadline`、`min_deadline`、`lag_ns`、`rt_used`、`rt_period_end`、`replenish`（alarm）。

**級と口**
- `struct sched_policy {class, priority, weight, request_ns, budget_ns, period_ns}` を置きました。
- `sched_set_policy(thread, policy)` は、走っている thread や待っている thread にもすぐ効きます。
- `sched_realtime(priority)` は周期 10 ms・予算 8 ms の既定を返します。
- **実時間の級**
  - 優先度の高い順、同じ優先度の中は来た順に選びます。横取りされた thread はその優先度の先頭へ戻ります（`rt_head` を減らす）。それ以外は末尾です。
  - 周期ごとに予算を数えます（`rt_renew`）。使い切ると `throttle` で公平の級へ移ります（lag 0、平均の位置）。
  - 周期の終わりに thread ごとの alarm `replenish` が `unthrottle` します。
- **公平の級（EEVDF、Stoica らの論文 1995）**
  - V = vbase + sum/weight で持ちます（sum は 128 bit、今走る thread も含む）。0 ≤ sum < weight に揃え直します（`rebase`）。
  - 資格は v ≤ V（lag ≥ 0）です。
  - 入る時は lag を保つ位置に置きます：v = V − vlag·(W+w)/W。出る時は lag を ±要求の範囲で記録します。締め切りは v ＋ 要求/重み です。
  - 木は (vruntime, seq) の AVL で、各節が部分木の一番早い締め切りを持ちます。資格のある中で締め切りの一番早いものを O(log n) で選びます（`fair_pick`）。
  - 起きた thread が資格を持ち、締め切りが今の thread より早ければ横取りします（`preempts`）。
  - yield は要求の残りを捨てます（v = deadline）。
- 部品として `lib/avl.c`・`avl.h`（侵入型、増やした AVL）を足しました。`lib/span` は同じ AVL の上に載せ直し、口は変えていません。

**横取りの点**
- `preempt_enable` で数が 0 に戻り、旗が立っているとき。
- `irq_restore` で割り込みが開くとき（cpu.h）。
- 割り込みから核へ戻る出口（isr.asm）。数が 0、戻る先の IF が立つ、旗が立つ、の三つがそろえば `sched_preempt_irq` を呼びます。
- 利用者への戻り（`sched_user_return`）。
- 旗は CPU ごとの `sched_resched` で、相手の queue の lock の下で立てます。
- `spin_take` は `preempt_disable` を最初に行う順に直しました。
- `kthread_start` は `irq_restore(0x200)` で始めます。
- `preempt_disable`・`preempt_enable` は `incl`/`decl %gs:` の一命令になることを逆アセンブルで確かめました。

**slice の alarm**
- queue ごとの `slice` を `alarm_set_range` で立てます。早めてよい幅は要求（公平）か予算（実時間）の 1/16 です。runnable が 2 以上の時だけ立ちます。
- ほかの CPU の thread を変えた時の扱いは次のとおりです（`tell`）。
  - 横取りが要るなら、相手の旗を立てて `cpu_kick`。
  - 要らなければ `slice_stale` を立てて kick し、相手の VEC_RESCHED の処理で slice を立て直します。

**均し**
- 群は LLC（CPU が要素、core 全体が idle かの印つき）、node（LLC が要素）、全体（node が要素）です。
- 各群は idle と busy の印を持ち、自分の lock の下で自分の bit だけを変えます。親の bit は伝えます。
- **起こし**：縛られていれば縛った CPU。そうでなければ、前の CPU が idle ならそこ、次に前の CPU の LLC の idle な CPU（core 全体が idle のものを先に）、最後に前の CPU です。
- **idle に入る CPU**：switch_once の中で queue が空のとき、core の兄弟 → LLC の busy → node → 全体の順に引きます。
- **忙しい CPU**：slice の期限に、core と LLC は毎回、node は 4×3 ms ごと、全体は 16×3 ms ごとに見ます。相手は idle な CPU を先に選び、無ければ順に一つを見て、runnable が自分より 2 少ない CPU へ一つ渡します。
- 熱い道に O(CPU) の走査は置いていません。決まった間隔の見回りもありません。

**FPU**
- 利用者の thread から離れる時、その CPU の `fpu_live` が立っていれば XSAVES します。終わった thread なら保存せず、持ち主を消します。
- 利用者へ戻る時、その CPU の `fpu_owner` が自分でないか、自分の `fpu_cpu` がこの CPU でなければ XRSTORS します。
- 核の thread では何もしません。lazy（#NM）にはしていません。
- `sched_cpu_start` で持ち主を空にします。FS・GS の基は利用者の thread だけ保存し、読み込みます。`tss_set_rsp0` も利用者の thread だけです。

**CPU の出入りと idle**
- `sched_cpu_leave` は queue の thread を CPU 0 へ移し、その CPU の旗を立てて kick します。
- 走っていた thread は `evicted` として、切り替えの後に `sched_post_switch` が置き直します。
- idle の輪は「割り込みを閉じる → `cpu_park_check` → 旗が立っていれば `schedule()` → そうでなければ `preempt_disable(); cpu_idle(); preempt_enable();`」の順です。待ちの中の割り込みで切り替えると idle の語が watching のまま残るため、`preempt_disable` で囲んでいます。

### 3. 横取りの監査

**`this_cpu`・`this_cpu_ptr`**
- 3c704be8 で数え直すと 21 ファイル 82 行でした（依頼文の 78／19 より多い）。

| ファイル | 行 | 扱い |
|---|---|---|
| apic.c | 1 | CPU の起動、割り込みは閉じたまま。安全 |
| gdt.c | 2 | 起動時。`tss_set_rsp0` は switch_once の中、queue の lock の下。安全 |
| idle.c | 3 | `cpu_idle` は idle の輪の中で、横取りも割り込みも閉じている。`cpu_perf_setup` は cpu_setup の中、`cpu_perf_sample` は alarm の処理の中。安全 |
| idt.c | 6 | 起動時と NMI・#MC の処理。安全 |
| irq.c | 7 | 割り込みの処理と vector の表の lock の下。安全 |
| lapic_timer.c | 11 | 起動・復帰の時は割り込みを閉じている。`lapic_wake_at` は alarm の lock の下。安全 |
| percpu.h 2、preempt.h 5、spin.h 10 | 17 | 定義。注記を横取りに合わせて直し、spin_take は preempt_disable を先に |
| smp.c 1、smp.h 1 | 2 | `cpu_park_check` は割り込みを閉じた idle の輪の中。smp.h は `cpu_current_index` の定義 |
| entropy.c | 1 | irq_save の下。安全 |
| heap.c | 5 | irq_save の下で、取り直しもある。`pages_alloc` の home は目安。安全。冒頭の注記を直した |
| pmm.c | 4 | 指は溜めそれぞれの lock の下で使い、node は目安。安全 |
| vmm.c | 7 | **直した**：`shoot` 全体を preempt_disable の区間へ。switch・leave・drain は irq_save の下、`vmm_init_cpu` は起動時 |
| clock.c | 5 | **直した**：CPU 0 側の `clock_cpus_compare` を preempt_disable の区間へ。AP 側は scheduler が動く前、割り込みを閉じたまま |
| prof.c | 1 | AP の起動時と idle0 の中（idle0 は CPU を離れない）。安全 |
| sched.c 6、sched.h 1 | 7 | 建て直し。全部が queue の lock の下か、割り込みを閉じた所 |
| thread.c 2、thread.h 1 | 3 | kstack の溜めは irq_save の下。`thread_current` は一度読むだけ |

- `cpu_current_index()` も中身は `this_cpu` なので、その呼び手も見ました。**直した**のは `alarm.c` の `alarm_clock_changed`（preempt_disable の区間へ）だけです。残りは割り込み・lock の下、目安、報告の文字でした。
- `spin_held` は古い番号を読みえますが、呼び手がありません。

**`need_resched`**
- 23 行・12 ファイル。消した後は 0 件です。

| ファイル | 行 | 直したこと |
|---|---|---|
| sched.c 6、sched.h 1 | 7 | マクロごと消した |
| channel.c | 2 | 送り・受けは一回の写しに |
| stream.c | 2 | 512 byte ずつの写しは stack の都合で残し、旗の所だけ消した |
| syscall.c | 4 | pager_supply、共有の写像の組み、gpu_user_span、gpu_user_copy。kill を見る所は残した |
| document.c | 1 | 消した。kill を見る所は残した（`STOP_PAGES`） |
| fbcon.c | 1 | 消した |
| net.c | 1 | `net_yield` ごと消した |
| vmm.c | 1 | `vmm_space_end` の輪から消した |
| ata.c 2、storvsc.c 1、virtio_gpu.c 1 | 4 | 消した |

- ほかに、acpi.c の生の `schedule()` を `sched_yield()` にしました。
- 使わなくなった `sys/sched.h` の include を ata.c・storvsc.c・virtio_gpu.c・channel.c・fbcon.c・document.c・stream.c から外しました。

**CPU を離れる待ちと reaper**
- pause の待ちは消しました。
- `sched_post_switch` は、前の thread が終わっていれば `thread_left(prev)` を呼びます。条件は ZOMBIE で、木に無く、追い出されたものでないことです。このとき CPU は自分の lock を放していて、割り込みは閉じたままです。
- `thread_left`（T の thread.c）の動きは次のとおりです。
  - 核の thread は `thread_free`。
  - process の main は `process_put`。
  - ほかは thread_lock の下で `ended` を立て、`p->wait` を起こします。detached なら一覧から外し、`thread_free` と `process_put` を呼びます。
- `thread_join`・`thread_end_others`・`thread_detach` は `ended`（CPU が離れた後の印）を見ます。
- `thread_exit` と `process_end` は、割り込みを閉じて ZOMBIE を立ててから `schedule()` するだけです。

### 4. 実時間の級にした thread

| thread | 優先度 | 理由 |
|---|---|---|
| irq の thread（`arch/irq.c` の `irq_thread`） | SCHED_PRIORITY_IRQ 40 | 走りが短く、装置が待つ |
| RDP の音（`vm/rdp.c` の `snd_thread`） | SOUND 30 | 遅れれば音が切れる |
| USB（`dev/xhci.c` の `usb_thread`） | INPUT 20 | キーボードと指の報告 |
| RDP の session（`vm/rdp.c` の `session`） | INPUT 20 | 相手の入力と画面の更新 |
| 合成（`gui/wm.c` の `wm_thread`） | COMPOSITOR 10 | 毎コマを期限どおりに |

- 実機の音には自分の thread がなく、irq の thread を通ります。
- 表示（screenmode や GPU）に常駐の thread は見つかりませんでした。合成の thread が表示を受け持ちます。
- 全部が予算 8 ms／周期 10 ms です。使い切ると残りは公平の級で走ります。

### 5. §3 から外れた所
1. **横取りの点に `irq_restore` を足した**：割り込みを閉じた所で立った旗は、こうしないと次の割り込みまで待つためです。
   - 旗の名は `sched_resched` にしました。inline の irq_restore が読むので、宣言は cpu.h にあります。
   - 名ばかりの `need_resched` は残していません。W・O・T への名の変更です。
2. **起こしで相手の旗を立てるのは、横取りが要る時だけ**：要らない時は slice を立て直させるために kick だけします（`slice_stale`）。
3. **idle に入る CPU が引く場所**：idle の輪ではなく、switch_once の中で queue が空と決まった時に引きます。idle が「仕事をしない」ままでいるためです。
4. **起動の文脈（CPU 0 の idle thread）も横取りされる**：kmain_late の間に横取りされると木には入らず、CPU 0 の queue が空になってから戻ります。`sched_idle` に入るまでは idle の印を立てません。そのため新しい thread はほかの idle な CPU へ行きます。
5. **slice の期限の締め切りは 1/16 だけ早く更新する**：alarm を早めてよい幅と同じだけ、要求の終わりより前に締め切りを更新します。
6. **実時間の thread の予算切れは公平の級へ lag 0 で入る**：平均の位置です。

### 6. 自分の持ち物の外に触れた所と、W・O との重なり

**持ち物の外**
- arch：`cpu.h`（`irq_restore`、`sched_resched` と `preempt_schedule` の宣言）、`percpu.h`（注記）、`spin.h`（`spin_take` の順）。
- 級を与える一行：`irq.c` の `irq_thread`、`wm.c` の `wm_thread`、`xhci.c` の `usb_thread`、`rdp.c` の `snd_thread`・`session`。
- T の持ち物
  - `thread.c`：`thread_left` を新しく、`thread_new`・`kthread_new`・`kthread_start`・`thread_detach`・`thread_join`・`thread_end_others`・`thread_exit` を直した。
  - `thread.h`：欄 `ended` を足し、`dead_next` を消した。
  - `process.c`（`process_end`）、`process.h`（注記）、`boot.c`。
- `prof.c`：`last_kei` を `last_sched` に。
- `lib/span.c`・`span.h`：AVL の上に載せ直した。
- mm：`vmm.c`（`shoot`、`vmm_space_end`）、`heap.c`（注記）。
- `sys/clock.c`（`clock_cpus_compare`）、`sys/alarm.c`（`alarm_clock_changed`）。
- need_resched を消した各ファイル、`acpi.c`、README。

**W との重なり**
- `wait.c`・`address_wait.c` には触れていません。
- W は `sched_make_ready`・`schedule`・`sched.idle`・`sched.state`・`sched.on_cpu` を使います。
- `wq_sleep` の idle thread の道（`sti; hlt; cli` のあと `schedule()`）は、そのままで動く形です。
- W は `need_resched` を使っていません。

**O との重なり**
- `channel.c` の `message_channel_send`・`message_channel_receive` で写しの輪を消しました。O も channel.c を書くので、衝突しうる所です。
- `stream.c` も重なりえます。
- `epoch_quiet` を入れる三か所は次のとおりです。どこも一行で入ります。
  - `switch_once` の頭（`schedule`・横取り・yield の全部が通る）。
  - `sched_idle` の輪の頭（`cli()` の直後）。
  - `sched_user_return` の輪の頭。
- `struct thread` には三人が欄を足すので、`ended` の近くで衝突しうります。

**本体へ**
- `.claude/rules/map.md` と `kernel.md` にはまだ kei・`need_resched`・reaper・「核は横取りされない」が残っています（依頼文のとおり触れていません）。

### 7. 確かめたこと・確かめていないこと

**確かめたこと**
- `kernel.h` に touch してカーネルの C ファイル 94 本を組み直し、警告もエラーも出ませんでした。最後の行は次のとおりです。
  ```
    CC   kernel/src/vm/virtio_gpu.c
    CC   kernel/src/vm/vm.c
    LD   ../../../build/wt/agent-a9a0494c32ce1b49c/kernel.elf
  ```
  終了コードは 0 です。uACPI と common/store は変えておらず、その回は組み直していません（その前の全体の組みでは通っています）。
- 逆アセンブルで、`preempt_disable`・`preempt_enable` が `incl`/`decl %gs:` の一命令であることを見ました（alarm_clock_changed）。

**確かめていないこと**
- QEMU と実機での起動・動作（決まりで起こしていません）。
- 横取りの実際の動き、EEVDF と実時間の予算の負荷の下での振舞い、均しの効き、S3 の出入り、FPU の切り替え。
- 生の `sti()` が残る所：`boot.c:102`、`idt.c:132`、`syscall.c:2831`、`thread.c:382`、`acpi.c:301/309`。ここでは旗を見ないので、立った旗は次の点（割り込みの出口など）まで待ちます。正しさは崩れず、遅れるだけの見込みです。
- `thread_free` から呼ぶ `alarm_cancel` が、ほかの CPU の fire の終わりを pause で待つ所は alarm の既存の待ちで、変えていません。

### 8. 枝とコミット
- 枝：`worktree-agent-a9a0494c32ce1b49c`（土台 main `3c704be8`）。
- `67e35a26` 段 2b-S：横取りする核、実時間と公平（EEVDF）の二つの級、topology の階層で均す run queue（38 ファイル、+1305／−1061）。
- main へは取り込んでおらず、push もしていません。
