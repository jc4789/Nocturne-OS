# HAL の建て直し：サブエージェントの依頼文と報告

依頼文と報告の全文。モデルは Sonnet 5.5 が 3 体。報告の主張は、その後の全体ビルドと QEMU の確かめの前のものである。

## 依頼文

### PCI のバスとストレージをつなぐ

リポジトリ D:\Programes\llm vibe slop os（Nocturne OS。main で直接働く。枝も作業木も使わない。他の二人の Sonnet と私が同じ木で同時に別のファイルを書いている）。コミットしない。ファイルは自分の担当だけを書く。

読むもの：`AGENTS.md`、`.claude/rules/kernel.md`・`code.md`・`architecture.md`、`docs/hal.md`、`kernel/src/hal/hal.h`、`common/hal_abi.h`、`common/hal_buses.h`、`kernel/src/dev/blk.h`。古い HAL は `git show c9ff99c5:kernel/src/dev/hal.h` など git にあるが、形の手本にしない。今あるコードは正しいと示されるまでゴミの山として扱う。悪いコードは修理せず、消して正しい実装で置き換える。差分の小ささは良さの尺度ではない。機能と対応の装置は失わない。

担当のファイル（ここだけ書く）：
`kernel/src/dev/pci.c`、`kernel/src/dev/pci.h`、`kernel/src/dev/nvme.c`、`kernel/src/dev/ahci.c`、`kernel/src/dev/blk.c`、`kernel/src/dev/blk.h`、`kernel/src/dev/iommu*.c`・`iommu.h`（`hal_bound(&fn->hal,"iommu")` の行など）、`kernel/src/vm/ata.c`、`kernel/src/vm/hv/storvsc.c`、`kernel/src/fs/` の blk を呼ぶ所（`cache.c`、`store.c`）、`kernel/src/dev/hibernate.c` の blk の呼出し。

作るもの：
1. **PCI をバスにする。** `struct pci_device` に `struct hal_device hal` を埋める。`pci_init` が列挙した機能を全部 `hal_publish` する（親は上流のブリッジ、無ければ NULL）。性質は `common/hal_buses.h` の `N_PROP_PCI_*`、資源は BAR ごとに slot を BAR の番号として `hal_resource_add`（メモリーは prefetchable の旗つき、I/O はポート）、`node` は今の node、location は `%04x:%02x:%02x.%x`。`static inline struct pci_device *pci_device_of(struct hal_device *d)` を `pci.h` に置く（ASSERT で bus を確かめる）。`struct hal_bus_ops` を実装する：`irq_enable` は MSI-X、MSI、INTx（`acpi_pci_gsi` と `irq_request_gsi`、INTx disable の旗の扱いも）の順で一つの道にし、ドライバーの INTx の道（`hda.c`、`e1000.c`、`tulip.c`、`ac97.c` にある）が消せるようにする。`irq_disable`、`irq_max`、`enable`（`pci_enable_busmaster`・`pci_enable_io` の役目）、`stop`（BME を切って確かめる）。`.claim` は別の担当者が `kernel/src/dev/pciuser.c` に `pci_claim_ops` を作るので、`extern const struct hal_claim_ops pci_claim_ops;` を参照して入れる。`kernel_owned` と `iommu_guest` は `enable` の中で今の意味を保つ。
2. **ストレージの契約を実装する。** `blk.h` の新しい形（`struct blk_ops`、`struct blk_request`、`blk_submit`、`blk_serial_ops`）は私が書いた。足りなければ `blk.h` を直してよい（別の担当者の `usbmsc.c` が `blk_serial_ops` を使う）。`blk.c`：`blk_read`・`blk_write`・`blk_persist` は同じ名前と意味で `blk_submit` の上に作る（27 か所の呼び手が変わらない）。部分ブロックの書込みと分割は `blk.c` が持つ（`max_sectors`、`max_segments`、`segment_align`、`block`）。`blk_hold` の門は完了までの数を数え、holder は通す。`blk_serial_ops` は装置ごとの thread で一つずつ `transfer` を呼ぶ。`blk_register` は装置に `hal_offer(hal, N_OFFER_DISK, index, name)` するため、`struct hal_device *` を受けるようにする。
3. **`nvme.c` を要求の鎖の形に書き直す。** 上の `submit` は呼んだ CPU の queue へ鎖の全部を入れ、doorbell を最後に一度だけ鳴らす。完了は割り込みから `done`。PRP と SGL は `blk_segment` から作る。眠りの間、休止の書込みの間（`blk_holder`、polling の道）も今の機能を保つ。`first_start` の thread、`blk_looking`、`blk_busy` は消す：`attach` は装置ごとの thread で走るので、`start` を直接呼ぶ。全部の controller を一つの `nvme_pm` で持つ形も消し、`struct hal_driver` の `quiesce`・`resume`・`detach` は装置ごとにする。資源は `hal_map`、`hal_irq_enable`、`hal_dma_alloc`。名前は `hal_name`、失敗は `hal_failed`、戻りは `hal_recovered`。`HAL_DRIVER(nvme_driver)` と規則 `N_PCI_CLASS_PROGIF(1, 8, 2)`。
4. **`ahci.c` を同じ形に。** NCQ があれば鎖で一度に発行する。ポートごとの thread は残してよいが、ポートの探索の待ちは `hal_looking_begin/end`、装置に触る仕事は `hal_busy_begin/end`。
5. **`vm/ata.c` と `vm/hv/storvsc.c`。** 一つずつの装置は `blk_serial_ops`。storvsc は VMBus の実装で、`hal_driver` の `attach` が `struct hal_device *` から channel を受ける形は担当者 2（`vm/hv/vmbus.h`）が決める。`N_VMBUS_TYPE(guid)` の規則で。ata は `N_FIXED("ide-ports")` の規則で、`vm/vm.c` が `hal_driver_add` し、固定のポートの節を `hal_publish` する。
6. **block cache の呼出しを鎖にする。** `fs/cache.c` の書き戻しと先読みが一度に複数の要求を持つなら `blk_submit` で鎖にして渡す。`store.c` の `blk_looking()` は `!hal_settled()`（私が直すかもしれないので、すでに直っていれば触らない）。
7. `pci.c` の中の `pci_bind_*` の残り、`hal_bound` などの古い呼出しは消す。古い名の `rg` が 0 になるようにする：`PCI_NOT_MINE`、`pci_driver`、`pci_bind`、`blk_looking`、`blk_busy`、`blk_by_blocks`、`pm_register`、`pm_device`、`hal_bound`、`hal_restarted`、`hal_unbound`（担当のファイルの中で）。

組みの確かめ：`python -X utf8 scripts/build.py -j 8 kernel` は他の担当者のファイルで落ちるのが普通である。自分のファイルの誤りだけを読む。`make` の目標を一つのオブジェクトへ絞ってもよい（Makefile を読む）。QEMU は私が最後に一度動かすので、起動しなくてよい。全部の変更に試験は要らない。

報告は日本語で：消したもの、建てたもの、足した行と消した行、他の担当者へ要る口、確かめていないこと。

### USB と網と音と VM をつなぐ

リポジトリ D:\Programes\llm vibe slop os（Nocturne OS。main で直接働く。枝も作業木も使わない。他の二人の Sonnet と私が同じ木で同時に別のファイルを書いている）。コミットしない。ファイルは自分の担当だけを書く。

読むもの：`AGENTS.md`、`.claude/rules/kernel.md`・`code.md`・`architecture.md`、`docs/hal.md`、`kernel/src/hal/hal.h`、`common/hal_abi.h`、`common/hal_buses.h`、`kernel/src/dev/blk.h`。今あるコードは正しいと示されるまでゴミの山として扱う。悪いコードは修理せず、消して正しい実装で置き換える。差分の小ささは良さの尺度ではない。機能と対応の装置は失わない。

担当のファイル（ここだけ書く）：
`kernel/src/dev/xhci.c`、`usb.h`、`usbhub.c`、`usbhid.c`、`usbmsc.c`、`usbnet.c`、`kernel/src/dev/hda.c`、`hda_codec.*`、`kernel/src/net/net.h`、`net.c`（`struct netif` を呼ぶ所）、`rtl8125.c`、`kernel/src/vm/vm.c`、`vm.h`、`e1000.c`、`tulip.c`、`ac97.c`、`vbe.c`、`virtio_gpu.c`、`rdp.c`（要るなら）、`kernel/src/vm/hv/vmbus.c`、`vmbus.h`、`hv_input.c`、`hv_util.c`、`netvsc.c`、`hvsock.c`。PCI のバスは別の担当者が `pci.c`・`pci.h` で作っている：`struct pci_device` に `struct hal_device hal`、`pci_device_of(struct hal_device *)`、資源は BAR の番号を slot に、`hal_irq_enable` は MSI-X・MSI・INTx の順の一つの道、`hal_enable(d, HAL_ENABLE_*)`。ストレージも別の担当者で、`blk_register` は `struct hal_device *` を受ける形になる。

作るもの：
1. **USB を HAL のバスにする。** `struct usb_device` に `struct hal_device hal`（`N_BUS_USB`、親は hub の装置か xHCI の PCI 機能）。USB の interface を子の節にする（`N_BUS_USB_INTERFACE`、性質 `N_PROP_USB_CLASS`・`N_PROP_USB_INTERFACE`、親の `N_PROP_USB_VENDOR/PRODUCT` も載せる）。class の実装（`usbhid`、`usbmsc`、`usbnet`）は `struct hal_driver` で `HAL_DRIVER`、規則は `N_USB_INTERFACE_CLASS` など。attach は interface の節を受ける。`usb_drivers` の表、`USB_DRIVER_*`、`state[]`、`struct usb_driver`、`USB_NOT_MINE`、`h->sibling` の鎖は消す。hub も `HAL_DRIVER` の実装にする。設定の選択は `hal_driver_exists` で聞く（xHCI が class の名を知らない）。CDC は `usbnet` が comm の interface で attach し、data の interface も自分のものにする（data の節には何も結ばないよう、規則を comm だけに）。`work` の呼出しは USB のバスの口にする（`usb_note_work`、実装が `usb_work_set(d, fn)` などで登録）。`xhci_refuse` は `hal_driver.refuse` にする。xHCI の探索は `hal_looking_begin/end`、xHCI の worker が装置に触る仕事は `hal_busy_begin/end`。`hal_remove` を使って抜けた装置を片づける（`release_slot` の寿命と合わせる）。`usb_control` は `hal_gone` で止まる。controller ごとの `struct hal_driver` の quiesce・resume・detach（全部の controller を一つの `xhci_pm` で持つ形は消す）。`hal_map`、`hal_irq_enable`、`hal_name`、`hal_failed`、`hal_recovered`。location は `USB 2-1.3` の形。
2. **`usbmsc.c`** は `blk_serial_ops`（`dev/blk.h`）で、一つずつの bulk。名前は `hal_name`。
3. **`struct netif` を速さを縛らない形に作り直す。** `net/net.h` に：送信は `unsigned (*transmit)(struct netif *self, unsigned queue, const struct net_tx_frame *frames, unsigned count)`（受け取った数を返す。`struct net_tx_frame` は部分の並びと offload の要求）、受信は `size_t (*poll)(struct netif *self, unsigned queue, size_t budget, net_input_fn input)`、`bool (*rx_pending)(struct netif *self, unsigned queue)`、申告は `tx_queues`、`rx_queues`、`offloads`（`NET_OFFLOAD_CSUM_TX`、`CSUM_RX`、`TSO` など）。`net.c` は申告を読み、一度の流れで作るフレームがあれば鎖にして渡し、受信は budget で回す。1 フレーム用の古い `send`・`poll_rx` は消した（`net.h` の残りを直す）。net の thread が読む順と `net_lock` の決まりは保つ。実装は `rtl8125`（可能なら複数の輪は持たず 1 と申告、ハードの持つ checksum offload を申告）、`e1000`、`tulip`、`netvsc`、`usbnet`。`net_register` は `struct hal_device *` を受け、`hal_offer(N_OFFER_NET, ...)`。
4. **音。** `hda.c` と `ac97.c` は `struct hal_driver`。資源は `hal_map`・`hal_port`、割り込みは `hal_irq_enable`（INTx の分岐を消す）、DMA は `hal_dma_alloc`。出口ごとに `hal_offer(N_OFFER_AUDIO, ...)`。`HAL_DECLINE` は読み手がいるときだけ。`iommu_guest` は今の意味を保つ。
5. **VM。** `vm/devices.h` は消した。`vm/vm.c` が VM の中でだけ `hal_driver_add` で `e1000`、`tulip`、`ac97`、`vbe`、`virtio_gpu`、`ata`、`storvsc`、`netvsc`、`hv_input`、`hv_util` を足す（`extern const struct hal_driver ...` は `vm.c` の中）。VMBus の channel を節にする（`N_BUS_VMBUS`、`N_PROP_VMBUS_TYPE` に GUID、RESCIND で `hal_remove`）。`vmbus.h` に `struct vmbus_channel *vmbus_channel_of(struct hal_device *)`。これを別の担当者の `storvsc.c` も使う：先に `vmbus.h` を書く。`vm_init` の明示の `hv_input_init` などは消し、規則で結ぶ。固定のポートは `vm/vm.c` が IDE の節（`N_BUS_FIXED`、`N_PROP_FIXED_NAME` = `"ide-ports"`、ポートの資源）を `hal_publish` する。`vbe.c`：`fb_boot_release()`（`dev/fb.h`、私が書く）を attach で呼ぶ。そうすると firmware の画面の節が外れ、`vbe` の `resume` が画面を戻す。`vbe_display_pm` は消す。
6. 古い名の `rg` が担当のファイルで 0 になるようにする：`pci_driver`、`PCI_NOT_MINE`、`usb_driver`、`usb_drivers`、`USB_DRIVER`、`USB_NOT_MINE`、`pm_register`、`pm_device`、`hal_bound`、`hal_restarted`、`hal_unbound`、`vm_pci_drivers`、`vm_bind_fixed`、`n_pci_match`。

組みの確かめ：`python -X utf8 scripts/build.py -j 8 kernel` は他の担当者のファイルで落ちるのが普通である。自分のファイルの誤りだけを読む。QEMU は私が最後に一度動かすので、起動しなくてよい。全部の変更に試験は要らない。

報告は日本語で：消したもの、建てたもの、足した行と消した行、他の担当者へ要る口、確かめていないこと。

### プロセスのドライバーと ABI をつなぐ

リポジトリ D:\Programes\llm vibe slop os（Nocturne OS。main で直接働く。枝も作業木も使わない。他の二人の Sonnet と私が同じ木で同時に別のファイルを書いている）。コミットしない。ファイルは自分の担当だけを書く。

読むもの：`AGENTS.md`、`.claude/rules/kernel.md`・`code.md`・`architecture.md`、`docs/hal.md`、`kernel/src/hal/hal.h`、`common/hal_abi.h`、`common/hal_buses.h`、`kernel/src/dev/pciuser.c`・`pciuser.h`、`docs/nocturne-native-abi.md` の §9。今あるコードは正しいと示されるまでゴミの山として扱う。悪いコードは修理せず、消して正しい実装で置き換える。差分の小ささは良さの尺度ではない。機能と対応の装置は失わない。

担当のファイル（ここだけ書く）：
新しい `kernel/src/hal/claim.c`、`kernel/src/hal/hal.h` の「プロセスの実装」の節と `struct hal_claim_ops`（他の節は変えない）、`kernel/src/dev/pciuser.c`・`pciuser.h`、新しい `kernel/src/dev/screen_driver.c`（`dev/fb.h` に見出しの節を足してよい）、`kernel/src/mm/shared.c`・`shared.h` の claim に触る所、`kernel/src/gui/screenmode.*` と `gui/wm.c` の pciuser に触る所、`kernel/src/sys/syscall.c`、`common/abi.h`、`user/libc/sys.c`、`user/include/nocturne.h`、`user/apps/services.c`、`user/apps/settings.c`、`user/apps/settings_words.h`、`user/apps/lspci.c`、`user/apps/neofetch.c`、`user/apps/syscallfuzz.c`、`ports/nvidia/*`、`tests/pcitest.c`・`tests/nvrm*`・`tests/nvk_rm_hosttest.c`、`docs/nocturne-native-abi.md`。

作るもの：
1. **どのバスの装置もプロセスが持てる口。** 新しい object の種類 `N_KIND_HAL_CLAIM`（`N_KIND_PCI` を消す）。`N_CAP_PCI_CLAIM` は `N_CAP_HAL_CLAIM` にする。syscall：
   - `n_hal_claim(cap, device_id)`：装置を持つ。`hal_bind_process` の後、バスの `struct hal_claim_ops` の begin を呼ぶ（PCI ならグループの隔離、FLR の回復など、今の `pci_user_claim` の働き全部）。
   - `n_hal_claim_resources(claim, struct n_hal_resource *out, max)`：資源の一覧。
   - `n_hal_map(claim, slot)`：メモリーの資源を共有 object の handle として返す（今の `SYS_PCI_BAR`。`mm/shared.c` の claim への結びつきを PCI の型から切り、`struct hal_claim` を指す）。アプリは今のとおり `n_shared_map` で写し、後は MMIO を直接読み書きする。
   - `n_hal_interrupts(claim, count)`・`n_hal_interrupt_take(claim, bits, words, deadline)`：今の `n_pci_interrupts` を `hal_irq_enable` の上に。バスを問わない。
   - `n_dma_create(claim, ...)`・`n_dma_addresses`：claim の型を一般にする。
   - `n_hal_bus_call(claim, op, const void *in, size_t in_size, void *out, size_t out_size)`：バスだけの操作。番号はバスごと（`common/hal_buses.h` に PCI の `N_PCI_CALL_INFO`、`N_PCI_CALL_CONFIG_READ`、`N_PCI_CALL_CONFIG_WRITE`、`N_PCI_CALL_HOLD_SCREEN`、`N_PCI_CALL_ACPI_EVAL`、`N_PCI_CALL_ACPI_NOTIFY_TAKE`）。`struct hal_claim_ops` の `call` へ振り分ける。今の `SYS_PCI_*` の全部を消す。
   - `n_hal_offer(claim, kind, instance, label)`・`n_hal_offer_withdraw`：`hal_offer` を呼ぶ。
   - `n_hal_power_wait(claim, deadline)` と `n_hal_power_done(claim, ok)`：眠りの知らせ。`n_hal_power_wait` を一度でも呼んだ claim は `hal_claim_sleeps` が真で、`hal_claims_suspend` が `N_HAL_POWER_SUSPEND` を渡して期限つきで答えを待ち、`hal_claims_resume` が `N_HAL_POWER_RESUME` を渡す。呼ばない claim は眠りを断る（`HAL_REFUSAL_PROCESS`）。
   - `struct hal_claim_ops` の begin・end・call・bar 相当の op を決める。`hal/claim.c` はバスを知らず、PCI の `pci_claim_ops`（`pciuser.c` に置く。`pci.c` の担当者がこの名前を `extern` で参照する）を通す。バスが claim の op を持たなければ `-N_ERR_UNSUPPORTED`。資源とメモリーと割り込みだけで足りるバス（ACPI）のために、`hal/claim.c` に `const struct hal_claim_ops hal_claim_plain` を置く（私が `dev/acpi_devices.c` で参照する）。`hal_claim_revoke` は今の claim の終わりの全部（BAR の写像を外す、割り込みとバスマスターを止める、IOMMU の遮断）を走らせる。
2. **画面の口を PCI から離す。** `SYS_PCI_GIVE_SCREEN`、`SCREEN_FLIPS`、`SCREEN_VBLANK`、`SCREEN_STATE`、`SCREEN_IMAGE`、`SCREEN_IMAGE_END`、`SCREEN_MODES` を、claim の handle を受ける `SYS_SCREEN_*`（`n_screen_give` など）にし、実装を `pciuser.c` から `kernel/src/dev/screen_driver.c` へ移す。`pciuser.c` は PCI の claim の実装だけを持つ。
3. **台帳の読み手。** `SYS_HAL_LIST` は `hal_syscall_list(void *out, bytes)`（核は書いてある）。`user/libc/sys.c`：`n_hal_list(void *out, size_t bytes)`、`n_hal_snapshot(struct n_hal_snapshot *)`（全部の記録を持つ緩衝、`n_hal_device_next` で歩く）、`n_hal_watch`。`pcilist` を消す。`lspci`・`neofetch`・`libnvrm` は記録の性質（`N_PROP_PCI_*`）を読む。`struct n_pciinfo`、`struct n_pci_match`、`n_hal_bus` は消してある。
4. **サービスの管理者。** `user/apps/services.c` の宣言は `struct n_hal_rule` の表にする（`N_PCI_ID` などは `common/hal_buses.h`）。`SCOPE_FUNCTION` を `SCOPE_DEVICE` にする。状態が `N_HAL_UNBOUND` で規則に合う装置ごとに一つの実例を起こし、引数は装置の番号の 16 進数。`nvrm` は `n_hal_claim(cap, id)` で持つ。
5. **設定の窓の「装置」の頁。** `user/apps/settings.c`：木を親の下に子が続く順に並べる。バスの名前で分岐しない。場所は記録の `location`、名前は `name`。無ければ PCI の class の文か性質から文を作る。動かしている実装、動かせない理由、提供するもの（`n_hal_device_offers` の `label`）を見せる。文は `settings_words.h` の表に足す。実装の無い機能と memory controller のような画面に要らない行の扱いは、人の用事で決める。
6. **`ports/nvidia`。** `n_pci_*` を新しい口へ全部直す。動きは変えない。眠りは今と同じく断ったまま（`n_hal_power_wait` を呼ばない）。`hal_offer` で `N_OFFER_GPU` と `N_OFFER_SCREEN` を見せる。
7. **`docs/nocturne-native-abi.md`** の §9 を今の形へ書き直す。経緯は書かない。
8. 古い名の `rg` がリポジトリ全体（`docs/records`、`third_party` を除く）で 0 になるようにする：`SYS_PCI_`、`n_pci_`、`N_KIND_PCI`、`N_CAP_PCI_CLAIM`、`pcilist`、`N_HAL_BUS_`、`n_pciinfo`、`SCOPE_FUNCTION`。`n_dma_*` は claim の型だけ変える。

組みの確かめ：`python -X utf8 scripts/build.py -j 8 kernel` は他の担当者のファイルで落ちるのが普通である。自分のファイルの誤りだけを読む。ユーザー空間は `python -X utf8 scripts/build.py -j 8 user`。QEMU は私が最後に一度動かすので、起動しなくてよい。全部の変更に試験は要らない。

報告は日本語で：消したもの、建てたもの、足した行と消した行、他の担当者へ要る口、確かめていないこと。

## 報告

### a29e5e1b8f3f77af9

## 報告：PCI のバス化、ストレージの契約、NVMe と AHCI の作り直し

**消したもの**
- 古い blk の契約：`blk_looking`、`blk_busy*`、`blk_by_blocks`、`blkdev` の `read`/`write`/`persist` の関数ポインター。
- 古い PCI の結びつけ：`pci_driver`、`pci_bind`、`PCI_NOT_MINE`。
- 古い電源の登録：`pm_register`、`pm_device`、グローバルの `nvme_pm`、`ahci_pm`、`hbas` の一覧。
- NVMe と AHCI の初回起動 thread：`first_start`、`blk_looking` の括り。
- `vm/ata.c` の公開関数：`ata_count`、`ata_get`、`ata_read`、`ata_write`、`ata_flush`、`ata_init`。
- `storvsc_init`（`storvsc.c` 側）。
- `ahci.c` の古い読み書き：`rw`、`blk_ahci_*`、`prds` のバッファー版、`low_lock`、`alloc_dma`。
- 古い名の `rg` の結果：0 件。対象は `PCI_NOT_MINE`、`pci_driver`、`pci_bind`、`blk_looking`、`blk_busy`、`blk_by_blocks`、`pm_register`、`pm_device`、`hal_bound`、`hal_restarted`、`hal_unbound`。範囲は私のファイル全部。

**建てたもの**
- **`dev/pci.c`・`pci.h`**
  - `struct pci_device` に `struct hal_device hal` を埋めた。
  - `pci_init` が全機能を `hal_publish` する。親は上流の bridge。
  - 性質は `N_PROP_PCI_*`。資源は BAR ごとに一つ（slot は BAR の番号）。I/O BAR も測り、`N_HAL_PORT` になる。
  - 場所の文字列は `%04x:%02x:%02x.%x`。
  - `pci_bus_ops`（`irq_enable` は MSI-X、MSI、INTx の順。`irq_disable`、`irq_max`、`enable`、`stop`、`.claim = &pci_claim_ops`）。
  - `pci_device_of`。
- **`dev/blk.h`・`blk.c`**
  - 新しい形は `blk_ops.submit`、`blk_request` の鎖、物理の `blk_segment`、`blk_complete`、`blk_serial_ops`・`blk_serial_start`・`blk_serial_stop`。
  - `blk.c` が分割（最大セクター数、最大 segment 数、境界）と、部分ブロック書き・不整列の read-modify-write を持つ。
  - 電源の門 `blk_hold` は、request が終わるまで数える。
  - 書き込みの計数と `blk_cache_lost` は元の意味のまま。
  - `blk_register(hal, d)` が `N_OFFER_DISK` を出す。
  - `blk_read`/`blk_write`/`blk_persist` は同じ名前と意味で、`blk_submit` の上に建てた。
- **`dev/nvme.c`（全面）**
  - `HAL_DRIVER(nvme_driver)`、規則は `N_PCI_CLASS_PROGIF(1,8,2)`。
  - 鎖は呼んだ CPU の queue に全部入れ、doorbell は一度。完了は割り込みで request を終える。
  - PRP と SGL（Identify Controller の SGLS で選ぶ）を segment から建てる。
  - 失敗して再試行できるもの、reset が打ち切ったもの、queue に入らなかった鎖は、コントローラーの thread へ回し、最大 3 回。thread は他に、reset、割り込みの無い場合の poll、30 秒の時間切れの監視を持つ。
  - コントローラーごとの `quiesce`、`resume`、`detach`。
  - 古い `first_start` と `blk_looking`、`blk_busy`、グローバルの `nvme_pm` は無い。
  - 管理コマンドは同期のまま。
- **`dev/ahci.c`**
  - `HAL_DRIVER(ahci_driver)`、規則は `N_PCI_CLASS_PROGIF(1,6,1)`。
  - NCQ の disk は `ahci_submit` で、鎖の request ごとに slot を取り、取れた分を一度の `PxSACT`/`PxCI` で発行する。
  - 完了の割り込みが `slots_end` で request を終える。
  - NCQ の失敗、flush、非 NCQ の disk、4 GiB までしか届かない adapter は、disk ごとの `blk_serial` thread で一つずつ走らせる（失敗は単独で最大 3 回。低位の buffer 経由の bounce もここ）。
  - adapter の thread が、エラーを越えて走った port と、時間切れの request（30 秒）を止める。
  - `hal_looking_*`、`hal_busy_*`、`hal_map`、`hal_dma_alloc`、`hal_irq_enable` を使う。INTx の共有線のため、ISR は `IS == 0` なら `IRQ_NONE`。
  - 回復の段（settle、halt、port_up、HBA reset）は元のまま。
- **`vm/ata.c`・`ata.h`**：`HAL_DRIVER` 相当の `ata_driver`（規則 `N_FIXED("ide-ports")`）。`blk_serial_start` で `ata%d` を登録する。
- **`vm/hv/storvsc.c`（全面）**
  - `HAL_DRIVER`、規則は `N_VMBUS_TYPE(scsi_guid)`。
  - 読み書きは GPA 直接のパケットで、request の segment をそのまま使う。64 slot。
  - 120 秒の watchdog が adapter を落とす。
- **`dev/iommu.c`**：HAL の driver `iommu`（`N_PCI_CLASS(0x08,0x06)`）。`iommu_amd.c` の `hal_bound` を削除。
- **`fs/store.c`**：`#include "hal/hal.h"` を一行足した（`hal_settled` の宣言が無く、私の範囲のコンパイルが通らなかったため）。
- `fs/cache.c` と `hibernate.c` は blk の公開名のまま通るので、変更なし。

**行数**：私のファイル 12 個で追加 1946 行、削除 908 行（`git diff --stat`）。`pci.c`・`pci.h` の数字には他の agent の編集が混ざっている可能性がある。

**他の agent に要ること**
- `hal_init` を `pci_init` の前に呼ぶ。
- `pciuser.c` が `pci_claim_ops` を定義する。
- hda の guest が INTx で動かす場合、`pci_device.no_messages` を立てる。
- `pci_drivers.h` から `nvme_driver` と `ahci_driver` を消す。どちらも `static` になった。
- `vm/vm.c`：`hal_driver_add(&ata_driver)` と、固定ノード `"ide-ports"` の公開。
- `vmbus.h` から `storvsc_init` の宣言と呼び出しを消す。
- `usbmsc.c`：`blk_by_blocks` と旧 `read`/`write` から、`blk_serial_ops` と `blk_serial_start` へ移す。
- `blk_register` は `(struct hal_device *, struct blkdev *)` を取る。
- `blkdev` の新しい宣言欄は `max_sectors`、`max_segments`、`segment_align`、`segment_boundary`、`has_cache`。
- 古い `ata_*` 公開関数を呼ぶ側は直す。
- `pm.c` が `d->cache_lost`、`blk_hold`、`blk_persist` を使うのは変更なし。

**確かめた範囲**
- 次のファイルを `/tmp/cc.sh` で単体コンパイルし、error と warning が 0 件。対象：`blk.c`、`nvme.c`、`ahci.c`、`pci.c`、`iommu.c`、`iommu_amd.c`、`vm/ata.c`、`storvsc.c`、`cache.c`、`store.c`、`hibernate.c`。

**確かめていないこと**
- 全体ビルド、リンク、QEMU、Hyper-V、実機での動作。全部未確認。
- NVMe の SGL の経路、AHCI の NCQ のバッチ発行と時間切れの停止、storvsc の watchdog、blk.c の分割と bounce。どれも走らせていない。
- `ahci_detach` は disk の serial thread を止めない（止めると、完了側から呼ぶ `blk_serial_ops.submit` が NULL を引くため）。thread は idle のまま残る。
- AHCI の `has_cache` は常に true で、flush するかは `transfer` の `d->cache` が毎回決める。
- NVMe の `health_check` が全 queue の slot を数える。周期は 250 ms で、`inflight` が 0 なら走らない。CPU 数が極端に多い機械での費用は測っていない。

### a7b07708d937c58c7

## 報告（依頼 5 の続き。取り替えと取り込みが全部済んだ）

### 消したもの
- `SYS_PCI_*` の全部。`sys_pci_*`、`sys_pci_bar`、`sys_pci_acpi_eval`、`N_KIND_PCI`、`N_CAP_PCI_CLAIM`。
- 旧 `pciuser.c` の中身（1000 行超）。新しい `pciuser.c` は 671 行。
- libc の `pcilist` と `struct n_pciinfo`。
- `services.c` の `SCOPE_FUNCTION`、`struct n_pci_match` の表、`function_matches`。
- 設定の窓の `hardware_where` と、バス名での分岐（`N_HAL_BUS_*`）。語 `W_HW_USB`、`W_HW_VMBUS`。
- `nvrm_pci_is_gpu`。
- `docs/nocturne-native-abi.md` §9 の旧い呼出しの記述と、「`pcilist` は…」の段落。

### 建てたもの
- `kernel/src/hal/claim.c`（新規、566 行）。どのバスの装置でも持てる claim。claim の終わりは冪等で、最後の handle が閉じたときと `hal_remove` のどちらでも走る。
- `kernel/src/dev/pciuser.c`（`pci_claim_ops`）。
- `kernel/src/dev/screen_driver.c`（新規、644 行）。`SYS_SCREEN_*` を claim の handle で受ける。画面の hold の見張りスレッドもここへ移した。
- `mm/shared.c`・`shared.h`・`shared_parts.h`：`hal_claim_*`、`shared_hal_memory`（slot が BAR の番号）。DMA は `hal_claim_dma_bits` が 0 なら `N_ERR_UNSUPPORTED`。
- `sys/syscall.c`・`common/abi.h`：`SYS_HAL_CLAIM`〜`SYS_HAL_POWER_DONE` と `SYS_SCREEN_*`。`N_ABI_GENERATION` は 4。
- libc とヘッダー：`n_hal_claim`、`n_hal_claim_resources`、`n_hal_map`、`n_hal_interrupts`、`n_hal_interrupt_take`、`n_hal_bus_call`、`n_hal_offer(_withdraw)`、`n_hal_power_wait/done`、`n_hal_pci_*`、`n_screen_*`、`n_hal_list`、`n_hal_snapshot`、`n_hal_watch`。
- `services.c`：`SCOPE_DEVICE` と `struct n_hal_rule` の表。台帳の `N_HAL_UNBOUND` の装置ごとに一つ起こし、引数は装置 id の 16 進数。
- 設定の窓「装置」の頁：木の順に並べ、バス名での分岐は無い。`location` と `name` を出し、名が無ければ PCI の級の性質から呼び名を引く。実装の名、動けない理由、提供の名を出す。台帳の根は見せず、その子から並べる。`W_HW_ATTACHING` を足した。
- `lspci`・`neofetch`：台帳の `N_PROP_PCI_*` を読む。`syscallfuzz`：`SYS_HAL_*` と `SYS_SCREEN_*` へ移した。
- `ports/nvidia` の全部：`nvrm_dev.claim`、`n_hal_*`、`n_screen_*`。`nvrm.c` は引数の id を `strtoul` して自分で `n_hal_claim` する。`N_OFFER_GPU` は GPU が起動した後、`N_OFFER_SCREEN` は画面の開始後に出し、止めるときに外す。`n_hal_power_wait` は呼ばず、眠りは今までどおり拒む。README も更新した。
- `third_party/mesa/.../nvkmd_nocturne_rm.c`：旧 `pcilist` に頼っていたので、台帳の読みへ移した。NVK の GPU の身元は `N_PCI_TARGET` のまま。claim 側の装置 id へは `device_of_target` で引く。`nvkmd_nocturne_rm.h` と `nvrm.h` に `hal_buses.h` を足した。
- 試験：`pcitest`、`nvrmtest`、`nvk_rm_hosttest`、`nvrm_transport_hosttest`、`nvrm_display_hosttest`、`nvrm_dma_hosttest`、`nvrm_servetest` を新しい呼出しと台帳の模擬へ直した。ホストの試験は走らせていない。
- `docs/nocturne-native-abi.md` §9：今形で書き直した（claim、権利の呼出し、`n_hal_bus_call`、offers、power、画面、台帳）。FLR・SBR・guard・ACPI の事実は残してある。

### 行数
- 新規 2 ファイルで 1210 行。差分は 46 ファイルで +1623/−2033。`hal.h`、`abi.h`、`hal_buses.h` は一部がリードの分を含む。

### 旧名の `rg`
- `SYS_PCI_`、`n_pci_`、`N_KIND_PCI`、`N_CAP_PCI_CLAIM`、`pcilist`、`N_HAL_BUS_`、`n_pciinfo`、`SCOPE_FUNCTION` を `git grep` した（`docs/records` と `third_party` は除く）。0 件。
- 最後に残っていた `kernel/src/dev/acpi.h:204` のコメントだけは、他の持ち主のファイルだが `n_hal_pci_acpi_eval` に直した。`ahci.c` と `xhci.c` の旧名は、確かめたときには 0 件。
- `third_party` は除外のため見ていないが、`nvkmd_nocturne_pdev.c` は `N_PCI_TARGET_*` を使うだけで旧名は無い。

### 確かめたこと
- `python -X utf8 scripts/build.py -j 8 user` は exit 0。`nvrm` を含むアプリと libc は組めた。
- `... nvk` は exit 0。`build/drivers/nvk` が組めた。途中で `nvrm.h` と `nvkmd_nocturne_rm.h` の include 不足を直した。
- カーネルは `build.py kernel` では最初の失敗（他者の `xhci.c` が `dev/pci_drivers.h` を見つけられない）で止まる。自分のファイルだけを個別に組んだ：`claim.o`、`screen_driver.o`、`shared.o`、`syscall.o`、`screenmode.o`、`wm.o`。エラーは 0。`pciuser.o` と `fb.o` は「更新済み」だった。`syscall.c` の警告は他者の `net.h` の `struct netif`。

### 確かめていないこと
- QEMU で動かしていない。実機でも動かしていない。製品として見ていない。
- カーネル全体のリンク。他者のファイルが組めるかによる。
- ホストの試験と `pcitest`・`nvrmtest` の実行。
- 設定の窓の「装置」頁の見た目。

### 他の持ち主が満たす必要のあるもの
- `struct pci_device` に `struct hal_device hal` を持たせる。`pciuser.c` は `PCI_OF()` で `offsetof(struct pci_device, hal)` を使う。
- `pci_bus_ops.claim = &pci_claim_ops;`
- PCI の `irq_enable` は claim の割り込みの指定を受ける。
- メモリ資源は `slot = BAR の番号`、プリフェッチ可能なら `N_HAL_PREFETCHABLE`。
- `pm` は `hal_claims_suspend` と `hal_claims_resume` を呼ぶ。眠りが取り消されたときも `hal_claims_resume` を呼ぶ。
- 台帳の根は、`parent == 0` の記録である。

### 限界と、変わった振る舞い
- 呼出しの最中の `hal_remove` は守っていない。
- `hal_find` から `hal_bind_process` までの間に取り外せるバスには、寿命の隙間がある。
- 画面の呼出しは PCI の級を見ない。claim が画面の資源を持たなければ、各呼出しの検査で拒む（`N_ERR_DENIED`）。以前は表示機能の級でない claim を拒んでいた。
- 画面を hal id と出力で数えるので、抜き差しした装置の画面の同一性が変わりうる。
- NVIDIA の channel 名は `nocturne.nvidia.rm.<8 桁>` のまま、中身が台帳の id になった。`nvrm_transport_hosttest` の期待名も id の値に合わせて直した。
- `map.md` の旧名の行（113 行付近）は触っていない。

### a1160a60e81624e26

## 結果

消したものと建てたものを先に書く。

`python -X utf8 scripts/build.py -j 8 kernel` は、私の担当ファイルに誤りも警告も出さずに通った。最後の実行は「行うべき事はありません」で、組みは済んでいた。QEMU は起動していない。

### 消したもの
- USB
  - `usb_drivers` の表、`USB_DRIVER_*`、`state[]`、`struct usb_driver`、`USB_NOT_MINE`、兄弟の鎖、`bind_hub`、`usb_next_interrupt_interface` の公開、`usbhub_attach`、`usbhid_claims`、`usbmsc_claims`、`usbnet_claims`
  - 単一の `xhci_pm`、`xhci_refuse`、`xhci_matches`、`controllers[]`、`ncontrollers`
  - xHCI 内の BAR 検索、`vmm_map_mmio`、`pci_enable_busmaster`、`pci_irq_enable`、`blk_looking_*`
- 音と網
  - `hda.c` の INTx と message の自前の割り当て、`pm_device`
  - `net.h` の古い `send` と `read` の契約、`usbnet` の `gone` 一覧
- VM と VMBus
  - `pci_driver`、`vm_pci_drivers`、`vm_bind_fixed`、`vmbus_find`、`vm/devices.h` の使用
- 古い名の確認
  - 担当のファイルで `rg` した。0 件。対象は `pci_driver`、`usb_driver`、`USB_DRIVER`、`USB_NOT_MINE`、`pm_register`、`pm_device`、`hal_bound`、`hal_restarted`、`hal_unbound`、`hal_add`、`vm_pci_drivers`、`vm_bind_fixed`、`n_pci_match`、`vm/devices.h`、`vmbus_find`、`hv_input_init`、`hv_util_init`、`vbe_display_pm`、`netvsc_probe`、`blk_looking`。

### 建てたもの
- USB の木
  - `struct usb_device` は `struct hal_device hal`（`N_BUS_USB`）を持つ。親はハブの節か xHCI の PCI 機能。性質は `N_PROP_USB_VENDOR/PRODUCT/RELEASE/CLASS`。場所は `USB 2-1.3` の形。
  - 各 interface は子の節 `N_BUS_USB_INTERFACE`（`struct usb_interface`）。性質は class（`N_USB_CLASS_CODE`）、interface 番号、親の vendor と product。
- class driver
  - `hub`、`usbhid`、`usbmsc`、`usbnet` はそれぞれ `struct hal_driver`（`HAL_DRIVER`）。rule は `N_USB_INTERFACE_CLASS`、`N_USB_INTERFACE_SUBCLASS`、MSC の厳密な class code。
  - CDC は comm interface に付き、data interface を自分で取る。
  - 設定の選択は `hal_driver_exists`。
- xHCI
  - 一台ごとに `struct hal_driver`（rule は PCI の class と progif 0x0C/03/30）。`attach`、`detach`、`quiesce`、`resume` を持つ。
  - `hal_map`、`hal_enable`、`hal_irq_enable`、`hal_failed`、`hal_recovered`、`hal_looking_begin/end`、`hal_busy_begin/end` を使う。
  - 装置が見る番地は全部 `hal_dma_address`（`bus_of`）を通す。`hc_stop` を `hc_restart` から切り出した。detach は worker を止めてから抜ける。
  - `release_slot` と `usb_remove` は `hal_remove` を使う（interface の節、次に装置の節の順）。
- 他
  - `usbmsc` は `blk_serial_start` と `blk_register(hal, ...)` に乗せ、segment が複数ならバウンスバッファを通す。`msc_refuse` は USB ディスクがある間の眠りを断る。
  - `usbnet` は `transmit`、`poll`、`rx_pending`、`net_register(hal, ...)` に乗せた。NCM は二部の frame を一続きにする。
  - 新しい `kernel/src/vm/rdp.h` に `rdp_init`。`vm/devices.h` が無くなったための置き場。
  - `net.c` の `net_rx` の変数の衝突と、`net.h` の `struct netif;` の前方宣言を直した。
  - `\n` が本物の改行になっていた `hv_input.c` と `hv_util.c` の文字列を直した。

### 行数
担当のうち `git diff --numstat` で数えたファイルの合計。足した行は 2388、消した行は 1633。`ata.c`、`ata.h`、`storvsc.c` は他の担当なので含まない。

### 他の担当者へ要る口
- USB
  - `struct usb_interface`、`usb_interface_of(hal)`、`usb_interface_interrupt(i, &extra, &extra_len, &ep)`、`usb_work_set(i, fn)`。
  - `usb_remove` の中でハブの `usbhub_detach` も呼ぶ（二度呼んでも害は無い）。
- 網とストレージ
  - `net_register(hal, nif)`、`net_unregister`、`net_rx(dev, frame, len, flags)`。
  - `blk_serial_start/stop`、`blk_register(hal, ...)`。
- VM と VMBus
  - `vmbus_channel_of`。
  - IDE の固定の節 `N_FIXED("ide-ports")`。slot 0 = 0x1F0、1 = 0x3F6、2 = 0x170、3 = 0x376。`ata_driver` は無視する。
  - `vm.c` が `hal_driver_add` で足す driver：`vbe_driver`、`virtio_gpu_driver`、`e1000_driver`、`tulip_driver`、`ac97_driver`、`ata_driver`、`netvsc_driver`、`hv_keyboard_driver`、`hv_mouse_driver`、`hv_util_driver`。
  - `hda_driver`、`rtl8125_driver`、`hub_driver`、`hid_driver`、`msc_driver`、`usbnet_driver`、`xhci_driver` は `HAL_DRIVER` で登録される。
  - `storvsc_driver` も `HAL_DRIVER`。

### 確かめていないこと
- QEMU で動かしていない。ビルドが通っただけで、実行、USB の挙動、眠りは未確認。
- HAL の欠陥（私のファイルの外）
  - `hal/bind.c` の `bind_thread` は、`hal_remove` が返って bus が節を解放した後に `d->bind` を取る恐れがある。USB の interface 節と VMBus の節に当たる。
- 私の側の注意
  - `msc_detach` の `blk.hal = NULL` の代入は、`blk.c:343` の `d->hal->node` の読みと競合する。
  - xHCI の detach は、割り込みが detach の後に止まるので、controller の構造体を解放しない（メモリーが残る）。
  - interface ごとの attach の thread が、以前は worker だけが使っていた USB の API を同時に使う。
- 他に未確認なもの
  - tulip、virtio_gpu、netvsc は `resume` が無いので眠れない。どの driver にも `cold_ok` は付けていない。
  - TSO は宣言していない。
  - `virtio_gpu` の `test_flag` の分岐と単一の状態は残っている。
  - PCI の `hal_resource` の slot 番号が BAR の番号と同じだという前提で `hal_map(d, 0)` を使っている。
  - `hal_map` と `hal_dma_alloc` の後始末は `hal_release` に任せている。
  - `rdp.c` は中身を見ておらず、include の差し替えだけ。
  - `hv.c` と `hv.h` に古い参照が残っていないかは見ていない。
