# WebAssembly ネイティブ境界：修理後の第二考察

## 初案の所有権を反証した確認

全 import callback を global State の mark へ集めると、callback が Instance を捕捉した循環を global namespace が保持し続ける。この初案を撤回し、native Store の参照に対応した GC 可視の anchor graph へ修理した。Instance は import／resource、関数は Instance、Table は各要素、参照 Global は現在値だけを保持し、変更通知は変更範囲だけを更新する。

State と Handle／constructor は C++ の共有所有権で寿命を分離する一方、JS の mark で報告した物理辺は JS finalizer 内で即時解放する。特に Binding の JS 値と State の error prototype／DataView getter は即座に UNDEFINED 化して解放し、白循環の破棄後に遅い Store／shared_ptr destructor が再解放しないようにした。

Handle finalizer は native 回収を pending にするだけで、各 wrapper から全 Store を走査しない。最外 Active の終了、安全な browser／Worker task 終了、State 破棄へまとめた。callback の JS 辺を遅延して寿命の保証を壊す方法ではない。ただし、重い循環を意図的に大量生成・強制 GC した回収量の測定は未実施である。

## 別の具体的反例と製品修理

QuickJS の通常 ArrayBuffer.transfer は、同じ長さの場合に custom free callback と raw pointer を新 buffer へ移す。Wasm memory buffer の owner property は一緒に移らないため、初版には解放済み linear memory を新 buffer から参照する危険があった。現行 WebAssembly JS 仕様の ArrayBufferDetachKey と実エンジンを照合して主担当へ報告し、主担当が QuickJS の内部 transferability と clone／transfer 経路を修理した。native memory_buffer は生成直後に `JS_SetArrayBufferTransferable(..., false)` を設定する。prototype を作者が差し替えられるだけの防止策ではない。

そのほかの実装修理は、DataView 内部 getter の捕捉、作者の Array.from に依存しない iterator 処理、長さ付き Atom による NUL 名、厳密な compile-options／重複 builtins、WABT の実 realloc 失敗、Global の missing と明示 undefined の区別、Table.set の missing 初期値、i64 address の符号・overflow、Memory::PageSize の実契約を含む。

## 証拠の読み直しと残る限界

修理後の `build/wasm-repaired-native-20261014/test-serial.log` では native 基本 168、境界 41、実 HTTP Worker 29 と関連 11 群が失敗 0 で完了した。これは単なる host の標準 WebAssembly の結果ではない。旧報告のソース指紋や旧コンパイル成功を、修理後の実行へ流用していない。

未確認または未対応の境界は残る。

- 共有メモリ／並列 Wasm threads、GC／typed function-reference 拡張、JS Tag／Exception オブジェクト、文字列組込みの全面対応。
- 異なる realm に作った独立 Store をまたぐ Module／外部オブジェクト共有。
- 巨大な単一 function body の解析中断、巨大 bulk 命令内の GUI 応答、実アプリ負荷と GC の定量比較。
- 初期化や Instance の寿命を落とした後の単純な参照保持は検査したが、大量の callback／Instance 循環を強制 GC した実回収量は測っていない。
- 実サイト作者が Wasm を実際に compile／instantiate／call したかは、軽量 native counter と主担当の browser console 証拠で別に確認する。画面や動画が正常になった証拠へ昇格しない。
