# USB の driver の差分の敵対的な監査（三回目）

## 依頼文

Nocturne OS のカーネルの USB の driver の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は作業木の未コミットの変更を含みます。

材料：
- 差分：作業木で `git diff 34ab1dbe -- kernel/src/dev/xhci.c kernel/src/dev/usbhid.c kernel/src/dev/usb.h`。必要なら `git diff 55142942` の全部も。
- 今のコード：`kernel/src/dev/xhci.c`、`usbhid.c`、`usbhub.c`、`usbmsc.c`、`usb.h`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt、mscboot.txt、mscoverview.txt、scsi.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順を調べます。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix3-usb.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 15 件である。

| 重さ | 件数 | 指摘 |
|---|---|---|
| データを失う | 0 | なし |
| 当たれば壊れる | 4 | U1、U2、U3、U4 |
| きれいさ | 11 | U5〜U15 |

行番号は監査の時点の作業木のものである。文書は xHCI 1.2、USB 2.0、USB 3.2、HID 1.11、HUT、BOT である。

`34ab1dbe` からの差分の行そのものに当たる指摘は U10 と U12 である。その差分で古くなった行は U9 と U13 である。残りは `55142942` からの差分か、差分が触れた道の周りのコードにある。

## 当たれば壊れる

### U1：USB3 のルートポートの Error と Compliance から回復しない

- 場所：`kernel/src/dev/xhci.c` 1739 行（`root_port` が CCS=0 で戻る）、1752 行、1717–1728 行（`link_trained`）。
- 仕様：xHCI 4.19.1.2 の Figure 4-27 は、Error（SS.Inactive）、Compliance、Polling の信号を CCS=0、PED=0、PR=0 とする。Table 5-27 の脚注 81 は、USB3 の CCS は Polling から Enabled へ移るときに立つと書く。4.19.1.2.5 は、Error から PR か WPR で Reset へ移れると書く。
- 差：`link_trained` は PLS が Inactive か Compliance のとき false を返し、呼ぶ側が warm reset を出す形である。その判定は CCS=1 の後でしか走らない。この二つの状態の CCS は 0 なので、判定に届かず WPR が出ない。
- 筋書き：動いている USB3 機器のリンクが Inactive に落ちる。PED が落ちて `root[p]` が外れる。CCS=0 なので 1739 行で戻る。warm reset が出ないので、抜き差しまで機器は消えたままである。USB ディスクなら mount が失われる。
- 差の比べ：hub の下のポートは `usbhub.c` 290–300 行で stuck を扱う。ルートポートだけがこの回復を持たない。
- 直す案：`root_port` で usb3 かつ PLS が Inactive か Compliance なら、CCS=0 でも warm reset へ進む。`link_trained` の中の二つの判定は CCS=1 の後では成り立たないので消す。

### U2：キーを押したまま抜くと解放後の alarm が走る

- 場所：`kernel/src/dev/usbhid.c` 575 行（`usbhid_detach` の `alarm_cancel`）、128–140 行（`repeat_fire`）、586 行（`kfree(h)`）。
- 規則：`kernel/src/sys/alarm.h` は、owner は alarm が armed か firing の間は生かすと定める。`alarm_cancel` は別の CPU の fire の終わりを待つだけで、fire が張り直すと armed のまま返る（`sys/alarm.c` 147–157 行）。
- 差：`repeat_fire` は `h->repeat_usage` が 0 でない間 `alarm_set` で張り直す。`usbhid_detach` は `repeat_usage` を消さずに `alarm_cancel` を呼び、続けて `kfree(h)` を呼ぶ。
- 筋書き：キーを押したまま USB キーボードを抜く。別の CPU で `repeat_fire` が走っていて、`alarm_cancel` の待ちの間に張り直す。`h` が解放された後に alarm が発火し、解放済みの記憶で `spin_take` と `send_usage` が走る。結果は記憶の破壊か panic である。
- 直す案：`h->lock` の下で `repeat_usage` を 0 にしてから `alarm_cancel` を呼ぶ。

### U3：メモリー切れの列挙で panic する

- 場所：`kernel/src/dev/xhci.c` 1579–1581 行（確保の失敗で `goto fail`）、1645–1649 行、1010–1016 行（`release_slot`）、990–998 行（`slot_quiet`）。
- 仕様：xHCI 4.6.9 は、Stop Endpoint の前提を Slot State が Default、Addressed、Configured のどれかであることとし、違えば Context State Error とする。4.6.3 と 4.6.5 の手順では、Enable Slot の直後の slot はまだ Address Device を受けていない。
- 差：`release_slot` は `xd` が非 NULL なら必ず `slot_quiet` を呼び、DCI 1 に Stop Endpoint を出す。`out_ctx` の確保に失敗した `xd` では `xd->out_ctx` が NULL である。Stop は Context State Error で返るので、995 行の `out_ctx(xd, dci)[0]` が NULL 基点の番地を読む。
- 筋書き：メモリーが尽きた時に機器を差す。`dma_page` が NULL を返す。カーネルの頁違反で panic する。メモリー切れは拒否で済むはずの場面である。
- もう一つの差：`in_ctx`、`ep0`、`xfer` の確保に失敗した場合は `hc->dcbaa[slot]` が 0 のまま Stop を出す。読む値は 0 なので壊れないが、Address Device 前の slot へのコマンドは 4.6.9 の前提に合わない。
- 直す案：`slot_quiet` は Address Device を出した slot だけで行う。`xd` にその印を持たせ、印が無ければ Stop を出さずに Disable Slot へ進む。

### U4：BIOS が所有していないときに OS の所有権を取らない

- 場所：`kernel/src/dev/xhci.c` 1943–1949 行（`hc_start` の USBLEGSUP）。
- 仕様：xHCI 4.22.1 の Figure 4-37 は、driver の読み込みで OS Owned を 1 にすると定める。Table 7-4 は、所有権が得られるのは OS Owned が 1 で BIOS Owned が 0 と読めるときだと定める。4.22.1 は、BIOS が所有権を取ってよいのは OS Owned が 0 のときだと定める。
- 差：コードは BIOS Owned が 1 のときだけ OS Owned を書く。BIOS Owned が 0 なら OS Owned は 0 のままである。この間、firmware は文書の上で所有権を取ってよい。
- 筋書き：起動の時点で BIOS Owned が 0 の機械で OS が xHC を使い始める。後で SMM の firmware が OS Owned=0 を見て所有権を取る。OS と firmware が同じ controller を同時に動かす。
- 同じ行の小さな差：`mmio_write8(hc->cap + x + 3, 1)` は bits 31:25 に 0 を書く。Table 7-4 はこの範囲を RsvdP とする。読んだ値を保って書く。
- 直す案：`hc_start` のたびに BIOS Owned の値によらず OS Owned を立て、OS Owned=1 かつ BIOS Owned=0 を待つ。byte の書込みは読んだ値に bit 0 を足す。

## きれいさ

### U5：BOT の CSW の読み直しが Bulk Error を外している

- 場所：`kernel/src/dev/usbmsc.c` 105–109 行。
- 仕様：BOT 5.3.3 の Figure 2 は、CSW の読みで Bulk-In が STALL か Bulk Error になれば halt を解いてもう一度読むと書く。
- 差：コードは `USB_STALL` のときだけ読み直す。Bulk Error の -1 は読み直さず、CSW の検査に落ちて Reset Recovery へ進む。
- 筋書き：CSW の読みでバス誤りが一度起きる。状態が取れず `BOT_BROKEN` になる。`transfer` が要求ごと再試行するので、データは失わない。
- 直す案：`r < 0` でも `usb_clear_halt` をして一度読み直す。timeout は Figure に無いので分けてよい。

### U6：hub の下の SSP 機器の Slot Context の Speed が rank を見ない

- 場所：`kernel/src/dev/xhci.c` 1535–1539 行（`port_speed_id`）、1665 行。
- 仕様：xHCI 4.5.2 と 6.2.2.1 は Speed にポートの定める速度を入れると書く。Table 6-4 はこの欄を deprecated かつ Reserved とする。
- 差：`port_speed_id` は kind だけで引く。hub の下の Gen1x2 と Gen2x2 の子にも、最初に見つかった SSP の ID が入る。
- 筋書き：Gen2x2 の hub の下の Gen1x2 の機器で、Slot Context の Speed が実際の速度と違う。文書がこの欄を deprecated とするので、controller が値を使う機械だけで影響が出る。
- 直す案：`port_speed_id` に rank も渡して PSI から ID を引く。

### U7：コメントの節番号が Configure Endpoint を Evaluate Context の節と書く

- 場所：`kernel/src/dev/xhci.c` 769 行、1632 行。
- 仕様：6.2.2.3 は Evaluate Context の節である。Configure Endpoint は 6.2.2.2 で、4.6.6 も 6.2.2.2 を指す。
- 差：コメントが「6.2.2.3」と書く。
- 筋書き：挙動には当たらない。読み手が別の節を開く。
- 直す案：両方を 6.2.2.2 に直す。

### U8：`ring_fit` のコメントの節と No Op の扱い

- 場所：`kernel/src/dev/xhci.c` 396–407 行。
- 仕様：4.11.5.1 は Link TRB の節である。No Op の節は 6.4.1.4 と 4.11.7 である。6.4.1.4 は連続する No Op を避けよと書く。4.11.7.1 は TD Fragment が segment を跨ぐことを「should not」、4.11.5.1 は「shall not」とする。
- 差：コメントが No Op の根拠を 4.11.5.1 と書く。`ring_fit` は必要な数の No Op を続けて積む。
- 筋書き：残りが 2 entry 以上の segment の端で、No Op が連続する。文書の勧めからの差である。
- 直す案：コメントの節を 6.4.1.4 と 4.11.7 に直す。No Op は 1 entry しか埋めないので、連続を消すには余りの entry を別の形で扱う設計が要る。今の形は勧めからの差として記録する。

### U9：端点の `len` のコメントが古い

- 場所：`kernel/src/dev/xhci.c` 163 行。
- 差：コメントは stream の `len` を 1 service interval の payload と書く。今回の差分で `usb_interrupt_length` から `longest`=0 の道が消えた。今は HID は 1 packet、hub は最長を packet に切り上げた値、`usbnet.c` は自分の長さである。
- 筋書き：挙動には当たらない。
- 直す案：コメントを「`usb_stream_start` の呼び手が渡す 1 転送の長さ」に直す。

### U10：scratchpad のコメントが Table 6-1 を誤って読む

- 場所：`kernel/src/dev/xhci.c` 1998–2000 行。
- 仕様：Table 6-1 の Scratchpad Buffer Array は最大 248 バイト、Boundary が PAGESIZE、Alignment が 64 である。6.6 と 4.20 は、配列の大きさは Max Scratchpad Buffers で決まると書く。
- 差：コメントは「248 バイト、64 バイトの境界」と読み、「Lo だけの大きさ」と説明する。Boundary の値が違う。「Lo だけ」は文書に無い。
- 筋書き：512 を超える entry の配列では、Table 6-1 の Boundary と 6.6 が合わない。コードの 2 頁の確保は 512 以下で PAGESIZE を跨がない。挙動には当たらない。
- 直す案：コメントは文書の値だけを書き、最大値の 1023 entry で表と節が合わないことを事実として書く。

### U11：`fails` のコメントが挙動と違う

- 場所：`kernel/src/dev/xhci.c` 179 行、577 行、1795 行。
- 差：コメントは「最後の良い転送からの失敗した回復の数で、worker のもの」と書く。実際は成功した回復も数え（1795 行）、drain も書く（577 行）。
- 筋書き：良い転送の無い回復が 8 回で端点は dead になる。コメントからはこの条件が読めない。
- 直す案：コメントを「良い転送の無い回復の数。8 で dead。drain が 0 に戻す」に直す。

### U12：高帯域の interrupt 端点で HID が ESIT あたり 1 packet に縮む

- 場所：`kernel/src/dev/usbhid.c` 651 行（`usb_interrupt_length(d, ep, 1)`）、`kernel/src/dev/xhci.c` 1184–1188 行。
- 仕様：xHCI 4.14.3 は、xHC は ESIT あたり TD を一つまでしか消費しないのがよいと書く。短い packet は IN の TD を終え、次の TD は次の ESIT に回る。4.14.2 は Max ESIT Payload を packet と burst の積で定める。
- 差：差分で HID の TD は 1 packet になった。端点が ESIT あたり 2 packet 以上を持てても、1 packet しか運ばない。予約した帯域の残りは使われない。
- 筋書き：`bInterval` が短く Mult か Max Burst が 1 以上の HID を差す。packet を跨ぐ report は packet の数と同じ ESIT を要する。端点が持てる量まで機器が送ると、入力が遅れる。
- 直す案：report の長さが全部同じで packet より長い端点だけ、TD を report の長さにする。その端点では TD が report のちょうどの長さで終わる。長さの違う report を持つ端点は 1 packet の TD のままにする。packet の倍数の短い report が長い TD に留まるからである。

### U13：interrupt 端点の Average TRB Length が実値から遠い

- 場所：`kernel/src/dev/xhci.c` 1072 行。
- 仕様：xHCI 4.14.1.1 は 1 KiB を初期値として妥当とし、周期の端点では値の正確さが大事だと書く。
- 差：今回の差分で TD は 1 packet（多くは 8〜64 バイト）になった。値は 1024 のままである。
- 筋書き：controller の帯域の計算が実際の量より多く見積もる。割り込み端点の多い hub の下で帯域の予約に効く。
- 直す案：端点を開くときの初期値を `MIN(max_packet, 1024)` にする。6.2.3 の Average TRB Length の下限は MBP の大きさで決まり、MBP の小さい interrupt 端点はこの値で下限に触れない。6.2.3.3 により DCI 2 以上は Evaluate Context で評価されないので、開いた後の更新は Configure Endpoint の Drop と Add になる。

### U14：`usb_control` の 4096 の上限が記述子を切る

- 場所：`kernel/src/dev/xhci.c` 917 行、1442 行、`kernel/src/dev/usbhid.c` 611 行、666 行。
- 仕様：USB 2.0 9.3.5 の wLength は 16 ビットである。
- 差：4096 を超える report 記述子は読まない。config 記述子は 4096 で切る。
- 筋書き：多くの interface を持つ複合機器の記述子が 4096 を超える。後ろの interface の端点が見えず、機器の一部が使えない。AGENTS.md の「資源を小さな固定の上限で拒まない」に当たる。
- 直す案：`xfer` を頁の数で確保する形にして 65535 バイトまで受ける。

### U15：`xhci_resume` が `running` を錠の外で書く

- 場所：`kernel/src/dev/xhci.c` 2173 行、245 行のコメント、299 行（`hc_set_running`）。
- 差：コメントは `running` をどの interrupter の錠の下でも書くと定める。`xhci_resume` は `hc_set_running` を通さず atomic で書く。
- 筋書き：resume の中で割り込みが閉じているので drain は同時に走らない。今の挙動には当たらない。コメントの約束が一か所で破れている。
- 直す案：`hc_set_running` を通すか、コメントに resume の例外を書く。

## 照らして合っていた手順

### 差分の中

- `usb_clear_halt` と `pipe_reset` の halted の順序：Reset Endpoint（TSP=0）、CLEAR_TT_BUFFER、CLEAR_FEATURE(ENDPOINT_HALT)、Set TR Dequeue、doorbell。4.6.8 の note のとおりである。control 端点は CLEAR_FEATURE を出さず、Set TR を必ず出す。
- 非 halted の pipe：Stop、Error なら Set TR、Configure Endpoint の Drop と Add。4.6.8 の最後の文、4.6.6、4.8.3 の遷移 6 のとおりである。Input Context の EP State は 0（6.2.3）で、DCS と TR Dequeue と Output からの写しも 6.2.3 に合う。
- `transfer_event` の `live` の判定：短い転送の二つの事象（4.10.1.1）、`got` の計算、Stopped の無視（4.6.9、Table 4-2）、空のリングの Stopped の行き先（4.17.4、4.6.9）。
- `endpoint_recover` から clear_device を外したこと：4.6.8 の note と合う。
- `open_endpoints` の burst は HS だけ（6.2.3.4、USB 2.0 9.6.6）。bulk OUT の Interval は NAK rate を 0〜15 に切る（6.2.3.6、USB 2.0 Table 9-13）。
- `configure` の失敗で `SLOT_CONFIGURED` のときだけ `deconfigure`：Addressed の slot に DC を出さない形が 4.6.6 に合う。
- `PORTLI`：5.4.10.1 のとおり offset は PORTSC+8、RLC は 19:16、0 起点、CCS=1 で有効である。`root_port` は PED の後で読む。
- `port_speed_rank`：PSI は全レーンの値（Table 7-13 の ID 6 は PSIM 10、ID 7 は PSIM 20）である。割り算と rank の順は USB 3.2 7.5.4.5.1 の Gen2x2、Gen2x1、Gen1x2、Gen1x1 と合う。
- HID の `hid_report`：HID 8.4（1 転送に 1 report、短い packet で終わる）に合う。`partial` の容量は longest+packet で溢れない。長さ 0 の packet は無視する。`want`=0 は来た分の全部である。recovery の後は捨てる。
- `values_in` と bits 0 の item の飛ばし：bits が 0 より大きいときだけ呼ばれ、size は 0 でない。Local の消去と Main、Global、Local の tag、7.2.2、7.2.4、7.2.6 の値が合う。
- `usb.h` の差分のコメント：`usb_interrupt_length` の説明は今の挙動と合う。

### 差分の周り

- 端点の状態：`endpoint_quiet` の Context State Error の後の Output の EP State の読み（4.6.9 の競りの段落、4.6.8）、EP 状態の値 0〜4（Table 6-8）、Halted と Error への遷移（4.8.3）、`state_after`（4.10.2）。
- TRB：TD Size（4.11.2.4）、Length 16:0、TD Size 21:17、Target 31:22、ISP 2、CH 4、IOC 5、IDT 6、DIR 16、TRT 17:16、制御転送の DIR と TRT（Table 4-7）、Setup の IDT、status の IOC、最初の TRB を最後に出す hold（4.11.7.1）、同じ Interrupter Target（4.11.7）。
- Slot Context：Table 6-4〜6-7、route の 15（脚注 106）、TT（Table 6-6）、TTT、Slot State 3 の Configured、Number of Ports。
- Endpoint Context：Table 6-8〜6-11、Max Burst（6.2.3.4）、MPS（6.2.3.5）、Interval（6.2.3.6、Table 6-12、脚注 113）、Mult と LEC（Table 6-8）、ESIT の Hi と Lo（6.2.3.8）、CErr=3（脚注 112）、Average の 8（Table 6-11）。
- コマンド：Configure と DC（4.6.6）、SET_CONFIG の順、hub の Default 経由（4.5.2 の note、4.6.11、6.2.2.4、6.2.3.7）、Reset Device の後の DCS=1（6.2.3.1）、Address Device の TT のクリア（4.6.5 の note）、Disable Slot の前提と DCBAA のクリア（4.6.4）、Evaluate（6.2.3.3）、コマンドの中止（4.6.1.2、5 秒）、CRCR、Command Ring Stopped。
- Clear TT Buffer：wValue（USB 2.0 Table 11-18）、wIndex=1（11.24.2.3）、11.17.5。多 TT の hub も protocol 1 の interface では単一 TT として動く（USB 2.0 11.23.1）。
- Protocol Capability：PSIE、PLT、LP（Table 7-10）、Hub Depth（Table 7-14、7-15）。HCSPARAMS2 の scratchpad（5.3.4）、確保の手順（4.20、6.6）。
- USBLEGCTLSTS：`(legacy & ~0xE011u) | 0xE0000000u` は enable の bits 0、4、13、14、15 を落とし、RW1C の bits 29、30、31 を消す。Table 7-5 と合う。OS の待ちの 1 秒は 4.22.1 と合う。
- USB 3.2 の hub：Get Port Status の型と長さ（Table 10-12）、dwExtPortStatus（Table 10-15）、feature selector（Table 10-9）、wPortChange（Table 10-14）、Set Hub Depth（10.16.2.9）、bPwrOn2PwrGood（2 ms 単位）、Get Hub Descriptor（10.16.2.3、USB 2.0 11.24.2.5）、Companion（9.6.7）。
- 時間：USB 2.0 の 9.2.6.1（5 秒）、9.2.6.3（2 ms）、TATTDB 100 ms、TRSTRCY 10 ms。xHCI の 16 ms、IMODI の単位。
- HID：7.2 の要求、boot 形式、HUT 10 の 1〜3、5.10 の Null、8.1、`report_bytes` に ID を含むこと。
- BOT：CBW と CSW の形、3.1、3.2、5.3.1（CBW の stall は Reset Recovery）、5.3.2、5.3.3.1、5.3.4 の順、6.3、residue、データの stall の後の clear と CSW。
- 錠の順：`ir->lock` → `ep->lock`（`fn` の前に放す）、`ir->lock` → `h->lock` → `hids_lock`、`in_mutex` → `cmd_mutex`、`ctl_mutex` → hub の `ctl_mutex`（子から親）。spinlock の下で眠らず、撃ち落としも無い。drain の中の `fn` と recover は 1 interrupter の上で直列である。

## 読み切れなかった範囲

- `kernel/src/dev/usbnet.c` は呼出しの行（277、292、293、300）だけを読んだ。`usb_open_endpoints` の「開いている端点はそのまま」が 4.6.6 の Drop と Add の要求に当たるかは、usbnet の alternate の中身を照らしていないので分からない。
- `pci_irq_enable`、MSI-X の設定、S3 の後の PCI の復元、input 層の錠は読んでいない。
- `mscboot.txt`、`mscoverview.txt`、`scsi.txt` は節ごとに一字ずつは照らしていない。CDB の形（INQUIRY、REQUEST SENSE、READ CAPACITY、READ と WRITE の 10 と 16、SYNC CACHE、START STOP、TEST UNIT READY）だけを照らした。
- `usbhid.c` の `usage_set1` の表は HUT と一字ずつは照らしていない。
- 実機と QEMU では動かしていない。これは依頼の範囲の外である。
