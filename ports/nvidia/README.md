# ports/nvidia：NVIDIA RM を Nocturne のドライバーアプリ `C:\Nocturne\System\nvrm` にする

RTX 4070（AD104、Ada）を、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM（`third_party/nvidia-open`）で
動かすための層。台帳は `docs/nvidia/LEDGER.md`。**Nocturne は Unix でも POSIX でもない。** この層は
Nocturne のネイティブ ABI（`n_*`）と ISO C（`malloc`、`<threads.h>`、`timespec_get`）だけで書いてあり、
`fork`・`ioctl`・`mmap` などの互換層は無い。修理は OS 層へ統合し、RM 本体の変更は既存の MIT
`arch/nvalloc/unix/src/os.c` の Nocturne 条件付き user-map 寿命コールバックに限定する。

状態：RM、OS 層、クライアントと NVK の接続がある。2026-10-20 の再監査修理は
`docs/nvidia/review-87dd1e2f-map.json` で責務と検証範囲を管理する。ホスト試験は native サービスを mock にするため、
その成功から PCI、GPU DMA、キャッシュ、GSP 起動の成功を推論しない。実機（RTX 4070）では 2026-10-25 に、
モード設定（280 Hz まで）と Zink＋NVK による WebGL が動いた（台帳の実機 11 回目）。

## 構成

| ファイル | 役目 |
|---|---|
| `nvrm.mk` | RM と OS 層のビルド規則（トップの Makefile から `include` する。変数と規則だけで、既定の目標は変えない） |
| `nvrm_os.h` | OS 層の各ファイルが共有する宣言（デバイス、ログ、スレッド局所の状態、クライアント側のマッピング） |
| `os.c` | ログ、メモリ、文字列、時計・待ち、ロック、作業キュー、RC／nano タイマー、ファームウェア読込み、レジストリ（os-registry.c：`nv_parms` の表を RM へ書く、装置ごとの一覧、除外する GPU） |
| `os_pci.c` | PCI 設定空間、BAR、DMA メモリ（`nv_alloc_pages`）、カーネル／クライアント向けマッピング、MSI／MSI-X、GPU の情報のログ（nv-procfs.c の information／power。`nvrm_log_gpu_information`） |
| `os_ioctl.c` | nv.c の要求の受け口：全エスケープの引数の大きさの検査と、RM でないエスケープ（カード情報、GPU の結付け、版の照合、システム引数、NUMA、デバイス割り込み、開き終わりの状態）。`nvrm_gpu_get／put`（nvidia_dev_get／put） |
| `rm_api.c` | リクエストサーバーが使う入口 `nvrm_rm_file_open／close／ioctl／mapping_object`（`os_ioctl.c` の後に `rm_ioctl` へ。GPU 専用のエスケープは GPU の `nv_state_t` で渡す） |
| `os_caps.c` | RM の能力（nv-caps.c と nvlink_caps.c の fabric management）。能力ファイルの代わりに、認証済みの接続の権利で mode を判定する |
| `os_acpi.c` | RM の ACPI メソッド（nv-acpi.c）と Notify。カーネルの PCI claim の ACPI ABI（`n_pci_acpi_eval`、`n_pci_acpi_notify_take`）で評価する。claim の外の名前は下の「未対応」。Notify の受取りスレッド（`nvrm_acpi_notify_start`／`_stop`、`nvrm.c` が呼ぶ）は RM が扱う値（0x81、0xC0、0xD1〜0xD5）だけを `rm_acpi_notify` へ渡し、ほかはログに書いて捨てる |
| `os_stubs.c` | Nocturne に対応が無い、または RM のこの構成で要らない `os_*`／`nv_*`（下の「未対応」） |
| `nvlink_os.c` | NVLink／NVSwitch ライブラリーが求める OS 関数（メモリ・文字列・時刻・識別） |
| `spdm_crypto.c` | libspdm の暗号バックエンド 71 個（libspdm_*.c の移植）。BearSSL で SHA-2、HMAC、HKDF、AES-GCM、ECDH／ECDSA（P-256／P-384）、RSA-PSS、X.509 の検証、乱数は `n_random` |
| `nvrm.c` | `main`、割り込みスレッド、起動手順 |
| `nvrm_serve.c／.h`、`libnvrm.c`、`include/` | リクエストサーバー、クライアント側ライブラリー、通信規約 |
| `nvkms.mk`、`nvkms_os.c／.h`、`nvkms_kapi_rm.c` | NVIDIA の画面設定ドライバー NVKMS（nvidia-modeset）を nvrm に入れるビルド規則、その OS 層と RM との接続、KAPI の memory の RM ハンドル（下の「NVKMS」「直接表示」） |
| `display.c／.h` | nvrm が画面を持つ：KAPI の唯一のクライアントとしてモニターの接続を張り直し、カーネルの画面を走査面へ移す。全画面のアプリの絵の直接表示（下の「画面」） |

起動手順（`nvrm.c`）：init が `N_KIND_PCI` をハンドル 3 で渡す → `n_pci_info` → `nv_state_t` を埋めて BAR0 を写す →
`NVRM_REGISTRY_DWORDS`（環境変数、任意）を `rm_parse_option_string` へ → `rm_init_rm` → `rm_is_supported_device`
→ `rm_init_private_state` → `rm_init_event_locks` → `rm_init_dynamic_power_management`
→ `rm_set_rm_firmware_requested` → 除外の確かめ → `rm_enable_dynamic_power_management` → 割り込みスレッド
→ `rm_init_adapter`（ここで GSP が起動する）→ ACPI の Notify の受取り（`nvrm_acpi_notify_start`）→
`rm_request_dnotifier_state` → `rm_get_device_name` をログへ → `nvrm_serve()`。
`rm_init_adapter` が失敗しても、診断のためにサーバーは動かし続ける（ログに失敗を残す）。
後始末（`tear_down`）は Notify の受取りを `rm_disable_adapter` より前に止める（上流の `nv_stop_device` と同じ順）。
`rm_cleanup_dynamic_power_management` は NVKMS と `rm_shutdown_adapter` の後、`rm_free_private_state` の前（上流の remove と同じ順）。
動的電源管理の三つは上流の probe／remove と同じ位置で呼ぶ。RM はここで電源管理の錠を作り、参照数を 1 にし、
`console_device` を起動画面（`nv_get_screen_info`）から決める。NVKMS が画面を取るときの VT 切替え
（`NV0080_CTRL_OS_UNIX_VT_SWITCH`）は、mode を問わず `console_device` が真ならこの錠を取るので、
初期化を飛ばすと NULL の錠で nvrm が落ちる（実機 4 回目、LEDGER C61）。`console_device` は移植側では設定しない。
RM は `nv_acpi_methods_init`／`_uninit` を `rm_init_adapter`／`rm_shutdown_adapter` の中で API ロックと GPU
ロックを持ったまま呼び、`rm_acpi_notify` は同じロックを取るので、そこでスレッドを join するとデッドロックしうる。
そのため受取りスレッドの開始と join は `nvrm.c` が RM のロックの外で行い、`nv_acpi_methods_init`／`_uninit` は
RM の取決めどおりメソッドの準備と片付けだけをする。
init／libnvrm／NVK は共通の PCI 選別条件（NVIDIA、VGA または 3D、progif 0）を使う。
NVK は生の PCI 件数を GPU 数にせず、HELLO で adapter 初期化成功を確認し、接続先 RM の probed ID と PCI 情報が一致する BDF の動的 snapshot を列挙する。probed ID だけでは初期化成功を保証しない。

装置ごとに `nocturne.nvidia.rm.<N_PCI_TARGETの4桁16進数>` を公開する。
`nvrm_connect_device()` は指定装置へ、従来の `nvrm_connect()` は動的 PCI 列挙から最初に接続可能な装置へつなぐ。
描画側の `nvrm_connect_ready()` は診断専用の未初期化装置を飛ばし、後続の ready 装置も探す。旧 HELLO の echo を ready と誤認しない。
`nvrm_connect_ready_status()` は同じ探索で、使える装置が無い理由も返す（nvrm がどの GPU も出していない `N_ERR_NOT_FOUND`、
最初の GPU が答えたが ready でない `N_ERR_NOT_READY`、または最初の GPU へつなげなかった理由）。Mesa の GL はこれで
softpipe へ後退した理由をシステムの記録へ書く。
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
$(PROGRAMS)/nvrm: $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) $(LIBC_OBJ) $(BR_OBJ) $(QJS_OBJ) $(LXB_OBJ) $(FF_OBJ) $(LIBC_RSP) user/user.ld
	@mkdir -p $(dir $@)
	@echo "  LD   $@"
	@$(LD) $(ULDFLAGS) --gc-sections $(BUILD)/u/user/libc/crt0.asm.o $(NVRM_OUT)/nv-kernel.o $(NVRM_OS_OBJ) $(NVKMS_LINK_OBJ) --start-lib @$(LIBC_RSP) --end-lib -o $@
```

`nvrm.mk` の注意：`srcs.mk`（RM のソース一覧の正本）を読むので、RM のバージョンを替えても一覧は追従する。
`LD` に渡す引数が長いので、`nv-kernel.o` は応答ファイル（`nv-kernel.o.rsp`）で部分リンクする。

## 未定義シンボルの内訳

RM のオブジェクト（`nv-kernel.o`）が必要とするが RM 自身は定義しない名前は **400 個**。すべて OS 層が定義する。

| 分類 | 個数 | どこで |
|---|---|---|
| `os_*`／`nv_*` のうち実装したもの | 166（`os.c` 96、`os_pci.c` 44、`os_caps.c` 7、`rm_api.c` 9、`os_acpi.c` 10） | 実装。下の対応表 |
| `os_*`／`nv_*` のうち未対応・中立の答え | 115 | `os_stubs.c`（下の「未対応」） |
| `nvlink_*`／`nvswitch_os_*` | 48 | `nvlink_os.c`（実装 48。GPU に NVLink／NVSwitch は無いので、DMA・イベント・fabric manager は未対応を返す） |
| `libspdm_*`（暗号バックエンド） | 71 | `spdm_crypto.c`（BearSSL で実装。既知の試験ベクターで `tests/nvrm_spdm_crypto_hosttest.c` が照合） |

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
| `os_flush_cpu_cache_all`／`write_combine_buffer`／`user_cache` | `os_flush_cpu_cache_all` は WB の DMA オブジェクトをすべて `clflush`（全 CPU のキャッシュに届き、GPU は IOMMU でこれらにしか届かない）。RM は DMA の能力を調べ終える前にファームウェアの写しで求める（実機で確認）。`user_cache` は空、WC バッファーは `sfence` |
| `os_get_random_bytes`、`os_get_version_info` ほか | `n_random`（システムの乱数）、版は "Nocturne OS" の固定値 |

スレッドの前提：割り込みスレッド（`n_pci_interrupt_take` → `rm_isr` → 必要なら `rm_isr_bh`）、ACPI の Notify の
受取りスレッド（`n_pci_acpi_notify_take` → `rm_acpi_notify`）とリクエストスレッド
は、**RM 自身のロック（GPU ロック、スピンロック＝mutex）だけで互いを守る**。nvrm はその外側に追加のロックを置かない。
`nvrm_in_isr` は `rm_isr` の間だけ真。

## kernel-open/nvidia の移植表

NVIDIA の Linux 用モジュール（open-gpu-kernel-modules 615.78.08 の `kernel-open/nvidia/`、58 個の .c、すべて MIT）の
各ファイルが Nocturne のどこにあるか。Linux の仕組み（デバイスファイル、procfs、kthread、`struct page`）は写さず、
RM が OS 層に求める約束（`os_*`／`nv_*`／`nvlink_*`／`libspdm_*`）と、nv.c が自分で答える要求を移す。

状態の意味：**移植済み**＝元の働きが Nocturne の方法である。**一部**＝働きの一部が無い（理由を書く）。
**未移植**＝必要だが無い（何が要るかを書く）。**該当ハードウェア無し**＝x86 の dGPU（RTX 4070）の機械に存在しない
ハードウェア向け。**対象外**＝RM もアプリも使わない、Linux の他のドライバー向けの口（理由を書く）。

| 元のファイル | 役目 | Nocturne での置き場（関数） | 状態 |
|---|---|---|---|
| `nv.c` | モジュールの本体：制御デバイスと GPU デバイスの開閉、要求の受け口、GPU の get／put、カード情報、GPU の結付け、版の照合、NUMA、イベント、割り込み | `os_ioctl.c`（`nvrm_nv_ioctl`：全エスケープの大きさの検査、`CARD_INFO`、`ATTACH_GPUS_TO_FD`、`CHECK_VERSION_STR`、`SYS_PARAMS`、`NUMA_INFO`、`SET_NUMA_STATUS`、`QUERY_DEVICE_INTR`、`WAIT_OPEN_COMPLETE`、`nvrm_gpu_get／put`）、`rm_api.c`（`nvrm_rm_file_open／close／ioctl`、`nv_post_event`／`nv_get_event`。`ALLOC_MEMORY`／`I2C_ACCESS` は GPU の状態で RM へ）、`nvrm.c`（起動、割り込みスレッド） | 移植済み。`IOCTL_XFER_CMD` は該当しない（通信規約が引数全体を運ぶので `NOT_SUPPORTED`） |
| `nv-pci.c` | PCI の probe／remove、BAR、バスマスター、電源 | `nvrm.c` の起動手順、`os_pci.c`（`nvrm_pci_attach`、`nvrm_pci_bus_master`、`nvrm_pci_detach`） | 移植済み。Grace の C2C／EGM／NUMA の部分は該当ハードウェア無し。ランタイム電源管理は下の「未対応」 |
| `nv-pci-table.c` | 引き受ける PCI 装置の表 | init／libnvrm／NVK の共通の選別（NVIDIA、VGA か 3D、progif 0）と `rm_is_supported_device` | 移植済み |
| `nv-msi.c` | MSI／MSI-X の有効化 | `os_pci.c`（`nvrm_irq_enable`：`n_pci_interrupts`、`NV_FLAG_USES_MSI(X)`） | 移植済み（ベクター 1 本） |
| `nv-acpi.c` | GPU の ACPI メソッド（`_DSM`、`_ROM`、`_DOD`、`_DDC`、MUX）と Notify | `os_acpi.c`（全メソッド。`nv_acpi_register_notifier`／`unregister_notifier` は `nvrm_acpi_notify_start`／`_stop`） | 移植済み：GPU の装置・その子・上流のブリッジのメソッドと Notify は、カーネルの PCI claim の ACPI ABI（`n_pci_acpi_eval`、`n_pci_acpi_notify_take`）で評価する。カーネルが断る ACPI のメソッド（ABI 文書 §9。電源・取出し・`_OSC` など）は RM のどのメソッドにも当たらない。claim の外（`_PSR`、電池、`\_SB.NPCF`、NVIF／WMMX）は下の「未対応」 |
| `nv-caps.c` | 能力（MIG、fabric、profiler など）の木と、その証明 | `os_caps.c`（`os_nv_cap_*`。能力ファイルの代わりに、システムが認証した接続の権利で mode を判定） | 移植済み |
| `nvlink_caps.c` | NVLink の能力ファイル（fabric-mgmt） | `os_caps.c`（`nvlink_fabric_mgmt`、RM の FM session） | 移植済み |
| `nv-caps-imex.c` | IMEX チャンネル（複数ノードの NVLink のメモリ共有） | `os_stubs.c`（`os_imex_channel_get` は −1、`count` は 0） | 該当ハードウェア無し（複数ノードの NVLink 構成） |
| `nv-cray.c` | Cray XT／XE の機械へ誤りを渡す | 無し（RM は呼ばない） | 該当ハードウェア無し |
| `nv-dma.c` | IOMMU による DMA の写し、他の GPU の BAR への写し | `os_stubs.c`（`nv_dma_map_*`）。`nv_requires_dma_remap` が偽なので RM の `iovaspace_id` は NONE になり、`osIovaMap` は呼ばれない。IOMMU 隔離はカーネルの PCI／DMA 経路が受け持ち、`n_dma_addresses` がバス番地を返す | 対象外（IOMMU はカーネル）。他の GPU との peer は該当ハードウェア無し（GPU 1 枚） |
| `nv-dmabuf.c` | RM のメモリを dma-buf として書き出す | `os_ioctl.c`（`EXPORT_TO_DMABUF_FD` は `params->status = NOT_SUPPORTED`）、取り込みは `os_stubs.c` | 未移植：通信規約（`include/nvrm_protocol.h`）が共有オブジェクトを添えるのは `MAP_MEMORY` の応答だけなので、書き出したものをクライアントに渡せない。規約の拡張が要る |
| `nv-i2c.c` | GPU の I2C ポートを Linux の I2C 層へ登録する、Tegra の I2C | `os_stubs.c`（`nv_i2c_add_adapter` は NULL、transfer／bus_status は Tegra） | 対象外：Nocturne に I2C 層が無い。アプリは `NV_ESC_RM_I2C_ACCESS`（GPU の状態で RM へ）で GPU の I2C に届く |
| `nv-kthread-q.c` | カーネルスレッドの作業キュー | RM は使わない（RM の作業は `os.c` の `os_queue_work_item`）。UVM／NVKMS の分は各担当 | 対象外（RM） |
| `nv-memdbg.c` | DEBUG ビルドの確保の追跡 | 無し（RM は呼ばない） | 対象外 |
| `nv-mmap.c` | クライアントへの写像（mmap） | `rm_api.c`（`nvrm_rm_mapping_object`）、`os_pci.c`（`nvrm_mapping_view`、`nvrm_bar_view`、`nvrm_alloc_view`） | 一部：`nv_revoke_gpu_mappings`（電源管理で写像を一時的に失効し、fault で戻す）は `NOT_SUPPORTED` |
| `nv-nano-timer.c` | ナノ秒のタイマー | `os.c`（`nv_create_nano_timer` ほか、タイマースレッド） | 移植済み |
| `nv-p2p.c` | 他社のカーネルドライバー（GPUDirect RDMA の NIC）向けの P2P API | 無し | 該当ハードウェア無し（相手の NIC とそのドライバーが無い） |
| `nv-procfs.c` | `/proc/driver/nvidia` の情報、レジストリ、版、suspend／unbind_lock | `os_pci.c`（`nvrm_log_gpu_information`：information と power をログへ）、`os.c`（与えられた NVreg の文字列をログへ）、版は `NVRM_FIRMWARE_VERSION` | 一部：proc ファイルシステムは無く、読む代わりにログに出す。`suspend`／`unbind_lock` の書込み口は電源管理・装置の切離しで未対応 |
| `nv-report-err.c` | 他のカーネルモジュールが誤りの通知を登録する | 無し（RM は呼ばない） | 対象外 |
| `nv-rsync.c` | Mellanox の rsync ドライバーとの同期（GPUDirect） | 無し | 該当ハードウェア無し |
| `nv-usermap.c` | クライアント写像の番地 | `os_pci.c`（`nv_alloc_user_mapping`、`nv_free_user_mapping`、`nv_add_mapping_context_to_file`） | 移植済み |
| `nv-vm.c` | DMA に使う system memory の確保 | `os_pci.c`（`nv_alloc_pages`／`nv_free_pages`：`n_dma_create`） | 移植済み |
| `nv-vtophys.c` | カーネル番地から物理番地 | 無し（RM は呼ばない） | 対象外 |
| `os-interface.c` | `os_*` の大部分（メモリ、時計、ロック、文字列、CPU） | `os.c`、`os_pci.c`、`os_stubs.c` | 移植済み（該当の無いものは下の「未対応」） |
| `os-mlock.c` | クライアントのページを固定して DMA に使う | `os_stubs.c`（`os_lock_user_pages` ほか `NOT_SUPPORTED`） | 未移植：他のアプリのページを固定する ABI が無い。クライアントのメモリは native の共有オブジェクトで渡す。ホストのメモリを GPU に見せる機能（Vulkan の外部ホストメモリ）にはカーネルの ABI が要る |
| `os-pci.c` | `os_pci_*` | `os_pci.c`（`n_pci_config_read／write`） | 移植済み |
| `os-registry.c` | RM のレジストリ（`NVreg_*`） | `os.c`（`os_registry_init`：`nv_parms` の表を RM へ書く、`nvrm_registry_per_device`、`nvrm_gpu_excluded`）。`NVreg_RegistryDwords` は `nvrm.c` の `NVRM_REGISTRY_DWORDS` | 移植済み（装置ごとの一覧と除外は `nvrm.c` からの呼出しが要る） |
| `os-usermap.c` | 写像の番地の照合 | `os_pci.c`（`os_match_mmap_offset`） | 移植済み |
| `nvlink_linux.c` | NVLink ライブラリーの OS 関数と、NVLink のデバイスファイル | `nvlink_os.c`（`nvlink_*`） | 移植済み（OS 関数）。デバイスファイルと fabric manager は該当ハードウェア無し（RTX 4070 に NVLink は無い） |
| `linux_nvswitch.c`、`i2c_nvswitch.c`、`procfs_nvswitch.c` | NVSwitch のドライバー | `nvlink_os.c`（RM が組み込む NVSwitch ライブラリーの `nvswitch_os_*`） | 該当ハードウェア無し |
| `libspdm_aead.c`、`libspdm_aead_aes_gcm.c`、`libspdm_ec.c`、`libspdm_ecc.c`、`libspdm_hkdf.c`、`libspdm_hkdf_sha.c`、`libspdm_hmac_sha.c`、`libspdm_internal_crypt_lib.c`、`libspdm_rand.c`、`libspdm_rsa.c`、`libspdm_rsa_ext.c`、`libspdm_sha.c`、`libspdm_shash.c`、`libspdm_x509.c` | libspdm の暗号 | `spdm_crypto.c`（71 個、BearSSL と `n_random`） | 移植済み。NVIDIA 版が持たないもの（ECDSA の署名、RSA-PSS の検証など）は同じく false |
| `nv-bpmp.c`、`nv-clk.c`、`nv-dsi-parse-panel-props.c`、`nv-gpio.c`、`nv-host1x.c`、`nv-imp.c`、`nv-ipc-soc.c`、`nv-platform.c`、`nv-platform-pm.c`、`nv-backlight.c` | Tegra（SoC）の表示・クロック・GPIO・電源・輝度 | `os_stubs.c`（`NOT_SUPPORTED`） | 該当ハードウェア無し（PCI の dGPU ではない） |
| `nv-modeset-interface.c` | RM と NVKMS の間 | 下の「NVKMS」 | 担当外（NVKMS） |
| `nv_uvm_interface.c` | RM と UVM の間 | UVM の移植 | 担当外（UVM） |

`kernel-open/common/inc`：OS に依らないヘッダー（`nv.h`、`nv-ioctl*.h`、`nv-caps.h`、`os-interface.h`、`nvstatus.h` など）は
同じものが `third_party/nvidia-open/src` にあり、そちらを使う。Linux の仕組みのヘッダー（`nv-linux.h`、`nv-mm.h`、
`nv-pgprot.h`、`nv-lock.h`、`nv-time.h`、`nv-timer.h`、`nv-kref.h`、`nv-hash.h`、`nv-list-helpers.h`、`nv-pci*.h`、
`nv-procfs*.h`、`nv-kthread-q*.h`、`nv-proto.h`、`nv-msi.h`、`nv-memdbg.h`、`conftest.h`、`nv-retpoline.h`、`nv-rethunk.h`）は
写さず、その役目を `nvrm_os.h` が持つ。Tegra のヘッダー（`nv-platform.h`、`nv-tegra-clk.h`、`os_dsi_panel_props.h`、
`os_gpio.h`）は該当ハードウェア無し。`nv-dmabuf.h` は上の dma-buf。`nv-modeset-interface.h` は NVKMS、
`nv_uvm_interface.h`、`rm-gpu-ops.h` は UVM の担当。

## 未対応（`os_stubs.c`）

どれも「未対応の値」（`NV_ERR_NOT_SUPPORTED`、`NV_FALSE`、何もしない）を返す。理由は種類別。

- **Tegra／SoC**（クロック、I2C、DCE、IMP、BPMP、輝度）：PCI の GPU ではない。
- **claim の外の ACPI**：電源の状態（`_PSR`）、電池、`\_SB.NPCF`、NVIF／WMMX。カーネルの ACPI ABI は claim した機能の装置・その子・上流のブリッジにしか届かないので、`os_acpi.c` は `NV_ERR_NOT_SUPPORTED` を返す（RM はファームウェアにそのメソッドが無い機械として進む）。
- **NUMA・dma-buf・外部scatter-gatherの取り込み・任意ユーザーページの固定・P2P API**：dma-buf の書き出し（`NV_ESC_EXPORT_TO_DMABUF_FD`）は `os_ioctl.c` が `NOT_SUPPORTED` を返す（通信規約が共有オブジェクトを添えるのは `MAP_MEMORY` の応答だけ）。クライアントのメモリはnative共有オブジェクトで渡す。IOMMU隔離はカーネルのPCI/DMA経路が担当し、nvrmが独自のIOMMU表を作らない。

PCI claimは既定でDMA隔離を必須とする。管理者が`appdrivers-unisolated`を明示すると非隔離利用を許すが、
全物理メモリへ届く危険がある。異常終了したGPUの非隔離再claimは許可しない。
`N_PCI_NEEDS_RECOVERY`がある場合、nvrmは未回復をログに残してRMの初期化へ進む。
起動画面GPUのgeneric FLRやscanoutの再建を行ったという意味ではなく、実GPUの回復は別途受入が必要。
- **仮想化**（vGPU、GRID、VFIO）、**cgroup**、**IMEX チャンネル**（複数ノードの NVLink 構成）。能力は `os_caps.c` が接続の権利で判定する（ファイルは無い）。
- **電源管理**（ランタイムサスペンド、S0ix、動的電源）：未対応。動的電源管理の初期化・有効化・後始末は上流の位置で呼ぶが、
  `nv_dynamic_power_available` が偽なので mode は常に NEVER で、GPU を止めることは無い（上流のポートの `_PR3` も調べない）。`nv_revoke_gpu_mappings` は、fault 時復元を伴う一時失効を提供できないため、状態・既存写像を変えず `NV_ERR_NOT_SUPPORTED` を返す。永久取消で代用しない。`safe_to_mmap` は独立した新規写像の許可状態で、取消完了の報告とは混同しない。
- **イベント**：`nv_post_event` はクライアント（`struct nvrm_file`）ごとのキューに積み、`nv_get_event`（`NV_ESC_RM_GET_EVENT_DATA`）で取り出す。プロトコルは要求・応答なのでクライアントがポーリングし、プッシュ通知は無い。`nv_get_file_private` は無し（記述子が無い）。
- **ファイル**（`os_allocate_temporary_file` など）：ページアウト用。無い。

## NVKMS（nvidia-modeset）

NVIDIA の画面設定の中核（`third_party/nvidia-open/src/nvidia-modeset`：EVO／NVDisplay、DisplayPort ライブラリー、
HDMI パケット、EDID・タイミング、headsurface のシェーダー）を、Linux の別モジュール nvidia-modeset.ko ではなく
nvrm の中に入れる。NVKMS は RM のカーネルクライアント（`rm_kernel_rmapi_op`）として GPU を使うので、
GPU を持つ nvrm の中にあるのが自然な置き場で、RM との間に通信は挟まない。

取り込み（上流 615.78.08 と同じ内容・同じ相対位置、変更なし）：`src/nvidia-modeset`（139 ファイル、
`Makefile`・`srcs.mk`・`exports_link_command.txt`・生成済みの `g_nvkms-evo-states.c` とシェーダーを含む）、
`src/common` の `displayport`・`modeset`（hdmipacket、timing）・`softfloat`・`unix`（nvidia-3d、nvidia-push、
headsurface、xzminidec、共通の utils）、`kernel-open/common/inc/nv-modeset-interface.h`。
ライセンスは MIT のほか、SoftFloat 3d（BSD-3）、xzminidec（パブリックドメイン）、`nvkms-pow.c` の fdlibm
（Sun の許諾文）で、本文は `third_party/nvidia-open/NOTICES` にある。GPL のファイルは無い。

### ビルド（`nvkms.mk`、`nvrm.mk` から `include`）

```
python scripts/build.py nvkms-objs        # NVKMS の中核と OS 層（build/nvkms/）
python scripts/build.py nvkms-undefined   # RM＋NVKMS＋両 OS 層の未定義シンボル（build/nvkms/undefined.txt）
```

- ソース一覧は上流の `srcs.mk`（C 184、C++ 40）をそのまま読む。旗は上流の `Makefile`／`utils.mk` に従い、
  カーネルモジュール専用のもの（`-mcmodel=kernel`、`-mno-sse`／`-msoft-float`、retpoline、`-fcf-protection`）を除いて
  nvrm と同じプログラムの旗（`-msse2`、フレームポインター）にする。C++ は上流どおり `-std=gnu++11 -fno-rtti
  -fno-exceptions`。DisplayPort ライブラリーは自前の `operator new` を持ち、C++ ライブラリーも静的初期化子も使わない。
- 上流は gcc で作る。clang だけが出す警告（入れ子の初期化子の括弧、加算だけの変数、xzminidec の
  `static inline __always_inline`、DisplayPort の `override` の付け方の不揃い）は、その 4 つだけを止めている（`NVKMS_CLANG_ONLY` ほか）。
- シェーダーは上流どおり `xz -C none` で縮め、`ld -r --format=binary` で `_binary_<arch>_shaders_xz_start／_end` にする。
- 全オブジェクトを `nv-modeset-kernel.o` に部分リンクし、`exports_link_command.txt` の入口（`nvKms*` の 23 個）以外の
  大域シンボルを局所化する。RM も同じ共通ファイル（`nvstatus.c` など）を持つので、名前が衝突しない。
  上流の `nvkms.ld` は使わない：GNU ld 用に関数ごとのセクションをまとめるもので、lld でそうすると C++ の COMDAT 群が
  壊れる。セクションを分けたままなので nvrm の `--gc-sections` も効く。
- トップの Makefile の nvrm のリンクに `$(NVKMS_LINK_OBJ)`（`nv-modeset-kernel.o`、`nvkms_os.o`）を加えた。

### OS 層の対応（`nvkms_os.c`）

`nvidia-modeset-os-interface.h` の関数すべてと、nvidia.ko 側の `nv-modeset-interface.c`（`nvidia_modeset_rm_ops_t`）の役目を
一つのファイルで持つ。RM の OS 層（`os.c`）にある物は使い回し、同じ物を作り直さない。

| NVKMS が呼ぶもの | Nocturne での実装 |
|---|---|
| `nvkms_call_rm` | `rm_kernel_rmapi_op(nvrm_dev.sp, ops)`。スタックは nvrm の共有のダミー（`NV_ENTER_RM_RUNTIME` はスタックを替えない） |
| `nvkms_open_gpu`／`close_gpu` | `nvidia_dev_get／put` の移植。`nvrm_dev` の `gpu_id` で、`adapter_ready` かつ登録済みのときだけ成功。reset aware でなければ `rm_set_external_kernel_client_count` |
| `nvkms_enumerate_gpus` | 登録済みなら nvrm の GPU 1 台（PCI 位置、`is_soc_disp`。NUMA の設定は不要、`os_device_ptr` は NULL） |
| `nvkms_allow_write_combining` | 真（BAR は `N_CACHE_WRITE_COMBINE` で写せる。Linux の x86-64 と同じ） |
| `nvkms_alloc`／`free`、`mem*`、`str*`、`*snprintf` | libc。`fail_malloc`／`malloc_verbose` も上流どおり |
| `nvkms_usleep`、`nvkms_get_usec`、`nvkms_yield`、`nvkms_dump_stack` | `os_delay_us`（1 ms 未満は回って待つ）／`os_delay`（ms、0xFFF で頭打ち）、`nvrm_now_ns`、`os_schedule`、`os_dump_stack` |
| `nvkms_copyin`／`copyout` | `os_memcpy_from_user`／`to_user`（クライアントの引数の場の番地だけを読み替える） |
| `nvkms_log` | `nvrm_log`（`nvidia-modeset: ` ＋水準＋GPU の前置き） |
| `nvkms_sema_*`、NVKMS の大域ロック、電源管理ロック | `os_alloc_semaphore`（1）、`os_alloc_rwlock`（読み手＝各入口、書き手＝サスペンド） |
| タイマー（`nvkms_alloc_timer`、`_with_ref_ptr`、`nvkms_free_timer`）、`ref_ptr`、KAPI のイベント通知 | NVKMS の作業スレッド 1 本（Linux の `nvkms_kthread_q`）が、期限の来たタイマーと通知を順に、大域ロックの下で実行する |
| `nvkms_open_from_kapi`／`close_from_kapi`／`ioctl_from_kapi`（`_try_pmlock`） | 上流どおり（close は通知を取り下げ、走っている通知を待ってから解放） |
| 設定 | 起動設定 `NVKMS_OPTIONS`（`name=value;...`）。名前と既定値は Linux のモジュール引数と同じ（`output_rounding_fix=1`、`disable_hdmi_frl=0`、`disable_vrr_memclk_switch=0`、`hdmi_deepcolor=1`、`max_output_color_depth=10`、`opportunistic_display_sync=1`、`debug_force_color_space=0`、`enable_overlay_layers=1`、`fail_malloc=-1`、`malloc_verbose=0`、`force_frl_rate=0`、`conceal_vrr_caps=0`、`enhanced_pcon_support=0`、`fail_alloc_core_channel=-1`、`debug=0`、`config_file`＝無し） |
| `config_file`（`nvKmsReadConf`） | `n_document_load`。EDID の上書きファイルも同じ関数で読む（8192 バイトまで） |

作業スレッドを RM の作業キュー（`os.c`）に載せないのは、あちらが RM の作業項目（`rm_execute_work_item`）を最大 8 本で
並行に走らせるのに対し、NVKMS の項目は互いに追い越してはならず、RM のキューにも入れられないため（上流でも
RM と NVKMS は別々のキューを持つ）。汎用の `nv_kthread_q` 相当が OS 層にできれば、両方をそれに載せられる。

nvrm の中の入口（`nvkms_os.h`）：`nvrm_nvkms_start()` を `rm_init_adapter` 成功（`adapter_ready`）の後に、
`nvrm_nvkms_stop()` を終了時の `rm_disable_adapter` の前に呼ぶ。KAPI の関数表は `nvKmsKapiGetFunctionsTable`
（`versionString` に `NV_VERSION_STRING` を入れて渡す）で得る。`nvrm_nvkms_suspend`／`resume` は nvrm にサスペンドの経路が
できたとき用。

### 無いもの（Nocturne に該当が無い）

- **procfs**（`nvKmsGetProcFiles`）：Linux の `/proc/driver/nvidia-modeset`。呼ばない。
- **バックライト**（`nvkms_register_backlight`）：Linux の backlight クラス。NULL（＝装置無し）を返す。
  上流でも `CONFIG_BACKLIGHT_CLASS_DEVICE` の無いカーネルでは同じ。デスクトップの RTX 4070 には内蔵パネルも無い。
- **Tegra の syncpoint**（`nvkms_syncpt_op`、`nvkms_kernel_supports_syncpts`）、**Android の extcon**：偽／何もしない。
- **ユーザー空間のクライアント**（`/dev/nvidia-modeset`、`nvkms_get_per_open_data`、`nvkms_fd_is_nvidia_chardev`）：
  nvrm に NVKMS 用のクライアントの通り道がまだ無いので、NULL／偽を返す。nvrm の中の利用者は KAPI を使う。

未確認：NVKMS は実機でもホスト試験でも動かしていない（ビルドと未定義シンボルの照合だけ）。

## 画面（nvidia-drm に当たるもの）

`display.c` が NVKMS の KAPI の唯一のクライアントになり、起動時に GOP が点けていたモニターのモードをシステムへ知らせ、
システムが選んだモード（モニターの推奨モード、または設定アプリで利用者が選んだもの）を GPU 側で設定して、
カーネルの画面（WM が描く面）を GOP の面からそのモードの大きさの VRAM の走査面へ移す。モードを選ぶのはシステムと利用者で、
ドライバーは列挙・検証・設定だけをする（`docs/nocturne-native-abi.md` §9 の「モニターのモード」）。nvidia-drm の DRM の構造（drm_device、crtc、
plane、connector のオブジェクトや atomic の状態）は真似ず、KMS の論理だけを移した。入口は `display.h` の
`nvrm_display_start()`／`nvrm_display_stop()` で、`nvrm.c` が `nvrm_nvkms_start()` の成功後と `nvrm_nvkms_stop()` の前に呼ぶ。
失敗しても RM の提供は続け、画面は渡さない（GOP のまま）。理由は `NVRM display:` の行でログに出る。

### 立ち上げ（`nvrm_display_start`）

1. 起動画面が NVIDIA の BAR に無い（`framebuffer_bar` が無い）なら、NVKMS に触らずにやめる。
2. `nvKmsKapiGetFunctionsTable` → `allocateDevice`（`nvrm_dev.nv.gpu_id`、MIG 無し、イベントは `kapi_event`）→
   `grabOwnership` → `declareEventInterest`（DPY_CHANGED と DYNAMIC_DPY_CONNECTED）→ `getDeviceResourcesInfo`。
   NVKMS がコンソールの面（RM の予約コンソールメモリー）を取り込めたかもログに出す。
3. 起動ディスプレイ：RM のカーネルクライアントで `NV0073_CTRL_CMD_SYSTEM_GET_BOOT_DISPLAYS` を読む（root → device →
   `NV04_DISPLAY_COMMON` → 制御 → client を解放）。KAPI のディスプレイのハンドルは dpyId のビットなので、それと照らす。
4. `getDisplays` → 各ディスプレイの `getConnectorInfo`／`getStaticDisplayInfo`／`getDynamicDisplayInfo`。全部をログに出し
   （コネクター、head、接続、EDID の製造者・製品・名前、起動時に点いていたか）、起動ディスプレイのうち繋がっている最初のもの
   （無ければ繋がっている最初のもの）を選ぶ。head はそのディスプレイの `headMask` の最下位ビット。
5. カーソル：表示エンジンが 32×32 の A8R8G8B8（非 premultiplied のアルファ）のカーソルと、主層の X8B8G8R8 を扱えれば、
   pitch linear の vidmem（pitch は NVKMS の 32×32 の最小の 256）を取り、写して `createSurface` する。直接表示（下）で
   アプリの絵の上にポインターを出すのに使う。作れなければ直接表示は無し（合成のみ）。
6. モードの提示：`getDisplayMode` で列挙し、有効で、interlaced／doubleScan でなく、表示エンジンの面の最大
   （`maxWidthInPixels`／`maxHeightInPixels`）に収まるものを、同じ幅・高さ・リフレッシュは一度だけ、EDID の推奨モードに
   `N_SCREEN_MODE_PREFERRED` を付けて、`n_pci_screen_modes` でシステムへ出す。モニターの識別子は EDID の製造者・製品・
   製造番号（例 `ACR0A2B-12345678`）、名前は EDID のモニター名。出せるモードが無ければ渡さない。
7. システムが望むモード（`n_pci_screen_state` の `mode`、`mode_serial`）をすぐ読み、出す（`mode_follow_now`。望みが変わり
   続ける間 4 回まで）。`validateDisplayMode` で確かめ、大きさが今の面と違えば、その大きさの面を作る：pitch を NVKMS の
   pitch の整列に揃えた pitch linear の X8R8G8B8 を `allocateMemory`（vidmem、SCANOUT、noDisplayCaching）→
   `mapMemory`（KERNEL）→ view → `createSurface`。最初の面が起動画面と同じ大きさで 32 bpp なら GOP の画面を行ごとに写し、
   それ以外は黒。リフレッシュだけの変更は同じ面のままモード設定だけをする。
8. `applyModeSetConfig`（commit=FALSE で検査 → 新しい面を `n_pci_give_screen` で渡す → TRUE で確定 → 前の面を解放）。
   システムは head がそのモードを出す時には新しい大きさで描いている。確定に失敗すれば head は前のものを出し続け、
   前の面を渡し直す（面がまだ無ければ起動画面を返す）。どの失敗でも解放した memory へシステムが描くことはない。
   出したモード（出せなければ head が出しているもの）を `done`＝`mode_serial` で知らせ直す。望まれたものと違えば、
   システムはそのモードを拒否として覚え、次の候補を望む。モード設定も、デスクトップの面を主層へ載せるフリップとして
   その完了を待つ（下）。
   フリップの完了：KAPI の関数表の `getFlipPendingStatus` は `nvKmsKapiGetFunctionsTable` が埋めない（NULL）。
   そこで `nvkms_kapi_rm.c` が KAPI の通知面を読む：層に面を載せる commit ごとに KAPI はその層の二つの完了通知の片方
   （`layerState[head][layer].currFlipNotifierIndex` の次）を名指し、表示エンジンがフリップの完了でそこへ「完了」を書く。
   KAPI は名指す前に通知を消さないので、二つ前のフリップの書込みが残っている。`nvrm_nvkms_flip_arm` が commit の直前に
   その通知を「未開始」へ戻し、`nvrm_nvkms_flip_done` が commit の後で読む（`nvKmsResetNotifier`／`nvKmsParseNotifier`）。
   KAPI は面を載せる commit なら、head を点けるモード設定でも通知を名指す（`nvkms-kapi.c` がフリップの要求を作るとき、
   `surfaceChanged` かモード設定なら通知を指定し、面があれば通知の番号を進める。NVKMS は `nvkms-hw-flip.c` の
   `nvAssignCompletionNotifierEvoHwState` で表示エンジンに書かせる）。だから待つのは面を載せるすべての commit
   （モード設定、面の差し替え、失った head へデスクトップを戻すフリップ）で、面を外す commit と失敗した commit は待たない。
   待ったフリップが 3 秒（`FLIP_TIMEOUT_MS`）で完了しなければ、表示エンジンが nvrm の面を出していない（何も出ていない
   かもしれない）とみなし、画面を手放す（下の「手放す」）。その前に、最後の commit が名指した通知の番号と、二つの通知の
   生の 4 語（語 0 が状態、語 2・3 が時刻）と、commit に NVKMS がかかった時間をログに出す（`nvrm_nvkms_flip_words`）。
   語が「未開始」（0 と時刻の無効値）のままなら表示エンジンは書いておらず、「完了」なら nvrm の読み方の誤り。
   0a1d812a は「点けるモード設定では通知が書かれない」としてこの待ちを外し、待てなかったフリップの後も画面を渡したままに
   した。その根拠は NVKMS のソースと合わず（上）、実機 6・7 回目はモニターが暗いまま戻らなかった（5 回目は待ちが失敗し、
   手放して起動画面が出ていた）。この待ちと手放しを戻した。
9. 渡すのは `n_pci_give_screen(view, {幅, 高さ, pitch, XRGB8888 | N_SCANOUT_DIRECT, offset})`。カーソルが無いか、システムが
   `N_SCANOUT_DIRECT` を断れば、カーソルを解放して `N_SCANOUT_DIRECT` 無しで渡し直す（直接表示は無し、合成のみ）。
   head を点けたら `registerVblankIntrCallback`。
10. 画面の状態を追う状態スレッド（システムが望むモードと直接表示。常に起動）→ 抜き差しを追う作業スレッドを起動する。
   状態スレッドが起動できなければ、モードはそのまま、直接表示もやめる。立ち上げ中に画面を手放していた（上の 8）なら、
   状態スレッドが無いと次の望みで取り戻せないので、その時は立ち上げを失敗にする。

設定アプリの「Resolution and refresh rate」で選んだモードは試行として出し、15 秒以内に Keep を押さなければ前のモードへ戻る。
Keep したモードはモニターごとに `desktop.conf` に保存し、次の起動でも推奨モードの代わりに使う。

BAR1 の offset：RM のカーネル写像は `os_map_kernel_space` が BAR の一部を写したもの。`os_pci.c` が写像ごとに bus 番地を
覚えておき、`nvrm_kernel_mapping_phys()`（`os_pci.c` の末尾）が写像の番地から bus 番地を返す。面の先頭と末尾がともに同じ写像の
中で連続していることを確かめ、`nvrm_bar_view`（写像の型 WC）で共有オブジェクトを作る。RM の写像と同じ範囲を WC で写すので、
同じページに違う型の写像を作らない（C42）。

垂直同期：NVKMS が head の vblank ごとに（nvkms の lock の下で）呼ぶコールバックで、渡している間だけ回数を進めて
`n_pci_screen_vblank(回数, 時刻)` を呼ぶ。重い処理はしない。時刻は NVKMS の値（無ければ `nvrm_now_ns`）。

抜き差し：イベントは作業スレッドへ起こすだけ。表示中のディスプレイが繋がったままなら、モードの一覧が変わったときだけ
出し直す（出しているモードが残っていればそのまま）。抜けたら vblank を外して head を止めるが、カーネルの画面は渡したままに
する（WM はそのまま面へ描き続ける）。次に繋がっているディスプレイのモードを出し、システムが望むモード（そのモニターの
推奨か利用者の選択）をその大きさの面で出す。どれも繋がっていなければモードを取り下げる（モード 0 件）。
画面を手放している間も同じように追い、モードを出し直した結果の望みで画面を取り戻す（下）。

### 手放す（`hand_back`）と取り戻す

`n_pci_give_screen(N_SCANOUT_BOOT)`（カーネルは GOP の面へ戻り、その大きさで描く。アプリの絵の非常画面用の保持も
カーネルが離す）→ vblank を外す → `releaseOwnership`（NVKMS が `nvEvoRestoreConsole` で起動画面を復元する。
コンソールの面が無いか復元に失敗したときは RM 側の復元に任せる）→ 直接表示をやめてカーソルを解放 → デスクトップの面を
解放。モードは取り下げず、状態スレッドと作業スレッドも続ける（設定アプリはモードを出し続け、`SCREEN_SOURCE_HANDED_BACK`
と示す）。待ったフリップが 3 秒で行われないとき（上の 8）に起きる。起動中でも `nvrm_display_start` は成功を返し、
画面は GOP のまま、次の望みを待つ。

取り戻す：手放した後に新しい `mode_serial` の望み（設定アプリで試すモード、試行の終わり、システムの次の候補、出し直した
モニターのモード）が来たら、`grabOwnership` → カーソル → そのモードの面を作って渡し、モード設定とフリップ（上の 7・8）→
アプリの絵を非常画面用にカーネルへ持たせ直す → 出したモードを知らせる。別の NVKMS クライアントが持っていれば何もせず
ログに残す。同じ望みを nvrm が自分から試し直すことはない。

カーネルへ「出せなかった」（`done`＝その望み、`shown` 無し）と知らせるか：`screenmode.c` の `screen_modes_offer` は、
それを受けるとそのモードを拒否し、試行中なら直前のモードへすぐ戻し、試行でなければ次の候補を望む。次の候補も出せなければ
また次へ進むので、フリップを全く行わない表示エンジンでは、モニターのモードを一つずつ 3 秒ずつ暗くして回ることになる。
そこで：

- 時間切れ（望みに従って手放した、または取り戻した後に出せなかった）は一度だけ知らせる。カーネルは試行なら戻し、
  そうでなければ次の候補を一度試させる。その後はモード設定のフリップが一度行われるまで知らせない（`unshown_said`）。
  画面は手放したまま、選ぶのは利用者。試行中のモードは `trial_watch` が 15 秒で戻す。

### 後始末（`nvrm_display_stop`）

直接表示の要求を止め、状態スレッドと作業スレッドを止める → モードを取り下げる（`n_pci_screen_modes` の 0 件。取り下げは
ここだけ）→ 手放す（上）→ 残っているアプリの絵を解放 → `declareEventInterest(0)` → `freeDevice`。その後 `nvrm.c` が
`nvrm_nvkms_stop()` を呼ぶ。

nvrm は終了時に起動画面へ戻すので、起動コンソールを捨てる `framebufferConsoleDisabled` は呼ばない。

### 直接表示（`NVRM_SCANOUT`）

システムがアプリの全画面の窓を合成せずに出してよいと言う間（`N_SCANOUT_DIRECT` 付きで渡した画面の状態
`n_pci_screen_state` の `app`）、そのアプリが GPU で描いた絵を head の主層へそのまま出す。規約は `nvrm_protocol.h` の
`NVRM_SCANOUT`、クライアント側は libnvrm の `nvrm_scanout_create`／`_destroy`／`_present`／`_import`。サーバーは
`nvrm_serve.c` が接続の RM ファイルと認証済みの principal を付けて `display.c` の `nvrm_display_scanout` へ渡す。

- **CREATE**：その時のデスクトップの大きさ・pitch の X8B8G8R8（pitch linear、vidmem、SCANOUT）を KAPI で取り、`createSurface`。
  後でモードの大きさが変わっても、絵は自分の大きさのまま、表示エンジンがモードの大きさへ拡大縮小して出す。他のアプリが
  使った VRAM の中身が見えないよう、黒で埋めてから返す。番号・幅・高さ・pitch・バイト数を返す。絵は接続とアプリのもの。
  さらに、RM が写した BAR1 の範囲の view を `n_pci_screen_image`（`N_SCANOUT_XBGR8888`）でカーネルに登録する。カーネルが
  止まると、表示エンジンは最後にフリップした絵を出し続け、デスクトップへ戻すフリップをする者はもういない。カーネルは
  パニック画面をデスクトップの面と登録された絵のすべてへ描く。BAR1 のひと続きの範囲でないか、登録を断られた絵は作らない
  （`NV_ERR_INSUFFICIENT_RESOURCES`）。パニック画面を出せない絵をフリップしないため。絵の解放では最初に
  `n_pci_screen_image_end` で登録を外し、それから写しと面と memory を解放する。`N_SCANOUT_DIRECT` 無しの渡し直しと起動画面へ
  戻すことでも、カーネルは登録をすべて外す。
- **IMPORT**：絵の memory をアプリの RM client のものにする。Nocturne には Linux の `exportMemory` の fd が無いので、NVKMS の
  RM client（`nvkms_kapi_rm.c` の `nvrm_nvkms_memory_handle` で KAPI の構造から読む）が、カーネル権限の `NV04_SHARE` で
  その memory を要求者が名指した client だけに DUP_OBJECT で共有し（RS_SHARE_TYPE_CLIENT）、アプリの**自分の** RM ファイルで
  `NV_ESC_RM_DUP_OBJECT` を行い、共有はすぐ取り消す。DUP は利用者権限のファイルの要求なので、RM が他の要求と同じく
  client がそのファイルのものかを確かめる（`rmclientValidate`）。
- **PRESENT**：最新の画面の状態を読み、そのアプリが直接表示されていなければ `NV_ERR_INVALID_STATE`。前のフリップの完了を
  待ってから、head の表示・モードはそのままで主層の面を替えるフリップ（commit のみの `applyModeSetConfig`）をする。待つのは
  一つ前のフリップだけで、GPU の描画の完了は待たない（アプリが描画を待ってから PRESENT する）。フリップの完了が 3 秒
  見えなければ画面を手放す（上の「手放す」）。取り戻すまで CREATE・PRESENT・IMPORT は `NV_ERR_NOT_SUPPORTED`（直接表示は
  無い）で、アプリは窓を通して描き、起動画面へ戻ったデスクトップがそれを合成する。取り戻した後は、手放す前の絵もカーネルに
  持たせ直してから再び出せる（持たせ直せなかった絵の PRESENT は `NV_ERR_INVALID_STATE`）。
- **カーソル**：絵を出している間は、システムのポインター（状態の 32×32 の画素、位置、hot spot）をカーソルの面へ写し、
  モードの画素に直した位置でカーソル層に出す。デスクトップの面にはポインターが描かれているのでカーソルは消す。
- **状態スレッド**：`n_pci_screen_state` で変化を待ち、システムが新しいモードを望めばそれを出し（上の 7〜8）、
  出しているアプリがもう直接表示されないならデスクトップの面へ戻し、そのままならカーソルを動かす。
- **DESTROY**・接続の終わり・stop：出ている絵はデスクトップへ戻すフリップの完了を、一つ前に出ていた絵はそのフリップの完了を
  待ってから `destroySurface`／`freeMemory` する。デスクトップへ戻すフリップが失敗したときは、NVKMS が面の破棄で絵を head
  から外す（head には面が無くなる）ので、絵を解放した直後と、以後の画面の状態の変化ごとに、デスクトップへ戻すフリップを
  やり直す（`scr.lost`）。他のアプリの絵は「無い」と同じ `NV_ERR_INVALID_OBJECT_HANDLE`。
- 抜き差しのモード設定はデスクトップの面を出す。head が無ければ PRESENT は断る。
- 排他：head を変えるもの（作業スレッド、状態スレッド、要求のスレッド）は `scr.head_lock` で順に行う。

試験：`python tests/nvrm_display_hosttest.py`（偽の KAPI・RM・ハンドルと、gui/screenmode.c の選び方を真似た偽のシステムで、
立ち上げと後始末の順序、システムへ出すモード（有効・順次走査・面の最大以内・一度ずつ・推奨の印・EDID によるモニターの識別）、
システムが望むモードを出すこと（大きさの変更は新しい面を検査と確定の間に渡し、前の面は後で解放。リフレッシュだけなら同じ面）、
出せないモードを知らせて拒否されること、確定の失敗で前の面へ戻すこと、利用者の選択から始めること、失敗時に
画面を渡さないこと、抜き差し（同じ head の点け直しでフリップの完了が見えないまま点いた head）、直接表示（作成とパニック画面の
登録、登録を断られたときと BAR1 のひと続きでないときに絵を作らないこと、許可の有無と PRESENT、一つ前の
フリップ待ち、commit の直前の完了通知の消去、失敗した commit の後は待たないこと、カーソル、デスクトップへの戻り、戻すフリップの
失敗からの回復、DESTROY・接続の終わり・stop の順序、IMPORT の持ち主と共有→DUP→取消、直接表示が無いとき、状態スレッドが
無く `N_SCANOUT_DIRECT` 無しで渡し直せないとき、フリップの完了が見えなかった後に待たず直接表示を断ること）を確かめ、
製品の `display.c` を対象の設定でも構文検査する。偽の KAPI は実機の規則（通知を書くのは commit の前に面を出していた
点いた head の層だけ）で通知を書く。
`tests/nvrm_servetest.c` は `NVRM_SCANOUT` が接続の RM ファイルと principal 付きで届くことと、
接続の絵が RM ファイルより先に終わることを確かめる（Nocturne 上で動く）。

### nvidia-drm の全 43 ファイルとの対応

状態：**済**＝同じ役目を実装した、**一部**＝画面を移すのに要る分だけ、**次**＝下の「次の段階」、**無し**＝Nocturne に該当が無い。

| 上流のファイル | 役目 | Nocturne での置き場 | 状態 |
|---|---|---|---|
| `nvidia-drm-linux.c` | モジュールの入口と出口 | `nvrm.c` の bring_up／tear_down | 済 |
| `nvidia-drm.c／.h` | KAPI の関数表を得て装置を探す | `display.c` の `nvrm_display_start`（`nvKmsKapiGetFunctionsTable`） | 済 |
| `nvidia-drm-drv.c／.h` | 装置の確保、所有、イベント、DRM の登録と ioctl | 確保・所有・イベント・解放は `display.c`。DRM の装置ノードは無く、要求は nvrm の `NVRM_SCANOUT`（fence は次） | 一部 |
| `nvidia-drm-priv.h` | 装置ごとの状態 | `display.c` の `scr` | 済 |
| `nvidia-drm-connector.c／.h` | 接続の検出、モードの列挙と検査、EDID | `choose_display`、`pick_mode`、`edid_text` | 済 |
| `nvidia-drm-encoder.c／.h` | ディスプレイと head の対応、抜き差しのイベント | `choose_display`、`kapi_event`、`rescan` | 済 |
| `nvidia-drm-crtc.c／.h` | head と層の要求、面、vblank、カーソル、CRC、色 | `head_request`／`primary_request`（PRIMARY 層）、`surface_create`、`vblank_on／off`、カーソル（`cursor_create`、`show`）。オーバーレイ・CRC は無し | 一部 |
| `nvidia-drm-modeset.c／.h` | 設定の確定、フリップ、表示のセマフォ | `apply`、`flip_completed`、フリップは `show`。セマフォは次 | 一部 |
| `nvidia-drm-fb.c／.h` | gem から走査面を作る | `surface_create`（デスクトップ）、`image_create`（アプリの絵、画面の大きさ） | 一部 |
| `nvidia-drm-format.c／.h` | DRM の画素形式と NVKMS の形式の変換 | デスクトップは X8R8G8B8（`N_SCANOUT_XRGB8888`）、アプリの絵は X8B8G8R8、カーソルは A8R8G8B8 | 一部 |
| `nvidia-drm-utils.c／.h` | コネクター情報、モードの変換 | `mode_text`。KAPI のモードをそのまま使うので変換は要らない | 済 |
| `nvidia-drm-color-pipeline.c／.h` | DRM の色処理の段 | 無し（csc は IDENTITY） | 次 |
| `nvidia-drm-helper.c／.h` | DRM の atomic の補助 | 無し（DRM の構造を持たない） | 無し |
| `nvidia-drm-gem.c／.h` | GEM の物体 | 無し | 次 |
| `nvidia-drm-gem-nvkms-memory.c／.h` | NVKMS の memory を GEM にする | アプリの絵（NVKMS の memory）をアプリの RM client へ DUP する `image_import` | 一部 |
| `nvidia-drm-gem-user-memory.c／.h` | ユーザーのページを GEM にする | 無し | 次 |
| `nvidia-drm-gem-dma-buf.c／.h` | dma-buf の取り込み | 無し | 次 |
| `nvidia-drm-fence.c／.h` | 描画完了のフェンス、セマフォ面 | 無し | 次 |
| `nvidia-dma-resv-helper.h` | dma-buf の予約オブジェクトの補助 | 無し | 次 |
| `nv_drm_common_ioctl.h` | nvidia-drm の ioctl の形 | nvrm の要求 `NVRM_SCANOUT`（`nvrm_protocol.h`） | 一部 |
| `nvidia-drm-os-interface.c／.h` | 確保、作業スレッド、タイマー、ユーザーのページ、sync_file | libc の確保と `threads.h`（`display_thread`）。ユーザーのページと sync_file は次 | 一部 |
| `nvidia-drm-conftest.h` | Linux カーネルの機能の判定 | 無し | 無し |
| `nv-kthread-q.c` | 作業キュー | NVKMS 側は `nvkms_os.c` の作業スレッド、画面側は `display_thread` | 済 |
| `nv-pci-table.c／.h` | 対応する PCI ID の表 | init／`nvrm.c` の共通の PCI 選別条件 | 済 |
| `nv_common_utils.h` | 共通のマクロ | 無し（標準の C で足りる） | 無し |

### 次の段階（Mesa が走査面へ直接描く）

直接表示（上）で、nvrm が作った走査面の絵へ NVK が描き、全画面のアプリの間はそれを合成せずに出せる。画面の持ち主は
カーネルの `N_SCANOUT_DIRECT` と `n_pci_screen_state` で決まる。残るもの：

- **fence に当たるもの**：今の PRESENT は一つ前のフリップを待つだけで、描画の完了はアプリが待つ。KAPI の
  `importSemaphoreSurface`／`registerSemaphoreSurfaceCallback`（または表示のセマフォ）で、描画の完了を表示エンジンに待たせ、
  フリップの完了をクライアントが待てる Nocturne の待ち合わせのハンドルとして返す。
- **アプリが確保した memory を面にする**：今は nvrm が絵を確保してアプリへ DUP する向き。逆向き（アプリの vidmem を KAPI の
  `importMemory` で面にする）は、`isVidmem`／`isContiguous` で走査面にできるかの確認とともに要るときに足す。
- **窓の大きさの絵・オーバーレイ層**：今の絵は画面の大きさだけで、主層だけを使う。
- **形式と色**：X8B8G8R8／X8R8G8B8 以外（ARGB8888、10 ビット、YUV）の対応表と、色処理の段。
- **実機**：RTX 4070 での KAPI のフリップ・フリップの完了通知の書込み・カーソル・`NV04_SHARE`＋DUP の挙動と、BAR1 の
  絵へのパニック画面の描画は未確認。

## UVM（nvidia-uvm）

CUDA の統合メモリー（Unified Virtual Memory）を担う NVIDIA の UVM ドライバー（`kernel-open/nvidia-uvm`）を、Linux の別モジュール
nvidia-uvm.ko ではなく nvrm の中に入れる。UVM は RM の `nvUvmInterface*`（nvidia.ko 側の `kernel-open/nvidia/nv_uvm_interface.c`）を
通して GPU を使うので、GPU を持つ nvrm の中に置き、RM との間に通信は挟まない。クライアントは RM と同じ接続で `NVRM_UVM`
（`include/nvrm_protocol.h`）を送る。

取り込み（上流 615.78.08 と同じ相対位置。変更は `NVRM_NOCTURNE` の条件の追加だけ：`nvidia-uvm` の 40 ファイルで Linux の
ヘッダーの読込みを外し、`uvm_linux.h` は Linux の層の代わりに `uvm_os.h` を、`nv_uvm_interface.c`・`nv_uvm_interface.h` は `uvm_rm.h` を
読む。処理の本文は変えない）：`kernel-open/nvidia-uvm` の 223 ファイル（`hwref` を含む、
約 15 万 4 千行）、`kernel-open/nvidia/nv_uvm_interface.c`、`kernel-open/common/inc` の `nv_uvm_interface.h`・`nv-kthread-q.h`・
`rm-gpu-ops.h`。すべて MIT（`COPYING`）で、`NOTICES` に加える物は無い。`uvm_common.c` は GPLv2 以降なので取り込まず、
本文も参照しない（`uvm_common.h` の宣言する関数は `uvm_os.c` が自前で持つ）。`uvm_linux.c`（Linux の memcg と scatterlist）と
`nv-kthread-q.c`（Linux のカーネルスレッド）も入れない。

### ビルド（`uvm.mk`、`nvrm.mk` から `include`）

```
python scripts/build.py uvm-objs        # UVM の中核と OS 層（build/uvm/）
python scripts/build.py uvm-undefined   # RM＋NVKMS＋UVM＋各 OS 層の未定義シンボル（build/uvm/undefined.txt）
```

- ソース一覧は上流の `nvidia-uvm-sources.Kbuild` をそのまま読む。定義は上流の `nvidia-uvm.Kbuild` から Linux の物
  （`-D__KERNEL__`、conftest）を除いた物。`NVRM_NOCTURNE` が `uvm_linux.h` に `uvm_os.h` を選ばせる。
- 全オブジェクトを `nvidia-uvm.o` に部分リンクし、入口（`nvrm_uvm_module_init`／`exit`）以外の大域シンボルを局所化する
  （UVM も `nvstatus.c` などを自前で持つ）。
- OS 層は二つの側に分かれる。`uvm_os.c`・`uvm_mm.c`・`uvm_serve.c` は UVM と同じ `uvm_os.h` で、`uvm_rm.c` と
  `nv_uvm_interface.c` は RM の OS 層（`nvrm_os.h`、`uvm_rm.h`）で作る。両方が見るのは `uvm_nvrm.h` と `uvm_shared.h` だけ。
- トップの Makefile の nvrm のリンクに `$(UVM_LINK_OBJ)` を加えた。

### OS 層の対応

| UVM が使うもの | Nocturne での実装 |
|---|---|
| `printk`、`dump_stack`、モジュール引数 | `nvrm_log`。引数は起動設定 `UVM_OPTIONS`（`name=value;...`、名前と既定値は Linux と同じ）（`uvm_os.c`） |
| スレッド、`current`、`kthread_*`、`nv_kthread_q_*`、遅延作業 | C11 のスレッドと `wait_on_address`。要求を処理する間は、そのスレッドがクライアントのタスク（pid＝アプリ ID、mm＝接続のアドレス空間）になる |
| ロック、待ち行列、ビット待ち、rb 木、radix 木、ビットマップ | C11 の mutex と条件、`wait_on_address`、自前の実装（`uvm_os.c`） |
| `kmalloc`／`vmalloc`／`kmem_cache` | libc の確保（`vmalloc` は範囲で引ける登録つき） |
| `alloc_pages`、`dma_alloc_coherent`、`dma_map_page` | RM の DMA メモリー（`nv_alloc_pages`、連続、write-back）。一回の確保が `struct page` の配列一つ（`uvm_mm.c`） |
| `vmap` | 一つの群の連続したページはそのままの番地、それ以外は `n_shared_compose` で一つの写像にする |
| `pci_dev`、BAR の写像（`nv_ioremap_cache`） | RM の PCI 情報（`nvrm_uvm_pci_info`）、`nvrm_bar_view` |
| `vm_area_struct`、`vm_insert_page`、`remap_pfn_range`、`unmap_mapping_range` | ページャー（下記）の `n_pager_supply`／`n_pager_revoke` |
| `get_user_pages_remote`、`copy_from_user`／`copy_to_user` | クライアントの引数の場（arena）の中だけ。`nvrm_user_view`／`nvrm_user_translate` |
| `/dev/nvidia-uvm`、`/dev/nvidia-uvm-tools`（`cdev_add`、`fget`） | 接続ごとの番号つきのファイル（`uvm_serve.c`）。minor 0 が nvidia-uvm、1 が tools |

### UVM のメモリーをクライアントが写す（ページャー）

Linux では UVM のファイルを `mmap` すると、CPU のページフォールトが UVM のフォールト処理を呼ぶ。Nocturne では
ページャーつきの共有メモリー（`docs/nocturne-native-abi.md` §7、`n_pager_*`）でこれを行う。

1. クライアント（libnvrm の `nvrm_uvm_map`）が `NVRM_UVM_MAP` を送ると、nvrm は `n_pager_create` でメモリーを作り、
   そのハンドルを返事に付ける。ハンドルは UVM が持ち続け、サーバーは送るだけで閉じない。
2. クライアントがそれを読み書きで写し、`NVRM_UVM_MAP_COMMIT` で番地を知らせる。UVM は写像のファイル位置が番地であることを
   求めるので、その番地で `vm_area_struct` を作り、UVM の `mmap` を呼ぶ。
3. クライアントが写像のページに初めて触ると、そのスレッドは止まり、接続のフォールトスレッド（接続ごとに 1 本、最初の
   写像のときに起こす）が `n_pager_take` で受け取って UVM のフォールト処理を走らせる。UVM の `vm_insert_page` が
   `n_pager_supply` でページを結び、触れたスレッドが進む。処理がページを結ばなければ、その触りは `n_pager_fail` で失敗する。
4. UVM がページを取り上げる（`unmap_mapping_range`）と `n_pager_revoke` で全ての写しから外れる。どのページが結ばれているかは
   写像ごとに覚えていて（2 MiB ごとの表）、`vm_insert_page` が既に在るページに `-EBUSY` を返す、という UVM の前提を守る。
5. `nvrm_uvm_unmap`（`NVRM_UVM_UNMAP`）か、クライアントが写像を手放して誰も持たなくなったとき（ページャーの CLOSED）、
   UVM の `close` の後に写像は終わり、全ページが外れる。接続の終わりでは、プロセスの終了と同じく先にアドレス空間が
   （UVM には mm が無いように見えて）終わり、その後でファイルが閉じる。これは RM のファイルを閉じる前に行う。

### nvrm の中の入口（`uvm_nvrm.h`）

- `nvrm_uvm_start()`：RM が GPU を起こした（`adapter_ready`）後、クライアントの提供を始める前に呼ぶ（`nv_uvm_init`、
  UVM のモジュール初期化）。失敗しても RM と画面の提供は続け、`NVRM_UVM` は `NV_ERR_NOT_SUPPORTED` を返す。
- `nvrm_uvm_stop()`：要求サーバーが終わった後、`rm_disable_adapter` の前に呼ぶ。
- `nvrm_uvm_interrupt()`：割り込みスレッドが `rm_gpu_handle_mmu_faults` の後、`rm_isr` の前に呼ぶ。`nv_schedule_uvm_isr` からも呼ぶ。
- 電源（`nv_uvm_suspend`／`resume`）と P2P（`nv_uvm_drain_P2P`／`resume_P2P`）は RM の経路が直接呼ぶ。

### 無いもの・制限

- **HMM と ATS**（クライアントの普通のメモリーを GPU が直接使う）：無い。`UVM_CAN_USE_MMU_NOTIFIERS` は 0。
- **写像の一部だけの munmap、mremap、fork**：無い。写像は丸ごと終わる。
- **procfs**（`/proc/driver/nvidia-uvm`）：無い。
- `NVRM_UVM_POLL` は 1 要求で最大 1 秒待つ（接続のスレッドを長く止めない）。libnvrm が繰り返して長い待ちにする。
- クライアントのメモリーを固定する（tools のバッファー）のは引数の場の中だけ。その外は `-EFAULT`。
- 他の装置（P2P）のメモリーを CPU に写すこと（`vm_insert_page` に nvrm の物でないページ）は `-EINVAL` で失敗する。
- 試験用ファイル（`UVM_TEST_*`）の写像も、ファイル位置＝番地の決まりに従う。
- UVM の引数の `rmCtrlFd` は、上流でも RM のファイルの照合に使われていない（Bug 1624521 の TODO）。RM のオブジェクトは
  `hClient` などのハンドルで引かれるので、ある接続の UVM が別の接続の RM クライアントのハンドルを名指しできるかは RM の
  検査次第で、nvrm では未確認。
- フォールトスレッドは接続ごとに 1 本で、アプリのスレッド上限（128）に数えられる。

未確認：CUDA のクライアントが Nocturne に無く、実機の GPU でも UVM を動かしていない。確かめたのはビルド、未定義シンボルの照合、
入口を残した試験リンク、ページャーの GPU 無しの試験（`pagertest`）、要求サーバーの偽 UVM 層での試験（`nvrm_servetest`、
`nvrm_transport_hosttest`）だけ。

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
