# 記憶の driver の差分の監査（八回目）

## 依頼文

> Nocturne OS（独自の OS、カーネルは C、対象は実機）の記憶の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。
>
> 作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。
>
> 材料：
> - 差分：`git diff fe71d739 HEAD -- kernel/src/dev/ahci.c kernel/src/dev/nvme.c kernel/src/dev/blk.h kernel/src/dev/blk.c kernel/src/dev/pci.c kernel/src/dev/pci.h`。周りのコードは今のファイルで読む。
> - 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の ahci.txt（AHCI 1.3.1）、nvme14.txt、nvme24.txt、nvmcmd.txt、nvmepcie.txt、pcie6.txt、pci30.txt。原本は `D:\Programes\OS DEV INFO`。
> - カーネルの規則：`.claude/rules/kernel.md`（spinlock の下で眠らない、割り込みの処理と呼ぶ側は同じ spinlock、など）。
>
> 範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外の問題は、データを失うものだけを挙げてください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。
>
> 報告：全文を `docs/records/kernel-core-20261101/audit-usbstorage-fix8-storage.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（データを失う／当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 6 件。データを失う 1 件、当たれば壊れる 2 件、きれいさ 3 件。

証拠の範囲はコードと文書の静的な照合だけ。ビルド、試験、QEMU、実機は動かしていない。行番号は `9bea7571` のファイルのもの。差分の範囲は `kernel/src/dev/ahci.c`・`nvme.c`・`blk.h` で、`blk.c`・`pci.c`・`pci.h` に差分は無い。

| 番号 | 重さ | 場所 | 一行 |
|---|---|---|---|
| N1 | データを失う | `nvme.c:142`・`nvme.c:545` | 別の装置の判定が SN と MN だけで、文書の一意の識別子（SUBNQN、古い版は VID+SN+MN）を見ない |
| A1 | 当たれば壊れる | `ahci.c:815`・`ahci.c:575` | `halt` が FRE まで落とすので、`port_up` が FRE=0 の PxTFD を読み、要らない COMRESET から cache 喪失へ進む |
| A2 | 当たれば壊れる | `ahci.c:308` | 書込みの overflow で文書が要ると言う COMRESET を、BSY・DRQ の読みに任せた |
| N2 | きれいさ | `nvme.c:347`・`nvme.c:371` | FLR の前に通る錠が cq_lock だけで、submit の doorbell は sq_lock の下にある |
| N3 | きれいさ | `nvme.c:548`・`nvme.c:1022` | 別の装置を `cut` にしたので、動く装置に SHN を書かずに電源が落ちる |
| N4 | きれいさ | `nvme.c:451`-`455` | 命令ごとの CSTS の MMIO 読出しが cq_lock と割り込み閉じの中へ入った |

## データを失う

### N1. 別の装置の判定が文書の一意の識別子を見ない

- **重さ**：データを失う。当たる条件は狭い。
- **場所**：`kernel/src/dev/nvme.c:142`（`identity[60]`）、`nvme.c:545`（`memcmp(c->identity, id + 4, sizeof c->identity)`）。
- **文書**：NVMe Base 2.4 の 4.7.1.2（NVM Subsystem Unique Identifier）。NVM subsystem の一意の識別子には Identify Controller の SUBNQN（bytes 1023:768）を使う。1.2.1 より前の版の装置は、PCI Vendor ID・SN・MN を組み合わせる（Figure 141 は SSVID も入れる）。SN と MN の値を一意にする方法は仕様の外と書いてある。Figure 338 で SUBNQN は 1.2.1 以降で必須。コードの注が引く 4.5.2 は informative の節で、SN と MN の書式だけを書いており、一意とは書いていない。
- **起きる道筋**：
  1. 眠りの間に装置が入れ替わる。新しい装置の SN と MN が前の装置と同じで、SUBNQN か VID が違う。
  2. `awake` → `enable` → `read_controller` の `memcmp` が一致する。
  3. `c->discovered` が真なので namespace は読み直さない。前の装置の block device のまま、ファイルシステムの書込みが新しい装置へ入る。新しい装置の中身が壊れる。
- **直し方の案**：比べる範囲を bytes 63:00（VID、SSVID、SN、MN）と bytes 1023:768（SUBNQN）にする。SUBNQN を持たない古い装置はそこが 0 同士で一致し、前半が VID+SN+MN の組になる。注の節番号を 4.7.1.2 に直す。
- **同じ行の文言**：`nvme.c:543` の注と `nvme.c:546` の文は「眠りの後」と書く。`read_controller` は `recover` の `enable` からも通る。文は「前の装置と違う」とだけ書く。

## 当たれば壊れる

### A1. FRE を落とした後の PxTFD を読み、要らない COMRESET へ進む

- **重さ**：当たれば壊れる。
- **場所**：`kernel/src/dev/ahci.c:815`（`halt` の `port_stop`）、`ahci.c:804`・`ahci.c:306`-`307`（差分の注）、`ahci.c:575`（`port_up` の読み）、`ahci.c:577`（FRE を立て直す行。読みより後）、`ahci.c:415`（`comreset` の `disk_cache_lost`）。
- **文書**：AHCI 1.3.1。
  - 6.2.2：HBA が転送を止めたとき、装置は BSY・DRQ が 0 で ERR が 1 の D2H Register FIS を送る。そのときは COMRESET なしで再開できる。
  - 6.2.1：HBA はこの D2H を待たずに割り込みの状態を立てる。
  - 6.2.2.1：手順は ST を 0 にして CR を待ち、PxSERR と PxIS を消し、BSY か DRQ があれば COMRESET。FRE を落とす手順は無い。
  - 3.3.7 の FRE：0 の間は受信した FIS を受け付けない（初期化の後の最初の D2H を除く）。
  - 10.3.2：FRE が 0 の間、FIS は内部に留まる。IFS などで止まった HBA は、ST を 0 にするまで FIS を載せないことがある。
- **起きる道筋**：
  1. 非 queued の命令に IFS・HBDS・HBFS が出る。`settle` は `need_reset` を立てない（差分の行）。
  2. 待ち手が `halt` に入り、`port_stop` が ST に続けて FRE を落とす。ST を落とす前の HBA は FIS を載せないことがあり、ST と FRE の間は CR が落ちるまでの短い間だけなので、D2H はここで受け付けられない。
  3. 次の呼ぶ側の `port_up` は `reset` が偽のまま、FRE が 0 の状態で PxTFD を読む（`ahci.c:575`）。命令を送ったときに立った BSY が残っている。
  4. `reset` が真になり、`comreset` が `disk_cache_lost` を呼ぶ。流していない書込みがあれば `cache_lost` が立ち、以後の `blk_persist` は全部失敗する（`blk.h:66`-`73`）。volume は再起動まで commit できない。
- **比べたもの**：`fe71d739` はこの道で必ず COMRESET をした。`9bea7571` も D2H が FRE の前に届かなければ COMRESET をする。退行ではない。差分の注（`ahci.c:306`・`ahci.c:547`「engine は単独で止める」）のとおりに動かず、6.2.2.1 の手順とも違う。
- **直し方の案**：`halt` の `port_stop` を `list_stop` に換える（`if (!lost && !list_stop(p)) port_force_stop(p)`）。呼ぶ側のメモリーへ入るのは command list の engine の DMA で、FIS の受信先は port 自身のメモリーなので、呼ぶ側へ返す条件はこれで満たす。COMRESET の道は `port_up` が `port_stop` を呼ぶので変わらない。文書は D2H を待つ時間を定めていないので、読むまでに来なければ 6.2.2.1 のとおり COMRESET でよい。

### A2. 書込みの overflow の COMRESET を BSY・DRQ の読みに任せた

- **重さ**：当たれば壊れる。当たる条件は狭い。
- **場所**：`kernel/src/dev/ahci.c:308`（`if (is == 0xFFFFFFFFu || p->queued) p->need_reset = true;`）。`IS_FATAL` は `IS_OFS` を含む（`ahci.c:118`）。
- **文書**：AHCI 1.3.1 の 6.1.5。書込みの overflow では HBA が HOLD を送り続けることがあり、片付けるのに COMRESET が要ると書いてある。DMA の書込みで HBA が転送を終わらせようとする場合も、致命の状態と書いてある。読出しの overflow は致命の誤りの回復手順でよく、COMRESET は要求していない。
- **起きる道筋**：
  1. 非 queued の書込みで PxIS.OFS が立つ。`settle` は `need_reset` を立てない。
  2. `halt` が止め、`port_up` は PxTFD を読む。普通は BSY が残っているので COMRESET になる。
  3. HBA が転送を終わらせ、装置の D2H で BSY と DRQ が 0 になっていた場合は、COMRESET なしで ST を立てる。文書が要ると言う COMRESET が無いまま次の命令を送る。
- **直し方の案**：`if (is == 0xFFFFFFFFu || is & IS_OFS || p->queued) p->need_reset = true;`。`settle` の `is` は reap の値か PxIS の読みを持つので、どちらの道でも OFS を見る。読みと書きを分けるなら命令の向きを持ち込むことになるので、OFS は一律に COMRESET がよい。誤りの中で OFS は呼ぶ側が記述を誤った時だけの重い誤りで、頻度の損は無い。

## きれいさ

### N2. FLR の前に通る錠が submit の道を含まない

- **重さ**：きれいさ。文書の上で害は出ない。
- **場所**：`kernel/src/dev/nvme.c:347`-`348`（注）、`nvme.c:371`-`372`（cq_lock だけを通る）、`nvme.c:421`-`438`（`run` は sq_lock の下で `up` を読み、同じ錠の下で `submit` が doorbell を書く）。
- **文書**：PCIe 6.0 の 6.6.2。Implementation Note の手順 1 は、FLR をする側が関数に直接触れる他の software と同期し、触れないようにする、と書く。同じ節は、FLR の間に届いた Request を関数が黙って捨ててよいとも書く。
- **起きる道筋**：
  1. CPU A が sq_lock の下で `ready` の真を読む。
  2. CPU B の `controller_stop` が `controller_down` と cq_lock の通過を済ませ、`pci_function_reset` に入る。
  3. CPU A の doorbell の書込みが関数へ届く。

  doorbell は posted の書込みで、捨てられても CPU も関数も困らない。`queue_clear` が sq_lock を取るので、`enable` までは延びない。差分の注「up を読んだ者は cq_lock より先へ進まない」は、この道では偽になる。
- **直し方の案**：`nvme.c:371`-`372` で sq_lock も通す。`queue_clear` と同じく sq_lock が先。注の「run, reap」に submit を足す。

### N3. 別の装置を `cut` にしたので SHN が書かれない

- **重さ**：きれいさ。こちらの書込みは新しい装置に入っていないので、こちらのデータは失わない。
- **場所**：`kernel/src/dev/nvme.c:548`（`cut` を立てる）、`nvme.c:1022`（`cut` なら飛ばす）、`nvme.c:1010`-`1014`（注）。
- **文書**：NVMe Base 2.4 の Figure 41 の CC.SHN。電源を落とす前と PCI の電源状態を変える前に、host が書くべきとある。3.6.1 も通常の shutdown の手順で同じことを書く。
- **起きる道筋**：
  1. 差分の前まで、`cut` は止まらない controller だけに立った。注の「one cut off can keep nothing」はその前提で書いてある。
  2. 差分の `read_controller` は、応答する健全な別の装置にも `cut` を立てる。
  3. `nvme_shutdown` は MMIO が動く装置を飛ばす。その装置は SHN の通知なしで電源を失い、unsafe shutdown になる。
- **直し方の案**：止まらない controller と、使わないだけの別の装置を分ける。別の装置なら `nvme_shutdown` は `live` が偽なので `delete_queues` を飛ばし、CC.SHN の書込みと `shutdown_done` だけをする。どちらも MMIO だけで済み、Bus Master が切れていても動く。戻り値は `in_use` を見ない形にして、この装置の完了の遅れで電源の変更を止めない。

### N4. 命令ごとの CSTS の読出しが錠と割り込み閉じの中へ入った

- **重さ**：きれいさ。性能の測定はしていないので、損の大きさは分からない。
- **場所**：`kernel/src/dev/nvme.c:451`-`455`。
- **文書**：PCIe 6.0 の 2.8。MMIO の読出しは non-posted で、応答が来なければ Completion Timeout まで終わらない。Function の既定の範囲は 2.8 が 50 ms までと書く。CPU 側の Root Complex の値はこのコードからは分からない。
- **起きる道筋**：
  1. 命令を出した直後の最初の周回は `seen == start` なので、ほとんどの命令がここを通る。`fe71d739` もこの読出しを一度していたが、錠の外だった。
  2. 今は `cq_lock` を取り、割り込みを閉じたまま `rd32(c, CSTS)` を待つ。同じ queue の `nvme_isr` と他の待ち手は、その間 spin する。
  3. 装置が抜けた場合は、完了の時間切れまで割り込みが閉じたままになる。

  錠の中に置く理由は FLR との順序で、`.claude/rules/kernel.md` の「spinlock の下で眠らない」には反していない。
- **直し方の案**：起こされた後の周回でだけ読む。命令を出した直後の周回は読まずに、短い期限で待つ。死んだ controller の発見は短い期限の分だけ遅れる。

## 調べて指摘にしなかったもの

- `settle`・`halt`・`attention_over`・`reap` から `need_reset` を外した行：link の変化は `port_changed` が必ず `need_reset` を立て（`ahci.c:1218`）、呼ぶ側は `unverified` の間 `enter` で待つ。queued は `settle` と `halt` が `p->queued` を見る（`ahci.c:308`・`ahci.c:319`・`ahci.c:804`）。6.2.2.2 と合う。
- `port_up` の COMRESET なしの道が `ahci.c:573` で PxSERR を消す行：`resetting` の間の link の変化は記録なしで消える。それでも COMINIT の後は HBA が BSY を立てる（10.4.2、5.3 の P:ComInit）ので、`ahci.c:575` の読みで COMRESET になる。装置が短い `list_stop` の間に応答まで戻らない限り抜けない。旧版も COMRESET で同じ変化を隠していた。指摘にしない。
- 時間切れの `disk_cache_lost` を消した行：非 queued で BSY が残れば次の命令の前に `comreset` が呼ぶ。queued は必ず COMRESET になる。どちらの場合も、次の flush の成功より先に `blk_cache_lost` が来る。`blk_persist` は flush の後で `cache_lost` を読む。失うものは無い。
- `reap` の doorbell の条件（`nvme.c:229`）：`nvme_shutdown` は `live` が真の時だけ `run_polled` を使う（`nvme.c:1025`）。`live` が偽になった後は必ず次の `enable` の `queue_clear` が通る。admin の CQ が詰まる道は無い。
- `controller_stop` の `spin_unlock(l, spin_lock(l))`：`spin_lock` は引数として先に評価される（`arch/spin.h:169`）。`nio` と `c->io` の順序は `io_alloc` の release と acquire で合う。
- `read_controller` の `cut` の道の書込み：`pci_disable_busmaster` は眠らない道で、`reset_lock` の下で呼ばれる。以後の `transfer` と `blk_nvme_persist` は `recover` → `enable` が偽で失敗する。`blk_persist` は未 flush の書込みがあれば失敗を返す。
- `blk.h:73` の注：flush が終わって数える前に `blk_cache_lost` が来ると `flushed < given` で `cache_lost` が立つ。以後の persist は全部失敗し、失うものは無い。注のとおり。
- `was_live` への名の変更：振舞いは同じ。Figure 41 の EN は、Controller Reset が完了した命令の結果へ影響しないと定める。`lost` の判定と合う。
- 差分の外のデータ喪失：見つからなかった。
