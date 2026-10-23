# data Fetch 共通経路の修理（初回整理）

## 製品変更

親担当が実Puterで確認した最初の失敗は、zstd loader が要求する `data:application/wasm;base64,...` を Fetch が HTTP(S) 専用として拒否したことである。実サイトの成功やFirefox起動を、この補助試験の結果で代用しない。

- `util.c` / `webi.h`: Fetch の data URL processor を共有化。fragment 除外、本文だけ percent decode、forgiving-base64、MIME parse/serialize（quoted parameters・先勝ち重複を含む）。動的本文/MIME、算術・実確保失敗、native checkpoint 取消を区別する。
- `js_encoding.c/h`: 同 processor の結果を実際の QuickJS ArrayBuffer へコピーし runtime 課金する共通 private wrapper。Window / Worker はそれぞれの opaque に合う既存 poll を渡す。返却 tuple は own data properties で作る。
- `js.c` / `browserjsworker.c`: private `host.dataURL` を接続。既存 script decoder と `doc.c` の画像/CSS decoder は共有 processor に委譲する。
- `js_fetch.js`: 既存 Request/Response/bodySlots/SRI とローカル data を接続。ネットワーク・CORS・HTTP cache 等の専用拒否より前に処理し、basic 応答、HEAD の null body、Abort reason、task、immutable Headers、observer flow control を保つ。
- `js_worker_runtime.js`: 独立した既存 workerFetchBridge へ同じ native decoder を接続。Worker 非空 integrity は従来どおり明示 NotSupportedError とし、黙って無視しない。
- `js_xhr.js`: 非streaming fallback の既存 Blob と同様に data を Fetch 共通境界へ送る。streaming observer には request-body 完了を先に通知する。

## 実行済み補助確認

- production `util.c` をリンクした `data_url_hosttest`: **108 checks、0 failed**。HTTP canonicalizer は未使用なら呼ばれないことを abort stub で保証する。初回リンクは未使用 HTTP 関数の依存不足で失敗し、この責務分離後に成功。
- production C decoder + production JS Fetch/XHR を組み合わせた Node 補助実行: **53 checks、0 failed**、streaming host / 非streaming host の両条件。これは QuickJS native wrapper や QEMU の成功ではない。最初の補助実行には harness の installHandlers / Document がなく失敗し、未使用の周辺環境を補った後の結果である。
- 関連 JS の構文、共通 native wrapper の C 構文、CRLF を考慮した対象差分検査は成功。
- 親担当の独立 cross-compile では `doc.c` の既存 fragment percent decode が共有していた `hexv` の削除漏れ依存により失敗した。navigation 近傍へ既存 hex helper を保全して修理し、正規 `UCFLAGS` 相当による `build/u/user/libc/web/doc.o` 単独再コンパイルが成功（既存 sandbox 未使用関数警告2件のみ）。他 object の親担当コンパイル成功と、この修理後の単独結果を区別する。
- guest の既存 Fetch 群へ data 53、Wasm core へ data→実 instantiate / instantiateStreaming 2、実 HTTP Worker へ data / Abort / 実 Wasm 21 の検査を追加。**この記録時点で新ソースの guest 結果は未確認**。

## 仕様と限界

一次仕様: [Fetch main fetch](https://fetch.spec.whatwg.org/#main-fetch)、[scheme fetch](https://fetch.spec.whatwg.org/#scheme-fetch)、[data URL processor](https://fetch.spec.whatwg.org/#data-url-processor)、[MIME parse/serialize](https://mimesniff.spec.whatwg.org/#parse-a-mime-type)、[Infra forgiving-base64](https://infra.spec.whatwg.org/#forgiving-base64-decode)。

新 Fetch の MIME は動的で、4096-byte parameter を確認済み。既存 script loader の短い MIME essence 出力契約はこの Fetch 経路へ流用していない。Worker Request 全面統合、Worker integrity、JSPI、shared memory、Module clone、Puter Firefox 起動は完成を主張しない。配布媒体と実画面の受入は親担当が別途確認する。
