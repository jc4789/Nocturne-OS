# Nocturne の限定 FFmpeg 9.0.2 移植

入力はユーザー指定の `D:/Programes/cloned repos/ffmpeg-9.0.2`。`RELEASE` と `VERSION` はともに `9.0.2`。ローカルソースの実在・版は確認したが、配布 tarball の署名認証を行ったという意味ではない。

`ports/ffmpeg/vendor.py` は、実際に freestanding コンパイルを通過した C ファイルと依存ヘッダーだけを取り込む。`manifest.json` に各ファイルの SHA-256 と未変更 upstream / configure 生成の区別を記録する。upstream に存在するファイルの内容が変わっていれば取り込みを拒否する。通常の OS ビルドはこの限定ソースと固定構成を用いる。FFmpeg CLI や外部プレーヤーは OS に入らない。

## 構成

- `libavcodec`, `libavformat`, `libavutil`, `libswresample` の静的 C 構成。実コンパイル閉包は338 C / 759 manifest files。`libswresample` の generic C は native Opus の SILK 経路とステレオ混合行列にも必要で、外部の libopus/libvorbis/libvpx は入らない。
- デコーダー: MP3, FLAC, AAC, H.264, MJPEG, VP8, VP9, Opus, Vorbis, rawvideo、および WAV 向け PCM。
- コンテナー: WAV, MP3, FLAC, ADTS AAC, AVI, MOV/MP4, Matroska/WebM, Ogg, MPEG-TS。
- network、URL protocol、device、filter、encoder、外部 library、pthread/Windows thread、assembler/GPL 最適化を無効にする。
- native `media.c` の custom AVIO が Nocturne の `read/lseek/fstat` または所有メモリーに接続する。codec handle は単一所有・単一実行コンテキスト専用。FFmpeg の無 thread ビルドを勝手にマルチスレッドで使用してはならない。
- PCM 出力は 48 kHz stereo。mono は両側、最大8チャンネルの既知 speaker layout は `swr_build_matrix2` による正規化行列でステレオへ混合する。center/surround を約 -3 dB、LFE を弱く混合し、S16 の飽和処理を行う。並びの不明な複数チャンネル、行列が混合できず欠落する speaker は明示拒否する。mask のない1/2チャンネルだけは標準 mono/stereo と解釈する。リサンプルは従来の線形補間で、各フレーム末尾は最後のサンプルを保持する簡易処理。
- 動画出力は 8-bit YUV420/422/444、RGB/BGR、gray から opaque ARGB へ。VP8 は native 8-bit YUV420、VP9 は profile 0 / 8-bit YUV420 / SDR に限定し、10/12-bit の上流 DSP がコンパイルされても高bitdepthやHDRを再生能力に含めない。10-bit H.264、AV1、HEVC は再生機能として宣言しない。
- Ogg demuxer に必要な Dirac/Theora/Speex の header parser が含まれても、それらの decoder は無効。VP8 の decoder/parser は明示有効化した。コンテナーの header parser が存在することだけを codec 再生対応と混同しない。
- browser は既存 fetch/CORS、native worker の匿名 HTTP Range 入力、または有界単一GETのmanifest/segment入力を使う。native MSEは既存packet buffer/別workerを維持する。HLS/DASH入力はNocturne側の `media_adaptive.c` がclear VOD manifestを解析し、各segmentをcustom AVIOでdemuxしてpersistent decoderへ供給する。FFmpegのHLS/DASH demuxer・network protocolは有効化しない。
- adaptiveの対象はmuxed HLS TS/fMP4 VOD、およびstatic単Period DASHの有限SegmentTemplate/SegmentTimeline（分離AV最大2track）。manifest256KiB、segment8MiB、init1MiB、2048segment/track、最小対応bandwidthの固定選択。DRM/暗号化、live/LL-HLS、alternate HLS rendition、DASH dynamic/複数Period/SegmentBase/SegmentList、未知control extensionを明示拒否する。全サイト対応やABR切替を宣言しない。実Nocturne再生の検証範囲はrootの受け入れ記録に従い、対象compileだけを映像/可聴音声の証明としない。

## 再生成

先に Nocturne の `build/sysroot/usr/lib/libc.a` を作る。その後:

```
python ports/ffmpeg/prepare.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2" --profile adaptive --stage build/media-adaptive/fresh-reproduction --import-vendor
```

空白を含む upstream パスの configure 制限を避けるため、`build/` 内の新しい stage に複製して in-tree 生成する。既存 stage は拒否し、前段階の `build/nocturne-platform/media/reproduce-stage` と証拠は上書きしない。`--profile legacy` は旧7経路の構成を再現する選択肢として残す（その場合も新しい `--stage` を選ぶ）。相対 include/link path は stage の深さから生成する。元ソースは読み取りのみ。native mmap/fcntl/mkstemp を POSIX file helper と誤検出した３項目は専用構成で明示的に無効化する。`ports/ffmpeg/include` の scoped C header glue は OS グローバル ABI を変更しない。

上流の library 規則と同じく内部 C は `-DHAVE_AV_CONFIG_H -D_ISOC11_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE` 付きでコンパイルする。公開 API の利用者には `HAVE_AV_CONFIG_H` は不要。`media.o` にも専用 `errno.h` を先頭 include に用いること（`EDOM > 0` による `AVERROR` の符号判定を維持するため）。

`ports/ffmpeg/make-fixtures.py` は host FFmpeg 8.1.1 で自作 tone/pattern の実圧縮回帰入力を生成する。host バイナリーは OS の dependency ではない。生成 clip は実サイトの受け入れ試験の代用品ではない。

追加 WebM/Ogg 入力は `ports/ffmpeg/make-webm-fixtures.py` で生成し、専用 `tests/media-fixtures/webm-manifest.json` に引数・codec/profile・hash・host参照PCM数を残す。旧 fixture と旧 manifest は変更しない。能力宣言は `user/libc/media_types.c` の限定 allow-list と実 native/browser 試験を照合する。`vp09` の profile/bitdepth/color suffix は [WebM公式の定義](https://www.webmproject.org/vp9/mp4/) に従い、未対応 profile/HDR は拒否する。旧 `vp9`/`vp9.0` と WebM の Opus/Vorbis 記法は [公式 container guideline](https://www.webmproject.org/docs/container/) を参照する。

## ライセンス

この構成の configure 判定は LGPL-2.1-or-later。元ライセンス・著作者ヘッダーを保持し、同梱 `COPYING.LGPLv2.1` と `LICENSE.md` に従う。静的配布では対応ソース、変更、再リンクに必要な Nocturne object/build 手順も利用者が取得できるようにする。`sources.mk` を lazy static link へ含め、実体の不要なアプリへ decoder を丸ごと入れない。

MJPEG が使用する `libavcodec/jrevdct.c` は Independent JPEG Group の成果に由来する。**このソフトウェアは Independent JPEG Group の成果に一部基づく。** 同ファイルへの追加・削除・変更はない。その他の source と同様に manifest で未改変を検査する。

制約付き移植の保証範囲は Nocturne 内の実 codec 試験・GUI/browser 実経路の記録で判断し、コンパイル成功を音声の可聴性・A/V 品質・全サイト対応の証明とはしない。
