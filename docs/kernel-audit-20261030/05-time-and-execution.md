# {"agentType": "Plan", "description": "時間と実行（sys）の監査", "toolUseId": "toolu_01JE1Db3qzZK3pMMAJarswyB", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log・git show を読むのはよい）。Linux や他の OS のソースを開かない（設計の比べは公開された設計の知識で）。third_party は読まない。作業木には未コミットの変更があり（git diff --stat で見える）、今のファイルをそのまま監査する。断片でなくファイルを全部読んで判定する。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。この監査の結果で、どの持ち主の単位を消し、何を（あるなら）残すかを決める。直し方（patch）は提案しない（ユーザー：「Please do not suggest "patching" slop or bad architecture」）。

## 中心の問い：時間の全体
ユーザーの言葉：「our timing is still off. the tolerances youve allowed are way too big.」「THIS IS NOT JUST ABOUT SOUND.. THIS IS ABOUT EVERYTHING」「LIKE IVE SAID THE WHOLE TIME」、今日：「"audio issues," are not relevent. its a sytom of a deeper issue with our timing as a whole」。
時間の道を端から端まで追う：時計の源と単調な時刻（どの CPU で読んでも同じか、CPU を移ったとき・S3 の後）、利用者が時刻を読む口、期限の置き方と発火（alarm と LAPIC の deadline）、割り込み→起こし→実行キュー→実際に走るまでの遅れ、横取りの有無と、それが遅れの上限に与えるもの、待つ・起こすの基本の部品と時間切れ、system call の締め切りの単位と変換、事象の配り、期限のある仕事（音・合成）の走りの類があるか。各段で、遅れや誤差の上限が何で決まるか（決め打ちの猶予・丸め・拍・周期・単位の変換）を file:line で示す。

## あなたの範囲
- kernel/src/sys/ の全部：sched.c・sched.h（スケジューラー、task、thread）、clock.c（時計）、alarm.c（期限）、syscall.c（system call の全部）、object.c、channel.c、events.c、stream.c、watch.c、keep.c、prof.c、proc.c（ELF）、kei.c。
- 時間の道をたどるのに要る分：kernel/src/arch/apic.c（timer・IPI）、arch/idt.c と arch/isr.asm（入口と利用者へ戻る道）、arch/spin.h、common/abi.h（時間・待ち・事象・channel・object の ABI）、docs/nocturne-native-abi.md。利用者側の境 user/libc/sys.c の時刻と待ちの関数は、カーネルの口の形を判定するのに要る分だけ読む。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・アプリ・thread・timer・接続・窓の数が増えても成り立つ形（ユーザー：「WHAT IF 20 apps were open at once? … what if the browser had 50 tabs open???」「IF YOUR KERNAL DESGIN IS NOT BETTER THEN LINUX AND NT, THEN DONT EVEN SHOW IT TO ME」）。
- 対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）。CPU の下限は Ryzen 5 7600X（Zen 4、不変の TSC）、上は 2 ソケットの EPYC 9996（論理 CPU 1024、NUMA）。数を決め打ちしない。
- 後ろ向きの互換は要らない。UNIX・POSIX・Linux ではない（ネイティブ ABI）。GUI が先。
- shim（代役・何もしない lock・偽の関数）とスパゲッティ（一つの呼ぶ側のための包み、渡すだけの関数、その場の特例の分岐、決め打ちの値、ある環境の都合を別の部品へ持ち込むこと）は禁止。抽象化の層は責務の本当の継ぎ目に要る。ポーリングと決め打ちの猶予は悪い。

## 判定の物差し
- 時間：上の中心の問い。
- 規模：CPU 1024・thread・timer・待つ者・object・channel が十倍で何が詰まるか。数に比例する走査、一つの大きな lock、一つの CPU や一つの thread に寄せる仕事、決め打ちの上限と表の大きさ、ポーリング。
- 正しさ：起こしの取りこぼし、競争、memory ordering、lock の順、寿命（task・object の解放）、割り込みの文脈で眠ること、仕様との違い（D:\Programes\OS DEV INFO の AMD APM 40332-r4.05.pdf・Intel SDM 325462-093…pdf：TSC・APIC timer の章。要る節だけ）。
- 持ち主：一つの責務が一つの持ち主にあるか。syscall.c に他の部品の責務が入っていないか。分断・重複・stub。
- Linux・NT の公開された設計と軸ごとに比べて劣る所（例えば時計・期限・スケジューラー・待ち・IPC・handle の設計の軸）。

## 報告の形（日本語、平文）
初めに「時間の道」を一つの図か箇条で：段ごとに file:line、その段の誤差・遅れの上限を決めるもの。
次に持ち主の単位（ファイルか部品）ごとに：
1. 責務（一行）
2. 判定：「残せる」「消して建て直す」「消すだけ（要らない）」のどれか一つ。「直せば使える」という中間は書かない。
3. 根拠：file:line と、その行が何をしているか。コードで確かめたものだけ。確かめていないものは「未確認」と書く。
4. 十倍・1024 CPU で何が詰まるか。
5. Linux・NT の公開の設計と比べて劣る軸（あれば）。
6. 建て直すなら、正しい形が満たすべき要件（設計の要件。patch ではない）と、他の部品との継ぎ目。
最後に：この範囲で「残せる」と言えるものとその理由、全体の所感（三行まで）。同じことを繰り返さない。

## reply

時間の道とカーネルの sys の監査です。読むだけで、何も変えていません。

作業木の今のファイルを監査しました。まだコミットしていない変更も含みます。根元は D:\Programes\llm vibe slop os で、下の path はここからの相対です。行番号は今のファイルのものです。

kernel/src/sys の 26 個のファイルは全部読みました。時間の道に入る arch の部品、common/abi.h、user/libc の時計、ports/nvidia/os.c の時計の部分も読みました。

ハードウェアそのものの事実はコードからは決められないので「未確認」と書きました。Zen 4 に TSC-deadline があるか、ARAT があるか、LFENCE が直列化するか、の三つです。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
■ 時間の道（上流から下流へ。各段の書式：場所 → その段の誤差・遅れの上限を決めるもの）
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

1. 刻みの源：sys/clock.c:58-62 tsc_read（lfence; rdtsc）
→ 上限を決めるのは、命令の並びの保証です。AMD で LFENCE を直列化させる MSR（C001_1029）の設定は、核のどこにもありません。Zen 4 で既定のままでも直列になるかは未確認です。

2. 速さを測る：clock.c:164-208
PM timer の 10 ms の窓を 5 回測り、その中央値を取ります。PM timer が無ければ PIT です（177-194）。AMD 向けの CPUID 15h の道はありません。
→ 上限を決めるのは三つです。
・PM timer の量子化：10 ms の窓で 1 刻みが約 28 ppm にあたります。
・I/O port を読む遅れ。
・水晶そのものの偏差：数十 ppm あります。

3. 速さを直す：clock.c:210-237、416-421
起動から 64 秒目に一度だけ測り直します。その時に速さを段で切り替え、以後は凍ったままです。
壁時計を直す道は clock_set_wall（460）だけです。これを呼ぶのは vm/hv/hv_util.c:136 だけです。
→ 測り直しの誤差が一生残ります。実機では壁時計は一度も直りません。水晶の偏差がそのまま壁時計のずれになります。50 ppm なら一日に約 4 秒です。

4. CPU の間のずれ：clock.c:248-343
CPU 同士を比べて offset を測ります。往復の下限を使う考え方は正しいです。核の中の読みは、自分の CPU の offset を引きます（82-86、116）。
→ 上限は、比べの往復の下限です。ただし利用者の道はこの offset を使いません（第 8 段）。

5. 換算：clock.c:64-79（seqlock、mult は 2^32 の固定小数）、conv_set 127-137
精度は ppb の桁で、十分です。
→ tsc_hz だけは seq を閉じた後に書きます（137）。そのため clock_tsc_hz を読む側は、新しい換算と古い速さを組み合わせてしまうことがあります。

6. 壁時計と眠り
・RTC は秒だけを読みます。二度続けて読んで値が合うかを確かめていません（363-384）。
・起動時の壁時計は、RTC の秒そのものです（415）。
・S3 の扱い（427-449）：
  ・単調時計は眠っている間止まります。
  ・CPU ごとの offset は 0 に戻します（443-444）。
  ・壁時計には RTC の秒の差を足します（445-447）。
・眠りの間も数える時計（boottime）がありません。
→ 壁時計は起動時から ±1 s ずれ、眠るたびにさらに ±1 s ずつ積み重なります。

7. VM の混ざり
・clock.c:30、88-97、121-125、389-394：SOURCE_HYPERVISOR が、核の時計の中心にある switch に入っています。
・syscall.c:2274-2293：Hyper-V の時計の頁を利用者の空間に写しています。
・sched.h:50：task の構造体に clock_page の欄があります。
・common/abi.h:273-286：n_reference_tsc が ABI に入っています。
→ ユーザーの前提（実機だけ、VM の論理を混ぜない）と、プロジェクト自身の規則（.claude/rules/kernel.md:22）の両方に反します。

8. 利用者の時計：syscall.c:2902-2920 SYS_CLOCK_INFO（写しを一つ返す）、user/libc/sys.c:85-136
・process ごとに、最初に時計を読んだ時の写しを一つだけ持ちます（93-100）。その後、速さを更新しません。
・CPU ごとの offset を引きません。
・S3 の後は `now < tsc_sample` になり、以後ずっと ms 単位の system call に落ちます（118-136）。
・時計が戻りそうな時は、止めて隠します（85-90）。
・ports/nvidia/os.c:270-312 に、同じ写しを使う二つ目の時計の実装があります。
→ 64 秒目より前に時計を読んだ process は、核の時計からずれ続けます。ずれは「速さの差 × 経過時間」で、上限がありません。そういう process は長く生きるものが多いです。S3 の後は、全 process が 1 ms の分解能になります。

9. ABI の単位
・common/abi.h:53、syscall.c:2845-2847：sleep は ms、uptime は ms、time は秒です。
・syscall.h:11-14：期限は ms で受け、入口で「入口の時刻＋相対」に換えます。
・events の timer は ms です（syscall.c:2809-2816）。
・番地の待ちも ms です（2940-2951）。UINT32_MAX は永久の意味です（syscall.h:16-18）。
・docs/nocturne-native-abi.md §6-8（310-315、400-411）も ms と秒で書いてあります。
・libc も同じです：threads.c:150-171 は ms へ切り上げ、time.c:237-262 は ms の分解能で、壁時計は整数秒です。
→ 誤差の床が 1 ms あります。16.667 ms（60 Hz）や 6.944 ms（144 Hz）の周期は表せません。期限が相対なので、system call の入口までの遅れがそのまま足されます。

10. 期限の器
・sys/alarm.c：CPU ごとの pairing heap です（15-78）。立てた CPU の heap に置きます（107-119）。ここは正しいです。
・眠りの期限だけを待つ task は、大域の deadline_wait 一つに入ります（sched.c:766）。wq_take_blocked は線形の走査です（813-828）。

11. 起こしの装置：arch/apic.c:329-408
・TSC-deadline を使うのは、CPUID.01H:ECX[24] がある時だけです。Zen 4 にあるかは未確認で、無ければ LAPIC の数の one-shot になります。
・LAPIC の速さは、起動時に核の時計で 20 ms 測るだけです（346-355）。第 3 段の 64 秒目の測り直しの後も、直しません。
・WAKE_LATE_SHIFT 13（339、374）：速さをわざと 1/8192 高く取っています。すべての期限が、距離に比例して遅れます。1 s 先の期限なら約 122 µs 遅れます。
・ARAT（CPUID 6 EAX[2]）を確かめていません。
→ 上限は「起動時の測りの誤差 ＋ 固定の 122 ppm ＋ 数の丸め（405-407）」です。

12. 発火
・alarm.c:135-154：割り込みを閉じたまま、期限が来た callback を全部その場で呼びます。
・idt.c:344-348：EOI を先に送ってから処理します。
→ 起こしの仕事（sched_make_ready から choose まで）が、hard IRQ の中で走ります。

13. 起こし
sched.c:831-834 wake_fire → sched_make_ready（394-398）→ choose（306-318、全 CPU を線形に走査）→ IPI（arch/smp.c:112-121）の順です。xAPIC では、送れるようになるまで最大 1e6 回空回りします（apic.c:273-279）。
→ 遅れは CPU の数に比例します。

14. 走るまで
・kei.c:125-136：起きた task は「clock_v ＋ 正の先送り」の位置に置かれるだけです。眠っていた分の貸しはありません。
・retention は 990 µs です（sched.c:655）。全 task が同じ重みです（698）。遅れに敏感な task のための級（実時間・低遅延）はありません。
・核は横取りされません（sched.h:108-111、isr.asm:147-148）。need_resched を見る所は、核全体で約 20 か所だけです。
・idle の輪は、割り込みを閉じて仕事を処理します（sched.c:522-537）。
・cpu_parallel_for は、一つの組みを処理する間ずっと割り込みを閉じます（smp.c:168-204）。使う所は wm.c:1605 の合成と fb.c:401 です。
・spinlock は TAS で、割り込みを閉じたまま回ります（spin.h:22-44）。
→ 遅れの上限は設計の上にありません。足し合わせると「retention × 前に並ぶ task の数 ＋ 核の最長の区間 ＋ 割り込みを閉じた最長の組み」です。

15. 事象の届け
・events.c:232-277：待つたびに、全 timer と全 watch を q->lock の下、割り込みを閉じて走査します。
・object.c:63-72 と channel.c:221-236 は、待っている者を全員起こします。
→ 遅れは watch の数に比例し、群れ起こしになります。

16. 時刻の印
・common/abi.h：
  ・n_event に時刻がありません（236-240）。
  ・gui_event に時刻がありません（505-513）。
  ・EV_FRAME は周期を µs で持つだけで、表示の時刻がありません（528-531）。
  ・n_audio_status に（frame, 時刻）の組がありません（469-478）。
・gui/wm.c:2456-2460 wm_vblank：system call が着いた時の clock_now_ns を vblank の時刻にしています。表示の拍の基準が、装置の driver の thread が起きた遅れになります。VBLANK_LOST_NS は 100 ms です（wm.c:127）。
・核の log の行にも時刻がありません（kernel.md:21）。

→ まとめ
どの段も、誤差・遅れの上限を設計で持っていません。上流の誤差が下流の遅れと足し合わさります。
・上流：ppm 単位でずれたまま直らない速さ、固定の遅れの偏り、ms の量子化。
・下流：横取りの無い核、級の無いスケジューラー、割り込みを閉じた組み、走査と群れ起こし。
さらに事象・表示・音に時刻の印が無いので、受け手は「着いた時」を時刻にするしかありません。誤差は積み重なり、しかも測れません。音の乱れは、この鎖の末端に出た症状です。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
■ 持ち主の単位ごとの判定
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

【sys/clock.c・clock.h】
1. 責務：TSC から、単調時計・壁時計・TSC の期限への換算を作る。

2. 判定：消して建て直す

3. 根拠
・速さは、起動時の 10 ms の窓（164-208）と 64 秒目の一度（210-237）で決めるだけです。その後は直し続けません。
・壁時計を直す道は Hyper-V からだけです（460、hv_util.c:136）。
・RTC は秒だけを読みます（363-384、415）。
・S3 で単調時計が止まり、壁時計に秒を足します（427-449）。
・Hyper-V の源が、中心の switch に入っています（30、88-97、121-125、389-394）。
・clock.h:1-12 は「CPU の間で時計が合う」と約束していますが、利用者の道はこれを守りません（libc sys.c:93-136）。
・tsc_hz を seq の外で書きます（137）。
・clock.h:38-40 の clock_spin を、driver が ms 単位の空回りに使っています（rtl8125.c:547 で 10 ms、ahci.c:180 で 2 ms）。

4. 十倍・1024 CPU で詰まる所
・CPU の比べ（248-343）は、CPU ごとに 256 回の往復を起動時に順に行います。CPU の数に比例して起動が遅れます。
・2 socket の機械で offset が 0 でないと、offset を知らない利用者の時計が、thread が CPU を移るたびに戻ったり進んだりします。
・clock_spin を使う driver は、CPU を ms 単位で占めます。

5. Linux・NT の公開の設計に劣る軸
・速さを直し続けません。Linux は TSC の速さを後から細かく直し、NTP（adjtimex）で mult を少しずつ動かします（slew）。
・時計の種類がありません。Linux は MONOTONIC・BOOTTIME・REALTIME を分けます。NT も interrupt time と、眠りを含む時間を分けます。
・共有の時計の頁がありません。Linux は核が vDSO の頁を更新します。NT は KUSER_SHARED_DATA を通して利用者が QPC を読みます。
・壁時計の規律がありません。
・VM と実機が一つの道に混ざっています。

6. 建て直すなら：要件と継ぎ目
要件：
・源は実機の不変 TSC だけにします（CPUID 8000_0007 EDX[8] を確かめる）。
・速さは CPUID で分かれば CPUID から取ります。分からなければ長い窓で測り、その後も段を作らずに滑らかに直し続けます。
・時計を三つ持ちます：単調、眠りを含む、壁。
・核が書き、全 process が読む時計の頁を持ちます。中身は ns、seqlock で守り、速さが変わるたびに更新します。
・CPU の間の TSC は起動時に確かめ、利用者が offset を要らない形にします。それが無理なら、RDTSCP/RDPID で CPU を知る道を頁に載せます。
・RTC は、更新中かを確かめ、秒の境に合わせて読みます。
・壁時計は、利用者の時刻の service（NTP）からの slew で直します。
・S3 で眠った長さは、眠りを含む時計に足します。
・VM の源は入れません。
継ぎ目：
・clock_now_ns：核の全員が使います。
・clock_tsc_at：起こしの装置が使います。
・時計の頁の形：ABI です。
・suspend/resume の手順。
・壁時計を設定する system call。

【sys/alarm.c・alarm.h】
1. 責務：CPU ごとの期限の heap を持ち、一番早い期限を起こしの装置へ渡す。

2. 判定：残せる

3. 根拠
正しい所：
・CPU ごとの pairing heap です（15-78）。
・期限は ns の絶対値です。
・立てた CPU の heap に置きます（107-119）。
・別の CPU で発火している alarm を取り消す時は、その終わりを待ちます（121-131）。
・発火のたびに時計を読み直します（135-154）。
・眠りの前に、alarm を CPU 0 へ移します（168-186）。
弱い所（契約）：
・発火している間は armed_on が 0 です（90-105）。そのため、同じ alarm を二つの CPU が同時に立てると、二つの heap に入ってしまいます。
・dev/serial.c はこの契約を破っています（確認済み）。86 行は lock を放した後に alarm_set を呼び、drain_fire（89-95）は lock の外で立て直します。この二つは同時に走りえます。
・ほかの呼ぶ側は守っています。printf.c:152 は exchange で一人に絞り、audio.c:326 は audio_lock の下で立て、sched の slice は run queue の lock の下で立てます。

4. 十倍・1024 CPU で詰まる所
・heap が CPU ごとなので、詰まりません。
・callback が hard IRQ の中で重い仕事をすると（起こしから choose の O(N) まで）、その重さが alarm の遅れになります。ただし、これは呼ぶ側の責任です。

5. Linux・NT の公開の設計に劣る軸
・時間の正しさの軸では劣りません。形は Linux の hrtimer と同じです。
・Linux の hrtimer は、timer ごとに base の lock で、同時の start と移しを直列にします。ここは「一人が立てる」約束が header に書かれていません。
・期限の幅（Linux の slack、NT の TolerableDelay）がありません。これは電力の軸で劣ります。

6. 残すなら：条件と継ぎ目
条件：「同じ alarm を立てるのは一度に一人」という契約を、header に書くか alarm 自身が守らせることです。
継ぎ目：
・下：起こしの装置（program 83-87、apic の wake_at）。
・上：alarm_set、alarm_cancel、fire。
建て直す時計（clock_tsc_at）に付け替えるだけで済みます。

【arch/apic.c の起こしの装置（329-408）】
1. 責務：CPU ごとの次の期限を、LAPIC の割り込みにする。

2. 判定：消して建て直す

3. 根拠
・WAKE_LATE_SHIFT 13（339、374）で、固定の遅れの偏りを入れています。
・LAPIC の速さは起動時に 20 ms 測るだけで（346-355）、時計の 64 秒目の直しに追いつきません。
・ARAT を確かめません。
・TSC-deadline は CPUID.01H:ECX[24] の時だけ使います。Zen 4 にあるかは未確認です。
・数を丸めます（405-407）。

4. 十倍・1024 CPU で詰まる所
・CPU ごとの装置なので、数では詰まりません。
・ただし全 CPU が、起動時の一回の測りを共有しています。そのため誤差は、全 CPU に同じ向きで載ります。

5. Linux・NT の公開の設計に劣る軸
・Linux の clockevents は、TSC-deadline を優先します。LAPIC を使う時は較正し、min_delta と max_delta を持ちます。ARAT が無ければ broadcast の装置へ切り替えます。早すぎる発火は hrtimer の側で立て直します。NT の HAL も同じ考えです。
・ここは、許容を定数で埋めています。

6. 建て直すなら：要件と継ぎ目
要件：
・TSC-deadline があれば使います。期限は TSC の値そのもので、偏りを入れません。
・無ければ LAPIC の one-shot を、CPU ごとに TSC に対して測ります。時計の速さが変わるたびに測り直します。
・早すぎる発火は、alarm の側の読み直しで受け止めます。遅れの偏りは置きません。
・ARAT が無い CPU では、深い C-state を許さないか、別の装置（HPET）で起こします。
継ぎ目：wake_at（ns か TSC の値）。

【sys/sched.c・sched.h】
1. 責務：一つの file に七つの持ち主が入っています。run queue、task の作りと終わり、thread、待ち行列、mutex、番地の待ち（futex）、kill と stderr への印字です。

2. 判定：消して建て直す

3. 根拠
容量と均し：
・「どの queue の heap も全 task を持てる」という容量の設計です（63-67）。capacity_reserve は、task を作るたびに全 queue の lock を順に取ります（155-191）。
・busiest は O(N)（266-275）、choose も O(N)（306-318）です。slice_fire は rebalance を IRQ の中で O(N) で行います（332-348）。
・slice の alarm を、起こした側の CPU に立てます。違う CPU なら kick します（323-328、350-356）。
・idle の輪で、割り込みを閉じて cpu_jobs_poll を呼びます（522-537）。
・FPU を毎回 eager に保存・復元します（426-470）。
・全 task が同じ重みで（698）、遅れの級がありません。
task と thread：
・pid を再利用せず、INT32_MAX に達すると task を作れなくなります（691-696）。
・アプリの thread は 128 本までです。数えるのに全 task を走査します（929-933）。
・thread_of_locked は O(T)（970-974）、task_find も O(T)（1256-1260）です。
待ちと mutex：
・眠りの期限は、大域の deadline_wait 一つです（766）。
・待ち行列は先頭に入れる LIFO です（772-808）。起こしは全員起こししかありません（863-873）。wq_take_blocked は線形です（813-828）。
・mutex は全員を起こし、手渡しも優先度の継承もありません（887-918）。
・番地の待ちは、大域の address_wait・address_lock・address_watches の一本です。ADDRESS_BUCKETS 256 は数を数えるだけです（1032-1049）。起こしは「待ち × count」の走査です（1105-1155）。
持ち主の逆転と横取り：
・task_exit が全体の list を走査し、pci・wm・net の後片付けを直に呼びます（1324-1405）。持ち主が逆転しています。
・核は横取りされません（sched.h:108-111）。

4. 十倍・1024 CPU で詰まる所
・task を作るたびに 1024 個の lock を取ります。
・heap の場所が「CPU × task」になります。1024 × 1 万なら 1000 万の場所です。
・起こしと均しが、毎回 1024 CPU の行を読みます。
・futex が、系全体で一つの lock に直列になります。deadline_wait も一つです。
・task_lock の下で O(T) の走査をします。
・thread 128 本の上限は、ブラウザーの 50 タブには足りません。
・mutex の群れ起こしで convoy が起きます。

5. Linux・NT の公開の設計に劣る軸（全軸で劣ります）
・遅れの級：Linux には SCHED_FIFO/RR/DEADLINE があり、NT には 32 の優先度と実時間の級、MMCSS があります。
・起こしでの横取り：Linux には EEVDF の lag と資格があり、NT には起こしの boost と前面の boost があります。
・核の横取り：Linux には PREEMPT と PREEMPT_RT があり、NT の核は PASSIVE_LEVEL で横取りできます。
・均しの規模：Linux は sched domain の階層で均し、LLC の中で idle を探します。
・待ちの規模：Linux の futex は hash と bucket ごとの lock を持ちます。
・優先度の逆転：Linux には rt_mutex の継承と、mutex の楽観の回りと手渡しがあります。

6. 建て直すなら：要件と継ぎ目
要件：
・横取りできる核にします。preempt の数を持ち、spin_unlock と割り込みの出口で横取りします。
・級を二つ持ちます。
  ・実時間/低遅延：合成・入力・音・表示の driver の thread が使います。予算を持たせ、飢えを防ぎます。
  ・公平：起こしで横取りできるものにします。
・均しは topology（SMT・LLC・NUMA）の階層で行い、熱い道に O(CPU) の走査を置きません。
・queue ごとの heap は、自分の task の分だけ持ちます。
・slice の期限は、その queue の CPU に立てます。
・idle の道では、割り込みを閉じて仕事をしません。
・待ち行列は、排他の起こしと FIFO を持ちます。
・futex は番地の hash と bucket ごとの lock で、期限は絶対、PI を持ちます。
・pid/tid は再利用する割り当てにし、上限の定数を置きません。
・task の終わりは、各持ち主が object の close で片付けます。sched が他の持ち主を呼びません。
分ける持ち主：run queue と方針、待ちの部品、futex、task の一生。
継ぎ目：
・sched_make_ready、schedule、wq の API。
・sleep_until（絶対 ns）。
・alarm。
・isr.asm の出口に入れる横取りの呼び（147-152）。

【sys/kei.c・kei.h】
1. 責務：run queue の中の公平な順（仮想時間の heap）と、retention（続けて走らせる長さ）。

2. 判定：消して建て直す

3. 根拠
・起きた task は「clock_v ＋ 正の先送り」に置くだけで、眠りの貸しがありません（125-136）。
・retention は、切り替えの値段から作る固定の許容です（226-236）。KEI_SWITCH_COST_NS は 10000、KEI_OVERHEAD_PPM は 10000 で（sched.c:39-43）、990 µs になります（sched.c:655）。
・重みの仕組みはありますが使われず、全 task が 1 です。
・使われない部品があります。kei.c の外に呼ぶ者がいません。
  ・kei_charge_detached（115-124、BSP の頃の注記つき）
  ・kei_set_bonus（165-171）
  ・kei_choose_cpu（247-262）
  ・kei_validate
  ・kei_set_online
・固定容量の heap の API が、sched の「全 queue に全 task」という設計を強いています。

4. 十倍・1024 CPU で詰まる所
・queue に並ぶ task が 10 倍になると、起きた task が走るまでの時間は約「retention × 前に並ぶ数」に伸びます。ms 単位で増えます。
・容量が固定なので、「CPU × task」の場所を強いられます。

5. Linux・NT の公開の設計に劣る軸
・Linux の EEVDF は、起こしで lag を保ち、資格があれば横取りし、task ごとに slice を要求できます。CFS には眠った task への貸しがありました。NT には起こしの boost があります。
・ここには、遅れに敏感な task を区別する道がありません。

6. 建て直すなら：要件と継ぎ目
要件：建て直す sched の公平の級の中身として作ります。
・起こしで横取りするかの判断を持ちます。
・task ごとに要求（遅れを取るか量を取るか）を持ちます。
・重み（優先度）を持ちます。
・heap は自分の分だけ伸びます。
継ぎ目：sched との間の enqueue、dequeue、pick、tick。

【sys/syscall.c・syscall.h】
1. 責務：system call の入口です。ほかに十余りの持ち主の中身が入っています。利用者の写し、共有メモリー、PAT のピン、pager、program の cache、空間の予約、Hyper-V の時計の頁、GPU・PCI・document・channel・音・事象の取り次ぎです。

2. 判定：消して建て直す（入口の振り分けだけを残す形で）

3. 根拠
3119 行の中に持ち主が十余りあります。
・47-145：写し
・260-1084：共有メモリー
・599-710：PAT のピン
・1085-1482：pager
・1484-1695：program の cache
・1767-1858：共有の頁違反
・1861-2245：空間の予約
・2274-2293：Hyper-V の時計の頁
・2836-3119：振り分け
時間と規模の問題：
・時間の引数がすべて ms か秒です（2845-2847、2809-2816、2940-2951、syscall.h:11-18）。
・SYS_CLOCK_INFO は一度きりの写しを返します（2902-2920）。docs にも書いてありません。
・大域の shared_lock があり（347）、取り消しのたびに shared_all を全部走査します（439-458）。
・report_memory_exhaustion は、task_lock の下で O(P×T) です（152-187）。proclist と sysinfo も、task_lock の下で O(T) です。

4. 十倍・1024 CPU で詰まる所
・共有メモリーの操作が、系全体で shared_lock に直列になります。
・取り消しの重さが、共有の数に比例します。
・メモリー切れの報告が task_lock を長く持つので、その間すべての CPU で task の作りと終わりが止まります。

5. Linux・NT の公開の設計に劣る軸
・Linux は入口の表が薄く、各持ち主が自分の system call を持ちます。時間は timespec の ns、clock id、TIMER_ABSTIME で渡します。
・NT は時間を 100 ns 単位で渡し、負の値が相対、正の値が絶対です。Nt* の呼出しは、それぞれ executive の部品が持ちます。

6. 建て直すなら：要件と継ぎ目
要件：
・入口は、番号から持ち主の関数への表と、写しの部品だけにします。
・時間はすべて ns で、絶対期限と時計の id で渡します。
・共有メモリー、pager、program、空間は mm の持ち主へ移します。
・VM の頁は入れません。
継ぎ目：
・各持ち主の sys_* 関数。
・copy_from_user と copy_to_user。
・isr.asm の syscall_entry。

【sys/events.c・events.h】
1. 責務：事象の行列（handle の状態の見張り、番地、timer）と、その待ち。

2. 判定：消して建て直す

3. 根拠
・handle の最後の close ごとに、大域の watchers_lock を取ります（31。object.c:50 から呼ばれます）。
・timer は並んでいない list です（178-192）。
・events_wait は待つたびに、全 timer と全 watch の ready() を、q->lock の下で割り込みを閉じて走査します（232-277）。
・遅れた繰り返しの timer は、位相を作り直します（252-253）。周期が保てません。
・EVENTS_BATCH は 64 です。
・timer は核の中では ns（events.h:13）なのに、system call は ms で受けます（syscall.c:2809-2816）。
・n_event に時刻がありません。

4. 十倍・1024 CPU で詰まる所
・待つたびに、watch の数に比例する走査を、割り込みを閉じて行います。ブラウザーの 50 タブなら watch は多数になります。
・全 process の close が、一つの lock に直列になります。

5. Linux・NT の公開の設計に劣る軸
・Linux の epoll は、起こしの callback で ready の list に入れるので、走査がありません。edge と level を選べます。timerfd は hrtimer で動きます。
・NT の IOCP は、完了を行列に置き、LIFO の thread で同時に走る数を限ります。

6. 建て直すなら：要件と継ぎ目
要件：
・object の notify が、callback で ready の list に入れます。
・timer は一つずつ alarm にします。期限は絶対 ns、周期も ns で、位相を保ちます。
・lock は行列ごとのものだけにします。
・事象に時刻（単調 ns）を付けます。
・一つの ready に対して一人だけを起こします。
継ぎ目：object の notify の hook、alarm、handle の close。

【sys/object.c・object.h】
1. 責務：kernel object の参照、process ごとの handle の表、変化の知らせ。

2. 判定：消して建て直す

3. 根拠
・object_notify は、object の spinlock の下で全員を起こします（63-72）。
・handle_get のたびに、process の spinlock を取ります（155-162）。
・一番小さい空き番号を再利用し、世代がありません。同じ番号が別の object を指すことがあります。
・権利は writable だけです。
・handle_close が眠りえます（44-57）。

4. 十倍・1024 CPU で詰まる所
・同じ app の多数の thread が、すべての system call で一つの handles_lock を取ります。
・notify が群れ起こしになります。

5. Linux・NT の公開の設計に劣る軸
・Linux の fd 表は RCU で読み、単一 thread の fdget は lock を取りません。
・NT の handle 表は lock を取らずに読め、ACCESS_MASK を持ち、handle の値の再利用を遅らせます。

6. 建て直すなら：要件と継ぎ目
要件：
・読みは lock なし（RCU か epoch）にします。
・handle の値に世代を入れます。
・権利は mask で持ちます。
・notify は、待ち手の callback にします。events と wait を一つの道にします。
継ぎ目：handle_get/put/install、object の ops、notify。

【sys/channel.c・channel.h】
1. 責務：message の通り道と、名前つきの待ち合わせ。

2. 判定：消して建て直す

3. 根拠
・大域の names_lock と、固定 64 個の bucket です（29-31）。
・MESSAGE_QUEUE_LIMIT は 1 MiB の固定です（169）。
・put は全員を起こし、さらに notify します（221-236）。
・送り手は期限なしで待ちます（252-263）。
・受け手には timeout がなく、wait の旗だけです（channel.h:14）。

4. 十倍・1024 CPU で詰まる所
・名前の操作が一つの lock に集まり、bucket も伸びません。
・群れ起こしになります。
・期限のない待ちのせいで、相手が止まるとこちらも止まります。

5. Linux・NT の公開の設計に劣る軸
・NT の ALPC は、port ごとに持ち、message に属性を付けられ、期限を付けて待てます。
・Linux の unix socket は socket ごとの lock を持ち、SO_SNDTIMEO/RCVTIMEO で期限を付けられ、epoll と結べます。

6. 建て直すなら：要件と継ぎ目
要件：
・lock は channel ごとにします。
・名前の空間は、伸びる hash か、セッションごとにします。
・起こすのは一人にします。
・送りも受けも、絶対期限で待てるようにします。
・容量は、受け手が出す信用（credit）で決めます。
継ぎ目：object、events、写しの部品。

【sys/stream.c・stream.h】
1. 責務：console・system log・null の byte stream と、log の C: への保存。

2. 判定：消して建て直す

3. 根拠
・console の ring は 1024 の固定です（123-142）。
・log_keep は、核の thread が C: に書きます（85-116）。
・log の行に、時刻も CPU もありません（kernel.md:21）。時間の問題を log から調べられません。

4. 十倍・1024 CPU で詰まる所
・全 CPU の印字が、一つの ring と lock に直列になり、溢れて落ちます。

5. Linux・NT の公開の設計に劣る軸
・Linux の printk は、lock を取らない ring で、記録ごとに時刻と CPU を持ちます。保存は利用者側の journald がします。
・NT の ETW は、CPU ごとの buffer に QPC の時刻を付けます。

6. 建て直すなら：要件と継ぎ目
要件：
・記録ごとに、単調 ns と CPU と task を付けます。
・ring は CPU ごとにし、lock を取りません。
・保存は利用者の service がします。
継ぎ目：kprintf と klog_write、stream の object。

【sys/watch.c・watch.h】
1. 責務：変化の数を持つ小さな object。取る時は CAS で読みます。

2. 判定：残せる

3. 根拠：CAS の取り（61-79）が正しく、lock も大域の状態もありません。

4. 十倍・1024 CPU：詰まりません。

5. 劣る軸：ありません。Linux の eventfd に相当します。

6. 継ぎ目：object の notify。建て直す object に合わせるだけです。

【sys/keep.c・keep.h】
1. 責務：核が設定の text を読み書きし、古い形から移す。

2. 判定：消すだけ（要らない）

3. 根拠
・古い配置から移す処理があります（older[] 14-16、218-249）。これは後ろ向きの互換で、前提で要りません。
・KEEP_AFTER_NS 500 ms という固定の遅れがあります。
・設定の text の解析が、核の中にあります。

4. 十倍・1024 CPU：規模の問題より前に、核に要りません。

5. 劣る軸
・NT は設定を核の Configuration Manager に持ちますが、汎用の hive と transaction の形です。この形ではありません。
・Linux は、設定を利用者の空間に置きます。

6. 建て直さない
設定は、利用者の service と store に置きます。今 keep を読んでいる核の部品は、起動の引数か、利用者からの system call で値を受けます。

【sys/prof.c・prof.h】
1. 責務：alarm で 1 ms ごとに標本を取り、serial に出す。

2. 判定：消して建て直す

3. 根拠
・SAMPLE_NS 1 ms の、CPU ごとの alarm です。有効な時は 1 kHz の拍になり、測る対象そのものを乱します。
・表は固定で、65536 個と PIDS 64 個です。
・dump は横取りされない kthread の中で、O(65536×400) の仕事をします（157-254）。
・enabled が素の bool で、競合します（262-264）。
・task_name は、task_lock の下で O(T) です（148-155）。

4. 十倍・1024 CPU で詰まる所
・dump の間、serial と task_lock を長く持ちます。
・表が溢れます。

5. Linux・NT の公開の設計に劣る軸
・Linux の perf は、PMU の overflow を NMI で受け、CPU ごとの mmap の ring に書き、利用者が読みます。
・NT の ETW profile も、PMU を使い、時刻を付けます。

6. 建て直すなら：要件と継ぎ目
要件：PMU と NMI で取り、CPU ごとの buffer に時刻付きで書き、読み手は利用者にします。
継ぎ目：NMI の入口、CPU ごとの変数。

【sys/proc.c・proc.h】
1. 責務：program の起動（ELF の読み）、app の記録の object、環境の差し込み。

2. 判定：消して建て直す

3. 根拠
・ET_EXEC しか受けません（193-196）。PIE も ASLR もありません。
・古い tcc の入口を特別扱いしています（223-227）。
・USERFOLDER と TEMP を、核が環境に差し込みます（254-267）。
・proc_app_id を lock なしで読みます（88）。
・PCI を取れるのは init_pid だけです（proc.h:20）。権利の模型が一つの pid です。

4. 十倍・1024 CPU で詰まる所
・時間の道とは関係ありません。
・起動は、syscall.c の中にある大域の program の cache に頼っています。

5. Linux・NT の公開の設計に劣る軸
・Linux は PIE・ASLR・binfmt を持ち、環境は利用者が作ります。
・NT は section object で image を共有し、process を作る仕事の多くを利用者側の部品が持ちます。

6. 建て直すなら：要件と継ぎ目
要件：
・PIE と ASLR を持ちます。
・image の共有は mm の持ち主がします。
・環境は、起動する側が渡します。
・権利は handle の権利で表します。
継ぎ目：object（app の記録）、mm の image、handle の受け渡し。

【時間の ABI（common/abi.h の時間・待ち・事象の部分、docs §6-8）】
1. 責務：利用者に、時間・期限・時刻の印を渡す形。

2. 判定：消して建て直す

3. 根拠
・SYS_SLEEP、UPTIME、TIME（abi.h:53）が ms と秒です。
・n_event と gui_event に時刻がありません（236-240、505-513）。
・EV_FRAME は周期を µs で持つだけです（528-531）。
・n_audio_status に（frame, 時刻）の組がありません（469-478）。
・n_clockinfo と n_reference_tsc（Hyper-V）が ABI に入っています（273-286）。
・n_cpuinfo の注記が古いままです（288-289）。
・SYS_CLOCK_INFO が docs に書いてありません。
・docs は 310-315、400-411 で ms と秒を定めています。

4. 十倍・1024 CPU：規模に関係なく、誤差の床が 1 ms あります。

5. Linux・NT の公開の設計に劣る軸
・Linux：clock_gettime が ns・clock id・vDSO で読めます。input_event に時刻があります。DRM の vblank には時刻と sequence があります。ALSA には htimestamp と audio_tstamp があります。
・NT：QPC、GetMessageTime、DXGI の frame statistics、IAudioClock の位置と QPC の組があります。

6. 建て直すなら：要件と継ぎ目
要件：
・時間はすべて ns で、期限は絶対、時計の id を持ちます。
・時計の頁を持ちます。
・事象・入力・表示・音のすべてに、単調 ns の印を付けます。
・VM の構造は入れません。
継ぎ目：libc の時計、wm の拍、音の位置。

【user/libc の時計（sys.c:85-136、threads.c:150-171、time.c:237-262）と ports/nvidia/os.c:270-312】
1. 責務：利用者の空間で時計を読み、眠る。

2. 判定
・libc の時計：消して建て直す
・nvidia の二つ目の時計：消すだけ

3. 根拠：上の時間の道の第 8 段と第 9 段のとおりです。

4. 十倍・1024 CPU で詰まる所
S3 の後は、全 process が ms の system call で時計を読みます。

5. Linux・NT の公開の設計に劣る軸
vDSO と QPC に劣ります。

6. 建て直すなら：要件
時計の頁を読む関数を一つだけ持ち、丸めをしません。

【arch の中で時間の道に入る部品（時間の目からだけ判定します）】
・arch/spin.h：消して建て直す
  ・TAS で不公平、割り込みを閉じたまま回ります（22-33、40-44）。
  ・1024 CPU では、全員が一つの cache line を叩き、待っている CPU の alarm が遅れます。
  ・Linux の qspinlock（MCS 系で公平）と NT の queued spinlock に劣ります。
  ・要件：列に並ぶ lock で、待ち時間に上限があること。
  ・継ぎ目：spin_lock と spin_unlock の API は、そのまま保てます。
・arch/idt.c：消して建て直す
  ・vector は機械全体で一つの pool で、0x50-0xEF の 160 個しかありません（132-133）。
  ・装置の割り込みは全部 CPU 0 に来ます。仕事はすべて hard IRQ の中で行い、EOI を先に送ります（344-348）。
  ・1024 CPU で多 queue の NVMe や NIC を使うと、CPU 0 が詰まり、160 個の vector では足りません。
  ・Linux は CPU ごとの vector、affinity、CPU ごとの MSI-X、threaded IRQ と softirq/NAPI を持ちます。NT は CPU ごとの IDT と DPC を持ちます。
  ・要件：CPU ごとの vector、NUMA を考えた affinity、短い hard IRQ と、優先度つきの thread での後回しの仕事。
  ・継ぎ目：irq の登録、isr_interrupt。
・arch/isr.asm：残せる
  ・出入りの機構は正しいです。sysret の canonical・TF・RF の確かめ（235-266）、IST での GS の扱い、context_switch（282-303）を確かめました。
  ・核へ戻る時は横取りしません（147-148）。利用者へ戻る時は sched_user_return を呼びます（149-152、229-230）。
  ・offset の静的な確かめがあるのは gdt.c だけです。
  ・横取りは、sched を建て直す時に 147-148 の継ぎ目に入れます。
・arch/smp.c の cpu_parallel_for（168-204）：消すだけ
  ・一つの組みを処理する間ずっと割り込みを閉じ、idle の全 CPU を O(N) で kick し、呼ぶ側は空回りして待ちます。
  ・使う所は wm.c:1605 と fb.c:401 です。fb.c の既定値は、WHPX で測った値で決めています（fb.c:391-393）。
  ・1024 CPU では、参加する全員の割り込みが閉じます。
  ・並列の仕事は、建て直す scheduler の普通の thread でします。
・cpu_kick と cpu_alarm（smp.c:112-121）、xAPIC の送りの空回り（apic.c:273-279）：判定は arch の監査に任せます。時間の目から見て、上の要件以外に問題は見つけていません。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
■ この範囲で「残せる」と言えるものとその理由
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
・sys/alarm.c・alarm.h
  CPU ごとの heap と、絶対 ns の API は、Linux の hrtimer と同じ形です。規模でも詰まりません。新しい時計と起こしの装置へは、継ぎ目（clock_tsc_at と program）で付け替えられます。
  条件は、「一人が立てる」契約を守らせることです。dev/serial.c:86 と 89-95 は、今この契約を破っています。
・sys/watch.c・watch.h
  小さく、lock も大域の状態もなく、正しいです。
・arch/isr.asm
  出入りの機構は正しいです。横取りの呼びを入れる場所（147-148）もはっきりしています。

file としては消すものの、考えとして要件に持っていけるものが二つあります。clock.c の「CPU の比べを下限で測る」考え（248-343）と、seqlock の換算（64-79）です。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
■ 所感（三行）
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
時計の源（不変 TSC）と期限の器（alarm）は正しいです。しかしその間と前後（速さの規律、時計の種類、利用者の時計、ABI の単位、起こしの装置、級も横取りも無い scheduler、走査と群れ起こしの事象）は、どれも上限を設計で持たず、許容の定数で埋めてあります。
そのうえ事象・表示・音のどこにも時刻の印が無いので、誤差は積み重なり、しかも測れません。音や表示のかくつきはこの鎖の末端の症状で、時間の道（clock・起こしの装置・sched/kei・events・ABI・libc）を一つの設計で建て直す必要があります。
VM の時計が核の中心と ABI に入っていることも、前提に反します。

━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
■ 判定の要になる file（絶対 path）
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
- D:\Programes\llm vibe slop os\kernel\src\sys\clock.c
- D:\Programes\llm vibe slop os\kernel\src\arch\apic.c
- D:\Programes\llm vibe slop os\kernel\src\sys\sched.c
- D:\Programes\llm vibe slop os\kernel\src\sys\kei.c
- D:\Programes\llm vibe slop os\common\abi.h
- D:\Programes\llm vibe slop os\user\libc\sys.c
- D:\Programes\llm vibe slop os\kernel\src\sys\events.c
- D:\Programes\llm vibe slop os\kernel\src\sys\alarm.c
