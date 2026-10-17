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
| `src/util/os_time.c/.h` | `msleep`／`yield` と ISO `localtime` を使う。 |
| `src/util/os_misc.c` | 物理メモリ量などを `sysinfo` で得る。 |
| `src/util/memstream.c/.h` | メモリ上の FILE は無いので、`/tmp`（RAM）の一時文書を書き、flush／close で読み戻す。 |
| `src/util/u_string.h` | `asprintf` 系は libc に無いので、Mesa 自身の Windows 用実装を使う。 |
| `src/util/log.c` | 共有の一時フォルダーは無い。`MESA_LOG_FILE` だけを使う。 |
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

段階4・5で旧 API 呼出しはすべてネイティブ ABI へ書き換えた。
