# shared Memory / Module clone の接続候補（未実装）

JSPI の実経路確認を先行し、この境界の製品編集・追加試験は行わない。

## 現在の実ソース

- `Makefile:139-140` は QuickJS を `CONFIG_NOCTURNE` で構築する。`quickjs.c:76-77` は Atomics を除外し、`61893-61903` は SharedArrayBuffer constructor を除外する。Worker runtime も冒頭で両globalを削除する。上流本体の存在を製品対応と数えない。
- Nocturne の `n_shared_create/map` は実共有ページの既存責務。ただし descriptor import は直接の親子だけであり、任意processのpointer/fdを渡すAPIではない。
- Store Memory は `FallibleBuffer<u8>` と各objectの page count を所有し、現在の grow は resize と古い ArrayBuffer の detach に接続する。shared=true の解禁だけでは実共有にならない。
- `js_clone.js` は既存seen mapとprepare/deserializeを持ち、未知のnative classを拒否する。Worker C輸送は `JS_WRITE_OBJ_REFERENCE` / `JS_READ_OBJ_REFERENCE` のprivate graphを送る。QuickJSのSAB用バイナリ形式はraw pointerを保存するため、別processへそのflagを追加するだけでは安全でない。

## 共通責務へ統合する契約案

1. **Memory正本:** Store Memoryへ共有backingの所有・data/length/max/grow契約を追加し、SAB wrapperとnative wasm2cのmemory viewは同じbackingを見る。各Storeで独立reallocや page count の正本を増やさない。共有growは旧SABをdetachせず、その旧viewの長さと新bufferの長さを分ける。現在の固定共有object APIで拡張可能かは実APIの追加設計が必要。
2. **capability:** 既存Worker brokerが共有backing identity、親子map capability、参照leaseを管理する。子から別子への再配送も元processのraw pointerではなくbroker所有capabilityで行う。publication失敗・Worker終了・文書破棄でleaseを解放する。SABは共有cloneであり、transfer listのdetach対象ではない。
3. **Module clone:** `js_clone.js` の既存walkへnative brandのserializer/restore責務を接続する。元Moduleが保持するbinary/validated module metadataを使い、target realmの本物Moduleを作る。JS関数やTCC実行pointerをwireへ出さない。同じgraph内の重複参照はseen mapで保ち、cloneしたModule自体は元JS objectと別identityにする。既存canonical `wrapper()` をそのまま呼ぶと元objectを返すため不適切。Instanceはclone可能へ便乗させない。内部再構成時のCSPは公開new Moduleの迂回ではなくWeb APIのserialization契約として別途確定する。
4. **公開条件とAtomics:** COOP/COEP、agent cluster、same-cluster clone条件を実際のdocument/Worker policyへ接続してから公開する。`crossOriginIsolated=true` の固定値やglobal削除の撤去だけは不可。既存QuickJS Atomics本体をNocturneのwait/wakeへ統合し、Windowのblocking waitは禁止、Worker終了時の待機取消と実メモリ順序を保つ。pthread/POSIX依存をそのまま有効化しない。

Puter loader は shared Memory と Module/Memory のWorker配送を使うが、このメモはそれらの起動成功を示さない。次の実失敗として観測した後に、必要なまとまりで修理する。
