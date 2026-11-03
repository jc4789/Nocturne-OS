# WebAssembly メモリ転送保護：独立レビュー

## 対象と方法

ネイティブ Wasm 担当が、主担当の製品差分を独立に静的確認した。対象は QuickJS の `transfer_locked`／Set・Is API、`js_clone.js`、`frame_native.h`、`js_worker_transfer.h`、`js.c`／`browserjsworker.c` の detach guard と task 終了時 collect。製品ソースは変更していない。

## 確認結果

新たな重大欠陥の具体的反例は見つからなかった。以下をソースで確認した。

- QuickJS は buffer 構築時に lock を false へ初期化する。Wasm bridge が生成直後に true 相当へ設定し、作者から可視な property／prototype に依存しない。
- transfer／transferToFixedLength は同長の raw pointer 移動、長さ変更による copy／realloc、ゼロ長化のどの分岐より前にも lock を拒否する。長さ引数の作者による変換後に detached 状態も再確認する。
- `JS_IsArrayBufferTransferable` は ordinary ArrayBuffer の内部ブランド、shared／detached／lock を確認する。clone の早期 preflight と browser／Worker の native detach の双方が利用する。
- frame／Worker の commit は全 buffer を検証し保持してから detach へ進む。private plan の書込先は既存の書込可能 own-data slot に限定し、旧値も commit 後まで保持する。trusted plan の配列 index は捕捉した defineProperty で作られ、作者の Array.prototype index setter を呼ばない。
- したがって、通常 buffer の後ろに Wasm buffer が含まれるリストでも、通常 buffer を先に detach してから失敗する経路にはならない。native guard は JS preflight を迂回された場合の追加防御にもなる。
- `JS_DetachArrayBuffer` 自体は host の正当な Wasm grow のため利用可能なまま。公開 transfer の拒否と、grow 成功時の既存 buffer detach を混同していない。
- lock されていない通常 buffer の既存 transfer 分岐は維持され、転送なしの structuredClone は独立した通常 ArrayBuffer を作る。clone された byte 列が native linear memory の別所有者になることはない。
- browser の最外 task 終了と Worker の jobs 終了に collect が置かれ、bridge 側も active 実行中の外部 collect を拒否する。Handle は JS の辺を即時解放し native 回収のみ pending にするため、各 wrapper ごとの全 Store 走査へ戻っていない。

## 実行証拠と限界

既に担当自身で読んだ `build/wasm-repaired-native-20261014/test-serial.log` では基本 168、境界 41、実 HTTP Worker 29 と関連 11 群が失敗 0 で完了した。境界群には Wasm buffer の prototype 転送禁止、structuredClone／混在 transfer list の失敗原子性、通常 buffer の transfer／fixed transfer／structuredClone 転送、転送なし clone の独立性が含まれる。

これは全ての部分 commit 状況、意図的な OOM、全 GC finalizer 順序を実行した証明ではない。private plan を任意の作者オブジェクトへ公開した場合の安全性や既存の転送量制限全般も、今回の追加 guard の受入へ混ぜていない。新しい重大な UAF／通常転送回帰／この guard による partial commit は静的確認の範囲では認めなかった。
