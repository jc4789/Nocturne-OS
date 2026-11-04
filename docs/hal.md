# HAL

HAL は、機械の全部の装置を一つの木に持ち、実装に結びつける。口は `kernel/src/hal/hal.h`、ABI は `common/hal_abi.h`、知られたバスの名前は `common/hal_buses.h` にある。

## 二つの約束

1. **HAL は変わらない。** 新しい装置、新しい種類、新しいバスのどれを足しても `kernel/src/hal/` と `common/hal_abi.h` は変わらない。
2. **HAL は速さを縛らない。** HAL の部品はデータの道に入らない。実装は `attach` で番地と割り込みと DMA の番地を受け取り、その後は装置へ直接話す。

## 形

| 物 | 形 |
|---|---|
| 装置 | `struct hal_device`。バスの物に埋める。木の一つの節 |
| 身元 | 性質の並び（32 bit の鍵と、数かバイト）。HAL は意味を知らない |
| 実装 | `struct hal_driver`。規則の表と `attach`・`detach`・`quiesce`・`resume`・`refuse` |
| 結びつけ | 規則の照合だけ。一番具体的な規則（項が多い）の実装から試す |
| バス | ほかの実装が子を `hal_publish` するだけ。PCI も xHCI も VMBus も ACPI も同じ |
| 資源 | `struct n_hal_resource`（メモリー、ポート、GSI）。割り込みと DMA は `hal_irq_enable`・`hal_dma_alloc` |
| 提供 | `hal_offer`。実装が装置の上で提供するもの（ディスク、NIC、音の出口、画面）を台帳に見せる |
| 依存 | `hal_depends`。木の外の依存（ACPI の `_DEP`）。止める順と戻す順に効く |
| 眠り | 木の順。子と依存する側が先に止まり、親と依存先が先に戻る |
| プロセスの実装 | 同じ装置を `n_hal_claim` で持つ。資源の写しと割り込みと DMA は同じ口で、バスだけの操作は `n_hal_bus_call` |

## 足し方

- **実装を足す**：`struct hal_driver` を書き、`HAL_DRIVER(名前)` を置く。表は無い。
- **プロセスの実装を足す**：`user/apps/services.c` の宣言に規則を書く。
- **バスを足す**：`common/hal_buses.h` に名前を足し、見つけた物を `hal_publish` する。`struct hal_bus_ops` を書く。
- **種類を足す**：種類の見出しと一覧を作り、実装から `hal_offer` する。HAL は種類を知らない。
- **VM だけの実装**：`HAL_DRIVER` を置かず、`vm/vm.c` が VM の中でだけ `hal_driver_add` で足す。

## 文脈

- `attach` は装置ごとの thread で走る。眠ってよく、秒の単位で待ってよい。別の装置を待たせない。
- `attach` の答えは三つ：0 は持った。`HAL_DECLINE` は持たず次の実装へ回す。負は持てなかった理由で `N_HAL_FAILED` になる。
- 0 以外なら HAL が割り込みを切り、DMA を止め、DMA のメモリーを返す。止まったと確かめられないメモリーは返さない。
- 動いた後に死んだら `hal_failed`。立ち上げ直して戻ったら `hal_recovered`。
- `hal_remove` は実装の `detach`、動き出した `attach` の thread、走っている claim の呼出しの終わりを待ってから戻る。戻った後、バスは記録を消してよい。出した者が先に子を消す。
- 実装の thread が装置に触る間は `hal_busy_begin`・`hal_busy_end`。電源の変化はこれを待つ。
- 実装が `attach` の後も探している間は `hal_looking_begin`・`hal_looking_end`。C: を探す `store_init` が `hal_settled` で見る。

## データの道

種類の契約が持つ。HAL を通らない。

| 種類 | 契約 | 道 |
|---|---|---|
| ストレージ | `dev/blk.h` の `struct blk_ops` | 要求の鎖を一度に渡し、完了は割り込みから。多重の queue は実装の持ち物 |
| 網 | `net/net.h` の `struct netif` | 複数のフレームを一度に渡す。queue の数と offload を申告する |
| 音 | `dev/audio.h` | DMA の輪を直接 |
| 入力 | `dev/input.h` | 実装から sink へ直接 |
| 画面 | `dev/fb.h` | 緩衝とフリップを直接 |
| GPU | `gpu_abi.h` | 輪と doorbell を直接 |

## 台帳を読む

`n_hal_snapshot` が全部の記録を一つの時点で写す。記録は可変の長さで、`n_hal_device_next` で次へ進む。変化は `n_hal_watch` で知る。読み手は設定の窓の「装置」の頁と、サービスの管理者である。設定の窓は、名も級も提供も無く失敗もしていない節（firmware の記述やバスの内部の節）を行にしない。

## 載らないもの

CPU は `arch/topology.c`、IOMMU は `dev/iommu.c`、割り込みと時計は `arch/` が持つ。一つの物は一つの台帳に載る。ACPI の `ACPI0007`（CPU）は載せない。
