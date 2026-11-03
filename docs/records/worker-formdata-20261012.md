# Worker Fetch の本文と FormData（2026-10-12）

## 製品変更

- `js_worker_runtime.js` は Window と同じ `js_formdata.js` を正規 include する。multipart／URL-encoded の解析、binary File、multipart の生成を重複実装しない。
- Worker の Response は共通 Streams の実本文を持つ。`formData()`、`blob()`、本文の消費・lock・disturbance・tee clone・abort を接続した。HEAD／204／205／304／opaque の本文は null。作者コードの実行前、START のネイティブ policy URL 確立後に crypto と FormData を初期化する。
- Fetch は FormData、Blob／File、BufferSource、URLSearchParams、UTF-8 文字列を実バイトへ snapshot し、自動 Content-Type と request header guard を適用する。GET／HEAD の本文、禁止 method、no-cors の非 safelisted method を拒否する。
- `browserjsworker.c` は private IPC で `[url, method, headers, ArrayBuffer|null]` を送る。`js_worker.c/h` の requester は親の既存 native request へ接続し、startup／importScripts の GET loader を保つ。本文は文字列化しない。空の非 null ArrayBuffer も本文ありとして渡す。
- 文書世代・Worker owner・通信取消・flow control・CORS／cookie／redirect authority は既存の親 transport を維持する。Worker の close では保留 Fetch を AbortError で終了する。
- Blob／multipart の旧 16MiB quota を、QuickJS が実際に使う `INT32_MAX` の ArrayBuffer 表現可能長へ置換。加算 overflow、実確保失敗、native wire 制約は残す。
- `js_worker_embed.py` は Window と同じ pinned Streams ソース・manifest SHA-256 を使用する。生成物だけは編集していない。

## 仕様根拠

- [WHATWG Fetch §5.2 BodyInit](https://fetch.spec.whatwg.org/#bodyinit)：型別の本文抽出・byte snapshot・自動 Content-Type。
- [WHATWG Fetch §5.3 Body.formData](https://fetch.spec.whatwg.org/#dom-body-formdata)：本文消費後の MIME 別解析と unusable body の Promise rejection。
- [W3C File API](https://w3c.github.io/FileAPI/#blob-section)：Blob／File の実バイト所有・metadata・stream。

## 境界・未確認

最新の追加試験停止指示に従い、このまとまりの build・generator・fixture・実 VM／実サイト POST は未実行。ソース接続を実ネットワーク成功として報告しない。主担当が共有 Makefile の依存と正規生成を担当する。

Worker の Request クラス、stream upload、SRI、default 以外の cache／redirect／credentials 等、Blob／File の Worker 間 structured clone は今回の有限範囲外。未対応 upload／options は明示拒否し、空成功で隠さない。FormData(form, submitter) と FormDataEvent は Window 専用の既存フォーム境界へ別担当が接続し、Worker の引数あり constructor は拒否する。

## Blob Worker の暗号権限の修理

HTTPS 文書の `Worker(blobURL)` で `subtle`／`isSecureContext` が失われる欠陥を修理した。private START metadata の field 10 に作成時の native `c->origin` snapshot を送り、Worker が所有する `security_url` と private `host.securityURL()` へ接続する。blob の場合だけ crypto の trust 判定が creator origin を使う。通常 Worker は従来の native policy URL、script URL と CSP documentURI は blob のまま維持する。空・opaque・非 trustworthy origin は fail closed、初期化失敗と Worker 終了で snapshot を回収する。作者の location／isSecureContext は読まない。

根拠は [W3C Secure Contexts §1.3–1.4 と §3.1](https://w3c.github.io/webappsec-secure-contexts/#is-origin-trustworthy)。この修理は creator origin の継承であり、文書 ancestor を含めた広い secure-context 判定の完成を主張しない。追加試験停止を守り、実行確認は未実施。

## 後続の検証許可による結果

ユーザーの「十分変更したので今検証する」という最新訂正により、既存 `worker_stream_hosttest.c` に新 `--fetch-body` 経路、既存 `sharedworker_hosttest.c` に実 `web_worker_run_one` の binary-record dispatcher 補助を追加した。実製品 embedded runtime、native encoding、BearSSL crypto と binary requester に限定し、旧成功済 suite は実行していない。

`python -X utf8 tests/crypto_aes_hosttest.py --worker-body` の最終結果は compile 0、runtime `3221226505`（Windows `0xC0000409` fast-fail）。その前の実評価で native START、blob script location の維持、HTTPS native creator の `subtle` 公開の 3 項目が成功したが、本文／暗号／POST／取消の一括確認は中途停止し未達。`build/worker-body-native-20261012.log` と実行ファイルを保全した。初期 harness compile の CRT ssize_t／SYS_OPEN／HTTP header 衝突、MIN macro の漏出、未使用 URL helper link／署名を fixture 側で修正した経緯がある。guest・実 HTTP/CORS・実サイトの成功へは昇格しない。

主担当が実サイト初期化停止修理への即時切替を指示したため、この host-model fast-fail の追加診断は停止。新 VM は起動せず、主担当の実 VM も操作していない。
