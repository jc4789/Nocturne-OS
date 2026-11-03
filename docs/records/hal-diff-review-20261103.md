# HAL の差分を物差しで読んだ報告（2026-11-03）

別のサブエージェント（Opus 5.5）が枝 `HAL-DEV` の差分を一度読んだ報告の全文である。ビルドと動作の試験はしていない。

## 依頼文

日本語で答えてください。読むだけの仕事です。ファイルを変えないでください。ビルドも動作の試験もしないでください。GPL のファイルと Linux のソースは読まないでください。

場所：D:\Programes\llm vibe slop os（git の枝 HAL-DEV）。読む差分は `git diff main...HAL-DEV` です。カーネルとサービスと libc にまたがる大きな置き換えです。

物差しは次のファイルに書いてあります。先に読んでください。
- AGENTS.md の「物差し」「今あるコードの扱い」「完了」「製品のコード」
- .claude/rules/code.md
- .claude/rules/architecture.md
- .claude/rules/kernel.md（錠と文脈の決まり）
- docs/hal.md（この差分が作ろうとした形）

仕事：差分を物差しで読み、所見を「欠陥」と「好み」に分けて書いてください。
- 欠陥：動きが誤る（競合、錠の順の違反、寿命の誤り、失敗の道の抜け、組みでは見つからない呼ぶ側の抜け、起動の順の誤り、ABI の型の誤り、ユーザーの指定した規則への違反）。docs/hal.md とコードが食い違う所も欠陥に入れてください。
- 好み：動きは正しいが、別の形の方が読みやすいと思う所。

速さについて：台帳と実装の表が、ストレージ、網、音、画面の速い道を遅くする所があれば欠陥に入れてください。

書き方：一つの所見ごとに、ファイルと行、何が起きるか（具体的な入力か順序と、その結果）、確かさ（コードで確かめた／見込み）を書いてください。確かめていないことは見込みと書いてください。褒める文は要りません。所見が無い節は「無し」と書いてください。読んだ範囲と、読まなかった範囲を最後に書いてください。

## 報告

結論は次のとおりです。
- 欠陥は 24 件です。
- 先に直すべきものは欠陥 1 から欠陥 4 です。
- 欠陥 1 は、条件が揃う機械で起動が止まります。
- 欠陥 2 は、台帳の読出しが初期化していない記録を返す競合です。
- 欠陥 3 は、ABI の世代の規則への違反です。
- 欠陥 4 は、AVX の無い機械で ABI の申告が偽になることです。
- 速い道（ストレージ、網、音、画面）を遅くする所は、読んだ範囲で見つかりませんでした。

ビルドも動作の試験もしていません。「コードで確かめた」は、コードを読んで確かめたという意味です。動かして確かめた所見はありません。

---

### 欠陥

#### 失敗の道

**欠陥 1. PCI の親の輪で `pci_init` が止まらない再帰になり、起動が死ぬ**
- 場所：`kernel/src/dev/pci.c:198-203`（`device_list`）、`:260-263`（`upstream` の決め方）、`:276`。
- 順序：
  1. firmware が未設定のまま残した橋が、bus 0 に二つあるとします（secondary 0、subordinate 0）。
  2. `:262` は `secondary == devices[i].bus` だけを見ます。U1 の親は U2 になり、U2 の親は U1 になります。
  3. `device_list` は `listed` を `hal_add` の後でしか立てません。U1 → U2 → U1 と再帰が止まりません。
  4. カーネルの stack が尽きます。C: を探す前に起動が終わります。
- 比べた相手：`acpi.c:642` と `:678` の同じ親の辿りには、深さの上限 `PCI_DEPTH_MAX` があります。
- 補足：
  - 既存の `iommu.c:426`（`ids_of`）も同じ輪で止まりません。ただし、これは IOMMU の道でしか走りません。`device_list` はどの機械でも必ず走ります。
  - 未設定の橋が一つだけのときは止まりません。その場合、bus 0 の全部の機能がその橋の子として台帳に載り、設定の窓の木が誤ります。
- 確かさ：コードの道はコードで確かめました。そのような firmware が実機にあるかは見込みです。

**欠陥 2.（競合の節に書きます）**

**欠陥 5. 最初の xHCI の worker が作れないと、xHCI の電源の項目が二度と登録されない**
- 場所：`kernel/src/dev/xhci.c:2389-2396`。
- 順序：
  1. 1 台目の `kthread_create` が失敗すると、`:2394` で戻ります。`pm_register` は通りません。
  2. 2 台目は `ncontrollers == 2` なので、`:2396` の条件が偽です。
  3. `xhci_quiesce` と `xhci_refuse` は呼ばれません。
- 結果：2 台目の USB ディスクがあっても眠りを断りません。眠る前に DMA も止めません。
- 比べた相手：main の `usb_init` は、worker の失敗に関わらず登録していました。
- 確かさ：コードで確かめました。起きるのはメモリーが足りない起動のときだけです。

**欠陥 6. 実装が後から死んでも台帳は `N_HAL_RUNNING` のままである**
- 場所：`kernel/src/dev/pci_drivers.c:19-21` が PCI の唯一の `hal_failed` です。`docs/hal.md:156` は「実装が死んだと分かったら `hal_failed` を呼ぶ」と書いています。
- 順序：
  - `xhci_attach` は `hc_start` の前に 0 を返します。`xhci.c:2298-2300` で `hc_start` が失敗しても RUNNING です。
  - `xhci.c:2262`（halt しない）と `:2279`（失敗が続いて止める）も RUNNING のままです。
  - `hda.c:919-921` は thread が作れなくても 0 を返します。
  - AHCI と NVMe が動作中に死ぬ道にも呼出しがありません（`hal_failed` の全部の呼出しを rg で確かめました）。
- 結果：設定の窓は、動いていない実装を「動かしています」と言います。
- 確かさ：コードで確かめました。

**欠陥 7. USB の実装の失敗が「動かすドライバーがありません」と出る**
- 場所：`kernel/src/dev/xhci.c:1736-1737`、`usb.h` の `struct usb_driver` の `attach`（戻りは `bool`）。
- 順序：
  - `usbmsc_attach` が「no usable bulk endpoints」で false を返すと、状態は `N_HAL_UNBOUND` のままです。
  - `bool` では「自分の物ではない」と「持てなかった」を分けられません。
  - 逆に、`usbhid.c:680` は報告を始められなくても `interface_attach` が true を返し、RUNNING になります。
- 食い違い：`docs/nocturne-native-abi.md:925` は `N_HAL_FAILED` を「`driver` が持てなかった」としています。USB の記録はこの状態になりません。
- 確かさ：コードで確かめました。

**欠陥 8. VMBus の実装の状態が抜ける**
- 場所：
  - `kernel/src/vm/hv/hv_input.c:67-82`（`kbd_init` に `hal_bound` がありません）。
  - `hv_input.c:274-293`（mouse の失敗の道）。
  - `hv_util.c:323`（`vmbus_open` の失敗）。
  - `storvsc.c:301-308` の早い `return`。
- 結果：
  - Hyper-V の動いているキーボードが「動かすドライバーがありません」と出ます。
  - 上の失敗の道も、`N_HAL_FAILED` にならず `N_HAL_UNBOUND` のままです。
- 確かさ：コードで確かめました。

**欠陥 9. `attach` の失敗の理由が誤る**
- 場所：
  - `kernel/src/net/rtl8125.c:640`：`rtl_one` の全部の失敗が `-N_ERR_UNSUPPORTED` です。`:551` の写し切れと `:625` の `no_dma` も含みます。
  - `kernel/src/vm/tulip.c:160`、`:167`、`:178`：メモリー切れが `-N_ERR_UNSUPPORTED` です。
  - `kernel/src/vm/vbe.c:184`：`fail` の道の全部が `-N_ERR_UNSUPPORTED` です。
- 結果：設定の窓はメモリー切れを「対応していない」と言います。`code.md` の規則 9（理由を返す）に反します。
- 確かさ：コードで確かめました。

**欠陥 10. virtio の GPU の二度目の `attach` に守りが無い**
- 場所：`kernel/src/vm/virtio_gpu.c:574-587`、`:736-739`、`kernel/src/dev/pm.c:51-58`。
- 順序：
  1. main の `attempted` が消えました。ほかの一台だけの実装（e1000、tulip、ac97）は `-N_ERR_BUSY` で守っています。virtio だけ守りがありません。
  2. 試験のカーネルで `gpu-enable` を付け、virtio の GPU の機能が二つあるとします。
  3. 二度目の `gpu_start` が `transport_init` で静的な状態を上書きします。
  4. 同じ `gpu_pm` を二度 `pm_register` します。`d->next = NULL` の後に自分を自分の次へ繋ぎます。
  5. 次の `pm_register` は `pm_lock` の下の `while` から出ません。
- 結果：起動が止まります。
- 確かさ：コードで確かめました。製品のカーネルは `test_flag` が偽なのでこの道を通りません。

**欠陥 11. 製品のカーネルで virtio の GPU が失敗として出る**
- 場所：`kernel/src/vm/virtio_gpu.c:575-579`、`:737-738`。
- 順序：`gpu-enable` が無いと `gpu_start` は意図して何もしません。`ready` は偽なので `-N_ERR_UNSUPPORTED` が返ります。
- 結果：QEMU の virtio-vga で、設定の窓が警告の色で「virtio-gpu が動かせませんでした」と出します。何も壊れていません。
- 確かさ：コードで確かめました。

#### 競合

**欠陥 2. `hal_syscall_list` が、書いていない記録を含む数を返す**
- 場所：`kernel/src/dev/hal.c:87-99`、`user/libc/sys.c:639-644`、`user/apps/settings.c:300-305`。
- 順序：
  1. `max` が 32、台帳が 20 件のとき、`:88` の `room` は 20 です。
  2. `kmalloc` から `spin_lock` までの間に、別の CPU の USB の worker が `hal_add` します。
  3. 錠の下で `total` は 21、書いた `n` は 20 になり、21 を返します。
  4. `n_hal_snapshot` は `21 <= 32` なので全部あると判断し、21 を返します。`list[20]` は `realloc` のままの初期化していない記憶です。
- 結果：
  - `id` と `parent` が 0 の記録なら、`hardware_walk` は親 0 で再帰を繰り返します。`hardware[]` の末尾を越えて書き、設定の窓が壊れます。
  - `hardware_where` は未知の `bus` で `where` を埋めず、`settings.c:1143` が stack のごみを読みます。
  - `pcilist` は `bus` 0 を PCI と読みます。
- 起きやすさ：窓は短いです。ただし `TOKEN_HARDWARE` の読出しは、抜き差しの最中に走ります。
- 確かさ：コードで確かめました。

**欠陥 12. claim の終わりと次の claim で台帳の状態が逆になる**
- 場所：`kernel/src/dev/pciuser.c:554-557`、`:832-838`。
- 順序：
  1. A が `claim_mutex` の下で `claimed = false` にして錠を放します。
  2. B が同じ機能を claim し、`hal_bound` します。このとき state が RUNNING なら、`driver` は「nvrm, nvrm」になります。
  3. A の `hal_unbound` が走ります。
- 結果：B が持っているのに `N_HAL_UNBOUND` で空になります。
- 影響：今のサービスの管理者は起動の一度しか読まないので、窓の表示だけが誤ります。`docs/hal.md:262` の「だから二度起こさない」は、再度読むようにした日に誤ります。
- 確かさ：コードで確かめました。両方の呼出しを `claim_mutex` の中へ入れれば順が揃います。

#### 錠の順

無し。`hal_lock` は葉です。`watched_changed` は錠の外で呼ばれます。`on_offer`、RESCIND、`release_slot` の呼出しも `chan_lock` などを持っていません。

#### 寿命

無し。
- USB の `free_device` の呼出しは `release_slot` の一か所で、その頭に `hal_remove` があります。
- VMBus の `channel_free` の二か所は、どちらも RESCIND の `hal_remove` の後です。
- `h->input.name` の指す記憶は `usbhid_detach` の後まで生きます。

#### 起動の順

順の誤りは無し。比べた項目は次のとおりです。
- `hal_init` は `vm_init` の前です。
- `iommu_init` は全部の `attach` の前です。
- `net_init` と `audio_init` の中の準備は `attach` の前です。
- `vbe` は `fb_pm_init` の前です。

実機の表の順は main と同じです。IO の段の電源の順では display が先頭から末尾へ移りましたが、依存は見つかりませんでした。

#### 動きの変わった所（組みでは見つからないもの）

**欠陥 13. 全部の USB の装置で、class の `attach` の前に文字の読出しが走る**
- 場所：`kernel/src/dev/xhci.c:1732-1734`、`:125`（`CONTROL_NS` は 5 秒）、`:883-889`、`kernel/src/fs/store.c:310`。
- 順序：
  1. main で文字を読むのは HID だけでした。今は hub、ディスク、網も読みます。
  2. 文字の要求に答えない装置では、言語と本体の二回で最大 10 秒かかります。
  3. `control_recover` が失敗すると `ctl_broken` が立ち、以後の制御転送が全部失敗します。
  4. その後に走る class の `attach` が巻き添えになります。
- 影響：C: が USB にあるなら、その間は `usb_enumerating` が真のままで、起動も待ちます。
- 確かさ：コードの道はコードで確かめました。そのような装置の有無は見込みです。

**欠陥 14. 入力の装置の名前が 63 バイトで切れ、UTF-8 の途中でも切れる**
- 場所：`kernel/src/dev/usbhid.c:609`、`kernel/src/dev/hal.c:37`、`kernel/src/dev/input.c:294`。
- 順序：main は読んだ文字の全部（最大 379 バイト）を渡していました。今は `name[64]` への `strlcpy` です。
- 結果：長い日本語の製品名は、文字の途中で切れた値が設定の窓の入力の頁へ渡ります。
- 確かさ：切れることはコードで確かめました。窓の表示がどう崩れるかは確かめていません。

**欠陥 15. VM の網が「どれか一つ」から「全部」へ変わり、adapter 0 が変わる**
- 場所：`kernel/src/vm/vm.c`（`emulated[]`、`vm_bind_fixed`）、`kernel/src/vm/hv/hv_util.c:181`、`docs/hal.md:217`。
- 順序：
  1. main は `netvsc || e1000 || tulip` で一つだけ起こしていました。
  2. 今は tulip が先に登録され、netvsc が後です。
  3. Hyper-V の第 1 世代で両方の NIC を付けると、adapter 0 が tulip になります。
  4. `hv_util.c:181` は adapter 0 の住所をホストへ言います。
- 食い違い：この変更は `docs/hal.md` にありません。文書は「adapter の番号は今のまま」と書いています。
- 確かさ：コードで確かめました。Hyper-V では動かしていません。

#### ABI

**欠陥 3. `N_ABI_GENERATION` を上げていない**
- 場所：`common/abi.h:247-248`、`:60-62`（溝 34 の差し替え）。
- 規則：`abi.h:247` は「呼出し、構造体、システムのサービスの文の形を変えたら一つ上げる」と書いています。
- 順序：
  1. 溝 34 は `SYS_PCILIST`（10 バイトの記録、戻りは写した数）から `SYS_HAL_LIST`（152 バイトの記録、戻りは全部の数）へ変わりました。
  2. 世代は 1 のままなので、`proc.c:183` は前の組みのプログラムも起こします。
  3. libc は静的に入っているので、前の組みの `lspci` は `struct n_pciinfo p[64]`（640 バイト）を渡します。
  4. カーネルはそこへ 152 バイトの記録を書きます。
- 結果：呼ぶ側の記憶が壊れます。
- 確かさ：コードで確かめました。

**欠陥 4. AVX が無くても `N_CPU_AVX` を申告する**
- 場所：`kernel/src/arch/smp.c:168`、`:158-160` のコメント、`docs/nocturne-native-abi.md:528`。
- 順序：
  1. AVX は必須の表から外れ、`fpu.c` は CPU が持つときだけ有効にします。
  2. `smp_get_info` は `N_CPU_AVX` を決め打ちで立てます。
  3. コメントの「checks ... require SSE2 and AVX」は今は偽です。
- 結果：ABI の文書は `flags` を「全 CPU が持つ機能」としていますが、偽になります。`tests/smptest.c:68` は AVX の命令を実行し、未定義の命令で落ちます。`tests/cpuinfotest.c:29` は -1 を返します。
- 試し方：`docs/hal.md:338` の書き方なら `-cpu host,avx=off` で踏みます。
- 確かさ：コードで確かめました。動かしていません。

#### ユーザーの指定した規則

**欠陥 16. 同じ引き方の決まりが二か所にある**
- 場所：`user/apps/services.c:59`、`ports/nvidia/include/nvrm.h:33-35`（`libnvrm.c:118` が使います）。
- 規則：`architecture.md` の規則 13（宣言は一か所）と規則 19、`code.md` の規則 14。
- 順序：main の `services.c` は `nvrm_pci_is_gpu` を呼んでいました。今は同じ条件の表があります。片方に機能を足しても組みは何も言いません。起こされないか、つながらないかのどちらかになります。
- 確かさ：コードで確かめました。今の二つの条件は同じです。

**欠陥 17. 読み手の無い値が ABI にある**
- 場所：`common/abi.h:497`（`node`）、`:500-501`（`at.usb.cls`、`port`、`route`、`at.vmbus.type`）。
- 規則：`code.md` の規則 13、`docs/hal.md` の「読む者の無い種類と読む口」は作らない。
- 確かさ：`user/`、`ports/`、カーネルを rg で確かめ、書く所しかありませんでした。

**欠陥 18. カーネルの英語の語が窓の見出しになる**
- 場所：`kernel/src/vm/hv/vmbus.c:209`（`guid_name`：「keyboard」「network」など）、`kernel/src/vm/ata.c:117`（「IDE」）、`user/apps/settings.c:1142`。
- 規則：`name` は `docs/hal.md:57` と ABI の文書 `:925` で「装置が言う名」です。これらは装置が言ったものではありません。文の表（`settings_words.h`）も通りません。`common/abi.h:503` のコメントは文書と違い「人に見せる名前」です。
- 確かさ：コードで確かめました。VM だけで起きます。

**欠陥 19. 窓に内部の値が出る**
- 場所：`user/apps/settings.c:338-348`、`:1143`。
- 規則：`architecture.md` の規則 15（内部の値を窓へ出さない）。
- 内容：`PCI 00:1f.2  8086:2922`、`I/O 1f0` がそのまま出ます。`docs/hal.md:165` が見せるとしたのは「動かしている実装か、動かせない理由」だけです。
- 確かさ：表示はコードで確かめました。これが規則の違反かどうかはユーザーの判断です。

**欠陥 20. 呼ぶ側が一つの包みを足した**
- 場所：`kernel/src/vm/virtio_gpu.c:736`（`gpu_attach` → `gpu_start`）、`kernel/src/vm/vbe.c:187`（`vbe_attach` → `vbe_start`）、`kernel/src/net/rtl8125.c:632`（`rtl_attach` → `rtl_one`）。
- 規則：`architecture.md` の規則 10。本体を一つにまとめれば足ります。
- 確かさ：コードで確かめました。

**欠陥 21. 検索で一意でない名前を足した**
- 場所：`kernel/src/dev/hal.c:9-13`（`first`、`last`、`ids`、`listed`、`changed`）、`kernel/src/dev/pci_drivers.c`（`bind`、`drivers`）、`kernel/src/arch/mca.c`（`layout`）。
- 規則：`code.md` の規則 3。`first` は `arch/cpu.c` にもあります。
- 確かさ：コードで確かめました。動きは正しいです。

#### `docs/hal.md` とコードの食い違い

**欠陥 22. 文書の決まりをコードが守っていない**
- `docs/hal.md:63`「指す側はポインターでなく番号を持つ」と、`usbhid.c:609` の `h->input.name = d->hal.info.name`（記録の中を指します）。
- `docs/hal.md:80`「読み手は錠の下で写す」と、`input.c:294`、`pci_drivers.c:15-17`、`xhci.c:1708` の直接の読出し。書き手と重ならないので今は競合しません。
- `docs/hal.md:150`「`vm_init` は VM の判定…を持つ」と、今のコード。判定は `vm_detect` です。`vm_init` は VMBus の起こしと `hv_input`、`hv_util` の結びを行います。
- `docs/hal.md:136-144` の起動の順に `hal_init` と `vm_init` がありません。実際は VMBus の記録が `pci_init` より先に載ります。`vm_bind_fixed` の行は「VMBus」と書いていますが、`hv_input` と `hv_util` はそこで結びません。
- `docs/hal.md:347`「`hal.h` を読むのは、バスと、台帳を読む者だけ」と、今のコード。`pci.h`、`usb.h`、`vmbus.h` が include するので、`pm.c`、`mm/shared.c`、`fs/store.c`、`hibernate.c` など 35 ファイルに届きます。
- 確かさ：コードで確かめました。

**欠陥 23. 行の参照がこの差分でずれた**
- `docs/hal.md:194` の `xhci.c:2311-2314` は `hc_restart` の行を指しています。`xhci_refuse` は `:2364` です。
- `docs/hal.md:327` の `init.c:56` は `stream_console_open` の行です。入力の道は `:51` です。
- `docs/hal.md:103` の `nvme.c:1022-1029` はコメントの行です。輪は `:1036-1052` です。
- 確かさ：行を開いて確かめました。ほかの参照（`pm.c`、`pciuser.c`、`rtl8125.c`、`fb.c`、`thread.c`、`vbe.c`、`net.h`、`ahci.c`、`fb.h`、`input.c`）は合っていました。

**欠陥 24. ほかの文書の残り**
- `README.md:53` が「下限は Zen 4（x2APIC、PCID、AVX-512 など）」のままです。
- `user/include/nocturne.h:447` の「The count of PCI functions…」が、台帳の宣言の上に離れて残っています。
- 確かさ：コードで確かめました。

#### 速さ

無し。読んだ結果は次のとおりです。測っていません。
- ストレージ：`nvme.c`、`ahci.c`、`usbmsc.c` の差分は `attach` の周りと状態の場所だけです。`struct blkdev` の道は変わっていません。
- 網：`rtl8125.c`、`net.c`、`e1000.c`、`tulip.c`、`netvsc.c` の差分は `attach` と登録だけです。
- 音：`hda.c`、`ac97.c`、`audio.c` の差分は `attach` と初期化だけです。
- 画面：`fb.c` に差分はありません。`vbe.c` の差分は `attach` だけです。
- 増えた費用で見つけたものは次の二つで、どちらも測れる大きさではないという見込みです。
  - `fpu_save` と `fpu_restore` が定数でなく `components` を読みます。
  - `struct usb_device` が記録の分だけ大きくなりました。
- 起動の遅れの見込みは欠陥 13 にあります。

---

### 好み

1. `pcilist` は呼ぶたびに全部を写します。`libnvrm.c:100-115` と `nvkmd_nocturne_rm.c:112` は、その上でさらに容量を倍にする輪を回しています。呼ぶ側が `n_hal_snapshot` を読んで絞る方が読みやすいです。
2. `lspci.c:28` の 64 件と `neofetch.c:30` の 32 件の固定の配列は、黙って切れます。これは差分の前からです。全部の数が取れるようになったので直せます。
3. `SYS_HAL_LIST` は公開の呼出しのままで、USB の製品名や VMBus の種類まで普通のプログラムに見えます。普通のプログラムの中で要るのは、見込みでは `nvk` の PCI の機能だけです。狭い口を分ける案があります。
4. `struct cpu_maker` の `mca_layout`、`perf_way`、`cache_leaf` は関数です。製造者ごとのデータと `cpu_features` の旗で書ける所があります。
5. `pci_bind_all` は一台ずつ待ちます。NVMe が多い機械では起動が伸びます。main と同じ形で退行ではありません。
6. `struct n_hal_device` に `_Static_assert` がありません。今の大きさは 152 バイトと計算しました。隣の ABI の構造体には大きさの確かめがあります。
7. `.claude/rules/kernel.md` の錠の順に `hal_lock` がありません。葉であることは `hal.c` の頭にしか書いてありません。
8. QEMU の pc の機械では、PIIX の IDE の PCI の機能（UNBOUND）と `vm/ata` の記録（RUNNING）が、同じ物を二行で見せます。
9. `usb_drivers.c` の表と `usb_drivers.h` の番号は二か所で揃える必要があります。`state[]` の場所を `struct usb_driver` の中に持つ形もあります。

---

### 読んだ範囲と読まなかった範囲

読んだ：
- 物差し：`AGENTS.md`（システムの指示で全文）、`.claude/rules/code.md`、`architecture.md`、`kernel.md`、`map.md`、`docs/hal.md`。
- `git diff main...HAL-DEV` の全部のファイルのうち、ソースと文書の差分。`ports/mesa/README.md` と `ports/ffmpeg/prepare.py` はコメントだけでした。
- 差分の外で周りを読んだ所：`sys/watch.[ch]`、`dev/pm.[ch]` の登録と順、`xhci.c` の `device_attach`、`usb_remove`、`release_slot`、`hc_restart`、`control`、`pciuser.c` の claim の道、`vmbus.c` の offer と rescind と release、`netvsc_probe`、`storvsc_start`、`hv_input.c`、`pci.c` の列挙、`entropy.c`、`smp_get_info`、`libnvrm.c`、`lspci.c`、`neofetch.c`、`net_register`。

照らして差を見つけなかった所：
- 実装の表の引き方と、main の条件。
- `services.c` の表と `nvrm_pci_is_gpu` の条件。
- `arch/mca.c`、`cpu_amd.c`、`cpu_intel.c` への移動と、main の機械の検査、TSC、性能の要求、topology。
- 必須の表から外した機能の使い手。Makefile の旗、rust の target、wabt、BearSSL は、実行時に選ぶか `-msse2` です。

読まなかった：
- 各ドライバーの全文（差分と周り以外）。
- `third_party/` の中（`nvkmd_nocturne_rm.c` の該当の行だけを除く）。
- `docs/records/`（`hal-read-test-*` も）。
- `nocturne_gl.c` と `js_webgl.c` の参照は一行だけを見ました。
- GPL のファイルと Linux のソースは読んでいません。

確かめていないこと：
- ビルド、`make check`、QEMU、Hyper-V、実機。
- `tests/check_boundaries.py` を読んだのは頭の部分だけで、走らせていません。

依頼の「ファイルを変えない」に従い、`AGENTS.md` の「報告の全文を `docs/records/` へ」は行っていません。
