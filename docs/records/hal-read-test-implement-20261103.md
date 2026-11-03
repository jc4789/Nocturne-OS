Nocturne OS（`D:\Programes\llm vibe slop os`）の設計の読む試験です。返事と報告は全部日本語で書きます。識別子とパスは元の表記のままです。

## 読むだけ
コード、設定、git の状態を変えません。ビルド、QEMU、スクリプトの実行をしません。書いてよいファイルは報告の `docs/records/hal-read-test-implement-20261103.md` の一つだけです。頭にこの依頼文を全文そのまま写し、続けて報告の全文を書きます。

## 材料
`AGENTS.md`、`.claude/rules/map.md`、`.claude/rules/code.md`、`.claude/rules/architecture.md`、`docs/hal.md`、それに今のコードです。`docs/hal.md` は設計で、まだ作られていません。

## やること
`docs/hal.md` の設計どおりに作るとして、次の三つの仕事の手順を、今のコードの実際のファイルと関数に照らして具体的に書きます。

1. `kernel/src/dev/nvme.c` を、この設計の形へ書き換える。
2. 機械に新しい有線の NIC の実装を一つ足す（製造者は Intel の e1000e の実機とします）。
3. AMD の GPU の画面の実装を足す。プログラムの Vulkan と、画面への表示の両方まで通す。

## 答えてほしいこと
- 各仕事で、文書だけでは決まらず迷った所を全部挙げます。迷った所ごとに、文書のどの文を読んでどう迷ったかを書きます。
- 文書と今のコードが食い違う所を、`file:line` つきで全部挙げます。
- 文書の形では実装できない、または正しく実装するのが難しいと考える所を、理由つきで挙げます。
- 文書が要らないものを作らせている所を挙げます。

証拠の無い所見は出しません。確かめられないことは、確かめられないと書きます。良かった点を褒める必要はありません。

最後の返事は、報告のパスと、重い順の要点を 15 行以内で返します。

---

# 報告：`docs/hal.md` の読む試験（三つの実装の仕事）

## 結論

- 仕事 1（`nvme.c`）は手順を書ける。ただし文書だけでは 12 の点が決まらない。止まるほど重いのは、`attach` から `struct pci_device` へ着く道、記録の粒度、`attach` の失敗の意味、`attach` の文脈と順の四つである。
- 仕事 2（e1000e）は、文書だけでは書き始められない。`hal.md:105` の「新しい輪の見出し」に、名前も形も無い。網の手術が先に要る。
- 仕事 3（AMD）は、文書の外で止まる。特権の半分を書く材料が、許された範囲に見つからない（A1）。文書の中でも、`hal.md:140` がメモリー、同期、優先度、窓の合成との受け渡しを決めていない。Vulkan と画面を端まで通すには、この四つが要る。
- `hal.md:9-13` の物差しの 1（Claude が正しく実装できるか）への答えは、この試験では「仕事 1 は補えばできる。仕事 2 と 3 は設計を足さないとできない」である。

## 読んだ範囲と限界

- 読んだもの：`docs/hal.md`、`docs/open.md`、`AGENTS.md`（ディスクの今の版）、`code.md`、`architecture.md`、`kernel.md`、`nvidia.md`、`map.md`、下に行番号を書いたソース。
- 動かしていない。ビルド、QEMU、スクリプトの実行はしていない。所見は全部、静的な読みである。
- 手順の中の「（補い）」は、文書に無く私が補った所である。その補いが正しいとは言わない。
- `kernel/src/dev/hal.h` と `hal_add` は今のコードに無い（`rg` で 0 件）。手順は、HAL の共通の部分が建った後を仮定する。
- 網の手術と GPU の手術の後の形は、文書に細部が無い。仕事 2 と 3 の手順は、その分だけ推測を含む。

---

## 仕事 1：`kernel/src/dev/nvme.c` を設計の形へ書き換える

### 手順

1. `nvme.c:1063-1064` の `nvme_pm` と、`nvme.c:1066-1083` の `nvme_init` を消す。
2. `init.c:28` の宣言と `init.c:48` の呼出しを消す。
3. `nvme.c:186` の `ctrls` と、`nvme.c:132` の `next` を消す。`nctrls` は番号のためだけに残す（補い）。
4. `struct hal_identity` の表を書く。行は `{cls 0x01, subcls 0x08, progif 0x02}` の一行である（`nvme.c:1070` の条件）。`bus` の値の名は文書に無い（補い）。
5. `nvme_attach(struct hal_device *d)` を書く。中身は `nvme.c:1071-1080` と `start`（`nvme.c:920-975`）である。`c` を `d->state_of_driver` に置く。
6. `start` を、結果を返す形にする（`nvme.c:923-926`、`:929-932`、`:944-947`、`:948-950`、`:955-962` の戻り）。
7. `nvme_shutdown`（`nvme.c:1030-1051`）の輪を外し、一つの制御器の `nvme_quiesce(struct hal_device *d)` にする。
8. `nvme_resume`（`nvme.c:1056-1061`）の輪を外し、一つの制御器の形にする。
9. `nvme_lost(struct hal_device *d)` を書く。材料は `nvme.c:393-396` と `nvme.c:557-561` の「切る」処理である（補い）。
10. `const struct hal_driver nvme_driver` を書き、`.stage` を今の `PM_STAGE_STORAGE` に当たる値にする。`kernel/src/dev/hal_drivers.c` に一行足す。
11. `nvme.c:25` の `dev/pm.h` を `dev/hal.h` に換える。
12. `nvme.c:970-974` の `blk_register` は残す（`hal.md:104`「今のまま」）。`.read`、`.write`、`.persist`（`nvme.c:640-642`）も残す。
13. `nvme.c:1-16` の頭のコメントと、`map.md` の行を直す。
14. 確かめる。QEMU の NVMe は今の既定である（`.claude/skills/nocturne-project/references/qemu-visual.md:17`）。起動、`store:` の行、再起動の SHN、眠りの拒否の行を見る。

変えないもの：`run`、`reap`、`controller_stop`、`enable`、`recover`、PRP の処理。

### 迷った所

**N1. `attach` から `struct pci_device` へ着く道が無い。**
- 読んだ文：`hal.md:37-47` の `struct hal_device`、`hal.md:50`「`void *` を許すのは `state_of_driver` だけである」、`hal.md:57` の `struct hal_identity`。
- 迷い：`nvme.c` は `pci_bar_address`、`pci_irq_enable`、`pci_reset_left`、`pci_function_reset`、`pci_disable_busmaster` に `struct pci_device *` を渡す。記録には PCI の住所も指も無い。`state_of_driver` は実装のものなので、バスは使えない。`pci_of(d)` を足すのか、`struct pci_device` に記録を埋めるのか、決まらない。

**N2. 記録の粒度が決まらない。**
- 読んだ文：`hal.md:31`「ディスクは USB メモリーの子で、USB メモリーは xHCI の子で、xHCI は PCI の機能の子である」、`hal.md:77` の `hal_add(parent, &identity)`、`hal.md:34` の `enum hal_kind`。
- 迷い 1：`xHCI は PCI の機能の子` を字のとおりに読むと、一つの機能に二つの記録がある。NVMe でも「機能の記録」と「制御器の記録」が別なのか、決まらない。
- 迷い 2：namespace ごとに `HAL_STORAGE` の子を作るのか、決まらない。一台の制御器は複数の `struct blkdev` を持つ（`nvme.c:970-974`）。
- 迷い 3：`hal_add` は引数に種類が無く、必ず実装を引く。namespace の子を `hal_add` で作ると、実装が引けず `HAL_UNBOUND` になる。`hal.md:165` はサービスの管理者が `HAL_UNBOUND` を照らすと書く。子の記録が照らされることになる。
- 迷い 4：`attach` の中から `hal_add` を呼んでよいのか、書いていない。木の錠を持ったまま `attach` を呼ぶなら、ここで詰まる。

**N3. `attach` の失敗の意味が無い。**
- 読んだ文：`hal.md:64` の `int (*attach)`、`hal.md:164`、`hal.md:43`。
- 迷い：失敗したら記録はどの状態か、書いていない。`HAL_UNBOUND` へ戻すと `quiesce` が呼ばれない。今の `nvme_shutdown` は、持っていない制御器にも SHN を書く（`nvme.c:1022-1029`「for every controller of the machine」）。`start` が失敗しても `c` は `ctrls` に残るので、これが働く。文字どおりに移すと、この保証が消える。
- 迷い：`attach` は常に 0 を返し、失敗は実装の中に持つのか。その場合、`rtl8125`（`rtl8125.c:555-559`）のような「この版は扱わない」の失敗と、形が分かれる。

**N4. `attach` の文脈と順が無い。**
- 読んだ文：`hal.md:77`、`hal.md:161-162`。
- 迷い 1：`attach` が眠ってよいのか書いていない。`enable` は眠る（`nvme.c:334`、`:480`）。`CAP.TO` の待ちは 127.5 秒まである（`nvme.c:937`、`:942`）。
- 迷い 2：`hal_add` を列挙の中で呼ぶと、`iommu_init`（`init.c:43`）より前に `attach` が走る。`iommu.h:40` は「after pci_init, before a driver enables an interrupt」を求める。`pci_irq_enable` は `iommu_msi_message` を使う（`pci.h:96-97`）。
- 迷い 3：同期なら、止まった NVMe が後の xHCI を遅らせる。今は `init.c:44-48` の順なので、NVMe は最後である。非同期なら、`store_init` の待ちの条件が足りない（`store.c:310` は `usb_enumerating` だけを見る）。

**N5. `lost` を誰が呼ぶのか、終わりの状態か、決まらない。**
- 読んだ文：`hal.md:78`、`hal.md:87`、`hal.md:35` の `HAL_LOST`。
- 迷い 1：PCI に抜き差しのコードが無い（`pci.c:1199` は読むだけ、`pci.h:198-199` は「the firmware's」）。外から `hal_lost` を呼ぶ者がいない。実装が自分で呼ぶのか、決まらない。
- 迷い 2：`nvme.c` の「切る」には三通りある。止まらない（`:393-396`）、別の装置に換わった `stranger`（`:557-561`）、スレッドの kill で諦めた `called_off`（`:389-391`）。どれが `HAL_LOST` か書いていない。
- 迷い 3：`stranger` には SHN を書く必要がある（`nvme.c:1026-1027`、`:1035`）。`HAL_LOST` の後に `quiesce` を呼ぶのか、書いていない。呼ばないなら、この処理が消える。

**N6. 眠りの順が決まらない。**
- 読んだ文：`hal.md:86`「眠りの順は木と `stage` で決まる」。
- 迷い 1：木と `stage` がぶつかったらどちらが勝つか、書いていない。
- 迷い 2：今の順は `stage` の中で登録の順で、失敗で止まり、触れた分だけ逆順で戻す（`pm.c:187-198`、`:327-334`、`:191` の `d->down`）。`quiesce` が false を返した記録は `HAL_QUIESCED` か、書いていない。今は戻す側に入る（`pm.c:191` のコメント）。

**N7. 台帳が二つになる。**
- 読んだ文：`hal.md:31`「全部一つの木に載る」、`hal.md:104`「今のまま」、`architecture.md:26`「一つの物は一つの台帳に載る」。
- 迷い：`blk.c:7-14` の一覧と `blk_get`（読むのは `pm.c:178-179` と `store.c:292-293`）を残すのか、`hal_each(HAL_STORAGE)` に換えるのか、書いていない。`struct blkdev` は記録を指さないので、`n_hal_storage` は申告を読めない。
- 迷い：`hal_device.name` と `blkdev.name`（`blk.h:19`、`nvme.c:971`）の関係も書いていない。

**N8. 「一回の間接呼出し」に合わせて `blk_by_blocks` を変えるのか。**
- 読んだ文：`hal.md:104`、`hal.md:190`、`hal.md:19`。
- 迷い：今は `d->read`（`blk.c:85`）と `move`（`blk.c:113`、`nvme.c:885`）の二回である。文書の「今」が誤りか、一回へ減らせという指示か、決まらない。C8 を見る。

**N9. `resume` の後の状態。**
- 読んだ文：`hal.md:35`。
- 迷い：`nvme_resume` は印を立てるだけで、次の要求が制御器を起こす（`nvme.c:1053-1059`、`:798-807`）。`HAL_RUNNING` は起きていない制御器を指す。問題が無いのか、文書では分からない。

**N10. `kernel_owned` と `HAL_UNBOUND` の関係。**
- 読んだ文：`hal.md:43`、`hal.md:164`。
- 迷い：今の持ち主は `pci_device` の `kernel_owned`（`pci.h:28`、`pci.c:283`、`nvme.c:933`）と `claimed`（`pci.h:30`）である。`pciuser.c:831` がこれを読む。記録の `driver` と `state` は同じことを言う。どちらを残すか書いていない。

**N11. IOMMU で絞るのは NIC だけか。**
- 読んだ文：`hal.md:121`。
- 迷い：NVMe も絞られていない（`nvme.c:10-12`、`iommu.h:9-10`）。文書は NIC だけに触れる。NVMe は呼ぶ側の記憶へ直接 DMA する（`nvme.c:819-837`）ので、絞るなら形が変わる。今回は触らないと読んだが、文書に理由が無い。

**N12. `quiesce` と `hal_lost` が同時に来たとき。**
- 読んだ文：`hal.md:78`「どの文脈からでも呼べる」、`hal.md:212`「状態の書き手を `hal.c` の一つにする」。
- 迷い：`quiesce` は割り込みを閉じて眠らない（`pm.h:28-29`）。`lost` の仕事が眠ってよいか書いていない。眠るなら、同じ mutex で排他できない。H4 を見る。

---

## 仕事 2：Intel の e1000e の実機の実装を足す

### 手順

前提：`hal.md:105`、`:117`、`:121` の網の手術が済んでいる。`AGENTS.md:91` が「悪いコードを動かしたまま、その上に新しいコードを足さない」と書くので、`struct netif` の上には書かない。

1. 置き場を `kernel/src/net/e1000e.c` にする（補い）。Makefile は `kernel/src` を `find` で拾う（`Makefile:40`）。
2. 材料を `freebsd-src/sys/dev/e1000/` にする。`AGENTS.md:174-175` が FreeBSD のドライバーの移植を許す。`e1000_ich8lan.c:2-4` は BSD-3-Clause で Intel の著作である。`if_em.c:566-567` は `iflib` の上なので、書き直す。著作権表示を残す。
3. `matches` に機種の行を書く。e1000e の機種だけを選ぶ。`freebsd-src/sys/dev/e1000/e1000_hw.h` の `E1000_DEV_ID_` は 194 行で、ほかの系も混ざる。
4. `kernel/src/vm/e1000.c:284`、`:288` から `0x10D3` と `new_eerd` を消す（補い。E9 を見る）。
5. `attach` を書く。`pci_bar_address`、`vmm_map_mmio`、`pci_enable_busmaster`、リセット、NVM から MAC、PHY、`pci_irq_enable`、輪の確保、の順である（補い。形は `rtl8125.c:534-623`）。
6. 新しい輪の見出しの、開始、停止、送信の合図を実装する（`hal.md:105`）。
7. 割り込みで受信の輪を進め、サービスのスレッドを起こす（補い）。
8. `quiesce` と `resume` を書く（形は `rtl8125.c:276-284`、`:406-422`）。`stage` は今の `PM_STAGE_IO` に当たる値である。
9. `lost` を書く。受け取り手に輪の終わりを知らせる（補い）。
10. `hal_drivers.c` に一行足す（`hal.md:71`）。
11. IOMMU で絞る（`hal.md:121`）。今の口ではできない。E7 を見る。
12. 確かめる。QEMU の `-device e1000e` で 82574 の道を見る（補い。今の手順は `-device e1000`、`qemu-visual.md:18`）。実機の道は、手元で確かめる手段が書かれていない。

### 迷った所

**E1. 輪の形が無い。**
- 読んだ文：`hal.md:105`「新しい輪の見出し」「共有の輪をまとめて渡す」、`hal.md:117`。
- 迷い：見出しの名前、記述子の形、緩衝の持ち主、写すのか写さないのか、輪は adapter に一つか queue に一つか、満ちたときの待ち方、全部が無い。今の `net.h:35-39` の「満ちたら通知で再試行」の取り決めを残すのかも書いていない。

**E2. 網の手術の範囲が HAL の文書に無い。**
- 読んだ文：`hal.md:115`、`hal.md:121`、`docs/open.md:13`。
- 迷い：スタックの移動は `net.c`（2467 行）、`SYS_NET_*` と `SYS_TCP_*`（`abi.h:96-110`）、libc に及ぶ。`open.md:13` はソケットを object にすると書く。形はどちらの文書にも無い。e1000e を足すのに、ここまで全部を先にするのか。文書の順（`hal.md:232-236`）に網の位置が無い。

**E3. 「レジスターへ直接」と「送信の合図」が合わない。**
- 読んだ文：`hal.md:19`「パケット…は、HAL が作った輪とレジスターへ直接行く」、`hal.md:105`「送信の合図」、`hal.md:194`。
- 迷い：スタックのサービスは権限が無い。e1000e の送信の末尾のレジスターは、`TDBAL` と同じ頁にある（`vm/e1000.c:31-35`、同じ系の 8254x の番地）。その頁を写すと、サービスが DMA の番地を書ける。だから合図は syscall になる。`hal.md:19` は NIC には当たらない、と読んだ。文書はそう書いていない。

**E4. 表の三つの操作で足りない。**
- 読んだ文：`hal.md:105` の「開始、停止、送信の合図」。
- 迷い：MAC の読出し（`net.h:16`）、link の状態（`rtl8125.c:424-440`）、統計（`net.h:22`、`vm/hv/netvsc.c:513`）、受信の絞り、の置き場が無い。`hal.md:22` は「違いは実装の申告で受ける」と書くが、申告の項目が無い。

**E5. 持っている機能を使い切るのか。**
- 読んだ文：`architecture.md:45`「契約を最小公倍数にしない。ハードウェアが持つものは、有れば使い切り」、`hal.md:105`。
- 迷い：checksum の offload、TSO、複数の queue、割り込みの間引きを、輪の形が受けるのか決まらない。`struct n_net_adapter`（`abi.h:890-897`）にも項目が無い。`AGENTS.md:2` の「CPU の数が増えても成り立つ」には複数の queue が効くと考えるが、文書に無い。

**E6. 共有の輪の object を誰が作るのか。**
- 読んだ文：`hal.md:19`「HAL が作った輪」、`hal.md:105`。
- 迷い：DMA の object を作る口は PCI の handle を取る（`mm/shared.h` の `shared_dma_create(struct handle *pci, …)`）。カーネルの実装には handle が無い。カーネルが作ってサービスへ渡す口が要る。文書に無い。

**E7. IOMMU で絞る口が無い。**
- 読んだ文：`hal.md:121`「網の手術で絞る」。
- 迷い 1：`iommu_attach` は、群に `guest` を持たない `kernel_owned` の機能があると断る（`iommu.c:647`、`iommu.h:45-51`）。`pci_enable_busmaster` が `kernel_owned` を立てる（`pci.c:283`）。カーネルの NIC 自身が断られる。`guest` は app の claim に同居する機能の形で（`iommu.h:25-33`）、カーネルが自分の NIC を絞る口ではない。
- 迷い 2：群の仲間もカーネルの実装なら断られる。群を一つの domain にして中で共有するのか、決まらない。
- 確かめられない：I219 は PCH の多機能の装置の一つで、ACS が無く、仲間と同じ群になるはずである。これは記憶であり、手元の資料で確かめていない。

**E8. IOMMU が無い機械で動かしてよいのか。**
- 読んだ文：`hal.md:121`。
- 迷い：app の claim は既定で隔離を求める（`iommu.h:15-17`）。NIC は「絞れないなら動かさない」のか「そのまま動かす」のか、書いていない。

**E9. `0x10D3` の行が二つの実装に当たる。**
- 読んだ文：`hal.md:71`「表は一つだけ」、`hal.md:73`、`hal.md:81`。
- 迷い 1：`vm/e1000.c:284` は `0x10D3` を持つ。e1000e の実装も同じ行を持つ。`hal_drivers.c` と `vm_hal_drivers()` のどちらを先に見るか、書いていない。
- 迷い 2：`vm/e1000.c:338` は「the models this drives have no MSI」と書く。行を消すのが正しいと読んだ。`architecture.md:33` に従う判断であり、文書には無い。
- 確かめられない：QEMU の `e1000e` が `8086:10D3` であることは記憶である。今回は動かしていない。

**E10. adapter の身元と記録の番号が合わない。**
- 読んだ文：`hal.md:38`「起動の間、二度と使わない番号」。
- 迷い：今の adapter は同じ MAC が戻れば同じ番号である（`net.h:27-28`、`net.c:2128-2130`、`abi.h:888-889`）。経路の表はこの番号で指す（`abi.h:900-904`）。記録の番号とどちらを使うか、決まらない。

**E11. 実装がフレームを読む場面がある。**
- 読んだ文：`hal.md:115`「読むのは装置の記述子で、網から来た値ではない」。
- 迷い：`rtl8125.c:507-511` は送信のフレームの中を読み、詰め物を足す。輪を共有するなら、サービスが書き換えられる記憶を読むことになる。詰める場所も無い。e1000e に同じ類の回避が要るかは、FreeBSD の `e1000_ich8lan.c` を読まないと決まらない。契約が余白や写しを定めるのか、書いていない。

**E12. 上流のコードをどう入れるのか。**
- 読んだ文：`hal.md:20`、`AGENTS.md:175`「FreeBSD の口の代役（shim）を作らない」、`architecture.md:32`。
- 迷い：`e1000_*.c` の共有のコードは `e1000_osdep.h` を通して OS を呼ぶ（`e1000_osdep.h:39-46` は `sys/` の見出しを読む）。この層を Nocturne 向けに書くのが接着か shim か、私には決められない。

**E13. `lost` の呼び手。**
- 読んだ文：`hal.md:78`。
- 迷い：N5 と同じである。受信の輪の相手へ知らせる道は「今ある watch」（`hal.md:92`）と読めるが、輪の閉じ方は無い。

**E14. カーネルに残る読み手。**
- 読んだ文：`hal.md:115`。
- 迷い：`vm/hv/hv_util.c:181`、`:212` は `net_adapter_info` で住所を読む。`net.h:45-47` は「Any context」である。スタックが出ると、カーネルは住所を知らない。`net.c` の TCP の設定（`tcp_buffer_setting_put`）も同じである。文書に無い。
- 迷い：`hal.md:115` の「権限の無いサービス」は今の `network` か。`network` は `N_CLASS_SYSTEM` である（`services.c:87`）。

---

## 仕事 3：AMD の GPU の画面と Vulkan を足す

### 手順

前提：HAL の共通の部分と、GPU と画面の手術が済んでいる。手順の多くは補いである。

1. 特権の半分を書く。場所は `ports/amd/`、プログラムは `C:\Nocturne\System` の下とする（補い。形は `ports/nvidia/README.md`）。A1 のとおり、材料が無い。
2. 機能を claim する。`N_CAP_PCI_CLAIM`、`SYS_PCI_CLAIM`、`SYS_PCI_INFO`、`SYS_PCI_BAR`、`SYS_DMA_CREATE`、`SYS_PCI_INTERRUPTS`（`abi.h:133-135`）。
3. 画面を持つ。今の口は `n_pci_hold_screen`、`n_pci_screen_modes`、`n_pci_give_screen`、`n_pci_screen_flips`、`n_pci_screen_vblank`、`n_pci_screen_state`（`pciuser.h:42-69`、使い方は `ports/nvidia/display.c:10-31`）。`hal.md:127` で `SYS_SCREEN_*` に換わる。
4. `user/apps/services.c:61-98` の `declared[]` に行を足す。`function_matches` の代わりに宣言のデータを書く（`hal.md:165`）。名前は `gpu` と別にする（`services.c:102-105` は名前で一つを引く）。
5. `n_hal_publish` で `HAL_GPU` の記録を作る（`hal.md:133`）。
6. Mesa の RADV を取り込む。`third_party/mesa/src` に `amd` は無い。`D:\Programes\cloned repos\mesa\src\amd` から `ports/mesa/README.md:12` のコミットで取る（補い）。
7. 新しい winsys を書く。継ぎ目は `src/amd/vulkan/radv_radeon_winsys.h` の表である（関数の指は `grep` で 32 個）。形は `third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/`。
8. `ports/mesa/radv.mk` を書く。形は `ports/mesa/nvk.mk`。
9. 像の表を書く。形は `ports/nvidia/nvk_image.c:41-55`。`log_*` と `scanout_*` は外す（`hal.md:138`）。
10. `user/libc/vulkan.c:45-62` を、`n_hal_list(HAL_GPU, …)` と `n_hal_gpu` を読む形に換える。
11. `ports/mesa/nocturne_gl.c:127-133`、`:154-162`、`:471-626` と `user/libc/web/js_webgl.c:760`、`:773` を直す。
12. `common/services_abi.h:67`、`common/gpu_abi.h:61-64`、`docs/nocturne-native-abi.md:1352`、`tests/zink_start_hosttest.c:161`、`tests/drivertest.c:37` を直す。
13. firmware を媒体へ入れる。`scripts/layout.py:28` は NVIDIA の名を決め打ちしている。
14. 確かめる。QEMU に AMD の GPU は無い。A21 を見る。

### 迷った所

**A1. 特権の半分の材料が、許された範囲に見つからない。**
- 読んだ文：`hal.md:109`、`hal.md:229`「AMD と Intel を見ていない」、`AGENTS.md:7`、`AGENTS.md:11`「Linux のソースは参照しない」、`AGENTS.md:176`。
- 証拠 1：`D:\Programes\cloned repos` に、使ってよい AMD の GPU のカーネルの半分が見つからない。`freebsd-src/sys/dev` に `amdgpu` も `radeon` も無い。`sys/dev/drm2` と `tools/tools/drm/radeon` は、中身を読んでいない。`sys/compat/linuxkpi` は手本にしない（`AGENTS.md:176`）。
- 証拠 2：`D:\Programes\OS DEV INFO` に AMD の GPU の資料が無い（`find` で CPU の資料だけが当たる）。
- 証拠 3：AMD の GPUOpen の文書の頁は ISA と MES だけで、DCN、PSP、SMU、GFX の輪の初期化、GPU の頁表の文書が無い（出典 1）。頁は全般の検索へ案内するが、そこは調べていない。
- 迷い：NVIDIA の道は、作った者が OS 層つきで出した `open-gpu-kernel-modules` があったから成り立つ。AMD の同じ物は Linux の木の中にある。これを `AGENTS.md:7` の「作った者」と読むか、`AGENTS.md:11` の「Linux のソース」と読むかは、ユーザーにしか決められない。
- Haiku の `radeon_hd` は手元にある。`AGENTS.md:178` が「設計もコードも写さない」と書くので材料にしない。

**A2. 文書が決めない四つが、この仕事の中身である。**
- 読んだ文：`hal.md:140`「メモリー、同期、優先度、窓の合成との受け渡しは、GPU と画面の手術で決める。ここでは決めない」。
- 迷い：Vulkan と画面を端まで通すには四つとも要る。実装者が今決めると、後の手術で書き直しになる。`AGENTS.md:39` は今のコードを手本にしないと書くので、NVK の形（`nvkmd_nocturne_*`）を写してよいとも言えない。

**A3. カーネルの GPU の汎用の層があるのか。**
- 読んだ文：`architecture.md:19`「画面と GPU の汎用の層はカーネルの第一級の部品」、`docs/open.md:12`、`hal.md:131`「GPU の契約は二つだけである」、`hal.md:136`。
- 迷い：規則はカーネルに層を置く。文書の GPU の契約にカーネルの層は無い。GPU の時間を全プロセスで調停する者が決まらない。

**A4. 「プログラムから GPU へ直接」が AMD に当たらない。**
- 読んだ文：`hal.md:19`、`hal.md:109` のデータの道。
- 証拠：NVK は自分で輪を書き、doorbell を叩く（`nvkmd_nocturne_rm.c:1354-1359`）。RADV は送信のたびに `ac_drm_cs_submit_raw2` を呼ぶ（`mesa/src/amd/vulkan/winsys/amdgpu/radv_amdgpu_cs.c:1876`）。`winsys/amdgpu` に user queue の語は無い（`ac_linux_drm.c` と `ac_gpu_info.c` にはある）。
- 迷い：今の RADV のままなら、送信のたびに特権の半分へプロセスを越える。`hal.md:195-197` の費用に、これは入っていない。`hal.md:136` によれば実装の中の話だが、表の欄とは合わない。user queue へ行くのか、文書では決まらない。

**A5. `n_hal_publish` の形が無い。**
- 読んだ文：`hal.md:133`。
- 迷い 1：どの handle を権限にするのか。claim の handle か、新しい能力か。
- 迷い 2：親はどの記録か。
- 迷い 3：いつ呼ぶのか。今のローダーはサービスの状態で像を選ぶので、起動の途中でも像を読み、`ready` が理由を返す（`vulkan.c:54-59`、`nocturne_gl.c:155-156`）。`publish` の前は記録が無いので、理由が「無い」に変わる。
- 迷い 4：プロセスが落ちたら記録はどうなるのか。`hal.md:212` の「状態の書き手は `hal.c` の一つ」とどうつながるのか。
- 迷い 5：像の名はどう確かめるのか。権限の低いプロセス（`services.c:65` は `N_CLASS_USER`）が書いた文字列を、全プログラムが `N_DRIVER_FOLDER` の下で開く（`vulkan.c:57`）。

**A6. 「できること」の中身が無い。**
- 読んだ文：`hal.md:133`「名前、メモリーの量、できること、像の名」、`hal.md:91`。
- 迷い：型が無い。読み手も、ローダー以外は名指しされていない。ローダーが要るのは像の名だけである。

**A7. 二つの GPU があるときの選び方が無い。**
- 読んだ文：`hal.md:134`「記録の一覧から像を選ぶ」。
- 証拠：`vulkan.c:19` の `driver` は一つで、`vulkan.c:2-4` は「one per process」「nothing in between」と書く。Ryzen 5 7600X と RTX 4070 の機械（`AGENTS.md:187`）は、AMD の表示の機能と NVIDIA の機能の両方を持ちうる。`nvidia.md:12` は iGPU（AMD）に触れている。
- 迷い：一つを選ぶのか。選ぶ基準は何か。両方の像を読んで `vkEnumeratePhysicalDevices` をまとめるのか。まとめるならローダーの間に層が要る。`abi.h` の `n_thread_block` は複数の写しを許すので、できる形ではある。Zink はどちらを使うのか。

**A8. 特権の半分の置き場。**
- 読んだ文：`hal.md:109`「特権の半分の置き場は実装が決める」、`hal.md:167`「GPU の PCI の機能はカーネルの実装が持たない」、`hal.md:20`。
- 迷い：二つの文が合わない。AMD をカーネルに置くと `hal.md:167` が崩れる。プロセスに置くなら「実装が決める」は要らない。私はプロセスと読んだ。

**A9. プロセスが持っている機能を、記録で言えない。**
- 読んだ文：`hal.md:35`、`hal.md:43`、`hal.md:165`。
- 迷い 1：プロセスの実装が動いても、`driver` は NULL で `state` は `HAL_UNBOUND` のままに読める。`HAL_RUNNING` にするなら、書き手と条件が無い。
- 迷い 2：サービスの管理者が `HAL_UNBOUND` を照らし直すと、動いている実装を二度起こす形になる。今は起動に一度だけ読む（`services.c:831`）。
- 迷い 3：`n_hal_list(kind, …)` は種類で引く。GPU の PCI の機能の種類は何か（`HAL_GPU` は `publish` で作る子である）。
- 迷い 4：宣言の実例の引数は PCI の住所である（`services.c:275`）。`struct hal_identity` に住所が無い。`SYS_PCI_CLAIM` は住所を取る（`pciuser.h:12`）。

**A10. 画面の記録を誰が作るのか。「引き継ぐ」とは何か。**
- 読んだ文：`hal.md:167`「画面の記録を `nvrm` が引き継ぐだけである」、`hal.md:133`。
- 迷い：`HAL_GPU` の作り方はあるが、`HAL_SCREEN` の作り方が無い。今は `give_screen` の中でカーネルが作る（`pciuser.c:358-370`）。引き継ぐとは親を換えることか、`driver` を換えることか。実装が落ちたとき記録は `HAL_LOST` か。`fb.h:59-60` は「A screen never goes away」と書く。同じ画面は次の起動の間も同じ番号である（`pciuser.c:360-362`）。

**A11. 起動の画面がどの機能の上にあるか。**
- 読んだ文：`hal.md:167`「firmware の面は PCI の機能を持たない」。
- 証拠：今のコードは、起動の画面がある BAR を機能ごとに調べ（`pciuser.c:166-176`）、`output == 0` を画面 0 に結ぶ（`pciuser.c:359`）。reset も止める（`pciuser.c:679`、`:757`）。
- 迷い：AMD でも output 0 が起動の画面を出す head なのか、文書では分からない。

**A12. `SYS_SCREEN_*` の形。**
- 読んだ文：`hal.md:127`。
- 迷い：権限の handle が無い。今の claim の handle には、DMA、BAR、claim の終わりで画面を返す処理（`pciuser.c:547` の `outputs_end`）がつながる。同じ接頭辞の `SYS_SCREEN_INFO`、`SYS_SCREEN_MODE_APPLY`、`SYS_SCREEN_ARRANGE`（`abi.h:72`、`:191-192`）は別の権限である。

**A13. `scanout_*` を外した後の直接表示。**
- 読んだ文：`hal.md:138`。
- 証拠：WebGL の全画面の直接表示がこれを使う（`nocturne_gl.c:471-626`、`js_webgl.c:760`、`:773`）。Vulkan へ渡す印は `VkImportMemoryNocturneScanoutInfoMESA`（`vk_internal_exts.h:133`）で、受けるのは NVK だけである（`nvk_device_memory.c:151`）。Zink も型を持つ（`zink_resource.c:1018`、`:1142`）。
- 迷い：代わりの道が無い。`N_SCANOUT_DIRECT`（`abi.h:526`）は残るのか。AMD は最初から持たなくてよいのか。NVIDIA の道は消えて退行にならないのか。
- 迷い：`log_*` も読み手がある（`nocturne_gl.c:128`、`:159-162`）。外す理由が文書に無い。像は自分の libc を持つので、表の外に道が無い（`user/image.map`）。

**A14. プロセスの実装の眠り。**
- 読んだ文：`hal.md:85`「全部の実装が `attach`、`lost`、`quiesce`、`resume` を持つ」。
- 証拠：`pm.c:351-354` は、claim された機能があると眠りを断る。`fb.c:633-635` も同じことを言う。
- 迷い：プロセスの実装は「全部の実装」に入らないのか。入らないなら、AMD の機械は眠れないままである。

**A15. firmware の置き場。**
- 読んだ文：文書に無い。
- 証拠：`scripts/layout.py:28` は `gsp_ga10x.bin` などを決め打ちしている。`local/firmware/` には `nvidia/` だけがある。
- 迷い：AMD の firmware の取り方と配布の条件は、確かめていない。`hal.md:165` の導入の形式が来るまで、どこへ置くのか決まらない。

**A16. 宣言の行の形。**
- 読んだ文：`hal.md:57`、`hal.md:61`、`hal.md:81`。
- 迷い 1：「機種の行」と「種類の行」の区別の印が無い。`vendor == 0` が無視なのか、書いていない。
- 迷い 2：今の NVIDIA の条件は「製造者 + 種類」である（`nvrm.h:33-35`）。どちらの行か。
- 迷い 3：AMD の表示の機能の `subcls` に何が要るかは確かめていない。

**A17. IOMMU の群。**
- 読んだ文：`hal.md:167`「一つの装置を二つの実装が争う場面は無い」。
- 証拠：GPU の音の機能はカーネルの `hda` が持ち、claim の間は guest になる（`hda.c:21-24`、`:914-915`、`iommu.h:25-38`）。記録の寿命に guest の止まりと再開が無い。
- 確かめられない：内蔵の GPU の機能が xHCI と同じ多機能の装置に並ぶ機種では、`iommu.c:647` で claim が断られるはずである。これは記憶で、Raphael の配置を手元で確かめていない。

**A18. RADV の接着の範囲。**
- 読んだ文：`hal.md:136`、`architecture.md:32`。
- 証拠：RADV の同期は `vk_drm_syncobj` である（`radv_amdgpu_winsys.c:281`、`:360`）。装置の情報は DRM の値から作る（`ac_gpu_info.c` に `ac_drm_` などが 34 行、`ac_linux_drm.c` に 79 行、`ac_linux_drm.c:33` は `int fd` を取る）。
- 迷い：Linux の形を写さずに、どの層で切るのか（`AGENTS.md:127`）。`ac_drm_*` の代役なら shim である。`radeon_winsys` の表で切っても、`ac_query_gpu_info` は残る。文書は実装の中の話とする。妥当だが、費用の見込みが `hal.md:229` の「書き直しが広がらない」と合うかは分からない。

**A19. 「プログラムの Vulkan から画面へ」の道が今無い。**
- 読んだ文：`hal.md:109`、`hal.md:140`。
- 証拠：`third_party/mesa/src/vulkan/wsi/` に Nocturne の surface が無い。`wsi_common.c:49-51` は印の外しだけである。`user/apps` に `VK_KHR_swapchain` の使用が無い。
- 迷い：依頼の「画面への表示」を、デスクトップの表示と読むか、Vulkan のプログラムの窓と読むか。後者なら WSI も作る。文書はどちらも言わない。

**A20. 落ちたときの reset。**
- 読んだ文：`hal.md:171-174`。
- 証拠：カーネルは起動の画面の機能を reset しない（`pciuser.c:757-758`、`:768-769`）。
- 迷い：AMD の reset の手順は確かめていない。`hal.md:174` は NVIDIA についてだけ書く。

**A21. 確かめる場所が無い。**
- 読んだ文：`hal.md:213-214`。
- 迷い：QEMU に AMD の GPU は無い。文書の「実機だけの範囲」に AMD が無い。ユーザーの機械で試すには内蔵の GPU へ画面を出すことになるが、`nvidia.md:12` はこの環境の変更を勧めないと書く。
- 迷い：行を足すだけで、ユーザーの機械の起動時に AMD の実装が走る（`services.c:65` の `at_boot` の形）。`AGENTS.md:47` の「任意の機能は既定で起こさず」に従うのか、決まらない。

**A22. 実装を足す手間の見積り。**
- 読んだ文：`hal.md:71`「実装を足すのは、`struct hal_driver` を一つ書いて、この表に一行足すことである」。
- 迷い：プロセスの実装には当たらない。宣言の行、`ports/` の組みの規則、像、firmware、ローダーが要る。文書はカーネルの実装のことだけを書いていると読んだ。

---

## 文書と今のコードの食い違い

| # | 文書 | コード | 食い違い |
|---|---|---|---|
| C1 | `hal.md:167`「firmware の面は PCI の機能を持たない」 | `pciuser.c:166-176`、`:359`、`:679`、`:757`、`:873`、`:1176`、`vm/vbe.c:119`、`fb.h:38-45`、`pci.h:204-205` | 起動の画面は機能の BAR の中にあり、コードはそれを調べて扱いを変える。 |
| C2 | `hal.md:167`「GPU の PCI の機能はカーネルの実装が持たない」 | `vm/virtio_gpu.c:578`、`vm/vbe.c:186-191` | VM ではカーネルの実装が持つ。`hal.md:133` の virgl とも合わない。 |
| C3 | `hal.md:86`「`struct pm_device` は `struct hal_driver` に入り、消える」 | `pm.h:57-67`、`xhci.c:2311-2317`、`fb.c:636`、`pm.c:250`、`pm.c:357` | `refuse`、`cold_ok`、`ctx`、`down` の行き先が無い。`hal.md:59-68` に項目が無い。 |
| C4 | `hal.md:85`「要らないものは `NULL` にする」 | `pm.h:61`、`pm.c:250`、`fb.c:630-636`、`vm/tulip.c:115`、`vm/virtio_gpu.c:590` | 今の `resume == NULL` は「戻れないので眠らない」である。文書どおりなら、実機の firmware の面で S3 が出る。 |
| C5 | `hal.md:86` 同上 | `dev/input.c:184`、`fb.c:636` | 装置でないものも登録している。`input_pm` は修飾キーを戻すだけで、記録が無い。 |
| C6 | `hal.md:161`「PCI、xHCI、VMBus が見つけた物に `hal_add`」 | `vm/ata.c:110-113`、`boot.c:173`、`vm/rdp.c:817`、`arch/topology.c` | ports で探す ATA、Limine の面、RDP の入力、CPU は、三つのバスのどれでもない。 |
| C7 | `hal.md:57`「`uint16_t vendor, device`」 | `vm/hv/vmbus.h:7`、`:53`、`:88`、`vm/hv/storvsc.c:14`、`vm/hv/netvsc.c:19`、`vm/hv/hv_input.c:10-12` | VMBus の身元は GUID で、入らない。 |
| C8 | `hal.md:104`、`:190`「要求ごとに一回の間接呼出し」 | `blk.c:85`、`blk.c:113`、`nvme.c:885`、`store.c:320` | ドライバーの中でもう一回ある（`ahci.c:1010`、`usbmsc.c:227` も同じ）。上の `part_read` も表である。 |
| C9 | `hal.md:87`「`struct blkdev` の `gone` がこの形である」 | `blk.c:83-88`、`ahci.c:966`、`:970`、`:1165-1169`、`nvme.c:431`、`:712`、`blk.h:24` | `blk.c` は `gone` を読まない。AHCI は速い道で読む。AHCI の `gone` は戻る（`:1168`）。`HAL_LOST` の一方向と合わない。 |
| C10 | `hal.md:193`「装置の結びつけ：今は起動に一度」 | `usb.h:5-6`、`blk.c:7`、`net.h:26` | USB は今も抜き差しのたびに結ぶ。 |
| C11 | `hal.md:117`「`send` と `poll_rx` の受け手が 1 フレームごとの間接呼出し」 | `usbnet.c:125`、`:138`、`net.c:362`、`usb.h:7-8` | `poll_rx` を持たない実装は `net_rx` で写す。`usbnet` は割り込みか alarm の文脈で写す。 |
| C12 | `hal.md:115`「読むのは装置の記述子」 | `rtl8125.c:507-511`、`usbnet.c:110-125`、`vm/hv/netvsc.c:2-3` | 送信のフレームを読む実装がある。NCM と RNDIS は枠の形式を実装が解く。 |
| C13 | `hal.md:162`「`ahci_init`、`nvme_init` などの列…無くなる」 | `net.c:2178`、`audio.c:275`、`xhci.c:2320-2336`、`init.c:40-42` | 列の関数は装置の探索と、部品の初期化が混ざる。`usb_init` は機能の総数を先に数える（`unsettled`）。`store.c:310` がそれを読む。 |
| C14 | `hal.md:163` の三つ | `vm/vm.h:32`、`:36-38`、`init.c:38`、`:40`、`:42` | `vm_init`、`vm_display_init`、`vm_display_pm`、`gpu_init`（実体は `vm/`）の扱いが書かれていない。`vm_session_init` もある。 |
| C15 | `hal.md:121`「カーネルの NIC の実装は今 IOMMU で絞られていない」 | `iommu.h:9-10`、`nvme.c:10-12`、`hda.c:21` | 正しいが NIC に限らない。カーネルの機能は全部素通りである。`hda` だけが guest の時に `iommu_` を呼ぶ。 |
| C16 | `hal.md:121`「網の手術で絞る」 | `iommu.c:647`、`pci.c:283`、`iommu.h:45-51` | 今の `iommu_attach` は、`guest` を持たない `kernel_owned` の機能を断る。 |
| C17 | `hal.md:138` | `nocturne_gl.c:128`、`:159-162`、`:476-626`、`js_webgl.c:760`、`:773` | 外す項目に読み手がある。代わりが無い。 |
| C18 | `hal.md:93`、`:181`「版の取り決めは作らない」 | `gpu_abi.h:66`、`:69`、`vulkan.c:34` | `N_GPU_DRIVER_ABI` があり、`hal.md:138` はこれの扱いを言わない。 |
| C19 | `hal.md:109`「プログラムから GPU へ直接」 | `abi.h:60-63`、`user/libc/gpu.c`、`virtio_gpu.c` | virgl はカーネルを通る。`hal.md:136` 自身もそう書く。 |
| C20 | `hal.md:109` の「`struct n_gpu_driver`」の実装に virgl | `ports/nvidia/nvk_image.c:41`、`gpu_abi.h:16-18` | 像を持つのは `nvk` だけである。virgl の像は無い。 |
| C21 | `hal.md:133`、`:215` | `virtio_gpu.c:579-581`、`boot.c:76`、`docs/open.md:80` | virgl は製品のカーネルで起きない。契約の二つ目の実装は今、製品に無い。 |
| C22 | `hal.md:35`、`:78` の `HAL_LOST` | `fb.h:59-60`、`blk.h:24`、`blk.c:7` | 画面と block device は消えない取り決めである。 |
| C23 | `hal.md:38` の `id` | `net.h:27-28`、`net.c:2128-2130`、`abi.h:888-889`、`pciuser.c:360-362` | adapter と画面は、抜けても戻っても同じ番号を持つ。 |
| C24 | `hal.md:52`「名前…を持たない」 | `abi.h:110` | `SYS_NET_ADAPTER_NAME` はカーネルがドライバーの名を返す。 |
| C25 | `hal.md:153`「選ぶのは `cpu_features_init` で一度だけ」 | `boot.c:150`、`boot.c:176`、`vm.c:32-42` | VM の判定は後に走り、ACPI の OEM も読む。一度で選ぶには順を変える。 |
| C26 | `hal.md:151`「共通コードの `f->hypervisor` を無くす」 | `arch/cpu.c:78`、`:143`、`:183`、`vm.c:32` | `vm_cpu` を選ぶ判定に、この bit が要る。読む場所が残る。 |
| C27 | `hal.md:43`、`:164` | `pci.h:28`、`:30`、`pci.c:283`、`hda.c:908`、`pciuser.c:831` | 持ち主の印が既にある。文書は扱いを言わない。 |
| C28 | `hal.md:85` | `pm.c:351-354`、`fb.c:633-635` | プロセスの実装は眠りの輪の外にあり、機能があると眠りを断る。 |
| C29 | `hal.md:71` | `vm/e1000.c:80-88`、`vm/tulip.c:118-119`、`vm/ac97.c:281`、`vm/virtio_gpu.c:578` | 実装には静的な一台の形がある。`attach(d)` の形へは書き直しが要る。「一行」は新しい実装の話である。 |
| C30 | `hal.md:57`（住所が無い） | `services.c:275`、`pciuser.h:12`、`ports/nvidia/libnvrm.c:118-120` | 宣言の引数と claim は PCI の住所で動く。 |

正しいと確かめた引用：`hal.md:172` の `pciuser.c:296`、`hal.md:215` の `test_flag("gpu-enable")`（`virtio_gpu.c:580`）、`hal.md:50` の `usb.h:103-106`、`hal.md:121` の `rtl8125.c`（`iommu_` は 0 件）、`hal.md:165` の `services.c:52`、`hal.md:171` の `services.c:50`。

## 文書と規則の食い違い、文書の中の食い違い

| # | 文 | 相手 | 食い違い |
|---|---|---|---|
| R1 | `hal.md:52`「記録は、名前…を持たない」 | `hal.md:45` の `char name[32]`、`hal.md:133`「記録が名前…を申告する」 | 文書の中で合わない。`docs/open.md:24` は未決としている。 |
| R2 | `hal.md:19` | `hal.md:104`、`:105` | ブロックとパケットは「直接」ではなく、間接呼出しと合図を通る。 |
| R3 | `hal.md:71`「一つだけ」 | `hal.md:73` の `vm_hal_drivers()` | 表は二つある。順が無い。 |
| R4 | `hal.md:81`「実装の引き方は一つ」 | `hal.md:165`、`architecture.md:27` | サービスの管理者の照合が二つ目である。 |
| R5 | `hal.md:93`「全部を一緒に組むからである」 | `hal.md:165`、`docs/open.md:17` | ドライバーの包みが来ると、この理由が消える。 |
| R6 | `hal.md:109`「置き場は実装が決める」 | `hal.md:167` | 片方はカーネルに置けると読め、片方は置かないと読める。 |
| R7 | `hal.md:21` | `hal.md:109` | `struct n_gpu_driver` の実装は今一つである。像の境目に表が要るのは分かるが、規則の例外として書かれていない。 |
| R8 | `hal.md:184`「読む者の無い種類」を作らない | `hal.md:34` の `HAL_CPU`、`hal.md:91` の `n_hal_storage` | U1、U2 を見る。 |
| R9 | `hal.md:131` | `architecture.md:19`、`docs/open.md:12` | A3 を見る。 |
| R10 | `hal.md:111`「温度…今は作らない」、表に電源が無い | `architecture.md:40`「電源と温度」 | 規則は契約を一つ持てと書く。電源の契約は文書に無い。 |
| R11 | `hal.md:222`「手術の間に main が組めないと…作業木で行う」 | `AGENTS.md:110`、`:113` | 規則は「main を守るためには使わない」と書く。 |
| R12 | `hal.md:174`、`:214`「USB のログと写真」 | `nvidia.md:12`「写真だけ」、`AGENTS.md:144` | `AGENTS.md` とは合い、`nvidia.md` とは合わない。 |
| R13 | `hal.md:133` の `n_hal_publish` | `architecture.md:18` | U3 を見る。 |
| R14 | `hal.md:71` の表 | `architecture.md:25`「製品の名前はドライバーの中だけ」 | 表は実装の名を並べる。例外として書かれていない。 |

---

## 実装できない、または正しく実装するのが難しい所

重い順である。

**H1. AMD の特権の半分（A1）。** 許された材料が見つからない。これが解けないと、仕事 3 は `hal.md` の形を問う前に止まる。理由は作った者の公式の文書が手元に無く、Linux の木を読めないからである。

**H2. GPU の契約が端まで通らない（A2、A3、A7、A13、A19）。** 契約は記録と像の表だけである。Vulkan を画面まで通すのに要る、メモリー、同期、優先度、合成との受け渡し、WSI、複数の GPU が外にある。`hal.md:229` は「記録と像の表の外は実装の中の話なので、書き直しが広がらない」と書く。しかし窓の合成との受け渡しは、実装の中の話ではない。全部の GPU と窓のサービスの間の取り決めである。

**H3. NIC の共有の輪（E1、E3、E6、E7）。** 理由は四つある。
- サービスはレジスターに触れない。合図は syscall になる。
- カーネルの実装が作る共有の DMA の object の口が無い。
- IOMMU の今の口はカーネルの機能を断る。
- 輪の形が無いので、`usbnet`（`usb_send`、`usb.h:150-155`）と `netvsc`（VMBus の緩衝）が同じ形に入るかを確かめられない。`hal.md:115` の「置き場が揃う」は置き場の話で、輪の話ではない。

**H4. 木を辿る眠りと、`hal_lost` の競合。**
- 今の `stages[]` は伸びるだけなので、錠なしで辿れる（`pm.c:37-38`）。
- `quiesce` は割り込みを閉じて走り、眠れない（`pm.h:28-29`）。
- 電源を切るときは、ほかの CPU が核のスレッドを走らせている（`pm.h:30-32`）。
- 眠りのときは、ほかの CPU が止まり、ほかのスレッドは走らない（`pm.h:30`、`pm.c:284-285`）。
- 記録は参照が無くなるまで残る（`hal.md:78`）。`lost` の仕事が眠るなら、途中まで進んだ記録を `quiesce` が見る。眠りの前に途中の仕事を待つのかは、文書に無い。
- `hal.md:212` は「難しい」と書くが、手段は「書き手を一つにする」だけである。錠の種類、順、辿り方が要る。`.claude/rules/kernel.md:17` の錠の順にも行が要る。

**H5. 一つの通しの列挙と、起動の順（N4、C13）。** `pci_init` は `upstream` と `node` を全機能の後に決める（`pci.c:252-268`）。`iommu_init` はその後で、全機能の群を読む。`net_init`、`audio_init` は `attach` より前に要る。`usb_enumerating` は総数に頼る。列が無くなるなら、「列挙の完了」と「結びつけの開始」の二段と、部品の初期化の場所が要る。文書に無い。

**H6. 記録の口の形（N1、N2、C6、C7、C30、A9）。** `hal_add(parent, &identity)` は、種類を渡せず、必ず実装を引く。`hal.md:133` の「virgl は `hal_add` で作る」も、この形では作れない。`struct hal_identity` は GUID と住所を持てない。機械的に直せるが、直すと「三つの操作」と 300 行の見込みが変わる。

**H7. 眠りの三つの場合（C3、C4）。** 「戻れる」「要らない」「戻れない」を `NULL` で言い分けられない。`usbhid`、`usbnet`、`usbmsc` は今 `pm_device` を持たず、`xhci` がまとめて持つ。これらが `resume == NULL` の `hal_driver` になる。今の意味なら眠りが止まり、文書の意味なら `display` の眠りが誤って出る。`refuse` も要る（`xhci.c:2311-2314`）。

**H8. 非同期の `lost` と USB の外し方。** `usb.h:10-11` は「its class driver's detach waits for its users」と書く。`usb_remove`（`xhci.c:1713`）はその後に装置を消す。`hal.md:78` の「仕事を積む」は非同期である。xHCI の worker が待つのか、`struct usb_device` の寿命を記録の参照へ移すのか、決まらない。

**H9. 戻る `gone`（C9）。** AHCI は同じ disk が戻ると同じ `blkdev` を使い続ける（`ahci.c:166-167`、`:1167-1169`）。`store` はこの指を持ち続ける（`store.c:301-302`）。記録を子にするなら、戻ったときに新しい番号の記録を古い `blkdev` に結ぶことになる。

**H10. `scanout_*` と `log_*` を外すこと（A13、C17）。** 代わりの道が決まるまで、外すと全画面の直接表示の退行になる。`AGENTS.md:143` は回帰を失敗とする。

**H11. 300 行の見込み（`hal.md:27`）。** 根拠が書かれていない。ここに入るものは、木、参照の数、二つの表の照合、`lost` の仕事の列、眠りの辿り（`pm.c:187-198`、`:248-253`、`:327-334`、`:355-362` の置き換え）、`n_hal_*` の syscall、`publish`、watch である。超えるかは書いて数えないと分からない。確かめていない。

---

## 文書が要らないものを作らせている所

**U1. `HAL_CPU` の記録（`hal.md:34`）。** CPU の台帳は `arch/topology.c` にある。CPU の申告は別のデータに置く（`hal.md:94`）。`struct cpu_maker` は `struct hal_driver` でない（`hal.md:100`）。`attach` も `lost` も無い。読む者が名指しされていない。`hal.md:184` に当たる。1024 個の記録が要るだけである。

**U2. 種類ごとの読む口のうち、読み手の無いもの（`hal.md:91`）。**
- `n_hal_storage` に読み手が無い。`blk_*` を読むのは `pm.c` と `store.c` だけで、`sys/syscall.c` は読まない。
- `HAL_NETWORK`、`HAL_AUDIO`、`HAL_INPUT`、`HAL_SCREEN` には今の一覧がある（`abi.h:96` の `SYS_NET_ADAPTERS`、`audio.c:275` の一覧、`input.h:33-41`、`display_abi.h:73` の `screens`）。文書は古い口を消すと言わない。残せば台帳が二つになる（`architecture.md:26`）。`SYS_PCILIST`（`abi.h:59`）も同じである。
- 読み手が今要るのは GPU の像の名と、サービスの管理者の照合だけと読める。

**U3. カーネルが GPU の申告を持つこと（`hal.md:133`）。** 名前、メモリーの量、できること、像の名を、カーネルは読まない。読むのはプログラムのローダーである。`architecture.md:18` は、権限、割り込み、全プロセスの調停のどれかが要るものだけをカーネルに入れると書く。今の `client_image` はサービスの一覧で足りている（`services_abi.h:67`）。`client_image` を無くす理由は製品の名前と読める（`docs/open.md:60`）が、それなら宣言の外へ出せば足りる。`n_hal_publish` の syscall、権限、寿命が丸ごと増える。

**U4. `SYS_SCREEN_*` への分離（`hal.md:127`）。** プロセスの画面の実装は全部 PCI の機能である（`hal.md:20`）。画面の処理は claim の機能を読む（`pciuser.c:359`、`:873`）。claim の終わりに画面を返す（`pciuser.c:547`）。`docs/open.md:42` の K4 が理由なら、ファイルを分ければ足りる。新しい syscall の族と handle が要る理由が書かれていない。これは判断であり、確定ではない。

**U5. 木と `stage` の両方（`hal.md:86`）。** 今は `stage` だけで順が決まり、動いている（`pm.c:189`）。木を足す場面が書かれていない。USB の子は `pm_device` を持たない。

**U6. PCI の実装の `lost`（`hal.md:85`）。** PCI に抜き差しが無い。`NULL` でよいなら問題は無い。ただし「全部の実装が持つ」と書くと、実装者は書く。N5 の自己呼出しに使うなら、理由を書く必要がある。

**U7. `vm/vm_cpu.c` の表（`hal.md:151`）。** `f->hypervisor` を読むのは `cpu.c:143`、`cpu.c:183`、`vm.c:32` の三か所である。`AGENTS.md:126` の要求は正しい。ただし三か所のために `struct cpu_maker` の第三の実装を作るのが要るかは、表の項目が決まるまで判断できない。

**U8. `hal_each`（`hal.md:79`）。** 名指しされた読み手が無い。種類ごとの一覧（`blk_get`、`screen_get`、`pci_at`）が残るなら、使うのは眠りの辿りと syscall だけである。眠りは種類で辿らない。

---

## 確かめられなかったこと

- 動かしていない。QEMU の `e1000e` の身元、NVMe の道の結果は見ていない。
- I219 の PCI の配置と群は記憶である。`OS DEV INFO` をファイル名（e1000、82574、i219、82579、gbe、ethernet）で探して、当たらなかった。Intel の公式の文書の取得は試していない。
- Raphael の内蔵の GPU の PCI の配置は記憶である。
- AMD の GPUOpen の頁以外の公式の文書は調べていない。Web 検索の結果の Linux の資料は開いていない。
- `freebsd-src/sys/dev/drm2`、`freebsd-src/tools/tools/drm/radeon`、Haiku の `radeon`、`radeon_hd` の中身は読んでいない。
- `D:\Programes\cloned repos\mesa` のコミットが `ports/mesa/README.md:12` と同じかは確かめていない。RADV の行番号は手元のクローンのものである。
- `hal.md:199-205` の三つの測定はしていない。プロセスを越える費用は分からない。
- AMD の firmware の配布の条件、reset の手順、`subcls` の値は確かめていない。

## 出典

1. [AMD GPU architecture programming documentation（GPUOpen）](https://gpuopen.com/amd-gpu-architecture-programming-documentation/)：開いて読んだ。文書は ISA と MES だけであった。
2. 下の Web 検索の結果は題だけを見た。Linux の資料なので開いていない。AMD の公式の一覧には着かなかった。
   - [git.zx2c4.com の diff](https://git.zx2c4.com/linux-dev/diff/?id=d59f1774bef998deaf3ff3fe1d2d3b069fea5aa2)
   - [git.zx2c4.com の commit](https://git.zx2c4.com/linux-dev/commit/?id=d59f1774bef998deaf3ff3fe1d2d3b069fea5aa2)
   - [tangled.org の写し](https://tangled.org/tjh.dev/kernel/commit/d59f1774bef998deaf3ff3fe1d2d3b069fea5aa2)
   - [code.opensuse.org の patch](https://code.opensuse.org/kernel/kernel-source/blob/0ead8eda8f50859d6d4eccf67f7ee141679568e8/f/patches.suse/0889-Documentation-gpu-include-description-of-AMDGPU-hard.patch)
   - [patchwork.kernel.org の patch](https://patchwork.kernel.org/project/dri-devel/patch/20241025132336.2416913-1-alexander.deucher@amd.com)
   - [gitlab.freedesktop.org の commit](https://gitlab.freedesktop.org/isinyaaa/linux/-/commit/a723c6d0785acf9e4bf5b77167d6a0c32ecf8503)
   - [amd-gfx の pull の案内](https://lists.freedesktop.org/archives/amd-gfx/2022-September/084006.html)
