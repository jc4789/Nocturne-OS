# WebAssembly Promise / Web API 初回報告（2026-10-14）

## 担当と実装

- 担当: `wasm_web`。`user/libc/web/js_wasm.js` と `tests/js_wasm_cases.js` を追加。
- `compile` は BufferSource の実 byte offset / length から即時 snapshot を取り、同期 Module の検証を次の timer task に送り Promise へ解決する。
- `instantiate` は byte 入力では `{module, instance}`、Module 入力では Instance を返す。Module 入力の作者 import getter は呼出し時に読み、同名 import の個別読取値を native の順序どおり private queue から再生する。start/core 実行は次 task に分離する。
- streaming は Response の捕捉済み getter と arrayBuffer を使用し、作者の上書きによる brand / header / status / body 検査の迂回を避ける。Fetch / Streams が body の locked / disturbed / abort と所有権を処理する。
- MIME は前後の HTTP tab / space を除いて `application/wasm` と ASCII 大小区別なしに一致する場合だけ許可する。パラメータ付き、複合値、欠落を拒否する。これは現行 Web API §2 の規定であり任意の制限ではない。
- `setTimeout` は作者実行前に捕捉する。window 専用 host.timer に依存せず Worker realm と共有できる。正規 include / init / jstest selector は主担当の所有範囲。

## 確認

- `node --check user/libc/web/js_wasm.js`: 終了 0。
- `node --check tests/js_wasm_cases.js`: 終了 0。
- Node v26.8.1 の native WebAssembly と Node Response / Streams 上で full `runWasmCases()`: `WASM facade host checks 135`、終了 0。
- Node VM に製品 `js_fetch.js` と `js_streams.js` を読み、正規 `third_party/web_streams/ponyfill.mjs` の export を既存 js_embed.py と同じ return へ変換して組み込んだ接続確認: `WASM real Fetch/Streams bridge checks 135`、終了 0。native Wasm の実行は Node を使用しているため WABT / Nocturne の確認ではない。
- 最初の接続確認は shell による replacement 中の `$1` 展開で Streams 戻り値が空となり失敗した。replacer 関数による同じ正規変換に修正した後の結果だけを上記へ記載している。製品を変更して通した失敗ではない。

試験内容は i32 / i64、import / export と object identity、Memory grow と detach、Table、Global、start、trap と作者例外 identity、bulk memory、SIMD、Module / bytes overload、即時 snapshot、重複 import getter、MIME、Response brand、body 使用済み / lock、source / body 拒否理由の保持である。未対応機能を skip する処理や固定成功値は追加していない。

## 仕様と未確認

- JS API: https://webassembly.github.io/spec/js-api/index.html
- Web API: https://webassembly.github.io/spec/web-api/index.html
- streaming の全 body 取得後コンパイルは仕様が許す buffered model。並列コンパイルや速度改善の証拠ではない。
- `js_html_safety.js` の dynamicCode は Trusted Types の文字列 sink 専用で、Wasm byte の実行許可には使わない。`script-src` / `wasm-unsafe-eval` は native Module 共通境界での接続が必要という調査結果を主担当へ通知済み。
- 実 Nocturne native Wasm、jstest、Worker、実サイト表示・操作・性能、配布媒体はこの担当では未確認。
- 最新受入は百度百科、指定 YouTube `https://www.youtube.com/watch?v=clh5LrNDj-o`、CNN の三サイト。最大化と JavaScript コンソールを開いた状態で主担当が QEMU を確認する。Squoosh / Photopea は受入へ計上しない。

## 次

作者が prototype / global API を変更した場合の非迂回性と Worker で必要な bootstrap 順序を独立に見直し、第二報告にする。

## 統合レビュー後の更新

主担当から `nearbyint` の境界確認を依頼されたため、f32 / f64 の `nearest` 実命令を各型13件追加した。ties-to-even、符号付きゼロ、最小 subnormal の正負、無限大の正負、NaN を含む。さらに捕捉済み timer / Module / Object.create と Response / Headers prototype の改変境界を6件追加した。

最終の二経路はともに終了0であり、Node標準 Response では `WASM facade host checks 167`、製品 Fetch / Streams では `WASM real Fetch/Streams bridge checks 167`。構文検査も再度終了0。初回135件は以前の試験範囲であり、最終結果へ使い回していない。独立再考察で検出した既存依存と試験条件の訂正は第二報告へ記録する。

さらに native Module が次 task で compile options を再変換する依存を確認し、private builtins snapshot に専用 iterator を設けた。task 間の Array iterator 改変を加えた最終結果は二経路とも **168件・終了0**。realm、Symbol.iterator、TypeError も bootstrap 時点に捕捉し、作者による globalThis / Symbol / TypeError の置換を既存の改変試験へ含めた。二ファイルの構文検査も終了0。

## 追加の native / Worker 接続依頼

native 担当と主担当の追加依頼により、同じ試験ファイルへ `runWasmNativeBoundaryCases()` と `runWasmWorkerCases(url)` を追加し、HTTP fixture が読み込む独立 Worker script `tests/js_wasm_worker_cases.js` を追加した。binary encoder は同じ試験ファイル内で共有する。

- 追加 native suite: new 必須、未知/重複 builtin、同期 options 辞書、Wasm 命令による memory.grow の detach、外側 Instance 変数を保持しない exported function / Table、externref、multi-value iterable、埋込み NUL 名、Wasm 再入と内部 trap 回復。
- Node v26.8.1 では初めの16境界を通過後、同期 primitive options の TypeError 期待で終了1。Node 自身が primitive options と重複 builtin を許容することを別途確認している。現行 draft に基づく Nocturne の期待は弱めず、基本168件とは別の suite にした。
- 独立 Worker script を隔離 Node VM で実行した補助確認: `ok:true, checks:8`。Module / bytes overload、callback、Memory detach、compileStreaming / instantiateStreaming が42を返した。これは HTTP Worker / Nocturne での成功ではない。
- `runWasmWorkerCases(url)` は本物の HTTP Worker を起動して結果を受け取り、成功・失敗とも finally で terminate する。structured cloned Module へのすり替えはない。root が fixtures routing と jstest selector を接続する。
- 追加後の基本 `runWasmCases()` は168件・終了0。js_wasm.js / js_wasm_cases.js / js_wasm_worker_cases.js の構文確認は終了0。

## 初回 QEMU 結果を受けた転送安全性の追加

主担当から初回 Nocturne 実行の失敗を受け取った。`build/wasm-native-20261014/test-serial.log` を読み、基本 suite と Worker は Memory.grow の戻り値で失敗したことを確認した。native 担当が修理中であり、この報告から修理後の成功を主張しない。boundary の完了行が見えない件は jstest の receive_log が ` API checks ` だけを転送し、`WebAssembly boundary checks` を捨てる点を特定し、主担当へ通知した。marker 待ちの終了だけから suite 未実行とも完全成功とも断定しない。

root が発見した Wasm backing ArrayBuffer の transfer による UAF 反例に対し、facade を固定したまま必須回帰を追加した。

- boundary は41項目。transfer / transferToFixedLength の TypeError、structuredClone transfer の DataCloneError、拒否後の memory buffer と通常 transfer list 要素の未 detach、通常 ArrayBuffer の両 transfer と clone transfer、転送しない memory buffer clone の独立 copy、host grow による正当な detach を含む。
- Worker は script 内21項目と親8項目の計29項目。上記に加え上下両方向の Worker postMessage でも Wasm buffer 転送を拒否し、元 memory を保持する。
- Node v26.8.1 の実 worker_threads / MessagePort を使う補助では Worker script が `ok:true, checks:21`、終了0。HTTP DedicatedWorker / Nocturne の成功に置き換えない。
- Node boundary は新規転送検査を含む先頭33項目を進み、既知の同期 primitive options 期待で終了1。Nocturne の期待を Node に合わせた skip は入れていない。
- 追加後の基本 suite は168項目・終了0、両 tests ファイルの構文検査は終了0。最終 Nocturne の再実行は主担当へ引き渡す。

根拠は WebAssembly JS API §5.3 の `ArrayBufferDetachKey` 設定と、ECMAScript の ArrayBufferCopyAndDetach / DetachArrayBuffer。作者の transfer はキーなしのため拒否し、host Memory.grow の内部 detach は許可されなければならない。

- https://webassembly.github.io/spec/js-api/#memories
- https://tc39.es/ecma262/2025/multipage/structured-data.html#sec-arraybuffercopyanddetach
- https://tc39.es/ecma262/2025/multipage/structured-data.html#sec-detacharraybuffer
