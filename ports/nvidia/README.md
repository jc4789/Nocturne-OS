# ports/nvidia：NVIDIA RM を Nocturne のドライバーアプリ `/bin/nvrm` にする

RTX 4070（AD104、Ada）を、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM（`third_party/nvidia-open`）で
動かすための層。台帳は `docs/nvidia/LEDGER.md`。**Nocturne は Unix でも POSIX でもない。** この層は
Nocturne のネイティブ ABI（`n_*`）と ISO C（`malloc`、`<threads.h>`、`timespec_get`）だけで書いてあり、
`fork`・`ioctl`・`mmap` などの互換層は無い。修理は OS 層へ統合し、RM 本体の変更は既存の MIT
`arch/nvalloc/unix/src/os.c` の Nocturne 条件付き user-map 寿命コールバックに限定する。

状態：RM、OS 層、クライアントと NVK の接続がある。2026-10-20 の修理は
`docs/nvidia/repair-rm-first.md` と第二検討報告へ記録する。ホスト試験は native サービスを mock にするため、
その成功から PCI、GPU DMA、キャッシュ、GSP 起動の成功を推論しない。**NVIDIA 実機は未確認。**

## 構成

| ファイル | 役目 |
|---|---|
| `nvrm.mk` | RM と OS 層のビルド規則（トップの Makefile から `include` する。変数と規則だけで、既定の目標は変えない） |
| `nvrm_os.h` | OS 層の各ファイルが共有する宣言（デバイス、ログ、スレッド局所の状態、クライアント側のマッピング） |
| `os.c` | ログ、メモリ、文字列、時計・待ち、ロック、作業キュー、RC／nano タイマー、ファームウェア読込み、レジストリ表 |
| `os_pci.c` | PCI 設定空間、BAR、DMA メモリ（`nv_alloc_pages`）、カーネル／クライアント向けマッピング、MSI／MSI-X |
| `rm_api.c` | リクエストサーバーが使う入口 `nvrm_rm_file_open／close／ioctl／mapping_object`（`rm_ioctl` への橋） |
| `os_stubs.c` | Nocturne に対応が無い、または RM のこの構成で要らない `os_*`／`nv_*`（下の「未対応」） |
| `nvlink_os.c` | NVLink／NVSwitch ライブラリーが求める OS 関数（メモリ・文字列・時刻・識別） |
| `spdm_stubs.c` | libspdm の暗号バックエンド（71 個、すべて 0 を返す） |
| `nvrm.c` | `main`、割り込みスレッド、起動手順 |
| `nvrm_serve.c／.h`、`libnvrm.c`、`include/` | リクエストサーバー、クライアント側ライブラリー、通信規約 |

起動手順（`nvrm.c`）：init が `N_KIND_PCI` をハンドル 3 で渡す → `n_pci_info` → `nv_state_t` を埋めて BAR0 を写す →
`NVRM_REGISTRY_DWORDS`（環境変数、任意）を `rm_parse_option_string` へ → `rm_init_rm` → `rm_is_supported_device`
→ `rm_init_private_state` → `rm_init_event_locks` → `rm_set_rm_firmware_requested` → 割り込みスレッド
→ `rm_init_adapter`（ここで GSP が起動する）→ `rm_get_device_name` をログへ → `nvrm_serve()`。
`rm_init_adapter` が失敗しても、診断のためにサーバーは動かし続ける（ログに失敗を残す）。

装置ごとに `nocturne.nvidia.rm.<N_PCI_TARGETの4桁16進数>` を公開する。
`nvrm_connect_device()` は指定装置へ、従来の `nvrm_connect()` は動的 PCI 列挙から最初に接続可能な装置へつなぐ。
従来名 `nocturne.nvidia.rm` は一つの daemon だけが既存アプリ用 alias として公開する。
Protocol1 の48byte header と field offset は維持し、通常応答で未使用だった hello 用領域へ
RM 戻り値と map access を格納する。旧バイナリーにも RM 関数失敗を負の status で通知する。
RO view を RW map しようとする旧クライアントは拒否される。RO 対応には新しい libnvrm との再リンクが必要で、
互換性のために写像権利を緩めない。

ファームウェア：`/usr/share/firmware/nvidia/615.78.08/`、無ければ `/data/firmware/nvidia/615.78.08/`
（`nv_get_firmware`）。git には入れない（台帳 D9）。

ビルド時、Makefile は `NVIDIA_FIRMWARE`（既定 `local/firmware/nvidia/615.78.08`、gitignore 済み）に
`firmware/gsp_ga10x.bin` があれば `gsp_ga10x.bin`・`ucodes_ga10x.bin`・`LICENSE` を initrd へ入れる。
**その手元の媒体はファームウェアを含むので配布しない。** 配布する媒体は `make NVIDIA_FIRMWARE=` で作る
（そのときは利用者が `/data/firmware/nvidia/615.78.08/` へ置く）。

## ビルド

```
make -f ports/nvidia/nvrm.mk BUILD=build CC=clang LD=ld.lld nvrm-objs    # RM＋OS 層（build/nvrm/）
make -f ports/nvidia/nvrm.mk ... nvrm-undefined                           # 未定義シンボル一覧（build/nvrm/undefined.txt）
```

トップの Makefile に取り込むときは、`include ports/nvidia/nvrm.mk` の後に次の規則を足す
（RM の 1100 個のオブジェクトは `nv-kernel.o` に部分リンクされ、`memcpy`／`memset` は局所化される）。
`NM`／`OBJCOPY` は binutils の `nm`／`objcopy`（この環境に llvm-nm／llvm-objcopy は無い）。

```make
$(BUILD)/root/bin/nvrm: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) --start-lib @$(LIBC_RSP) --end-lib -o $@
```

`nvrm.mk` の注意：`srcs.mk`（RM のソース一覧の正本）を読むので、RM のバージョンを替えても一覧は追従する。
`LD` に渡す引数が長いので、`nv-kernel.o` は応答ファイル（`nv-kernel.o.rsp`）で部分リンクする。

## 未定義シンボルの内訳

RM のオブジェクト（`nv-kernel.o`）が必要とするが RM 自身は定義しない名前は **397 個**（libc の関数を除く）。

| 分類 | 個数 | どこで |
|---|---|---|
| `os_*`／`nv_*` のうち実装したもの | 138（`os.c` 96、`os_pci.c` 42） | 実装。下の対応表 |
| `os_*`／`nv_*` のうち未対応・中立の答え | 140 | `os_stubs.c`（下の「未対応」） |
| `nvlink_*`／`nvswitch_os_*` | 48 | `nvlink_os.c`（実装 48。GPU に NVLink／NVSwitch は無いので、DMA・イベント・fabric manager は未対応を返す） |
| `libspdm_*`（暗号バックエンド） | 71 | `spdm_stubs.c`（0 を返す。SPDM は Hopper 以降の機密計算と認証にだけ使われ、Ada では始まらない） |

最後に残るのは libc の関数（`malloc`・`mtx_*`・`thrd_*`・`snprintf` など）と Nocturne の `n_*`／`clock_info`／`cpu_info`／`msleep`
だけ。これらは `user/libc/sys.c` などが持つ。

## OS 層の対応表（実装したもの）

| RM が呼ぶ関数 | Nocturne での実装 |
|---|---|
| `nv_printf`、`nv_vprintf`、`nv_dev_vprintf`、`os_log_error`、`nv_log_error` | システムログ（`n_system_log_open`）と、`/data` があれば `/data/logs/nvrm.log`（`nvrm_log`）へ |
| `os_alloc_mem`／`os_free_mem`、`os_mem_*`、`os_string_*`、`os_snprintf` ほか | `malloc`／`free`／libc の文字列関数（`os_alloc_mem` は 0 初期化しない。RM の約束どおり） |
| `os_memcpy_from_user`／`to_user` | リクエストサーバーの `nvrm_user_translate`。クライアントの「引数の場」の中の番地だけを nvrm の番地へ読み替える。場の外は `NV_ERR_INVALID_ADDRESS`（台帳 D4） |
| `os_get_system_time`、`os_get_monotonic_time_ns*`、`os_delay*`、`os_schedule` | `clock_info`（TSC）、`msleep`、`thrd_yield`、`timespec_get` |
| `os_alloc_mutex`／`os_acquire_mutex` ほか、`os_alloc_spinlock`、`os_alloc_semaphore`、`os_alloc_rwlock_*`、`os_alloc_wait_queue` | C11 の `mtx_t`／`cnd_t`。スピンロックも mutex（ISR も普通のスレッドのため）。待ち行列は `cnd_t`＋ラッチ |
| `os_queue_work_item`、`os_flush_work_queue`、`os_is_queue_flush_ongoing` | 最大 8 本のワーカースレッド（`rm_execute_work_item`） |
| `nv_start_rc_timer`／`nv_stop_rc_timer`、`nv_create_nano_timer` ほか | 1 本のタイマースレッド（1 Hz の RC、nano タイマー） |
| `nv_get_firmware`／`nv_put_firmware` | `n_document_load`（上のフォルダー順。ログの追記は `n_document_append`） |
| `os_get_cpu_count`／`os_get_cpu_number`／`os_get_cpu_frequency` | `cpu_info`／`cpu_index` |
| `os_get_current_process`／`_name`／`os_get_current_thread`、`os_get_euid`、`os_is_administrator`、`os_check_access` | 要求／cleanup は再利用しない接続固有の RM 主体と非特権 TLS で実行。daemon 自身のスレッドは daemon の PID と管理者権限。認証済み peer PID API が無いため、同一アプリの別接続も暗黙に同一主体とはしない |
| `os_pci_init_handle`、`os_pci_read_*`／`write_*` | `n_pci_config_read／write`。自分の機能だけ書ける。他の機能（上流ブリッジ）は読むだけ |
| `os_map_kernel_space`／`os_unmap_kernel_space` | BAR は `n_pci_bar`＋`n_shared_view`（UC／WC）＋`n_shared_map`。BAR0 は全体を一度だけ写す |
| `nv_alloc_pages`／`nv_free_pages` | `n_dma_create`（`N_DMA_CONTIGUOUS`、`N_DMA_32BIT`）＋`n_dma_addresses`。ページ 4 KiB より大きいときは余分に取って view で整列 |
| `nv_alloc_kernel_mapping`／`nv_free_kernel_mapping` | 共有の WB 写し、または view の写し |
| `nv_alloc_user_mapping`／`nv_free_user_mapping`、`os_match_mmap_offset`、`nv_add_mapping_context_to_file` | クライアントのアドレス＝ページの bus 番地＋オフセット（`os_match_mmap_offset` が逆引き）。マップ要求の間だけスレッドに「保留の文脈」を置き、`nvrm_rm_mapping_object` が view ハンドルにしてクライアントへ添付で渡す |
| `nvrm_irq_enable`（`os_pci.c`） | `n_pci_interrupts(1)`。ここで `nv->flags` に **`NV_FLAG_USES_MSIX`（MSI-X が使えたとき）か `NV_FLAG_USES_MSI`** を立てる（RM は読まない） |
| `nv_get_screen_info`、`nv_set_dma_address_size`、`nv_requires_dma_remap` | 起動時フレームバッファーを RM に知らせて保護（D8）／マスクの検査／IOMMU が無いので不要 |
| `os_flush_cpu_cache_all`／`write_combine_buffer`／`user_cache` | x86 の DMA はコヒーレントなので多くは空。WC バッファーは `sfence` |
| `os_get_random_bytes`、`os_get_version_info` ほか | `n_random`（システムの乱数）、版は "Nocturne OS" の固定値 |

スレッドの前提：割り込みスレッド（`n_pci_interrupt_take` → `rm_isr` → 必要なら `rm_isr_bh`）とリクエストスレッド
は、**RM 自身のロック（GPU ロック、スピンロック＝mutex）だけで互いを守る**。nvrm はその外側に追加のロックを置かない。
`nvrm_in_isr` は `rm_isr` の間だけ真。

## 未対応（`os_stubs.c`）

どれも「未対応の値」（`NV_ERR_NOT_SUPPORTED`、`NV_FALSE`、何もしない）を返す。理由は種類別。

- **Tegra／SoC**（クロック、I2C、DCE、IMP、BPMP、輝度）：PCI の GPU ではない。
- **ACPI メソッド**（`_DSM`、`_DDC`、`_DOD`、`_ROM`、MUX、電源）：アプリから AML を実行する手段が無い。
- **NUMA・ページ・IOMMU・dma-buf・scatter-gather の取り込み・ユーザーページの固定・P2P**：Nocturne にその概念が無い。クライアントのメモリは「共有オブジェクト」で渡す。
- **仮想化**（vGPU、GRID、VFIO）、**cgroup**、**IMEX チャンネル**、**能力ファイル**（`nvidia_caps_root` は NULL）。
- **電源管理**（ランタイムサスペンド、S0ix、動的電源）：最初は扱わない。
- **イベント**：`nv_post_event` はクライアント（`struct nvrm_file`）ごとのキューに積み、`nv_get_event`（`NV_ESC_RM_GET_EVENT_DATA`）で取り出す。プロトコルは要求・応答なのでクライアントがポーリングし、プッシュ通知は無い。`nv_get_file_private` は無し（記述子が無い）。
- **ファイル**（`os_allocate_temporary_file` など）：ページアウト用。無い。

## 既知のリスク・不確かなこと

- 4 KiB を超えるページサイズの連続確保は、余分に取って整列する。非連続の大きなページも連続を強いるので、大きな確保は失敗しうる。
- `nv_alloc_user_mapping` の符号化は bus 番地が一意であることに頼る（IOMMU が無いので成り立つ）。
- 再配置可能な BAR（Resizable BAR）で BAR1／BAR2 が 16 GiB のとき、カーネル側の写しが失敗しうる。
- 隣接 BAR range は個数制限なしで結合するが、真に非連続の BAR mapping は native shared view の能力外。
- `os_is_efi_enabled` は常に真。`os_disable_console_access` は何もしない（表示の乗っ取りは GSP 初期化の間にありうる。D8）。
- memory BAR の有効索引と64bit幅から RM aperture を割当てる。通常 VRAM 以外は UC に固定する。
- user-map ごとの RM private token を保持し、明示／暗黙 unmap と close で当該 view と派生写像を失効する。
  `nv_revoke_gpu_mappings` は既存全 view を失効し、失効に失敗した場合は成功と報告しない。
- `cnd_timedwait` は `TIME_UTC` の期限で待つ。Nocturne の `TIME_UTC` は戻らない（libc の `time.c`）ので、壁時計が進められても待ちが早く終わるだけで、タイマーの線は単調時計で測り直す。
- RM の呼出しの深さに対し、スレッドのスタックが 256 KiB で足りるかは未確認。
- 実機でのファームウェア起動・割り込み・ブートの結果は未確認。
