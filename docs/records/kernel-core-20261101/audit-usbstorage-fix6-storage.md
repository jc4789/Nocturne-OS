# 記憶の driver と PCI の差分の敵対的な監査（六回目）

## 依頼文

Nocturne OS のカーネルの記憶の driver（AHCI、NVMe、block 層）と PCI の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 26b39b07 HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.c kernel/src/dev/blk.h kernel/src/dev/pci.c kernel/src/dev/pci.h`。
- 今のコード：上のファイルと、呼ぶ側 `kernel/src/fs/store.c`、`common/store/volume.c`、`kernel/src/dev/hibernate.c`、`kernel/src/dev/pm.c`、`kernel/src/dev/pciuser.c`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順・眠ってよい所かを調べます。データを失う筋書きと、差分が新しく生んだ行き詰まり（待ちが終わらない、電源の変更が止まる、通常の動きで持続が失敗する）を最初に探します。数える値（given、ended、flushed、written、persisted）は、同時に走る書込みと持続と装置の喪失を組み合わせて、損失を見逃す順と、損失の無いのに立てる順の両方を探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix6-storage.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 10 件です。

| 重さ | 件数 | 番号 |
|---|---|---|
| データを失う | 2 | 1、2 |
| 当たれば壊れる | 6 | 3、4、5、6、7、8 |
| きれいさ | 2 | 9、10 |

- 一番当たりやすいのは指摘 3 です。眠りからの復帰のたびに起こりえて、C: が読み取り専用に落ちます。
- 指摘 3 の欠陥は差分の前からあります。差分は同じ関数を書き換え、この欠陥を残しました。
- 差分が新しく生んだ、終わらない待ちはありません。
- 差分の数え方（given、ended、flushed）そのものは正しく働きます。証明は「合っていた手順」に書きます。
- 指摘 1 と 2 は数え方の外にあります。`blk_cache_lost` を呼ぶべき道で呼んでいません。

行番号は HEAD（682f34dd）のものです。

## 指摘

### 1. driver 自身の COMRESET が cache の喪失に数えられない

- **場所**：`kernel/src/dev/ahci.c:316`（NCQ の誤り）、`:306`（致命の報告）、`:327`（`attention_over`）、`:363`（命令の無いときの誤り）、`:568`（BSY か DRQ）。どれも `:571` の `comreset` へ進みます。比べる先は `:931` と `:1167` です。
- **仕様**：AHCI 1.3.1 の 6.2.2.2 は、BSY か DRQ が立つときに COMRESET を送るとだけ書きます。AHCI 1.3.1 は COMRESET で disk の cache がどうなるかを書きません。その答えは SATA と ACS の文書の領分で、手元の材料に無いので分かりません。
- **差**：`blk.h:66-73` の規則は「cache が残ると言う文書が無いまま装置が始め直したら喪失に数える」です。時間切れの道（`:931`）とリンクの変化の道（`:1167`）はこの規則に従います。上の五つの道は同じ COMRESET を送るのに従いません。`p->unverified` も立たないので、`port_examine` も走りません。
- **筋書き**：
  1. NCQ の書込み W1 と W2 が終わり、disk の cache に載ります。
  2. 別の NCQ の書込みがメディアの誤りで止まります（`settle`、`:316`）。
  3. `transfer` が単独で出し直し、`port_up` が COMRESET を送ります。
  4. disk が cache を捨てるなら、W1 と W2 は消えます。
  5. 次の `blk_persist` は FLUSH CACHE に成功し、`cache_lost` は立ちません。
  6. `volume_write`（`common/store/volume.c:254-260`）は新しい super を書きます。
  7. 媒体の上の最新の commit が、無いブロックを指します。
- **重さ**：データを失う。COMRESET が cache を捨てる disk に限ります。
- **直す案**：ACS と SATA の文書を取り、hardware reset と cache の関係を確かめます。保持を言う文書が無ければ、`comreset()` の頭で `if (p->disk) disk_cache_lost(p->disk)` を呼びます。眠りの後の COMRESET は `flush_disks` の後なので、flushed と given が等しく、誤って立ちません。文書が保持を言うなら、`blk.h` の規則と時間切れの道をその節に合わせて書き直します。

### 2. 自分で下ろしていない CC.EN=0 を健全と読む

- **場所**：`kernel/src/dev/nvme.c:349`、`:356-357`。
- **仕様**：NVMe Base 2.4 の Figure 41（EN）が損失の無さを約束するのは、host が CC.EN を 1 から 0 に書いた Controller Reset だけです。3.7.2 は Controller Level Reset の起こり方として、ほかに NVM Subsystem Reset と Cross-Controller Reset（5.4.1）と transport の reset を挙げます。
- **差**：`controller_stop` は CC.EN が 0、CSTS が正常と読むと、`lost` を false のままにします。`:335-337` のコメントは「健全な reset は失わない」と書きますが、この道では host が reset を書いていません。
- **PCIe で残る範囲**：NVM Subsystem Reset はリンクを Detect へ移します（NVMe over PCIe Transport 3.3）。Conventional Reset と FLR は PCI の register を reset します（同 3.3）。これらは Memory Space Enable が落ちて CSTS が全部 1 に読めるので、`lost` が立ちます。残るのは、PCI の register を reset しない Controller Level Reset です。文書で該当するのは、同じ subsystem の別の controller からの Cross-Controller Reset です。
- **筋書き**：
  1. 書込み W が終わり、cache に載ります。
  2. 別の controller からの reset で、この controller の CC.EN が 0 に落ちます。
  3. 次の命令は答えず、30 秒で時間切れになります。
  4. `controller_stop` は EN=0 を読み、`stopped` を true、`lost` を false にします。
  5. 再開した controller への Flush が成功します。
  6. W が失われていても `cache_lost` は立たず、次の commit が進みます。
- **重さ**：データを失う。複数の controller を持つ subsystem に限るので、まれです。
- **直す案**：`controller_down` の前に `ready` か `live` を読みます。それが立っていて CC.EN が 0 なら、`lost` を true にします。`live` が立っている間 CC.EN を 1 にしているのはこの driver だけです。眠りの後と起動の時は `live` が落ちているので、誤って立ちません。

### 3. enable の間に来た要求が、上がったばかりの controller をもう一度 reset する

- **場所**：`kernel/src/dev/nvme.c:673`（generation を enable の頭で進める）、`:745-752`（`recover`）、`:802`、`:818`、`:859-864`。
- **仕様**：NVMe Base 2.4 の 3.7.2 は、Controller Level Reset で未完了の命令の処理が止まると定めます。`:154-155` のコメントは「最初の呼び手だけが reset する」と主張します。
- **差**：enable の最中に来た要求は、進んだ後の generation を読みます。`ready` が false なので `run` はすぐ FAILED を返します。`recover` は enable の終わりを待ちますが、generation は読んだ値のままです。`moved` が false なので、`ready` が true でも `enable` を呼びます。コメントの主張は成り立ちません。
- **筋書き**（眠りからの復帰）：
  1. T1 が `awake` で `woke` を false にし、enable #1 に入ります。CAP.TO の範囲で、これには時間がかかります。
  2. commit の thread T2 が来ます。`woke` は false なので `awake` を素通りします。
  3. T2 の Flush は `!up` で FAILED になります（1 回目）。
  4. enable #1 が終わります。T2 の `recover` は enable #2 を始めます。
  5. enable #2 の間に別の要求 T3 が来て、同じ道に入ります。
  6. T2 の 2 回目の Flush が controller に入った所で、T3 の enable #3 が controller を止めます。
  7. T2 の Flush は FAILED になり、`blk_nvme_persist` の 2 回の試しが尽きます。
  8. `volume_write` が失敗し、`volume_written` が `STORE_TROUBLE_WRITE` にします。C: は読み取り専用に落ちます。
- **広がり**：要求が途切れない限り、reset は連鎖します。書込みも 3 回の試し（`:818`）が尽きると失敗します。起点は眠りからの復帰、hibernate の復帰、一度の時間切れのどれでもよいです。controller は健全なので `cache_lost` は立たず、媒体は壊れません。最後の commit より後の変更は失われます。
- **重さ**：当たれば壊れる。通常の動きで持続が失敗します。
- **直す案**：`recover` は錠を取った後で先に `ready` を見ます。

  ```c
  bool ok = ready || (moved && !called_off ? false : enable(c));
  ```

  `ready` は全ての stop で落ちるので、錠の下で true なら、呼び手の失敗の後に reset が済んでいます。

### 4. 呼び止められた stop の結果を、殺されていない呼び手が受け取る

- **場所**：`kernel/src/dev/nvme.c:345`、`:384-386`、`:674`。
- **仕様**：`:338-339` と `:743-744` のコメントが「殺された thread が諦めた reset は次の要求が続ける」と主張します。文書の節の問題ではありません。
- **差**：差分は `recover` にこの筋を入れましたが、`enable` の中の `controller_stop` には入れていません。
- **筋書き**：
  1. 殺された thread T1 が、`run` の中から `controller_stop` に入り、stop_lock を持ちます（`reset_lock` は持ちません）。
  2. controller は止まらず、`pci_function_reset` は `reset_cancelled` で諦めます。
  3. T1 の stop の間に別の要求 T2 が来て、`!up` で FAILED になります。
  4. T2 の `recover` は `moved` が false なので `enable` に入ります。
  5. `enable` は `stops` を n と読み、`controller_stop(c, n)` で stop_lock を待ちます。
  6. T1 が `stops` を n+1 にして放します。
  7. T2 は「済んだ stop がある」と読んで何もせず、`c->stopped` の false を受け取ります。
  8. `enable` は false を返し、T2 は殺されていないのに要求を失敗させます。
- **重さ**：当たれば壊れる。T2 が commit の書込みなら C: が読み取り専用に落ちます。controller の故障と thread の kill が重なるときに限ります。
- **直す案**：`enable` は `controller_stop` が false を返し `called_off` が立っているとき、`stops` を読み直してもう一度呼びます。自分も殺されていれば、その呼出しはまた諦めて終わります。

### 5. 他の命令の待ち手が FLR の最中に CSTS を読む

- **場所**：`kernel/src/dev/nvme.c:442`。主張は `:339-341`。
- **仕様**：PCIe 6.0 の 6.6.2 の実装の注は、FLR を行うソフトウェアが他のソフトウェアの直接のアクセスを止めると書きます（手順 1）。FLR の最中に届いた Request は黙って捨ててよいとも書きます。
- **差**：`:339-341` のコメントは、残された FLR へは次の呼出ししか届かないと主張します。stop が終わった後は `queue_clear` で全部の命令が失敗するので、`run` からは届きません。stop が FLR を進めている間は、待ち手の `rd32(c, CSTS)` が届きます。
- **筋書き**：
  1. 同じ頃に出た命令がいくつも時間切れになります。
  2. 最初の待ち手が stop に入り、FLR へ進みます。FLR の待ちは 100 ms から 1 s 以上続きます。
  3. 他の待ち手の期限がこの間に来て、CSTS を読みます。MSI の無い controller では全部の待ち手が 1 ms ごとに読みます（`:452`）。
  4. FLR の前に Memory Space Enable が落ちているので Unsupported Request になり、全部 1 が返ります。
  5. 待ち手は「vanished」と誤った行を出し、stop_lock で待ちます。
  6. FLR の最中の読みに function が答えないと、読みは Completion Timeout まで CPU を止めます。
- **重さ**：当たれば壊れる。データは失いません。
- **直す案**：`up`（I/O は `ready`、admin は `live`）が落ちているなら CSTS を読まず、`s->seq` だけを待ちます。`controller_down` は stop の最初の手で、`queue_clear` が必ず seq を進めます。

### 6. `nvme_shutdown` が残された FLR の function に触れ、眠りを止める

- **場所**：`kernel/src/dev/nvme.c:990`、`:995`、`:997-999`。主張は `:339-341`。
- **仕様**：PCIe 6.0 の 6.6.2 の実装の注の手順 1 です。
- **差**：差分は `controller_stop` に `pci_reset_left` の確かめを入れましたが、`nvme_shutdown` には入れていません。コメントの「register にも configuration にも触れない」は成り立ちません。
- **筋書き**：
  1. 殺された thread の stop が FLR の途中で諦め、record が残ります（`called_off`）。Memory Space Enable は落ちています。
  2. その後に書込みが無いまま、ユーザーが眠りを選びます。`flush_disks` は flush を出しません。
  3. `nvme_shutdown` が CSTS を読み、全部 1 を受け取ります。
  4. `CC` に書き、`shutdown_done` が false を返します。
  5. `in_use` が立ち `woke` が落ちているので `all` が false になり、`machine_sleep` が「nvme did not stop」で眠りをやめます。
  6. 次の眠りは `woke` が立っているので進みます。
- **重さ**：当たれば壊れる。電源の変更が一度止まります。
- **直す案**：`:990` の `continue` の条件に `pci_reset_left(c->pci)` を足します。この controller の cache は stop が既に喪失に数えているので、守る物がありません。

### 7. app の claim が kernel driver の FLR の record を横取りする

- **場所**：`kernel/src/dev/iommu.c:520-524`、`kernel/src/dev/pciuser.c:710-714`、`:690-692`、`kernel/src/dev/pci.c:812-813`、`:1078`。
- **仕様**：`pci.c:812-813` のコメントが「d を reset するのは一者だけ」と主張します。文書の節の問題ではありません。
- **差**：`acs_set` は `f->reset` を見て -N_ERR_NOT_READY と `log->resetting` を返します。`claim_attach` はそれを受け、`m->claimed` と `iommu_in_use` だけを見て `reset_finish` を呼びます。`kernel_owned` は見ません。`group_check` の `kernel_owned` の拒否は、そこより後です。
- **筋書き**：
  1. NVMe が複数の function を持つ装置で、claim する function と同じ group に入ります。`reach` の `acs_set(log, m, …)` は `alone` が false のときだけ m を読みます。
  2. NVMe の FLR の record が生きています。
  3. app の claim が group を作り、NVMe の function を `reset_finish` に渡します。
  4. `pci_function_reset` の後に `pci_irq_disable` が走り、NVMe の vector を解放します。
  5. `c->nvectors` は変わらないので、`run` は割り込みを待ち続け、全部の命令が 30 秒の時間切れになります。
- **付随**：`pci_reset_stuck`（`pci.c:1078`）と `acs_set` の `f->reset` は錠なしで読みます。NVMe の stop が別の CPU で `reset_free` すると、`d->reset->stuck` が解放後の記憶を読みます。
- **重さ**：当たれば壊れる。init 以外の claim は `appdrivers=any` のときだけなので、とてもまれです。
- **直す案**：`claim_attach` で `m->kernel_owned` なら -N_ERR_BUSY を返します。`pci_reset_stuck` は `pci_reset_left` と同じく `reset_lock` の下で読みます。`pci.c:812` のコメントは、その形に合わせます。

### 8. 損失の無いのに `cache_lost` が永久に立つ狭い順

- **場所**：`kernel/src/dev/blk.c:153-155`、`:190-193`、`kernel/src/dev/nvme.c:363-364`、`kernel/src/dev/ahci.c:1167`、`kernel/src/dev/hibernate.c:372`、`:377`。
- **仕様**：NVMe Base 2.4 の 7.2 は、Flush の完了で提出の前に完了した命令が不揮発になると定めます。
- **差と筋書き**（順 A）：
  1. Flush の完了が host に見えます。
  2. `blk_persist` が `count_up_to(&d->flushed, ended)` に届く前に、別の thread の stop が `blk_cache_lost` を呼びます。
  3. `flushed` は古いので `cache_lost` が立ちます。
  4. データは媒体にあるのに、その後の persist は全部失敗します。
- **差と筋書き**（順 B）：
  1. hibernate の image の書込みに失敗し（`hibernate.c:372`、`:377`）、`sleep_now` が `blk_release` へ進みます。
  2. 持ち手は既に port を確かめたので、`unverified` は落ちています。app の書込みは止まらず進みます。
  3. 待っていた adapter の thread が、`look_all` の分の `port_changed` を走らせます。
  4. 書き途中の命令か flush されていない image の書込みがあれば、`port_examine` が `cache_lost` を立てます。
- **重さ**：当たれば壊れる。C: は最後の commit のまま読み取り専用に落ち、媒体は壊れません。順 A の窓は Flush の完了を見てから数えるまでの間で、controller の故障と重なるときだけです。順 B は disk の書込みが既に失敗した後です。
- **直す案**：順 A は残してよい順です。安全の側に倒れるので、`blk.h` のコメントに一文を足します。順 B は、持ち手が確かめた port を `look_all` から外します。

### 9. `command_in` のコメントが `disk_cache_lost` の上に離れている

- **場所**：`kernel/src/dev/ahci.c:872-878`。
- **差**：差分が `disk_cache_lost` を `command_in` のコメントと本体の間に入れました。`:872-874` は今 `disk_cache_lost` のコメントの上にあり、二つのコメントが続きます。
- **重さ**：きれいさ。
- **直す案**：`disk_cache_lost` を `:872` の前へ移します。

### 10. `blk.h` の「driver は呼ばない」と NVMe が合わない

- **場所**：`kernel/src/dev/blk.h:71-72`、`kernel/src/dev/nvme.c:363-364`。
- **差**：コメントは cache の無い装置では driver が `blk_cache_lost` を呼ばないと書きます。NVMe は `c->vwc` を見ずに全部の namespace で呼びます。`persist` が NULL の装置では `blk_persist` が先頭で 0 を返すので、害はありません。given と ended も数え続けます。
- **重さ**：きれいさ。
- **直す案**：コメントを「呼んでも persist の無い装置には効かない」に直すか、`c->vwc` のときだけ呼びます。

## 照らして合っていた手順

### 数える値

- **不変の式**：`flushed <= ended <= given` が常に成り立ちます。`flushed` は `ended` の読んだ値へだけ進み、`ended` は `given` より先に進みません。
- **損失を見逃さない**：`flushed == given` のとき、given の全部の命令が、ある成功した flush の前に終わっています。よって flush が覆います。数は集合ではありませんが、比べるのが等しさだけなので、どの命令が残っても `flushed < given` になります。
- **読む順**：`blk_persist` は `written` を先に、`ended` を後に読みます。`ended` は `written` より先に進むので、`upto` に数えた要求の全部の命令が `ended` に入ります。
- **静かな時の等しさ**：動いている要求が無く `persisted == written` なら、`flushed == given` です。`upto` が全数になった persist が読んだ `ended` は全部の命令を含むからです。私は反例を作れませんでした。
- **flush の省略**：`persisted == upto` で flush を省いても、`flushed` は既に上の値に届いています。誤って立ちません。
- **複数の persist**：`count_up_to` は戻らないので、順が入れ替わっても値は下がりません。
- **失敗した persist**：`r != 0` では何も進めません。
- **読む順の保守性**：`blk_cache_lost` は `flushed` を先に、`given` を後に読みます。間に増えた分は立てる側に倒れます。
- **直接の書込み**：`d->write` の呼び手は `blk_write` だけ、`d->persist` の呼び手は `blk_persist` だけです。
- **消えた `began`**：`began` を読む所は残っていません。
- **喪失の持続**：`cache_lost` は一度立つと落ちません。`blk_persist` は flush の後に見ます。
- **commit の順**：`volume_write` はデータの persist の後で super を書きます。persist が失敗すれば super を書きません。「媒体は最後の完全な commit を保つ」は、`cache_lost` が立った道では成り立ちます。指摘 1 と 2 の道は立たないので別です。

### 眠りと hibernate

- `pm.c` は `blk_hold` の下で `flush_disks` を走らせます。動く要求が無いので `flushed == given` になります。
- `ahci_quiesce` の `flush(d)` は `flushed` を進めませんが、`flush_disks` の後なので害がありません。
- S3 の復帰では `ahci_resume` が `unverified` を立てます。書き手は `port_examine` の後まで待つので、`disk_cache_lost` は誤って立ちません。
- hibernate の snapshot の中の数は flush の後の値です。image の書込みの分は復元で消えます。
- `pm.c:128` は既に立った `cache_lost` で眠りを止めません。コメントのとおりです。
- `flush_disks` は `gone` の装置を飛ばします。戻ったときに `port_examine` が喪失に数えます。

### AHCI

- AHCI 1.3.1 の 3.3.13：PxSACT を PxCI より先に書きます。`blk_write_given` はその前で、装置が命令を見る前に数えます。
- `p->disk` の安定：命令は slot を持ち、`port_examine` は全部の slot が空くまで `p->disk` を変えません。given と ended は同じ disk に数えます。
- `blk_write_ended` は `finish` の後です。fatal の報告では `settle` が `finish` せず、`halt` が engine を止めた後に終えます。
- AHCI 1.3.1 の 7.3：リンクの変化が電源の喪失を語らないというコメントは、節の内容と合います。
- `disk_cache_lost` は `known->cache` を書き換える前に古い値を読みます。cache が on から off に変わる再起動でも喪失に数えます。
- `blk_ahci_persist` の二重コメントの統合に誤りはありません。

### NVMe

- Base 2.4 の 7.2：Flush は提出の前に完了した命令と、指定の namespace に及びます。`ended` は CQE を見た後に進むので、前に読んだ値は全部これに入ります。`writing` は書込みの `run` にだけ渡り、Flush と admin は数えません。
- Figure 41（EN）：Controller Reset は CQE を post した命令の結果を損ないません。`:335-336` のコメントのとおりです。
- Figure 41（EN）：RDY が 0 のときに EN を 1 から 0 にすると未定義です。`controller_stop` は先に RDY を待ちます。
- Figure 41（SHN）と 3.6.1：stop の書込みで SHN を 00b に戻します。enable も SHN を 0 で書きます。
- 3.7.2 と Transport 3.3：FLR は Controller Level Reset です。Controller Reset 以外は PCI の register も reset します。Transport の Figure 50 は NVMe に FLR を必須とします。
- 9.5：CFS には先に Controller Reset を試します。NSSR の代わりに FLR へ進むのは、9.5 が NSSR を避けてよいと書く範囲です。
- `lost` は FLR の前に判定を終え、`blk_cache_lost` は `queue_clear` の前です。動いている書込みは `given > flushed` に残ります。
- `pci_reset_left` が true の道：CC と CSTS を全部 1 と扱い、`lost` と FLR へ進みます。`pci_disable_busmaster` を飛ばします。record のある function の Command は BME が落ちています。
- `recover` の `called_off`：`moved` かつ `ready` が落ちているときに `enable` を呼び、次の stop が `called_off` を書き直します。指摘 3 と 4 の外では意図どおりです。
- 待ちの kill：`sys/wait.h` のとおり kill は待ちを一度だけ終えます。殺された thread の `run` と `wait_ready` は空回りしません。

### PCI

- `pci_reset_left`：`reset_lock` の下で `d->reset` を読みます。呼び手は `controller_stop` で、mutex だけを持ち、spinlock を持ちません。
- `pci.c:845` のコメント：PCIe 6.0 の 7.7.2.5 から 7.7.2.7 は Address と Data の既定を undefined とし、7.7.2.8 は Mask Bit の既定を 1 とします。合っています。
- `msix_entries_again`：Vector Control の予約ビットを保ちます（7.7.2.8）。`state_restore` が Memory Space を開いた後に表へ書きます。
- `reset_quiesce` と 6.6.2 の実装の注の手順 2 から 5：合っています。
- `pci.h:157-158` のコメント変更：`msix_entries_again` の働きと合います。

### 錠と眠り

- `blk_write_given` は `sq_lock` と `p->lock` の下で呼びますが、atomic の加算だけです。
- `blk_write_ended` は錠の外です。
- `controller_stop` は眠るので、`run` と `enable` からだけ呼ばれます。`run_polled` と `nvme_shutdown` は呼びません。
- `reset_lock` は `stop_lock` の下で取ります。逆の順はありません。
- 撃ち落としを起こす呼出しは、差分に増えていません。

## 読み切れなかった範囲

- SATA 3.3 と ACS-3 は手元の材料にありません。COMRESET と cache の関係（指摘 1）、IDENTIFY の word 85 と 87、FLUSH CACHE の範囲は文書で照らせていません。
- `kernel/src/dev/usbmsc.c`、`kernel/src/vm/ata.c`、`kernel/src/vm/hv/storvsc.c` は読んでいません。grep では `blk_cache_lost` も `blk_write_given` も呼びません。USB の disk の始め直しが数えられないことは、この監査の範囲の外です。
- `pci.c` は 780 行から 1480 行と、呼ばれる関数だけを読みました。
- `pciuser.c` と `iommu.c` は reset と claim の周りだけを読みました。
- `store.c` は `blk_*` の呼出しの周りだけ、`volume.c` は commit の周りだけ、`hibernate.c` は disk の周りと復元の終わりだけを読みました。
- AHCI と NVMe の差分の外の手順（初期化の順、PRP、FIS の組み立て）は、差分に触れる範囲だけを照らしました。全部の再監査はしていません。
- ビルド、QEMU、実機の動きは確かめていません。指摘の筋書きはコードの読みから組み立てたもので、再現は取っていません。
