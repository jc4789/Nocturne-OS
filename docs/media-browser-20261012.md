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
