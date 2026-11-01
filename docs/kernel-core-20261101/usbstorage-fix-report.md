# USB と記憶の driver の修理の報告

## 依頼文

Nocturne OS（独自の OS。カーネルは C。対象は実機。下限は Zen 4 の時代の今の PC。特定の機械に決め打ちしない）のカーネルの USB と記憶の driver を直してください。返事と文書とコミットメッセージは日本語で書いてください。

ユーザーの言葉：
- "just make the kernel good"
- "any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."

材料：
- 監査の報告 `docs/kernel-core-20261101/audit-spec-usb-storage.md`（48 件）。別の者が書いたものです。報告の主張を真実と見なさず、一件ごとに今のコードと公式の文書を照らして決めてください。報告が間違っているなら直さず、その理由を書いてください。報告の全部を扱い、一部で止めないでください。
- 公式の文書は `D:\Programes\OS DEV INFO`（xHCI、USB、AHCI、NVMe など）。記憶でなくここで照らしてください。節番号を自分で確かめてください。
- 規則は `AGENTS.md`、`.claude/rules/kernel.md`、`.claude/rules/map.md`。ビルドと QEMU の手順は `.claude/skills/nocturne-project/SKILL.md`。

条件：
- 症状を隠す hot fix、shim、stub、決め打ちの値を入れないでください。悪い作りは消してから建ててください。
- 待ち時間は、文書が定める値か、装置が申告する値を使ってください。
- どの OS のソースコードも参照しないでください。GPLv2 だけのコードを足さないでください。POSIX・Linux・UNIX の API を足さないでください。
- 作業木の中で働き、ビルドの出力は `build/wt/<作業木の名>/` か、その作業木の規則に従った場所へ置いてください。push しないでください。main へ merge しないでください。作業木の枝へコミットしてください。コミットの末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` を付けてください。
- カーネルは警告なしで組んでください。QEMU は窓付き（`-display none`・`-nographic` は禁止）で、ほかの書き手と合わせて 2 台までです。フックが止めたら、枠を取り合わず、読む仕事を進め、枠が空いてから確かめてください。Hyper-V と実機には触れないでください。

確かめ：直した後に、窓付き QEMU の NVMe と AHCI の両方の起動、xHCI のキーボードと tablet、USB の記憶装置（`usb-storage`）を確かめてください。実機でしか確かめられないものは、確かめていないと書いてください。

報告は全文を `docs/kernel-core-20261101/usbstorage-fix-report.md` に書いてください（この依頼文も先頭に）。指摘ごとに、直した／直さなかった理由、文書の節、差分の場所を一行で書いてください。確かめた範囲と確かめていない範囲を分けて書いてください。最後のメッセージには、枝の名、コミット、残ったものを短く書いてください。

## 結論

- 枝は `worktree-agent-a85eb97cac31b3d64`。分岐点は main の 55142942。最後のコミットは cc4f3729。push も merge もしていない。
- 48 件の全部を扱った。A11 は NCQ の誤りだけ COMRESET を残した（SATA・ACS の文書が無い）。A14 は報告の案の一部が文書と違うので、文書に合わせた。N6 も同じである。X13 はコメントだけを直した。X15、M3、U2 は今の動きで違反でも壊れもしないが、案の形に揃えた。
- 差分は 11 回、別の監査役に照らさせた。最後の回の指摘は 1 件（きれいさ）で、直した。その直し（`usbhub_work` の末尾の 6 行）だけは監査に回していない。
- 確かめは QEMU の範囲だけである。実機と Hyper-V は確かめていない。

## コミット

| コミット | 内容 |
|---|---|
| 545a102f | 48 件の修理 |
| 34ab1dbe | 一回目の監査の指摘 |
| 883c546d | 二回目と三回目の監査の指摘 |
| 26b39b07 | 四回目の監査の指摘 |
| 682f34dd | 五回目の監査の指摘 |
| fe71d739 | 六回目の監査の指摘 |
| 9bea7571 | 七回目の監査の指摘 |
| 97e41771 | 八回目の監査の指摘 |
| 592078e0 | 九回目の監査の指摘 |
| 36cac80c | 十回目の監査の指摘 |
| cc4f3729 | 十一回目の監査の指摘 |

main は 4b5fbd9d（High Definition Audio）へ進んでいる。`vm/ac97.c` と `pci.h` の `iommu_guest` の差は main 側の変更である。この枝を載せるときは、`vm/ac97.c` と `vm/tulip.c` の `pci_enable_io(d);` を残す。

## 48 件

場所は cc4f3729 のファイルの行である。節は手元の公式の文書で確かめた。

### AHCI 1.3.1（`kernel/src/dev/ahci.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| A1 | 直した。halt の間は `settle` が完了を読まず、ST を落とす前に PxCI・PxSACT を読む | 5.3.2.10、6.2.2.2 | `settle` 297、`halt` 794 |
| A2 | 直した。止まらなければ COMRESET、HBA reset、bus master の切断の順。呼ぶ側へは DMA が止まった後に返す | 10.1.2、10.4.2、10.4.3 | `port_force_stop` 494、`hba_reset` 472 |
| A3 | 直した。PI の全 port を止め、PxCLB・PxFB を書く | 10.1.2 手順 3、5 | `hba_set_up` 638 |
| A4 | 直した。PxSERR、PxIS、IS の順に消してから PxIE を開く | 10.1.2 手順 7、10.7.2.1 | `port_listen` 503 |
| A5 | 直した。先頭で AE を立てる。HR の後も立て直す | 3.1.2、10.1.2 手順 1 | `hba_take` 600、`hba_reset` 483 |
| A6 | 直した。PCS・PRCS を受け、worker が `port_examine` で確かめる。確かめるまで命令を出さない | 3.3.5、6.2.2.3、7.3 | `IS_LINK` 119、`port_changed` 1217、`port_examine` 1122 |
| A7 | 直した。port ごとに SUD を立て、DET を待つ | 10.10、10.10.2、10.1.1 手順 4-5 | `port_spin_up` 623 |
| A8 | 直した。PxSACT の消えた命令は成功、残りは出し直す。試行の数は誤りの命令だけ | 6.2.2.2 | `settle` 297、`transfer` 980 |
| A9 | 直した。復帰で `lost` を立て、次の `hba_set_up` が `hba_take` を行う | 10.6、10.6.1、10.6.2 | `ahci_resume` 1380、`hba_set_up` 639 |
| A10 | 直した。止まらない port へ COMRESET を試す | 10.1.2 手順 3、10.4.2 | `port_force_stop` 494 |
| A11 | 非 queued は直した（BSY か DRQ か DET のときだけ COMRESET）。queued の誤りは COMRESET を残した。もう一つの道の READ LOG EXT の命令と頁は ACS と SATA の文書にあり、手元に無い | 10.4、6.2.2.1、6.2.2.2 | `settle` 297、`port_up` 553 |
| A12 | 直した。PSS を受ける | 3.3.5、5.3.9 | `IS_ALL` 120 |
| A13 | 直した。OFS は致命の回復にし、COMRESET を打つ（書込みに要る） | 6.1.5 | `IS_FATAL` 118、`settle` 297 |
| A14 | 直した。報告の案は一部が違う。BIOS は BB を消してから BOS を消す（10.6.4）ので BOS の落ちで終わり、BB を見たら 2 秒を与える | 10.6.3、10.6.4 | `hba_take` 600 |

### NVMe Base 2.4（`kernel/src/dev/nvme.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| N1 | 直した。命令は stop の後にだけ終わる。stop が通らなければ bus master を切る | 3.7.2 | `run` 425、`controller_stop` 359 |
| N2 | 直した。RTD3E は最短の待ちで、上限は `SHUTDOWN_MOST_NS` 60 秒。60 秒は文書に無い方針の値である | 3.6.1、Figure 338 | `shutdown_done` 998 |
| N3 | 直した。IOCSS なら CC.CSS 110b と手順 8 | 3.5.1 手順 3、8 | `enable` 711、`select_command_set` 591 |
| N4 | 直した。Identify Controller を先に行い、SQES・CQES を確かめる | 3.5.1 手順 7-11、Figure 41 | `read_controller` 547 |
| N5 | 直した。I/O queue を消してから SHN を書く | 3.6.1 手順 1 | `delete_queues` 979、`nvme_shutdown` 1030 |
| N6 | 直した。報告の案は SHST と ST を一緒に見るが、ST=1 の subsystem の shutdown も完了を SHST で示すので（3.6.3）、SHST だけで読む | 3.6.1、3.6.3 | `shutdown_done` 998 |

### PCI

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| P1 | 直した。bus master の関数は I/O Space を開けない。使う driver が `pci_enable_io` を呼ぶ | PCI 3.0 6.2.2 | `pci.c` 282、289、`vm/ac97.c` 269、`vm/tulip.c` 121 |

### xHCI 1.2（`kernel/src/dev/xhci.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| X1 | 直した。Configure Endpoint の後に SET_CONFIGURATION。全 class の端点を一つの命令で開く | 3.3.5、4.3.5、4.3.6 | `configure` 1432 |
| X2 | 直した。CA は 64 ビット一回で書き、RsvdP を保つ | 5.4.5、4.6.1.1、5.1.1 | 749-757 |
| X3 | 直した。止まらなければ `hc->failed` から `hc_restart` | 4.6.1.2 | 757、`hc_restart` 2195 |
| X4 | 直した。端点の状態を事象とコマンドの完了で自分で持つ | 4.8.3、4.6.9、4.6.10 | `state_get` 792、`endpoint_quiet` 812 |
| X5 | 直した。順を 4.6.8 に揃え、CLEAR_TT_BUFFER を入れる | 4.6.8、4.6.5、USB 2.0 11.17.5、11.24.2.3 | `pipe_reset` 1229、`tt_clear` 842 |
| X6 | 直した。Supported Protocol の PSI を読む。同じ率は Table 7-13 の ID を先に取る | 7.2.2、Table 7-13 | `port_speed_id` 1565、1550-1597 |
| X7 | 直した。PAGESIZE を読む。4 KiB を超える制御器は Scratchpad の確保を合わせる | 5.4.3、4.20 | 2124-2128 |
| X8 | 直した。最初の TRB の Cycle を最後に書き、Link の前を No Op で埋める | 4.11.7.1、4.11.5.1 | `ring_push` 378、`ring_fit` 408、`endpoint_push` 434 |
| X9 | 直した | 5.1.1 | `wr64` 280 |
| X10 | 直した。HCH を待ち、立たなければ HCRST を書かない | 5.4.1 | 2106 |
| X11 | 直した。Next と BAR の大きさだけで止める | 7 | 2067 |
| X12 | 直した。5 秒 | USB 2.0 9.2.6.1 | `CONTROL_NS` 124 |
| X13 | コメントを直した。今の動きは違反でない | 4.22.1 | `pci.c` 341、`xhci.c` 2041 |
| X14 | 直した | 5.4.8 | `PS_KEEP` 106 |
| X15 | 直した。今の呼ぶ側では当たらない。失敗で `xd->ep` を NULL に戻す | 4.8.3 | `open_endpoints` 1115、1182 |

### HID（`kernel/src/dev/usbhid.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| H1 | 直した。最初は最長の report を TD にした。一回目の監査の後で一 packet の TD に換え、短い packet で report を閉じて組み直す | HID 8.4、xHCI 4.14 | `longest` 59、541 |
| H2 | 直した | HID 6.2.2.8 | 356、492-506 |
| H3 | 直した。main item で結合する | HID 6.2.2.8 | `usage_full` 371 |
| H4 | 直した。範囲の外はそのコントロールを更新しない | HID 5.10、6.2.2.5 | `in_range` 235、267 |
| H5 | 直した。Output の item から LED の bit、ID、長さを読む | HID 6.2.2.5、7.2.2、付録 F.4 | 56-57、404-405、543 |
| H6 | 直した。伸びる配列 | HID 6.2.2.7、6.2.2.8 | 352-353、494 |
| H7 | 直した。usage 1-3 を同じに扱う | HUT 10 | 170-178 |

### BOT（`kernel/src/dev/usbmsc.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| M1 | 直した。subclass 00h、01h、02h、06h だけ受ける | MSC Overview 2 章 | 60 |
| M2 | 直した | BOT 3.2 | `MAX_LUNS` 16 |
| M3 | 監査でも仕様違反でない。residue を文書どおりに読み、実際に動いた長さで抑える | BOT 5.3.3、6.7.2 | 111-121 |

### Hub（`kernel/src/dev/usbhub.c`）

| 番号 | 扱い | 文書 | 場所 |
|---|---|---|---|
| U1 | 直した。ポートの数の上限を消し、14 ポートを超える USB 2 の hub は Route String の欄を 15 にする | xHCI 6.2.2 脚注 106、USB 2.0 11.23.2.1 | `xhci.c` 1697、`usbhub.c` 76 |
| U2 | 直した。壊れる道は無かった。EXT_PORT_STATUS と BOS の Sublink を読む | USB 3.2 10.16.2.6、9.6.2.5、Table 10-15 | `ss_speed`、`read_sublinks` |

## 監査の回ごとの扱い

報告はこの作業木の `docs/kernel-core-20261101/audit-usbstorage-fix*.md` にある。監査役には差分と文書の場所だけを渡し、他の回の報告を読ませなかった。下に書かないものは直した。

| 回 | 記憶 | USB | 直さなかったもの |
|---|---|---|---|
| 1 | 23 | 19 | F18 の MSC の 12 バイトの CDB：Boot 2 は「装置は 12 を受ける」で host の義務でなく、BOT は 1-16 を許し、06h は SPC の長さである。記憶 10 の SET FEATURES：SATA の文書が無い（AHCI 7.3.1.1 は情報的）。IPM=7h と ADSE の消去は入れた |
| 2 | 14 | 11 | 記憶 13：A11 と同じ理由で queued の COMRESET を残す |
| 3 | 18 | 15 | U8 の No Op の連続：一つの No Op は一つの entry しか埋めない。コメントの節を直した。U12 の高帯域 HID：report の切れ目を短い packet で閉じる形を保つ。packet を跨ぐ report は ESIT を余分に使うが、壊れない |
| 4 | 14 | 9 | K4 は順を変えて直した |
| 5 | 5 | 13 | なし |
| 6 | 10 | 16 | 記憶 8 順 B：hibernate の image の書込みが disk の誤りで失敗した後だけに当たる。`hba_reset` の `look_all` は `unverified` を立てないので look_all から外せない。USB 13：`tt_clear` の結果を `endpoint_recover` と `address_device` で捨てる。文書上、健全な hub は断らない。log にとどめる |
| 7 | 7 | 11 | USB 6：壊れた非 disk の装置が settle を止めるのは起動のディスクが未発見のときだけで、上限は `FIND_MOST_NS` 60 秒である。USB 8：stuck の間の上限 64 秒は方針の値である |
| 8 | 6 | 10 | なし |
| 9 | 2 | 3 | なし |
| 10 | 5（USB hub と記憶を一度に） | | なし |
| 11 | | 1 | なし。この直しだけは監査に回していない |

## 判断と文書の問題

### 手元に無い文書

- SATA-IO の Serial ATA は販売物（公式の `sata-io.org/developers/purchase-specification`）で、取っていない。T13 の ACS も手元に無い。ユーザーがフォルダーに置けば、`LINK_NS` 1 秒、`READY_NS` 10 秒、STANDBY、FIS、IDENTIFY の word、READ LOG EXT、COMRESET と揮発 cache の関係を照らせる。
- `ahci.c` の注の「SATA 3.3」「ACS-3」の節番号は、この仕事より前のコミット（cdb91913）のもので、照らしていない。

### 文書に値が無い方針の値

| 値 | 場所 |
|---|---|
| `SHUTDOWN_MOST_NS` 60 秒 | `nvme.c` 38 |
| `ADMIN_NS` 5 秒、`IO_NS` 30 秒 | `nvme.c` 36-37 |
| `IO_NS` 30 秒、`READY_NS` 10 秒、`LINK_NS` 1 秒、`RETRY_NS` 1 秒 | `ahci.c` 43-50 |
| BOT の時間切れ | `usbmsc.c` |
| 試しの間 1-64 秒、`USB_PORT_TRIES` 7、`USB_DEBOUNCES` 5 | `usb.h` 21-22 |
| `CHANGE_READS` 8、`asking` の hub の読みの間 1-64 秒 | `usbhub.c` |

### cache の損失

- link の揺れ、COMRESET、NVMe の CFS・FLR・覚えのない EN=0 で覆われていない書込みがあれば、`blk_cache_lost` が立つ。以後の `blk_persist` は全部失敗し、volume は読むだけになる。失わない方を取った判断である。
- COMRESET で揮発 cache が残るかは、ACS と SATA が無いので分からない。
- AHCI の queued の誤りは COMRESET を打つので、commit の中の media error で C: は読むだけになる。READ LOG EXT の文書が入れば、この道は軽くできる。
- USB の記憶装置と VM の driver は `blk_cache_lost` を呼ばない。
- 六回目の記憶 8 順 A は残した：flush の後の競りで以後の persist が失敗するが、媒体は壊れない。

### NVMe

- 別の装置の判定は VID、SSVID、SN、MN（bytes 63:00）と SUBNQN（bytes 1023:768）で行う（4.7.1.2）。namespace の大きさは読み直さない。
- NSSR は入れない。subsystem の全部に及び、制御器の reset で足りる。
- dead の adapter、切った制御器は電源の変更を止めない。止めても cache を救えない。
- CAP.MPSMIN が 4 KiB を超える制御器は記録して飛ばす。
- 3.5.1 の 8.b.ii.2 の問い合わせの結果で使うのは NSTAT だけである。
- 命令の CSTS の読みは、最初の 1 ms で終わらなかった命令だけにした。速さの変化は測っていない。

### 文書の中の食い違い

| 文書 | 内容 | 取ったもの |
|---|---|---|
| xHCI Table 6-1 | Scratchpad の配列の 248 と、最大 1023 個 | 全部が入る大きさで PAGESIZE の境界に置く |
| xHCI Table 6-4 と 6.2.2.1、4.5.2 | Slot の Speed の廃止と、有効の条件・下りポートの速さ | 速さを入れる |
| xHCI 4.3.7 と Table 6-6 | Parent Hub の項目を入れる装置 | 個別の規定の Table 6-6 |
| xHCI 6.2.3.6 | HS bulk の Interval の単位 | NAK rate の読み |
| xHCI Table 5-27 と 4.19.2 | OCC の方向 | 状態の読みで両方に合う |
| xHCI Table 5-27 の脚注 92 と 4.19.4 | 過電流で接続が消えたときの CSC | CSC に頼らず状態を読むので両方に合う |
| xHCI 4.19.5 の note | USB3 を除いていない | 目的は共有のバスの address 0 で、USB3 の link には当たらない。USB3 は disable しない |

### USB

- hub の順は、configure、descriptor、reset、configure、SET_HUB_DEPTH である。port の reset が一度増える。両方の文書に合う。
- control の非既定の端点は開かない。使う driver が無い。
- TT の clear は、文書が halt の処理としか書かないので、そこだけで送る（F9 の判断）。
- 10.10.4 の offline は入れない。
- 局所電源は「一度あった hub」でだけ「切れた」と読む（USB 2.0 11.5.1.2、USB 3.2 10.3.1.1）。一度も無い hub は bus 給電で動く。
- 状態は変化を消した後に読み直す（USB 2.0 Figure 11-21、USB 3.2 Figure 10-23）。読みの途中で hub が答えなくなれば、確認した変化を `held` と `hub_held` に持ち、hub を毎秒読む。
- 報告の endpoint を諦めた hub は毎秒読む。答えなければ間を 64 秒まで延ばす。

### 試験の手順の誤り

途中の AHCI の試験で `echo hi > q` が消えた。別プロセスの二度の click は double-click にならず、最初の Enter が icon を開いたためで、製品の誤りではない。同じ手順の別の回で `sendkey` を付け忘れて `>` が抜けたのも、手順の誤りである。

## 確かめた範囲

### 静的

- カーネルと媒体は cc4f3729 のソースで警告なしで組めた（`scripts/build.py -j 8 qemu-image`）。出力は `build/wt/agent-a85eb97cac31b3d64/`。
- 差分は 11 回、別の監査役が文書と照らした。監査役の確かめは静的で、実行していない。

### QEMU（窓付き、WHPX、4 CPU）

NVMe の回は cc4f3729 の媒体で確かめた。AHCI と USB の記憶装置の回は、その一つ前のコミットの媒体（`usbhub_work` の末尾の 6 行が無い）で確かめた。この 6 行は `asking` の hub でだけ走り、この回の道には入らない。

| 確かめたこと | 結果 |
|---|---|
| NVMe の起動 | `store: C: "Nocturne" on nvme0` |
| xHCI のキーボード（usbhid）と tablet（absolute pointer） | tablet の click で icon を選び、Enter で Terminal が開き、鍵で打てた |
| 書いて読む | Terminal の `echo hi > q`、`cat q` が `hi`（NVMe、AHCI、USB 記憶装置の三つで画像を自分で見た） |
| hub（FS）の後ろのキーボードと `usb-storage` | `usbhid`、`usbmsc`、`blk: usb0` |
| ホットプラグ | hub の追加、後ろの mouse の追加と除去、後ろの keyboard の追加、hub ごとの除去（子も removed） |
| AHCI の起動 | `store: C: "Nocturne" on sata0`、queued 32 |
| USB 記憶装置からの起動 | super speed、`store: C: "Nocturne" on usb0` |
| 電源断 | 三つとも `pm: stopping the devices` の後に QEMU が終わった |
| serial | 三つとも panic、timed out、failed の行が無い |

## 確かめていない範囲

- 実機と Hyper-V の全部。
- S3 と hibernate（`pm: sleep (S3) available` と出るが、眠らせていない）。
- AHCI のホットプラグ、SSS の板、NCQ の誤り、COMRESET の回復、overflow。
- NVMe の FLR、CFS、別の装置への差し替え、kill での取り消し、時間切れ。
- SSP hub、rank と TT の後ろ（QEMU に HS hub が無い）、CLEAR_TT_BUFFER、PSI のある制御器、PAGESIZE が 4 KiB を超える制御器。
- root port の stuck、Compliance、接続に失敗する装置の試しの間、過電流、局所電源の喪失、`asking` の hub。
- 性能。前の記録も測定も無い。
- `usbnet` は CDC の文書と照らしていない。
- ゲストの試験と WPT は回していない。

## 残ったもの

- SATA と ACS の文書が要るもの：queued の誤りの READ LOG EXT、DevSleep の SET FEATURES、待ち時間の根拠。
- 十一回目の直し（`usbhub_work` の末尾の 6 行）の監査。
- 実機の確かめ。
