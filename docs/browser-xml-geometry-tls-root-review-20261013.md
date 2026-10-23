# XML・style-only geometry・TLS回帰の統合確認

## 独立したソース確認

- 実Redditの公開Source25:19284にXMLSerializer.serializeToString呼び出しがあり、未実装APIで挿入前に停止していた。主担当が実原画の左欄空白と公開source excerptを確認した。HTML serializerの代用ではなく既存html_serialize.c/hにXML専用処理を統合し、既存js_document.js/native_getから接続。namespace、属性、Fragment、template、doctype、HTML rawtext保全、OOM/取消rollbackの実装を独立して読んだ。XML parser/CDATA作成は別の未対応範囲であり、全XML対応とはしない。
- 新XML支持関数は53条件と数え、native集約側も53 exactへ強化。従来documents群を消さず併走する。
- 非生成before/afterの計算済みbodyを保持した版は、原paint12と疑似CSSOM原24＋20経路がnative成功。しかし新12条件のgeometry世代維持が失敗。担当がdoc.cでcascade後の確定geometry再使用を修理した。主担当はpost/cache/font/viewport/dirty条件、通常commit出口、paint/hit別世代と画像intrinsic invalidatorを確認した。原失敗期待は維持、5反例を追加して17条件にした。最新nativeはこれから確認する。
- 単独百度の新位相では、homeのTCP3.296秒＋TLS16.705秒、TCP15.278秒＋TLS4.722秒が新共有20秒予算で失敗した。従来成功取得も拒否する回帰として扱い、TCPとTLS予算を元caller契約へ分離。TLS全体は一操作の絶対期限であり、recordごとの延長には戻さない。既存HTTPの長いcaller予算を新規容量policyと呼ばない。debugのcallback内wallと外wallを分け、外を暗号CPU時間とは呼ばない。
- TLS担当は容量エラーで最終報告を返せなかったため、主担当が残った製品差分を引き継ぎ、TLS/HTTP/private metadataを読んで構文確認した。既存生成済み支持実行物の再実行は43/11/12条件で失敗0だが、実TLS認証・実サイト速度の代替にしない。private行の数値検証、旧形式互換、native callback前除去も確認。

## 実画面と残る受入

最新単独百度のphase-early/phase-homeを主担当が見た。記事はantispamのTCP20秒失敗、homeは上の共有期限失敗により白のまま。HTTP状態やdeadlineで早く失敗したことを高速化・表示成功としない。IPv4のhost HEAD対照はTCP3.466秒、TLS確立9.936秒、header10.686秒で200。hostは別経路なのでNocturne性能の証拠ではない。

XML・geometry・TLS最終凍結を正規embed/allへ統合し、IMG/VHD/VHDX/ISO更新後に原native失敗と実Reddit・単独百度を確認する。GitHub/DDG/HTML5testの先行成功を新sourceの回帰なしへ流用しない。

## 統合媒体での実確認と次の失敗

- compat-xml-sidebar-reddit-20261013 の xml-current/sidebar-page/down/up 原画を主担当が見た。左欄にHome/Popular/News/Explore/Resources/規約等が表示され、検索placeholderは1本、画像と丸ボタンも表示。実F12閉・PageDownで次の投稿群へ移動、PageUpで元の先頭へ戻った。正MIME partialの継続feed取得も始まり、以前のXMLSerializer TypeErrorはこのrunでは解消。
- 検索欄の実hover位置とnative pointer到達を原画/ackで確認し、実click後にcodex＋Enterを送った。URLは/search/?q=codexに変化したが、本文は旧feedのまま。現Fetchはreferrer overrideをNotSupportedErrorで拒否し、実検索は未達。これを共通Request/Fetch境界で修理する。作者DOM/.click()/style差替えで成功画面を作らない。
- browser-xml-geometry-tls-nativeのpaint12・CSSOM元24＋20経路は成功。XML53 exactを含む内部9チェックと従来documents21も成功。しかし外側runtestsがXMLSerializer count出力を要求する一方、fixture loggerがその新prefixを印字せず登録不備で失敗した。loggerへ正規prefix印字を追加し53 exactを要求した。失敗ログは保全。
- 非生成geometry原期待は17条件中1失敗のまま。診断を原条件に追加してroot dirty/cache/post/font/寸法を絞り、製品修理を続ける。原paint/CSSOM成功でこの失敗を相殺しない。診断と修正したXML登録だけを次のnative対象へ絞り、同じ未変更成功群を繰り返さない。

## 単独百度・TLS予算復元媒体の観測

compat-tls-restored-baike-home-20261013 はWHPX/4CPU/4096MiB/1920x1080、他VM/build/重いhost検査を同時に走らせず観測。主担当がhome-page/home-hover/home-wheel-down/up/reload-wheel-in-flight/settled、article-currentとwheel前後原画を見た。home本文・3hero画像を確認し、hoverで矢印が出ても画像は維持。実wheel4下で192px下へ、4上で先頭へ戻った。再読込中のwheel4を送り、白画面から表示後は先頭を維持。後の自走はこの観測では無し。

homeの4,117,334B scriptは5,469ms、再読込は5,797msで成功。前の100秒取得との差を実装による恒常的高速化率とはしない。新TLS内訳はcallback内2365ms/外9ms、再読込1598ms/外5ms。外を暗号CPUと呼ばず、このrunではTLS時間の多くがnative transport callbackにあるとの観測に限定する。

同VMで通常アドレス欄から戦鷹記事へ遷移。本文・portrait・hero動画の時系列進行を原画で確認。約30万DOM callのposted task実時間は3,080ms（元28,376msの同call数300043条件に対応）。1/64のnative推定値をこの実task時間へ置き換えない。新記事本文でnativewheelはvideo上と本文上の両方で位置不変のため、記事scrollは失敗。loaded本文だけで入力受入としない。共通wheel/default action境界を修理する。

長いURLのnativeキー入力はHMP送信完了直後にまだ末尾が表示されず、短い末尾入力を追加して実URLを目視確認してから記事受入した。HMP ackをGUI入力完了としない。console読取もpointer ack/入力focusが未成立だったので診断結果を成功扱いにしない。音声は聴取していない。動画進行は入力・音声・シークの全成功の代わりではない。

仕様：XMLSerializerの現行interfaceは https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-xmlserializer-interface 、serialization algorithmは https://w3c.github.io/DOM-Parsing/#xml-serialization 。疑似計算値は https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle 。

## Fetch・geometry統合媒体と入力遅延の確定

- `build/browser-referrer-geometry-wheel-integrated-20261013.log` の全体build成功。IMG/VHD/VHDX/ISOを更新し、隔離nativeの原pseudo17、新Fetch244と内部7チェックは2群とも成功、skip無し。証拠は `build/browser-referrer-pseudo-native-20261013/test-serial.log`。TLS、原XML、元CSSOMの成功を新たなWorker受入へ流用しない。
- rootがFetch/Requestのprivate参照元、旧64-byte wire、各redirect hop、forbidden Referer、opaque/srcdoc source、parser changed publicationを独立読取。新getterの全DOM同期と127.* suffix信頼の初版不備は具体反例を受け修理済み。Worker最終支持は原と修正版の両失敗が残る。debuggerもDLL不足で対象実行前に停止し、backtraceは未取得。
- 記事の最初の診断起動は通常窓だったため基準viewportとは比較不能。原early画像を見てそのVMを止め、次の `compat-wheel-maximized-baike-article-20261013` を同WHPX/4CPU/4096MiB/1920x1080、最大化・debug consoleで観察した。
- rootがloading/page/down/up原画を見た。F12閉後に旧console高220pxが空白のままで、本文pointer ACK成立後もwheel前後の本文位置は不変。その後のserialでは8入力全てallowed=1、rootmax12292、0→192→0へ遅れてまとめて実行。84,901msループのうちhost layout2回79,110msであり、取消・短い本文・範囲不足ではなかった。ハーネスACKを操作結果と誤認しない。動画のraw IPC量をネット本文量にしない。
- 共通媒体serviceが開始時計を次の待間隔へ算入し、供給自身が1/5msを消費するとC safepointごとに再供給する源の境界を独立読取。担当の完了時計基準修理は既存pump/PTS/FIFO/codec世代を保全し、制御時計の実関数支持16条件成功。実79秒解消・音声動画の回帰は修理後媒体の確認待ちであり、host成功で相殺しない。

## 最新2VM機能バッチの原画・検索結果

`compat-referrer-final-reddit-20261013` と `compat-referrer-final-ddg-20261013` のbatch-current/ready原画をrootが同区間で見た。2VMは機能確認用で、速度比較はしていない。

- Reddit：左欄・投稿・丸ボタンを確認。検索欄のnative pointerと実hover画像を見てclick、expanded panelが表示された後、codex＋Enterを実キー入力。検索URLへ変わり旧referrer未対応例外は消えたが、search-settledとscreen-finalで本文は旧feedのまま。作者timerのAbortControllerがAbortErrorを作っており、native期限と断定しない。pending20は全pending件数、stream ENDのbody_len0も空本文の証拠ではない。検索は未達、追加opt-in数値診断で製品境界を絞る。
- DuckDuckGo：丸い検索欄・影・inlineフィルター・丸favicon・知識cardを原画確認。consoleを閉じ実PageDownで下の結果群、PageUpで元先頭へ戻った。検索欄のnative pointerとhoverを確認し実click、Ctrl+A、nocturne os＋Enter。screen-finalで新URL、入力文字列、対応する新検索結果を確認。初期送信直後の部分文字表示だけでは受入していない。
- 両VMは600秒で終了し、遅い最終control要求は未実行。保存済みscreen-finalをrootが開いて結果を確認した。終了後のcontrol timeoutをサイト失敗の証拠にはしない。Reddit最新上下操作とGitHub/HTML5test最新媒体の影響確認はまだ未完了。
- 今回再開後に停止・build内解決済path・非link・非使用を確認した20一時boot/dataを回収。ログ・原PNG・metadataと通常data、最新配布IMG/VHD/VHDX/ISOを保全。削除した試験媒体は再実行可能と報告しない。

## 媒体cadence修理後の実操作と残る長task

最新全媒体build `browser-media-cadence-abort-integrated-20261013.log` 成功後、単独 `compat-media-cadence-baike-article-20261013` を同WHPX/4CPU/4096MiB/1920×1080で確認した。root自身がpage/down/up/reload-in-flight/reload-current/screen-final原画を開いた。F12後の旧console高さの灰空白は復旧し、native本文pointer ACK後のwheel4下と4上が短い操作バッチ内で0→192→0へ成立。page/down/up取得時刻は199.343/202.276/204.331秒。Ctrl+R中のwheel4は旧表示で即移動し、新文書のcurrent/finalは先頭を維持、この観測で後の自走は無かった。映像原画とPTS進行は確認、音声の聴取は未実施。

実loopのhost layout最大は旧79,110→1,423msへ減ったが、初期ロード・全体性能は未達。Timer675は実45,188msのうちlayout45,091ms、87flushes、geometry170calls/insert89、DOMrevision268。媒体cooperative7,587回/24,952msもこのtaskに含まれる。推定1/64 native時間とは区別する。css child-list親dirtyから既存本文forest全recascadeと全box再建へ広がる境界を担当が修理中であり、源だけの変更をこの実結果へ流用しない。

## 最新媒体の機能バッチと実失敗の反証

- `compat-abort-snapshot-reddit-20261013` と `compat-media-final-github-20261013` のroot-page/down/up原画を同区間でrootが見た。両サイトのnativewheel4＋PgDnで下の本文へ、wheel4＋PgUpで先頭へ戻った。Reddit左欄、GitHub右上の全ボタン・Aboutとtableの分離・省略glyphは維持。2VMの機能観察で速度比較はしていない。
- GitHubはhoverしたPlatformをnativeclickしてもroot-menu-settled/finalで開かず未達。保存公開HTMLと新実read-only elementFromPointはdetailsではなく通常buttonだった。新 `compat-button-target-github-20261013` でもbutton/type button/expanded false、nativeclick down/up/target同一、React expando trueだがexpanded false。SVG computed transform noneと共通rotate parser未対応は矢印方向の別不足。aria-controlsを特例toggleして成功画面を作らない。
- 初回Redditの検索欄はcodex実入力までで観察期限が終了し、Enter未実行。Abort診断を取得したとはしない。操作時間不足を訂正した `compat-abort-interaction-reddit-20261013` では左欄・expanded panelを確認し、read-only shadow active chainはREDDIT-SEARCH-LARGE→FACEPLATE-SEARCH-INPUT→TEXTAREA。実入力値長10に対しrect713,19,484,0・block/visibleで不可視。Ctrl+Aで自身の入力をcodexへ置換し実Enter、search codex URLへ変わるが初期feedのままで検索未達。私的値やopaque query IDは記録しない。
- `compat-media-final-html5test-20261013` のroot-early/page/down/up-hover/plus-open原画をrootが見た。5＋は白main内、nativePgDnでforms内容へ、PgUpで先頭へ、実pointer941,328とhoverを確認してclick後のmenu表示を確認した。この機能観察を互換性スコア全体の成功としない。

## 未読Fetchと生存transportの所有を分離

新実Redditの作者Abort時の数値snapshotはP_FETCH33件で全host受理済み、26件はResponse header未公開、7件は公開済みでEND未達。これを33件全部が検索要求、期限切れ、空本文と断定しない。各snapshotはnative resource IDでありwire IDではない。新GHのbuttonにはReact onClick data functionが存在し、実click後のactiveElementはBODYのまま。focus不備とmenu原因は別である。

調査助言を受けrootがwebnet・kernel pipe/poll/waitの実源を独立確認した。paused Responseはpipeとrequestを保持するが、既存helperは動的pipeへ全文/ENDを入れて終了可能。旧webnetはpause中に回収せず、終了済みhelperまでorigin枠へ計上し続けていた。rootはHUP時のWNOHANG回収と肯定的PID/ECHILD判定、実生存PIDだけのorigin計上を修理。本文/pause/部分frame/callback/END/取消の所有を保ち、新FIFO・一律枠解除・期限変更は加えていない。通常finishingも一時waitエラーで所有者を捨てない。

既存schedulerの期待を残した実関数抽出支持48条件は成功。最初のhost includeはWindows CRTとnative ssize_tの型衝突で失敗し、型失敗ログを保全、native header全体でなく実ABI flagを用いるprefixへ訂正した。候補の支持であり実helper成功ではない。既存webnettestの境界群に、未読5Responseの実helper回収→同origin第6Fetch→resume後の全byte/順序/END/取消回収を追加。native・実Reddit受入は最新全媒体の次段階。

rootはこの作業の終了済み6runについてbuild内解決済path、非link/junction、非使用、user_data未接続を確認し、一時boot/data12個だけ回収。原PNG/log/metadataと最新配布媒体・通常dataは保全、D空き78.23→80.86GiB。build実使用154GiBを確認した。新Fetch担当IDはj-spaceの履歴8枠上限で登録拒否、ID偽装や台帳手編集をせず読み取り助言だけで終了し、製品修理はrootが引き取った。

## 局所CSS・paused transport・focus統合媒体の実結果

正規binding生成後の `browser-local-layout-stream-focus-integrated-20261013.log` allは終了0。IMG/VHD/VHDX/ISOを更新した。kernel hashは3cb380c0120aac230abb5420e87ea1f3d67fad87cea663e4e4e31a130f0c4e3a、initrd hashは01f51ee5db7be548e2e37a3df25d9c8f15bbc01292312efa8cb0963b79980e6c。rootはfocus候補、共有textarea行配置・描画、wrapのpresentation hintと作者優先、内部padding clipの親漏出防止を独立読取した。

隔離nativeは2PASS/2FAIL、skip無し。原geometry17とchild-list25は成功。通信の新5条件は失敗した。源を追うと既存http.cがGETにもUPLOAD(0/0)をHEADERS前に通知し、追加fixtureが未知イベントとしてpauseしたため、HEADERSへ進めていなかった。fixtureだけをUPLOAD検証・resumeへ修理し、元のHEADERS/CHUNK/END/全byte/第6要求/取消期待は保全。textareaはJS errors/failures0だが、追加正常出力名が既存loggerの「 API checks 」印字契約と合わず外側marker不合格。27条件を弱めず出力名とruntests markerを一致させた。原FAILログは `browser-local-layout-stream-focus-native-20261013/test-serial.log` に保全、修正版nativeは未実行。製品不合格と検査契約不備を混同しない。

`compat-local-focus-github-20261013` と `compat-local-focus-reddit-20261013` の原画をroot自身が見た。GitHubは実pointer120,33・hoverを確認してnativeclickし、root-menu-clickでPlatform menuの本文開封が成立。Escで閉じ、本文pointer→PgDnでファイル下部、PgUpで先頭へ戻り、hover popupも表示。右上全ボタン、About/table分離、ellipsisは維持。矢印は右向きで共通rotate不足は未解消。Redditは左欄、丸ボタン、投稿を確認し、実935,29 hover→click→codex入力。root-search-settledで可視の文字と候補、実Enter後root-search-currentで対応する検索結果本文を確認。さらにnativePgDn/PgUpで検索下部と先頭へ戻った。初期submit原画は旧feedのままだったので、それだけで受入せず後の実結果で判断した。2VMは機能観察で性能比較ではない。

上記VMとnative/buildプロセスを止めて、同WHPX/4CPU/4096MiB/1920x1080、同裸記事URL・最大化/debug設定の単独 `compat-local-layout-baike-article-20261013` を起動。root-early/page/loading-down/down/up原画で本文、portrait、動画の時系列進行、F12後の本文高、nativewheel合計4下と4上の0→192→0を確認。最初のwheel requestのpackets付加はハーネスが消費しないため1入力だけ、後で3入力を追加し、元4条件へ合わせた。音声聴取は未実施。初期本文が見えたことは全ロード速度の完了証拠ではない。

**性能はまだ不合格。** 同じgeometry170/insert89/87flush/DOMrevision268のtaskは旧Timer675 45188ms/layout45091msから新Timer741 41104ms/layout40968msへ減っただけで、約41秒が残る。新区間style-nodesは810→1161batch、11335→15092nodes、work214541→754150、wall8188→28449ms。媒体cooperative6946回/22308msもinclusiveであり、CPU処理と二重加算しない。局所枝支持成功を大幅性能改善・回帰解消とはしない。担当へCSS候補/宣言/box適用境界の修理継続を渡し、rootは媒体yieldの実負荷を別に査読する。

この段階で終了したGH/Reddit/nativeの8一時boot/data/extracted payloadを、build内解決済path・非link/junction・停止状態を確認して回収。原PNG/log/metadata、通常data、最新配布媒体は保全し、D空き78.88→80.59GiB。単独記事の媒体はこの時点ではまだ回収していない。
