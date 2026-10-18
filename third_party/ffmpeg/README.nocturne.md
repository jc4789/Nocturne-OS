# Nocturne の限定 FFmpeg 9.0.2 移植

入力はユーザー指定の `D:/Programes/cloned repos/ffmpeg-9.0.2`。`RELEASE` と `VERSION` はともに `9.0.2`。ローカルソースの実在・版は確認したが、配布 tarball の署名認証を行ったという意味ではない。

`ports/ffmpeg/vendor.py` は、実際に freestanding コンパイルを通過した C ファイルと依存ヘッダーだけを取り込む。`manifest.json` に各ファイルの SHA-256 と未変更 upstream / configure 生成の区別を記録する。upstream に存在するファイルの内容が変わっていれば取り込みを拒否する。通常の OS ビルドはこの限定ソースと固定構成を用いる。FFmpeg CLI や外部プレーヤーは OS に入らない。

## 構成

- `libavcodec`, `libavformat`, `libavutil`, `libswresample`（browser profile は `libavfilter` も）の静的構成。現在の実コンパイル閉包は409 C + 62 NASM `.asm` / 958 manifest files（SIMD 有効化前は372 C。下の「x86-64 SIMD」参照）。`libswresample` の generic C は native Opus の SILK 経路とステレオ混合行列にも必要で、外部の libopus/libvorbis/libvpx は入らない。
- デコーダー: MP3, FLAC, AAC, H.264, MJPEG, VP8, VP9, Opus, Vorbis, rawvideo、および WAV 向け PCM。
- コンテナー: WAV, MP3, FLAC, ADTS AAC, AVI, MOV/MP4, Matroska/WebM, Ogg, MPEG-TS。
- network、URL protocol、device、filter（browser profile の PCM 3種を除く）、encoder、外部 library、pthread/Windows thread、GPL 部分を無効にする。x86-64 の SIMD（NASM と inline asm）は有効で、実行時 CPU 判定で選ぶ（下の「x86-64 SIMD」）。
- native `media.c` の custom AVIO が Nocturne の `read/lseek/fstat` または所有メモリーに接続する。codec handle は単一所有・単一実行コンテキスト専用。FFmpeg の無 thread ビルドを勝手にマルチスレッドで使用してはならない。
- PCM 出力は 48 kHz stereo。mono は両側、最大8チャンネルの既知 speaker layout は `swr_build_matrix2` による正規化行列でステレオへ混合する。center/surround を約 -3 dB、LFE を弱く混合し、S16 の飽和処理を行う。並びの不明な複数チャンネル、行列が混合できず欠落する speaker は明示拒否する。mask のない1/2チャンネルだけは標準 mono/stereo と解釈する。リサンプルは従来の線形補間で、各フレーム末尾は最後のサンプルを保持する簡易処理。
- 動画出力は 8-bit YUV420/422/444、RGB/BGR、gray から opaque ARGB へ。VP8 は native 8-bit YUV420、VP9 は profile 0 / 8-bit YUV420 / SDR に限定し、10/12-bit の上流 DSP がコンパイルされても高bitdepthやHDRを再生能力に含めない。10-bit H.264、AV1、HEVC は再生機能として宣言しない。
- Ogg demuxer に必要な Dirac/Theora/Speex の header parser が含まれても、それらの decoder は無効。VP8 の decoder/parser は明示有効化した。コンテナーの header parser が存在することだけを codec 再生対応と混同しない。
- browser は既存 fetch/CORS、native worker の匿名 HTTP Range 入力、または有界単一GETのmanifest/segment入力を使う。native MSEは既存packet buffer/別workerを維持する。HLS/DASH入力はNocturne側の `media_adaptive.c` がclear VOD manifestを解析し、各segmentをcustom AVIOでdemuxしてpersistent decoderへ供給する。FFmpegのHLS/DASH demuxer・network protocolは有効化しない。
- adaptiveの対象はmuxed HLS TS/fMP4 VOD、およびstatic単Period DASHの有限SegmentTemplate/SegmentTimeline（分離AV最大2track）。manifest256KiB、segment8MiB、init1MiB、2048segment/track、最小対応bandwidthの固定選択。DRM/暗号化、live/LL-HLS、alternate HLS rendition、DASH dynamic/複数Period/SegmentBase/SegmentList、未知control extensionを明示拒否する。全サイト対応やABR切替を宣言しない。実Nocturne再生の検証範囲はrootの受け入れ記録に従い、対象compileだけを映像/可聴音声の証明としない。

## 再生成

先に Nocturne の `build/sysroot/usr/lib/libc.a` を作る。その後:

```
python ports/ffmpeg/prepare.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2" --profile browser --stage build/media-browser-simd-20261017 --import-vendor
```

MSYS2 の PATH（`tools/msys64/ucrt64/bin` と `usr/bin`。`nasm`・`clang`・`rg` を使う）で実行する。stage は毎回新しい名前にする。

現行の browser profile は `--profile browser`（`--import-vendor` で `ports/ffmpeg/vendor.py` まで実行）。`--scalar` を付けると SIMD 化前の asm 無し構成を再現できる。stage の build 前に、manifest の origin が `nocturne-native-port` のファイル（下記）を stage へ重ねて POSIX ヘッダー無しでも compile でき、`vendor.py` はそれらを上流版で上書きせず origin も保つ。`libavutil/file_open.[ch]` と `libavformat/os_support.c` は取り込まない（`vendor.py` の `NATIVE_DROPPED`）。

空白を含む upstream パスの configure 制限を避けるため、`build/` 内の新しい stage に複製して in-tree 生成する。既存 stage は拒否し、前段階の `build/nocturne-platform/media/reproduce-stage` と証拠は上書きしない。`--profile legacy` は旧7経路の構成を再現する選択肢として残す（その場合も新しい `--stage` を選ぶ）。相対 include/link path は stage の深さから生成する。元ソースは読み取りのみ。native mmap/fcntl/mkstemp を POSIX file helper と誤検出した３項目は専用構成で明示的に無効化する。`ports/ffmpeg/include` の scoped C header glue は OS グローバル ABI を変更しない。

上流の library 規則と同じく内部 C は `-DHAVE_AV_CONFIG_H -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE` 付きでコンパイルする。公開 API の利用者には `HAVE_AV_CONFIG_H` は不要。`media.o` にも専用 `errno.h` を先頭 include に用いること（`EDOM > 0` による `AVERROR` の符号判定を維持するため）。

`ports/ffmpeg/make-fixtures.py` は host FFmpeg 8.1.1 で自作 tone/pattern の実圧縮回帰入力を生成する。host バイナリーは OS の dependency ではない。生成 clip は実サイトの受け入れ試験の代用品ではない。

追加 WebM/Ogg 入力は `ports/ffmpeg/make-webm-fixtures.py` で生成し、専用 `tests/media-fixtures/webm-manifest.json` に引数・codec/profile・hash・host参照PCM数を残す。旧 fixture と旧 manifest は変更しない。能力宣言は `user/libc/media_types.c` の限定 allow-list と実 native/browser 試験を照合する。`vp09` の profile/bitdepth/color suffix は [WebM公式の定義](https://www.webmproject.org/vp9/mp4/) に従い、未対応 profile/HDR は拒否する。旧 `vp9`/`vp9.0` と WebM の Opus/Vorbis 記法は [公式 container guideline](https://www.webmproject.org/docs/container/) を参照する。

## 2026-10-12 browser profile拡張

`--profile browser` はadaptiveにHEVC Main/Main10、AC3/EAC3と限定PCM filter（abuffer/abuffersink/atempo）を追加する。正規compile閉包は374 C。MP4 Opus/VP9の既存decoder/demuxerをnative能力宣言と整合し、planar 10/12/16bit SDRをphysical 8bit surfaceへ変換する。上記の旧8bit限定記録はこの拡張前の検証範囲であり、追加codecの実guest確認は`docs/media-timing-20261012.md`と主担当記録で判断する。AV1のsoftware decoderとHDR tone mappingは未対応で、HDR transfer/gamutを偽ってSDRとして出力しない。

PCM速度変更はGUI/packet decoderとは別のnative媒体責務で、pitch維持はatempo WSOLA、pitch変更はresamplerを用いる。decoder handleをthread-safeに変えたという意味ではない。通常1xは新filterを通さない。追加ソースはLGPL構成、CONFIG_GPL=0。既存許諾文書を削除していない。

## 2026-10-18 x86-64 SIMD（NASM と inline asm）

H.264/HEVC/VP8/VP9/AAC/AC3 等の IDCT・動き補償・デブロック・DSP が scalar C だったため、upstream の x86 最適化を有効にした。
- configure（`ports/ffmpeg/prepare.py` の `SIMD_ARGS`）: `--enable-x86asm --x86asmexe=nasm --enable-runtime-cpudetect --disable-avx512 --disable-avx512icl --malloc-prefix=nmedia_ff_`。inline asm は既定で有効。`--disable-asm` 系は外した。`-march` は固定しない（`-msse2` の基底のみ）。
- 実行時選択: `libavutil/x86/cpu.c` が `cpuid`（`ff_cpu_cpuid`、`libavutil/x86/cpuid.asm`）と `xgetbv` で MMX/SSE2/SSSE3/SSE4/AVX/AVX2/FMA3/BMI を判定する。各 `*_init.c` が `av_get_cpu_flags()` で関数ポインターを選ぶので、AVX 無しの QEMU/WHPX でも SSE2/SSSE3 に落ち、AVX2 機では AVX2 を使う。`libavutil/cpu.c`（native-port）の `ARCH_X86` 分岐はそのまま `ff_get_cpu_flags_x86` を呼ぶ。CPU 数だけ `cpu_info` を用いる。
- AVX-512 は構成で無効（`HAVE_AVX512*=0`）。カーネルの XCR0 は x87|SSE|AVX のみ（`kernel/src/arch/fpu.c`）で opmask/ZMM 状態を保存しないため、`cpu.c` が元から行う XCR0 の `0xe0` 確認と合わせて二重に防ぐ。XCR0 に ZMM を許可するなら、先にカーネルの XSAVE 領域を拡張してから構成を変える。
- 出力は static・non-PIC の ELF64。`config.asm` の `CONFIG_PIC=0`、x86inc は `elf64` で UNIX64 呼出規約（SysV）になる。NASM 出力の再配置は `R_X86_64_PC32` のみ（絶対32bitや GOT/PLT 無し）。C 側は従来通り `-fno-pic -fno-pie`。`HAVE_ALIGNED_STACK=1` なので asm は呼出時の 16 byte stack 整列を前提とする（`-mstack-alignment=16`）。
- `av_malloc` は 32 byte 境界（`HAVE_SIMD_ALIGN_32`）を要する（AVX の `mova ymm`）。`user/libc/media_alloc.c` に `nmedia_ff_posix_memalign` / `nmedia_ff_memalign` を追加し、`MALLOC_PREFIX=nmedia_ff_` 経由の `libavutil/mem.c` が使う。既存と同じ課金・`nmedia_ff_free` 経路で、整列の slack だけ追加課金する。`prepare.py` は configure の link 判定のため同 `.c` を stage 内でだけ compile して `--extra-libs` に渡す。
- 取り込み: `vendor.py` が `.d` から NASM 依存（`config.asm`、`config_components.asm`、`x86inc.asm`、`x86util.asm`、各 `*_template.asm`）と x86 の `*_init.c`・ヘッダーを閉包へ追加する。`sources.mk` は `FFMPEG_C` と `FFMPEG_ASM` を持ち、`Makefile` は `build/ffmpeg/**/*.asm.o` を `nasm -f elf64 -I third_party/ffmpeg/ -I <asmのdir>/ -Pthird_party/ffmpeg/config.asm -MD` で作って既存の `FF_OBJ`（lazy static link）へ加える。swscale は取り込んでいないので色変換は従来の native 経路のまま。
- 表示・再生の速度向上は SIMD 有効化の見込みであり、Nocturne 実機/QEMU の同条件比較（同動画・同 viewport）で確認するまで改善とは主張しない。

## ネイティブAPI移行（段階4）

POSIX の `unistd.h` / `fcntl.h` / `sys/*.h` を前提とする FFmpeg の OS 層は、Nocturne のネイティブ API へ直接書き換えた。manifest の `origin` が `nocturne-native-port` のファイルが対象で、`vendor.py` は現行 manifest の origin が `nocturne-native-port` のファイルを上流版で上書きしない（`prepare.py` が stage に重ねて build し、hash が一致する場合だけ維持する）。この7ファイル以外を移植した場合は、manifest の origin を同じ値にしてから再生成すること。
`libavutil/{file,log,cpu,time,random_seed}.c` と `libavformat/utils.c`（`ff_mkdir_p`）と `os_support.h` を変更した。`av_file_map` は `n_document_load`、`isatty` は `n_terminal`、CPU 数は `cpu_info`、乱数は `n_random`、時刻は `n_clock_wall`（秒単位）／`n_clock_uptime_ms`（ミリ秒単位）、`av_usleep` は `n_sleep_ms`（切り上げ）を用いる。fd を返す `file_open.[ch]` と、ネットワーク無効では空の `os_support.c` は閉包から外して削除した。`config.h` の `HAVE_UNISTD_H` `HAVE_SYS_TIME_H` `HAVE_GETTIMEOFDAY` `HAVE_ISATTY` `HAVE_MPROTECT` も 0 にした（`prepare.py` に同じ上書きを追加済み）。

## ライセンス

この構成の configure 判定は LGPL-2.1-or-later。元ライセンス・著作者ヘッダーを保持し、同梱 `COPYING.LGPLv2.1` と `LICENSE.md` に従う。静的配布では対応ソース、変更、再リンクに必要な Nocturne object/build 手順も利用者が取得できるようにする。`sources.mk` を lazy static link へ含め、実体の不要なアプリへ decoder を丸ごと入れない。

SIMD 化で追加した NASM `.asm` 62本と x86 の C/ヘッダーは、取り込み時に全ファイルの許諾ヘッダーを確認した。`libavcodec/x86`・`libavutil/x86`・`libswresample/x86` の127ファイルは LGPL-2.1-or-later（"or (at your option) any later version"）。`libavutil/x86/x86inc.asm` は x264 project の ISC 許諾（permissive、LGPL と矛盾しない）で、ヘッダーを保持している。`config.asm` / `config_components.asm` は configure 生成物。GPL 専用のファイルは無く、`CONFIG_GPL=0` のまま。

MJPEG が使用する `libavcodec/jrevdct.c` は Independent JPEG Group の成果に由来する。**このソフトウェアは Independent JPEG Group の成果に一部基づく。** 同ファイルへの追加・削除・変更はない。その他の source と同様に manifest で未改変を検査する。

制約付き移植の保証範囲は Nocturne 内の実 codec 試験・GUI/browser 実経路の記録で判断し、コンパイル成功を音声の可聴性・A/V 品質・全サイト対応の証明とはしない。
