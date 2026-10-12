# HTML解析改善：実装と検証記録

## 最新統合の状態

- **ユーザー指示で停止。** 最後の26群は23合格／3失敗／skip0、実QEMU0台。停止前に保存されたdeadline追加修理は未build・未受入。最後の媒体のAmazon画像とReddit tooltip位置は主担当目視で改善、本menu／取消回復等は未達。段階別履歴の「実行中」「最新」を現在状態と誤読しない。最終状態・失敗・媒体・片付けは`docs/html-parser-20261011-stop.md`を参照。
- filter-geometryの初回Amazon／Reddit QAは主担当の誤った`/data/bin/browser`指定で起動に達せず、serialのcommand not found後に終了した。製品クラッシュではない。専用媒体とログを区別して保全し、実在する`browser`指定へ訂正した別ラベルのQAを開始。nativeでは新しいShadow filter固定CBの4条件が失敗し、期待を弱めず担当へ原因修理を戻した。
- 最新の`build-filter-geometry.log`正規embed/all成功。IMG/VHD/VHDX内容同一、ISO内kernel/initrd・initrd内browser/worker/webfetch/libcのSHA一致を主担当が確認。initrd=`057bdaa43c0157ce9a55096804ed0f2755315edf4a2ef55388860a350c7fdd57`、browser=`e1329e73a0f8d5e77b97f6ea4143659716fa300ecf4f172ee9d39f22a25a3131`。この媒体の影響範囲26群、Amazon／Redditを4GiB×3台で受入開始。変更していないHTTP／cookie／worker経路は直前29群の結果を保持し、基準worker6失敗を除外して全体合格にはしない。
- filterの実関数列・計算値／used clamp、HTML subtree／inline／atomic SVG rootの一括合成、overflowとwrapped padding CB、小数DOMRect／clientを統合。独立レビューのwrapped CB漏れ・層配列OOM abortも局所修理。実alpha文書だけviewport transactionにし、取消で旧frameへ戻す。noneの追加全走査はない。内部SVG group・全製品のOOM注入保証・厳密な同負荷性能比較は未達／未確認であり、filter全面対応とはしない。
- adoption-cssom媒体の29群は終了し、24合格／5失敗／skip0（guest727秒）。新CSSOM／flex配置／長さASTの回帰は合格。失敗は旧SVGG prototype期待1、追加CE試験の非公開Element.customElementRegistry参照2（全JSにも重複）、基準からのbudget9、旧hover=false期待1、基準worker6。元失敗を保全し、SVGG継承・公開CE動作のfixtureへ訂正。hoverとbudgetの実責務も調査中であり、失敗を除外して全体合格にはしない。
- 同媒体Amazonの実検索・指定3商品bitmap・上下・戻る／F5を主担当目視。Redditは上下とF5が成立するが左端popupと空menuが残る。読取geometry-probeはclient／realm正常、作者のcomposed祖先探索がcomputed.filterの空値をnoneと誤判定する境界を支持した。次の共通修理は本物のfilter none／opacity()解析・group合成・absolute/fixed CBとCSSOM接続、小数DOMRect／principal clientであり、サイト専用補正や固定noneは追加しない。
- 同媒体OpenAIは実Esc後、複数PageDownで日本語本文／図へ到達し、PageUpで先頭本文へ戻ることを主担当がscreen15／16で確認。最終F5画面とconsoleも目視。WebGL黒hero、図の欠字・配置、資源403は残り、サイト完全表示や性能回帰解消とはしない。
- adoption-cssomのAmazon／Reddit、geometry-probe、OpenAI、native29群は停止済み・永続data未接続・実非使用・build内を確認し、専用boot/data合計10個を回収。画面／log／metadataと配布・基準媒体は保全。回収後D空き136.10GiB。古い430個の一括回収拒否は迂回せず保全した。
- `build-adoption-cssom.log`の正規embed/all成功。文書間adoption、SVGGElement、実CSSOM、flex絶対配置／used auto margin、取消可能な計算長さserializationを統合。IMG/VHD/VHDX内容比較とISO/initrd内部6payloadのSHA一致をroot照合。initrd=`5bc316ca3f7cfcae2eca22354709aa1f3af086cea894db43a1a37bd56ae40995`、browser=`e7602dc5bada636d6414492bbebbf7b8bf945f166c8e3e7325a5597388fff1e4`。以下float-flat-tree欄は前段の媒体・経過であり、この統合の合格へ流用しない。
- この統合はselector修理後のnative29群とAmazon／Reddit実サイトを4GiB×3台で受入中。rootは両サイト初期・console画面を目視し、Amazon実検索と先頭書籍／映画bitmapを確認。Redditは実click後も左端tooltipと空メニューが残る。新観測では作者inline自体が負leftなのでlayout固定補正はせず、geometry／CSSOM境界を再調査。SVG回帰一条件は旧exact SVGGraphicsElement prototype期待が残っており、SVGGElement＋継承の現行契約へ別途訂正中。今回のrunは元失敗を保全する。
- `build-float-flat-tree.log`の正規embed/all成功。IMG/VHD/VHDXは実内容同一、ISO内kernel/initrdとinitrd内browser/browserjsworker/webfetch/libc.aのSHA一致を主担当が照合。initrd=`a66fdc9721a69c11799bca11b142940446ba1b0a64d88313b7f1a184026e0c75`、browser=`4b025e3d1c7246fecee94a2d389d6b1a6142f5d7af68d926b88773a23d803fe0`。この媒体のnative関連群を`final-float-flat-tree-tests`で実行中。今回CE/XHR/frame/CSS/geometry修理への旧PASS流用はしない。
- 同媒体Amazon検索`html-plan-amazon-final-integrated-20261011`のscreen-7（console付き）/10（下）/12（上）/final（戻るとF5後ホーム）を主担当が目視。実8文字nocturne検索、指定書籍と2026/2020映画bitmap、PageDown899→PageUp0、戻る、再読込成立。HTTP400未検出。ホーム`html-plan-amazon-final-sizing-20261011`screen-7/finalも目視し、従来298×0だった自然855×1368画像が298×477へ修復、白い画像領域は解消。carousel全体・下のカード余白まで完全一致とはしない。
- 同媒体Reddit`html-plan-reddit-final-flat-tree-20261011`screen-7/9/11/finalを主担当が目視。feed/画像、trusted menuクリック、下899→上0、F5成立。offsetParentはShadowを越えてheaderへ接続したがpopup[-89,53]、左端tooltipとメニュー本体欠落は未解消。下端の次ページ読込エラーも残る。consoleはtemplate内容のfaceplate-partialをclone→挿入した直後のr.load()非関数を示し、公開JSの作成式を読取確認。CSSOM実insetの未公開とCE挿入時upgrade境界を共通製品修理として担当へ戻した。
- 上記3VM（PID21824/37824/23428）は停止済み・永続data未接続・build内・実非使用を確認し、各専用boot/data計6ファイルのみ非再帰回収。画面・ログ・metadata・配布/基準媒体は保全。旧430回収拒否を迂回していない。native＋co＋百度は4GiB×3台の資源内バッチで実行中。
- float-flat-tree媒体coはscreen-7（console）/9（下）/10（上）/13（about）/final（戻る・F5後採点）を主担当目視。338/588、DOCTYPE/tree/SVG/MathMLはYes、tokenizer Noと2/5残存、browser終了なし。百度もscreen-7（console）/10（下本文）/12（上書影）/final（戻る・F5ホーム）を主担当目視し、全8文字検索・上下・履歴/再読込成立。研究データAxios通信失敗、skeleton、固定ヘッダーのbutton折返し等は残る。終了PID12840/16532の専用boot/data4ファイルだけ、停止/永続data未接続/リンクなし/実非使用を確認して回収。co/百度の旧画像へ新sourceの成功を流用しない。
- 同媒体全JSでは新CE2/XHR4/frame4の失敗が解消し、新geometry9も失敗なし。残るDOM6はTLA前のDCLで未定義名を参照した連鎖失敗、latewrite一律拒否期待、SVG gの旧prototype期待を現行一次仕様と分けて確認。TLA待ちを製品へ追加せず、元FAIL理由を保全し意味を分離する試験訂正と、SVGGElement/native geometryの本接続を進める。budget9とworker6の基準由来期待は別で、合格偽装・除外をしない。
- `final-float-flat-tree-tests`は19合格/1失敗/skip0（guest532秒、host575秒）。全JS1194条件/15失敗。予定全群より少ない理由をrootがソースで確認：runtestsがselectorを16個まで黙って受け、後方の画像寿命・metadata・通信・Cookie・script-lifecycleを未実行にしていた。未実行を成功扱いせず、selector保存をargcに応じて確保し未知名を明示失敗にする補助修理を投入。次媒体の統合試験では指定全群を数える。元実行ログと未実行の事実を保全。
- float-flat-tree媒体comのscreen-5（console）/9（実Endで最下部）/10・13（about）/final（戻る・F5採点）を主担当目視。319/555、支払等の不足項目を読み、採点だけで仕様対応を主張しない。OpenAIはscreen-3未装飾→6黒hero＋console→9実下入力の日本語本文→12Esc後黒維持→13下入力の黒中段→final F5黒heroを主担当目視。取消後の下入力では本文まで届いておらず、取消後の全文受入は未確認。WebGL contextとchallenge資源失敗は未解消、次の統合では複数PageDownで本文まで確認する。終了PID36556/36448/8060の専用媒体6個だけ実QEMUなし/build内/リンクなしを確認して回収、PNG/log/metadataと配布/基準媒体は保全。
- 現行WHATWGを優先し、旧HTML5testのtokenizer判定に合わせる退行・採点偽装はしない（ユーザー再確認済み）。以下の経過欄の「最新」は各段階の媒体を指す。最終受入はこの欄で区別する。
- `build-recovery.log`媒体の関連guestは18合格/2失敗、parserdedupは78条件成功。新しい失敗は外部fragment targetへのDSDをowner resolverが欠損bindingと誤認した偽OOM。合法なexternal hostの実ownerを境界にする最小製品修理を投入し、内部hostの最終owner依存・循環検査は保持した。
- もう一つの既存失敗はShadow CSS容量の計数不備。snapshotに加え、所有者が異なる外部cached AST arenaを一度ずつ実計数するよう修正した。元の16MiB超/32MiB以内、実scoped styling、期待件数は変更しない。製品へ無駄なAST複製を追加しない。
- `build-final.log`段階の正規allは成功。対応媒体のguest20群は20合格/0失敗/0skip（guest495秒、host550秒、FAT276files正常）。parserdedup78条件、tree1014条件、HTML入力67条件、encoding60条件、選択corpus610/610、JS安全API58条件は全成功。宣言的Shadow DOMの偽OOM修理後の既存安全APIも成功。Shadow CSSは外部arena込み実AST 30,035,720bytes・snapshot 65,512bytes・sheets2・errors0、元16～32MiB条件を満たして61条件全成功。別実行のscript lifecycle/image promiseも2合格/0失敗/0skip。parse errorの全仕様照合や全WHATWG対応を証明する数字ではなく、以下の追加修理へ結果を流用しない。
- Amazonのユーザー画像と同じ検索結果の3商品は、recovery媒体で実bitmapを主担当も確認したが、最終媒体ではaltだけの欠落が再現した。主担当がscreen-7/8/9を目視。readonly observerのcurrentSrcに一致する書籍・映画の3画像は、serialのResource #20/#21/#22でHTTP 400・122bytes・text/html、complete=true/natural=0だった。取得失敗を確定した状態であり、lazy待ちやJPEG復号失敗と決めつけない。一般的なHTTP要求境界を調査中。video/audio専用のmedia paint診断は商品imgの証拠にしない。observerのsrcset候補要約はURL内カンマを分割する診断上の限界があり、製品candidate選択の根拠にはcurrentSrcと実資源URLを使う。
- OpenAIのobserver無し切分では記事本文へ到達した後、Escより前にJSON解析失敗とchunk HTTP403によるサイトの読込エラー画面へ変化。F5後は本文があるが未装飾。取消回復成功やHTML解析回帰の証拠へ混同せず、サイト受入は未達。Challenge回避や代替HTML注入はしない。
- 同じ旧`build-final.log`媒体の六サイト受入を終了。com/coの採点・about実クリック・戻る・F5・上下移動、百度/Amazonのnocturne全8文字実入力・検索結果・上下移動・戻る・F5を主担当目視。Redditはfeedと上下移動/F5あり、menu項目・次ページと重なりは未達。OpenAIは黒heroのEsc取消後も白化せず、screen-13で下方向入力後の日本語本文を主担当自身が確認、上方向・F5も応答。作者停止、WebGL/font欠落とサイト資源失敗は残り、全面サイト合格や性能改善とはしない。
- ユーザーの最新許可でDOM/CSSの共通不足も対象へ追加。現行DOMのclone/import fallback registry規則を既存native cloneへ接続し、template内容のimport時upgrade、scoped identity・遅延define、template/clonable ShadowRootへのfallback非伝播、明示nullを保全。Reddit popupへの因果・実改善は新媒体で未確認。
- CSSの画像/Canvasを含む自然比付きreplaced elementのmin制約転送と、通常生成された匿名blockのpercentage height参照透過を既存layout.cで修理。実decoded PNGの寸法・flow・paint回帰を追加。Amazon白領域の直接原因とはまだ断定しない。
- Amazonの拒否境界へ、debug時だけ要求header総量・不正制御文字数・非ASCII数・Cookie/Content-Length有無の数値診断を既存worker/private wireへ接続。任意値・認証情報・本文は出さず、native callback前に特権診断行を除去する。単独native要求の200はブラウザー画像解消の証拠ではない。既存worker-network試験で本文/header/Cookie offset保全を追加。
- 正規embed再生成後`build-dom-css-final.log` all成功。新媒体の関連guest12selectorとAmazon/Reddit実画面受入を開始。初回宣言観測の起動は検証コマンドの未対応`--console`で失敗したため製品不具合に数えず、現行browser引数をソースで照合して訂正。旧結果と新変更の受入を混ぜない。
- 同媒体のguestは16群中12合格/4失敗。新しい失敗はscoped registryの遅延define二条件と自然比寸法の8条件。inactive helper documentへのclone/import登録漏れとfloat丸めを製品側で修理し、期待値を弱めず次媒体で確認する。全JSの21失敗のうち19条件とworker-networkの6条件は、保存したHEADのソース・boot・payload hashを厳密照合した基準試験でも同じ失敗だった（engine基準190条件成功）。基準由来の失敗も隠さず、今回の修理で全JS/通信が完全対応したとはしない。
- 主担当は新媒体Amazon検索のscreen-7/8を目視：書籍画像は表示されたが映画画像はaltのまま。実8文字検索・上下移動・戻る・F5は成立。数値通信診断ではheader92bytes/CTL0/非ASCII0/Cookieなし/CLありで200と400が混在し、直接原因HTTP400の理由は未確定。Amazonホームはdecoded画像855×1368に対し表示298×0、白領域が残る。CSSOMのstyle/group読み取り不足で宣言観測が空だったため、仕様宣言なしとは判断しない。
- Reddit新媒体の実画面とconsoleでrpl-popperの実constructor/reposition/popup生成を確認し、以前のnot-a-functionは未検出。ただし実メニュー項目は未表示で未達。観測補助の本文focus候補がリンクだったため投稿へ遷移した事実を記録し、focus成功へ誤分類しないよう実hitが対話要素なら選ばない補助修理を投入した。
- Fetch仕様のnull本文と空の実本文を区別する意味をJS→native→wire→HTTPへ接続。null GET/HEAD/PATCH/DELETE/OPTIONSは余計なCL0を生成せず、POST/PUTのnull CL0、空の実本文、303破棄/307保持を保全。既存回帰へHTTP31・worker10・JS10条件を追加。debug時の画像高さ0診断も既存layout責務へ接続し、画像更新後の一回だけ走査する。`build-body-registry.log`の正規embed/allを開始、これらの受入はまだ未確認。
- `build-body-registry.log` all成功。Amazon実検索screen-7で指定書籍と2026/2020映画のbitmapを主担当が確認。同currentSrcのnatural寸法が正値になり、数値要求200が75件/204が2件、400は未検出、余計なCLが無くなった。商品画像欠落の改善を確認したがホーム白領域は別不具合。今回の上下キーはコンソールが再度開いた状態に送られたため、ページスクロールの合格にしない。次媒体ではコンソール付き撮影後に一度だけ閉じて実ページへ送る。
- 同媒体nativeは13合格/3失敗。画像・CSS自然比8失敗とHTTP/workerの追加本文条件は解消したが、CEのinert target初期registry漏れ2条件と、XHR producerがnullを空本文へ変える4条件が新たに失敗。両方を既存責務へ修理中。全JS25失敗は基準19＋この6で、worker-network6失敗は基準同一。期待値・失敗の除外はしない。
- Amazonホームのnative寸法traceは、ULのdefinite477.511pxが浮動LIの呼出しでcbh=-1へ捨てられ、block-in-inline代理spanでも参照が遮断されることを示した。place_floatの全consumerへcbhを接続し、inline代理箱は高さ宣言を適用せず参照を透過する一般修理と28条件を追加。frame正規TT timer wrapperの確定時点もbootstrapの作者実行前へ修理（基準frame4失敗の原因、19件全部とはしない）。次allで受入する。
- Redditの本文focusクリックは非リンクIMGにも作者delegateがあり、コメント/lightboxへ遷移した。補助からそのクリックを除いたmenu-only実行ではr/codex上のmenu実クリックを確認、主担当screen-6/mouse-click-2を目視。tooltipが画面左端に出る一方、menu本体は未表示。anchor[1852,5,44,45]に対しpopup[-89,53,133,32]。Shadow/flat-tree offsetParent不足を仕様とソースで調べ、推測の原点補正は製品へ入れない。
- 旧履歴を含む430一時ファイルの広い回収は自動審査に拒否されたため実行しなかった。安全な限定対象として、この実行で起動・終了を記録したPID37208/28892/31952のboot/data計6個だけ、metadata停止/永続data未接続・build内・リンクなし・実非使用を確認して非再帰回収した。PNG/PPM/log/metadata・旧媒体・基準/配布媒体を保全。D実空き約137.85GiB、古い回収候補は残置。

## 作業契約

- J-spaceはmedium、協調作業。既存のhighコントローラー履歴は書き換えず、この短い記録と既存semantic mapで境界・証拠・残件を管理する。
- 目標：html5test.coのbrowser終了を共通製品経路で修理し、文書モード、fragment/script、同期write、解析応答性と寿命を改善する。
- 現行WHATWG優先。既存PI、Lexbor/native DOM、Shadow DOM、CE、CSP/Trusted Typesを保全。サイト専用分岐と採点偽装は禁止。
- 受入：html5test.com/co、百度百科、Amazon、指定OpenAI日本語記事、Reddit r/codex。実QEMU本文・console/log・実入力・上下scroll・遷移を主担当が確認する。
- 基準sourceはHEAD 467f311、開始時tracked差分なし。実サイト基準媒体とsourceの同一性は別途確認する。

## 担当と共有契約

- root：実QEMU、console/log、クラッシュ診断、製品統合、build/媒体、実サイト受入、最終差分と担当成果の独立確認。
- parser：html_lexbor.c/.h、webi.hの解析契約、文書モード/srcdoc、協調yield/取消、fragment context。共有結果/モード契約は変更前に通知。
- scripts：js.c、frame_native.h、js_html_safety_host.hと対応する既存JS公開入口。fragment実行規則、stylesheet待ち、stream/世代寿命。
- bridge：html_lexbor_bridge.cと必要なLexbor DOM内部変更追跡。identity/所有権/名前空間/差分同期を保全。追加bridgeは作らない。
- 生成物とMakefileの統合はroot。担当はそれぞれ既存の関連試験へ回帰条件を加え、小さい報告を編集ツールで残す。

## 基準観測と原因

- 空きRAM約20.3GB、Dドライブ空き約169.5GB。全6VMを同時に24GiBで起動せず、独立VMを資源内でバッチ化する。
- 起動中QEMUなし。最初のcom/coハーネスはMSYS2名前付きobject権限で起動前に失敗した。生成したpartial媒体を保全対象と区別し、許可された実行へ切り替える。
- 6対象を資源内の独立VMバッチで起動。主担当がconsole付き実画面と上下PageDown/PageUp後の実画面を開いて確認。comは319/555、coはbrowserのDivide errorで終了。百度は本文・画像・上下scroll、Amazonは主要本文が空白、OpenAIはhero黒で下の本文あり、Redditはfeedあり・上部overlay不整合と次ページerror。ハーネス終了成功を実サイト受入にしない。
- coの実媒体browserの.textはstripなし再リンク版とSHA256一致。RIP 0x43bf54はjebp__read_vp8l_imageの行幅division。サイトの有効な1x1透明palette WebPがround-to-nearestで幅0になっていた。一般WebPデコーダーのceil幅・alpha累積・読取失敗を修理。サイト分岐なし。
- 開始HEADのtests/header snapshotと旧bootを組合せた隔離guest試験は10group合格、0失敗（298秒）。currentscript、waitingstream、parserdedup、htmlparser5、htmlworker、htmlcsp。`lexbor`はgroup名ではなく実行されていなかったため、正規の`web/html-tree-builder`等9関連groupを別途開始した。

## 製品変更と進行

- 解析：DOCTYPE/srcdocモード保持、128KiBまたは16msの協調yield、cancelとEOF区別、scripting4モード、target/context分離、閉鎖tokenでfragment script候補を確定。streamとsandboxの有効判定を統一。
- DOM同期：Lexbor低層/native変更追跡とsubscription、差分worklist、fields準備後のcoherent commit、null namespace保持。doc.cでparser終了をhelper doc解放前へ移し、allocation-owned nodeの退役通知を接続。
- script：fragment async/defer規則、stylesheet待ち、document.openの旧script/要求世代取消、完了イベントと再入世代の整合。
- 正規全build開始。既存CRLFファイルはHEADもCRLFのため`git -c core.whitespace=cr-at-eol diff --check`で確認した。担当構文成功は実ゲスト成功ではない。
- 最初のallは成功。一次媒体com/coのconsole付き実画面を主担当が目視し、coの338/588採点完了・窓生存、両サイトDOCTYPE/SVG/MathML/treeのYesを確認。trusted hover/実mouse clickでabout本文へ到達、Alt+Leftで採点へ戻り、F5後の再採点も目視。tokenizer Noと2/5表示は残る（現行PIを古いcomment期待へ偽装しない）。一次媒体の結果を後続変更へ流用しない。
- 追加reviewでtarget/contextのtemplate owner・ShadowRoot registry・文脈文書のscriptingを修理。Trusted Types default policy/準備microtask中のdocument.openによる未公開script世代漏れも修理し、既存公開入口・回帰へ接続。正規JS埋込み再生成を含む二回目allは成功。最新媒体の実サイトとnative試験を開始。
- 旧DOM関連基準は7合格/2失敗。開始時からlexbortestのDOCTYPE/fragment-mode2条件とshadow renderのAST容量1条件が失敗。期待値は変更・除外せず、最新版の結果を比較する。

## 未完了

- 最新媒体com/co：console付きscreen-5、about本文screen-13、戻る後screen-15、F5後screen-finalを主担当が目視。co338/588・com319/555、採点・窓生存・実mouse遷移・戻る・再読込成立。DOCTYPE/tree/SVG/MathMLはYes、tokenizer Noと2/5表示は残る。
- 最新関連guest20群は18合格/2失敗。parserdedupはsrcdocを通常文書として期待していたfixture不備を分離し、43件0失敗。初回の件数文字列37のハーネス不備も43へ厳密訂正し、単独再実行1群合格。shadow renderは基準から同じ一条件失敗で、外部CSS ASTの所有先を見ていない古いarena計数が原因候補。期待弱化/skipなし、容量条件の受入は未達のまま。
- 最新tree1013条件、入力67条件、encoding60条件、corpus610、JS解析・安全API、worker/CSP、属性/Shadow native/semantic/form/image/metadataの製品経路は合格。script lifecycleと画像promise寿命の既存2群は追加確認中。
- 正規all二回目成功。IMG/VHD/VHDXのqemu-img compare、ISO内kernel/initrd、initrd内browser/worker/libcのSHA一致を確認。kernel=07e2138949e4efa41023080b35aa84044ba323c5daffcb1007378f7e6bbaeb46、initrd=6784e47654f7422e1e6c84c434ef53098fee3d2a771ea4075b9ebb5ae2d46054。
- 百度/Amazonの最新版本文・console・上下scrollは主担当目視済み。Amazon実検索で結果/価格へ到達したが先頭キーが欠けocturneになり、検索結果の画像は欠落。百度の観測補助が検索欄を選ばず検索は未確認。OpenAI/Redditの最新版は確認中。
- 同条件の厳密な性能比較とHyper-V実機は未確認。並行native試験を含むhost負荷差を速度改善の根拠にしない。
- doc_mutated(NULL)/doc_sync_treeの全走査と単一巨大token内部の中断には残存境界がある。差分同期導入だけで全面高速化・無回帰・完全取消対応としない。
- script lifecycleと画像promiseは追加2群合格。OpenAIは黒heroからEscで白bodyになり、F5で黒hero/headerに復旧。取消が作者停止と描画永久停止を共用する既存経路が今回の取消接続でも問題になるため、主担当目視後に外側transactionでの安全な描画回復を修理中。白化を合格にはしない。
- Redditのconsole/feed/images、実trusted hover/menuクリック、上下scrollとF5後feedを主担当目視。menuはクリック/aria-expandedまでログがあるが期待する項目の表示が見えず未達、上部overlayと次ページerrorも残る。
- 終了済みの本作業専用出力だけを解決済みbuild内・リンク無し・metadata process_stoppedで確認し、270個（論理約11.82GiB）のboot/data・展開payload・重複PPMを回収。基準bootはbaseline-tests/test-boot.imgを1つ保全。PNG/log/metadata/source snapshot/クラッシュ逆引きELF/最新配布媒体とユーザー永続データは保全。検索ackの実行中2VMは対象外。D空きは約144.68→156.43GiB。削除済みの個別試験媒体を保存済み・そのまま再実行可とは扱わない。
- 検索ack修正後の百度/Amazonは両VM終了。実trusted clickの到達確認後にnocturne全８文字をPS/2入力し、検索結果へ遷移。主担当は百度のscreen-9下部本文、screen-10上部本文と画像、screen-11戻る、screen-final再読込ホーム画像＋consoleを目視。Amazonのscreen-9下部結果、screen-10上部結果、screen-11戻る、screen-finalホーム＋consoleも目視。検索・上下移動・戻る・再読込は成立、Amazonホーム空白と画像alt表示は未解消。先頭キー欠落は製品改善ではなく、検証補助のfocus確認不足を訂正した結果。
- 独立再レビューで、parser由来coherent commitを他parser subscriptionへ通知しない漏れと、DOMContentLoaded/load前の作者再入・動的script走査中TT再入の世代漏れを発見。既存の各責務内で修理・回帰を追加中。取消回復も含めた全員の編集終了後、正規buildと最新媒体で再受入する。
