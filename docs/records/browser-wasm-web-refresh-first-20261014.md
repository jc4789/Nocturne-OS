# WebAssembly Web API・data Fetch・JSPI 第一報（更新版、2026-10-14）

担当は `wasm_web`。本書は旧 Web API / data Fetch 報告の後に固定した実装・試験をまとめる。旧記録は削除せず、当時の未確認事項を後の成功へ書き換えない。controller の同期・登録は主担当の順番指示待ちであり、ここでは実施していない。

## 製品経路と担当範囲

- `user/libc/web/js_wasm.js`: 本物の native Module / Instance に接続する Promise overload、即時 byte/options/import snapshot、Response による buffered streaming。作者実行前に捕捉した API を使い、Window 専用 timer に依存しない。
- `html_policy.c/h`: 既存 policy 保管・継承・snapshot 内へ Wasm コンパイル許可を追加。script-src 優先、default-src fallback、unsafe-eval / wasm-unsafe-eval、先勝ち重複、複数policyの共存を扱う。違反通知と report-only 非拒否は主担当の Window / Worker callback が既存 queue へ接続した。
- `util.c` / `webi.h` / `js_encoding.c/h`: 共通 data URL processor と runtime 課金 ArrayBuffer wrapper。本文だけ percent decode、fragment 除外、forgiving-base64、MIME parse/serialize を共有する。動的確保・算術限界・OOM・取消を扱い、新しい本文/MIMEの人工上限は設けない。
- `js_fetch.js` / `js_worker_runtime.js`: それぞれの既存 Fetch 境界へ data URL を接続。private native hook は `js.c` と `browserjsworker.c`。task、Abort reason、HEAD、immutable Headers、body / clone、Window SRI、observer / XHR upload 完了を既存責務へ統合した。Worker 非空 integrity は従来の NotSupportedError を維持し、無視しない。
- 既存 script decoder と `doc.c` の画像/CSS decoder は同じ processor に委譲。`js_xhr.js` の非streaming local fallback も既存 Fetch を使う。独立 cross-compile が検出した `doc.c` の別利用 `hexv` 削除退行は、既存 navigation 責務の helper を保全して修理し、doc.o の実target再コンパイルを確認した。
- native wasm2c/TCC・GC・JSPI 本体は別担当の製品変更。本担当は `js_wasm.cpp` / WABT host-resource 境界を独立レビューし、具体的な寿命・取消反例を通知した。修理内容と根拠は第二考察へ分離する。

## 仕様と試験の対応

Wasm の根拠は [JS API](https://webassembly.github.io/spec/js-api/index.html) と [Web API](https://webassembly.github.io/spec/web-api/index.html)。CSP は [Wasm byte compilation gate](https://w3c.github.io/webappsec-csp/#can-compile-wasm-bytes)、data URL は [Fetch processor](https://fetch.spec.whatwg.org/#data-url-processor)、[MIME parse](https://mimesniff.spec.whatwg.org/#parse-a-mime-type)、[Infra forgiving-base64](https://infra.spec.whatwg.org/#forgiving-base64-decode) に照合した。

JSPI はユーザー指定の [Overview](https://github.com/WebAssembly/js-promise-integration/blob/main/proposals/js-promise-integration/Overview.md) と [現行規範 JS API](https://webassembly.github.io/js-promise-integration/js-api/) を区別する。旧 Overview の IsPromise 時だけ suspend する記述ではなく、現行 formal の intrinsic PromiseResolve / Await と active promising context を期待値にした。

`tests/js_wasm_cases.js` は基本、native境界、HTTP Worker 親側、CSP に加え、独立 entry `runWasmJspiCases()` を持つ。JSPI は実 Wasm import/export、sync / Promise / thenable、timer、拒否理由、複数中断、重なる呼出し、再入、作者JSframe横断拒否、i64 / externref / multi-value / void、引数変換を確認する。未対応を skip や固定値で隠さない。

## 本担当が実行した補助確認

- production C data processor: 108検査、失敗0。
- production C decoder と production JS Fetch/XHR の Node 接続補助: streaming / 非streaming の両条件で53検査、失敗0。初回の harness 周辺環境不足と、修理前の POST upload 完了通知不足を、修理後結果とは区別した。
- 関連 JS 構文、native wrapper の C 構文、CRLF を考慮した差分検査、修理後 doc.o の target compile は成功。これらだけを QuickJS / Worker / QEMU / 実サイトの成功とはしない。
- Node v26.8.1 の JSPI 本体は Window suite の最初の71検査後、末尾の引数変換を同期 throw して formal 期待と不一致になった。期待を弱めず保持し、Nocturne の74検査結果を別に確認した。

## 最新 Nocturne ログの独立確認

実行と画面操作は主担当が行い、本担当は次のログを読んだ。目視・操作を自分が行ったとは主張しない。

`build/wasm-jspi-first-20261014/test-serial.log`:

- 15群成功、失敗0、skip0。
- Wasm 基本170、native境界41、Window JSPI74、HTTP Worker50。
- Window CSP 17 / 12 / 22 / 17、HTTP Worker CSP44、Fetch297、XHR89。
- guest wasm2c → guest TCC → native executable の実行結果42。script lifecycle、storage、DOM、network、Worker、native thread の既存回帰群も含む。

`build/nocturne-platform/wasm-jspi-puter-first-20261014/serial.log`:

- `WASM_GC_PASS iterations=256`。native module258 / call259 / fallback0の既存GC境界に続き、中断中 code/ref のGC、interpreter fallback thread のref保持、retirement unwind、取消後の次import / thenable getter非実行、settlement中context保持が成功している。
- TCC guard は29成功、失敗0。これは無制限の安全性証明でも、実サイト成功でもない。
- metadata の媒体payload: kernel SHA256 `4fb811e718dc681b0b8c0731451dfd73a08767e4ccee1bb5bb63aee23257a76d`、initrd SHA256 `5aca4867c938b12d338e22b9452d97154542bc1501197e63e7864b13f40cb55d`。WHPX、4 CPU、4096MiB、独立snapshot、ユーザーdata未接続。後続撮影は1920×1080、初期boot撮影は1280×800と区別する。

### 追加 Worker JSPI の最終実結果

`tests/js_wasm_worker_cases.js` の既存 HTTP Worker entry へ12検査を追加して固定した。defined Wasm relay が import 復帰後に算術を行うため、API名やexported importだけの確認ではない。real API、非callable Suspending marker、sync / Promise / thenable / timer / data Fetch の native再開、ordinary SuspendError のcallback非実行、拒否理由identityを含む。

構文・差分確認と Node 本物JSPIによる追加ブロック12検査は成功。ただしこの補助実行は実Workerではない。

その後、主担当が同じ製品で追加試験を実行した。本担当が `build/wasm-jspi-worker-final-20261014/test-serial.log` を独立に読み、69行の **WebAssembly worker checks 62**、harness 8検査・失敗0、1群成功・失敗0・skip0を確認した。`build/wasm-jspi-worker-final-20261014.log` も全成功・全体48秒を記録する。既存50に新12が加わった本物のNocturne HTTP Worker結果であり、旧50やWindow74の成功の流用ではない。これで当該試験sourceを固定する。

## Puter の実際の到達点と未達

対象は [Puter Firefox Wasm](https://developer.puter.com/labs/firefox-wasm/)。主担当が最大化・JS console を開き、有効になった Launch Firefox の実targetを確認して実クリックした。

- 以前の `fetch supports HTTP(S) resources only` は共通 data Fetch 修理後に前進した。サイト自身の loader Wasm が wasm2c/TCC で実行された。
- 初期ロードで native call5。Launch実クリックで同loaderが再構築され、後続累積は module2 / 108788 bytes / native call10 / fallback0。Retryの実クリックはしていない。
- `console-evidence.log` 446行付近に `TypeError: cannot read property 'waitAsync' of undefined`。現製品の Atomics 未公開が次の実失敗であり、Firefox UI / Firefox内Puterページには未到達。
- 同consoleのJSPIは suspensions0 / resumptions0 / pending0。JSPI APIが存在してLaunchが有効になったことと、この実ページでJSPIの中断・再開が使われたことは別である。

shared Memory / SAB / Atomics / Module structured clone は未実装のまま。名前だけの追加、global削除の撤去、shared=trueの強制許可、DOM差替え、Launchの強制有効化、代替Firefox画面は行っていない。接続候補は `browser-wasm-shared-integration-notes-20261014.md` に保全し、現在の成功へ数えない。

百度百科・指定YouTube・CNNの旧媒体での実操作は主担当の別記録。最新JSPI媒体のPuter / 百度百科受入は主担当が継続中であり、古い成功を最新の回帰なしへ繰り上げない。Photopea / Squoosh はユーザー受入対象に含めない。性能改善、音質、Hyper-V実機、WebAssembly全仕様完成は本書から主張しない。
