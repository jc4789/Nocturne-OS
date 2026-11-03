# 記憶の driver の差分の監査（第三回）

## 依頼文

Nocturne OS のカーネルの記憶の driver（AHCI、NVMe）の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は作業木の未コミットの変更を含みます。

材料：
- 差分：作業木で `git diff 34ab1dbe -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.c kernel/src/dev/blk.h`。必要なら `git diff 55142942` の全部も。
- 今のコード：`kernel/src/dev/ahci.c`、`nvme.c`、`blk.c`、`blk.h`、`pm.c`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順・眠ってよい所かを調べます。データを失う筋書きを最初に探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix3-storage.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 18 件です。

| 重さ | 数 | 番号 |
|---|---|---|
| データを失う | 4 | D1〜D4 |
| 当たれば壊れる | 8 | B1〜B8 |
| きれいさ | 6 | C1〜C6 |

- 一番重いのは D1 です。HBA reset の間に、別の port の NCQ の命令が成功として返ります。書いていないデータを書いたと報告します。
- 34ab1dbe からの差分が新しく生んだ欠陥は B1、B2、B3、B5、B6、B7、B8、C1 です。
- B1 は必ず当たります。C: が SATA の機械の hibernate は、今の版では毎回失敗します。
- 行番号は今の作業木のものです。

この監査は静的な読みだけです。ビルド、QEMU、実機では何も確かめていません。

## データを失う

### D1. HBA reset の間に、別の port の NCQ の命令が成功で終わる

- **場所**：`kernel/src/dev/ahci.c:453-463`（`hba_reset`）、`:292-305`（`settle`）、`:339-340`（`reap`）、`:860`、`:879-880`（`command_in` の 10 ms ごとの `reap`）。
- **仕様**：AHCI 10.4.3。GHC.HR を立てると、PxFB・PxCLB と HwInit を除く port のレジスターは全部 reset される。3.3.13 と 3.3.14 は、PxSACT と PxCI のビットが消えることを命令の終わりとして定める。
- **差**：`hba_reset` は GHC.HR を書き、最長 1 秒待ち、その後で `port_fail_all` を各 port に呼びます。この間、ほかの port の `fatal` も `halting` も立っていません。`settle` はこの間に PxSACT と PxCI を読んで 0 を受け取り、`done = p->issued` と判断します。
- **当たる筋書き**：
  1. disk が二台あります。port A の engine が止まらず、`port_force_stop` が `hba_reset` へ進みます。
  2. port B では NCQ の write が動いています。その waiter は 10 ms ごとに `reap` を呼びます（polled なら 1 ms ごと）。
  3. reset の間に `reap` が走ります。PxIS は 0、PxSACT は 0、PxCI は 0 です。
  4. `done & ~p->queued` は 0 なので、`settle` の 305 行が `finish(p, done, DONE)` を返します。
  5. write は成功として返ります。データは disk に届いていません。read なら、埋まっていない buffer が成功として返ります。
- 非 queued の命令は助かります。HBA reset 後の PxTFD の 7Fh に ERR のビットがあり、305 行の条件で FAILED になるからです。これは偶然の救いです。
- **重さ**：データを失う。黙って失います。
- **直す案**：`hba_reset` の HR を書く前に、全 port の `p->lock` の下で `fatal = true`、`live = false` を立てます。`settle` は `fatal` で何も終わらせなくなり、`port_fail_all` が全部を FAILED にします。呼ぶ側は COMRESET の後で与え直します。

### D2. 復旧の COMRESET の間に差し替えた disk へ書く

- **場所**：`ahci.c:334`（`!p->resetting` の条件）、`:398-407`（`comreset`）、`:476-484`（`port_listen`）、`:506-549`（`port_up`）。
- **仕様**：AHCI 7.3（挿入は COMINIT で DIAG.X、抜去は PhyRdy の変化で DIAG.N）、7.4（新しい disk が、software が変化を見る前に入ることがある）、6.2.2.3（PCS を見たら fatal の復旧をして、数え直す）。
- **差**：`resetting` の間の link の変化は「自分の COMRESET のもの」として捨てられます。`port_up` は COMRESET で link が上がった後も最長 10 秒、BSY が消えるのを待ちます。その間に起きた DIAG.X と DIAG.N は `port_listen` が PxSERR ごと消します。`unverified` は立ちません。IDENTIFY もしません。
- **当たる筋書き**：
  1. 壊れかけの disk が BSY のままで、queued の命令が失敗します。`transfer` が `port_up` へ進み、COMRESET を送ります。
  2. `wait_reg(PxTFD, …, READY_NS)` の 10 秒の間に、利用者が hot swap の bay で disk を入れ替えます。止まった disk を替えるのは自然な操作です。
  3. 新しい disk が BSY を下げます。`port_up` は成功します。`port_listen` が PCS と PRCS を消します。
  4. 待っていた write が、別の disk の同じ LBA に書かれます。
- `port_up` の失敗の道でも同じ穴があります。`comreset` が false のとき、`port_listen` は disk を gone にせず、worker も起こしません。次の呼ぶ側の `port_up` の間に新しい disk が上がれば、確かめないまま live になります。
- **重さ**：データを失う。別の disk を壊します。
- **直す案**：`comreset` が PxSERR を消した後で、`port_listen` で消す前に PxSERR の DIAG.X と DIAG.N を読みます。立っていれば、`port_listen` の後で `noted` と `unverified` を立てて worker を起こします。`comreset` が false のときと READY を待ち切れなかったときも同じにします。`port_examine` の中の `port_up` ではこの扱いをしません。

### D3. 戻ってきた disk の、失われた cache を persist が成功と言う

- **場所**：`ahci.c:1086-1091`（`known` の disk を戻す）、`blk.c:132-144`（`blk_persist`）。
- **仕様**：文書の差ではありません。7.3 は link の変化を伝えるだけで、device が電源を失ったかは伝えません。筋書きの欠陥です。
- **差**：link が落ちて同じ serial の disk が戻ると、driver は `gone` を false にして続けます。blk の層は何も知りません。`written` と `persisted` はそのままです。
- **当たる筋書き**：
  1. store が write を返され、その data は disk の揮発の cache にあります。まだ persist していません。
  2. 電源の線が緩み、disk が一瞬電源を失います。cache は消えます。
  3. PRCS の後で同じ disk が戻ります。`known` です。
  4. store が FLUSH CACHE を出し、成功します。空の cache の flush だからです。
  5. store は commit の記録を書きます。commit が指す data は媒体にありません。
- **重さ**：データを失う。
- **直す案**：`known` を戻すときに、その時点で `written > persisted` なら blkdev に印を付けます。次の `blk_persist` は一度 `-N_ERR_IO` を返します。store は保存の失敗として扱います。link の揺れだけでも失敗になりますが、失わないほうを取ります。

### D4. `halt` の `settle(p, 0)` が PxIS を読まず、fatal の前に命令を終わらせる

- **場所**：`ahci.c:733`（`halt`）、`:294`、`:305`、`:331`（`reap` の読む順）。
- **仕様**：AHCI 6.1.1（HBDS の後も HBA は続けてよい）、6.2.2（fatal のとき、どの命令が処理されなかったかは software が決める）。コードの 288-290 行のコメントは「報告の後はレジスターで命令を終わらせない」と言います。
- **差**：`halt` は PxIS を見ずに `settle(p, 0)` を呼びます。PxIS に HBFS か HBDS があっても、PxSACT の消えた命令を DONE にします。`port_changed` の `halt(p, 0xFFFFFFFF)` は、その前に `reap` を呼びません。
- `reap` にも同じ形の窓があります。PxIS を先に読み、その後で PxSACT を読みます。間に立った fatal は次の `reap` まで見えません。
- **当たる筋書き**：
  1. NCQ の read の data で HBDS が立ちます。HBA は続け、device の SDB が PxSACT のビットを消します。
  2. 同じ頃に link の変化が来て、worker が `port_changed` へ入ります。割り込みの処理はまだです。
  3. `settle(p, 0)` がその read を DONE にします。呼ぶ側は壊れたデータを受け取ります。
- 窓は狭いです。当たれば黙って壊れます。
- **重さ**：データを失う。
- **直す案**：`settle` の頭で PxIS の `IS_FATAL` を、PxSACT と PxCI を読んだ後にもう一度読み、引数の `is` に足します。ビットは消しません。`reap` の次の回が消します。これで `halt` の道と `reap` の窓の両方が閉じます。

## 当たれば壊れる

### B1. C: が SATA の機械の hibernate は必ず失敗する

- **場所**：`ahci.c:1273-1279`（`ahci_resume` が `unverified` を立てる）、`:660-668`（`enter` が `blk_holding()` で -2）、`:1169`（worker の `blk_busy_begin`）、`pm.c:287-291`、`hibernate.c:168`、`:353`。
- **仕様**：文書の差ではありません。この差分が入れた設計の欠陥です。
- **差**：
  1. `machine_sleep(hibernate_snapshot, …)` は snapshot の後で全装置の `resume` を呼びます。`ahci_resume` が全 port の `unverified` を立てます。
  2. `unverified` を消せるのは worker だけです。worker は `blk_busy_begin` の門で止まります。`sleep_now` が `blk_hold` を持っているからです。
  3. `hibernate_write` は `blk_write` で image を書きます。`enter` は `blk_holding()` を真と見て、すぐ -2 を返します。
  4. `disk_at` が失敗し、「C: could not be written」で hibernate は終わります。
- **当たる筋書き**：C: が AHCI の disk にある機械で、hibernate を選ぶと毎回です。`pm_can_hibernate` は真のままなので、窓からは選べます。
- **重さ**：当たれば壊れる。必ず当たります。data は失いません。
- **直す案**：holder が自分で確かめます。`enter` で `blk_holding()` かつ `may_sleep()` なら、-2 を返さず `port_examine(p)` をその場で呼びます。worker は門で止まっているので、呼ぶ者は一人です。`examining` の引数は今のまま使えます。`port_examine` の中の `blk_register` も眠ってよい所です。

### B2. `unverified` の待ちの 30 秒が、worker の順の処理より短い

- **場所**：`ahci.c:664-666`（`deadline = now + IO_NS`）、`:1170-1171`（worker が port を順に処理）、`:577-586`（`port_spin_up`）、`:537`（`READY_NS`）、`:989`（`identify` の `IO_NS`）。
- **仕様**：AHCI 10.10.2 と 10.1.1（S3 からの復帰では、順に spin up して、各 disk が ready になるのを待つ）。
- **差**：呼ぶ側の待ちは 30 秒で切れ、-2 になり、I/O の失敗として返ります。worker は port を一つずつ処理するので、自分の port の番が来るまでの時間は前の port の数で増えます。disk は健康なのに失敗します。
- **当たる筋書き**：
  - staggered spin-up の板で HDD が 4 台あり、それぞれ 8 秒で ready になります。`hba_set_up` だけで 32 秒かかり、C: の呼ぶ側は S3 の復帰の直後に I/O の失敗を受けます。
  - 普通の板で port 0 の disk が死にかけています。COMRESET で link が上がり、IDENTIFY が 30 秒で切れます。合計 31 秒以上なので、port 1 の C: の呼ぶ側が失敗します。
- コメント（`:651`）の「for as long as a command may take」は、この時間を説明しません。
- **重さ**：当たれば壊れる。
- **直す案**：待ちの期限を固定の時間にしません。worker が自分の port を処理し終えるまで待ちます。worker が動いている限り待ちは終わります。各 `port_changed` は有限の時間で終わるからです。port ごとの確かめを並べて走らせる案もありますが、`h->reset` を取るので順は変わりません。

### B3. 電源の変更の直前の link の変化で、flush が飛ぶ

- **場所**：`ahci.c:337`、`:666`、`:1169`、`pm.c:150-151`、`:277-279`。
- **仕様**：文書の差ではありません。B1 と同じ根です。
- **差**：`reap` が `unverified` を立てた後、worker が `blk_busy_begin` を通る前に `blk_hold` が門を閉めると、worker は `blk_release` まで動けません。
- **当たる筋書き**：
  - 電源を切る直前に link の揺れがあります。`finish` の `flush_disks` と `ahci_quiesce` の flush と STANDBY IMMEDIATE は -2 になります。電源はそのまま切れます。cache は書かれません。HDD は head を退避しません。
  - 門の内側にいる別の thread の request は、`enter` で 30 秒待って失敗します。`blk_hold` もその分だけ待ちます。
  - sleep の場合は「A disk did not write its cache back」で断ります。こちらは安全です。
- 窓は worker が起きるまでの時間で、狭いです。
- **重さ**：当たれば壊れる。
- **直す案**：B1 の案で閉じます。holder が自分で確かめます。割り込みを閉じた後の `ahci_quiesce` では `may_sleep()` が偽なので、`finish` の `cli` の前の `flush_disks` が確かめを済ませます。

### B4. PxSSTS.DET = 1h を「何も無い」と読み、disk を gone にする

- **場所**：`ahci.c:1060-1063`、`:1086-1087`。
- **仕様**：AHCI 3.3.10（DET 1h は device を検出して通信が未確立）、7.3（COMINIT を受けると DET は 1h、PhyRdy が落ちても 1h にしてよい）。
- **差**：`(rd(p, PxSSTS) & 0xF) == DET_UP` だけを「何かある」と読みます。DET 1h は `again` にならず、`had` が gone になります。1060 行のコメントの「no link is nothing there」は DET 1h について誤りです。`DET_FOUND` の定義（117 行）は文書どおりです。
- **当たる筋書き**：S3 の復帰で disk の Phy が遅れ、`comreset` の 1 秒の後も DET が 1h です。`sata0` が gone になり、`unverified` は消えます。呼ぶ側は -2 を受けます。後で PRCS が来て disk は戻りますが、それまでの store の write は失敗します。
- 抜去された port が DET 1h のままのときは、`again` にすると gone になりません。この場合は `RETRY_MOST_NS` の間を置いて見続け、DET が 0 になった時に gone にします。
- **重さ**：当たれば壊れる。
- **直す案**：`again` の条件を `rd(p, PxSSTS) & DET_FOUND` にします。1063 行の分岐の外（slot を持って DET が 3h でない）も `again` にします。

### B5. `port_to_listen` は ST を止めた後で ICC を書く

- **場所**：`ahci.c:1251-1252`（`port_stop` の後で呼ぶ）、`:1222-1226`。
- **仕様**：AHCI 5.3.2.3（P:NotRunning の遷移に PxCMD.ICC は無い）、5.3.2.11（P:Idle の遷移 8 が PM:ICC）、5.3.4.2（ICC は HBA が idle のときに受ける）、10.10.3（STANDBY、ICC、IPM を待つ、DET と SUD）、10.3.1（SUD は ST と CR が 0 のときだけ）。
- **差**：3.3.7 と 8.3.1.2 は Link 層が L_IDLE なら動くと書きます。5.3 の状態機械は P:Idle からしか PM:ICC へ進みません。状態機械に従う HBA は、ST が 0 の port の ICC に反応しません。
- **当たる筋書き**：CAP.SSS の板で S3 に入ります。各 disk の port で IPM が 1h のまま 1 秒の spin を割り込みを閉じて待ち、listen に入らずに戻ります。`all` は真のままなので、誰も気づきません。10.10.3 の目的の、次の起動の順の spin up が消えます。
- **重さ**：当たれば壊れる。
- **直す案**：順を次のように直します。STANDBY の後、ST が 1 のまま SCTL の IPM を緩めて ICC に 6h を書き、IPM を待ちます。その後で `port_stop` をし、DET を 0 に、SUD を 0 にします。port が live でないときは ICC の段を飛ばします。

### B6. CAP.SSC を見ずに Slumber を求める

- **場所**：`ahci.c:125`、`:1223-1224`。`struct hba` に SSC の欄がありません。
- **仕様**：AHCI 3.1.1 の CAP.SSC（bit 14）。0 なら、PxCMD.ICC で Slumber へ移らせてはならず、PxSCTL.IPM は device からの Slumber の要求を断る値にしなければならない。
- **差**：`port_to_listen` は `h->sss` だけを見て、IPM を Slumber 可にし、ICC に 6h を書きます。
- **当たる筋書き**：CAP.SSS が 1 で CAP.SSC が 0 の HBA です。文書が禁じる操作をします。電源の切れる直前の一度です。
- **重さ**：当たれば壊れる。
- **直す案**：`h->ssc` を CAP から読み、0 なら IPM と ICC の段を飛ばして戻ります。device がいる port の SUD はそのままです。

### B7. `controller_stop` は CFS で RDY を待たず、controller を切り捨てる

- **場所**：`nvme.c:308-314`（`wait_ready` の `ready && (csts & CSTS_CFS)`）、`:335-341`、`:345-350`。
- **仕様**：NVMe Base 2.4 Figure 41 の EN（RDY が 0 のときに EN を 1 から 0 にするのは未定義）、Figure 42 の CFS（初期化の間の fatal なら reset 値が 1）、9.5（CFS なら Controller Reset を試みて再初期化する）。
- **差**：
  - 323-324 行のコメントは「RDY is given its time to rise, whatever else the status says」と言います。コードは違います。`wait_ready(c, true)` は CFS を見るとすぐ false を返します。RDY に時間を与えません。
  - EN が 1、RDY が 0、CFS が 1 のとき、`stopped` は false になります。`cut` が立ち、bus mastering が落ち、namespace が全部 gone になります。起動し直すまで戻りません。
- 34ab1dbe の版はこの場合も EN を落としていました。この差分で変わりました。
- **当たる筋書き**：timeout の復旧の `enable` で、controller が RDY を上げる前に CFS を立てます。`enable` は false で戻ります。次の I/O の `recover` が `enable` を呼び、`controller_stop` が即座に `cut` にします。C: が消えます。一度の失敗で永久です。9.5 の reset の試みは一度もありません。
- **重さ**：当たれば壊れる。
- **直す案**：`controller_stop` の待ちは CFS で切らず、`ready_ns` の間 RDY を見ます。それでも RDY が 0 なら、`cut` の前に transport の reset を試みます。3.7.2 は Function Level Reset を CLR の一つに挙げ、`pci_function_reset` があります。それでも戻らなければ `cut` にします。

### B8. `woke` の除外を消したので、EN が 0 の controller で二度目の sleep が断られる

- **場所**：`nvme.c:954`、`:961`、`:920-942`（`shutdown_done` の `shst == SHST_NONE` で false）。
- **仕様**：NVMe Base 2.4 の 3.6.1 は、手順 2 の SHN の書き込みに EN の条件を付けません。完了は EN の値によらないと書きます。Figure 85 は EN 0、RDY 0、SHST 00b を「実装の定めで電源を切ってよい」とします。NVMe 1.4c の 7.6.2 と Figure の SHN は、EN が 0 のときの shutdown を定めません。
- **差**：2.4 の文には合っています。一方で、このファイルは 1.0 の controller まで扱うと書きます（`nvme.c:592`）。EN 0 で SHST を動かさない controller は、`least` の後で `false` になり、`in_use` なので `all` が false になります。
- **当たる筋書き**：
  1. NVMe を二台持つ機械で S3 に入り、起きます。電源を失った controller は EN 0、RDY 0、SHST 00b です。`woke` は真です。
  2. C: でない二台目は I/O が無いので、`woke` のままです。
  3. もう一度 S3 を選びます。`nvme_shutdown` が SHN を書き、RTD3E か 1 秒の間 spin し、false を返します。
  4. `pm_quiesce_devices(true)` は止まり、「nvme did not stop」で sleep は断られます。二台目に I/O が来るまで毎回です。
- 34ab1dbe の版は `woke` を除外していたので、この筋書きはありませんでした。
- **重さ**：当たれば壊れる。data は失いません。その controller には起きた後で何も渡していないからです。
- **直す案**：SHN は今のとおり全部の controller に書きます。`all` を false にする条件に `!woke` を足します。`woke` が真なら、起きた後でその controller に命令を渡していません。

## きれいさ

### C1. `port_to_listen` の listen の条件が足りない

- **場所**：`ahci.c:1222-1228`。
- **仕様**：AHCI 10.10.1（SUD を消すのは device が無いと信じるときだけ、DET が 0h であることを確かめる）、10.10.4（機械のスイッチか cold presence のある port は listen でなく offline）。
- **差**：IPM が 1h でなければ listen へ進みます。DET 1h の port は IPM 0h なので、そのまま SUD を消します。PxCMD.MPSP と PxCMD.CPD も見ません。10.10.3 と 10.10.4 は informative です。
- **当たる筋書き**：電源を切る直前の port だけで、次の `hba_set_up` が SUD を立て直します。害は残りません。
- **直す案**：IPM が 0h のときは DET が 0h のときだけ listen に入ります。MPSP か CPD が立つ port は飛ばします。

### C2. overflow の「none ended well」は文書に無い

- **場所**：`ahci.c:114`、`:289`。
- **仕様**：AHCI 6.1.5（fatal の復旧の手順を取るべき）、6.2.2（OFS は非 fatal に分類）。
- **差**：動きは保守的で問題ありません。全部を失敗にして与え直します。コメントの「その port の命令のどれも正しく終わっていない」は文書の主張ではありません。
- **直す案**：「6.1.5 は fatal の復旧を求めるので、レジスターで命令を終わらせない」と書きます。

### C3. 5.3.1 の引用が違う

- **場所**：`ahci.c:827`、`:849`。
- **仕様**：AHCI 5.3.1 は状態機械の変数の節です。tag と slot の対応と、PxCI の前に PxSACT を書くことは 3.3.13 が定めます。
- **差**：動きは 3.3.13 に合っています。引用だけが違います。
- **直す案**：3.3.13 に直します。

### C4. `hba_set_up` の「nothing is in any port」は厳密には違う

- **場所**：`ahci.c:590-591`、`:750`（`halt` の `port_stop` は `h->reset` の外）、`:388-393`。
- **仕様**：AHCI 3.3.7（ST か CR が 1 の間は FRE を消さない）、10.3.2。
- **差**：別の port の `hba_reset` の後、その port の `halt` がまだ `port_stop` の中にいることがあります。`hba_set_up` が `port_give_memory` で FRE を立てた後で、遅れた `halt` が FRE を消せます。
- **当たる筋書き**：その port の `taken` は残っているので、`port_up` はまだ走りません。ST は 0 です。次の `port_up` は `need_reset` で FRE を立て直します。害は残りません。
- **直す案**：`halt` が `port_stop` の前に `p->h->lost` を見て、立っていれば engine に触りません。コメントもそう書きます。

### C5. `blk.h` の「a driver serializes」は AHCI と NVMe で偽で、RMW は錠が無い

- **場所**：`blk.h:9-10`、`blk.c:114-117`。
- **差**：4 KiB の block の disk で、同じ block の別の 512 byte を二つの thread が同時に書くと、片方の更新が消えます。AHCI の NCQ と NVMe は要求を並べます。
- **当たる筋書き**：今の呼ぶ側では当たりません。store は `STORE_BLOCK` の倍数で書きます。hibernate は他の書き手を門で止めています。`fs/store.c:227` の LBA 0 の書き込みは部分の書き込みですが、並ぶ相手がいません。
- **直す案**：`blk_by_blocks` の RMW の間だけ device の mutex を取ります。`blk.h` の文を今の形に直します。

### C6. MDTS が reset で block より小さくなると `most` が 0 になる

- **場所**：`nvme.c:503`、`:799`、`blk.c:107-110`。
- **差**：`add_namespace` の `(1u << shift) > c->max_xfer` は最初の発見のときだけ見ます。差分の `__atomic_store_n` は、reset で `max_xfer` が変わることを認めています。`most` が 0 なら `n` が 0、`take` が 0 になり、`blk_by_blocks` は止まりません。
- **当たる筋書き**：MDTS の最小は 8 KiB です。16 KiB 以上の block の namespace で、firmware の更新の後の reset だけです。
- **直す案**：`rw` で `most` が 0 なら `-N_ERR_IO` を返します。

## 照らして合っていた手順

### AHCI

- 非 queued の復旧（6.2.2.1）。`settle` が PxCI を読み、`list_stop`、PxSERR、PxIS、BSY か DRQ なら COMRESET、ST の順です。
- NCQ の復旧（6.2.2.2）。PxSACT を読み、停止し、COMRESET します。READ LOG EXT の代わりに一つずつ与え直します。文書は reset をしたときに log を読みません。合っています。
- COMRESET（10.4.2）。DET を 1h にして 1 ms 以上、0h に戻し、3h を待ち、PxSERR を消します。DET の値は 3.3.11 に合います。
- PxSACT と PxCI は COMRESET で消えません（3.3.13、3.3.14）。COMRESET の後で `settle` が誤って DONE にすることはありません。消えるのは ST を消したとき（5.3.2.10）と HBA reset です。後者が D1 です。
- task file の誤りでは PxIE に関わらず HBA が止まります（5.3.16.5、5.3.16.6）。polled の port でも、`ahci.c:23-24` のコメントの主張は成り立ちます。6.1.4 の最初の段落は TFEE を条件に書きますが、同じ節の後の段落と状態機械は無条件に halt します。
- 最小の初期化の順（10.1.2）。AE、PI、idle、CLB と FB、FRE、PxSERR、PxIS の次に IS、PxIE、GHC.IE です。
- ST の前提（10.3.1、10.1.2）。CR が 0、FRE が 1、BSY と DRQ が 0、DET が 3h です。`port_up` の両方の道で成り立ちます。
- FRE と PxFB（3.3.7、10.3.2）。engine の停止の順も合っています。
- staggered spin-up（10.1.1 の手順 2〜7）。ERR も見ています。手順 5 と手順 7 の時間の値は「読み切れなかった範囲」に書きます。
- DET と SUD の組（10.10.1）。`comreset` が DET を 1h にする前に SUD を立てます。`port_to_listen` は DET を 0h にしてから SUD を消します。
- HBA reset（10.4.3）。HR の前の AE、1 秒の限度、後の AE、SUD が 0 に戻ること。
- 手放しの手順（10.6.3、10.6.4）。OOS、BOS を待つ、25 ms の間に BB なら 2 秒以上。
- PCS と PRCS（3.3.5、7.3）。PxSERR を消すまで残るので、`reap` が PxIE を閉じるのは正しいです。
- 不意の COMINIT（6.2.2.3）。先に COMRESET、次に PCS を消し、数え直します。`port_changed` の順です。
- link の変化での停止（7.4）。
- 値とビット。PxCMD の ICC、ALPE、ASP、CR、FR、FRE、POD、SUD、ST（3.3.7）。PxSSTS.IPM の 1h と 6h（3.3.10）。PxSCTL.IPM の 1h、3h、5h、7h（3.3.11）。
- 命令の頭と PRD（4.2.2、4.2.3.3）。CFL、W、PRDTL、CTBA、DBA、DBC の偶数。PRD の 4 MiB。
- 信号の値（3.3.9）。リセット値の FFFFFFFFh を「まだ」と読むのは正しいです。
- `blk_holding`、`p->lock`、`gate_lock` は入れ子になりません。spinlock の下で眠りません。`h->reset` の mutex の下で眠るのは kernel.md に合います。撃ち落としの呼出しはありません。

### NVMe

- 初期化の順（3.5.1 の手順 1〜11）。手順 3 の CSS の選択、手順 7 の Identify、手順 8.a の CNS 1Ch と FID 19h、手順 8.b の CNS 07h と 00h と残りの三つ、手順 9、10、11。
- CC への IOSQES と IOCQES の書き込み（Figure 41）。I/O の queue が無い間です。
- EN の遷移（Figure 41）。0 から 1 は RDY が 0 の後です。AMS、MPS、CSS は EN が 0 の間に、EN を立てる一度の書き込みで入ります。
- shutdown の後の再開（3.6.1、Figure 42）。EN が 1 なら Controller Reset、EN が 0 なら EN と SHN 00b を同じ書き込みでします。`enable` の `wr32(c, CC, CC_EN | …)` は SHN を 00b にします。
- SHN を 00b で書き戻すこと（Figure 41）。EN を落とすのと同じ書き込みで SHN が残れば shutdown も起きるからです。
- shutdown の手順（3.6.1 の 1.a〜2）と Delete の順（5.3.4 の次に 5.3.3）。
- 待ちの下限（3.6.1）。RTD3E、0 なら 1 秒。文書に上限は無く、60 秒は我々の値です。
- `SHST_DONE` の controller を飛ばすこと（3.6.1 の A、B、C）。命令を渡すには必ず `enable` を通るので、完了の報告はその間は有効です。
- SHST の 11b は予約（Figure 42）。
- 全部の controller へ SHN を書くこと（Figure 41 の SHN）。
- CAP のビット（Figure 36）。TO、DSTRD、CSS の NCSS、IOCSS、NOIOCSS、MPSMIN、CRMS。
- CRTO（Figure 57、3.5.4）。CRIME を 0 で使うので CRWMT が要ります。500 ms の単位です。
- Identify の位置（Figure 338）。MDTS 77、RTD3E 88、SQES 512、CQES 513、NN 516、VWC 525。
- CNS 1Ch の CNTID FFFFh（5.2.14.2.19）。FID 19h の IOCSCI（5.2.30.1.18、Figure 494）。値が 0 の組は断られるので、`vectors[i] & 1` で探すのは正しいです。
- Identify Namespace（NVM Command Set Figure 114、5.5）。FLBAS の下 4 ビットと 6:5、合計が 16 以下なら上を無視、NLBAF は 0 基準、NULBAF は 1 基準。
- NSTAT の byte 14（Figure 346）。
- Number of Queues（Figure 472、473）。Create I/O CQ と SQ の dword（5.3.1、5.3.2 の QSIZE、QID、IV、IEN、PC、CQID）。
- 状態の欄の DNR、SCT、Phase（CQE の定義）。doorbell の式（Transport 3.1.2.1、3.1.2.2）。
- 割り込み（Transport 3.1）。queue の数が決まるまで一つにする文に対し、それまでは polled です。文に反しません。
- `run` の slot の待ちに期限が無いこと。queue の中の命令は全部、自分の時間で終わるか controller を止めます。`freed` は `sq_lock` の下で読むので起こしを失いません。
- `controller_stop` と `queue_clear` の順。`sq_lock` を通るので、提出した命令は必ず失敗として終わります。`stops` の読み取りも同じ錠の下です。
- 錠の順 `reset_lock` → `stop_lock` → `sq_lock` → `cq_lock`。

### blk と pm

- `blk_persist` の `written` と `persisted` の順序。persist の始まりより前に数えた write だけを覆います。
- `finish` の二重の `blk_hold`。holder が同じで、`inflight` は 0 です。
- `flush_disks` の `gone` の飛ばし。
- `machine_sleep` の `pci_suspend` は設定空間を写すだけで、SHN の前に電源の状態を変えません（Figure 41 の順に合います）。

## 読み切れなかった範囲

- SATA 3.3 と ACS-3 は手元にありません（`D:\Programes\OS DEV INFO` にも `spec\` にも無い）。次の主張は照らせませんでした。
  - `ahci.c:287`、`:797` の SATA 13.6 の NCQ の誤りの手順と FIS の欄。
  - `fis_make` の FPDMA の count と tag の位置。
  - IDENTIFY の語 60、75、76、83、85、87、100、106、117。
  - `LINK_NS` の 1 秒が COMINIT の待ちとして足りるか（10.1.1 の手順 5 は SATA 2.6 を指します）。
  - `READY_NS` の 10 秒（手順 7 は ATA/ATAPI-7 を指します）。
  - COMRESET で揮発の write cache が残るか。
- pci30.txt と pcie6.txt は開きませんでした。この差分の AHCI と NVMe の行が PCI の文書の値に触れないからです。`pci_irq_enable`、`pci_disable_busmaster`、`pci_function_reset` の中身は監査していません。
- `git diff 55142942` の usb と xhci は範囲外です。`vm/` の `pci_enable_io` の呼出しも見ていません。
- 割り込みの CPU と S3 の復帰の間の配線（`pci_resume`、`irq_resume`）は入口だけ読みました。CPU を止めた後の NVMe の I/O は、CPU 0 が queue 0 と vector 0 を使うので成り立つと読みましたが、`irq_set_cpu` の失敗の道は追っていません。
- hibernate の restore の全体は、`pm_quiesce_devices` を呼ぶ箇所だけ読みました。
- この監査は静的な読みだけです。実機、QEMU、Hyper-V での再現はしていません。D1 と B1 はコードの道で言い切っています。
