# HAL-DEV の差分の読み：PCI の segment と結びつけの待ち

枝 `HAL-DEV`、コミット `a34d34a3` に対する作業木の差分（54 ファイル）を、別のサブエージェント（Opus 5.5）が一度だけ読んだ記録。

## 依頼文

リポジトリ `D:\Programes\llm vibe slop os`（独自の OS、Nocturne）の、未コミットの作業木の差分を一度だけ読み、欠陥を探してください。読むだけです。ファイルを変えず、コミットせず、ビルドも QEMU も動かさないでください。返事は全部日本語で書いてください。

読むもの：

- 差分：`git diff`（枝 `HAL-DEV`、HEAD に対する作業木の変更。54 ファイル）。
- 主な変更の先：`kernel/src/dev/pci.c`、`pci.h`、`iommu.c`、`iommu.h`、`iommu_amd.c`、`iommu_intel.c`、`acpi.c`、`hibernate.c`、`pciuser.c`、`nvme.c`、`ahci.c`、`xhci.c`、`blk.c`、`blk.h`、`kernel/src/fs/store.c`、`kernel/src/init.c`、`common/abi.h`、`user/libc/sys.c`、`ports/nvidia/`、`third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/`。
- 物差しと規則：`AGENTS.md`、`.claude/rules/code.md`、`.claude/rules/architecture.md`、`.claude/rules/kernel.md`、`docs/hal.md`。
- 仕様書は `D:\Programes\OS DEV INFO`（PCIe、NVMe、AHCI、xHCI、IOMMU など）。要るときに照らしてください。GPL のファイルと Linux のソースは読まないでください。

差分だけで足りないときは、変更されたファイルの全体と、その呼ぶ側を読んでください。

変更の目的：

1. PCI の住所に segment を通す：カーネルの列挙、構成空間の読み書き、IOMMU の身元、ACPI の探索、休止の照合、ABI、ユーザー空間、NVIDIA のドライバー、NVK。
2. PCI の結びつけ（`pci_bind_all`）で、実装の `attach` が秒の単位で待たないようにする：NVMe と AHCI の立ち上げを thread へ移し、C: を探す側の待ちを直す。

問い：次の三つを、差分とコードから自分で判断してください。

- 複数の segment がある実機で、誤った機能に触れる、機能を見落とす、誤った IOMMU の表へ書く、という道が残っていないか。
- thread へ移したことで、錠の順、競合、電源の変化（眠り、電源断、休止からの戻り）との順序、失敗の道、C: の探し方に壊れた所がないか。
- `AGENTS.md` の物差しと `code.md`・`architecture.md` の規則に反する形（shim、スパゲッティ、古い道の残り、呼ぶ者の無いコード、古くなったコメント）が差分に入っていないか。

報告の形：所見を「欠陥」と「好み」に分けてください。欠陥には、`ファイル:行`、何がどう壊れるかの具体の筋書き（どの機械で、どの順で、何が起きるか）、確からしさを書いてください。コードを読んで筋書きが成り立たないと分かったものは書かないでください。見つからなければ、見つからなかったと書いてください。読んだ範囲と読まなかった範囲も書いてください。

## 報告

### 結論

- 欠陥は 10 件ある。
- すぐ直すものは 1 と 2 である。1 は、segment が 0 でない GPU が動かない欠陥で、コードで確定している。2 は世代の規則違反である。
- ① の「誤った機能に触れる、機能を見落とす」道は 3 と 9 に残っている。隔離の穴は 4 にある。
- 「誤った IOMMU の表へ書く」道は見つからなかった。ただし VT-d の割り込みの表は全 segment で共有している（欠陥 4）。
- ② で壊れた所は、C: の探し方に集中している（5、6、8）。錠の順と、眠り、電源断、休止からの戻りとの順序には、欠陥を見つけなかった。
- ③ は古いコメントと文書（10）、整形の崩れ（好み）である。shim は見つからなかった。
- 確かめは全部コードを読んだだけである。ビルドも QEMU も動かしていない。ファイルは変えていない。

### 欠陥

#### 1. segment が 0 でない GPU で `nvrm` が接続を受けず、立ち上げを繰り返す

- 場所：`ports/nvidia/nvrm_serve.c:291`。関数の頭は `nvrm_serve(uint32_t pci_target)`（`:290`）。`ports/nvidia/nvrm.c:395`、`:406` が関係する。
- 原因：`if (pci_target > UINT16_MAX) return -N_ERR_INVALID;` が残っている。`libnvrm` の側の同じ検査は消したが、このファイルは差分に入っていない。
- 筋書き：
  1. segment 1 に GPU がある機械で、サービスの管理者が `nvrm` を起こす。
  2. `N_PCI_TARGET` は 0xFFFF を超える。
  3. `nvrm.c:395` の claim と `bring_up` は通る。
  4. `nvrm.c:406` の `nvrm_serve` が -INVALID で戻り、`tear_down` して終わる。
  5. 管理者が起こし直し、そのたびに GPU を立ち上げ直す。NVK は接続できない。
- 試験で捕まらない理由：`tests/nvrm_servetest.c:33` の `TARGET` は `N_PCI_TARGET(0, 0xfe, 0x1f, 7)` で、segment は 0 である。`nvrm_transport_hosttest.c` も segment 0 だけである。
- 確からしさ：確か。コードで確定している。動かしてはいない。

#### 2. `N_ABI_GENERATION` を上げていない

- 場所：`common/abi.h:248`。規則は `common/abi.h:247` にある。`kernel/src/sys/proc.c:183` が関係する。
- 変えた形：`struct n_pciinfo`（頭に segment）、`struct n_pci_function`、`n_pci_claim` の引数、`N_PCI_SELF` の値、`upstream` の意味。値は 2 のままである。規則は「呼出し、構造体、システムのサービスの文の形を変えたら一つ上げる」である。
- 筋書き：
  1. 前の組みのプログラムが C: に残ったまま、カーネルだけが新しくなる。
  2. `proc.c:183` は同じ世代と見て、そのプログラムを起こす。
  3. 古い `lspci` などは `n_hal_device.at.pci` を 2 バイトずれて読む。
  4. 古い `n_pci_config_read` の `N_PCI_SELF`（0xFFFFFFFF）は別の機能の住所として読まれる。
- 前例：`docs/records/hal-diff-review-20261103.md` の欠陥 3 が同じ欠陥である。
- 確からしさ：規則違反は確か。媒体を丸ごと組み直す機械では踏まない。

#### 3. MCFG の base を start_bus の番地として読むので、0 以外の segment で機能を読み違える

- 場所：`kernel/src/dev/pci.c:68-70` 付近（`ecam_regs` の `n = (bus - w->start_bus) << 8 | ...`、`phys = w->base + (n << 12)`）。`struct ecam_window` のコメント `/* of start_bus, device 0, function 0 */`。`kernel/src/dev/acpi.h:170`。
- 参照との差：FreeBSD（`sys/amd64/pci/pci_cfgreg.c:299,340`）と Fuchsia（`src/devices/board/lib/acpi/pci.cc:377`）は、どちらも base を bus 0 の番地として読む。
- この差分で変わったこと：式は前からある。segment 0 は `ecam_activate` の CF8 との照合が守っていた。この差分で、0 以外の segment はその照合なしに ECAM だけで列挙する。誤りは素通りする。
- 筋書き（二重に載る場合）：
  1. MCFG に {segment 1、buses 0x80-0xFF、base A} と {segment 0、buses 0-0x7F、base A} がある。
  2. segment 1 の bus 0x80 を A+0、つまり segment 0 の bus 0 として読む。
  3. 機能が二重に載る。
  4. `bar_probe` が別の機能の BAR を書く。
  5. 実装が二度結ばれる。
- 筋書き（見落とす場合）：base が他の窓と重ならなければ、誤った範囲を読み、segment 1 の機能を丸ごと見落とす。
- 確からしさ：参照 2 つとの式の差は確か。PCI Firmware Specification は手元に無く、仕様の原文では照らしていない。start_bus が 0 でない窓を持つ機械は少ない。

#### 4. VT-d の割り込みの表を全 segment で共有し、SID 検査が segment を区別できない

- 場所：`kernel/src/dev/iommu_intel.c:193`（`irt` が一つ）、`:1131`（全 unit に同じ `R_IRTA`）、`:776-791`、`:841`（IRTE の SID は 16 bit）。
- 筋書き：
  1. segment 1 の `0001:01:00.0` をアプリのドライバーが claim する。
  2. カーネルの NVMe が `0000:01:00.0` にある。
  3. アプリは装置に、NVMe の IRTE の番号で MSI を出させる。
  4. SID 0x0100 が一致して通る。
  5. 隔離の目的を破る割り込みの注入になる。
- AMD：domain ごとの IRT なので当たらない。
- 確からしさ：コードから筋書きは成り立つ。動かしていない。

#### 5. 最初の見が「まだ答えない」でも `blk_looking_end` を呼び、C: を待たずに panic する

- 場所：`kernel/src/dev/ahci.c:1169-1172`（`again`）、`:1255`（`first_look` の `blk_looking_end`）。`kernel/src/fs/store.c:311`。反する文は `kernel/src/dev/blk.h:45` の「may yet bring a disk」である。
- 原因：worker の再試行（`RETRY_NS` 1 s）は looking に数えない。
- 筋書き：
  1. C: の SATA ディスクが、`READY_NS`（10 s）を超えて 12 s で答える。
  2. `first_look` は約 10-11 s で `again` になり、looking が消える。
  3. `store_init` は同時に数え始めているので、waited が `FIND_NS` 以上で、すぐに break する。
  4. `panic("store: no disk holds a Nocturne volume")` になるか、別のディスクの C: を取る。
- 変更前：attach が同期で終わった後に 10 s の猶予があり、再試行で見つかった。IDENTIFY が `IO_NS`（30 s）で時間切れになる場合も同じ道を通る。
- 確からしさ：コードで筋書きは成り立つ。起動元のディスクは firmware が回した後なので、普通は踏まない。

#### 6. 走査の後で `blk_looking()` を読むので、最後に現れたディスクを見ずに諦める

- 場所：`kernel/src/fs/store.c:293-311`。
- 筋書き：
  1. waited が 10 s 以上の状態で、走査の最後の `blk_count()` を読む。
  2. `blk_looking()` を読む前に、ディスクの登録と `blk_looking_end` が入る。
  3. そのディスクを見ないまま break する。
- 直し方の向き：`blk_looking()` を走査の前に読めば消える。
- 範囲：前の `usb_enumerating` にも同じ窓があった。この差分で NVMe と AHCI にも広がった。
- 確からしさ：競合は確か。窓は数 µs で、横取りのときに広がる。頻度は低い。

#### 7. IOMMU の fault の行に segment が出ない

- 場所：`kernel/src/dev/iommu_amd.c:512,515`、`kernel/src/dev/iommu_intel.c:551,554`。
- 内容：書式は `%02x:%02x.%u` のままである。`u->segment` は手元にあるのに、`fault_show` は `(void)u` としている。
- 筋書き：複数 segment の機械で fault が出ると、ログは別の機能を指しうる。`docs/hal.md:62` の印字の決まりに反する。
- 確からしさ：確か。影響はログだけである。

#### 8. boot ディスクの GUID が分からないとき、C: が起動ごとに変わりうる

- 場所：`kernel/src/fs/store.c:301`。
- 筋書き：
  1. `known` が false で、Nocturne の区画が二つある機械を起動する。
  2. 「最初に見つかったもの」を取る。
  3. thread 化で登録の順が競争になり、起動ごとに違うディスクが C: になる。
- 変更前：表の順（AHCI、次に NVMe）で決まっていた。
- 確からしさ：コードで成り立つ。`known` が false になる機械は稀である。

#### 9. segment の窓を持てないとき、機能の見落としを記録しない

- 場所：`kernel/src/dev/pci.c:58-65` 付近（`pci_config_init`）と、`ecam_regs` の `vmm_map_mmio` 失敗。
- 筋書き：
  1. 起動時のメモリー切れで窓か写像を作れない。
  2. その segment の機能を見落とす。
  3. `devices_lost` が立たず、`pci_all_known()` は true のままである。
  4. `iommu.c` の「全部の機能」の群と `bus_reset_refused` が、見落としを知らずに決める。
- 確からしさ：コードで成り立つ。発生はほぼ無い。

#### 10. 古い文書、古いコメント、偽の試験の行

| 場所 | 古い内容 |
|---|---|
| `.claude/rules/kernel.md:21` | `on nvme0`、`sata0` と書いている。 |
| `docs/hal.md:168` | `ahci.c:1167-1169` を指す。今の `gone` の行は 1181-1182 である。 |
| `kernel/src/dev/ahci.c:1134` | 「One caller at a time: the start, then the adapter's thread.」と書いている。今は `first_look`、worker、holder の順を `examining` が決める。 |
| `kernel/src/dev/ahci.c:244` | `hbas` の「set up at boot, before anything else looks」は今の順に合わない。 |
| `kernel/src/fs/store.c:24-25` | 「past what an NVMe controller may take to be ready」と書いている。`enable` は `controller_stop` と合わせて CAP.TO の 2-3 倍かかりうる。CRTO はもっと長い。 |
| `tests/nvrmtest.c:41-43` | `where` を `%04x:%02x:%02x.%x` で渡すが、`nvrm.c` は `strtoul(argv[1], NULL, 16)` で読み、0 と解する。この行を触ったのに偽のままである。不具合は前からで、試験の表 `runtests.c` と Makefile の明示には無い。 |

確からしさ：どれも読んで確かめた。

### 好み

- `kernel/src/dev/pci.c` の「a window is never mapped as a whole」は文が緩い。0 以外の segment の列挙は、全 bus の全 device の function 0 の頁（窓あたり 8192 頁）を写す。動きは正しい。
- `kernel/src/dev/iommu.c:596-601` の「every function is in the group」は全 segment を群に入れる。別の segment へ要求は渡らない。安全側で壊れはしないが、segment 1 の GPU が segment 0 の NVMe のために隔離できなくなる。
- `kernel/src/dev/xhci.c` が `dev/blk.h` を include する。バスがディスクの待ちを知る形である。
- 置き換えた行の整形が崩れている。
  - `hda.c`、`vbe.c`、`pci.c`、`iommu.c` に 120 桁を超える `kprintf` がある。
  - `pci_write16(d->address, PCI_COMMAND,` の後の折り返しが崩れている。
  - `ahci.c:208`、`:716-717` の折り返しが崩れている。
- `kernel/src/dev/ahci.c:1243` の前方宣言は、`start` を上へ動かせば要らない。
- `kernel/src/dev/iommu_amd.c:194` の `IOMMU_AMD_HOSTTEST` を定義する試験が無い。差分の外で、前からある。
- hda の `key` の形が変わり、保存された音の出口と形式の選択は更新で一度外れる。「互換は持たない」の範囲である。切り詰めは起きない。

### 確かめて、成り立たなかったもの

segment と IOMMU：

- AMD の device table は segment ごとである。`dte_write` の ASSERT は `is_covered` で守られる。
- `entry_flush`、`irt_flush`、`contexts_flush`、`intel_attach` は segment で絞ってある。
- IVMD 0x20 は pass 1 で全 unit の segment に入る。alias は segment つきである。
- `pci_config_firmware_*` は spinlock の前に頁を写す。`pci_config_init` は heap と `acpi_find` の後である。
- `n_pci_function.upstream` の読み手は `tests/pcitest.c` だけで、`N_PCI_HAS_UPSTREAM` に直してある。
- hosttest の初期化子は designated で、segment の追加でずれない。
- `N_SERVICE_ARGUMENT_MAX` 16、`N_SERVICE_NAME_MAX` 64、`NVRM_CHANNEL_NAME_BYTES` は足りる。

thread 化：

- `blk_register` は mutex で複数の thread でも安全である。`intel_msi` は `tables_lock` で並行に安全である。kthread は戻れば `thread_exit(0)` する。
- `port_up` の錠の開放は安全である。`h->lost` か `need_reset` で live にならない。
- `p->examining` と `blk_busy` の順で、hold との deadlock は無い。holder が走るとき、`examining` を持つ者はいない。
- `nvme_shutdown`、`nvme_resume`、`ahci_quiesce`、`ahci_resume` は未起動の制御器（`regs` NULL、`ports` NULL）を飛ばす。
- thread が門の前にいる間の眠り、電源断、休止の戻りは順が保たれる。`hibernate_restore` の `blk_hold` は `first_start` と `first_look` を待ち、DMA は quiesce で止まる。
- `blk_looking` の begin と end の数は全部の道で合う。
- `usb_enumerating`、`unsettled`、`ndisks` の残りは kernel で 0 件である。
- `rtl_attach` と `hda_attach` の待ちは 100 ms 級で、秒の単位に届かない。`hda` の probe は thread である。

### 読んだ範囲

- 差分の全部（54 ファイル）。
- 差分の外で読んだもの：`kernel/src/dev/ahci.c`（180-840、1040-1467）、`kernel/src/dev/nvme.c`（120-240、365-770、840-1112）、`kernel/src/dev/blk.c`・`blk.h`・`pm.c` の全部、`kernel/src/dev/hibernate.c`（100-160、560-716）、`kernel/src/fs/store.c:225-374`、`kernel/src/dev/xhci.c:2305-2445`、`kernel/src/dev/pci_drivers.c`、`kernel/src/dev/iommu_amd.c`（150-210、222-292、296-425、560-1035）、`kernel/src/dev/iommu_intel.c`（1-20、146-215、285-414、540-560、700-1030）、`kernel/src/dev/iommu.c:540-689`、`kernel/src/dev/pci.c`（147-185、285-325、1385-1505）、`kernel/src/init.c`、`kernel/src/sys/thread.c` の `kthread_start`、`docs/hal.md`、`.claude/rules/code.md`・`architecture.md`・`kernel.md`、`ports/nvidia/nvrm_serve.c:280-320`・`os_pci.c:40-110`・`nvrm.c:385-410`、`user/apps/services.c:150-285`、`tests/nvrmtest.c:25-50`。
- 参照として読んだもの：FreeBSD の `pci_cfgreg.c`、Fuchsia の `pci.cc`（欠陥 3 の該当の行）。

### 読まなかった範囲

- `kernel/src/dev/xhci.c` の worker と attach 以外。
- `kernel/src/dev/hda.c` の全体（差分の行は読んだ）。
- `kernel/src/dev/iommu_intel.c` の 1030 行以降の細部（`unit_tables`、probe、resume）。`:1131` の `R_IRTA` の行だけは読んだ。
- `kernel/src/dev/iommu.c` の残り。
- `ports/nvidia` の RM 本体と `kernel-open`。
- Mesa の他の部分。
- `scripts/`。
- VM の実装（`kernel/src/vm/` の差分以外）。
- 仕様書の原文との照らし。PCI Firmware Specification は手元に無い。

### 証拠の範囲

- 静的な読みだけである。依頼のとおり、ビルド、試験、QEMU、実機のどれも動かしていない。
- 「確か」と書いたものも、コードの読みで確定したという意味で、動かして見たものではない。
- 行番号は読んだ時点の作業木のものである。「付近」と書いた行は、関数の中で多少ずれている見込みがある。
