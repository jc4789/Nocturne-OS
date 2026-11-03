# {"agentType": "Explore", "description": "fs・store・net の監査", "toolUseId": "toolu_01TG8mmT9A3WBBFh2rspwvpW", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log を読むのはよい）。Linux や他の OS のソースを開かない（設計の比べは公開された設計の知識で）。third_party は読まない。作業木には未コミットの変更があり、今のファイルをそのまま読む。断片でなくファイルを全部読んで判定する。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。この監査の結果で、どの持ち主の単位を消し、何を（あるなら）残すかを決める。直し方（patch）は提案しない（ユーザー：「Please do not suggest "patching" slop or bad architecture」）。

## あなたの範囲
- kernel/src/fs/（store.c・document.c・cache.c・watch.c と見出し）
- common/store/（格納庫の書式のエンジン：names・fold_table・catalog・namespace・space・volume・crc32c・latch。カーネルとホストの道具 host/store が共に組む）
- kernel/src/net/（net.c・rtl8125.c・dns_name.h・net.h）
- これらが下の層（kernel/src/dev/blk.c・nvme.c・ahci.c など）と上の層（kernel/src/sys/syscall.c の該当の口、common/abi.h）とつながる所は、つながりを判定するのに要る分だけ読む。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・メモリー・装置・プログラム・利用者・接続の数が増えても成り立つ形（ユーザー：「WHAT IF 20 apps were open at once? … what if the browser had 50 tabs open???」）。
- 対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）。CPU の下限は Ryzen 5 7600X（Zen 4）、上は 2 ソケットの EPYC 9996（論理 CPU 1024、NUMA、CXL のメモリーだけの node）。ユーザーの PC の NIC は Realtek RTL8125。
- 後ろ向きの互換は要らない。古い装置・古い ABI のための道は悪い。
- UNIX・POSIX・Linux ではない。ネイティブ ABI は common/abi.h（仕様 docs/nocturne-native-abi.md）。
- ディスクは一台（起動の区画と C: が同じ媒体）。
- shim（代役・何もしない lock・偽の関数）とスパゲッティ（一つの呼ぶ側のための包み、渡すだけの関数、その場の特例の分岐、決め打ちの値、ある環境の都合を別の部品へ持ち込むこと）は禁止。抽象化の層は責務の本当の継ぎ目に要る。

## 判定の物差し
- 規模：数（CPU 1024、接続・ファイル・開いた文書・見張り・アプリが十倍）で何が詰まるか。数に比例する走査、一つの大きな lock・mutex、一つの thread や一つの CPU に寄せる仕事、決め打ちの上限と表の大きさ、ポーリング・周期の見回り・決め打ちの回数の輪。
- 持ち主：一つの責務が一つの持ち主にあるか。分断・重複。
- stub・空の実装・固定値を返す関数・名前だけの API。
- 時間：時間の扱い（締め切り・再送・timer・待ち）の単位と、時計の読み方。
- Linux・NT の公開された設計と軸ごとに比べて劣る所。
- 正しさ：競争、memory ordering、lock の順、寿命、仕様との違い（TCP の RFC などは D:\Programes\OS DEV INFO にある。要るときだけ）。

## 報告の形（日本語、平文）
持ち主の単位（ファイルか部品）ごとに：
1. 責務（一行）
2. 判定：「残せる」「消して建て直す」「消すだけ（要らない）」のどれか一つ。「直せば使える」という中間は書かない。
3. 根拠：file:line と、その行が何をしているか。コードで確かめたものだけ。確かめていないものは「未確認」と書く。
4. 十倍・1024 CPU で何が詰まるか。
5. Linux・NT の公開の設計と比べて劣る軸（あれば）。
6. 建て直すなら、正しい形が満たすべき要件（設計の要件。patch ではない）と、他の部品との継ぎ目。
最後に：この範囲で「残せる」と言えるものとその理由、全体の所感（三行まで）。長さは要点が尽きるまででよいが、同じことを繰り返さない。

## reply

# Nocturne OS カーネル監査：fs・common/store・net

この範囲で残せるのは、名前の規則（names）と DNS の名前の符号化（dns_name.h）の二つだけです。ほかの持ち主の単位は全部「消して建て直す」です。原因は個々の関数ではなく構造にあります。木・空き地図・cache・文書の表・門・網の lock が、それぞれ全 CPU が叩く大域の一点になっていて、下の block の層も同期の単発です。

**読んだもの。** 範囲のファイルは全部、断片でなく全体を読みました。作業木の未コミットの変更は、この範囲のファイルにはありません（git status）。つながりを見るために、次は必要な所だけ読みました：kernel/src/dev/blk.c・blk.h（全体）、nvme.c の 1-320 行、ahci.c（説明の行）、syscall.c の 2340-2509 行・1488-1517 行・1660-1690 行、sys/watch.c（全体）、sys/object.c の notify、idt.c の pic、pci.c の MSI、vm.c の 55-95 行、abi.h の 60-259 行、docs/nocturne-native-abi.md の 25-74 行。

以下、行番号の前に書くパスは D:\Programes\llm vibe slop os\ からの相対です。

---

## 1. D:\Programes\llm vibe slop os\kernel\src\fs\store.c（store.h を含む）

**責務：** C: の媒体を探して mount する。道（path）での操作、変更と commit の門、フォルダーの lock、書き手の thread、raw 文書の口を持つ。

**判定：消して建て直す**

**根拠**
- **門（446-482）。** C: へのすべての変更が、大域の spinlock `store.gate` と一つの数 `store.changes` を二度通ります。
- **commit 中の停止（485-498）。** commit_locked は門を閉じたまま volume_freeze を呼びます。その中の freeze_ready → space_alloc → map_load（common/store/volume.c:195-206、space.c:30-57）は媒体を読むことがあり、その間は系全体の変更が止まります。
- **書き手。** 核の thread「store」一本（440、513-530）が、全文書の flush と commit を受け持ちます。待ちは固定の 5 秒です（17）。
- **媒体の探し。** 100 ms ごとの見回り（309）、USB を数えている最中という特例（304）、32 台までの bitmask（290-292）があります。
- **区画の層の仕事がファイルシステムにある。** GPT の読み・CRC・書き換えと、保護 MBR の書き換えがここにあります（66-260、特に gpt_grow 193-229）。
- **利用者は一人の決め打ち。** C:\Users の最初の非 PRIVATE フォルダーを「その人」とし、大域の文字列 user_folder に置きます（389-419、403-405）。権限は「app か核か」と SYSTEM／PRIVATE の二つの bit だけです（store.h:10-11、place_may_change 686-690）。
- **名前の解決。** place_find（624-658）は名ごとに ns_find と ns_record で目録を二度降ります（644-645）。名の cache はありません。同じ歩きが common/store/namespace.c:140-148 の ns_lookup にもあり、こちらは host/store だけが使っています。
- **一覧の続き（cursor、718-759）。**
  - 部屋は CPU 数×2 個（733）で、一つの spinlock の下で線形に探し（741）、memmove で詰めます（745、755）。
  - 一つの cursor は struct ns_entry を持ちます（名 256 B と鍵 400 B を含み、約 700 B）。
  - 1024 CPU では約 2048 個×700 B ≈ 1.4 MB を、割り込みを閉じた spinlock の下で、一覧の呼び出しのたびに動かします（796）。
- **一覧の中身（761-799）。** 項ごとに ns_next（根からの降下）と ns_record（もう一度の降下）を呼び、さらに entry_fill → document_size_now（document.c:1110-1116）で大域の docs.lock を取ります。
- **raw 文書（976-1061）。** 目録の extent に区画の先頭を足して LBA にし、外へ渡します（992）。hibernation が store を通らずに disk へ直に書く穴です。
- **別の媒体の型が混じっている。** CD/DVD 起動の「memory 上の C:」がこのファイルに入っています（30-61、266-273）。
- **時間。** 時刻は壁時計の ns（325）、待ちは単調時計の clock_after で、単位は揃っています。

**十倍・1024 CPU で詰まる所**
- 門の一語、cursor の memmove、docs.lock、一本の書き手。
- commit の間の全停止（媒体の読みを含む）。
- 利用者が二人になった時点で user_folder の前提が壊れる。

**Linux・NT と比べて劣る軸**
- 名前の解決：Linux は dcache と RCU の歩き、NT は FCB と SCB を持ち回る。ここは名ごとに B+木を二度降りる。
- 書き戻し：Linux は装置ごとの flusher と balance_dirty_pages、NT は lazy writer。ここは一本の thread と固定の秒数。
- 区画：Linux も NT も区画の層が持つ。
- 権限：Linux は uid・mode・ACL、NT は security descriptor。

**建て直すときの要件**
- 名前の解決は、lock を取らずに読める世代つきの名の cache を使う。目録へ降りるのは cache に無いときだけ。
- 変更を数えるのは CPU ごとか node ごとの数にし、凍結（freeze）の間に媒体を読まない。
- 書き戻しは装置（queue）ごとに、汚れの量と memory の圧で動かす。
- GPT の発見と拡張は区画の層へ移す。
- 利用者と権限は session／security の部品が持つ。fs は object ごとの持ち主と許可を記録し、照らすだけにする。
- hibernation には、block の層の上で「場所を固定した領域」を渡す口を用意する。LBA を fs の外へ漏らさない。

**継ぎ目：** 下は block の層、上は syscall.c の SYS_DOCUMENT_*／SYS_ENTRY_*／SYS_FOLDER_*、横は mm・pm（store_sync）・hibernation。

---

## 2. D:\Programes\llm vibe slop os\kernel\src\fs\document.c（document.h を含む）

**責務：** 開いた文書を持つ。書いた内容を memory の頁（work）に溜めて目録へ入れ、READ で開いた者には版（version）を読ませ、REPLACE の一時文書と保存の待ちを扱う。

**判定：消して建て直す**

**根拠**
- **大域の lock。** 読み・書きの呼び出しごとに、大域の spinlock docs.lock を二度取ります（opening_lock 599-611、opening_unlock → doc_put 205-217。表は 89-92）。
- **新しい 4 KiB の頁ごとの費用（page_new 413-441）。**
  - space_reserve(1)（417）で空き地図の大域の latch を排他で取る。
  - kzalloc(4096)（419）。
  - 並んだ配列へ memmove で挿入する（431-437）。
- **決め打ちの閾値（29-32）。** WORK_PAGES 16 MiB、COMMIT_BLOCKS 64 MiB、YIELD 64、READ_BLOCKS 64。
  - work の頁は kmalloc のままで、mm が回収しません。
  - 文書ごとに最大 16 MiB で、全体の上限はありません。
  - 汚れが 64 MiB を越えると、書いたアプリの thread がその場で volume 全体を commit します（762）。
- **close が volume 全体の commit になる。** 明示の close は N_COMMIT_WAIT です（syscall.c:2351）。変わった文書の close ごとに、volume 全体の commit と flush 二回（volume.c:254、260）が走ります。
- **書き戻しは一本の thread。** documents_flush（1211-1235）が全文書を順に処理します。保存待ちの一覧は大域の一本の鎖で、commit ごとに全走査（1050-1065）、close ごとに線形に外します（1073-1081）。
- **版の読みの費用。** version_read は区切りごとに ns_record で降ります（333）。読む者がいるときは、flush のたびに全 extent を写します（supersede 274-295）。
- **ns_read と同じ仕事を二度書いている。** version_read は namespace.c:572-609 の ns_read と同じ仕事を別に実装しています。
- **文書の中身の持ち主が三つ。** block cache、work の頁、syscall.c のプログラム cache（1488-1517、1663-1690：store_document_read で読み、別の frame へ memcpy）。
- **lock の順の文書が実物と違う。** document.h:4-14 は「latch と表の spinlock の下では何も取らない」と書いています。実際は次のとおりです。行き詰まりは見つけていません。
  - space_committed が latch の下で cache の forget（spinlock と kfree）を呼ぶ（common/store/space.c:369-382）。
  - entry_watch_publish が watches.lock の下で object_notify を取る（watch.c:62-82）。
- **良い考え（要件として残す）。** READ で開いた者は版を読み、変更が落とした block は最後の読み手が去るまで持ちます（35-44、256-271、297-301）。

**十倍・1024 CPU で詰まる所：** docs.lock、新しい頁ごとの空き地図の latch、一本の書き手、close ごとの commit、上限の無い work の memory。

**Linux・NT と比べて劣る軸**
- Linux は一つの page cache（inode ごとの xarray）が read・write・mmap・exec を兼ねる。NT は Cache Manager と section object。
- どちらも close は永続を約束しない（fsync／FlushFileBuffers を明示する）。
- Linux は汚れの割合で書き手を絞る。

**建て直すときの要件**
- 文書の内容は mm が持つ一つの頁 cache に置く（node ごとの LRU、回収できる）。プログラムの読み込みと mmap も同じ頁を使う。
- 文書の参照は大域の lock なしで取れる。
- 空き地の予約は CPU ごとに前借りする。
- 永続の要求は close と分け、group commit でまとめる。
- 版（snapshot 読み）の要件は保つ。

**継ぎ目：** syscall.c（文書の syscall、close、program loader）、新しい namespace、mm、イベント（N_EVENT_SAVED）。

---

## 3. D:\Programes\llm vibe slop os\kernel\src\fs\cache.c（cache.h を含む）

**責務：** C: の block を memory に持つ（store_supply の実装）。

**判定：消して建て直す**

**根拠**
- **一つの spinlock。** ハッシュ・LRU・全 block の参照数と状態を、一つの spinlock が守ります（3-8、31）。
- **一つの待ち行列。** loaded（32）を全 block が共有し、どれか一つの読みが終わると全員を起こします（178-192）。
- **割り込みを閉じた rehash。** ハッシュを倍にするとき（145-170）、spinlock の下で全 block を付け替えます。上限は memory の 1/16 なので（store.c:334）、大きな機械では数千万件になります。件数は推定で、時間は未計測です。
- **縮まない上限。** 上限は固定で、mm の圧で縮みません。shrinker はありません（401-421）。
- **三度の写し。** 装置 → NVMe／AHCI の bounce buffer → stage（298-305）→ cblock。
- **良い点。** LOADING の重複を避ける作りと、失敗の扱い（1-8、213-260）は丁寧です。

**十倍・1024 CPU で詰まる所：** 一語の spinlock、全員起こしの待ち、割り込みを閉じた rehash。

**Linux・NT と比べて劣る軸：** Linux は頁ごとの lock bit とハッシュした待ち、node／memcg ごとの LRU。NT は PFN ごとの状態。

**建て直すときの要件**
- 頁ごとの状態と待ち、分割した索引、node ごとの LRU。
- mm の回収に応じる。
- 装置から cache の頁へ直に DMA する（IOMMU の写像つき）。

**継ぎ目：** store_supply の口（common/store/volume.h:85-109）。新しい頁 cache に吸収されるべきものです。

---

## 4. D:\Programes\llm vibe slop os\kernel\src\fs\watch.c（watch.h を含む）

**責務：** C: の見張り（名とフォルダー）を folder id で引き、変更を数える。

**判定：消して建て直す**

**根拠**
- 全部の見張りが一つの spinlock と一つの表を使います（28-32）。見張りが一つも無くても、名前の変更のたびにこの lock を取ります（61-63）。
- 伝える変更は ns_told の三つまでで（namespace.h:83-86）、place_lock（111）にも縛られています。下の部品を建て直すと口が変わります。
- 見張りを作るときフォルダーの lock を持ち、「見た後、開く前の変更」を落とさない設計（6-9）は正しいです。

**十倍・1024 CPU で詰まる所：** 変更の頻度に比例して一つの spinlock に集まります。見張りの数には folder ごとの鎖で比例しない点は良いです。

**Linux・NT と比べて劣る軸：** inotify は inode ごとの mark、NT は FCB ごとの notify list。

**建て直すときの要件：** 見張りは対象の object に付け、その object の lock だけで伝える。見張りが無いときの費用はゼロ。数えるだけの level の見張りの意味は、sys/watch.c の汎用部と合わせて保つ。

---

## 5. D:\Programes\llm vibe slop os\common\store\latch.h

**責務：** 格納庫の memory 上のデータを守る、回って待つ読み書きの lock。

**判定：消して建て直す**

**根拠**
- 一つの 32 bit の語を全員が CAS で叩きます（13-15、24-46）。待つ者は並ばず、回るだけです。
- 書き手が一人でも待つと、新しい読み手は全員回り続けます（27）。
- 読み手 16 bit、書き手 15 bit の決め打ちがあります（17-20）。
- この lock が、目録の木（v->tree）と空き地図（space.latch）という大域の二つに使われています。

**十倍・1024 CPU で詰まる所：** 一つの cache line に 1024 CPU の CAS が集まり、書き手（catalog_freeze、葉の COW）の間は全読み手が回ります。

**Linux・NT と比べて劣る軸：** Linux は qrwlock（MCS の待ち行列）と RCU。NT は pushlock。

**建て直すときの要件：** 読みは lock を取らない（RCU か版の検査）。書きの待ちは並ぶ。大域の一語に頼らない構造にする。

---

## 6. D:\Programes\llm vibe slop os\common\store\catalog.c（catalog.h を含む）

**責務：** volume 全体で一本の B+木（記録・名・extent）を COW で持つ。

**判定：消して建て直す**

**根拠**
- **根と枝は一つの latch。** 読みは毎回 latch_share(&v->tree) を取ります（357、390、431）。
- **排他の道に落ちやすい。** 共有のまま葉を変えられるのは、葉が今の世代にあるときだけです（643）。commit の後に初めて触る葉は必ず排他で降ります（change_whole 597-623）。保存のたびに commit が起きるので、排他に落ちる割合が高くなります。
- **やり直し。** memory に無い node があると、全部の latch を放して読み、根からやり直します（147-170、382）。
- **commit 中の停止。** catalog_freeze（933-970）は木を排他で持ったまま、今の世代の全 node を置きます。その間、全読み手が回ります。
- **書式の癖。** 枝の item 0 の鍵は何も区切りません（57-70、store_format.h:130-135）。これに由来する崩れを直したコミットが二つあります（304be1a1、82f32e43）。互換が要らないなら、この癖を残す理由はありません。
- **良い点。** 変更が途中で失敗しない作り（spare、470-518）。

**十倍・1024 CPU で詰まる所：** 木の latch 一語と、commit 中の全停止。

**Linux・NT と比べて劣る軸：** btrfs は node ごとの lock（lock coupling）。NTFS はディレクトリごとの索引。XFS は AG ごとの木。

**建て直すときの要件**
- node ごとの lock か楽観的な版の検査。
- commit の凍結中も前の世代の根を読める。
- 索引を分けて一点に集めない。
- 枝の最初の鍵も境として整える。

---

## 7. D:\Programes\llm vibe slop os\common\store\namespace.c（namespace.h を含む）

**責務：** 目録の上の名前空間（フォルダー・名・記録・extent・中身の更新）。

**判定：消して建て直す**

**根拠**
- **ns_clear がフォルダーの深さだけ再帰する（345-357）。**
  - store_start は起動のたびに利用者の AppData\Temp と C:\Nocturne\Temp を空けます（store.c:414、436）。
  - 道の長さに上限はありません（store.h:7-8）。
  - アプリが Temp の下に深い鎖を作ると、次の起動で 32 KiB の核の stack（sched.h:17）を使い切りうる、という読みです。何段で溢れるかは未計測です。
- **四つ目の変更を黙って捨てる。** tell は三つまでで、それ以上は捨てます（46）。
- **extent が大域の木に入る。** 128 block ごとに一項で、大きな文書は一本の大域の木に何万項も足します（474-499）。
- **一つの呼び出しで何度も降りる。** ns_update と ns_take は一回の呼び出しで何十回も木を降ります（632-756）。
- **二重の実装。** ns_lookup と ns_read を核は使わず、同じ処理を store.c と document.c に別に書いています。
- **良い点。** 記録 → 名の順で作り、名 → 記録の順で消す（191-197、336-340）。move で失敗しうることを最初の変更の前に全部済ませる（250-259）。

**Linux・NT と比べて劣る軸：** ext4・XFS はディレクトリごとの索引と inode ごとの extent 木、NTFS は record ごとの run list。

**建て直すときの要件：** 再帰しない消去、上限の無い変更通知、文書ごとの extent の地図、道の歩きと読みを一か所にまとめる。

---

## 8. D:\Programes\llm vibe slop os\common\store\space.c（space.h を含む）

**責務：** 空き地図（bitmap と索引、hold・pending・予約）。

**判定：消して建て直す**

**根拠**
- 予約・割り当て・使用・解放・commit 後の処理が、すべて一つの latch を排他で取ります（219-303、342-383）。
- 割り当ては一つの cursor から線形に探す first-fit です（238-284）。
- hold と pending は並んだ配列への memmove です（141-190、322-331）。
- latch を持ったまま cache の forget を呼びます（378）。
- space_freeze は commit ごとに全 bitmap を回ります（543-561）。費用は disk の大きさに比例します。
- 読んだ bitmap は放されません（space_trim は拡大の後だけ、484-494）。
- **良い点。** 「使用・hold・空き」の三状態と、「次の commit まで再利用しない」規則（space.h:3-6、39-41）は電断に対して正しいです。

**十倍・1024 CPU で詰まる所：** 一つの latch と一つの cursor。

**Linux・NT と比べて劣る軸：** XFS は AG ごとの空き索引と lock、ext4 は群ごとの bitmap と per-CPU の preallocation。

**建て直すときの要件：** 群ごとの索引と lock、CPU ごとの前借り、範囲で引ける索引、回収できる bitmap の cache。再利用の規則は保つ。

---

## 9. D:\Programes\llm vibe slop os\common\store\volume.c（volume.h と common/store_format.h の書式を含む）

**責務：** mount・format・commit・拡大・dirty の一覧・block の頭の検査。

**判定：消して建て直す**

**根拠**
- commit の書き出しは同期です。stage へ memcpy して一本ずつ書き（169-179、248-263）、地図は一 block ずつ書きます（253）。
- freeze は木の latch の下で行います（208-246）。
- dirty の一覧は一つの latch です（94-146）。
- 書式に互換の道があります。map_copy が 0 なら「古い volume」として扱います（store_format.h:83-84、116-120）。
- **良い点。** 二つの superblock を世代の偶奇で交互に書き、「中身 → 地図 → flush → superblock → flush」の順に書く（248-263）。mount では新しい方から確かめて、古い方へ戻る（349-395）。これは正しく、要件として残すべきです。

**十倍・1024 CPU で詰まる所：** 同期の逐次の書き出しと、commit の間の latch。

**Linux・NT と比べて劣る軸：** btrfs・jbd2 は並列の bio を投げてまとめて待つ。

**建て直すときの要件：** 非同期の多数の書きと flush／FUA の順、読みを止めない凍結、互換の道を書式に置かない。

---

## 10. common/store/names.c・names.h・fold_table.h

D:\Programes\llm vibe slop os\common\store\names.c、names.h、fold_table.h

**責務：** 名の規則、volume に載せた case folding の表、道の分解。

**判定：残せる**

**根拠**
- lock・待ち・大域の状態を持たない純粋な関数です。
- UTF-8 の検査は overlong・surrogate・範囲外を拒みます（37-55）。名の規則（57-70）は製品の決まりどおりです。
- folding の表を volume に載せる作り（namespace.c:781-808）は NTFS の $UpCase と同じ考えで、表の検査もあります（5-19）。
- 折った名の最大が鍵の余り 391 B に収まります（store_format.h:153）。
- 費用は名の長さに比例するだけで、数が増えても詰まりません。

**事実として一つ：** 生成の道具 scripts/gen_fold.py:17 の出力先は kernel/src/fs/fold_table.h で、今の置き場と違います。

**劣る軸：** Unicode の正規化はしません（NTFS も同じで、これは選択の問題）。

---

## 11. D:\Programes\llm vibe slop os\common\store\crc32c.c

**責務：** CRC-32C を計算する。

**判定：消して建て直す**

**根拠**
- 8 表を引く slicing-by-8 です（5-32）。対象の CPU（Zen 4 以上）が全部持つ SSE4.2 の crc32 命令を使いません。
- 全 block の読み（cache.c:252、305）と書き（namespace.c:520、volume.c:59-66）がこれを通ります。
- 表を初めて作るとき、複数の thread が同じ値を同時に書きます（8-17）。害の無い data race です。
- 速さの差は未計測です。

**建て直すときの要件：** 核では命令を使った実装にする。書式に CRC-32C を使う選択は保ってよい。

---

## 12. D:\Programes\llm vibe slop os\kernel\src\net\net.c（net.h を含む）

**責務：** Ethernet・ARP・IPv4・ICMP・UDP・DHCP・DNS（cache 込み）・TCP と、その syscall の全部。

**判定：消して建て直す**

### 規模
- **一つの mutex。** net_lock 一つが全状態と adapter の呼び出しを守り（37、11-19）、全部の net syscall が入口から出口まで持ちます（2726-2731）。利用者 memory の写しもその下です（1345-1351、2210、2618）。一つのアプリの頁違反で、全部の通信が止まります。
- **一本の thread。** 受けは thread「net」一本です（2383-2439）。lock を渡す相手は 2 ms／256 KiB／2 ms の決め打ちで決めます（2372-2422）。
- **線形の表。**
  - 接続の引き（1700-1708）、listen の待ち数（1628-1633）、accept（2080-2089）、port の選び（2035-2063）、batch ごとの後片付け（2353-2370、2432-2435）は、どれも全 TCB を線形に走査します。
  - 表は縮まないので（247-248、2017-2033）、費用は今の数でなく過去の最大に比例します。
  - UDP の配り（648-661）も線形です。
- **固定の表。** 受け queue 64 個（231-232）、出力の frame 一つ（349）、ARP 16（413）、ping 8（517）、UDP の queue 8（589）。
- **接続ごとの先取り。** 接続ごとに受け 1 MiB・送り 1 MiB を最初に確保します（1188、2131-2136）。listen は SYN を受けた時点で 2 MiB を確保し（1659-1662）、SYN cookie はありません。
- **見回り。** net_setup_cancel の待ちは 50 ms ごとに起きて見ます（113-117）。

### 持ち主と層
- **socket が仕様のハンドルでない。**
  - 仕様では n_handle の N_KIND_CONNECTION です（docs/nocturne-native-abi.md:36、65、abi.h:155）。
  - 実装は大域の表の添字を pid で照らすだけで（2011-2015、737-741）、N_KIND_CONNECTION はどこでも使われていません。
  - イベントの待ち行列で socket を待てないので、socket ごとに待つ thread が要ります。添字は使い回されます。
- **ブラウザーの都合が二つの部品に入っている。** HTTP/2 の取り消しのための欄が struct task にあり（sched.h:56）、スケジューラーが網の後始末を呼びます（sched.c:604、1328、1367）。
- **故障注入が本番の道にある。** SYS_NET_TEST（1281-1291、1719-1740、2659-2672）。
- **DHCP と DNS の resolver・cache が核にある**（745-1180）。
- **網の初期化が VM の道を知っている。** rtl8125 が見つからないと vm_net_probe（netvsc・e1000・tulip）を呼びます（2506、vm.c:72-75）。
- **adapter は一つだけ。** 二つ目は無視します（2456-2473）。netif は自分を指す引数を受けないので（net.h:13-22）、同じドライバーの二枚目を作れません。

### 正しさと仕様
- **PMTU の発見が無い。** 常に DF を立てます（400）が、ICMP は 8 と 0 しか見ません（525-546）。断片は捨てます（2339）。IPv6 はありません。DNS は A だけです（1071）。
- **TIME-WAIT が短い。** 2 秒で、2 MSL より短く（1216）、PAWS もありません。
- **推測できる乱数。** ISN と DNS の id は、一度だけ種を入れた xorshift です（139-147、1682、2150、1142）。DNS の送り port は連番です（616-619、1149）。全アプリが共有する核の DNS cache（1012-1033）に、偽の答えを入れられる形になっています。
- **RST とセグメントの検査。** RST は窓の中ならどこでも受けます（1757、RFC 5961 の challenge ACK は無い）。受け入れの検査より前に ACK を処理します（1812-1876）。
- **DHCP の更新が RFC 2131 の表 5 と違う。** 0.0.0.0 から broadcast し、ciaddr は 0、option 50 と 54 を付けます（780-791、803、874-879）。lease の満了で番地を手放す道もありません。
- **ARP の表が荒れる。** どの送り手の ARP も表に入れ、一番古く覚えたものを追い出します（440、468）。16 台より多い LAN では gateway の項が追い出されます。
- **近隣の変化に追従しない。** 相手の MAC を接続ごとに固定で持ちます（1676、2149）。

### 時間
- clock の ns で揃っています（7-10）。
- 接続ごとの alarm → queue → thread で、早める方向だけ動かす timer（240-300）と、RFC 6298 の RTO 計算（1491-1503）は良い作りです。

### 十倍・1024 CPU で詰まる所
一つの mutex、一本の thread、線形の表、接続ごとに 2 MiB の先取り。

### Linux・NT と比べて劣る軸
- Linux：socket ごとの lock、ハッシュの接続表、NAPI と RSS/RPS、GRO/TSO、SYN cookie、siphash の ISN、RFC 6056 の port の乱択、PMTUD、IPv6、epoll。DHCP と DNS は利用者空間。
- NT：partition と RSS、AFD と IOCP。DHCP と DNS は service。
- 全部の軸で劣ります。

### 建て直すときの要件
- 接続は native handle にし、イベント待ち行列に入れる。
- 接続ごとの lock、RCU で引くハッシュの表、RSS で queue ごとに CPU を分ける。
- buffer は自動調整、listen は SYN cookie。
- RFC 6528・6056・5961、PMTUD、断片の組み立て、IPv6。
- DHCP と DNS は利用者空間へ移す。
- 故障注入は本番の外に置く。
- task の網の欄を無くし、取り消しは handle の一般の仕組みで行う。

**継ぎ目：** syscall.c:3115 の旧い番号範囲 96-127、net.h、sched、vm、hv_util（vm/hv/hv_util.c:192、222 が net_config を読む）。

---

## 13. D:\Programes\llm vibe slop os\kernel\src\net\rtl8125.c

**責務：** RTL8125B の初期化、ring、割り込み、電源の変化。

**判定：消して建て直す**（初期化の手順は資料として持ち越す）

**根拠**
- 静的な大域で一台だけです（150-159）。ring は受け・送り一本ずつ 256 個で（139-140）、RSS も複数 queue もありません。
- vector は決め打ちの 0x4C で、NVMe の範囲とコメントで結んでいます（50）。割り当て器 irq_vector_alloc（idt.c:138）は使いません。
- MSI が無いときの道は 8259 PIC の unmask です（609-613、idt.c:215-218）。APIC で動く実機には届きません。
- 送りは frame 全体を memcpy し、offload はありません（494-514）。
- 呼び出しは全部 net_lock の下という前提です（net.h:9-12）。
- link は 1 秒ごとに見回ります（466）。
- 対応は 8125B だけで（555-559）、microcode は当てません（2-3）。実機での安定は未確認です。
- DMA には物理番地をそのまま渡します（589-599）。IOMMU が翻訳の mode のときにどうなるかは未確認です。
- **良い点。** 割り込みで mask して thread が戻す作り（441-492）、quiesce と resume、初期化の手順（230-375）。

**十倍・1024 CPU で詰まる所：** 一本の queue、CPU 0 への割り込み、一本の thread。

**Linux と比べて劣る軸：** r8169 は MSI-X、RSS、offload、firmware の読み込みを持つ。

**建て直すときの要件：** 装置ごとの instance、queue ごとの vector と CPU、vector は割り当て器から、offload、firmware、self を受ける netif。

---

## 14. D:\Programes\llm vibe slop os\kernel\src\net\dns_name.h

**責務：** DNS の名を wire 形式に encode／decode する（RFC 1035）。

**判定：残せる**（置き場所は核の外の resolver）

**根拠：** 純粋な関数です。label 63・全体 255・圧縮の指の歩きの上限を守ります（17-19、33、47）。空の label と NUL を拒み、末尾の点と根を扱います（12、22）。

---

## 判定の外の継ぎ目（建て直しの前提）

- **block の層が同期の単発。**
  - blk は一度に一つの同期の要求で、大域の gate_lock を通り、装置は 16 台までです（blk.c:8、36-81）。
  - NVMe は「I/O queue 一本、command は一度に一つ、bounce buffer」で（nvme.c:1-2、76）、MSI があっても 10 ms ごとに起きます（167-168）。
  - AHCI も一度に一つで slot 0、NCQ は無く、28 bit の古い道があります（ahci.c:2-3）。
  - この層のままでは、格納庫をどう作っても queue の深さを使えません。CPU／node ごとの queue、非同期の完了、IOMMU の写像つきの直 DMA が先です。
- **上の syscall の互換の道。** 大きさの無い旧い SYS_ENTRY_INFO と SYS_FOLDER_LIST が残っています（abi.h:185-190、syscall.c:3074、3080）。

---

## 残せるものと所感

**残せるもの：**
- names.c・names.h・fold_table.h：純粋で数に依存せず、volume に表を載せる設計が正しい。
- dns_name.h：純粋で RFC の上限を守る。置き場所は核の外。

**コードでなく要件として持ち越すもの：**
- volume の書き順と二つの superblock。
- space の「次の commit まで再利用しない」規則。
- 文書の版（snapshot 読み）。
- TCP の接続ごとの alarm と RFC 6298 の RTO 計算。
- rtl8125 の初期化の手順（資料として）。

**所感**
- 格納庫は電断に対する順序の設計は筋が良いです。ただ、木・空き地図・cache・文書の表・門がそれぞれ大域の一点で、下の block 層も同期の単発なので、CPU と数が増えると全部が一列に並びます。
- 網は、一つの mutex と一本の thread と線形の表の上に作った小さな client です。socket は仕様のハンドルになっておらず、DHCP・DNS・故障注入・ブラウザーの都合まで核に入っています。
- どちらも規模の欠陥が構造そのものにあるので、部分を残して継ぐ形にはなりません。
