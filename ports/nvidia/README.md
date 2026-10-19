# ports/nvidia：NVIDIA RM を Nocturne のドライバーアプリ `/bin/nvrm` にする

RTX 4070（AD104、Ada）を、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM（`third_party/nvidia-open`）で
動かすための層。台帳は `docs/nvidia/LEDGER.md`。**Nocturne は Unix でも POSIX でもない。** この層は
Nocturne のネイティブ ABI（`n_*`）と ISO C（`malloc`、`<threads.h>`、`timespec_get`）だけで書いてあり、
`fork`・`ioctl`・`mmap` などの互換層は無い。RM のソースは変更しない（変更が要るなら OS 層側で合わせる）。

状態：RM 全体（1100 個）と OS 層がコンパイルでき、`/bin/nvrm` のリンクで未解決なのは、まだ再ビルドしていない
libc 側の `n_pci_*`／`n_dma_*`／`n_shared_view` だけ（ソースには有る）。**実機（GSP の起動）では未確認。**

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
| `nvrm_serve.c／.h`、`libnvrm.c`、`include/` | リクエストサーバー、クライアント側ライブラリー、通信規約（主担当の所有。ここでは変更しない） |

起動手順（`nvrm.c`）：init が `N_KIND_PCI` をハンドル 3 で渡す → `n_pci_info` → `nv_state_t` を埋めて BAR0 を写す →
`NVRM_REGISTRY_DWORDS`（環境変数、任意）を `rm_parse_option_string` へ → `rm_init_rm` → `rm_is_supported_device`
→ `rm_init_private_state` → `rm_init_event_locks` → `rm_set_rm_firmware_requested` → 割り込みスレッド
→ `rm_init_adapter`（ここで GSP が起動する）→ `rm_get_device_name` をログへ → `nvrm_serve()`。
`rm_init_adapter` が失敗しても、診断のためにサーバーは動かし続ける（ログに失敗を残す）。

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
| `os_get_current_process`／`_name`／`os_get_current_thread`、`os_get_euid`、`os_is_administrator`、`os_check_access` | nvrm のプロセスは一つ。スレッド局所の `nvrm_thread_admin`（既定は管理者）。クライアントごとの隔離は無い |
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
- **イベント**：`nv_post_event` は `nvrm_event_post`（弱い既定は何もしない）へ。`nv_get_event`／`nv_get_file_private` はクライアントへ届けない。
- **ファイル**（`os_allocate_temporary_file` など）：ページアウト用。無い。

## 既知のリスク・不確かなこと

- 4 KiB を超えるページサイズの連続確保は、余分に取って整列する。非連続の大きなページも連続を強いるので、大きな確保は失敗しうる。
- `nv_alloc_user_mapping` の符号化は bus 番地が一意であることに頼る（IOMMU が無いので成り立つ）。
- 再配置可能な BAR（Resizable BAR）で BAR1／BAR2 が 16 GiB のとき、カーネル側の写しが失敗しうる。
- BAR を複数の範囲に分けたマッピングは未対応（`nvrm_rm_mapping_object` が -1 を返す）。
- 全クライアントが RM の目には一つのプロセス（`n_app_self`）。クライアントごとの隔離・権限は無い。
- `nv->bars[2]`（インスタンスメモリ）を設定空間の BAR 3 としたのは推測。
- `os_is_efi_enabled` は常に真。`os_disable_console_access` は何もしない（表示の乗っ取りは GSP 初期化の間にありうる。D8）。
- `nv_revoke_gpu_mappings` はクライアントの view を取り上げられない。
- `cnd_timedwait` は `TIME_UTC` で待つ（単調時計との差の影響は未確認）。
- RM の呼出しの深さに対し、スレッドのスタックが 256 KiB で足りるかは未確認。
- 実機でのファームウェア起動・割り込み・ブートの結果は未確認。
