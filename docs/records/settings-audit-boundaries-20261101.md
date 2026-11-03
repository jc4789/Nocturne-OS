# 設定と境界の監査：全体の形（2026-11-01）

## 依頼文（原文のまま）

あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、対象は実機）の敵対的なアーキテクチャの監査者です。読むだけで、製品のコードを変えません。ビルドも QEMU も起動しません。

### 持ち主の言葉（原文）
"This IS NORMAL. WE LACK ANY TYPE OF REAL settings SUPPORT. AND its ALL HARD CODED SLOP THAT IS FUNDAMENTALLY WRONG. THIS IS FOR THE ENTIRE OS. BEFORE ANY MORE FEATURES ARE ADDED. THIS NEEDS TO GET FIXED. OR YOU WILL KEEP WRITING HARDCODED spaghetti code. EVERYTHING IS NOT A FILE. THAT IS STUPID AND WRONG. THERE ARE BOUNDARIES. THE UI. THE SYSTEM PROCESSES. KERNEL. THERE ARE NOT FILES TO EDIT. THERE ARE SETTINGS TO CHANGE AND APPLY. TREAT THIS AS adversarial FIXING OF misconceptions and mistakes in fundamental design and architecture. DO NOT BLUR BOUNDARIES. MAKE SURE THE RIGHT THINGS HAVE THE RIGHT OWNERSHIP. DO NOT TREAT THE CURRENT STATE OF THE OS OR THE DOCS OR YOUR MEMORIES AS ACCEPTABLE." "THIS IS NOT JUST FOR AUDIO BUT FOR EVERYTHING." "OUR OS IS FUNDAMENTALLY FLAWED."

### あなたの範囲
全体の形です。
- 契約：`common/abi.h`、`common/display_abi.h`、`common/gpu_abi.h`、`docs/nocturne-native-abi.md`、`user/libc/sys.c`、`kernel/src/sys/syscall.c`。
- カーネルとユーザー空間の分担：`kernel/src/gui/`、`kernel/src/net/`、`kernel/src/fs/`、`kernel/src/sys/keep.c`、`kernel/src/sys/proc.c`、`kernel/src/init.c`・`boot.c`、`user/apps/init.c`、`user/apps/uihost.c`。
- 規則と文書そのもの：`AGENTS.md`、`.claude/rules/*.md`、`.claude/oboegaki.md`、`C:\Users\cesta\.claude\projects\D--Programes-llm-vibe-slop-os\memory\*.md`。ここに書かれた前提も監査の対象で、正と見ません。

### 調べること
1. 全部の `SYS_*` を分類する表を作ります。各 syscall について：機構か方針か、本来カーネルの物か、システムのプロセスの物か、UI の物か。
2. カーネルの中にある物で、カーネルの外（システムのプロセス、UI、アプリ）の責務である物。窓の管理、デスクトップ、タスクバー、設定の保存、画面のモードの選択、網の方針、読み書きの形式など。逆に、ユーザー空間にあってカーネルの責務である物。
3. 「全部ファイル」の考え方が残っている所。設定・状態・通信を文書やパスで表している所。ABI の中で文書の形を流用している所。
4. 境界を越える依存。カーネルが UI を呼ぶ、カーネルがユーザーの文書を読み書きする、UI が装置に触れる、など。include の方向とコールグラフで確かめます。
5. 設定を表す共通の型・共通の道が今あるか。値の型、範囲、既定、能力、変更の通知、適用、失敗の戻し、権限、利用者ごとと機械ごとの区別、装置ごとの区別。無い物を一つずつ挙げます。
6. 権限と利用者の模型。誰がどの設定を変えられるか。システムのプロセスは何の権限で動くか。
7. 規則・メモリー・文書の中の誤った前提。例えば、設定の置き場所、持ち主、カーネルの責務について、それ自体が今の誤りを固めている行を引用（15 語以内）付きで挙げます。

他の OS のソースを読みません。今の形を弁護しません。

### 報告
全文を `docs/records/settings-audit-boundaries-20261101.md` に日本語で書きます。頭にこの依頼文を写します。項目ごとに `ファイル:行`、今の形、なぜ誤りか、本来の持ち主を書きます。最後に、根の誤りを重い順に 15 件まで、一つが他の誤りをどう生んでいるかと一緒に書きます。書きぶりは説明書のように、一文に一つのことを書きます。実際に読んだコードだけを書き、推測を書きません。

返事は、報告のパスと根の誤りの一行ずつだけにします。

---

## 0. この報告の土台

- 対象のコミットは `7de8c35f` の作業木です。`AGENTS.md` だけ未コミットの一行（持ち主の言葉）があります。
- 全部を読んだファイル：`common/abi.h`、`common/display_abi.h`、`common/gpu_abi.h`、`kernel/src/sys/syscall.c`、`kernel/src/sys/keep.c`・`keep.h`、`kernel/src/sys/proc.c`、`kernel/src/init.c`、`kernel/src/boot.c`、`kernel/src/gui/desktop.c`・`screenmode.c`・`screenmode.h`・`wm.h`、`kernel/src/fs/store.h`、`kernel/src/net/net.h`、`user/apps/init.c`、`user/apps/uihost.c`、`AGENTS.md`、`.claude/rules/*.md`、`.claude/oboegaki.md`、メモリーの全部、`docs/records/settings-ownership-20261027.md`。
- 一部を読んだファイル：`kernel/src/gui/wm.c`（1〜70、425〜504、1035〜1334、2300〜2389、3000〜3100、3376〜3652 と行の抜き出し）、`kernel/src/net/net.c`（頭、940〜987、2440〜2499 と行の抜き出し）、`kernel/src/dev/audio.c`（70〜95、598〜617、690〜714、794〜850）、`kernel/src/fs/store.c`（395〜470、774〜790 と行の抜き出し）、`kernel/src/dev/pm.c`、`kernel/src/dev/pciuser.c`、`kernel/src/dev/usbhid.c`、`kernel/src/dev/input.c`、`kernel/src/sys/object.h`、`kernel/src/sys/process.h`・`process.c`、`kernel/src/sys/channel.c`、`user/libc/sys.c`、`user/apps/settings.ts`、`user/apps/js.c`、`docs/nocturne-native-abi.md`（§0〜§2、§5、§9 の一部、§10、§12）。
- 「本来の持ち主」の語は三つの層で書きます。
  - **カーネル**：CPU・メモリー・割り込み・装置の取り合いと、object・handle・channel の機構です。
  - **システムのプロセス**：カーネルの外で動き、機械全体や利用者の状態を受け持つプロセスです。今の Nocturne にこの層はほとんどありません（§6）。
  - **UI**：利用者に見えて、利用者が触る物です。デスクトップのシェルと設定の窓です。
- 「機構」は、何をするかを決めずに、できることだけを渡す物です。「方針」は、何をするかを決める物です。

---

## 1. 全部の `SYS_*` の分類

数は 148 です。`SYS_NET_LIMIT` と `SYS_MAX` は呼出しではありません。番号は `common/abi.h:49-151` から数えました。`kernel/src/sys/syscall.c:1015-1287` がカーネル側の振り分けです。

表の「持ち主」は本来の持ち主です。今の持ち主は全部カーネルです。

### 実行とスレッドと時間

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 0 | `SYS_EXIT` | 機構 | カーネル | |
| 19 | `SYS_SLEEP` | 機構 | カーネル | |
| 20 | `SYS_APP_CPU_TIME` | 機構 | カーネル | |
| 28 | `SYS_YIELD` | 機構 | カーネル | |
| 45 | `SYS_THREAD_CREATE` | 機構 | カーネル | |
| 46 | `SYS_THREAD_JOIN` | 機構 | カーネル | |
| 47 | `SYS_THREAD_EXIT` | 機構 | カーネル | |
| 48 | `SYS_WAIT_ADDRESS` | 機構 | カーネル | |
| 49 | `SYS_WAKE_ADDRESS` | 機構 | カーネル | |
| 50 | `SYS_THREAD_ID` | 機構 | カーネル | |
| 57 | `SYS_THREAD_SCHEDULE` | 機構 | カーネル | 重みと周期の上限が `abi.h:331-333` の定数です。 |
| 58 | `SYS_THREAD_STACK_BOUNDS` | 機構 | カーネル | |
| 59 | `SYS_THREAD_DETACH` | 機構 | カーネル | |
| 89 | `SYS_WAIT_ADDRESS64` | 機構 | カーネル | |

### メモリー

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 56 | `SYS_SHARED_CREATE` | 機構 | カーネル | |
| 88 | `SYS_SHARED_RESERVE` | 機構 | カーネル | |
| 166 | `SYS_MEMORY_RESERVE` | 機構 | カーネル | |
| 167 | `SYS_MEMORY_RELEASE` | 機構 | カーネル | |
| 168 | `SYS_MEMORY_PROTECT` | 機構 | カーネル | |
| 169 | `SYS_SHARED_MAP` | 機構 | カーネル | |
| 179 | `SYS_SHARED_VIEW` | 機構 | カーネル | |
| 180 | `SYS_SHARED_REVOKE` | 機構 | カーネル | |
| 183 | `SYS_SHARED_COMPOSE` | 機構 | カーネル | |
| 192 | `SYS_PAGER_CREATE` | 機構 | カーネル | |
| 193 | `SYS_PAGER_TAKE` | 機構 | カーネル | |
| 194 | `SYS_PAGER_SUPPLY` | 機構 | カーネル | |
| 195 | `SYS_PAGER_REVOKE` | 機構 | カーネル | |
| 196 | `SYS_PAGER_FAIL` | 機構 | カーネル | |

### handle と事象と通信

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 53 | `SYS_CHANNEL_LISTEN` | 混在 | カーネル（機構）とシステムのプロセス（名前の割当て） | 名前は早い者勝ちです（§6-7）。 |
| 54 | `SYS_CHANNEL_CONNECT` | 機構 | カーネル | 相手の証明がありません。 |
| 55 | `SYS_CHANNEL_ACCEPT` | 機構 | カーネル | |
| 128 | `SYS_HANDLE_CLOSE` | 機構 | カーネル | 文書の版の差替えを閉じる時に行います（`syscall.c:527`）。閉じる操作が文書の意味を持つ特例です。 |
| 129 | `SYS_HANDLE_KIND` | 機構 | カーネル | |
| 144 | `SYS_CHANNEL_PAIR` | 機構 | カーネル | |
| 145 | `SYS_CHANNEL_SEND` | 混在 | カーネル | message と byte stream の二つの意味があります（`syscall.c:724-727`）。 |
| 146 | `SYS_CHANNEL_RECEIVE` | 混在 | カーネル | 同じです（`syscall.c:746-753`）。 |
| 182 | `SYS_CHANNEL_RECEIVE_FROM` | 機構 | カーネル | |
| 152 | `SYS_EVENTS_CREATE` | 機構 | カーネル | |
| 153 | `SYS_EVENTS_WATCH` | 機構 | カーネル | |
| 154 | `SYS_EVENTS_UNWATCH` | 機構 | カーネル | |
| 155 | `SYS_EVENTS_WATCH_ADDRESS` | 機構 | カーネル | |
| 156 | `SYS_EVENTS_TIMER` | 機構 | カーネル | |
| 157 | `SYS_EVENTS_WAIT` | 機構 | カーネル | |
| 207 | `SYS_WATCH_TAKE` | 機構 | カーネル | |

### アプリの管理

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 147 | `SYS_APP_LAUNCH` | 混在 | カーネル（起こす機構）とシステムのプロセス（誰を何の権限で） | `settings` は環境変数の形です（§3-5）。 |
| 148 | `SYS_APP_RESULT` | 機構 | カーネル | |
| 149 | `SYS_APP_STOP` | 機構 | カーネル | 権限の確かめがありません（`syscall.c:851-862`）。 |
| 150 | `SYS_APP_SELF` | 機構 | カーネル | |
| 151 | `SYS_APP_PARENT` | 機構 | カーネル | |
| 158 | `SYS_APP_OPEN` | 機構 | カーネル | どのアプリでも、どのアプリの handle でも得ます（`syscall.c:864-869`）。 |
| 159 | `SYS_APP_ID` | 機構 | カーネル | |
| 26 | `SYS_PROCLIST` | 機構 | カーネル | 物理番地 `wait_phys` を全部のアプリへ渡します（`abi.h:269`、`syscall.c:193`）。 |
| 73 | `SYS_GUI_LAUNCH` | 方針 | システムのプロセス（起動の窓口）。この呼出しは消す | `SYS_APP_LAUNCH` の二つ目の道です。親が無く、handle も渡せません（`proc.c:348-360`）。 |

### 情報と電源とログ

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 27 | `SYS_SYSINFO` | 混在 | カーネル（メモリー・CPU の数字）と UI（画面の大きさ） | `syscall.c:218-220` が画面の大きさを混ぜます。 |
| 29 | `SYS_POWER` | 混在 | カーネル（実行）とシステムのプロセス（決定） | 権限の確かめがありません（`syscall.c:1205-1210`）。 |
| 33 | `SYS_SYSTEM_LOG` | 機構 | カーネル | |
| 160 | `SYS_SYSTEM_LOG_OPEN` | 機構 | カーネル | どのアプリも読み書きできます。 |
| 34 | `SYS_PCILIST` | 機構 | カーネル | |
| 38 | `SYS_CPU_INFO` | 機構 | カーネル | `worker_cpus` などは常に 0 の死んだ欄です（`abi.h:335-342`）。 |
| 165 | `SYS_RANDOM` | 機構 | カーネル | |

### 文書とフォルダー

C: の格納は今カーネルの中にあります。私はこの群を機構に分類します。誤りは三つです。パスだけで全部へ届くこと、呼ぶ側の区別が `bool user` だけなこと、古い形が残っていることです。

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 130 | `SYS_DOCUMENT_OPEN` | 機構 | カーネル | パスだけで開きます。 |
| 131 | `SYS_DOCUMENT_READ_AT` | 機構 | カーネル | |
| 132 | `SYS_DOCUMENT_WRITE_AT` | 機構 | カーネル | |
| 133 | `SYS_DOCUMENT_SIZE` | 機構 | カーネル | |
| 134 | `SYS_DOCUMENT_SET_SIZE` | 機構 | カーネル | |
| 135 | `SYS_DOCUMENT_CLAIM` | 機構 | カーネル | |
| 136 | `SYS_DOCUMENT_RELEASE` | 機構 | カーネル | |
| 137 | `SYS_DOCUMENT_SAVE` | 機構 | カーネル | |
| 138 | `SYS_DOCUMENT_APPEND` | 機構 | カーネル | |
| 185 | `SYS_DOCUMENT_COMMIT` | 機構 | カーネル | |
| 186 | `SYS_DOCUMENT_INFO` | 機構 | カーネル | |
| 139 | `SYS_ENTRY_INFO` | 機構 | カーネル | 188 の古い形です。消す物です。 |
| 188 | `SYS_ENTRY_INFO_SIZED` | 機構 | カーネル | |
| 140 | `SYS_ENTRY_REMOVE` | 機構 | カーネル | |
| 141 | `SYS_ENTRY_MOVE` | 機構 | カーネル | |
| 187 | `SYS_ENTRY_REPLACE` | 機構 | カーネル | |
| 142 | `SYS_FOLDER_CREATE` | 機構 | カーネル | |
| 143 | `SYS_FOLDER_LIST` | 機構 | カーネル | 189 の古い形です。消す物です。 |
| 189 | `SYS_FOLDER_LIST_SIZED` | 機構 | カーネル | |
| 208 | `SYS_ENTRY_WATCH` | 機構 | カーネル | |

### 窓と画面（`wm_syscall`、`wm.c:3602-3652`）

この群の全部が、カーネルの番号表にある UI の契約です。窓の面は共有 object で、事象は channel で運べます。だから窓の群はカーネルの番号を要りません。

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 64 | `SYS_WIN_CREATE` | 方針 | UI（窓の管理のプロセス） | |
| 65 | `SYS_WIN_SURFACE` | 方針 | UI（窓の管理のプロセス） | 面そのものは共有 object の機構です。 |
| 66 | `SYS_WIN_PRESENT` | 方針 | UI（窓の管理のプロセス） | |
| 67 | `SYS_WIN_SET_TITLE` | 方針 | UI（窓の管理のプロセス） | |
| 69 | `SYS_WIN_MOVE` | 方針 | UI（窓の管理のプロセス） | 持ち主を確かめません（`wm.c:3507-3514`）。 |
| 72 | `SYS_WIN_RESIZE` | 方針 | UI（窓の管理のプロセス） | |
| 75 | `SYS_WIN_STATE` | 方針 | UI（窓の管理のプロセス） | |
| 76 | `SYS_WIN_PRESENTATION` | 方針 | UI（窓の管理のプロセス） | |
| 77 | `SYS_WIN_RECEIVE` | 方針 | UI（窓の管理のプロセス） | channel で足ります。 |
| 78 | `SYS_WIN_DIRECT` | 方針 | UI（窓の管理のプロセス） | |
| 79 | `SYS_WIN_FRAME` | 方針 | UI（窓の管理のプロセス） | |
| 80 | `SYS_WIN_POINTER` | 方針 | UI（窓の管理のプロセス） | |
| 81 | `SYS_WIN_ACQUIRE` | 方針 | UI（窓の管理のプロセス） | |
| 68 | `SYS_SCREEN_INFO` | 方針 | 分ける：画面の情報はシステムのプロセス、設定は設定の道 | 一つの番号に七つの意味です（§3-1）。 |
| 70 | `SYS_CLIPBOARD_SET` | 方針 | UI（利用者のセッション） | |
| 71 | `SYS_CLIPBOARD_GET` | 方針 | UI（利用者のセッション） | 権限の確かめがありません（`wm.c:3592-3600`）。 |
| 74 | `SYS_SCREEN_GRAB` | 方針 | UI（窓の管理のプロセス）と権限 | どのアプリも画面の全部を読めます（`wm.c:3379-3400`）。 |

### 網（`net_syscall`、`net.c:2586-2779`）

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 96 | `SYS_NET_INFO` | 混在 | システムのプロセス（網の窓口） | adapter が一つの形です（`abi.h:734-740`）。 |
| 97 | `SYS_NET_PING` | 方針 | システムのプロセス（網の窓口） | |
| 98 | `SYS_NET_DNS` | 方針 | システムのプロセス（網の窓口） | |
| 108 | `SYS_NET_DNS_ALL` | 方針 | システムのプロセス（網の窓口） | |
| 99 | `SYS_UDP_SOCKET` | 機構 | カーネル | 名前が BSD の形です（§3-9）。 |
| 100 | `SYS_UDP_SEND` | 機構 | カーネル | |
| 101 | `SYS_UDP_RECV` | 機構 | カーネル | |
| 106 | `SYS_UDP_CLOSE` | 機構 | カーネル | `SYS_HANDLE_CLOSE` と別の閉じ方です。 |
| 102 | `SYS_TCP_CONNECT` | 機構 | カーネル | |
| 103 | `SYS_TCP_SEND` | 機構 | カーネル | |
| 104 | `SYS_TCP_RECV` | 機構 | カーネル | |
| 105 | `SYS_TCP_CLOSE` | 機構 | カーネル | `SYS_HANDLE_CLOSE` と別の閉じ方です。 |
| 109 | `SYS_TCP_SEND_UNTIL` | 機構 | カーネル | |
| 110 | `SYS_TCP_CANCEL` | 機構 | カーネル | |
| 113 | `SYS_TCP_LISTEN` | 機構 | カーネル | backlog の既定が `net.c:1219` の定数です。 |
| 114 | `SYS_TCP_ACCEPT` | 機構 | カーネル | |
| 115 | `SYS_TCP_ADDRESS` | 機構 | カーネル | |
| 116 | `SYS_TCP_FINISH` | 機構 | カーネル | |
| 107 | `SYS_NET_TEST` | 試験 | ABI から消す | 損失の模擬が製品の ABI にあります（`abi.h:753-757`）。 |
| 111 | `SYS_NET_STATS` | 機構 | カーネル | |
| 112 | `SYS_NET_SETUP_CANCEL_CHANNEL` | 機構 | カーネル | |

### 音

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 161 | `SYS_AUDIO_OPEN` | 機構 | カーネル | 48 kHz 固定です（`abi.h:576`）。 |
| 162 | `SYS_AUDIO_SUBMIT` | 機構 | カーネル | |
| 163 | `SYS_AUDIO_STATUS` | 機構 | カーネル | |
| 41 | `SYS_AUDIO_FLUSH` | 機構 | カーネル | |
| 164 | `SYS_AUDIO_OUTPUT` | 方針 | 設定の道とシステムのプロセス | 音量と消音の読み書きです。権限の確かめがありません（`syscall.c:924-939`）。 |
| 209 | `SYS_AUDIO_OUTPUT_WATCH` | 方針 | 設定の道 | 設定ごとに別の見張りの口です。 |

### GPU

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 39 | `SYS_GPU_INFO` | 機構 | ドライバーのプロセス | VM の装置の形が ABI にあります（`gpu_abi.h:16-18`）。 |
| 40 | `SYS_GPU_RENDER` | 機構 | ドライバーのプロセス | 同じです。 |
| 42 | `SYS_GPU_RENDER_BATCH` | 機構 | ドライバーのプロセス | 同じです。 |
| 43 | `SYS_GPU_BLIT` | 機構 | ドライバーのプロセス | 同じです。 |
| 181 | `SYS_GPU_RENDER_LIST` | 機構 | ドライバーのプロセス | 同じです。 |

### 装置のドライバー

| 番号 | 名前 | 種類 | 持ち主 | 備考 |
|---|---|---|---|---|
| 170 | `SYS_PCI_CLAIM` | 混在 | カーネル（機構）とシステムのプロセス（許可） | 権限が `init_pid` の決め打ちです（`pciuser.c:811-812`）。 |
| 171 | `SYS_PCI_INFO` | 機構 | カーネル | |
| 172 | `SYS_PCI_CONFIG_READ` | 機構 | カーネル | |
| 173 | `SYS_PCI_CONFIG_WRITE` | 機構 | カーネル | |
| 174 | `SYS_PCI_BAR` | 機構 | カーネル | |
| 175 | `SYS_PCI_INTERRUPTS` | 機構 | カーネル | |
| 176 | `SYS_PCI_INTERRUPT_TAKE` | 機構 | カーネル | |
| 177 | `SYS_DMA_CREATE` | 機構 | カーネル | |
| 178 | `SYS_DMA_ADDRESSES` | 機構 | カーネル | |
| 184 | `SYS_PCI_HOLD_SCREEN` | 機構 | カーネル | |
| 190 | `SYS_PCI_GIVE_SCREEN` | 機構 | カーネル | |
| 191 | `SYS_PCI_SCREEN_VBLANK` | 機構 | カーネル | |
| 197 | `SYS_PCI_ACPI_EVAL` | 機構 | カーネル | |
| 198 | `SYS_PCI_ACPI_NOTIFY_TAKE` | 機構 | カーネル | |
| 199 | `SYS_PCI_SCREEN_STATE` | 混在 | カーネル（機構）とシステムのプロセス（望むモード、直接表示する窓、ポインター） | `struct n_screen_state` に方針の結果が混ざります（`abi.h:486-496`）。 |
| 200 | `SYS_PCI_SCREEN_IMAGE` | 機構 | カーネル | |
| 201 | `SYS_PCI_SCREEN_IMAGE_END` | 機構 | カーネル | |
| 202 | `SYS_PCI_SCREEN_MODES` | 機構 | カーネル | 受け手の選択が方針です（§2-3）。 |
| 203 | `SYS_PCI_SCREEN_FLIPS` | 機構 | カーネル | |

### 表の数

- 機構のみで持ち主もカーネルで正しい物は 100 を超えます。
- 方針がカーネルに入っている物は、窓の 13、画面と clipboard の 4、網の 3、音の 2、起動の 1 です。
- この 23 の口の裏に、3652 行の `wm.c`、754 行の `desktop.c`、527 行の `screenmode.c`、343 行の `keep.c`、DHCP と DNS を含む 2779 行の `net.c` があります。

---

## 2. カーネルの中にある、カーネルの外の責務

### 2-1. デスクトップのシェルの全部

- **`kernel/src/gui/wm.c:1-25`**
  - 今の形：頭の注釈が窓の管理、タスクバー、moon menu、音量の panel、toast をカーネルの持ち物と書きます。
  - なぜ誤りか：利用者に見える物が CPU の特権で動いています。絵を一つ変えるのにカーネルの組み直しが要ります。絵の誤りがカーネルの誤りになります。
  - 本来の持ち主：UI（デスクトップのシェルのプロセス）。
- **`kernel/src/gui/wm.c:1048-1071`**
  - 今の形：スタートメニューの全部のアプリ名、パス、絵の番号が C の表です。`C:\Programs\Browser\browser` まで書いてあります。
  - なぜ誤りか：アプリを一つ足すとカーネルを組み直します。入れたアプリはメニューに出ません。アプリの一覧は登録された情報で、カーネルの定数ではありません。
  - 本来の持ち主：システムのプロセス（アプリの登録）と UI（メニュー）。
- **`kernel/src/gui/wm.c:1092`**
  - 今の形：デスクトップの絵の並びが、上の表の添字の数字列 `{0, 1, 2, 3, 4, 8, 9, 10, 13, 14, 16, 17}` です。
  - なぜ誤りか：表の順を変えると別の絵に化けます。利用者は絵を並べ替えられません。
  - 本来の持ち主：UI。利用者ごとの設定です。
- **`kernel/src/gui/wm.c:1155`**
  - 今の形：`"About Nocturne"` という文字列を比べて、絵の下の字を `"About"` に替えます。
  - なぜ誤りか：表示の字をカーネルの中の文字列の比較で決めています。
  - 本来の持ち主：UI。
- **`kernel/src/gui/wm.c:1224-1225`**
  - 今の形：`"@reboot"`・`"@sleep"` などのパスの形の文字列で電源の操作を表します。
  - なぜ誤りか：操作をパスの字に押し込めた、全部ファイルの形です。
  - 本来の持ち主：UI が操作を出し、システムのプロセスが受けます。
- **`kernel/src/gui/wm.c:2335-2350`**
  - 今の形：Super、Ctrl+Esc、Alt+Tab、Alt+F4、Ctrl+Alt+T が C の分岐です。Ctrl+Alt+T は `SYSTEM "term"` を起動します。
  - なぜ誤りか：キーの割当ては利用者が変える物です。今は変えられません。アプリはこのキーを受けられません。
  - 本来の持ち主：UI。利用者ごとの設定です。
- **`kernel/src/gui/wm.c:51-59`、`:487`、`:492`、`:1239-1241`、`:1315-1319`**
  - 今の形：タスクバーの高さ 40、題の高さ 28、影 10、二度押しの 450 ms、toast の 4 秒、メニューの幅 260、音量の panel の幅 300 が定数です。
  - なぜ誤りか：どれも利用者の好みか UI の寸法です。画面の倍率を変える道がありません。高い解像度で字と部品が小さくなります。
  - 本来の持ち主：UI。
- **`kernel/src/gui/desktop.c:107-201`、`:209-377`、`:632-743`**
  - 今の形：壁紙の絵、全部のアイコン、全部のポインターの絵を、カーネルの C で描きます。
  - なぜ誤りか：壁紙を選ぶ設定が存在できません。テーマも存在できません。
  - 本来の持ち主：UI。
- **`kernel/src/gui/wm.c:3057`、`:1233`、`:1775`、`kernel/src/dev/pm.c:99-107`、`:247-309`**
  - 今の形：英語の文がカーネルにあります。`"Welcome to "`、`"Could not start program"`、`"No sound device"`、`"Waiting to restart"` などです。
  - なぜ誤りか：表示の言語の設定が存在できません。カーネルが利用者に話しています。
  - 本来の持ち主：UI。カーネルは出来事を報告するだけです。
- **`common/gfx.c` と `common/unifont.*`（`Makefile:40-41`）**
  - 今の形：描画の部品と字体がカーネルに組み込まれます。
  - なぜ誤りか：panic の画面に要るのは小さな部分だけです。今は窓の題とメニューも同じ字体で描きます。字体を選ぶ道がありません。
  - 本来の持ち主：panic の分だけカーネル。残りは UI。

### 2-2. 合成と窓の管理

- **`kernel/src/gui/wm.c:8-13`、`wm.h:5`**
  - 今の形：合成のスレッドと窓の順と焦点がカーネルの thread と `wm_lock` の下にあります。
  - なぜ誤りか：窓の方針（順、焦点、最大化、全画面の判断、窓が画面の外へ出ない処理）がカーネルの中です。`SYS_WIN_*` が UI の契約を syscall の番号に固定します。
  - 本来の持ち主：UI（窓の管理のプロセス）。カーネルが持つのは面の共有 object、scanout、vblank、入力の配達、締切の級です。
  - 移す時の制約：`abi.h:327-328` は一つのアプリの締切の級を合わせて CPU 半分までと決めます。画面が何枚でも合成するプロセスはこの上限に当たります。この上限は同時に見直す物です。

### 2-3. 画面のモードの選択

- **`kernel/src/gui/screenmode.c:151-168`**
  - 今の形：`pick` が、利用者の選択、推奨、最大の順で決めます。
  - なぜ誤りか：どのモードを見せるかは方針です。カーネルは「これを見せられる」と「見せた」を運ぶだけで足ります。
  - 本来の持ち主：システムのプロセス（画面の管理）。
- **`kernel/src/gui/screenmode.c:488-521`**
  - 今の形：試す、残す、戻すの三つの操作と、その秒読みのカーネルの thread `trial_watch` があります。
  - なぜ誤りか：秒読みと確認は設定の窓の仕事です。適用と失敗の戻しの道が、この一つの設定にしかありません。
  - 本来の持ち主：設定の道（試行と戻しを一般の操作にする）。
- **`common/display_abi.h:67`**
  - 今の形：`SCREEN_MODE_TRIAL_MS 15000u` が ABI の定数です。
  - なぜ誤りか：確認の秒数は UI の方針です。
  - 本来の持ち主：UI。
- **`kernel/src/gui/screenmode.c:163`**
  - 今の形：デスクトップが動いていなければ firmware の大きさを選ぶ分岐があります。
  - なぜ誤りか：選択の中に起動の形の特例があります。
  - 本来の持ち主：システムのプロセス。

### 2-4. 設定の保存

- **`kernel/src/sys/keep.c` の全部（`:1-343`）**
  - 今の形：カーネルの thread `keep` が、利用者のフォルダーの `.conf` を読み、書き、古い文書を消します。
  - なぜ誤りか：カーネルが利用者の文書の読み手と書き手です。カーネルの中に文字列の解析があります（`:122-142`）。カーネルが書式の面倒を見ます（`keep_put`、`:77-84`）。
  - 本来の持ち主：システムのプロセス（設定の窓口）。
- **`kernel/src/sys/keep.c:17`、`:199-250`**
  - 今の形：「以前の系」の文書の名前二つと、その引き継ぎの処理があります。
  - なぜ誤りか：`AGENTS.md:50` の「互換は持たない」に反します。カーネルが過去の文書の形を知っています。
  - 本来の持ち主：消します。
- **`kernel/src/gui/wm.c:1169-1221`、`kernel/src/dev/audio.c:794-850`、`kernel/src/gui/screenmode.c:398-418`**
  - 今の形：各部品が自分の `line` と `text` で文字列を解析し、組み立てます。`Desktop.conf` の読み手は、もう無い `pointer_y` と `refresh` の鍵まで知っています（`wm.c:1209`）。
  - なぜ誤りか：同じ形の解析と組立てが部品ごとに三つあります。カーネルの部品が書式を持ちます。
  - 本来の持ち主：設定の道の一つの型。

### 2-5. 網の方針

- **`kernel/src/net/net.c:758-904`**
  - 今の形：DHCP の client がカーネルにあります。再送の間隔も定数です（`:904`）。
  - なぜ誤りか：住所の決め方は方針です。固定の住所を選ぶ設定がありません。
  - 本来の持ち主：システムのプロセス（網の窓口）。
- **`kernel/src/net/net.c:950-983`、`:971-975`**
  - 今の形：既定の DNS は DHCP の値か、起動の言葉 `dns-server=` です。
  - なぜ誤りか：利用者の選択が、起動の項目を手で編むことです。動いている間に変えられません。
  - 本来の持ち主：設定の道とシステムのプロセス。
- **`kernel/src/net/net.c:985-988`、`:2305-2327`**
  - 今の形：DNS の cache と、TCP での DNS の問合せがカーネルにあります。
  - なぜ誤りか：名前解決は応用の処理です。カーネルが TCP の上の応用を話しています。
  - 本来の持ち主：システムのプロセス。
- **`kernel/src/net/net.c:2486-2499`**
  - 今の形：TCP の buffer の大きさが起動の言葉 `tcp-rx-bytes=`・`tcp-tx-bytes=` です。
  - なぜ誤りか：同じです。
  - 本来の持ち主：設定の道。
- **`kernel/src/net/net.h:23-26`、`net.c:2470`、`common/abi.h:734-740`**
  - 今の形：最初の adapter だけが stack の物です。`struct n_netinfo` は一組の住所だけを返します。
  - なぜ誤りか：`AGENTS.md:13` の「数が増えても成り立つ形」に反します。adapter ごとの設定が存在できません。
  - 本来の持ち主：カーネルが adapter の一覧を渡し、システムのプロセスが構成を決めます。

### 2-6. 遠隔のデスクトップの server

- **`kernel/src/vm/rdp.c:1-20`（全 1803 行）、`kernel/src/dev/audio.c:727-742`、`kernel/src/gui/wm.h:51-70`**
  - 今の形：RDP の server がカーネルの中にあります。接続、capability の交換、clipboard の channel まで持ちます。
  - なぜ誤りか：網の応用の protocol です。VM の分離は正しい場所を意味しません。窓の管理と音に remote の特例が入ります（`abi.h:581-582`、`display_abi.h:76`）。
  - 本来の持ち主：システムのプロセス。

### 2-7. 入力の解釈

- **`kernel/src/dev/input.c:15-30`**
  - 今の形：US 配列の文字の表がカーネルに一つだけあります。
  - なぜ誤りか：キーボードの配列の設定が存在できません。日本語の配列で記号が違う字になります。
  - 本来の持ち主：カーネルは物理キーの番号を渡します。文字への変換は UI（入力のプロセス）です。
- **`kernel/src/dev/usbhid.c:81-96`**
  - 今の形：USB の usage を PS/2 の scancode set 1 に変えてから文字にします。
  - なぜ誤りか：USB の装置の値を、別の装置の古い形へ通しています。Nocturne の下限の機械に合いません。
  - 本来の持ち主：`input` の口を usage にします。
- **`kernel/src/dev/usbhid.c:18-19`**
  - 今の形：キーの繰返しの 500 ms と 33 ms が定数です。
  - なぜ誤りか：利用者の設定です。しかもドライバーの中です。
  - 本来の持ち主：UI。
- **`kernel/src/gui/wm.c:2604-2610`**
  - 今の形：ポインターの加速の閾値と、五段の倍率の表が定数です。
  - なぜ誤りか：マウスごとの設定が存在できません。倍率の段を増やすと ABI と表と窓の三か所が変わります。
  - 本来の持ち主：UI。
- **`docs/nocturne-native-abi.md:681`**
  - 今の形：マウスの上下の向きはカーネルが自動で決め、設定は無いと書きます。
  - なぜ誤りか：判定に誤った時の戻し道がありません。
  - 本来の持ち主：設定の道。

### 2-8. 音の方針

- **`kernel/src/dev/audio.c:80-81`、`:807-811`**
  - 今の形：音量の既定 80 と、音量の二乗の曲線がカーネルにあります。音量は機械に一つです。
  - なぜ誤りか：出口ごとの音量が存在できません。
  - 本来の持ち主：設定の道。曲線は音の層の方針です。
- **`kernel/src/dev/audio.c:85`、`:698-710`、`audio.h:136`**
  - 今の形：「the one the user chose」と書いた `chosen` があります。`audio_sink_choose` を呼ぶ者は `kernel/src` の中に一つもありません。
  - なぜ誤りか：出口を選ぶ部品だけがあり、ABI も窓もありません。Settings の Sound の頁にも出口の選択がありません（`settings.ts:240-247`）。
  - 本来の持ち主：設定の道と UI。
- **`kernel/src/gui/wm.c:1399`**
  - 今の形：音量を変えると、カーネルの UI が `audio_system_tone(880, 70)` を鳴らします。
  - なぜ誤りか：音の高さと長さが UI の定数で、カーネルの中です。
  - 本来の持ち主：UI。

### 2-9. 起動と利用者の決定

- **`kernel/src/fs/store.c:395-416`、`store.h:27-29`**
  - 今の形：`C:\Users` の最初のフォルダーが「機械を使う人」です。無ければ `User` を作ります。
  - なぜ誤りか：フォルダーの並びが利用者の決定です。利用者の数は一人に固定です。
  - 本来の持ち主：システムのプロセス（セッション）。
- **`kernel/src/fs/store.c:432-436`、`:397-416`**
  - 今の形：カーネルが `Nocturne`、`Users`、`AppData`、`AppData\Temp` のフォルダーを作り、Temp を空にします。
  - なぜ誤りか：利用者のフォルダーの中の配置はカーネルの知ることではありません。
  - 本来の持ち主：システムのプロセス。
- **`kernel/src/sys/proc.c:251-263`**
  - 今の形：カーネルが起動の時に `USERFOLDER=`・`TEMP=` の文字列を作って stack に積みます。
  - なぜ誤りか：カーネルが環境変数の名前を知っています（§3-5）。
  - 本来の持ち主：起動する側のシステムのプロセス。
- **`kernel/src/sys/proc.c:348-360`、`kernel/src/init.c:66-71`**
  - 今の形：カーネルが `FOLDER=` と `TERMINAL=0 1 2` を作ります。
  - なぜ誤りか：同じです。
  - 本来の持ち主：同じです。
- **`kernel/src/sys/proc.c:220-224`**
  - 今の形：入口が実行不可の segment を「old tcc build」として実行可にします。
  - なぜ誤りか：古い道具のための互換の特例が loader にあります。`abi.h:252` の書込みと実行を分ける約束の例外です。
  - 本来の持ち主：消します。
- **`kernel/src/init.c:53`、`:60`、`kernel/src/sys/stream.c:166-181`、`kernel/src/dev/fbcon.c:151-224`**
  - 今の形：`nogui` の分岐があります。カーネルが ANSI の端末を解釈し、矢印キーを `\x1b[A` に変えます。
  - なぜ誤りか：端末の方言がカーネルにあります。これは UNIX の形です。
  - 本来の持ち主：panic と起動の表示だけカーネル。端末は UI。

### 2-10. 電源の方針

- **`kernel/src/dev/pm.c:33-36`、`:86-107`**
  - 今の形：窓を閉じさせて数え、待ち時間を定数で決め、toast を出す手順がカーネルです。
  - なぜ誤りか：「窓が閉じなければ取り消す」は UI の方針です。眠りまでの時間、ふたを閉じた時の動作、という設定の場所がありません。
  - 本来の持ち主：システムのプロセス（セッション）。実行の順はカーネル。

### 2-11. clipboard

- **`kernel/src/gui/wm.c:497-499`、`:3575-3600`、`wm.h:21`**
  - 今の形：clipboard は機械に一つで、カーネルの memory にあり、UTF-8 だけで、上限 1 MiB です。
  - なぜ誤りか：利用者のセッションの物です。形式の区別がありません。どのアプリも読めます。
  - 本来の持ち主：UI。

### 2-12. 逆向き：ユーザー空間にあって、カーネルの責務である物

- **`user/libc/sys.c:386-400`、`docs/nocturne-native-abi.md:281-283`**
  - 今の形：libc が `USERFOLDER` と `TEMP` の文字列を子へ渡します。これが「誰のために働くか」の全部です。
  - なぜ誤りか：識別がアプリの書ける文字列です。アプリは嘘の値で子を起こせます。起動の属性はカーネルが持って引き継ぐ物です。
  - 本来の持ち主：カーネル（process の属性として運ぶ）とシステムのプロセス（値を決める）。
- **`user/libc/time.c:62`、`user/include/time.h:21`、`common/abi.h:298`、`kernel/src/vm/hv/hv_util.c:128-133`**
  - 今の形：`abi.h` は壁時計を Unix time と書きます。`time.h` は「the clock keeps local time」と書きます。`localtime` は `gmtime` です。Hyper-V の道はカーネルの時計に時差を足します。
  - なぜ誤りか：壁時計の意味が三か所で違います。時間帯の設定が存在しません。時計はカーネルが UTC で持つ物です。
  - 本来の持ち主：カーネルは UTC だけ。時間帯は設定の道。
- **`user/apps/js.c:879`、`:896`、`:902`、`:927`、`:973`**
  - 今の形：一般の JS の host が `__syscall(SYS_SCREEN_INFO, …)` を直に呼びます。
  - なぜ誤りか：どの TypeScript のアプリも `desktop.set` でシステムの設定を変えられます。設定の窓の特権がありません。libc の包みも通りません。
  - 本来の持ち主：設定の道の一つの口。

---

## 3. 「全部ファイル」の考えが残っている所

1. **`common/display_abi.h:13-66`、`kernel/src/gui/wm.c:3604-3612`、`:3536-3573`**
   - 今の形：`SYS_SCREEN_INFO` 一つが、第三引数と第二引数の大きさで意味を変えます。`0` と `sizeof` で二つの返事、`SCREEN_SETTINGS_GET`、`SET`、`SCREEN_MODES_GET`、`SCREEN_MODE_SET`、`SCREEN_WATCH` です。画面の番号は op の上位のビットです（`op | screen << 8`）。
   - なぜ誤りか：これは一つの番号に op と struct を押し込む形で、万能の ioctl です。ポインターの速さが「画面の情報」の中にあります。
   - 本来の持ち主：設定の道。
2. **`kernel/src/sys/keep.h:5-7`、`keep.c:96-100`**
   - 今の形：設定の値の永続の形が `AppData\Nocturne\<name>.conf` の `key=value` の文字列です。`#` の行に「change it in Settings」と書きます（`wm.c:1218`、`audio.c:845`、`screenmode.c:409`）。
   - なぜ誤りか：「no interface」と書きながら、人に読ませる注釈があります。編集できる文書のままです。
   - 本来の持ち主：設定の窓口の持つ形。形はカーネルにも利用者にも見せません。
3. **`kernel/src/sys/keep.c:314-316`、`kernel/src/fs/store.c:782-785`**
   - 今の形：`AppData\Nocturne` は属性 0 のフォルダーです。
   - なぜ誤りか：どのアプリも `Sound.conf` を書き換え、次の起動でカーネルに読ませられます。`nocturne-native-abi.md:1152` の「書くのはその持ち主だけ」を守る機構がありません。
   - 本来の持ち主：設定の窓口。
4. **`user/libc/webprefs.c:1-7`、`docs/nocturne-native-abi.md:1140-1143`**
   - 今の形：プログラムの設定の通知は「文書を見張って読み直す」です。
   - なぜ誤りか：通信を文書の変更で表しています。読み書きの競合を claim で避けています。
   - 本来の持ち主：プログラムの設定もプログラムの持ち物のまま、OS が型と通知の道具を渡します。
5. **`common/abi.h:221-223`、`kernel/src/sys/syscall.c:805-811`、`kernel/src/sys/proc.c:146-152`**
   - 今の形：`n_launch.settings` は `"NAME=value"` の文字列で、`getenv` が読みます。ABI はこれを「設定」と呼びます。カーネルは `=` の有無を確かめます。
   - なぜ誤りか：環境変数の改名です。`AGENTS.md:58` が禁じる形です。「設定」の語を環境変数に使うので、OS の語彙の中に本物の設定の場所がありません。
   - 本来の持ち主：型のある起動の属性（作業の場所、利用者、端末）。
6. **`common/abi.h:221-222`、`kernel/src/sys/syscall.c:707-709`、`:726-727`、`:748-753`、`kernel/src/sys/object.h:27-32`、`kernel/src/sys/stream.h:1-3`**
   - 今の形：handle の 0・1・2 が入力・出力・ログの約束です。`object_ops` に一般の `read` と `write` があります。channel の呼出しが byte stream にも効きます。
   - なぜ誤りか：標準入出力と read・write の形です。message channel の呼出しの意味が相手の種類で変わります。
   - 本来の持ち主：端末は UI のプロセス、ログは専用の口。
7. **`common/abi.h:173-178`、`docs/nocturne-native-abi.md:99-108`**
   - 今の形：ABI がフォルダーの配置を `N_FOLDER_*` の文字列で定めます。`user/libc/tls.c:140` は `C:\Nocturne\Certificates` を読みます。
   - なぜ誤りか：信頼する証明書という設定が「フォルダーの中の文書」です。SYSTEM の属性のため、どのアプリも足せません（`store.c:657`）。利用者が足す道がありません。
   - 本来の持ち主：設定の道とシステムのプロセス。
8. **`user/libc/sys.c:537-549`、`user/apps/files.c:213-218`**
   - 今の形：名前からのプログラムの検索は、`C:\Nocturne\System` と `C:\Programs\*` を順に覗きます。文書の種類から開くアプリは C の分岐で決め打ちです。
   - なぜ誤りか：アプリの登録がフォルダーの走査です。既定のアプリを選ぶ設定が存在できません。
   - 本来の持ち主：システムのプロセス（アプリの登録）と設定の道。
9. **`common/abi.h:82-90`、`:762-765`、`user/include/nocturne.h:423-444`**
   - 今の形：`udp_socket`、`tcp_accept`、`struct n_sockaddr`、`backlog` です。TCP の閉じ方が `SYS_HANDLE_CLOSE` と別です。
   - なぜ誤りか：`AGENTS.md:58` が禁じる BSD の形の改名です。§1 の「カーネル資源はすべて種類つきハンドル」と汎用操作の約束から外れます。
   - 本来の持ち主：カーネルの網の機構は残し、形を Nocturne の handle と事象に揃えます。
10. **`user/apps/init.c:11`、`:96-104`**
    - 今の形：`C:\Tests\autorun.sh` があれば、init は実行して電源を切ります。
    - なぜ誤りか：文書の有無が機械の動きを切り替えます。試験の入口が製品の init にあります。
    - 本来の持ち主：試験の道具。
11. **`user/apps/init.c:109-111`**
    - 今の形：`nvrm` の文書があれば GPU のドライバーを起こします。
    - なぜ誤りか：文書の有無が装置の方針です。
    - 本来の持ち主：システムのプロセス（ドライバーの対応）。
12. **`user/apps/init.c:32-40`、`:75-83`、`.claude/rules/kernel.md:21`**
    - 今の形：機械のログが利用者の `AppData\Nocturne\System.log` に書かれます。上限 32 MiB は定数です。
    - なぜ誤りか：機械の物が利用者のフォルダーにあります。
    - 本来の持ち主：システムのプロセスと機械の場所。
13. **`user/apps/agent.c:13-17`**
    - 今の形：`agent.conf` の `key=value` を利用者が書きます。`api_key` もこの文書です。設定の窓はありません。
    - なぜ誤りか：編集する文書そのものです。秘密が平文です。
    - 本来の持ち主：プログラムの設定の窓と、秘密を預かるシステムのプロセス。
14. **`boot/limine.conf:17-21`、`.claude/rules/kernel.md:20`**
    - 今の形：`nogui` の入口が起動の項目です。計測の `prof` は「手で編む」と規則に書いてあります。
    - なぜ誤りか：編集する文書で機械の動きを変えています。
    - 本来の持ち主：§5 の表を見ます。

---

## 4. 境界を越える依存

### 4-1. include の向き（`kernel/src` の grep で確かめました）

| 下の層から | 上の層へ | 行 |
|---|---|---|
| `dev/audio.c` | `gui/wm.h` | `audio.c:3` |
| `dev/pm.c` | `gui/wm.h`、`fs/store.h`、`sys/keep.h` | `pm.c:12`、`:15`、`:19` |
| `dev/pciuser.c` | `gui/wm.h`、`gui/screenmode.h` | `pciuser.c:31-32` |
| `dev/hibernate.c` | `fs/store.h` | `hibernate.c:27` |
| `vm/hv/hv_util.c` | `gui/wm.h` | `hv_util.c:11` |
| `vm/rdp.c` | `gui/wm.h` | `rdp.c:25` |
| `sys/syscall.c` | `gui/wm.h`、`net/net.h`、`fs/store.h` | `syscall.c:18`、`:31-32` |
| `sys/keep.c` | `fs/store.h` | `keep.c:5` |
| `sys/proc.c` | `fs/store.h` | `proc.c:14` |
| `gui/wm.c` | `dev/audio.h`、`dev/pm.h`、`sys/proc.h`、`sys/keep.h` | `wm.c:31`、`:33`、`:41`、`:45` |
| `gui/screenmode.c` | `sys/keep.h`、`gui/wm.h` | `screenmode.c:26`、`:30` |

なぜ誤りか：装置が UI を知り、UI が装置を知ります。依存が輪になります。`audio ↔ wm`、`pm ↔ wm`、`pciuser ↔ wm`、`screenmode ↔ wm` です。

### 4-2. カーネルが UI を呼ぶ

- `kernel/src/dev/audio.c:816`：`sound_put` の中で `wm_taskbar_changed()` を呼びます。音の部品が、タスクバーが自分を描くことを知っています。
- `kernel/src/vm/hv/hv_util.c:138`：時計を変えて `wm_taskbar_changed()` を呼びます。`wm.h:33-34` の注釈は呼ぶ側を `sys/clock.c` と書きますが、`clock.c` に呼出しはありません。
- `kernel/src/dev/pm.c:86-107`：`wm_window_count`、`wm_close_windows`、`wm_notify` を呼びます。
- `kernel/src/dev/pm.c:247-248`、`:270`、`:291`、`:306-309`、`:338`：利用者への文を `wm_notify` で出します。
- `kernel/src/dev/pciuser.c:266`、`:438`、`:990`：`wm_repaint`、`wm_screen_direct`、`wm_screen_state`。ドライバーの呼出しの答えを窓の管理が作ります。
- `kernel/src/gui/screenmode.c:193-194`：`wm_screen_mode`、`wm_screen_info_changed`。

本来の形：カーネルの部品は状態の変化を見張りの口で知らせます。誰が聞くかを知りません。

### 4-3. UI が装置に触れる

- `kernel/src/gui/wm.c:1404`、`:2458`、`:2697`：`audio_set_volume`、`audio_set_muted` を直に呼びます。
- `kernel/src/gui/wm.c:1773-1775`：描く時に `audio_volume()`、`audio_output()` を読みます。
- `kernel/src/gui/wm.c:1399`：`audio_system_tone`。
- `kernel/src/gui/wm.c:1225`：`pm_request`。
- `kernel/src/gui/wm.c:1229`：`proc_spawn_simple` で直にプログラムを起こします。

カーネルの中なので、§1 の呼出しを通りません。UI と Settings の窓とで道が二つあります。

### 4-4. カーネルが利用者の文書を読み書きする

- `kernel/src/sys/keep.c:106-112`：読みます。
- `kernel/src/sys/keep.c:154`：書きます。
- `kernel/src/sys/keep.c:244`：利用者のフォルダーの文書を消します。
- `kernel/src/sys/keep.c:315`：利用者のフォルダーの中にフォルダーを作ります。
- `kernel/src/fs/store.c:397-416`：利用者のフォルダーの中の `AppData\Temp` を空にします。

### 4-5. `sys_screen_info` が画面の方針を ABI へ出す

- `kernel/src/gui/wm.c:3562`：`si.height = sh - TASKBAR_H` です。`screen_format` の高さはタスクバーを引いた値です。
- `common/display_abi.h:17`：`/* the work area, as struct screen_info */`。
- なぜ誤りか：画面の情報の ABI がタスクバーの高さを含みます。

---

## 5. 起動の言葉（cmdline）という設定の道

`kernel/src/boot.c:70-95` が読みます。下の全部が、起動の項目を手で編むか起動の画面で打つ設定です。動いている間に変わらず、窓がなく、値の型もありません。

| 言葉 | 場所 | 持ち主 |
|---|---|---|
| `nogui` | `init.c:53` | 起動の選択。今は `limine.conf` |
| `nosmp` など | `arch/smp.c:303` | 試験の道具 |
| `noaml` | `dev/acpi.c:480` | 試験の道具 |
| `fbsim10` | `dev/fb.c:101` | 試験の道具 |
| `noresume` | `dev/hibernate.c:600` | 設定の道 |
| `noiommu` | `dev/iommu.c:131` | 機械の設定（権限が要る） |
| `appdrivers-unisolated` | `dev/iommu.c:829`、`:875` | 機械の設定（権限が要る） |
| `noappdrivers` | `dev/pciuser.c:811` | 機械の設定（権限が要る） |
| `appdrivers=any` | `dev/pciuser.c:812` | 権限の模型そのものの代わりです |
| `fbhold=` | `dev/pciuser.c:237` | 機械の設定 |
| `nopcid` | `mm/vmm.c:649` | 試験の道具 |
| `prof`、`prof_ms=` | `sys/prof.c:274-275` | 実行中に切り替える計測の窓 |
| `dns-server=` | `net/net.c:954-978` | 網の設定 |
| `tcp-rx-bytes=`、`tcp-tx-bytes=` | `net/net.c:2488` | 網の設定 |
| `e1000-itr-us=` | `vm/e1000.c:185` | VM の試験の道具 |
| `nogpu`、`gpu-enable`、`gpu-slow-test`、`gpu-timing` | `vm/virtio_gpu.c:240-672` | VM の試験の道具 |

`AGENTS.md:60` は「システムの内部を引数や旗の起動で組まず」と書きます。コードはこの規則に反しています。

`net.c:977`、`:2487` は `extern const char *kernel_cmdline` を自分で宣言し、文字列を自分で解析します。`boot.c` の `cmdline_number` と重複します。

---

## 6. 設定を表す共通の型と道：無い物

調べた結果、共通の型も共通の道もありません。`common/`、`user/include/`、`kernel/src` の header に `n_setting` の類はありません。`n_settings` は環境変数です。

今の「設定」は次の全部です。形はそれぞれ別です。

| 設定 | 値の形 | 読み書きの口 | 保存 | 窓 |
|---|---|---|---|---|
| ポインターの速さ | `int8_t`（`display_abi.h:32`） | `SYS_SCREEN_INFO` の op | `Desktop.conf` | `settings.ts:230-235` |
| なめらかな階調 | `uint8_t`（`display_abi.h:36`） | 同じ | `Desktop.conf` | `settings.ts:225-226` |
| 音量・消音 | `int32_t`・`uint32_t`（`abi.h:586-590`） | `SYS_AUDIO_OUTPUT` | `Sound.conf` | `settings.ts:245-247` とタスクバー |
| 画面のモード | `struct n_screen_mode`（`abi.h:448-452`） | `SYS_SCREEN_INFO` の別の op | `Displays.conf` | `settings.ts:150-222` |
| ブラウザーの好み | `webprefs` の文字列 | 文書 | `AppData\Browser\preferences` | `browser_settings.h` |
| エージェントの設定 | `key=value` | 文書 | `agent.conf` | 無し |
| 既定の DNS・TCP の buffer | 文字列 | cmdline | `limine.conf` | 無し |

一つの設定を足すと、`display_abi.h` の struct、`wm.c` の put・line・text・`sys_screen_info`、`js.c` の binding、`ui.d.ts`、`settings.ts`、ABI の文書の §9・§12 が変わります（`docs/records/settings-ownership-20261027.md:39-40` の塊の表が同じ範囲を示します）。これが持ち主の言う「決め打ちのスパゲッティ」の作り方です。

無い物を一つずつ書きます。

1. **値の型。** 真偽、整数、列挙、文字列、一覧を表す型がありません。各 struct の欄が C の型です。`desktop_settings` は `reserved[5]` で固定です（`display_abi.h:39`）。
2. **範囲。** 値の範囲を問う道がありません。範囲は `wm.c:3543`、`syscall.c:928`、`wm.c:1194` のように、確かめる所に定数で書かれます。窓の側にも同じ値を書きます（`settings.ts:231-233` の -2〜2 と名前、`:245` の 0〜100）。
3. **既定。** 既定を問う道がありません。既定はカーネルの初期値です（`audio.c:80`、`wm.c:3021`、`pointer_speed` は 0 埋め）。「既定に戻す」がありません。
4. **能力。** 今の機械で何ができるかを言う共通の道がありません。`smooth_gradients_available`（`display_abi.h:38`）と `pm_can_sleep`（`wm.c:1077`）が一つずつの特例です。
5. **列挙。** 設定の一覧を問う道がありません。Settings の頁は `settings.ts:17-19` の三つの決め打ちです。無い頁：キーボード、時間と時間帯、言語、網、電源、利用者、既定のアプリ、見た目、起動。
6. **変更の通知。** 三つの別の口があります（`SCREEN_WATCH`、`SYS_AUDIO_OUTPUT_WATCH`、`SYS_ENTRY_WATCH`）。どれも何が変わったかを言いません。設定を足すたびに口を足す形です。特例として `wm_taskbar_changed`（§4-2）もあります。
7. **適用。** 適用と保存が分かれていません。`sound_put` は値を入れて、保存の印を付け、知らせます（`audio.c:805-817`）。まとめて適用する道がありません。
8. **失敗の戻し。** 試行と戻しは画面のモードだけです（`screenmode.c:488-521`）。ほかの設定には失敗の報告がありません。読み戻しで範囲外の値は黙って捨てます（`keep.h` の `line`、`wm.c:1194`）。
9. **保存の失敗の報告。** 保存の失敗は `kprintf` だけです（`keep.c:156`）。呼んだ窓に戻りません。設定の窓は保存されたかを知りません。
10. **権限。** 無いです（§7）。
11. **利用者ごとと機械ごとの区別。** 無いです。三つの文書は全部 `store_user_folder()` の下です（`keep.c:89-100`）。モニターのモードと音の出口は機械の物に近く、ポインターの速さは人の物に近いですが、区別する欄がありません。
12. **利用者が決まる前の値。** 無いです。`keep_start` は init の前に、カーネルが決めた唯一の利用者の文書を読みます（`init.c:55-58`）。ログイン画面の前の設定という場所がありません。
13. **装置ごとの区別。** 共通の形がありません。モニターだけが文字列の鍵を持ちます（`screenmode.c:398`）。マウス、キーボード、音の出口、網の adapter の鍵はありません。画面は順の番号です（`display_abi.h:54-56`）。
14. **版と書式の変更。** 無いです。鍵の変更は `older[]` と `deep_colour` の特例です（`keep.c:17`、`wm.c:1201`）。
15. **既定の管理者の方針。** 機械の管理者が利用者の設定に上限や固定を掛ける道はありません。
16. **実行中の切替えの区分。** 「すぐ効く」と「再起動で効く」を言う道がありません。cmdline の物はすべて後者ですが、窓にも出ません。
17. **監査の記録。** 誰がいつ何を変えたかの記録はありません。`kprintf` の `keep:` の行と `screen:` の行だけです。
18. **プログラムの設定の道具。** OS が渡すのはフォルダーと保存の呼出しだけです（`nocturne-native-abi.md:1140-1143`）。型のある設定と通知の道具はありません。アプリごとに書式を作ります（`webprefs.c`、`agent.c`）。
19. **テーマ。** `user/include/ui.h:26-33` の色と大きさが定数です。`ui_theme` は `const` です（`ui.c:189`）。明暗や字の大きさの設定が存在できません。
20. **画面の倍率。** ABI に倍率の欄がありません（`struct screen_format`、`display_abi.h:16-21`）。
21. **時間帯と言語。** `time.h:21` は時間帯が無いと書きます。表示の言語の値はどこにもありません。

---

## 7. 権限と利用者の模型

### 7-1. 今の模型の全部

- **`kernel/src/sys/process.h:15-60`**：`struct process` に利用者の欄がありません。プログラムの身元の欄もありません。
- **`kernel/src/fs/store.h:9-10`**：文書の権限は `bool user` です。カーネルか、それ以外かです。
- **`common/store_format.h:164`、`:167`、`kernel/src/fs/store.c:657-658`**：SYSTEM と PRIVATE の二つの属性だけです。どのアプリも `C:\Users` の全部を読み書きできます。
- **`kernel/src/dev/pciuser.c:811-812`、`kernel/src/sys/proc.h:22`**：特権は `pid == init_pid` の比較一つです。
- **`kernel/src/mm/pager.c:237`、`kernel/src/mm/shared.c:742`**：作った本人だけという確かめが object ごとにあります。
- **`common/abi.h:213-219`**：channel の `sender_app` は pid です。身元の種類を言いません。

`N_ERR_DENIED` の使い手を grep で全部見ました。`pciuser.c`、`fs/`、`mm/`、`wm.c:3105` だけです。`sys/syscall.c`、`dev/audio.c`、`gui/screenmode.c`、`sys/keep.c`、`net/net.c`、`dev/pm.c` には一つもありません。

### 7-2. 誰が何を変えられるか

全部「どのアプリでも」です。確かめの行がありません。

| 操作 | 場所 |
|---|---|
| 音量・消音 | `syscall.c:924-939` |
| ポインターの速さ・階調 | `wm.c:3536-3554` |
| 画面のモード | `screenmode.c:439`、`:479-527` |
| 電源を切る・再起動 | `syscall.c:1205-1210` |
| 別のアプリを止める。init も止められます | `syscall.c:851-869`、`process.c:155-161` |
| 画面の全部を読む | `wm.c:3379-3400` |
| clipboard を読む | `wm.c:3592-3600` |
| 全部のアプリの一覧。物理番地を含みます | `syscall.c:179-194`、`abi.h:269` |
| システムログを読み書きする | `syscall.c:882-885` |
| 設定の文書を書き換える | `store.c:657-658`、`keep.c:315` |
| 名前付きの channel の名前を先に取る | `channel.c:99-121` |
| 窓を動かす、題を変える（持ち主の確かめ無し） | `wm.c:3498-3514` |
| TCP の損失の模擬を入れる（自分の接続だけ） | `net.c:2704-2709` |

### 7-3. システムのプロセスは何の権限で動くか

- システムのプロセスの層はありません。`user/apps/init.c:155-165` の init は、ログを写し、`nvrm` を起こし、60 秒ごとに眠ります。
- init が持つ特権は PCI の claim だけです。
- `nvrm` は init から受けた handle だけが特別で、それ以外はふつうのアプリです（`init.c:141-149`）。
- `user/apps/uihost.c` はアプリごとの部品のエンジンで、システムの権限を持ちません。
- 機械全体の仕事は全部カーネルの thread です：`desktop`、合成、`keep`、`screen mode`、`net`、`store`、音、電源。
- 利用者の区別も、ログインも、セッションもありません。`store_user_folder()` が一つの文字列を返します（`store.c:395-416`）。

なぜ誤りか：境界を守るためには、カーネルが身元を運び、システムのプロセスが判断します。今はその土台が両方ともありません。だから権限の要る物が全部カーネルの中に入っています。

本来の持ち主：カーネルは身元を process の属性として運び、channel の相手を証明します。身元と役割の判断はシステムのプロセスです。

---

## 8. 規則・メモリー・文書の中の誤った前提

### 8-1. メモリー

- **`memory/gui-first-not-linux.md:8`**：「窓の系と GPU はカーネルの第一級の部品」。
  - なぜ誤りか：「GUI が先」を「GUI をカーネルに入れる」と固定します。第一級は優先の話で、特権の層の話ではありません。`wm.c` の全部の根拠になります。
  - 持ち主の言葉の "THE UI. THE SYSTEM PROCESSES. KERNEL." と反します。

### 8-2. `AGENTS.md`

- **`AGENTS.md:60`**：「設定の持ち主はシステムで」。
  - なぜ誤りか：「システム」の定義がありません。今のコードでは「カーネルの部品」と読まれました。三層を分けていません。
- **`AGENTS.md:60`**：「プログラムの設定はそのプログラムの持ち物」。
  - なぜ誤りか：持ち物であることは正しいです。ただしこの一文は、OS が型と道具を渡さないことの根拠に使われました（`nocturne-native-abi.md:1136`）。
- **`AGENTS.md:61`**：「ディスクは一台」。
  - なぜ誤りか：`AGENTS.md:13` の「数が増えても成り立つ形」と矛盾します。`store.h:3` の「one volume」と `N_FOLDER_*` の決め打ちの根拠です。
- **`AGENTS.md:5` 付近（"UNIX ではない"）と `:58` の本文**：規則は正しいです。コードが §3-5、§3-6、§3-9 で反しています。

### 8-3. `.claude/rules/`

- **`map.md:31`**：`sys/keep.c` の行「`.conf` に保存し、init の前に読み戻す」。今の形を地図が正として書きます。
- **`map.md:35`**：`gui/` の行「窓管理と、画面ごとの合成のスレッド」。カーネルの地図に窓管理があります。
- **`map.md:34`**：`net/` の行「TCP・DHCP・DNS まで」。同じです。
- **`map.md:87`**：「claim するのは init だけ」。pid の比較を規則として固定します。
- **`kernel.md:22`**：「解像度と拍は EDID を変数にして、カーネルと表示経路」が決める。
  - なぜ誤りか：EDID を変数にするのは正しいです。決める者をカーネルと書く部分が誤りです。
- **`kernel.md:20`**：「limine の起動の項目を手で編む」。編集する文書を手順として固定します。
- **`kernel.md:21`**：`AppData\Nocturne\System.log`。機械のログを利用者のフォルダーに置く形を固定します。

### 8-4. ABI の文書

- **`nocturne-native-abi.md:1135`**：「値を効かせる者がその値の持ち主」。
  - なぜ誤りか：適用と所有を一つにします。音量を効かせるのはミキサーでも、音量の所有はカーネルではありません。この一文が全部の設定をカーネルへ入れました。
- **`nocturne-native-abi.md:1137`**：「値も文書も持たない」操作盤。
  - なぜ誤りか：窓が値を持たないのは正しいです。その裏に、値を受ける者がカーネルしかありません。
- **`nocturne-native-abi.md:1144`**：「今はどれもカーネルの中」。
  - なぜ誤りか：誤りを事実の説明として書きます。
- **`nocturne-native-abi.md:1151`**：「インターフェースではない」。
  - なぜ誤りか：どのアプリも書けて、カーネルが次の起動で読みます。形の上でインターフェースです。
- **`nocturne-native-abi.md:1162`**：「以前の系からの引き継ぎ」。`AGENTS.md:50` の「互換は持たない」と矛盾します。
- **`nocturne-native-abi.md:28`**：「カーネル資源はすべて種類つきハンドル」。
  - なぜ誤りか：文書とフォルダーはパスの文字列で届きます。`N_KIND_FOLDER` の handle はありません（`store.c:664` は種類の報告だけです）。TCP と UDP に別の閉じ方があります。
- **`nocturne-native-abi.md:270`**：「設定は `"名前=値"` の形」。環境変数を設定と呼びます。
- **`nocturne-native-abi.md:681`**：「マウスの上下の向きはカーネルが自動で決め」。
- **`nocturne-native-abi.md:889`**：欠番の一覧が `20〜25` です。`abi.h:47` は `21-25` で、20 は `SYS_APP_CPU_TIME` です。
- **`nocturne-native-abi.md:928`**：`SYS_DMESG` という、もう無い名前です。
- **`common/abi.h:46`**：「The ABI keeps no older form」。
  - なぜ誤りか：同じファイルの `:120-122` と `:196-197`、`display_abi.h:13-15`（`so existing programs are unaffected`）、`:53`、`gpu_abi.h:6` が古い形を残します。

### 8-5. 作り直しの文書

- **`docs/records/settings-ownership-20261027.md:9`**：「値を効かせる者が持ち主」。
- **`docs/records/settings-ownership-20261027.md:11`**：「今はどれもカーネルの中」。
  - なぜ誤りか：当時の持ち主の問い "why are userland PROGRAMs, forced to talk to the kernel to save settings?" に、システムの部品の設定はカーネルのままという形で答えました。
- **`docs/records/settings-ownership-20261027.md:15`**：「鍵を一つも知らない」を良さとして書きます。カーネルが文書を読み書きすること自体を問いません。

### 8-6. 注釈

- **`kernel/src/sys/keep.h:3`**：「A part of the system that applies something the user chose owns it」。
- **`kernel/src/gui/screenmode.c:3-4`**：「is the system's to say」。「system」はカーネルです。
- **`kernel/src/gui/wm.h:33`**：「What the taskbar shows of state the desktop does not own」。カーネルの部品がタスクバーのために呼ばれることを正当化します。
- **`common/display_abi.h:23`**：「Desktop settings, read and changed by the Settings app」。ABI の型が一つの窓の名前を持ちます。
- **`README.md:103`**：「値はそれを効かせるシステムの部品」の持ち物。利用者向けの文書にも同じ前提があります。

### 8-7. `.claude/oboegaki.md`

設定と持ち主についての行はありません。`oboegaki.md:58` は窓をカーネルの空間の参照として書きます。これは今のコードの事実の記録です。

---

## 9. 根の誤り（重い順）

1. **三つの層が無い。** カーネル・システムのプロセス・UI の区別が設計にありません。「GUI が先」が「GUI はカーネルの中」と読まれました（`memory/gui-first-not-linux.md:8`、`wm.c:1-25`）。
   - これが生むもの：2、4、6、8、9、10、11、12 の全部です。
2. **設定という概念が OS に無い。** 値の型、範囲、既定、能力、通知、適用、戻し、scope の共通の形がありません（§6）。
   - これが生むもの：部品ごとの struct と口（`SYS_SCREEN_INFO`、`SYS_AUDIO_OUTPUT`）、窓の決め打ち（`settings.ts`）、3 と 5 の特例です。一つの設定を足すと八か所が変わります。
3. **「値を効かせる者が持ち主」という規則。** 適用と所有を一つにしました（`nocturne-native-abi.md:1135`、`keep.h:3`）。
   - これが生むもの：装置とカーネルの部品が値も保存も持つ形です。ミキサーが音量の文書を持ち、合成が速さの文書を持ちます。1 を固定し、4 を正当化します。
4. **永続を編集できる文書で表す。** カーネルが `key=value` を解析します。形は「interface ではない」と言うだけで、どのアプリも書けます（`keep.c`、`store.c:782-785`）。
   - これが生むもの：カーネルから利用者の文書への依存（§4-4）、起動の順の縛り（`init.c:55-58`）、`older[]` の互換（`keep.c:17`）、保存の失敗が窓へ戻らないことです。
5. **身元が無い。** process に利用者も身元もありません。利用者は `C:\Users` の最初のフォルダーで、子へは環境変数の文字列で渡ります（`process.h:15-60`、`store.c:395-416`、`sys.c:386-400`）。
   - これが生むもの：6 の全部、7、一人の利用者の固定、ログインの場所が無いことです。
6. **権限が無い。** 設定の呼出しのどれにも呼ぶ側の確かめがありません。特権は `pid == init_pid` の比較一つです（§7-2、`pciuser.c:811-812`）。
   - これが生むもの：権限の要る物をカーネルに入れるしかない形（1 を強めます）、`appdrivers=any` の起動の言葉、`SYS_NET_TEST` や `wait_phys` が全部のアプリへ出ることです。
7. **scope が無い。** 機械・利用者・装置の区別がありません。全部が一人の利用者の `AppData\Nocturne` です（`keep.c:89-100`、`init.c:76-80`）。
   - これが生むもの：機械のログが利用者のフォルダーにあること、音量が機械に一つなこと、マウスごとの設定が存在できないこと、ログイン前の設定の場所が無いことです。
8. **システムのプロセスの層が空。** init は眠るだけです。設定の窓口、アプリの登録、ドライバーの対応、セッション、網の窓口がありません（`user/apps/init.c:155-165`）。
   - これが生むもの：`SYS_GUI_LAUNCH`、フォルダーの走査によるアプリの検索（`sys.c:537-549`）、`files.c:213-218` の既定のアプリの決め打ち、カーネルの DHCP・DNS・RDP です。
9. **UI の契約がカーネルの syscall にある。** `SYS_WIN_*` と `SYS_SCREEN_INFO` と `struct desktop_settings` です（`abi.h:64-81`、`display_abi.h:23-40`、`wm.c:3562`）。
   - これが生むもの：UI を変えると ABI が変わること、一つの番号に七つの意味があること、ポインターの速さが画面の情報にあること、画面の高さからタスクバーを引く ABI です。
10. **デスクトップの中身がカーネルの定数。** アプリの一覧、キーの割当て、寸法、絵、文、キー配列、キーの繰返し、加速です（§2-1、§2-7）。
    - これが生むもの：持ち主の言う "ALL HARD CODED" の大半です。どの設定も、値の置き場を作る前に定数を消す必要があります。
11. **起動の言葉が設定の道になっている。** 20 以上の言葉が `limine.conf` を手で編む形です（§5、`net.c:977`）。
    - これが生むもの：動いている間に変えられない設定、窓が無い設定、製品のコードの中の試験の入口、権限の代用（`appdrivers=any`）です。
12. **カーネルの下の層が UI を呼ぶ。** `audio → wm`、`pm → wm`、`pciuser → wm`、`hv_util → wm` です（§4-1、§4-2）。
    - これが生むもの：依存の輪、カーネルの中の英語の文（`pm.c:99`）、UI をカーネルの外へ移す時の全部の呼出しの作り直しです。見張りの口があるのに使っていません。
13. **パスが権限と登録と切替えを兼ねる。** どのアプリも文字列で全部へ届きます。文書の有無が機械の動きを決めます。フォルダーの中身が設定です（`syscall.c:542-550`、`init.c:96-111`、`abi.h:173-178`、`tls.c:140`）。
    - これが生むもの：handle の約束の例外（`nocturne-native-abi.md:28`）、利用者が証明書を足せないこと、`C:\Tests\autorun.sh` で電源が切れることです。
14. **UNIX の形が改名して残る。** 環境変数を「設定」、0・1・2 の handle、byte stream の `read`・`write`、BSD の socket、ANSI の端末です（§3-5、§3-6、§3-9、`fbcon.c:151`）。
    - これが生むもの：「設定」の語が環境変数に取られていること（2 を見えなくします）、カーネルが環境変数の名前を知ること（`proc.c:251-263`、`init.c:66-71`）です。`AGENTS.md:58` に直接反します。
15. **規則と文書が今の誤りを事実として固める。** 地図と ABI の文書と作り直しの文書が、カーネルの中の物を正の形として書きます（§8）。規則の互換の禁止とコードの互換の残りが矛盾したまま残ります（`AGENTS.md:50`、`abi.h:46`、`keep.c:17`、`proc.c:220-224`）。
    - これが生むもの：次の作業者が今の形に合わせて書くことです。2026-10-27 の作り直しが、問いの答えとしてカーネルの `keep.c` を作った経路がこれです（`docs/records/settings-ownership-20261027.md:3`、`:9`、`:15`）。
