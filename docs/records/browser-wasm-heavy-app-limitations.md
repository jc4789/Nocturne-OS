# 重い Wasm アプリに残るネイティブ境界

Puter 上の Firefox Wasm に関する主担当の実画面・実クリック観察を、一般的な API 試験の成功と区別する。今回これらを名前だけ追加したり、機能検出を強制的に通したりしない。

- **JSPI は実装・実ゲスト検証済み。** 同一 thread の独自 stackful coroutine が実 Wasm frame と値・参照を保持し、intrinsic Promise reaction から再開する。現行 formal の active-context 検査、PromiseResolve／Await、拒否 identity、再入、強制 GC、文書退役取消を検査した。`wasm-jspi-first-20261014` の JSPI 74 検査と Puter VM 内の native／fallback 強制 GC は成功。ただし実 Puter ページの JSPI 使用回数は 0 であり、fixture 成功を Firefox 本体起動へ昇格させない。

- **共有 linear memory／Wasm threads は未対応。** native descriptor は shared を明示的に拒否し、WABT の threads feature も false。WABT 自身の interpreter memory API は atomic 処理を fake atomics としており、feature を true にするだけでは実並列の順序・wait／notify・共有寿命を保証しない。現在の Worker は別プロセスの runtime／Store であり、native memory の共通 backing とその所有権を接続していない。512MiB という容量を理由にした製品上限ではなく、**共有**の意味論が未実装である。

- **WebAssembly.Module の structured clone／Worker 受渡しは未対応。** `js_clone.js` は Module の native class を clone 対象として登録しておらず、unknown-native-class として拒否する。Worker transport の一般 JS グラフ直列化に C++ Store や raw pointer を渡すことはできない。Module の bytes／compile options／agent-cluster と宛先 realm での再構築・cache・寿命を扱う専用経路が必要である。既存の「Worker 自身が bytes から compile して呼び出す」成功を、Module clone 成功としない。

最新 Puter 実画面では JSPI 警告が消え Launch が有効になり、主担当が実 target を確認してネイティブクリックした。loader の 54394 bytes の Wasm は初期 native 5 呼出し、Launch 後に同 loader を再構築し累積 2 modules／108788 bytes／native 10 呼出し／fallback 0／JSPI 中断 0。その後 `TypeError: cannot read property 'waitAsync' of undefined` で Retry となり、提供参照画像の Firefox UI は未達。Retry のクリックはしていない。共有メモリ・Atomics・agent cluster・COOP／COEP を一般の所有権と通信へ接続する仕事が残る。空の Atomics、固定 crossOriginIsolated、強制 Launch 有効化は追加していない。

根拠は `user/libc/web/js_wasm.cpp`、`ports/wabt`、`third_party/wabt/include/wabt/interp/interp.h`、`third_party/wabt/src/interp/interp.cc`、`third_party/wabt/include/wabt/feature.def`、`user/libc/web/js_clone.js`、`user/apps/browserjsworker.c` と `build/nocturne-platform/wasm-jspi-puter-first-20261014/serial.log`。Fetch の `data:` 対応は共有実行機構とは別の共通通信境界であり、その修理だけで Firefox Wasm が起動したとは報告しない。
