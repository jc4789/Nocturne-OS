# {"agentType": "Explore", "description": "gui・vm の監査", "toolUseId": "toolu_017EYGCL9WUcHJhTKHRegDi3", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log を読むのはよい）。Linux や他の OS のソースを開かない（設計の比べは公開された設計の知識で）。third_party は読まない。作業木には未コミットの変更があり、今のファイルをそのまま読む。断片でなくファイルを全部読んで判定する。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。この監査の結果で、どの持ち主の単位を消し、何を（あるなら）残すかを決める。直し方（patch）は提案しない（ユーザー：「Please do not suggest "patching" slop or bad architecture」）。

## あなたの範囲
- kernel/src/gui/（wm.c・desktop.c・screenmode.c と見出し）：カーネルの中の窓管理と合成、デスクトップ、画面の型の切り替え。これらがカーネルの中にあること自体の是非も、責務と境界の事実として書く（決めるのはユーザー）。common/display_abi.h・common/gpu_abi.h の口と、kernel/src/dev/fb.c・fbcon.c・input.c とのつながりは要る分だけ読む。
- kernel/src/vm/（vm.c・vm.h・hyperv.c・hv/ の VMBus・storvsc・netvsc・hv_util・hv_input・hvsock、rdp.c、virtio_gpu.c、e1000.c・ac97.c・ata.c・tulip.c・vbe.c、devices.h）。
- ユーザーは今日こう言った：「Hyper-V is not wanted. WE TARGET REAL HARDWARE NOT VM'S」。vm/ については、(a) 中身の責務、(b) vm/ の外のカーネルが vm/ の何に頼っているか（include・呼出し・vm_hosted() などの分岐を全部、file:line で）、(c) vm/ を消したとき QEMU での試験に残る道：kernel/src/dev/ と kernel/src/net/ にある実機の装置の driver のうち QEMU も模せる装置はどれか（記憶装置・USB・網・画面・音・入力の各々について、事実として）、を書く。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・メモリー・装置・プログラム・利用者・窓の数が増えても成り立つ形（ユーザー：「WHAT IF 20 apps were open at once? … what if the browser had 50 tabs open???」）。
- 対象は実機だけ。CPU の下限は Ryzen 5 7600X（Zen 4）、上は 2 ソケットの EPYC 9996（論理 CPU 1024、NUMA）。ユーザーの PC の GPU は RTX 4070（NVIDIA の RM をアプリ nvrm として動かす作業がある：user/apps/init.c、ports/nvidia）。画面の解像度と拍は EDID とカーネル・表示経路が決め、起動ローダーで固定しない。
- 後ろ向きの互換は要らない。古い CPU・古い装置・古い ABI のための道は悪い。
- UNIX・POSIX・Linux ではない。GUI が先。ネイティブ ABI は common/abi.h（仕様 docs/nocturne-native-abi.md）。
- shim（代役・何もしない lock・偽の関数）とスパゲッティ（一つの呼ぶ側のための包み、渡すだけの関数、その場の特例の分岐、決め打ちの値、ある環境の都合を別の部品へ持ち込むこと）は禁止。抽象化の層は責務の本当の継ぎ目に要る。

## 判定の物差し
- 規模：窓・アプリ・画面・CPU が十倍で何が詰まるか。数に比例する走査、一つの大きな lock・mutex（特に合成の間に持つもの）、一つの thread や一つの CPU に寄せる仕事、決め打ちの上限と表の大きさ（窓の数・事象の列の長さなど）、ポーリング・周期の見回り・決め打ちの回数の輪。
- 時間：合成・描画の拍、入力の事象の時刻、締め切りの単位と時計の読み方。
- 持ち主：一つの責務が一つの持ち主にあるか。分断・重複。stub・空の実装・固定値を返す関数。
- Linux・NT の公開された設計（合成器・表示の driver の分け方、入力の経路）と軸ごとに比べて劣る所。
- 正しさ：競争、lock の順、寿命、利用者の記憶の扱い。

## 報告の形（日本語、平文）
持ち主の単位（ファイルか部品）ごとに：
1. 責務（一行）
2. 判定：「残せる」「消して建て直す」「消すだけ（要らない）」のどれか一つ。「直せば使える」という中間は書かない。
3. 根拠：file:line と、その行が何をしているか。コードで確かめたものだけ。確かめていないものは「未確認」と書く。
4. 十倍・1024 CPU で何が詰まるか。
5. Linux・NT の公開の設計と比べて劣る軸（あれば）。
6. 建て直すなら、正しい形が満たすべき要件（設計の要件。patch ではない）と、他の部品との継ぎ目。
最後に：この範囲で「残せる」と言えるものとその理由、vm/ を消すときの (b)(c) の一覧、全体の所感（三行まで）。同じことを繰り返さない。

## reply

【監査の範囲と読み方】
次のファイルは全部読みました。
- gui/wm.c、wm.h、desktop.c、screenmode.c、screenmode.h
- dev/fb.c
- vm/ の全部
- そこへつながる呼出し（init.c、clock.c、apic.c、pstate.c、syscall.c、hibernate.c、pm.c、power.c、net.c、audio.c、boot.c）の該当の行

次のものは一部しか読んでいないので、判定はせず、つながりだけを書きます。
- dev/input.c、fbcon.c、pciuser.c
- common/display_abi.h、common/gpu_abi.h

コードは変えていません。

---

【1. gui/wm.c（3362 行）と wm.h（89 行）】

1. 責務：カーネルの中でまとめてやっていること。
   - 窓の表、z 順、焦点、窓ごとの事象の列
   - CPU による合成と表示への出力
   - 入力の振り分け
   - シェル（タスクバー、スタートメニュー、音量、通知、時計、ショートカットキー）
   - クリップボード
   - 遠隔表示（RDP）用の口
   - 試験と測定

2. 判定：消して建て直す

3. 根拠

一つの大域の mutex を、合成の間も持っています。
- wm.c:88-99 の lock の順の注記が、wm_mutex を確保、写像、写し、一コマの描画の間持つと書いています。
- 合成（1865-1907）が lock を放すのは fb_present の前後だけです。
- composite_ram（1757-1809）は wm_lock を持ったまま cpu_parallel_for で並列に描きます。
- 入力の処理も wm_lock の下です（2553-2562）。
- 窓の syscall は全部 wm_lock の下です（3356-3358）。
- この mutex は再帰します（sys/sched.c:887-918、owner と depth）。wm_process_exit（2906-2914）は、その再帰に頼って win_destroy を入れ子で呼びます。

上限が決め打ちです。
- 窓は系全体で 64 まで（MAX_WIN、41 と 2989）。
- 窓ごとの事象の列は 128（42）。満ちると捨てます（362）。キーを離した事象も捨てます。
- 入力の輪は 256（1915）。満ちると捨てます（2004-2006）。
- 傷の矩形は 32（223）。溢れると全部を一つの外接矩形にまとめます（261-280）。
- 遠隔の傷の矩形は 16（175）。

窓の数に比例する走査があります。
- 焦点 302-307、当たり判定 328-335、z の番号 387-391
- 前に出すと配列をずらす 412-424
- 毎回、全部の窓を見る（frames_asked 2488-2492）
- タスクバーで挿入ソート（1214-1282）

窓の面について。
- 窓ごとに必ず二枚の面を vmalloc します（676-698）。
- 面は PTE_SHARED でアプリに写します（701-717）。
- 二枚は ABI で固定されています（common/abi.h:526 WIN_BUFFER_COUNT 2）。

一本の thread がすべてを回します。
- wm_thread（2508-2619）が、入力、合成、時計の分、音量の比べを毎回の輪で回します。

時間について。
- 表示の周期は、分からないとき 60 Hz の代わりの値を使います（117、2470-2485）。
- gui_event に時刻がありません（common/abi.h:505-513）。
- EV_FRAME は「合成の読みが終わった」ことを知らせるだけです。vblank や実際に表示された時ではありません（abi.h:528-531）。渡すのは周期だけです。

シェルの方針がカーネルにあります。
- アプリの path が決め打ちです（800-823、844）。
- launch が @reboot や @sleep を扱います（1017-1030）。
- スタートメニュー 1033-1107、音量の UI 1109-1211、タスクバー 1214-1282、通知 1441-1451
- ショートカットキー 2080-2136
- Desktop.conf 963-1015
- クリップボード 2630-2667（上限 1 MiB、wm.h）

製品の中に試験と測定があります。
- wmbench（1649-1755）は panic します。
- wmverify も panic します（1781、1799）。
- wmtrace 1927-1997、cmdline の旗 2868-2874、fb_benchmark の呼出し 2866

遠隔表示（RDP）の口が合成器に織り込まれています。
- wm_remote_* と wm_pointer_image（2742-2840）。大きさを 640..3840 × 480..2160 に丸めます（2742-2766）。
- 使うのは vm/rdp.c だけです。

VM の都合が入っています。
- 絶対座標の道（2351-2354）の注記は「Hyper-V's synthetic mouse」です。
- ポインターの加速は決め打ちです（2357-2358）。
- wm_init が gpu_init（2845）を呼びます。その唯一の実装は vm/virtio_gpu.c で、vm.h の入口を通りません。

正しさの問題があります。
- SYS_SCREEN_GRAB（3218-3233）は、合成した画面全部（ほかのアプリの窓も含む）を、権限を確かめずに、wm_lock を持ったまま copy_to_user で写します。注記（3227）自身が「sleeping copy」と書いています。dispatch（3321-3362）にも確かめはありません。
- SET_TITLE と MOVE（3160-3181）には、MAP（3050）と RESIZE（3185）にある持ち主（vmm_space）の確かめがありません。handle をほかのアプリへ渡す道があるかは未確認です。
- RESIZE（3182-3217）は wm_lock の下で次をします：新しい面の確保、全行の写し、vmm_user_free（TLB の撃ち落とし）、vfree、写し直し。
- 一枚の面の窓の PRESENT（3139-3158）は、wm_lock の下で memcpy します。

4. 十倍・1024 CPU で詰まる所
- 20 アプリが本体の窓に加えてダイアログやメニューを開くと、64 を超えて窓が作れなくなります。
- 全画面の 4K の窓は、二枚の面で一つ約 66 MB です。64 窓で約 4 GB の vmalloc になります。node を指定する呼び方ではありません（vmalloc の中で node を選ぶかは未確認）。
- 動く窓が増えると、傷の矩形が 32 を超えて、毎コマ全画面を合成します。
- 事象の列が満ちるとキーを離した事象が消え、キーが押されたままになります。
- 合成の間 wm_lock を持つので、次のものが合成一回分ずつ待ちます：窓を作る、動かす、present、クリップボード、画面の問い合わせ、入力。
- cpu_parallel_for（arch/smp.c:129-204）の形：
  - 大域の batch が一つだけです。
  - 呼ぶ側は割り込みを閉じ、暇な CPU 全部へ IPI を送り（187-188）、回って待ちます。
  - 二つ目の呼ぶ側は直列で走ります（174-178）。
  - 1024 CPU では、batch ごとに千近い IPI と待ち合わせが起き、その間 wm_lock を持ち、割り込みも閉じたままです。
- 合成は CPU だけで、表示への写しも CPU のスカラーの写しです（fb.c:145-183）。4K、高い拍、複数画面では、記憶の帯域で詰まります。

5. Linux・NT と比べて劣る軸
- 置き場所：
  - Linux はカーネルに DRM/KMS（モード、plane、vblank、flip）だけを置き、合成と窓の方針は利用者側（Wayland の合成器）が持ちます。
  - NT は win32k（窓と入力の列）をカーネルに持ちますが、Vista からは合成を user mode の DWM が GPU で行います。
  - 両者とも、タスクバー、メニュー、壁紙は利用者側のシェルの過程です。
- 合成：両者とも GPU で合成し、overlay plane を使います。Nocturne は CPU だけです。
- 入力：
  - Linux の evdev は事象ごとに時刻を持ち、加速は利用者側の libinput がします。
  - NT はメッセージに時刻を持ち、入力を thread ごとの列へ配ります。
  - Nocturne の事象には時刻がなく、加速はカーネルの決め打ちです。
- 表示の時刻：
  - KMS の flip の完了の事象は、vblank の時刻と番号を持ちます（Wayland の presentation-time）。
  - DXGI は frame statistics を返します。
  - Nocturne の EV_FRAME は周期だけです。
- 上限：Wayland には窓の決め打ちの上限がありません。NT の上限は過程あたり一万の桁です。Nocturne は系全体で 64 です。
- 面の枚数：swap chain や dma-buf では、アプリが枚数を決めます。Nocturne は 2 枚に固定です。

6. 建て直すときの要件と継ぎ目

要件：
- カーネルに置く場所を決めるのはユーザーです。どこに置くにしても、一番下に要る口は三つです。
  - 表示の出口：scanout、plane、時刻つきの vblank の事象、モード
  - 時刻つきの入力の事象の配り
  - 共有の面（GPU の記憶も含む）の持ち主と寿命
- 窓の方針、シェル、壁紙、クリップボードの方針は、その口の上に乗る別の持ち主にします。
- 合成の間、窓の表や syscall を止める大域の lock を持ちません。合成は、そのコマの変わらない写し（scene）を読みます。
- 窓、事象の列、傷の矩形に、決め打ちの上限を持ちません。事象が溢れても、キーが押されているかどうかといった状態を失いません。
- 時間：
  - 入力は起きた時刻（ns）を持ちます。
  - 表示は vblank の番号と、実際に表示された時刻を返します。
  - 拍は EDID が決めた値から来ます。決め打ちの 60 Hz は持ちません。
- 合成：
  - GPU の道を主にし、CPU は GPU が無いときの道にします。
  - 複数画面と、直の scanout の plane を最初から扱います。
  - 並列にするときは、NUMA の近い少数の働き手を使います。全 CPU への IPI や、割り込みを閉じた待ちはしません。
- 画面を読むこと、ほかのアプリの窓に触れることは、持ち主の確かめと明示の権能を通します。
- 製品に試験、測定、panic の道を入れません。遠隔表示は合成器の中の特別な口にせず、表示の出口の使い手の一つとして同じ口を使います。

継ぎ目：
- dev/fb.c（fb_present）
- dev/pciuser.c
  - wm_repaint 230
  - wm_screen_resized 299、727、730
  - wm_screen_direct 363、753
  - wm_vblank 763
  - wm_screen_state 774
- dev/input.c：input_set_sinks（wm.c:2886）
- dev/fbcon.c：fbcon_set_enabled（wm.c:2884）
- dev/pm.c：wm_window_count、wm_close_windows、wm_notify（81-102、238-329）
- sys/sched.c:1366：wm_process_exit
- sys/syscall.c:3114：SYS_WIN_CREATE..SYS_NET_INFO の番号の範囲を wm_syscall へ渡す
- gui/screenmode.c
- vm/rdp.c
- vm/hv/hv_util.c:136-137：wm_taskbar_changed
- ABI：common/abi.h:505-597、common/display_abi.h

---

【2. gui/desktop.c（754 行）】

1. 責務：壁紙（手続きで描く）、アイコン、ポインターの絵をカーネルの中で描く。

2. 判定：消すだけ（カーネルには要らない）

3. 根拠
- 壁紙 107-201、アイコン 209-377、ポインターの絵 379-754
- static の buffer（391-393、491）
- SN/64 == 2 という前提（498）
- desktop_pointer は絵を一度作って溜めておきます（745-754）。
- 呼ぶのは wm.c だけです。

4. 十倍・1024 CPU：絵の大きさが static の buffer と 498 の前提で決まっているので、画面の倍率（HiDPI）が上がっても伸びません。ポインターは ABI で 32×32 です（abi.h:398、590）。

5. Linux・NT：両者とも、壁紙、アイコン、カーソルの絵は利用者側が持ちます（NT は user32 が .cur を読む、Linux はカーソルの theme）。カーネルにあるのは、ハードウェアのカーソル plane を出す口だけです。

6. 建て直しません。絵はシェルのアプリが持つものです。表示の経路に要るのは、どんな大きさのポインターの像でも plane に出せる口だけです。

---

【3. gui/screenmode.c（445 行）と screenmode.h】

1. 責務：表示の driver（pciuser のアプリ）が EDID から出したモードの一覧を受け取り、使うモードを選び（推奨、利用者の選択、試し）、Displays.conf に残し、syscall で見せる。

2. 判定：消して建て直す

3. 根拠
- 画面は一つだけです。
  - 受けたモードの一覧（offered）を一つしか持ちません（39）。
  - ABI の screen_mode_set.monitor は「unused: 0」です（display_abi.h:95）。
- 利用者の選択は 8 個まで（CHOICES_MAX、35）。溢れると一番古いものを捨てます（104）。
- 拍の代わりの値が決め打ちです（FALLBACK_MHZ 60000、36）。
- 自分の mutex（20）を WM の lock より先に取ります（38、wm.c:3324-3326）。選んだモードは wm の状態を通って driver へ届きます（publish 170-177）。
- 試しを見張る thread を、使うときに作ります（190-209、417-420）。試しの時間は 15 秒です（display_abi.h:64）。
- 遠隔表示の状態が、モードの答えに混ざります（wm_remote_viewing 376、SCREEN_SOURCE_REMOTE 384、display_abi.h:73）。
- Displays.conf：291-365

4. 十倍・1024 CPU：二つ目の画面は、ABI ごと表せません。モードの数は N_SCREEN_MODES_MAX 128 です（abi.h:375）。CPU の数で詰まる所はありません（呼ばれる頻度が低い）。

5. Linux・NT と比べて劣る軸：
- KMS は connector ごとに EDID とモードを持ち、全部の画面の組を atomic に試せます（TEST_ONLY）。
- NT は VidPN で複数画面の組を一度に決め、画面の識別子ごとに設定を残します。15 秒の確かめは利用者側の設定画面が持ちます。
- Nocturne は一つの画面、一つのモードで、組をまとめて試す道がありません。

6. 建て直すときの要件と継ぎ目

要件：
- 画面の識別子（EDID の製造者、製品、serial）ごとに選択を残します。
- 複数の画面の配置を、一度に試して確かめます。
- 方針（選択、試しと戻し）は利用者側が持ちます。表示の経路は「この組を出せるか」の試しと切り替えだけを持ちます。
- 遠隔の viewer は別の出口として扱い、モードの答えに混ぜません。
- 拍を決め打ちしません。

継ぎ目：
- pciuser.c：screen_modes_offer 792、screen_modes_gone 359
- wm.c：wm_screen_mode、wm_remote_viewing
- fb.c：fb_boot_shown（screenmode.c:376）
- sys/keep.c（init.c:106-108）
- display_abi.h

---

【4. dev/fb.c（448 行。装置の監査と重なります）】

1. 責務：表示の出口（起動時の GOP、driver の scanout、同時に映す「also」の scanout）と、合成した面を出す写し、出口を押さえておく hold。

2. 判定：消して建て直す

3. 根拠
- 出口は static の scanouts[3] で、映すのは一つだけです（40-41）。
- present_rows はスカラーの写しで、注記が「WHPX does not emulate SIMD MMIO」です（145-183）。
- present_area の既定値（391-393）は、注記が「Real WHPX fbbench measured 2.4x slower … Keep the known-faster scalar default」です。VM で測った値で実機の既定を決めています。これは .claude/rules/kernel.md:22 が禁じていることです。
- presents_drain が 100 µs ごとに見回ります（208-210）。
- fb_benchmark が製品の中にあり、panic します（414-433）。
- fb_pm_init は vm_display_pm を呼びます（444-448）。注記（435-441）のとおり、実機の画面を眠りから戻す道はありません。

4. 十倍・1024 CPU：映すのは一つだけ、CPU の写しの帯域、100 µs の見回り。

5. Linux・NT：KMS も WDDM も、基本は flip（scanout の先を替える）で写しはせず、完了は vblank で知らせます。

6. 建て直すときの要件と継ぎ目

要件：
- connector、CRTC、plane ごとに持ち主を決めます。
- 基本は flip にし、写すなら GPU か DMA で写します。
- 完了は割り込みで受け、見回りしません。
- 測定は製品の外に置き、VM の測定値を実機の既定に使いません。
- 眠りからの戻りは各 driver の責務にします。

継ぎ目：
- wm.c
- pciuser.c：295、723-730、811、954
- screenmode.c
- vm.c:81-83

---

【5. dev/input.c・fbcon.c とのつながり（全部は読んでいないので判定しません）】
- input.c
  - :9 で vm/vm.h を include
  - :201-207 PS/2 マウスの Y の向きを、cmdline の mouse_y と vm_ps2_flips_y で決める
  - :290 Hyper-V Gen2 の注記
  - :316 input_unicode。呼ぶのは vm/rdp.c:862、884 と vm/hv/hv_input.c:52 だけ
- wm.c:2884-2886：fbcon_set_enabled(false) と input_set_sinks
- init.c:111：WM が無いときに console の sink を使う

---

【6. vm/vm.c（119 行）・vm.h（58 行）】

1. 責務：VM の種類の判定と、カーネルから VM のコードへ入る入口の一覧。

2. 判定：消すだけ

3. 根拠
- 31-46：判定（ACPI の OEM が BOCHS か VBOX か、hv.present）
- 51-54：時計の呼出しを渡すだけの関数
- 57-87：各入口
- 93-97：PS/2 の Y の向きを Hyper-V の build で決める
- 99-106：QEMU（0x604、0xB004）と VirtualBox（0x4004）の電源の port
- 109-119：hibernate、suspend、resume
- vm.h:28：vm_hosted
- vm.h:5 は「一か所の入口」を名乗りますが、守られていません。
  - virtio_gpu.c は dev/gpu.h から直に呼ばれます。
  - rdp.c が使う wm_remote_*、audio_remote_*、input_unicode は、共通のコードの中にあります。

4・5. 実機には要らないので対象外です。

6. 建て直しません。新しいカーネルは hypervisor の判定を持ちません。

---

【7. vm/hyperv.c・hyperv.h】

1. 責務：Hyper-V の CPUID と MSR、LAPIC と TSC の Hz、参照 TSC 頁。

2. 判定：消すだけ

3. 根拠
- 15-33：判定
- 35-43：Hz
- 45-60：参照 TSC 頁。一度確保したら返しません。
- 71-98：参照時刻を読む。88 の注記のとおり、この頁を ABI（n_reference_tsc）で読ませています。

5. Linux も Hyper-V の上では TSC 頁を clocksource の一つとして使いますが、利用者から見えるのはカーネルが配る vDSO の中だけです。ABI の型として libc に入れてはいません。

---

【8. vm/hv/（hv.c・vmbus.c・storvsc.c・netvsc.c・hv_util.c・hv_input.c・hvsock.c）】

1. 責務：hypercall と SynIC、VMBus、Hyper-V の合成の SCSI、網、統合サービス、キーボードとマウス、Hyper-V socket。

2. 判定：消すだけ（全部）

3. 根拠
- hv.c：SynIC を CPU 0 でだけ有効にし、post_message を spinlock の下で送ります。
- vmbus.c：MAX_RELID 2048、要求の mutex は一つ、回って待つ（145-147）。
- storvsc.c：同期の SCSI、制御器ごとに要求は一つ、64 KiB の bounce、64 LUN を探ります。
- netvsc.c：buffer は 1 MiB です。
- hv_util.c:136-137：clock_set_wall と wm_taskbar_changed を呼びます。VM の部品が窓の管理へ手を伸ばしています。
- hv_input.c:119-185：自前の HID の記述子の解析が、dev/usbhid.c の解析と重複しています。
- hvsock.c：MAX_LISTEN 4、PENDING 16。

6. 建て直しません。

---

【9. vm/rdp.c（1784 行）】

1. 責務：Hyper-V の拡張セッションのための RDP の server（hvsock の 3389）。

2. 判定：消すだけ

3. 根拠
- 1-3 の注記：standard RDP security で、暗号はありません。
- 1702-1703：30 ms と 10 ms ごとに見回ります。
- 767-796：wm_remote_damage(16) の後、64 px の tile ごとに wm_remote_copy を呼び、そのたびに大域の wm_lock を取ります。
- 734：ポインターの像を static に持ち、session の間で共有します。
- 802-890：入力。絶対座標の input_mouse、input_unicode。
- 1139-1140、1197：3840×2160 の上限。
- 1235-1461：rdpsnd（audio_remote_*）。
- 1776-1779：session ごとに kthread を一本。
- 共通の部品に形を残しています。
  - ABI のポインターの像の上限 32 の理由に「the remote desktop's」とあります（abi.h:588-590）。
  - SCREEN_SOURCE_REMOTE（display_abi.h:73）
  - wm_remote_*（wm.c:2742-2840）、audio_remote_*（audio.c:395-450）、input_unicode（input.c:316）

6. 建て直しません。遠隔表示が要るかどうかはユーザーが決めます。要るなら、利用者側の過程が表示の出口の使い手として作るものです。

---

【10. vm/virtio_gpu.c（741 行）と dev/gpu.h・common/gpu_abi.h・SYS_GPU_*】

1. 責務：virtio-gpu の virgl で三角形と blit を描く。dev/gpu.h の唯一の実装。

2. 判定：消すだけ

3. 根拠
- 580：VM でなければすぐ戻ります。583：cmdline の gpu-enable が要ります。
- 状態は全体で一つ（busy）で、ほかの呼出しは -N_ERR_BUSY を返します（700、713、725、735）。
- 243-279：2 秒と 10 秒の時間切れで、壊れた扱いにします。
- 316 の注記：この host では fence に約 4 秒かかりました。
- 626-674：起動時の自己試験。
- dev/gpu.h は dev/ にありますが、実装は vm/ にしかありません。wm.c:2845 と syscall.c:2953-3037 が vm.h を通らずに呼びます。実機では SYS_GPU_* に実装がありません。
- gpu_abi.h：
  - 三角形は 32 まで（N_GPU_MAX_TRIANGLES、9）
  - N_GPU_BACKEND_VIRGL（17）
  - CPU の道は「library fallback only」（18）
  - command buffer、提出の列、GPU の記憶の対象、fence がありません。

5. Linux の DRM は GEM と dma-buf で記憶を扱い、command の提出と sync file を持ちます。NT は dxgkrnl が GPU の scheduler と記憶を管理し、UMD が API を訳します。どちらも三角形の数の上限を ABI に持ちません。

6. この形では建て直しません。GPU の ABI は、実機の GPU（RTX 4070、nvrm のアプリ）を前提に、記憶の対象、提出の列、fence、表示と共有できる面を持つ形で、別に決めるものです（決めるのはユーザー）。

---

【11. vm/ のエミュレーター向けの装置の driver（ata.c/h・ac97.c・e1000.c・tulip.c・vbe.c）と devices.h】

1. 責務：エミュレーター（QEMU、VirtualBox、Hyper-V Gen1）だけが出す装置の driver。

2. 判定：消すだけ（全部）

3. 根拠
- ata.c：IDE の PIO を見回りで扱い、最大 200 万回回ります（49-70）。disks[4]。
- tulip.c：
  - 送りで最大 100 万回 pause して待ちます（88）。
  - MAC が決め打ちです（144）。
  - 割り込みは 8259 です（193-194）。
  - PCI の ID は 1011:0009（Hyper-V）と 1011:0019（QEMU）です（114-115）。
- ac97.c：10 ms の記述子が 32 個。割り込みは 8259 か alarm。
- e1000.c：記述子の輪は 256。割り込みの間隔（ITR）を cmdline から取ります。
- vbe.c：Bochs VBE の register を眠りの前後で保存して戻します。
- devices.h：これらを呼ぶのは vm.c だけだと書いてあります。

6. 建て直しません。

---

【この範囲で「残せる」もの】

ありません。理由：
- gui/ は、一つの再帰の mutex、決め打ちの上限、時刻のない事象、画面が一つという前提が、どの部分にも入っています。どこかだけ残しても、それを持ち込むことになります。
- fb.c には、VM の測定で決めた既定値と、画面が一つという前提があります。
- vm/ は実機に要りません。

正しい考えは、コードとしてではなく、上の要件に書きました：EDID のモードの一覧、試して 15 秒で戻す、表示の拍に合わせた窓ごとの EV_FRAME、傷の矩形による部分的な合成。

---

【(b) vm/ の外が vm/ に頼っている所】

vm_hosted() で分けている所は二つだけです：pstate.c:40 と virtio_gpu.c:580。

起動：
- boot.c:6（include）、:128（vm_detect）、:152（clock_init の注記）
- init.c:15（include）、:89（vm_init）、:98（vm_storage_init）、:110（vm_session_init）

CPU：
- arch/apic.c:8（include）、:365-366（LAPIC の拍の源に vm_lapic_hz）、:3-4（注記）
- arch/pstate.c:16（include）、:31、40、44、74、94、105（VM なら P-state と APERF の標本を止める）

時計（ABI と libc まで漏れています）：
- sys/clock.c:22（include）、:43-45、:93、:121（clock_hypervisor_at）、:389-397（時計の源と TSC の拍に vm_clock_ns と vm_tsc_hz）、:433（vm_clock_resume）、:440
- sys/clock.h:7-9、:44-46
- sys/syscall.c:5（include）、:2272-2293（clock_page_of が vm_clock_page をアプリへ写す、2277）、:2908-2910（N_CLOCK_HV_REFERENCE）
- sys/sched.h:50（task の clock_page）
- common/abi.h:273-286（N_CLOCK_HV_REFERENCE、struct n_reference_tsc）
- user/libc/sys.c:91-127（利用者側で参照 TSC 頁を読む）

装置：
- net/net.c:33（include）、:2506（rtl8125 が無いとき vm_net_probe）
- dev/audio.c:15（include）、:769（vm_audio_init）、:395-450（audio_remote_*。使うのは rdp だけ）
- dev/fb.c:12（include）、:446（vm_display_pm）、:146 と :391-393（WHPX の制約と測定で実機の既定を決める）
- dev/fb.h:76（注記）
- dev/hibernate.c:26（include）、:146（vm_hibernate_blocker）、:637（vm_suspend）
- dev/pm.c:23（include）、:220（vm_resume）
- dev/power.c:8（include）、:89（vm_power_off）
- dev/input.c:9（include）、:201-207（vm_ps2_flips_y）、:290（注記）、:316（input_unicode）

GPU：
- dev/gpu.h の全体（実装は vm/ だけ）
- gui/wm.c:17、:2845（gpu_init）
- sys/syscall.c:19、:2953-3037（gpu_get_info、gpu_render、gpu_blit）

遠隔表示：
- gui/wm.c:2742-2840（wm_remote_*、wm_pointer_image）、:2351-2354（注記）
- gui/screenmode.c:376、:384
- common/display_abi.h:73
- common/abi.h:588-590

VM の都合を注記に書いている実機のコード（注記だけ確かめました。中身はこの範囲の外で未確認）：
- arch/cpu.h:16-19（WHPX の命令の解読に合わせた書き方の決まり）
- arch/cpu.c:5-7（WHPX が見せない機能）
- arch/fpu.c:5-6（WHPX の古い 0xD.0 EBX に合わせた xsave の大きさ）
- dev/pciuser.c:634（QEMU の edu 装置で試験）
- 注記だけ：dev/blk.h:1、dev/input.h:42、dev/usbhid.c:3

vm/ の中から外への呼出し：
- hv_util.c:136-137 → clock_set_wall、wm_taskbar_changed
- rdp.c → wm_clipboard_*、input_key_set1、input_mouse、audio_remote_*
- ac97.c → audio_set_card
- e1000.c → net_rx_notify

---

【(c) vm/ を消した後、QEMU で試せる実機の driver】

記憶装置：
- dev/ahci.c：q35 に内蔵の ICH9 AHCI、または -device ahci
- dev/nvme.c：-device nvme
- dev/usbmsc.c：Bulk-Only（usbmsc.c:48）で、QEMU の usb-storage
- ata.c（IDE）は消えます。今の QEMU の起動の機械の型は未確認です。

USB：
- dev/xhci.c：qemu-xhci
- dev/usbhub.c：QEMU の usb-hub と結べるかは未確認です。

網：
- net/rtl8125.c は QEMU が模しません。
- dev/usbnet.c（CDC ECM と NCM、usbnet.c:61）は、QEMU の usb-net が出す CDC ECM の構成に、コードの上では結びます。xhci.c:962-987 が、driver の取れる最初の構成を選ぶからです。実際に動くかは未確認です。

画面：
- dev/fb.c の起動時の GOP の framebuffer だけです。
- EDID、モードの切り替え、vblank、直の scanout は、pciuser の表示の driver（実機では nvrm）が要るので、QEMU では試せません。

音：
- ありません。audio.c:769 のとおり実機の音の driver が無く、QEMU が模す intel-hda に対する driver も無いので、QEMU でも実機でも試せません。

入力：
- dev/usbhid.c：usb-kbd、usb-mouse、usb-tablet。絶対座標も扱います（usbhid.c:181）。
- dev/input.c の i8042 PS/2

時計（試験に響くので書いておきます）：
- vm/ が無いと、clock.c は TSC を CPUID 15h、PM timer、PIT で測ります（396-407）。
- WHPX は invariant TSC の bit を見せません（cpu.c:5 の注記）。
- QEMU でこの道が成り立つかは未確認です。

---

【全体の所感】
- gui/ は、一つのカーネルの mutex の下に、窓の表、CPU の合成、入力、シェルの方針、遠隔表示、測定をまとめた形で、規模と時間のどの物差しにも耐えません。
- vm/ は「一か所の入口」を名乗りながら、VM の都合が次の所まで入り込んでいます：時計（ABI と libc まで）、LAPIC、P-state、電源、眠り、入力、画面の既定、GPU の ABI。
- この範囲で残すコードはありません。vm/ を消すと、QEMU でも音は試せず、網（usb-net 頼み）は動くか未確認になります。画面は起動時の GOP の framebuffer だけ映り、EDID とモードの切り替えは試せません。
