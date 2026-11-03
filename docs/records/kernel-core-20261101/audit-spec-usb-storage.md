# 敵対的な仕様の監査：USB・入力・記憶

## 依頼文

あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機）の敵対的な仕様の監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。

持ち主の言葉（そのまま）："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

### 対象

main の今の版の次のコード。変更の範囲は `git log --stat` で 7c1fd4ac（USB と入力）と 8f09aaf8（I/O）を見る。報告書や設計の文書は読まない。コードと仕様の原文だけで判断する。

- USB と入力：`kernel/src/dev/xhci*`、`kernel/src/dev/usb*`、`kernel/src/dev/input.*`。
- 記憶：`kernel/src/dev/nvme*`、`kernel/src/dev/ahci*`、`kernel/src/dev/blk*`。これらが使う MSI・MSI-X・DMA の道（`kernel/src/arch/irq.*`、`kernel/src/dev/pci*`、`kernel/src/dev/iommu*`）のうち関わる所。

### 仕様（`D:\Programes\OS DEV INFO`）

このフォルダーから該当する公式の文書を探す（xHCI、USB 2/3、USB HID と Usage Tables、USB Mass Storage、NVMe Base と NVM Command Set と PCIe Transport、AHCI、SATA、PCI Express と PCI Local Bus の MSI・MSI-X）。文書の版を報告に書く。手元に無い文書は、無いと一度だけ書く。

### 仕事

レジスターの順序と必須の手順、ビットの位置と意味、予約の値、待ち時間と時間切れ、バリアと所有権の受け渡し、リセットと初期化の順、キューの歯車とドアベル、エラーと回復、割り込みの消し方、装置の capability の読み方を、一行ずつ仕様の原文で照らす。合格を探さない。仕様に合わない所、仕様が必須とする手順の欠け、仕様が許さない決め打ちを探す。特定の機械でしか成り立たない前提も探す。

各指摘に：ファイル:行、仕様の文書と節と原文の短い引用、壊れ方の具体的な筋書き、重さ（当たれば壊れる／データを失う／きれいさ）、直す案を一つ。調べて合っていた手順は、何を照らしたかを一行で書く。読み切れなかった範囲はそう書く。

### 報告

`docs/records/kernel-core-20261101/audit-spec-usb-storage.md` へ日本語で全文（この依頼文を先頭に）。コミットしない。返事にも要点を日本語で返す。サブエージェントをさらに出さない。Python で書くなら `newline='\n'`。

## 結論

- 指摘は 48 件ある。データを失うものが 5 件、当たれば壊れるものが 21 件、きれいさが 22 件ある。
- 一番重いのは AHCI の A1 で、失敗した I/O を成功として返す。
- 次に重いのは N1・A2・A3 で、制御器が動いているメモリーを手放す。
- 仕様の手順が丸ごと欠けているのは、xHCI の X1（Configure Endpoint と SET_CONFIGURATION の順）と X5（reset a pipe の順と CLEAR_TT_BUFFER）と、AHCI の A4（IS の消去）である。
- 実機で一番当たりやすいのは X4 である。GET_MAX_LUN の STALL のような普通の出来事で、制御パイプが抜き差しまで死ぬ。
- ビット位置・命令の番号・ドアベルの歯車に誤りは見つけていない。誤りは手順と回復の道に集まっている。

### 引用の扱い

原文の文は写していない。著作物の写しを避ける規則があるため、依頼の「原文の短い引用」には従えなかった。代わりに、節番号と日本語の要旨を書く。要旨の後の `(shall)`・`(should)`・`(may)` は原文の強さを示す。`xhci.txt:行` は、PDF を pdftotext で起こした次の場所の行番号で、原文へすぐ辿るためのものである。

`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec`

## 文書の版

| 略称 | 文書 | 版 |
|---|---|---|
| xHCI | `extensible-host-controler-interface-usb-xhci.pdf` | Revision 1.2、May 2019 |
| USB2 | `usb_20_20250603/.../usb_20.pdf` | USB 2.0 |
| USB3 | `usb_32_202206/USB 3.2 Revision 1.1.pdf` | USB 3.2 Revision 1.1、June 2022 |
| HID | `hid1_11.pdf` | HID 1.11 |
| HUT | `hut1_12v2.pdf` | Usage Tables 1.12 |
| BOT | `usbmassbulk_10.pdf` | Bulk-Only Transport 1.0 |
| MSC | `Mass_Storage_Specification_Overview_v1.4_2-19-2010.pdf` | Overview 1.4 |
| NVMe | `NVM-Express-Base-Specification-Revision-2.4-Ratified-2026.07.31.pdf` | Base 2.4 |
| NVMe14 | `NVM-Express-1_4c-2021.06.28-Ratified.pdf` | 1.4c |
| AHCI | `serial-ata-ahci-spec-rev1-3-1.pdf` | AHCI 1.3.1 |
| PCI | `PCI Local Bus Specification Revision 3.0.pdf` | PCI 3.0 |
| PCIe | `PCI Express Base Specification Revision 6.0.pdf` | PCIe 6.0 |

手元に無い文書は、NVM Command Set 単独の仕様、NVMe over PCIe Transport 単独の仕様、Serial ATA 本体の仕様、ATA ACS、SCSI の SPC・SBC である。

## 指摘の一覧

| 重さ | 番号 |
|---|---|
| データを失う | A1、A2、A3、N1、N2 |
| 当たれば壊れる | X1、X2、X3、X4、X5、X6、X7、X8、X10、A4、A5、A6、A7、A8、A9、A10、H1、H2、H4、M1、U1 |
| きれいさ | X9、X11、X12、X13、X14、X15、A11、A12、A13、A14、N3、N4、N5、N6、P1、H3、H5、H6、H7、M2、M3、U2 |

きれいさの欄の X13 と X15 と M3 は、今のコードの動きでは仕様違反に当たらない。

## AHCI（`kernel/src/dev/ahci.c`）

### A1：engine を止めている間に、終わっていないコマンドを成功と報告する

- 場所：`ahci.c:211-223`（`reap`）、`ahci.c:371-388`（`halt`）。
- 仕様：AHCI 5.3.2.10（ahci.txt:3436）。PxCMD.ST を消すと HBA が PxCI と PxSACT を 0 にする。
- 筋書き：コードは「PxCI にも PxSACT にも居ないコマンドは終わった」と読む。`halt` は `p->lock` を放して `port_stop` を呼び、ST を消す。この間に別のコマンドの待ち手が 10 ms の見回りで `reap` を呼ぶか、割り込みが `reap` を呼ぶ。`reap` は `out == 0` と読み、`p->issued` の全部を `DONE` にする。時間切れの呼び手も `status == DONE` を見て 0 を返す（`ahci.c:516`）。読みは中身の無いバッファーを成功で返し、書きはディスクに届かないまま成功で返る。`p->halting` を読むのは `halt` だけで、`reap` は見ていない。
- 重さ：データを失う。NCQ で 2 つ以上のコマンドが走っている間の時間切れで当たる。
- 直す案：`reap` は `p->halting` の間は `done` を求めず、`halt` が FAILED を付けるのに任せる。

### A2：engine が止まらなくても、呼び手へバッファーを返す

- 場所：`ahci.c:380-387`（`halt` が `port_stop` の結果を捨てる）。`ahci.c:247-249` の注釈は「この後は何も動かない」と言い切っている。
- 仕様：AHCI 10.1.2 手順 3、10.4.2、10.4.3。CR か FR が 500 ms で落ちなければ port reset か HBA reset で回復してよい (may)。CR が落ちない間は HBA は動いている。
- 筋書き：止まった HBA で CR が落ちない。`halt` は全コマンドを FAILED にし、呼び手はバッファーを解放する。HBA が後から DMA を続け、再利用されたメモリーを壊す。
- 重さ：データを失う。
- 直す案：`port_stop` が false なら、GHC.HR の HBA reset を行い、それも効かなければ bus master を切ってから FAILED を付ける。

### A3：装置の無いポートを止めず、firmware の番地のまま残す

- 場所：`ahci.c:730-734`（`det` が 0 のポートには何もしない）。
- 仕様：AHCI 10.1.2 手順 3 と手順 5。実装された各ポートを idle にし、各ポートの PxCLB と PxFB を書かなければならない (shall)。
- 筋書き：firmware が空のポートに FRE=1 と自分の FIS 領域を残す。カーネルはその領域を普通のメモリーとして使う。後でディスクが繋がるか COMINIT が来ると、HBA が D2H FIS をその番地へ書き、カーネルのメモリーを壊す。
- 重さ：データを失う。
- 直す案：`start` で PI の全ポートに `port_stop` をかけ、PxIE を 0 にしてから `det` を見る。

### A4：PxIS と IS を消さずに割り込みを開く

- 場所：`ahci.c:729`（`GHC.IE`）、`ahci.c:293-295`（PxIS だけ消して PxIE を書く）、`ahci.c:765`（`ahci_resume`）。
- 仕様：AHCI 10.1.2 手順 7 の Note（ahci.txt:7205 付近）。PxIE と GHC.IE を書く前に、PxIS、次に IS.IPS の順に消さなければならない (must)。10.7.2.1：MSI は IS が変わったときだけ出る。
- 筋書き：firmware が IS のビットを残す。そのポートの完了で PxIS が立っても IS は 1 のままで変わらず、MSI が出ない。割り込みが来ないので `ahci_isr` は走らず、IS は消えない。`reap` は PxIS しか消さない（`ahci.c:209`）。全コマンドが 10 ms の見回りでしか終わらず、毎秒 100 コマンドが上限になる。ディスクが一台なら直る機会が無い。
- 重さ：当たれば壊れる。
- 直す案：`start` と `ahci_resume` で GHC.IE の前に全ポートの PxIS と IS を消し、`port_start` でも PxIS の後に IS のそのポートのビットを消す。

### A5：GHC.AE を立てる前に CAP2 と BOHC に触れる

- 場所：`ahci.c:687-695`（`hba_take`）。
- 仕様：AHCI 3.1.2（ahci.txt:1357）。他の AHCI レジスターへ触れる前に AE を立てなければならない (shall)。10.1.2 手順 1。
- 筋書き：legacy との両用の HBA（CAP.SAM=0）で AE=0 のまま CAP2 を読む。BOH を読み違えると handoff を飛ばし、firmware の SMI と同時に HBA を触る。
- 重さ：当たれば壊れる（まれ）。
- 直す案：`hba_take` の先頭で AE を立てる。

### A6：Port Connect Change と PhyRdy Change を受けない

- 場所：`ahci.c:80`、`ahci.c:294`。
- 仕様：AHCI 6.2.2.3。要求なしの COMINIT を受けた HBA は COMRESET を返し、PxIS.PCS が消えるまで実行を止める。ソフトは割り込みで PCS を見て fatal の回復をするのがよい (should)。3.3.5：PCS は PxSERR.DIAG.X を消さないと消えない。
- 筋書き：ディスクの一瞬の電源の揺れで COMINIT が来る。HBA は止まるが PxIE に PCE が無いので割り込みは来ず、`IS_ERRORS` にも入っていない。全コマンドが `IO_NS` の 30 秒を待って初めて回復する。抜かれたディスクも同じで、30 秒ごとに待つ。
- 重さ：当たれば壊れる。
- 直す案：PxIE に PCE と PRCE を足し、`reap` で PCS・PRCS を fatal として扱い、回復の中で PxSERR を消す。

### A7：staggered spin-up を全部のポートで同時に、固定の 50 ms で行う

- 場所：`ahci.c:719-728`、`ahci.c:32`、`ahci.c:732-733`。
- 仕様：AHCI 10.10（目的は電源の同時の大電流を避けること）、10.10.2（電源投入と S3 の復帰でソフトが 10.1.1 の手順を行う (should)）、10.1.1 手順 4-5（SUD を立て、PxSSTS.DET が 1h か 3h になるまで見回る）。
- 筋書き：CAP.SSS の板で全ディスクが同時に回り始め、電源の余裕の無い機械で落ちる。50 ms で DET が上がらないディスクは `det == 0` で捨てられ、再び探されない。起動の後の再走査も無いので、見つからないままになる。
- 重さ：当たれば壊れる。
- 直す案：ポートごとに SUD を立て、PxSSTS.DET を時間切れまで見回り、次のポートへ進む。

### A8：NCQ のエラーで無実のコマンドの試行回数を減らす

- 場所：`ahci.c:215-220`、`ahci.c:532-535`。
- 仕様：AHCI 6.2.2.2。回復は PxSACT を読んで終わっていないものを知ることから始まり、終わっていないものを報告するか出し直す。
- 筋書き：コードは PxSACT を読まずに `p->issued` の全部を FAILED にする。既に終わったコマンドも失敗になる。読めないセクターへの読みが 3 回失敗する間、隣の健全な書きも 3 回巻き込まれ、`transfer` が -1 を返す。健全な LBA の書きが I/O エラーになる。
- 重さ：当たれば壊れる。
- 直す案：`issued & ~out` は DONE にし、残りはエラーの主に当たる一つだけ試行を数え、他は数えずに出し直す。

### A9：S3 の後に BIOS/OS handoff をやり直さない

- 場所：`ahci.c:762-767`（`ahci_resume`）。
- 仕様：AHCI 10.6（OS の所有は次の system reset まで）、10.6.1（電源投入で OOS・BOS は 0）、10.6.2（BIOS は所有を宣言できる）。
- 筋書き：S3 で HBA の電源が切れ、OOS が 0 に戻る。BIOS が復帰の途中で BOS を立てる。OS は OOS を立て直さないので、SMI と同時に HBA を触る。
- 重さ：当たれば壊れる（まれ）。
- 直す案：`ahci_resume` から `hba_take` を呼ぶ。眠れない文脈なら、次の `port_start` の前に行う。

### A10：止まらないポートに port reset を試さない

- 場所：`ahci.c:278-281`。
- 仕様：AHCI 10.1.2 手順 3、10.4.2。CR が落ちなければ COMRESET へ進んでよい (may)。
- 筋書き：firmware が詰まらせたポートで `port_start` が false を返し、ディスクが見つからない。
- 重さ：当たれば壊れる（まれ）。
- 直す案：CR が落ちなくても COMRESET を試し、それでも駄目なら GHC.HR を行う。

### A11：回復のたびに必ず COMRESET する

- 場所：`ahci.c:274-287`。
- 仕様：AHCI 10.4（軽い reset から順に行うのを勧める）、6.2.2.1（COMRESET は BSY か DRQ が残るとき）。
- 筋書き：一つの読み誤りごとにリンクを張り直す。HDD では毎回大きく遅れる。
- 重さ：きれいさ。
- 直す案：ST を落として BSY・DRQ を見て、残っているときだけ COMRESET する。

### A12：IDENTIFY の完了に割り込みが来ない

- 場所：`ahci.c:294`（PSE が無い）、`ahci.c:493`、`ahci.c:508`。
- 仕様：AHCI 3.3.5 の PSS、5.3 の PIO の流れ（ahci.txt:4412）。PIO の読みは D2H Register FIS で終わらない。
- 筋書き：IDENTIFY は毎回 10 ms の見回りで終わる。ディスク一台につき起動が 10 ms 遅れる。
- 重さ：きれいさ。
- 直す案：PxIE に PSE を足す。

### A13：Overflow を見ない

- 場所：`ahci.c:80`、`ahci.c:294`。
- 仕様：AHCI 6.1.5（ahci.txt:5975）。読みの overflow は深刻で、fatal の回復をするのがよい (should)。
- 筋書き：PRD より多く送るディスクのコマンドが成功で終わる。
- 重さ：きれいさ。
- 直す案：OFS を `IS_ERRORS` と PxIE に足す。

### A14：handoff の 2 秒を固定で待ち、BOS を見直さない

- 場所：`ahci.c:693`。
- 仕様：AHCI 10.6.3 手順 3-4。BOS が落ちるのを待ち、BB が立ったら最低 2 秒与える。
- 筋書き：BOS がすぐ落ちても 2 秒止まる。2 秒で BOS が落ちなくてもそのまま進む。
- 重さ：きれいさ。
- 直す案：BOS を見回り、2 秒を過ぎて落ちなければ記録して進む。

### AHCI で合っていた手順

- CAP の S64A・SNCQ・SSS・NCS、CAP2.BOH、GHC の AE・IE、BOHC の各ビットを 3.1 で照らした。
- PxCMD の ST・SUD・POD・FRE・FR・CR、PxTFD、PxSSTS.DET、PxIS の TFES・HBFS・HBDS・IFS・SDBS・DHRS の位置を 3.3 で照らした。
- コマンドヘッダーの CFL・W・PRDTL・CTBA、PRD の DBC（bit 0 が 1、最大 4 MiB）を 4.2 で照らした。
- コマンドリスト 1 KiB・FIS 領域 256 B・テーブル 128 B の整列を 4.2 で照らした。
- ST の前に FRE を立て、CR=0 と BSY=0・DRQ=0・DET=3h を確かめる順を 10.3.1 で照らした。
- FB を FR=0 の間に書く順を 10.3.2 で照らした。
- COMRESET の 1 ms 以上の DET=1、DET=0 の後の DET=3h 待ち、PxSERR の消去を 10.4.2 で照らした。
- 通常の割り込みの順（PxIS の後に IS）を 10.7.2.1 で照らした。
- 単一の MSI（MME=0）を 10.7.2.1 で照らした。
- PxSACT を PxCI の前に新しいビットだけ書く順を 5.5.1 で照らした。
- NCQ の PxCI の条件と `issued & ~out` の完了の読みを 3.3.13、3.3.14 で照らした（A1 の窓を除く）。
- 回復の STOP→SERR→PxIS→ST の順を 6.2.2.1 で照らした。
- COMRESET をしたので READ LOG EXT を省く判断を 6.2.2.2 で照らした。
- fatal で HBA が新しいコマンドを出さないので `reap` がバッファーを返してよいことを 6.2.2 で照らした。
- D3 の前に ST を消す規則を 10.3.1 で照らした。

## NVMe（`kernel/src/dev/nvme.c`）

### N1：reset の前に PRP リストとバッファーを手放す

- 場所：`nvme.c:490`（時間切れでも `pmm_free(list)`）、`nvme.c:497`（三度目は reset せずに返す）、`nvme.c:357-359` と `nvme.c:499`（reset が通らなくても返す）。
- 仕様：NVMe 3.7.2（nvme24.txt:8609）。制御器が出ているコマンドの処理を止めるのは Controller Level Reset で、完了の印は CSTS.RDY=0 である。
- 筋書き：コマンドが時間切れになる。`run` が返った所で PRP リストの頁を解放する。制御器はまだコマンドを持っていて、再利用された頁を PRP リストとして読み、その中の番地へ DMA する。三度目の時間切れでは `transfer` が reset なしで -1 を返し、呼び手はバッファーを再利用する。遅れて来た読みのデータがそのメモリーを上書きする。
- 重さ：データを失う。
- 直す案：時間切れのときは `recover` が CSTS.RDY=0 を確かめた後にだけ `list` とバッファーを手放す。RDY が落ちなければ `pci_disable_busmaster` で切ってから返す。

### N2：shutdown の待ちを RTD3E の典型値で切る

- 場所：`nvme.c:700-704`。
- 仕様：NVMe 3.6.1（nvme24.txt:8120 付近）。host は RTD3E を最低でも待つのを勧める。図 338 の RTD3E は「典型の」遅れである。
- 筋書き：RTD3E が 2 秒で、実際に 3 秒かかる SSD で 2 秒で諦める。RTD3E が 10 秒を超える SSD では 10 秒で諦める。電源が切れ、書込みキャッシュを失う。
- 重さ：データを失う。
- 直す案：RTD3E を下限にし、上限は RTD3E の 4 倍と 60 秒の大きい方にする。

### N3：CC.CSS を CAP.CSS に従わせない

- 場所：`nvme.c:367`、`nvme.c:649`。
- 仕様：NVMe 3.5.1 手順 3 と手順 8。CAP.CSS.IOCSS=1 なら CC.CSS は 110b にし (should)、CNS 1Ch と FID 19h と CNS 07h を行う。
- 筋書き：NVM だけを使うなら 000b で動く。IOCSS の制御器で手順から外れる。
- 重さ：きれいさ。
- 直す案：IOCSS を読み、手順 8 を入れる。

### N4：Identify Controller の前に I/O キューを作る

- 場所：`nvme.c:373-413`、`nvme.c:657`、`nvme.c:664`。
- 仕様：NVMe 3.5.1 手順 7-11 (should)。図 41 の IOSQES・IOCQES は Identify の SQES・CQES に従う。
- 筋書き：CQES・SQES を確かめないまま作る。64 B・16 B を受けない制御器で Create が失敗し、理由が分からない。
- 重さ：きれいさ。
- 直す案：`enable` の中で Identify Controller を先に行い、SQES・CQES を確かめる。

### N5：shutdown の前に I/O キューを消さない

- 場所：`nvme.c:694-708`。
- 仕様：NVMe 3.6.1 手順 1 (should)。
- 筋書き：制御器が消す手順を省く。普通は害が無い。
- 重さ：きれいさ。
- 直す案：Delete SQ、Delete CQ を出してから CC.SHN を書く。割り込みを閉じた文脈では見回りで待つ。

### N6：CSTS.ST を見ない

- 場所：`nvme.c:702`、`nvme.c:704`。
- 仕様：NVMe 3.6.1。controller shutdown の完了は SHST=10b かつ CSTS.ST=0 である。
- 筋書き：別の制御器が始めた NVM Subsystem Shutdown の完了を、自分の完了と読む。多制御器の SSD でだけ起きる。
- 重さ：きれいさ。
- 直す案：SHST と ST を一緒に見る。

### NVMe で合っていた手順

- CAP の MQES・TO・DSTRD・CSS・MPSMIN・CRMS の位置を図の CAP で照らした。
- CC の EN・IOSQES・IOCQES・SHN と CSTS の RDY・CFS・SHST の位置を照らした。
- CRTO の CRWMT と待ちの選び方を 3.5.4 で照らした。
- EN を落とす前に RDY=1 を待つ順、RDY=0 を待つ順を 3.5.1 手順 1 と CC の定義で照らした。
- shutdown の後の再起動（EN を落としてから SHN=00b と EN=1）を 3.6.1 で照らした。
- 32 ビット二回での 64 ビットのレジスターの読み書きを NVMe14 の 3 章（nvme14.txt:2208）で照らした。
- AQA の 0 基準、ADMIN の 32 エントリー、MQES が I/O キューだけに効くことを図 44 と CAP で照らした。
- Number of Queues の FID 07h、0 基準、65534 の上限、割り当て値の読み方を図 472・473 で照らした。
- Create CQ の QID・QSIZE・IV・IEN・PC と Create SQ の CQID・PC を図 572・573・576・577 で照らした。CQ を SQ より先に作る順も照らした。
- ドアベルの番地と stride を CAP.DSTRD で照らした。
- phase の反転、CQ head のドアベル、DW3 の status の SC・SCT・DNR を 4.2.3、4.2.4 で照らした。
- キューの満杯の規則（size-1 個まで）を 3.3.1 で照らした。
- PRP1 の dword 整列、PRP2 と PRP リストの頁整列、512 エントリーで鎖の要らないことを 4.3.1 で照らした。
- MDTS の単位（MPSMIN）を図 338 で照らした。
- Identify の CNS 00h・01h・02h、バイト位置（MN、MDTS、RTD3E、NN、VWC）を図 338 で照らした。
- NVM の Read・Write・Flush の opcode、SLBA、NLB、FLBAS bits 3:0、LBAF の位置を NVMe14 で照らした。

## PCI と MSI・MSI-X（`kernel/src/dev/pci.c`）

### P1：bus master の関数が I/O Space も開ける

- 場所：`pci.c:282-287`。
- 仕様：PCI 3.0 6.2.2、6.2.5。decode を開ける前に番地の地図が整っている必要がある。
- 筋書き：firmware が AHCI の legacy の I/O BAR を割り当てないまま渡す。I/O decode を開けると、その関数が番地 0 付近の I/O に応答する。AHCI と NVMe に I/O Space は要らない。
- 重さ：きれいさ。
- 直す案：Memory Space と Bus Master だけを開ける。I/O Space を使う driver だけが別に開ける。

### PCI で合っていた手順

- MSI の Message Control の MMC bits 3:1、MME bits 6:4、64 ビット bit 7、mask bit 8 を 6.8.1 で照らした。
- Message Data の上位 2 バイトを保つ書き方を 6.8.3.1 と PCIe 6.0 の 6.1.4 で照らした。
- MSI の vector の block を数に整列する `vectors_take` を 6.8.3.4 で照らした。
- MSI と MSI-X を同時に有効にしないことを 6.8.3.3 で照らした。
- MSI-X の Table Size、BIR と offset の分け方、Enable と Function Mask の位置を 6.8.2 で照らした。
- Table Entry を mask してから書き、Vector Control の予約ビットを保つ書き方を 6.8.3.5 で照らした。

## xHCI（`kernel/src/dev/xhci.c`）

### X1：SET_CONFIGURATION を Configure Endpoint より先に送る

- 場所：`xhci.c:1238`（SET_CONFIGURATION）、`xhci.c:1247`（`bind_classes` → `open_endpoints`）、`xhci.c:1248`（`usbmsc_attach` → `usb_open_endpoints`）。
- 仕様：xHCI 3.3.5（xhci.txt:3363）、4.3.5・4.3.6（xhci.txt:4110）。Configure Endpoint が成功しなければ SET_CONFIGURATION を送ってはならない (shall)。送れば動作は未定義である。
- 筋書き：hub の多い構成で Configure Endpoint が Bandwidth Error か Resource Error で失敗する。装置は既に Configured で、xHC の側に端点が無い。装置は電流も帯域も取ったままになる。
- 重さ：当たれば壊れる。
- 直す案：全 class の端点を集めて Configure Endpoint を通してから SET_CONFIGURATION を送る。

### X2：CRCR の Abort を二つの dword で書く

- 場所：`xhci.c:649-650`。
- 仕様：xHCI 5.4.5（xhci.txt:19722）、4.6.1.1。CRR=1 の間はポインターの書込みは無視され、CRR=0 の間に書いた値は次に読む番地になる。
- 筋書き：下位の CA で CRR が 0 に落ちた後に、上位の 0 が届く。4 GiB より上のリングでは番地の上位が消え、次のドアベルで別の番地を命令として読む。
- 重さ：当たれば壊れる。
- 直す案：CA は一度の 64 ビット書込みで書き、止まった後に正しいポインターと RCS を CRCR へ書き直す。

### X3：Abort で止まらないときに HCRST しない

- 場所：`xhci.c:651`（`command_wait` の結果を捨てる）。
- 仕様：xHCI 4.6.1.2（xhci.txt:4896）。CRR が 5 秒で落ちなければ HCRST するのがよい (should)。
- 筋書き：制御器が固まる。以後の全コマンドが一つずつ時間切れになる。
- 重さ：当たれば壊れる。
- 直す案：false なら `hc->failed` の道で `hc_restart` する。

### X4：Output Context の EP State で回復の道を決める

- 場所：`xhci.c:685-687`（`endpoint_recover`）、`xhci.c:954-959`（`endpoint_reset`）、`xhci.c:699`。
- 仕様：xHCI 4.8.3（xhci.txt:7933-7949）。EP State は今の状態を映さないことがあり、更新は事象より遅れることがある。ソフトは内部の変数で状態を持つのがよい (should)。4.6.10：Set TR Dequeue は Error か Stopped でだけ行える。
- 筋書き：STALL の事象を受けた時点で Output Context がまだ Running のままである。コードは Stop Endpoint を出し、Context State Error で弾かれる。Set TR Dequeue も弾かれ、`ctl_broken` が立ち、制御パイプが抜き差しまで死ぬ。GET_MAX_LUN や SET_IDLE の STALL は普通に起きる。
- 重さ：当たれば壊れる。
- 直す案：事象の完了コードとコマンドの完了で端点の状態を持ち、Stop の Context State Error は 4.6.9 の手順で読み直す。

### X5：reset a pipe の順が違い、CLEAR_TT_BUFFER が無い

- 場所：`xhci.c:686-689`、`xhci.c:1393`、`xhci.c:981`、`xhci.c:1208-1210`。
- 仕様：xHCI 4.6.8 の Note（xhci.txt:6111-6128）。順は Reset Endpoint、TT の後ろなら CLEAR_TT_BUFFER、ENDPOINT_HALT の解除、Set TR Dequeue、ドアベルである (shall)。4.6.5 の Note（xhci.txt:5306）：Address Device の Transaction Error でも CLEAR_TT_BUFFER を送る (shall)。USB2 11.17.5（usb20.txt:19719）、11.24.2.3。
- 筋書き：HS hub の後ろの FS の装置で bulk がエラーになる。TT の buffer が busy のまま残り、同じ TT の次の転送が詰まる。コード全体に CLEAR_TT_BUFFER が無い。
- 重さ：当たれば壊れる。
- 直す案：回復を仕様の順に並べ、`tt_slot` のある装置の control と bulk には hub へ CLEAR_TT_BUFFER を送る。

### X6：PSI を読まずに Port Speed を固定の表で読む

- 場所：`xhci.c:1359`、`usb.h:15`、`xhci.c:1537-1544`。
- 仕様：xHCI 表 7-13 脚注 120（xhci.txt:26091）。既定の Speed ID は PSIC=0 のときだけ使える。7.2.2。
- 筋書き：PSI で別の PSIV を定める制御器で、EP0 の MPS、Interval の式、TT の判定が全部狂う。
- 重さ：当たれば壊れる（まれ）。
- 直す案：Supported Protocol の PSI を読み、PSIV から速さを引く。

### X7：PAGESIZE を読まない

- 場所：`xhci.c:1582-1591`（Scratchpad に 4 KiB の `dma_page`）。
- 仕様：xHCI 5.4.3（xhci.txt:19573）、4.20（xhci.txt:16241）。Scratchpad は PAGESIZE の大きさで PAGESIZE の境界に置く。
- 筋書き：PAGESIZE が 8 KiB 以上の制御器が Scratchpad の頁を越えて書き、隣のメモリーを壊す。
- 重さ：当たれば壊れる（まれ）。
- 直す案：PAGESIZE を読み、大きさに合わせて確保する。

### X8：複数 TRB の TD で TD Fragment の規則を破る

- 場所：`xhci.c:334-348`（`ring_push`）、`xhci.c:366-390`（`endpoint_push`）。到達は `usbnet.c:290`、`usbnet.c:292`。
- 仕様：xHCI 4.11.7.1（xhci.txt:11422）。最初の TRB を最後に書く。4.11.5.1（xhci.txt:11091）。TD Fragment はセグメントをまたがない。
- 筋書き：NCM の送信が 64 KiB の境界をまたぐ。走っている端点へ順に Cycle を書くので、制御器が TD の途中までを読んで Short か TRB Error で止まる。
- 重さ：当たれば壊れる（まれ）。
- 直す案：最初の TRB の Cycle を最後に書き、Link の前に足りなければ No Op で埋めて Link の後へ送る。

### X9：64 ビットのレジスターを二つの dword で書く

- 場所：`xhci.c:245-248`。
- 仕様：xHCI 5.1.1（xhci.txt:17893）。Qword で書くのがよい (should)。Qword を出せない機械だけが下位から書く。
- 筋書き：x86-64 は Qword を出せる。X2 の窓の元である。
- 重さ：きれいさ。
- 直す案：`mmio_write64` で書く。

### X10：HCH を確かめずに HCRST する

- 場所：`xhci.c:1641-1645`。
- 仕様：xHCI 5.4.1（xhci.txt:19214 付近）。HCH=0 の間は HCRST を立ててはならない (shall not)。
- 筋書き：止まらない制御器へ HCRST を書き、動作が未定義になる。`hc_start` の側（`xhci.c:1555-1560`）は合っている。
- 重さ：当たれば壊れる（まれ）。
- 直す案：`wait_reg` が false なら HCRST を書かずに制御器を諦める。

### X11：拡張 capability の数を 64 で切る

- 場所：`xhci.c:1525`。
- 仕様：xHCI 7。一覧は Next が 0 で終わる。
- 筋書き：65 個目以降の Supported Protocol を読まない。
- 重さ：きれいさ。
- 直す案：MMIO の範囲と Next だけで止める。

### X12：`usb_control` の時間切れが 2 秒である

- 場所：`xhci.c:720`。
- 仕様：USB2 9.2.6.1（usb20.txt:13283）。上限は 5 秒である。
- 筋書き：遅い装置の標準要求が 2-5 秒で正常に終わるのを失敗と読む。
- 重さ：きれいさ。
- 直す案：5 秒にする。

### X13：BAR の注釈が今のコードと違う

- 場所：`xhci.c:1504-1506`、`pci.c:233-236`。
- 仕様：xHCI 4.22.1（handoff）。
- 筋書き：注釈は「firmware が手放した後に測る」と書く。実際は `pci_init` の列挙で測っており、firmware が SMM で動かしている間に memory decoding を切っている。
- 重さ：きれいさ。
- 直す案：注釈を今の形に書き直す。

### X14：PORTSC の PIC を書き戻さない

- 場所：`xhci.c:91`（`PS_KEEP`）。
- 仕様：xHCI 5.4.8 の PIC（bits 15:14）。
- 筋書き：ポートの書込みのたびに PIC が 0 になる。PIND の無い機械では害が無い。
- 重さ：きれいさ。
- 直す案：`PS_KEEP` に PIC を足す。

### X15：Configure Endpoint が失敗しても `xd->ep[dci]` が残る

- 場所：`xhci.c:903`、`xhci.c:912-915`。
- 仕様：xHCI 4.8.3（xhci.txt:7920）。Disabled の端点のドアベルを鳴らしてはならない (shall not)。
- 筋書き：class driver が別の呼出しで同じ端点を見ると鳴らせる。今の呼ぶ側は失敗で止まるので当たらない。
- 重さ：きれいさ。
- 直す案：失敗で `xd->ep[dci]` を NULL に戻す。

### xHCI で合っていた手順

- BIOS handoff の byte 書込み（5.1.1 の Note）、1 秒の待ち（xhci.txt:16435）、USBLEGCTLSTS のマスクを照らした。
- CNR 待ち、HCH の 16 ms（xhci.txt:19172）、HCRST の順を 4.2 で照らした。
- CONFIG、DCBAAP、CRCR の RCS、ERST の順（ERSTBA を最後）を 4.2 で照らした。
- Event Ring の 64 KiB 境界を 6.5 で照らした。
- HCSPARAMS・HCCPARAMS・DBOFF・RTSOFF のビットを 5.3 で照らした。
- MSI-X の IP の自動の消去（xhci.txt:14445）、IMAN、EHB の消し方、IMODI の単位を 4.17、5.5.2 で照らした。
- TRB の型番号とビット位置、Setup・Data・Status の TRT・DIR・IDT を 6.4 で照らした。
- Normal TRB の Length・TD Size・Interrupter Target、ISP と IOC を 4.11.7 で照らした。
- 同じ TD で同じ Interrupter Target を使うこと（xhci.txt:11342）を照らした。
- Link を TD の先頭と末尾に置かないこと（xhci.txt:11363）を照らした。
- Slot Context・Endpoint Context のビット（Route、Context Entries、Root Hub Port、TT、Interval、Max ESIT、Avg TRB）を 6.2.2、6.2.3 で照らした。
- 表 6-12 の Interval の計算を照らした。
- Address Device の Input Context を 4.3.3 で照らした。
- EP0 の MPS の更新と Evaluate、Configure の A0=1・A1=0（xhci.txt:5392）を照らした。
- hub の Slot の項目を最初の Configure で入れること（xhci.txt:4447）を照らした。
- Port の debounce 100 ms、TRSTRCY 10 ms、PP の 20 ms（xhci.txt:19887）を照らした。
- PR を USB2 だけに、WPR を USB3 だけに使うことを 5.4.8 で照らした。
- 完了コードの値、Reset Endpoint の TSP=0、Set TR Dequeue の DCS を 6.4 で照らした。
- Stop の Transfer Event を捨てること、primary ring に全部の Command Completion が来ること（xhci.txt:4817）を照らした。
- CAS は D3 の話（xhci.txt:16108）で、warm reset の欠けとは言えないことを照らした。

## HID（`kernel/src/dev/usbhid.c`、`xhci.c` の `usb_interrupt_start`）

### H1：wMaxPacketSize を超える report を切って別の report として読む

- 場所：`xhci.c:942-945`（TD の長さを `esit` に固定）、`usbhid.c:255-269`。
- 仕様：HID 8.4（hid.txt:2651-2668）。一つの report は複数のトランザクションにまたがってよい。一つの USB 転送に一つの report だけが入る。最長の report は short packet を要らない。
- 筋書き：full-speed の wMaxPacketSize 8 の NKRO キーボードの 16 バイトの report が、8 バイトの TD 二つになる。後半を次の report と読み、report ID の無い装置ではキーの配列を誤る。report ID のある装置では後半の先頭を ID と読む。
- 重さ：当たれば壊れる。
- 直す案：Report Descriptor から最長の入力 report の長さを求め、TD の長さをそれにする。

### H2：Delimiter を扱わない

- 場所：`usbhid.c:392-396`（local の tag 10 が無い）。
- 仕様：HID 6.2.2.8（hid.txt:1938-1947）。parser は Delimiter を扱わなければならない (must)。別名の支持は任意である。
- 筋書き：`Delimiter(開) X, 別名 Delimiter(閉) Y` の descriptor で、usage の並びが X、別名、Y になる。二つ目のコントロールが別名に当たり、Y が三つ目に回る。
- 重さ：当たれば壊れる（まれ）。
- 直す案：Delimiter の組の中は最初の usage だけを `usages[]` に入れる。

### H3：Usage Page を Usage を読んだ時に結合する

- 場所：`usbhid.c:393`。
- 仕様：HID 6.2.2.8（hid.txt:1928）。parser は main item に出会ったときに、最後に宣言された Usage Page と結合する。
- 筋書き：`Usage(x), Usage Page(Button), Input` の descriptor で page を誤る。
- 重さ：きれいさ。
- 直す案：32 ビットの Usage は結合済みとし、16 ビット以下は main item で `p->g.page` と結合する。

### H4：Null 値を動きとして読む

- 場所：`usbhid.c:213-218`（`scale_abs` が範囲外を端に寄せる）、`usbhid.c:209-211`、`usbhid.c:237-249`。
- 仕様：HID 5.10（hid.txt:810-813）。範囲外の値を受けたら、そのコントロールの今の値を変えない。6.2.2.5 の Null state（hid.txt:1333-1345）。
- 筋書き：ペンや touch screen が届かない間に X・Y を範囲外で送る。カーソルが画面の隅へ飛ぶ。相対のマウスでは範囲外の値がそのまま動きになる。
- 重さ：当たれば壊れる。
- 直す案：`value_of` の値が Logical Min-Max の外なら、その項目を更新しない。

### H5：LED の report の形を descriptor から読まない

- 場所：`usbhid.c:412-414`、`usbhid.c:524-527`。
- 仕様：HID 6.2.2.5、7.2.2、付録 F.4（hid.txt:3624）。boot protocol の形は 5 ビットの LED である。report protocol の形は descriptor が決める。
- 筋書き：LED の項目が bit 0 から Caps でない report protocol のキーボードで別の LED が灯る。report ID のあるキーボードには送らない（`usbhid.c:203`、`usbhid.c:524`）。
- 重さ：きれいさ。
- 直す案：Output の item から Caps Lock（LED page 02h）の bit 位置と report ID と長さを記録して送る。

### H6：parser の固定の上限

- 場所：`usbhid.c:281`（Push 4 段）、`usbhid.c:283`（usage 16 個）、`usbhid.c:47`（keyfield 16 個）。
- 仕様：HID 6.2.2.7、6.2.2.8 は上限を定めない。
- 筋書き：17 個以上の usage の並びが切れて、残りのコントロールが最後の usage に当たる。Push の 5 段目が捨てられ、Pop で global がずれる。
- 重さ：きれいさ。
- 直す案：伸びる配列にする。

### H7：配列の POSTFail と ErrorUndefined をキーの解放と読む

- 場所：`usbhid.c:151-166`（usage 1 だけを phantom と読み、2 と 3 は `u >= 4` で捨てる）。
- 仕様：HUT 10 の脚注 9（hut.txt:3084）。usage 1-3 は状態や誤りで、実際のキーではない。
- 筋書き：キーボードが usage 3 を送ると、押されているキーが全部離されたことになる。
- 重さ：きれいさ。
- 直す案：usage 1-3 を同じに扱い、前のキーを保つ。

### HID で合っていた手順

- SET_PROTOCOL の値（0 が boot）、SET_IDLE の wValue、SET_REPORT の wValue 0200h を 7.2 で照らした。
- HID descriptor のバイト位置と Report Descriptor の GET_DESCRIPTOR を 6.2.1、7.1 で照らした。
- item の前置バイト、long item、global・local・main の tag を 6.2.2 で照らした。
- Logical の符号の読み方を 6.2.2.7 で照らした。
- local の消去、最後の usage の繰り返しを 6.2.2.8 で照らした。
- boot のキーボードと mouse の形を付録 B で照らした。
- 第 2 バイトの無視と、設定の後の SET_PROTOCOL を付録 F.4 で照らした。
- ErrorRollOver で前のキーを保つことを HUT 10 で照らした。
- Generic Desktop の Pointer・Mouse・Keyboard・Keypad・X・Y・Wheel、Digitizer の 01h-04h を HUT で照らした。
- usage から set 1 への表のうち HUT の usage の番号を HUT 10 で照らした。set 1 の値に当たる公式の文書は手元に無く、照らせない。

## Bulk-Only Transport（`kernel/src/dev/usbmsc.c`）

### M1：subclass を見ずに SCSI の命令を送る

- 場所：`usbmsc.c:55-57`。
- 仕様：MSC 2 章の表 1（mscoverview.txt:340-371）。subclass はどの命令の定めで話すかを示す。07h は先に交渉が要り、08h は IEEE 1667、04h は UFI である。
- 筋書き：subclass 08h の BOT の interface を SCSI のディスクとして探り、INQUIRY と READ CAPACITY を送る。別の扱いの要る装置を詰まらせる。
- 重さ：当たれば壊れる（まれ）。
- 直す案：subclass が 00h、01h、02h、06h のときだけ受ける。

### M2：LUN を 8 で切る

- 場所：`usbmsc.c:16`、`usbmsc.c:333`。
- 仕様：BOT 3.2（bot.txt:258-263）。LUN の最大は 15 である。
- 筋書き：LUN 8-15 のある読取機で後ろのカードが見えない。
- 重さ：きれいさ。
- 直す案：上限を 15 にする。

### M3：データ段の実際の長さを捨てる

- 場所：`usbmsc.c:88-97`、`usbmsc.c:111`。
- 仕様：BOT 5.3.3、6.7.2。host は CBW の長さと residue の差で意味のあるデータの量を決める。コードは仕様に合う。
- 筋書き：residue を 0 と偽る装置で、受けていない領域を成功として返す。
- 重さ：きれいさ。
- 直す案：`r` と `len - residue` が違えば reset recovery に送る。

### BOT で合っていた手順

- CBW と CSW のバイト位置、署名、31 と 13 の長さを 5.1、5.2 で照らした。
- Reset（21h、FFh）と Get Max LUN（A1h、FEh、長さ 1）を 3.1、3.2 で照らした。
- Get Max LUN の STALL を 0 と読むことを 3.2 で照らした。
- Reset Recovery の順（Reset、IN の HALT 解除、OUT の HALT 解除）を 5.3.4 で照らした。
- CBW の失敗で Reset Recovery を行うことを 5.3.1 で照らした。
- データ段の STALL の解除、CSW の STALL の後の一度の読み直しを 5.3.2、5.3.3、6.7 で照らした。
- 無効な CSW と Phase Error で Reset Recovery を行うことを 5.3.3.1、6.5 で照らした。
- valid の条件（13 バイト、署名、tag）と meaningful の条件（status と residue）を 6.3 で照らした。

## Hub（`kernel/src/dev/usbhub.c`）

### U1：USB 2 の hub の 16 番目以降のポートを見ない

- 場所：`usbhub.c:12`、`usbhub.c:95`、`xhci.c:1261`。
- 仕様：xHCI 6.2.2 脚注 106（xhci.txt:21887）。14 ポートを超える HS・FS の hub では Route String のそのポートの欄を 15 にする。USB2 11.23.2.1 の bNbrPorts に 15 の上限は無い。
- 筋書き：コードは port>15 のときだけ 15 にする。15 ポートの hub のポート 5 は 5 のままで、仕様に外れる。16 ポート以上の hub の後ろの装置は見えない。
- 重さ：当たれば壊れる（まれ）。
- 直す案：USB 2 の hub で bNbrPorts>14 なら欄を 15 にし、`HUB_PORTS` の上限を消し、`pending` を bitmap にする。USB 3 の hub の上限 15 は残す。

### U2：Enhanced SuperSpeed の hub の後ろの速さを常に SuperSpeed とする

- 場所：`usbhub.c:231`。
- 仕様：USB3 10.16.2.6（usb32.txt:25692）の EXT_PORT_STATUS と表 10-15。xHCI 6.2.2（xhci.txt:21880）は Slot Context の Speed を廃止とし、同じ版の 4.5.2（xhci.txt:4403）は下りポートの速さを入れると書く。
- 筋書き：SuperSpeedPlus の装置を Speed 4 として登録する。Interval と EP0 の MPS は同じなので、今のコードで壊れる道は見つけていない。
- 重さ：きれいさ。
- 直す案：hub が SuperSpeedPlus の capability を持てば EXT_PORT_STATUS を読み、Rx Sublink Speed を記録する。

### Hub で合っていた手順

- feature selector と status・change のビットを USB2 表 11-17、11-21、11-22 と USB3 表 10-9、10-13、10-14 で照らした。
- hub descriptor の型（29h、2Ah）、bNbrPorts と bPwrOn2PwrGood の位置、2 ms の単位を USB2 11.23.2.1 と USB3 10.15.2.1 で照らした。
- SET_HUB_DEPTH の番号 12、wValue（根の直下が 0、上限 4）、configured の後に送る順を USB3 10.16.2.9 で照らした。
- TT の think time の読み方と multi TT を選ばない形を USB2 11.23.1 と xHCI 6.2.2 で照らした。
- debounce 100 ms、reset の後の 10 ms を USB2 7.1.7.3、9.2.6.2 で照らした。
- trained の SuperSpeed のリンクが reset なしで enabled なことを USB3 10.3.1.5 で照らした。
- Inactive と Compliance で warm reset を使うことを USB3 10.3.1.6 で照らした。

## 読み切れなかった範囲

- `kernel/src/dev/iommu_amd.c`、`kernel/src/dev/iommu_intel.c`、`kernel/src/dev/pciuser.c` は読んでいない。DMA の道と割り込みの再写像は照らしていない。`iommu.c` は `iommu_msi_message` だけを読んだ。
- `kernel/src/dev/usbnet.c` は TD の長さの確かめだけに読み、CDC の仕様とは照らしていない。
- `kernel/src/dev/input.c` には照らす規格の事実が無い。
- `blk.c` には照らす規格の事実が無い。
- `kernel/src/arch/irq.c` は MSI の vector と message の所だけを読んだ。
- ATA の IDENTIFY の語の位置、ATA の opcode、H2D FIS のバイト位置、SCSI の CDB の位置は、上の無い文書の中身なので照らしていない。
- COMRESET の後のディスクの準備の 10 秒（`ahci.c:34`）と、COMRESET で write cache の設定が保たれるかも同じである。
- FLBAS の bits 6:5（NLBAF が 16 を超える場合）は NVM Command Set の領分で、分からない。NVMe14 では bits 7:5 は予約で、`nvme.c:553` はそれに合う。
- PCIe 6.0 は MSI の節（6.1.4）と Extended Message Data だけを読んだ。
- USB 3.2 は 10 章の hub の要求だけを読んだ。リンクの層は読んでいない。
- HUT 1.7 は起こしていない。1.12 で照らした。
- ここに書いた判断はコードと仕様の読みだけによる。実機でも QEMU でも動かしていない。
