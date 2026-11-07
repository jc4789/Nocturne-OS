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
| 実装 | `struct hal_driver`。規則の表と `attach`・`detach`・`suspend`・`resume`・`refuse`・`after_sleep` |
| 結びつけ | 規則の照合だけ。一番具体的な規則（項が多い）の実装から試す |
| バス | ほかの実装が子を `hal_publish` するだけ。PCI も xHCI も VMBus も ACPI も同じ |
| 資源 | `struct n_hal_resource`（メモリー、ポート、GSI）。割り込みと DMA は `hal_irq_enable`・`hal_dma_alloc` |
| 提供 | `hal_offer`。実装が装置の上で提供するもの（ディスク、NIC、音の出口、画面）を台帳に見せる |
| 依存 | `hal_depends`。木の外の依存（ACPI の `_DEP`）。止める順と戻す順に効く |
| 電源 | HAL の電源の walk（`hal/power.c`）。動いている装置を段ごとに、子と依存する側から止め、親と依存先から戻す。眠りでは止めた装置を D3 にする |
| 装置の電源の状態 | バスの方法（`struct hal_bus_ops` の `power`、PCI は PMCSR）と firmware の方法（`struct hal_firmware_ops`、ACPI は `_PSx` と電源資源）。実装は触らない |
| 機械の装置 | `struct hal_machine`（`HAL_MACHINE`）。核が firmware の表から直に使う装置（HPET、RTC、PCI のホスト、割り込みの link など）を持ち主の部品が宣言する。HAL は結ばず、claim させず、台帳には持ち主の名で `N_HAL_RUNNING` と見せる |
| firmware の節 | `hal_device` の `firmware`（中身の見えない `struct firmware_node`、今は ACPI の名前空間の節）。バスが書く。`hal_find_firmware`・`hal_parent_firmware` で引く |
| プロセスの実装 | サービスの管理者が装置ごとに `n_hal_claim` で持ち、claim の handle を実装のプロセスへ渡す。資源の写しと割り込みと DMA は同じ口で、firmware の記述は `n_hal_acpi_eval`、バスだけの操作は `n_hal_bus_call` |

## 足し方

- **実装を足す**：`struct hal_driver` を書き、`HAL_DRIVER(名前)` を置く。表は無い。
- **プロセスの実装を足す**：`user/apps/services.c` の宣言に規則とプログラムを書く。管理者が台帳を見張り、合う空いた装置ごとに claim を取って `N_SERVICE_HANDLE_DEVICE` で渡す。
- **バスを足す**：`common/hal_buses.h` に名前を足し、見つけた物を `hal_publish` する。`struct hal_bus_ops` を書く。
- **種類を足す**：種類の見出しと一覧を作り、実装から `hal_offer` する。HAL は種類を知らない。
- **VM だけの実装**：`HAL_DRIVER` を置かず、`vm/vm.c` が VM の中でだけ `hal_driver_add` で足す。

## 文脈

- `attach` は装置ごとの thread で走る。眠ってよく、秒の単位で待ってよい。別の装置を待たせない。
- `attach` の答えは三つ：0 は持った。`HAL_DECLINE` は持たず次の実装へ回す。負は持てなかった理由で `N_HAL_FAILED` になる。
- 0 以外なら HAL が割り込みを切り、DMA を止め、DMA のメモリーを返す。止まったと確かめられないメモリーは返さない。
- 動いた後に死んだら `hal_failed`。立ち上げ直して戻ったら `hal_recovered`。
- `hal_remove` は実装の `detach`、動き出した `attach` の thread、走っている claim の呼出しの終わりを待ってから戻る。戻った後、バスは記録を消してよい。出した者が先に子を消す。
- 実装の thread が装置に触る間は `hal_busy_begin`・`hal_busy_end`。電源の変化はこれを待ち、これは電源の変化の終わりを待つ。
- 装置は実装と claim に D0 で渡る（`attach` と `n_hal_claim` の前に firmware の D0 とバスの D0）。
- 実装が `attach` の後も探している間は `hal_looking_begin`・`hal_looking_end`。C: を探す `store_init` が `hal_settled` で見る。

## 電源

眠り（S3）、休止、電源断の装置の段は HAL の電源の walk が持つ。機械の順（`_TTS`、`_PTS`、CPU、写し）は `dev/pm.c` と `dev/hibernate.c` が持つ（ACPI 6.5 7.5、16.1.6、16.1.7）。

- **段。** 門を閉じた後、動いている装置（カーネルの実装か claim を持つ装置。機械の装置は除く）を、木の親と依存先で数えた深さの段に分ける。止めるのは深い段から、戻すのは浅い段から。同じ段の装置は互いに上でも依存先でもないので、仕事の列から助けの thread が並んで取る。秒の単位の手順（NVMe の shutdown など）が同じ段で足し算にならない。
- **実装の段。** `suspend(d, target)` と `resume(d)` は眠ってよい thread で走る。割り込みは開いていて、全部の CPU が動いている。自分の割り込みを待ってよい。ほかの thread は走り続けるので、実装は自分のデータの道を自分の錠で閉じる。`suspend` と `resume` の中から `hal_busy_begin`・`hal_remove`・`hal_publish` を呼ばない（門を待つ）。装置を載せ直す仕事は、門が開いた後に実装の thread が行う。
- **目標。** `HAL_POWER_SLEEP`（S3。止めた後に装置は D3 になり電源を失う）、`HAL_POWER_IMAGE`（休止の写し。電源は残る）、`HAL_POWER_OFF`（電源断、再起動、休止の写しへ渡す。`resume` は来ない。止まらない装置があっても進む）。
- **戻せるか。** `after_sleep` が宣言する。`LOST`（既定：結んでいる間、眠りも休止もしない）、`BOOT`（次の起動の firmware が戻す：休止だけ）、`RESUME`。開始メニューは `hal_power_cannot` で聞く。今だけ眠れない理由は `refuse` が言う。
- **装置の D 状態。** 眠りでは、止めた装置をバスの D3hot（PCI は構成を保存して PMCSR、PCIe 6.0 5.3.1、表 5-14）にし、firmware の D3（ACPI は `_PS3` と、参照の無くなった電源資源の `_OFF`）にする。電源が切れたらバスへ D3cold を知らせる。戻りは firmware の D0（資源の `_ON`、`_PS0`）、バスの D0（D3cold の後は link と準備の待ち、PCIe 6.0 6.6.1。構成の戻し）、そして `resume`。
- **プロセスの実装。** claim は同じ walk の中で `N_HAL_POWER_SUSPEND` と `N_HAL_POWER_RESUME` を受ける（ABI §9）。`n_hal_power_wait` を呼ばない claim を持つ装置がある間、眠りと休止は始まらない。
- **取りやめ。** 止まらない装置があれば、その段の後で walk を止め、止まった装置だけを戻す。眠りの取りやめでも `_TTS(0)` を、`_PTS` の後なら `_WAK` も呼ぶ。

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
