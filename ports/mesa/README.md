# Mesa（WebGL／WebGL2のソフトウェア描画）

ブラウザーのWebGL／WebGL2は、Mesaの GLSL／NIR コンパイラー、OpenGL ES 3 の
state tracker、Gallium softpipe ラスタライザーで描く。LLVM、GLX／EGL、DRM、
窓システムは含めない。ブラウザー側の接続は `nocturne_gl.c`／`nocturne_gl.h` と
`user/libc/web/js_webgl.c`。

## 出所

- 上流：`https://gitlab.freedesktop.org/mesa/mesa`、commit `745709a000`
  （`VERSION` は `26.3.0-devel`）。
- ライセンス：MIT（`third_party/mesa/docs/license.rst`、`third_party/mesa/licenses/`）。
- 取り込み先：`third_party/mesa`。ビルドは `ports/mesa/mesa.mk`（Mesa の meson.build の
  softpipe 構成に沿ったソース一覧）。

## 生成物

- `generate.py` が Mesa 自身の生成器（Python）を `build/mesa/gen` へ実行する。内容が
  変わらない出力は書き換えない。
- `generated/` は、ゲスト向けに bison／flex で作った GLSL・GLCPP の構文解析器。
  元の `.yy`／`.ll` を変えたら、同梱の `tools/mesa-tools` で再生成する：

```text
PATH=tools/mesa-tools/root/usr/bin:$PATH BISON_PKGDATADIR=$PWD/tools/mesa-tools/root/usr/share/bison bison -o ports/mesa/generated/src/compiler/glsl/glsl_parser.cpp -p _mesa_glsl_ --defines=ports/mesa/generated/src/compiler/glsl/glsl_parser.h third_party/mesa/src/compiler/glsl/glsl_parser.yy
```

## Nocturne向けの改変

Nocturne は UNIX／POSIX／Windows のどれでもない。OS 側へ互換関数を足さず、Mesa の
ソースを `-DNOCTURNE`（`DETECT_OS_NOCTURNE`）の条件コンパイルで Nocturne の
ネイティブ API と ISO C へ直接合わせる。改変箇所は `rg -i nocturne third_party/mesa` で
すべて見つかる。

| ファイル | 内容 |
|---|---|
| `src/util/detect_os.h` | `DETECT_OS_NOCTURNE` を定義。 |
| `include/c99_alloca.h` | コンパイラー組込みの alloca を使う。 |
| `src/c11/threads.h`、`src/c11/time.h` | libc の ISO C11 `threads.h`、`timespec_get`（`TIME_UTC`／`TIME_MONOTONIC`）を使う。 |
| `src/util/cnd_monotonic.c/.h` | ISO C11 の条件変数で単調時計の期限待ちを作る。 |
| `src/util/rwlock.c/.h` | ISO C11 の mutex と条件変数で読み書きロックを作る。 |
| `src/util/os_time.c/.h` | `n_sleep_ms`／`yield` と ISO `localtime` を使う。 |
| `src/util/os_misc.c` | 物理メモリ量などを `sysinfo` で得る。 |
| `src/util/memstream.c/.h` | メモリ上の FILE は無いので、一時文書を書き、flush／close で読み戻す。 |
| `src/util/u_string.h` | `asprintf` 系は libc に無いので、Mesa 自身の Windows 用実装を使う。 |
| `src/util/log.c` | 共有の一時フォルダーは無い。`MESA_LOG_FILE` だけを使う。机上から起動したアプリには stderr が無いので、既定の出力先にシステムの記録（`n_system_log_open`、`MESA_LOG=system` でも選べる）を足す。行は `MESA: warning: …` の形で、USB の C: では System.log に残る。`mesa_log_hold`／`mesa_log_release` は、呼んだスレッドの行をその間だけ書かずに預かり（同じ形の文字列で返す）、`mesa_log_write_lines` が後でまとめて書く（Nocturne GL が Zink を起こす間に使う）。 |
| `src/util/u_debug.h` | set-user-ID プログラムは無い。 |
| `src/util/u_endian.h` | コンパイラーの `__BYTE_ORDER__` を使う。 |
| `src/util/disk_cache.h`、`src/gallium/auxiliary/util/u_screen.c` | ファイル状態取得と DRM 記述子による screen 共有を外す。 |
| `src/util/u_qsort.cpp`、`src/util/texcompress_astc_luts.cpp/.h` | libc++ にスレッド支援が無いので、Mesa 自身のロックを使う。 |
| `src/gallium/drivers/softpipe/sp_tex_sample.c/.h` | 圧縮・深度以外の色テクスチャーは、32×32タイルを丸ごとfloat変換する16項目のタイルキャッシュを通さず、資源の記憶から1texelずつ読む（mipmap／繰返しでキャッシュが入れ替わり続け、描画時間の大半を占めていた）。 |
| `src/gallium/drivers/softpipe/sp_tile_cache.c/.h` | 描画先タイルを64画素から32画素にし、タイルキャッシュを50項目から4096項目（横64×縦64タイル＝2048×2048画素まで衝突なし）へ広げる（1080pの全画面canvasでは、50項目だと描画中にタイルの書戻しと再読込みが繰り返されていた）。タイルは初めて触れたときだけ確保する。 |
| `src/gallium/drivers/softpipe/sp_prim_vbuf.c`、`sp_setup.c/.h`、`sp_context.h`、`sp_state_derived.c`、`sp_tile_cache.c/.h` | 三角形の描画をCPUの数（最大8）のスレッドへ分ける。各スレッドは draw の全三角形を辿り、32画素のタイル行のうち自分の行だけを塗る（画素ごとの描画順は保たれ、同じタイルを二つのスレッドが触らない。自分の行に掛からない三角形は設定前に捨てる）。呼出し側以外のスレッドは、文脈の影の写し・quad段・TGSI機械・サンプラーを自分用に持つ。起動・完了の待ちは短く回転してから条件変数で眠る。統計／遮蔽クエリー、層描画、image／buffer書込み、タイルキャッシュ経由のテクスチャー、64×64タイルを超える描画先は従来どおり一つのスレッドで描く。頂点バッファーを64KiB／4096索引へ広げ、draw の分割回数を減らす。 |
| `src/gallium/drivers/softpipe/sp_texture.c` | 新しいテクスチャー領域を必ずゼロ初期化する（他ページの解放済みメモリを Web 内容から見せない）。 |
| `src/gallium/auxiliary/driver_trace/tr_dump.c` | `access()` の代わりに、トリガー文書が ISO `fopen("r+b")` で開けるかで判定し、ISO `remove` で消す。 |
| `src/gallium/auxiliary/rtasm/rtasm_execmem.c/.h`、`rtasm_x86sse.c` | 頂点変換の x86 コード生成は `n_memory_reserve` の書込み可ページへ書き、`x86_get_func` で `n_memory_protect` により実行専用へ封印する（書込みと実行を同時に許さない）。 |
| `src/compiler/glsl/glsl_parser.yy` | `strcasecmp` は `<string.h>` にある。`generated/` の解析器も再生成済み。 |
| `src/util/u_debug.h`、`src/util/u_process.c`、`src/util/xmlconfig.c` | POSIX のヘッダー（`unistd.h`、`sys/types.h`、`fcntl.h`）を読まない。 |
| `src/util/rand_xor.c` | 乱数の種は `n_random` から取る。 |
| `src/util/memstream.c`、`src/util/perf/u_trace.c` | アプリの識別子は `n_app_self`。 |
| `src/mesa/main/externalobjects.c` | 取り込んだハンドルは `n_handle_close` で閉じる。 |
| `src/gallium/auxiliary/util/u_tests.c` | フェンスの記述子は無い。 |
| `src/mesa/program/program_lexer.l` | 文字列だけを読むので `never-interactive`（`isatty` を使わない）。`generated/` の `lex.yy.c` も再生成済み。 |
| `src/util/u_dl.c` | 共有ライブラリーは無い。`util_dl_open("vulkan")` は同じプログラムにリンクされた Vulkan ドライバー（NVK）を指し、`vk_icdGetInstanceProcAddr`／`vk_common_GetDeviceProcAddr`（弱参照）でローダーと同じ答え方をする。ドライバーが無ければ NULL。ローダーの持つ関数はローダー役が答える：`vkGetInstanceProcAddr` は自分を返し、`vkEnumerateInstanceLayerProperties` は層が無い（0 個、VK_SUCCESS）と答える（ICD である NVK には無く、無いと Zink はインスタンスを作らない）。 |
| `src/gallium/drivers/zink/zink_screen.c`、`zink_public.h` | `VK_LIBNAME` は `"vulkan"`。`unistd.h` を読まず、DRM の記述子を閉じない。DRM の記述子から screen を作る `zink_drm_create_screen` を外す（GL は `zink_create_screen` で作る）。インスタンスを作れなかったときは数を戻す（後の文脈で作り直し、理由もまた書く）。作れなかった論理装置は次の screen へ渡さない。物理装置が一つも無い、`VK_TIME_DOMAIN_DEVICE` が無い、cache の thread を作れない、を `mesa_loge` で書く（`debug_printf` はリリース版では消える）。 |
| `src/gallium/drivers/zink/zink_instance.py`、`zink_device_info.py` | ローダーに大域の関数が無いことと、必須の拡張が無いこと（拡張の名前と装置の Vulkan の版）を `mesa_loge` で書く。 |
| `src/vulkan/runtime/vk_log.c` | 記録が預かられている間（`mesa_log_holding`）は、リリース版でも `vk_error*()`／`vk_log*()` を `MESA: warning: 文書:行: 内容 (VK_ERROR_…)` の形で Mesa の記録へ出す（Vulkan のデバッグ記録が無くても、GPU を使えない理由が残る）。 |
| `src/gallium/drivers/zink/zink_resource.c`、`zink_context.c`、`zink_fence.c/.h` | ファイル記述子が無いので、dma-buf／opaque-fd のメモリ共有と sync file のフェンス取込みを外す。 |
| `src/gallium/include/frontend/winsys_handle.h`、`src/gallium/drivers/zink/zink_resource.c` | ハンドルの型 `WINSYS_HANDLE_TYPE_NOCTURNE_SCANOUT`（nvrm の表示用画像の番号）を足す。`resource_from_handle` はこの型の PIPE_BUFFER だけを受け、`VkImportMemoryNocturneScanoutInfoMESA` を付けて VRAM に取り込む（下位割当て・キャッシュに入れない）。 |

## Zink（GPU での WebGL）

`zink.mk` が `src/gallium/drivers/zink`（クローンの同じコミットから、`ci/` を除いて取り込み）を
作る。`nocturne_gl.c` は `nvrm_connect_ready_status` で nvrm の ready な GPU を探し、あれば
`zink_create_screen` を試し、どちらかが駄目なら softpipe へ戻る。どうなったか（「drawing on the GPU with …」、
GPU が使えない理由、「Zink did not start」）を、前に書いたものと変わったときだけ `Nocturne GL:` で始まる一行で
Mesa の記録（システムの記録）へ書く。nvrm がどの GPU も出していない機械では何も書かない。`zink_create_screen`
の間は記録を預かり（`mesa_log_hold`）、Zink と NVK の書いた行（`ZINK:`、`NVK:`、`vk_error*()` の
`文書:行: … (VK_ERROR_…)`）を、結果と行のどちらかが前と変わったときだけ、その一行の前にまとめて書く。
同じ理由の再試行は文脈ごとに書かない。行が一つも無ければ「it gave no reason」と書く。GPU のときは描画用バッファーの消去を
`clear_texture`（GPU 上、描画の後ろに積むだけ）で行い、softpipe では従来どおり直接書く。
driconf が使う `os_read_file` は `nocturne_gl.c` が `n_document_load` で提供する。

## NVK（NVIDIA の Vulkan、Turing 以降）

`nvk.mk` が Vulkan 共通 runtime、NVK、NAK・nil（Rust）を作る。GPU との間は DRM ではなく
`src/nouveau/vulkan/nvkmd/nocturne/`（Nocturne 専用の nvkmd）が、ユーザー空間の RM `C:\Nocturne\System\nvrm`
（`ports/nvidia/`）へ libnvrm でつなぐ。

- `nvkmd_nocturne_rm.c`：RM の SDK の型を使う唯一の層（client／device／subdevice、メモリ、VA、
  GPFIFO チャンネル、ドアベル）。他のファイルは `nnrm_*` だけを見る。`nnrm_open_device` と `nnrm_chan_create` は失敗した段階
  （RM の制御・物体の名前など）を返し、`pdev`・`dev`・`ctx` がそれを Mesa の記録へ書く（`ctx` は
  `vk_errorf` で：Nocturne GL が記録を預かる間は出る）。BAR1 の大きさ（`NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE`）は VRAM と同じく KiB。
  装置は旗なしで確保する（GSP 構成では `RESTRICT_RESERVED_VALIMITS` の範囲が GSP へ届かず断られる）。
  グラフィックスとコピーのクラスはそのエンジンの一覧（`NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST`）から選ぶ
  （装置の一覧には下位バイトが重なる MIG のクラスがある）。
- `pdev`・`dev`・`mem`・`va`：NVK の物理・論理装置、メモリ（VRAM か sysmem、常に一貫）、
  VA は `util_vma_heap` で選び RM の仮想メモリへ対応付ける。GPU の番地は 8 GiB（`NNRM_VA_START`）から：
  その下の [4 GiB, 8 GiB) は RM の場所（GSP-RM の範囲と、CPU 側 RM が写すチャネルの文脈バッファー）。
- `ctx`：実行文脈ごとにチャンネル一つ、フェンス一つ（GART の 64 ビット値）、ホスト用の命令片
  64 枠。待ちは GPU のセマフォ取得、完了はリリースで書く。GP の環の空きはフェンスで数える
  （USERD の GP_GET は RM が書き戻しを選んだときしか動かない）。flush は環に入る大きさに分ける。
  束縛（bind）は CPU で同期的に行う。
- `sync`：Nocturne 独自の vk_sync（二値と timeline、GPU 待ち・CPU 待ち）。fd は無い。

Rust の 3 crate（NAK、nil、nvk_rs）と std は `rustc` の staticlib `libnvk_mesa_rs.a` 一つにまとめる。
NVK・Zink・libnvrm・Rust は libc の遅延メンバーで、`nocturne_gl.c` の強い参照により GL を使う
プログラムにだけ入る。Vulkan の実行時・WSI・NVK は `ld -r` で一つの物体 `vulkan_nouveau.o` にして、
一つのメンバーとして入れる（上流の共有ライブラリーと同じ、丸ごとの単位）。入口の表は実装を弱い参照で
名指しし、弱い参照は遅延メンバーを引き込まない。物体ごとのメンバーだった間は、名前で呼ぶ者の無い実装
（`vk_common_GetPhysicalDeviceProperties2`、`vk_common_CmdCopyBuffer` など 51 個）がリンクされず、表の項が
NULL のまま実機で呼ばれた。`scripts/link_unit.py` は単位を作るとき、単位から出る弱い参照を他のメンバーが
定義していればビルドを止める。SDK の `libc.a`（`scripts/mksdk.sh`）には Rust の .a の各メンバーを展開して入れる。
GPU が無い（`C:\Nocturne\System\nvrm` が動いていない）機械では、チャンネル接続がすぐ失敗し、Zink は softpipe へ戻る。

| 変更したファイル（NVK・Vulkan） | 内容 |
|---|---|
| `nvk_instance.c`、`nvk_physical_device.c/.h` | DRM の装置列挙ではなく `physical_devices.enumerate` で nvkmd の GPU を数える。ELF の build-id は無いので、版と NVK のビルド時刻の hash を driver identity にする。`NVX_binary_import` は libelf が要るので出さない。適合していない GPU を使わないとき、その名前と 3D class を書く（リリース版の上流は何も言わない）。 |
| `nvkmd/nvkmd.c/.h`、`nvkmd_device_info.c/.h` | 装置情報の共通部分を DRM 版から分け、Nocturne の nvkmd を登録する。同期の型と payload の複製は backend が渡す。 |
| `nvk_device.c`、`nvk_buffer.c`、`nvk_image.c`、`nvk_device_memory.c`、`nvk_descriptor_table.c` | DRM の syncobj と dma-buf を使わない（backend に無ければ外部メモリは非対応と答える）。`sys/mman.h` を読まない。 |
| `nvk_device_memory.c`、`nvkmd/nvkmd.c/.h`、`nvkmd/nocturne/` | `VkImportMemoryNocturneScanoutInfoMESA`（`vk_internal_exts.h`）が付いた割当ては、nvrm の表示用画像を `nvrm_scanout_import` で自分の装置のメモリー物体として取り込む（ホストから見えるメモリー型には付けられない。ゼロ初期化しない）。解放は他のメモリーと同じ。 |
| `nak_bindings.h` | NAK のハードウェア試験用の winsys／DRM を読まない（試験は作らない）。 |
| `vk_instance.c`、`vk_fence.c`、`vk_semaphore.c`、`wsi_common.c` | fd が無いので sync file・opaque fd・dma-buf の出し入れと `dlfcn.h` を外す。 |
| `wsi_common_headless.c` | 待ちの期限は POSIX の `clock_gettime` ではなく `util/os_time`。 |
| `vk_device.c` | 時刻の領域 MONOTONIC／MONOTONIC_RAW は Nocturne の単調時計。 |
| `vk_image.c/.h` | NVK が画像の配置に使う `drm_format_mod` 欄を Nocturne でも持つ（DRM の機能ではない）。 |
| `rmv/vk_rmv_exporter.c` | プロセス番号は無いので 0。 |

段階4・5で旧 API 呼出しはすべてネイティブ ABI へ書き換えた。

## 直接表示（全画面の WebGL）

全画面の WebGL canvas の絵は、窓を通さず（C20 の読戻しをせず）GPU 上で表示用の画像へ写す。

1. ブラウザー（`user/apps/browser.c`）は、ページが全画面の canvas だけのとき（`web_direct_open`：
   上に dialog が無い、canvas の箱が自分では何も塗らない、object-fit した絵が箱に収まる、GPU で描き
   alpha が無いか乗算済み）に限り `win_direct` を求める。GPU の無い softpipe の文脈は求めない。
2. 系が `EV_DIRECT`（key 1）を送ると、各フレームを `ngl_scanout_present` で出す。`nocturne_gl.c` は
   `nvrm_scanout_create` で画面の大きさの画像を三枚取り、Zink の `resource_from_handle` で buffer として
   取り込む。描画バッファーを（大きさが違えば線形に拡大縮小してから）`image_copy_buffer` で画像の
   該当位置へ写し、置く位置が変わった画像は先に黒で埋め、描画の完了を待って `nvrm_scanout_present`
   する。PRESENT が返れば二つ前の画像は表示から外れているので、三枚を順に使う。
3. PRESENT は垂直帰線で返るので、その時点を requestAnimationFrame の次の刻みにする。
4. `EV_DIRECT`（key 0）や条件が外れたときは、窓へ全体を描き直す。画像は表示が窓へ戻った後で返す。
