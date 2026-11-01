# 敵対的な仕様の監査：AHCI と NVMe の直しの差分

## 依頼文

あなたは Nocturne OS（独自の OS、カーネルは C、対象は実機）の敵対的な仕様の監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。コミットしない。サブエージェントを出さない。ダウンロードしない。返事も報告も日本語。

持ち主の言葉（そのまま）："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

作業木：D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64（ここを cwd に使う。`git -C` は使わない）。

対象：`git diff 545a102f HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.h kernel/src/dev/pm.c` の差分と、HEAD の ahci.c と nvme.c の全文（kernel/src/dev/blk.c と pm.c は呼ぶ側として読む）。

仕様：`D:\Programes\OS DEV INFO` の AHCI 1.3.1（serial-ata-ahci-spec-rev1-3-1.pdf）、NVMe Base 2.4（NVM-Express-Base-Specification-Revision-2.4-Ratified-2026.07.31.pdf）、NVM Command Set（NVM-Express-NVM-Command-Set-Specification.pdf）、PCIe Transport（NVMe-PCIe-Transport-Specification.pdf）。pdftotext で起こした文字は C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\ にある（ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt）。表が崩れている所は PDF で確かめる。SATA-IO の Serial ATA の文書と T13 の ACS は手元に無い。

仕事：変更された行ごとに原文で照らす。レジスターの順序と値、誤りの回復の手順、割り込みの消し方、reset と初期化の順、PxSCTL・PxDEVSLP の項目、Identify の CNS と CSI の値、Create I/O Queue の順、shutdown の手順と待ち時間、CC の書き方。合格を探さない。仕様に合わないものと、変更で新しく壊れたもの（完了の失い、DMA の残っている buffer の解放、錠の順、spinlock の下の眠り、眠ってはならない所での眠り、競り、記憶の漏れ、止まらない待ち、起こしの失い、繰り返しで止まらない回復）を探す。カーネルの規則は `.claude/rules/kernel.md` と `AGENTS.md` にある。報告書や設計の文書は読まない（docs/ の監査の報告も読まない）。コードと仕様の原文だけで判断する。

各指摘に：ファイル:行、仕様の文書と節と原文の短い要旨（原文を長く写さない）、壊れ方の具体的な筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を一つ。調べて合っていた手順は何を照らしたかを一行で書く。読み切れなかった範囲はそう書く。

報告：`docs/kernel-core-20261101/audit-usbstorage-fix2-storage.md` へ日本語で全文（この依頼文を先頭に）。Python で書くなら `newline='\n'`。最後は SubagentHandback で、指摘の番号と一行の要旨を返す。

## 結論

差分はレジスターの値と順序では AHCI 1.3.1 と NVMe 2.4 に合っている。誤りの回復と身元の扱いに、データを失う欠陥が二つある。

- 指摘 1：致命の報告を次の `settle` が忘れ、壊れたデータの読み出しが成功で返る。
- 指摘 2：身元の分からない装置を呼ぶ側が動かし、前の disk 宛ての書き込みが別の disk へ行く。

証拠の範囲はコードと仕様の静的な読みだけである。ビルド、QEMU、実機は動かしていない。

## 指摘

### 1. 致命の報告を次の `settle` が忘れ、壊れたデータの読み出しが成功で返る

- 場所：`kernel/src/dev/ahci.c:280-303`（`settle`）、`:328-334`（`reap`）、`:704`（`halt`）、`:831`（`command_in` の呼び直し）。
- 重さ：データを失う。
- 仕様：AHCI 1.3.1 6.1.1。データの誤りでは HBA は止まってもよく、続けてもよい。3.3.5 の HBDS は system memory の読み書きの誤りである。6.2.2 は HBDS を回復の要る誤りに数える。
- コード：最初の `settle(p, HBDS)` は致命の枝に入り、`attention` を立てて非 queue の command を残す。保護の語は `overflowed` だけで、HBDS・HBFS・IFS には無い。起こされた待ち手は `looked` が真なので `reap` を呼び、`is` は 0 で `settle(p, 0)` が走る。
- 筋書き：
  1. 非 queue の READ DMA のデータを memory へ書く所で uncorrectable ECC が起き、HBA が HBDS を立てて続ける。
  2. device は誤りを知らないので正常に終わり、PxCI が落ち、PxTFD.ERR は 0 である。
  3. 二度目の `settle(p, 0)` は `done` に command を見つけ、`:295` で `finish(done, DONE)` する。
  4. `command_in` は 0 を返す。buffer の中身は壊れたまま成功になる。
- 同じ欠陥の他の形：
  - NCQ では最初の `settle` の `:292` が、PxSACT の落ちた command を HBDS でも DONE にする。どの tag で誤ったかは分からないので、壊れた tag も DONE に入る。
  - 致命の後に 6.2.1 の D2H（ERR あり）が来ると、二度目は `:301-302` の TFES の枝を通る。`:274-278` が「停止を待つ」と書く command が停止の前に FAILED で返る。
- 直す案：`overflowed` を「致命の報告があった」語に広げて OFS・HBDS・HBFS・IFS で立て、立っている間の `settle` は何も終えず、HBDS の回では `:292` の DONE も出さない。

### 2. 身元の分からない装置を呼ぶ側が動かし、前の disk 宛ての書き込みが別の disk へ行く

- 場所：`kernel/src/dev/ahci.c:1050-1053`（`again` で `had` を残す）、`:1126-1127`（`live` なら見直しを取り消す）、`:646-654`（`enter` の `port_start`）、`:1220-1228`（`ahci_resume`）。
- 重さ：データを失う。この差分で広がった。前の版は IDENTIFY が答えなければ `had` を offline にしていた。
- 仕様：AHCI 1.3.1 6.2.2.3。COMRESET と回復の後に、新しい device が入ったかを数え直す。7.4 も device の差し替えを前提に、再開の前に必要な処置を求める。
- 筋書き：
  1. disk A の port へ disk B を差し替える。PCS で `port_changed` が走る。
  2. B は回転の立ち上げ中で `READY_NS` の 10 秒では BSY が落ちない。`enter` は -1 を返し、DET は 3h なので `again` になる。A は `gone` にならない。
  3. A への書き込みが来る。`command` は `gone` を見ず通り、`enter(p, false)` は `!live` なので `port_start` を呼ぶ。今度は B が ready で `live` が立つ。
  4. IDENTIFY なしで A の LBA の WRITE FPDMA QUEUED が B へ行き、B の内容を上書きする。
  5. worker は `live && p->disk && !gone` を見て `retry_at` を消す。身元は以後も確かめない。
- 同じ欠陥の他の形：
  - sleep の間の差し替えでは、`blk_release` の後に worker と呼ぶ側が同時に起き、呼ぶ側が先なら確かめる前に書く。
  - `p->all` も A の値のままなので、B が A より少ない tag しか持たないと tag の範囲も外れる。
  - `!found`（記憶なし）で `again` になった回も同じである。
- 直す案：`port` に `unverified` の語を置き、`port_changed` が立て、`port_examine` が身元を確定して下ろす。立っている間 `enter` は examiner 以外に -1 を返す。`:1126-1127` の取り消しは消す。

### 3. CSTS.RDY が 0 のまま CC.EN を落とす

- 場所：`kernel/src/dev/nvme.c:331-335`（`controller_stop`）。
- 重さ：当たれば壊れる。
- 仕様：NVMe Base 2.4 Figure 41 の EN。RDY が 0 のときに EN を 1 から 0 にすると結果は未定義である。
- コード：`wait_ready(c, true)` の戻り値を捨て、RDY が立たなくても EN を落とす。続く `wait_ready(c, false)` は RDY が元から 0 なので即座に真を返し、停止の証拠にならない。
- 筋書き：`run` が CSTS.CFS を見て `controller_stop` を呼ぶ。RDY が 0 に落ちていると `wait_ready(true)` は CFS で偽を返し、EN を落とし、`wait_ready(false)` が真を返す。`cut` にならず `queue_clear` が command を FAILED にし、呼ぶ側は bounce buffer を返す。controller が DMA を止めた証拠は無い。bus mastering は開いたままである。
- 直す案：`wait_ready(c, true)` が偽で RDY が 0 なら EN に触れず、`cut` の道（bus mastering を切る）へ進む。

### 4. PxCMD.SUD が 0 の port へ COMRESET を出しうる

- 場所：`kernel/src/dev/ahci.c:589`（`hba_set_up` の `port_force_stop`）、`:452-457`、`:386-394`（`comreset`）。
- 重さ：当たれば壊れる。CAP.SSS のある板で、engine が止まらないときだけ当たる。
- 仕様：AHCI 1.3.1 10.10.1 の表。DET=1h と SUD=0 は Illegal で、結果は不定である。10.4.3 は HBA reset で SUD が 0 に戻ると書く。
- 筋書き：staggered spin-up の板が sleep から戻り、SUD は 0 である。`hba_set_up` の `port_stop` が失敗すると、SUD を立てる前の `comreset` が DET=1h を書く。`port_up` の道は `:516` で先に SUD を立てるので当たらない。
- 直す案：`comreset` の頭で DET に触れる前に SUD を立てる。

### 5. `c->max_xfer` が reset の間に一時だけ大きく見える

- 場所：`kernel/src/dev/nvme.c:491-492`（書く側）、`:788`（`rw` が錠なしで読む）。
- 重さ：当たれば壊れる。窓は狭い。
- 仕様：NVMe Base 2.4 Figure 338 の MDTS。MDTS は command の転送量の上限である。
- 筋書き：thread A の `recover` が `read_controller` で `max_xfer` を 2 MiB に書く。次の行で 128 KiB へ下げる前に、thread B の `rw` が 2 MiB を読む。MDTS を超える command は controller が status で拒み、`DNR` なら I/O 誤りが上へ返る。
- 直す案：局所の変数で計算し、`__atomic_store_n` で一度だけ書き、`rw` は atomic に読む。

### 6. `ndisks++` が adapter の間で競る

- 場所：`kernel/src/dev/ahci.c:1083`。
- 重さ：当たれば壊れる。AHCI の adapter が二つあり、hot plug が重なるときだけ当たる。
- 仕様：仕様の問題ではない。`.claude/rules/kernel.md` の「共有の状態は持ち主の錠で守る」に反する。
- 筋書き：二つの adapter の thread が同時に新しい disk を見つけ、同じ `sataN` の名を付ける。`nvme.c:882` は `__atomic_fetch_add` を使っている。
- 直す案：`nvme.c` と同じ `__atomic_fetch_add` にする。

### 7. 電源を切る前の通知を省く controller がある

- 場所：`kernel/src/dev/nvme.c:944-946`（`nvme_shutdown`）。
- 重さ：きれいさ。利用者のデータは失わない。unsafe shutdown の数が増え、次の起動の準備が遅くなる。
- 仕様：NVMe Base 2.4 Figure 41 の SHN。電源が落ちる前に書くものとされる。3.6.1 は、shutdown 済みの状態が CLR で終わることがあると書く。S3 の電源断の後の controller はこの状態である。
- 筋書き：S3 から戻り、その controller へ command を出さずに電源を切る。`woke` が真なので飛ばし、SHN を書かない。media は使用中の扱いのまま電源が落ちる。`in_use` が偽の controller（`enable` の失敗）も、firmware が有効にしていれば同じである。
- 直す案：`woke` と `in_use` で飛ばすのをやめ、CSTS.SHST が 10b でなければ SHN を書く。

### 8. shutdown の待ちの上限の根拠が仕様に無い

- 場所：`kernel/src/dev/nvme.c:911-912`、`:921`（`shutdown_done`）。
- 重さ：きれいさ。Flush は先に済むので利用者のデータは失わない。
- 仕様：NVMe Base 2.4 3.6.1 は RTD3E の分、または 1 秒を最低の待ちとして勧め、上限を書かない。Figure 338 の RTD3E は「典型の遅れ」で、最悪の値ではない。電源を切ってよいのは SHST が 10b のときである。
- 筋書き：RTD3E が 0 で CAP.TO が 1 の controller は 1.5 秒で打ち切る。SHST が 01b のまま電源が落ちる。`ready_ns` は CC.EN から RDY までの時間で、shutdown の時間ではない。
- 直す案：上限を driver の方針の定数として名前を付け、CAP.TO から導くのをやめ、コメントにも方針と書く。

### 9. `ahci_quiesce` の途中の HBA reset が、止めた port を動かし直す

- 場所：`kernel/src/dev/ahci.c:1205-1207`、`:1203`、`:620`。
- 重さ：きれいさ。失敗の道で、sleep なら中止され、電源断なら害が無い。
- 仕様：AHCI 1.3.1 10.3.1。D3 の前に PxCMD.ST を落とす。コードのコメントは「この後は DMA が無い」と書く。
- 筋書き：port 1 が止まらず `hba_reset` に至る。port 2 の `command` が `hba_set_up` を呼び、止めた port 0 の FRE を再び立てる。SSS の板では spin down 済みの port 0 の disk を回し直す。
- 直す案：`all` が偽になった後は残りの port へ command を出さず、止めるだけにする。

### 10. queue の満杯の時間切れが controller を reset する

- 場所：`kernel/src/dev/nvme.c:377`、`:772-773`。
- 重さ：きれいさ。
- 仕様：仕様の問題ではない。3.7.2 の CLR は出ている command を全部止める。
- 筋書き：CPU の多い機械で queue を共有し、ある thread が 30 秒間 slot を取れない。controller は正常でも `FAILED` になり、`recover` が controller を reset する。
- 直す案：slot の時間切れには別の戻り値を返し、reset しない。

### 11. コメントが実際の動きより強く言っている

- 場所：`kernel/src/dev/ahci.c:21-23`、`:595`。
- 重さ：きれいさ。
- 仕様：AHCI 1.3.1 6.2.2、3.3.17。
- 内容：
  - `:21-23` は、失敗を返すのは DMA の終わった後だと書く。TFES の枝は PxCMD.ST を落とす前に FAILED を返す。6.2.2 はこの状態の HBA が新しい command と DMA Setup を受けないと書くので、動きは許される。誤っているのはコメントである。
  - `:595` は ST を落として書くと 3.3.17 を引く。その条件は DITO・MDAT・DETO のもので、ADSE には書かれていない。
- 直す案：`:21-23` に TFES の例外を書き、`:595` の引用を消す。

### 12. S3 の前の 10.10.3 の手順 2 と 3 を行わない

- 場所：`kernel/src/dev/ahci.c:1191-1215`。
- 重さ：きれいさ。節は Informative である。
- 仕様：AHCI 1.3.1 10.10.3。STANDBY の後に Slumber へ入れ、DET=0h、SUD=0 で listen mode にする。
- 筋書き：コードは STANDBY と engine の停止だけを行う。IPM で Slumber を禁じているので手順 2 は行えない。S3 の間も HBA の電源が保たれて SUD が 1 のまま残る板では、`port_spin_up` が `:564` で戻り、順番の spin-up を飛ばす。
- 直す案：SSS の板に限り、`port_stop` の後に DET が 0h であることを確かめて SUD を落とす。

### 13. NCQ の誤りで常に COMRESET する

- 場所：`kernel/src/dev/ahci.c:300`。
- 重さ：きれいさ。禁じられてはいない。
- 仕様：AHCI 1.3.1 6.2.2.2 の流れは、BSY か DRQ が立つときだけ COMRESET し、そうでなければ READ LOG EXT で原因を読む。10.4.1 は port reset を回復の好ましい手段と書く。
- 筋書き：一つの sector の読み出しの誤りで、queue の command が全部中断され、最大 3 回の COMRESET が起きる。
- 直す案：BSY と DRQ が落ちていれば READ LOG EXT で失敗した tag を読み、その command だけを失敗にする。

### 14. 使わない定義と変数

- 場所：`kernel/src/dev/nvme.c:51`（`SHST_BUSY`）、`:583`・`:597`（`after` は書くだけ）。
- 重さ：きれいさ。
- 直す案：両方を消す。

## 照らして合っていた手順

### AHCI

- `CAP2_SDS`・`CAP2_SADM` は CAP2 の bit 3 と bit 4（3.1.10）。
- `PxDEVSLP` は offset 44h、`ADSE` は bit 0。SDS と SADM が無いときは書かない（3.3.17）。
- `SCTL_NO_POWER_STATES` の 3h と 7h は 3.3.11 の IPM の値。`~0xF0F` は SPD を保つ（10.5）。
- `comreset` は DET=1h、1 ms 以上の待ち、DET=0h、DET=3h の待ち、PxSERR の全 1 の順（10.4.2）。
- `port_force_stop` は CR が 500 ms で落ちなくても COMRESET へ進む（10.4.2）。
- `hba_reset` は AE、HR の順で書き、HR の待ちは 1 秒、reset の後に AE を立て直す（10.1.2、10.4.3）。
- `hba_take` は OOS を立て、BB を 25 ms 見て、立てば 2 秒待つ。BOS が落ちれば終わる（10.6.3、10.6.4）。
- `port_spin_up` は SUD、DET が 1h か 3h、PxSERR、TFD の BSY・DRQ・ERR の順（10.1.1）。
- `hba_set_up` は engine の停止、memory、FRE、PxSERR、PxIS、IS、PxIE、GHC.IE の順（10.1.2）。
- `port_up` の非 reset の道は ST を落とし、CR を待ち、PxSERR と PxIS を消し、BSY か DRQ なら COMRESET し、ST を立てる（6.2.2.1）。
- ST を立てる条件は CR=0、FRE=1、BSY=0、DRQ=0、DET=3h（10.1.2、10.3.1）。
- PxSACT を PxCI より先に書く（3.3.13）。
- `halt` は PxCI と PxSACT を ST を落とす前に読む。ST を落とすと両方が消える（5.3.2.10）。
- PCS と PRCS は PxSERR.DIAG の X と N を映し、PxIS への書き込みでは消えない（3.3.5、7.3）。
- PCS の回復は COMRESET、PCS の消去、致命の回復、数え直しの順（6.2.2.3）。ただし数え直しの前に呼ぶ側が入れる（指摘 2）。
- OFS は全部の command を失敗にし、COMRESET する（6.1.5）。
- 割り込みの消去は PxIS、IS の順（10.7.2.1）。PCS が残っても Figure 22 の MSI の再送で失わない。
- command header の CFL、W、PRDTL、PRD の DBC、table の 128 byte の整列（4.2）。
- spinlock の下の眠りは無い。`may_sleep` が偽の道は `mutex_lock` と `sleep_until` を通らない。`pci_disable_busmaster` は眠らず、読み戻しで posted write を押し出す。
- `blk_hold` の下の `port_start` は他の thread と重ならない。`ahci_worker` は `blk_busy_begin` の内側で動く。

### NVMe

- `CAP_CSS_NVM`・`CAP_CSS_SETS`・`CAP_CSS_NOIO` は bit 37、43、44。CC.CSS は 000b か 110b（3.5.1 手順 3）。
- 順は EN、RDY、Identify 01h、1Ch と FID 19h、07h と 00h・05h・06h・08h、Number of Queues、割り込み、Create CQ、Create SQ（3.5.1 手順 5-11）。
- 割り込みを Number of Queues の後にする（Transport 3.1）。それまでは polling で、単一の割り込みの勧めを超えない。
- CNS の値 00h、01h、02h、05h、06h、07h、08h、1Ch（Figure 336）。CSI は全部 00h（NVM）。
- CNS 1Ch の CNTID の FFFFh は処理する controller を指す（5.2.14.2.19）。
- 一覧の続きは最後の NSID を渡す。0 から始めてよい（5.2.14.2.2、5.2.14.2.7）。
- NSTAT は byte 14、NRDY は bit 0（Figure 346）。
- Number of Queues は CLR ごとに最初の一回が割り当てる。値は 0 始まりで 65534 まで（5.2.30.1.5）。
- FID 19h の IOCSCI は CDW11 の bit 8:0（Figure 494）。
- FLBAS の下位 4 bit と bit 6:5 の FIDXU、16 形式以下で無視、NULBAF は byte 83（NVM Command Set）。
- IOSQES と IOCQES は queue を作る前に書く（Figure 41）。
- `controller_stop` は SHN を 00b にして EN を落とす（Figure 41）。EN=1 の shutdown 後の再開は CLR を通す（3.6.1）。EN を落とす前の RDY の条件は指摘 3。
- shutdown は I/O の停止、Delete SQ、Delete CQ、SHN=01b、SHST=10b の順（3.6.1）。
- SHST の 11b は Reserved で、失敗に扱う（Figure 42）。
- CRTO は offset 68h、CRWMT は bit 15:0、500 ms 単位（Figure 57、3.5.4）。
- `run_polled` は `cq_lock` の下で `busy` を落とす。錠の順は `sq_lock` から `cq_lock` のままである。
- `shutdown_done` は `pause` だけで眠らない。`rtd3e * NS_PER_US` は 64 bit で溢れない。

### pm.c と blk.h

- `finish` の `blk_hold` は `ahci.c`・`nvme.c`・`usbmsc.c` の要求の時間切れで終わる。同じ thread の二度目の呼出しは待たない。
- `flush_disks` の `gone` の読みは atomic で、`blk_get` は範囲の内である。
- `blk.h` の変更は `gone` の追加とコメントだけである。

## 読み切れなかった範囲

- SATA 3.3 と ACS は手元に無い。コードが引く SATA 13.6 の節、IDENTIFY の語の番号、FPDMA の FIS の形、`LINK_NS` と `READY_NS` の値、FLUSH と STANDBY の時間は判定していない。
- `kernel/src/vm/` の block driver は読んでいない。`finish` の `blk_hold` がその driver の要求で終わるかは分からない。
- `msix_enable`・`msi_enable` の中身は読んでいない。polling の間の INTx の pin は PCI Command の Interrupt Disable を立てておらず、同じ pin を共有する handler があるかは分からない。
- `hibernate.c` は `blk_read`・`blk_write` を通ることだけを確かめた。
- PDF は開いていない。Figure 41、336、472 は文字起こしの列が崩れていたが、節の本文と Figure 473 で値を確かめた。
