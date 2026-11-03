# HAL の設計

状態：設計である。まだ作っていない。作り終えたら、今の形の説明へ書き直す。

規則は `.claude/rules/architecture.md` の HAL の節にある。ここは、その形を決める。読む試験の報告は `docs/records/hal-read-test-*-20261103.md` にある。

## 判定の物差し

この設計は、次の三つで判定する。

1. Claude が正しく実装できるか。
2. OS のほかの部分を手伝うのを、邪魔しないか。
3. 一か月後の Claude も正しいと思うか。

性能への影響は低く保つ。形の美しさは物差しにしない。

## 五つの決まり

1. **HAL は制御の道だけを持つ。** 見つける、結びつける、申告、開始、停止、失敗は HAL の形で行う。パケット、ブロック、音、GPU の命令は HAL の部品を通らない。データの道は種類ごとに下の表に書く。
2. **種類ごとに置き場を決める。** 実装はカーネルに組み込む。実機の画面と GPU はプロセスで動かす。網から来た値を読む実装もプロセスで動かす（`architecture.md` の規則 20）。
3. **関数の表は、実装が二つ以上ある種類だけが持つ。** 実装が一つなら、契約は見出しとファイルの境目だけで、呼出しは直接にする。
4. **契約の上のコードは、製造者、機種、世代を知らない。** 違いは実装の申告で受ける。
5. **カーネルへコードを読み込まない。** 実装はカーネルに組み込むか、プロセスで動かす。

## 部品

HAL は三つの部品でできている。

- 種類ごとの契約：見出しと、要るなら関数の表。多くは今もある。
- バスごとの実装の表：見つけた物に実装を結びつける。
- 台帳：機械で見つかった物の一覧。

### 台帳

`kernel/src/dev/hal.h` と `hal.c` が持つ。一つの記録は、バスで見つかった一つの物を指す。

- 載るもの：PCI の機能、USB の装置、VMBus の channel、固定のポートの制御器。
- USB の interface は記録にしない。キーボードは一つの記録である。
- 実装が作る物は種類の一覧に載る：ディスク、NIC の adapter、音の出口、画面、入力の装置。一つの物は一つの台帳に載る。
- 種類の一覧の項目は `hal_id` を持ち、元の記録を番号で指す。
- 載らないもの：CPU（`arch/topology.c` の一覧）、IOMMU、割り込み、時計、`vm/rdp`、ACPI の装置。ACPI の装置は `dev/acpi.c` の中の話で、EC と電池の実装を足す日に表を作る。

```c
enum hal_bus { HAL_BUS_PCI, HAL_BUS_USB, HAL_BUS_VMBUS, HAL_BUS_PORT };
enum hal_state { HAL_UNBOUND, HAL_RUNNING, HAL_FAILED };

struct hal_device {
    uint32_t id;                /* 起動の間、二度と使わない番号 */
    uint32_t parent;            /* 親の番号。0：親なし */
    enum hal_bus bus;
    enum hal_state state;
    int reason;                 /* HAL_FAILED の N_ERR_* */
    int node;                   /* NUMA の node。-1：不明 */
    union hal_identity identity; /* PCI：住所、機種、種類。USB：機種、種類、route。VMBus：GUID。ポート：番地 */
    char *name;                 /* バスか実装が埋める。NULL：無い */
    char *driver;               /* 持っている実装の名。プロセスの実装はプログラムの名 */
};
```

- 記録はバスの物に埋める（`struct pci_device`、`struct usb_device`、VMBus の channel、`vm/ata`）。寿命はその物と同じである。
- 指す側はポインターでなく番号を持つ。参照の数は持たない。番号は二度と使わないので、古い番号は何にも当たらない。

操作は次のとおりである。

| 操作 | 文脈 | 働き |
|---|---|---|
| `hal_add(d, parent, bus, &identity)` | 眠ってよい | 台帳に載せる。`HAL_UNBOUND` で始まる |
| `hal_name(d, text)` | 眠ってよい | 名前を写して持つ |
| `hal_bound(d, driver)` | 眠ってよい | `HAL_RUNNING` にする |
| `hal_unbound(d)` | 眠ってよい | `HAL_UNBOUND` へ戻す |
| `hal_failed(d, reason)` | どこでも | `HAL_FAILED` にする |
| `hal_remove(d)` | どこでも | 台帳から外す |

- 台帳は葉の spinlock を一つ持つ。実装を呼ばない。順序の仕事を持たない。
- 変化は `watched_changed`（`sys/watch.h`）で知らせる。これはどの文脈からでも呼べる。
- 記録を読むのは写しだけである。読み手は錠の下で写す。

### 実装の表

結びつけはバスが行う。バスごとに型のある表を一つ持つ。実装は自分のバスの型を直接受ける。

```c
struct pci_match { uint16_t vendor, device; uint8_t cls, subcls, progif, fields; };

struct pci_driver {
    const char *name;
    const struct pci_match *matches;
    size_t match_count;
    int (*attach)(struct pci_device *d);
};
```

PCI：

- 表は `kernel/src/dev/pci_drivers.c` に一つ置く。VM の実装は `vm_pci_drivers()` が返す。実機では空である。
- 行は `PCI_ID(vendor, device)`、`PCI_CLASS(cls, subcls, progif)`、`PCI_VENDOR_CLASS(vendor, cls, subcls)` で書く。`fields` が見る欄を言う。
- 引き方は一つである。機種の行が合う実装を先に、ほかの行が合う実装を後に取る。実機の表を先に、VM の表を後に見る。点数も `probe` の関数も持たない。
- `attach` は眠ってよい。0 を返すと `HAL_RUNNING` になる。負を返すと `HAL_FAILED` になる。
- 失敗した実装も、持ち続ける物を自分で持ってよい。止まらない NVMe に SHN を書く今の輪（`nvme.c:1022-1029`）はそのまま残る。
- 実装は `attach` の中で `pm_register` を呼ぶ。制御器ごとの `struct pm_device` でも、一つの `struct pm_device` でもよい。
- `kernel_owned` と `claimed` は IOMMU と claim の判断の事実で、`pci.c` と `pciuser.c` が今のまま持つ。

USB：

```c
struct usb_driver {
    const char *name;
    bool (*claims)(const struct usb_interface_desc *intf);
    void (*attach)(struct usb_device *d, const uint8_t *cfg, unsigned total);
    void (*detach)(struct usb_device *d);
    void (*work)(struct usb_device *d);
};
```

- 表は `kernel/src/dev/usb_drivers.c` に置く。行は `usbhid`、`usbmsc`、`usbnet` である。hub はバスの一部で、表に入らない。
- `configuration_supported`（`xhci.c:1446`）は `claims` を順に引く。xHCI は class の名を知らなくなる。
- `usbhid_attach` は `usbmsc_attach` と同じ形に揃える。CDC の二つの interface は、今のとおり `usbnet` が設定の全体を読んで持つ。
- `struct usb_device` の `void *hid, *msc, *net` は、表の番号で引く `state[]` に換える。
- 文脈の決まり（`usb.h:5-11`）は今のままである。`attach`、`detach`、`work` は controller の worker で一度に一つ走り、眠ってよい。
- USB の装置の product の文字は、バスが読んで `hal_name` で渡す。今は `usbhid` と `usbnet` が別々に読む。

ほかのバス：

- VMBus の実装は `vm/hv/` の中の表で GUID を引く。
- 固定のポートの `vm/ata` は `vm_bind_fixed()` が探し、見つけたら `hal_add` と `hal_bound` を呼ぶ。

実装を足すのは、バスの表に一行足すことである。プロセスの実装は、サービスの宣言に行を足す。実装の表は、ドライバーの外で製品の名を並べてよい唯一の場所である。

### 起動の順

```text
pci_init()                      列挙。記録を作る。親と node を決める
iommu_init()                    全部の機能の群を読む。実装より先に走る
net_init() audio_init() usb_init()   部品の下ごしらえ。装置を探さない
pci_bind_all()                  表の順に attach を一つずつ呼ぶ
vm_bind_fixed()                 VM の固定のポートと VMBus
store_init()                    C: を探す
```

- `pci_bind_all` は同期である。`store_init` の前に終わる。
- 表の順は今の `init.c` の順に合わせる。待ちが一番長い `nvme` を最後に置く。
- `usb_enumerating` の数（`xhci.c:2335`）は xHCI の `attach` で足す。全部の `attach` が `store_init` の前に終わるので、`store.c:310` はそのまま正しい。
- USB の装置は、worker が抜き差しのたびに結ぶ。PCI は列挙の後に増えない。
- 消えるもの：`ahci_init`、`nvme_init`、`rtl8125_probe`、`vm_net_probe`、`vm_storage_init`、`vm_audio_init`、`vbe_display_init` と、各実装の `pci_count` の輪。
- 残るもの：`vm_init`。VM の判定と VMBus の起こしを持つ。

### 抜けたとき

- USB：`usb_remove`（`xhci.c:1713`）の順は今のままである。`removed` を立て、drain を止め、class の `detach` を呼び、`hal_remove` を呼び、slot を返す。`hc_restart` はこの全部を待ってから `hc_start` する。
- VMBus：`on_rescind` で `hal_remove` を呼ぶ。実装の後始末は今のままである。
- PCI：抜き差しも AER も今は無い。だから PCI の表に `lost` は無い。作る日に足す。実装が死んだと分かったら `hal_failed` を呼ぶ。
- 種類の物の決まりは変えない。`struct blkdev` は消えず、`gone` は戻りうる（`ahci.c:1167-1169`）。`struct screen` は消えない（`fb.h:59-60`）。
- `gone` と `removed` は速い道の今の確かめで、増やさない。

### 読む口

- プログラムは `n_hal_list` で台帳の写しを読む。`SYS_PCILIST` を置き換える。
- 変化は watch で知る。
- 種類の物は今の口で読む（`n_net_adapters`、`n_audio_outputs`、`n_screen_modes_get`、`N_INPUT_DEVICE`）。項目に `hal_id` を足す。
- 起動をまたぐ鍵は種類の一覧が今のまま持つ（音の `key`、入力の `id`、網の MAC、画面のモニターの識別子）。台帳は持たない。
- ディスクの一覧の口は今無い。読み手（設備の頁）を作るときに足す。

## 種類の表

| 種類 | 契約 | 実装 | 置き場 | データの道 | 関数の表 |
|---|---|---|---|---|---|
| CPU | `arch/cpu.h` | AMD、Intel | カーネル | 命令を直接 | `struct cpu_maker`（遅い操作だけ） |
| 割り込みと時計 | `arch/irq.h`、`sys/clock.h` | x2APIC、TSC | カーネル | 直接 | なし |
| バス | `dev/pci.h`、`dev/usb.h`、`vm/hv/vmbus.h` | PCI、xHCI、VMBus | カーネル | なし | 実装の表 |
| IOMMU | `dev/iommu.h` | AMD、Intel | カーネル | 表を直接 | `struct iommu_hw`（今のまま） |
| 電源 | `dev/pm.h` | ACPI | カーネル | なし | `struct pm_device`（今のまま） |
| ストレージ | `dev/blk.h` | `nvme`、`ahci`、`usbmsc`、`vm/ata`、`vm/hv/storvsc` | カーネル | `struct blkdev` の表を通る（今のまま） | `read`、`write`、`persist` |
| 網 | `net/net.h` | `rtl8125`、`usbnet`、`vm/e1000`、`vm/tulip`、`vm/hv/netvsc` | カーネル。スタックはサービス | 実装の呼出しの上にソフトの輪 | `struct netif` |
| 音 | `dev/audio.h` | `hda`、`vm/ac97` | カーネル | `struct audio_ring` | `struct audio_output_ops`（今のまま） |
| 入力 | `dev/input.h` | `usbhid`、`vm/hv/hv_input` | カーネル | 窓のサービスへの輪 | なし |
| 画面 | `dev/fb.h` | firmware の面、`vm/vbe`、`nvrm` | カーネルが面とフリップを持つ。実装はカーネルかプロセス | 緩衝を直接 | `struct screen_ops` |
| GPU | 次の設計で決める | `nvrm` と `nvk`、`vm/virtio_gpu` | 実機はプロセス。VM はカーネル | 実装が決める | `struct n_gpu_driver`（今のまま） |

温度の種類は、読む者ができたときに足す。今は作らない。

### 眠り

眠りの持ち主は `dev/pm.c` のままである。`struct pm_device` も今のままである。HAL の部品は眠りを持たない。理由は次のとおりである。

- 段の一覧は伸びるだけなので、CPU を止めた後に錠なしで辿れる（`pm.c:37-38`）。変わる木はこの歩き方を許さない。
- `resume` の `NULL` は「戻れないので眠らない」である（`pm.h:61`、`pm.c:250`）。実機の firmware の面はこの印で S3 を止めている（`fb.c:636`）。
- `refuse` は USB ディスクがある間の眠りを断る（`xhci.c:2311-2314`）。これが無いと C: を壊しうる。
- 装置でないものも登録する（`input.c:184`）。

足りないのはプロセスの実装の眠りである。`dev/pm.c` に段を一つ足す。

1. スレッドが動いている間（CPU を止める前）に、claim を持つプロセスへ型のある事象で「眠る」を知らせる。
2. 答えを期限つきで待つ。期限を過ぎたら眠りを取りやめる。
3. 戻るときは、全部のスレッドが動いた後に「戻った」を知らせる。

この段ができるまで、今の断り（`pm.c:351-354`）を残す。作るのは GPU の手術である。GPU のある実機の S3 はこの道で成り立つ。

### 網

NIC の実装はカーネルに残す。プロトコルを読むスタックだけを権限の無いサービスへ移す。

- 実装の口は `struct netif` を土台にする。`send`、`rx_pending`、`poll_rx`、`net_rx` を残し、申告（link、輪の数、checksum と TSO）を足す。
- だから新しい NIC の実装は、網の手術を待たずに今の口で書ける。
- カーネルの `net/netdev.c` がサービスとの輪を持つ。輪はソフトの輪で、普通の共有メモリーである。DMA の番地を持たない。
- 装置の記述子とレジスターはカーネルだけが書く。NIC の送信の末尾のレジスターは DMA の番地と同じ頁にあるので（`vm/e1000.c:31-35`）、サービスへ写さない。
- 送信の合図は syscall で、輪の中の複数のフレームを一度に渡す。受信はサービスのスレッドを起こす。
- `netdev.c` は輪から実装の緩衝へ写してから `send` を呼ぶ。写しの数は今の `rtl8125.c:505` と同じである。実装は共有メモリーを読まないので、読んだ後に値が変わる問題が無い。`rtl8125.c:507-511` の詰め物の判定も写しを読む。
- 実装は網の値を解かない。例外は装置の枠の形式で、NCM（`usbnet`）と RNDIS（`netvsc`）は実装が解く。
- 輪は adapter に複数持てる。数は申告で言う。今の実装は全部 1 である。
- adapter の番号は今のままである。同じ MAC が戻れば同じ番号である（`net.h:27-28`）。項目に `hal_id` を足す。
- 写しの費用が足りないと測れたら、輪の形を変える。

無線はこの形に入れない。空から来た管理の枠を実装が読むので、無線を作るときに置き場を決める。

### 画面

カーネルが `struct screen`、面、フリップ、垂直帰線を持つ。理由は、起動の画面と panic の画面をカーネルが描くからである。

- 画面は接続口の単位である。モニターの抜き差しでも、実装の終わりでも消えない（`fb.h:59-60`）。
- プロセスの実装のための口を、`dev/pciuser.c` から `dev/screen_driver.c` へ移す。権限は claim の handle のままである。プロセスの画面の実装は全部 PCI の機能で、claim の終わりに画面を返す処理（`pciuser.c:547`）がつながるからである。新しい handle の族は作らない。
- firmware の面は bootloader が渡す面で、バスの物ではない。ただし面は PCI の機能の BAR の中にある（`pciuser.c:166-176`）。画面 0 は `hal_id` でその機能を指す。
- プロセスの実装がその機能を claim すると、今のとおり `fb_hold` で書込みを止め、出力 0 を画面 0 に結ぶ（`pciuser.c:359`）。

### GPU

GPU の契約は、この文書で決めない。次の設計 `docs/gpu.md` で決める。契約は全部の GPU と窓のサービスの間の取り決めなので、実装の中の話では済まない。この文書から決まるのは次のことである。

- 実機の特権の半分はプロセスで動く。PCI の機能は claim で持つ。カーネルの実装は持たない。
- VM の `vm/virtio_gpu` と `vm/vbe` はカーネルの実装で、PCI の機能を `attach` で持つ。
- データの道は実装で違う。NVK は輪と doorbell を直接使う（`nvkmd_nocturne_rm.c:1354`）。virgl は `SYS_GPU_*` を通る。HAL は形を決めない。
- `struct n_gpu_driver` は今のままである。`log_*` と `scanout_*` には読み手がある（`nocturne_gl.c:128`、`:476-626`、`js_webgl.c:760`）。代わりの道が決まるまで外さない。
- サービスの宣言の `function_matches` は、宣言のデータに換える。`client_image` の行き先は GPU の設計で決める。

### CPU

CPU の処理を分ける。`arch/cpu.c` は CPUID の読出しと必須の表だけにする。

| ファイル | 持つもの |
|---|---|
| `arch/cpu.c` | CPUID、必須の表、任意の機能の表 |
| `arch/cpu_amd.c`、`arch/cpu_intel.c` | `struct cpu_maker` の実装：TSC の速さ、性能の要求、機械の検査の差、speculation の差 |
| `arch/mca.c` | 機械の検査の共通の部分 |

- VM は製造者ではない。AMD の上の VM は AMD の実装を使う。機械の検査と speculation の設定は VM でも走り、hypervisor が断った書込みは無いものとして扱う（`cpu.c:290`）。
- VM で違うのは二つだけである。TSC の速さの申告（`cpu.c:143`）と、CPU が隠す機能の扱い（`cpu.c:183`）である。この二つは `vm/vm.h` の関数で受ける。hypervisor の bit を読むのは `vm/` の中だけにする。
- 製造者を選ぶのは `cpu_features_init` で一度だけである。遅い操作だけが表を通る。TSC の読出し、EOI、IPI は直接呼ぶ。
- 必須の表の行には、それが無いと動かないコードがある。AVX-512 は任意へ移す。`arch/fpu.c` は CPU が持つ状態を有効にする。
- プログラムへの CPU の申告は、入口の像の後ろの読むだけのデータに置く。置き方は時計（`common/clock_data.h`）と同じである。最初の読み手と同時に作る。
- プログラムは申告を読んで開始時に一度だけ道を選ぶ。呼出しごとに確かめない。

## プロセスの実装

- サービスの管理者は `n_hal_list` を読み、`HAL_UNBOUND` の PCI の機能を宣言のデータと照らして起こす。行は `pci_match` と同じ形である。実例の引数は記録の住所である（`services.c:275`）。
- `pci_user_claim` が `hal_bound` を呼ぶ。名前はプログラムの名である。claim の終わりに `hal_unbound` を呼ぶ。だから動いている実装を二度起こさない。
- 導入の形式ができたら、宣言はドライバーの包みへ移る。包みで入るのは、今はプロセスの実装の口がある PCI の機能だけである。USB の interface をプロセスで動かす口は、要る日に足す。
- 装置が firmware を要るなら、実装はそれを読み込んで使う（`nvrm` の GSP がこの形である）。カーネルの実装が要る日も同じである。置き場は実装の持ち物にし、ほかの実装の道を塞がない。

## 落ちたとき

- プロセスの実装が落ちたら、サービスの管理者が起こし直す。宣言の `on_failure` は今もある。
- 既定の実装へ戻ることは約束しない。firmware の面へ戻しても、表示エンジンは別の状態にある（`dev/pciuser.c:296`）。
- 実装が一度も起きない機械では、firmware の面がそのまま画面になる。
- `nvrm` の起こし直しで画面が戻るかは、確かめていない。GPU の手術で、USB のログと写真で確かめる。

## 作らないもの

- 点数で名乗る仕組みと `probe` の関数。
- 全部のバスに共通の実装の表。身元と文脈がバスごとに違うからである。
- 実装を呼ぶ木と、木で決まる順序。台帳は親の番号だけを持つ。
- 眠りの二つ目の持ち主。
- 記録の参照の数。
- 種類をまたいだ万能の口（番号で振り分ける要求、ファイルに見せる装置）。
- カーネルの実装をプロセスで動かすための共通の代理。
- カーネルへ読み込むドライバー。
- 実装が一つの種類の関数の表。
- 読む者の無い種類と読む口。

## 費用

HAL の部品は速い道に入らない。

| 道 | 今 | この設計 |
|---|---|---|
| ストレージの要求 | `struct blkdev` の表を通る | 同じ |
| 音 | 輪 | 同じ |
| CPU の遅い操作 | 分岐 | 表を通る。速い道は通らない |
| 装置の結びつけ | 起動に一度。USB は抜き差しのたび | 同じ |
| 台帳の読出し | `SYS_PCILIST` | `n_hal_list` |

速い道の費用は、合成とスタックをカーネルの外へ移す決定から来る。HAL の費用ではないが、契約の形に関わるので書く。

| 道 | 増えるもの |
|---|---|
| 網 | 送受信で一つ多くプロセスを越える。合図で複数のフレームを渡す。写しの数は同じ |
| 入力 | カーネルのスレッドの代わりにプロセスを起こす。8000 Hz で報告ごとに起こすと毎秒 8000 回になる。まとめて起こす |
| 提示と窓の事象 | `SYS_WIN_PRESENT` と窓の事象がサービスとの通信になる |
| アドレス空間 | カーネルのスレッドは CR3 を替えない。サービスは替える |
| IBPB | `cpu_ibpb` に今は呼ぶ者が無い。直すとプロセスを越えるたびに費用が足される |
| 締切 | アプリの締切のスレッドは合わせて一 CPU の半分まで（`thread.c:198-233`）。280 Hz の画面 3 枚と入力で、1 本の予約の上限は 0.446 ms である（計算）。今のカーネルのスレッドは 3.348 ms である（計算） |
| フリップ | 帰線で上へ、フリップで下へ越える。`nvrm` の帰線とフリップの糸は今 fair 級である |
| 直接表示 | `scanout_*` の後継が要る。全画面の WebGL が使う |

減る費用もある。カーネルは SSE を使えない（`Makefile:35`）。サービスの合成は SIMD を使える。

今の性能は基準にしない。今の値は良い値ではない。見るのは、HAL が速さと流量を縛らないことである。

1. HAL の部品は速い道に入らない。入れば設計の誤りである。
2. 契約は装置の持つ速さを使い切れる形にする。`struct netif` の 1 フレームごとの呼出しが縛ると測れたら、まとめて渡す形へ換える。
3. 窓のサービスと入力のスレッドは、能力の handle で今のカーネルのスレッドと同じ締切の上限を持つ。`nvrm` の糸を同じ級にする。
4. `cpu_ibpb` を先に呼ぶようにする。
5. 落ちたコマの数え方と、入力から表示までの時刻の列を作る。今は無い（`docs/records/audit-compositor-20261102.md`）。
6. 測るのは実機で、結果は USB のログで読む。QEMU はフリップが即時で（`vbe.c:80-84`）、`rtl8125` が無く、WHPX は PCID を見せない。VM の値で決めない。

## この文書の外で決めること

| 次の設計 | 決めること |
|---|---|
| `docs/gpu.md` | 規則 5 のカーネルの GPU の層の中身。メモリー、同期、優先度、窓の合成との受け渡し。直接表示。複数の GPU（`vulkan.c:19`）。窓の WSI。`client_image` の行き先。firmware の置き場（`scripts/layout.py:28`） |
| 窓と合成の移動 | サービスの級。`fb_hold` と直接の緩衝（`shared_revoke` で取り上げる）。窓のサービスが落ちた間の入力の道（`init.c:56`）。`vm/rdp` の移動（`wm_remote_*`、`wm_clipboard_*` を読むので、窓の手術と一緒に移す。拡張セッションを止めない） |
| 網の移動 | ソケットの object。輪の項目の形。`vm/hv/hv_util.c:181` が読む住所の出どころ |
| 導入の形式 | 版の取り決め（`N_GPU_DRIVER_ABI`、`N_ABI_GENERATION`）。包みのプロセスの実装の宣言 |

## 三つの問いに対して

### 実装できるか

- できる：台帳、PCI の表、USB の表、CPU の分割、`nvme` や `hda` の `attach` への書き換え。`nvme.c` は `nvme_init` の輪を `attach` にして、表に一行足す程度である。
- 新しい NIC の実装：今の `struct netif` で書ける。
- 新しい GPU の実装：`docs/gpu.md` まで書けない。AMD は特権の半分を書く材料が、許された範囲に見つかっていない（`docs/open.md`）。
- 動かして確かめられる範囲：QEMU にある NVMe、AHCI、e1000、HD Audio、USB、VGA。USB の抜き差しは HMP の `device_del` で試せる。
- 動かして確かめられない範囲：S3 は WHPX で戻れない（`qemu-visual.md:68`）。`rtl8125` と `nvrm` は実機だけである。USB のログと写真で見る。
- virgl は QEMU の側に手で準備した DLL が要り（`ports/virgl/README.md`）、カーネルの門（`test_flag("gpu-enable")`）の後ろにある。GPU の契約を QEMU で動かせるかは確かめていない。

### 邪魔しないか

- 実装は自分のバスの型を受ける。呼び手と型を組みが教える。
- 関数の表は索引に出ない。だから実装が二つ以上の種類に限り、`.read = nvme_read` の形で `rg` で引けるようにする。
- プロセスの境目では呼出しの跡が切れる。実装を移すのは画面と GPU に限る。窓と網の移動で切れる跡は、時刻の列で補う。
- `hal.h` を読むのは、バスと、台帳を読む者だけである。`hal.h` を変えても全体の組み直しにならない。
- 手術が長くなるなら作業木で行い、ほかの仕事を止めない。

### 一か月後も正しいと思うか

約束はできない。分けて書く。

- 変わらないと思う：制御とデータの分離、種類ごとの型のある契約、バスごとの表、台帳が順序を持たないこと、眠りを `dev/pm.c` に残すこと。
- 間違いうる：USB の interface を記録にしないこと。USB の interface をプロセスで動かす日に変える。変えても台帳の項目が増えるだけである。
- 測るまで分からない：プロセスを越える費用。

## 試す順

1. 枝 `HAL-DEV` で、台帳、PCI の表、USB の表を作る。QEMU で NVMe、AHCI、e1000、HD Audio、USB のキーボードとタブレットとディスクの抜き差し、電源を切る道、休止を動かす。使い捨てにしない。受け入れられたら main へ merge する。
2. 計測の道具：落ちたコマの数え方と時刻の列を今のカーネルに作り、実機の値を USB のログで読む。
3. `docs/gpu.md` を書く。
4. 手術の計画：消す範囲、建てる形、閉じる段で見ることを見せる。
