# ブラウザー Wasm ネイティブ実行・JSPI 第二考察

担当は `wasm_native`。第一報の件数を再列挙するのではなく、成立しそうで成立しなかった前提と、その反例への修理を記録する。製品 source/hash の対象は `docs/browser-wasm-native-jspi-first-20261014.md` と同一。ここで新たな製品変更・同条件の再試験は行っていない。

| 弱い前提 | 具体的反例／結論 | 製品で閉じた境界 |
|---|---|---|
| 生 JS object を保持すれば externref を保持したことになる | global→Wasm local、JS import が global を null 化して強制GC、local を返す。Foreign wrapper の finalizer は object が生きていても binding->value を消せる。 | native result の canonical anchor 自体を Active.roots に保持してから旧 snapshot を捨てる。実GC補助の returned-ref と code lifetime を確認。 |
| Store が interpreter 参照を mark すれば JSPI fallback も安全 | 上記 local を `Suspending` import で停止し、main stack でGCする。Store ref だけでは Foreign の JS edge は辿れない。 | Thread::VisitRoots、owner cookie、Task の保存 anchors を追加。import-target tail が実未対応である binary を用い、fallback増加/native_calls不変を別途確認。 |
| coroutine が native register を保存すれば JS roots も保存される | 停止中に Module/Instance 変数と local の最後の JS 所有者を失う。main の Active/native head を mark しても停止 stack は守れない。 | Task が QuickJS context/exception と保存 Active chain、参照 anchors を mark。nativeの停止コード寿命と fallbackのlocal寿命を別経路で実検査した。 |
| paused Task だけ取消せば退役後の作者実行を止められる | running import A の内部で retire→A が return→通常 import B。別例は cancel 後に戻った thenable の getter。 | 全 import入口/直後、await入口/PromiseResolve直後、native/interpreter poll と変換の後続境界でcancel判定。実補助の B/getter 実行0を確認。 |
| done=true なら外部cancelが即cleanupしてよい | promising が externref を返し、Promise resolver が読む then getter 内で retire。resume がまだ ctx を使うのに外部cleanupが解放する。 | settlement 中もrunningを維持し、外部cleanupはdoneかつ非runningに限定。再入反例を実ゲストで確認。 |
| 正常値だけを返す Suspender なら普通exportで呼べる | 古い Overview の説明と現行規範案の active-context/Await 手順に差がある。 | 現行formalに合わせcallback前拒否。同期値・thenableもAwaitする。74検査はこの意味を検査し、Nodeの引数coercion同期throwとの差は期待を弱めず区別した。 |
| nativeに下げられればtail callも同じ意味になる | indirect-tail、importを跨いで再入するtailがC stackを累積し得る。 | 未実装のtrampolineに具体的理由を付けinterpreter fallback。単なる性能差として誤ったnative意味へ置換しない。 |
| Module JS wrapperの寿命だけコードを保てばよい | Module/Instance変数を捨てた後、export/Table経由で関数を呼ぶ。停止JSPIでも同じ境界がある。 | StoreのFunc→Instance graphと実NativeInstance/NativeModuleの所有を結合。native counter必須の実GC検査で確認。 |

## 反証後も残す制限

- TCCのC-only guardは実測したOOM地点と取消を回復したが、すべてのC++ STL確保失敗、任意の未instrumented host関数のstack exhaustionまで回復した証明ではない。coroutineは実stack量を確保し、実資源不足を返す。個数や深さを小さい固定値で隠していない。
- `JS_HasActiveJSFrames` は作者frame越しsuspend禁止を担い、QuickJS state交換は例外所有権も含む。違反をPromiseの名前だけで吸収する実装ではない。一方、全面的なEH/JS Tag/Exception API適合は未証明である。
- shared memoryは、reallocの単独所有をSABへ包むだけではpointer安定性・同時grow・Worker所有権を満たさない。Module cloneもTCC/raw pointerの送信ではなくtarget realmの本物Moduleと輸送寿命が必要で、現在は未接続である。
- 第一報のPuterは loader のnative実行を証明するがJSPI使用は0。Launchが有効になり実クリックできたことをFirefox起動成功へ昇格させない。次の実失敗はAtomicsの欠落であり、共有実装を名前だけ公開して隠さない。
- native/fallback counter、実API補助、実サイト表示・入力、性能、Hyper-Vは別の証拠層である。15群成功とGC成功から四サイト完成・性能無回帰は導かない。主担当の実サイト受入は継続中。

最初の実行へ進む前に通知された2件のcancel反例はweb担当が独立に再読し、その後主担当の最新ゲストが同じ境界を実行した。静的再確認だけでPASSを宣言せず、具体的反例→製品修理→実ゲストという順に根拠を区別した。
