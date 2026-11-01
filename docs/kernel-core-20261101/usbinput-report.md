# usbinput の報告

## 依頼文

あなたの名は「usbinput」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の b6388a38。QEMU のポートは HMP 45462・QMP 45463。

# 仕事：入力の道を、装置の列挙や記憶の転送に止められない形にする
持ち主は USB と入力の部品：`kernel/src/dev/xhci.c`・`usbhid.c`・`usbhub.c`・`usbmsc.c`・`usbnet.c`・`usb.h`・`input.c`・`input.h`。監査 `audit-kernel.md` の要点 1・5（xHCI の部分）と `audit-compositor.md` の入力の項を読むこと。

必ず入れる形：
- 今は全部の制御器と class driver を一本の "usb" thread で回し、HID の report も同じ thread が処理し、その thread が port の reset・debounce・usbmsc の待ちで眠る。眠っている間 event ring が読まれず、マウスとキーボードが 100 ms〜10 s 止まる。これを消す：event ring の処理（転送の完了を配る）は眠らない文脈で回し、列挙・hub・記憶の装置の長い手順は眠ってよい別の流れ（装置ごと、または制御器ごと）に移す。HID の report は完了からすぐ入力へ届く。
- xHCI の interrupter を複数使える形（MSI-X の vector を割り込みごとに、装置の種類で ring を分けられる）にし、interrupter 0 だけの前提を消す。制御器の数の固定の上限（MAX_HC など）を消す。
- 入力の事象に、完了を受けた時刻（単調時計の ns、`sys/clock.h`）を付けて渡す（`input.c` まで。アプリへの ABI の欄は「compositor」の書き手が入れるので、`input.h` の内側の事象に時刻を持たせればよい）。大域の `input_lock` で割り込みを止める区間を短くする。
- 中核の締切の級（`sched_deadline`）を、周期のある仕事にだけ使う。周期は端点の service interval から（今の `input_period_ns` は短くなる方へしか動かない欠陥がある。装置が抜けたら戻ること）。
- usbmsc の 250 ms × 40 回の待ちのような固定回数の spin・眠りを、仕様（USB Mass Storage BOT、xHCI）の事象で待つ形に。

範囲の外：中核は使うだけ。窓の系・`common/abi.h` の窓と画面の部分は「compositor」が持つ。

確かめ：QEMU で、起動、USB の kbd と tablet の入力（QMP の `input-send-event` で指、HMP の `sendkey` でキー）、Terminal に打てる、窓を動かせる。可能なら QEMU に USB の記憶の装置（`-device usb-storage`）を足して、列挙の間も指が止まらないこと（列挙の時刻と指の事象の時刻を serial の行で並べる）。

## 設計

### 文脈の分け方

| 文脈 | 何をするか | 眠るか |
|---|---|---|
| 割り込み（`xhci_isr`） | interrupter ごとの event ring を読み切る（`drain`）。転送の完了は端点の callback へ、command の完了は待ち手へ、port の変化は worker への知らせへ配る。 | 眠らない。 |
| class の report の callback | `usb_report_fn` は割り込みの中で、完了の時刻 `at` を受けて走る。HID はその場で `input_key`・`input_mouse` を呼ぶ。hub は状態変化の report を写し、worker を起こすだけ。usbnet は受けた frame を網へ渡す。 | 眠らない。 |
| 制御器ごとの worker（`usb%d` の核の thread、公平の級） | 起動、root port の列挙、hub の port の列挙（`usbhub_work`）、HID の LED の SET_REPORT（`usbhid_work`）、止まった端点の回復、制御器の再起動、S3 の後の再開。 | 眠ってよい。 |
| 呼ぶ側の thread | usbmsc の読み書き・flush は、`blk` を呼んだ thread がそのまま BOT の三段（CBW・data・CSW）を流し、完了を端点の wait queue で待つ。 | 眠ってよい。 |

一本の "usb" thread と、その呼出しの行列（`usb_run`）は無い。列挙や記憶の待ちで眠るのは worker か呼ぶ側の thread で、event ring は割り込みで読まれ続ける。

### 錠と待ち

- 端点ごとに spinlock と wait queue を持つ。転送を積む者と完了を配る割り込みは、その端点の錠だけを取る。大域の `send_lock` は無い。
- 装置ごとに control の mutex（control 転送を一度に一つ）と、spinlock・wait queue（control の完了）を持つ。
- 制御器ごとに command の mutex と、spinlock・wait queue を持つ。command の完了は interrupter 0 に来る（xHCI 1.2 4.17）。
- 装置を外すときは `removed` を立て、`sync_drains` で全部の interrupter の錠を一度取って放す。これで割り込みの中の callback が外す装置に触れ終わったことが決まり、その後で class の detach と slot の解放をする。
- worker への知らせは制御器の数 `kicks` を増やして起こす形で、待つ側は `wq_wait_unless` で見た数から動いたかを待つ。hub の port の reset は、状態変化の report が来て数が動くのを `usb_kicks`・`usb_wait_kick` で待つ。

### interrupter と MSI-X

- interrupter は ring の種類（`RING_CONTROL`・`RING_PERIODIC`・`RING_BULK`）の数、制御器の MaxIntrs、PCI が与えた message の数の小さい方だけ作る。MSI-X では interrupter n が vector n を鳴らし、MSI では n を有効な message の数で割った余りになる（4.17）。
- 端点の TRB は、状態の dword の Interrupter Target（31:22）に種類ごとの interrupter を入れる。interrupt 端点は 1、bulk 端点は 2、control と command と port は 0。interrupter が足りなければ 0 にまとめる。
- interrupt 端点の interrupter は IMOD を 0 にし、完了ごとにすぐ割り込む。control と bulk の interrupter は IMODI を 1000（250 ns の単位で 250 µs）にし、まとめて割り込む。
- 各 interrupter の割り込みは `pci_irq_enable` が node の中で vector の空いた CPU に置く。起動の行に interrupter ごとの CPU と vector を出す。
- 起動の終わりに No Op の command（型 23）を一度送り、interrupter 0 の割り込みが来るかを確かめる。event が ring にあるのに割り込みが一度も来なければ、その制御器は割り込みを止めて poll に移る（`interrupts_lost`・`hc_poll`）。
- 制御器は `kappend` で伸ばす配列に入る。数の上限（`MAX_HC`）は無い。

### 周期と締切の級

- HID の report は割り込みの中で配るので、周期で回す thread が要らない。列挙と回復は周期の無い仕事なので、公平の級の worker で回す。USB は `sched_deadline` を使わない。依頼の「締切の級を周期のある仕事にだけ使う」は、周期のある thread が USB に残らない形で満たす。
- 端点の service interval は、poll に落ちた制御器の拍に使う。制御器ごとに開いている interrupt 端点の数を interval の指数ごと（`periodic[16]`）に数え、端点を閉じると減らす。poll の alarm（`poll_fire`）は一番速い端点の間隔（125 µs × 2^n、上限 1 ms）で次を立てる。装置が抜けて数が 0 になれば、その指数は拍の計算から外れ、拍は長い方へ戻る。
- `input_period_ns` は無い。

### 入力の時刻と錠

- `struct key_event` と `struct mouse_event`（`input.h`）に `time_ns` を足した。値は event ring を読んだ時の `clock_now_ns()` で、`drain` が一回の読み切りごとに一度取り、callback の `at` として渡す。
- `input.c` の `input_lock` は無い。key と mouse の受け手、修飾キーの状態、Caps Lock は atomic の語で持ち、割り込みを止める区間が無い。Caps Lock は `__atomic_xor_fetch` で切り替える。
- キーの repeat は HID ごとの alarm（`repeat_fire`）で、周期の thread（`usbhid_tick`）は無い。repeat の事象は alarm の時刻を `time_ns` に持つ。
- Caps Lock を押すと全部のキーボードに LED の更新の知らせを立て、各装置の worker が SET_REPORT を送る。
- Hyper-V の入力（`vm/hv/hv_input.c`）と RDP（`vm/rdp.c`）も、受けた時の `clock_now_ns()` を `time_ns` に入れる。

### 記憶の装置（usbmsc）

- 一回の command の上限は 10 s（`CMD_MS`）。待ちは端点の完了の事象で、数を数える spin や眠りは無い。
- 装置の用意（`unit_ready`）は SCSI の答えで進む。UNIT ATTENTION はすぐ問い直す。NOT READY の 04/xx には START STOP UNIT（IMMED=0）を送る。この command の状態は装置が回り終えた時に返るので、回り終わりを事象として待てる（上限 30 s、`START_MS`）。3A は媒体が無いとして止める。250 ms × 40 回の輪は無い。
- 装置ごとの mutex が BOT の三段を一つずつに保つ。detach はその mutex を取ってから外す。

### 仕様の時間で眠る所

次の待ちは事象ではなく、仕様が決めた最小の時間なので、worker が `sleep_until` で眠る。

- hub の port の電源が安定するまで：bPwrOn2PwrGood × 2 ms（下限 100 ms）。
- 接続の debounce：100 ms（USB 2.0 7.1.7.3）。
- reset の後の回復：10 ms（USB 2.0 7.1.7.5）。

hub の状態変化の report が動いていない間（hub の起動の途中）は、port の reset の完了を 10 ms ごとに見に行く。

## 変えたファイル

| ファイル | 変えたこと |
|---|---|
| `kernel/src/dev/xhci.c` | 書き直した。interrupter の配列、割り込みでの読み切り、端点・装置・制御器ごとの錠と待ち、制御器ごとの worker、poll の alarm、制御器の動的な配列。 |
| `kernel/src/dev/usb.h` | 文脈の説明、`usb_report_fn` に `at`、`usb_note_work`・`usb_kicks`・`usb_wait_kick`・`usbhid_work`・`usbhub_work`・`usbhub_pending`、`removed` を atomic に。 |
| `kernel/src/dev/usbhid.c` | 割り込みの中の report、HID ごとの錠と repeat の alarm、LED を worker で、全部の HID の一覧を錠で守る。 |
| `kernel/src/dev/usbhub.c` | hub ごとの作業（`usbhub_work`）、port の reset を状態変化の report で待つ、大域の hub の一覧を消した。 |
| `kernel/src/dev/usbmsc.c` | 呼ぶ側の thread での BOT、SCSI の答えで進む `unit_ready`、装置ごとの mutex。 |
| `kernel/src/dev/usbnet.c` | callback に `at` を足し、錠の説明を今の文脈に合わせた。 |
| `kernel/src/dev/input.h`・`input.c` | `time_ns`、錠の無い状態、`input_key_set1` と `input_unicode` に時刻。 |
| `kernel/src/vm/hv/hv_input.c`・`kernel/src/vm/rdp.c` | 入力の事象に受けた時刻を入れる。 |

## 消したもの

- `xhci.c`：`usb_run` と呼出しの行列、`usb_lock`・`usb_wq`・`usb_thread`、`input_period_ns`、`MAX_HC`、`send_lock`、`hc_wait`、`poll_all`、`any_polled`、`sched_deadline` の使用。
- `usb.h`：`usb_run`、`usbhid_tick`。
- `usbhid.c`：`usbhid_tick`。
- `usbhub.c`：大域の hub の一覧と `hubs_changed`。
- `usbmsc.c`：250 ms × 40 回の待ち、`struct io`・`do_io`・`do_flush`（usb thread へ渡す包み）。
- `input.c`：`input_lock`、`stream_e0`、`input_scancode`。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：作業木で通る。警告は無い。出力は `D:\Programes\llm vibe slop os\build\wt\agent-a6069a02e18e0dbd4\kernel.elf`。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：作業木では組めない。git の外のファイル `third_party/tinycc/conftest.c` が作業木に無いためで、組みの記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\build-img.log`。媒体を作業木へ写すことは、媒体を二つまでに保つフックが止めた。
- そのため QEMU は次の形で起こした：main の `build/nocturne.img` を `snapshot=on` で NVMe に置き（C: はここ）、作業木の `kernel.elf` と limine を読むだけの vvfat の USB の記憶（`fat:<作業場>/esp`、`usb-storage`、bootindex 0）から起動した。起こす命令は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\run.ps1`。C: の中身とユーザー空間は main のもので、カーネルだけがこの枝のもの。

## QEMU で見たもの

QEMU は q35、WHPX、4 CPU、4 GiB、窓付き、2560×1440。入力は xHCI の usb-kbd と usb-tablet。

### 起動

最後に組んだカーネル（一時の記録を消した後）の起動の行（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\serial-final.log`）：

```
[   18.282759 2:10] usb: xHCI 0 (1b36:000d) version 100, 8 ports, 64 slots, 3 interrupters of 16, MSI-X
[   18.283870 2:10] usb: controller 0 interrupter 0 (commands, ports, control) on cpu 2 vector 0x30
[   18.285868 2:10] usb: controller 0 interrupter 1 (interrupt endpoints) on cpu 3 vector 0x30
[   18.286034 2:10] usb: controller 0 interrupter 2 (bulk endpoints) on cpu 0 vector 0x31
[   18.294323 2:10] usb: 46f4:0001 class 00 on controller 0 port 3, super speed, slot 1
[   18.295097 2:10] usbmsc: slot 1 bulk-only storage, 1 unit
[   18.297130 2:10] blk: usb0: QEMU QEMU HARDDISK, 504 MiB, 512-byte blocks
[   18.323481 0:1] blk: nvme0: QEMU NVMe Ctrl, 1216 MiB, 512-byte blocks
[   18.411838 2:10] usb: 0627:0001 class 00 on controller 0 port 5, high speed, slot 2
[   18.412827 2:10] usbhid: slot 2 interface 0 is a keyboard
[   18.458435 0:1] wm: compositor started at 2560x1440
[   18.527309 2:10] usb: 0627:0001 class 00 on controller 0 port 6, high speed, slot 3
[   18.528379 2:10] usbhid: slot 3 interface 0 is a absolute pointer with wheel
```

三つの interrupter が三つの CPU に分かれて置かれた。

### 入力

- `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\boot1.png`：デスクトップとタスクバーが出た。
- `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\term0.png`：QMP の `input-send-event` の二度押しで Terminal が開いた。
- `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\term1.png`：HMP の `sendkey` で `echo Hi` と `free` を打ち、答えが出た。題の帯を引きずって窓が (110,32) から (656,380)（表示の縮尺の座標）へ動いた。
- `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\final.png`：最後のカーネルで、`echo Ok` を打ち、窓を右上へ引きずった。
- `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\caps.png`：Caps Lock、`a`、`b`、Caps Lock、`c` で `ABc` と出た。LED の SET_REPORT の誤りの行は serial に無い。

### 列挙の間の指

一時の行（`mouse_report` で完了の時刻 `at` を出す一行）を入れたカーネルで測った。この行は測った後に消し、組み直した。

QMP で指を 10 ms ごとに左右へ 400 回動かしながら、HMP の `drive_add` と `device_add usb-storage` で 64 MiB の記憶を差した。QEMU はこの記憶を、前の差し込みで自分が足した full speed の hub（slot 4）の port 2 に付けた。そのため列挙は hub の状態変化の report、接続の debounce（100 ms）、port の reset と回復（10 ms）、address、記述子、configuration、usbmsc の INQUIRY・TEST UNIT READY・READ CAPACITY を通る。

記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\usbinput\serial-hotplug.log`。列挙の終わりの前後の行：

```
[  111.608217 3:6] TMPPTR at 111608121982 x 17919
[  111.623383 3:6] TMPPTR at 111623300650 x 15360
[  111.639616 3:6] TMPPTR at 111639531854 x 17919
[  111.655802 3:6] TMPPTR at 111655711240 x 15360
[  111.672536 3:6] TMPPTR at 111672426305 x 17919
[  111.690237 3:6] TMPPTR at 111690129008 x 15360
[  111.706015 3:6] TMPPTR at 111700893625 x 17919
[  111.716198 3:6] TMPPTR at 111716133225 x 15360
[  111.731302 3:6] TMPPTR at 111731223836 x 17919
[  111.746621 3:6] TMPPTR at 111746582248 x 15360
[  111.762711 3:6] TMPPTR at 111762630757 x 17919
[  111.778951 3:6] TMPPTR at 111778759143 x 15360
[  111.794787 3:6] TMPPTR at 111794704644 x 17919
[  111.810198 3:6] TMPPTR at 111810104817 x 15360
[  111.826068 3:6] TMPPTR at 111825972371 x 17919
[  111.841662 3:6] TMPPTR at 111841578749 x 15360
[  111.857253 3:6] TMPPTR at 111857175382 x 17919
[  111.872889 3:6] TMPPTR at 111872801146 x 15360
[  111.889935 3:6] TMPPTR at 111889849273 x 17919
[  111.894919 1:10] usb: 46f4:0001 class 00 on controller 0 port 8 (hub port 2), full speed, slot 5
[  111.895602 1:10] usbmsc: slot 5 bulk-only storage, 1 unit
[  111.897343 1:10] blk: usb2: QEMU QEMU HARDDISK, 64 MiB, 512-byte blocks
[  111.905728 3:6] TMPPTR at 111905626336 x 15360
[  111.922764 3:6] TMPPTR at 111922663428 x 17919
[  111.940923 3:6] TMPPTR at 111940815132 x 15360
```

- 指の report は CPU 3（interrupter 1 の CPU）の割り込みで配られ、列挙は CPU 1 の worker（tid 10）で進んだ。
- 指を動かした 109.575 s から 115.861 s までの 400 個の事象で、`at` の間の一番長い間隔は 26.2 ms（109.739 s と 109.766 s の間で、差し込みの前）だった。debounce の 100 ms を含む列挙の間も、間隔は 15〜18 ms のまま続いた。指を送る側は 10 ms の眠りに QMP の往復を足した間隔で送るので、15 ms 前後がこの測り方の下限になる。
- 同じ測りを main の b6388a38 のカーネルではしていない。比べた二つの版の数は無い。言えるのは、この枝のカーネルで列挙の間に指の事象が途切れなかったことだけ。

### QEMU の警告

QEMU の stderr に `warning: Ignoring request for interrupt vector 0` が一度出る。起動から 1.4 s、serial にカーネルの行がまだ無い時（UEFI の段）に出た。rustenv の書き手の QEMU（この枝の変更を含まないカーネル）の stderr にも同じ行がある。この枝の変更から出たものではない。

## 残った欠陥と未確認

- `qemu-image` を作業木で組めなかった。C: とユーザー空間を含めた媒体は main へ載せた後に組む必要がある。
- `gui/wm.c` は入力の事象を受けた時に自分で時刻を取る。`time_ns` を使うのは compositor の持ち物で、この枝では触れていない。
- 記憶の転送（大きな読み書き）が流れている間の指の間隔は測っていない。測ったのは列挙の間だけ。
- 割り込みの来ない制御器の poll の道（`interrupts_lost`・`hc_poll`・`poll_fire`）は QEMU では通っていない。QEMU の xHCI は MSI-X の割り込みが来る。
- 制御器が二つ以上ある形、MSI だけの制御器、interrupter が一つの制御器は試していない。
- S3 からの再開（`xhci_resume`）は WHPX では試せない。
- 実機は試していない。
- hub の port の電源・debounce・reset の回復は、仕様の最小時間を worker が眠って待つ。指の道は止めない。
