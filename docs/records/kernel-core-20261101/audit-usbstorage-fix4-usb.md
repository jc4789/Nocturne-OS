# USB の driver の差分の敵対的な監査（34ab1dbe..883c546d）

## 依頼文

Nocturne OS のカーネルの USB の driver の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 34ab1dbe HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhid.c kernel/src/dev/usb.h kernel/src/dev/usbmsc.c`。必要なら `git diff 55142942 HEAD` の全部も。
- 今のコード：`kernel/src/dev/xhci.c`、`usbhid.c`、`usbhub.c`、`usbmsc.c`、`usb.h`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt、mscboot.txt、mscoverview.txt、scsi.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順を調べます。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix4-usb.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

監査した版は 883c546d である。

指摘は 9 件である。

| 重さ | 数 | 指摘 |
|---|---|---|
| データを失う | 0 | なし |
| 当たれば壊れる | 6 | F1、F2、F3、F4、F8、F9 |
| きれいさ | 3 | F5、F6、F7 |

動作は確かめていない。依頼がビルドと QEMU を禁じている。

## 指摘

### F1：USB3 の機器を抜いた瞬間を、動けない port と読む

- 場所：`kernel/src/dev/xhci.c:1765-1767`（`root_port`）。
- 重さ：当たれば壊れる。壊れ方は worker の遅れである。
- 仕様の節：xHCI 1.2 の Table 5-27 の PLC の脚注 94（xhci.txt 20277-20280）。4.19.1.2.5、4.19.1.2.6。
- 文書の要旨：U0 の間に機器が抜けると、PLS は U0、Recovery、Inactive、RxDetect の順に移る。PLC が立つのは、機器が本当に Inactive に入ったときだけである。
- 差：コードは PLS が Inactive であれば `stuck` とする。PLC も CCS も見ない。抜く途中の Inactive と、本当の Inactive を区別していない。
- コメントの誤り：「only a reset takes it out」は誤りである。Error の状態は Disconnect Detect でも出る（4.19.1.2.6）。
- 当たる筋書き：
  1. USB3 の機器を抜くと、Port Status Change の事象で `root_port` が走る。
  2. その読みが PLS=Inactive、CCS=0 に当たると、`stuck` が真になる。
  3. 1771 行より先へ進み、`link_trained` が PED を最大 1 秒待つ。
  4. 続いて `port_reset(…, true)` が warm reset をかける。これは最大 500 ms と回復の 10 ms を使う。
  5. その間、worker は止まる。ほかの port の列挙と、キーボードの LED の仕事が最大約 1.5 秒遅れる。
- 文書との差の別の面：Disconnected の port への warm reset は、4.19.1.2.5 の Error の遷移の対象ではない。
- 直す案：Inactive は、その読みで PLC（bit 22）が立っているときだけ `stuck` とする。PLC は 1757 行で消す前の `sc` に残っている。コメントも 4.19.1.2.6 に合わせて直す。

### F2：Compliance に入った port を見に来ない

- 場所：`kernel/src/dev/xhci.c:1767`、`kernel/src/dev/xhci.c:2143-2175`（`hc_worker`）、`kernel/src/dev/xhci.c:2074-2075`（`hc_start`）。
- 重さ：当たれば壊れる。当たるのは HCCPARAMS2 の CTC が 0 の controller である。
- 仕様の節：xHCI 1.2 の 4.19.4 の PP の項（xhci.txt 15766-15767）、4.19.1.2.4.1（xhci.txt 15013-15024）、5.3.9 の CTC（xhci.txt 18962-18971）。
- 文書の要旨：最初の LFPS の handshake が timeout すると、port は Compliance に移り、change の旗は立たない。CTC が 1 なら、ソフトウェアが CTE を立てない限りこの遷移は起きない。CTC が 0 なら自動で起きる。
- コードの扱い：CTE を立てる書込みはコードに無い。よって当たるのは CTC が 0 の controller だけである。
- 差：`root_port` は Compliance を `stuck` に数えて warm reset で救う。しかし事象が無いので、`root_port` は走らない。全部の port を見るのは `hc_start` の一回だけである。worker の 1 秒の拍は USBSTS だけを見て、PORTSC を見ない。
- 当たる筋書き：CTC が 0 の controller で、起動の後に差した機器の LFPS が timeout する。port は Compliance に入ったまま事象を出さない。機器は抜いて差し直すまで列挙されない。
- 直す案：worker の 1 秒の拍で、機器の無い USB3 port の PLS を読む。Compliance なら `port_pending` に立てる。

### F3：warm reset の回数の上限が効かない

- 場所：`kernel/src/dev/xhci.c:1766`、`kernel/src/dev/xhci.c:1768-1769`、`kernel/src/dev/xhci.c:265`。
- 重さ：当たれば壊れる。壊れ方は worker の占有である。
- 仕様の節：xHCI 1.2 の Figure 4-27、4.19.1.2.5。USB 3.2 の RxDetect と Polling の遷移。文書は回数を定めない。主張はコードのコメントの「a few tries」である。
- 差：`port_warms` は、`stuck` でない読みが一度でもあると 0 に戻る。warm reset の後の port は Reset、RxDetect、Polling を通るので、その読みは `stuck` でない。よって連続の数が 3 を超えない。
- 当たる筋書き：壊れた機器かケーブルで、次の輪が続く。
  1. Inactive に入り、PLC の事象が来る。
  2. `root_port` が warm reset をかける。
  3. Reset の完了の事象で `root_port` が走り、Polling などを読む。`port_warms` が 0 に戻る。
  4. link が再び Inactive に落ちる。
- 一周は warm reset だけで少なくとも約 510 ms である。worker はその間ほかの port と機器の仕事をしない。輪に止まる条件は無い。
- 別の抜け：`port_warms` は `uint8_t` である。上限を超えた後も `++` は続く。256 回目の `stuck` の読みで 0 に戻り、warm reset が再び始まる。
- 直す案：数を 0 に戻すのは、列挙が成功したときか、機器が抜けて CCS=0 かつ PLS=RxDetect を読んだときだけにする。数は上限で止める。

### F4：hub の後ろの Gen1x2 と Gen2x1 を区別しない

- 場所：`kernel/src/dev/xhci.c:1553-1564`（`port_speed_id`）。
- 重さ：当たれば壊れる。実機の controller がこの欄をどう使うかは、文書から分からない。
- 仕様の節：xHCI 1.2 の Table 7-13（既定の PSI の ID 5 は Gen2x1、ID 6 は Gen1x2、共に PSIM 10）、7.2.1、Table 6-4（Speed の欄）、4.5.2、6.2.2.1。USB 3.2 の 7.5.4.5.1（rank の順）。
- 文書の要旨：Gen2x1 と Gen1x2 は全 lane の率が共に 10 Gb/s である。PSI の Dword に lane の数の欄は無い。Table 6-4 は Speed を deprecated と書く一方で、4.5.2 と 6.2.2.1 は設定を求める。
- 差：コードは rank 1 も rank 2 も率 10 Gb/s で探し、最初に載った PSI を返す。
- 当たる筋書き：USB 3.2 の controller で、Gen2x1 の hub の後ろに Gen1x2 の機器がある。rank は 1 であるが、ID 5 が先に載っていれば Slot Context の Speed は Gen2x1 の 5 になる。
- 直す案：同じ率の PSI が複数あれば、rank の既定の ID（rank 1 は 6、rank 2 は 5）が載っていればそれを優先する。

### F5：64 KiB の規則の節番号が違う

- 場所：`kernel/src/dev/xhci.c:923-925`。
- 重さ：きれいさ。
- 仕様の節：xHCI 1.2 の 6.4.1（xhci.txt 22948）。
- 差：コメントは「so one TRB may cover it (4.11.7.1)」と書く。TRB が 64 KiB の境界をまたがない規則は 6.4.1 にある。4.11.7.1 に 64 KiB の規則は無い。
- 当たる筋書き：コードの動作には影響しない。次の読み手が違う節を開く。
- 直す案：コメントの節番号を 6.4.1 に直す。

### F6：IMODI のコメントが今の HID の TD と合わない

- 場所：`kernel/src/dev/xhci.c:114-117`。
- 重さ：きれいさ。
- 仕様の節：xHCI 1.2 の 4.14.2（ESIT）、6.2.3.4（Max Burst Size）、HID 1.11 の 8.4。
- 差：コメントは「each of those completes at most once per its own service interval」と書く。今の HID の TD は一つのパケットごとである。HS の高帯域と SS の burst では、一つの service interval に複数の完了がある。
- 当たる筋書き：間引きを持たない判断の根拠が誤っている。interrupt の endpoint の完了の数は、service interval あたりのパケットの数まで増える。上限は Max Burst と Mult で決まるので、嵐にはならない。
- 直す案：「each of those completes at most once per packet of its own service interval」の意味に直す。

### F7：usb_bulk の約束が timeout で成り立たない

- 場所：`kernel/src/dev/usb.h:100-102`、`kernel/src/dev/xhci.c:1289-1295`、`kernel/src/dev/xhci.c:521`。
- 重さ：きれいさ。
- 仕様の節：xHCI 1.2 の 4.6.9（Stopped の事象の Transfer Length が残りを示す）。
- 差：`usb.h` は「*moved, in each case, is the bytes that did move」と書く。timeout の道の `*moved` は 0 のままである。xhci.c:521 は Stopped の事象を捨てるので、動いた数を知らない。
- 当たる筋書き：timeout の転送が途中まで動いても、`*moved` は 0 を返す。今の呼ぶ側の BOT は timeout を broken として Reset Recovery へ進むので、害は無い。次の呼ぶ側は約束を信じて誤る。
- 直す案：コメントに timeout を除くと書く。または Stopped の事象から動いた数を数えて返す。

### F8：短いレポートで、マウスの値を 0 と読む

- 場所：`kernel/src/dev/usbhid.c:245-280`（`mouse_report`）、`kernel/src/dev/usbhid.c:121-125`（`values_in`）、`kernel/src/dev/usbhid.c:308`（`hid_report`）。
- 重さ：当たれば壊れる。当たるのは、宣言より短いレポートを送る機器である。
- 仕様の節：HID 1.11 の 8.4、6.2.2.7。
- 差：`hid_report` は `len < h->packet` で短いレポートを一つの報告として渡す。`key_report` は `values_in` で長さの外の値を読まない。`mouse_report` は同じ守りを持たない。範囲の外のボタンは 0 に読まれて離される。範囲の外の座標も 0 に読まれる。
- 当たる筋書き：絶対座標の機器が X だけを含む短いレポートを送ると、`in_range` が 0 を受け入れる場合に Y が 0 に飛ぶ。ボタンは押したまま離れたことになる。
- 直す案：長さの外の欄を「持たない」と扱う。ボタンは `h->buttons` の値を保ち、絶対座標は `last_x`・`last_y` を保ち、相対座標と wheel は 0 にする。

### F9：Stop で中断した split の後に TT の buffer を clear しない

- 場所：`kernel/src/dev/xhci.c:863-868`（`endpoint_recover`）、`kernel/src/dev/xhci.c:1229-1247`（`pipe_reset` の halted でない道）。
- 重さ：当たれば壊れる。判定は PLAUSIBLE である。
- 仕様の節：xHCI 1.2 の 4.6.8 の note、4.6.5 の note（xhci.txt 5295-5318）。USB 2.0 の 11.17.5（usb20.txt 19714-19730）、Table 11-18。
- 文書の要旨：bulk と control の split が失敗すると、TT の buffer は busy のまま残ることがある。halt の処理の一部として Clear_TT_Buffer を送る。
- 差：コードが `tt_clear` を呼ぶのは、endpoint が halted のときだけである。timeout を Stop で中断した道は halted でないので呼ばない。
- 当たる筋書き：HS の hub の後ろの FS の記憶装置で bulk が timeout する。Stop で TD を飛ばす。TT の buffer が busy のまま残ると、その後の bulk が通らない。
- 分からない点：文書は halt の処理としか書かず、host が中断した場合を書いていない。
- 直す案：`endpoint_recover` と `pipe_reset` の halted でない道でも、TT の後ろの control と bulk なら `tt_clear` を呼ぶ。

## 照らして合っていた手順

節を開いて確かめた。

### pipe の回復

- `pipe_reset` の順は、Reset Endpoint（TSP=0）、CLEAR_TT_BUFFER、CLEAR_FEATURE(ENDPOINT_HALT)、Set TR Dequeue である。4.6.8 の note と一致する。
- 非 halted の Drop+Add は、4.6.8 の最後の note と 4.6.6（Stopped か Running の idle）に一致する。
- Add の入力の EP State=0 は 6.2.3.2 に一致する。D0・D1・A1 は 0 で、A0 は 1 で、DCI+1 の添字も正しい。
- Configure の後の Running は 4.8.3 に一致する。
- Set TR Dequeue は Error と Stopped で許される（4.6.10）。
- `endpoint_quiet` は、Stop が Context State Error なら出力の EP State を読む。4.6.9（xhci.txt 6470-6480）に一致する。EP State の値は Table 6-8 に一致する。

### control 転送

- control の回復は 4.6.8 の note（Reset の後に Set TR Dequeue）と 4.11.2.2 の timeout と Stop に一致する。
- 方向は Table 4-7 に一致する。TRT、IDT、長さ 8、Average 8（Table 6-11）も合う。
- 長い control の `dma_block` の 16 頁は、pmm の約束（数以上の 2 の冪に整列）で 64 KiB の境界をまたがない。
- Data TRB の長さは 1〜64K である。wLength は USB 2.0 の 9.3.5 に一致する。
- `*over` の扱いは、halted なら DMA が止まる点で正しい。

### 事象と slot

- `transfer_event` の `!live` の濾しと `m.td==0` の道は、b を添字に使わない。
- 短い転送の二つの事象は 4.10.1.1.2 に一致する。
- `slot_quiet` は 4.6.4 の note、Table 6-7 の Slot State 0、4.6.9 に一致する。
- `SLOT_CONFIGURED`=3 は Table 6-7 に一致する。DC は 4.6.6 に一致する。

### endpoint context

- Average TRB Length は 4.14.1.1 と Table 6-11 に一致する。TD Fragment の下限 65 B は MBP 16K の例で、mps 以上の TD に当たらない。
- burst は 6.2.3.4 に一致する。
- bulk OUT の Interval は 6.2.3.6（0〜15、値の数の microframe）と USB 2.0 の Table 9-13 に一致する。
- Mult は Table 6-8 に一致する。

### port と速さ

- PORTLI は 5.4.10（オフセット 408h）で、RLC は 19:16、0 始まり、CCS=1 で有効である。使う時は CCS と PED を確かめた後である。
- PSI の率は全 lane の率（Table 7-13 の PSIM と 7.2.1）で、rank の計算は正しい。
- rank の順は USB 3.2 の 7.5.4.5.1 に一致する。
- 親の hub の条件は Table 6-6 に一致する。
- MHD の 27:25 は Table 7-14・7-15 に一致する。
- USB3 の Error と Compliance で CCS=0 であることは Figure 4-27 に一致する。WPR で Reset へ移ることは 4.19.1.2.5、4.19.1.2.7 に一致する。WRC と PRC は 4.19.5.1 に一致する。

### 起動の手順

- BIOS handoff の bit の位置は Table 7-4 に一致する。byte の書込みは 5.1.1 の note と 7.1.1 で許される。手順は 4.22.1 に一致する。
- USBLEGCTLSTS のマスクは Table 7-5 に一致する。
- Scratchpad は 4.20、6.6、Table 6-1（248、PAGESIZE）とコメントのとおりである。

### No Op

- 6.4.1.4 と 4.11.7（Chain 0、前の TRB の Chain 0）に一致する。
- 連続の No Op は「避ける」とある。64 KiB 以下の TD では実質一個なので当たらない。

### HID

- 8.4（一つの transfer に一つの report、短いパケットの終端、一番長い report は終端不要）に一致する。ZLP は have=0 で無視する。
- `partial` の上限は、`partial_len` < want ≤ longest、len ≤ packet で成り立つ。
- `values_in` の size≠0 は bits≠0 の条件で保証される。
- 6.2.2.7 の Report Count に一致する。
- SET_PROTOCOL、SET_IDLE、SET_REPORT の値は合う。
- `usbhid_detach` の repeat_usage=0 から alarm_cancel の順は、alarm.h の約束と `h->lock` を放した後の呼出しで正しい。

### BOT

- Figure 2 に「STALL Bulk-In - OR - Bulk Error」があり、変更は一致する。
- 5.3.1 の CBW の STALL、5.3.4 の順、6.5 に一致する。
- timeout は文書に無く、Reset Recovery へ行く。

### hub（差分の外）

- Table 10-15 の bit と 0 始まりの lane は合う。
- PORT_ENABLE の条件は合う。
- Table 6-4 の脚注 106 の route の 15、TTT は合う。
- Table 11-18 の wValue と、CLEAR_TT_BUFFER の wIndex は合う。

### 錠と文脈

- `pipe_reset` は `in_mutex` → `cmd_mutex` の順で、`usb_control` を `in_mutex` の外で呼ぶ。
- `dma_block` と `dma_free` は錠の外である。
- `hid_report` の `caps_toggled` は `h->lock` の外で扱う。
- `fails` は halted の間は worker だけが触る。

## 読み切れなかった範囲

- `usbnet.c` は呼ぶ側を grep しただけである。
- 55142942..34ab1dbe の差分は、今のコードとして読んだ。一行ずつの照合はしていない。
- scsi.txt、mscboot.txt、mscoverview.txt、hut.txt は開いていない。差分に SCSI の変更は無い。
- 実機の xHC が Slot Context の Speed の欄をどう扱うかは、文書から分からない。
- 動作は確かめていない。ビルドも QEMU も動かしていない。
