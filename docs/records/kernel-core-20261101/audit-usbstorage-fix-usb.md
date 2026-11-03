# USB と記憶の修理の差分：公式の文書との敵対的な照合

## 依頼文

あなたは Nocturne OS（独自の OS、カーネルは C、対象は実機）の敵対的な仕様の監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。コミットしない。サブエージェントを出さない。

持ち主の言葉（そのまま）："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

作業木：D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64（ここを cwd に使う。`git -C` は使わない）。

対象：`git diff main HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usb.h kernel/src/dev/usbhub.c kernel/src/dev/usbhid.c kernel/src/dev/usbmsc.c` の差分と、変更後のこれらのファイル全文（usbnet.c は呼ぶ側として読む）。

仕様：`D:\Programes\OS DEV INFO` の xHCI（extensible-host-controler-interface-usb-xhci.pdf）、USB 2.0（usb_20_20250603）、USB 3.2（usb_32_202206）、HID 1.11（hid1_11.pdf）、HUT（hut1_12v2.pdf）、BOT（usbmassbulk_10.pdf）、MSC Overview、MSC Boot（usb_msc_boot_1.0.pdf）、Seagate SCSI の手引き。pdftotext で起こした文字は C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\ にある（xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt、mscoverview.txt、mscboot.txt、scsi.txt）。表が崩れている所は PDF で確かめる。

仕事：レジスターの順序と必須の手順、ビットの位置と意味、Input Context・Slot Context・Endpoint Context の各項目の値、コマンドの順、端点の状態遷移、reset a pipe の手順、Cycle の順と TD の規則、hub の要求の値、HID の parser の規則、BOT の手順と SCSI の CDB を、変更された行ごとに原文で照らす。合格を探さない。仕様に合わないものと、変更で新しく壊れたもの（鎖の抜け、錠の順、競り、記憶の漏れ、解放の後の使用）を探す。カーネルの規則は `.claude/rules/kernel.md` と `AGENTS.md` にある。報告書や設計の文書は読まない。コードと仕様の原文だけで判断する。

各指摘に：ファイル:行、仕様の文書と節と原文の短い要旨（原文を長く写さない）、壊れ方の具体的な筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を一つ。調べて合っていた手順は何を照らしたかを一行で書く。読み切れなかった範囲はそう書く。

報告：`docs/records/kernel-core-20261101/audit-usbstorage-fix-usb.md` へ日本語で全文（この依頼文を先頭に）。Python で書くなら `newline='\n'`。返事は SubagentHandback で、指摘の番号と一行の要旨を返す。

## 結論

対象は作業木の HEAD `545a102f`。行番号はこの版のもの。指摘は 19 件。

- データを失う：0 件。
- 当たれば壊れる：13 件（F1、F2、F3、F4、F6、F7、F8、F9、F11、F12、F14、F16、F17）。
- きれいさ：6 件（F5、F10、F13、F15、F18、F19）。

根拠はコードと仕様の原文の読み合わせだけ。ビルドも実行もしていない。

| 番号 | 場所 | 要旨 | 重さ |
|---|---|---|---|
| F1 | xhci.c:716 | Command Abort の CRCR の書き込みが RsvdP を汚し、CRR=0 なら切り詰めた番地で再開する | 当たれば壊れる |
| F2 | xhci.c:799-800、823、832、1467 | 制御転送の Clear_TT_Buffer がいつも OUT を指す | 当たれば壊れる |
| F3 | xhci.c:1533-1536 | SS・SSP の上位ランクのハブの後ろで Parent Hub の項目を入れない | 当たれば壊れる |
| F4 | xhci.c:505、532-538、783 | 遅れた event が追跡中の状態を HALTED に戻し、回復が失敗する | 当たれば壊れる |
| F5 | xhci.c:540-548 | Short の最初の event で TD を完了にする | きれいさ |
| F6 | xhci.c:1096-1106、usbhid.c:481-482、591 | HID の TD の長さが TLC をまたぐ longest で、報告が混ざる | 当たれば壊れる |
| F7 | usbmsc.c:113-114 | 有効で意味のある CSW に Reset Recovery をする | 当たれば壊れる |
| F8 | xhci.c:1298-1299 | deconfigure の失敗でもリングを解放する | 当たれば壊れる |
| F9 | xhci.c:1019、1124 | Input Context に持ち主がいない | 当たれば壊れる |
| F10 | xhci.c:850 | 制御リングに No Op を詰め、Dequeue が Setup TD を指さない | きれいさ |
| F11 | xhci.c:1120-1122 | TRB Error の後に host 側の toggle を消さない | 当たれば壊れる |
| F12 | xhci.c:994、1042-1045、982-985 | isochronous の LEC と SSP の記述子を読まない | 当たれば壊れる |
| F13 | xhci.c:994、1054 | HS bulk OUT の Interval がいつも 0 | きれいさ |
| F14 | xhci.c:1375、1398-1401 | readdress の DCS と CLEAR_TT_BUFFER の抜け | 当たれば壊れる |
| F15 | usbhub.c:145、xhci.c:747 | FS ハブに TTT を入れる | きれいさ |
| F16 | xhci.c:950-953 | 転送の残る EP のまま Disable Slot をする | 当たれば壊れる |
| F17 | xhci.c:1728、1744、1854、1862-1866 | 確保失敗の後の再起動で NULL を踏み、記憶を漏らす | 当たれば壊れる |
| F18 | xhci.c:292、1000、usbhub.c:187、usbhid.c:351、396、usbmsc.c | 細かな不一致の組 | きれいさ |
| F19 | xhci.c:1128 | Output の RsvdO を Input Context へ写す | きれいさ |

## 指摘

### F1. Command Abort の CRCR の書き込み

- 場所：`kernel/src/dev/xhci.c:716`（注は 709-712）。
- 仕様：xHCI 5.4.5 Table 5-24。Pointer は 63:6 で、5:4 は RsvdP。CRR=0 のとき CA は無視される。CRR=1 のとき Pointer と RCS は無視される。4.6.1.2 は CRR の負を時間で見ると書く。
- 欠陥：`wr64(CRCR, (phys + enq*16) | cycle | CA)` は enq が 4 の倍数でないとき bit 5:4 に非 0 を書く。注の「止まっていれば、そこから続く」は誤りである。Pointer は 64 バイト単位でしか表せない。
- 筋書き：コマンドのタイムアウトと xHC の停止が重なり、CRR=0 でこの書き込みが届く。CA は無視される。Pointer は切り詰めた番地として受け取られる。次の doorbell で最大 3 TRB 前の古いコマンドを、Cycle が合えば再実行する。Stopped の event が来ないので 5 秒後に wedged となり HCRST に進む。
- 重さ：当たれば壊れる。
- 案：CRCR を読んで CRR を見る。CRR=1 のときだけ、Pointer を 0 にして CA を 64 ビットで書く。

### F2. 制御転送の Clear_TT_Buffer の方向

- 場所：`kernel/src/dev/xhci.c:799-800`、`823`、`832`、`1467`。
- 仕様：USB 2.0 11.24.2.3 Table 11-18。bit 15 Direction は対応する split transaction の値。11.17.5。
- 欠陥：制御の道は address=0 を渡すので、bit 15 はいつも 0（OUT）になる。
- 筋書き：HS ハブの後ろの LS・FS 装置で、Control の IN の段に Transaction Error が起きる。Clear は別の方向の buffer を指して何もせずに終わる。TT の bulk・control の buffer は busy のまま残る。buffer が尽きると、その TT の bulk と control が全部止まる。
- 重さ：当たれば壊れる。
- 案：制御では OUT と IN の両方を Clear する。原文は対象の buffer が無くても要求の誤りにしない。

### F3. SS・SSP の上位ランクのハブの Parent Hub の項目

- 場所：`kernel/src/dev/xhci.c:1533-1536`。
- 仕様：xHCI Table 6-6（Parent Hub Slot ID、Parent Port Number）と脚注 110。SS・SSP のバスで上位ランクのハブの後ろにある装置は、この二つの項目を入れる「shall」。例は Gen1x2 ハブの後ろの Gen1x1 装置。
- 欠陥：コードは HS ハブの後ろの LS・FS だけを設定する。
- 筋書き：Gen2 ハブの後ろの Gen1 装置の項目が 0 のまま xHC に渡る。xHC はランク変換の相手を知らずに転送する。
- 重さ：当たれば壊れる。
- 案：ハブと装置のランクを `dwExtPortStatus` の速度とレーンで比べ、装置のランクが低いとき二つの項目を入れる。
- 文書の内部の不一致は「仕様の内部の矛盾」の節に書く。

### F4. 遅れた event が追跡中の状態を上書きする

- 場所：`kernel/src/dev/xhci.c:505`、`532-538`、`783`。
- 仕様：xHCI 4.8.3。ソフトウェアが内部変数で EP の状態を追う方法を定める。Transfer Event と Command Completion は別のリングだと順が保証されない。
- 欠陥：505 行は TD が live かどうかを見ずに、エラーの event で `ep->state` を HALTED にする。532-538 行の stream も live を見ずに `halted` を立てる。783 行の Reset Endpoint は Context State Error でも Output を読み直さず false を返す。
- 筋書き：`usb_bulk` がタイムアウトする。Stop が Context State Error になり、Output の Halted を見て Reset と Set TR Dequeue を済ませ、状態は STOPPED になる。その後で IMOD で遅れた bulk の割り込み器のエラー event が届き、状態を HALTED に戻す。次の `usb_clear_halt` は Reset で Context State Error になり、false を返す。CLEAR_FEATURE は送られず、デバイス側の halt と toggle が残る。stream では 8 回の失敗で dead になる。
- 重さ：当たれば壊れる。
- 案：live の TD の event だけを状態に反映する。Reset の Context State Error では Output を読み直して続ける。

### F5. Short の最初の event で TD を完了にする

- 場所：`kernel/src/dev/xhci.c:540-548`。
- 仕様：xHCI 4.10.1.1.2。ISP だけが立つ TRB で short になると event は二つ来る。TRB Pointer が最後の TRB を指さないかぎり、完了と解釈してはならない。
- 欠陥：最初の event で `busy`、`gen`、`inflight` を進める。
- 影響：64KiB をまたぐ複数 TRB の TD で起きる。実害は `ring_room` の 2 倍の余裕で隠れている。
- 重さ：きれいさ（原文の違反）。
- 案：最初の event では `got` を記録し、最後の TRB の event で完了にする。

### F6. HID の TD の長さで報告が混ざる

- 場所：`kernel/src/dev/xhci.c:1096-1106`、`kernel/src/dev/usbhid.c:481-482`、`591`。
- 仕様：HID 1.11 8.4。一つの転送に一つの報告。wMaxPacketSize を超える longest 以外の報告は short で終わる。同じ top level collection の中で longest 以外の報告は short で終わる。
- 欠陥：`h->longest` はインターフェース全体の最大である。別の TLC の報告で長さがちょうど mps のものには、終端の義務が無い。
- 筋書き：mps=8 で、TLC1 の報告が 8 バイト、TLC2 の報告が 16 バイトの装置を考える。TD の長さは 16 である。8 バイトの報告は TD を完了させず、次の報告と同じ転送に入る。後ろの報告が失われ、キーの入力が遅れる。
- 重さ：当たれば壊れる。
- 案：サービス間隔ごとの TD を受け、報告 ID の長さで組み直す。

### F7. BOT の CSW の余分な条件

- 場所：`kernel/src/dev/usbmsc.c:113-114`（注の「6.5: not meaningful」も）。
- 仕様：BOT 6.3.2。meaningful は status 00h・01h かつ residue ≤ length、または 02h。6.5。Reset Recovery は not valid で shall、not meaningful で may。6.7.2 step 6。host が relevant と見るデータの量は差で決める。
- 欠陥：`(!csw[12] && len - residue > data_moved)` は、valid かつ meaningful な CSW に Reset Recovery をする。注の出典は誤りである。
- 筋書き：この差分で REQUEST SENSE の割り当て長を 18 から 252 に増やした。短い 18 バイトを返して residue を 0 と申告する装置は BOT_BROKEN になる。sense が消え、`unit_ready` が失敗し、ディスクが出ない。
- 重さ：当たれば壊れる。
- 案：この条件を消し、`MIN(len - residue, data_moved)` を結果の量として返す。

### F8. deconfigure の失敗でリングを解放する

- 場所：`kernel/src/dev/xhci.c:1298-1299`。
- 欠陥：Configure Endpoint（DC=1）の結果を見ずに `endpoints_free` を呼ぶ。`release_slot`（949-960 行）の「失敗なら記憶を保つ」と矛盾する。
- 筋書き：コマンドがタイムアウトか失敗で返る。xHC は EP を持ったままで、リングの頁は pmm に戻る。xHC の DMA が解放後の記憶を読み書きする。
- 重さ：当たれば壊れる。
- 案：cc が SUCCESS のときだけ解放し、それ以外は保つ。

### F9. Input Context に持ち主がいない

- 場所：`kernel/src/dev/xhci.c:1124`（`usb_clear_halt`、MSC のスレッド）、`1019`（`open_endpoints`、worker）。
- 欠陥：一つの装置の `in_ctx` を、二つのスレッドが錠なしで memset して書く。
- 筋書き：MSC と CDC の複合装置で、`blk_register`（usbmsc.c:309）の後に `usbnet_attach` が `usb_open_endpoints` を呼ぶ。別スレッドの `reset_recovery` が同じ `in_ctx` を書く。誤った Add・Drop の Configure Endpoint が走る。
- 重さ：当たれば壊れる（届く範囲は狭い）。
- 案：一つの mutex を、Input Context の記入からコマンドの完了まで持つ。

### F10. 制御リングの No Op

- 場所：`kernel/src/dev/xhci.c:850`（`ring_fit`）。
- 仕様：xHCI 4.11.2.2。Control Transfer Ring は Setup と Status、任意で Data の TD で作る。6.4.1.4。No Op は Transfer Ring に入れてよく、連続は避ける。4.6.8 の注。制御の EP の Reset の後は、Set TR Dequeue で Dequeue が Setup TD を指すようにする「shall」。4.11.7。各段は別の TD で、Link TD は TD の間に置ける。
- 欠陥：各段は Chain で結ばれないので、詰め物は要らない。`set_dequeue` の後に enq がリングの末尾にあると、次の転送で No Op が enq に入る。Dequeue は Setup TD でなく No Op を指す。No Op が二つ続くこともある。
- 重さ：きれいさ。4.6.8 の条件の文に反する範囲は、リングの末尾の 2 TRB の位置に限る。
- 案：ep0 の `ring_fit` を外す。

### F11. `usb_clear_halt` の Error の道

- 場所：`kernel/src/dev/xhci.c:1120-1122`。
- 仕様：xHCI 4.8.3。Error は Set TR Dequeue で Stopped になる。4.6.8 の最後の注。toggle を消すには Stopped で Drop と Add を行う。
- 欠陥：EP が Error（TRB Error の後）のとき、デバイス側の toggle だけを消す。host 側の toggle は残る。
- 筋書き：TRB Error の後で次の転送の toggle が合わず、転送が捨てられるか止まる。
- 重さ：当たれば壊れる（TRB Error の後だけ）。
- 案：`set_dequeue` の後に Drop と Add を行う。

### F12. isochronous の項目

- 場所：`kernel/src/dev/xhci.c:994`、`1042-1045`、`982-985`（`usb_endpoint_info`）。
- 仕様：xHCI Table 6-8。LEC=1 なら Mult は RsvdZ。4.3.6。SSPIsocCompanion=0 のときだけ Mult を使う。ESIT は SSP では dwBytesPerInterval。USB 3.2 9.6.7。bmAttributes の bit 7=1 なら Mult は無視され、wBytesPerInterval は 1。
- 欠陥：HCCPARAMS2 の LEC を読まない。SSP の Isoch Companion を読まない。この差分で alt 0 の isoch を開くようになったので、この道に新しく届く。
- 筋書き：SSP の isoch 装置で ESIT が 1 になるか、RsvdZ を汚す。Parameter Error で Configure Endpoint 全体が失敗し、装置全体が使えない。
- 重さ：当たれば壊れる（届く範囲は狭い）。
- 案：LEC を読む。bit 7 を見て SSP の記述子を読む。

### F13. HS bulk OUT の Interval

- 場所：`kernel/src/dev/xhci.c:994`、`1054`。
- 仕様：xHCI 6.2.3.6。HS の bulk と control の OUT は、最大 NAK 率を指定する「shall」。0 は決して NAK しないという意味。
- 欠陥：いつも 0 を書く。
- 重さ：きれいさ。
- 案：bInterval を入れる。

### F14. readdress の細部

- 場所：`kernel/src/dev/xhci.c:1375`、`1398-1401`。
- 仕様：xHCI 6.2.3.1。Address Device の入力は DCS=1 を有効の条件にする。4.6.5 の注。Address Device が USB Transaction Error で、装置が TT の後ろなら、CLEAR_TT_BUFFER を発行する「shall」。
- 欠陥 1：1375 行は `ep0.cycle` をそのまま DCS に入れる。リングが一周していると 0 になりうる。
- 欠陥 2：1467 行には TT の Clear があるが、readdress の道の 1398-1401 行には無い。
- 筋書き：HS ハブの後ろの FS ハブを readdress する。Transaction Error の後で TT の buffer が busy のまま残り、F2 と同じ止まり方になる。
- 重さ：当たれば壊れる（欠陥 2）。欠陥 1 はきれいさ。
- 案：Address の前に `ring_reset` で ep0 のリングを初期化する。1398 行の失敗の道でも `tt_clear` を呼ぶ。

### F15. TTT

- 場所：`kernel/src/dev/usbhub.c:145`、`kernel/src/dev/xhci.c:747`。
- 仕様：xHCI Table 6-6。Speed が HS でなければ TTT は 0。USB 2.0 Table 11-13。bit 6:5 は HS ハブの TT の値。
- 欠陥：FS ハブでも descriptor の値を渡す。
- 重さ：きれいさ。
- 案：`speed == USB_SPEED_HIGH` のときだけ渡す。

### F16. Disable Slot の前提（この差分の外）

- 場所：`kernel/src/dev/xhci.c:950-953`（`usb_remove` から `release_slot`）。
- 仕様：xHCI 4.6.4 の注。有効な EP は Stopped か Running:Idle で、未処理の Transfer Event を受け終えている「shall」。でなければ undefined。
- 欠陥：HID の stream は TD が常にあるまま Disable Slot する。
- 筋書き：USB キーボードを抜く。`usb_remove` は stream の TD をリングに残したまま Disable Slot を発行する。この状態は原文の前提の外で、xHC の動作は undefined である。
- 重さ：当たれば壊れる。
- 案：有効な EP を全部 Stop し、event を受け終えてから Disable Slot を発行する。

### F17. 確保失敗の後の再起動

- 場所：`kernel/src/dev/xhci.c:1728`、`1744`、`1854`、`1862-1866`。
- 欠陥：`interrupters_init` が途中で失敗すると `hc->intr` が残る。再起動で `!hc->intr` が偽になり、`interrupter_start` が NULL の `ev` に memset する。1862-1866 行は失敗で array と途中の頁を漏らす。
- 筋書き：起動時のメモリー不足で初期化が失敗する。S3 からの復帰で再起動が走り、panic する。
- 重さ：当たれば壊れる。
- 案：失敗の道で確保した物を全部戻し、`hc->intr` を NULL にする。

### F18. 細かな不一致の組

いずれも重さはきれいさ。

- `kernel/src/dev/xhci.c:292`：`hc_set_running` の直前の輪の中と同じ store が重なっている。案は片方を消す。
- `kernel/src/dev/xhci.c:1000`：注は 4.14.1.1 の「interrupt は約 1KiB」を挙げる。値は `ep->mps` である。原文の 1KB は初期値の「reasonable」な例で、義務ではない。案は注を実際の値に合わせる。
- `kernel/src/dev/usbhub.c:187` と `xhci.c:1528`：xHCI Table 7-14・7-15 の MHD（ポートごとの最大ハブ深さ）を読まず、深さ 5 で決める。低い MHD の xHC で、許さない深さの装置を列挙する。案は MHD を読んで深さの上限に入れる。
- `kernel/src/dev/usbhid.c:351`、`396`：検証していない Report Count（u32）で輪が回る。`h->longest` が巨大になり、確保も巨大になる。AGENTS.md の「上限と中断」に合わない。案は Get_Report の wLength（16 ビット）に収まらない報告を持つ descriptor を、輪の前に拒む。
- `kernel/src/dev/usbmsc.c`：MSC Boot 2.2 が保証するのは bCBWCBLength=12 の PAD 付き CDB だけである。コードは 6 と 10 を送る。BOT では有効で、subclass 06h では SPC の長さである。subclass が 06h でない最小の装置にだけ関わる。案は、その装置へ 12 バイトの PAD 付きの CDB を送る。

### F19. Output の RsvdO を Input Context へ写す

- 場所：`kernel/src/dev/xhci.c:1128`。
- 仕様：xHCI 5.1.1。RsvdO はソフトウェアが書いてはならない。xHC の作業領域である。6.2.3 の注。Endpoint Context の 14-1Fh は RsvdO。
- 欠陥：`memcpy(c, out_ctx(xd, dci), hc->ctx_size)` は bytes 14h から後ろも Input Context へ写す。Input の領域に xHC の作業の値が入る。
- 重さ：きれいさ。
- 案：dword 0 から 4 だけを写す。

## 調べて合っていた手順

- reset a pipe の順（Reset EP TSP=0、TT の Clear、CLEAR_FEATURE、Set TR Dequeue）：xHCI 4.6.8、4.10.2.1。制御は CLEAR_FEATURE なし。
- Stop の Context State Error で Output を読む：4.6.9。Set TR Dequeue は Stopped・Error で可：4.6.10。Drop と Add で Running へ：4.8.3 の 6。Add の EP State=0：6.2.3.2。
- Slot Context と Endpoint Context のビット位置：Table 6-4 から 6-11。Address の入力：6.2.2.1。
- EP Type、CErr（isoch は 0）、Max Burst：6.2.3.4。Interval：Table 6-12 と脚注 113。Average 8（制御）：4.14.1.1。
- Route の 14 ポート超は 15：脚注 106。
- ハブの readdress の設計：4.5.2 の注、USB 2.0 11.24.2.5、USB 3.2 10.16.2.3、xHCI 4.6.11、6.2.2.4、6.2.3.7、4.6.5。
- 構成の順と DC=1：4.3.5、4.6.6。
- TRB の型と CC の番号。Setup の TRT、DIR（Table 4-7）、IDT、Status の IOC、タイムアウトで Stop：4.11.2.2。
- TD Size：4.11.2.4。Chain、No Op の前の CH=0、同じ Interrupter Target：4.11.7。先頭の TRB を最後に書く：4.11.7.1。Link の CH と TC：4.11.5.1。
- 64 ビットのレジスターの書き込み：5.1.1。CRCR の初期化。初期化の順（ERSTSZ、ERDP、ERSTBA、IMOD、IE、R/S）：4.2。
- 構造の境界（Event Ring の 64KB など）：Table 6-1。IMOD の 250ns：5.5.2.2。MSI の IP の自動クリア：4.17。EHB：4.17.2。
- EINT を IP より先に消す：5.4.2。`xhci_isr` の順は合っている。
- HCH を待ってから HCRST、HCRST の完了を待つ：5.4.1。R/S の停止は 16 ms：HALT_NS の用途として合っている。
- 電源：PPC=1 のときだけ PP を立て、20 ms 待つ：5.4.8。R/S の後に書く：同じ節。
- `PS_KEEP`（PP、PIC、WCE、WDE、WOE）と `PS_CHANGES`（bit 17-23）：Table 5-27。RW1CS の PED、RW1S の PR、RW の LWS を写さない。
- ポートのリセット：4.3.1。WRC と PRC の完了の見方：4.19.2。
- Transfer Event の長さの意味：Table 6-38。Default Speed の表：7.2.2.1.1、Table 7-13、7.2.2.1.2。PSI の PSIV、PSIE、PLT、LP（bit 15:14、1 が SuperSpeedPlus）、PSIM：7.2.1。
- USBLEGSUP の byte の書き込み：7.1.1。Scratchpad の手順：4.20。
- ハブの要求の値：SET_HUB_DEPTH は 12、wValue が深さ。Table 10-9 の feature 値。USB 2.0 Table 11-17 の feature 値。
- Port Status の EXT：Table 10-12。Rx Sublink は bit 0-3：Table 10-15。`wPortChange`：Table 10-14。`wPortStatus` の bit 位置：Table 10-13。
- SSP の記述子：USB 3.2 9.6.2.5。SSAC は bit 4:0 で数は SSAC+1、属性は offset 12。
- Clear_TT のビット配置と wIndex=1（単一 TT のとき）：USB 2.0 Table 11-18、11.24.2.3。TTT のビット 6:5：Table 11-13。
- HID の範囲外の値は無視：5.10。Null：6.2.2.5。報告の長さの計算：8.4。SET_PROTOCOL（0 が boot）と SET_IDLE（0 が無期限）の値：7.2.4、7.2.6。
- HUT：ErrorRollOver 01h、POSTFail 02h、ErrorUndefined 03h は Table 12 と脚注 9。Caps Lock は LED の 02h。
- BOT：Reset Recovery の順：5.3.4。CSW の再読：Figure 2。stall のデータ保持：6.7.2。ZLP なし：6.7.3。CBW・CSW の形式。subclass と protocol 50h：MSC Overview Table 1、Table 2。
- SCSI：252 の割り当て：Seagate 3.37。READ・WRITE(10/16)、READ CAPACITY、INQUIRY、TEST UNIT READY、START STOP、SYNCHRONIZE CACHE の CDB。
- `pci_bar_size` は再 probe をしない。`dma_block` の 64KiB の境界は `pmm_alloc_contig` の 2 冪の整列で満たす。
- 錠の順：`ir->lock` の下に `ep->lock`・`ctl_lock`・`cmd_lock`。子の `ctl_mutex`、ハブの `ctl_mutex`、`cmd_mutex` の順に逆は無い。
- 解放後の使用：`endpoints_free` と `release_slot` の `sync_drains` が防ぐ（F8 の xHC 側の DMA は別）。`usbmsc_detach` は `m->lock` で待つ。`usb_remove` の順は合っている。

## 仕様の内部の矛盾

原文を読んでも決められない点がある。

- Table 6-4 は Speed を deprecated かつ Reserved と書き、6.2.2.1 は Speed を有効の条件にしている。コードは Speed を入れる。
- Table 6-1 の Scratchpad Buffer Array の大きさ「248」と境界 PAGESIZE は、最大 1023 個と合わない。コードは 2 頁を組で整列する。
- 4.3.7 は LS・FS でない装置の Parent Hub の項目を 0 にすると書き、Table 6-6 は SS・SSP の上位ランクのハブの後ろの装置に入れると書く。この監査は個別の規定である Table 6-6 を取って F3 にした。
- 再起動のときの Scratchpad を消すかどうかの規定は見つからない。

## 読み切れなかった範囲

次の節は原文を通読していない。

- xHCI 4.17.4 の全文、5.5.2 の各レジスターの細部。該当する行の値だけを照らした。
- HID Appendix B.1 の本文。`boot_keyboard` の形式は原文で照らしていない。
- HID 6.2.2.8 の本文。

表が崩れた箇所は行を合わせて読み、PDF の画像では確かめていない。
