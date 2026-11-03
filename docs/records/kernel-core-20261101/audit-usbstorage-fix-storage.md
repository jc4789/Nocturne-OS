# 記憶の driver の差分の敵対的な監査

## 依頼文

あなたは Nocturne OS（独自の OS、カーネルは C、対象は実機）の敵対的な仕様の監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。コミットしない。サブエージェントを出さない。

持ち主の言葉（そのまま）："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

作業木：D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64（ここを cwd に使う。`git -C` は使わない）。

対象：`git diff main HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.c kernel/src/dev/blk.h kernel/src/dev/pci.c kernel/src/dev/pci.h kernel/src/vm/ac97.c kernel/src/vm/tulip.c` の差分と、変更後の ahci.c と nvme.c の全文。

仕様：`D:\Programes\OS DEV INFO` の AHCI 1.3.1（serial-ata-ahci-spec-rev1-3-1.pdf）、NVMe Base 2.4（NVM-Express-Base-Specification-Revision-2.4-Ratified-2026.07.31.pdf）、NVM Command Set（NVM-Express-NVM-Command-Set-Specification.pdf）、PCIe Transport（NVMe-PCIe-Transport-Specification.pdf）、PCI Local Bus 3.0、PCIe 6.0。pdftotext で起こした文字は C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\ にある（ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt）。表が崩れている所は PDF で確かめる。SATA-IO の Serial ATA の文書と T13 の ACS は手元に無い。

仕事：レジスターの順序と必須の手順、ビットの位置と意味、待ち時間と時間切れ、バリアと所有権の受け渡し、reset と初期化の順、キューの歯車とドアベル、エラーと回復、割り込みの消し方、Identify の項目の読み方、shutdown の手順を、変更された行ごとに原文で照らす。合格を探さない。仕様に合わないものと、変更で新しく壊れたもの（完了の失い、DMA の残っている buffer の解放、錠の順、眠ってはならない所での眠り、競り、記憶の漏れ）を探す。カーネルの規則は `.claude/rules/kernel.md` と `AGENTS.md` にある。報告書や設計の文書は読まない。コードと仕様の原文だけで判断する。

各指摘に：ファイル:行、仕様の文書と節と原文の短い要旨（原文を長く写さない）、壊れ方の具体的な筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を一つ。調べて合っていた手順は何を照らしたかを一行で書く。読み切れなかった範囲はそう書く。

報告：`docs/records/kernel-core-20261101/audit-usbstorage-fix-storage.md` へ日本語で全文（この依頼文を先頭に）。Python で書くなら `newline='\n'`。返事は SubagentHandback で、指摘の番号と一行の要旨を返す。

## 結論

指摘は 23 ある。データを失うものが 1、当たれば壊れるものが 12、きれいさが 10 である。

- データを失う：5。
- 当たれば壊れる：1、2、3、4、6、7、8、9、10、11、12、15。
- きれいさ：13、14、16、17、18、19、20、21、22、23。

全部が静的な読みである。コードは動かしていない。

## 差分の範囲

- `main` は枝の分岐の後へ進んでいる。分岐点は 55142942 で、HEAD は 545a102f である。
- `git diff main HEAD` の ac97.c の大きな差分と pci.h の `iommu_guest` の行は、main の 4b5fbd9d（High Definition Audio）を逆に見たものである。これらはこの枝の変更ではない。
- この枝自身の変更は `git diff 55142942 HEAD` である。ac97.c と tulip.c の変更は、それぞれ `pci_enable_io(d);` の 1 行だけである。
- 監査はこの範囲で行った。
- この枝は usb.h・usbhid.c・usbhub.c・usbmsc.c・xhci.c も変えている。これらは今回の対象の外である。
- main の ac97.c と tulip.c には `pci_enable_io` が無い。枝を main へ載せるときは、この呼出しを残す。

## 指摘

行番号は HEAD 545a102f のものである。

### 1. enter の wanting の漏れで全 I/O が止まる

- 場所：`kernel/src/dev/ahci.c:606-610`。
- 重さ：当たれば壊れる。
- 仕様：仕様の違反ではない。変更後の全文に残る欠陥である。
- 由来：変更前の enter にも同じ形がある。新しい `port_changed` が live を落とす道を増やした。
- 筋書き：
  1. NCQ のディスクで失敗が起き、live が false になる。
  2. taken が 0 でない間に来た queued の Q と R が、`wanting++` と `wanted=true` を行う。
  3. R が port_start で live を戻す。
  4. Q は `alone || !live` が偽なので else-if へ行く。
  5. 自分の分で `!p->wanting` が偽のままになる。
  6. enter に時間切れは無いので、その port の queued の I/O が全部止まる。
- 案：else-if の前で `if (wanted) { p->wanting--; wanted = false; }` とする。

### 2. 電源断の finish に blk_hold が無い

- 場所：`kernel/src/dev/pm.c:145-150`、`kernel/src/dev/nvme.c:868-869`、`nvme.c:875`、`kernel/src/dev/ahci.c:1080-1081`。
- 重さ：当たれば壊れる。shutdown の取り消しと、quiesce の後の DMA が起きうる。
- 仕様：NVMe Base 2.4 の 3.6.1 は、新しいコマンドを止めてからキューを消すと定める。同じ節は、shutdown の処理中に Controller Reset で無効にすることを勧めない。
- コード：`pm.c` の 144 行のコメントは、割り込みを閉じるのは自分の CPU だけで、他の CPU は核の thread を走らせ続けると書く。`nvme.c` と `ahci.c` のコメントの「disks held」は、この道では偽である。
- 筋書き（NVMe）：
  1. `store` の writer（`fs/store.c:438`）か `keep` の thread（`sys/keep.c:332`）が、他の CPU で書く。
  2. `ready=false` なので run が FAILED を返す。
  3. transfer が recover、enable、controller_stop へ進む。
  4. shutdown の最中か後に EN が 0 になる。
- 由来（NVMe）：これは新しい退行である。古い nvme_shutdown は ready を落とさなかった。
- 筋書き（AHCI）：
  1. live=false を見た他の CPU の呼ぶ側が port_start へ進む。
  2. STANDBY と port_stop の後に COMRESET と ST=1 が起きる。
  3. ahci_worker の `blk_busy_begin` も、holder が無いので通る。
- 案：finish の中で `flush_disks` の前に `blk_hold()` を呼ぶ。

### 3. 一時の失敗でディスクが永久に offline になる

- 場所：`kernel/src/dev/ahci.c:43`、`ahci.c:459-466`、`ahci.c:445`、`ahci.c:971-974`。
- 重さ：当たれば壊れる。I/O が失敗し続ける。
- 仕様：AHCI 1.3.1 の 10.1.1 の手順 7 は、上限を ATA/ATAPI-7 の値に従うとする。その文書は手元に無いので、10 秒の定数の根拠は辿れない。
- 筋書き 1：
  1. 休止の復帰で look_all、port_changed、COMRESET と進む。
  2. 回転の遅い HDD が READY_NS を超える。
  3. port_up が false を返し、port_examine が `had->offline=true` にする。
  4. port_listen は SERR を消した後なので、後で準備ができても事象が来ない。
  5. offline は -2 で返るので、誰も port_start を呼ばない。
- 筋書き 2：
  1. port_up の 445 行の lost の道（HBA reset の直後）で slot が負になり、offline になる。
  2. PxIE=0、GHC.IE=0 のまま残る。
  3. その adapter のディスクが全部 offline なら、hba_set_up は二度と走らない。
- 案：offline は DET が 3 でないときか署名が違うときだけにする。準備待ちの失敗と lost の道は、offline にせず adapter の thread で再試行する。

### 4. shutdown_done が上限なしで回る

- 場所：`kernel/src/dev/nvme.c:847-865`。
- 重さ：当たれば壊れる。機械が止まったままになる。
- 仕様：3.6.1 が勧めるのは最低の待ち時間（RTD3E、0 なら 1 秒）で、上限は定めない。コメントのこの読みは正しい。仕様は終わらない待ちを求めていない。Figure 42 は SHST=11b を予約とする。
- 規則：AGENTS.md は止まらない同期処理を禁じる。変更前には 10 秒の上限があった。
- 筋書き：割り込みを閉じ、S3 では他の CPU を止めた状態で、SHST=01b が続くと永久に pause する。SHST=11b も輪を抜けない。
- 案：上限を置いて false を返し、SHST=11b も失敗に数える。

### 5. live が落ちた装置は flush も shutdown もせず成功を返す

- 場所：`kernel/src/dev/nvme.c:874`、`kernel/src/dev/ahci.c:1090`。
- 重さ：データを失う（当たれば）。
- 仕様：3.6.1 の手順 1 は「有効なら」が条件である。手順 2 の CC.SHN の設定には条件が無い。Figure 41 は電源断の前に SHN を書くよう求める。AHCI 10.10.3 は情報的な節である。
- 筋書き（NVMe）：recover が失敗すると live=false のまま揮発キャッシュが残る。SHN なしで電源が切れ、`all=true` が返る。
- 筋書き（AHCI）：直前のコマンドの失敗で live=false だと、割り込みを閉じた所では port_start ができない。flush も STANDBY も省かれ、`all=true` のままになる。
- 案：NVMe は live に関わらず SHN を書く。両方とも、省いたら `all=false` にする。

### 6. settle が engine を止める前に呼ぶ側へ失敗を返す

- 場所：`kernel/src/dev/ahci.c:268-282`、頭のコメント `ahci.c:19-23`、identify の pmm_free `ahci.c:889`。
- 重さ：当たれば壊れる。解放した頁へ DMA が走りうる。
- 仕様：6.1.1 は、Data FIS のデータ誤りで HBA は止まっても続けてもよいと書く。5.3.16.6 は、ST を消す前に CR が落ちることを任意とする。
- 筋書き：
  1. HBDS が立ち、HBA が転送を続ける。
  2. その間に IDENTIFY が FAILED を受け、頁を返す。
  3. HBA がその頁へ書く。
- 限り：transfer は再試行が port_up を通るので塞がれる。`gone` の 0xFFFFFFFF が一時の読み誤りのときも同じ道である。頭のコメントの主張はこの道で偽である。
- 案：broke の道も attention にし、halt で engine を止めてから FAILED にする。

### 7. overflow でも DONE を返す

- 場所：`kernel/src/dev/ahci.c:272-273`、`ahci.c:299-302`。
- 重さ：当たれば壊れる。切れたデータが成功として渡る。
- 仕様：6.1.5 は overflow を重大な誤りとし、fatal の回復を求める。
- 筋書き：コマンドの終わりと OFS が同じ reap に来ると、IS_ERRORS に OFS が無いので done から DONE になる。issued が空なら attention も立たない。
- 案：`is & IS_OFS` を broke に数え、need_reset を立てる。

### 8. issued が 0 の間の致命の誤りを捨てる

- 場所：`kernel/src/dev/ahci.c:292`、`ahci.c:297`。
- 重さ：当たれば壊れる。30 秒止まる。
- 仕様：6.2.2 と 5.3.16.6 は、ST を消すまで HBA は再開しないと書く。6.1.11 は、活動中の slot が無い DMA Setup FIS で IFS が立つと書く。
- 筋書き：PxIS を消した後に何も記録せず、live=true のまま残る。次のコマンドは IO_NS 待って halt に落ちる。
- 案：issued が無くても `is & IS_ERRORS` なら live=false、need_reset=true にする。

### 9. Controller Reset の書込みに SHN=01b が残る

- 場所：`kernel/src/dev/nvme.c:314`、`nvme.c:877`。
- 重さ：当たれば壊れる（小さい）。
- 仕様：Figure 41 は、shutdown を始める書込みが EN も消すと両方が起きると書く。3.7.2 は、CLR の最中の shutdown の開始は無効か CLR の後に起きると書く。
- 筋書き：休止の snapshot の後の復帰で、awake、enable、controller_stop と進む。controller_stop が `cc & ~CC_EN` を書く。その cc には SHN=01b が残っている。
- 案：`cc & ~(CC_EN | 0xC000u)` を書く。

### 10. IPM=3h は DevSleep を止めない

- 場所：`kernel/src/dev/ahci.c:110`、`ahci.c:352-355`、`ahci.c:533`。
- 重さ：当たれば壊れる（小さい）。
- 仕様：3.3.11 で IPM の 3h は Partial と Slumber だけで、7h が DevSleep も含む。PxDEVSLP.ADSE と CAP2.DESO の節は、DESO=0 ならどの状態からでも DevSleep に入ると書く。7.3.1.1（情報的）は SET FEATURES で device 側の開始も止めるよう挙げる。
- 筋書き：firmware が ADSE=1 を残すと、idle で PhyRdy が変わり PRCS が立つ。port_changed が COMRESET と再検査を繰り返す。
- 案：CAP2.SDS のあるとき IPM=7h にして ADSE を消し、SET FEATURES も発行する。

### 11. 抜いたディスクが休止を永久に拒む

- 場所：`kernel/src/dev/ahci.c:801`、`ahci.c:868`、`kernel/src/dev/pm.c:274`、`kernel/src/dev/blk.c` の blk_persist。
- 重さ：当たれば壊れる。
- 仕様：仕様の違反ではない。変更で入った道である。
- 筋書き：未 persist の書込みを持ったまま offline になると、persist が失敗し、persisted が進まない。`flush_disks` が毎回 false を返す。「A disk did not write its cache back」で休止が拒まれ続ける。
- 案：offline の装置は persist で 0 を返すか、flush の対象から外す。

### 12. 4096 を超える block の namespace が登録される

- 場所：`kernel/src/dev/nvme.c:708`、頭のコメント `nvme.c:13`、`kernel/src/dev/blk.h` の block の説明、`kernel/src/fs/store.c:103`。
- 重さ：当たれば壊れる。
- 仕様：NVM Command Set は LBADS に 4096 の上限を置かない。読み方は仕様に合っている。
- 由来：変更前は shift が 12 を超えるものを外していた。
- 筋書き：呼ぶ側のコメントは 512 か 4096 だけを前提にする。store の GPT の読みは device の block 1 を前提にする。8 KiB の block の namespace は、この前提の外で登録される。
- 限り：store は全部を読んでいない。
- 案：呼ぶ側まで通して確かめるか、上限を戻してコメントを合わせる。

### 13. cut の確かめが遅い

- 場所：`kernel/src/dev/nvme.c:515-516`、`nvme.c:316`。
- 重さ：きれいさ。
- 筋書き：止まらない装置では、要求のたびに controller_stop が reset_lock の下で ready_ns（最大 127.5 秒以上）待ってから cut を見る。
- 案：enable の先頭で cut を見る。

### 14. HR の後に AE=0 のまま他のレジスターに触れる

- 場所：`kernel/src/dev/ahci.c:391-399`、`ahci.c:763`、`ahci.c:1026`、`ahci.c:1096`。
- 重さ：きれいさ。CAP.SAM=0 の HBA では結果が不定である。
- 仕様：3.1.2 と 5.3.2.3 は他のレジスターの前に AE を立てるよう求める。10.4.3 は HR が AE を消すと書く。
- 案：HR が落ちた直後に AE を立て直す。

### 15. 他の呼ぶ側の port_up が need_reset を上書きする

- 場所：`kernel/src/dev/ahci.c:469-472`、`ahci.c:1010-1011`。
- 重さ：当たれば壊れる（まれ）。
- 仕様：6.2.2.3 は PCS を消す前に COMRESET を求める。
- 筋書き：
  1. reset なしの port_up の途中で port_changed が flag を立てる。
  2. port_up の最後に need_reset=false、live=true で flag が消える。
  3. port_listen が SERR を消す。
  4. PCS が COMRESET なしで消える。
- 案：世代の数を比べて、途中で来た要求を残す。

### 16. port_examine の記憶切れで link の割り込みが閉じたまま

- 場所：`kernel/src/dev/ahci.c:948-952`、`ahci.c:294`。
- 重さ：きれいさ。
- 筋書き：PxIE の IS_LINK が戻らず、以後の hot plug の事象が来ない。
- 案：enter より前に確保できなくても port_listen の道を通す。

### 17. A→B→A で別の装置になる

- 場所：`kernel/src/dev/ahci.c:147-148` のコメント、`ahci.c:975-978`。
- 重さ：きれいさ。
- 筋書き：`p->disk` は最後の一つしか持たない。A の構造は offline のまま残り、新しい sataN ができる。コメントと合わない。
- 案：port が見たディスクの一覧を持つ。

### 18. attention が残って次のコマンドを巻き込む

- 場所：`kernel/src/dev/ahci.c:299-302`、`ahci.c:640-644`。
- 重さ：きれいさ。
- 案：待つ者がいなくなった時点で回復の担当へ渡す。

### 19. CAP.CSS.NOIOCSS を見ない

- 場所：`kernel/src/dev/nvme.c:521`、`nvme.c:803`。
- 重さ：きれいさ。
- 仕様：3.5.1 の手順 3.a は、この bit（CAP の bit 44、Figure 36 の PDF で確かめた）があれば CC.CSS を 111b にすると書く。
- 案：先に見て飛ばす。

### 20. 割り込みとキューの順が仕様の「should」と違う

- 場所：`kernel/src/dev/nvme.c:811` と `nvme.c:536`、`nvme.c:548-563`。
- 重さ：きれいさ。
- 仕様：PCIe Transport の 3.1 は、I/O キューの数が決まるまで単一の割り込みを使うよう勧める。3.5.1 の手順 9 も同じ順である。手順 10、11 と Queue Setup の手順 5、6 は CQ の後に SQ を作る順である。
- コード：一対ごとに CQ と SQ を交互に作る。
- 案：Number of Queues の後に vector を増やし、CQ を全部作ってから SQ を作る。

### 21. 3.5.1 の手順 8.b の順と省略

- 場所：`kernel/src/dev/nvme.c:729-732`、`nvme.c:823`。
- 重さ：きれいさ。
- 仕様：namespace の Identify は手順 9 より前に書かれている。コードは手順 8.b.ii.2 の data structure の問い合わせを省くと明言する。
- 案：順を合わせ、問い合わせる。

### 22. run_polled の時間切れで slot が free かつ busy になる

- 場所：`kernel/src/dev/nvme.c:421`。
- 重さ：きれいさ。
- 案：FAILED のときは slot を返さず、次の queue_clear に任せる。

### 23. 古い節の番号

- 場所：`kernel/src/dev/nvme.c:530`。
- 重さ：きれいさ。
- 仕様：コメントの「5.27.1.5」は Base 2.4 では 5.2.30.1.5 である。
- 案：直す。

## 照らして合っていた手順

### AHCI 1.3.1

- HBA と port のレジスターの offset、CAP の S64A・SNCQ・SSS・NCS、GHC の HR・IE・AE を 3.1 と 3.3 で照らした。
- PxIS の bit（PSS 1、SDBS 3、PCS 6、PRCS 22、OFS 24、IFS 27、HBDS 28、HBFS 29、TFES 30）を PDF の 23・24 頁の表で照らした。
- BOHC の bit（BB 4、OOC 3、SOOE 2、OOS 1、BOS 0）を 3.1.11 で照らした。
- PxCMD の bit、PxSCTL の DET・SPD・IPM の位置を 3.3 で照らした。
- 初期化の順（AE、PI、idle、NCS、CLB・FB、FRE、SERR、PxIS、IS、PxIE、GHC.IE）を 10.1.2 で照らした。
- 500 ms の待ちは 10.1.2 の「少なくとも」を満たす。
- 10.1.1 の手順 1〜7 の順を照らした。
- FRE を消す条件、FB の書込みの条件、ST の前の確かめを 10.3.1 と 10.3.2 で照らした。
- COMRESET（1 ms 以上、DET=3 を待つ、SERR を消す、500 ms で固まりと見る）を 10.4.2 で照らした。
- HBA reset（1 秒、HR が残すもの、SUD の扱い）を 10.4.3 で照らした。
- 引き渡し（25 ms、2 秒、BB の次に BOS）を 10.6.3 と 10.6.4 で照らした。
- PxSACT を PxCI の前に書くこと、新しい bit だけ書くことを 5.5.1 で照らした。
- P を立てないことを 5.5.2 で照らした。5.5.3 と 10.7.2.1 も照らした。
- PCS と PRCS が PxIS の書込みで消えず PxSERR で消えることを 3.3.5 で照らした。コードはこれらを閉じてから IS を消している。
- PxSACT を止める前に読むこと、PxSACT が消えたものだけ成功とすることを 6.2.2.1 と 6.2.2.2 で照らした。
- COMRESET が PCS の消去より先であることを 6.2.2.3 と 7.4 で照らした。
- PSS の意味を 3.3.5 と 5.3.9 で照らした。
- コマンド header（CFL=5、W、PRDTL）、PRD の DBC と偶数の条件、CLB の 1024 境界、FB の 256 境界、table の 128 境界、PRD 数の足り方、tag が slot であることを 4.2 と 5.5 で照らした。

### NVMe

- 初期化の手順 1〜7 を Base 2.4 の 3.5.1 で照らした。
- CAP の CSS の bit 37・43、MPSMIN（51:48）、CRMS（60:59）を Figure 36 の PDF で照らした。
- DSTRD、CRWMT、TO の単位を Figure 36 の文字で照らした。
- CC の IOSQES と IOCQES を EN の後に書くことは、CC の前置きと Figure 41 と Queue Setup が許す。コードは I/O キューを作る前に書いている。
- EN と RDY の条件を照らした。
- ドアベルの式を PCIe Transport の Figure 4 で照らした。
- 32 bit のアクセスを PCIe Transport の 3.1.2 で照らした。
- CQE の Phase・CID・Status、SQE の DW の位置、Create の CDW10・CDW11 を照らした。
- Delete の opcode（00h、04h）と SQ が先の順を 5.3.3 と 5.3.4 で照らした。
- Number of Queues（0 基底、CLR の後に再度）を 5.2.30.1.5 で照らした。
- Feature 19h（CDW11 の 8:0、拒否の扱い）を照らした。
- CNS 1Ch と CNTID=FFFFh、CNS 07h と CSI を照らした。
- Identify Controller の offset（MDTS、RTD3E、SQES、CQES、NN、VWC）を照らした。
- FLBAS と NLBAF・NULBAF を Command Set の Figure 114 と 5.5 で照らした。LBADS=0、MS、DPS の扱いも照らした。
- shutdown の順と、shutdown の後の再開（CLR してから EN）、RTD3E が 0 なら 1 秒を 3.6.1 で照らした。
- CFS で Controller Reset とすることを 9.5 と 3.7.2 で照らした。

### 競りと所有権

- queue_clear と submit の順、stops の比較を読んだ。完了の失いは無い。
- slot の数え方を読んだ。free と busy は分かれ、二重に返さない。指摘 22 の時間切れの道だけが例外である。
- list の pmm_free は stop の後である。
- stop_lock は reset_lock の内側で順が揃う。
- spinlock の下で眠る所は無い。
- blk の門にデッドロックは無い。
- `pci_disable_busmaster` の読み戻しは致命の後始末に足りる。

### PCI

- `pci_enable_busmaster` から IO を外したことの呼ぶ側を確かめた。
- port I/O を使うのは ac97 と tulip で、両方に `pci_enable_io` がある。
- e1000、rtl8125、virtio_gpu、iommu_amd、ahci、nvme、xhci はメモリーの BAR である。
- Command レジスターの bit の位置を NVMe PCIe Transport の 3.8.1.2 の表で照らした。

## 読み切れなかった範囲

- SATA 3.3 と ACS は手元に無い。FIS の項目と IDENTIFY の word（69、75、76、83、85、86、87、100-103、106、117-118、230-233）は照らせない。
- LINK_NS と READY_NS と STANDBY の待ち時間も照らせない。
- コードが word 86 の bit 10 と word 69 の bit 3 を見ていないことの是非も照らせない。
- `pci_irq_enable` と irq の内部、`hibernate.c` の全部、`fs/store.c` の全部は読んでいない。
- PCI 3.0 と PCIe 6.0 の本文は読んでいない。Command レジスターは NVMe PCIe Transport の表で照らした。
- 枝の usb.h・usbhid.c・usbhub.c・usbmsc.c・xhci.c の変更は対象の外なので読んでいない。
- コードを動かしていない。
