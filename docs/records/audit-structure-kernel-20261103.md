# カーネルと ABI の構造の監査

調べた版は `9be8a389`（2026-11-03）。読むだけで、ビルドも QEMU も動かしていない。

結論：

- 構造の問題は、中核の置き直し `df893170` の外に残った部品に集まっている。窓（`gui`）、ソケット（`net`）、アプリのドライバーの口（`dev/pciuser.c`）、syscall の受け口（`sys/syscall.c`）、ABI の五つ。
- 一番重いのは、窓の管理と合成と入力の割り振りの全部がカーネルにあること（K1）。
- 次に重いのは、ソケットが handle でないこと（K2）と、表示のドライバーの役が PCI の claim の上に載っていること（K4）。
- 機械全体の装置の一覧は無い。装置の種類ごとに別の一覧、別の身元、別の見張りがある（K5）。
- 方針を引き抜く仕事は、機構をカーネルに残し、口を足す形で進んだ。ABI はそのたびに増えた（K16）。
- 中核（scheduler、待ち、handle、事象、見張り、サービス、身元、入口の像）は、読んだ範囲で一つの設計になっている。残してよい。
- 行数の見積りは、`kernel/src` の 62446 行のうち、残す 50219、書き直す 4976、消す・移す 7251。残す行の大半は本体を読んでいない（2 節）。

## 1. 依頼文

```
あなたは Nocturne OS（作業場所 `D:\Programes\llm vibe slop os`）の構造の監査を一人で担当します。読むだけの監査です。今日は 2026-11-03 です。

## このプロジェクトの持ち主の言葉（原文）

"what we need to do now, is to continue to audit, so that we know what the problems are structurally. ... there are to many layers that keep geting blured or done halfassed and its become an issue. things are allways added on top, nothing is ever refracted when needed or anything ... everything seems likes its dont with a LOT of shortsightedness and no consideration as for why. ... this is a pervasive issue with how implementations are done, how code is written, and lack of consideration as to what purpose the code is serving."

## あなたの範囲：カーネルと ABI

`kernel/src` の全部（約 6 万 3 千行）、`common/abi.h` と `common/*_abi.h`、仕様 `docs/nocturne-native-abi.md`。

答える問い：
1. 部品ごとの目的と持ち主。それはカーネルにあるべきものか。とくに `kernel/src/gui`（窓、合成、シェルの口）、`net`、`fs`、`dev` の各ドライバー、`sys/settings.c`、`sys/stream.c`、`sys/prof.c`、`vm/`（`rdp.c` など）。
2. 層の向き。`arch`・`mm`・`sys`・`dev`・`fs`・`net`・`gui` の間の include と呼出しの向きを実際に数える。逆向きと循環を挙げる。
3. syscall の表の一つ一つ（`common/abi.h` の `SYS_*` の全部）。誰が呼ぶか。呼ぶ者が 0 人か 1 人の口、同じことをする二つの口、一つのプログラムのための特例の口を挙げる。
4. 装置の持ち方。PCI、USB、入力、音、画面、ディスク、網の装置を、一つの形で持っているか、別々の一覧か。装置の種類が増えたとき、抜き差しのとき、どこが変わるか。機械全体の装置の一覧はあるか。
5. 同じ仕組みの重複。待ち、輪、一覧、見張り、handle、設定の適用など。
6. 試験や計測のための口と旗で、製品のカーネルに残っているもの。
7. 継ぎ足しの跡。`sys/syscall.c`（88 コミット）、`gui/wm.c`（73）、`sys/sched.c`（54）、`dev/pciuser.c`（42）、`mm/vmm.c`（40）などの履歴を `git log` で読む。古い道と新しい道が両方残っている所、症状ごとの分岐が積もった所を挙げる。

## 守ること

- コードを変えない。ビルド、QEMU、Hyper-V を動かさない。git の状態を変えない。書いてよいのは下の報告のファイル一つだけ。
- 先に `AGENTS.md` と `.claude/rules/map.md` を読む。
- 今あるコードに敬意を払わない。コメント、ファイルの頭の説明、docs、関数の名前は主張であって証拠ではない。証拠は動くコードで、誰が呼ぶか、誰がその値を読むかで確かめる。
- 構造は正しいのに読みにくいだけの所と、構造が間違っている所を分ける。正しい所は正しいと書く。
- 行ごとの不具合を並べない。書くのは構造の問題（層、持ち主、目的、重複、継ぎ足し）だけ。
- codegraph（MCP の `codegraph_explore`）と rg で探す。`SYS_*` の番号での呼出しと表からの呼出しは索引に出ない。
- Linux や他の OS のソースは読まない。仕様は `D:\Programes\OS DEV INFO`。
- 対象の外：ブラウザーの中（`user/libc/web`、`user/apps/browser*`）。カーネルの中にブラウザーだけのためのものがあれば、カーネルの問題として書く。firmware の有無は話題にしない。特定のマシンに決め打ちした判断をしない。
- 既にある報告：`docs/records/audit-compositor-20261102.md`、`docs/records/audit-display-20261102.md`、`docs/records/audit-gpu-generic-20261102.md`、`docs/records/audit-settings-20261103.md`。同じ所見は番号や節で引くだけにする。自分で読んで違っていれば、違うと書く。
- 全部を読めないなら、読んだ範囲と読まなかった範囲を正直に書く。推定は推定と書く。

## 報告

全文を `docs/records/audit-structure-kernel-20261103.md` に日本語で書く。Python で書くなら `newline='\n'` を付ける。節は次のとおり。

1. 依頼文（この文を全部そのまま）
2. 読んだ範囲と読まなかった範囲
3. 構造の問題。一つずつ：題、このコードの目的、今の持ち主とあるべき持ち主、証拠（`file:line`）、継ぎ足しの跡（コミットを見たなら hash）、消す範囲と書き直す範囲（ファイル、行数、数え方）、先に変える必要があるもの、確かさ（証明／可能性）
4. 残してよいもの（読んで構造が正しいと判断したものと、その理由）
5. 集計：部品ごとに、残す・書き直す・消すの行数の見積り
6. 依存の順序：どれを先に変えないと、どれを変えられないか

最後の返事は 15 行以内：ファイルのパス、重い問題の題、行数の集計。全文は返事に書かない。
```

## 2. 読んだ範囲と読まなかった範囲

全部は読んでいない。`kernel/src` の 62446 行のうち、本体まで読んだのは一部である。

### 読んだ（全部か大部分）

- `sys/syscall.c`、`sys/settings.c`、`sys/stream.c`、`sys/watch.c`、`sys/service.c`、`sys/authority.c`
- `sys/object.h`、`sys/wait.h`、`sys/thread.h`、`sys/process.h`、`sys/proc.h`、`sys/syscall.h`、`sys/settings.h`
- `dev/fb.h`、`dev/pci.h`、`dev/usb.h`、`dev/input.h`、`dev/input.c`、`dev/audio.c`、`dev/blk.c`、`dev/power.c`
- `vm/vm.h`、`vm/vm.c`、`vm/devices.h`
- `fs/store.h`、`fs/document.h`
- `common/abi.h`、`common/authority_abi.h`、`common/gpu_abi.h`

### 一部を読んだ

| ファイル | 読んだ所 |
|---|---|
| `gui/wm.c` | 1-520、1700-2680、2950-3370。関数の一覧は全部。520-1700 と 2680-2950 の本体は読んでいない。 |
| `net/net.c` | 1-110、713-745、860-990、1695-1725、2100-2467。関数の一覧は全部。 |
| `dev/pciuser.c` | 178-285、286 以降の表示の節、891-1067、1069 以降の ACPI の節。 |
| `dev/pm.c` | 1-60 と関数の一覧。 |
| `dev/fb.c` | 関数の一覧。 |
| `mm/shared.c` | 1-45 と、`pci_`・`fb.` の出る行。 |
| `mm/program.c` | `store_` の出る行。 |
| `fs/store.c`、`fs/bootrecord.c` | 頭の 50 行と 80 行、`usb_` の出る行。 |
| `sys/channel.c`、`sys/proc.c`、`sys/events.c`、`sys/sched.c` | 頭と、`terminals` などの出る行。 |
| `dev/fbcon.c`、`gui/screenmode.c` | 頭の 20〜30 行。 |
| `arch/*` | include と、`boot_setting`・`prof_`・`stream_`・`setting` の出る行だけ。 |
| `common/settings_abi.h`、`display_abi.h`、`services_abi.h` | 頭の 120 行、50 行、60 行。 |
| `docs/nocturne-native-abi.md` | 節の見出しだけ。 |

### 読まなかった

- `mm/vmm.c`、`pmm.c`、`heap.c`、`vmalloc.c`、`pager.c`、`space.c` の本体。
- `fs/document.c`、`cache.c`、`watch.c` の本体と `common/store/`。
- ドライバーの本体：`xhci`、`ahci`、`nvme`、`hda`、`hda_codec`、`iommu*`、`pci.c`、`acpi.c`、`usbhid`、`usbhub`、`usbmsc`、`usbnet`、`hibernate`、`serial`、`refclock`、`entropy`、`net/rtl8125.c`。
- `vm/rdp.c` の本体（include と `wm_remote_` の出る行だけ）、`vm/hv/*`、`e1000`、`ata`、`tulip`、`ac97`、`vbe`。
- `sys/sched.c`、`wait.c`、`clock.c`、`log.c`、`thread.c`、`process.c`、`alarm.c`、`epoch.c`、`prof.c` の本体の大部分。

### 数え方と限界

- 行数は `wc -l`。コメントと空行を含む。
- 層の数は `kernel/src` の `#include` だけを数えた。呼出しは数えていない。`extern` と自前の宣言は別に探した。
- `SYS_*` の呼ぶ者は、libc の包みの名前と `SYS_*` の名前の出現を、`user/`、`ports/`、`tests/` で数えた。コメントは除いた。`third_party` は見ていない。関数の指を通る呼出しは数えていない。
- 依頼文のコミット数と、今の `git log --format=%h -- <path>` の数は違う。今は `sys/syscall.c` 97、`gui/wm.c` 78、`sys/sched.c` 54、`dev/pciuser.c` 46、`mm/vmm.c` 40。
- 正しさを動かして確かめたものは一つも無い。全部が静的な読みである。

## 3. 構造の問題

### 3.0 問いごとの答え

#### 問い 1：部品の目的と持ち主

| 部品 | 目的 | カーネルにあるべきか | 問題 |
|---|---|---|---|
| `gui/wm.c`、`pointer.c` | 窓、合成、飾り、焦点、入力の割り振り、clipboard、シェルの口 | いいえ。判断。 | K1 |
| `gui/screenmode.c` | 画面のモードの一覧と望みの運び | 機構なので、あってよい。場所は `dev/fb` の側。 | K1、K4 |
| `net/net.c` | Ethernet、IP、UDP、TCP、adapter の一覧、経路 | 分からない。持ち主の決定が要る。形が間違っているのは確か。 | K2 |
| `net/rtl8125.c` | NIC のドライバー | あってよい。場所は不揃い。 | K14 |
| `fs/` | C: の store の実行側 | はい。プログラムの text と私的な領域が頼る。 | 無し |
| `dev/` の AHCI、NVMe、xHCI、HDA、IOMMU、ACPI | 装置の機構 | はい。本体は読んでいない。 | K5 |
| `dev/pciuser.c` | アプリのドライバーの口 | claim、BAR、DMA、割り込みははい。表示の役は別の持ち主へ。 | K4 |
| `dev/fb.c`、`fbcon.c` | 画面の buffer とフリップ、起動と panic の画面 | はい。端末の処理は要らない。 | K4、K11 |
| `sys/settings.c` | 設定の門 | 門はあってよい。形が逆。 | K8 |
| `sys/stream.c` | console、log、null の stream | log の読出しだけはい。ほかは要らない見込み。 | K11 |
| `sys/prof.c` | 標本の profiler | 製品にはいいえ。 | K10 |
| `vm/rdp.c`、`vm/hv/hvsock.c` | カーネルの中の RDP サーバー | いいえ。 | K6 |
| `vm/virtio_gpu.c` | virgl の 3D | カーネルの syscall の形はいいえ。道は消さない。 | K7 |
| `vm/` の残り | VM の装置 | はい。`vm/vm.h` の後ろで正しい。 | 無し |

#### 問い 2：層の向き

K14 に表がある。循環は十二組ある。

#### 問い 3：syscall の表

3.17 節に全部がある。名前は 182 個で、別名の `SYS_NET_FIRST`、`SYS_NET_LIMIT` を含む。

#### 問い 4：装置の持ち方

一つの形ではない。機械全体の一覧は無い。K5 に表がある。

#### 問い 5：重複

K15 に表がある。

#### 問い 6：試験と計測

K10 に表がある。

#### 問い 7：継ぎ足しの跡

各項の「継ぎ足しの跡」と K16 にある。

---

### K1. 窓の管理、合成、入力の割り振りがカーネルにある

**目的。** 窓を作り、重ねて焦点を決め、飾りを描き、画面ごとに一枚へ合成し、キーとマウスを窓へ配る。clipboard と hotkey も持つ。

**今の持ち主とあるべき持ち主。**

- 今は `kernel/src/gui`。
- あるべき持ち主は、窓のサービス（システムのプロセス）である。
- カーネルに残すのは三つの機構である。画面の buffer とフリップと垂直帰線（`dev/fb`）、入力の事象を一人の持ち主へ渡す口（`dev/input`）、共有の面（`mm/shared`）。
- これは AGENTS.md の「UI、システムのプロセス、カーネルは別のもの」からの判断である。

**証拠。** `gui/wm.c`（3370 行）の中身は次のとおり。行の範囲は節の印から数えた。

| 行 | 中身 | 行数 |
|---|---|---|
| 70-264 | 矩形、damage の tile、surface | 195 |
| 265-522 | 窓、画面、状態 | 258 |
| 523-639 | damage と窓の形の計算 | 117 |
| 640-712 | 窓ごとの mailbox | 73 |
| 713-792 | 焦点と重なり | 80 |
| 793-866 | direct scanout | 74 |
| 867-1033 | 窓の寿命 | 167 |
| 1034-1257 | 飾りの描画（`draw_layer`、`draw_layer_shadow`、`text_trunc`） | 224 |
| 1258-1490 | 合成 | 233 |
| 1491-1811 | 画面の絵、compositor の thread | 321 |
| 1812-2234 | 入力、Alt+Tab、hotkey、ドラッグ、大きさ変え、hover | 423 |
| 2235-2341 | desktop の thread、clipboard | 107 |
| 2342-2467 | 画面のドライバーとの間 | 126 |
| 2468-2596 | 遠隔の表示（K6） | 129 |
| 2597-2663 | 起動 | 67 |
| 2664-3162 | syscall | 499 |
| 3163-3319 | シェルの口 | 157 |

方針がカーネルに書いてある所：

- 題の文字と影とボタン：`wm.c:607`、`wm.c:1056`、`wm.c:1149-1242`。
- Alt+Tab と Alt+F4 の決め打ち：`wm.c:1975-1985`。
- 窓を動かす手の動き：`wm.c:2070-2233`。
- ポインターの絵：`gui/pointer.c` の全部（381 行）。
- 文字の clipboard：`wm.c:2299-2341`、`wm.c:3131-3162`。

規模の問題：

- 大域の `wm_mutex` が一つ（`wm.c:411`）。compositor 監査の H8 と同じ。
- 合成はカーネルのスカラーの処理。compositor 監査の H1 と同じ。
- 入力の sink は関数の指が一組（`dev/input.h:47`、`wm.c:2633`、`init.c:56`）。窓の管理か console のどちらか一人だけが受ける。

口の数：`SYS_WIN_*` 13、`SYS_SHELL_*` 6、`SYS_SCREEN_*` 4、`SYS_CLIPBOARD_*` 2 の 25 個が窓の管理の口である。

**継ぎ足しの跡。**

- 78 コミット。そのうち 2026-11-01 以降が 20。
- 中核の置き直し `df893170` が `wm.c` に触れたのは 7 行だけ。
- RDP の鉤：`7a9c1bf8`、`32578465`、`d1081e24`。
- 10 bpc の試し `99c9fef5` は、今は `wm.c:1790-1810` の `set_deep_simulation` と設定 `N_WINDOW_SMOOTH_GRADIENTS` になった。
- `6c144d29` はタスクバーと絵を出した。窓の管理は残し、`common/abi.h` の `SYS_` の行を 12 足した。
- `fee5108a` は dev から UI への呼出しを `struct screen_display` の関数の指（`dev/fb.h:113-121`）で裏返した。include の向きは直った。カーネルの中に UI があることは変わらない。

**消す範囲と書き直す範囲。**

- 移す：`wm.c` 3370、`pointer.c` 381、`wm.h` 74。計 3825 行。数え方は `wc -l`。
- 書き直す：`screenmode.c` 246、`screenmode.h` 25。`dev/fb` の側の持ち物にする。
- 新しく要る：画面の handle、入力の持ち主の口。設計していないので行数は見積らない。
- 呼ぶ側：libc の `gui.c` と `ui.c`、シェル、`display`、`machine`、ブラウザーの `WIN_DIRECT` などの道。libc の口を保てば、アプリの多くは変わらない見込み。確かめていない。

**先に変える必要があるもの。** K4（画面を handle にする）。K5 の入力の口。K14 の user copy の置き場。

**確かさ。**

- カーネルにあり、方針が混ざっていること：証明。
- サービスへ移すのが正しいこと：判断。持ち主の決定が要る。
- 移した後の遅延：未確認。`SYS_THREAD_SCHEDULE` に締切の級はある（`sys/thread.h`）が、「アプリの締切の thread は合わせて半分の CPU」という制限があり、足りるかは測っていない。

---

### K2. ソケットが object でない

**目的。** プログラムに UDP と TCP の口を渡す。

**今の持ち主とあるべき持ち主。**

- 今はソケットごとに番号があり、`tcbs[]` と `udp_socks[]` の添字で引く。
- あるべき形は、ソケットが `struct object` で、handle として持ち、待ち、渡し、閉じられること。ほかの全部の資源と同じである。
- TCP/IP をカーネルに置くかサービスに置くかは別の問いである。ここでは決めない。カーネルに置いたままでも、この問題は直せる。

**証拠。**

- 番号と pid で持ち主を確かめる：`net.c:867` の `user_udp`、`net.c:1700` の `user_tcb`。
- 片付けのための隠し handle `net_owner`：`net.c:2190-2229`。
- 片付けは全部のソケットの走査：`net.c:2204-2207`。
- その隠し handle のためだけに `handle_hold` と `handle_held` がある。`sys/object.c` の外の使い手は `net.c:2216`、`net.c:2226` だけ。
- `N_KIND_CONNECTION` の使い手はこの隠し handle だけ（`abi.h:287`、`net.c:2211`）。
- 閉じる口が別：`SYS_UDP_CLOSE`、`SYS_TCP_CLOSE` と `SYS_HANDLE_CLOSE`。
- event queue で待てない。プログラムの間で渡せない。
- 待ちは自前の `struct net_progress`（`net.c:49-58`）。
- 空きの番号を線形に探す（`net.c:1706` の `tcp_alloc_slot`）。
- 全部のプロトコルの状態を一つの mutex `net_lock` で守る（`net.c:14`、`net.c:44`）。接続の数と CPU の数が増えると、ここが詰まる。

**継ぎ足しの跡。** 取消しの口 `SYS_NET_SETUP_CANCEL_CHANNEL` がこの結果である。ソケットを閉じて接続の途中を取り消せないので、thread の欄 `net_setup_cancel`（`sys/thread.h:41`）に channel を持たせ、`net.c:113-150`、`net.c:589-612` で確かめる。

`fa54dda3`（住所と経路をサービス `network` へ引き抜いた）は、この形を変えていない。

**消す範囲と書き直す範囲。**

- 書き直す：`net.c` の口の部分 2190-2467（278 行）と、ソケットの持ち主と待ちの道。計 650 行の見積り。
- 消す：`net_owner`、`handle_hold`、`handle_held`、`N_KIND_CONNECTION`、`net_setup_cancel`、`SYS_NET_SETUP_CANCEL_CHANNEL`、`SYS_UDP_CLOSE`、`SYS_TCP_CLOSE`。
- 呼ぶ側：`user/libc/net.c`、`tls.c`、`webfetch.c`、`network.c`、`ports/go` の差分。
- プロトコルの本体（再送、窓、経路）は変えない。

**先に変える必要があるもの。** 無い。`sys/object` と `sys/events` は今のままで受けられる。

**確かさ。** 証明。ただし `net.c` の 110-713 と 990-1695 の本体は読んでいない。650 行は見積り。

---

### K3. syscall の受け口が持ち主を持たない

**目的。** アプリの呼出しを受け、持ち主へ渡す。

**今の持ち主とあるべき持ち主。**

- 今は `sys/syscall.c`（1395 行、`case SYS_` が 127 個）が、受け口と、一部の仕事の本体を両方持つ。
- あるべき形は、番号から持ち主の関数への表である。受け口は登録と呼出しだけをする。能力の確かめは持ち主で行う。

**証拠。** 分け方の形が四つ混ざる。

| 形 | 例 |
|---|---|
| `case` の中に本体 | GPU（`syscall.c:1145-1231`）、PCI、電源 |
| 同じファイルの `sys_*` | `sys_app_launch`（`syscall.c:819`）、`sys_handle_close`（`syscall.c:522`） |
| 持ち主の関数 | `audio_syscall_*`、`settings_syscall_*`、`pm_*_syscall` |
| `default:` の番号の範囲 | `syscall.c:1390-1391` の `wm_syscall`、`net_syscall` |

能力の確かめの場所も混ざる。受け口（電源、時計、画面のモード、セッション）、持ち主（音、設定、`shell_syscall`）、`net_syscall_inner` の三通りある。

持ち主の違うものがこのファイルにある：

- user copy の全部（`syscall.c:63-150`）。そのため `dev`、`fs`、`gui`、`net` が `sys/syscall.h` を include する（K14）。
- 起動の規則（`syscall.c:819-930`）。0・1・2 の補いが `syscall.c:887` にある。`sys/proc.c` が持ち主である。
- GPU の検査と写し（`syscall.c:484-521`）。
- 一般の close が文書を commit する（`syscall.c:527`）。close が store の型を知っている。持ち主は `fs/document.c` の `object_ops` である。
- 自前の宣言（`syscall.c:52-53` の `pci_list`、`net_syscall`）。

**継ぎ足しの跡。** 97 コミット。2026-10-31 以降が 32。引き抜きのたびに `case` が増えた（K16）。

**消す範囲と書き直す範囲。**

- 移す：GPU 134 行（K7 と重なる）、user copy 約 90 行、launch 約 110 行。計約 330 行。行の範囲から数えた。
- 書き直す：残りの約 1065 行を、各持ち主の関数と表へ分ける。受け口に残るのは 300〜400 行の見積り。

**先に変える必要があるもの。** 無い。機械的に進められる。K2、K4、K7 の前に行うと、移す先が楽になる。

**確かさ。** 証明。

---

### K4. アプリのドライバーの口に表示の役が載っている

**目的。** アプリのドライバーに PCI の function を渡す。加えて、そのドライバーが画面を出せるようにする。

**今の持ち主とあるべき持ち主。**

- 今は `dev/pciuser.c`（1384 行）が五つの仕事を持つ。claim、割り込み、画面を握る見張り、表示の出力、ACPI の評価。
- あるべき形は三つに分けることである。
  - `pciuser`：claim、BAR、DMA、割り込みだけ。
  - 画面の持ち主（`dev/fb`）：表示のドライバーの口。バスを問わない。
  - ACPI：名前空間の評価の口。
- `dev/fb.h:72` の `struct screen_ops` は正しい継ぎ目である。`vm/vbe.c` とアプリのドライバーが同じ口で画面を渡す。残してよい。

**証拠。**

- 五つの節：`pciuser.c:178-285`（見張り）、`pciuser.c:286` 以降と `891-1067`（表示）、`pciuser.c:562`（claim）、`pciuser.c:1069` 以降（ACPI）。
- 口の名前が役と合わない：`SYS_PCI_HOLD_SCREEN`、`GIVE_SCREEN`、`SCREEN_VBLANK`、`SCREEN_STATE`、`SCREEN_IMAGE`、`SCREEN_IMAGE_END`、`SCREEN_MODES`、`SCREEN_FLIPS`、`ACPI_EVAL`、`ACPI_NOTIFY_TAKE`。`SYS_PCI_*` の 17 個のうち 10 個が PCI の仕事でない。
- 使い手は `ports/nvidia` だけ（3.17 節）。「汎用の口」の形を試した二人目が居ない。AGENTS.md の「二つ目が来たら呼ぶ側は変わるか」は確かめられていない。
- 設定の鍵 `N_DISPLAY_HOLD_SECONDS` の持ち主が `pciuser.c:231`、`pciuser.c:244`。
- `mm/shared.c` が PCI の claim と画面を名指しする：`mm/shared.c:64-66`、`400-425`、`518-581`、`855-857`（`fb.phys` との比較）、`945`（`shared_scanout_open`）、`1027`（`shared_revoke_claim`）。`mm` が `dev` の上に立つ。
- 大域の `fb`（`dev/fb.h:26`）を `fb.c` のほかに 8 ファイルが読む。`pciuser.c` 8 行、`fbcon.c` 5 行、`hibernate.c` 5 行、`vm/vbe.c` 5 行、`boot.c` 4 行、`wm.c` 3 行、`mm/shared.c` 2 行、`syscall.c` 2 行。screen 0 だけが特別な道を持つ。
- 「A screen never goes away」（`dev/fb.h:59` あたり）。display 監査の D-11 はこの結果である。
- `wm_screen_state`（`wm.c:2367`）はドライバーから `screen_display->state` で呼ばれ、窓の管理の中で眠る。`n_screen_state` は `wscreen` の中にある。画面の状態の持ち主が窓の管理になっている。

**継ぎ足しの跡。**

- 46 コミット。始まりは `fd006ad8`。2026-11-01 以降が 13。
- 実機の症状ごとに直し、取り消している：`3838d79c`、`b9000ad5`、`87db38e4`、`95b3df57`、`f22f11e8`。
- 表示の口は後から足した：`8acc1589`、`b13cfa79`、`c0ea9bde`、`6e912418`、`fbf47b8d`、`5461059d`。

**消す範囲と書き直す範囲。**

- `pciuser.c`：表示と見張りと ACPI の約 700 行を書き直して持ち主へ移す。残りの約 680 行と `pciuser.h` は残す。行の範囲からの見積り。
- `dev/fb.c`、`fb.h`（813 行）：screen 0 の特例と鉤の約 400 行を書き直す。`fb.c` の本体は読んでいないので粗い見積り。
- `mm/shared.c`：claim と画面の名指し約 250 行を、mm の持つ抽象の口へ書き直す。
- `common/abi.h`：`SYS_PCI_SCREEN_*` などの番号と型。

**先に変える必要があるもの。** K14 の user copy の置き場。K5 と同時に設計するのがよい。

**確かさ。** 混在していること：証明。分け方：判断。D-01〜D-22 の所見と合わせて設計する必要がある。

---

### K5. 装置の型と機械全体の一覧が無い

**目的。** 機械にある装置を見つけ、ドライバーへ渡し、プログラムへ見せる。

**今の持ち主とあるべき持ち主。**

- 今は、装置の種類ごとに持ち主が別で、形も別である。
- あるべき形は、一つの装置の木と型である。バス、一致の表、抜き差しの事象、身元、見張りを一度だけ書く。種類ごとの部分は `audio_output`、`blkdev` のような今の型のままでよい。

**証拠。**

| 種類 | 一覧の持ち主 | 身元 | 抜き差し | 見張り | プログラムへの道 |
|---|---|---|---|---|---|
| PCI | `dev/pci.c`。列挙で一度だけ作る。Segment 0 だけ（`pci.h:50`）。 | bus・device・function | 無し | 無し | `SYS_PCILIST` |
| USB | `usb_device`。hub の木。 | プログラムへ渡す身元は無し | 有り | 無し | 無し |
| 入力 | `dev/input.c` の鎖 | `id` | 有り | `settings_changed(N_DOMAIN_INPUT)` | 設定の鍵 `N_INPUT_DEVICE`（`input.c:288`、`306`） |
| 音 | `dev/audio.c` の `audio_output` | 文字列の `key` | 出口の出入りは有り（RDP が足す） | `list_watched` | `SYS_AUDIO_DEVICE_*` |
| 画面 | `dev/fb.c` の `screen`。消えない。 | モニターの文字列 | 無し | `screen_watched` | `SYS_SCREEN_INFO` の op |
| ディスク | `dev/blk.c`。`gone` の旗。 | プログラムへ渡す身元は無し | USB の分 | 無し | 無し |
| 網 | `net/net.c` の adapter | MAC | 未確認 | `net_watch` | `SYS_NET_*`。名前は別の口 |
| GPU | 無し（gpu 監査の F6） | | | | `SYS_GPU_INFO` は virgl か CPU だけ |

装置が見つかる順は決め打ちである：

- `init.c` が順に呼ぶ。`init.c:25-28` は `acpi_init` などを自分で宣言する。
- ドライバーごとに自分で PCI を探す。全走査が `ahci.c:1402`、`hda.c:923`、`nvme.c:1068`、`xhci.c:2320`、`rtl8125.c:632`、`vbe.c:187`。`pci_find` が `ac97.c:281`、`e1000.c:287`、`tulip.c:118`、`virtio_gpu.c:578`。
- 一致の表は無い。

USB のクラスは決め打ちである：

- `struct usb_device` に `void *hub, *hid, *msc, *net`（`dev/usb.h:103-106`）。
- `xhci.c` がクラスの関数を直接呼ぶ：`xhci.c:1376`、`1391`、`1394`、`1451-1452`、`1675-1683`、`1730-1733`、`1919`、`1947-1949`、`1962-1963`。
- `xhci.c:2313` に「A USB disk is plugged in」の文がある。カーネルのドライバーが利用者への文を持つ。

一つの共通の型は既にある：`struct pm_device`（`dev/pm.c:51`）。電源の段の登録だけが共通である。

**種類が増えたときに変わる所。**

- `init.c`。
- 新しい一覧と新しい見張り。
- 新しい ABI の型と口。
- 設定の表（K8）。
- `machine`、設定の窓などのプログラム。

**抜き差しのときに変わる所。**

- 種類ごとに違う。ディスクと USB は誰にも知らせない。
- PCI は hot-plug を持たない。
- 画面は消えないので、窓が残る（D-11）。

**継ぎ足しの跡。** 一覧の見張りは、持ち主が要るたびに足した。入力の一覧は設定の ABI に載せた。形の揃う機会が無かった。

**消す範囲と書き直す範囲。** 書き直す約 510 行の見積り。`pci.c` 150、`input.c` 100、`xhci.c` と `usb.h` 100、各ドライバーの見つけ方 100、`init.c` 60。ドライバーの本体を読んでいないので粗い。新しい装置の木は 800〜1500 行の見積りで、集計に入れていない。

**先に変える必要があるもの。** 無い。K1 と K4 の前に行うと、入力の口と画面の口を二度作らずに済む。

**確かさ。** 別々の一覧であること、機械全体の一覧が無いこと：証明。全部のドライバーを読んではいないが、`rg` で別の一覧が見つからなかった。行数：可能性。

---

### K6. VM の境界の破れとカーネルの RDP

**目的。** Hyper-V の enhanced session で画面と入力と音を運ぶ。

**今の持ち主とあるべき持ち主。**

- 今は `vm/rdp.c`（1880 行）がカーネルの thread で RDP サーバーを動かす。
- あるべき持ち主はサービスである。カーネルは hvsock の transport を渡すだけにする。

**証拠。**

- include の規則は守られている。`vm/` の外は `vm/vm.h` だけを include する。`vm_hosted` と `vm_machine` の外の呼出しは 0。これは正しい。
- 意味の破れは三つある。
  1. `gui/wm.c` の中の遠隔の道：`remote_users` が `wm.c:355`、`450`、`813`、`1379`、`1448`、`1653`、`2485`、`2513`、`2526`、`2542` にある。`gui/screenmode.c:221` にもある。`wm_remote_*`（`wm.c:2468-2596`、`wm.h:57-63`）の呼ぶ者は `vm/rdp.c` だけ。`clipboard_seq`、`clipboard_open`（`wm.c:499`、`501`）もこのため。
  2. ABI の VM の語：`host_asks`、`host_action`（`abi.h:742`、`745`）。`pm_host_asks` の呼ぶ者は `vm/hv/hv_util.c:85` だけ。`user/apps/machine.c:255` が読む。
  3. `dev/gpu.h`（K7）。
- `vm/rdp.c` は `gui/wm.h`、`dev/input.h`、`dev/audio.h` を include する。RDP が音の出口を足す。

**継ぎ足しの跡。** `7a9c1bf8`、`32578465`、`d1081e24`（2026-10-06）。窓の管理の初期から鉤が入った。

**消す範囲と書き直す範囲。**

- 移す：`rdp.c` 1880。
- 書き直す：`hvsock.c` 249 を object の transport に。`hv_util.c` の約 20 行。
- 消す：`wm.c:2468-2596` の 129 行と `remote_users`（K1 の中で数える）、`host_asks`、`host_action`。

**先に変える必要があるもの。** K1。画面を読む口は `N_CAP_SCREEN_READ` と `SYS_SCREEN_GRAB` がある。入力を注入する口と damage を聞く口は無い。

**確かさ。** 破れ：証明。`rdp.c` の本体は読んでいない。サービスへ移せること：可能性。

---

### K7. `SYS_GPU_*` の道は製品で動かない

gpu 監査の F16、F17、F6 と事実は同じである。ここでは構造だけを書く。

**目的。** QEMU の virtio-gpu の 3D を、プログラムから三角形と blit で使う。

**今の持ち主とあるべき持ち主。**

- 今は `vm/virtio_gpu.c`（738 行）と `SYS_GPU_*` の 5 個。宣言は `dev/gpu.h`。
- あるべき形は、`struct n_gpu_driver`（`gpu_abi.h`）の継ぎ目の後ろの実装である。

**証拠。**

- 動かない：`virtio_gpu.c:577` は VM でないと戻る。`virtio_gpu.c:580` は試験の旗が無いと戻る。製品のカーネルは旗を読まない。
- 境界の迂回：`init.c:42` の `gpu_init` と `dev/gpu.h` が `vm/vm.h` の外にある。
- 受け口に本体：`syscall.c:484-521`、`1145-1231`。
- 「Source compatibility only」（`gpu_abi.h:6`）。ABI は古い形を持たないはずである（K12）。
- `gpu_abi.h:1` は「native GPU interface」と名乗る。使い手は `js_canvas.c` だけで、本当の GPU の道は同じファイルの下半分である。

**gpu 監査との違い。** F16 と表は「消す」と書く。メモリー `gpu-support` は virgl の道を消さないと決めている。だから「口の形を消し、virgl は継ぎ目の後ろへ移す」と読む。カーネルのドライバーのまま `vm/vm.h` の後ろに置くか、アプリのドライバーにするかは持ち主の決定である。

**消す範囲と書き直す範囲。** 移す：`virtio_gpu.c` 738。消す：`syscall.c` の 134 行（K3 と重なる）、`gpu_abi.h` の上半分 約 60、`dev/gpu.h` 14。呼ぶ側：`js_canvas.c`、`gpu_demo.c`、`gputest.c`。

**先に変える必要があるもの。** F6 の GPU の台帳。virgl の Vulkan のドライバーの像があるかは確かめていない。

**確かさ。** 証明。

---

### K8. 設定の門が持ち主を名指しする

`docs/records/audit-settings-20261103.md` は設定の窓から見た報告である。ここはカーネルの形だけを書く。

**目的。** サービス `settings` から、カーネルの中の値の持ち主へ、型のある値を渡す。

**今の持ち主とあるべき持ち主。**

- 今は `sys/settings.c:23-40` の表が全部の持ち主の関数を名指しし、`sys/settings.h` が全部を宣言する。
- あるべき形は、持ち主が自分の鍵を登録することである。門は誰が居るかを知らない。

**証拠。**

- `sys/settings.h:36-44`：`dev/`、`arch/`、`gui/`、`net/` の関数の宣言が一つの見出しに並ぶ。これが `arch/idle.c:26` の逆向きの include の理由である。
- 定義の無い宣言：`sys/settings.h:41-42` の `dns_setting_put`、`dns_setting_get`。`fa54dda3` の残りである。
- 表に無い型：`N_SETTING_ENDPOINT`。カーネルの鍵に使い手が無い。
- 製品の呼ぶ者が 0：`SYS_SETTINGS_DESCRIBE`。試験も 0。
- 空の領域：`N_DOMAIN_TIME`（`settings_abi.h` の `n_time_key`）。
- 適用の道が二つある。門（`N_DISPLAY_HOLD_SECONDS`、TCP の容量など）と、専用の口（`SYS_SCREEN_MODE_APPLY`、`SYS_NET_ADDRESS_SET`、`SYS_CLOCK_WALL_*`）。専用の口の値は門に載らない。何がどちらかの規則は書かれていない。
- 門の鍵の少なさ：18 行のうち 5 行は起動の記録で、試験の旗の名残である（`settings.c:125-146`、`bootrecord.c:58-66`、settings 監査の「試験の旗がそのまま設定になっている」）。

**継ぎ足しの跡。** `71fd2593`（+916/-768）、`fa54dda3`。保存を外へ出したが、門の形は持ち主の一覧のまま残った。

**消す範囲と書き直す範囲。** `settings.c` 147 と `settings.h` 約 60 を書き直す。消す：`dns_setting_*`、`SYS_SETTINGS_DESCRIBE`（使うなら残す）、`settings_test_flags`。

**先に変える必要があるもの。** 無い。

**確かさ。** 証明。

---

### K9. 電源の段取りでカーネルが郵便箱になっている

**目的。** 電源を切る前に窓を閉じ、残った窓の題を利用者へ見せ、取りやめる。

**今の持ち主とあるべき持ち主。**

- 今はサービス `machine` が `SYS_POWER_SAY` でカーネルへ状態を言い、カーネルが見張りでシェルへ渡す。
- あるべき形は `machine` とシェルの間の channel である。カーネルは段の実行（`dev/pm.c`）だけを持つ。

**証拠。**

- `dev/pm.c:158` の `pm_say_syscall` は値を預かって知らせるだけ。`user/apps/machine.c:100` が呼ぶ。
- `N_POWER_CLOSING`、`N_POWER_CANCELLED`（`abi.h:691-696`）の detail は窓の題である。窓の題がカーネルの電源の ABI を通る。
- `SYS_POWER_WINDOWS`、`SYS_POWER_WINDOWS_WATCH` は `SYS_SHELL_WINDOWS`、`SYS_SHELL_WATCH` と同じものを読む。見張りも同じ `shell_watched`（`wm.c:2642`、`3315`）。K1 の結果である。
- `dev/power.c` に新旧の道が両方ある。uACPI が先で、手書きの `_S5_` の走査（`power.c:49`）と 8042（`power.c:102`）と triple fault（`power.c:104`）が残る。`_S5_` の道は「AML を読めないとき」で、そこへ入るのは `N_BOOT_ACPI_AML` か失敗のときである。
- `acpi_init` が `dev/power.c:37` にある。名前と場所が合わない。本体の ACPI は `dev/acpi.c` にある。

**継ぎ足しの跡。** `d2ca6cee`（+480/-360）。本文に「媒体と QEMU の確かめは…行っていない」とある。その後に確かめたかは未確認。

**消す範囲と書き直す範囲。** `pm.c` 約 60、`power.c` 約 50。`SYS_POWER_SAY`、`SYS_POWER_WINDOWS`、`SYS_POWER_WINDOWS_WATCH` を消す。

**先に変える必要があるもの。** `SAY` は何にも依らない。`POWER_WINDOWS` は K1 の後。

**確かさ。** 証明。`power.c` の古い道を消してよいかは、持ち主の判断（公式の文書どおりの最後の手段なら残す）。

---

### K10. 試験と計測が製品のカーネルに残っている

**目的。** 計測と故障の注入。

**今の持ち主とあるべき持ち主。** 仕組みは正しい。`NOCTURNE_TEST_FLAGS` で試験の旗を読むカーネルを分けている（`kernel.h:71-78`、`Makefile:58-63`）。問題は、旗の後ろのコードが製品に組まれ、口が ABI に残っていることである。

**証拠。**

| 所 | 中身 | 製品での働き |
|---|---|---|
| `sys/prof.c`（287 行） | 標本の profiler | `prof.c:274-275` で戻る。何もしない。 |
| `arch/smp.c:22`、`233` | `prof_cpu_start` | 上と同じ |
| `arch/idle.c:184-207` | `cpu_perf_sample` など | 読み手は `prof` だけ（`sys/prof.c:135`、`170`） |
| `sched_get_stats`（`sys/sched.c:1182`）、`vmm_tlb_stats`（`mm/vmm.c:333`） | 統計 | 読み手は `prof` だけ（`sys/prof.c:233`、`237`） |
| `gui/wm.c:392-394`、`1699-1713` | `stats_print` | `prof_window_ns` が 0 なので何もしない |
| `net/net.c:971-981`、`1063`、`1408-1427`、`1677-1693`、`2179` | TCP の故障の注入 | 動く。`SYS_NET_TEST` で入る。 |
| `net/net.c:2391-2405` | `SYS_NET_TEST` | 動く。製品の呼ぶ者は `ifconfig.c` だけ。admin で入れる。 |
| `common/abi.h:906-915` | `n_netstats`、`n_netfault`、`n_tcpstats` | ABI の型 |
| `sys/settings.c:125-146` | 旗から設定へ | 試験のカーネルだけ |
| `fs/bootrecord.c:58-66` | 旗から起動の値へ | 試験のカーネルだけ |
| `vm/e1000.c:187`、`vm/virtio_gpu.c:240`、`265`、`579`、`580`、`672`、`mm/vmm.c:649`、`init.c:57` | `test_flag` | 製品では偽 |

`SYS_NET_TEST` は接続を壊す口である（`net.c:2392` のコメントも「接続を壊す試験の入口」と書く）。製品のカーネルで admin が使える。

`.claude/rules/kernel.md:20` の「cmdline に `prof`」は製品では効かない。文書が古い。

**継ぎ足しの跡。** 試験のカーネルを分けた後も、コードの本体は組み分けられていない。

**消す範囲と書き直す範囲。** `prof.c` 287、`idle.c` 24、`wm.c` の統計（K1 の中）、`net.c` の故障の注入 65 行と宣言。計 約 410 行の見積り。`SYS_NET_TEST`、`n_netfault`、`n_tcpstats` を消す。残すなら `#ifdef NOCTURNE_TEST_FLAGS` の中へ入れる。

**先に変える必要があるもの。** 無い。`tests/` の TCP の試験（`tcptest`）の置き場を決める。

**確かさ。** 証明。

---

### K11. 端末と byte stream の形がカーネルと ABI にある

判断の物差しは「UNIX に似ているか」ではない。「カーネルが端末の意味を持つ理由があるか」である。CLI の道具を持つことは問題でない。

**目的。** `sh` と `term` の入出力、エラーの文字、Ctrl+C。

**今の持ち主とあるべき持ち主。**

- 今はカーネルが 0・1・2 の規則、console、null、終了コードの慣習を持つ。
- あるべき持ち主は `term.c`、`sh.c` と libc である。カーネルは channel を渡すだけにする。log の読出しだけは自分の口を持ってよい。

**証拠。**

- 一つの番号に二つの意味：`SYS_CHANNEL_SEND`、`SYS_CHANNEL_RECEIVE` が `byte_stream`（`syscall.c:738-739`）で別の道へ分かれる（`syscall.c:758`、`780`）。
- `object_ops` の `read` と `write` の使い手は `sys/stream.c` の三つだけ（`stream.c:46`、`131`、`213`）。すべてのオブジェクトの表に、三つのための欄がある。AGENTS.md の「全部はファイルではない」と逆向きである。
- 0・1・2 が無ければ null を入れる：`syscall.c:887`。`authority_abi.h` の `N_INITIAL_ROOT_HANDLE`、`services_abi.h` の `N_SERVICE_HANDLE_CONTROL`、`N_DOCUMENT_WINDOW_HANDLE` は 3 番から始まる。
- `n_launch` の `terminals`、`interrupts`（Ctrl+C の channel）、`environment`（`abi.h:354-355`、`365-367`）。カーネルは stack へ写すだけ（`sys/proc.c:341-387`）。意味を読まない。
- UNIX の終了コード：`process_exit(128 + 13)`（`syscall.c:1043`）、`kill_code` の 130（`sys/thread.h:29`）。
- CPU の例外がアプリの handle 2 へ色つきの文字を書く：`arch/idt.c:178` の `stream_error_printf`。`arch` が handle と端末の文字を知っている。
- `dev/fbcon.c` は ANSI の処理を持つ（頭のコメント。本体は読んでいない）。
- `console_key`（`stream.c:158`）は窓の管理が無いときの入力の sink（`init.c:56`）。
- `N_KIND_DEVICE`（`abi.h:286`）を null、log、音の open の三つが共有する。

**継ぎ足しの跡。** 未確認。コミットを個別に追っていない。

**消す範囲と書き直す範囲。** `stream.c` 233 を log の読出しだけに書き直す。`object.h` と `object.c` の `read`、`write`、約 40。`proc.c` の起動の記録 約 60（`terminals` などを一般の起動の値に）。`fbcon.c` の端末の部分 約 90。libc の stdio、`sh`、`term` が変わる。

**先に変える必要があるもの。** 無い。`user/` の呼ぶ側が広い。

**確かさ。** 形：証明。`terminals` などを持ち主の決定なしに消してよいか：判断（メモリー `no-unix-compat` は「似ているだけで消さない」と書く。理由はカーネルが意味を持たないことに置く）。

---

### K12. ABI に古い形と死んだ欄が残っている

**目的。** プログラムとカーネルの間の唯一の約束。

**今の持ち主とあるべき持ち主。** 持ち主は `common/abi.h` で正しい。主張と中身が合わない。

**証拠。**

`abi.h:50` は「The ABI keeps no older form of a call」と書く。次が反する：

| 古い形 | 新しい形 | 証拠 |
|---|---|---|
| `SYS_ENTRY_INFO` | `SYS_ENTRY_INFO_SIZED` | `user/libc/sys.c:316`「stay for programs built before its id」。呼ぶ者は試験だけ。 |
| `SYS_FOLDER_LIST` | `SYS_FOLDER_LIST_SIZED` | 同じ |
| `SYS_TCP_SEND` | `SYS_TCP_SEND_UNTIL` | 製品の呼ぶ者は 0 |
| `SYS_SCREEN_INFO` の大きさ 0 | `struct screen_format` | `display_abi.h:13-15`「so existing programs are unaffected」 |
| `n_gpu_batch` | `SYS_GPU_RENDER_LIST` | `gpu_abi.h:6`「Source compatibility only」 |

死んだ欄と番号：

- `struct n_cpuinfo` の `worker_cpus`、`parallel_jobs`、`worker_chunks` は常に 0（`abi.h:464-468`）。`N_CPU_AP_WORKERS` は決して立たない（`abi.h:470`）。
- `N_CPU_SSE2`、`N_CPU_AVX`（`abi.h:471-472`）は、下限 Zen 4 では常に真である。
- 番号の穴（`abi.h:52`）。世代は一つなので、穴に意味は無い。`SYS_RANDOM = SYS_AUDIO_DEVICE_STATUS + 2`（`abi.h:127`）は穴を踏まないための足し算である。
- 値 12 の `N_KIND_*` は欠番。`N_KIND_FOLDER` は `n_entry.kind` だけで使う（`abi.h:280`、`327`）。object の種類と項目の種類が同じ番号の空間にある。
- `SYS_SYSINFO` の `fb_w`、`fb_h` は `SYS_SCREEN_INFO` と重なる。
- 孤児のコメント：`syscall.c:44`「sys/object.c: whether the handle is a capability」。

一つの番号に幾つも入る口：

- `SYS_SCREEN_INFO`（`wm.c:3098-3129`、`wm.c:3322-3330`）は、第三引数の op（`SCREEN_SETTINGS_GET`、`SCREEN_MODES_GET`、`SCREEN_WATCH`）と大きさで分かれる。第 8 ビットから上に画面の番号が入る。

**継ぎ足しの跡。** `common/abi.h` は 92 コミット。2026-10-31 以降が 37。番号は穴のまま足された。

**消す範囲と書き直す範囲。** `abi.h` の約 100 行を消し、約 300 行を書き直す。`display_abi.h` は K1 で動く。

**先に変える必要があるもの。** 無い。古い形の呼ぶ者は試験と `ports` だけ。番号の振り直しは全体の組み直しになる。

**確かさ。** 証明。`docs/nocturne-native-abi.md` との照合はしていない（節の見出しだけ読んだ）。

---

### K13. 一つのプログラムのための口

カーネルの中のブラウザーだけのものを含む。

| 口 | 呼ぶ者 | 判断 |
|---|---|---|
| `SYS_NET_SETUP_CANCEL_CHANNEL` | `webfetch.c`、`network.c` | **特例。** thread の欄（`sys/thread.h:41`）と `net.c` の分岐を持つ。K2 が直れば要らない。 |
| `SYS_WIN_PRESENTATION`、`SYS_WIN_STATE`、`SYS_WIN_DIRECT` | 製品は `browser.c` だけ（`DIRECT` は `web_presentation.c`、`js_webgl.h` も） | 口は一般の形である。使い手が一人なので、形が試されていない。K1 の設計で扱う。 |
| `SYS_WAIT_ADDRESS64` | `js_atomics.c`、`ports/llvm` | **特例の形。** 目的が wasm（`sys/address_wait.h:16`）。`SYS_WAIT_ADDRESS` の幅の引数にできる。 |
| `SYS_APP_PARENT` | `webfetch.c:2410` | 一般の口（`syscall.c:1346`、親の番号を返す）。使い手が一人なだけ。問題でない。 |
| `SYS_DOCUMENT_INFO` | `webcache.c`、`ports` | 一般の口。使い手が少ないだけ。問題でない。 |
| `SYS_ENTRY_WATCH` | `webnet.c`、`webstorage_shared.h` | 一般の口。問題でない。 |
| `SYS_TCP_CANCEL` | `tls.c`、`ports/go` | K2 の形なら handle の操作になる。 |

**目的、持ち主。** 上の表のとおり。

**継ぎ足しの跡。** `net.c` のコメントに「cancelled HTTP/2」の話がある。症状ごとに足した口である。hash は追っていない。

**消す範囲と書き直す範囲。** K2 の中で数える。thread の欄を一つ消す。

**先に変える必要があるもの。** K2。K1。

**確かさ。** `SYS_NET_SETUP_CANCEL_CHANNEL`：証明。`SYS_WAIT_ADDRESS64` を一つにできること：可能性。

---

### K14. 層の逆向きと循環

**目的。** 下の層が上の層を知らないこと。

**今の持ち主とあるべき持ち主。** 表の順を下から `lib`、`arch`、`mm`、`sys`、`fs`、`dev`、`net`、`gui`、`vm` とする。AGENTS.md に層の順は書かれていない。`vm/vm.h` の規則だけがある。

**証拠。** 行が列を include する。数はファイルの組の数。

```
        lib arch  mm sys  fs dev net gui  vm
lib       0    1   2   0   0   0   0   0   0
arch      0    0  13  14   1   7   0   0   0
mm        2   19   0  13   1   3   0   0   0
sys       5   59  38   0   4  16   1   2   0
fs        3    3   6  12   0   3   0   0   0
dev       1   69  47  77   5   0   1   0   5
net       0    6   4  13   0   4   0   0   1
gui       0    1   5  13   0   4   0   0   0
vm        0   25  22  28   0  22   4   2   0
```

循環は十二組：arch⇄mm、arch⇄sys、arch⇄dev、mm⇄sys、mm⇄dev、mm⇄fs、sys⇄dev、sys⇄fs、fs⇄dev、sys⇄net、sys⇄gui、dev⇄net。

全部を直す必要は無い。次のように分ける。

**直すもの（持ち主が間違っている）：**

| 向き | 証拠 | 直し方 |
|---|---|---|
| arch→sys/settings | `arch/idle.c:26`、`157-166` | K8 |
| arch→sys/stream | `arch/idt.c:9`、`178` | K11 |
| arch→sys/prof | `arch/smp.c:22`、`233` | K10 |
| arch→fs/bootrecord | `arch/smp.c:14`、`304` | 起動の値を読む口を低い層へ |
| dev→fs/bootrecord | `dev/acpi.c:5`、`dev/iommu.c:21`、`dev/hibernate.c:23` | 同じ。FAT32 を書く本体（`fs/bootrecord.c`）と、読むだけの `boot_setting` を分ける |
| mm→dev | `mm/shared.c:64-66` | K4 |
| fs→dev/usb | `fs/store.c:10`、`310`（`usb_enumerating`） | 起動の disk を待つ条件を K5 の型で |
| sys→gui、sys→net | `syscall.c:34-37` | K3 |
| vm→gui | `vm/rdp.c:25` | K6 |
| 全部→`sys/syscall.h` | `dev/audio.c`、`pciuser.c`、`pm.c`、`gui/*`、`net/net.c`、`fs/document.h` など | user copy を別の見出しへ（K3） |

**直さなくてよいもの（下の土台の位置の問題）：**

- arch→sys/thread、wait、clock、sched、alarm：`arch/irq.c` の irq thread と `arch/smp.c` の CPU の起動。`arch` が純粋な葉でないだけで、依存は実際の順に合う。
- mm⇄sys：`mm` は mutex と wait queue を使い、共有の面は object である。`wait` を別の土台と見れば循環でない。
- arch→dev/acpi：`arch/topology.c`、`apic.c`、`ioapic.c`、`irq.c` が表の形を読む。表の型の見出しを低い層に置くだけで済む。優先は低い。
- dev⇄net：ドライバーが adapter を登録する向きは正しい。
- `mm/program.c:21`→`fs/store.h`：プログラムの text は文書から読む。pager の口へ抽象できるが、害は小さい。

**隠れた依存（include に出ない）：**

- `init.c:25-28`、`syscall.c:52-53` の自前の宣言。
- `dev/power.c:37` の `acpi_init`。

**NIC のドライバーの場所が三つある：** `net/rtl8125.c`、`dev/usbnet.c`、`vm/e1000.c`、`vm/tulip.c`、`vm/hv/netvsc.c`。VM のものは規則どおりである。実機の二つが別の層にある。

**継ぎ足しの跡。** `fee5108a` は dev→gui を 0 にした。一つの組を裏返したが、層の順を決めた文書は無い。

**消す範囲と書き直す範囲。** `arch` 約 100、`fs` 約 60、`init.c` 約 60 を書き直す。ほかは各項の中で数える。

**先に変える必要があるもの。** 無い。

**確かさ。** 数：証明（include だけ）。直す・直さないの分け方：判断。

---

### K15. 同じ仕組みの重複

**目的と持ち主。** 行ごとに表へ書く。消す範囲は、判断の欄に書いた項の中で数える。先に変える必要があるものも同じである。

| 仕組み | 別名 | 証拠 | 判断 |
|---|---|---|---|
| 世代の語と wait queue | `net_progress`、`screen_events`・`screen_poke`、`usb_kicks`・`usb_wait_kick`、`desk_wakes`、`state.serial`・`state_wq`、`log_told`・`log_wait` | `net.c:49-58`、`dev/usb.h:168-169`、`wm.c:420-427` | 土台の `wq_wait_unless` は一つで正しい。型が無いだけ。小さい型を一つ足す。読みにくいだけの問題。 |
| 一覧の書き出し | `(out,max)→数`、`(out,bytes)→全バイトで足りなければ何も書かない`、「入るだけ写す」 | net・wm の `list_out`（`wm.c:3209`）、audio、settings | 構造の問題。ABI の約束を一つに。 |
| キーの事象の輪 | `wm.c:1835-1902` の入力の輪、`stream.c:151-183` の console の輪 | 左のとおり | K1 と K11 で消える。 |
| 事象の配達 | 窓の mailbox（`wm.c:640-712`、`SYS_WIN_RECEIVE`）、`sys/events`、channel の message | 左のとおり | 構造の問題。窓がサービスになれば channel になる。 |
| 番号の表 | `lib/idtable`、`tcbs[]`、`udp_socks[]` | `net.c:734-739`、`net.c:1700-1708` | K2 で消える。 |
| 資源の片付け | handle の表、`net_owner` の pid の走査 | `net.c:2204-2207` | K2 で消える。 |
| 閉じる口 | `SYS_HANDLE_CLOSE`、`SYS_UDP_CLOSE`、`SYS_TCP_CLOSE`、close の中の commit | `syscall.c:527` | K2、K3。 |
| 文書の書込み | `SYS_DOCUMENT_SAVE`、`SYS_DOCUMENT_APPEND` と open・write・commit | 3.17 節 | 製品の呼ぶ者は `SAVE` が 13 ファイル、`APPEND` が 4 ファイル。一度で済む保証があるなら別の意味を持つ。確かめていない。 |
| 設定の適用 | 門と専用の口 | K8 | 構造の問題。 |
| 見張り | `struct watched`（`sys/watch.c`） | | **重複でない。** 一つの型で正しい。 |
| 窓の一覧 | `SYS_POWER_WINDOWS`、`SYS_SHELL_WINDOWS` | K9 | K1 で消える。 |
| ログの読み | `SYS_SYSTEM_LOG`、`SYS_SYSTEM_LOG_OPEN` | 3.17 節 | `dmesg.c` だけが古い方を使う。一つにできる。 |

**確かさ。** 別名があること：証明。輪は全部を数えていない（ドライバーの輪は読んでいない）。

---

### K16. 方針の引き抜きは機構を残して口を足した

これは一つの部品でなく、進め方の問題である。

**目的と持ち主。** 方針をカーネルの外の持ち主へ出すこと。あるべき形は `dev/audio.c` とサービス `audio` の分け方である。機構だけを残し、ABI は減るか変わらないこと。

**消す範囲。** この項に独自の行は無い。K1、K2、K6、K9 の中で数える。

**先に変える必要があるもの。** 無い。次の引き抜きの前に、窓と網の機構の持ち主を決める（6 節）。

**証拠（時系列）。**

| コミット | 日付 | 中身 |
|---|---|---|
| `5e9c0146` | 10-31 | fs と dev を除いて kernel を消す。-32343 行。 |
| `372752e6` | 10-31 | GitHub の zip から戻す。+37657 行。 |
| `63b181ff` | 11-01 | 中核を消す。25 ファイル、-6282 行。`sys/sched.c` の -1184 を含む。 |
| `df893170` | 11-01 | 中核を置き直す。+7552/-11。39 ファイル。27 が `sys`、`mm`、`arch`。`wm.c` は 7 行、`xhci.c` は 18 行、`rdp.c` は 4 行。 |
| `6c144d29` | 11-01 | タスクバーと絵を引き抜く。+716/-1293。abi.h の `SYS_` の行が +12/-3。 |
| `fee5108a` | 11-01 | dev→UI を裏返す。+160/-238。 |
| `71fd2593` | 11-01 | 設定の保存を出す。+916/-768。 |
| `d671419e` | 11-01 | 身元と能力。+525/-227。abi.h の `SYS_` の行が +14/-3。 |
| `78d65017` | 11-02 | 音。+1040/-1861。abi.h の `SYS_` の行が +7/-5。 |
| `fa54dda3` | 11-02 | 網。+940/-1130。abi.h の `SYS_` の行が +8/-2。本文に「ここでは user/ は組めない」。 |
| `d2ca6cee` | 11-02 | 画面と電源。+480/-360。abi.h の `SYS_` の行が +12/-1。本文に確かめを行っていないとある。 |
| `e73494ae` | 11-02 | 台帳。本文に「組みと動作は確かめていない」。 |

読み取れること：

1. **構造の質の境目は、置き直しの境目と合う。** `63b181ff` と `df893170` が作り直した `sys`、`mm`、`arch` の部分は読んだ範囲で一つの設計である。作り直さなかった `gui`、`net`、`dev/pciuser.c` に問題が集まる。これは推定である。`arch` と `mm` の本体を読んでいないので、全部を確かめたわけではない。
2. **引き抜きは「文を出す」で止まった。** map.md は `gui/` を「文を言わず、タスクバーも壁紙も持たない」と書く。出たのは文と絵である。窓、合成、TCP/IP、RDP は残った。音だけは `dev/audio.c` に機構、サービスに方針の形で分かれた。音の引き抜きは行が減った（+1040/-1861）。
3. **引き抜きのたびに ABI が増えた。** 上の五つのコミットで、`SYS_` を含む行が足して 53、引いて 14。行の数で、番号の数ではない。
4. **同じ問題を別の層で何度も扱った。** `SYS_POWER_SAY` は、窓の管理がカーネルにあるのにシェルがプロセスになった結果の橋渡しである。`SYS_SHELL_*` も同じ。
5. **`sys/sched.c` の 54 コミットは積もりではない。** `63b181ff` で消え、`df893170` で新しく作られた（+1281）。その後のコミットは `8837d5bd` だけである。54 のうち 52 は前の版のものである。読んだ頭の範囲に古い道は見えない。本体は読んでいない。
6. **`mm/vmm.c` は `661db797` で消し、`d927e533` で戻した。** 履歴の振れであり、今の構造の問題かは本体を読んでいないので分からない。

**確かさ。** 履歴の事実：証明。1 の因果：推定。

---

### 3.17 syscall の表（問い 3）

数え方は 2 節にある。「1 人」は、呼ぶファイルが一つの意味である。そのファイルが libc なら、実際の呼ぶ者は全部のプログラムである。

#### 製品の呼ぶ者が 0

| 口 | 使い手 | 判断 |
|---|---|---|
| `SYS_NET_ROUTES`、`SYS_SETTINGS_DESCRIBE`、`SYS_CAP_KIND` | 誰も呼ばない。試験も 0。 | 消す |
| `SYS_GPU_RENDER_BATCH`、`SYS_GPU_RENDER_LIST` | `gputest` だけ | K7 |
| `SYS_TCP_SEND` | 試験だけ | `TCP_SEND_UNTIL` と同じ仕事。消す |
| `SYS_WIN_MOVE` | 試験だけ | 消す |
| `SYS_ENTRY_INFO`、`SYS_FOLDER_LIST` | `syscallfuzz`、`nativeabitest` だけ | K12。消す |
| `SYS_TCP_LISTEN`、`ACCEPT`、`ADDRESS`、`FINISH` | `ports/go` の差分だけ | 口は要る。使い手が一つ。K2 で扱う |
| `SYS_MEMORY_PROTECT` | `ports`（llvm、wabt）と試験 | 正しい |
| `SYS_THREAD_STACK_BOUNDS` | `ports`（nvidia、wabt）と試験 | 正しい |
| `SYS_PCI_CLAIM`、`INFO`、`CONFIG_READ`、`CONFIG_WRITE`、`BAR`、`INTERRUPTS`、`INTERRUPT_TAKE`、`SYS_DMA_CREATE`、`SYS_DMA_ADDRESSES` | `ports/nvidia` と試験 | 正しい。使い手が一つ（K4） |
| `SYS_SHARED_VIEW`、`SHARED_REVOKE`、`SHARED_COMPOSE` | `ports/nvidia` と試験 | 同じ |
| `SYS_PCI_HOLD_SCREEN`、`GIVE_SCREEN`、`SCREEN_VBLANK`、`SCREEN_STATE`、`SCREEN_IMAGE`、`SCREEN_IMAGE_END`、`SCREEN_MODES`、`SCREEN_FLIPS` | `ports/nvidia` と試験 | K4 |
| `SYS_PCI_ACPI_EVAL`、`ACPI_NOTIFY_TAKE` | `ports/nvidia` と試験 | K4 |
| `SYS_PAGER_*` | `ports/nvidia/uvm_mm.c` と試験 | 正しい。使い手が一つ |
| `SYS_CHANNEL_RECEIVE_FROM` | `ports/nvidia/nvrm_serve.c` と試験 | 使い手が一つ。本体は読んでいない |

`SYS_NET_FIRST`、`SYS_NET_LIMIT` は別名で、呼ぶものではない。

#### 呼ぶファイルが 1 つ

**設計どおり（能力で絞ったサービスの口）。正しい。**

- `SYS_SHELL_*` 6 個：`shell.c`（K1 で動く）
- `SYS_AUDIO_DEVICE_*` 5 個：`audio.c`
- `SYS_SCREEN_MODE_APPLY`、`SYS_SCREEN_ARRANGE`：`display.c`
- `SYS_CLOCK_WALL_STEP`、`SYS_CLOCK_WALL_SLEW`、`SYS_RTC_WRITE`：`timesync.c`
- `SYS_NET_ADDRESS_SET`、`SYS_NET_ROUTES_SET`、`SYS_UDP_SOCKET_ON`：`network.c`
- `SYS_POWER_EVENTS`、`SYS_POWER_EVENTS_WATCH`：`machine.c`
- `SYS_SESSION_STOP`：`session.c`
- `SYS_ENTRY_SET_OWNER`：`accounts.c`
- `SYS_CAP_DERIVE`、`SYS_SERVICE_REGISTER`：`services.c`
- `SYS_SETTINGS_APPLY`：`settingsd.c`

**libc の一つのファイルが全部のプログラムの入口。問題でない。**

- `SYS_APP_CPU_TIME`（`time.c`）、`SYS_THREAD_EXIT`（`threads.c`）、`SYS_MEMORY_RESERVE`（`malloc.c`）、`SYS_IMAGE_MAP`（`vulkan.c`）、`SYS_WIN_SET_TITLE`・`WIN_FRAME`・`WIN_POINTER`・`WIN_ACQUIRE`（`ui.c`）。

**特例か、橋渡しか、道具のもの。**

- K13 の表の口。
- `SYS_POWER_SAY`、`SYS_POWER_WINDOWS`、`SYS_POWER_WINDOWS_WATCH`：`machine.c`（K9）。
- `SYS_SYSTEM_LOG`：`dmesg.c`（K15）。
- `SYS_NET_TEST`、`SYS_NET_STATS`：`ifconfig.c`（K10）。
- `SYS_NET_PING`：`ping.c`。ICMP の特別な口。一般の口の無さの結果の見込み。
- `SYS_SCREEN_GRAB`：`machine.c` と試験。
- `SYS_GPU_RENDER`、`SYS_GPU_BLIT`：`js_canvas.c`（K7）。
- `SYS_WIN_RESIZE`：`shell.c`。ほかのアプリがこの口を呼ばない理由は読んでいない。
- `SYS_IDENTITY_SELF`：`sysmon.c`。身元の読み手が道具一つ。サービスは `n_channel_peer` で読む。問題でない。

#### 同じことをする組

| 組 | 残す方 |
|---|---|
| `SYS_ENTRY_INFO` と `SYS_ENTRY_INFO_SIZED` | 後者 |
| `SYS_FOLDER_LIST` と `SYS_FOLDER_LIST_SIZED` | 後者 |
| `SYS_TCP_SEND` と `SYS_TCP_SEND_UNTIL` | 後者 |
| `SYS_SYSTEM_LOG` と `SYS_SYSTEM_LOG_OPEN` | 後者 |
| `SYS_SHELL_WATCH` と `SYS_POWER_WINDOWS_WATCH` | K1 で両方動く |
| `SYS_POWER_WINDOWS` と `SYS_SHELL_WINDOWS`、`SYS_SHELL_WINDOW` の `N_SHELL_CLOSE` | K1 で両方動く |
| `SYS_UDP_CLOSE`、`SYS_TCP_CLOSE` と `SYS_HANDLE_CLOSE` | 最後 |
| `SYS_DOCUMENT_SAVE`、`APPEND` と open・write・commit | 確かめていない |
| `SYS_SYSINFO` の `fb_w`、`fb_h` と `SYS_SCREEN_INFO` | 後者 |

#### 全部の口

| 口 | 製品（`user/`） | 移植（`ports/`） | 試験 |
|---|---|---|---|
| `SYS_EXIT` | 0（libc の中だけ） | 2（`nocturne.patch`、`nocturne.patch`） | 1 |
| `SYS_SLEEP` | 17（`agent.c`、`audio.c`、`cppdemo.cpp`、`js.c`…） | 7（`nocturne.patch`、`nocturne.patch`…） | 53 |
| `SYS_APP_CPU_TIME` | 1（`time.c`） | 0 | 0 |
| `SYS_PROCLIST` | 0（libc の中だけ） | 0 | 12 |
| `SYS_SYSINFO` | 9（`agent.c`、`free.c`、`js.c`、`neofetch.c`…） | 0 | 7 |
| `SYS_YIELD` | 15（`cxxabi.cpp`、`threads.c`、`avmedia.c`、`js_attributes.js`…） | 2（`nocturne.patch`、`nocturne.patch`） | 15 |
| `SYS_POWER` | 3（`machine.c`、`poweroff.c`、`reboot.c`） | 0 | 2 |
| `SYS_IMAGE_MAP` | 1（`vulkan.c`） | 0 | 1 |
| `SYS_SYSTEM_LOG` | 1（`dmesg.c`） | 0 | 2 |
| `SYS_PCILIST` | 3（`lspci.c`、`neofetch.c`、`services.c`） | 1（`libnvrm.c`） | 4 |
| `SYS_CPU_INFO` | 6（`js.c`、`sysmon.c`、`parallel.c`、`js_compile.c`…） | 4（`nocturne.patch`、`os.c`…） | 13 |
| `SYS_GPU_INFO` | 2（`gpu_demo.c`、`js_canvas.c`） | 1（`nvkms_os.c`） | 2 |
| `SYS_GPU_RENDER` | 1（`js_canvas.c`） | 0 | 2 |
| `SYS_AUDIO_DEVICE_RUN` | 1（`audio.c`） | 0 | 1 |
| `SYS_GPU_RENDER_BATCH` | 0（libc の中だけ） | 0 | 2 |
| `SYS_GPU_BLIT` | 1（`js_canvas.c`） | 0 | 2 |
| `SYS_THREAD_CREATE` | 1（`js_compile.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_THREAD_JOIN` | 5（`cppdemo.cpp`、`network.c`、`webfetch.c`、`parallel.c`…） | 2（`nocturne.patch`、`nocturne.patch`） | 9 |
| `SYS_THREAD_EXIT` | 1（`threads.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_WAIT_ADDRESS` | 13（`cppdemo.cpp`、`js.c`、`webfetch.c`、`audio.c`…） | 5（`nocturne.patch`、`uvm_os.c`…） | 12 |
| `SYS_WAKE_ADDRESS` | 18（`audio.c`、`cppdemo.cpp`、`js.c`、`services.c`…） | 7（`nocturne.patch`、`nvrm_serve.c`…） | 16 |
| `SYS_THREAD_ID` | 2（`cxxabi.cpp`、`threads.c`） | 6（`nocturne.patch`、`nocturne.patch`…） | 3 |
| `SYS_SHARED_CREATE` | 4（`browsermseworker.c`、`machine.c`、`ui.c`、`js_atomics.c`） | 0 | 6 |
| `SYS_THREAD_SCHEDULE` | 3（`audio.c`、`browser.c`、`webaudio.c`） | 0 | 0 |
| `SYS_THREAD_STACK_BOUNDS` | 0 | 3（`os.c`、`uvm_os.c`…） | 2 |
| `SYS_THREAD_DETACH` | 8（`accounts.c`、`audio.c`、`network.c`、`programs.c`…） | 1（`nocturne.patch`） | 0 |
| `SYS_WIN_CREATE` | 6（`browser.c`、`gpu_demo.c`、`mandel.c`、`shell.c`…） | 0 | 9 |
| `SYS_WIN_SURFACE` | 0（libc の中だけ） | 0 | 1 |
| `SYS_WIN_PRESENT` | 6（`browser.c`、`gpu_demo.c`、`mandel.c`、`shell.c`…） | 0 | 8 |
| `SYS_WIN_SET_TITLE` | 1（`ui.c`） | 0 | 2 |
| `SYS_SCREEN_INFO` | 7（`browser.c`、`display.c`、`neofetch.c`、`session.c`…） | 0 | 5 |
| `SYS_WIN_MOVE` | 0 | 0 | 5 |
| `SYS_CLIPBOARD_SET` | 4（`browser.c`、`notepad.c`、`term.c`、`ui.c`） | 0 | 1 |
| `SYS_CLIPBOARD_GET` | 4（`browser.c`、`notepad.c`、`term.c`、`ui.c`） | 0 | 0 |
| `SYS_WIN_RESIZE` | 1（`shell.c`） | 0 | 2 |
| `SYS_SCREEN_GRAB` | 1（`machine.c`） | 0 | 4 |
| `SYS_WIN_STATE` | 1（`browser.c`） | 0 | 3 |
| `SYS_WIN_PRESENTATION` | 1（`browser.c`） | 0 | 3 |
| `SYS_WIN_RECEIVE` | 0（libc の中だけ。`gui.c:164`） | 0 | 1 |
| `SYS_WIN_DIRECT` | 1（`browser.c`） | 0 | 2 |
| `SYS_WIN_FRAME` | 1（`ui.c`） | 0 | 1 |
| `SYS_WIN_POINTER` | 1（`ui.c`） | 0 | 0 |
| `SYS_WIN_ACQUIRE` | 1（`ui.c`） | 0 | 2 |
| `SYS_SHELL_PLACE` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHELL_SCREENS` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHELL_WINDOWS` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHELL_WINDOW` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHELL_WATCH` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHELL_HOTKEY` | 1（`shell.c`） | 0 | 0 |
| `SYS_SHARED_RESERVE` | 2（`audio.c`、`js_atomics.c`） | 1（`libnvrm.c`） | 4 |
| `SYS_WAIT_ADDRESS64` | 1（`js_atomics.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_NET_FIRST` | 0 | 0 | 1 |
| `SYS_NET_ADAPTERS` | 4（`ifconfig.c`、`network.c`、`settings.c`、`timesync.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_NET_PING` | 1（`ping.c`） | 0 | 0 |
| `SYS_NET_ADDRESS_SET` | 1（`network.c`） | 0 | 0 |
| `SYS_UDP_SOCKET` | 2（`network.c`、`timesync.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_UDP_SEND` | 2（`network.c`、`timesync.c`） | 1（`nocturne.patch`） | 0 |
| `SYS_UDP_RECV` | 2（`network.c`、`timesync.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_TCP_CONNECT` | 2（`network.c`、`tls.c`） | 1（`nocturne.patch`） | 7 |
| `SYS_TCP_SEND` | 0 | 0 | 4 |
| `SYS_TCP_RECV` | 2（`network.c`、`tls.c`） | 1（`nocturne.patch`） | 6 |
| `SYS_TCP_CLOSE` | 2（`network.c`、`tls.c`） | 1（`nocturne.patch`） | 7 |
| `SYS_UDP_CLOSE` | 2（`network.c`、`timesync.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_NET_TEST` | 1（`ifconfig.c`） | 0 | 1 |
| `SYS_NET_ROUTES_SET` | 1（`network.c`） | 0 | 0 |
| `SYS_TCP_SEND_UNTIL` | 2（`network.c`、`tls.c`） | 1（`nocturne.patch`） | 3 |
| `SYS_TCP_CANCEL` | 1（`tls.c`） | 1（`nocturne.patch`） | 3 |
| `SYS_NET_STATS` | 1（`ifconfig.c`） | 0 | 0 |
| `SYS_NET_SETUP_CANCEL_CHANNEL` | 2（`network.c`、`webfetch.c`） | 0 | 2 |
| `SYS_TCP_LISTEN` | 0 | 1（`nocturne.patch`） | 0 |
| `SYS_TCP_ACCEPT` | 0 | 1（`nocturne.patch`） | 0 |
| `SYS_TCP_ADDRESS` | 0 | 1（`nocturne.patch`） | 0 |
| `SYS_TCP_FINISH` | 0 | 1（`nocturne.patch`） | 0 |
| `SYS_NET_ROUTES` | 0 | 0 | 0 |
| `SYS_NET_WATCH` | 3（`network.c`、`settings.c`、`timesync.c`） | 0 | 0 |
| `SYS_UDP_SOCKET_ON` | 1（`network.c`） | 0 | 0 |
| `SYS_NET_ADAPTER_NAME` | 2（`ifconfig.c`、`settings.c`） | 0 | 0 |
| `SYS_NET_LIMIT` | 0 | 0 | 1 |
| `SYS_HANDLE_CLOSE` | 54（`accounts.c`、`agent.c`、`audio.c`、`browser.c`…） | 10（`nocturne.patch`、`nocturne.patch`…） | 43 |
| `SYS_HANDLE_KIND` | 4（`files.c`、`js.c`、`sh.c`、`stdio.c`） | 5（`nocturne.patch`、`nocturne.patch`…） | 10 |
| `SYS_DOCUMENT_OPEN` | 22（`agent.c`、`browser.c`、`cat.c`、`cp.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 17 |
| `SYS_DOCUMENT_READ_AT` | 22（`accounts.c`、`agent.c`、`browser.c`、`cat.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 10 |
| `SYS_DOCUMENT_WRITE_AT` | 10（`accounts.c`、`cp.c`、`programs.c`、`settingsd.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 8 |
| `SYS_DOCUMENT_SIZE` | 15（`accounts.c`、`agent.c`、`browser.c`、`programs.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 8 |
| `SYS_DOCUMENT_SET_SIZE` | 7（`programs.c`、`sh.c`、`stdio.c`、`js_opfs.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 5 |
| `SYS_DOCUMENT_CLAIM` | 7（`stdio.c`、`js_opfs.c`、`webcache.c`、`webidb.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 8 |
| `SYS_DOCUMENT_RELEASE` | 7（`stdio.c`、`js_opfs.c`、`webcache.c`、`webidb.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 8 |
| `SYS_DOCUMENT_SAVE` | 13（`accounts.c`、`agent.c`、`js.c`、`notepad.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 14 |
| `SYS_DOCUMENT_APPEND` | 4（`js.c`、`systemlog.c`、`webidb.c`、`webstorage_shared.h`） | 2（`nocturne.patch`、`os.c`） | 7 |
| `SYS_ENTRY_INFO` | 0 | 0 | 2 |
| `SYS_ENTRY_REMOVE` | 11（`accounts.c`、`files.c`、`js.c`、`rm.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 19 |
| `SYS_ENTRY_MOVE` | 2（`js.c`、`js_opfs.c`） | 3（`nocturne.patch`、`nocturne.patch`…） | 4 |
| `SYS_FOLDER_CREATE` | 12（`accounts.c`、`agent.c`、`files.c`、`js.c`…） | 4（`nocturne.patch`、`nocturne.patch`…） | 14 |
| `SYS_FOLDER_LIST` | 0 | 0 | 2 |
| `SYS_CHANNEL_PAIR` | 12（`agent.c`、`audio.c`、`network.c`、`services.c`…） | 1（`nocturne.patch`） | 13 |
| `SYS_CHANNEL_SEND` | 30（`accounts.c`、`audio.c`、`browser.c`、`browserjsworker.c`…） | 5（`nocturne.patch`、`nocturne.patch`…） | 17 |
| `SYS_CHANNEL_RECEIVE` | 31（`accounts.c`、`agent.c`、`audio.c`、`browserjsworker.c`…） | 5（`nocturne.patch`、`nocturne.patch`…） | 13 |
| `SYS_APP_LAUNCH` | 0（libc の中だけ） | 1（`nocturne.patch`） | 1 |
| `SYS_APP_RESULT` | 11（`agent.c`、`js.c`、`services.c`、`session.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 20 |
| `SYS_APP_STOP` | 12（`agent.c`、`kill.c`、`services.c`、`session.c`…） | 2（`nocturne.patch`、`nocturne.patch`） | 11 |
| `SYS_APP_SELF` | 6（`browser.c`、`js.c`、`webidb.c`、`websitedata.c`…） | 4（`nocturne.patch`、`nocturne.patch`…） | 17 |
| `SYS_APP_PARENT` | 1（`webfetch.c`） | 1（`nocturne.patch`） | 1 |
| `SYS_EVENTS_CREATE` | 20（`agent.c`、`audio.c`、`browserjsworker.c`、`browsermediaworker.c`…） | 2（`nvrm_serve.c`、`uvm_mm.c`） | 15 |
| `SYS_EVENTS_WATCH` | 26（`agent.c`、`audio.c`、`browser.c`、`browserjsworker.c`…） | 2（`nvrm_serve.c`、`uvm_mm.c`） | 14 |
| `SYS_EVENTS_UNWATCH` | 18（`audio.c`、`browserjsworker.c`、`machine.c`、`network.c`…） | 1（`uvm_mm.c`） | 6 |
| `SYS_EVENTS_WATCH_ADDRESS` | 4（`services.c`、`js_atomics.c`、`js_compile.c`、`js_webaudio.c`） | 2（`nvrm_serve.c`、`uvm_mm.c`） | 3 |
| `SYS_EVENTS_TIMER` | 3（`audio.c`、`shell.c`、`timesync.c`） | 0 | 2 |
| `SYS_EVENTS_WAIT` | 23（`agent.c`、`audio.c`、`browserjsworker.c`、`browsermediaworker.c`…） | 2（`nvrm_serve.c`、`uvm_mm.c`） | 15 |
| `SYS_APP_OPEN` | 3（`kill.c`、`sysmon.c`、`webfetch.c`） | 1（`nocturne.patch`） | 2 |
| `SYS_APP_ID` | 5（`services.c`、`sh.c`、`gui.c`、`services.c`…） | 2（`nocturne.patch`、`nocturne.patch`） | 5 |
| `SYS_SYSTEM_LOG_OPEN` | 4（`browser.c`、`services.c`、`systemlog.c`、`webfetch.c`） | 2（`nocturne.patch`、`os.c`） | 6 |
| `SYS_AUDIO_DEVICE_OPEN` | 1（`audio.c`） | 0 | 1 |
| `SYS_AUDIO_DEVICE_LIST` | 1（`audio.c`） | 0 | 0 |
| `SYS_AUDIO_DEVICE_STATUS` | 1（`audio.c`） | 0 | 1 |
| `SYS_RANDOM` | 6（`accounts.c`、`network.c`、`webfetch.c`、`tls.c`…） | 6（`nocturne.patch`、`nocturne.patch`…） | 8 |
| `SYS_MEMORY_RESERVE` | 1（`malloc.c`） | 4（`nocturne.patch`、`runmain.c`…） | 7 |
| `SYS_MEMORY_RELEASE` | 15（`accounts.c`、`audio.c`、`browsermseworker.c`、`js.c`…） | 12（`nocturne.patch`、`nocturne_os.c`…） | 18 |
| `SYS_MEMORY_PROTECT` | 0 | 2（`nocturne.patch`、`interp-x64.cc`） | 7 |
| `SYS_SHARED_MAP` | 9（`audio.c`、`browsermseworker.c`、`machine.c`、`audio.c`…） | 6（`display.c`、`libnvrm.c`…） | 8 |
| `SYS_PCI_CLAIM` | 0 | 1（`nvrm.c`） | 3 |
| `SYS_PCI_INFO` | 0 | 1（`os_pci.c`） | 2 |
| `SYS_PCI_CONFIG_READ` | 0 | 1（`os_pci.c`） | 3 |
| `SYS_PCI_CONFIG_WRITE` | 0 | 1（`os_pci.c`） | 3 |
| `SYS_PCI_BAR` | 0 | 2（`display.c`、`os_pci.c`） | 3 |
| `SYS_PCI_INTERRUPTS` | 0 | 1（`os_pci.c`） | 2 |
| `SYS_PCI_INTERRUPT_TAKE` | 0 | 1（`nvrm.c`） | 2 |
| `SYS_DMA_CREATE` | 0 | 1（`os_pci.c`） | 3 |
| `SYS_DMA_ADDRESSES` | 0 | 1（`os_pci.c`） | 3 |
| `SYS_SHARED_VIEW` | 0 | 2（`nvrm_serve.c`、`os_pci.c`） | 4 |
| `SYS_SHARED_REVOKE` | 0 | 1（`rm_api.c`） | 3 |
| `SYS_GPU_RENDER_LIST` | 0（libc の中だけ） | 0 | 1 |
| `SYS_CHANNEL_RECEIVE_FROM` | 0 | 1（`nvrm_serve.c`） | 2 |
| `SYS_SHARED_COMPOSE` | 0 | 2（`os_pci.c`、`uvm_mm.c`） | 3 |
| `SYS_PCI_HOLD_SCREEN` | 0 | 1（`os_pci.c`） | 2 |
| `SYS_DOCUMENT_COMMIT` | 4（`accounts.c`、`programs.c`、`settingsd.c`、`stdio.c`） | 2（`nocturne.patch`、`nocturne.patch`） | 3 |
| `SYS_DOCUMENT_INFO` | 1（`webcache.c`） | 2（`nocturne.patch`、`nocturne.patch`） | 1 |
| `SYS_ENTRY_REPLACE` | 2（`systemlog.c`、`stdio.c`） | 2（`nocturne.patch`、`nocturne.patch`） | 2 |
| `SYS_ENTRY_INFO_SIZED` | 21（`accounts.c`、`browser.c`、`cat.c`、`cp.c`…） | 3（`nocturne.patch`、`nocturne.patch`…） | 11 |
| `SYS_FOLDER_LIST_SIZED` | 1（`rm.c`） | 2（`nocturne.patch`、`nocturne.patch`） | 5 |
| `SYS_PCI_GIVE_SCREEN` | 0 | 1（`display.c`） | 2 |
| `SYS_PCI_SCREEN_VBLANK` | 0 | 1（`display.c`） | 2 |
| `SYS_PAGER_CREATE` | 0 | 1（`uvm_mm.c`） | 1 |
| `SYS_PAGER_TAKE` | 0 | 1（`uvm_mm.c`） | 1 |
| `SYS_PAGER_SUPPLY` | 0（libc の中だけ） | 1（`uvm_mm.c`） | 1 |
| `SYS_PAGER_REVOKE` | 0 | 1（`uvm_mm.c`） | 1 |
| `SYS_PAGER_FAIL` | 0 | 1（`uvm_mm.c`） | 1 |
| `SYS_PCI_ACPI_EVAL` | 0 | 1（`os_acpi.c`） | 1 |
| `SYS_PCI_ACPI_NOTIFY_TAKE` | 0 | 1（`os_acpi.c`） | 1 |
| `SYS_PCI_SCREEN_STATE` | 0 | 1（`display.c`） | 2 |
| `SYS_PCI_SCREEN_IMAGE` | 0 | 1（`display.c`） | 2 |
| `SYS_PCI_SCREEN_IMAGE_END` | 0 | 1（`display.c`） | 2 |
| `SYS_PCI_SCREEN_MODES` | 0 | 1（`display.c`） | 2 |
| `SYS_PCI_SCREEN_FLIPS` | 0 | 1（`display.c`） | 1 |
| `SYS_WATCH_TAKE` | 11（`audio.c`、`display.c`、`machine.c`、`network.c`…） | 0 | 6 |
| `SYS_ENTRY_WATCH` | 2（`webnet.c`、`webstorage_shared.h`） | 0 | 4 |
| `SYS_AUDIO_DEVICE_WATCH` | 1（`audio.c`） | 0 | 1 |
| `SYS_IDENTITY_SELF` | 1（`sysmon.c`） | 0 | 2 |
| `SYS_CHANNEL_PEER` | 5（`accounts.c`、`machine.c`、`programs.c`、`session.c`…） | 0 | 2 |
| `SYS_CAP_DERIVE` | 1（`services.c`） | 0 | 2 |
| `SYS_SERVICE_REGISTER` | 1（`services.c`） | 0 | 1 |
| `SYS_SERVICE_CONNECT` | 8（`audio.c`、`settings.c`、`audio.c`、`gui.c`…） | 2（`nocturne.patch`、`libnvrm.c`） | 7 |
| `SYS_STORE_PRIVATE_OPEN` | 3（`accounts.c`、`programs.c`、`settingsd.c`） | 0 | 0 |
| `SYS_SETTINGS_APPLY` | 1（`settingsd.c`） | 0 | 1 |
| `SYS_SETTINGS_EFFECTIVE` | 2（`settings.c`、`settingsd.c`） | 0 | 1 |
| `SYS_SETTINGS_DESCRIBE` | 0 | 0 | 0 |
| `SYS_SETTINGS_WATCH` | 2（`settings.c`、`settingsd.c`） | 0 | 1 |
| `SYS_ENTRY_SET_OWNER` | 1（`accounts.c`） | 0 | 0 |
| `SYS_CAP_KIND` | 0 | 0 | 0 |
| `SYS_CAP_FIND` | 10（`accounts.c`、`audio.c`、`display.c`、`machine.c`…） | 1（`nvrm.c`） | 9 |
| `SYS_POWER_STATE` | 3（`machine.c`、`shell.c`、`sys.c`） | 0 | 0 |
| `SYS_POWER_WATCH` | 1（`shell.c`） | 0 | 0 |
| `SYS_SESSION_STOP` | 1（`session.c`） | 0 | 0 |
| `SYS_CLOCK_WALL_STEP` | 1（`timesync.c`） | 0 | 0 |
| `SYS_CLOCK_WALL_SLEW` | 1（`timesync.c`） | 0 | 0 |
| `SYS_RTC_WRITE` | 1（`timesync.c`） | 0 | 0 |
| `SYS_SCREEN_MODE_APPLY` | 1（`display.c`） | 0 | 0 |
| `SYS_SCREEN_ARRANGE` | 1（`display.c`） | 0 | 0 |
| `SYS_POWER_EVENTS` | 1（`machine.c`） | 0 | 0 |
| `SYS_POWER_EVENTS_WATCH` | 1（`machine.c`） | 0 | 0 |
| `SYS_POWER_WINDOWS` | 1（`machine.c`） | 0 | 0 |
| `SYS_POWER_SAY` | 1（`machine.c`） | 0 | 0 |
| `SYS_POWER_WINDOWS_WATCH` | 1（`machine.c`） | 0 | 0 |

## 4. 残してよいもの

読んだ範囲で構造が正しいと判断したものである。本体を読んでいないものは、その旨を書く。

| 部品 | 理由 | 読んだ範囲 |
|---|---|---|
| `sys/object.h`、`object.c`（handle の表） | 種類ごとの `object_ops`、錠なしの `handle_get`、アプリごとの表。15 ファイルが同じ形で使う。小さな特例は `read`・`write`（K11）、`closes_first`（使い手は `pciuser.c:563` だけ）、`handle_hold`（K2）。 | 見出し |
| `sys/sched.c`、`wait.h`、`wait.c`、`epoch.c` | CPU ごとの実行キュー、対称、待ちの土台が一つ。`wq_wait_unless` を全部の別名が使う。 | 頭と見出し。本体は読んでいない |
| `sys/thread.h`、`process.h` | thread と process の分け方、錠の順。悪い欄は `net_setup_cancel` だけ。 | 見出し |
| `sys/events.c`、`watch.c` | 事象の行列と見張りの型が一つ。`struct watched` を全部の持ち主が使う。 | 頭と `watch.c` の全部 |
| `sys/service.c`、`authority.c`、`common/authority_abi.h` | カーネルは名前を持たない。身元はカーネルが書き、能力は handle。サービスの管理者が名前を持つ。 | 全部 |
| `sys/entry.c`、`kernel/entry/` | 入口の像。`entry_came_from` の確かめ。 | 頭と呼出し |
| `sys/channel.c` | 型のある message、credit、錠の順。byte stream の分岐（K11）は `syscall.c` の側にある。 | 頭 |
| `sys/log.c` | CPU ごとの輪と読み手。カーネルは保存せず、`systemlog` が書く。 | 頭 |
| `dev/audio.c`、`audio.h`、`common/audio_abi.h` | `audio_output` と ops の形。機構はカーネル、方針はサービス `audio`。引き抜きの手本になる。 | 全部 |
| `dev/blk.c` | `blkdev` と ops。`gone` の旗。利用者への一覧が無いのは K5。 | 全部 |
| `dev/pm.c` の段の仕組み | `struct pm_device` の段、一つの thread。 | 頭と関数の一覧 |
| `dev/input.c` の持ち物 | 装置ごとの値を持ち、HID の usage を渡す。文字にしない（`layout` の番号を読まない。`dev/input.h`）。配列は `libc/keymap.c`。 | 全部。一覧の道は K5 |
| `dev/fb.h` の `struct screen_ops` | 表示のドライバーの継ぎ目。`vbe` とアプリのドライバーが同じ口。 | 見出し |
| `vm/vm.h` の include の境界 | `vm_hosted` が外に漏れない。実機では戻るだけ。 | 全部 |
| `fs/`（`store.h`、`document.h` の形） | 権限の決定が `place_allows` の一か所。錠の順が見出しにある。書式は `common/store` でホストの道具と共有。文書という型があり、全部をファイルにしていない。 | 見出し。本体は読んでいない |
| `mm/shared.c` の object の型 | 種類、scope の木による revoke、錠の順。名指し（K4）だけ悪い。 | 頭 |
| `common/services_abi.h`、`input_abi.h`、`clock_data.h` | 型のある記録。 | 頭 |
| `NOCTURNE_TEST_FLAGS` の分け方 | 製品のカーネルが起動の引数を読まない。 | `kernel.h`、`Makefile` |

### 構造は正しいが読みにくいだけの所

- `net/net.c` の詰めた書き方（一行に幾つも文がある）。K2 と別の話である。
- `sys/syscall.c` の GPU の部分の書き方。
- 待ちの語の別名（K15）。型を一つ足せば消える。
- `dev/pm.c` の長い include の列。電源は実際に全部へ触れる。
- `mm/shared.c`（1419 行）、`sys/clock.c`（959 行）、`dev/xhci.c`（2345 行）の長さ。一つの持ち主の長さであり、それ自体は問題でない。`xhci.c` のクラスの決め打ちは K5。

## 5. 集計

全部が見積りである。全行は `wc -l`（`.c`、`.h`、`.asm`）で数えた。

| 部品 | 全行 | 残す | 書き直す | 消す・移す |
|---|---:|---:|---:|---:|
| `arch` | 4977 | 4850 | 100 | 27 |
| `mm` | 5849 | 5599 | 250 | 0 |
| `sys` | 10492 | 8260 | 1615 | 617 |
| `fs` | 3518 | 3458 | 60 | 0 |
| `dev` | 21346 | 19582 | 1700 | 64 |
| `net` | 3156 | 2406 | 650 | 100 |
| `gui` | 4096 | 0 | 271 | 3825 |
| `vm` | 7233 | 4345 | 270 | 2618 |
| `lib` | 726 | 726 | 0 | 0 |
| 頭（`boot.c`、`init.c`、`kernel.h`、`limine.h`） | 1053 | 993 | 60 | 0 |
| **`kernel/src` の計** | **62446** | **50219** | **4976** | **7251** |
| `common/*_abi.h`、`abi.h`（別に数える） | 2078 | 1378 | 540 | 160 |

割合は、残す 80%、書き直す 8%、消す・移す 12%。

読むときの注意：

- 「残す」は、構造の問題が見えなかったという意味である。良いと確かめた意味ではない。大半は本体を読んでいない。とくに `dev` の 19582 行と `mm` の 5599 行。
- K1 を持ち主が「カーネルに残す」と決めれば、`gui` の 3825 行は「書き直す」へ移る。合成の持ち方と `wm_mutex` は別に直す必要がある。
- 新しく書く行は入れていない。窓のサービスの受け口、装置の木（800〜1500 行の見積り）、画面の handle など。
- 消す・移すの 7251 行のうち、製品から本当に消えるのは約 800 行の見積りである。残り（`wm.c`、`rdp.c`、`virtio_gpu.c`）はサービスや継ぎ目の後ろへ移る。

### 問題ごとの行

重なりがあるので、足しても上の表にならない。

| 問題 | 書き直す | 消す・移す | 内訳 |
|---|---:|---:|---|
| K1 窓 | 271 | 3825 | `gui` の全部 |
| K2 ソケット | 650 | 0 | `net.c` |
| K3 受け口 | 1065 | 330 | `syscall.c` |
| K4 表示の役 | 1350 | 0 | `pciuser.c` 700、`fb` 400、`mm/shared.c` 250 |
| K5 装置の型 | 510 | 0 | `pci.c`、`input.c`、`xhci.c`、`usb.h`、各ドライバー、`init.c` |
| K6 RDP | 270 | 1880 | `hvsock.c`、`hv_util.c`、`rdp.c` |
| K7 GPU | 0 | 946 | `virtio_gpu.c` 738、`syscall.c` 134、`gpu_abi.h` 60、`gpu.h` 14 |
| K8 設定 | 207 | 0 | `settings.c`、`settings.h` |
| K9 電源 | 60 | 50 | `pm.c`、`power.c` |
| K10 試験 | 0 | 約 410 | `prof.c` 287、`idle.c` 24、`net.c` 約 100 |
| K11 端末 | 423 | 0 | `stream.c` 233、`object` 40、`proc.c` 60、`fbcon.c` 90 |
| K12 ABI | 300 | 100 | `abi.h` |
| K14 層 | 220 | 0 | `arch` 100、`fs` 60、`init.c` 60 |

## 6. 依存の順序

### 持ち主が先に決めること

順序の前に、三つは持ち主の決定が要る。コードからは決まらない。

1. 窓の管理と合成の持ち主はサービスか（K1）。
2. TCP/IP の置き場はカーネルのままか（K2）。この報告はカーネルに置いたまま object にする道で書いた。
3. virgl の道をどこへ置くか（K7）。

### 「X を変えるには Y が先」

| 変えるもの | 先に要るもの | 理由 |
|---|---|---|
| K1 窓をサービスへ | K4 画面の handle | サービスが画面を持つ口が要る |
| K1 | K5 の入力の口 | 今の sink はカーネルの関数の指である（`dev/input.h:47`） |
| K6 RDP をサービスへ | K1 | `wm_remote_*` は窓の管理の中にある |
| K9 の `POWER_WINDOWS` | K1 | 窓の一覧の持ち主が動く |
| K13 の `WIN_*` | K1 | 窓の口の設計の中で扱う |
| K12 の `SYS_SCREEN_INFO` の多重 | K1、K4 | 画面の口が動く |
| K13 の `NET_SETUP_CANCEL_CHANNEL` | K2 | handle を閉じて取り消せるようになってから消す |
| K15 の閉じる口 | K2 | 同じ |
| K4 `mm/shared.c` の名指し | K4 の claim の抽象 | 同じ仕事の中で行う |
| K7 `SYS_GPU_*` | 持ち主の決定 3、F6 の台帳 | virgl の道を消さないため |
| K14 の `sys/syscall.h` | K3 の user copy の移動 | 同じ仕事 |
| K8 の門の形 | 無い | 持ち主が登録するだけ |

### 進める順

依存の無いものから並べる。

**段 0。何にも依らず、呼ぶ側が少ない。**

- K12：死んだ欄、古い形、番号の穴。呼ぶ者は試験と `ports` だけ。
- K10：試験と計測を製品から出す。
- K8：門の形と残り（`dns_setting_*`）。
- K14：`boot_setting` の移動、`init.c` と `syscall.c` の自前の宣言。
- K3：user copy と launch の移動。受け口を表にする。

**段 1。独立していて、呼ぶ側が広い。**

- K2：ソケットを object に。続けて K13 の取消しの口と K15 の閉じる口を消す。
- K11：byte stream と端末の形。libc の stdio、`sh`、`term` が変わる。
- K9 の `SYS_POWER_SAY`：`machine` とシェルの間の channel。

**段 2。土台を作る。**

- K5：装置の木、一致の表、抜き差しの事象、入力の口。
- K4：`pciuser` を分ける。画面を handle にする。`mm/shared.c` の名指しを消す。display 監査の D-01〜D-22 と一緒に設計する。
- K7：持ち主の決定の後。

**段 3。上に載るもの。**

- K1：窓のサービス。compositor 監査の H1、H8、C1、C2 と一緒に設計する。
- K6：RDP をサービスへ。
- K9 の残り、K13 の `WIN_*`、K12 の `SYS_SCREEN_INFO`。

### 順序の限界

- この順序は、読んだ範囲からの判断である。読まなかった `mm`、`arch`、ドライバーの本体に重い問題があれば変わる。
- K5 は K1 と K4 の厳密な前提ではない。先に行えば入力の口と画面の口を二度作らずに済む、という理由である。
- 段 0 と段 1 は並べて進められる。K2 と K11 はどちらも `user/libc` に触れるが、持ち物は別である。
- 全部の段で、確かめは製品の媒体と窓付きの QEMU で行う必要がある。ここには動かして確かめた結果は一つも無い。
