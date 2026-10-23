# Nocturneネイティブ境界と実サイトJavaScript — 続報

## 方針と判定

GUI優先、RAM上の実行環境、`/data`の永続保存と`/data/bin`のPATHを維持する。
ブラウザーのためにUnix/POSIX化せず、必要なNocturne自身の改善を行う。
今回は既存のネイティブheap拡張処理も修正する。`fork()`や互換層、新しいプロセスAPIは追加しない。
Hyper-Vはユーザーの検証対象であり、こちらでは既存VM・媒体・dataを変更しない。

**指定4サイトの全面動作は、まだ達成していない。** 以下は実Nocturne/QEMUの前後差と、
別枠の補助回帰検査。ローカルHTMLやホストのJavaScript実行を、実サイト合格の代わりにはしない。
前の[追補](browser-js-progress-2026-10-06.md)はbuild13までの記録として保持する。

## 実DuckDuckGoから特定した共通不具合

通常版の実検索 `https://duckduckgo.com/?q=quickjs+javascript` に接続した。
build14/15では`queueMicrotask`とサイト側Promise実装が同期に再帰し、5秒の実行上限、
または128MiBのメモリ上限へ達した。build15のmainモジュール準備直後は約38.6MBで、
ソース準備だけで128MiBを必要としていたわけではない。

旧ブラウザーの`queueMicrotask`は公開`Promise.resolve().then(...)`に依存していた。
サイトのPromise実装は逆に公開`queueMicrotask`を呼ぶため、循環していた。
実スタックと同じ公開vendor bundleの対応箇所を別担当でも照合した。
サイト名による分岐は追加せず、QuickJSの`JS_EnqueueJob`へ直接接続した。
例外をPromise rejectionへ変換せず、通常の例外を報告した後も残りのmicrotaskを配送する。
既存の有限heap・task予算は維持する。

build16の実検索2回ではこの再帰とquota到達は再現せず、Reactの描画処理へ進んだ。
そこで`DOMParser`未実装の例外と別の実行時間超過を確認した。
**検索結果表示の合格ではない。** `js-live-16`の画面とログには到達点と残る失敗を保存した。

根拠:

- `build/nocturne-audit/js-live-14/qa-serial.log`
- `build/nocturne-audit/js-live-15/qa-serial.log`
- `build/nocturne-audit/webfetch-native/live14-probe2/qa-serial.log`
- `build/nocturne-audit/ddg-independent/round2-report.md`
- `build/nocturne-audit/js-live-16/qa-serial.log`

microtaskの直接キューイングは[HTMLのmicrotask規定](https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#microtask-queuing)に沿う。
複数realmやWindowのerrorイベントを含む全イベントループ適合を意味しない。

## Nocturneのメモリ不足時の後始末

既存のheap拡張が途中で失敗すると、確保済みの物理ページを返さず、heap終端だけは旧値のままだった。
隔離QEMUで、失敗した1回の拡張により空き375357440バイトが0へ減ることを再現した。
正常なENOMEM復帰の後も他アプリや通信ワーカーのメモリを奪うOS側の不具合である。

修正は、その試行で実際に確保したページだけを解放してから失敗を返す。
旧heapの端数ページと内容は保持する。初回案の「要求範囲すべてを走査して解放」は親レビューで退け、
確保に成功した範囲だけを走査する。大きな要求の未確保末尾まで無駄に走査しない。
既存`mem`試験群へ`tests/sbrktest.c`を追加した。

過去のwebfetch faultの命令位置は、約70KiBのstack frameを作った直後の呼出しだった。
ただし、その実行の物理空きページ数は未採取で、今回のheap欠陥と同一原因とは断定していない。
担当の再接続試験では同じwebfetch faultを再現できなかった。この点は未解決として残す。

## 描画・共通処理

- `location`の同じURLを属性読取りごとに再解析する無駄を削減する。ネイティブURLを毎回照合し、
  非公開の文字列値だけを再利用する。公開URLオブジェクトや変更可能なsearchParamsは共有しない。
  ホストの補助計測はQEMUの性能保証とは分けて記録する。
- JavaScript有効時の`noscript`をUAスタイルで非表示にする。watchdog停止を「文書作成時からJS無効」と
  誤解して、raw textとして解析済みのHTML断片を画面へ出してはいけない。
  あわせてUAのimportant宣言が作者側importantより優先される順序を直す。
  根拠は[HTMLのhidden elements](https://html.spec.whatwg.org/multipage/rendering.html#hidden-elements)と
  [CSSのcascade順序](https://www.w3.org/TR/css-cascade-5/#cascade-sort)。
- 未処理PromiseのネイティブErrorについてstackも記録し、停止位置を失わないようにした。
  任意のthrowオブジェクトの文字列化・stack getterを呼ぶ既存の共通例外報告経路には、別の改善余地が残る。

## 最終統合検証

今回の最終媒体はbuild17。以後の変更は試験のviewport初期化と記録のみで、製品バイナリーは同じ。
MSYS2の`make -j8 all`が終了コード0で完了した。ホストの`build.ps1`外側やHyper-V起動を実行したとの主張ではない。

### 実サイト（隔離Nocturne/QEMU、512MiB）

実際のネットワークからページとスクリプトを読み、ブラウザーGUIのアドレス欄・ポインターを操作した。
専用scratch dataとboot snapshotを使用し、ユーザーの`data.img`やHyper-V用媒体を使用していない。

| 対象 | 今回の直接観測 | 判定 |
| --- | --- | --- |
| 通常DuckDuckGoの`?q=quickjs+javascript` | Promise相互再帰ではなくReactの描画へ進む。`noscript`のHTML断片は非表示になった。`DOMParser`未定義と別の5秒停止が残り、検索結果は表示されない | 不合格 |
| DeepMind | 初期画面とポインター操作によるメニュー反応を確認。メニューと背面見出しの重なり等は正しくない。cookie barの`getItem`とサイトのメディア処理で例外が残る | 部分動作、全体は不合格 |
| YouTube | 10845816バイトのmainスクリプトを10869msでコンパイルし実行へ進む。`HTMLTemplateElement`未定義で停止。`performance.timing`系と他のブラウザーAPIも不足。画面は未完成のまま | 不合格、再生未達 |
| ChatGPT | 確認ページが「Browser not supported」と表示。本体アプリへ未到達 | 不合格 |

根拠は`build/nocturne-audit/js-live-17/qa-serial.log`と同ディレクトリーの
`ddg-results.png`、`deepmind-initial.png`、`deepmind-models-later.png`、
`youtube-loaded.png`、`chatgpt-loaded.png`。
DeepMindのイベント反応は観測できるが、メニュー内容・位置まで正常との判定ではない。
ChatGPTの制限回避やブラウザー偽装は行っていない。
この遷移系列では過去のwebfetch faultは再現しなかった。解決確認ではない。

### 補助回帰（実サイト判定とは別）

`build/nocturne-audit/js-contracts-17b/qa-serial.log`:

| 試験 | 結果 |
| --- | --- |
| font / Shift_JIS / web | 83 / 26 / 99検査、失敗0 |
| JavaScript | 791検査、失敗0 |
| media policy / media events / hover | 106 / 12 / 447検査、失敗0 |
| image | 失敗0、5要求と5完了 |
| QuickJSのOOM・例外backtrace | 207検査、失敗0 |

初回`js-contracts-17`は新しい`noscript`試験で2失敗した。単独のinline-scriptページだけ、
最初のJS実行前にviewportを与えておらず、computed styleを計算できていなかった。
実ブラウザーの文書切替は`relayout()`後にJSを実行する。
試験も同じ順序に修正し、単独9検査・失敗0と上記全体再実行を確認した。
失敗記録は削除せず保持している。期待値や製品の非表示処理を緩めたのではない。

`python scripts/test.py --quick --no-net --timeout 300 mem gui fs`は6件成功・失敗0・skip0、
ホスト側FAT32検査も成功した（46秒、ホスト全体55秒）。
`sbrktest`は17検査・失敗0。1TiBの有効範囲要求を失敗させても、未確保末尾の走査をせず601msで戻り、
繰返し失敗でも空き物理ページを保持した。W^X試験の意図的faultは成功判定の一部で、ブラウザーfaultとは別。
保存ログは`build/nocturne-audit/os-tests-17-serial.log`。

### 媒体の確認

- `build/nocturne.img`内のkernel、initrd、UEFI loaderを現在の出力と照合した。
- initrd内のbrowserとwebfetchも現在の出力と一致した。
- `qemu-img compare`で`build/nocturne.vhdx`とraw媒体の仮想ディスク内容が一致した（終了コード0）。
- 生成済みJS埋込文字列が最新sourceと一致することを確認した。
- 内容hashと検査結果は`build/nocturne-audit/final17-identity.json`、ビルドログは`build17.log`。

**VHDXを生成・照合したことと、Hyper-V Gen2で実起動したことは別。** 後者はユーザー担当のまま。
既存VMへのコピー・停止・更新は行っていない。

## 残る作業

次の共通境界は、独立したDocumentの所有権・寿命を伴う`DOMParser`とtemplate DOM、
ページ保存API、メディアAPI、CSS描画である。空のstubを足して例外だけ消した状態を完成とは扱わない。
DDGの残る5秒超過も、今回直したPromise再帰やLocationの反復解析と同じ原因と決めつけず、再計測が必要。
QuickJSの言語機能とブラウザーが提供するWeb API・描画機能は別であり、ESNextや全Web APIへの全面対応は主張しない。

正式J-space全tree gateは、固定の除外集合が稼働中Hyper-V媒体まで含むため未成立。
そのためにユーザー媒体へ触れたり、限定sourceのhashを全tree検証と呼んだりはしていない。
