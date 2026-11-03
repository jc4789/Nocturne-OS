# WebAssembly ネイティブ境界：初回報告

## 範囲と結果

担当は `user/libc/web/js_wasm.cpp` と `js_wasm.h`。WABT 1.0.42 の検証器・インタープリターへ QuickJS を直接接続し、同期 Module、Instance、Memory、Table、Global、validate、Module の記述情報・customSections、CompileError／LinkError／RuntimeError を追加した。

関数の import／export、i64 と BigInt、複数戻り値、JS import 例外の同一性、実メモリを参照する ArrayBuffer、grow 成功時の detach を実装した。非同期 compile／instantiate と Fetch／streaming は別担当の既存埋込み層が同期 API を使用する。

## 確認した根拠

- WABT の `interp.h`、`interp-inl.h`、`interp.cc` と `binary-reader-interp.h` の実 API、所有権、Store の mark／collect、Thread の実行経路を読んだ。
- QuickJS の class finalizer／gc_mark、ArrayBuffer detach、C function data、コンストラクター呼出し、DataView／TypedArray の内部ブランド処理を読んだ。
- `scripts/build_cxx.py compile user/libc/web/js_wasm.cpp -o build/user/js_wasm.o -I ports/wabt/include -I third_party/wabt/include -I third_party/quickjs` は終了コード 0。最初のコンパイルで page-size macro に namespace を付けた 2 箇所がエラーとなり修理した。
- 主担当が製品の正規ビルドと QEMU 実行を担当する。この初回報告の時点では、担当自身によるゲスト API 実行、実サイト表示・操作、性能比較は未確認。コンパイル成功を実行成功とはしていない。

## 再考の対象

初案の「共有 Store の gc_mark から全 import JS 値を保持」は、JS callback が Instance を捕捉する循環を global namespace から保持し続ける危険がある。さらに State の finalizer 順序、DataView の BufferSource、作者による Array.from 上書き、埋込み NUL を持つ import／export 名、実確保失敗を確認対象とした。

## 既知の対応境界

WABT の実命令対応と JS への公開範囲は同一ではない。共有メモリとスレッド、GC／typed function-reference 拡張、JS Tag／Exception オブジェクト、文字列組込みを全面対応したとはしていない。異なる realm の独立 Store 間で Module／外部オブジェクトを共有する経路も未対応である。TCC による wasm2c のゲスト内コンパイルはこの実装では使用していない。
