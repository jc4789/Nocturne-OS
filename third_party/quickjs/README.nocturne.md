# Nocturne の局所変更

`quickjs.c` の ArrayBuffer backing allocation 前に、要求バイト数を含む
既存 cycle GC の判定を行う。適応 GC 閾値が有限 runtime limit を超えた場合も
割り当て前に回収機会を与える。`SIZE_MAX` による自動 GC 無効化は維持する。

HLS 公式 demo の約13MB remux buffer が、使用量約123MB（117.5MiB）/ 上限128MiBで
GC 機会なしに失敗した実例から追加した。生存 buffer の破棄、上限拡大、
allocator 内の再入 GC、同期での FinalizationRegistry callback 実行は行わない。
live データが上限を超える場合は、従来通り実際のメモリー不足を返す。

## 読み取り専用アクセサーの診断

`call_setter` に既存のプロパティ atom を渡し、setter がない例外に名前を
含める。atom の所有権、Strict mode の判定、例外になる条件は変更しない。
ページの getter や文字列化処理を追加で実行しない。
実 YouTube の曖昧な例外が `style` 代入だと判明し、ブラウザー側で欠落していた
CSSOM `PutForwards=cssText` を実装するために利用した。

上流の著作権・ライセンスは変更しない。

## RegExp の旧式静的キャプチャー

実 Baidu の公開 ESL loader が `extReg.test(source)` の直後に
`RegExp.$1` を拡張子として使っており、欠落値 `undefined` がスクリプト等の URL に
後置されていたため、JavaScript monkeypatch ではなく native exec 成功境界で
`$1`〜`$9`、`input`、`lastMatch`、`lastParen`、`leftContext`、`rightContext`
と各別名を実装した。exec を迂回する既存高速 replace 経路も同じ更新を行う。

保持するのは realm 単位の subject 参照と UTF-16 offset 表だけで、成功のたびに
全文 context や capture string を複製しない。getter 呼出時に必要な部分文字列
だけを生成する。失敗した一致では前状態を維持し、getter の割当失敗でも状態は
変更しない。未参加 capture は空文字、lastParen は9を超える最後のcaptureも
扱う。`input`/`$_` への代入は文字列化し、他のcaptureを変更しない。

一次資料は https://github.com/tc39/proposal-regexp-legacy-features の Stage 3
legacy draft（最終ECMA-262本文ではない）。このdraftのrealm/receiver境界に
合わせ、別realmのexecを借用しても相手のstatic状態を変更せず、RegExp subclass
による成功はそのrealmのlegacy状態を無効化する。getterは無効なreceiver/状態で
TypeErrorを返す。input以外はsetterなし、全accessorは非enumerable/configurable。
既存 `compile` の挙動全体は今回の修理対象に含めない。

RegExpから作成realmへの所有参照には既存の `JS_DupContext`/`JS_FreeContext`
方式とGC markを使い、context解放時に保持subjectを解放する。補助回帰は
`tests/js_regexp_legacy_cases.js`。実Baiduの表示・操作受入れとは区別する。
