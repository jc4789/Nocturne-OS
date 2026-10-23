# HTML解析拡張：停止時の全作業記録

## 状態

2026-10-10、ユーザーの「LOG ALL WORK AT STOP」指示で停止。実装は作業ツリーに保存しているが、完了・全面適合・無回帰とは宣言しない。コミット、既存変更の巻戻し、Hyper-V実行は行っていない。

停止時のプロセス記録は `build/html-parser-20261010/stopped-processes.json`。担当エージェント custom_elements / modern_tree / safety は停止。作業専用QEMUのBaidu、OpenAI、Redditを終了。Amazonは既に終了していた。ビルドと focused-3 試験は終了済み。再開の明示指示まで実装・試験を続けない。

## ユーザーの要件

- GUI-first、C、非UNIX/POSIXのNocturneを維持する。RAM-root、`/data`の永続化、既存PATHを変更しない。fork等を導入しない。
- WHATWG HTML解析を基準に、2011 tokenization文書も参照。既存Lexbor/native DOM、PI、Attr、script、Shadow、FACEを取り消さず、新しい木構築機能を含める。
- J-space medium、必要な担当エージェントには全コンテキストを渡す。
- QEMU WHPX、4096MiB、1920×1080。Hyper-V Gen2/enhanced sessionの実機検証はユーザー担当。
- Baidu百科、Amazon、指定OpenAI日本語記事、Reddit r/codexを実際のOSブラウザーで検証し、スクロールを確認する。
- **スクリーンショットは私自身の目視検証と不具合修正のためであり、ユーザーへの提示や試験ログの代替ではない。複数画面をまとめて撮影・検査できる。**

## 保存した実装

### 入力・tokenizer接続

`html_encoding.c/.h`、`html_lexbor.c/.h`、`webi.h`。

- バイト入力とDOMString入力を分離。DOMStringをcharset decoderへ再投入せず、FEFF/NUL/孤立surrogateの扱いを維持する。
- BOM、HTTP charset、1024-byte meta prescan、Lexbor encoding label/decoder、既存Shift-JIS経路を接続。UTF-8 scalar検査とreplacementを追加。
- certain/tentative encodingを保持。実際に受け入れたlate METAによる再解析を追加。UTF-16宣言とx-user-definedを正規化。
- 元bytes、Document root identity、初期base、CSP header/meta境界を保全してdecoder/parserを再生成。未実行script/moduleの準備状態を取り消す。
- encoding確定化は実際のauthor script/CE constructor入口で行う。別registryの候補に存在するだけでは確定しない。
- tokenizerそのものを別実装に置換していない。既存Lexbor state machineを使用する。

### 木構築・native DOM同期

Lexbor HTML tree局所patch、`html_lexbor_bridge.c`、`dom.c`、`doc.c`、`html_serialize.c/.h`。

- 宣言的Shadow DOMをtoken時点で構築。既存shadow identity、registry、closed/open、clone/serialize flagsを接続。
- template forの単一・range・nested内容更新とPIをnative木へ接続。
- fragmentの実際のShadowRoot文脈、foreign namespace、template contentを維持する。
- parser由来の非祖先form ownerをnative metadataとして保全。DOM移動・adoption・form属性編集で適切に解除する。associated-node listにより全node走査を避ける。
- no/limited/full quirks metadataとdocument encodingを公開。
- navigation METAの二重処理を避け、fragmentから実際のheadへ挿入したMETA policyはnative側で処理する。

### custom elements

`js_custom_elements.js`、HTML各interface constructor、`js_shadow.js`、bootstrap、`js.c`。

- scoped registry、customized built-in、internal is、owner realmを接続。
- parser constructorを属性・子・接続より前に実行。属性反応、挿入後connected反応の境界を分離。
- constructor失敗時の別bare fallbackと作者が捕捉したthisのidentityを維持。
- constructor内document.writeのInvalidStateErrorとactual author constructor encoding hookを追加。
- Imageを含むinterface constructorのnew.target経路を接続。既存FACE機能を残す。

### script・document stream

`js.c`、`doc.c`、`html_lexbor.c`、`js_script_safety.h`。

- bounded synchronous document.write挿入点とネストinline scriptの即時実行。SVG/未完tokenでは順序を壊さず保留する。
- top-level document.open/write/closeでDocument/Window identityを維持。既存generation/retirementを保全。
- parser由来script eligibilityを保持し、既開始scriptを再実行しない。
- HTMLScriptElementのprivate admitted-text snapshotを追加。child mutationで本文が変化した場合はprepare時にowner realmでTrustedScript検査を行う。
- default policy変換結果は準備済み実行sourceへ使用し、DOM本文・observer・serializationを改変しない。

### PI API

`html_pi.c/.h`、`js_pi_native.h`、`js_pi.js`。

- get/set/remove/toggle/getAttributeNames/hasAttribute/hasAttributesの7 API。
- native arena所有の順序付き属性map。ordinary data書換えで無効化。parseのOK/INVALID/OOMを区別。
- 作者prototype setterを呼ばないown data propertyで名前配列を返す。

### Sanitizer・Trusted Types・CSP

`js_html_safety.js`、native safety/host headers、constants、`html_policy.c/.h`、QuickJS局所hook。

- native木を処理するSanitizer config/安全baseline、HTML更新・parse/serialize API、安全/unsafe optionsを追加。
- TrustedHTML/Script/ScriptURL/ParserOptions、Sanitizer configのprivate native brand。偽装toStringを認証に使わない。
- cross-realm owner/default policy routing。Attr、Node alias、標準sink、timers、write、Range、dynamic compilationを接続。
- QuickJS evalの非string identityとdirect lexical evalを保全。Functionは元引数のslotを検査する。
- native HTTP headers、report-only、META、複数policyのintersection、policy name/duplicate制限を保存。作者オブジェクトでnative policyを置換できない。
- policy単位のtrusted非同期violation event、原policy・disposition・sample・endpointを保存。isTrustedはprivate slotで昇格を防ぐ。
- 自動default callbackの再入guardと例外復元を追加。
- 専用WEB_RESOURCE_REPORT/WEBNET_REPORTを末尾追加。POST、正規Content-Type、same-origin credentials、redirect errorをsubmit/wireの両境界で強制。作者Fetchのno-cors制約は緩めない。report response本文/headerは破棄する。
- **Reporting API/report-toは未実装**。report-uriに偽装して置換せず、能力不足を明示する。一般CSP source-listの全面実装やXML parserは今回の対象外。
- violationの実source行/列/HTTP status取得は十分でなく、sourceFileはdocument URL fallbackを含む。全面CSP適合とは言えない。

### Worker

`browserjsworker.c`、`js_worker.c/.h`、worker runtime、wire header。

- author sourceより前にnative brand/dynamic-code hook/CSPを初期化。
- HTTP Workerは実response headers、blob/dataはprivate creator snapshot。BlobはURL entry作成時のpolicyを選択。
- policy単位の非同期violation。private wire op12で親のnative reporterへ送る。公開sendから同opへ入れない。
- IPCのprototype復元後にもreport/init/endpointsをnull-protoへ戻し、作者getter/toJSONを実行しない。
- existing task/pipe/generation cancellationを維持。POSIX task modelを追加していない。

### 試験・生成・ビルド統合

- official WPT固定commit `6a3c46c7c119fc84a80a1082fc8d21f9f5c24e66` のtests1/foreign-fragment/templateをprovenance/licenseとともに保存。
- 305 source casesをscriptingの両設定で610 product-tree casesへ変換。expected treeを弱めずskipを追加しない。parse error code/countは未検証。
- modern native tree、byte/DOMString input、encoding restart、CE、PI、Sanitizer/TT、strict script snapshot、Worker CSP、window CSPのfocused fixtureを追加。
- jstestのinline fixture内literal script end-tagをescape。guest libcに存在しないfscanfをfgets/sscanfへ変更。
- Worker fixtureはfake 404 transportではなく、実webnet/webfetchとhost HTTP serverへ接続。
- CSP報告を実HTTP204応答で確認する追加試験を保存したが、**最新buildで未実行**。
- Worker/main generated embedとMakefile依存を更新。guest SDKコピーに追加headersを接続。

## 実行した試験と失敗

すべてfixture試験は表示受入の代替ではない。

| 証拠 | 結果と範囲 |
|---|---|
| build.log / build-2.log / build-3.log / build-4.log | 統合buildは各終了0。最新build-4は専用report transport/isTrusted/default guardを含む。 |
| focused-1/test-serial.log | currentscript21、waitingstream29、parserdedup37、native-tree52、input19成功。CE shadow contextとfixture script end-tagでJS失敗。原因を修正。 |
| focused-2/test-serial.log | input19成功。form association listとbridgeの一時的統合不一致、guest fscanfで失敗。いずれもsource修正。 |
| focused-3/test-serial.log | native-tree52、input19、encoding restart53成功。WPT610/610、skip0成功だがharnessのsummary prefix誤りでFAIL扱い。期待prefixを修正済み。 |
| 同focused-3 | CE、PI248、script snapshot、parser CE timing、top-level open経路成功。Sanitizer batchでisTrusted偽装防止assertionが失敗。fixtureより古いbuild-3だったため、最新private-slot実装をbuild-4に反映。build-4の再試験は未実行。 |
| 同focused-3 | native Worker inheritance group成功、CSP enforce/report/metaのAPI64/44/44成功。これは最新専用POST配送の受入ではない。 |
| 同focused-3全体 | harnessはpass5/fail2/skip0、終了1。失敗を成功として扱わない。FAT整合性成功。 |
| 担当補助試験 | CE host31、safety host43、report boundary host32、対象C syntax-only成功。担当報告の補助証拠であり、rootの実サイト受入ではない。 |

既存web全体回帰、最新build-4 focused再試験、fault injection、全面WPT tokenizer corpus、Hyper-Vは未実行。既存parser3群成功だけで全回帰なしとは宣言しない。

## 実サイトと実画面

全VMはC:/Program Files/qemuのWHPX、4096MiB、4 CPU、1920×1080。ユーザーの永続dataを接続せず、専用boot/data snapshotを使用。ブラウザー窓はデスクトップ内の通常サイズで、1080p全画面viewportへ最大化してはいない。

| サイト | 停止時点 |
|---|---|
| https://baike.baidu.com | 実ページを開いたQEMU framebufferのmanual-now.pngを撮影しrootが目視。中国語文字・画像は出るが検索欄周辺の重なり、右側見切れがある。正しい表示として受入しない。スクロール未確認。 |
| https://amazon.com | 実Amazonへredirect。screen-final.pngをrootが目視。ヘッダーは出るが下が大きく白い。表示不合格。原因は未調査/未修正、scroll未確認。 |
| https://openai.com/ja-JP/index/gpt-6-astra/ | Redditと並列で専用QEMUを開始。停止までにサイト表示の目視受入を行っていない。未確認。 |
| https://reddit.com/r/codex | OpenAIと並列で専用QEMUを開始。停止までにサイト表示の目視受入を行っていない。未確認。 |

画像は `build/nocturne-platform/html-baike-4/manual-now.png` と `build/nocturne-platform/html-amazon-4/screen-final.png`。それぞれ実際の1920×1080 QEMU framebufferであり、host Chromeや模擬HTMLの画面ではない。

### 撮影手順の失敗

1. 実サイト目視確認を後回しにしてfocused機能試験を先行させすぎた。
2. 最初のobserver引数を `/tests/site-observer.js` とした。guest-fileはdataの/testsへコピーされるため、実際のnative絶対パスは `/data/tests/site-observer.js`。ready markerが出ず、撮影ハーネスが待ち続けた。
3. ready待ちの間は定期撮影しないハーネスを選んだため、起動済み実ページをすぐ確認できなかった。
4. ユーザーの指摘後、Baidu用ハーネスだけを止め、QEMUは維持してHMPで直接screendumpした。最初はmonitorの初期promptを読み切らない不備で画像取得に失敗。修正後manual-now.pngを取得。
5. Amazonはハーネス終了時のscreen-final.pngを確認した。OpenAI/Redditは並列起動したが停止までに目視未完。
6. Chrome基準表示を開く補助CUA呼出しはtimeoutし、比較画面を取得していない。
7. 一括撮影・自分自身の目視確認を最初から優先すべきだった。画像提示やfixture成功はレイアウト/操作/scrollの検証完了ではない。

## 保存先と未完了の引継ぎ

- 変更ファイル全一覧・SHA256：`build/html-parser-20261010/stop-manifest.json`。
- tracked差分：`build/html-parser-20261010/stop-tracked.patch`。新規untracked sourceも作業ツリーに残り、manifestに列挙する。このpatch単体に新規source本文は含まれない。
- 担当報告：custom-first/second、safety-first/second、modern-tree-report-1/2、pi-api-report-1/2、html5lib-adapter-report-1/2。
- semantic map：`docs/html-parser-20261010-map.json`。停止までのJ-space記録は`.jspace/`に保存。最終ship gate/担当report登録・review cycleは未完了。未実施を通過扱いにしない。
- 表示不具合の原因調査・修正、4サイトの実画面/操作/scroll、最新focused再試験、既存web回帰が残る。再開時はまず現行QEMU実画面を**複数まとめて撮影しrootが目視**する。変更sourceを削除/巻戻しして未完了を隠さない。
