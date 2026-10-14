# WebAssembly ネイティブ境界：修理後の初回報告

## 実装と接続

`user/libc/web/js_wasm.cpp`／`js_wasm.h` が WABT の検証・命令実行を QuickJS の同期 WebAssembly API へ接続する。Module／Instance／Memory／Table／Global、import／export、i64 BigInt、複数戻り値、trap、customSections を実装した。Promise／streaming は `js_wasm.js`、メイン browser／Worker の初期化・中断・安全な task 終了時の回収は各既存所有層へ接続されている。

## 実際の失敗と修理

最初の実 Nocturne 実行では、基本 API と本物の HTTP Worker の Memory.grow 戻り値が失敗した。WABT の `Memory::PageSize()` がページの byte 数でなく現在の `pages_` を返す契約を誤読し、旧ページ数を誤計算していたためである。実装元 `interp-inl.h` と命令実行側 `interp.cc` に照合し、`old = mem->PageSize()` へ修理した。失敗は `build/wasm-native-20261014/test-serial.log` に残る。

同じ初回の boundary 群で完了行が見えなかった点は、製品 Promise 未完の証拠ではなかった。`jstest.c` のログ転送 filter が完了文言を捨てていた。主担当が filter を修理し、期待値の緩和や失敗除外ではなく実完了出力を確認した。

## 修理後に読んだゲスト結果

`build/wasm-repaired-native-20261014/test-serial.log` の実 Nocturne／TCC 経路の結果を担当自身で読んだ。

- WebAssembly 基本・Promise／streaming：168 項目、`jstest: 7 checks, 0 failed`。
- 追加の native 境界：41 項目、`jstest: 7 checks, 0 failed`。
- 本物の HTTP Dedicated Worker：29 項目、`jstest: 8 checks, 0 failed`。
- 同時に実行した関連群は Fetch 244、XHR 89、DOM／イベント／observer、Worker、storage／policy 等を含み、総括は `TESTS DONE pass=11 fail=0 skip=0 (33 s)`。

補助試験は、実メモリの同一参照、JS と Wasm 双方の grow／detach、import と export の同一性、trap、JS import 例外、再入、i64、SIMD、bulk memory、DataView、埋込み NUL 名、iterator protocol、転送禁止境界を含む。実際の主担当の画面・操作確認を担当のログ確認へ置き換えない。3 実サイトの表示・入力・スクロール・console と性能の受入は主担当の別証拠である。

## 到達範囲

WABT C++ のクロスコンパイルと実 native 実行が成立した。API 名だけの固定戻り値やサイト専用分岐は追加していない。wasm2c を TCC で動的コンパイルする経路は採用していない。Core 3.0／JS API／Web API の全面適合、実アプリの速度改善、全 GC 循環の回収量、Hyper-V 実機成功はこの結果から主張しない。
