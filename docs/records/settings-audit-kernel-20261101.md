# 設定の監査：カーネル・common・boot（2026-11-01）

## 依頼文

あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、カーネルと libc は C の独自の OS、対象は実機）の敵対的な監査者です。読むだけで、製品のコードを変えません。ビルドも QEMU も起動しません。

### 持ち主の言葉（原文）

"This IS NORMAL. WE LACK ANY TYPE OF REAL settings SUPPORT. AND its ALL HARD CODED SLOP THAT IS FUNDAMENTALLY WRONG. THIS IS FOR THE ENTIRE OS. BEFORE ANY MORE FEATURES ARE ADDED. THIS NEEDS TO GET FIXED. OR YOU WILL KEEP WRITING HARDCODED spaghetti code. EVERYTHING IS NOT A FILE. THAT IS STUPID AND WRONG. THERE ARE BOUNDARIES. THE UI. THE SYSTEM PROCESSES. KERNEL. THERE ARE NOT FILES TO EDIT. THERE ARE SETTINGS TO CHANGE AND APPLY. TREAT THIS AS adversarial FIXING OF misconceptions and mistakes in fundamental design and architecture. DO NOT BLUR BOUNDARIES. MAKE SURE THE RIGHT THINGS HAVE THE RIGHT OWNERSHIP. DO NOT TREAT THE CURRENT STATE OF THE OS OR THE DOCS OR YOUR MEMORIES AS ACCEPTABLE." "THIS IS NOT JUST FOR AUDIO BUT FOR EVERYTHING." "OUR OS IS FUNDAMENTALLY FLAWED."

持ち主が見せた「普通」の例は Windows のサウンドの設定です。装置ごとに、装置が受ける 16・24・32 bit と 44100・48000・88200・96000 Hz の組が一覧で出て、利用者が既定の形式を選び、OK で適用します。これは一例で、対象は OS の全領域です。

### あなたの範囲

`kernel/src/` の全部（arch、mm、sys、fs、net、dev、gui、vm、lib）、`common/`、`boot/`。地図は `.claude/rules/map.md`。ファイルを探すのは codegraph（MCP の `codegraph_explore`）と rg。

### 調べること

カーネルの全領域を回って、次のものを一つずつ挙げます。音だけに偏らず、CPU・スケジューラー・メモリー・記憶・ファイルの層・網・入力・画面・GPU・電源・USB・PCI・IOMMU・時計・ログを全部回ります。

1. 決め打ちの値と決め打ちの方針。装置に問えば分かる値、利用者か管理者が選ぶべき値、起動の引数や旗でしか変えられない値、固定の上限、固定の優先順位、固定の表。
2. カーネルが持っている方針。機構でなく方針（何を選ぶか、どの順か、既定は何か）をカーネルの中で決めている所。
3. 装置の能力を読んでいるのに捨てている所、一つの形だけに丸めている所。
4. 変えても効かない、または再起動しないと効かない値。

正当なもの（仕様が定めた定数、ハードウェアのレジスターの番号）は挙げません。挙げるか迷ったら、仕様の原文を `D:\Programes\OS DEV INFO` で確かめます。他の OS のソースを読みません。

### 報告

全文を `docs/records/settings-audit-kernel-20261101.md` に日本語で書きます。頭にこの依頼文を写します。項目ごとに：`ファイル:行`、今の値と振る舞い、なぜ誤りか、本来の持ち主（カーネルの機構・システムのプロセス・UI・装置の申告・利用者の選択のどれか）。領域ごとに節を分け、最後に重い順の上位 20 件を書きます。今の形を弁護しません。書きぶりは説明書のように、一文に一つのことを書きます。実際に読んだコードだけを書き、推測を書きません。

返事は、報告のパスと上位 20 件の一行ずつだけにします。

## 読み方

- 各項目の「分類」は依頼文の 1〜4 の番号です。
- 「持ち主」は依頼文の五つの語で書きます。
- 行番号は監査の時点の main（`7de8c35f`）のものです。

## 読んだ範囲と限界

- 全文を読んだもの：`boot/limine.conf`、`kernel/src/boot.c`、`kernel/src/init.c`、`sys/keep.c`・`keep.h`、`dev/input.c`・`input.h`、`dev/audio.h`、`dev/hda_codec.h`、`gui/screenmode.h`、`common/display_abi.h`、`common/gpu_abi.h`、`docs/nocturne-native-abi.md` の §12。
- 大部分を読んだもの：`arch/idle.c`、`gui/screenmode.c`。
- 残りは、定数・起動の引数・方針の語を rg で全部引き、当たった所の前後を読みました。`kernel/src` の 69,066 行を一行ずつは読んでいません。
- 「無い」と書いたものは、そのファイルと語を rg で引いて当たらなかったものです。
- 仕様書を開いて確かめた項目はありません。仕様の定数と分かるレジスターの番号・書式の定数は挙げていません。理由の節の最後に、挙げなかった群を書きます。

---

## 0. 設定の形そのもの

### S-01 設定という object が無い

- 場所：`common/abi.h:50-150`（syscall の一覧）、`common/display_abi.h:23-30`・`42-58`、`common/abi.h:579-590`（`n_audio_output`）
- 分類：1・2
- 今：設定を読む呼出しと変える呼出しは領域ごとに別の形です。
- 今：画面は `SYS_SCREEN_INFO` の第 3 引数の op に相乗りします。
- 今：音は `SYS_AUDIO_OUTPUT` の `set` のビットです。
- 今：網、時計、入力、CPU、電源には変える呼出し自体がありません。
- 誤り：設定を列挙する口、値の型と範囲を問う口、適用する口がありません。
- 誤り：領域が増えるたびに syscall の形が一つ増えます。
- 誤り：`SYS_SCREEN_INFO` という「情報」の呼出しが書き込みと試行と見張りを兼ねています。
- 持ち主：カーネルの機構は値を受ける口だけを持ちます。設定の一覧と型はシステムのプロセスが持ちます。

### S-02 適用と取り消しの手順が画面のモードにしか無い

- 場所：`common/display_abi.h:88-100`、`kernel/src/gui/screenmode.c:488-520`
- 分類：2
- 今：試す・保つ・戻すの三段（TRY・KEEP・BACK）は画面のモードだけにあります。
- 今：音量、ポインターの速さ、なめらかな階調は即時に当たるだけです。
- 誤り：Windows のサウンドの例の「選んで OK で適用」にあたる手順が OS の共通の形になっていません。
- 持ち主：システムのプロセス（手順）と UI（OK の押し場所）。

### S-03 設定の実体がカーネルの解析するテキストの文書である

- 場所：`kernel/src/sys/keep.h:1-11`、`kernel/src/sys/keep.c:96-100`（`part_path`）、`131-143`（`each_line`）、`194-197`（`own_line`）
- 分類：2・4
- 今：カーネルが `AppData\Nocturne\<name>.conf` を開き、`key=value` の行を解析します。
- 誤り：持ち主の言う「THERE ARE NOT FILES TO EDIT」に真っ向から反します。
- 誤り：カーネルがテキストの解析器を持ちます。
- 誤り：カーネルの部品が保存の書式を一つずつ持ちます（`sound_line`・`desktop_line`・`displays_line`）。
- 持ち主：保存はシステムのプロセス。カーネルは起動の後に値を渡されるだけです。

### S-04 カーネルの設定が利用者の書ける場所に置かれている

- 場所：`kernel/src/sys/keep.c:96-100`、`kernel/src/fs/store.c:657-658`、`common/store_format.h:164`
- 分類：2
- 今：保存先は利用者のフォルダーの `AppData\Nocturne` です。
- 今：書込みを拒む属性は `STORE_SYSTEM` だけで、`C:\Nocturne` に付きます。
- 誤り：どのアプリも文書を書き換えられ、次の起動でカーネルがそれを読みます。
- 誤り：装置に属する値（モニターのモード、出口の選択）と利用者に属する値の区別がありません。
- 持ち主：システムのプロセス。機械の値と利用者の値を分けて持ちます。

### S-05 手で直しても再起動まで効かない、読むのは起動の一度だけ

- 場所：`kernel/src/sys/keep.c:328-343`（`keep_start`）、`docs/nocturne-native-abi.md` の §12
- 分類：4
- 今：文書を読むのは `kmain_late` の `keep_start` の一度です。
- 今：§12 は「動いている系には効かない」と定めています。
- 誤り：文書が第二の入口になっていて、二つの経路の値が食い違います。
- 持ち主：カーネルの機構（受ける口を一つにします）。

### S-06 悪い値を黙って捨てる

- 場所：`kernel/src/dev/audio.c:825-838`（`sound_line`）、`kernel/src/gui/wm.c:1190-1210`（`desktop_line`）、`kernel/src/gui/screenmode.c:399-405`（`displays_line`）
- 分類：4
- 今：範囲の外の値は `apply` の中で何もせずに通ります。
- 誤り：利用者にも log にも知らせません。
- 持ち主：システムのプロセス（検証と報告）。

### S-07 以前の系の文書の引き継ぎがカーネルにある

- 場所：`kernel/src/sys/keep.c:17`（`older[]`）、`219-253`（`read_older`）、`kernel/src/gui/wm.c:1187-1205`（`deep_colour`）
- 分類：1・2
- 今：カーネルが昔の二つのパスを決め打ちで読み、古い鍵の名を覚えています。
- 誤り：AGENTS.md の「互換は持たない」に反します。
- 誤り：カーネルが過去のファイルの名を持ちます。
- 持ち主：消します。要るならシステムのプロセスの一度きりの移行です。

### S-08 保存の遅れと読む大きさが決め打ち

- 場所：`kernel/src/sys/keep.h:22`（`KEEP_AFTER_NS` 500 ms）、`kernel/src/sys/keep.c:11`（`READ_MOST` 64 KiB）
- 分類：1
- 今：64 KiB を超える文書は読みません。
- 誤り：Displays.conf はモニターの数で増えます。上限を超えると全部の行を失います。
- 持ち主：システムのプロセス。

---

## 1. 起動の引数と boot

### B-01 起動の引数でしか変えられない値の一覧

次の値は `boot/limine.conf` の項目を手で編むか起動の画面で打つしか変えられません。全部、再起動が要ります。全部、GUI の入口がありません。

| 引数 | 場所 | 何を決めるか | 持ち主 |
|---|---|---|---|
| `nogui` | `kernel/src/init.c:53`、`boot/limine.conf:21` | 窓を起こすか | システムのプロセス |
| `noaml` | `kernel/src/dev/acpi.c:480` | ACPI の AML を使うか | 利用者の選択 |
| `nopcid` | `kernel/src/mm/vmm.c:649` | PCID を使うか | カーネルの機構（試験は別の口） |
| `nosmp` | `kernel/src/arch/smp.c:303` | CPU を起こすか | 利用者の選択 |
| `fbsim10` | `kernel/src/dev/fb.c:101` | なめらかな階調の初めの値 | 利用者の選択 |
| `noiommu` | `kernel/src/dev/iommu.c:131` | DMA を訳すか | 利用者の選択 |
| `appdrivers-unisolated` | `kernel/src/dev/iommu.c:829`・`875` | 隔離なしのドライバーを許すか | 利用者の選択 |
| `noappdrivers`・`appdrivers=any` | `kernel/src/dev/pciuser.c:811-812` | 誰がドライバーになれるか | システムのプロセス |
| `fbhold=` | `kernel/src/dev/pciuser.c:237` | 画面を握る時間の上限 | システムのプロセス |
| `noresume` | `kernel/src/dev/hibernate.c:600` | 休止の像を捨てて起きるか | 利用者の選択 |
| `prof`・`prof_ms=` | `kernel/src/sys/prof.c:274-275` | プロファイラー | システムのプロセス |
| `dns-server=` | `kernel/src/net/net.c:950-983` | 既定の DNS | 利用者の選択 |
| `tcp-rx-bytes=`・`tcp-tx-bytes=` | `kernel/src/net/net.c:2486-2510` | TCP の緩衝 | システムのプロセス |
| `nogpu`・`gpu-enable`・`gpu-timing`・`gpu-slow-test` | `kernel/src/vm/virtio_gpu.c:240`・`265`・`579-580`・`672` | VM の GPU | 利用者の選択 |
| `e1000-itr-us=` | `kernel/src/vm/e1000.c:185` | VM の NIC の割り込みの間隔 | カーネルの機構 |

- 分類：1・4
- 誤り：AGENTS.md の「システムの内部を引数や旗の起動で組まない」に、この表の全部が反します。
- 誤り：`net.c` は引数の解析を二つ自前で持ち、`cmdline_number` と重複します。

### B-02 起動の項目と起動の画面が手で編むファイル

- 場所：`boot/limine.conf:4`（`timeout: 3`）、`:5-10`（名と色）、`:12-21`（項目）
- 分類：1・4
- 今：待ち時間も既定の項目もファイルにあります。
- 誤り：OS の中から変える口がありません。
- 持ち主：利用者の選択。システムのプロセスが書きます。

### B-03 同じ色の表が二か所にある

- 場所：`boot/limine.conf:6-9`、`kernel/src/dev/fbcon.c:48-53`（`palette`、`DEF_BG`）
- 分類：1
- 今：起動の画面の色が二つの場所で別々に決め打ちです。
- 持ち主：一つの場所（カーネルの機構の既定）。色の選択は利用者の選択です。

### B-04 init のパスと環境がカーネルの定数

- 場所：`kernel/src/init.c:30`（`INIT_PATH`）、`:69`（`TERMINAL=0 1 2`）
- 分類：1・2
- 今：最初のプログラムと渡す環境がカーネルの文字列です。
- 持ち主：最初のプログラムの場所はカーネルの機構でよく、渡す値はシステムのプロセスです。

### B-05 ファームウェアの画面の一つ目だけを使う

- 場所：`kernel/src/boot.c:155`（`framebuffers[0]`）
- 分類：3
- 今：Limine が渡す枠の一つ目だけを `fb_init` に渡します。
- 誤り：`framebuffer_count` が 2 以上でも残りを捨てます。
- 持ち主：装置の申告。

---

## 2. CPU・idle・性能

### C-01 性能の好みを変える関数に呼び手が無い

- 場所：`kernel/src/arch/idle.h:15-21`、`kernel/src/arch/idle.c:112`・`156-163`
- 分類：1・4
- 今：`CPU_PERF_PREFERENCE_DEFAULT` は 0x40 です。
- 今：`cpu_perf_set_policy` を呼ぶ所はカーネルに一つもありません。
- 今：注釈は「the setting is the system's, which may change it at any time」と書きます。
- 誤り：注釈が嘘です。誰も変えられません。
- 持ち主：利用者の選択（電源の方針）。カーネルの機構は MSR に書くだけです。

### C-02 性能の範囲を決め打ちで全部にする

- 場所：`kernel/src/arch/idle.c:97-99`（`cppc_request`）、`:106-109`（`hwp_request`）、`:121`
- 分類：2・3
- 今：上限は highest、下限は floor に固定です。
- 今：`cppc_caps` は nominal を読みますが、log に出すだけです。
- 誤り：上限を nominal に抑える（ブーストを切る）選択がありません。
- 持ち主：利用者の選択。読んだ値は装置の申告です。

### C-03 idle の深さが C1 だけ

- 場所：`kernel/src/arch/idle.c:7-8`、`:57`
- 分類：3
- 今：MWAIT の hint は常に 0 です。
- 今：注釈が「`_CST` は評価しない」と書きます。
- 誤り：装置が持つ深い C 状態を一つも使いません。
- 持ち主：装置の申告（`_CST`）とカーネルの機構。

### C-04 online-capable の CPU を捨てる

- 場所：`kernel/src/arch/topology.c:207`
- 分類：3
- 今：MADT が online-capable と書く CPU を「not used (no hot-add)」で捨てます。
- 持ち主：装置の申告。

### C-05 投機の対策が決め打ち

- 場所：`kernel/src/arch/cpu.c:471-477`（`speculation_setup`）
- 分類：2
- 今：Automatic IBRS、IBRS、STIBP の入れ方がカーネルの中の一つの方針です。
- 誤り：安全と速さの選択を管理者ができません。
- 持ち主：利用者の選択。

### C-06 機械検査の閾値が決め打ち

- 場所：`kernel/src/arch/cpu.c:266`（`THRESHOLD_ERRORS 1`）
- 分類：1
- 今：訂正できた誤りを一件ごとに割り込みます。
- 持ち主：システムのプロセス。

---

## 3. スケジューラー

### P-01 優先度を決められるのは自分のアプリだけ

- 場所：`kernel/src/sys/thread.c:248`、`common/abi.h:62`・`322-333`
- 分類：2
- 今：`t->process != p` は `N_ERR_NOT_FOUND` です。
- 誤り：システムのプロセスも利用者も、他のアプリの分け前を変えられません。
- 誤り：タスクマネージャーの「優先度」にあたる口がありません。
- 持ち主：システムのプロセス。カーネルの機構は権限のある handle で受けます。

### P-02 アプリの重みの上限が既定と同じ

- 場所：`kernel/src/sys/thread.c:239`
- 分類：1・2
- 今：アプリの重みは 1〜1024 です。カーネルの `SCHED_WEIGHT_MAX` は 2^20 です。
- 誤り：どのアプリも既定より上がれません。誰にも上げられません。
- 持ち主：システムのプロセス。

### P-03 要求の長さをアプリが選べない

- 場所：`kernel/src/sys/thread.c:240`、`kernel/src/sys/sched.h:52-54`
- 分類：1・2
- 今：fair の `request_ns` は常に `SCHED_REQUEST_DEFAULT_NS`（3 ms）です。
- 今：EEVDF は要求の長さで遅れを分ける機構です。
- 誤り：機構を持ちながら口を開いていません。
- 持ち主：カーネルの機構の口。値はアプリとシステムのプロセスです。

### P-04 アプリの deadline の枠が半 CPU に固定

- 場所：`kernel/src/sys/thread.c:198-203`（`APP_DEADLINE_SHARE`）
- 分類：1・2
- 今：アプリの deadline のスレッドは合わせて一つの CPU の半分までです。
- 誤り：CPU の数も、用途（音の制作、動画）も見ません。
- 持ち主：システムのプロセス。

### P-05 deadline の予約の比が決め打ち

- 場所：`kernel/src/sys/sched.c:79-81`（`DL_*`）
- 分類：1
- 今：下限は周期の 1/64、上限は周期の 15/16、減衰は 1/32 です。
- 持ち主：カーネルの機構の既定。変える口はシステムのプロセスです。

### P-06 均しの間隔が決め打ち

- 場所：`kernel/src/sys/sched.c:86`（`look_every`）
- 分類：1
- 今：node は 12 ms、全体は 48 ms です。
- 誤り：SLIT の距離を読みながら、間隔に効かせていません。
- 持ち主：装置の申告（距離）とカーネルの機構。

### P-07 CPU の割り当ての口が無い

- 場所：`common/abi.h:50-150`、`kernel/src/sys/sched.h:126`
- 分類：2
- 今：thread を CPU に縛る口は核の thread（`kthread_create_on`）だけです。
- 誤り：アプリも管理者も CPU の集合を選べません。
- 持ち主：システムのプロセス。

### P-08 核の thread の級がそれぞれの部品の決め打ち

- 場所：`kernel/src/arch/irq.c:116`、`kernel/src/dev/audio.c:635`、`kernel/src/gui/wm.c:2075`・`2085`・`2760-2762`
- 分類：2
- 今：各部品が自分の級を決めます。
- 今：窓の合成の thread は画面の周期の deadline です。
- 持ち主：周期は装置の申告、級の順はシステムのプロセスです。

### P-09 epoch の待ちは注釈が「方針」と認める定数

- 場所：`kernel/src/sys/epoch.c:15-19`（`EPOCH_FORCE_NS` 10 ms）
- 分類：1
- 今：注釈に「a policy, not a measure」とあります。
- 持ち主：カーネルの機構。測って決めます。

---

## 4. メモリー

### M-01 主のスタックが 8 MiB に固定

- 場所：`kernel/src/mm/vmm.h:30`（`USER_STACK_MAX`）、`kernel/src/sys/syscall.c:61`、`kernel/src/mm/space.c:115`
- 分類：1
- 今：どのプログラムも主のスタックは 8 MiB までです。
- 誤り：プログラムが申告する口がありません。
- 持ち主：プログラムの申告（ELF）。上限はシステムのプロセスです。

### M-02 起動の引数の大きさが 4 MiB に固定

- 場所：`common/abi.h:232`（`N_LAUNCH_MAX_BYTES`）、`kernel/src/sys/proc.c:268`、`kernel/src/sys/syscall.c:782`
- 分類：1
- 今：スタックの大きさの半分に結び付いた固定の上限です。
- 持ち主：カーネルの機構。資源の残りで決めます。

### M-03 チャンネルの与信が 1 MiB に固定

- 場所：`kernel/src/sys/channel.c:200-202`（`CHANNEL_CREDIT`）
- 分類：1
- 今：1 MiB を超える送信は `N_ERR_NO_MEMORY` です。
- 誤り：メモリーが足りていても拒みます。
- 持ち主：受け手の申告とシステムのプロセス。

### M-04 メモリー切れの方針が「報告して失敗」だけ

- 場所：`kernel/src/sys/process.c:113-120`、`kernel/src/mm/space.c:214-215`、`kernel/src/mm/shared.c:1290`・`1400`
- 分類：2
- 今：log に一行書いて確保を失敗させます。
- 今：報告は 10 秒に一回までの決め打ちです。
- 誤り：どのアプリを閉じるかを問う相手も、利用者への知らせもありません。
- 持ち主：システムのプロセスと UI。カーネルの機構は事象を出すだけです。

### M-05 予約の条件が空きメモリーに固定

- 場所：`kernel/src/mm/space.c:202-215`
- 分類：2
- 今：空きより大きい予約は拒みます。
- 誤り：申し込みの方針（commit の扱い）を選べません。
- 誤り：`mm/` に頁を退避する先を持つ部品がありません（`swap` は当たりません）。
- 持ち主：システムのプロセス。

### M-06 プロセスごとの限度が無い

- 場所：`kernel/src/mm/space.c:211`（「No size cap of its own」）
- 分類：2
- 今：メモリーの上限を与える口がありません。
- 持ち主：システムのプロセス。

### M-07 調律の定数

- 場所と値：
  - `kernel/src/mm/vmm.c:53`（`TLB_RANGE_MAX` 32）、`:55`（`PCID_SLOTS` 8）、`:829`（`DEMAND_AROUND` 16）
  - `kernel/src/mm/program.c:251`（`PROGRAM_AROUND` 16）、`:254`（空き 4096 頁の閾）
  - `kernel/src/mm/space.c:207`（`RESERVE_EAGER` 64 KiB）
  - `kernel/src/mm/pmm.c:33`・`35`、`kernel/src/mm/heap.c:47-52`
  - `kernel/src/mm/pager.c:39`（`PAGER_BATCH`）
- 分類：1
- 誤り：`TLB_RANGE_MAX` の注釈は「whole flush が安い」と書きますが、CPU ごとの測定も CPUID の申告もありません。
- 誤り：`program.c:254` の 4096 はメモリーの総量を見ません。
- 持ち主：カーネルの機構。装置から測るか、総量から決めます。

### M-08 NUMA の置き方が一つ

- 場所：`kernel/src/mm/pmm.c:274`・`397`・`612-622`
- 分類：2
- 今：近い node から順に取るだけです。
- 誤り：アプリごとの方針（交互に置く、CXL の node を避ける）を選べません。
- 持ち主：システムのプロセス。距離は装置の申告です。

---

## 5. 記憶

### D-01 NVMe の待ち行列を 64 に切り詰める

- 場所：`kernel/src/dev/nvme.c:34`、`:525`、`:936`
- 分類：3
- 今：`CAP.MQES` を読み、`MIN(64, mqes + 1)` にします。
- 誤り：装置が受ける深さを捨てます。理由の注釈は「一頁に収まる」だけです。
- 持ち主：装置の申告。

### D-02 NVMe の一度の転送を 2 MiB に切り詰める

- 場所：`kernel/src/dev/nvme.c:35`、`:574-575`
- 分類：3
- 今：`MDTS` が大きくても PRP の一頁分で止めます。
- 持ち主：装置の申告。

### D-03 NVMe の電源と割り込みの機能を使わない

- 場所：`kernel/src/dev/nvme.c:88-89`（`FEATURE_QUEUES`・`FEATURE_COMMAND_SETS` だけ）
- 分類：3
- 今：`nvme.c` に APST の語は当たりません。
- 今：割り込みの束ね（Interrupt Coalescing）の語も当たりません。
- 誤り：省電力の方針も、遅れと負荷の選択もありません。
- 持ち主：利用者の選択。

### D-04 AHCI のリンクの省電力を常に切る

- 場所：`kernel/src/dev/ahci.c:126-127`、`:412-421`、`:654-656`
- 分類：2・3
- 今：`comreset` は Partial、Slumber、DevSleep を禁じます。注釈に「Its power states are kept to active」とあります。
- 今：`CMD_ALPE` と `CMD_ASP` は消す所だけがあり、立てる所がありません。
- 誤り：`CAP2` の `sds` と `sadm` を読んで、切るために使っています。
- 誤り：省電力と速さの選択がありません。
- 持ち主：利用者の選択。

### D-05 AHCI の転送の大きさと時間が決め打ち

- 場所：`kernel/src/dev/ahci.c:40`（`XFER_PAGES` 512 KiB）、`:49`（`IO_NS` 30 秒）、`:51`（`RETRY_MOST_NS` 60 秒）
- 分類：1
- 持ち主：装置の申告（IDENTIFY）とシステムのプロセス。

### D-06 NVMe と USB の記憶の時間切れが決め打ち

- 場所：`kernel/src/dev/nvme.c:36-38`、`kernel/src/dev/usbmsc.c:21-22`
- 分類：1
- 今：NVMe の I/O は 30 秒、シャットダウンは 60 秒、USB の命令は 10 秒、起動は 30 秒です。
- 誤り：`nvme.c:38` の注釈が「our limit」と認めます。
- 持ち主：システムのプロセス。

### D-07 書込みのキャッシュを選べない

- 場所：`kernel/src/dev/nvme.c:968`（`c->vwc`）、`kernel/src/dev/ahci.c:174`（`cache`）
- 分類：3
- 今：装置のキャッシュの有無を読むだけです。
- 誤り：利用者が切る口がありません。
- 持ち主：利用者の選択。

---

## 6. ファイルの層

### F-01 利用者を「最初のフォルダー」で決める

- 場所：`kernel/src/fs/store.c:397-412`、`:389-392`
- 分類：1・2
- 今：`C:\Users` の最初のフォルダーが利用者です。
- 今：無ければ `"User"` を作ります。
- 今：カーネルの中に利用者は常に一人です。
- 誤り：利用者の選択も、複数の利用者も、名の変更もありません。
- 誤り：`keep` の保存先がこの一つに縛られます。
- 持ち主：システムのプロセス（ログインと利用者の管理）。

### F-02 書込みの遅れが決め打ち

- 場所：`kernel/src/fs/store.c:20`（`WRITE_DELAY_NS` 5 秒）
- 分類：1
- 今：最初の変更は 5 秒待ちます。
- 誤り：5 秒の消失の窓を、利用者も装置の速さも選べません。
- 持ち主：利用者の選択。

### F-03 未確定の量が決め打ち

- 場所：`kernel/src/fs/document.c:30-31`（`WORK_PAGES` 16 MiB、`COMMIT_BLOCKS` 64 MiB）
- 分類：1
- 誤り：メモリーの量も記憶の速さも見ません。
- 持ち主：カーネルの機構。装置から決めます。

### F-04 キャッシュがメモリーの 1/16 に固定

- 場所：`kernel/src/fs/store.c:24`・`334-335`、`kernel/src/fs/cache.c:167`
- 分類：1
- 今：上限は起動の時に一度だけ決まります。
- 今：`fs/` と `mm/` に圧力（pressure、reclaim）の語は当たりません。
- 誤り：空きが余っていても増えません。足りなくても減りません。
- 持ち主：カーネルの機構。

### F-05 権限が一つのビットだけ

- 場所：`common/store_format.h:164-167`、`kernel/src/fs/store.c:432`・`657-658`
- 分類：2
- 今：`STORE_SYSTEM` はアプリの書込みを拒むだけです。
- 誤り：利用者ごと、アプリごとの権限がありません。
- 持ち主：システムのプロセス。

### F-06 フォルダーの配置が ABI の定数

- 場所：`common/abi.h:173-178`、`kernel/src/dev/hibernate.c:36`、`kernel/src/init.c:30`
- 分類：1
- 今：`C:\Nocturne\System`、`C:\Programs`、`C:\Nocturne\Hibernate` がカーネルの文字列です。
- 持ち主：システムのプロセス。カーネルは起動に要る一つだけを持ちます。

### F-07 起動の媒体を待つ時間が決め打ち

- 場所：`kernel/src/fs/store.c:21-22`、`:304-308`
- 分類：1
- 今：10 秒、USB を見ている間は 60 秒です。
- 持ち主：装置の申告。

---

## 7. 網

### N-01 IPv4 の設定を変える口が無い

- 場所：`kernel/src/net/net.c:61`、`:853-863`、`:2588-2704`（`net_syscall`）
- 分類：1・2・4
- 今：`my_ip`・`my_mask`・`my_gw`・`my_dns` を入れるのは DHCP の応答だけです。
- 今：`SYS_NET_*` の情報の系統は読出しと試験だけです。
- 誤り：固定のアドレスを設定できません。
- 誤り：DHCP を切れません。
- 持ち主：利用者の選択。DHCP はシステムのプロセスの仕事です。

### N-02 DNS は起動の引数か DHCP だけ

- 場所：`kernel/src/net/net.c:950-983`
- 分類：1・4
- 今：`dns-server=` は起動の時に一度読むだけです。
- 誤り：動いている系で変えられません。
- 持ち主：利用者の選択。

### N-03 TCP の緩衝は起動の引数だけ

- 場所：`kernel/src/net/net.c:1203`、`:1210`、`:2486-2510`、`:1673-1674`、`:2145-2146`
- 分類：1・4
- 今：既定は 1 MiB です。
- 今：接続ごとに同じ大きさです。
- 誤り：回線の帯域も遅れも見ません。
- 持ち主：カーネルの機構（自動の調律）。

### N-04 使う adapter は最初の一つだけ

- 場所：`kernel/src/net/net.c:58`、`:2470-2480`、`kernel/src/net/net.h:23-26`
- 分類：2・3
- 今：`nif` は一つです。
- 今：二つ目の adapter の登録は黙って無視されます。
- 誤り：経路の表も、adapter の一覧も、選ぶ口もありません。
- 持ち主：装置の申告（一覧）と利用者の選択（使う順）。

### N-05 実機の NIC ドライバーが一つの機種の一つの個体

- 場所：`kernel/src/net/net.c:2517`、`kernel/src/net/rtl8125.c:533-534`、`:147-159`
- 分類：1
- 今：`pci_find(0x10EC, 0x8125)` で最初の一枚を探します。
- 今：レジスターもリングも `static` の一組です。
- 誤り：同じ機種の二枚目を使えません。
- 持ち主：カーネルの機構（個体ごとの状態）。

### N-06 NIC の機能を決め打ちで全部または無し

- 場所：`kernel/src/net/rtl8125.c:289`（EEE+ 切）、`:291`（magic packet 切）、`:280`・`292`（ASPM 切）、`:363`（`INTR_MOD` 0x00630063）、`:367-374`（広告は全速度）、`:93`（`RX_ACCEPT`）、`:130-131`（`BUFSZ`、`FRAME_MAX`）
- 分類：1・2
- 誤り：速度と二重の選択、省電力、Wake-on-LAN、割り込みの束ね、ジャンボフレームの選択がありません。
- 持ち主：利用者の選択。

### N-07 MTU と MSS の決め打ち

- 場所：`kernel/src/net/net.c:1206`（`TCP_MSS`）、`:418`（TTL 64）
- 分類：1・3
- 今：MTU は 1500 に固定です。
- 誤り：`struct netif` に MTU の欄がありません（`kernel/src/net/net.h:12-21`）。
- 持ち主：装置の申告。

### N-08 固定の表と上限

- 場所と値：
  - `kernel/src/net/net.c:430`（ARP 16 件）、`:431-432`（寿命 600 秒、再送 1 秒）
  - `:534`（ping 8 件）
  - `:605`（UDP の受信 8 個）
  - `:248`（`RXQ` 64）
  - `:987-988`（DNS のキャッシュ 256 KiB、再送 2 秒）
  - `:1219`（backlog 128）、`:1221`（順序外 8 区間）
  - `:1225-1227`（RTO）、`:1231-1232`（TIME_WAIT 2 秒、20 秒）
  - `:2386-2388`（service の窓）
  - `kernel/src/dev/usbnet.c:25`（TX 16 個）
- 分類：1
- 誤り：`ARP_N` 16 は同じ網の機器の数で押し出しが起きます。
- 誤り：AGENTS.md の「資源を小さな固定の上限で拒まず」に反します。
- 持ち主：カーネルの機構。

### N-09 輻輳の制御が一つ

- 場所：`kernel/src/net/net.c:1246`（NewReno）
- 分類：2
- 誤り：選ぶ口がありません。
- 持ち主：システムのプロセス。

### N-10 IPv6 が無い

- 場所：`kernel/src/net/net.c`（`IPv6` の語は当たりません）、`common/abi.h:762-765`（`struct n_sockaddr` は 32 bit の ip）
- 分類：3
- 誤り：ABI の形が IPv4 に丸められています。
- 持ち主：カーネルの機構の ABI。

---

## 8. 入力

### I-01 キーボード配列が US の表でカーネルの中にある

- 場所：`kernel/src/dev/input.c:15-30`（`map_norm`・`map_shift`）、`:88-94`
- 分類：1・2
- 今：配列は一つで、変える口がありません。
- 誤り：ユーザーの言語にも機械の配列にも合わせられません。
- 持ち主：利用者の選択。変換はシステムのプロセスです。

### I-02 キーコードが ASCII の文字で、それ以外は捨てる

- 場所：`kernel/src/dev/input.h:5-12`・`22`、`kernel/src/dev/input.c:117-121`
- 分類：3
- 今：`key` は「printable keys use their character value」です。
- 今：`input_unicode` は 0x80 以上を `KEY_UNKNOWN` にします。
- 今：注釈に「no room for other characters yet」とあります。
- 誤り：物理のキーと文字が一つの欄に混ざっています。
- 持ち主：カーネルの機構は物理のキーだけを渡します。文字はシステムのプロセスです。

### I-03 HID の値を PC の set 1 へ変換する

- 場所：`kernel/src/dev/usbhid.c:81-92`、`:95-99`、`kernel/src/dev/input.c:45-97`
- 分類：3
- 今：USB の usage を scan code set 1 に写してから文字にします。
- 今：usage が 256 以上なら捨てます。
- 誤り：AGENTS.md の「互換は持たない」に反する中間の形です。
- 誤り：consumer page（音量キー、再生キー）が届きません。
- 持ち主：装置の申告。

### I-04 キーの繰り返しの値が USB のドライバーの定数

- 場所：`kernel/src/dev/usbhid.c:18-19`、`:129-139`
- 分類：1・2
- 今：遅れは 500 ms、間隔は 33 ms です。
- 誤り：持ち主がドライバーで、利用者が変えられません。
- 誤り：他の入力の経路には効きません。
- 持ち主：利用者の選択。繰り返しの機構はドライバーでなく入力の層です。

### I-05 ボタンの 4 番以降を読んで捨てる

- 場所：`kernel/src/dev/usbhid.c:67`、`:255-262`、`kernel/src/dev/input.h:32`
- 分類：3
- 今：ボタンは 8 個まで読みます。
- 今：`e.buttons = buttons & 7` で 3 個に丸めます。
- 誤り：`usbhid.c:2` の注釈は「extra buttons ... work」と書きます。動きません。
- 持ち主：装置の申告。割り当ては利用者の選択です。

### I-06 横のホイールが無い

- 場所：`kernel/src/dev/usbhid.c:450`、`kernel/src/dev/input.h:31`
- 分類：3
- 今：Generic Desktop の Wheel だけを読みます。
- 今：`0x000C0238`（AC Pan）の語は当たりません。
- 持ち主：装置の申告。

### I-07 NumLock の状態が無い

- 場所：`kernel/src/dev/input.c:19-21`、`kernel/src/dev/usbhid.c:570`
- 分類：1・3
- 今：テンキーは常に数字です。
- 今：LED の報告は Caps Lock だけです。
- 持ち主：利用者の選択。

### I-08 修飾の状態が全部のキーボードで一つ

- 場所：`kernel/src/dev/input.c:11-12`
- 分類：2
- 今：`mods` と `caps` は機械に一つです。
- 誤り：キーボードごとの状態も配列もありません。
- 持ち主：カーネルの機構。

### I-09 ポインターの加速と速さの表が決め打ち

- 場所：`kernel/src/gui/wm.c:2606-2610`、`common/display_abi.h:32`
- 分類：1
- 今：5 カウントを超えると 2 倍です。
- 今：速さは 5 段（x0.5〜x2）です。
- 誤り：加速を切れません。
- 誤り：マウスごとの値がありません。
- 持ち主：利用者の選択。

### I-10 ダブルクリックの時間と距離が決め打ち

- 場所：`kernel/src/gui/wm.c:487`（450 ms）、`:2449`（6 画素）
- 分類：1
- 持ち主：利用者の選択。

### I-11 ホイールの量の割り当てが決め打ち

- 場所：`kernel/src/gui/wm.c:2697`（音量 5 刻み）、`:2704`
- 分類：1
- 今：スクロール量の設定がありません。
- 持ち主：利用者の選択。

### I-12 入力の行列が固定

- 場所：`kernel/src/gui/wm.c:2207`（`INQ` 1024）、`:61-63`（`MAILBOX_MAX` 4096）
- 分類：1
- 今：溢れると失います。
- 持ち主：カーネルの機構。

---

## 9. 画面と窓の管理

### W-01 窓の管理、タスクバー、スタートメニュー、壁紙がカーネルの中にある

- 場所：`kernel/src/gui/wm.c`（3,652 行）、`kernel/src/gui/desktop.c`（754 行）、`kernel/src/init.c:53-54`
- 分類：2
- 今：描画の部品、文字、色、絵をカーネルが持ちます。
- 誤り：持ち主の言う境界（UI、システムのプロセス、カーネル）が一つに潰れています。
- 誤り：§12 の文書が「今はどれもカーネルの中」と書きます。
- 持ち主：UI とシステムのプロセス。カーネルの機構は面、フリップ、入力の配達だけです。

### W-02 アプリの一覧がカーネルの表

- 場所：`kernel/src/gui/wm.c:1048-1071`（`apps[]`）、`:1092`（`desktop_apps[]`、数字の添字）
- 分類：1・2
- 今：Piano、Snake、Tetris のパスと順番がカーネルにあります。
- 今：デスクトップのアイコンは添字の決め打ちです。
- 誤り：アプリを入れても一覧に出ません。
- 誤り：利用者がアイコンを並べ替えられません。
- 持ち主：システムのプロセス（アプリの登録）と UI。

### W-03 文言がカーネルの英語の定数

- 場所：`kernel/src/gui/wm.c:1465-1468`、`:1384`（`"Volume"`）、`:3057`（`"Welcome to"`）、`:1233`、`kernel/src/dev/pm.c:99`・`107`・`247-248`・`270`・`291`・`309`・`338`
- 分類：1・2
- 今：電源の部品（`dev/pm.c`）がトーストの文を書きます。
- 誤り：言語の設定がありません。
- 誤り：装置の層が UI の文を持ちます。
- 持ち主：UI。カーネルの機構は事象だけを出します。

### W-04 見た目の寸法と色がカーネルの定数

- 場所：`kernel/src/gui/wm.c:51-58`（`TITLE_H` 28、`TASKBAR_H` 40、`SHADOW` 10、最小の窓）、`:1094-1095`、`:1142`、`:1239-1241`、`:1315-1319`、`:1458`（`RGB(70, 62, 140)`）、`kernel/src/gui/wm.c:492`（`TOAST_NS` 4 秒）
- 分類：1
- 今：テーマ、アクセントの色、タスクバーの位置や大きさを変えられません。
- 持ち主：利用者の選択と UI。

### W-05 壁紙が手続きの絵で、選べない

- 場所：`kernel/src/gui/desktop.c:107`、`:189`
- 分類：1
- 今：夜空と山の絵を種の定数で描きます。
- 持ち主：利用者の選択と UI。

### W-06 拡大率（DPI）が無い

- 場所：`kernel/src/gui/wm.c:51-58`、`common/gfx.h:38`（`FONT_SMALL`・`FONT_LARGE`）
- 分類：1・3
- 今：`wm.c`・`desktop.c`・`gfx.h` に `dpi`、`scale` の語は当たりません。
- 誤り：どの解像度の画面でも全部の部品が同じ画素の数です。
- 持ち主：装置の申告（既定）と利用者の選択。

### W-07 画面の配置が左から右の一列に固定

- 場所：`kernel/src/gui/wm.c:8`、`:1999-2010`（`layout`）
- 分類：1・2
- 今：`ws->y = 0` で順に並べます。
- 今：画面の順はドライバーの出力の順です。
- 誤り：利用者が位置を決められません。
- 誤り：上下の配置も、複製もありません。
- 持ち主：利用者の選択。保存はシステムのプロセスです。

### W-08 主画面が screen 0 に固定

- 場所：`kernel/src/gui/wm.c:563`、`:1999`、`common/display_abi.h:55`
- 分類：1・2
- 今：タスクバーは screen 0 だけです。
- 持ち主：利用者の選択。

### W-09 モードの形に色深度も色の形式も無い

- 場所：`common/abi.h:448-452`（`struct n_screen_mode`）、`:394-396`（`N_SCANOUT_*` 三つ）、`kernel/src/dev/fb.c:109-111`
- 分類：3
- 今：モードは幅、高さ、拍、旗だけです。
- 誤り：Windows の例の音と同じ欠けが画面にあります。
- 誤り：色深度、YCbCr の形式、色域、HDR を一覧にも選択にも載せる場所がありません。
- 持ち主：装置の申告（一覧）と利用者の選択（既定）。

### W-10 VRR を切れない

- 場所：`common/abi.h:443-447`、`common/display_abi.h:99`（「flags ignored」）、`kernel/src/gui/screenmode.c:81-83`（`same` は旗を見ない）、`:236-241`（`refresh_from`）、`kernel/src/gui/wm.c:2080`・`2110`
- 分類：2・4
- 今：ドライバーが VRR の旗を立てると VRR で動きます。
- 今：TRY は旗を無視します。
- 今：同じ幅・高さ・拍の二つのモードは並べられません（`common/abi.h:455-461`「no two alike」）。
- 誤り：利用者が VRR の入り切りを選べません。
- 誤り：下限（`vrr_min_mhz`）を変えられません。
- 持ち主：利用者の選択。

### W-11 モードの選択の方針がカーネルにある

- 場所：`kernel/src/gui/screenmode.c:155-168`（`pick`）、`:132-149`（`nearest`）
- 分類：2
- 今：保存の値、preferred、同じ大きさの近い拍、最大の順がカーネルの決め打ちです。
- 持ち主：システムのプロセス。カーネルの機構はドライバーへ望みを渡すだけです。

### W-12 試す時間が ABI の定数

- 場所：`common/display_abi.h:67`（15 秒）、`kernel/src/gui/screenmode.c:494`
- 分類：1
- 持ち主：UI。

### W-13 モードの数を超えると申し出ごと拒む

- 場所：`common/abi.h:462`（128）、`kernel/src/dev/pciuser.c:997`
- 分類：1・3
- 今：129 個のモードを出すと `N_ERR_INVALID` です。
- 誤り：一覧の全部を失います。
- 持ち主：カーネルの機構。

### W-14 EDID の基本の 128 バイトしか読まない

- 場所：`common/edid.h:74-106`（`edid_modes`）
- 分類：3
- 今：詳細タイミング、標準タイミング、確立タイミングだけです。
- 誤り：拡張のブロック（CTA）を読みません。
- 誤り：色深度、HDR、音の能力、VRR の範囲を読みません。
- 持ち主：装置の申告。

### W-15 なめらかな階調が全画面に一つの値

- 場所：`common/display_abi.h:31-40`、`kernel/src/dev/fb.c:473-476`、`kernel/src/gui/wm.c:2755`・`2764`
- 分類：2
- 今：値は機械に一つです。
- 今：できるかどうかは「shown の画面」で決まります。
- 誤り：モニターごとの値がありません。
- 持ち主：利用者の選択（モニターごと）。

### W-16 固定の上限

- 場所と値：
  - `kernel/src/gui/wm.h:21`、`kernel/src/gui/wm.c:2801`・`3577`（クリップボード 1 MiB、超えると `N_ERR_TOO_LARGE`）
  - `common/abi.h:642`（窓の面 8）、`:645`（窓 16384）、`:414`（画面のバッファー 4）
  - `common/abi.h:485`（ポインターは 32 画素の正方形）
  - `kernel/src/sys/syscall.c:242`（フリップ 64）
- 分類：1
- 誤り：クリップボードの 1 MiB は画像や長い文を拒みます。
- 誤り：ポインターの大きさを選べません。
- 持ち主：カーネルの機構。ポインターの大きさは利用者の選択です。

### W-17 キーの割り当てが決め打ち

- 場所：`kernel/src/gui/wm.c:2335-2347`
- 分類：1・2
- 今：Super、Ctrl+Esc、Alt+Tab、Alt+F4、Ctrl+Alt+T がカーネルにあります。
- 今：Ctrl+Alt+T は `SYSTEM "term"` を起こします。
- 持ち主：利用者の選択とシステムのプロセス。

### W-18 音の操作がカーネルの UI の中で完結する

- 場所：`kernel/src/gui/wm.c:1399`（`audio_system_tone(880, 70)`）、`:1401-1405`、`:2696-2699`
- 分類：1・2
- 今：操作音の高さと長さが決め打ちです。切れません。
- 持ち主：UI と利用者の選択。

---

## 10. GPU

### G-01 実機の GPU が GPU の ABI に出ない

- 場所：`kernel/src/vm/virtio_gpu.c:573-577`、`common/gpu_abi.h:16-18`、`:54-58`
- 分類：3
- 今：`gpu_init` は実機で何もせずに戻ります。
- 今：`N_GPU_BACKEND_*` は NONE、VIRGL、CPU だけです。
- 今：`struct n_gpu_info` は一つの GPU を返す形です。
- 誤り：RTX 4070 のある機械で `SYS_GPU_INFO` は backend NONE です。
- 誤り：複数の GPU の一覧、選択、アプリごとの割り当ての場所がありません。
- 持ち主：装置の申告（ドライバー）と利用者の選択。

### G-02 ドライバーを誰にするかの方針

- 場所：`kernel/src/dev/pciuser.c:1`・`811-812`、`user/apps/init.c:106-142`
- 分類：2
- 今：claim できるのは init だけです。
- 今：NVIDIA の表示の function は `nvrm` に行きます。
- 誤り：装置とドライバーの対応の表を見る口も選ぶ口もありません。
- 持ち主：システムのプロセス。

### G-03 互換のためと書かれた固定の値

- 場所：`common/gpu_abi.h:6-9`（`N_GPU_MAX_SIDE` 512、`N_GPU_MAX_TRIANGLES` 32）、`:33-36`
- 分類：1
- 今：注釈は「Source compatibility only」と書きます。
- 誤り：`common/abi.h` の「The ABI keeps no older form of a call」と AGENTS.md の「互換は持たない」に反します。
- 持ち主：消します。

---

## 11. 音

### A-01 出口の形式を 48 kHz の一つに丸める

- 場所：`kernel/src/dev/hda_codec.c:86-90`、`:212-219`（`converter_bits`）、`:420-423`（`hda_output_format`）、`kernel/src/dev/hda_codec.h:8-12`・`101`
- 分類：3
- 今：`w->pcm` は装置の受ける全部の率と全部の幅を持ちます。
- 今：読むのは `PCM_48K` の一ビットと、幅の四ビットだけです。
- 今：16 bit があれば、20・24・32 bit があっても 16 bit です。
- 誤り：44100・88200・96000 Hz のビットを受け取り、見ずに捨てます。
- 誤り：`hda_output_format` の率の欄は 48 kHz の 0 に固定です。
- 持ち主：装置の申告（一覧）と利用者の選択（既定の形式）。

### A-02 出口の形式の欄が一組しかない

- 場所：`kernel/src/dev/audio.h:62-67`、`:127-133`（`audio_sink_info`）、`:101`
- 分類：3
- 今：`struct audio_format` は一組です。
- 今：注釈に「AUDIO_RATE, which every device here has」とあります。
- 誤り：装置が受ける組の一覧を持つ場所がありません。
- 持ち主：装置の申告。

### A-03 48 kHz でない出口は登録を拒む

- 場所：`kernel/src/dev/audio.c:647-651`
- 分類：1
- 今：率が `AUDIO_RATE` でなければ登録しません。
- 誤り：将来の USB の音や 44.1 kHz だけの装置が出口になれません。
- 持ち主：カーネルの機構（率の変換）と装置の申告。

### A-04 混合の率がカーネルの定数

- 場所：`kernel/src/dev/audio.h:27`、`kernel/src/dev/hda.c:114-115`、`:561`
- 分類：1
- 今：`AUDIO_RATE` は 48000 です。
- 誤り：利用者が 96 kHz を既定にしても、混合が 48 kHz で止まります。
- 持ち主：利用者の選択。

### A-05 ストリームの ABI が 48 kHz・16 bit・2 チャンネルに固定

- 場所：`common/abi.h:576`（`N_AUDIO_RATE`）、`kernel/src/dev/audio.h:5-6`、`:28`
- 分類：1・3
- 今：`n_audio_submit` の形は一つです。
- 誤り：アプリが元の率と幅と数を伝えられません。
- 誤り：変換はアプリの負担になります。
- 誤り：32 bit float と 5.1 チャンネルの音を渡せません。
- 持ち主：アプリの申告とカーネルの機構。

### A-06 出口の一覧と選択に呼び手が無い

- 場所：`kernel/src/dev/audio.h:124-136`、`kernel/src/dev/audio.c:682-696`（`audio_sinks`）、`:698-710`（`audio_sink_choose`）、`common/abi.h:586-590`
- 分類：1・4
- 今：`audio_sinks` と `audio_sink_choose` を呼ぶ所はカーネルに一つもありません。
- 今：syscall もありません。
- 今：`n_audio_output` は名前の文字列を一つ返すだけです。
- 誤り：注釈は「The outputs for the settings」と書きます。設定の口がありません。
- 誤り：利用者はどの出口で鳴らすかを選べません。
- 持ち主：利用者の選択。

### A-07 出口の優先の順が決め打ち

- 場所：`kernel/src/dev/audio.c:593-602`（`sink_rank`）、`kernel/src/dev/audio.h:108-110`
- 分類：2
- 今：ヘッドホン、スピーカー、ライン、画面、デジタル、その他の順です。
- 誤り：カーネルが順を決めます。
- 誤り：差したら自動で切り替えるかどうかを選べません。
- 持ち主：システムのプロセスと利用者の選択。

### A-08 選んだ出口を保存しない

- 場所：`kernel/src/dev/audio.c:83-85`（`chosen`）、`:825-849`
- 分類：4
- 今：`Sound.conf` は `volume` と `muted` だけです。
- 誤り：`chosen` を仮に変えても、再起動で消えます。
- 持ち主：システムのプロセス。

### A-09 音量が機械に一つ

- 場所：`kernel/src/dev/audio.c:80-82`、`kernel/src/dev/audio.h:169-175`
- 分類：1・2
- 今：既定は 80 です。
- 今：曲線は二乗に決め打ちです。
- 誤り：出口ごとの音量がありません。
- 誤り：アプリごとの音量（ミキサー）がありません。
- 誤り：ヘッドホンへ切り替わっても同じ値です。
- 持ち主：利用者の選択。

### A-10 装置の増幅器を 0 dB に固定する

- 場所：`kernel/src/dev/hda_codec.c:94-95`、`:425-445`、`kernel/src/dev/hda_codec.h:102-105`
- 分類：3
- 今：`AMP_STEPS` と `AMP_OFFSET` を読み、0 dB にします。
- 今：音量はソフトの `gain` だけです。
- 誤り：装置の階段を音量に使いません。
- 誤り：16 bit の出口では、ソフトで音量を下げた分の分解能を失います。
- 持ち主：装置の申告とカーネルの機構。

### A-11 多チャンネルの出口を捨てる

- 場所：`kernel/src/dev/hda_codec.h:8-12`、`kernel/src/dev/hda_codec.c:294`・`339`（`first_of_association`）
- 分類：3
- 今：同じ関連のピンは最も小さい順の一本だけが出口です。
- 今：注釈に「as the sound here is stereo」とあります。
- 誤り：`CAPS_CHANNELS` を読みますが、5.1 や 7.1 の構成を選べません。
- 持ち主：装置の申告と利用者の選択。

### A-12 録音が全く無い

- 場所：`common/abi.h:104`、`kernel/src/dev/hda.c:79`（`GCAP_ISS`）、`kernel/src/dev/hda_codec.h:29`（`HDA_WIDGET_INPUT`）
- 分類：3
- 今：`SYS_AUDIO_*` は出力だけです。
- 今：`audio.c`・`hda.c`・`hda_codec.c` に capture、microphone の語は当たりません。
- 誤り：装置は入力の widget を申告しています。使いません。
- 持ち主：装置の申告とカーネルの機構。

### A-13 ブロックの大きさと遅れが決め打ち

- 場所：`kernel/src/dev/hda.c:114`（256 フレーム）、`:116`（32）、`:119-120`（先読み 4〜16）、`:122`（1 秒で止める）、`kernel/src/dev/audio.c:21`（170 ms）
- 分類：1
- 誤り：低遅延と省電力の選択がありません。
- 持ち主：利用者の選択。

### A-14 ストリームの数が 8 に固定

- 場所：`kernel/src/dev/audio.c:20`、`:74`、`kernel/src/dev/audio.h:37`
- 分類：1
- 今：9 個目は `N_ERR_BUSY` です。
- 誤り：AGENTS.md の「資源を小さな固定の上限で拒まず」に反します。
- 持ち主：カーネルの機構。

### A-15 デジタル出力の扱いが決め打ち

- 場所：`kernel/src/dev/hda_codec.c:217`
- 分類：2
- 今：デジタルの 32 bit を常に使いません。
- 誤り：パススルーの選択がありません。
- 持ち主：利用者の選択。

---

## 12. 電源と ACPI

### E-01 スリープと休止が ABI に無い

- 場所：`common/abi.h:553-554`、`kernel/src/sys/syscall.c:1205-1207`、`kernel/src/gui/wm.c:1067`・`1224-1225`
- 分類：2
- 今：`SYS_POWER` は切ると再起動だけです。
- 今：スリープと休止はカーネルの窓の管理のメニューの `@sleep`・`@hibernate` だけが頼めます。
- 誤り：システムのプロセスも UI のプログラムも頼めません。
- 持ち主：カーネルの機構の口。方針はシステムのプロセスです。

### E-02 電源ボタンの動作が決め打ち

- 場所：`kernel/src/dev/acpi.c:456-466`
- 分類：1・2
- 今：常に `PM_POWER_OFF` です。
- 誤り：「ボタンでスリープ」「何もしない」を選べません。
- 持ち主：利用者の選択。

### E-03 スリープボタンと蓋を見ない

- 場所：`kernel/src/dev/acpi.c:495-497`
- 分類：3
- 今：登録するのは固定の電源ボタンと `PNP0C0C` だけです。
- 今：`PNP0C0E`・`PNP0C0D` は当たりません。
- 持ち主：装置の申告と利用者の選択。

### E-04 温度、電池、ファン、明るさが無い

- 場所：`kernel/src/`（`_TMP`、`_CRT`、`_BST`、`ACPI0003`、`PNP0C0A`、`battery`、`backlight`、`brightness`、`fan` の語は `uacpi` の外に当たりません）、`kernel/src/arch/apic.c:26`・`120`
- 分類：3
- 今：`thermal` の語が当たるのは、CPU の温度の割り込みを塞ぐ `LAPIC_LVT_THERMAL` だけです。
- 誤り：ACPI の申告を一つも読みません。
- 誤り：過熱の方針が OS にありません。
- 持ち主：装置の申告とシステムのプロセス。

### E-05 放置の方針が無い

- 場所：`kernel/src/`（`idle_timeout`、`screensaver`、`dpms` の語は当たりません）
- 分類：2
- 誤り：画面を消す時間も、スリープまでの時間もありません。
- 持ち主：利用者の選択。

### E-06 終了の待ち時間が決め打ち

- 場所：`kernel/src/dev/pm.c:33-36`（5 秒、15 秒、60 秒、3 秒）
- 分類：1
- 持ち主：システムのプロセス。

### E-07 スリープの可否が全か無か

- 場所：`kernel/src/dev/pm.c:164-187`
- 分類：2
- 今：一つの装置が戻れないとスリープはメニューから消えます。
- 今：理由は log に書くだけです。
- 持ち主：システムのプロセスと UI。

### E-08 休止の像の決め打ち

- 場所：`kernel/src/dev/hibernate.c:36`（パス）、`:41`（`RESERVE` 64 MiB）、`:212`（余白 8192 頁）、`:217-218`（下限 16 MiB、3/4 ずつ縮める）
- 分類：1
- 誤り：像の大きさや置き場所を選べません。
- 誤り：像を捨てて起きる選択は起動の引数だけです（`:600`）。
- 持ち主：利用者の選択。

### E-09 Wake の設定が無い

- 場所：`kernel/src/net/rtl8125.c:291`
- 分類：2
- 今：Wake-on-LAN を切るだけです。
- 誤り：どの装置で起きるかを選べません。
- 持ち主：利用者の選択。

---

## 13. USB

### U-01 コントローラーの割り込みの束ねが決め打ち

- 場所：`kernel/src/dev/xhci.c:117-121`（`IMODI_GATHER` 1000）、`:2030`
- 分類：1
- 持ち主：カーネルの機構。

### U-02 構成の選択の方針と上限

- 場所：`kernel/src/dev/xhci.c:1458-1465`
- 分類：1・2
- 今：ドライバーのある最初の構成を選びます。
- 今：8 個目で止めます。
- 誤り：`bNumConfigurations` は 8 bit です。8 は装置の申告を捨てる値です。
- 持ち主：装置の申告とシステムのプロセス。

### U-03 USB の省電力が無い

- 場所：`kernel/src/dev/xhci.c`、`kernel/src/dev/usbhub.c`（`U1`、`U2`、`LPM`、`autosuspend` の語は当たりません）
- 分類：3
- 誤り：装置とハブが持つ省電力を使わず、選べません。
- 持ち主：利用者の選択。

### U-04 電力の予算を見ない

- 場所：`kernel/src/dev/xhci.c`、`kernel/src/dev/usbhub.c`（`MaxPower`、`cfg[8]` の語は当たりません）
- 分類：3
- 誤り：構成の記述子の電力の申告を読みません。
- 持ち主：装置の申告。

---

## 14. PCI とアプリのドライバー

### X-01 Resizable BAR の大きさを選ばない

- 場所：`kernel/src/dev/pci.c:604-606`、`:640-644`、`kernel/src/dev/pciuser.c:606-611`
- 分類：3
- 今：制御のレジスターを保存して戻すだけです。
- 今：能力のレジスター（受ける大きさの一覧）を読みません。
- 今：アプリのドライバーには書込みを禁じます。
- 今：ABI に口がありません。
- 誤り：BAR の大きさはファームウェアの決めたままです。
- 持ち主：装置の申告とカーネルの機構。

### X-02 PCIe の調整を何もしない

- 場所：`kernel/src/dev/pci.c`（`MPS`、`MRRS`、`ASPM` の語は当たりません）、`kernel/src/dev/pci.h:117-146`、`kernel/src/net/rtl8125.c:280`
- 分類：3
- 今：Device Control と Link Control は保存して戻すだけです。
- 誤り：ペイロードの大きさ、読み要求の大きさ、ASPM はファームウェアの決めたままです。
- 誤り：ASPM を触るのは RTL8125 のドライバーが自分を切る所だけです。
- 持ち主：装置の申告と利用者の選択。

### X-03 誰がドライバーになれるかの方針がカーネルの条件

- 場所：`kernel/src/dev/pciuser.c:809-815`
- 分類：2
- 今：pid が init の pid でなければ拒みます。
- 今：ブリッジ（class 0x06）を拒みます。
- 持ち主：システムのプロセス。カーネルの機構は権限の handle で受けます。

### X-04 固定の上限

- 場所：`kernel/src/dev/pciuser.c:43-44`（`CLAIM_GUARDS` 32、`CLAIM_ACPI_NOTES` 32）、`kernel/src/sys/syscall.c:303-305`
- 分類：1
- 持ち主：カーネルの機構。

---

## 15. IOMMU

### O-01 隔離の方針が起動の引数

- 場所：`kernel/src/dev/iommu.c:131`、`:829`、`:875`
- 分類：1・4
- 今：`noiommu` と `appdrivers-unisolated` が安全の境界を決めます。
- 誤り：利用者が見て変える画面がありません。
- 誤り：実際の状態を示す場所が log だけです。
- 持ち主：利用者の選択。状態の報告はシステムのプロセスです。

### O-02 割り込みの再割り当ての表が 32 に固定

- 場所：`kernel/src/dev/iommu_amd.c:90`・`743`、`kernel/src/dev/iommu_intel.c:131-136`・`839`、`kernel/src/dev/pciuser.c:42`・`1292`
- 分類：1・3
- 今：装置は最大 2048 の MSI-X を申告します。
- 今：`pci_user_interrupts` は申告まで受けます。
- 今：AMD は一つの claim で 32 まで、Intel も 32 の塊です。範囲の外は false を返します。
- 誤り：キューの多い NVMe や GPU の申告を捨てます。
- 持ち主：装置の申告とカーネルの機構。

### O-03 Intel の表の大きさが決め打ち

- 場所：`kernel/src/dev/iommu_intel.c:131-133`（`IRT_ENTRIES` 4096、`IOAPIC_ENTRIES` 256）
- 分類：1
- 今：4096 は機械に一つの表です。
- 持ち主：装置の申告（CPU と装置の数）。

---

## 16. 時計

### T-01 時差が無い

- 場所：`kernel/src/gui/wm.c:1785`、`:2740`、`kernel/src/sys/clock.c:934`、`kernel/src/dev/refclock.c:296`、`kernel/src/vm/hv/hv_util.c:107-109`、`kernel/src/fs/document.h:48`
- 分類：1・2
- 今：`rtc_read` は RTC の値を unix 秒として返します。
- 今：タスクバーはその値をそのまま時刻にします。
- 今：`hv_util.c` の注釈は「Nocturne has no time zones, so the clock runs in local time」と書きます。
- 今：`fs/document.h:48` は文書の時刻を「nanoseconds since 1970 UTC」と書きます。
- 誤り：同じ時計の意味が二つの場所で食い違います。
- 誤り：RTC が地方時の機械でも協定世界時の機械でも、利用者が直せません。
- 持ち主：利用者の選択。

### T-02 日付と時刻を設定できない

- 場所：`kernel/src/sys/clock.c:742`、`:752`、`kernel/src/vm/hv/hv_util.c:137`、`common/abi.h:50-150`
- 分類：1・4
- 今：`clock_set_wall` の呼び手は Hyper-V の道具だけです。
- 今：`clock_adjust_wall` の呼び手はありません。
- 今：syscall はありません。
- 誤り：実機で時刻を合わせる手段が RTC だけです。
- 誤り：インターネットの時刻合わせ（NTP）を作る口がありません。
- 持ち主：システムのプロセスと利用者の選択。

### T-03 時刻の表示の形が決め打ち

- 場所：`kernel/src/gui/wm.c:1465-1469`
- 分類：1
- 今：24 時間、英語の曜日と月です。
- 持ち主：利用者の選択と UI。

### T-04 基準の時計の順が決め打ち

- 場所：`kernel/src/dev/refclock.c:20-21`
- 分類：2
- 今：HPET、PM timer の順です。
- 持ち主：カーネルの機構。表示はシステムのプロセスです。

---

## 17. ログと診断

### L-01 記録の重さで絞れない

- 場所：`kernel/src/sys/log.c:214`（`kvlog`）、`kernel/src/sys/log.h:39-42`
- 分類：2
- 今：`enum log_level` は印を付けるだけで、全部を残します。
- 誤り：領域ごとの詳しさも、絞りもありません。
- 持ち主：利用者の選択（システムのプロセスの設定）。

### L-02 ログの輪の大きさが決め打ち

- 場所：`kernel/src/sys/log.c:86`、`:98-99`
- 分類：1
- 今：メモリーの 1/2048 です。
- 持ち主：システムのプロセス。

### L-03 シリアルの速度と出す出さないが決め打ち

- 場所：`kernel/src/dev/serial.c:53-55`、`:158`、`:208`
- 分類：1
- 今：SPCR が無ければ COM1 の 115200 bps です。
- 誤り：実機で切る口がありません。
- 誤り：速度を選ぶ口がありません。
- 持ち主：利用者の選択。

### L-04 panic の後は止まるだけ

- 場所：`kernel/src/sys/panic.c:47`、`:19`（`BACKTRACE_FRAMES` 10）
- 分類：1・2
- 今：止まったままです。
- 誤り：自動で再起動する選択も、記録をディスクへ残す選択もありません。
- 持ち主：利用者の選択。

### L-05 プロファイラーが起動の引数でしか入らない

- 場所：`kernel/src/sys/prof.c:27`、`:31`、`:274-278`
- 分類：1・4
- 今：動いている系で入れられません。
- 今：出力は log の行だけです。
- 持ち主：システムのプロセスと UI。

---

## 18. VM の処理

実機の方針を決めない範囲で、次を記録します。

### V-01 VM の GPU が起動の引数で入る

- 場所：`kernel/src/vm/virtio_gpu.c:240`、`:265`、`:579-580`、`:672`
- 分類：1・4
- 今：`gpu-enable` が無いと virgl を使いません。
- 持ち主：利用者の選択。

### V-02 VM の装置の定数

- 場所：`kernel/src/vm/e1000.c:185`、`kernel/src/vm/ac97.c:51-56`、`kernel/src/vm/hv/netvsc.c:40-48`、`kernel/src/vm/hv/storvsc.c:64`、`kernel/src/vm/hv/hvsock.c:22-25`
- 分類：1
- 持ち主：カーネルの機構。

### V-03 遠隔の音と入力の級が決め打ち

- 場所：`kernel/src/vm/rdp.c:1402`、`:1723`
- 分類：2
- 持ち主：システムのプロセス。

---

## 19. lib・common の残り

### R-01 字体が 2 種類に固定

- 場所：`common/gfx.h:38`、`common/unifont.asm`
- 分類：1
- 今：Unifont の小と大だけです。
- 誤り：カーネルの UI の字の大きさも字体も選べません。
- 持ち主：UI。

### R-02 名前の欄が 32 バイト

- 場所：`kernel/src/sys/process.c:84`、`common/abi.h:455-461`（`monitor[32]`、`name[32]`）、`kernel/src/dev/audio.h:98`（`name[32]`）
- 分類：1
- 今：プロセスの名も、出口の名も、モニターの名も 32 バイトで切ります。
- 持ち主：カーネルの機構。

### R-03 lib に設定の項目は無い

- 場所：`kernel/src/lib/`
- 今：`lz` の定数は書式の定数です。`idtable` の 16 は初めの大きさで、上限ではありません。
- 今：挙げる項目はありません。

---

## 20. 挙げなかった群

次はレジスターの番号、書式の定数、仕様の手順の値、または初めの大きさであり、挙げていません。

- `arch/` の `apic`、`gdt`、`isr`、`irq.h` の vector の番号、`fpu`、`pat`。
- `dev/ahci.c`、`dev/nvme.c`、`dev/xhci.c`、`dev/hda.c`、`net/rtl8125.c`、`dev/iommu_*.c` のレジスターの番号。
- `common/store_format.h` の書式の版と魔法の数、`common/store/` の木の深さ。
- `dev/audio_pace.h`（生成物）と `audio.c` の PACE の係数（内部の制御の値）。
- `common/edid.h` の確立タイミングの表（EDID の定義）。
- `arch/smp.c:32-34` の注釈が SDM の節を示す待ち時間。

---

## 21. 重い順の上位 20 件

1. W-01：窓の管理、タスクバー、メニュー、壁紙がカーネルの中にあり、UI・システムのプロセス・カーネルの境界が無い。
2. S-01：設定という object が無く、列挙・型・適用の口が領域ごとに別か、そもそも無い。
3. S-03・S-04：設定の実体がカーネルの解析するテキストの文書で、利用者とアプリが書ける場所にある。
4. A-01・A-02：音の出口の形式を 48 kHz の一組に丸め、装置が申告する率と幅の一覧を捨てる。
5. A-06・A-08：音の出口の一覧と選択に syscall も呼び手も無く、選択を保存もしない。
6. A-05・A-04：音のストリームの ABI と混合が 48 kHz・16 bit・2 チャンネルに固定。
7. B-01・O-01：DMA の隔離やドライバーの権限を含む安全の方針が起動の引数でしか変わらない。
8. N-01・N-02・N-03：IP・DNS・TCP の設定を動いている系で変える口が無く、起動の引数か DHCP だけ。
9. N-04・N-05：網の adapter が一つだけで、実機の NIC ドライバーは一機種の一枚だけ。
10. I-01・I-02・I-03：キーボード配列が US の表で、物理のキーと文字が混ざり、ASCII 以外を捨てる。
11. T-01・T-02：時差が無く、実機で日付と時刻を設定する口が無い。
12. F-01：利用者が C:\Users の最初のフォルダーで決まり、カーネルの中に一人しかいない。
13. C-01・C-03：CPU の性能の好みを変える関数に呼び手が無く、idle は C1 だけ。
14. E-01・E-02・E-03・E-04：スリープと休止が ABI に無く、電源ボタンが決め打ちで、蓋・温度・電池を見ない。
15. W-09・W-10：画面のモードに色深度と色の形式が無く、VRR の入り切りを選べない。
16. W-02・W-03・W-04：アプリの一覧、英語の文言、見た目の寸法と色がカーネルの定数。
17. W-06・W-07・W-08：DPI が無く、画面の配置は左から右に固定で、主画面は screen 0 に固定。
18. P-01・P-02・P-03・P-07：他のアプリの優先度を誰も変えられず、重みも要求の長さも CPU の集合も選べない。
19. G-01：実機の GPU が GPU の ABI に出ず、複数の GPU を一覧にも選択にも載せられない。
20. D-01・D-04・F-04・X-01・X-02：NVMe の深さを 64 に切り、省電力を切り、キャッシュは 1/16 に固定で、ReBAR と PCIe の調整を装置に問わない。
