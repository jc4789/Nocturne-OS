# 記憶の driver の差分と公式の文書の監査（四回目）

## 依頼文

Nocturne OS のカーネルの記憶の driver（AHCI、NVMe）の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 34ab1dbe HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.c kernel/src/dev/blk.h`。必要なら `git diff 55142942 HEAD` の全部も。
- 今のコード：`kernel/src/dev/ahci.c`、`nvme.c`、`blk.c`、`blk.h`、`pm.c`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pci30.txt、pcie6.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順・眠ってよい所かを調べます。データを失う筋書きを最初に探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/kernel-core-20261101/audit-usbstorage-fix4-storage.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

- 対象は枝の HEAD `883c546d` です。行番号はこの版のものです。
- 指摘は 14 件です。データを失うものが 5 件、当たれば壊れるものが 3 件、きれいさが 6 件です。
- 今回の差分で入った `blk_cache_lost` の仕組みには穴が三つあります（S1、S2、S3）。
- `blk_cache_lost` の考えは NVMe に通っていません（S5）。AHCI でも再認識の道だけに通っています（S4、S5）。
- 今回の差分で入った NVMe の FLR の段は、MSI-X の機械では戻って来ません（B1）。
- レジスターのビット位置と値に誤りはありません。節番号の引用は一か所を除いて合っています。

## データを失う

### S1. `blk_persist` が旗を読むのが flush の前である

- 場所：`kernel/src/dev/blk.c:141`（`__atomic_exchange_n(&d->cache_lost, false, ...)`）、`:144`（`d->persist`）、`:149`（`lost` の判定）。呼ぶ側は `kernel/src/dev/ahci.c:702-718`（`enter` が再認識を待つ）、`:1149`（`blk_cache_lost`）、`:1165`（`unverified` を下ろす）。
- 仕様の節：AHCI 7.3 は、ホットプラグの知らせが電源の断たれ方を教えないことを示します。7.4 と 6.2.2.3 は、コマンドの出し直しと再列挙を求めます。`blk.h:60-63` のコメントは「次の blk_persist は失敗する」と約束しています。
- 差：旗は flush の前に読みます。flush は `unverified` の間 `enter` で待ちます。旗は `unverified` が下りる直前に立ちます。だから、再認識の前に始まった `blk_persist` は、自分の待ちの間に立った旗を見ません。
- 当たる筋書き：
  1. 書きが全部終わり、データがディスクの揮発キャッシュにあります。
  2. 電源コネクターの瞬断でディスクの電源が落ち、キャッシュが消えます。リンクの知らせが来て `unverified` が立ちます。
  3. 再認識（COMRESET、最大 10 秒の準備待ち、IDENTIFY）の間に `blk_persist` が始まります。`lost` は false です。
  4. flush は `enter` で待ちます。再認識が旗を立てます。flush は空のキャッシュに通って成功します。
  5. `blk_persist` は 0 を返し、`persisted` を進めます。
  6. `common/store/volume.c:254` の一回目の持続が成功とされます。`:258` が新しい superblock を書きます。
  7. `:260` の二回目の持続は flush を通して superblock を媒体に載せ、その後で旗を見て失敗します。
  8. 媒体には、消えたブロックを指す新しい superblock が残ります。次の起動で C: はそれを読みます。
- 当たる窓：最後の書きが終わってから `blk_persist` が始まるまでの間に、リンクが切れるときです。`volume_write` ではこの間が短いです。書きと持続の間が空く呼び手では、再認識の秒の単位まで窓が広がります。
- 重さ：データを失う。誤りが返らない一回目の持続が、順序の保証を壊します。
- 直す案：旗の読み取りを flush の後へ移します。`r = d->persist(d)` と `persisted` の更新の後で `lost = exchange(cache_lost, false)` を読み、`!r && lost` なら失敗にします。`blk.h:60-63` のコメントはこの順で正しくなります。

### S2. `blk_cache_lost` が途中の書きを数えない

- 場所：`kernel/src/dev/blk.c:95`（`written` は `d->write` が戻った後に増える）、`:179`（`persisted < written` の判定）、`kernel/src/dev/ahci.c:1149`。
- 仕様の節：AHCI 7.4 は、外れたときにコマンドが途中で残りうることを示します。6.2.2.2 は、終わったものと残ったものの見分け方を示します。ディスクが受け取り済みで `blk_write` がまだ戻っていないチャンクは、この区別の外にあります。
- 差：`written` は「戻った要求」の数です。複数のチャンクに分かれた要求は、前のチャンクがキャッシュにあっても `written` に入っていません。`blk_cache_lost` はそれを失われうるものと数えません。
- 当たる筋書き：
  1. 前の持続の後で最初の要求が始まります。AHCI の一コマンドは 512 KiB までなので、大きな要求は `blk_by_blocks` で分かれます。
  2. 前のチャンクがキャッシュに入ります。次のチャンクの途中で電源が瞬断します。
  3. `written == persisted` のまま `port_examine` が走ります。旗は立ちません。
  4. 落ちたチャンクは `transfer` が出し直して成功し、`blk_write` は 0 を返します。
  5. `blk_persist` は成功します。前のチャンクは消えています。
- 当たる道：`kernel/src/dev/hibernate.c:168` の像の書き込みは、前の持続の後で最初の要求になります。ここで欠けた像は、次の起動で読み戻されます。一コマンドの要求でも、DONE から `written` の加算までの短い間は同じです。
- 重さ：データを失う。
- 直す案：要求の始めで増やす `began` を `blk_write` に足します。`blk_cache_lost` は `persisted < began` で判定します。`persisted` は `written` の値を写すだけなので、数の尺は同じです。まだ届いていない書きを数えても、失敗が余分に一度出るだけです。

### S3. 旗は最初に見た一人が消す

- 場所：`kernel/src/dev/blk.c:141`。別の読み手は `kernel/src/dev/pm.c:127`、`kernel/src/dev/hibernate.c:180`、`kernel/src/fs/store.c:54`。
- 仕様の節：なし。`blk.h:60-63` の約束との差です。
- 差：旗は一度の `blk_persist` で消えます。消えたものを書いた呼び手と、失敗を受け取る呼び手が同じとは限りません。
- 当たる筋書き：
  1. volume の commit がブロックを書き、持続の前に `blk.c` の門で止まります。門を閉じたのは眠りの `blk_hold` です。この commit は `store_sync` の後に始まったものです。
  2. 瞬断の再認識で旗が立っています。
  3. `pm.c:127` の `flush_disks` が旗を消費し、眠りを断ります。flush は通っているので `persisted` は進んでいます。
  4. 門が開きます。commit の持続は `lost` が false で、`persisted == upto` なので 0 を返します。
  5. commit は superblock を書き、消えたブロックを指す媒体ができます。
- 同じ形は、二つの呼び手が同時に `blk_persist` するときにも起きます。
- 重さ：データを失う。窓は狭いです。
- 直す案：旗を「持ち主が認めるまで残る状態」にします。`blk_persist` は旗が立っている間いつも失敗し、volume が `volume_broken` を経て認めたとき、または次の mount で下ろします。眠りは、その間ずっと断ります。もう一つの案は、失った世代の数を `blkdev` に持ち、commit の始めに読んだ値と持続の後の値を比べることです。

### S4. 戻ってきたディスクのキャッシュの状態を読み直さない

- 場所：`kernel/src/dev/ahci.c:1059`（`d->cache` は IDENTIFY の値）、`:1146-1150`（`known` の道。`found` の `cache` は捨てる）、`:1182`（`blk.persist` は最初の認識でだけ決まる）、`:1319`（`d->cache && flush(d)`）、`kernel/src/dev/blk.c:139`（`persist` が NULL なら 0）。
- 仕様の節：AHCI 10.10.3 は、S3 以上でディスクの電源が切れることを書きます。7.3 と 6.2.2.3 は再列挙を求めます。ACS-3 の word 85 の定義は材料にありません。コードのコメントはそれを「今のキャッシュの状態」として読みます。
- 差：再認識は IDENTIFY を読み直します。`same_disk` が真なら、その結果の `cache` を捨てます。
- 当たる筋書き：
  1. 起動のとき、ファームウェアがキャッシュを切ったディスクが `cache` false で認識されます。`blk.persist` は NULL です。
  2. S3 かリンクの瞬断で電源が切れ、ディスクは電源投入の既定でキャッシュを有効にして戻ります。
  3. `found->cache` は true ですが捨てられます。
  4. `blk_persist` は flush を出さずに 0 を返します。volume の順序の保証は無くなります。
  5. 次の停電で、superblock だけが載った媒体ができます。
- 重さ：データを失う。キャッシュを切るファームウェアの板でだけ当たります。
- 直す案：`known` の道で `known->cache = found->cache` を更新し、`blk.persist` を常に `blk_ahci_persist` にします。`blk_ahci_persist` は `d->cache` が偽のとき flush を飛ばします。`persist` は `blk_register` の行の表示にも使うので、表示は `cache` から決めます。

### S5. NVMe の致命の状態と FLR の後、キャッシュの損失を知らせない

- 場所：`kernel/src/dev/nvme.c:330-361`（`controller_stop`）、`:415-422`（`run` が CFS で止める）、`:824-837`（`blk_nvme_persist`）。AHCI 側の同じ形は `kernel/src/dev/ahci.c:545-587`（時間切れからの COMRESET は `port_examine` を通らない）。
- 仕様の節：NVMe Base 2.4 の Figure 41 の EN は、Controller Reset が起こす Controller Level Reset について、完了済みのコマンドの結果に影響しないと保証します。保証の範囲は「その reset が起こす影響」です。9.5 は、CFS を「完了を伝えられないほどの重い誤り」と定め、キャッシュの扱いを書きません。3.7.2 は FLR も Controller Level Reset とし、Figure 41 の保証の文は Controller Reset の項にだけあります。Flush（7.2）は、その時に揮発キャッシュにある内容だけを対象にします。
- 差：`blk_cache_lost` は AHCI の `port_examine` からだけ呼ばれます。CFS または FLR を経た NVMe では、文書の保証の外なのに、次の Flush の成功がそのまま「全部載った」とされます。
- 当たる筋書き：
  1. 書きが完了し、キャッシュにあります。
  2. 制御器のファームウェアが落ちて CSTS.CFS が立ちます。キャッシュの内容は消えています。
  3. `run` が `controller_stop` を呼び、次の `recover` が `enable` します。
  4. Flush は空のキャッシュに通って成功します。
  5. volume は commit を完了し、消えたブロックを指す superblock が残ります。
- AHCI の時間切れの道も同じです。ディスクのファームウェアがリンクを落とさず再起動すると、リンクの知らせが来ません。COMRESET だけで戻り、`blk_cache_lost` を通りません。
- 重さ：データを失う。
- 直す案：`controller_stop` で、CFS を見たとき、または FLR に進んだときに、`c->found` の全部へ `blk_cache_lost` を呼びます。CC.EN だけで止まった時間切れは Figure 41 の保証の中なので呼びません。AHCI は `halt` の時間切れの道で `need_reset` と一緒に、`p->disk` へ `blk_cache_lost` を呼びます。どちらも S1 と S2 を先に直します。

## 当たれば壊れる

### B1. FLR の後、MSI-X の表が戻らない

- 場所：`kernel/src/dev/nvme.c:345-347`（今回の差分で足した段）、`kernel/src/dev/pci.c:928`（`state_save(d, &r->state, false)`）、`:678`（`d->msix_table && s->msix_table`）、`kernel/src/dev/nvme.c:425`（割り込みの場合は期限まで眠る）。
- 仕様の節：PCIe 6.0 の 6.6.2 は、FLR が機能のレジスターを初期値へ戻すことを定めます。7.7.2.8（Table 7-55）は、MSI-X の表の Mask Bit の既定を 1 と定めます。PCI 3.0 の 6.8.2 も同じです。NVMe Base 3.7.2 は、CLR の後に transport の状態を host が直すことを求めます。NVMe over PCIe の 3.3 は、FLR で PCI のレジスターが初期化されることを書きます。
- 差：`pci_function_reset` は表を保存しません（第三引数が false）。`state_restore` は MSI-X の Enable を戻しますが、表は戻しません。全部の項目は Mask 1 のままです。コメントの「its configuration comes back with no bus mastering」は、表について誤りです。
- 当たる筋書き：
  1. Controller Reset で RDY が落ちない制御器に FLR をかけます。FLR は成功します。
  2. `enable` の最初の Identify は完了しますが、割り込みが来ません。`run` は `c->nvectors` が正なので CQ を見ません。
  3. 5 秒で時間切れになり、`controller_stop` が走り、`enable` は失敗します。
  4. 以後の読み書きは毎回 `recover` で同じ失敗を繰り返します。
  5. ディスクは再起動まで使えません。
- FLR の段は、MSI-X を持つ NVMe では戻って来ません。NVMe の機能はふつう MSI-X を持ちます。MSI の機能だけは `state_restore` の `msi_regs` で戻ります。
- 重さ：当たれば壊れる。今回足した回復の段が働きません。
- 直す案：`reset_attempt` の `state_save` を table 付きにします。`:1207` のバスリセットの道の同じ呼出しも同時に見直します。`pciuser` の呼び手は表の持ち主が別なので、呼び手が選べる引数にするか、`pci.h` に表を書き直す口を足します。`nvme.c` はその後に `irq_msi_writer` と同じ経路で書き直された表を前提にします。`pci.h:171-183` の約束にも表の扱いを書きます。

### B2. kill された thread の FLR は取り消され、制御器を切り捨てる

- 場所：`kernel/src/dev/nvme.c:345-354`、`kernel/src/dev/pci.c:872`（`reset_cancelled`）、`:1049`（`pci_function_reset` の最初の判定）、`pci.h:170-173`（取り消しは次の呼出しへ持ち越す約束）。
- 仕様の節：NVMe Base 9.5 と 3.7.2 は、reset による回復を求めます。PCIe 6.6.2 に取り消しの概念はありません。`kernel.md` の規則では、kill は mutex を終わらせません。ここの取り消しは `pci.c` の約束です。
- 差：`pci_function_reset` は `thread_current()->killed` で false を返します。`pci.h` はその場合に次の呼出しで続けることを前提にします。`controller_stop` は false を「FLR が効かなかった」と読み、`cut` を立てます。一度きりで戻りません。
- 当たる筋書き：
  1. 制御器が固まります。アプリは読み書きの syscall の中で待っています。
  2. 利用者がそのアプリを閉じ、thread に `killed` が立ちます。
  3. 30 秒の時間切れで、その thread が `controller_stop` に入ります。Controller Reset は効きません。
  4. FLR は取り消され、`cut` が立ちます。全部の namespace が `gone` になります。
  5. FLR で戻せたはずの C: が、再起動まで消えます。
- 重さ：当たれば壊れる。B1 を直しても残ります。
- 直す案：止める仕事を核の thread（制御器ごとの回復の thread）へ渡し、呼んだ thread はその完了を待ちます。これは `killed` を見なくなります。`pci_function_reset` に取り消さない版を足す案もあります。その場合は `wait_ready` の `sleep_until` が kill で即座に戻ることも同時に直します。

### B3. `lost` を立てるのがアダプターのリセットより前である

- 場所：`kernel/src/dev/ahci.c:466`（`lost` を立てる）、`:476`（`GHC.HR`）、`:477`（完了の待ち）、`:800-801`（`lost` なら `port_stop` を飛ばす）、`:806-807`（`FAILED` で終える）、`:1040-1043`（`identify` が頁を返す）、`:22-24`（ファイル頭のコメント）。
- 仕様の節：AHCI 10.4.3 は、GHC.HR が 0 に戻るまでリセットの完了を保証しません。最大 1 秒です。10.3 は、ST と CR が落ちるまで DMA エンジンが動いていることを示します。6.1.1 は、誤りの後も DMA が続きうることを示します。
- 差：`halt` のコメントは「after an adapter reset its engines are stopped」と言います。`lost` はリセットの前に立つので、リセットの最中や開始の前にも真です。`halt` はエンジンを止めずに `FAILED` を返します。ファイル頭の「失敗を返すのは DMA が終わってから」という約束が破れます。
- 当たる筋書き：
  1. アダプターが固まり、ポート 0 と 1 のコマンドがほぼ同時に時間切れになります。この同時の時間切れがアダプターの故障の普通の形です。
  2. ポート 0 の thread が `hba_reset` に入り、`lost` を立てます。
  3. ポート 1 の thread が `halt` に入ります。`lost` が真なので止めずに `FAILED` を返します。
  4. 三度目の試みの `transfer` と `identify` は、ここで呼び手へ戻ります。`identify` は `pmm_free(page)` します。
  5. まだ生きている読みの DMA が、返した頁へ書きます。その頁は別の持ち主のものです。
- `transfer` の一度目と二度目は、次の `enter` が `h->reset` で待つので、リセットが先に終わります。だから窓は三度目と `identify` に絞られます。
- 重さ：当たれば壊れる。記憶の破壊です。
- 直す案：`lost` を `wait_mmio` の成功と `hba_kill` の後で立てます。`port_fail_all` の前に立てれば `halt` の判定は正しくなります。`hba_set_up` と `port_up:558` の読みは `h->reset` の下なので順序を変えても通ります。別の案は、`halt` が `lost` を見たとき `hba_lock` を一度取って放し、リセットの終わりを待つことです。

## きれいさ

### K1. Slumber へ入るときに PRCE を閉じない

- 場所：`kernel/src/dev/ahci.c:1282-1288`。
- 仕様の節：AHCI 7.3 と 7.3.1 は、Slumber への出入りで DIAG.N が立つことを書きます。7.3.1.2 は、電源管理を使うなら PxIE.PRCE を閉じることを勧めます（informative）。
- 差：`PxIE` を閉じずに ICC を書きます。
- 当たる筋書き：`finish` の道では、ほかの CPU の `ahci_isr` がこの PRCS を受け、`unverified` を立て、`ahci_worker` を起こします。止まった先は門で、データに害はありません。呼び止められた眠りでは、起きた後の二重の見直しになります。
- 重さ：きれいさ。
- 直す案：`wr(p, PxIE, 0)` を ICC の前に置きます。

### K2. 部分書きの錠は丸ごとの書きを止めない

- 場所：`kernel/src/dev/blk.c:115-122`。
- 仕様の節：なし。コメントとの差です。
- 差：コメントは「another request's bytes in the same block are not to be written back as they were」と言います。丸ごとの書きは `d->partial` を取りません。読みと書きの間に来た丸ごとの書きは、古い内容で上書きされます。
- 当たる筋書き：同じブロックへの同時の丸ごとの書きと部分書きです。重なる範囲の同時書きであり、`common/store` はこれを出しません。
- 重さ：きれいさ。
- 直す案：コメントに「部分書きどうしだけ」と書きます。

### K3. Controller Reset で CFS が残ったときの次の段が無い

- 場所：`kernel/src/dev/nvme.c:336-348`、`:654`。
- 仕様の節：NVMe Base 9.5 は、CFS が Controller Reset で消えなければ、対応する機械では NVM Subsystem Reset を勧めます。同じ節は、リンクが落ちて困る host には勧めません。
- 差：`stopped` が真なら FLR へ進みません。CFS が残ると `enable` は毎回 `wait_ready` で失敗します。`controller_stop` のコメントは、FLR が CFS にも効くように読めます。
- 当たる筋書き：CFS が Controller Reset で消えない制御器は、FLR を試さずに使えないままです。
- 重さ：きれいさ。NSSR を入れないことは 9.5 の但し書きの範囲です。
- 直す案：`stopped` の後に CFS を読み、残っていれば FLR の段へ進めます。B1 を直した後です。

### K4. 空のポートごとに一秒、起きた後の I/O が待つ

- 場所：`kernel/src/dev/ahci.c:414`（`LINK_NS`）、`:1214`、`:1229-1230`、`:1269`。
- 仕様の節：AHCI 7.3 は、挿入を COMINIT で知らせます。空のポートへの COMRESET は文書の要求ではありません。
- 差：`look_all` は全部のポートを順に回り、空のポートも `comreset` で 1 秒待ちます。ディスクのあるポートの `unverified` は、その順番まで下りません。
- 当たる筋書き：ポート 5 のディスクは、空のポート 0 から 4 を待って 5 秒遅れます。S3 の復帰と `hba_reset` の後です。AGENTS.md の「性能」に当たります。
- 重さ：きれいさ。
- 直す案：ポートごとに回復を並べるか、DET が 0 のポートを後回しにします。

### K5. コメントの引用の 10.4.1

- 場所：`kernel/src/dev/ahci.c:540-541`。
- 仕様の節：10.4.1 は Software Reset の節で、エンジンだけを止める根拠ではありません。根拠は 6.2.2.1 だけです。
- 重さ：きれいさ。
- 直す案：引用を 6.2.2.1 だけにします。

### K6. `retry_at` だけが原子的でない

- 場所：`kernel/src/dev/ahci.c:1138`、`:1141`、`:1219-1221`。
- 仕様の節：なし。`.claude/rules/kernel.md` の共有の状態の規則です。
- 差：今回の差分で `retry_ns` は原子的になりました。`retry_at` は、`blk_holder` の thread の `port_examine` が書き、`ahci_worker` が門の外で読みます。
- 当たる筋書き：x86-64 の整列した 64 ビットは裂けません。害は無く、書き方が揃っていません。
- 重さ：きれいさ。
- 直す案：`retry_at` も原子的に読み書きします。

## 照らして合っていた手順

### AHCI（Serial ATA AHCI 1.3.1）

- レジスターのビット：`CAP_SSC` は 3.1.1 の bit 14 です。`SERR_LINK` は 3.3.12 の N（16）と X（26）です。`CMD_MPSP`（19）、`CMD_CPD`（20）、`CMD_ICC`（31:28）、ICC の Slumber 6h は 3.3.7 です。`IPM_*` は 3.3.10 です。`SCTL_SLUMBER_ALONE` の 1h と 5h は 3.3.11 です。`IS_*` は 3.3.5 です。
- 3.3.13：`PxSACT` のビットは TAG とスロットに対応します。`PxCI` より先に書きます。ST が 1 の間だけ書きます。`command_in` は合っています。
- 3.3.5：PCS と PRCS は PxSERR の該当ビットを消さないと消えません。`reap` の `PxIE` を閉じる処理と `port_listen` は合っています。
- 3.3.11：DET は ST が 1 の間に変えません。`port_to_slumber` は DET を 0 のまま書き戻すので、変更に当たりません。
- 6.2.2：TFES の後、HBA は新しいコマンドを出さず、DMA Setup に応えません。ファイル頭のコメントは合っています。
- 6.2.2.1：非キューの回復の順序（ST を落とし、CR を待ち、PxSERR、PxIS、BSY と DRQ を見て COMRESET、ST を立てる）は `port_up` と合っています。
- 6.2.2.2：`settle` は ST を落とす前に `PxSACT` と `PxCI` を読みます。`PxSACT` の消えたものだけを完了にします。3.3.13 の「成功したものだけ消す」と合っています。
- 6.1.1、6.1.5、5.3.16.6：致命の報告では完了を取らず、全部をエンジン停止の後に失敗させます。合っています。OFS の扱いは 6.1.5 の勧めのとおりです。
- 6.2.2.3：COMRESET が先で PCS の消去が後です。`port_changed` の後の `port_up`、`comreset`、`port_listen` の順で合っています。
- 7.3：COMRESET で DET は 0、COMINIT で 1、完了で 3 です。切断後に DET が 1 のこともあります。`port_examine` の `DET_FOUND` の分岐は合っています。
- 7.4：PCS で HBA は止まり、ソフトは `PxCI` と `PxSACT` を見てから ST を落とします。`halt` の順で合っています。
- 10.1.1 手順 4-7：`port_spin_up` と合っています。
- 10.1.2：PxIS、IS の順の消去と、ST の前の条件は合っています。DIAG.X を消してから PxTFD を待つことも合っています。
- 10.3.1：ST は FRE の後です。SUD と DET は ST と CR が 0 の間に変えます。合っています。`port_force_stop` の例外は 10.4.2 が許します。
- 10.4.2：DET 1h を 1 ms 以上保ち、0h に戻し、DET 3h を待ち、PxSERR を消します。`comreset` と合っています。
- 10.4.3：GHC.HR、1 秒、AE と IE と全ポートのレジスターのリセット、SUD のリセットは `hba_reset` と `hba_set_up` と合っています。
- 10.6.3、10.6.4：25 ms と 2 秒、BB の後に BOS は `hba_take` と合っています。
- 10.7.2.1：`ahci_isr` の PxIS、IS の順は合っています。`resetting` の隙間の PCS は、線の水準が残るので再送されます。
- 10.10.1：DET 1h と SUD 0 の組は禁じられています。`comreset` が先に SUD を立てます。合っています。
- 10.10.3：STANDBY、ICC 6h、IPM が active でなくなってから DET 0 と SUD 0 の順は、`port_to_slumber` と `port_to_listen` と合っています。5.3.2.11 と 5.3.4.2 の P:Idle の分岐も、ST が 1 の間に ICC を書くことと合っています。
- 10.10.4：MPSP と CPD のポートを listen にしません。合っています。
- 3.1.1 の SSC：0 のとき ICC で Slumber を求めません。IPM は Slumber を禁じたままです。合っています。
- 7.3.1.1：IPM、ALPE、PRCE は `hba_set_up` と合っています。SET FEATURES の項は行いません。informative であり、3.3.11 の PMNAK が同じ働きをします。
- 4.2.3.3：DBA の語境界、DBC の奇数、4 MiB は `prds` と `rw` の直接転送の条件に合っています。

### NVMe（Base 2.4、NVM Command Set 1.1、over PCIe）

- Figure 41：SHN は 15:14、EN と RDY の組の未定義、SHN を 00b へ戻すこと、EN を 0→1 にする書きで SHN を消すことは `controller_stop` と `enable` と合っています。
- Figure 42：SHST 01b と 10b と 11b、ST の扱いは `shutdown_done` と `nvme_shutdown` の頭と合っています。
- 3.5.1：手順 1-11 の順は `enable` と合っています。
- 3.6.1：SQ の削除、CQ の削除、SHN の順と、RTD3E、0 なら 1 秒、上限なしは合っています。60 秒はコードの自前の上限で、コメントのとおりです。
- 3.6.1 の「complete の後の ready to power off」：`nvme_shutdown` の `SHST_DONE` の早抜けと合っています。
- 3.6.3：ST が 1 の complete の扱いは合っています。
- 3.7.2 と over PCIe 3.3：FLR は CLR です。コメントの引用は合っています。B1 は表の扱いです。
- 9.5：CFS で Controller Reset を出します。`run` と合っています。
- 7.2：Flush の範囲は `blk_nvme_persist` のコメントと合っています。
- PCIe 6.6.2：FLR の前の Command レジスターの消去、Transactions Pending の待ち、100 ms は `reset_quiesce` と `reset_attempt` と合っています。
- NVM Command Set Figure 114 の FLBAS：FIDXU は 6:5、16 個以下なら無視、FIDXL は 3:0 です。`add_namespace` と合っています。
- MDTS（Figure 338）：単位は CAP.MPSMIN で、`start` が MPSMIN 0 を確かめています。`read_controller` と合っています。
- `run` の待ちの時間の変更と `wait_ready` の引数の変更は、`stops` と `queue_clear` の順序を読み、失われる起こしが無いことを確かめました。

### 錠と眠り

- `h->reset` の後に `p->lock` です。逆の順はありません。
- `halt`、`port_force_stop`、`port_up` は `h->reset` を二重に取りません。
- `d->partial` は `p->low_lock` と `usbmsc` の `m->lock` の後です。逆の順はありません。
- `blk_hold` と `enter` の待ちは、`holder` を見て `IO_NS` で切れます。詰まりはありません。
- `enter` の `port_changed` は `holder` の道で、ワーカーと同時に走りません。`blk_busy_begin` の `inflight` が分けています。
- `controller_stop` と `pci_function_reset` は眠ります。呼ぶ道はどちらも眠ってよい所です。`run_polled` の道からは呼びません。
- `spinlock` の下で眠る所はありません。
- TLB の撃ち落としに当たる呼出しは差分にありません。

## 読み切れなかった範囲

- SATA 3.3 と ACS-3 は材料にありません。FIS の形（13.6）、IDENTIFY の語、FLUSH、STANDBY、COMRESET の後のディスクのキャッシュの扱いは、文書と照らしていません。S4 と S5 の AHCI 側は、この範囲に触れます。
- `kernel/src/dev/pci.c` は、FLR と状態の保存の道だけを読みました。`links_retrain`、`rrs_*`、`pci_resume` の詳細は読んでいません。
- USB と `usbmsc.c` は対象の外です。`blk_hold` に時間の上限はありません。`finish` が新しく `blk_hold` するので、上限の無い要求を持つ driver は電源断を止めます。AHCI と NVMe の要求には上限があることを確かめました。`usbmsc.c`、`hv/`、`vm/` は確かめていません。
- 文字起こしの段組みが崩れた表（3.3.7、PCI 3.0 の 6.8.2 の表）は、節の本文とビットの順で照らしました。PCI 3.0 の Mask Bit の節番号は、崩れて読めません。節番号は PCIe 6.0 の 7.7.2.8 で確かめました。
- 実機、QEMU、ビルドでの確かめはしていません。全部がコードと文書の読みです。
