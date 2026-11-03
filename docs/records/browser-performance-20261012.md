# ブラウザー性能改善の実装・確認結果（残件あり）

## 条件と実装

- 基準開始時の作業木は差分なし。三担当と主担当で既存責務に統合し、サイト専用処理・DOM置換・POSIX再設計を入れていない。
- MSE VIDEOは共有ページ二重バッファ＋136byte通知。実PTS/DMA起床、SSE2双線形、同寸法動画の部分present／damage fallback。
- computed style本体は文書slab、可変tailのみ遅延arena。inline／WAAPIの宣言ASTは内容とbaseで失効する。
- prepare／intrinsic／relative／overflowは安全な無変更枝を再用し、overflowと文書最下端を統合集計。文字計測と共有fontchoice値キーは文書寿命で回収する。
- 描画順はbox所有の安定merge sort cache。背景・border primitiveをclip外で省略し、hit primitiveのpaint順を保つBVHを文書所有する。dialog／fullscreenの強制targetは従来walkを安全fallbackにする。
- 選択／媒体の既知atomic damageをunionしclip描画＋部分present。未知damage／layout／寸法変更は全文書fallback。
- 通信は4固定からCPU／空き物理メモリに応じた最大32slotへ。CPU側は `max(8,4*online)`、物理メモリ側は64MiB毎のreserveで制御する。初回request origin毎6接続、Fetchは5までとし文書依存の枠を保つ（redirect先の実socket総数を6にする保証ではない）。低メモリの1slotでも混在classが公平に進む。parkedを含む実生存PIDを数え、予算縮小後はdrainだけにする。dispatchの待ち時間診断を追加。

## 基準の実観測

WHPX／4CPU／4096MiB／1920×1080／最大化／録音有りの独立二VMでYouTube `jNQXAC9IVRw` とShaka demoを並行観察した。主担当が各原画像を見た。

- 最初のハーネスに途中実操作を送れなかったため、既存platform_qaに単一monitor所有のnative操作受付を追加。最初の二VMは終了し、同じ媒体の操作可能な基準二VMへ切替。これは試験補助の不備の訂正であり、製品改善ではない。
- YouTubeは本文／実動画を表示し、自動次動画へ遷移した。PageDownで下の内容へ、PageUpで上へ戻る実画面を見た。遷移後も旧title／channelが残る不整合、長いnative処理、starvationを基準の失敗として保持。
- Shakaは実asset一覧を表示。最初の位置誤認でPRELOADを押し、配置が変わったため古い座標を再クリックせず二度hoverを撮って修正した。最後にPLAYのhoverを主担当が見てクリックし、実HLS playerを表示。player内Playも別hoverで確認した。再生結果は下の追記に分離する。
- 基準画像・ログは `build/nocturne-platform/browser-perf-base-native-{youtube,shaka}-20261012/`。最初の二VMは `browser-perf-baseline-*`。後で不要媒体を回収し、小さい証拠だけ残す。

## 確認の境界

正規全build `build/browser-performance-final-build-20261012.log` は終了0でIMG／VHD／VHDX／ISOを更新し、対象objectが対応sourceより新しいことを照合した。これは実サイトやHyper-Vの合格ではない。

担当の限定C／構文確認は各担当の第一報・第二報に記載。主担当の独立source reviewでは共有map予約／FD再利用identity／巨大dest算術、fontchoice重複を第二考察へ戻して修理した。旧snapshotのhost scaler時間を最終製品速度へ流用しない。

次は統合guestの寿命／hit順／damage／style／layout／媒体／通信と、最新媒体の同条件二サイト実操作。失敗を除外せず、原因修理後に変更した境界のみ再確認する。

## 第一統合guestと発見した失敗

`browser-performance-guest-20261012` は14成功／2失敗。媒体6経路、native取消2経路、style150項目、damage／hit14項目、共有ページ25項目、translate、observer、alpha pixelは成功。これはその時の媒体に対する証拠であり、後のresize flow修理は新しい13境界と実サイトへ分ける。

- 自分の通信回帰：origin計数のためのHTTP URL解析失敗をリクエスト拒否へ誤接続し、既存のローカルmodule・安全拒否・意図あるlocal navigationの非同期完了を壊した。origin計数とworkerの受付／intent／error policyを分離して製品を修理した。
- CSS animation試験の2失敗は、以前追加済みのtranslationを「未対応」としcomputed transformを空文字と期待した古いassert。現在のtranslation実装と既に通ったtranslate実経路に合わせ、translation支持＋rotation非支持、computed `none` を確認するよう訂正した。今回のpool変更を理由に未対応のrotation等まで支持宣言していない。
- 通信試験の旧URL16384byte・body16MiB・header64KiB・30秒deadline拒否は、開始時からのowned URL／dynamic body／dynamic headers／32bit期限契約と相反する。元の失敗を上書きせず、完全な16384byte URLの実配送、16MiB超payload全byte一致、35秒の正当なresponse完了を確認へ訂正。大きいheaderの末尾へ不正Content-Lengthを加える新負例で、全文拒否と先行Set-Cookieのrollbackを引き続き厳しく確認する。

期待をskip／除外して成功にせず、製品の回帰と旧fixtureの不整合をそれぞれ修理した。再確認はこの失敗した二境界だけで行う。

## 修理後の限定guestと独立レビュー

- `browser-performance-repaired-guest-20261012`：上の失敗したCSS animation／network二群だけを修理後媒体で確認し2成功／0失敗。16384byte URL、16MiB超全byte、35秒応答、不正末尾headerとCookie rollbackも成功。後の通信admission／callback修理の証拠とは区別する。
- 最新の `browser-performance-reviewed-build-20261012.log` は正規all終了0。IMG／VHD／VHDX／ISOを梱包した。
- `browser-performance-boundaries-guest-20261012`：新paint境界の実出力は17検査／0失敗、networkの新admission／generation取消callback loan／nested pumpは6079ms・0失敗。ただし主担当のruntests期待prefixが旧分岐用だったため総括は1成功／1失敗。元serialを保持し、実分岐名へprefixを訂正した。訂正時点では製品・17検査を変更／反復していない。
- 独立paint反例を受け、普通LI markerのindex-only target、長sortのnative取消を修理。paint orderはwindow pixelを書き換える前に準備し、取消時は未完成cacheを公開しない。BVHもinterruptible typed mergeへ接続した。
- 通信review反例を受け、縮小予算、parked child、1slot混在公平性、stream callback中取消のloan解放を修理。host追加26境界は成功し、native callbackは上のguestで成功。

## 変更後の実観測（第一回）

`browser-perf-after-{youtube,shaka}-20261012`。上と同じWHPX／4CPU／4096MiB／1920×1080／最大化／録音有り。起動媒体のkernel／initrdは最新build hashと照合。二画面を一括撮影し主担当が原画像を見た。最初の約90秒は追加境界guestも並行していたので、その区間の性能は基準二VMと厳密比較しない。

- YouTube：最初の実動画、本文、画像を表示。PageDown／PageUpで本文下と上へ戻る実画像を確認した。19秒動画の二回目のauthor-play→native-eos-stopはゲストuptimeログで19.252秒（基準二回目19.197秒）であり、ここを速度向上／ホスト実時計の厳密測定とは呼ばない。初回は同ログで40秒かかり、長いJS/native layoutが残る。自動次動画後の旧title／channel残留も基準から未解消。
- Shaka：実PLAYとplayer内Playを現hoverで確認してクリックし、同じHLS実動画を再生。複数の進行画像と最後の映像を見た。MSEは末尾でwaitingがあるが372333ms／ended1／audio closedまで到達。音声は録音のみで聴取していない。
- Shakaの再生中には通常full drawが0回の2秒区間があり、動画patchのwindow publicationは増える。基準では同種区間にfull drawが7～16回。部分更新は実経路で成立したが、ABRのsource寸法と時刻が異なる区間を一律fps／速度比較へ昇格しない。
- controls再表示とspinner残留は未判別。基準にもcontrols非表示があり、入力target／媒体状態event／overlay更新の境界を分ける。待ち表示をサイト名で消したり音声時計を架空に進めて隠さない。
- 実PgDnでShakaの全cardが画面約603pxで切れる問題を発見。祖先947px高scrollportを文書全体ごと411px動かす旧入力経路と、overflow祖先を無視するabs別extent集計が共通境界。新cache起因とは断定していない。この観測を受け、内部scrollportへのnative入力chainと確定root extentを修理中。終了して新媒体の新入力条件で再確認する。

第一回の変更後二VMと追加境界guestは終了済み。Hyper-Vの実機起動・設定・使用中媒体は操作していない。

## 共通スクロール・damage修理後（2026-10-13）

- 内部 `auto/scroll` port→子frame viewport→frame祖先→rootの残deltaを、native offsetsだけで渡す。hiddenはユーザー入力から除外し、既存CSSOMのprogrammatic scrollingは保全する。未commit layout／active layoutへ再入しない。
- node通知は普通tickから既存のtrusted／非bubbling／postTask重複排除へ接続。callback中再入力は次tickへ残す。確保元と現在ownerが異なるadoption／兄弟frame破棄では、確保元の解放前に他ownerのpending borrowも外す。
- 既知video damageの後にscroll／marquee／child-frame／caption／画像完了が来る順序では、共通 `doc_paint_full` がunknown damageへ昇格する。未知の画面変更を古い小矩形へ縮小しない。
- 正規all `browser-performance-scroll-build-20261012.log` の後、追加寿命修理のdoc objectが古いことをmtimeで検出。`browser-performance-scroll-lifetime-build-20261012.log` で再梱包終了0、最新objectを照合。生成元JS hookから `js_embed.py` が正規にembedded bindingを更新した。
- 新guest `browser-performance-scroll-guest-20261013`：paint新17境界はprefix訂正後PASS。scroll-input48条件は47成功／1失敗で、失敗はviewport200をauto root高さへ要求した誤期待だった。
- 同じclip入力とabs無しflow対照だけを `browser-performance-extent-guest-20261013` で確認：4検査／0失敗。両者のroot/doc/scroll高さ40、clip40、private overflow350、abs y300／height50。製品は内部overflowをrootへ漏らさず、誤期待だけ200→40へ訂正した。元FAILを保全し、他47／旧17を追加反復していない。
- 最新二サイト `browser-perf-scroll-{youtube,shaka}-20261013` を同じVM条件で並行起動・撮影・主担当目視。Shaka実PLAY→player内Play、PgDn後の `scroll-inner-down.png` はカードの説明／ボタン／footerまで表示し、旧603px切断を解消。PgUpの受付sequence12の後の `screen-final.png` でnavbar／再生中の映像へ戻ることも主担当が見た。指定名の追加shotはハーネス期限後に届かなかったため、実際に保存された最終原画で判断した。新VMを増やして同じ確認を反復していない。
- YouTubeはコメント・本文・画像へのPageDownと上へのPageUpを確認したが、自動遷移先の映像は指定動画の再生成功へ数えない。ユーザー提供画像でもURLと旧題名の不整合が見える。広告という断定や、正しい指定映像という成功報告には使わない。
- 部分描画の費用をfull draw計数から隠さないよう、debug時だけpartial draw／partial present／video copy-presentを別集計した。Shakaの一例は2062ms区間にfull draw0、video patch48／60ms。別区間はpartial81／239ms＋present88ms、video patch6／8ms。scale／overlay費用はmedia supplyに属し、これらの計数から総処理費用ゼロ／全体速度向上とは主張しない。

最新実サイト二VM・scroll guest・extent guestは終了。WPTは提供checkoutからtestdriver不要の関係原本3ページだけを選び、実assert／testharnessを改変しない限定確認を準備した。WPTは実サイト受入の代わりではない。

## WPT原本の結果

提供checkout `D:/Programes/cloned repos/wpt` の原本HTML3件＋原testharness2scriptをloopback HTTPで配信。native guest `browser-performance-wpt-guest-20261013` の受信長・SHA256はすべて原本と一致。原harness／DOM／CSS／assertを変更せず、標準completion callbackの観測だけで結果を受信した。全suite・testdriverは実行していない。

- dynamic anonymous scrollboxのvisibility変更後のhit：1成功。
- 負marginとoverflow visible／hidden／auto／clipのscrollWidth／Height：4成功。
- negative-z inline／single-axis clip：1失敗。原本の名前付き参照 `container` が未定義であり、ヒットassertより前で止まった。clip／negative-zの合格証拠にはしない。試験へglobal aliasを注入したりassertを移植して成功を作らない。
- 原ケース6件：5成功／1失敗、原harness statusは全ページ0、通信／checksum／観測基盤失敗0。runner総括は3ページ中2成功／1失敗。原name／status／message／stackをserialに保存。3原本の選定・hashはWPT担当記録に記載。
- これは性能変更した同じnative DOM／CSS／layout／paint coreの補助確認であり、実GUI、全WPT適合、速度改善を証明するものではない。所有を確認したHTTP server（PID35196、127.0.0.1:8766、checkout root）は終了した。

## 一時媒体の回収

終了metadataと実process absence、resolve済み絶対path、通常ファイル・対象build配下を照合して、今回の旧9runの使い捨てboot/data/PPM/未聴取WAV計228ファイル（論理4.429GiB）を回収した。ソース、最新配布IMG/VHD/VHDX/ISO、通常の永続data、ユーザーHyper-V、PNG原画と小ログ・metadataは保全。一つの基準boot `browser-perf-base-native-youtube-20261012/boot.img` を保全した。回収後のD実空きは103.96GiB。疎性のある媒体の論理削除量を、そのまま空き増加量とみなさない。削除済みrunのdiskを保存済み／そのまま再実行可能とは報告しない。

最新4runも同じ停止・path照合後、51一時ファイル（論理1.681GiB）を回収した。回収後のD実空き105.41GiB。GNU duが報告したbuild使用量は138,139,121,664byte（約128.65GiB）。この作業に属すると個別確認していない他runや古いデータは削除しない。WPT追加runが作る一時媒体も終了後に同じ確認で回収する。

最後にguest固有の `test-boot.img`／`test-initrd.tar`／`test-kernel.elf` とWPTのdata等20ファイル（論理3.765GiB）も、六guestの停止metadata・絶対path・非symlink・実process absenceを確認して回収した。D実空き108.34GiB、QEMU残り0。全回収は299ファイル／論理約9.875GiB。初期の `boot.img` filterではguestの異名 `test-boot.img` を拾わなかったため、別の明示したfilterで確認・回収した。通常buildのkernel/initrd／配布媒体と一つの基準bootは保全。上のdu値はこの最後の回収前の測定である。

## 引渡しと残件

- 製品の17変更C sourceに対応する正規objectのmtimeを最終照合し、sourceより古いもの0。これは動作試験とは別の梱包整合補助。主担当が最新実QEMU二サイト原画、PageDown／PageUp、PLAY操作、元失敗を含むguest serialを確認した。
- 最新通常成果物：`build/kernel.elf`、`initrd.tar`、`nocturne.img`、`nocturne.vhd`、`nocturne.vhdx`、`nocturne.iso`。kernel SHA256 `d42fd10a36f1d4d1b1a4c6ba75a64cae28253293893e24d0a717cc5d822a9e45`、initrd SHA256 `af8902fd8c2fc5c6dc008af0c4875a364798ffc61ce104e849d9e66d7d76f7fa`。Hyper-V実機は未確認であり、VHDX生成と混同しない。
- 未解決／未確認：YouTubeのURL変更後の旧題名・channel、長いJS/native処理と初期待ち、Shakaのcontrols再表示／waiting UI、実動画のpause／seek／再open一式、聴取音質、Hyper-V実機。今回の部分描画・共有転送と限定検査成功でこれらを相殺しない。
- WPTの1原ケースは名前付き参照未定義で本来のhit判定へ未到達。五成功を全WPT適合・ブラウザー全面高速化の根拠にしない。
- 停止中のbuild／guest／QEMU／WPT HTTP serverはない。通常data・ユーザーHyper-V・他作業のprocessは操作しない。ソースは作業木に保持し、無関係なreset／restore／cleanは行っていない。
