# ports/nvidia：NVIDIA RM を Nocturne のドライバーアプリ `C:\Nocturne\System\nvrm` にする

RTX 4070（AD104、Ada）を、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM（`third_party/nvidia-open`）で
動かすための層。台帳は `docs/nvidia/LEDGER.md`。**Nocturne は Unix でも POSIX でもない。** この層は
Nocturne のネイティブ ABI（`n_*`）と ISO C（`malloc`、`<threads.h>`、`timespec_get`）だけで書いてあり、
`fork`・`ioctl`・`mmap` などの互換層は無い。修理は OS 層へ統合し、RM 本体の変更は既存の MIT
`arch/nvalloc/unix/src/os.c` の Nocturne 条件付き user-map 寿命コールバックに限定する。

状態：RM、OS 層、クライアントと NVK の接続がある。2026-10-20 の再監査修理は
`docs/nvidia/review-87dd1e2f-map.json` で責務と検証範囲を管理する。ホスト試験は native サービスを mock にするため、
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
init／libnvrm／NVK は共通の PCI 選別条件（NVIDIA、VGA または 3D、progif 0）を使う。
NVK は生の PCI 件数を GPU 数にせず、HELLO で adapter 初期化成功を確認し、接続先 RM の probed ID と PCI 情報が一致する BDF の動的 snapshot を列挙する。probed ID だけでは初期化成功を保証しない。

装置ごとに `nocturne.nvidia.rm.<N_PCI_TARGETの4桁16進数>` を公開する。
`nvrm_connect_device()` は指定装置へ、従来の `nvrm_connect()` は動的 PCI 列挙から最初に接続可能な装置へつなぐ。
描画側の `nvrm_connect_ready()` は診断専用の未初期化装置を飛ばし、後続の ready 装置も探す。旧 HELLO の echo を ready と誤認しない。
従来名 `nocturne.nvidia.rm` は一つの daemon だけが既存アプリ用 alias として公開する。
Protocol1 の48byte header と field offset は維持し、通常応答で未使用だった hello 用領域へ
RM 戻り値と map access を格納する。旧バイナリーにも RM 関数失敗を負の status で通知する。
RM が引数を更新してからエラーを返した場合も、有効な応答の引数と arena 内の結果を呼出し元へ返す。
通信の破損・未実行の要求と RM のエラーを区別し、作成済み handle の後始末情報を捨てない。
RO view を RW map しようとする旧クライアントは拒否される。RO 対応には新しい libnvrm との再リンクが必要で、
互換性のために写像権利を緩めない。
RM の WRITE_ONLY 要求は NVOS33 が許す RW 写像で提供する（x86 の writable page は readable でもある）。
READ_ONLY の最大権利は引き続き RO であり、派生 view や合成 view でも昇格できない。

ファームウェア：`C:\Nocturne\Firmware\NVIDIA\615.78.08\` だけから読む（`nv_get_firmware`）。
システムの部分なのでアプリからは変えられず、ビルド時にディスクへ入れる。git には入れない（台帳 D9）。

ビルド時、Makefile は `NVIDIA_FIRMWARE`（既定 `local/firmware/nvidia/615.78.08`、gitignore 済み）のフォルダーが
あれば、その `firmware/gsp_ga10x.bin`・`firmware/ucodes_ga10x.bin`・`LICENSE` を C: の上のフォルダーへ入れる
（`scripts/layout.py`。3 つのどれかが欠けていればビルドは失敗する）。
**その手元のディスクはファームウェアを含むので配布しない。** 配布するディスクは `make NVIDIA_FIRMWARE=` で作り、
ファームウェアを含まない。

## ビルド

```
make -f ports/nvidia/nvrm.mk BUILD=build CC=clang LD=ld.lld nvrm-objs    # RM＋OS 層（build/nvrm/）
make -f ports/nvidia/nvrm.mk ... nvrm-undefined                           # 未定義シンボル一覧（build/nvrm/undefined.txt）
```

トップの Makefile は `include ports/nvidia/nvrm.mk` の後の次の規則で nvrm をリンクし、
`scripts/layout.py` がそれを `C:\Nocturne\System\nvrm` に置く
（RM の 1100 個のオブジェクトは `nv-kernel.o` に部分リンクされ、`memcpy`／`memset` は局所化される）。
`NM`／`OBJCOPY` は binutils の `nm`／`objcopy`（この環境に llvm-nm／llvm-objcopy は無い）。

```make
$(PROGRAMS)/nvrm: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
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

最後に残るのは ISO C の関数（`malloc`・`mtx_*`・`thrd_*`・`snprintf` など）と Nocturne のネイティブ関数
（`n_*`／`clock_info`／`cpu_info`／`pcilist`）だけ。これらは `user/libc/sys.c` などが持つ。ネイティブ関数は
失敗を負の `n_result` で返し、`errno` には触れない。ドライバーも `errno` を使わない。

## OS 層の対応表（実装したもの）

| RM が呼ぶ関数 | Nocturne での実装 |
|---|---|
| `nv_printf`、`nv_vprintf`、`nv_dev_vprintf`、`os_log_error`、`nv_log_error` | システムログ（`n_system_log_open`）と、nvrm のデータフォルダーの `nvrm.log`（`n_app_data_folder("NVIDIA")`、`C:\Users\<名前>\AppData\NVIDIA\nvrm.log`。`nvrm_log`）へ |
| `os_alloc_mem`／`os_free_mem`、`os_mem_*`、`os_string_*`、`os_snprintf` ほか | `malloc`／`free`／libc の文字列関数（`os_alloc_mem` は 0 初期化しない。RM の約束どおり） |
| `os_memcpy_from_user`／`to_user` | リクエストサーバーの `nvrm_user_translate`。クライアントの「引数の場」の中の番地だけを nvrm の番地へ読み替える。場の外は `NV_ERR_INVALID_ADDRESS`（台帳 D4） |
| `os_get_system_time`、`os_get_monotonic_time_ns*`、`os_delay*`、`os_schedule` | `clock_info`（TSC）、`n_sleep_ms`、`thrd_yield`、`timespec_get` |
| `os_alloc_mutex`／`os_acquire_mutex` ほか、`os_alloc_spinlock`、`os_alloc_semaphore`、`os_alloc_rwlock_*`、`os_alloc_wait_queue` | C11 の `mtx_t`／`cnd_t`。スピンロックも mutex（ISR も普通のスレッドのため）。待ち行列は `cnd_t`＋ラッチ |
| `os_queue_work_item`、`os_flush_work_queue`、`os_is_queue_flush_ongoing` | 最大 8 本のワーカースレッド（`rm_execute_work_item`） |
| `nv_start_rc_timer`／`nv_stop_rc_timer`、`nv_create_nano_timer` ほか | 1 本のタイマースレッド（1 Hz の RC、nano タイマー） |
| `nv_get_firmware`／`nv_put_firmware` | `n_document_load`（上のフォルダーから。ログの追記は `n_document_append`） |
| `os_get_cpu_count`／`os_get_cpu_number`／`os_get_cpu_frequency` | `cpu_info`／`cpu_index` |
| `os_get_current_process`／`_name`／`os_get_current_thread`、`os_get_euid`、`os_is_administrator`、`os_check_access` | 要求／cleanup はカーネルが送信時に認証した native app ID と非特権 TLS で実行。同じ app の別接続・thread は同じ主体。HELLO 後の全 message でも送信元を照合し、endpoint を別 app に渡しても偽装できない。daemon 自身の thread は daemon の PID と管理者権限 |
| `os_pci_init_handle`、`os_pci_read_*`／`write_*` | `n_pci_config_read／write`。自分の機能だけ書ける。他の機能（上流ブリッジ）は読むだけ |
| `os_map_kernel_space`／`os_unmap_kernel_space` | BAR は `n_pci_bar`＋`n_shared_view`（UC／WC）＋`n_shared_map`。BAR0 は全体を一度だけ写す |
| `nv_alloc_pages`／`nv_free_pages` | `n_dma_create`（`N_DMA_CONTIGUOUS`、`N_DMA_32BIT`）＋`n_dma_addresses`。ページ 4 KiB より大きいときは余分に取って view で整列 |
| `nv_alloc_kernel_mapping`／`nv_free_kernel_mapping` | 共有の WB 写し、または view の写し |
| `nv_alloc_user_mapping`／`nv_free_user_mapping`、`os_match_mmap_offset`、`nv_add_mapping_context_to_file` | クライアントのアドレス＝その写像だけの番地の範囲＋ページ位置＋オフセット（`os_match_mmap_offset` が同じ要求の写像から逆引き）。マップ要求の間だけスレッドに「保留の文脈」を置き、`nvrm_rm_mapping_object` が view ハンドルにしてクライアントへ添付で渡す |
| `nvrm_irq_enable`（`os_pci.c`） | `n_pci_interrupts(1)`。ここで `nv->flags` に **`NV_FLAG_USES_MSIX`（MSI-X が使えたとき）か `NV_FLAG_USES_MSI`** を立てる（RM は読まない） |
| `nv_get_screen_info`、`nv_set_dma_address_size`、`nv_requires_dma_remap` | 起動時フレームバッファーを RM に知らせて保護（D8）／マスクの検査／native DMAはIOMMUの有無によらず物理アドレスと同じbus addressを返すので追加変換不要 |
| `os_flush_cpu_cache_all`／`write_combine_buffer`／`user_cache` | x86 の DMA はコヒーレントなので多くは空。WC バッファーは `sfence` |
| `os_get_random_bytes`、`os_get_version_info` ほか | `n_random`（システムの乱数）、版は "Nocturne OS" の固定値 |

スレッドの前提：割り込みスレッド（`n_pci_interrupt_take` → `rm_isr` → 必要なら `rm_isr_bh`）とリクエストスレッド
は、**RM 自身のロック（GPU ロック、スピンロック＝mutex）だけで互いを守る**。nvrm はその外側に追加のロックを置かない。
`nvrm_in_isr` は `rm_isr` の間だけ真。

## 未対応（`os_stubs.c`）

どれも「未対応の値」（`NV_ERR_NOT_SUPPORTED`、`NV_FALSE`、何もしない）を返す。理由は種類別。

- **Tegra／SoC**（クロック、I2C、DCE、IMP、BPMP、輝度）：PCI の GPU ではない。
- **ACPI メソッド**（`_DSM`、`_DDC`、`_DOD`、`_ROM`、MUX、電源）：アプリから AML を実行する手段が無い。
- **NUMA・dma-buf・外部scatter-gatherの取り込み・任意ユーザーページの固定・P2P API**：クライアントのメモリはnative共有オブジェクトで渡す。IOMMU隔離はカーネルのPCI/DMA経路が担当し、nvrmが独自のIOMMU表を作らない。

PCI claimは既定でDMA隔離を必須とする。管理者が`appdrivers-unisolated`を明示すると非隔離利用を許すが、
全物理メモリへ届く危険がある。異常終了したGPUの非隔離再claimは許可しない。
`N_PCI_NEEDS_RECOVERY`がある場合、nvrmは未回復をログに残してRMの初期化へ進む。
起動画面GPUのgeneric FLRやscanoutの再建を行ったという意味ではなく、実GPUの回復は別途受入が必要。
- **仮想化**（vGPU、GRID、VFIO）、**cgroup**、**IMEX チャンネル**、**能力ファイル**（`nvidia_caps_root` は NULL）。
- **電源管理**（ランタイムサスペンド、S0ix、動的電源）：未対応。`nv_revoke_gpu_mappings` は、fault 時復元を伴う一時失効を提供できないため、状態・既存写像を変えず `NV_ERR_NOT_SUPPORTED` を返す。永久取消で代用しない。`safe_to_mmap` は独立した新規写像の許可状態で、取消完了の報告とは混同しない。
- **イベント**：`nv_post_event` はクライアント（`struct nvrm_file`）ごとのキューに積み、`nv_get_event`（`NV_ESC_RM_GET_EVENT_DATA`）で取り出す。プロトコルは要求・応答なのでクライアントがポーリングし、プッシュ通知は無い。`nv_get_file_private` は無し（記述子が無い）。
- **ファイル**（`os_allocate_temporary_file` など）：ページアウト用。無い。

## 既知のリスク・不確かなこと

- 4 KiB を超えるページサイズの連続確保は、余分に取って整列する。非連続の大きなページも連続を強いるので、大きな確保は失敗しうる。
- RM はクライアントの写像を、map が返した番地だけで探す（unmap、RM 自身の失敗時の後始末、番地を含む写像の検索）。bus 番地を返すと同じ範囲の二つの写像が同じ名になり、片方の unmap が生きている他方を失効させる。そこで写像ごとに 64 ビット空間の上半分（bus 番地の無い所）から重ならない番地の範囲を割当て、それを RM の番地にする（`nvrm_user_mapping_new`、system memory は `nv_alloc_user_mapping`、BAR は `osMapPciMemoryAreaUser` の `NVRM_NOCTURNE` 部）。範囲は再利用しない（nvrm の起動から 2^63 バイト分で尽き、尽きたら map は失敗してログに出る）。
- 再配置可能な BAR（Resizable BAR）で BAR1／BAR2 が 16 GiB のとき、カーネル側の写しが失敗しうる。
- 非連続 BAR range は順序を保持して native `n_shared_compose` で一つの VA へ並べる。範囲数に人工的な上限は無く、ページ境界・整数幅・実資源を検証する。各範囲の cache、RO 上限、source view の取消、PCI claim の寿命を合成・派生 view へ引き継ぐ。
- `os_is_efi_enabled` は常に真。`os_disable_console_access` は何もしない（表示の乗っ取りは GSP 初期化の間にありうる。D8）。
- memory BAR の有効索引と64bit幅から RM aperture を割当てる。register／doorbell は UC、prefetchable な FB／IMEM は RM の WC 要求を尊重する。RM が設定する USERD 範囲と重なるページは UC、`rm_disable_iomap_wc()` は user／kernel 両経路へ反映する。BAR0 全体の UC 写像と矛盾する cache alias は作らない。
- user-map ごとの RM private token を保持し、明示／暗黙 unmap と close で当該 view と派生写像を失効する。
  RM 自身の処理中にも token を作成でき、IPC request 不在や一時的な写像禁止をメモリ不足として報告しない。
- NVK の BAR1 読み戻し領域は nnrm／VAspace 単位で共有し、channel が寿命参照を保持する。owner を先に閉じても生存 channel から参照でき、最後の参照で解放する。読み戻し→GPPut→fence→doorbell の順序は保持する。実機での速度改善は未測定。
- `cnd_timedwait` は `TIME_UTC` の期限で待つ。Nocturne の `TIME_UTC` は戻らない（libc の `time.c`）ので、壁時計が進められても待ちが早く終わるだけで、タイマーの線は単調時計で測り直す。
- RM の呼出しの深さに対し、スレッドのスタックが 256 KiB で足りるかは未確認。
- 実機でのファームウェア起動・割り込み・ブートの結果は未確認。
