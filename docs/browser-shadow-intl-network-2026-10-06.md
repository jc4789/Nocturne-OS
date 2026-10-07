# Shadow DOM・照合処理・TCP接続枠の拡張

これは [属性・Fetch等の一括拡張](browser-api-batch-2026-10-06.md) の続き。
QuickJSがJavaScript言語を実行することと、ブラウザーAPI・描画・通信が完成することは別である。
今回もNocturneのnative DOM、描画、イベントループ、TCP、spawnベースのworkerを使い、
fork・pthread・Unix互換層は追加していない。RAMfsと永続`/data`の扱いも変えていない。

## 実装したもの

### Shadow DOM

- native DOMにShadowRoot、host、open/closed境界、named/manual slot割当を保持する。
  JavaScriptだけの別DOMは作らない。通常のquery・containsは境界を越えない。
- `attachShadow`、`shadowRoot`、`getRootNode`、`assignedSlot`、`HTMLSlotElement`、
  `assignedNodes/Elements`、`assign`、ShadowRootのHTML挿入・activeElementを接続。
- hostを含むclone/adopt、接続状態、custom elementの接続・切断・adopt callback、
  shadow内の動的scriptとstylesheet・画像読込を扱う。
- stylesheetのscope、`:host`、`:host(...)`、`::slotted(...)`、継承、
  flat treeのlayout/paint/hit-testとfocusを実装。slotは標準で`display:contents`。
- イベントのcomposed経路、target/relatedTargetのretarget、closed境界での
  `composedPath()`非公開化、経路のdispatch前snapshot、hostのAT_TARGETを扱う。
  target capture内の停止も次のlistener呼出し段階に正しく反映する。
- MutationObserverとslotchangeを同じmicrotask通知処理へ統合。
  関係ない属性や文字列変更ではslot割当を全走査しない。

宣言的Shadow DOM、`::part`、`:host-context`、scoped custom element registry、
ElementInternals、adoptedStyleSheets/CSSOMは未完成・未実装であり、対応済みとはしない。

### Intl.Collator

- 固定版ICU 78.1 / Unicode 17 / CLDR 48から生成した照合データを同梱。
  guestがWindowsのICU DLLやホストのlocaleサービスを呼ぶことはない。
- **対応localeは英語と日本語**。その他は英語へfallbackする。
  Unicode全域の重み、正規化、contraction/context、数値照合、感度、caseFirst、
  punctuationの比較と`String.prototype.localeCompare`を実装した。
- 未実装のlocaleやcollationを`resolvedOptions`等で対応済みと偽らない。
- 初回利用時だけ照合表を展開する。最初のQEMU一括試験でJSだけの展開が
  5秒watchdogに到達したため、privateなC decoderへ移した。
  **watchdogは延長していない。** 入力上限・厳格なformat検証と
  QuickJSのメモリ課金・ArrayBufferの所有権・GC・割当失敗処理を維持する。

生成手順・license・対応範囲は [照合データの説明](../third_party/unicode_collation/README.md) を参照。

### 関連API

- `atob` / `btoa`：HTMLのbinary string・forgiving base64処理、例外、引数変換。
- `document.domain`：documentの由来に基づくgetterと安全な同一host設定。
  public suffixとorigin-domainの完全な実装なしにsuffix緩和を許さず、明示的に拒否する。
  same-origin policyを弱めてサイトの例外を隠す変更はしない。

### 実サイトで判明したCSS解析用メモリ

GitHubのCSS 13応答が全てHTTP 200で届いた一方、raw合計1,552,029 bytesの
解析後ASTが専用8 MiB枠を超え、authored sheetが全消去されていた。
診断値は`allocated=8341728, limit=8388608`。Shadowのscope判定による除外ではない。

- selector構造体の不要なpaddingを除去し32→24 bytesへ戻した。
- CSS専用AST予算を、文書とShadow全体で共有する**有限32 MiB**へ変更。
  raw CSS取得上限8 MiB、DOM予算32 MiB、JS予算128 MiBは維持する。
- CSS/DOMそれぞれのarena不足を、実使用量と破棄sheet数付きでconsoleへ通知する。
  debug起動時だけCSS応答のstatus・サイズ・URLも記録する。
- 上限超過で不完全なsheet参照を残さない処理と、次回の再構築による復帰を維持。
  取得成功と解析メモリ不足を区別できるようにした。

YouTubeでもraw 3,731,236 bytesで旧8 MiBのAST不足を観測した。
回帰には文書とShadow各50,000ルール、raw合計1,800,045 bytesで
共有ASTが16 MiB超～32 MiB以下となる検査を追加した。

### Fetchのキャッシュ指定

GitHubで実際に出た`Fetch cache controls are not supported`をきっかけに追加。
`Request.cache`の６モードを、**HTTP応答キャッシュを持たないホスト**の動作として扱う。

- `default` / `force-cache`はキャッシュ未命中として実ネットワークへ進む。
- `no-store` / `reload`は未指定時だけ`Pragma: no-cache`と`Cache-Control: no-cache`を生成。
  `no-cache`は未指定の`Cache-Control`を`max-age=0`にする。
- 条件付きrequestのdefault処理、既存headerの保持、redirect先での再生成を実装。
- `only-if-cached`は本当に未命中なのでTypeErrorで拒否し、HTTP/OPTIONSを送信しない。
  clone・継承・mode上書き・pre-abortの条件も検査する。
- policyはnative/wireの専用metadataとして渡し、CORSの著者header検査の**後**で生成する。
  自動生成headerを理由に余分なpreflightを発生させず、著者指定の権限は緩めない。
  wireの64-byte layoutと旧default値の互換は維持する。

HTTP cacheそのものやCacheStorageを実装したわけではない。
[Fetch公式の処理順序](https://fetch.spec.whatwg.org/#http-network-or-cache-fetch)に従い、
ネットワーク取得とキャッシュ専用失敗を区別する。

## `cannot load module` / `too many open files`

443は通常のHTTPSポートであり、それ自体が異常ではない。
今回ソースとQEMUで確認した一因は、Nocturneの**システム全体で16枠のTCP制御ブロック**。
これは通常のファイルdescriptor枠とは別である。
workerがcloseしてもFIN処理・TIME_WAITで枠がしばらく残り、旧実装は空きがないと
即座に`EMFILE`を返していた。TLS接続失敗を経てmodule読込失敗として表に出る。

- 満杯ならnative wait queueで空きを待ち、解放時にwakeする。
- 枠待ち・ARP・SYNで最初の接続期限を共有する。待つたびに期限を延ばさない。
- kill/cancelを維持。稼働中の接続を奪わず、FIN/TIME_WAIT寿命も短縮しない。
- 接続が終了しない場合は依然として正しいtimeoutになり得る。
  MIME・CORS・import map・未対応scheme等によるmodule失敗まで直ったとは主張しない。

## 検証環境と途中で検出した問題

QEMUを既定**2 GiB**に変更。UEFI、実Nocturne kernel/libc/QuickJS、privateなboot copyと
新規scratch diskを使う。`hyperv/`の稼働媒体と`build/data.img`には接続していない。
Hyper-V Generation 2での実動作確認はユーザー側の検証である。

初回一括試験で次を検出し、まとめて修正した。

- Collator初回表展開のwatchdog到達：上記native decoderへ変更。
- 2 GiBでのsbrktest誤判定：固定1 MiB許容では、失敗後にprocess終了まで保持する
  空page tableをdata page漏れと誤認した。VA/RAMからtableの上限を計算し、
  native process情報でdata page数の完全なrollbackを別に検査する。kernelを緩めていない。
- native slot試験の誤った期待：先にあるnested default slotがlight childを受け取る
  正常なtree-order割当を明示検査し、未割当fallbackの試験とは分離した。
- TCP kill試験：実際の待機状態、killの戻り値、終了コード130まで確認するよう強化。

ローカル回帰試験はAPIの根拠であり、実サイトの合格判定の代用ではない。
修正後の一括結果と、2026-10-07までの実サイト観測を以下へ記録する。

## 検証結果

最終production画像の一括試験 `shadow-intl-cache-final` は完走した。
新規の大型CSS fixtureだけは、request callback未登録のまま取得済み通知を直接入れたため、
58項目中3項目が失敗した。正式なrequest callbackからresource完了を返すfixtureへ修正し、
**productionコードを変更せず**関連4スイートを再実行した。
`shadow-intl-css-boundary-final` はShadow描画61・CSS supports 143・Web engine 99・
native Shadow 71項目すべて失敗ゼロ。大型CSSの実測値はraw 1,800,045 bytes、
共有AST 21,912,232 bytes、上限33,554,432 bytes、2要求・2sheet・エラー0だった。

以下はこの再検証を含めた結果で、全て失敗ゼロ。
`jstest`全体の最終QEMU時間は478,880 ms。標準runnerのスイート全体期限を
180秒から900秒へ変更した。ページ単位のJS実行制限5秒とは別の設定である。

| 検査 | 結果 |
| --- | --- |
| ブラウザーJavaScript全体 | 908 checks |
| 上記内のShadow DOM | 156項目 |
| 上記内のCollator | 7,287項目（固定ICU oracleとAPI検査） |
| 上記内のbase64/domain等 | 149項目 |
| 上記内のFetch | 203項目 |
| native Shadow所有権・slot | 71 checks |
| Shadow CSS・描画・focus・大型AST | 61 checks |
| private照合decoder・不正入力・割当失敗・GC | 77 checks |
| TCP枠圧力・キャンセル・回復・連続module | 36 checks |
| QuickJS割当失敗 | 207 checks |
| sbrk失敗時rollback | 21 checks |
| Lexbor | 895 checks |
| 属性native / CSS supports / SVG | 16 / 143 / 30 checks |
| font / Shift_JIS | 83 / 26 checks |
| Web engine / heap trim | 99 / 53 checks |
| message / import map / storage / cookie | 16 / 89 / 38 / 339 checks |
| media policy / media events / mouse境界 | 106 / 12 / 447 checks |
| image所有権 / metadata | 23 / 85 checks |

worker通信、画像要素、メモリ検査も失敗ゼロ。Fetch cache指定は実worker経路でも
生成header・CORS・redirect・キャッシュ専用時のHTTP/OPTIONS要求ゼロを確認した。
TCP枠が満杯の状態を実際に作り、
150msの接続期限、blocked waiterのkillと終了130、FIN後の回復、2workerで48moduleの
連続取得を確認した。別のfresh ARP probeは151msでtimeoutした。
共通wait queueによる他の通知の助力や、全種類の実サーバーのFIN動作まで網羅した試験ではない。

## 実サイトの観測

すべてQEMU内のNocturneブラウザーで公開URLを開いた結果であり、
ホストのChromeによる描画や、保存したHTMLだけの試験ではない。
ログイン・CAPTCHAの突破はしていない。初期表示だけを全面対応の根拠にしない。

| サイト | 観測と未達事項 |
| --- | --- |
| DuckDuckGo通常版 | 最終画像でも`?q=Nocturne+OS`の検索結果を表示。検索欄を`cats`へ置換しEnterで実際の検索結果へ遷移した。従来のCollator例外は消えたが、結果の追加処理で5秒watchdogに到達（5,392 ms）。layout崩れと継続操作の問題は残る。 |
| GitHub | 最終画像でCSS付きの暗色hero・ナビ表示へ改善。観測範囲でCSS arena不足、cache指定拒否、attachShadow/atob不足、EMFILEは出なかった。しかしメニュー操作の成功は未確認。別の`TypeError: not a function`とReact処理のwatchdogが残る。 |
| Google検索 | 実scriptを取得・実行したが`/sorry/`検証画面へ遷移。検索結果には到達せず、iframeも未完成。 |
| DeepMind | 背景・ナビ・見出し・記事リンクまで部分描画。動画初期化の`load()`不足で例外。動画・完全な操作は未達。 |
| HTML5test | 最終画像で公開テストscriptを実行し、ページをスクロールして**126/555**を確認。`data:` module拒否と`performance.timing.navigationStart`不足は残る。この点数を全Web標準の充足率とは扱わない。 |
| YouTube | 最終画像では旧AST不足が出ず、CSS付きの動画カードの読み込み用骨組みまで描画。10,845,860-byteの実scriptも取得・compile・実行したが、Timing・NodeFilter・アニメーション等の不足で例外。実際の動画一覧・再生には到達していない。 |
| ChatGPT | 未ログイン入口の「Just a moment...」とロゴから先へ進まず。アプリ画面・会話は未確認。 |
| 百度百科「三体」 | 指定URLを開くとqueryが追加され、タイトルが疑問符になり本文は白いまま。原因未特定で、文字コードや検証処理の問題と断定しない。 |

DuckDuckGo・GitHub・HTML5test・YouTubeはCSS予算・Fetch cache修正後の最終画像で再確認した。
Google・DeepMind・ChatGPT・百度百科の観測は、Shadow/Intl/TCP修正後、
CSS予算・Fetch cache追加前の画像による。後者を最終画像の合格結果とは扱わない。

DuckDuckGoの入力は、短い間隔で送った最初のQEMUキー操作が意図した文字列にならず、
focus確定後の全選択と各文字入力の間に3秒置いて再試行した。
正しい`cats`入力と遷移先を別々に撮影したが、入力の追従性まで正常と確認したわけではない。

GitHub最終画像では、1つのposted taskが32,072 ms、そのうちnative呼出しが
31,265 msを占めた。JS watchdogがnative処理から戻るまで停止できない経路が残っている。
その後のCSS応答1件に`Request deadline exceeded`も記録されたが、
native長時間処理との因果関係までは未検証である。
**即時EMFILEの修正は、全module失敗・timeout・性能問題の解消を意味しない。**

このように機能・描画の改善は確認できた一方、現代の主要サイトが全面正常動作する状態ではない。

## 証拠

- 初回一括試験：`build/nocturne-audit/gemini-repair/shadow-intl-tcp-batch-01/`
- 修正後一括試験：`build/nocturne-audit/gemini-repair/shadow-intl-tcp-batch-02/`
- 最終画像の一括試験：`build/nocturne-audit/gemini-repair/shadow-intl-cache-final/`
- 大型CSS fixture修正後の関連一括試験：`build/nocturne-audit/gemini-repair/shadow-intl-css-boundary-final/`
- 中間画像の実サイト：同directory内の`shadow-intl-live-ddg`、`shadow-intl-live-github`、
  `shadow-intl-live-github-css`、`shadow-intl-live-google`、`shadow-intl-live-deepmind`、
  `shadow-intl-live-html5`、`shadow-intl-live-youtube`、`shadow-intl-live-chatgpt`、`shadow-intl-live-baidu`
- GitHub最終画像：`build/nocturne-audit/gemini-repair/shadow-intl-github-final/`
- DuckDuckGo最終画像：`build/nocturne-audit/gemini-repair/shadow-intl-ddg-final/`
- HTML5test最終画像：`build/nocturne-audit/gemini-repair/shadow-intl-html5-final/html5-final-score.png`
- YouTube最終画像：`build/nocturne-audit/gemini-repair/shadow-intl-youtube-final/`
- ビルド：`build/nocturne-audit/platform-build.log`
- 選択したソースと媒体の同一性：`build/nocturne-audit/gemini-repair/final-identity.json`

検証用の媒体・serial・画面はローカル成果物で、Gitへ追加していない。
