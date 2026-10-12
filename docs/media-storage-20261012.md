# 媒体保存・Navigator担当記録（2026-10-12）

## 実装と統合契約

- `user/libc/web/js_navigator.js`：仕様上の `vendor:''` などNavigator値を追加。Shakaの `vendor.includes` 例外は主担当の変更前実QEMUログで観測済み。`mediaCapabilities.decodingInfo` は既存ネイティブ `avmedia('type')` の実codec判定を利用し、未測定の滑らかさ／電力効率や未対応HDRを成功として広告しない。暗号化能力は存在するEMEへの照会で判定する。
- `user/libc/web/js_indexeddb.js`：専用責務のIndexedDB。schema/object store/index、数値／Date／文字列／binary／配列キーと範囲、key generator、構造化複製、request、readonly/readwrite/versionchange transaction、abort、cursor、接続終了／blockedを実装。既存 `cloneData`・private `host.pack/unpack` を使い、JSON／localStorageへの置換はしない。
- `user/libc/webidb.c` と `user/include/webidb.h`：`/data/browser/indexeddb`へorigin/database名を長さ付きSHA-256で分離。変更した値だけimmutable payloadへ保存し、manifestにはschema／key／index key／参照を置く。全動画segmentの読戻し・再pack・再writeはしない。通常のprimary-key取得は二分探索し、指定値だけ復号する。
- native leaseはトランザクション中保持、`F_NTRYLOCK` busyならJavaScript側で次taskへ延期。主担当がこのNocturne固有APIを既存vnode leaseへ追加した。POSIX互換能力は追加しない。
- SHA-256付き二重manifest slotで部分書込み時は旧世代を保持。payload回収は新／前の両世代から未参照の物だけ。stage journalはprocessがcommit前に消えた場合の次begin回収に使う。
- `/tmp/browser-indexeddb`の接続registryはnative process PIDと開始時刻で寿命を照合。upgrade/delete通知は別browser processにも配送し、旧接続が閉じるまでblocked。closeは進行中transactionの終了を待つ。retire/freeでlease・接続を解放する。
- 主担当が `js.c` のprivate host、origin/policy gate、state寿命、bootstrap include、イベントhandler target hook、listener例外flagを統合する。`js_indexeddb.js`は生成元であり、生成済includeのみを直接修正していない。

### native private host形式

`begin(name)` → `[status,token,ArrayBuffer|null]`、`stage(token,packedValue)` → `[status,hash]`、`value(token,hash)` → `[status,ArrayBuffer]`、`commit(token,packedMetadata,hashArray,version)` → status、`abort(token)` → status。

`connections(token,desiredVersion)` → `[status,count]`、`connect(token,version)` → `[status,connection]`、`poll(connection)` → `[status,pendingVersion|null]`、`disconnect(connection)` → status。desiredVersion 0はdelete。statusはOK 0／BUSY 1／SECURITY 2／IO 3／MEMORY 4。

## 確認範囲と失敗

- 実JS module＋native契約adapter：最初17条件成功。optional null query／必須undefined key／reverse unique cursor／primary-key探索を修理した後19条件成功。adapterのため実native filesystemや実bootstrap handlerの受入ではない。
- 実 `webidb.c` ＋BearSSL＋隔離ホストファイル／lease／process adapter：20条件成功。origin隔離、NULを含むdatabase名、同DBlease busy、1MiB payloadのlazy取得、未変更値の無再write、部分manifest書込みからの復旧、接続通知once、closeによる解除を確認。
- native検査の初compileはホストの `SYS_OPEN` macro衝突とfixtureのCRT include不足で失敗。fixtureを修理して初runtimeで20条件成功。製品期待値は弱めていない。
- native freestanding syntax／警告確認成功。実QEMU・実Shaka offline保存／再起動後再生は主担当統合確認の対象で、これらをfocused確認から推定しない。

## 字幕の既存境界と限界

- 現行 `js_texttracks.js` のactive cue → `captionText` → `avmedia.c` の `paint_caption` まで製品描画へ接続済み。提供Nocturne画像にも字幕は表示されている。未実装と誤認して別字幕overlayを追加しない。
- plain text字幕の時計同期は媒体タイミング担当へ引き渡した。現在のWebVTT markup／内部timestamp／region／非default設定の制限は残る。
- IndexedDBのWorker realmへの公開、全WPT互換は未達。metadata自体は二重slot snapshot方式で、非常に多数のキーに対するmanifestの増分journal/B-tree化は未実装。payload全量O(n²)I/Oは避けたが、metadataの規模無制限性能を証明していない。
- 外部IndexedDB実装は取り込んでいない。今回のライセンス訂正で別機能を削除していない。

## 仕様参照

- [Indexed Database API 3.0](https://www.w3.org/TR/IndexedDB/)
- [Media Capabilities](https://www.w3.org/TR/media-capabilities/)

## 追加割当の製品実装（同日）

- `indexedDB.databases()` をnative origin gate経由の実一覧へ追加。manifest v3に長さ付きorigin/name/version/generationと独立catalog checksumを内包。full manifest checksumも4KiB単位で検証し、`.v`動画payloadを開かず、部分書込み・破損の新slotを旧commit世代で補完する。deleteのversion 0 tombstoneは旧backupを一覧へ戻さない。名前はNULを含め長さで扱い、private hashファイル名との一致も検証。
- v2 manifestはopen/read互換を維持して次commitでv3へ移行する。v2には逆引き名情報がないため、移行前のv2をcatalogへ虚偽の名前で登録しない。このv2は今回未配布実装の途中形式であり、以前の出荷版IndexedDBが存在したという意味ではない。
- `webidb_forget_connection` を追加し、JS結果構築OOMで未公開になったnative接続を、短いRAM-root lease取得後に退役。普通のauthor closeは非blockingを維持し、freeも共通APIを使う。
- `js_texttracks.js` にWebVTT cue tokenizer/tree、`b/i/u/c/v/lang/ruby/rt`、定義済色クラス、内部timestampノード、`getCueAsHTML`の安全なDOM構築を追加。raw HTMLをdocumentへ注入しない。cue position/size/align/line/snap-to-lines/vertical、`VTTRegion`とREGION block／anchor／幅／行数／scroll設定を追加。
- `avmedia.c` の既存 `paint_caption` へstyled glyph、ruby annotation、cue位置・折返し・縦書き・region clipping/積上げを統合。PiPも同じ描画を使う。`captionCues` は全変換成功後だけpublishし、差分がある場合だけrevisionを更新。既存 `captionText` は維持、unloadで全caption資源を解放。regionごとのflowは受信時にgroup化し、毎frameの全cue二乗走査をしない。字幕更新はpaintのみで全layoutをinvalidateしない。
- catalog実C重点確認は製品形式/API変更後27条件成功。追加項目は実一覧、payload非読戻し、origin/opaque隔離、partialwrite旧version保持、delete tombstone。最初の追加fixture compileはMinGW構造体名不足で失敗し、fixture型を訂正して初runtime成功。未変更の20条件だけを反復していない。
- 字幕の最初syntax確認はNocturneにない`llround`を使ったため失敗。既存`lround`へ修理し、allocation overflow式の警告も修理。最後のC／JS syntax確認は成功。実字幕の見え方は統合済み主担当VMでの受入が必要。

### 追加後も残る字幕境界

WebVTT STYLE stylesheet／作者`::cue` CSS、完全なbidi/shaping・衝突回避・複数ruby分節、region scroll-upの規定アニメーション、in-band字幕抽出、WebVTT全WPTは未達。内部timestampはcue treeとpast/future状態へ反映するが、作者CSSによるkaraoke装飾はSTYLE対応が必要。これらを今回のparser/native描画追加だけで全面対応とは報告しない。

- [WebVTT仕様](https://www.w3.org/TR/webvtt1/)
