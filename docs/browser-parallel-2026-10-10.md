# ブラウザとユーザースレッドの並列化

## 実行契約

- `thread_create` は同じアドレス空間・プロセスの FD/cwd/ウィンドウを共有する。各スレッドは独立したスタック、カーネル継続、XSTATE、FS ベースの TLS/errno を持つ。`thread_exit` は当該スレッドだけ、プロセス終了は兄弟を停止してから共有資源を回収する。
- 全オンライン CPU のユーザー用ランキューを使う。AP のシステムコール、ページフォールト、ウィンドウ操作などの可変カーネル状態は BSP への mailbox 継続として処理する。カーネル全体を無条件に並列安全とする変更ではない。
- `wait_on_address` は共有アドレスの 32bit 値を比較して待機する。`wake_address` の count=0 は全待機者を起床する。ユーザー側は atomic release/acquire で状態を公開する。
- ウィンドウのユーザー描画バッファと compositor 用スナップショットを分離する。`present` の矩形コピー後にだけ compositor が読む。
- 時計は Hyper-V reference TSC page の sequence/scale/offset、またはネイティブ invariant TSC を使用する。reference page はユーザーへ読み取り専用・非実行で公開し、sequence=0/不安定時はシステムコールへ戻る。

## ブラウザ

1. `debug_js` が無効なら DOM などのプロファイル時計を呼ばない。
2. CSS は共有 AST ごとに索引を一度作り、各 document/shadow root は軽量 binding を持つ。要素は自 scope、`:host`、`::slotted` の候補だけを調べる。
3. 静的 DOM 呼び出しを 67 magic opcode に変換。MutationObserver は native 記録と microtask バッチ配信。CE と live Range の必要な同期順序は維持し、無関係な hook は省略する。
4. storage は RAM を更新し、100ms の静穏期間／最大1秒の汚れ期間で flush。navigation/終了時は強制 flush。失敗時は RAM を保持して非強制再試行を1秒抑制する。全 snapshot の読み戻しはしない。既存の cross-process storage coherence と電源断耐久性を新たに保証するものではない。
5. dirty ノード／祖先の style 再計算と semantic geometry 比較を使い、互換な変更は boxes を維持する。各 box の line 配列を保持し、無関係な formatting context は再利用する。resource/viewport/構造変更、複雑な table/SVG 等には full fallback を残す。
6. `parallel_for_service` は持続的な共有メモリ helper pool。仕事は必ず join してから資源を公開・解放する。待機中の supervisor は native chrome/cancellation のみを処理し、JS/DOM を並行変更しない。
7. style は親確定後の独立子ツリー、layout は intrinsic sizing と幅確定後の普通の flex/grid 子ツリー、raster と画像変換／縮小は非重複行タイルを並列化。大きい CSS 解析・画像 decode は helper へ出す。ネットワークは既存の非同期 fetch プロセスを維持し、全コア scheduler の対象となる。

## 安全な直列経路

- nested pool 呼び出し、小さい仕事、浮動・絶対配置・table・media・疑似 inline が共有 node anchor を使う full layout は順次処理。
- ancestor placement、flex cross-axis / grid track の縮約、paint の重なり順は順次処理。全 DOM を無条件に並列変更しない。
- font の lazy load／glyph cache と malloc の共有状態には待機可能 mutex を置く。重い decoder の共有 STB 状態も排他する。
- worker の arena trap は worker のスタックだけへ戻る。失敗時も全 helper を join してから supervisor を unwind する。

## 検証経路

`tests/threadtest.c`、`tests/paralleltest.c`、`tests/styleincrementaltest.c`、`tests/paralleljstest.c` が共有 VM/TLS/FD/wait、複数 CPU、直列版との画素一致、局所更新、scope 索引、実 `web_live` の通知順を検証する。fixture の合格を実サイトや Hyper-V 実機の合格と読み替えない。

配布用は `build/nocturne.vhdx`（Hyper-V）。QEMU/WHPX は隔離テストにだけ使用する。実行ログと画面は `build/parallel-*` と `build/nocturne-platform/parallel-*` に保存する。

## 動的分配と統計の読み方

仕事を固定等分せず、bounded CAS により一件ずつ取得する。共有 descriptor は全 helper の完了まで有効であり、停止時も epoch 自体を変更してから wake/join する。fatal helper によるプロセス abort 後は、未実行の待機 syscall を再登録しない。

`--debug-js` だけで style、intrinsic/layout、raster、decode/convert/scale、CSS parse の統計を記録する。`items` は callback 件数、`work` は訪問・試行した node/box、候補 pixel、入力 bytes であり、完成出力数ではない。表示中の `AP` は native helper task を指す。task は移動できるので CPU0 上で動く場合もある。CPU mask は標本、worker 時間は包含経過時間であり CPU 使用時間や速度向上の倍率ではない。

修理済みカーネルの `parallel-pending-abort-verified-20261010` では共有 VM/TLS/FD/wait/WM と意図的 helper fault の status134 が合格し、pool/画素/取消/停止回帰は30検査・0失敗。1920x1080x32、WHPX、4コアで実行した。

Reddit の実ページでも style と raster 等の helper 実作業を記録した。ただし表示はまだ不合格であり、同時 VM 負荷のある測定から速度向上は断定しない。

指定 OpenAI 日本語記事の旧画像では確認スクリプトの実 HTTP404 で本文に未到達だった。Navigator の HTTP/JS 識別情報と実 cookie/core 能力を修正した画像の一回観察では、同じ404を記録した後、通常の確認処理が進み、記事タイトル・h1・article と Next の実資源へ到達した。ただし修理との因果関係は未確定で、最終画面は header の下が黒い。Next の `document.currentScript` 不変条件エラーがあり、冒頭・日本語本文・動画・グラフの受入れは未完了。ほかの実サイトや fixture の合格で代替しない。

この指定ページでも style 訪問188,932／helper136,616、CSS parse入力92,523,106 bytes／全量helperを記録し、両 stage の標本 CPU mask は全4コアの `f`。これは実callback内の仕事の証拠であり、ページ全体の合格や速度向上率の証拠ではない。

同じ統合画像の変更境界では、選択編集34検査・0失敗、Fetch集計7検査・0失敗（内部JS表208件）、Navigator214検査・0失敗を確認した。初回ハーネスの古い37件期待とCSS内部ヘッダ不足は隠さず保存し、成功した検査を繰り返していない。

新コンテナ検査の初回80件・8失敗は、CSSOM再構成でstandaloneのtype0群が消える共通根因だった。認識済みgroupを保存し、importなしの二重parseを除いた後、実guestで87件・0失敗、banner128/192切替を確認した。classic scriptのstack-empty checkpointより早くcurrentScriptを復元する欠陥も修理し、別の実guestで21件・0失敗。成功したこれらの検査は反復していない。

入力なしdocument.open待機はexport/importを省略し、次write/EOFではnative DOM exportと新CSS/本文を保持する。新fixtureのヘッダ不足、viewport未初期化、既存RGBA表現との期待差は保存し、修理後の限定guestでは29件・0失敗、実RGBA値3箇所も一致した。

さらに、完了済み外部CSSの不変ASTを独立arenaに保持し、再scanではscope/order/media wrapperだけを再生成する。CSSOM編集済みAST、importのsource order/cycle/mediaを維持する新23件が実guestで全て通過した。保守的fullscan/index再生成/full cascadeは残っており、差分通知全体を解決したとはしない。

currentScript修理後の指定OpenAIでは旧不変条件エラーが消えたが、実Next chunkのSVG `baseVal` 未定義エラーで記事がReactエラー画面に置換された。黒いheroから先へ進んだことも合格とはしない。実ブラウザの通常same-origin fetch一回で公開chunkを200取得し、失敗式は `svg?.viewBox.baseVal` と確定した。現在は一般SVGのlive viewBox、getCTM/getScreenCTM、実path長さ・点、DOMPoint/DOMMatrixを実装し、実guestで109検査・0失敗を確認した。CSS/3D transformや全SVG interfaceの対応を意味しない。

外部CSS ASTキャッシュ入りの実OpenAI診断では、CSS parseの累積入力は4,721,624 bytes（101 callback、全量helper）、styleの実訪問207,655／helper147,861、標本CPU maskは全4コアの `f`。以前の反復parseによる101,398,938 bytesまでの増幅はこの観察にはない。ただし読み込み内容・観察期間が異なるため速度倍率は算出せず、SVG例外で記事が消えたこの診断もサイト不合格のまま保存する。

Lexbor importは属性・text・全child順序・ownership・root/quirksの実差分を検出し、内容不変のimportで全体revision/dirtyを増やさない。66文書の固定通知上限を動的集合へ置き換え、adoptの旧・新ownerを通知する。実変更に対する保守的full fallbackは残す。最初のfixtureが既存top-level document.open非対応で失敗した記録を残し、対応済みchild contextへ修理した限定guestでは37検査・0失敗だった。

SVG＋差分import入りの一回の実OpenAI観察ではbaseVal例外と記事消失が再現しなかったが、実1920x1080画面は黒いheroのままで受入れ不合格。読み込み途中の古いchart座標に対してhostは誤clickを拒否した。fixture合格やarticle DOMの存在を、銀河・日本語本文・動画・グラフの表示成功へ読み替えない。

重いobserver callbackが再びlayoutを汚すとrender observerが各tickを先取りし、rAF/timer/postedを永久に選ばないtask-source飢餓を修理した。observer後はdueを完了時点から設定し、次turnで別のready taskへ機会を渡す。通常taskがない場合のobserver deliveryは維持する。継続ResizeObserverフィードバック下でtimer/rAF/MessageChannel、rAFによるnative Canvas2D生成と緑RGBA pixel、disconnectを確認する新限定guestは11検査・0失敗。次の実OpenAI画面が同じ成果になるかは別の受入れで判定する。

## 追加の実ページ修理と未完の境界

Redditの本文リンクは実HTMLで、資源のない `<object role="none"><a>…</a></object>` に包まれていた。未対応objectのfallback childrenを普通のflowで生成・描画するよう修理し、objectだけをUAの置換要素既定サイズ・灰色背景から除外した。作者CSSは維持し、iframe/embed/canvasは変更しない。新実guestは37検査・0失敗。`parallel-reddit-object-fallback-live-20261010` の実画面では、以前の300×150の灰色枠が通常のリンクへ変わり、バナー128px、中央732px／右316px・間隔24pxも維持した。実PgDn後のscrollYは1798。ただし左ナビは空で、固定配置の子まで文書scrollを引いてしまう共通native座標欠陥も具体化した。Reddit全体の表示合格とはしない。

native待機中のF12にも、通常event経路と同じconsole/page focus切り替えを追加した。JSやDOMを待機中に並行変更するものではない。新実RedditではF12で閉じた後のPgDnがページに届いたが、この画面だけで長いnative待機branch自体の全操作を証明したとはしない。

observer公平化後のOpenAIは通常の確認ページに留まり、記事へ未到達だった。sandbox browsing-context flag未対応によるframe拒否を記録したが、これを唯一の原因や前回修理の退行と断定しない。安全なsandbox制約を実装せず拒否だけ外す変更、ブラウザ名の偽装、記事HTMLや描画APIの偽造は行わない。最新画像とreadonly frame policy診断の観察も、冒頭の銀河・日本語本文・動画・グラフ・実操作とは別に判定する。

Hyper-V用VHDXは生成済みで、形式・rawとの論理内容比較が成功した。起動媒体は実際にはMBR/FAT32-LBAであり、最初の検証補助のGPT仮定を修理し、抽出kernel/initrdのSHA256と生成物の一致を確認した。`build/parallel-vhdx-verify-20261010/result.json` に対応画像のhashを保存する。これはHyper-V上での実bootではなく、既存VM・ユーザーデータも使っていない。その後のsource変更は、この検証対象と区別して新成果物へ紐付ける。

object/F12修理画像による次の指定OpenAI観察は、通常の確認後105.459秒で記事へ到達し、日本語本文の実画面と12回のtrusted scroll（scrollY 10788）を記録した。SVG例外による記事消失も再現しなかった。一方、main heroは黒く、動画・グラフとtrusted chart clickは未合格のまま。別iframeのWebGL contextエラーをmain heroの原因とは断定しない。この実ページではstyle訪問1,287,941／helper928,361、raster候補602,434,804／helper579,803,780、CSS parse 9,902,096 bytes／全量helperを記録した。仕事と標本CPU分布の証拠であり、速度向上倍率やサイト全体の合格ではない。

上のReddit固定要素は、scrollY 1798の時にclient y=-1742となっていた。`box_visual_x/y` を共通の文書座標契約に直し、fixed祖先のviewport offsetを子にも一度だけ適用する。paint、DOMRect、top-layer modal、iframe cropの二重補正を除き、内部scroll/overflow clipは保持してfixedより外のclipからは分離した。IntersectionObserverも同じfixed祖先境界で外側clipの歩行を止める。新57件は統合画像の実guest待ちであり、左ナビのlazy取得が解決したとはまだしない。

sandboxは全面的な対応ではなく、安全な限定subsetを実装した。明示allow-scripts/allow-same-originのHTTP(S)文書が全祖先とcross-originの場合だけ新たに許可する。pending policyとactive文書generationを分離し、祖先制約を継承する。初期blankではauthor JS realmを作らず、native WindowProxy tokenだけを維持する。opaque、作者srcdoc/about:blank、祖先same-originへのredirect、response CSP sandbox、未対応profileは拒否を維持する。フォーム・dialog method・navigation・auxiliary・借用非同期入口にもnative guardを接続した。child formの実送信やsandboxを継承する新windowは依然未対応。新92件は統合画像の実guest待ち。

debug_js時だけ、実Canvas getContext要求の既知6種類を文書ごと各一度、originのみ記録する診断を追加した。未知作者文字列とpath/queryは採取せず、2Dの実画素や未対応GPUのnull契約を変えない。新22件は統合画像の実guest待ち。指定ページのmain renderer APIと別iframeを区別するための診断であり、描画対応自体ではない。既成功したSVG/parser/observer/objectの検査は反復しない。

この統合画像の新guestではsandbox92件が全て成功した。fixed/contextの初回はincremental web_liveのparser準備前にsetupしたfixture不備で失敗し、その記録を保存した。初期parser task待機だけを追加した後、失敗2groupだけを再実行してfixed57件・context22件が全て成功した。合格済みsandboxは反復していない。対応するinitrd SHAは`d23ced04b236e1ff42a308ebc41bd9558e4e7458228cd620b4ff8e39d694aea4`、WHPX4コア・実1920x1080x32である。新VHDXもrawの論理内容とkernel/initrd/UEFI資源のSHA一致を一度確認した。Hyper-V実bootは引き続き未検証。
### 固定座標を含む実サイトの結果（2026-10-10、d23画像）

`build/nocturne-platform/parallel-reddit-fixed-sandbox-live-20261010` は隔離 WHPX、1920×1080、4 CPU、4 GiB、既存ユーザーデータなしで終了した。実スクロール 1798 後も固定ナビの矩形は `[0,56,272,891]` で、以前の画面外への移動は解消した。バナー、投稿本文、右欄は描画された。ただし左欄の遅延読み込み要素は高さ 0／子要素 0 のまま、末尾には次ページ取得エラーが表示された。ハーネス成功はサイト全体の合格ではない。

`build/nocturne-platform/parallel-openai-fixed-sandbox-live-20261010` では通常の確認を経て 62.123 秒で記事へ到達し、日本語本文を実画面で確認した。銀河ヒーロー、動画、グラフは依然未合格。実ネイティブポインタが対象へ到着しても、JavaScript 側の対象確認が期限内に届かず、ハーネスはクリックを拒否した。誤った対象へのクリックは送っていない。別 iframe の WebGL エラーを主ヒーローの原因とは断定しない。

### 後続の実グラデーション／入力境界修理

線形・二円放射の実 sRGB CanvasGradient を native RGBA bitmap の fillRect、path fill、stroke、fillText、clip、save/restore、paint-time CTM へ接続した。停止色は変更時だけ安定整列して再構築し、ピクセルごとの JS 往復や CSS パースは行わない。描画ループは共通 native checkpoint を各行で呼び、内部の時刻間引きと二重にしない。

新 `browserdeferred` は27/0。新 `canvasgradient` は初回89/1（接円の接点）を保存し、係数の浮動小数点 cancellation だけを相対判定する一般修理後、失敗 group のみを再実行して89/0となった。最終この段階のinitrdは `c974e5a98cb849f07ca3bcb3b8c08e690b99d12065e7db6cfde4a51a54e981ff`。いずれも隔離WHPX、4CPU、1920×1080、既存ユーザーデータなしで終了した。これを指定サイトのヒーローやグラフの成功へ転用しない。

新しい実OpenAI observerは、既採取の12 renderer文脈を繰り返さず、intro atlas factoryの後段と限定可視幾何を通常のsame-origin公開資源取得から採取した。実コードは `fetch().blob()` → `createImageBitmap()` → 実 `drawImage()` → `close()` の寿命を使用する。これは mobileFrames 経路の確認であり、1080p desktop 主ヒーローの唯一原因とは断定しない。

入力滞留の修理は、押下していないマウス移動の連続した末尾だけを最新の実イベントへ集約する。キー、ホイール、押下、解放、ドラッグ、修飾キーの変更は跨がない。キューへ入らず即処理されるブラウザ操作も連続性を破棄する。ネイティブ入力確認は従来どおり実 dequeue 時点で記録し、DOM コールバックの確認へ置き換えない。新しい `browserdeferred` 27 項目は実guestで成功したが、実サイトの入力遅延解消は別途確認が必要である。

この段階の実OpenAIでは63.335秒でJP本文へ到達し12回のtrusted scroll（scrollY10788）を記録した。主heroは黒く、動画・グラフも未合格。候補tabは存在したが対象hitの安全条件を満たすCLICK markerが出ず、pointer実操作は行われなかった。入力集約の実サイト合格とは扱わない。

### 独立画像と一般legacy行省略の統合

ImageBitmapは実decoderからQuickJS所有の独立ARGB画素を作り、Blob/ImageData/Canvas/loaded image/Bitmap snapshot、crop・resize・flipY・drawImage・close/finalizer・taintへ接続する。Window-onlyかつ同期native decode＋deferred Promise settlementの限定実装で、transfer、Worker、cross-realm、SVG Blob、完全EXIF/ICC、明示premultiplyと高品質resizeは未対応として拒否する。

Redditの実本文プレビューで使われる `-webkit-line-clamp` は、サイト名や6行固定ではなく、一般の正整数予算・実line fragment・実font幅ellipsisとして実装した。段落間予算、hidden paint/hit/scroll、動的解除とCSSOMを扱う。独立レビューで見つかった非表示段落の検索scroll漏れも最小修理した。clamp subtreeの前計算layoutは残り、通常の並列・cache経路は保持する。標準unprefixed shorthand、完全bidi/縦書き/grapheme処理は未完成。

行省略は実guestで83件・0失敗。ImageBitmapは初回のTCC非対応atomicをfixtureだけ修理した後、109件中107件成功・2件失敗だった。JS画素・codec・crop・resize・closeなど84件は全成功し、2失敗はfixtureのdefault QuickJS allocatorが実ブロックサイズを数えないためだった。本番は既存の正確な sized allocator を使う。成功84件と行省略83件は反復しない。

fixtureを本番同等の sized allocator へ直した新heap専用境界は9件中8件成功。唯一の失敗はpixel payloadとQuickJS内部header込みのbackend requestを混同した精密期待だった。この記録も保持し、実成功requestを測定する新OOM専用3件だけを実行した結果は3件・0失敗。成功requestと失敗requestはともに262152 bytes、payload262144 bytes、cleanupはbaseline170352 bytesへ戻り、runtime解放後live=0だった。期待へ固定の+8や広い許容範囲を足して合わせていない。

同じ新画像の実Redditでは、実画面で冒頭4行＋箇条書き2行だけ描画されたが、プレビュー高さ876px／投稿高さ1026pxの巨大空白が残った。左ナビの定義済みlazy要素も子0・高さ0のまま。6行のpaint抑制をサイト全体の合格とはせず、匿名inlineとlistが混在する高さの新境界を修理対象にする。この観察のmetadata経過16643.57秒は要求210秒と異常に乖離し、初期stageだけなので、通常の完走や速度測定として扱わない。

317点のHTML5testスクリーンショットはユーザー報告であり、新画像で点数検査を反復せず、指定実サイトを受入れ対象にする。

### SVGサイズとCSSアニメーションの新境界

ユーザーのinline SVGのヒントから、CSSで片軸を確定してもauto軸が親の無関係な寸法で上書きされる一般欠陥を確定し修理した。viewBox比2:1で幅40px／高さautoなら高さ20pxへ、高さ20px／幅autoなら幅40pxへ導出する。比のないfallbackと両軸明示を保持し、SVGだけのmin/max自動軸転送を含む新37件へ分離した。CSS `aspect-ratio` 属性自体は未対応のまま、旧SVG109の成功をこの新境界に転用しない。

このSVG37件、早期画像Promise14件、匿名inline/listのclamp親flow35件は `parallel-svg-promise-flow-verified-20261010` の初回実guestで全成功した。clamp混在例はfull160／cut120／post120／親130px、早期Promiseはreject反応→後続microtaskという実順序を記録した。既成功SVG109／画像84／行省略83を反復していない。一般の混在例が通っても、実Redditの巨大空白の根因が解決したとはまだ扱わない。

公開ページのCSSにあるopacity animationに対し、native parserが `@keyframes` を捨てanimation宣言も未実装という別欠落を確認し、一般CSS opacity animationの単一・通常要素限定実装を保存した。immutable keyframesを既存scope bindingで解決し、doc-owned playbackへASTポインタを保持しない。負delay、fill、方向、小数iteration、pause、bezier、var、同offset後勝ち、important／WAAPIの優先を扱う。時計はsupervisorが一度snapshotしAPは値だけ読み、16ms tickはactive nodeだけdirtyにする。新91件と実画面は別の受入れである。transform描画、複数animation、CSSAnimation object/events、未対応GPUの成功を偽装するものではない。実OpenAIの銀河・グラフが直ったとは新画面を見るまで認定しない。

独立レビューで、delay/backwards fill中のstep-start誤適用と、通常shadow要素で未解決keyframes名が最寄りhost treeへfallbackしない2欠陥を修理した。自scopeで解決できる名前をancestorで上書きせず、無関係shadowの全走査も追加しない。新しい反証を含めて現在94件（native84＋JS10）。:host／::slotted／明示inheritの宣言root保持、複数trackやtransform/events等は未完成であり、完全tree-scoped適合とは呼ばない。

初回 `parallel-css-opacity-reviewed-verified-20261010` はCSS94件中34失敗（JS10/1）、新clamp predicate25件は全成功で、記録を保存した。native fixtureは非live `web_parse` 文書に後からstyle/adopted sheetを追加していたため、native `doc_rescan` がresource出版を行わない境界と不一致だった。実browser同等のweb_liveではopacity等のJS9件が成功しており、残るJS1件は未提供transform getterのfixture期待だった。失敗33opacity期待と新unsupported1だけをweb_live準備済み経路へ分離し、成功60項目やdiagnostic25を反復しない。非live全resource取得を根拠なく有効化する本番変更は行わない。

`parallel-reddit-clamp-predicate-diagnostic-20261010` の新実候補はgate1／eligible1／lines6だがapplied0、full=cut=post=final872.63pxだった。高さ指定はauto（usedh/sh=-1）、legacy/orientも成立していたので、CSS predicate不成立や作者固定高さという仮説は退ける。scanで予算6へ到達した後の切断状態と再レイアウト状態を、次の一般修理境界として扱う。新観察は46.255秒でobserver到達、126.54秒で終了・VM停止、媒体initrd073a35e842ede0c08ba27901062a7ea70fb8a5c0d91354f227451f28985fd909。harness成功は新数値出版の成功だけであり、実画面の大きな空白・lazy左ナビは未合格のまま。

具体根因は、layout_doc冒頭のvisibility resetが一度だけなのに、flex/gridの測定→最終stretchが同一pass中にclamp subtreeへ再入することだった。full runs／nlines／heightは再生成されても旧clamp_hiddenが残り、scanがその末尾をskipしてused==limit／truncated==falseとなり、縮約とellipsisを飛ばしていた。active clampの新layout開始だけ専有subtreeの旧visibilityをresetし、既存clamp_depthで子・nested BFCを再layoutする最小一般修理を保存した。非clampのAP/cacheを変更せず、paint変更やサイト定数もない。新lineclampreentry60件へ分離し、成功済み83／35／25は呼び直さない。

`parallel-css-clamp-reentry-verified-20261010` の初回新60件は全成功。CSS失敗専用34件は33成功・1失敗（実opacity raster pixel）。元94成功60／新predicate25／旧35/83は反復しなかった。opacity補間・時刻・scope等の33成功を残し、画素の位置・背景・本番paintを別の失敗専用境界へ分離する。

lazy左navの追加仮説「zero-area targetを必ず非交差にするIntersectionObserver」は現native/jsと一次仕様を読んで反証された。端接触を含む交差とarea0時の比率1は既に実装済みであり、根拠のない補正や成功済み検査の再実行は行わない。box生成／root所属／overflow clipの実状態は未確定として残す。

修理後の新実Reddit `parallel-reddit-clamp-reentry-live-20261010` では、eligible/appliedとも1、full872.63→cut/post/final138.05px、親142.05px、cb_reached1になった。同じ本文長2424の摘要rectは高さ876→142px、投稿1026→291px。実screen-finalでは巨大空白が消え、評価ボタンと次投稿が通常flowへ戻った。第二摘要150px／第三74px。これは再入の空白境界の改善であり、空のlazy左navや全詳細一致は未解決。媒体initrd7e70541bed6fe3f7b40c6ba094f4a6744655b5fbfa57a01d7a0ba297cafb854b、observer45.856秒到達、VMは正常終了・停止。後続gfx_mix算術修理を含む最終媒体とは区別する。

CSS新34の唯一画素失敗は一般 `gfx_mix` の算術欠陥だった。重み総和255なのに除256しており、白と赤をopacity0.5で合成しても不変R255が254へ暗くなり、端点・同色も保存しなかった。共通1関数を各channel独立のround-to-nearest除255へ修理し、旧R255期待は保持、G/Bもより精密にした。新cssanimationpixelは旧画素失敗1＋直結する端点/同色3の4件だけ。旧CSS成功33、clamp60、元34入口は再実行しない。GUI/ブラウザ共通の一般修理であり、サイトだけの色補正ではない。

新専用 `parallel-css-opacity-pixel-verified-20261010` は初回4/0。実target rect[0,0,20,20]、style/box opacity0.5、style/box背景ffff0000、canvas背景ffffffff、actual pixelffff7f7f。不変端点はweight0=ffffffff、weight255=ffff0000、same128=ffff0000で厳密一致した。実WHPX4CPU／4GiB／1920×1080x32／userdataなし、65秒で終了・停止。元94/34失敗と新34/1失敗は保全し、成功した全検査の再走や元94全合格への読み替えはしない。

### 最終画像の指定記事と配布境界

`parallel-openai-bitmap-lineclamp-live-20261010` は実WHPX4CPU／4GiB／1920×1080、既存userdataなしで61.307秒にarticleへ到達した。親のscreen-0直接確認では主heroは黒いまま。新SVG寸法記録はviewBoxと実CSSの両軸サイズを採取したが、それを可視表示成功としない。AstraHero SVGのopacity0は形状用の作者ruleにanimation指定がない入力もあり、foreign SVG motion除外仮説は現ソースで反証されたため、作者opacityを無理に上書きしない。

その後 `418jhr9--e_bu.js` の実HTTP403を2回とChunkLoadErrorを記録し、記事はエラー画面へ置換された。親のpage-1／screen-finalはそのエラー画面だった。これは別iframeのWebGL非対応エラーと区別する。最後までJP本文／動画／グラフ／trusted chart clickを受入れ済みとは言えず、同条件の再検査やchallenge回避を行わない。新画像はkernel4d115f411a875a7c407680724a31c6b0304e11afebbd2294b943e73ee03baac3、initrd5978336b4d3d8a52073e267cc467c59b7c83e116c78c445012be13c1a03b94ee、271.7秒でguest exit0・VM停止／harness不合格。以前の画像でのJP本文可視証拠を、この最終画像へ移し替えない。

同じ最終画像のHyper-V用 `build/nocturne.vhdx` は、一度のread-only検証でVHDX形式／raw論理一致／active MBR FAT32-LBA／kernel・initrd・UEFI・boot設定の一致を確認した。結果は `build/parallel-gradient-hover-vhdx-verify-20261010/result.json`。Hyper-V上の実bootは未検証、既存VM／userdataは未変更。配布可能な生成物の検証と、指定サイトの受入れ・7項目全体の無条件完成は別の判断である。

新媒体2afeによる実Reddit観察 `parallel-reddit-clamp-height-diagnostic-20261010` は46.701秒でobserverへ到達、126.99秒で終了・VM停止。冒頭は6行だけpaintされたが、摘要876px／投稿1026pxの空白とlazy左ナビ子0は残った。初版の適用済みだけのnative clamp診断は一件も出版せず、harnessはその欠落だけで不合格だった。これを高さ根因の確定とはせず、debug限定・私有arena・join後・文書最大4件のまま未適用候補とeligible／legacy／orientation／行記録を観察できる一般修理を加えた。新25件は旧35／83を反復しない別境界である。

