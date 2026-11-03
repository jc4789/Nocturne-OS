# CPU・Canvas・FFmpeg・MSEの統合（2026-10-08）

## 前提と到達点

3担当で製品コードを変更し、独自ABI、GUI、RAM rootfs、`/data`永続を維持した。POSIX環境、fork、外部プレーヤーは導入していない。元FFmpeg木、既存data媒体、ユーザーのHyper-V VMを変更・試験接続していない。

**日常利用OSの完成でも、現代のサイト全般への対応完了でもない。** MSEは存在判定だけのstubではなく、JSから実packet buffer、ネイティブ子プロセス、FFmpeg、Nocturneの音声・描画へ接続した。DRMは明示的に未対応。

## 製品の変更

### CPU

- Limineのx2APIC起動指定、xAPIC MMIOとx2APIC MSRの選択、32bit APIC宛先と64bit ICR、APの機能・モード一致確認。
- 小さい仕事でも必要なworkerだけへ現在epochを投入・join。GUI合成からAP1の専用ring3 runnerを除外し、System Monitorの使用率を実scheduler容量で正規化。
- WHPX・4CPU・2GiBでx2APIC、4/4 APのSSE2、部分仕事stress、3CPU合成のmask `d`と画素・guard一致を実行確認した。証拠: `build/nocturne-platform/oct08-final-native/serial.log`。
- kernel/IRQのBSP所有、専用runnerと純計算APという境界は維持。一般プロセスSMP、user thread、AVX、一般TLB shootdownは未完了。CPUクロックやWindows設定は変更していない。

### Canvas・DOM・ブラウザー

- 実font rendererに接続したfillText/measureText/TextMetrics。native immutable clip maskをsave/restore/reset/freeと共有し、rect/path/text/imageすべてに適用。
- ellipse、arcTo、Path2D（SVGパス・clone・addPath行列）を実native点列へ接続。既存256点と有界曲線分割の制限は残る。
- 透明度を保持するRGBA PNG、96dpi、実toDataURL/toBlob。taint拒否と文書予算を維持。JPEG/WebPはPNGへのfallbackであり、対応encoderとは広告しない。
- private data script/module processorを追加。通常navigation/fetchの権限を広げず、percent/base64、module MIME、opaque相対importを処理。
- Blob/File/FileReaderを実bytesと非同期イベントへ接続。文書単位の実Blob/MediaSource object URL、取消、GET/HEAD、単一Rangeを追加。一般画像/script/CSSのBlob URL loaderは未完了。
- NodeFilter、TreeWalker、NodeIteratorをnative DOM関係へ接続。YouTubeがNode getterをwalker経由に置換しても再帰しない。JS削除のiterator補正はあるが、HTML parser内部の全削除には未接続。
- performance.timingを実navigation/fetch完了/DOMイベントの観測へ接続。未観測DNS/TCP/TLS/responseStart等は0で、偽の時刻を作らない。完全Navigation Timing対応とは呼ばない。

今回のCanvas追加は**WebGLやGPU高速化の完成ではない**。既存virglは限定clear/triangle/readbackのopt-inで、旧測定GPU47ms対CPU5msの制限を覆す新しい高速化結果は得ていない。

### FFmpegとMSE

- ローカルFFmpeg 9.0.2由来のVP8閉包を追加し、原文hashとlicenseを維持。native codec/コンテナ判定に接続した。FFmpegのnetwork/POSIX protocol・threads・CLIは導入しない。
- 5.1等の既知speaker layoutをFFmpegの行列で実stereo downmix。center/surroundとLFEを混合し、未知layout/扱えないspeakerは拒否。
- MP4/WebMのinit・fragment境界を増分処理し、complete spanを一度demuxしてimmutable encoded packetsへ保存。保持履歴の全体再連結・全再decodeではない。persistent codecは一時的な入力枯渇とEOSを区別する。
- 実MediaSource/SourceBuffer/TimeRanges、append/updateイベント、timestampOffset、append window、segments/sequence、remove、abort、EOS/reopen、seek、live seekable range、限定changeTypeを接続。
- initに明示されたMP4 mvhd/mehdまたはWebM Info durationを読み、未取得fragmentの終端から総時間を捏造しない。実netfix MP4のinitは1,907bytes、明示時間60.095秒。
- audio/video最大各1track、muxed1個または分離2個のSourceBuffer。実enabled/selected変更をchild decoderと音声停止/表示停止/再有効化へ接続。activeSourceBuffersはinit取得と実selectionに基づく。
- GUI側のpipe通信は非blockingで、子プロセスがdemux/decodeを所有。generation/sequence/slot incarnation、固定wire、範囲・quota・出力長を確認し、30秒期限とkill/reapを持つ。
- input/staging/encoded packetは各SourceBuffer最大32MiB、文書入力64MiB、最大2SourceBuffer、最大4decoder。FFmpeg系割当は親64MiB、子32MiB予約を終了確認まで保持。JS/HTTP/kernel全heapの総上限ではない。
- cross-origin初回の単一Range GETはFetchのsafelisted条件で不要なOPTIONSを省略するが、**各応答のACAO、表現同一性、長さ、HTTPS境界は引き続き確認**。If-Rangeが必要な要求はpreflightを維持。

MSE仕様参照: [W3C Media Source Extensions](https://www.w3.org/TR/media-source-2/)。ManagedMediaSource、JS dedicated-worker handle、字幕track、audio splice、全codec変更、全規格適合は未完了。暗号化媒体は拒否する。native HLS/DASH manifest loader、HEVC、AV1、高bitdepth/HDRは未対応。WebM unknown-size Clusterや未知sample-size MP4は次境界/EOSまで待つ有界実装。

## 実サイトとHTML5test

Wikipediaを除外した。Nocturne自身のHTTPS・script・画面を使用し、hostブラウザーや静的取得を製品成功に置き換えていない。サイト互換性の自作fixtureは追加していない。

HTML5testは `oct08-html5-list-second` で15ページ送りし、literal一覧と最下部まで親が画像を確認した。**254/555**。CanvasのPath/Ellipse/PNG、Blob URL、MSE等はYes、WebGL、lineDash、hit testing、JPEG/WebP encoder、HLS/DASH等はNoのまま。この検出点数は規格適合・性能・全サイト動作を証明しない。ユーザー添付236と同条件の対照測定でもない。

未対応一覧は確認だけで終えず、上記の実パス・楕円・PNG・Blob URL・MSE・実トラック切替・performance.timingを実装へ戻した。一方、IndexedDB、ServiceWorker、Streams、WebSocket、WebRTC、Pointer Events、full screen等は残る。非標準の旧script execution eventsのprobeを新機能と偽装しない。

公開作者の[実MSEデモ](https://nickdesaulniers.github.io/netfix/demo/bufferAll.html)では、実XHRのfragmented H264/AAC MP4→append→EOS→playをNocturne内で実行した。`oct08-mse-netfix-second`の花・草とウサギの異なる画面を確認し、QEMU AC97出力に非zero stereo PCMを採取した。外部プレーヤーやローカル配信fixtureではない。音声の聴感・長時間安定・厳密lipsyncの証明ではない。QEMU WAV headerの長さが未確定だったので、元ファイルを保存したまま実PCM bodyを解析した。

**最終媒体の分割配信**: [bufferWhenNeeded.html](https://nickdesaulniers.github.io/netfix/demo/bufferWhenNeeded.html) を実Nocturneで130秒動かした。作者ページ自身が実HEADの長さから5回のRange XHRを発行し、9.677/19.321/28.935/38.595秒に次fragmentへ進み、最後のsegment/EOSまで到達した。花・草→ウサギという異なる映像、最終controlsのPlay、JS/MSEエラーなしを確認。実QEMU出力は44.1kHz/16bit/stereo、非zero 5,267,192 samples、最大絶対値9,481。元WAVと解析JSONを `oct08-final-mse-incremental` に保存した。native出力48kHzからQEMU側の変換が入る。高解像度時のpipe転送量・滑らかさ・長時間品質は未保証で、受信budget256KiB/tickの制限を残す。

### 最終媒体の指定サイト（一巡・各90秒）

| 実URL・証拠label | 画面と残る失敗 |
|---|---|
| `https://baike.baidu.com` / `oct08-final-baike-mse` | 中国語header・検索・本文カード・画像を部分表示。Axios API404が残り、全面検索/記事操作は未受入。 |
| `https://amazon.com` / `oct08-final-amazon-mse` | wwwへredirect、header・検索・商品/広告カード。緑の画像崩れ、crypto未対応、not-a-function、single Range worker制限が残る。初回Range CORS preflightの従来拒否は今回のログにはなく、別のworker容量境界へ進んだが、購入/login/全面media成功ではない。 |
| `https://openai.com/ja-JP/index/gpt-6-astra/` / `oct08-final-openai-mse` | exact URL、logo・Cloudflare待機。iframeのdocument参照例外で本文未到達。challenge迂回はしていない。 |
| `https://www.youtube.com` / `oct08-final-youtube-mse` | homeskeletonのまま。旧NodeFilter/PerformanceTiming欠落を越えたがwebcomponentsのprototype参照例外と別not-a-functionが残る。公開polyfillはCDATASectionも期待し、現在globalにはない。動画一覧・YouTube再生成功ではない。 |
| `https://github.com/FFmpeg/FFmpeg` / `oct08-final-github-mse` | header・repository tabs後にUnable to load page。React RouterがHTMLHeadingElement未定義を報告し、hydrationも未完了。 |

5つともharness/QEMU終了0だが、**5サイト全面合格ではない**。画像を親が直接読み、失敗を次の具体的なDOM/JS/描画課題として残した。サイト自身の障害かOS不足か未切分けのAPI404も、勝手にOS修理済みとはしない。上記の実MSEデモとHTML5testは別途追加の実サイトであり、簡単なWikipediaや自作サイトで件数を水増ししていない。

## 証拠と残余

ビルド・統合・実サイトのログは `build/platform-20261008/` と `build/nocturne-platform/oct08-*` に保存。起動imageからkernel/initrdを抽出して現在buildとhash照合する。WHPX4CPU/2GiB、新規scratchとsnapshotだけを接続し、harness終了成功をサイト全面成功とは扱わない。

phase1 nativeはcodec98,593条件・WebM197,320条件で失敗0、VP8/file/所有memory4frameと実5.1左右energyを確認した。これは後のJS MSE全境界を保証しない。各担当二報と独立sourceレビューを保存し、未検証を区別する。

最終媒体 `oct08-final-native-mse` でも、既存jstest **939条件/失敗0**、Canvas **43/失敗0・実paint**、codec **98,593/失敗0**、WebM **197,320/失敗0** を一度確認した。全4summaryの後、guestが正常poweroff・QEMU終了0。harnessは同時のmonitor操作がWinError10054となり形式上失敗を記録したので、元metadataを消さず `native-adjudication.json` に実ログと終了コードによる判定を保存。harness側だけに「正常終了0・全要求marker・guest shutdownが確認できた場合に限る」race処理を追加し、都合のよいテスト再実行はしていない。これら既存回帰は実サイト成功の代用ではない。

Hyper-V Gen2・Enhanced Sessionは人間による検証が残る。一般SMP、実用GPU高速化、完全WebGL、複雑サイト全体、日常OS完成を達成済みとは報告しない。

再構築: `python -X utf8 user/libc/web/js_embed.py && python -X utf8 scripts/build.py`。サイト確認は既存 `scripts/platform_qa.py` の新規label・実URLを用いる。既存dataを接続しない。
