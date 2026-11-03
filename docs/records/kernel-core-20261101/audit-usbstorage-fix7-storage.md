# 記憶の driver と PCI の差分の監査（七回目）

## 依頼文

Nocturne OS のカーネルの記憶の driver（AHCI、NVMe、block 層）と PCI の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 682f34dd HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.h kernel/src/dev/pci.c kernel/src/dev/pciuser.c`。
- 今のコード：上のファイルと `kernel/src/dev/blk.c`、呼ぶ側 `kernel/src/dev/pm.c`、`kernel/src/dev/hibernate.c`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

スコープ：この差分の行が生んだ欠陥か、この差分の行そのものの文書との差だけを報告します。差分の外の前からある欠陥は、データを失うものだけを報告します。

やること：差分の全部の行を文書の該当の節と照らし、順序・状態の遷移・誤りの道・競り・錠の順・眠ってよい所かを調べます。通常の動きで持続が失敗する道、待ちが終わらない道、電源の変更が止まる道を最初に探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix7-storage.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 7 件です。

| 重さ | 数 | 指摘 |
|---|---|---|
| データを失う（差分の外） | 1 | 3 |
| 当たれば壊れる | 2 | 1、2 |
| きれいさ | 4 | 4、5、6、7 |

- 差分の行で、データを失う欠陥は見つかりませんでした。
- 待ちが終わらない道と、電源の変更が止まる道は、差分の行にありません。
- 通常の動きで持続が失敗する道は一つあります。指摘 1 です。
- 版は枝の HEAD（`fe71d739`）です。読んだだけで、ビルドと実行はしていません。

## 指摘

### 1. 一度の界面の誤りや NCQ の誤りで、C: が再起動まで読むだけになる

- **場所：** `kernel/src/dev/ahci.c:414`（差分の行）。効くのは `ahci.c:306`、`ahci.c:316`、`ahci.c:327`、`ahci.c:363` が立てる `need_reset` です。
- **重さ：** 当たれば壊れる。
- **仕様：** AHCI 1.3.1 の 6.2.2、6.2.2.1、6.2.2.2、6.1.2、6.1.10。
- **文書の要旨：**
  - 6.2.2 は、fatal の誤りの回復を PxCMD.ST の 0 から 1 への付け直しと定めます。
  - 6.2.2.2 が COMRESET を求めるのは、PxTFD の BSY か DRQ が 1 のときだけです。
  - 6.2.2.2 は、COMRESET をしなかったときの原因の読み方を READ LOG EXT と定めます。
  - 6.2.2 は、機器が止まった状態なら COMRESET なしで転送を再開できると書きます。
- **差：**
  - `settle` は、NCQ の task file error で BSY と DRQ を見ずに `need_reset` を立てます（`ahci.c:316`）。
  - `settle` は、CRC 誤りなどの IFS でも `need_reset` を立てます（`ahci.c:306`）。
  - 前の版で、この COMRESET は `cache_lost` を立てませんでした。立てたのは時間切れと `port_examine` だけです。
  - 今の版の `comreset` は、その度に `disk_cache_lost` を呼びます。
  - `blk_cache_lost` の印は戻りません。以後の `blk_persist` は全部失敗します（`blk.c:157`、`blk.h:33`）。
- **当たる筋書き：**
  1. store の commit が書込みを出します（`common/store/volume.c:252`）。
  2. その書込みの一つが SATA の線の CRC 誤りで IFS になります（6.1.10）。読みの UNC でも同じです。
  3. `settle` が `need_reset` を立てます。
  4. `transfer` が命令を一人で出し直します。`enter` から `port_up` と `comreset` へ進みます。
  5. commit の書込みの分だけ `flushed < given` なので、`cache_lost` が立ちます。
  6. 書込み自体は再送で通ります。device の cache が残っていても、次の `blk_persist` は失敗します。
  7. `volume_written` が `volume_broken` を呼び、C: は「read only from now on」になります（`volume.c:267`）。
  8. 再起動まで保存はできません。hibernation も `disk_flush` で失敗します（`hibernate.c` の `hibernate_write`）。
- **当たる範囲：**
  - store の書込みは commit の中にだけあり、最後に persist します。
  - 当たるのは commit の最初の書込みから最後の persist までの間の誤りです。
  - commit の外の誤りでは `flushed == given` なので印は立ちません。
- **データ：** media には最後の完全な commit が残ります。失うのは、この後の保存の機会です。
- **cache の文書：**
  - AHCI の文字起こしに、device の write cache の記述はありません（`grep -i cache` で確かめました）。
  - SATA と ACS の文書は `D:\Programes\OS DEV INFO` にありません。
  - COMRESET の後に device の cache が残るかは、手元の文書では分かりません。
  - `ahci.c:414` のコメント「no document here」は、手元の文書については正しいです。
- **直す案（原因を消す）：**
  - 6.2.2.2 の流れどおり、NCQ の task file error と IFS を PxCMD.ST の付け直しで回復します。
  - COMRESET は BSY か DRQ が残るときだけにします。
  - NCQ の誤りの解除に READ LOG EXT を出します。命令の番号と log の頁は SATA と ACS の文書で確かめます。手元に無いので、この文書が要ります。
  - `disk_cache_lost` は本当に COMRESET をした所に残します。

### 2. FLR に誰も触れないという約束に、読みと割り込みの隙間が残る

- **場所：** `kernel/src/dev/nvme.c:441` から `nvme.c:451`（差分の行とその続き）、`nvme.c:224`（`reap` の doorbell）。コメントは `nvme.c:339` から `nvme.c:343`、`nvme.c:439` から `nvme.c:440`。
- **重さ：** 当たれば壊れる。頻度は低いです。
- **仕様：** PCIe 6.0 の 6.6.2。
- **文書の要旨：**
  - FLR の間に届いた Request は黙って捨ててよいと定めます。
  - 実装の注の手順 1 は、FLR を行う者が直接触れる他のソフトと同期し、触れさせないことを示します。
- **差：**
  - `run` は `live` か `ready` を読んでから、`rd32(c, CSTS)` まで錠を持ちません（`nvme.c:441`、`nvme.c:451`）。
  - カーネルは横取りされるので、この間で thread が止まります。
  - その間に別の thread の `controller_stop` が FLR へ進めます。
  - 再開した thread の読みは FLR 中の function へ届きます。
  - `nvme_isr` の `reap` も、`ready` を見ずに CQ の doorbell を書きます（`nvme.c:224`）。
- **当たる筋書き：**
  1. A の命令の待ちが `nvme.c:441` を通った直後に、A が横取りされます。
  2. B の命令の時間切れで `controller_stop` が走り、stop が効かず `pci_function_reset` へ進みます。
  3. A が再開して CSTS を読みます。
  4. Memory Space Enable は `reset_quiesce` で切れているので、読みは全部 1 で返るか、Request が捨てられます。
  5. 全部 1 なら A は「vanished」と誤って log に書きます。`controller_stop` は `stops` が古いので何もしません。
  6. Request が捨てられたら、CPU の読みは Root の Completion Timeout まで待ちます。その先の扱いは platform によります。文書は決めていません。
- **doorbell：** 書込みは posted で、捨てられても害はありません。`reap` は `cq_lock` の下で短く、割り込みの遅れが要るので窓は狭いです。残るのはコメントとの差です。
- **直す案：**
  - CSTS の読みを、`admin.cq_lock` の下の `live` か `ready` の確認と一組にします。
  - `reap` は同じ錠の下で、止まっている間は doorbell を書きません。後の `queue_clear` が queue を空にするので、書かなくても失うものはありません。
  - `controller_stop` は `controller_down` の後、`pci_function_reset` の前に、全部の `cq_lock` を一度取って放します。これで読み手が一人も残らないと言えます。
  - 新しい錠は要りません。

### 3. 差分の外：眠りの間に NVMe の SSD が替わっても、同じ disk として書く

- **場所：** `kernel/src/dev/nvme.c:527`（`read_controller`）、`nvme.c:701`（`discovered`）、`kernel/src/dev/pci.c:1469`（`pci_resume` の比較）。
- **重さ：** データを失う。差分の外で、物理の交換が要るので頻度は低いです。
- **仕様：** NVMe 2.4 の 4.5.2（SN と MN）。Identify Controller は Figure 338 です。
- **差：**
  - `pci_resume` は vendor と device の ID だけを比べます。
  - `enable` は Identify Controller を読みますが、SN を比べません。
  - namespace の表と大きさは最初の `enable` のものを使い続けます。
  - AHCI には `same_disk` があり、同じ場面で serial と大きさを比べます（`ahci.c:1111`）。
- **当たる筋書き：**
  1. S3 の間に、同じ型の SSD へ差し替えます。
  2. 起きた後の `awake` が `enable` を通し、`nvme0` はそのまま使えます。
  3. store は前の disk の block の番地で別の SSD へ書き、その SSD の中身を壊します。
- **直す案：**
  - `read_controller` で SN と MN を覚え、2 回目以降に比べます。
  - namespace の NSZE と block の大きさも `enable` で確かめます。
  - 違えば `blk.gone` を立て、`cut` と同じく使わない状態にします。

### 4. `comreset` の説明が関数から離れた

- **場所：** `kernel/src/dev/ahci.c:405` から `ahci.c:413`。
- **重さ：** きれいさ。
- **差：** `disk_cache_lost` が `comreset` の説明と `comreset` の間に入りました。説明は `disk_cache_lost` のものに見えます。
- **直す案：** `disk_cache_lost` を説明の前へ移します。

### 5. 時間切れの `disk_cache_lost` が余り、余分に印を付ける

- **場所：** `kernel/src/dev/ahci.c:932`。差分の外の行ですが、`ahci.c:414` が追加されたことで余りました。
- **重さ：** きれいさ。
- **差：**
  - 時間切れの後の `halt` は `need_reset` を立て、次の `port_up` が `comreset` を通ります。
  - 次の flush はその後にしか出ないので、`ahci.c:414` が同じ印を付けます。
  - 一方で、期限の直後に `settle` が命令を DONE と見ると、COMRESET は起きません（`ahci.c:795`）。
  - このときも `ahci.c:932` は印を付けます。
- **筋書き：** commit の中の遅い命令が期限ちょうどで終わると、reset なしで C: が読むだけになります。
- **直す案：** `ahci.c:932` の呼出しを消します。

### 6. `blk.h` のコメントが失敗の重さを小さく書く

- **場所：** `kernel/src/dev/blk.h:72` から `blk.h:73`、`kernel/src/dev/blk.c:154` から `blk.c:155`。
- **重さ：** きれいさ。
- **差：**
  - コメントは「that may fail a persist」と書きます。
  - 実際は `cache_lost` が戻らないので、以後の全部の persist が失敗します。
  - 「loses nothing」は media のデータについて正しいです。
- **筋書き：**
  1. flush が成功し、`count_up_to(&d->flushed, ...)` の前で thread が横取りされます。
  2. その間に別の命令の誤りで COMRESET か NVMe の失敗が起きます。
  3. 実際は flush 済みでも、C: は読むだけになります。
- **直す案：** コメントを「every persist from then on」と直します。窓は短いので、コードはそのままでよいです。

### 7. 局所変数 `in_use` と field `c->in_use` の意味が違う

- **場所：** `kernel/src/dev/nvme.c:347`、`nvme.c:359`、`nvme.c:151`。
- **重さ：** きれいさ。
- **差：** field は「block device になった」を、局所変数は「admin queue が命令を受けていた」を指します。`nvme_shutdown` は field の方を読みます。
- **直す案：** 局所変数を `was_live` にします。

## 照らして合っていた手順

### AHCI

- `comreset` の手順は 10.4.2 と合います。DET を 1h にして 1 ms 以上待ち、0h に戻し、DET 3h を待ち、PxSERR を全部 1 で消します。`ahci.c:414` の追加は順序を変えません。
- `disk_cache_lost` は atomic だけなので、割り込みを閉じた所からも呼べます。
- HBA の自動の COMRESET（10.4.3、10.10.1 の SUD の立上がり、6.2.2.3 の COMINIT）の後も、`need_reset` か `port_changed` により flush の前に `comreset` を通ります。印は漏れません。
- `given` は `p->lock` の下で `live` のときだけ増えます。`comreset` の呼び手はどれも `live` を落としてあるので、見た後に増える `given` はありません。
- `p->disk` の読みは錠なしですが、ディスクの構造体は解放されず、値はどちらも有効です。
- 眠りの道で `flushed == given` が保たれます。`flush_disks` の後に書く thread は `blk_hold` で止まります。起きた後の `comreset` で印は立ちません。

### block 層

- `flushed` と `given` の比べは正しいです。`flushed` は、flush の開始前に終わった命令の数の下限です。NVMe 2.4 の 7.2 の範囲と合います。
- 書込みの無い `blk_persist` が flush を省いても、`blk_hold` の下では `flushed >= given` が成り立ちます。
- persist の無い device の `cache_lost` は、`blk.c` の先頭の戻りと `pm.c:128` の条件のどちらでも効きません。`blk.h` の新しい文は正しいです。

### NVMe

- `nvme.c:336` のコメントは文書と合います。Figure 41 の EN が無損失を約束するのは、host の Controller Reset が起こす CLR だけです。
- 3.7.2 は、Controller Reset 以外の CLR で通信がすぐ失われると書きます。Transport 3.3 は FLR を CLR に数えます。
- `nvme.c:359` の判定は正しいです。使用中に EN が 0 なのは host が起こしていない CLR で、3.7.2 の他の方法に当たります。
- 通常の道で `nvme.c:359` は立ちません。眠りと hibernation では `nvme_shutdown` が先に `live` を落とし、前の stop や失敗した `enable` の後も `live` は偽です。
- `nvme.c:441` の待ちは終わります。`ready` や `live` を落とす者を全部調べました。
  - `controller_stop` は必ず `queue_clear` まで進みます。kill されても同じです。
  - `enable` の失敗の道では、I/O も admin も中に命令がありません。
  - `nvme_shutdown` は `blk_hold` の下で、中に命令がありません。
- kill された thread の待ちも回り続けません。`sys/wait.h` のとおり kill は一回の待ちだけを終えます。
- `enable` の二度目の `controller_stop` は正しいです。前の thread の called off を受けたときだけ走り、自分が kill されていれば `pci_function_reset` はすぐ戻ります。
- `recover` の新しい式は、前の版の無駄な reset を消します。`enable` の途中で `generation` を読んだ呼び手が、成功の後にもう一度 reset する道でした。ほかの場合の結果は前と同じです。
- `nvme_shutdown` の `pci_reset_left` の飛ばしは正しいです。
  - 記録が残る stop は `lost` を数え終えています。
  - `blk_hold` の下で進行中の stop はありません。
  - `live` は stop の中で落ちています。
  - `all` は変わらないので、電源の変更は止まりません。
  - 割り込みを閉じて `reset_lock` を取っても、`nvme_shutdown` は他の錠を持ちません。
- 9.5（CFS のときの Controller Reset）と Figure 41（RDY が 0 のときに EN を落とさない）は、`controller_stop` の順序と合います。

### PCI

- `pci_reset_stuck` の `reset_lock` は、`reset_free` との解放の競りを閉じます。呼び手は `iommu.c:515` と `pciuser.c` で、どちらも mutex だけを持ち、spinlock を持ちません。
- `pciuser.c:710` の `kernel_owned` の確認は必要で十分です。
  - `acs_set` は、kernel の driver の function に記録があると `-N_ERR_NOT_READY` と m を返します。
  - この確認が無いと、アプリの claim が driver の FLR を引き継ぎます。
  - `group_check` は記録が無いときだけ後で弾きます。
- `pci.c:812` のコメントは正しいです。`pci_function_reset` の呼び手は `nvme.c:364` と `pciuser.c` の `reset_finish` だけです。
  - `reset_finish` の相手は `claim_attach` の m、`claim_recover` の domain の member、`d` です。どれも `kernel_owned` ではありません。
  - `bus_reset` も `kernel_owned` を拒みます（`pciuser.c:667`）。
- 6.6.2 の FLR の手順は `reset_attempt` と合います。Command の消去、Transactions Pending の待ち、100 ms、RRS の扱いです。この差分はそこを変えていません。

## 読み切れなかった範囲

- SATA と ACS の文書は手元にありません。COMRESET の後の cache の扱い、READ LOG EXT の中身、IDENTIFY の語の定義は照らせません。
- 実機と QEMU では動かしていません。指摘 2 の読みの結果は、platform の扱いによります。
- `acs_set`（`iommu.c:513`）は、kernel の driver の function の ACS を `f->reset` の確認の後に錠なしで読み書きします。FLR と競る道です。差分の外でデータを失わないので、指摘にしません。
- `smp_suspend`、`acpi` の眠りの道、`store` の上の層は、呼ばれる順と持続の失敗の扱いだけを読みました。`nvmcmd.txt` と `pci30.txt` は、この差分の行に関わる箇所がありません。
