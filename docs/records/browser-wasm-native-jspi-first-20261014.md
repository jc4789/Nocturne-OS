# ブラウザー Wasm ネイティブ実行・JSPI 第一報

担当は `wasm_native`。2026-10-14 の固定製品ソースについて記録する。旧インタープリター媒体の成功を新実装へ流用せず、下記の最新ゲストログを独立に読んだ。実サイトの画面・実入力は主担当の観測であり、自担当の目視としては報告しない。

## 製品へ接続した経路

- `user/libc/web/js_wasm.cpp` は検証済み WABT Module の IR を `WriteC(host_resources=true)` で C へ変換し、C-only の `wasm_rt_compile_tcc` でメモリー内コンパイル・再配置する。定義関数の dispatch は `wasm_rt_native_invoke` から生成機械語へ進む。CLI を置いただけの実装ではない。
- `ports/wabt/include/wasm-rt-host.h` は生成 C とブラウザーの資源境界。Memory/Table/Global/関数参照/segment の唯一の正本は WABT Store であり、生成 C 側へ別 instance/storage を作らない。start は正本の初期化後に一度だけ実行する。Memory の view はアクセス時に更新し、JS import 内の grow 後に古い realloc ポインターを使わない。
- TCCState はネイティブ Module/Instance の実寿命へ結び付ける。JS の Module 変数だけに所有させず、export/Table から生きる定義関数もコードを保持する。
- C の参照 local/stack を shadow frame で記録し、JS 再入前に既存の JS wrapper anchor を保持する。externref の生 JS object だけでは Foreign binding の寿命を保証しないため、戻り参照も anchor 自体を結果変換まで保持する。Memory.buffer は非転送属性を持ち、grow(0) を含め旧 buffer を detach する。
- native 変換の実未対応は理由付き interpreter fallback とし、native 成功 counter と分離する。現在は EH の異なる payload/tag ABI、`return_call_indirect`、import 先への `return_call` trampoline が該当する。cancel/OOM は「未対応」として再実行しない。直接定義関数の tail spill は実 signature 由来の union を使う。

## 本物の中断・再開

[`JSPI` の現行規範案](https://webassembly.github.io/js-promise-integration/js-api/) の active-context 検査と Await を採用した。`Suspending` は通常の export 呼出しからは callback 実行前に `SuspendError`。promising 内では同期値・thenable を含め intrinsic PromiseResolve/Await へ進み、作者が差し替えた Promise.then/species を反応登録に使わない。引数変換の失敗も返却 Promise の拒否になる。

`PromiseTask` は別の実 Nocturne stack 上で正本の Func::Call を実行する。runtime の coroutine が native register/TLS/stack bounds を交換し、bridge は QuickJS frame/例外所有権、State の Active chain、native shadow head と anchors を交換する。中断中の C++ frame はコピー・破棄しない。

fallback 側も `Thread::VisitRoots` と execution-owner cookie によって、その Task の interpreter local/stack の参照を JS graph へ保持する。別の停止 Task の roots を現在の Task へ混ぜない。退役時は再開して no-author trap から通常 unwind し、DONE 後に stack/context を解放する。通常 import・thenable 同化・native/interpreter poll は retired/cancelled を認識する。Promise settlement の then getter が退役を再入しても、settlement 復帰前には Task の context を解放しない。

## 最新媒体で読取り確認した結果

- `build/wasm-full-build-jspi-first.log`: 全体構築成功は主担当確認。自担当の最新 bridge/interpreter 限定クロスコンパイルも終了 0。
- `build/wasm-jspi-first-20261014/test-serial.log:134–201`: 基本170、境界41、JSPI74、実 HTTP Worker50、Window/Worker CSP、ゲスト wasm2c→ゲスト TCC→native の結果42、関連 DOM/Fetch/XHR/Worker/thread 回帰を含む **15群、失敗0、skip0**。metadata の要求・実解像度はともに1920×1080。
- `build/nocturne-platform/wasm-jspi-puter-first-20261014/serial.log:111–127`: TCC guard 29成功/0失敗。GC256反復、buffer/参照/export の寿命、native258 module/259 call/fallback0、JSPI native call260→261、fallback interpreter local の中断GC、退役時の作者 callback 抑止、次の通常 import/thenable getter 抑止、settlement 退役再入が成功。`WASM_GC_PASS`。これを全 OOM/stack 状態の証明とはしない。
- 同実サイトログ:54394 byte の Puter loader は初期 native1 module/5 call/fallback0、JSPI suspension/resumption0。主担当の実 Launch クリック後に同容量 loader を再構築し、最終累積は native2 module/108788 byte/10 call/fallback0/JSPI0。Retry はクリックしていない。続いて `Atomics.waitAsync` 相当の欠落で失敗（log:446 は Atomics 自体が undefined）。Firefox 本体へは未到達。**サイトで JSPI が使われた証拠にはしない**。初期5 callとLaunch後の最終10 callを区別する。

## 残る境界

共有 memory/SAB/Atomics/Worker への Module clone、全 SIMD・EH・Core 3.0・JS API の全面適合を主張しない。現 shared descriptor は拒否し、単独 realloc backing を共有と偽装しない。明示 fallback の存在を native 全命令対応と呼ばない。指定実サイトの全操作、同条件の性能改善、Hyper-V 実機はこの補助試験の成果ではない。旧 GC ハーネスの marker/解像度失敗も成功へ改記しない。

## 担当ソースの固定値（SHA256）

| ファイル | SHA256 |
|---|---|
| `user/libc/web/js_wasm.cpp` | `088268e9581806b9e8171d8ecdf80e807b182f0d1f92aace2e84cfb8efe87dc9` |
| `user/libc/web/js_wasm.h` | `05d58816858d04f66b25144832f906515b2e2526b8c0d7bfa8ec9602c6789b06` |
| `third_party/wabt/src/c-writer.cc` | `d9b7b239de17c25bd7b0eeb691de52e7d68fc727733725fa759d33bcb3395a53` |
| `third_party/wabt/include/wabt/c-writer.h` | `8af35e1e24e1726cb4e2c001f00a2e4afac4c23a3db4778d7ea61a5d942ea245` |
| `third_party/wabt/src/interp/interp.cc` | `f97dcef973b94876e33dc5d02af440ff6fe08a0697786b73ffc39c5a845c0c80` |
| `third_party/wabt/include/wabt/interp/interp.h` | `7baab6344cff4f2e6bf07f21f31d49f60ed1ab5ab35999d2d74628da1573599c` |
| `ports/wabt/include/wasm-rt-host.h` | `f43de6d2a860d4f37a3fe012698537a9c90ccb1acfb910129ab57964a1b35074` |
