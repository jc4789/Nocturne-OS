# 媒体暗号の実装（2026-10-12）

## 製品経路と契約

- `user/libc/web/js_crypto.c/js` の既存責務へ AES-CBC / AES-CTR / AES-GCM を統合。BearSSL の定数時間 CT64 実装を使用し、鍵の raw / JWK import / export、generate、CryptoKey の非公開 brand、用途・抽出可能性、Promise のエラー、BufferSource の同期 snapshot を接続した。CBC は PKCS#7 を付加・検査。CTR は 1〜128 ビットの指定範囲だけを増加し、繰返しになる長さを拒否。CTR は 1 block ごとの暗号初期化ではなく low-32／指定 field の境界までまとめて処理する。GCM は CT64 と定数時間 GHASH を使い、32 / 64 / 96 / 104 / 112 / 120 / 128 bit tag、IV、AAD を接続し、認証に失敗した plaintext を公開せず消去する。
- `user/libc/web/js_eme.js` は一時 ClearKey session／license の独立責務。`requestMediaKeySystemAccess` → `createMediaKeys` → `setMediaKeys` → `generateRequest` → JWK `update` → native key snapshot まで接続する。MP4 / `cenc` / `keyids` と一時 session だけを実際のコーデック能力に基づき選択し、他の key system、堅牢性、永続ライセンス、暗号 scheme は拒否する。
- `js_mse.js` の通常 append 経路から初期化 segment の PSSH を増分で読み、chunk を跨ぐ header／payload を組み立てる。SourceBuffer 別に最大16 byte headerとPSSH断片のみを保持し、mdatや全media payloadを累積保存／再コピーしない。完全な親boxとnative append成功ACKの両方を待って`encrypted` eventを配送する。abort / changeType / remove / detach / error は部分状態と未配送eventを取消す。DOM、ストリーム、Shaka の関数を差し替えない。共有 bootstrap の include / handler brand は主担当が統合する。
- native `avmedia("emeKeys", node, buffer)` の buffer は `{KID 16 bytes, AES key 16 bytes}` の packed records。空 snapshot は全鍵を取り消す。element の MediaKeys は media の `load()` を越えて残るが、文書／element の破棄で native 鍵を消去する。媒体担当が所有する `avmedia.c` と MSE worker 操作へ接続した。
- `media_mse.c` は FFmpeg が解釈した `AV_PKT_DATA_ENCRYPTION_INFO` を使う。保持した packet は暗号化状態のまま。decode 直前に copy-on-write packet を作り、CENC AES-CTR を復号して暗号 marker を除去する。clear subsample を保持し、暗号部分の partial-block counter を連続させる。鍵未着は cursor を進めず EAGAIN、鍵 snapshot の到着で再開する。暗号文を decoder へ渡さない。
- IPC は v4 `NMSW_KEYS`。鍵 payload は完了／取消／破棄時に volatile wipe。`key_waiting` は metadata と browser state へ接続し、`waitingforkey` を配信する（worker／時計担当の変更）。

## 確認と失敗の扱い

- 実 QuickJS + 実 BearSSL の focused 確認：AES 48 条件成功、EME lifecycle 25 条件成功。AES は NIST CBC/CTR vector、padding、用途、brand、抽出禁止、raw/JWK、snapshot、counter 周期を確認。EME は一時 session、license message、native snapshot、PSSH、revocation を確認した。**EME の媒体 bridge は記録 mock であり、暗号動画の再生成功ではない。**
- 途中の確認 harness に、BearSSL little-endian codec の link 不足、URL より前の encoding 初期化不足、host UTF-8 binding 不足があった。それぞれ依存／setup を訂正した。snapshot の失敗は、破損 padding 入力の test が基準 ciphertext の同じ ArrayBuffer を変更していたため。独立 copy へ訂正し、期待値を弱めず確認した。
- native crypto / MSE / parser の freestanding syntax 確認成功。
- 後続追加の重点確認：実BearSSL GCMのNIST vector／認証拒否／AAD snapshot／各tagLength／JWKは16条件成功、SourceBuffer別PSSHの1byte分割／abort／未配送event取消／新generation／独立scannerは5条件成功。Native ACKで配送を確定する修理後にも重点範囲だけ確認した。以前のCBC/CTR48条件とEME25条件を全て繰り返す形にはしていない。GCM追加直後の`stdbool.h`不足によるcompile errorはheaderを追加して修理した。
- `tests/media_cenc_test.c` は実 native packet callback と FFmpeg packet side data を使う 11 条件の guest 確認用。主担当の媒体バッチに引き渡した。ここでは guest 実行成功を未確認として扱う。
- 全体ビルド、ネットワーク越しの AES-128 HLS／ClearKey 動画、実 GUI と音声の受入は主担当の結果と分離する。暗号担当は VM を起動していない。

## 残る範囲

- RSA、署名、derive、wrap / unwrap は未実装で明示拒否。
- 永続 EME、商用 DRM、WebM 暗号、CBCS / CENS / CBC1、`seig` key rotation group は未対応。暗号サイト全般の互換性を主張しない。
- BearSSL は既存依存を使用し、新しい暗号 vendor は導入していない。今回のライセンス編集は行っていない。

仕様：[W3C Web Cryptography](https://www.w3.org/TR/webcrypto/)、[W3C Encrypted Media Extensions](https://www.w3.org/TR/encrypted-media/)。

## 実YouTubeの追加MSE境界修理

- `build/nocturne-platform/media-yt-repaired-20261012/serial.log` では音声が19.0秒へ達した時に映像が11.1秒で、後に18.8秒へ飛んだ。parser/appendの失敗ログはなく、codecのcatch-upと別VIDEO laneへのPCM先行は媒体時計担当が修理している。暗号担当はこれを暗号障害やparser原因と断定していない。
- 独立に確実な公開遅延を修理：合法なMP4 `trex.default_sample_size` 継承を以前のframing parserが持たず、完全な`mdat`到着後も次fragmentのheader／EOSまで保留していた。initのtrack defaultsを一度だけ抽出してimmutable配列として保持し、track IDのbinary lookupで`tfhd`／`trun`のサイズを補う。明示`tfhd`サイズは従来通り優先し、閉じた`mdat`から現在segmentを直ちに公開する。次initへの切替は成功時のみ、失敗／changeType／破棄でmetadataを正しく回収する。
- 新試験や既存検査の反復はせず、変更した製品Cのfreestanding syntaxだけ確認。実YouTubeが当該`trex`形式だったかは未確認であり、この修理単独で映像欠落が解消したとは報告しない。
