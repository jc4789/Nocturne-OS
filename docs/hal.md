# HAL の設計

状態：設計である。まだ作っていない。作り終えたら、今の形の説明へ書き直す。

規則は `.claude/rules/architecture.md` の HAL の節にある。ここは、その形を決める。

## 判定の物差し

この設計は、次の三つで判定する。

1. Claude が正しく実装できるか。
2. OS のほかの部分を手伝うのを、邪魔しないか。
3. 一か月後の Claude も正しいと思うか。

性能への影響は低く保つ。形の美しさは物差しにしない。

## 五つの決まり

1. **HAL は制御の道だけを持つ。** 設定、選択、開始、停止、失敗は HAL を通る。パケット、ブロック、音、GPU の命令は、HAL が作った輪とレジスターへ直接行く。
2. **種類ごとに置き場を決める。** 実装はカーネルの中に置く。例外は画面と GPU だけで、下の表に書く。
3. **関数の表は、実装が二つ以上ある種類だけが持つ。** 実装が一つなら、契約は見出しとファイルの境目だけで、呼出しは直接にする。
4. **契約の上のコードは、製造者、機種、世代を知らない。** 違いは実装の申告で受ける。
5. **カーネルへコードを読み込まない。** 実装はカーネルに組み込むか、プロセスで動かす。

## 部品

HAL の共通の部分は小さい。`kernel/src/dev/hal.h` と `hal.c` だけで、見込みは 300 行である。

### 装置の記録

機械にある物は、全部一つの木に載る。ディスクは USB メモリーの子で、USB メモリーは xHCI の子で、xHCI は PCI の機能の子である。

```c
enum hal_kind { HAL_BUS, HAL_CPU, HAL_GPU, HAL_SCREEN, HAL_AUDIO, HAL_STORAGE, HAL_NETWORK, HAL_INPUT, HAL_OTHER };
enum hal_state { HAL_UNBOUND, HAL_RUNNING, HAL_QUIESCED, HAL_LOST };

struct hal_device {
    uint32_t id;                     /* 起動の間、二度と使わない番号 */
    enum hal_kind kind;
    enum hal_state state;
    struct hal_device *parent;
    struct hal_identity identity;    /* バスが埋める */
    const struct hal_driver *driver; /* NULL：カーネルの実装が持っていない */
    void *state_of_driver;           /* 読むのは attach した実装だけ */
    char name[32];
    /* 親子のつながり、参照の数 */
};
```

`void *` を許すのは `state_of_driver` だけである。今の `struct usb_device` の `void *hub, *hid, *msc, *net` は、一つの装置が四つの役を持つ形なので無くなる。USB の interface は子の記録になる。

記録は、名前と利用者に見せる形を持たない。それはサービスが持つ。

### 実装の宣言

```c
struct hal_identity { enum hal_bus bus; uint16_t vendor, device; uint8_t cls, subcls, progif; };

struct hal_driver {
    const char *name;
    const struct hal_identity *matches;  /* 機種の行が先、種類の行が後 */
    size_t match_count;
    enum hal_stage stage;                /* 眠りの順。今の enum pm_stage */
    int  (*attach)(struct hal_device *d);
    void (*lost)(struct hal_device *d);
    bool (*quiesce)(struct hal_device *d);
    bool (*resume)(struct hal_device *d);
};
```

実装の表は `kernel/src/dev/hal_drivers.c` に一つだけ置く。実装を足すのは、`struct hal_driver` を一つ書いて、この表に一行足すことである。

VM の実装は `vm_hal_drivers()` が返す。実機では空である。共通コードは VM の実装の名を知らない。

### 三つの操作

- `hal_add(parent, &identity)`：バスが記録を作り、実装を引いて `attach` を呼ぶ。
- `hal_lost(d)`：どの文脈からでも呼べる。子から順に `lost` を呼ぶ仕事を積む。記録は参照が無くなるまで残る。
- `hal_each(kind, ...)`：種類ごとに記録を辿る。

実装の引き方は一つである。表を順に見て、機種の行が合うものを先に、種類の行が合うものを後に取る。点数も `probe` の関数も持たない。

### 寿命

- 全部の実装が `attach`、`lost`、`quiesce`、`resume` を持つ。要らないものは `NULL` にする。
- 眠りの順は木と `stage` で決まる。今の `struct pm_device` は `struct hal_driver` に入り、消える。
- `lost` は速い道に確かめを足さない。今ある失敗の口で返す。`struct blkdev` の `gone` がこの形である。

### 読む口

- プログラムは `n_hal_list(kind, ...)` で番号を得て、種類ごとの型のある関数（`n_hal_gpu`、`n_hal_storage` など）で申告を読む。
- 変化は今ある watch で知る。
- 申告は起動のたびの刻印（`common/abi.h`）で守る。版の取り決めは作らない。全部を一緒に組むからである。
- CPU の申告は、入口の像の後ろの読むだけのデータに置く。置き方は時計（`common/clock_data.h`）と同じである。syscall も `cpuid` も要らない。

## 種類の表

| 種類 | 契約 | 実装 | 置き場 | データの道 | 関数の表 |
|---|---|---|---|---|---|
| CPU | `arch/cpu.h` | AMD、Intel、VM | カーネル | 命令を直接 | `struct cpu_maker`（遅い操作だけ） |
| 割り込みと時計 | `arch/irq.h`、`sys/clock.h` | x2APIC、TSC | カーネル | 直接 | なし |
| バス | `dev/pci.h`、`dev/usb.h`、VMBus | PCI、xHCI、VMBus | カーネル | なし | なし |
| IOMMU | `dev/iommu.h` | AMD、Intel | カーネル | 表を直接 | `struct iommu_hw`（今のまま） |
| ストレージ | `dev/blk.h` | `nvme`、`ahci`、`usbmsc`、`vm/ata`、`vm/hv/storvsc` | カーネル | 要求ごとに一回の間接呼出し | `read`、`write`、`persist`（今のまま） |
| 網 | 新しい輪の見出し | `rtl8125`、`usbnet`、`vm/e1000`、`vm/tulip`、`vm/hv/netvsc` | カーネル。スタックはサービス | 共有の輪をまとめて渡す | 開始、停止、送信の合図 |
| 音 | `dev/audio.h` | `hda`、`vm/ac97` | カーネル | `struct audio_ring` | `struct audio_output_ops`（今のまま） |
| 入力 | `dev/input.h` | `usbhid`、`vm/hv/hv_input`、`vm/rdp` | カーネル | 窓のサービスへの輪 | なし（読んで確かめる） |
| 画面 | `dev/fb.h` | firmware の面、`vm/vbe`、プロセス（`nvrm`） | カーネルが面とフリップを持つ。実装はカーネルかプロセス | 緩衝を直接 | `struct screen_ops` |
| GPU | 記録と像の表（`common/gpu_abi.h`） | `nvrm` と `nvk`、virgl | 像はプログラムの中。特権の半分の置き場は実装が決める | プログラムから GPU へ直接 | `struct n_gpu_driver` |

温度の種類は、読む者ができたときに足す。今は作らない。

### 網

NIC の実装はカーネルに残す。読むのは装置の記述子で、網から来た値ではない。プロトコルを読むスタックだけを権限の無いサービスへ移す。だから `usbnet` と `netvsc` のようなバスの違う実装も置き場が揃う。

今の `struct netif` は、`send` と `poll_rx` の受け手が 1 フレームごとの間接呼出しである。これを輪に換える。

無線はこの形に入れない。空から来た管理の枠を実装が読むので、無線を作るときに置き場を決める。

カーネルの NIC の実装は今 IOMMU で絞られていない（`rtl8125.c` に `iommu_` の呼出しが無い）。網の手術で絞る。

### 画面

画面だけは、カーネルの実装とプロセスの実装の両方を受ける。理由は、起動の画面と panic の画面をカーネルが描くからである。

カーネルが `struct screen`、面、フリップ、垂直帰線を持つ。プロセスの実装のための口は `SYS_SCREEN_*` に分ける。今は `pci_user_*screen*` として PCI の handle に載っている（`dev/pciuser.h`）。

### GPU

GPU の契約は二つだけである。

- **記録：** `HAL_GPU` の記録が名前、メモリーの量、できること、像の名を申告する。`nvrm` は `n_hal_publish` で作り、virgl は `vm/` の中で `hal_add` で作る。
- **像の表：** プログラムの Vulkan ローダーは、記録の一覧から像を選ぶ。今のサービスの宣言の `client_image` は無くなる。

特権の半分と像の間の話は、実装の中の話である。`nvrm` の RM の channel も、virgl の `SYS_GPU_*` も、契約に入らない。

`struct n_gpu_driver` の `log_*` と `scanout_*` は外す。Vulkan の入口と `ready` を残す。

メモリー、同期、優先度、窓の合成との受け渡しは、GPU と画面の手術で決める。ここでは決めない。

### CPU

CPU の処理を分ける。`arch/cpu.c` は CPUID の読出しと必須の表だけにする。

| ファイル | 持つもの |
|---|---|
| `arch/cpu.c` | CPUID、必須の表、任意の機能の表 |
| `arch/cpu_amd.c`、`arch/cpu_intel.c` | `struct cpu_maker` の実装：TSC の速さ、性能の要求、機械の検査の差、speculation の差 |
| `arch/mca.c` | 機械の検査の共通の部分 |
| `vm/vm_cpu.c` | VM の `struct cpu_maker`。共通コードの `f->hypervisor` を無くす |

選ぶのは `cpu_features_init` で一度だけである。遅い操作だけが表を通る。TSC の読出し、EOI、IPI は直接呼ぶ。

必須の表の行には、それが無いと動かないコードがある。AVX-512 は任意へ移す。`arch/fpu.c` は CPU が持つ状態を有効にする。

プログラムは、申告を読んで開始時に一度だけ道を選ぶ。呼出しごとに確かめない。

## 実装の結びつけ

- PCI、xHCI、VMBus が見つけた物に `hal_add` を呼ぶ。
- 今の `init.c` の `ahci_init`、`nvme_init` などの列と、各実装の `pci_count` の輪は無くなる。
- 今の `vm_net_probe`、`vm_storage_init`、`vm_audio_init` は無くなる。
- カーネルの実装が持たない PCI の機能は、`HAL_UNBOUND` のまま残る。
- プロセスの実装は、サービスの管理者が `HAL_UNBOUND` の記録を宣言の表と照らして起こす。今の `services.c` の `function_matches` の関数は、宣言のデータに換わる。導入の形式ができたら、宣言はドライバーの包みへ移る。

GPU の PCI の機能はカーネルの実装が持たないので、一つの装置を二つの実装が争う場面は無い。firmware の面は PCI の機能を持たない。画面の記録を `nvrm` が引き継ぐだけである。

## 落ちたとき

- プロセスの実装が落ちたら、サービスの管理者が起こし直す。宣言の `on_failure` は今もある。
- 既定の実装へ戻ることは約束しない。firmware の面へ戻しても、表示エンジンは別の状態にある（`dev/pciuser.c:296`）。
- 実装が一度も起きない機械では、firmware の面がそのまま画面になる。
- `nvrm` の起こし直しで画面が戻るかは、確かめていない。GPU の手術で、USB のログと写真で確かめる。

## 作らないもの

- 点数で名乗る仕組み。
- 種類をまたいだ万能の口（番号で振り分ける要求、ファイルに見せる装置）。
- カーネルの実装をプロセスで動かすための共通の代理。
- 版の取り決め。
- カーネルへ読み込むドライバー。
- 実装が一つの種類の関数の表。
- 読む者の無い種類。

## 費用

| 道 | 今 | この設計 |
|---|---|---|
| ストレージの要求 | 一回の間接呼出し | 同じ |
| 音 | 輪 | 同じ |
| CPU の機能 | 我々のコードは選ばない | 開始時に一度選ぶ |
| 装置の結びつけ | 起動に一度 | 起動に一度、抜き差しのたびに一度 |
| 網のフレーム | 1 フレームごとの間接呼出し。スタックはカーネル | まとめて渡す。スタックとの間でプロセスを越える |
| フリップ | 合成はカーネル、`nvrm` へ 1 フレームに一度越える | 合成はサービス。越える回数は GPU の手術で決める |

速い道で増えるのは、網と合成だけである。どちらも HAL でなく、スタックと合成をカーネルの外へ移す決定から来る。

切る前に、今の媒体で三つを測る。

1. 割り込みからプロセスのスレッドが起きるまで。
2. フリップの鎖。
3. 網の速さと往復の時間。

測って間に合わなければ、その道の形を変える。

## 三つの問いに対して

### 実装できるか

- できる：記録、宣言の表、CPU の分割、`nvme` や `hda` の `attach` への書き換え。
- 難しい：抜き差しと `lost` の競合。状態の書き手を `hal.c` の一つにする。
- 動かして確かめられる範囲：QEMU にある NVMe、AHCI、e1000、HD Audio、USB、VGA。
- 実機だけの範囲：`rtl8125` と `nvrm`。USB のログと写真で見る。
- virgl は QEMU の側に手で準備した DLL が要り（`ports/virgl/README.md`）、カーネルの門（`test_flag("gpu-enable")`）の後ろにある。GPU の契約を QEMU で動かせるかは確かめていない。

### 邪魔しないか

- 関数の表は索引に出ない。だから実装が二つ以上の種類に限り、`.read = nvme_read` の形で `rg` で引けるようにする。
- プロセスの境目では呼出しの跡が切れる。だから画面と GPU に限る。
- `hal.h` を読むのは、実装と木を辿る者だけにする。呼ぶ側は種類の見出しだけを読む。`hal.h` を変えても全体の組み直しにしないためである。
- 手術の間に main が組めないと、ほかの仕事を確かめられない。HAL の手術は作業木で行う。

### 一か月後も正しいと思うか

約束はできない。分けて書く。

- 変わらないと思う：制御とデータの分離、種類ごとの型のある契約、一本の木。
- 間違いうる：GPU の契約の細部。AMD と Intel を見ていない。外れても、記録と像の表の外は実装の中の話なので、書き直しが広がらない。
- 測るまで分からない：プロセスを越える費用。

## 試す順

1. 読む試験：記憶の無いサブエージェントに、この文書とコードだけを渡す。結果は `docs/records/` に残す。
2. 試作：作業木で、画面の種類を firmware の面と `vm/vbe` で作る。QEMU で動かす。悪ければ枝ごと捨て、この文書を直す。
3. 手術の計画：消す範囲、建てる形、閉じる段で見ることを見せる。
