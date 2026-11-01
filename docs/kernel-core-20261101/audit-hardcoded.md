# 決め打ちの値と、その裏で欠けている層：敵対的な監査

## 依頼文（そのまま）

````
あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機、GUI が先）の敵対的な監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。main の今の版（5043e3e6 以降）を読む。あなたの仕事は「このコードは決め打ちしていない」という主張を壊すこと。合格を探さず、欠けを探す。見つからないと言うなら、どこをどう探したかを書く。

# 持ち主の言葉（そのまま）
"we apply overship haphazardly to things, with out finding out what is normal. and we also have like verylittle real support for settings, overall."
"we lack a proper settings system. and stuff like this shouldn't be too hardcoded. we should be carefull, and maybe do an adversarial audit first of what we are missing."
AGENTS.md の規則：決め打ちの値、小さな固定の上限、一つの環境の都合を別の部品へ持ち込むことは禁止。ハードウェアの値は装置の申告と公式の文書から取る。

# 読む所
`kernel/src/` の全部（特に `gui/`、`dev/`、`sys/`、`arch/`、`net/`、`fs/`、`mm/`）、`common/`、`user/apps/`（`init.c`、`settings.ts`、デスクトップまわり）、`user/libc/gui.c`・`ui.c`・`media*.c`、`user/libc/web/` の窓と媒体の部分、`ports/nvidia`。地図は `.claude/rules/map.md`。`#define`、`static const`、数の直書き、文字列の直書きを rg で掘る。

# 分ける形
見つけた値を次の四つに分ける：
A. 利用者の設定であるべき値（例：キーの repeat、指の速さ、拡大率、音量の刻み、窓の見た目、時刻の形、字体の大きさ、既定の解像度の選び方）。
B. 装置の申告や測りから取るべき値（例：周波数、画面の大きさ、バッファーの数、queue の深さ、timeout のうち仕様が装置の報告を定めるもの）。出典（仕様書は `D:\Programes\OS DEV INFO`）。
C. 仕様が定める定数で、決め打ちでよい値（出典を書く。ここに入れる値は疑って、本当に仕様の値かを照らす）。
D. 小さな固定の上限で、規模で壊れる値（600 Hz、3 枚の画面、論理 CPU 1024、多くのアプリと窓）。

A・B・D は全部、ファイル:行、今の値、壊れ方、正しい持ち主を書く。重さ（毎日当たる／当たれば壊れる／きれいさ）を付ける。一コマの道（合成・入力・音・時間）に載る値を先に書く。

# 報告
`docs/kernel-core-20261101/audit-hardcoded.md` へ日本語で全文（この依頼文を先頭に）。返事にも要点を日本語で返す。サブエージェントをさらに出さない。Python で書くなら `newline='\n'`。
````

調整役が途中で足した指示（そのまま）：

````
持ち主の言葉："NO, you are not understanding. WE AREMISSING ABSTRACTION THAT NEEDS TO BE THERE. WE HAVE NO PROPER SETTINS SYSTEM ACROSS THE"（ここで切れている）

決め打ちの値の一覧だけで終えない。各値に「どの抽象化の層が無いから決め打ちになっているか」を付け、層ごとにまとめて先頭に置く。OS 全体の設定の層のほかに、画面と GPU の能力の問い合わせ、資源の上限の持ち方など、値の裏にある欠けた層を名指しする。層の名と、その層があれば消える値の数を書く。
````

## 1. 結論

「決め打ちしていない」という主張は成り立たない。読んだ範囲で 91 行、538 個の決め打ちの値を挙げた（色は 1 個ずつ数えた）。
この値の大半の原因は値そのものではなく、欠けた層である。層を 14 個名指しする。537 個が層に割り当たり、層に割り当たらない欠陥は 1 個である。

- OS 全体の設定の層は無い。システムの値は 5 種で、窓の頁は 3 個である。
- 分類の内訳は、A を含む値が 393 個、B を含み A を含まない値が 74 個、D だけの値が 68 個である。
- 設定の層（L1）を主とする値は 41 個である。A の 393 個は主な層が別でも、L1 が無いと窓に届かない。L1 は全部の A の土台である。
- 一番多く消えるのは外観の層（L2）の 264 個である。寸法が物理の画素なので、4K の画面の UI は同じ寸法の 1080p の画面の半分の大きさになる。
- 次は資源の上限の層（L8）の 56 個である。規則と問い合わせの口が無いので、ファイルごとに定数を発明する。
- その次はアプリの登録の層（L10）の 42 個である。カーネルがプログラムの一覧を持つ。
- 3 枚の画面は実機で成り立たない。カーネルは複数の画面を持つのに、`ports/nvidia/display.c` は一枚に縮める。
- VRR は実機で動かない。立てる driver が 0 個である。
- 実機で音が出ない。時間帯も NTP も無い。実機の壁時計を合わせる者がいない。
- 600 Hz の画面でも UI のアニメーションは 62.5 Hz である（`user/libc/ui.c:2833`）。

証拠の範囲は静的な読みだけである。ビルド、QEMU、実機を動かしていない。読んだ版は 5043e3e6 である。e67f52ed までの差は `docs/` だけである。行番号は 5043e3e6 の原文と機械で照らした。

## 2. 欠けている層

「消える値」は、決め打ちの定数でなくなる値の数である。既定値として記述子に残るものも数える。各行は主な層一つで数えるので、列の和が総数である。「関わる値」は、その層が主ではないが通る行の値の数である。

| 層 | 名 | 消える値 | 関わる値 | 行の数 | 毎日当たる行 |
|---|---|---:|---:|---:|---:|
| L1 | OS 全体の設定の層 | 41 | 25 | 14 | 4 |
| L2 | 外観の層（テーマ・字体・拡大率） | 264 | 10 | 5 | 4 |
| L3 | 画面の能力と配置の層 | 30 | 51 | 12 | 5 |
| L4 | GPU と表示エンジンの能力の問い合わせの層 | 18 | 11 | 5 | 3 |
| L5 | 入力の層（方針・配列・装置の能力） | 14 | 11 | 6 | 4 |
| L6 | 時刻と地域の層 | 10 | 2 | 3 | 3 |
| L7 | 音の装置の能力と経路の層 | 16 | 1 | 6 | 4 |
| L8 | 資源の上限の持ち方の層 | 56 | 3 | 25 | 3 |
| L9 | 装置と driver の結び付けの層 | 7 | 0 | 2 | 0 |
| L10 | アプリの登録の層 | 42 | 0 | 2 | 2 |
| L11 | 電源と方針の層 | 8 | 0 | 2 | 1 |
| L12 | 利用者とセッションの層 | 3 | 0 | 1 | 0 |
| L13 | 網の口の層 | 12 | 1 | 4 | 0 |
| L14 | Web へ利用者の選択を渡す経路 | 16 | 3 | 3 | 3 |
| L0 | 層で解けない一か所の欠陥 | 1 | 0 | 1 | 0 |
| 計 | | 538 | | 91 | 36 |

層の間の順序は次のとおりである。

- L1 が土台である。L2、L5 の方針、L6、L10 のショートカット、L11、L13 の設定、L14 は L1 に鍵を登録する。
- L3、L4、L7 は装置の申告の層である。L1 の「装置ごとの値」は、この層が作る装置の身元を要る。
- L12 は L1 の scope「利用者」に実体を与える。
- L8 は L1 と独立である。設定にしてはならない値が多い。利用者に上限を選ばせず、申告から計算する。

### L1 OS 全体の設定の層（消える値 41）

今の形：`sys/keep.c` は保存だけで、鍵を知らない。使い手は 3 個（`dev/audio.c`、`gui/screenmode.c`、`gui/wm.c`）である。システムの値は 5 種（`pointer_speed`、`smooth_gradients`、`volume`、`muted`、`mode.<monitor>`）だけである。輸送は持ち主ごとに別である（`SYS_SCREEN_INFO` の第 3 引数の副命令と sizeof の多重化、音は `SYS_AUDIO_OUTPUT`）。C のプログラムに libc の包みが無い（`js.c` が生の `__syscall` を呼ぶ）。記述子、発見、既定の表、既定へ戻す、装置ごとの範囲が無い。driver とアプリは層に載れない。

要る形：持ち主は値を持ったままとする。層が持つのは契約である：記述子（持ち主・鍵・型・範囲・既定・見出し）の公開、発見・取得・変更・見張りの一つの ABI、記述子から作る窓。部品の案は `audit-settings-missing.md` §2 に書いてある。`docs/settings-ownership-20261027.md` の決定（値を効かせる者が持ち主、共有の文書を作らない）と両立する。

当たる行：G6、G20、G24、I2、I4、I7、S7、M3、D9、P3、C1、C2、C3、C4

### L2 外観の層（テーマ・字体・拡大率）（消える値 264）

今の形：`user/include/ui.h:26-33` のテーマはコンパイル時の `const` である。`gui/wm.c` と `gui/desktop.c` は別に色を直書きする。拡大率の概念がどこにも無い。字体の大きさの設定も無い。寸法は全部が物理の画素である。

要る形：外観の持ち主を一つ置き、色・寸法・字体の token と拡大率を L1 に登録する。カーネルの `wm.c` とアプリの `ui.c` は同じ token を読み、窓の事象で変化を受ける。

当たる行：G8、G9、G10、G11、M6

### L3 画面の能力と配置の層（消える値 30）

今の形：`struct fb_info fb` は大域に一つで、色の深さが全画面で一つである。画面の物理の大きさを読まない。`SYS_SCREEN_INFO` に画面の番号が無い。配置は左から番号の順で、主画面も回転も無い。UI の時計は窓の周期を見ない。

要る形：画面ごとの記述（物理の大きさ、拡大率、色の深さ、色域、HDR、周波数の範囲、配置）を `gui/screenmode.c` が持ち、装置の身元ごとに保存し、番号つきの ABI で全部の層へ出す。

当たる行：G1、G2、G3、G7、G15、G16、G17、G18、G21、G22、I12、W2

### L4 GPU と表示エンジンの能力の問い合わせの層（消える値 18）

今の形：表示の契約は `n_screen_modes` だけである。`common/gpu_abi.h` の backend は VIRGL と CPU だけで、実機の GPU を問い合わせる口が無い。`ports/nvidia/display.c` は指の大きさなどの能力を読むのに、OS へ渡さず、出力も一つに縮める。VRR を立てる driver は 0 個である。

要る形：`display_abi.h` と `gpu_abi.h` に能力の記述を足す（出力の数、指の最大、バッファーの数、形式の一覧、VRR の範囲、HDR）。`nvrm` が申告し、次の driver も同じ形で申告する。

当たる行：G4、G5、G14、D1、D10

### L5 入力の層（方針・配列・装置の能力）（消える値 14）

今の形：repeat は `dev/usbhid.c` に、配列は `dev/input.c` に、指の曲線は `gui/wm.c` にある。key の ABI に Unicode が無い。指の曲線は poll 率で変わる。ボタンは 3 個に切られる。入力の装置ごとの設定が無い。

要る形：`dev/input.c` を入力の層にする。driver は usage と時刻だけを渡す。repeat、配列、曲線は層が持ち、装置の身元ごとに L1 に登録する。

当たる行：I1、I3、I5、I6、I9、I10

### L6 時刻と地域の層（消える値 10）

今の形：時間帯が無い（`localtime_r` は `gmtime_r` そのものである）。NTP が無い（`clock_set_wall` の呼び手は Hyper-V の手当てだけである）。言語が無い（`settings.ts` は英語、`browser_settings.h` は日本語の直書き）。時刻の形が無い。

要る形：時刻帯と言語と形の持ち主を一つ置き、L1 に登録する。libc、QuickJS、`wm.c` は同じ口を読む。NTP は網の上の service とする。

当たる行：T1、T2、T3

### L7 音の装置の能力と経路の層（消える値 16）

今の形：実機の音の driver が無い（`audio.c:770`）。48 kHz・stereo・S16 が ABI に固定されている。問い合わせる口が無い。遅れの値が AC'97 の輪から来ている。出力の選択とアプリごとの音量が無い。

要る形：`dev/audio.c` に装置の抽象を置く：能力（rate、channels、形式、周期）の問い合わせ、出力の一覧、経路、流れごとの音量。driver（HD Audio、USB Audio）は同じ口で申告する。

当たる行：S1、S2、S3、S4、S6、M5

### L8 資源の上限の持ち方の層（消える値 56）

今の形：上限はファイルごとの定数である。同じ判断（`channels > 8`、`rate > 384000`）が 6 か所に重複する。良い例は既にある（`log.c:98` は RAM と CPU から、`channel.c:196-202` は空きメモリーから、`object.c:93` は上限なし）。規則と共通の口が無いので、良い例が広がらない。アプリごとの割当も無い。

要る形：二つを定める。規則：上限は「装置の申告・メモリー・CPU の数・仕様の値」のどれかから計算し、計算できない固定の配列を置かない。口：CPU の数、node、空きメモリーを問い合わせる ABI と、アプリごとの資源の勘定。POSIX の rlimit の形を写さない。

当たる行：G12、G13、G19、G23、I11、I13、S5、M1、M2、M4、K1、K2、D4、D5、D6、D7、D8、D11、N4、N6、F2、F3、F4、F5、W5

### L9 装置と driver の結び付けの層（消える値 7）

今の形：`pci_find` は一つの id の最初の一枚を返す。`kernel/src/init.c:40-48` は呼ぶ順の直書きである。`user/apps/init.c:133-145` は NVIDIA だけを起こす。PCI は segment 0 だけである。

要る形：driver が class・vendor・device の表を登録し、列挙が全部の装置へ結ぶ。アプリの driver の選び方も同じ表から引く。

当たる行：D2、D3

### L10 アプリの登録の層（消える値 42）

今の形：カーネルの `wm.c:1047-1072` がプログラムの一覧を持つ。デスクトップのアイコンは番号の配列である。ファイルの関連付けとショートカットの登録が無い。

要る形：アプリが自分の記述（名前、アイコン、開ける形式、ショートカット）を置き、system service が読み、メニューは一覧を受け取る。カーネルから方針を出す。

当たる行：I8、R1

### L11 電源と方針の層（消える値 8）

今の形：電源ボタンは切るだけである。`cpu_perf_set_policy` に呼び手が無い。画面を消す時間と自動スリープの持ち主が無い。

要る形：`dev/pm.c` を方針の持ち主にし、L1 に登録し、窓を作る。

当たる行：P1、P2

### L12 利用者とセッションの層（消える値 3）

今の形：`fs/store.c:405-412` は `Users` の最初のフォルダーを唯一の利用者とする。`sys/keep.c:89-94` の保存先もこれに従う。ログインと切替が無い。

要る形：利用者の表とセッションを持ち主として置く。L1 の scope の「利用者」はこの層から実体を受ける。

当たる行：F1

### L13 網の口の層（消える値 12）

今の形：`net/net.c:58` の `nif` は大域に一つで、二つ目の口を無視する。MTU は `TCP_MSS` の直書きである。DHCP の option 26 を読まない。静的な IP と DNS の設定が無い。IPv6 が無い。

要る形：`struct netif` を口ごとの状態にし、ABI に番号を足す。MTU は driver と DHCP から取る。口ごとの設定は L1 に登録する。

当たる行：N1、N2、N3、N5

### L14 Web へ利用者の選択を渡す経路（消える値 16）

今の形：`user/libc/web/css.c:5160-5171` は全部の user preference の media feature を固定値で答える。`webprefs` は言語だけで、OS の明暗、動きの抑え、字の大きさ、拡大がブラウザーへ届かない。

要る形：L1 と L2 の値をブラウザーが見張り、`css.c` の media feature、`js_screen.js`、`webprefs` へ通す。変化は `change` の事象まで通す。

当たる行：W1、W3、W4

### L0 層で解けない一か所の欠陥（消える値 1）

今の形：欠けた層ではなく、その一か所の誤りである。

要る形：その場で直す。

当たる行：T4

## 3. 一コマの道に載る値

分類は A（利用者の設定）、B（装置の申告や測り）、D（小さな固定の上限）である。層の列の括弧は、関わるほかの層である。

### 合成

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| G1 | L3 | B | 毎日当たる | `user/libc/ui.c:2833` | `w->now + 16`（16 ms） | UI のアニメーションが 62.5 Hz で止まる。240 Hz でも 600 Hz でも同じである。`ui.c:2653` は `EV_FRAME` の周期を受け取っているのに使わない。 | 窓の周期（`EV_FRAME`） |
| G2 | L3（L4） | D・B | 毎日当たる | `common/abi.h:485,495,700`<br>`kernel/src/gui/wm.c:55,390-391`<br>`ports/nvidia/display.c:627-634,99` | `N_SCREEN_POINTER_SIZE` 32、`N_POINTER_IMAGE_MAX`、`POINTER_SIZE`、`CURSOR_MIN_PITCH` 256 | 指の像が 32×32 に固定される。4K で指が小さい。`display.c` は `caps.maxCursorSizeInPixels` を装置から読むのに、32 が通るかだけを見る。 | 表示エンジンの申告の中で、拡大率と利用者の大きさから決める |
| G3 | L3（L4） | B | 毎日当たる | `kernel/src/dev/fb.h:15-18`<br>`kernel/src/gui/wm.c:1916,3563-3567`<br>`ports/nvidia/display.c:1105,1112` | 大域の `fb.deep`・`fb.bpc`、`N_SCANOUT_XRGB8888` 固定 | 色の深さが全画面で一つである。NVIDIA は常に 8 bit である。10 bit と HDR の画面が 8 bit で動く。 | 画面ごとの能力と、表示エンジンの申告 |
| G4 | L4 | B | 毎日当たる | `ports/nvidia/display.c:336-378,1056,1193,1548` | `chosen` 一つ、`.output = 0`、`if (flips[i].output == 0)`、大域の `scr` 一つ | カーネルは複数の画面を持つのに、実機の NVIDIA は一枚だけを出す。3 枚の画面が実機で成り立たない。 | `nvrm` の出力の表（head ごとの状態） |
| G5 | L4（L1） | B | 毎日当たる | `kernel/src/gui/screenmode.c:241,327`<br>`kernel/src/gui/wm.c:2054` | `N_SCREEN_MODE_VRR`・`vrr_min_mhz` を立てる driver が 0 個 | ABI と合成の分岐だけがあり、実機で VRR が動かない。`git grep` で main の木を探して、値を設定する行は無かった。 | 表示エンジンの申告（`nvrm`）と、画面ごとの VRR の設定 |
| G6 | L1（L3） | A | 毎日当たる | `kernel/src/gui/screenmode.c:155-168` | `pick`：EDID の preferred が既定 | 高い周波数の画面が既定で最高の周波数にならない。選び方を変える設定が無い。 | `screenmode` が登録する設定 |
| G7 | L3（L1） | A | 毎日当たる | `kernel/src/gui/wm.c:2000,562,1630,1762,1908` | 左から番号の順の配置、taskbar とアイコンは画面 0 だけ | 主画面、配置、回転を変えられない。画面の番号が変わると見た目が移る。 | 画面の配置の層（装置の身元ごとの値） |
| G8 | L2（L3） | A・B | 毎日当たる | `kernel/src/gui/wm.c:50-57,575-577,603,1093-1094,1238-1240,1314-1318,1437,1487,1496,2437,2440,2647-2648,3229-3230` | `TITLE_H` 28、`TASKBAR_H` 40、`MENU_W` 260、`ICON_CELL` 88×82、`TB_START_W` 128、`VOL_W` 300、toast 320×64 ほか、画素の直書き 34 個 | 4K の窓の枠・タスクバー・メニューは、同じ寸法の 1080p の画面の半分の大きさになる。拡大率の概念が無い。 | 外観の層（拡大率×寸法の token） |
| G9 | L2 | A | 毎日当たる | `kernel/src/gui/desktop.c:35-37,76-77,118` | `wm.c` の色 67 個、`desktop.c` の色 126 個（`grep -oE` で数えた） | 明暗も強調色も変えられない。壁紙は手続きの絵一つで選べない。 | 外観の層（色の token と壁紙） |
| G10 | L2 | A | 毎日当たる | `user/include/ui.h:26-33`<br>`user/libc/ui.c:189` | テーマはコンパイル時の `const`（色 20、大きさ 6、`fontSize` 14 ほか） | アプリの UI の見た目が組むときに固定される。カーネルの色と別の表なので揃わない。 | 外観の層 |
| G11 | L2（L3） | A | 毎日当たる | `common/gfx.c:271-272`<br>`user/libc/font.c:164,299-320`<br>`user/apps/term.c:8-9` | bitmap 8×16 と 16×32、`C:\Nocturne\Fonts`、字体のファイル名、`CW` 8・`CH` 16 | 字体と大きさを変える設定が無い。4K で字が小さい。 | 外観の層（字体と大きさ） |
| G12 | L8 | D | 毎日当たる | `user/apps/term.c:10-12,324-325` | `MAXC` 240、`MAXR` 120、`SBLINES` 1000 | 4K で最大化すると 480 桁のうち 240 桁だけを使う。 | 窓の大きさとメモリーから決める |
| G13 | L8 | D | 毎日当たる | `kernel/src/gui/wm.c:308,327,336,348,492,3184,3500` | 窓の題 `title[64]`、toast 64 と 128 | 日本語の題は 21 字で切れる。 | 文字列の長さを持つ確保 |
| G14 | L4 | B | 当たれば壊れる | `common/abi.h:414`<br>`ports/nvidia/display.c:97` | `N_SCREEN_BUFFERS_MAX` 4、`DESK_BUFFERS` 2 | バッファーの数を装置の申告と遅れの方針から決められない。 | 表示エンジンの申告 |
| G15 | L3 | B | 当たれば壊れる | `common/edid.h:74`<br>`kernel/src/dev/fb.c:51` | `edid_modes` は基本ブロックだけ、`modes[32]` | CTA-861 と DisplayID の mode を読まない。物理の大きさ（基本ブロックの byte 21-22）も読まない。高い周波数の mode が拡張ブロックにある画面で選べない。 | EDID の読み手 |
| G16 | L3 | D | 当たれば壊れる | `common/abi.h:462`<br>`ports/nvidia/display.c:220,453`<br>`kernel/src/gui/screenmode.c:37,266` | `N_SCREEN_MODES_MAX` 128 | 超えた分は来た順に捨てる。高い周波数の mode が落ちる。 | 申告の数に従う確保 |
| G17 | L3 | B | 当たれば壊れる | `kernel/src/gui/wm.c:3380,3560-3561` | `screen_grab` と `SYS_SCREEN_INFO` の screen_format は画面 0 だけ、`sh - TASKBAR_H` | ABI に画面の番号が無い。アプリは二枚目の大きさを知れない。 | 画面の問い合わせの ABI |
| G18 | L3 | D | 当たれば壊れる | `kernel/src/gui/wm.c:2895-2896` | 遠隔の大きさ 640..3840 × 480..2160 | 4K より大きい遠隔の画面を 3840×2160 に切り詰める。RDP の都合が `wm.c` にある。 | `vm/` の RDP |
| G19 | L8 | D | 当たれば壊れる | `kernel/src/gui/wm.h:21`<br>`kernel/src/gui/wm.c:2800,3576` | `WM_CLIPBOARD_MAX` 1 MiB | 大きな文字列の貼り付けが切れる。文字だけで、型を持たない。 | クリップボードの持ち主（文書の型とメモリー） |
| G20 | L1 | A | きれいさ | `user/libc/ui.c:2274,2376,2496,3325,2065` | caret 530 ms、menu 200 ms | 点滅とメニューの遅れを変えられない。 | 設定の層（UI の時間） |
| G21 | L3 | B | きれいさ | `user/libc/ui.c:1434,2834` | spinner 33 ms | 周期と無関係に動く。 | 窓の周期 |
| G22 | L3 | D | きれいさ | `common/abi.h:635,638`<br>`user/libc/gui.c:76` | `N_WIN_SURFACES_MAX` 8、`N_WIN_SIZE_MAX` 16384、面の数 3 か 2 | 面の数を周期と遅れの方針から決められない。8K の二倍で窓の大きさが止まる。 | 窓の面の持ち主 |
| G23 | L8 | D | きれいさ | `kernel/src/gui/wm.c:58,866` | `TILE` 32、`RECEIVE_BATCH` 16 | 画面の大きさに従わない。 | `wm.c` の内部（画面の大きさから計算） |
| G24 | L1 | A | きれいさ | `kernel/src/gui/wm.c:491`<br>`common/display_abi.h:67` | toast 4 s、`SCREEN_MODE_TRIAL_MS` 15000 | 時間を変えられない。 | 設定の層 |

### 入力

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| I1 | L5（L1） | A | 毎日当たる | `kernel/src/dev/usbhid.c:18-19` | `REPEAT_DELAY` 500 ms、`REPEAT_RATE` 33 ms | キーの repeat を変えられない。USB の driver の中にあり、ほかの入力の口と共有しない。 | 入力の層（repeat は装置の driver の外） |
| I2 | L1（L5） | A | 毎日当たる | `kernel/src/gui/wm.c:486,2448`<br>`user/apps/files.c:20` | 二度押し 450 ms と 6 px、`DOUBLE_MS` 400 | 二度押しの時間が二つの値に割れている。変えられない。 | 入力の層が登録する設定 |
| I3 | L5（L1） | A・B | 毎日当たる | `kernel/src/gui/wm.c:2606-2607` | `if (ABS(dx) > 5) dx *= 2` | report ごとの閾値なので、mouse の poll 率（125 Hz と 8000 Hz）で曲線が変わる。dx 5 は 5 で、dx 6 は 12 になる。 | 入力の層（時間で割った速さの曲線） |
| I4 | L1（L5） | A | 毎日当たる | `kernel/src/gui/wm.c:2609` | quarters {2,3,4,6,8} | 指の速さが 5 段だけである。 | 入力の層が登録する設定 |
| I5 | L5 | B | 毎日当たる | `kernel/src/dev/usbhid.c:232,56` | `e.buttons = buttons & 7` | `usbhid.c:56` は 8 個解析するのに、4・5 番目（戻る・進む）を捨てる。 | 入力の事象の ABI |
| I6 | L5（L6） | A | 毎日当たる | `kernel/src/dev/input.c:15-30` | `map_norm`・`map_shift`（US 配列） | JIS と欧州の配列で記号が違う。`usbhid.c` の `[0x87]` の JIS のキーは `KEY_UNKNOWN` になる。key の ABI に Unicode が無く、USB usage から scan code set 1 を経て文字へ遠回りする。 | 入力の層（配列を装置ごとの設定に） |
| I7 | L1（L5） | A | 毎日当たる | `kernel/src/gui/wm.c:2696,1398`<br>`user/apps/settings.ts:245`<br>`user/libc/ui.c:2563,2582`<br>`user/apps/browser.c:18,3267` | wheel の刻み 5、tone 880 Hz 70 ms、wheel×48、`LINE` 48、`POINTER_SETTLE_MS` 100 | wheel の行の数を変えられない。 | 設定の層（wheel の刻み） |
| I8 | L10 | A | 毎日当たる | `kernel/src/gui/wm.c:2334-2348` | Super、Ctrl+Esc、Alt+Tab、Alt+F4、Ctrl+Alt+T | ショートカットを変えられない。アプリが登録できない。 | アプリの登録の層と入力の層 |
| I9 | L5 | B | 当たれば壊れる | `kernel/src/dev/usbhid.c:357,68-80,301-303` | wheel は usage `0x00010038` だけ、keyboard page だけ | AC Pan と Resolution Multiplier（`hut1_12v2.pdf` 4.3.1）を読まない。Consumer page の音量キーを読まない。 | HID の解析 |
| I10 | L5 | D | 当たれば壊れる | `kernel/src/dev/usbhid.c:47,314,283,289,388` | `keys[16]`（黙って捨てる）、`usages[16]`、`offset[256]`、stack 4 | 報告の記述子が大きい装置で項目を失う。 | HID の解析（記述子の長さに従う確保） |
| I11 | L8 | D | 当たれば壊れる | `kernel/src/gui/wm.c:2206,2250` | `INQ` 1024（静的）、`in_lost++` | 満ちると押す事象を黙って失う。 | `wm.c` の入力の列 |
| I12 | L3（L5） | B | 当たれば壊れる | `kernel/src/dev/input.h:28`<br>`kernel/src/gui/wm.c:2602-2603` | 絶対の指は画面 0 だけ | タブレットとタッチの装置が二枚目に届かない。 | 画面の配置の層 |
| I13 | L8 | D | きれいさ | `kernel/src/gui/wm.c:62` | `MAILBOX_MAX` 4096 | mouse move は合わさり、満ちたら `EV_INPUT_LOST` を出す。正しく働く。値は窓の数と無関係である。 | 窓の受け箱 |

### 音

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| S1 | L7 | B | 毎日当たる | `kernel/src/dev/audio.c:770` | `vm_audio_init(); /* no driver for real sound hardware yet */` | 実機で音が出ない。出力の選択、アプリごとの音量も無い。 | HD Audio と USB Audio の driver（`D:\Programes\OS DEV INFO` に `high-definition-audio-specification.pdf` がある） |
| S2 | L7 | B | 毎日当たる | `common/abi.h:569`<br>`kernel/src/dev/audio.h:21`<br>`user/libc/media_rate.c:36,42,73`<br>`user/libc/web/webaudio.h:20-22` | `N_AUDIO_RATE` 48000、stereo、S16、`WA_OUTPUT_RATE`、`WA_OUTPUT_CHANNELS` 2、`WA_OUTPUT_TARGET` 2048 | 装置の能力を問い合わせる口が無い。192 kHz と 5.1 の出口を使えない。 | 音の装置の能力の層 |
| S3 | L7 | B | 毎日当たる | `kernel/src/dev/audio.c:19-22,93-98` | `QUEUE_BYTES` 170 ms、`RING_BYTES`、`RUNS` 32、`NULL_BLOCK`、`audio_trace_ac97` の 144 | 遅れが装置の周期から決まらない。`RUNS` の根拠は AC'97 の輪で、VM の装置の都合が共通のコードにある。 | 音の装置の能力の層 |
| S4 | L7 | B | 毎日当たる | `user/libc/web/avmedia.c:151` | `WEB_AUDIO_TARGET` 7200（150 ms） | 映像と音の遅れが装置の遅れを見ない。 | 音の問い合わせの ABI |
| S5 | L8（L7） | D | 当たれば壊れる | `kernel/src/dev/audio.c:18,685` | `MAX_STREAMS` 8、`N_ERR_BUSY` | ゲーム、動画、通話、多くのタブで 9 本目が拒まれる。 | 音の合成（流れの数に従う確保） |
| S6 | L7 | D | 当たれば壊れる | `user/libc/media.c:164,270,726`<br>`user/libc/media_adaptive.c:125`<br>`user/libc/media_mse_worker.c:118`<br>`user/libc/media_worker.c:113` | channels > 8、rate > 384000 を拒む | 同じ判定が 6 か所に重複する。値の根拠が音の層に無い。 | 音の装置の能力の層 |
| S7 | L1 | A | きれいさ | `kernel/src/dev/audio.c:68,70` | volume 既定 80、v² の曲線 | 既定と曲線を変えられない。 | 設定の層 |

### 時間

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| T1 | L6 | A | 毎日当たる | `user/libc/time.c:62`<br>`third_party/quickjs/quickjs.c:48993-48998`<br>`kernel/src/gui/wm.c:1467-1468,1784,2739`<br>`kernel/src/dev/refclock.c:249` | `localtime_r` は `gmtime_r` と同じ、`getTimezoneOffset` 0、タスクバーは壁時計そのまま、RTC を UTC として読む | 時間帯の層が無い。RTC が地方時（Windows と併用）なら壁時計が UTC からずれ、TLS の証明書の時刻、Cookie の期限、HTTP の日付を誤る。RTC が UTC ならタスクバーは UTC を出す。 | 時刻と地域の層 |
| T2 | L6（L13） | A・B | 毎日当たる | `kernel/src/vm/hv/hv_util.c:137` | `clock_set_wall` の呼び手はこの行だけ | 実機で時刻を合わせる者が無い。`net.c` と `init.c` に NTP が無い。 | 時刻と地域の層（NTP の service） |
| T3 | L6 | A | 毎日当たる | `kernel/src/gui/wm.c:1465-1468,1048-1069,3056`<br>`user/apps/clock.c:29-30`<br>`user/apps/date.c:6`<br>`user/apps/browser_settings.h:19-20` | 24 時間表示、英語の曜日と月、システムの文字は英語、ブラウザーの設定は日本語の直書き | 時刻の形と言語を変えられない。文字列の表が無い。 | 時刻と地域の層 |
| T4 | L0 | B | 当たれば壊れる | `kernel/src/arch/cpu.c:147-148,154-164`<br>`kernel/src/sys/clock.c:799-801` | FID[7:0]・DID[13:8] を family を見ずに読む | 下限は「Zen 4 以上」なのに family の分岐が無い。合わないと起動が panic する。PPR 56713 と 55898 は Family 19h で読める。Family 1Ah の PPR は手元に無く、書式が同じかは分からない。 | `arch/cpu.c` |

### 媒体

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| M1 | L8 | B | 毎日当たる | `user/libc/media.c:126` | `c->thread_count = 1; c->thread_type = 0;` | 4K のソフトの復号が CPU の数を見ない。1024 CPU でも一本である。 | CPU の数と topology の問い合わせ |
| M2 | L8 | D | 当たれば壊れる | `user/include/media_adaptive_private.h:5-8`<br>`user/libc/media_adaptive.c:155` | manifest 256 KiB、区間 8 MiB、init 1 MiB、2048 区間 | 高いビットレートの区間と長い配信を拒む。 | メモリーと buffer の方針 |
| M3 | L1 | A・B | 当たれば壊れる | `user/libc/media_http.c:14` | 5000 ms | 遅い回線で媒体の取得が止まる。 | 網の方針（設定か応答の時間から） |
| M4 | L8 | D | きれいさ | `user/include/media_mse_worker_private.h:43` | `NMSW_VIDEO_SLOTS` 2 | slot の数を遅れの方針から決められない。 | 媒体の worker |
| M5 | L7 | B | きれいさ | `user/libc/media_mse_worker.c:257-261` | 16 ms の見回り | 根拠が 48 kHz の決め打ちである。 | 音の装置の能力の層 |
| M6 | L2 | A | きれいさ | `user/apps/mediaplayer.c:15-20` | `WIDTH` 768、`HEIGHT` 584、`PAD` 12、`VIDEO_H` 408、`SEEK_PX` 500 | 窓の寸法が拡大率を見ない。 | 外観の層 |

### スケジューリング

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| K1 | L8 | D | 当たれば壊れる | `kernel/src/sys/thread.c:202,220-228` | `APP_DEADLINE_SHARE`：CPU 一つの半分を等分 | 枠が CPU の数に従わない。1024 CPU でも同じである。多 thread のゲームは thread ごとの枠が小さくなる。 | アプリごとの割当（CPU の数から） |
| K2 | L8 | D | きれいさ | `kernel/src/sys/sched.h:50-59`<br>`kernel/src/sys/sched.c:80-86` | `SCHED_WEIGHT_MAX`、`SCHED_REQUEST_MAX_NS`、`SCHED_PERIOD_MAX_NS`、`DL_SHARE_SHIFT`、`DL_DECAY_SHIFT` | 方針の定数である。規模では壊れない。 | `sys/sched.c` |

## 4. そのほかの A・B・D

### 装置

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| D1 | L4 | B | 毎日当たる | `common/gpu_abi.h:6-9,16-18,54` | `Source compatibility only` の 512 と 32、backend は VIRGL と CPU だけ、`struct n_gpu_info` | 実機の GPU の問い合わせの口が無い。アプリは RTX 4070 の能力を ABI で知れない。AGENTS の「互換は持たない」に反する。 | GPU の能力の層 |
| D2 | L9 | B・D | 当たれば壊れる | `kernel/src/dev/pci.c:270`<br>`kernel/src/net/rtl8125.c:534,556-557`<br>`user/apps/init.c:9,133-145`<br>`ports/nvidia/include/nvrm.h:33`<br>`kernel/src/init.c:40-48` | `pci_find` で 0x10EC:0x8125 の最初の一つ、RTL8125B だけ、`NVRM` の直書き、`nvrm_pci_is_gpu`、呼ぶ順の直書き | 同じ NIC の二枚目が動かない。AMD と Intel の GPU の道が無い。新しい driver は `init.c` の編集を要る。 | 装置と driver の結び付けの層（PCI の class・vendor・device の表） |
| D3 | L9（L8） | D | 当たれば壊れる | `kernel/src/dev/pci.c:17-21,32`<br>`common/abi.h:348` | `Only segment 0 is used`、`struct n_pciinfo` に segment が無い | 2 ソケットの EPYC で segment 1 以降の装置が見えない。 | PCI の列挙（MCFG の全部の segment） |
| D4 | L8 | B | 当たれば壊れる | `kernel/src/dev/nvme.c:32,336` | `IO_ENTRIES` 64、`MIN(IO_ENTRIES, mqes+1)` | CAP.MQES（NVMe 1.4c）を読むのに 64 で頭打ちにする。深い queue の装置の速さを捨てる。 | CAP.MQES とメモリーから決める |
| D5 | L8 | D | 当たれば壊れる | `kernel/src/dev/usbmsc.c:16,332-333` | `MAX_LUNS` 8 | `usbmassbulk_10.pdf` 3.2 の最大 LUN は 15 である。9 番目以降の LUN が見えない。 | Get Max LUN の応答 |
| D6 | L8 | D | 当たれば壊れる | `kernel/src/dev/iommu_amd.c:90,743` | `IRT_LOG2` 5（32 vector） | `pciuser.c:41` の MSI-X 2048 に届かない。AMD IOMMU 48882 Rev 3.10 の `IntTabLen` は 2048 entries まで許す。 | 装置の MSI-X Table Size から決める |
| D7 | L8 | D | 当たれば壊れる | `kernel/src/dev/iommu_intel.c:131-136,195,839` | `IRT_ENTRIES` 4096、`IOAPIC_ENTRIES` 256、`MSI_BLOCK` 32、ブロック 120 | ブロックが尽きると 121 個目の装置の割り込みが通らない。 | 装置の数と Table Size から決める |
| D8 | L8 | D | 当たれば壊れる | `kernel/src/dev/acpi.c:517` | `LINKS_MAX` 32 | 33 個目の PCI Interrupt Link が S3 で復元されない。 | ACPI の列挙の数 |
| D9 | L1 | A | きれいさ | `kernel/src/dev/nvme.c:34-35` | 5 s と 30 s | 仕様が定めない方針である。CAP.TO・CRTO は `nvme.c:643-644` で読む。 | `dev/nvme.c` |
| D10 | L4 | B | きれいさ | `ports/nvidia/nvrm.c:44-45`<br>`ports/nvidia/display.c:94,96,98,100` | `IRQ_WAKE_MS` 50、`IRQ_POLL_MS` 1、`FLIP_TIMEOUT_MS` 3000、`STATE_WAIT_MS` 1000、`FLIPS_TAKEN` 4、`MODE_TRIES` 4 | 合成の周期と無関係である。 | `nvrm` の内部 |
| D11 | L8 | D | きれいさ | `kernel/src/dev/pciuser.c:43`<br>`kernel/src/dev/usbnet.c:25` | `CLAIM_ACPI_NOTES` 32、`TX_SLOTS` 16 | 数に従わない。 | 各 driver |

### 網

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| N1 | L13 | D | 当たれば壊れる | `kernel/src/net/net.c:58,2470`<br>`common/abi.h:730`<br>`user/apps/ifconfig.c:16` | `static struct netif *nif;`、`eth0` の直書き | 二つ目の口を無視する。Wi-Fi と有線、二枚の NIC が動かない。 | 網の口の層 |
| N2 | L13 | B | 当たれば壊れる | `kernel/src/net/net.c:1206,366,248-249`<br>`kernel/src/net/rtl8125.c:130-131` | `TCP_MSS` 1460、`frame[1600]`、`RXQ` 64×1536、`BUFSZ` 2048、`FRAME_MAX` 1522 | MTU を driver と DHCP option 26 から取らない。jumbo frame と PPPoE の小さな MTU で誤る。 | 網の口の層（口ごとの MTU） |
| N3 | L13（L8） | B・D | 当たれば壊れる | `kernel/src/net/rtl8125.c:139-140` | `NRX`・`NTX` 256 | 2.5 GbE で約 1.2 ms 分である。受ける処理が 1.2 ms 止まると frame を失う。 | 口の速さから決める |
| N4 | L8 | D | 当たれば壊れる | `kernel/src/net/net.c:605,609,1221` | `UDP_Q` 8、`data[1472]`、`TCP_OOO` 8 | WebRTC と音声で 9 個目の datagram を捨てる。順序の乱れた回線で再送が増える。 | 窓の大きさとメモリーから決める |
| N5 | L13（L1） | A | 当たれば壊れる | `kernel/src/net/net.c:809,418` | hostname `nocturne`、TTL 64 | 機械の名を変えられない。静的な IP と DNS の設定が無い。IPv6 が無い。 | 網の口の層が登録する設定 |
| N6 | L8 | D | きれいさ | `user/libc/http.c:37` | `POOL_MAX` 32 | 接続の池の数を変えられない。 | `http.c` |

### 格納庫とメモリー

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| F1 | L12（L1） | A | 当たれば壊れる | `kernel/src/fs/store.c:405-412,392`<br>`kernel/src/sys/keep.c:89-94`<br>`kernel/src/sys/proc.c:256` | `Users` の最初のフォルダー、名前 `User` | 利用者が一人の前提である。ログインと切替が無い。設定が利用者ごとに切り替わらない。 | 利用者とセッションの層 |
| F2 | L8 | B | 当たれば壊れる | `kernel/src/fs/document.c:30-31` | `WORK_PAGES` 16 MiB、`COMMIT_BLOCKS` 64 MiB | メモリーと disk の速さに従わない。 | メモリーと装置の速さから決める |
| F3 | L8 | D | 当たれば壊れる | `kernel/src/sys/keep.c:11` | `READ_MOST` 64 KiB | 長い文書は読まれず、system の既定へ戻る。 | 設定の層の保存の部品 |
| F4 | L8 | D | 当たれば壊れる | `user/apps/sysmon.c:14,19,21,24,35` | `MAXPROC` 128 | ブラウザーの子プロセスで超える。超えた分は窓に出ない。 | 一覧の数に従う確保 |
| F5 | L8 | D | きれいさ | `user/apps/init.c:40-41`<br>`user/apps/sh.c:9-10`<br>`kernel/src/sys/thread.h:16,18`<br>`kernel/src/mm/vmm.h:30`<br>`common/abi.h:232`<br>`kernel/src/mm/vmm.c:53,55`<br>`kernel/src/fs/store.c:21-22` | `KEEP_PIECE`、`KEEP_MOST` 32 MiB、`MAX_LINE`、`MAX_HIST`、`KSTACK_PAGES`、`THREAD_STACK_DEFAULT`、`USER_STACK_MAX`、`N_LAUNCH_MAX_BYTES`、`TLB_RANGE_MAX`、`PCID_SLOTS`、`FIND_NS`、`FIND_MOST_NS` | 当たる所が狭い。規則の外にある。 | 各持ち主 |

### 電源

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| P1 | L11 | A | 毎日当たる | `kernel/src/arch/idle.h:19`<br>`kernel/src/arch/idle.c:114`<br>`kernel/src/dev/acpi.c:459,464` | `CPU_PERF_PREFERENCE_DEFAULT` 0x40、`cpu_perf_set_policy` に呼び手が無い、電源ボタンは切るだけ | 性能と省電力を選ぶ窓が無い。画面を消す時間と自動スリープの設定が無い。 | 電源と方針の層 |
| P2 | L11 | A | きれいさ | `kernel/src/dev/pm.c:33-36`<br>`kernel/src/fs/store.c:20` | GRACE 5 s、STALL 15 s、FORCE 60 s、STOP 3 s、`WRITE_DELAY_NS` 5 s | 変えられない。 | 電源と方針の層 |
| P3 | L1 | A | きれいさ | `boot/limine.conf:4` | timeout 3 | 起動の待ちを窓から変えられない。 | 設定の層（起動の項目の持ち主） |

### 設定の口

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| C1 | L1 | A | きれいさ | `kernel/src/arch/smp.c:303`<br>`kernel/src/dev/acpi.c:480`<br>`kernel/src/dev/fb.c:101`<br>`kernel/src/dev/hibernate.c:600`<br>`kernel/src/dev/iommu.c:131,829,859`<br>`kernel/src/dev/pciuser.c:235,806-807`<br>`kernel/src/init.c:53`<br>`kernel/src/mm/vmm.c:649`<br>`kernel/src/sys/prof.c:274-275` | 起動の旗 `nosmp`、`noaml`、`fbsim10`、`noresume`、`noiommu`、`appdrivers-unisolated`、`fbhold`、`noappdrivers`、`appdrivers=any`、`nogui`、`nopcid`、`prof`、`prof_ms` | 設定の代わりである。窓から見えない。AGENTS の「引数や旗の起動で組まない」に反する。 | 設定の層の一時の上書き |
| C2 | L1（L4） | A | きれいさ | `ports/nvidia/nvkms_os.c:46-92`<br>`user/apps/init.c:142-145` | `NVKMS_OPTIONS`（`hdmi_deepcolor`、`max_output_color_depth=10`、`conceal_vrr_caps`）、`NVRM_REGISTRY_DWORDS` | `init.c` は起動のときに値を渡さないので、経路が死んでいる。窓の入口が無い。 | `nvrm` が登録する設定 |
| C3 | L1 | 互換 | きれいさ | `kernel/src/sys/keep.c:17`<br>`common/display_abi.h:87` | `older[]`、`SCREEN_MODES_FIRST_BYTES` 2176、第 2 引数 0 の応答 | 古い版のための互換であり、AGENTS の「互換は持たない」に反する。 | 消す |
| C4 | L1 | A | きれいさ | `kernel/src/gui/wm.c:1192,3542`<br>`common/display_abi.h:32`<br>`user/apps/settings.ts:232` | `pointer_speed` の範囲 -2..2 が 4 か所 | 範囲を変えると 4 か所を直す。 | 設定の記述子 |

### アプリの登録

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| R1 | L10 | A | 毎日当たる | `kernel/src/gui/wm.c:1047-1072,1091,1154`<br>`kernel/src/gui/wm.h:79-82` | `apps[]` 22 項、`desktop_apps[]` 12 個、`APP_SLEEP`、`!strcmp(label,"About Nocturne")`、`ICON_*` | カーネルがプログラムの一覧を持つ。移植した Firefox がメニューに出ない。ファイルの関連付けが無い。 | アプリの登録の層（system service） |

### ブラウザー

| 番号 | 層 | 分類 | 重さ | 場所 | 今の値 | 壊れ方 | 正しい持ち主 |
|---|---|---|---|---|---|---|---|
| W1 | L14（L1・L2・L3） | A | 毎日当たる | `user/libc/web/css.c:5160-5171` | `prefers-color-scheme` light、`reduced-motion`、`reduced-transparency`、`reduced-data`、`contrast`、`forced-colors`、`hover`、`pointer`、`color-gamut` srgb、`dynamic-range` standard | 標準が利用者の選択を前提にする値が固定である。AGENTS の規則に反する。OS の UI は暗いのに頁には light と答える。 | Web へ利用者の選択を渡す経路 |
| W2 | L3（L14） | B | 毎日当たる | `user/libc/web/css.c:5150-5151`<br>`user/libc/web/js_screen.js:19,10-11` | `resolution` 1、`devicePixelRatio` 1、`availLeft` 0 | 4K で頁が小さい。二枚目の画面の位置を出さない。 | 画面の能力の層 |
| W3 | L14 | A | 毎日当たる | `user/libc/web/css.c:5615,6750,8543,10365` | 既定の `font-size` 16 が 4 か所 | 既定の字の大きさを変えられない。`user/apps/browser.c` に zoom の語が無く、頁の拡大も無い。 | ブラウザーの設定（`webprefs`） |
| W4 | L14 | A | 毎日当たる | `user/apps/browser.c:19-20,2683` | `HOME`、`SEARCH` は `https://lite.duckduckgo.com/lite/?q=` | ホームと検索を変えられない。 | ブラウザーの設定 |
| W5 | L8 | D | きれいさ | `user/include/webstorage.h:18` | 5 MiB | Storage の割当を変えられない。 | Storage の持ち主 |

## 5. C：仕様の値で、決め打ちでよいもの

次は公式の文書で照らした。文書は `D:\Programes\OS DEV INFO` にある。

| 場所 | 値 | 出典 | 照らした結果 |
|---|---|---|---|
| `kernel/src/dev/pci.c:794` | 100 ms | PCI Express Base 6.0：「the Function must complete the FLR within 100 ms」 | 合う |
| `kernel/src/dev/pciuser.c:41` | `MSIX_MOST` 2048 | PCI Express Base 6.0：Message Control の Table Size は bit 10:0 | 合う |
| `kernel/src/dev/ahci.c:35` | STOP 500 ms | AHCI 1.3.1：PxCMD.CR と PxCMD.FR を少なくとも 500 ms 待つ | 合う |
| `kernel/src/arch/topology.c:17` | `LOCAL_DISTANCE` 10 | ACPI 6.5：SLIT の対角は 10 | 合う |
| `kernel/src/arch/cpu.h:113` | `CPU_COF_UNIT_HZ` 200 MHz | PPR 56713 の glossary：CoreCOF = FID/DID×200 | Family 19h で合う。同じ文書の別の節に「COF is defined as CpuFid * 5MHz」という記述もある。Family 1Ah は T4 に書いた。 |
| `kernel/src/arch/lapic_timer.c:33` | `COUNT_STEP` 8 | PPR 55898 2.1.12：APIC の timer は 1 から 8 の単位で進む | 合う |
| `kernel/src/dev/xhci.c:233-236,228` | `[256]`、`port_pending[8]` | xHCI：HCSPARAMS1 の MaxSlots は bit 7:0、MaxPorts は bit 31:24 | 合う |
| `kernel/src/dev/xhci.c:222` | `periodic[16]` | xHCI：Interval | 合う |
| `kernel/src/dev/usbhid.c:68-80` | usage の表 | HID Usage Tables の keyboard page | 合う |
| `kernel/src/arch/irq.c` | 256 vector | Intel SDM | 合う |
| `kernel/src/dev/nvme.c:33` | `PRPS` 512 | NVMe：4 KiB の頁に 8 byte の PRP | 合う |
| `common/edid.h:10` | 128 byte | EDID | 手元に EDID と DisplayID の文書が無く、照らせない。 |

次は仕様の値に見えて違う。

| 場所 | 値 | 仕様 | 違い |
|---|---|---|---|
| `kernel/src/dev/usbmsc.c:16` | `MAX_LUNS` 8 | `usbmassbulk_10.pdf` 3.2：最大 LUN は 15 | 分類は D である（D の表に載せた）。 |
| `kernel/src/dev/iommu_amd.c:90` | `IRT_LOG2` 5 | AMD 48882 Rev 3.10：`IntTabLen` は 2048 entries まで | 分類は D である。 |
| `kernel/src/dev/iommu_intel.c:131` | `IRT_ENTRIES` 4096 | VT-d：`IRTA.S` で `2^(S+1)` entries、handle は 64K | 分類は D である。 |
| `kernel/src/net/net.c:1206` | `TCP_MSS` 1460 | MTU 1500 の Ethernet の値であり、口の MTU から決まる | 分類は B である。 |
| `kernel/src/net/net.c:1231` | `TCP_TIME_WAIT` 2 s | RFC 9293 は 2MSL である | 仕様と違う。コメントが認めている。 |
| `kernel/src/net/net.c:1226` | `RTO_MIN` 200 ms | RFC 6298 | RFC 6298 は手元に無く、照らせない。手元の文書は `rfc9293.pdf` だけである。方針の値であり、C に入れない。 |

## 6. 一つの環境の都合を別の部品へ持ち込んだ所

AGENTS.md の「ある環境の都合を別の部品へ持ち込むこと」に当たる。

| 場所 | 持ち込まれた都合 | 壊れ方 |
|---|---|---|
| `kernel/src/dev/fb.c:177-179` | 実機の一番熱い写しの理由が「WHPX does not emulate SIMD MMIO」である | 実機の写しの形を VM の制限が決める。 |
| `kernel/src/dev/audio.c:21` | `RUNS` 32 の根拠が AC'97 の輪である | 実機の音の周期を VM の装置が決める。 |
| `kernel/src/gui/wm.c:2895-2896` | RDP の大きさの範囲が合成の部品にある | `vm/` の外に VM の値がある。 |
| `kernel/src/gui/wm.c:2601` | hypervisor の指の座標の処理が合成の部品にある | 同じである。 |
| `kernel/src/dev/input.c:117` | Hyper-V の unicode の入力の道が共通の入力にある | 同じである。 |

`vm/` の外は `vm/vm.h` だけを include するという規則は守られている。値と分岐は外にある。

## 7. 窓のある設定の値の内訳

窓から変えられるシステムの値は次の 5 種だけである。

| 値 | 持ち主 | 文書 |
|---|---|---|
| `pointer_speed`、`smooth_gradients` | `gui/wm.c` | Desktop |
| `volume`、`muted` | `dev/audio.c` | Sound |
| `mode.<monitor>` | `gui/screenmode.c` | Displays |

プログラムの設定で規則を満たす例は、ブラウザーの `user/include/webprefs.h` と `user/apps/browser_settings.h` である。節は言語と Privacy の 2 個である。

## 8. 探して欠けが見つからなかった所

探し方は次のとおりである。

- `git grep` で `#define` の数字、`static const`、固定の配列 `[N]`、`N_ERR_BUSY`、`C:\\` の文字列、`cmdline_*`、`_MS` と `_NS`、`RGB` を main の木で掘った。
- 全部読んだファイルは `sys/keep.c`、`sys/keep.h`、`gui/screenmode.c`、`dev/audio.c`、`dev/input.c`、`dev/input.h`、`user/apps/settings.ts`、`common/abi.h` の画面と窓の部分である。
- 部分を読んだファイルは `gui/wm.c`、`dev/fb.c`、`dev/usbhid.c`、`user/libc/ui.c`、`ports/nvidia/display.c` である。

規模に従うと確かめたものは次のとおりである。

| もの | 場所 | 形 |
|---|---|---|
| CPU の数 | `arch/percpu.h`、`arch/topology.c` | CPU の数で配列を切らない。 |
| 画面の数 | `dev/fb.c:282-300` | 表が伸びる。実機の NVIDIA の一枚は `display.c` の別の欠けである。 |
| 窓の数と損傷 | `gui/wm.c` の tile の bitmap | 窓の表が伸びる。 |
| handle の表 | `sys/object.c:93` | 上限が無い。 |
| log の輪 | `sys/log.c:98` | RAM と CPU から決める。 |
| heap と pmm | `mm/` の `CACHE_SHARE` | メモリーから決める。 |
| irq の vector | `arch/irq.c` | CPU ごとである。 |
| NVMe の queue の数 | `dev/nvme.c` | CPU ごとである。深さは D4 の欠けである。 |
| AHCI の slot | `dev/ahci.c:715` | CAP.NCS から取る。 |
| xHCI の MaxIntrs | `dev/xhci.c` | 装置の申告から取る。 |
| 周期の下限 | `sys/sched.h` の `SCHED_PERIOD_MIN_NS` 125 µs | 8 kHz まで届く。600 Hz に足りる。 |
| channel の credit | `sys/channel.c:196-202` | 空きメモリーの半分から決める。 |
| NVMe の timeout | `dev/nvme.c:643-644` | CAP.TO と CRTO を読む。 |

問題でないと判じたものは次のとおりである。

- `user/include/media_alloc_private.h:20` の `NMEDIA_ALLOC_DEFAULT_BYTES` は `SIZE_MAX` で、限界ではない。
- `user/include/media.h:13` の `NMEDIA_MAX_BYTES` 32 MiB は定義だけで、`user/libc` と `user/apps` に使い手が無い。死んだ定数である。
- `sched_deadline()` の `request_ns` は deadline の slice に使わない（`slice_arm` は `dl_reserve` を使う）。したがって決め打ちの問題にしない。

## 9. 読み切っていない範囲

- `user/libc/web/` は窓と媒体の部分だけを読んだ。残りの Web の定数の数は分からない。
- `ports/mesa` は読んでいない。
- `kernel/src/vm/` は境界だけを読んだ。
- `third_party/` は読んでいない。例外は QuickJS の `getTimezoneOffset` の 1 か所である。
- 色の 193 個は `\bA?RGB\(` と `0x` の 6〜8 桁の字面で数えた。`wm.c` と `desktop.c` の中の色でない `0x` の数値も少し混じる。
- 寸法の 34 個は `wm.c` の読んだ所の数である。`wm.c` の全部の画素の直書きの数ではない。
- 「消える値」の数は、この監査で挙げた値の数である。読んでいない範囲の値は含まない。
