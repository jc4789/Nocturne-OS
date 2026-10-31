# 段 2b-W：待ちの部品（依頼文と報告）

## 依頼文

あなたは Nocturne OS（独自の OS。UNIX でも POSIX でも Linux でもない。ユーザーはその三つを嫌う）のカーネルを建て直す、波 2b の書き手の一人（W）です。git の作業木（worktree）の中で働きます。同じ時に二人が別の作業木で書いています：S（run queue・級・横取り・均し）と O（object・handle・events・channel）。取り込みは本体の会話が S → W → O の順に行います。

### 必ず最初に読むもの
本体の作業木の絶対パスで読んでください。あなたの作業木の同じ名のファイルは古いことがあります。
- `D:\Programes\llm vibe slop os\AGENTS.md`：作業の決まり。特に「製品のコード」。
- `D:\Programes\llm vibe slop os\.claude\rules\kernel.md`：カーネルの約束（待ちは条件の lock の下で入る、lock の順、利用者のメモリーの写し）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\00-design.md`：全体の設計。§1〜§3（全員の決まり）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2-design.md`：波 2 の設計。§0（波 1 の口）、§1、§4（あなたの節：これが契約）、§6（継ぎ目）。§3・§5 は S と O の節で、あなたの形がそれを妨げないことだけ見てください。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2a.md` の「報告」：2a が作った `struct thread`、`sys/wait.c`・`address_wait.c` の割り方、§5 の口の名。
- 監査 `D:\Programes\時間の道とカーネルの sys の監査.txt` の【sys/sched.c・sched.h】（待ち・mutex・眠り・番地の待ちの部分）【sys/syscall.c・syscall.h】（`SYS_WAIT_ADDRESS` と眠りの部分）【時間の ABI】。行番号は少し前のもので、ずれています。

書き換えるコードは、あなたの作業木の中のもの（2a を取り込んだ main と同じ）です。

### 頼むこと（2-design.md §4 のとおり）
1. 待ち行列：FIFO の項目の列。項目は thread か callback（O の events が自分の ready の列へ入れるのに使う）で、排他の印を持つ。`wq_wake(n)`（名は決めてよい）は排他でない項目を全部と排他の項目を n 個起こす。`wq_wait_locked` の約束（条件の lock の下で入る、戻ったとき lock は持っていない）は保つ。今の `wq_wait` 系の使いは 85 件。呼ぶ側の形を変えるなら全部を直す。callback の項目は、O が二段目で `object_notify` と events をその上に移すための口で、あなたの段では使い手が無くてよいが、契約（呼ばれる文脈：割り込みの中か、どの lock の下か、眠れないこと）を見出しに書く。
2. 眠りの期限：thread ごとの alarm（`sys/alarm.h`）、絶対の ns と時計（monotonic か boot）。boot の期限は眠りから覚めた時に計り直す。大域の `deadline_wait` を消す。
3. mutex：持ち主と FIFO の待ち手。放す時は先頭の待ち手へ手渡す（後から来た者が割り込まない）。持ち主がほかの CPU で走っている間だけ回ってよい（回数でなく持ち主の状態で止める。走っているかは S の `struct sched_entity` の欄で読むことになるので、今の欄で書き、S の取り込みで本体が合わせる）。入れ子（今の `depth`）が要る所を数え、要らなければ消す。今の `mutex_lock` の使いは 21 ファイルに 78 件。
4. 読み書きの mutex：読み手は並び、書き手は FIFO の順で、書き手を飢えさせない。名前だけの口を作らないので、読みが多い本物の使い手（今の mutex か spinlock で、読みが書きよりずっと多い物）を探して移す。使い手が見つからなければ建てずに、探した所を報告に書く。
5. 番地の待ち（`SYS_WAIT_ADDRESS` の中身）：物理の番地の hash、bucket ごとの錠、bucket の数は CPU の数から（決め打ちしない）。期限は絶対の ns（ABI の ms は波 3 なので、syscall の入口で ms を絶対の ns に換える）。起こしは bucket の中の該当だけ。events の番地の見張り（今の `address_watch_*`）も同じ bucket に並ぶ。
6. 優先度の継承は 2c（本体が S の口につなぐ）。mutex と番地の待ちに、持ち主を辿れる欄を残しておく。

### 決まり
- 持ち主の単位で消してから建てる。古い名を残す別名・包み・渡すだけの関数を作らない。名前だけの口を作らない。
- 決め打ちの上限を置かない。待ちは割り込みか事象で起こされる形で、決まった間隔の見回りや回数の spin を作らない。
- run queue・級・横取り・`schedule` は S の持ち物で、中身を変えない。起こした thread は §6 の S の口（`sched_make_ready`、名は 2a の報告で確かめる）へ渡す。
- thread への欄は待ちの型一つにまとめ、`struct thread` には一つの欄として入れる（S と O も同じ時に自分の欄を足すので）。
- ABI（`common/abi.h`）は変えない。アプリから見える振舞いを変えない。
- 的は実機の Zen 4（Ryzen 5 7600X）以上。QEMU の都合で設計しない。VM だけの処理は `kernel/src/vm/vm.h` の後ろ。
- Linux・BSD・NT ほか OS のソースを読まない・写さない（patch のメールも）。GPLv2 だけのコードを入れない。
- コードの注記は周りに合わせて英語、今あるコードの書き方（名、注記の濃さ）に合わせる。
- 編集は Edit・Write で。シェルや Python でファイルを書き換えない（この Windows の Python は LF を CRLF に変える）。シェルは Bash（Git Bash）。探すのは `rg`。
- 組んで確かめる：作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`（出力は本体の `build/wt/<作業木の名>/`）。警告を増やさない。QEMU は起こさない。媒体は作らない（本体の会話が取り込んだ後に起動して確かめる）。
- 作業木の枝にコミットする（題は日本語で「段 2b-W：何をしたか」、本文の最後に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。main へ取り込まない。push しない。

### 報告（最後のメッセージ。日本語。本体の会話がそのまま下の「報告」へ写す）
1. 消したもの（ファイル・関数・定数・欄）。
2. 建てたもの：待ち行列の項目と起こしの算法、callback の項目の契約、眠りの期限、mutex の手渡しと回り、読み書きの mutex（と使い手）、番地の待ちの hash。
3. mutex の入れ子：要った所と消した所。
4. 2-design.md §4 から外れた所と、その理由。
5. 自分の持ち物の外に触れた所（ファイルと関数）。S と O の書き手の欄・ファイルと重なりうる所。S の欄を読む所（本体が S の取り込みで合わせる所）。
6. 確かめたこと（ビルドの最後の数行）と、確かめていないこと。
7. 作業木の枝の名と、コミットの一覧。

## 報告（書き手の最後のメッセージ、全文）

## 段 2b-W の報告（書き手 W）

作業木の HEAD は 3c704be8 でした。書き始める前に `git merge --ff-only main` で c4ec89df（0f0ff8a5 の alarm_suspend を含む）へ進め、その上に一つコミットしました。

### 1. 消したもの
- `sys/wait.c`
  - `KILLED_NAP_NS`（kill された thread の待ちを 10 ms ごとに終える見回り）
  - 大域の `static struct wait_queue deadline_wait`
  - `wake_all_held`
  - 先頭に積む thread の鎖（`wq->head` と `t->wait.next`）
  - 列を線形にたどって探す `wq_take_blocked` の中身
  - 全員を起こす mutex（放すと待ち手を全部起こし、取り合わせていた）
- `sys/wait.h` の旧 `struct thread_wait` の欄：`queue`・`next`・`phys`・`seq`
- `sys/address_wait.c`
  - 大域の `address_lock`・`address_blocked`・`address_watches`・`address_watch_tail`・`address_seq`
  - `ADDRESS_BUCKETS 256` と、数を数えるだけの表 `address_waiters`、`address_bucket_index`・`address_bucket`
  - `address_watch_unlink`、`notify_pending`・`notify_next`
  - 待ち×数の全走査
- `fs/store.c`：`gate_close`・`gate_open`。`fs/document.h` の `struct store` の欄 `gate`（spinlock）・`changes`・`closing`・`open`・`drained`。

### 2. 建てたもの
- **待ち行列**（`sys/wait.h`・`wait.c`）
  - `struct wait_item`：二重の繋ぎ、入っている列、thread（callback の項目では NULL）、callback、鍵 `key`、排他の印。
  - `struct wait_queue`：`head`・`tail`・`shared`（排他でない項目の数）・lock。零の記憶が空の列。
  - 起こしの算法（`wake_locked`）：先頭から、排他でない項目は全部、排他の項目は n 個まで起こす。n を使い切り、排他でない項目も残っていなければ止まる。
  - 起こすと項目を列から出す（一度きり）。thread は `item.queue` を NULL（release）にしてから `sched_make_ready`、callback はその場で呼ぶ。
  - 口：`wq_wake(wq, n)`、`wq_wake_all`、`wq_add`・`wq_remove`（起こし手が先に取ったら false）・`wq_wake_item`（どれも列の lock を持って）。`wq_wait_locked` の約束（条件の lock の下で入り、戻ったとき lock は持っていない）は保った。今ある待ちの口の名と形は変えていないので、呼ぶ側は直していない。
- **callback の契約**（wait.h に注記）
  - 呼ぶのは起こし手で、その文脈のまま呼ぶ（割り込みの処理、alarm の fire、先の spinlock を持った thread のどれもありうる）。列の lock を持ち、割り込みを閉じている。
  - してはいけないこと：眠る、利用者のメモリーを写す、mutex を取る、撃ち落とす、待ち行列より先の lock（thread_lock、条件の lock）を取る。
  - してよいこと：後の spinlock を取る、別の列を起こす（その lock はこの列の下に入れ子になる。callback の連なりは持っている列へ戻らない）。
  - 呼ばれた列へ項目を足し引きしない。呼ばれた時から項目は持ち主のもので、起こし手は後で読まない。
  - 持ち主が列の lock の下で `wq_remove` すれば、callback はその間もその後も走らないか、もう走った（false）かのどちらかが分かる。
  - 使い手は番地の見張り（下）だけ。O が二段目で `object_notify` と events をこの上へ移す。
- **thread の欄**：`struct thread_wait` 一つにまとめ、`struct thread` の `wait` の欄に入れた。中身は次のとおり。
  - `item`：待ちの項目
  - `alone`：期限だけで眠る時の自分の列
  - `alarm`、`deadline`、`clock`
  - boot の眠りの一覧の繋ぎ
  - `timed_out`、`killable`、`kill_heard`
  - `mutex`：待っている mutex
- **kill**
  - 公開の待ちは killable。mutex と読み書きの mutex の待ちは kill で終わらない。
  - kill は killable な待ちを一つだけ終える。入っている待ちか、走っている間に来た kill なら次に入る待ちで、その待ちは眠らずに戻る。
  - 仕組み：`kill_heard` を立てる。Dekker の形は旧と同じ（BLOCKED を書いてから killed を読む、kill 側は killed を書いてから state を読む）。
- **起動の thread（CPU 0 の idle）の待ち**：旧は `sti; hlt; cli` の後に `schedule()` だった。今は `schedule()` の後に、まだ列にいれば `cpu_idle()`。旧は割り込みが来なければ、待ち行列にある thread を走らせずに止まりえた。
- **眠りの期限**
  - thread ごとの alarm に絶対の ns を置き、時計は `enum deadline_clock`（monotonic か boot）で選ぶ。
  - 期限が過ぎていれば列に入らず false。alarm は列の lock を放してから立て、起きた後に外す（外すのは `alarm_cancel`）。
  - boot の期限：`boot_lock` の下の一覧に入れ、alarm は monotonic の換算で立てる。換算は boot を先に読むので早くならない。
  - `clock_wall_proof` が眠っていた時（asleep_ns）を足すたびに `wait_boot_clock_moved()` が一覧の全部を立て直す。`clock_boot_ns()` を足した。
  - 入口は `sleep_until`（monotonic）と `sleep_until_boot`。
- **mutex**
  - 欄：`owner`（thread | `MUTEX_WAITERS`）、`depth`、`cpu`（持ち主が取った CPU）、`wait`。
  - 取る：CAS(0→自分)。自分が持っていれば `depth++`。
  - 回る：待ち手の印が無く、`need_resched` も無く、`per_cpu(sched_thread, m->cpu) == 持ち主` の間だけ（回数でなく持ち主の状態で止める）。持ち主が眠るか移れば一致しなくなり、列に並ぶ。
  - 並ぶ：`wait.lock` の下で待ち手の印を立て、排他・killable でない項目で入り、`t->wait.mutex = m`。
  - 放す：CAS(自分→0)。待ち手の印があれば lock の下で先頭の thread へ手渡す（owner を先頭、次があれば印も）。後から来た者は割り込まない。新しい持ち主が `depth = 1` と `cpu` を書く。
  - `mutex_wait` の形と、depth 1 でなければ panic することは旧のまま。
- **読み書きの mutex**（`struct rwmutex`）
  - 欄：`state`（`RWMUTEX_WRITER` | `RWMUTEX_WAITERS` | 読み手の数）、`wait`。読み手は排他でない項目、書き手は排他の項目として、来た順に一つの列に並ぶ。
  - 読み手の速い道は、書き手も待ち手もいない時だけ。待ち手がいれば並ぶので、書き手は飢えない。
  - 最後の読み手か書き手が放す時に lock の下で手渡す：先頭が書き手ならその一人へ、先頭が読み手なら次の書き手までの読み手を全部まとめて（数を先に書いてから起こす）。回らない、kill で終わらない、入れ子にしない。
  - 使い手：格納庫の門。`change_begin`・`change_end` が読み、`store_start` と `commit_locked` の凍結が書き。変更（C: への書き込みのたび）は、数秒に一度の commit よりずっと多い。`dev/blk.c` の門は持ち手が持ったまま読むので合わず、移していない。
- **番地の待ち**（`sys/address_wait.c`）
  - bucket：`struct address_bucket { struct wait_queue queue; struct address_watch *fired; }`（64 byte に揃える）。数は 64×`topology.cpus` を 2 の冪に上げた数（下限 64）で、`address_wait_init()` を boot.c の `thread_init()` の後で確保する。
  - hash は `(phys * 0x9E3779B97F4A7C15) >> (64 - bits)`。
  - 待つ：bucket の lock の下で値を読み、`item.key = phys` として `wq_wait_locked_until`。
  - 起こす：その bucket の中で鍵の合う項目を FIFO で count 個（0 は全部）、thread と見張りを区別せず数える（旧と同じ）。
  - 見張りは callback の項目 `watch_fired`。callback は `object_ref_live` が取れたら bucket の `fired` に積むだけ。起こし手が lock を放してから届ける：見張りの列の lock の下で armed から ready へ移し、`object_notify`・`object_unref`。
  - 消し（`address_watch_remove`）が起こしと重なった時は `removed` を立てて 2 を返し、届け手がその見張りを解く。列の終わり（release）は armed の各々を bucket の lock の下で `wq_remove` して解く。
  - lock の順：events の列の lock → 見張りの列の lock → bucket の lock。
  - 期限は絶対の ns。syscall の入口の換算（`address_deadline`）は今あるものを使い、変えていない。`n_procinfo.wait_phys` は `t->wait.item.key` から読む（番地の待ちの外では 0）。
- **優先度の継承の欄（2c 用）**：mutex は `t->wait.mutex` から `owner` へ辿れる。番地の待ちは `item.key`（語の物理の番地）まで。語の持ち主は核には分からないので、波 3 の ABI で語に持ち主を入れる形が要る。

### 3. mutex の入れ子
`depth` は残しました。要った所：
- `gui/wm.c` の `wm_mutex`：`sys_win_create` の `handle_install` の失敗 → `handle_close` → `win_close` → `win_destroy` → `wm_lock`。また `wm_thread` → `launch` の失敗 → `wm_notify` → `wm_lock`。
- `dev/iommu.c` の `attach_mutex`：`iommu_attach` の失敗 → `iommu_domain_free`。また `iommu_domain_free` → `iommu_block`。
- `fs/store.c` のフォルダーの `l->mutex`：`lock_pair` が同じフォルダーで同じ mutex を二度取る。
- `fs/document.c` の `d->lock`（条件つき）：`store_document_write` → `copy_from_user` → `shared_fault` → `program_fill` → `store_document_read`。`version_read` の放しと取り直しは depth 1 を前提にしていて、潜む危険がある。

消した所は無しです。入れ子の無い mutex は次のとおりで、`mutex_held` の使いは net.c:92 の ASSERT（`net_lock.depth == 1`）だけです。
- net_lock、screenmode、pciuser の四つ、iommu の map、irq の requests
- keep、stream、fbcon
- ahci、nvme、ata、storvsc、vmbus、rdp、pci の reset_mutex、usbnet
- store.moves、store.committing

### 4. 2-design.md §4 から外れた所と理由
- `wq_wake_all` を `wq_wake(n)` と並べて残しました。約 85 の呼ぶ側が使う「全部」の形で、中身は同じ `wake_locked`（n は全部）です。
- 公開の待ちは排他でない項目で入ります。排他の項目を使うのは mutex と読み書きの mutex の中だけで、公開の排他の待ちの口は足していません（使い手が無いため）。`wq_wake(n)` の排他の部分は、O の二段目が最初の使い手と一緒に排他の待ちを足すまで外からの使い手がありません。
- `sleep_until_boot` には今は使い手がありません（時間の ABI は波 3）。今の SYS_SLEEP を boot の時計にすると、アプリから見える振舞いが変わるためです。
- 回るかどうかは、持ち主の `struct sched_entity` を読まず、mutex が持つ「持ち主が取った CPU」と S の `per_cpu(sched_thread, cpu)` を比べて決めます。thread の記録は fpu の領域と合わせて 3584 byte を超えることがあり、vmalloc から来て解かれた後に外されると、回る間に読めば落ちるためです。持ち主が移ると印が古くなり、回らずに並びます（安全側）。
- boot の期限の立て直しは、pm.c の起きた所ではなく `clock_wall_proof` から呼びます。asleep_ns が増えるのはそこだけで、`clock_resume` の最初の RTC の読みと、後で RTC の秒の境を見つけた時の詰め（`edge_found`）の両方を一つの所で拾えます。0f0ff8a5 の `alarm_suspend` があるので、APIC が戻る前の alarm_set は heap で待ちます。
- kill の意味を変えました：10 ms ごとにすべての待ちを終える形から、killable な待ちを一つ終える形へ。調べ（Explore）では、killed を見る呼ぶ側はどれも待ちの前か輪の条件で見ています。見ない呼ぶ側（装置の完了待ちなど）は一度起きて条件まで眠り直し、ring 3 へ戻る時に終わります。
- `change_end` は `rwmutex_unlock_read` を呼ぶだけの一行になりました。格納庫の「変更」の口として fs の複数のファイルが使うので残しました。渡すだけの関数として消すなら、呼ぶ側が門を直に見ることになります。

### 5. 持ち物の外に触れた所と、S・O と重なりうる所
- 触れたファイルと関数
  - `sys/clock.c`：`clock_boot_ns` を足し、`clock_wall_proof` の最後で `wait_boot_clock_moved()` を呼ぶ。include も一行足した。
  - `sys/clock.h`：`clock_boot_ns` の宣言と、`clock_wall_proof` の注記。
  - `boot.c`：`kmain` で `address_wait_init()` を呼ぶ。
  - `fs/store.c`：`change_begin`・`change_end`・`store_start`・`commit_locked`。`gate_close`・`gate_open` は消した。
  - `fs/document.h`：`struct store` の `gate`。
  - `sys/syscall.c`：`sys_proclist` の `wait_phys`。
  - `dev/pci.c`：`reset_wait` の注記。
  - `sys/thread.h`：`killed` の欄と `thread_kill_locked` の注記。
- S の欄を読む・書く所（本体が S の取り込みで合わせる所）。どれも `sys/wait.c` の中です。
  - `sleep_in`：`t->sched.state` に BLOCKED を書く（kill が先に来ていれば RUNNING に戻す）。`t->sched.idle` を読む。`schedule()` を呼ぶ。起動の thread の輪で `cpu_idle()` を呼ぶ。
  - `mutex_spin`：`per_cpu(sched_thread, cpu)`、`need_resched`。
  - `wq_wake_item`：`sched_make_ready`（idle の thread なら RUNNING と `cpu_kick` にする分岐を当てにしている）。
  - T の `thread_kill_locked` が `sched.state` を読んで `wq_take_blocked` を呼ぶ形は変えていない。
- O と重なる所
  - `sys/events.c` は `address_watch_*` を使う（口の名と返す値は同じ）。
  - `address_wait.c` は `object_ref_live`・`object_notify`・`object_unref`・`object_init` と `struct object_ops` を使う。O の一段目（e54783a1）が object の口を変えていれば、ここが衝突しうる。
  - O の二段目は wait.h の callback の項目の契約の上に建つ。
  - `struct thread` の欄は `wait` 一つのまま（型の中身だけ変えた）。

### 6. 確かめたこと、確かめていないこと
- 確かめたこと：作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`。自分の出力（`build/wt/agent-abf27d8be8da8cd90/` の `k/` と `kernel.elf`）を消して全部を組み直し、CC 119 件、警告 0・誤り 0。最後の行は次のとおり。
  ```
    NASM kernel/src/arch/wake.asm
    NASM common/unifont.asm
    LD   ../../../build/wt/agent-abf27d8be8da8cd90/kernel.elf
  ```
- 確かめていないこと：
  - 起動。QEMU は起こしていないし、媒体も作っていない。
  - 実行時の振舞いの全部：mutex の手渡しと回り、読み書きの mutex、番地の待ちと見張りの届け、kill の一度だけの終わり、起動の thread の待ち、boot の期限の S3 をまたぐ立て直し。
  - 試験、WPT、Hyper-V、実機。
- 知っている危険
  - 引き継いだもの：`wq_take_blocked`（期限と kill）は列の指を読んでからその lock を取る。起こし手が取り出した直後に列の持ち主がその記憶を解くと、解かれた記憶に書く窓がある。旧も同じ形で、閉じるには epoch（O）か「列は眠り手が起きるまで生きる」という約束が要る。
  - 調べきっていないもの：`sched_init` より前（`thread_current()` が NULL）に同じ mutex を入れ子で取ると、depth が 1 に上書きされる。そういう所は見つけていないが、全部は調べていない。

### 7. 枝とコミット
- 枝：`worktree-agent-abf27d8be8da8cd90`（main c4ec89df から早送りした上）
- `81d693ed` 段 2b-W：待ち行列を FIFO の項目に、mutex を手渡しに、番地の待ちを CPU の数の bucket に、格納庫の門を読み書きの mutex に
