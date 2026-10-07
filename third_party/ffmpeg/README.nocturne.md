# Nocturne の限定 FFmpeg 9.0.2 移植

入力はユーザー指定の `D:/Programes/cloned repos/ffmpeg-9.0.2`。`RELEASE` と `VERSION` はともに `9.0.2`。ローカルソースの実在・版は確認したが、配布 tarball の署名認証を行ったという意味ではない。

`ports/ffmpeg/vendor.py` は、実際に freestanding コンパイルを通過した C ファイルと依存ヘッダーだけを取り込む。`manifest.json` に各ファイルの SHA-256 と未変更 upstream / configure 生成の区別を記録する。upstream に存在するファイルの内容が変わっていれば取り込みを拒否する。通常の OS ビルドはこの限定ソースと固定構成を用いる。FFmpeg CLI や外部プレーヤーは OS に入らない。

## 構成

- `libavcodec`, `libavformat`, `libavutil` の静的 C 構成。
- デコーダー: MP3, FLAC, AAC, H.264, MJPEG, rawvideo、および WAV 向け PCM。
- コンテナー: WAV, MP3, FLAC, ADTS AAC, AVI, MOV/MP4。
- network、URL protocol、device、filter、encoder、外部 library、pthread/Windows thread、assembler/GPL 最適化を無効にする。
- native `media.c` の custom AVIO が Nocturne の `read/lseek/fstat` または所有メモリーに接続する。codec handle は単一所有・単一実行コンテキスト専用。FFmpeg の無 thread ビルドを勝手にマルチスレッドで使用してはならない。
- PCM 出力は 48 kHz stereo。mono は両側、複数チャンネルは先頭の２つを用いる（surround downmix ではない）。リサンプルは線形補間で、各フレーム末尾は最後のサンプルを保持する簡易処理。
- 動画出力は 8-bit YUV420/422/444、RGB/BGR、gray から opaque ARGB へ。10-bit H.264、VP9/AV1、Opus/Vorbis、HEVC、WebM は再生機能として宣言しない。
- browser は既存 fetch/CORS を用いる上限付き全体取得。MSE、DRM、HLS/DASH/live、HTTP Range streaming、autoplay、1x 以外の速度、字幕は未対応。これは Web 全面互換を達成したものではない。

## 再生成

先に Nocturne の `build/sysroot/usr/lib/libc.a` を作る。その後:

```
python ports/ffmpeg/prepare.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2" --import-vendor
```

空白を含む upstream パスの configure 制限を避けるため、`build/nocturne-platform/media/reproduce-stage` に複製して in-tree 生成する。元ソースは読み取りのみ。native mmap/fcntl/mkstemp を POSIX file helper と誤検出した３項目は専用構成で明示的に無効化する。`ports/ffmpeg/include` の scoped C header glue は OS グローバル ABI を変更しない。

上流の library 規則と同じく内部 C は `-DHAVE_AV_CONFIG_H -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE` 付きでコンパイルする。公開 API の利用者には `HAVE_AV_CONFIG_H` は不要。`media.o` にも専用 `errno.h` を先頭 include に用いること（`EDOM > 0` による `AVERROR` の符号判定を維持するため）。

`ports/ffmpeg/make-fixtures.py` は host FFmpeg 8.1.1 で自作 tone/pattern の実圧縮回帰入力を生成する。host バイナリーは OS の dependency ではない。生成 clip は実サイトの受け入れ試験の代用品ではない。

## ライセンス

この構成の configure 判定は LGPL-2.1-or-later。元ライセンス・著作者ヘッダーを保持し、同梱 `COPYING.LGPLv2.1` と `LICENSE.md` に従う。静的配布では対応ソース、変更、再リンクに必要な Nocturne object/build 手順も利用者が取得できるようにする。`sources.mk` を lazy static link へ含め、実体の不要なアプリへ decoder を丸ごと入れない。

MJPEG が使用する `libavcodec/jrevdct.c` は Independent JPEG Group の成果に由来する。**このソフトウェアは Independent JPEG Group の成果に一部基づく。** 同ファイルへの追加・削除・変更はない。その他の source と同様に manifest で未改変を検査する。

制約付き移植の保証範囲は Nocturne 内の実 codec 試験・GUI/browser 実経路の記録で判断し、コンパイル成功を音声の可聴性・A/V 品質・全サイト対応の証明とはしない。
