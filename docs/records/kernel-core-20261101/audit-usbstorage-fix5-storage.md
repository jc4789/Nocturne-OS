# 記憶の driver と PCI の差分の敵対的な監査（883c546d から 26b39b07）

## 依頼文

Nocturne OS のカーネルの記憶の driver（AHCI、NVMe、block 層）と PCI の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 883c546d HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.c kernel/src/dev/blk.h kernel/src/dev/pm.c kernel/src/dev/pci.c kernel/src/dev/pci.h kernel/src/arch/irq.c kernel/src/arch/irq.h`。
- 今のコード：上のファイルと、呼ぶ側 `kernel/src/fs/store.c`、`common/store/volume.c`、`kernel/src/dev/hibernate.c`、`kernel/src/dev/pciuser.c`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順・眠ってよい所かを調べます。データを失う筋書きと、差分が新しく生んだ行き詰まり（待ちが終わらない、電源の変更が止まる、通常の動きで持続が失敗する）を最初に探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix5-storage.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

- 調べた版は枝の HEAD 26b39b07 で、差分は 9 ファイルです。実機も QEMU も動かしていません。全部がコードと文書を読んだ結果です。
- 指摘は 5 件です。データを失うものは 0 件、当たれば壊れるものは 4 件、きれいさは 1 件です。
- 一番重いのは指摘 1 です。SATA の C: では、休止の状態（hibernation）が毎回失敗し、その後 C: が次の起動まで読むだけになります。
- 指摘 1 と指摘 2 は、この差分が新しく生みました。比べたのは 883c546d と 26b39b07 のコードです。

## 指摘

### 指摘 1：まだ装置へ渡していない書込みが、キャッシュを失った数に入る

- 重さ：当たれば壊れる。通常の動きで持続が失敗します。この差分が生みました。
- 場所：
  - `kernel/src/dev/blk.c:95`（`began` を増やす所）
  - `kernel/src/dev/blk.c:181`（`blk_cache_lost` の比べ）
  - `kernel/src/dev/ahci.c:1160`（`port_examine` の呼出し）
  - `kernel/src/dev/ahci.c:707`（`enter` の `unverified` の待ち）
  - `kernel/src/dev/hibernate.c:335`・`:372`（像の書込みと `disk_flush`）
  - `kernel/src/dev/pm.c:292`（`hibernate_write`）
- 仕様の節：文書の違反ではありません。`kernel/src/dev/blk.h:27`・`:65` の約束との差です。AHCI 7.3 は、リンクが落ちた理由が分からないことを書いています。その読みの扱いは合っています。
- 差：
  - `blk.h` は `began` を「装置へ渡した書込み」と書いています。
  - コードは `blk_write` の入口で、driver を呼ぶ前に `began` を増やします。
  - AHCI の書込みは、その後で `enter` の中で `unverified` が解けるのを待ちます。
  - その待ちの間に `port_examine` が `blk_cache_lost` を呼びます。
  - `persisted < began` が成り立ち、`cache_lost` が永久に立ちます。
  - 装置はその書込みを一度も受けていません。失うものはありません。

筋書き A（確実に当たる）：C: が SATA の機械で休止の状態に入ります。

1. `sleep_now` が `blk_hold` の後で `flush_disks` を呼びます。ここで `persisted == written == began` になります。
2. `machine_sleep(hibernate_snapshot, 4, ...)` が走ります。戻る道で `ahci_resume` が全部の port に `unverified` を立てます。
3. `hibernate_write` の最初の disk の操作は `disk_at` の書込みです。ほかの thread は門で止まっています。
4. `blk_write` が `began` を増やします。`began == persisted + 1` です。
5. `command` から `enter` へ入ります。持ち主は自分なので、`port_changed` から `port_examine` を自分で走らせます。
6. 同じ disk と分かり、`ahci.c:1160` が `blk_cache_lost` を呼びます。`cache_lost` が立ちます。
7. `hibernate.c:372` の `disk_flush` は、flush が成功しても `-N_ERR_IO` を返します。
8. 利用者には「Hibernation failed」が出ます。
9. その後の `hibernate_cleanup` の変更を含め、次の commit の `blk_persist` が失敗します。`volume_written` が `volume_broken` を呼び、C: は次の起動まで読むだけです。

筋書き B（眠りからの復帰）：C: が SATA の機械で S3 から戻ります。

1. `blk_release` の後、adapter の thread が port を調べます。
2. この間に書込みが一つでも始まれば、筋書き A の 4 から 6 と同じことが起きます。
3. 窓の長さは COMRESET から IDENTIFY までです。回っている disk では spin-up を待つので数秒です。
4. 書き手の遅れは `WRITE_DELAY_NS` の 5 秒です。`store.c:929` の同期の commit は遅れません。
5. HDD では普通に重なります。

筋書き C：一度のリンクの変化や adapter の reset の後の調べでも同じです。そのとき実際に書込みが待っていれば当たります。

883c546d では比べの相手が `written` でした。待っている書込みは `written` に入らないので、筋書き A も B も当たりません。

直す案：

- 「装置へ渡した」を、渡す所で数えます。
- AHCI では disk ごとに `unflushed` の印を持ちます。`command_in` が書込みを `PxCI` に入れる前に立てます。
- flush は port を一人で持つので、flush の成功で消せます。その間に別の書込みはありません。
- `port_examine` と `command_in` の時間切れは、`unflushed` のときだけ `blk_cache_lost` を呼びます。
- これで指摘 2 も同じ所で直ります。
- `began` は NVMe の `controller_stop` のためだけに残すか、NVMe も `submit` の所で数える形に換えます。

### 指摘 2：書込みキャッシュの無い SATA の disk でも、持続が永久に失敗する

- 重さ：当たれば壊れる。この差分が生みました。
- 場所：`kernel/src/dev/ahci.c:1194`（`persist` を必ず付ける）、`:1160`、`:925`、`kernel/src/dev/blk.c:152`、`:181`。
- 仕様の節：文書の違反ではありません。`blk.h:35` と `:62` の約束との差です。`blk_cache_lost` の前提は「揮発のキャッシュが持っていたものを失う」です。
- 差：
  - 883c546d では、IDENTIFY の word 85 がキャッシュ無しと言う disk は `persist` が NULL でした。`blk_persist` は 0 を返していました。
  - 今は全部の SATA の disk に `blk_ahci_persist` が付きます。
  - `blk_cache_lost` は `d->cache` を見ません。
- 筋書き：
  1. キャッシュを切った disk で、書込みと persist の間にリンクが一度変わります。または 30 秒の時間切れが一度起きます。
  2. `cache_lost` が立ちます。
  3. 書いたものは全部、媒体に届いています。
  4. それでも以後の persist は全部失敗し、C: は次の起動まで読むだけです。
- 直す案：
  - `ahci.c:1160` では、更新する前の `known->cache` が真のときだけ `blk_cache_lost` を呼びます。
  - `ahci.c:925` も `p->disk->cache` を見ます。
  - キャッシュの有無は電源の入れ直しでしか変わりません。その後は必ず `port_examine` が読みます。この順で足ります。
  - 指摘 1 の `unflushed` を使うなら、この条件は要りません。

### 指摘 3：殺された thread が reset を取りやめると、別の thread の読み出しが失敗する

- 重さ：当たれば壊れる。当たる頻度は低いものです。
- 場所：`kernel/src/dev/nvme.c:363`・`:665`・`:735`、`kernel/src/dev/nvme.c:807`。
- 仕様の節：NVMe 3.7.2 と 9.5 の手順には関わりません。`nvme.c:337` のコメントとの差です。コメントは「次の要求が続きを行う」と書いています。
- 差：
  - `enable` は入口で `generation` を進めます。
  - `recover` は `generation` が動いていれば、`enable` を呼ばずに `ready` を返します。
  - 取りやめの後は `ready` が偽です。
  - 同じ時に待っていた別の thread の要求は、続きを行わずに失敗します。
- 筋書き：
  1. コントローラーの命令が時間切れになります。
  2. アプリの thread T1 が殺されており、`recover` の `enable` の中で `pci_function_reset` が取りやめになります。
  3. 同じ時の別の thread T2 は `queue_clear` で FAILED を受けます。
  4. T2 の `recover` は `generation` の動きを見て偽を返します。
  5. T2 の `transfer` は `-1` を返します。コントローラーは次の要求で戻れるのに、T2 の読み出しは入出力の誤りになります。
- T2 が書込みのときは、`lost` が真なので指摘 3 が無くても `cache_lost` が立ちます。増える害は読み出しの失敗です。
- 直す案：`recover` で `generation` が動き、`ready` が偽で、`cut` が偽のときは、自分で `enable` を呼びます。取りやめた本人は `thread_current()->killed` で偽を返します。

### 指摘 4：終わっていない FLR の function へ、`pci_disable_busmaster` が configuration を読み書きする

- 重さ：当たれば壊れる。当たる頻度は低いものです。行は 883c546d からあり、この差分は `killed` の枝の前へ動かしました。
- 場所：`kernel/src/dev/nvme.c:362`、`kernel/src/dev/pci.c:699`。
- 仕様の節：
  - PCIe 6.0 6.6.2：FLR は 100 ms 以内に終わります。その間の Request は黙って捨ててよいものです。その後は RRS を返してよいものです。実装の注の手順 1 と 5 は、FLR の間ほかの触れを止めて 100 ms 待つことです。
  - PCIe 6.0 2.3.1・2.3.2：RRS を見せない Root Complex は configuration の要求を出し直します。
  - `kernel/src/dev/pci.h:175`：RRS を返す function へは書かないと自分で約束しています。
- 差：
  - `pci_function_reset` が偽を返す道には、FLR を出した後の道が含まれます。100 ms の待ちの中で取りやめた場合と、期限までに ID を返さなかった場合です。
  - このとき Bus Master Enable は FLR の前に消えています（`reset_quiesce`）。
  - `controller_stop` はそれでも読みと書きを出します。
  - この時点で Root Port の表示は `rrs_release` で元に戻っています。
- 筋書き：
  1. 読みが捨てられるか、完了の時間切れになります。CPU はその間止まります。
  2. 読みが `FFFFh` で戻ると、`command & ~MASTER` の `FFFBh` を Command へ書きます。
  3. RRS を返す function なら、この書込みも Root Complex が出し直します。
  4. `cut` の道では、その値がそのまま残ることがあります。
- 直す案：
  - `pci_disable_busmaster` は `d->reset` があるときは触れません。そのとき Bus Master Enable は消えています。
  - 読みが全部 1 のときも書きません。

### 指摘 5：コメントが今のコードと違う

- 重さ：きれいさ。
- 場所と差：
  - `kernel/src/dev/blk.h:27`：「装置へ渡した書込み」と書いてあります。今は渡す前です（指摘 1）。
  - `kernel/src/dev/blk.h:35`：NULL は揮発のキャッシュが無いことと書いてあります。今は AHCI のキャッシュ無しの disk にも `persist` があります。
  - `kernel/src/dev/blk.c:37`：同じ理由で、AHCI の disk には「no write cache」が出ません。`ahci.c:1196` の文言だけが残ります。
  - `kernel/src/dev/pci.h:157`：MSI-X の表の entry は driver のものと書いてあります。`pci.h:168` は書き直して開くと書いています。二つが食い違います。
  - `kernel/src/dev/pci.c:844`：引用は 7.7.2.8 だけです。7.7.2.8 が言うのは Mask bit の既定値 1 です。message が不定なのは 7.7.2.5・7.7.2.6・7.7.2.7 です。
  - `kernel/src/dev/pci.c:812`：reset する呼ぶ側を `dev/pciuser.c` だけと書いています。`nvme.c` も呼びます。
  - `kernel/src/dev/ahci.c:1022`・`:1023`：一つの関数にコメントが二つ続いています。

## 照らして合っていた手順

### AHCI 1.3.1

- `port_up` の注の引用の変更（`ahci.c:541`）：6.2.2.1 の順序は、ST を消す、CR を待つ、PxSERR を消す、PxIS を消す、BSY か DRQ なら COMRESET、ST を立てる、です。コードは同じです。10.4.1 の引用を外したのは正しいです。コードは software reset をしていません。
- `halt` の GHC.HR の主張（`ahci.c:805`）：10.4.3 は HR が 0 に戻るときが reset の完了と書いています。完了の後は port の registers が戻っています。ST も戻ります。
- `halt` の `h->reset` の順：`h->reset` を持ったまま `halt` を呼ぶ道はありません。`port_force_stop` は再び取りません。`low_lock` と `partial` を持つ道から `h->reset` へ進みますが、逆の順はありません。待ちの長さは、ほかの port の `port_up` の上限で決まります。
- `hba_ready` を adapter の thread で呼ぶ変更：`lost` が真のとき、全部の port の命令はすでに失敗し、`live` は偽です。`hba_set_up` の「port に命令が無い」の前提は保たれます。
- `ahci_worker` の順番の選び：`PxSSTS.DET` の bit 0 は 1h と 3h だけで立ちます。10.4.2 と 7.3 に合います。全部の port を一度ずつ調べて終わります。
- `port_to_slumber` の `PxIE` の 0：7.3 と 7.3.1 は Slumber の出入りで DIAG.N が立つと書いています。7.3.1.2 は PRCE を閉じる流れです。全部を閉じても、この port に後の命令はありません。再び開くのは `hba_set_up` の `port_listen` です。`finish` では他の CPU が動いているので、この変更は要ります。
- `SCTL_SLUMBER_ALONE`：3.3.11 の IPM は 1h が Partial 禁止、5h が Partial と DevSleep の禁止です。合っています。
- `blk_ahci_persist` の式：`(cache && flush < 0) ? -N_ERR_IO : 0` と読まれます。意図どおりです。
- `retry_at` の atomic：書き手と読み手が別の thread になりうる語です。必要な変更です。

### NVMe Base 2.4 と PCIe Transport

- `nvme.c:334` の主張：Figure 41 の CC.EN は、Controller Reset が CQE の出た命令の結果に影響しないと定めています。合っています。この保証は host の Controller Reset の Controller Level Reset にかかります。FLR と CFS には書いてありません。コメントのとおりです。
- `nvme.c:331` の主張：Figure 41 は RDY が 0 のときの EN の 1 から 0 を undefined としています。合っています。
- `controller_idle`：9.5 は CFS に Controller Reset を、消えなければ NVM Subsystem Reset を勧めています。コードは NVM Subsystem Reset の代わりに FLR を使います。9.5 は link を落とす副作用があれば使わないよう書いています。FLR は Transport 3.3 の Controller Level Reset です。
- `enable` の `pci_enable_busmaster` の位置：Transport 3.3 と PCIe 6.6.2 のとおり、FLR は Bus Master Enable を消します。CC.EN を立てる前に戻しています。順序は合っています。
- flush の後の順：7.2 は flush の前に完了した命令に効くと定めています。`blk_persist` の `upto` の読みはこれに合います。

### PCIe 6.0

- `msix_entries_again`：6.6.2 は FLR で function の registers を初期値に戻します。7.7.2.8 の Mask bit の既定は 1 です。`state_restore` の後の順は、Memory Space を開く、Enable を戻す、entry を覆ったまま message を書く、覆いを外す、です。Bus Master Enable はこの間消えたままなので、message は出ません。
- 上位のビットの保持：`mmio_read32(vc) & ~1u` は Reserved と Steering Tag を保ちます。7.7.2.8 に合います。
- 錠の順：`stop_lock`、`reset_mutex`、`requests` の順です。逆の道はありません。`iommu_msi_message` は `amd_msi` と `intel_msi` まで読みました。同じ entry を書き直すだけで、configuration へ触れません。`d->reset` も見ません。
- `pciuser.c` の呼ぶ側：`reset_finish` は FLR の後すぐ `pci_irq_disable` を呼びます。entry が開いている間 Bus Master Enable は消えています。害はありません。

### block 層と呼ぶ側

- `volume_write`（`common/store/volume.c:248`）：blocks、persist、superblock、persist の順です。`cache_lost` の下では最初の persist で止まり、superblock は書かれません。`blk.h:66` の「最後に整った commit が媒体に残る」は成り立ちます。
- flush を先に行う `blk_persist`：後の superblock の persist だけが失敗した場合も、媒体の superblock は古いか新しいかの一方で、どちらも整っています。
- `pm.c:128`：`cache_lost` のある disk で S3 を止めない変更です。休止の状態は `disk_flush` が止めます。行き詰まりはありません。
- `blk_persist` の `persisted` の数え方：同時の書込みと persist を組み合わせて調べました。flush していない書込みを `persisted` に入れる順はありません。

## 調べて指摘にしなかったもの

- CC.EN が 0 で見つかる場合の `lost`（`nvme.c:352`）：
  - NVM Subsystem Reset は Transport 3.3 で link が Detect へ移ります。configuration が消えるので、MMIO は全部 1 で読めて `lost` が立ちます。
  - Cross-Controller Reset は Nocturne が出しません。
  - S3 の後の EN 0 は shutdown の後なので失うものがありません。
- 一度の `reap` の中で flush が DONE になり、同時にリンクの変化を見る場合：Interrupt も poll も、リンクの変化を装置の再起動より前に読みます。今のコードの順で当たる道はありません。
- 殺された thread の待ちの空回り：`sys/wait.h` は一つの kill で一つの待ちだけが終わると定めています。空回りはありません。
- `halt` が `h->reset` を待つ間の port：その port の slot は取られたままなので、ほかの thread は `port_start` へ入れません。mutex は FIFO なので順も崩れません。
- `msix_entries_again` の書直しの失敗：`iommu_msi_message` が偽を返すのは IOMMU の unit が失敗した状態だけです。entry は覆われたままで行が残ります。NVMe は poll へ移らず時間切れを繰り返します。その前提で IOMMU 自体が失敗しているので、指摘にしませんでした。

## 読み切れなかった範囲

- ACS-3 と Serial ATA 3.3 は `D:\Programes\OS DEV INFO` にも依頼の文字起こしにもありません。IDENTIFY の word 85・word 87 の意味と、COMRESET で揮発のキャッシュが残るかどうかは、文書で確かめられません。分かりません。
- `controller_idle` は RDY が落ちた直後に CFS を一度だけ読みます。3.7.2 と Figure 42 の注 1 は、reset の間の値が初期値に変わる順を決めていません。CFS が RDY より後で消える装置があるかは、文書から分かりません。その装置では FLR へ一段進みます。
- `on_every_unit`、`irte_source` の中身と、`pci_resume` の全部は読んでいません。差分の外です。
- 実機、QEMU、Hyper-V の動きは確かめていません。頼まれた範囲の外です。
