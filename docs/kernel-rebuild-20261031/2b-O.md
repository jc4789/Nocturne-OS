# 段 2b-O：object・handle・events・channel（依頼文と報告）

## 依頼文（一段目）

あなたは Nocturne OS（独自の OS。UNIX でも POSIX でも Linux でもない。ユーザーはその三つを嫌う）のカーネルを建て直す、波 2b の書き手の一人（O）です。git の作業木（worktree）の中で働きます。同じ時に二人が別の作業木で書いています：S（run queue・級・横取り・均し）と W（待ちの部品）。取り込みは本体の会話が S → W → O の順に行います。

あなたの仕事は二つの段に分かれます。この依頼は一段目です。二段目（本体が S と W を取り込んだ後、同じあなたへ続きを頼む）では、main へ載せ直し、`object_notify` と events を W の待ち行列の callback の項目へ移し、静かな点の三行を S の `schedule`・idle・利用者への戻りへ入れます。

### 必ず最初に読むもの
本体の作業木の絶対パスで読んでください。あなたの作業木の同じ名のファイルは古いことがあります。
- `D:\Programes\llm vibe slop os\AGENTS.md`：作業の決まり。特に「製品のコード」。
- `D:\Programes\llm vibe slop os\.claude\rules\kernel.md`：カーネルの約束（handle の表、lock の順、利用者のメモリーの写し）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\00-design.md`：全体の設計。§1〜§3（全員の決まり）。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2-design.md`：波 2 の設計。§0（波 1 の口）、§1、§5（あなたの節：これが契約）、§6（継ぎ目）。§3・§4 は S と W の節で、あなたの形がそれを妨げないことだけ見てください。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\2a.md` の「報告」：2a が作った `struct process`（handle の表の置き場所）と、各持ち主の後片付けを object の close へ移した形。
- `D:\Programes\llm vibe slop os\docs\nocturne-native-abi.md` §1（handle の値）と事象・channel の節。
- 監査 `D:\Programes\時間の道とカーネルの sys の監査.txt` の【sys/events.c・events.h】【sys/object.c・object.h】【sys/channel.c・channel.h】【sys/watch.c・watch.h】【時間の ABI】。行番号は少し前のもので、ずれています。

書き換えるコードは、あなたの作業木の中のもの（2a を取り込んだ main と同じ）です。

### 頼むこと（一段目。2-design.md §5 のうち、W と S に依らない所）
1. epoch の回収（`sys/epoch.c`・`epoch.h`、あなたの持ち物）：口は `epoch_enter`／`epoch_leave`（読む側。横取りを止めるだけ）、後で返す `epoch_defer`、待つ `epoch_wait`、静かな点 `epoch_quiet`。全 CPU が静かな点を一度通った後に、defer された物を返す。CPU の数は topology から、決め打ちしない。静かな点を今は呼ぶ所が無いので、二段目で入る三か所（切り替え・idle・利用者への戻り）を報告に書き、epoch の待ちが止まらないことの根拠（idle の CPU が静かな点を通らずに眠り続ける場合の扱いを含む）を見出しに書く。
2. handle の表：読みは錠を取らない（`epoch_enter` の中で読む）。表の差し替え（伸ばす）と handle の解放は epoch で後に返す。handle の値は表の番号と世代（使い回した番号が古い値で別の object を指さない）。`n_handle` は非負の int32 のまま（ABI §1）。表の大きさに決め打ちの上限を置かない。
3. 権利：mask（読む・書く・待つ・渡す…、要る物を今の使い手から数える）。今の `writable` を置き換える。ABI に権利を見せるのは波 3 なので、syscall の口は今の振舞いのまま、核の中で mask を持つ。
4. channel：channel ごとの錠。名前の空間は伸びる hash（`lib/idtable` か同じ形）。容量は受け手の信用（決め打ちの 1 MiB を消す）。起こすのは一人。送りも受けも絶対期限で待てる核の口（ABI は波 3）。待ちは今の（2a の）口のまま使う。
5. timer の object：一つずつ alarm（`sys/alarm.h`）。期限は絶対 ns、周期は ns で位相を保つ（遅れても周期の格子に戻す）。ABI の ms は波 3 なので、syscall の入口で ns に換える。
6. 事象に時刻（単調 ns）を核の中で持つ（`struct n_event` に欄を足すのは波 3）。
7. `watch.c` は残す。一段目では触れる所を最小にする（二段目で新しい notify に合わせる）。
8. 2a が handle の表に足した番号なしの保持の handle（`handle_hold`・`handle_held`、網の `struct net_owner` が使う）は、新しい表にも持たせる。網のソケットを handle にするのは波 3（ABI）で、その時に保持の handle を消すかを決める。
9. 二段目に回すものとして記録だけする：網の setup の待ち（`net/net.c` の `net_setup_sleep`）は、取り消しの channel が閉じても起こされないので 50 ms ごとに見に行く（決まった間隔の見回り）。二段目で channel の close の notify が待ち手を起こす形にして、この見回りを消す。

### 決まり
- 持ち主の単位で消してから建てる。古い名を残す別名・包み・渡すだけの関数を作らない。名前だけの口を作らない（epoch の静かな点は二段目で呼ばれる。一段目の終わりに呼ぶ所が無いのはこの一つだけ）。
- 決め打ちの上限を置かない。待ちは割り込みか事象で起こされる形で、決まった間隔の見回りや回数の spin を作らない。
- 待ち行列・mutex・番地の待ちは W の持ち物、run queue・`schedule`・idle・利用者への戻りは S の持ち物で、一段目では中身を変えない。events の待ちの形（全 watch の走査）を消すのは二段目（W の callback の項目が要る）。
- thread か process に欄を足すなら、自分の型一つにまとめて一つの欄として入れる（S と W も同じ時に自分の欄を足すので）。
- ABI（`common/abi.h`）は変えない。アプリから見える振舞いを変えない。
- 的は実機の Zen 4（Ryzen 5 7600X）以上。QEMU の都合で設計しない。VM だけの処理は `kernel/src/vm/vm.h` の後ろ。
- Linux・BSD・NT ほか OS のソースを読まない・写さない（patch のメールも）。GPLv2 だけのコードを入れない。公開された論文（epoch・RCU の算法の論文）はよい。
- コードの注記は周りに合わせて英語、今あるコードの書き方（名、注記の濃さ）に合わせる。
- 編集は Edit・Write で。シェルや Python でファイルを書き換えない（この Windows の Python は LF を CRLF に変える）。シェルは Bash（Git Bash）。探すのは `rg`。
- 組んで確かめる：作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`（出力は本体の `build/wt/<作業木の名>/`）。警告を増やさない。QEMU は起こさない。媒体は作らない（本体の会話が取り込んだ後に起動して確かめる）。
- 作業木の枝にコミットする（題は日本語で「段 2b-O：何をしたか」、本文の最後に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。main へ取り込まない。push しない。

### 報告（最後のメッセージ。日本語。本体の会話がそのまま下の「報告」へ写す）
1. 消したもの（ファイル・関数・定数・欄）。
2. 建てたもの：epoch の算法と口、handle の表（読み・伸ばし・世代・権利）、channel（錠・名前の空間・信用）、timer。
3. 二段目に残した所（静かな点の三か所、events の callback への移し、watch.c）と、その時に必要な W・S の口。
4. 2-design.md §5 から外れた所と、その理由。
5. 自分の持ち物の外に触れた所（ファイルと関数）。S と W の書き手の欄・ファイルと重なりうる所。
6. 確かめたこと（ビルドの最後の数行）と、確かめていないこと。
7. 作業木の枝の名と、コミットの一覧。

## 報告（一段目、書き手の最後のメッセージ、全文）

## 報告（段 2b-O 一段目）

一段目の範囲（epoch、handle の表、権利、channel、timer、事象の時刻）を建て、カーネルは警告なしで組めた。コミットは作業木の枝に一つ（e54783a1）。QEMU は起こしていない（依頼どおり）。

先に伝えること：一段目では静かな点を呼ぶ所が無いので、grace period は一度も終わらない。起動の CPU を epoch_init から数に入れた（下の 2）ので、これは安全側に倒れている。ただし後で返す物（閉じた handle の記憶、一つ 64 byte ほど。差し替えた表の配列。終わった app の表の配列）は、二段目まで返らずに溜まる。この一段目だけを main へ載せないでほしい。

### 1. 消したもの
- `sys/object.h`・`object.c`
  - `struct handle` の `bool writable`（`handle_open(o, bool writable)` の引数も）。
  - `struct handle_table` の `INITIAL_HANDLES`（32）、`initial[]`、`capacity`、`hint`、`struct handle **slots`。
  - `table_free_slot`（hint から順に見る線形の探し）。
  - `handle_get_of` の表の錠。
  - `table_grow`・`table_add`・`table_take` の古い中身（名はそのままの static で、新しい形に書き直した）。
  - `handle_table_put` の `size_t index`。
- `sys/channel.c`
  - `CHANNEL_BUCKETS`（64）、`channel_names[]`、`channel_find`、`channel_listened`。
  - listener の `next`・`hash` の欄と、`names_lock` が listener の待ちの列まで守っていた形。
  - `MESSAGE_QUEUE_LIMIT`（決め打ちの 1 MiB）。
  - `message_queue`・`message_channel_send`・`message_channel_receive` の `bool wait`。
- `sys/events.c`・`events.h`
  - 待ちのたびに全 timer の list を見ていた走査と、`next_due`（待ちの期限を一番早い timer で縮める形）。
  - `events_timer(queue, ns, repeat, token)` の形と、timer を足した時の `object_notify`。
- `sys/syscall.c`：`shared_open(p, bool writable, …)` の bool と、`sys_shared_view` の `bool writable`。

### 2. 建てたもの

**epoch（`sys/epoch.c`・`epoch.h`、新しいファイル）**

算法は静かな点で数える回収（QSBR）。CPU は最終段の cache（topology の `llc`）を共にする群に分け、群ごとに数える。

- 口
  - 読む側は `epoch_enter`／`epoch_leave`。横取りを止めるだけで、割り込みの処理の中では使わない（ASSERT で止める）。
  - 後で返すのは `epoch_defer(item, free)`。`struct epoch_item` を返す物に埋め込む。どの文脈から呼んでもよい（run queue の錠を持つ時を除く）。
  - 静かな点は `epoch_quiet(bool idle)`。起動時の用意は `epoch_init()`。
- 持つ状態
  - CPU ごと：`seen`（最後に払った grace period）と `idle`（群の数から外れている）。
  - 群ごと：錠、`active`（idle でない CPU の数）、`owing`（今の grace period をまだ払っていない数）、`gp`、群の CPU の一覧、錠なしで積む defer の列、worker の待ち行列。
  - 全体：`started` と `done`（それぞれ自分の cache line）、`groups_owing`、`started_at`、`forced`、錠、終わりを待つ行列 `ended`。
  - CPU の数は topology から取る。上限の定数は無い。
- grace period の始まり：worker が要る時に `started = done + 1` とし、`groups_owing` を群の数にする。各群は自分の錠の下で `gp` を取り込み、`owing = active` とする。数が 0 の群はその場で払い済み。
- 静かな点
  - 速い道：払う物が無ければ、`started` と自分の CPU の状態を読むだけで戻る。
  - 遅い道（群の錠の下）：`seen != g->gp` なら払う。idle へ入る時は払ってから `active--`。idle から出る時は `active++` と `seen = g->gp` で、今走っている grace period には何も負わない。群の最後の払い手が `groups_owing` を減らし、最後の群が grace period を終える（`done` を進め、`ended` の待ち手を起こす）。
- `epoch_defer`：SEQ_CST の fence の後に `started + 1` を札にし、自分の群の列へ CAS で積む。列が空だった時だけ worker を起こす。
- worker：群ごとに一つ（`epoch%u`、7600X なら一つ）。列を全部取り、`done` 以下の札の物を返す（その間 `need_resched` を見る）。残りがあれば一番小さい札の grace period を始め、`ended` の上で眠る。始まりから `EPOCH_FORCE_NS`（10 ms）経っても終わらなければ、grace period ごとに一度だけ、遅れた群の、idle でなく払っていない CPU を `cpu_kick` で起こす。それ以外は終わりの知らせで起こされるだけで、決まった間隔の見回りや回数の spin は無い。
- 止まらないことの根拠は見出しに書いた。要点は次のとおり。
  - idle の CPU は数から外れ、idle の間は epoch が守る物を読まない（idle の輪も、そこで受ける割り込みの処理も読まない）。
  - 始まっていない CPU と、S3 のために idle の輪から止まった CPU は idle。
  - 起動の CPU だけは epoch_init から数に入る。カーネルの立ち上がりがそこで走るため。ほかの CPU は最初の切り替えで入る。
  - ring 3 で長く計算する CPU は kick で静かな点を通る。

**handle の表（`sys/object.c`・`object.h`）**
- 形：場所の配列 `struct handle_slots {capacity, retire, slot[]}`。各場所は `{handle, generation, next}`。表は `{slots, free_first, free_last, frontier, held, lock}`。
- 読み（錠なし）：`epoch_enter` の中で、`slots` を acquire で読み、番号の場所の `handle` を読む。その後に `generation` を読んで比べ、`handle_ref_live`（参照が 0 でなければ CAS で取る）。
- 書き（表の錠の下）
  - 入れる：空いた場所の FIFO の列（一番長く空いていた場所）から取る。無ければ一度も使っていない場所（`frontier`）、それも無ければ伸ばす。
  - 伸ばす：倍にする。最初は 32。確保は錠を外して行い、写して release で差し替え、古い配列は `epoch_defer`。
  - 取る：`handle = NULL` の後に `generation + 1`（release）、場所を列の後ろへ。
- 番号：下の 23 bit が場所、上の 8 bit が世代で、非負の int32 のまま。最初の世代は 0 なので、起動で渡される handle は今までどおり 0, 1, 2…。
- 閉じた handle の記憶は、最後の close の後に `epoch_defer` で返す。
- `handle_hold`／`handle_held`（番号なしの保持）はそのまま新しい表に持たせた。
- `handle_table_close`：closes_first の種類を先に、残りを次に、保持のものを最後に閉じ、配列は defer。

**権利**
- `uint32_t rights` と `HANDLE_WRITE`、`handle_allows(h, rights)`。
- 今の使い手で調べた権利は書く権利だけ。調べている所：
  - `sys_shared_map`
  - `sys_shared_view`（READ に絞れば bit を落とす）
  - 合成の入力
  - pager の run
  - `shared_scanout_open`
- 読む・待つ・渡すを handle ごとに調べている所は今無い（渡すのは `sys_channel_send` の種類の照らし）。なので bit は作っていない。
- 与え方は今までと同じ：true だった所は `HANDLE_WRITE`、false だった所は 0。

**channel（`sys/channel.c`・`channel.h`）**
- 名前の空間：`lib/idtable` の伸びる表。鍵は FNV-1a 64 の hash。`names_lock` は探す・足す・外す間だけ持ち、伸ばすのは `id_table_room` が錠を外して行う。
- listener は自分の錠を持ち、待ちの列と `open` を守る。二つの錠は同時に持たない。
- connect：`names_lock` の下で listener の object の参照を取り、錠なしで対を作り、listener の錠の下で、まだ開いていれば列へ入れる。
- 対の錠（channel ごとの錠）は前からあり、そのまま。
- 信用：受け手ごとの `credit[2]`。始まりの値は `CHANNEL_CREDIT`（ABI §4 が約束する 1 MiB）。満ちるのは `queued >= credit` の時で、空の列へは大きさに関わらず入る。
- 核の口の送りと受けは絶対の期限で待つ（2a の `wq_wait_locked_until`）：
  - `CLOCK_NEVER` は終わりなし。
  - 過ぎた期限（0）は待たずに `N_ERR_NOT_READY`。
  - 待つ間に来たら `N_ERR_TIMEOUT`。
- syscall からは、送りが `CLOCK_NEVER`、受けが `wait ? CLOCK_NEVER : 0` で、振舞いは前と同じ。`message_channel_post` は期限 0 で、満ちていれば捨てる（前と同じ）。

**timer（`sys/events.c`）**
- 一つずつ自分の alarm を持つ。期限は絶対 ns、周期は ns。
- alarm の割り込みは、行列の錠の下で ready の列（来た順）へ入れ、行列に notify する。
- `events_wait` は ready の列を先に読む（時刻は `due`）。一度きりのものは錠の外で `alarm_cancel` してから解放する。繰り返すものは、今より後の最初の格子の点（`due + k*period`、溢れたら来ない）に立て直す。逃した拍は一度だけ報告し、位相は保つ。
- 繰り返す timer は報告の時にだけ立て直すので、誰も待たない timer は割り込みを起こさない。
- syscall の入口で ms を換える：`ns = ms * 1e6`（溢れたら `CLOCK_NEVER`）。繰り返しで 0 なら `N_ERR_INVALID`（前と同じ）。`due = clock_after(ns)`、`period = repeat ? ns : 0`。

**事象の時刻**
- `struct object_waiter` に `at` を足した。`object_notify` が錠の前に読んだ `clock_now_ns` を、link の時にもその時刻を入れる。
- `events_wait` は核の中で `struct event {token, events, at}` を作る（timer は `due`、番地は行列の `on_addresses.at`、watch は `waiter.at`）。`n_event` へ写すのは token と events だけ（欄を足すのは波 3）。

### 3. 二段目に残した所と、その時に要る W・S の口
- **静かな点の三か所**（今の行は 2a の `sys/sched.c`）
  - (a) `schedule()` の入口。`irq_save` の後、run queue の錠を取る前に `epoch_quiet(false)`。
  - (b) `sched_idle()` の輪の頭。`cli()` の後、`cpu_park_check()` の前に `epoch_quiet(true)`。S3 で止まる CPU は数から外れた後に止まる。
  - (c) `sched_user_return()`。ring 3 へ `return` する直前（割り込みは閉じている）に `epoch_quiet(false)`。
- **S に要ること**
  - 上の三行。
  - 割り込みから ring 3 へ戻る道も (c) を通ること。
  - CPU を止める新しい道を作るなら、その前に `epoch_quiet(true)`。
  - `cpu_kick` → `need_resched` → 切り替えか ring 3 への戻り、という約束を保つこと。
  - 横取りが入れば `preempt_enable` から `schedule()` に入るので、そこも静かな点になる。
  - `EPOCH_FORCE_NS` を S の slice の長さと並べて見直すこと。runnable が 2 以上の時しか slice の期限が立たないので、kick は残す。
  - `sched_idle` の形を変えるなら (b) の置き場所も合わせる。
- **events を W の callback へ**
  - `object_notify` は、登録された待ち行列の callback の項目を呼ぶ形にする。
  - 事象の行列の callback は watch を自分の ready の列へ入れ、待ち手を一人起こす。
  - `events_wait` の全 watch の走査、`object_wait.generation`、`watchers_lock` の順の見直しは二段目。
  - W に要るもの：callback の項目（割り込みからも入る前提と、呼ばれる時に持つ錠の約束）、排他の印、`wq_wake(n)`、番地の見張りを W の bucket から行列の ready の列へ入れる口。
- **起こすのは一人**：channel の受け手と送り手を `wq_wake(…, 1)` にする。close はこれからも全員を起こす。
- **`watch.c`**：一段目は権利の 1 行だけ。二段目で新しい notify に合わせる。
- **網の見回り（記録だけ）**：`net/net.c:116-118` の `net_setup_sleep` は、取り消しの channel の close で起こされないので、`NET_SETUP_LOOK`（50 ms）ごとに見に行っている。二段目で、取り消しの channel の object に W の待ちの項目を登録し、close の notify で起こす形にして、この見回りを消す。`message_close` は今も相手の端に notify している。
- **`epoch_wait`**：最初の使い手（継ぎ目の表では波 3 の mm）が来た時に建てる。

### 4. 2-design.md §5 から外れた所と理由
- **`epoch_wait` を建てていない。** 一段目に呼ぶ所が無く、名前だけの口を禁じる決まりに当たるため。形は「呼んだ時の `started + 1` 以上まで `done` が進むのを `ended` で待つ」になる見込み。
- **`epoch_quiet` は `bool idle` を取る一つの口にした。** 三か所が同じ口を使い、idle の輪だけ true を渡す。
- **「起こすのは一人」は二段目。** 2a の口には `wq_wake_all` しか無く、自分で作れば W の `wq_wake(n)` と重なる。
- **信用の始まりの値は 1 MiB のまま。** 決め打ちの定数は消し、受け手ごとの欄にした。ただし始まりの値はアプリに見える約束（ABI §4 の流量の制御）なので変えていない。受け手が自分の信用を言う口は、ABI が付く波 3 まで作らない（名前だけの口になるため）。
- **権利は `HANDLE_WRITE` の 1 bit だけ。** 今の使い手が調べる権利がそれだけのため。
- **一つの app で同時に開ける handle は 2^23。** int32 の非負から世代の 8 bit を引いた結果で、選んだ上限ではない。前は番号の幅で 2^31 だった。閉じた番号が別の handle を指し直すのは、同じ場所が 256 回空いた後になる。FIFO の列なので、すぐには来ない。
- **timer は handle を持つ object ではなく、事象の行列の中の物のまま。** ABI の `n_events_timer` の形を変えないため。timer ごとの alarm と絶対 ns・格子は入れた。
- **`EPOCH_FORCE_NS` = 10 ms は方針の値で、測っていない。**
- **新しい錠（epoch の錠、群の錠、`names_lock`、listener の錠）には spin の rank を付けていない。** 今 rank を付けている持ち主は無い（`enum spin_rank` は `SPIN_UNRANKED` だけ）。
- **thread・process に欄は足していない。** 読む側は横取りを止めるだけなので、CPU ごとの状態で足りる。
- **handle の番号の値の出方が変わる。** 一番低い空きの番号ではなく、世代が付いて大きくなる。ABI §1 が約束するのは非負の int32 だけで、依頼の求めた形どおり。user/libc と ports に小さな番号を前提にした所は見当たらなかった（静的に読んだだけ。Go は map で持つ）。

### 5. 自分の持ち物の外に触れた所と、S・W との重なり
- **`kernel/src/boot.c`**：`#include "sys/epoch.h"`、`kmain` の `thread_init()` の後に `epoch_init()`。S か 2a がこの辺りを変えると衝突しうる。
- **`kernel/src/sys/syscall.c`**
  - `shared_open` を rights の形に。呼ぶ所は `sys_shared_create`、範囲、DMA、`sys_shared_view`、合成、pager。
  - `shared_scanout_open`、合成の入力、pager の run、`sys_shared_map` の書く権利の照らしを `handle_allows` に。
  - pager の `handle_open`。
  - `sys_events_timer`（ms から ns の時刻へ）。
  - `sys_channel_send`・`sys_channel_receive`（期限）。
  - W が `SYS_WAIT_ADDRESS` の期限を変えるなら、同じファイルの別の関数に当たる。
- **権利の 1 行だけ**（`handle_open` の引数）：
  - `net/net.c`（owner の handle、2545 行あたり）
  - `gui/wm.c`
  - `sys/proc.c`（2 か所）
  - `sys/stream.c`（`stream_open`）
  - `fs/document.c`
  - `dev/audio.c`
  - `dev/pciuser.c`
  - `sys/watch.c`
- **W との重なり**
  - `sys/wait.h` の `wq_wait_unless`・`wq_wait_locked_until`・`wq_wake_all`・`struct wait_queue`（0 で初期化）を channel.c・events.c・epoch.c・object.c で使っている。W がこれらの名や約束を変えれば、この四つで合わせが要る。
  - events.c の番地の見張り（`address_watch_take`・`address_watch_remove`）は今のまま使っている。
- **S との重なり**
  - `epoch.h` は `arch/preempt.h` に依る（S が `preempt_enable` を横取りの点にする）。
  - `cpu_kick`・`need_resched`・`schedule`・`kthread_create` を使っている。
  - 二段目で `sched.c` の三か所に 1 行ずつ足す。

### 6. 確かめたこと・確かめていないこと
- **確かめたこと**
  - 作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`。変えた見出しに `touch` して組み直し、26 ファイルが組まれ、終わりの値 0、warning・error の行は 0。
  - その後に epoch_init を直してもう一度組み、同じく終わりの値 0、警告なし。最後の行：
```
  CC   kernel/src/sys/syscall.c
  CC   kernel/src/sys/thread.c
  CC   kernel/src/sys/watch.c
  LD   ../../../build/wt/agent-ad3b09a25763b0a7a/kernel.elf
```
  - 消した名の残りを `rg` で探した：`INITIAL_HANDLES`・`MESSAGE_QUEUE_LIMIT`・`CHANNEL_BUCKETS`・`channel_find`・`channel_listened`・`next_due`・`table_free_slot`・`->writable` は無し。`vm/rdp.c` の `channel_names` は RDP の欄で無関係。
  - alarm の fire が heap の錠の外で呼ばれることを `alarm.c` で読んで確かめた。行列の錠から alarm の錠へ（`alarm_set`）の順と、fire が行列の錠を取る順は交わらない。
- **確かめていないこと**
  - QEMU での起動・GUI・実サイト（起こさない決まり）。媒体は作っていない。
  - 実行時の epoch。一段目では grace period が終わらない作りで、溜まる量も測っていない。
  - handle の世代の使い回し、channel の信用と期限の道、timer の alarm の道、割り込みの中で行列の錠を取る道。どれも読んで確かめただけ。
  - 実機（Zen 4）。

### 7. 枝とコミット
- 枝：`worktree-agent-ad3b09a25763b0a7a`（main 3c704be8 の上）。
- コミット：`e54783a1 段 2b-O：epoch の回収、錠なしの handle の表（番号と世代・権利の mask）、channel の錠と伸びる名前の表と受け手の信用、timer を一つずつ alarm に`
- main へは取り込んでいない。push もしていない。

## 二段目の依頼（本体の会話から同じ書き手へ、2026-10-31）

段 2b-O の二段目をお願いします。一段目（e54783a1）を読みました。S と W を main に取り込んだので、その上に載せ直して、epoch が実際に回り、events と channel が W の待ちの上で動く形まで建ててください。

### 今の main
- main は dd22aac3（78baf80f が段 2b-S、dd22aac3 が段 2b-W）。報告の全文は docs/kernel-rebuild-20261031/2b-S.md・2b-W.md。
- S：核は横取りされる。切り替えの点は `preempt_enable`（数が 0 に戻るとき）、`irq_restore`（割り込みが開くとき）、割り込みから核へ戻る出口（isr.asm → `sched_preempt_irq`）、`sched_user_return`。旗は CPU ごとの `sched_resched`（arch/cpu.h）で、`need_resched` は無い。切り替えは `sys/sched.c` の `reschedule` → `switch_once`（割り込みを閉じ、錠を持たずに入る）。idle は `sched_idle` の輪（`cli()` → `cpu_park_check()` → 旗なら `schedule()`、でなければ `preempt_disable(); cpu_idle(); preempt_enable();`）。終わった thread は `sched_post_switch` の `thread_left` で、大域の reaper・`thread_init` は無い。kei は無い。
- W：待ち行列は FIFO の項目（thread か callback、排他の印、鍵）。`wq_add`・`wq_remove`・`wq_wake_item`・`wq_wake(n)`・`wq_wake_all`。callback の項目の契約（起こし手の文脈、列の錠の下、割り込みを閉じて、眠らない）は `sys/wait.h`。番地の待ちは物理の番地の hash の bucket（64×CPU の数）で、thread と events の見張り（callback の項目）が同じ FIFO に並ぶ。mutex は手渡し、読み書きの mutex（格納庫の門）。kill は killable な待ちを一つ終える。起動の thread（idle の印を持つ）が待つときは `sleep_in` の輪で `schedule()` の後に `preempt_disable(); cpu_idle(); preempt_enable();`。

### 二段目の仕事
1. 枝を main（dd22aac3）へ載せ直す。S・W と重なる所（channel.c・stream.c・syscall.c・process.c・object・events・watch・net.c など）は、S・W の今の形を土台にして、一段目の設計を入れ直す。一段目の worker の輪にある `need_resched` は消えた名なので、横取りに任せる（核は横取りされる）。
2. 静かな点：報告の 3 に書いた三か所を S の今の形へ。(a) は `schedule()` の入口ではなく `switch_once` の頭（横取りの切り替えも同じ道を通るので、ここで両方を数える）。(b) は `sched_idle` の輪の頭、`cli()` の後、`cpu_park_check()` の前に `epoch_quiet(true)`。(c) は `sched_user_return` の ring 3 へ戻る直前。idle から出る印（`active++`）がどこで立つかを、idle の CPU が割り込みの中で `schedule()` へ入る道（横取りは idle の待ちの中では切り替えない：`sched_idle` は `cpu_idle` を `preempt_disable` で囲む）と合わせて決める。起動の thread の待ちの輪（`sys/wait.c` の `sleep_in`）も CPU を止める道なので、そこをどう数えるか（idle として外すか、kick で払わせるか）を決めて書く。
3. events と `object_notify` を W の callback の項目へ。報告の 3 に書いた形（object が持つ待ち行列に watch の callback の項目を入れ、callback が watch を行列の ready の列へ入れて待ち手を一人起こす）。`events_wait` の全 watch の走査、`object_wait.generation`、`watchers_lock` の順を消すか建て直す。番地の見張りは W の bucket の callback の項目から行列の ready の列へ。`watch.c` も新しい notify に合わせる。
4. channel：受け手を起こすのは要る数だけ（`wq_wake(…, n)`）。
5. 網の setup の見回り（`net/net.c` の `net_setup_sleep`・`NET_SETUP_LOOK` 50 ms）を消す。取り消しの channel の object に W の待ちの項目を登録し、close の notify で待ち手を起こす。
6. 地図（`.claude/rules/map.md`）と `.claude/rules/kernel.md` は本体が直すので触れない。

### 決まり
一段目の「決まり」と同じ（持ち主の単位で消してから建てる、別名・包み・渡すだけの関数を作らない、決め打ちの上限・決まった間隔の見回り・回数の spin を作らない、ABI を変えない、Zen 4 が的、VM だけの処理は vm.h の後ろ、OS のソースを読まない、GPLv2 だけのコードを入れない、注記は周りに合わせて英語、編集は Edit・Write、シェルは Bash と rg）。加えて：
- epoch の読む側の区間は眠らない（横取りを止めるだけ）。callback の項目は割り込みからも呼ばれうる前提で錠の順を書く。
- 組んで確かめる：作業木の根で `python -X utf8 scripts/build.py -j 8 kernel`。警告を増やさない。QEMU は起こさない、媒体は作らない（本体が取り込んで起動する）。
- 作業木の枝にコミット（題は日本語で「段 2b-O：…」、本文の最後に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。main へ取り込まない。push しない。

### 報告（最後のメッセージ。日本語。本体がそのまま下へ写す）
1. 載せ直しで S・W の形に合わせた所。
2. 静かな点の置き場所と、idle の出入り・起動の thread の待ち・S3 で止まる CPU の扱い、grace period が必ず終わる根拠。
3. events・`object_notify`・番地の見張り・`watch.c` の新しい形、錠の順。
4. channel の起こしと、網の見回りを消した形。
5. 消したもの、自分の持ち物の外に触れた所。
6. 確かめたこと（ビルドの最後の数行）と、確かめていないこと。
7. 枝の名とコミットの一覧。

## 報告（二段目、書き手の最後のメッセージ、全文）

段 2b-O 二段目の報告

枝 `worktree-agent-ad3b09a25763b0a7a` に二つのコミットがあります（c3931aa2 の上）。カーネルは警告なしで組めました。QEMU は依頼どおり起こしておらず、動作は未確認です。

### 1. 載せ直しで S・W の形に合わせた所

- 最初に dd22aac3 の上へ載せ直しました（S と W が入った main）。
  - `boot.c`：W の `address_wait_init` の後、S の `cpu_switch_stack` の前で `epoch_init` を呼びます。S が `thread_init` を消したので、それも外しました。
  - `channel.c`：送りは S の形にしました（`COPY_PIECE` を消し、`copy_from_user` を一度だけ）。受け取りは S の一回の `copy_to_user` に、一段目の `wait_over(deadline, waited)` を合わせました。
  - `epoch.c`：worker の輪から `need_resched` を消し、横取りに任せました。
  - `epoch_init` の最後で起動の CPU を数に入れます（`epoch_quiet(false)`）。
- 途中で知らせを受けた c3931aa2（段 2c）の上へも載せ直しました。二段目を書き始める前で、衝突はありませんでした。

### 2. 静かな点、idle の出入り、起動の thread、S3、grace period が必ず終わる根拠

**静かな点（`sys/sched.c`）**
- (a) `switch_once` の頭、`spin_take(&q->lock)` の前：`epoch_quiet(false)`。
  - 横取りの切り替えも、待ちの切り替えも、idle から出る切り替えもここを通ります。
  - `reschedule` は `preempt_count == 0` を確かめているので、読む区間の中ではありません。
  - 実行キューの錠を取る前なので、grace period を終えて worker を起こしてもよい所です。
- (b) `sched_idle` の輪の頭、`cli()` の後、`cpu_park_check()` の前：`epoch_quiet(true)`。
  - `sched_resched` を見る前に置きました。この点が起こした worker がこの CPU に置かれても、見落としません。
- (c) `sched_user_return`：`killed` を見た後、`sched_resched` を見る前に `epoch_quiet(false)`。
  - 旗より先に置いた理由は (b) と同じです。後に置くと、起こした worker を残したまま ring 3 へ戻ります（拍が無いので、次の割り込みまで走りません）。

**idle から出る印（`active++`）**
- idle を出た後の最初の切り替えの頭（a）で付きます。切り替え先の thread が一行も走る前です。
- 走るものが無かった切り替えは、数に入れた後、輪の頭 (b) でまた外します。

**起動の thread の待ち（`sys/wait.c` の `sleep_in`）**
- 数から外す方を選びました。kick で払わせると、grace period ごとに 10 ms 待たせるためです。
- 形は `preempt_disable(); epoch_quiet(true); if (!sched_resched && まだ待ち行列にいる) cpu_idle(); epoch_quiet(false); preempt_enable();` です。
- `epoch_quiet(true)` が自分の CPU へ worker を起こしたときは、`cpu_idle` に入りません。
- `cpu_idle` の間に走るのは割り込みの処理だけで、`epoch_enter` は `!in_interrupt()` を ASSERT しています。

**S3 で止まる CPU**
- `sched_cpu_leave` に kick された CPU は、idle の輪の (b) を通ってから `cpu_park_check` で止まります。数から外れたままです。
- 眠りの後は `sched_cpu_start` から idle の輪で始まり、印は外れたままです（記憶は保たれます）。
- 起動の締め切りに遅れて外された CPU は、halt したまま何も走らせません。CPU ごとの状態は「idle」で始まります。

**grace period が必ず終わる根拠（`epoch.h` に書きました）**
- 数に入っている CPU は、切り替え・ring 3 への戻り・idle の頭のどれかで払います。
- `EPOCH_FORCE_NS`（10 ms）を過ぎても払わない CPU には、`gp_force` が `sched_resched` を立ててから `cpu_kick` します。
  - ring 3 では、kick の割り込みの帰りが `sched_user_return` を通ります。
  - 核の中では、次に横取りが開く所（割り込みの出口、`preempt_enable`、`irq_restore`）で切り替わり、(a) を通ります。
  - 一段目は kick だけでした。`resched_irq` は旗を立てないので、核の中の CPU は切り替わらず払いませんでした。これを直しました。
- 他の CPU の旗を錠なしで立てるのは無害です。消えても、その CPU が自分の切り替えで静かな点を通るためです。
- `gp_force` を回す worker の CPU は、worker へ切り替わったときに払っているので、借りがありません。

### 3. events・`object_notify`・番地の見張り・`watch.c` の新しい形と錠の順

**object**
- `struct object` は `struct wait_queue waiters` を持ちます。`object_notify(o)` は `wq_wake_all(&o->waiters)` だけです。
- `object_wait`・`object_waiter`・その link と unlink、object の錠、`generation` を消しました。
- handle は `watch_lock` を持ちます（葉の錠）。

**events（`sys/events.c` を建て直しました）**
- 源（handle の見張り・timer・番地の見張り）を一つの型 `struct event_source` にし、待ち行列ごとの token の木（`lib/avl`）に持ちます。
  - 鍵は token と tag です（tag は見張る handle か、源そのもの）。watch と unwatch の線形の走査が消えました。
- 源は、変化が言われる所で武装して待ちます。
  - handle の見張り：handle の object の waiters に callback の項目を置きます。
  - 番地の見張り：bucket に置きます。
  - timer：自分の alarm です。
- callback（または alarm）は `source_tell` を呼びます。源を pending の列へ入れ（列の錠 `told`）、待ち行列の object の waiters から一人を `wq_wake(…, 1)` で起こします。
- `events_wait` は pending の列だけを見ます（全 watch の走査を消しました）。
  - 準備のできた handle は報告し、列の後ろへ戻します（レベル）。
  - 準備のできていない handle は武装し、もう一度見ます。武装の後に見るのは、見てから武装するまでの変化を落とさないためです。
  - timer と番地は一度だけ報告します。
  - 64 件で止まって列に残りがあるとき、または最後に見た後に入ったものがあるときは、次の一人を起こします。
- 眠りは、待ち行列の object の waiters の錠の下で「列が空」を確かめてから、新しい `wq_wait_locked_exclusive_until` で眠ります。
- `watchers_lock` は消しました。
  - 見張りは待ち行列の参照を持ちます。
  - handle の最後の close は、自分の列を `watch_lock` の下でまるごと取り、それから各待ち行列の錠を一つずつ取ります。
  - 待ち行列の側は、handle の列から自分が外せた見張りだけを解放します。
  - 待ち行列の後始末は `queue_ops.close` で、源を一つずつ外します。
- `events_watches_free` は `events_handle_closed` に一本化しました。

**番地の見張り（`sys/address_wait.c`）**
- W の `address_watch_queue`（object、armed・ready の列、`fired`、届け、`watch_fired`、`address_watch_add`・`remove`・`take`・`pending`・`queue_new`・release）を消しました。
- 新しい口は `address_word`（前の `address_phys` を公開）、`address_watch_arm`、`address_watch_disarm` です。
- 起こしの callback は bucket の錠の下で直に pending の列へ入れます。`phys_wake` は数えて起こすだけになりました。

**`watch.c`・`watch.h`**：`watch.c` のコードは変えていません。`watch.h` の契約の注記だけを新しい notify に合わせました。

**錠の順（先に取るもの → 後）**
- 待ち行列の錠 → object の waiters の錠、bucket の錠（武装と解除。そこで callback は走りません）。
- 待ち行列の錠 → 見張る handle の `watch_lock`（葉）。
- 待ち行列の錠 → `told`（葉）。
- 待ち行列の錠 → 待ち行列の object の waiters の錠 → 実行キュー。
- 待ち行列の錠 → alarm の錠。
- 待ち行列の錠 → `ops->ready` が取る錠。
- 割り込みからも呼ばれる callback：起こし手が持つ object の waiters の錠、または bucket の錠 → `told`。`told` を放してから、待ち行列の object の waiters の錠（入れ子の起こし）→ 実行キュー。timer の fire も同じです。
- handle の最後の close：`watch_lock` を一人で取り、放してから各待ち行列の錠。
- 待ち行列は待ち行列に入れられないので、callback の連なりが元の錠へ戻ることはありません。

### 4. channel の起こしと、網の見回りの置き換え

**channel**
- 受け手と送り手は排他の待ちです（`wq_wait_locked_exclusive_until`）。
- 手紙を入れると受け手を一人、取ると送り手を一人起こします。
- 待った送り手が入れた後も満杯でなければ、次の送り手を起こします。
- 待った受け手が手紙を残して去るとき（peek、`TOO_LARGE`、悪い buffer）は、次の受け手を起こします。
- 端を閉じると、今までどおり全員を起こします。
- 起こされた者は先に条件を見るので、期限や kill で起こしが失われることはありません。

**網（`net/net.c`）**
- `NET_SETUP_LOOK` と `net_setup_sleep` を消し、`net_sleep(p, deadline, setup)` 一つにしました。
- setup の待ちの順は次のとおりです。
  1. `generation` を読む。
  2. 取り消しの channel の端の object の waiters に、stack の上の callback の項目（`setup_cancel_watch`）を置く。
  3. `net_setup_cancelled()` を見る。
  4. 眠る。
  5. 項目を外す。
- 端の変化（相手の close）で callback が `progress_wake` を呼びます。
- 「setup なら A、そうでなければ B」の呼び分けは、旗一つの呼び出しにまとめました。

### 5. 消したもの、自分のファイルの外に触れた所

**消したもの**
- object：`object_wait`、`object_waiter`、`object_waiter_link`・`unlink`、object の錠、`generation`。
- events：`watchers_lock`、`struct event_watch`・`event_timer`（`event_source` へまとめました）、`events_watches_free`、`events_wait` の全 watch の走査。
- 番地の見張り：上の 3 に挙げた W の仕組み。
- net：`NET_SETUP_LOOK`、`net_setup_sleep`。
- channel：put と take の `wq_wake_all`。

**外に触れた所**
- `sys/sched.c`（S）：静かな点三つと、頭の注記の一段。
- `sys/wait.c`・`wait.h`（W）：起動の thread の待ちを数から外す処理と、公開の排他の待ち。
- `sys/syscall.c`：spare の型と、錠の順の注記一行。
- `gui/wm.c`・`dev/audio.c`：「object の錠」と書いた注記の言葉だけ。
- `net/net.c`。
- `.claude/rules` と地図には触れていません。ただし `kernel.md` 17 行目の「`watchers_lock` → 事象の行列の lock」は古くなったので、本体で直してください。

### 6. 確かめたこと、確かめていないこと

**確かめたこと**
- 作業木の根で `python -X utf8 scripts/build.py -j 8 kernel` を流しました。
  - 作業木のカーネルの目的物（`build/wt/agent-ad3b09a25763b0a7a/k/kernel` と `kernel.elf`）を消して全部を組み直し、CC 95 件で警告と誤りは 0 件でした。
  - 最後の行は `NASM kernel/src/arch/smp.asm` と `LD ../../../build/wt/agent-ad3b09a25763b0a7a/kernel.elf` です。
  - 注記を一行直した後の増分の組みも、`CC kernel/src/sys/channel.c` と `LD …/kernel.elf` で警告なしでした。

**確かめていないこと**
- QEMU（依頼どおり起こしていません）と、起動・デスクトップ・アプリでの動き。
- epoch で実際に grace period が終わり、解放が進むかどうか。
- 事象の待ち・channel・網の取り消しの実際の動き。
- 錠の順は読んで確かめただけです（rank は使われていないので、実行時の照らしもありません）。

**気づいた点**
- 一つの待ち行列を複数の thread が待つとき、変化一つで起きるのは一人になりました（残りは pass-on で）。
  - 既に眠っている thread は、他の thread が受け取ったレベルの handle では起きません。
  - 待ちに入る thread には今までどおり報告されます。
  - 利用者のコードを見た範囲では、待ち行列は thread ごとでした。
- `events_wait` は待ち行列の錠を持ったまま pending の列を歩きます。報告は 64 件までですが、準備のできていない源を見る数に上限はありません（どれも実際の通知一回ぶんです）。
- `handle_remove` の `object_notify` は、待ち行列そのものに当たると、その待ち手を全員起こします。害はありません。
- net の callback は `shared_progress` の眠り手を全員起こします（取り消しの端が変わるときだけです）。

### 7. 枝とコミット

- 枝：`worktree-agent-ad3b09a25763b0a7a`（土台は main c3931aa2）。
- cc5a538b 段 2b-O：一段目（epoch、錠なしの handle の表、channel、timer）。
- 9348cb58 段 2b-O：二段目（静かな点、事象の待ち行列、番地の見張り、channel の起こし、net の取り消し）。
- main へは取り込んでおらず、push もしていません。
