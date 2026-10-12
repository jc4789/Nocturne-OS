# 媒体同期・Shaka・YouTube

運用: J-space medium、共同実装。開始git差分は空。前回DDG/font/SharedWorker修理は現HEADに保全されている。

## 目的・所有境界
- 主担当: 共通YouTube読み込み／配置、fullscreen/PiP、native lease、host bridgeと最終媒体・実QEMU。
- media_timing: 音声／映像同期・途切れ・速度、実codecとMSE。
- media_crypto: WebCrypto AESとClearKey/EME、sample復号。
- media_storage: IndexedDB、navigator/vendor、mediaCapabilities、字幕。
- semantic mapは `docs/media-browser-20261012-map.json`。以前の停止済みHTML parser作業は再開しない。

## 基準・受入
- 添付2画面を自分で見た。Nocturneは0aGwyCSw0bo、FirefoxはjNQXAC9IVRwという別videoIDで、Firefoxは配置の参照に使い速度・内容比較を同一条件としない。
- 実動画の開始／時間進行／音声同期／停止／seek／再開／取消、Shakaのclear DASH/HLS・AES HLS・ClearKey、速度0/2、字幕、fullscreen/PiP、offline保存は別々の受入とする。未変更の同試験を反復しない。
- 指定動画とShaka公式新URLをWHPX/4CPU/4096MiB/1920x1080/音声ありの隔離VMで並行起動。`browser --maximized`で画面全体にし、最初の両画像を主担当が見た。YouTubeはblack playerとskeleton、Shakaは白画面とvendor/includes例外。使用媒体は変更前の基準。
- Shaka旧appspot demoは現在GitHub Pagesへ移転。新URLは https://shaka-project.github.io/shaka-player/demo/ 。
- 原因調査: YouTubeは約10.9MB scriptのcompile1694ms/execute808ms、別1145byte scriptでlayout432ms/22読出しを観測。HTTP成功や取得済みをUI完成としない。

## 製品修理の不変条件
- サイト専用分岐・偽装・注入代替・POSIX互換追加をしない。
- 実PCM消費、media PTS、同じrate/generation、取消・寿命を保つ。
- IndexedDBの媒体payloadは変更recordだけを永続化し、全動画DBの毎回読戻し／書戻しを避ける。
- codec/DRM/画面APIは実decoder・復号・native表示を持つものだけ対応を宣言する。

## 次
媒体・暗号・保存の統合をまとまりとしてビルドし、最大化した実サイトの操作で受け入れる。実装・故障修理を主とし、無変更の検査ループをしない。

## 統合修理
- 音声実DMA時計・速度DSP・新BMFF codec・AES/ClearKey・差分payload IndexedDBの担当差分を保全。主担当が共通ABIとprivate host、イベント配送、bootstrap includeを接続した。
- IndexedDBの戻り値確保失敗でnative取引／接続が孤児化する問題を独立レビューで発見し、property定義失敗もabort/disconnectへ接続。空ArrayBufferとpending例外の境界をQuickJS実ソースで照合した。
- 共通CSS修理：inline ASTをresource snapshotごとに捨てて再解析する経路を、immutableな共有cacheへ変更。各scope/order/mediaは別instance、scan成功後に未参照ASTを回収し、CSSOMの編集済みASTは別所有を保持する。文書寿命を超える無制限蓄積をしない。
- 変更前の最終両画像を主担当が見た。Shakaはvendor/includes例外を表示。指定YouTube `0aGwyCSw0bo` は現在利用不可を表示した。このURLで動画進行やA/V改善を判定しない。別URL使用時は外部条件変更として区別する。
- 主担当のjs/doc/webidb製品object限定ビルドは成功。実サイト受入と全媒体梱包はまだ未確認。native Fullscreen/PiPの実窓・top-layer接続を統合中。

## 最初の統合媒体の実結果と追加修理
- 正規全体buildでIMG/VHD/VHDX/ISOを生成。最大化YouTubeとShakaを並行で起動し、起動中／本文／console画面を主担当が一括撮影して自分で見た。
- Shakaはvendor例外がなくassetカードとPLAY等を表示した。ただし左drawerの重なりが残り、実動画のPLAY／offline操作は未達。
- YouTube `jNQXAC9IVRw` は実動画と0:19 controlを表示し再生開始したが、長い停止と音声clock／映像PTSの差が残った。完全正常再生とは報告しない。
- 根拠：Timer90 85102ms、layout84139ms、cooperative媒体供給19673回／64631ms。空producerの同期wait/yieldをhot DOM/layout safepointで反復する製品経路を特定。media_timingがMSE／Rangeの協調経路を実ノンブロッキングへ修理し、入力枯渇・鍵待ちの再要求を停止した。
- 一回の関連guest確認はpass6／fail4。DSP22、BMFF232、CENC11、CSS cache27、observer11、DOMイベント25は成功。これら成功済み検査は反復しない。
- 失敗：媒体codec試験の範囲外入力Pagefault、旧EMEなしassert、VP9高profile gate、短い音声末尾のWebM EOF停滞。製品gate／owned video lookaheadを修理し、廃止済quotaを偽lengthで試すfixtureと追加APIに相反する旧assertを現契約へ訂正。期待値を弱めて失敗を除外していない。
- 最新ユーザー指示に従い既知不足全体を `docs/browser-missing-20261012.md` に記録。暗号担当はAES-GCMと分割PSSH、保存担当は実catalogによるdatabases()とWebVTT字幕、主担当は実Body.formData()を追加。Web全規格の完全対応とは扱わない。
- 次はこの修理・追加を最新媒体へ梱包し、以前失敗した経路と変更した製品境界だけ有限に確認する。検査拡大・未変更反復はしない。

## 最新訂正
### 第二媒体と実機能の未達
- `build/media-repaired-build-20261012.log` の正規全体buildは成功し、IMG/VHD/VHDX/ISOへ追加機能を梱包した。前回失敗した媒体4経路とFetchの限定guest確認は5成功／0失敗。ただし、この結果は実動画の正常再生を証明しない。
- 最大化したYouTube／Shakaを再び並行観察し、主担当が実画像を見た。YouTubeの同じTimer90は85.102秒から10.868秒へ短縮したが、音声clock19006msに対し映像11133ms／decoded101で停止し、後に映像が飛ぶ。実A/Vは依然不合格。録音保存を音質の聴取確認とは扱わない。
- 次の製品修理は、独立MSE VIDEO laneが詰まっていてもPCMを先に配送してしまう供給選択。実映像のPTSとdurationまでPCMを流量制御し、未復号映像を音声先行に伴うSourceBuffer.removeで失わないようにする。時計の数値だけを変えて同期済みに見せない。
- worker契約v5で実decoderのdecoded／drop差分を伝える。旧drop0表示はcatch-up破棄を数えていなかったため、滑らかさの成功根拠に使わない。
- 別の共通MP4境界も修理：tfhd／trunで省略したsample sizeを初期化trexから継承し、完全なmdatを次segment／EOSまで不必要に保留しない。今回YouTubeが当該形式だったとは断定しない。
- 成功済みfixtureの反復ではなく、この供給修理後の実動画進行・停止・シーク・再開を次の受入にする。Fullscreen／PiP／Shaka実再生・offlineは別々に未確認として保つ。

### ライセンス範囲
ライセンスの変更対象はFreeTypeのFTL選択のみ。主担当が誤って削除した
TinyCC runtime/CIL opcode/PE工具部分とFreeType代替ライセンス文書は復元し、
TinyCC置換・全体GPL検査ゲートを取消した。上流の追加許諾を無視して
ライセンス名だけで削除しない。媒体修理の差分は保全し、元の実装作業を続ける。

## 追加観測を停止して実装へ戻した範囲
- flow媒体の実YouTubeは音声5414ms／映像5200msまで進んだが、dueな先読み2枚を残して停止し、長いnative taskも残った。Shakaは外部buffering_observer.jsのHTTP503で初期化できなかった。両QEMUと操作補助は停止済み。ハーネス終了0をサイト合格に扱わない。
- 媒体修理：協調取得がSourceBuffer slot／revision更新を飛ばす早期returnを修理。空BUSY反復は実bytes／decode進捗に基づく非blocking予約で抑え、新APPEND／KEYSは即解除。同一pumpの音声device snapshotを共有し、PCM投入／sink変更で失効する。
- 主担当は、次decodeがBUSY／AGAINだと既にdueなowned映像まで表示されない境界も修理。再生可能な先読みは新decode待ちより前に消費し、作者PTS・実時計・世代／seek失効を保持した。
- 不足の実装：Worker実POST／FormData binary IPCとResponse.formData／blob／body／clone、Blob Workerのnative creator origin継承、FormData(form,submitter)の既存native collector・実File identity・FormDataEvent・再入防止、Window FetchのSRIを追加。画像submitterの実CSS座標も既存click/default actionへ接続した。
- 根拠は [W3C MSE](https://www.w3.org/TR/media-source-2/#sourcebuffer-monitoring)、[WHATWG HTML媒体](https://html.spec.whatwg.org/multipage/media.html#playing-the-media-resource)、[WHATWGフォーム](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constructing-the-entry-list)、[WHATWG Fetch](https://fetch.spec.whatwg.org/#main-fetch)、[W3C SRI](https://www.w3.org/TR/sri/#verification-of-fetch-requests)。PCM flow／wire／committed rasterは内部設計であり、仕様がその方式を義務付けるという主張ではない。
- 最新指示後は追加VM／guest検査を実行していない。追加実装のソース統合・正規生成・媒体梱包と、実機能の受入は区別する。実再生の停止・同期、音質、Fullscreen／PiP、暗号配信／offlineは全面合格としない。
- 最終梱包は `build/media-due-frame-build-20261012.log`、終了0。IMG／VHD／VHDX／ISOを更新し、avmedia／bootstrap／Worker runtimeの生成元より製品objectが新しいことを照合した。新しい媒体での実サイト再試験は行っていない。

## ユーザー訂正後の再開と表示停止の修理

- 今回の追加統合：IndexedDBの全native I/Oを非同期storage helperへ分離し、差分journal・小さいcommit head・cache・directory分散・VALUES batch・文書退役時の回収を接続。完了配送をFIFOにし、壊れたhelperの旧handleを新helperへ再結合しない。BEGIN/VALUEのowned bufferはJSへ所有移譲し、ページ側の大きい再copyを除去した。
- iframe Fullscreen/PiPは祖先権限とnative窓へ接続し、40ms pollingをFIFO通知へ置換。実復号能力はcodec/profile/bitdepth/寸法/媒体世代と実動画・音声費用を照合して予測する。高bitdepth/422/444変換はSSE2、AC97の上限ASSERTは背圧と安全なEIO回収へ変更した。関連記録は担当文書にある。
- `window-published=0`につながる別の共通描画境界も修理した。video snapshot probe時、横方向にclip外の画像・SVGや非交差画像のscale cache missまで全動画を拒否していた。置換画像の水平clipと描画領域交差判定を追加し、probeでcacheを更新しないまま交差領域だけを拒否する。半透明suffixの完全合成まで対応したという意味ではない。
- これらのソース保存・構文確認を実サイトの正常再生へ昇格しない。最新生成物と全媒体を更新した後、最大化した実YouTube/Shakaの表示・実操作・動画進行を確認する。以前の失敗と未確認操作は保持する。
- 上の「追加VMなし」は訂正前の記録。ユーザーは検証禁止ではなく、未変更の試験反復を止め、実装後に検証するよう求めた。due-frame媒体の最大化YouTube／Shakaを並行起動し、主担当が開始・進行画像を見た。YouTubeは19秒の参照動画から次動画へ実遷移したが、途中の入力starvationと長い壁時計遅延が残る。Shakaは実アセット一覧に到達したが、左drawerが本文へ重なる。どちらも全面合格ではない。両VMは観察期限で終了、操作補助も終了した。
- 新しいユーザー画像はlayout depth 1／author depth 0／parser completeで取消後に作者スクリプトまで永久停止している。ネイティブ描画のみの取消と実行中作者／parserの中断を区別し、前者だけ独立queueを保全する修理を追加した。半完成boxesは破棄し、次web_tick／console評価は作者処理より先にstack外でcoherent layoutを再構築する。中断した作者stack／parserは復活させない。WHATWG HTMLの[スクリプト中断](https://html.spec.whatwg.org/multipage/webappapis.html#killing-scripts)と[イベントループ](https://html.spec.whatwg.org/multipage/webappapis.html#event-loop-processing-model)を参照した内部回復設計であり、独自checkpointの方式が仕様に義務付けられているとは扱わない。
- 同一DOM revisionのmetadata全走査をcacheし、inline CSS ASTの線形探索をcollision安全な動的hash indexへ変更。同期custom-element constructorごとのidentity table全再構築も局所backshift更新へ修理した。DOM／CE反応順・scope／order・所有権を変更して速度を偽装しない。実際の速度差は次媒体の同条件観測待ち。
- 動画snapshotは非交差iframe／opacity領域まで全rejectする条件を実矩形へ限定し、probe中の子文書paint再入を防ぐ。入力／outputが共にないMSEの無効pump・短周期起床を除去し、controlとVIDEO readinessのpollをまとめた。実PTS／DMA時計は維持。
- CSS translationは[CSS Transforms](https://drafts.csswg.org/css-transforms/)の通常flow非影響、描画／hit／矩形の一致、fixed containing block、元と変換後のoverflow unionに従い追加中。個別translateのdefault 0px省略は[Level 2 §5.1](https://drafts.csswg.org/css-transforms-2/#individual-transform-serialization)を直接照合して修理。回転／scale／一般3Dまで対応したとしない。

## 主担当単独の規格照合・修理（サブエージェント／J-space停止後）

- `js.c`：WHATWGの描画更新順に、同一機会のrAF集合・callback後microtask・layout/ResizeObserver・IntersectionObserverを配送する。登録順snapshotは取消と再利用をID/orderで照合し、callback中の新規rAFを次の機会へ送る。画像は正規src/srcset/pictureで選び、非標準data-*推測を除去。画像登録を総pending数8未満へ制限する条件と、重複判定の全pending走査を除去した。`webnet.c`の既存通信workerへclass間の公平な供給を接続した。内部queue方式そのものが規格指定とは主張しない。
- フォント／レイアウト：成功したフォントのmetrics・cache失効と、floatを外へ出さない共有BFCの安全な再利用を修理。元のstyleincremental 3失敗は期待を弱めず修理した。FormDataは実出力名とruntestsの期待prefixの不一致を訂正した。
- 実CSSの取得で、Shakaの`Roboto`に`unicode-range:U+E0??`限定の別書体が宣言されていることを確認した。`css.c`で単一点・区間・wildcardを解析・統合し、計算済みstyleごとに独立所有する複合選択へ接続。`layout.c`の計測と描画は同じ文字範囲と実cmapを使い、範囲外の英字へ私用文字書体を使わない。未使用subsetは取得しない。style等価比較はポインターではなくresource/face/範囲の値で行う。CSS Fontsの全機能・複雑な文字cluster整形まで完了とはしない。
- 灰色の動画面：空のoverlay canvasを不透明にしていた独自UA背景色を`cssprop.c`から除去した。WHATWG Canvasのcontext-none bitmapは透明黒であり、作者CSSを差し替えず下の実動画が見えるよう修理した。
- 媒体状態：メタデータ到達を現在データ到達まで隠していた通知と、readyState 2でのcanplayを修理。世代で保護したmedia taskに loadedmetadata/loadeddata/canplay/canplaythrough を分離し、MSE append/EOS/duration後も配送へ接続。初期paused preloadとpause/resumeは実復号データを保持し、停止中にカーソルだけ進めない。初期の「先行復号が存在せず相互待ち」という断定は誤りで、既存経路を再読後に訂正した。
- MSEの偽の欠落：VP9 MP4の実fixtureは10fpsだがpacket.durationが空で、一律40msのfallbackが0–40、100–140ms等の偽の穴を作っていた。`media_mse_parser.c/h`の既存trex/tfhd/trun解析から、sample位置・track ID・実durationを一度取り出してsortし、`media_mse.c`がpacket位置で照合する。毎packetの全fragment再走査はしない。PTSとdurationを個別丸めせず、実sample終端を一度だけ丸める。EOSの秒/millisecond往復による不要removeも修理した。
- 規格：[WHATWG描画更新](https://html.spec.whatwg.org/multipage/webappapis.html#update-the-rendering)、[画像選択](https://html.spec.whatwg.org/multipage/images.html#updating-the-image-data)、[媒体ready state](https://html.spec.whatwg.org/multipage/media.html#ready-states)、[Canvas](https://html.spec.whatwg.org/multipage/canvas.html#the-canvas-element)、[CSS Fonts unicode-range](https://www.w3.org/TR/css-fonts-4/#descdef-font-face-unicode-range)、[MSE](https://www.w3.org/TR/media-source-2/)、[ISO BMFF byte stream](https://www.w3.org/TR/mse-byte-stream-format-isobmff/)を照合した。
- 確認：単独前半のguestではstyleincremental 131、observerfairness 11、DOM 25、FormData 27が失敗0。文字範囲追加後のstyleincrementalは141/0。新MSE回帰はcanplaythrough待ちで失敗し、期待を維持して原因を修理した。最終`browser-mse-sample-time-guest-20261012`はブラウザー媒体30/0、BMFF233/0、fatcheck成功。全ビルド`browser-mse-sample-time-build-20261012.log`は成功しIMG/VHD/VHDX/ISOを更新した。
- 実画面：`browser-font-range-shaka-20261012/assets.png`でサンセリフの本文、`playing.png`で実映像を主担当が見た。初めてのPLAYは読込みで、さらに実playerの再生ボタンをhover確認して押した。60秒終端までnative時計と映像は進行したがdrop 53・starvationが残るため滑らかな再生合格ではない。YouTubeは本文・推薦画像・参照動画を表示し次動画へ移ったが、遷移後に旧titleが残る不整合もあり未達。これらは最終sample時間修理より前の画面として区別する。
- 残件：YouTubeの初期化・DOM/layout連鎖と履歴／媒体所有の不整合、長い媒体starvation、音声の実聴取とA/V同期、range controlの値に連動しない中点描画、自動再生方針・play Promiseの完全な状態モデル、FontFace/FontFaceSet API、iframe Fullscreen/PiPの実GUI受入、AV1/HDR、Worker/IndexedDBの実サイト負荷、Hyper-V実機。これらを今回のfixture成功で完了扱いにしない。依存追加・ライセンス採用変更・ファイル削除は行っていない。
- 最終sample時間媒体の実サイト：`browser-sample-time-shaka-20261012`のassets/player/hover画像を主担当が見た。メタデータ到着で427→683pxへplayerが変わったため、古い座標をそのままクリックせずpointerと画像を確認し直した。ただしplayer再生操作の前に420秒の観察期限でVMが終了し、最後の操作はsocket失効で実行されなかった。最新媒体の実サイト再生・停止・再開は未確認であり、旧媒体の映像表示やnative試験へ置き換えない。今回所有の全build/test/QEMUと操作補助は終了済み。試験側の期限運用の不備も残件として保持する。
