# USB と記憶の driver の修理の差分の監査（xHCI、hub、HID、BOT）

## 依頼文

あなたは Nocturne OS（独自の OS、カーネルは C、対象は実機）の敵対的な仕様の監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。コミットしない。サブエージェントを出さない。ダウンロードしない。返事も報告も日本語。

持ち主の言葉（そのまま）："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

作業木：D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64（ここを cwd に使う。`git -C` は使わない）。

対象：`git diff 545a102f HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usb.h kernel/src/dev/usbhub.c kernel/src/dev/usbhid.c kernel/src/dev/usbmsc.c` の差分と、HEAD のこれらのファイルの全文（kernel/src/dev/usbnet.c は呼ぶ側として読む）。

仕様：`D:\Programes\OS DEV INFO` の xHCI（extensible-host-controler-interface-usb-xhci.pdf）、USB 2.0（usb_20_20250603）、USB 3.2（usb_32_202206）、HID 1.11（hid1_11.pdf）、HUT、BOT（usbmassbulk_10.pdf）、MSC Overview、Seagate SCSI の手引き。pdftotext で起こした文字は C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\ にある（xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt、mscoverview.txt、scsi.txt）。表が崩れている所は PDF で確かめる。

仕事：変更された行ごとに原文で照らす。レジスターの順序と値、CRCR の扱い、端点の状態遷移とコマンドの順、Slot Context と Endpoint Context の項目の値、Transfer Event の解釈、Disable Slot の前提、hub の要求の値、速度と rank の読み方、HID の report の切り方、BOT の CSW の読み方。合格を探さない。仕様に合わないものと、変更で新しく壊れたもの（鎖の抜け、錠の順、spinlock の下の眠り、競り、記憶の漏れ、解放の後の使用、バッファの溢れ、完了の失い、止まらない待ち）を探す。カーネルの規則は `.claude/rules/kernel.md` と `AGENTS.md` にある。報告書や設計の文書は読まない（docs/ の監査の報告も読まない）。コードと仕様の原文だけで判断する。

各指摘に：ファイル:行、仕様の文書と節と原文の短い要旨（原文を長く写さない）、壊れ方の具体的な筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を一つ。調べて合っていた手順は何を照らしたかを一行で書く。読み切れなかった範囲はそう書く。

報告：`docs/records/kernel-core-20261101/audit-usbstorage-fix2-usb.md` へ日本語で全文（この依頼文を先頭に）。Python で書くなら `newline='\n'`。最後は SubagentHandback で、指摘の番号と一行の要旨を返す。

## 結論

照らした版は `545a102f` と `34ab1dbe`（HEAD）の差分である。
指摘は 11 件ある。
データを失うものは 0 件、当たれば壊れるものは 9 件、きれいさは 2 件である。
`545a102f` に無く、`34ab1dbe` で入った誤りは 1、2、5、6、8 である。
3、4、9、10 は `545a102f` にも同じ形であり、直っていない。
7 は `34ab1dbe` で足した検査が止められていない。
11 は `34ab1dbe` で書いた注である。
仕様との照合はコードと原文の読みだけで行った。ビルドと QEMU は動かしていない。

| 番号 | 場所 | 要旨 | 重さ |
|---|---|---|---|
| 1 | `usbhid.c:299-305` | 一つの転送の残りを次の report として切る | 当たれば壊れる |
| 2 | `xhci.c:1131` | HS bulk OUT の Interval に 16 以上の bInterval をそのまま入れる | 当たれば壊れる |
| 3 | `xhci.c:1207`、`1233-1238` | `usb_clear_halt` の失敗で pipe が halted に残らない | 当たれば壊れる |
| 4 | `xhci.c:856-863`、`1787` | `endpoint_recover` が Halted でない端点に CLEAR_FEATURE だけを送る | 当たれば壊れる |
| 5 | `xhci.c:1544-1554` | 表のある root port で Gen1x2 を Gen2x1 と読む | 当たれば壊れる |
| 6 | `xhci.c:523-528` | TD でない項目を指す誤りの event を捨てる | 当たれば壊れる |
| 7 | `usbhid.c:491-498` | Report Size 0 が REPORT_BITS_MOST の検査を通る | 当たれば壊れる |
| 8 | `usbhid.c:644-647`、`xhci.c:1182-1186` | ESIT が wMaxPacketSize より大きいと report が次の report まで届かない | 当たれば壊れる |
| 9 | `xhci.c:1112` | LS・FS の端点に bits 12:11 を Max Burst Size として入れる | 当たれば壊れる |
| 10 | `xhci.c:1409` | `configure` が Configure Endpoint を出していない slot に `deconfigure` を呼ぶ | きれいさ |
| 11 | `xhci.c:1992-1993` | 注が Table 6-1 を誤って引く | きれいさ |

## 指摘

### 1. 一つの転送の残りを次の report として切る

- 場所：`kernel/src/dev/usbhid.c:299-305`。注は `usbhid.c:61-63` と `288-290`。
- 仕様：HID 1.11 8.4。一つの USB 転送に入る report は一つだけである。report は複数の transaction にまたがってよい。一番長い report を除き、wMaxPacketSize を超える report は短い packet で終わる。
- 差：注の「several reports one (HID 8.4)」は 8.4 の逆を書いている。`while (done < have)` は同じ転送の残りを次の report として切り続ける。
- 筋書き：ID なしで 4 バイトと宣言し、wMaxPacketSize が 8 の装置が、毎回 8 バイトを送る。後ろの 4 バイトは 0 である。最初の 4 バイトの report の直後に、0 の report が届く。押したキーとボタンは直ちに放される。
- 筋書き：同じ装置が 5 バイトと宣言していると、3 バイトの詰め物が `partial` に残る。次の転送はその後ろに付く。以後の report は全部 3 バイトずれ、接続し直すまで戻らない。
- 比較：`545a102f` は宣言した位置だけを `bits_at` で読むので、後ろの詰め物で壊れない。
- 重さ：当たれば壊れる。
- 直す案：report の終わりが packet の途中なら、その packet の残りを捨てる。次の report を切るのは、report が packet の境で終わり、まだ続きの packet が届いている場合だけにする。

### 2. HS bulk OUT の Interval に bInterval をそのまま入れる

- 場所：`kernel/src/dev/xhci.c:1131`。`endpoint_context` の `c[0]`（`xhci.c:1066`）へ入る。
- 仕様：USB 2.0 Table 9-13 は HS bulk と control OUT の bInterval を 0〜255 とする。xHCI 6.2.3.6 の冒頭は Interval を 0〜15 とする。xHCI 6.2.3.2 は Configure Endpoint の Input Endpoint Context を、Interval が範囲内のときだけ有効とする。xHCI Table 6-91 の Completion Code 17 は、無効な Context の値を Parameter Error とする。
- 筋書き：bInterval が 16 以上の HS の記憶装置かネットの装置を挿す。Configure Endpoint が Parameter Error で失敗する。`open_endpoints` は失敗を返し、装置が使えない。
- 比較：`545a102f` はこの Interval を 0 のままにしたので、同じ装置で Configure Endpoint は失敗しなかった。
- 文書の中の差：6.2.3.6 の冒頭と Table 6-8 は `125 us * 2^Interval` と書く。同じ節の HS bulk の箇条は「Interval number of microframes」と書く。Table 6-12 に HS bulk の行は無い。0〜15 の範囲はどちらの読みでも満たす必要がある。
- 重さ：当たれば壊れる。
- 直す案：`ep->interval = MIN(e->interval, 15)` にする。

### 3. `usb_clear_halt` の失敗で pipe が halted に残らない

- 場所：`kernel/src/dev/xhci.c:1207`、`1233-1238`。注は `xhci.c:1197-1199`。呼ぶ側は `usbmsc.c:67-73`。
- 仕様：xHCI 4.6.8 の注。TSP 0 の Reset Endpoint の後に device 側の reset が成功しないと、toggle が揃わず動作は未定義になる。同じ節の最後の注は、Halted でない端点の host 側の reset を Configure Endpoint で行うとする。
- 差：`endpoint_restart(ep, ep->halted)` は入口の値を保つ。入口で false なら、Stop、Configure Endpoint、CLEAR_FEATURE のどれが失敗しても false のままである。注の「until both are done the pipe stays halted」と合わない。
- 筋書き：BOT の Reset Recovery が、halted でない bulk pipe に `usb_clear_halt` を呼ぶ。Configure Endpoint は通り、host の toggle は 0 になる。CLEAR_FEATURE は失敗する。`reset_recovery` は戻り値を見ない。次の `usb_bulk` は拒まれずに走る。host と device の toggle がずれ、device は最初の packet を重複として捨てる。CBW が失われ、CSW の待ちが時間切れになる。
- 重さ：当たれば壊れる。
- 直す案：入口で `ep->halted` を true にし、全部の手順が成功したときだけ false にする。

### 4. `endpoint_recover` が Halted でない端点に CLEAR_FEATURE だけを送る

- 場所：`kernel/src/dev/xhci.c:856-863`。呼ぶ側は `xhci.c:1787`（`hc_recover`）。
- 仕様：xHCI 4.6.8 の最後の注。Halted でない端点の toggle の reset は、Drop と Add を立てた Configure Endpoint で行う。Reset Endpoint を通らない端点では host の toggle は 0 にならない。
- 筋書き：TRB Error で端点が Error になり、`ep->halted` が立つ。`endpoint_quiet` は `*halted = false` で返す。`clear_device` は true なので CLEAR_FEATURE が送られる。device の toggle だけが 0 に戻り、host の toggle は残る。Reset Endpoint が Context State Error を返して Stopped と分かった場合も同じである。次の転送の packet は toggle の不一致で失われる。
- 比較：`usb_clear_halt` はこの場合に Configure Endpoint を通す。`hc_recover` の道だけが違う。
- 重さ：当たれば壊れる。
- 直す案：`hc_recover` の Halted でない場合も、`usb_clear_halt` と同じ手順の共通の関数へ通す。

### 5. 表のある root port で Gen1x2 を Gen2x1 と読む

- 場所：`kernel/src/dev/xhci.c:1544-1554`。
- 仕様：USB 3.2 7.5.4.5.1 は Gen1x1、Gen1x2、Gen2x1、Gen2x2 の順を定める。xHCI Table 7-13 で Gen1x2 と Gen2x1 の bit rate は同じ 10 Gb/s である。xHCI 5.4.10.1（Table 5-31）の PORTLI の RLC（bits 19:16、0 始まり）は、CCS=1 のとき受信の lane 数を示す。
- 差：注は「a listed one says its rate, which tells 20 Gb/s from less but not Gen2x1 from Gen1x2」と書く。PORTLI を読めば区別できる。
- 筋書き：PSIC が 0 でない root port に Gen1x2 の hub を挿す。hub の rank は 2 になる。その子の Gen1x2 の device の rank は 1 になる。`usb_attach_child` は親が上の rank と判断し、Slot Context に親の hub の slot と port を入れる。Table 6-6 では rank が同じなので 0 である。
- 重さ：当たれば壊れる。
- 直す案：`PORTSC + 8` の RLC を読み、`psi_rate / (RLC + 1)` の lane rate と lane 数から rank を出す。hub 側の `ss_speed` と同じ判定の形にする。

### 6. TD でない項目を指す誤りの event を捨てる

- 場所：`kernel/src/dev/xhci.c:523-528`。
- 仕様：xHCI 4.8.3。TRB Error の Transfer Event を受けたら端点は Error で、software は控えの状態を更新する。注の「a late event of a TD that is gone says nothing of the endpoint as it is now (4.8.3)」の裏づけは 4.8.3 に無い。
- 差：`live` は `m.td` が 0 の項目も偽にする。`m.td` が 0 なのは `ring_fit` の No Op と、Link TRB の位置である。これらの誤りは遅れた event ではなく、今の端点の状態を示す。
- 筋書き：stream の端点で、controller が Link TRB か No Op の位置を指す TRB Error か Transaction Error を報告する。event は捨てられ、`ep->state` は更新されず、`ep->halted` は立たず、`hc_recover` も走らない。端点は Error か Halted のまま止まり、stream は黙って途絶える。`545a102f` は門の前で `ep->state` を更新し、halted を立てて回復した。
- 条件：当たるのは TD の外を指す誤りの event だけである。
- 重さ：当たれば壊れる。
- 直す案：`m.td` が 0 でなく gen が違う event だけを捨てる。`m.td` が 0 の誤りの event は、今の端点の失敗として `state_after` と回復へ通す。

### 7. Report Size 0 が REPORT_BITS_MOST の検査を通る

- 場所：`kernel/src/dev/usbhid.c:491-498`。輪は `usbhid.c:153`、`164`、`176`、`383`、`427`。
- 仕様：HID 1.11 6.2.2.7 の Report Size と Report Count は report の項目の幅と数を定める。8.4 は項目が 4 バイトをまたげないとする。size 0 の項目の数に意味は無い。
- 筋書き：descriptor が Report Size 0 と Report Count 0xFFFFFFFF の配列の項目を持つ。`have + size * count` は 0 で検査を通る。`k.count` は 2^32 に近い値になる。`key_report` の輪は、割り込みを閉じた spinlock の下で report ごとに回る。その CPU は実質止まる。
- 筋書き：size 1 でも count は 524272 まで通る。report ごとに同じ輪が割り込みを閉じて回る。
- 重さ：当たれば壊れる（止まらない待ち）。
- 直す案：Input と Output の項目の検査で size が 0 で count が 0 でないものを拒み、`key_report` の輪を届いた report の長さで止める。

### 8. ESIT が wMaxPacketSize より大きいと report が次の report まで届かない

- 場所：`kernel/src/dev/usbhid.c:644-647`、`kernel/src/dev/xhci.c:1182-1186`。
- 仕様：HID 1.11 8.4。一番長い report は短い packet で終わらなくてよい。wMaxPacketSize を超えない report も短い packet で終わらなくてよい。xHCI 4.14.3。短い packet が interrupt IN の TD を終える。TD の大きさは Max ESIT Payload を超えてよく、software が決める。
- 差：新しい版の TD は `usb_interrupt_length(d, ep, 0)`、つまり ESIT の全部である。
- 筋書き：HS の high-bandwidth か、bMaxBurst が 0 でない SS の HID 装置で、ESIT が wMaxPacketSize の 2 倍とする。device が wMaxPacketSize ちょうどの report を送る。短い packet は来ない。TD は終わらず、`hid_report` は呼ばれない。次の report が届いて初めて、二つが一度に渡される。キーを押した事象は放すまで届かない。
- 比較：`545a102f` の TD は `longest` を packet の単位に切り上げた長さで、一番長い report では遅れなかった。wMaxPacketSize ちょうどの短い方の report では、前の版にも同じ遅れがある。
- 重さ：当たれば壊れる。
- 直す案：HID の TD を wMaxPacketSize にする。report は `partial` で packet ごとに組み、指摘 1 の切り方と合わせる。

### 9. LS・FS の端点に bits 12:11 を Max Burst Size として入れる

- 場所：`kernel/src/dev/xhci.c:1112`。`esit`（`xhci.c:1113`）と `usb_interrupt_length` に波及する。差分の変更行ではない。
- 仕様：xHCI 6.2.3.4。LS・FS の端点では Max Burst Size を 0 にする。HS の isochronous と interrupt だけが wMaxPacketSize の bits 12:11 を入れる。USB 2.0 Table 9-13 でも bits 12:11 に意味があるのは HS だけである。
- 筋書き：FS の HID 装置が予約の bits 12:11 を立てている。Max Burst Size が 0 でなくなり、6.2.3.2 の有効の条件から外れて Configure Endpoint が Parameter Error になる。通る controller でも ESIT の値が実際の倍になる。
- 重さ：当たれば壊れる。
- 直す案：`xd->dev.speed == USB_SPEED_HIGH` のときだけ bits 12:11 を読む。

### 10. `configure` が Configure Endpoint を出していない slot に `deconfigure` を呼ぶ

- 場所：`kernel/src/dev/xhci.c:1409`。
- 仕様：xHCI 4.6.6。Deconfigure（DC=1）は slot が Configured のときの手順である。
- 筋書き：端点の数 `n` は 0 でないが、全部の wMaxPacketSize が 0 で `added` が 0 になる。SET_CONFIG が失敗すると `deconfigure` が走り、Context State Error になる。slot は Addressed のままで、実害は余分なコマンドが一つである。
- 重さ：きれいさ。
- 直す案：`open_endpoints` が Configure Endpoint を実際に出したかを `configure` へ返し、それで判断する。

### 11. 注が Table 6-1 を誤って引く

- 場所：`kernel/src/dev/xhci.c:1992-1993`。
- 仕様：xHCI Table 6-1 の Scratchpad Buffer Array の Max Size は 248 バイトである。5.3.4 と 6.6 は配列の大きさを Max Scratchpad Buffers の項目数とし、4.20 も同じである。この項目は Hi と Lo で 10 bit あるので、最大は 1023 項目（8184 バイト）である。
- 差：注は「two pages aligned as a pair hold (Table 6-1)」と書く。Table 6-1 はそれを言わない。文書の中で Table 6-1 と 5.3.4 は食い違う。
- 動作：`dma_block(2, ...)` は 8 KiB の境に揃う。31 項目以下は最初の頁に収まり、Table 6-1 を満たす。動作は 5.3.4 も満たす。
- 重さ：きれいさ。
- 直す案：注を「項目数は 5.3.4 の Hi と Lo。Table 6-1 の 248 は Lo だけの 31 項目の大きさで、5.3.4 に従って 1023 項目を持てる 2 頁を取る」に書き直す。

## 調べて合っていた手順

- CRCR の中断（Table 5-24、4.6.1.1、4.6.1.2）：bits 5:4 の保持、CRR=1 のときだけ CA を書く、5 秒の待ち、書かなければ Dequeue Pointer から再開する。
- 短い転送（4.10.1.1）：ISP だけの TRB で二つの event、完了は最後の TRB の event、バイト数の式。
- Disable Slot の前提（4.6.4 の注）：`slot_quiet` の Stop、Reset、Set TR Dequeue、drain、完了の後の DCBAA の 0。
- Address Device の注（4.6.5）：Transaction Error で CLEAR_TT_BUFFER、Disable Slot で回復。
- hub の項目の変更（4.5.2 の注）：Default 状態を通す手順と `device_readdress`。
- DCS=1（6.2.3.1）と `ring_reset`。Reset Device（4.6.11、6.2.3.7）は Addressed か Configured で通ること。
- 端点の状態（4.6.8、4.6.9、4.8.3、Figure 4-5）：Stop は Running のみ、Halted と Error では Context State Error、状態の控えの更新。
- pipe の reset の順（4.6.8 の注）：Reset Endpoint、CLEAR_TT_BUFFER、CLEAR_FEATURE、Set TR Dequeue。
- Set TR Dequeue（4.6.10）：Stopped か Error のときだけ通り、`endpoint_quiet` の戻りの条件と合う。
- Stop の後に TT の clear が要らないこと（4.6.9 の注）。Stopped の event を捨てること（4.6.9）。
- Configure Endpoint の旗（4.6.6）：A1・D0・D1=0、A0=1、Drop と Add の Stopped からの遷移、`deconfigure` の DC=1。
- Mult、LEC、Max ESIT（Table 6-8、6.2.3.8、4.14.2）、HCCPARAMS2 bit 4。
- dwBytesPerInterval の offset 4（USB 3.2 Table 9-29）、companion の bit 7（9.6.7）。
- Interval の計算（Table 6-12、`endpoint_interval`）。
- Average TRB Length（4.14.1.1）：control 8、interrupt 1 KiB、bulk と isochronous 3 KiB。
- Route の 15（Table 6-4 の脚注）、TTT（Table 6-6）。
- rank の順（USB 3.2 7.5.4.5.1）、Table 6-6 の例と `usb_attach_child` の条件。
- hub の要求（USB 3.2 Table 10-12、Table 10-15、9.6.2.5）：wValue 2、長さ 8、`st[4] & 15`、`st[5] & 15`、Sublink の LSE・LP・LSM。
- MHD（xHCI Table 7-14、USB2 でも bits 27:25）。
- PSI（7.2.1）：PSIV、PSIE、PLT、LP、PSIM と `psi_rate`、`speed_kind_of_psi`。
- Clear_TT_Buffer の wValue と no-op（USB 2.0 11.24.2.3、11.17.5、Table 11-18）。
- status change の再報告（USB 2.0 11.12.4）と `hub_report` の注。
- 制御の TD（4.11.2.2、4.11.7）：Setup、Data、Status は別の TD で Link が間に入ってよいこと、No Op の前の Chain=0。
- BOT（5.3.4、6.3.1、6.3.2、6.5、6.7.2）：CSW の有効と意味の判定、STALL の後の処理、residue と moved の小さい方、Reset Recovery の順。
- SCSI の変更行：READ CAPACITY(10)・(16)、START STOP UNIT、REQUEST SENSE の固定形式の offset。
- Scratchpad（4.20）：項目数、PAGESIZE の境、0 埋め、DCBAA の項目 0。HCCPARAMS1 の AC64 が 0 の controller は `xhci.c:1929` で拒む。
- `hid_report` の `partial` の大きさ：`longest + transfer` で溢れない。
- `hc_set_running` の後の store を消したこと：`nintr` が 0 のとき running はもう false である。
- 錠の順：`in_mutex` → `cmd_mutex`、`address_device` の `tt_clear` は unlock の後。
- `hc_recover` の NULL の報告：`usbnet` の `rx` と `notify`、`hub_report`、`hid_report` のどれも安全で、spinlock の外で呼ばれる。
- `usbmsc_detach` の `blk.gone`：`blk.h` の定義と `pm.c:126` の読み手に合い、錠の下で立てる。
- 書きの完了：`transfer` は `r == bytes` のときだけ 0 を返し、短い書きを成功にしない。

## 文書の中の矛盾（指摘でなく記録）

- xHCI Table 6-4 は Speed を deprecated かつ Reserved とする。4.5.2 と 6.2.2.1 は値を要求する。コードは 4.5.2 に従う。
- xHCI 6.2.2.1 の (5) は LS・FS 以外で parent の項目を 0 とする。Table 6-6 は SS・SSP の rank で値を入れるとする。コードは Table 6-6 に従う。
- xHCI 6.2.3.6 の HS bulk の Interval の単位は、指摘 2 に書いたとおり節の中で食い違う。
- xHCI Table 6-1 と 5.3.4 の Scratchpad Buffer Array の大きさは、指摘 11 に書いたとおり食い違う。

## 読み切れなかった範囲

- HUT、MSC Overview、Seagate SCSI の手引きは、変更行に関わる所だけを読んだ。全文は照らしていない。
- Table 6-1 は PDF の頁をまたいで文字の層が崩れている。pdftotext の `-layout` と `-raw` で読んだ行の並びから Scratchpad の行を読み取った。画像としては見ていない。
- `arch/spin.h` の rank の検査と、`hid_report` や `usbnet` の report 関数の中の錠の rank の整合は読んでいない。
- `usbnet.c` は呼ぶ側として `rx` と `notify` を読み、装置の手順は照らしていない。
- 差分の外の `dev/pci.c`、MSI の割り当て、電源の Save と Restore の手順（4.23.2）は照らしていない。
- 実機、QEMU、Hyper-V での動作は確かめていない。依頼が禁じている。
