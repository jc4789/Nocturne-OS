# ブラウザーの既知の不足・修理割当

対象は現ソース、提供ログ、今回の実画面、担当の実装境界から判明した不足。
Web標準全体の完全監査を済ませたという意味ではない。未知の項目を対応済みとも扱わない。
新しい不足はこの表へ追加し、実装・修理を主にする。検査件数や行数だけを成果にしない。

## 最優先の実不具合

|不足／不具合|根拠・境界|担当・状態|
|---|---|---|
|YouTubeの長い初期化停止|実VM Timer90 85.102秒、layout84.139秒中、媒体協調供給64.631秒|media_timing：producer待ち/yieldを除去して梱包済み。同task10.868秒へ短縮、全体の遅延解消は未達|
|音声先行／映像停止とジャンプ|修理後も実VM音声clock19006ms、映像PTS11133ms、decoded101で枯渇|media_timing：独立VIDEO laneと実PCMの供給流量制御を修理中。実A/Vは不合格|
|実decoderのcatch-up破棄がdrop診断に出ない|旧workerは配送frameだけを数えてdrop0と表示|media_timing：wire v5で実decoded/drop差分へ接続して梱包済み、実停止修理後の再受入は未実施|
|trexからsample-sizeを継承するfragmentの公開遅延|tfhd/trun省略時に完全mdatを次segment/EOSまで保留|media_crypto：初期化のimmutable track defaultsを既存parserへ統合して梱包済み、当該形式の実配信受入は未確認|
|短い音声終了後、WebM映像／EOFが進まない|既存vp9-opus媒体試験失敗|media_timing：owned video lookaheadとEOF境界を修理|
|高profile VP9の旧拒否gate|新pixel converterとopen gateの不整合|media_timing：profile0〜3へ整合|
|媒体memory試験の範囲外読出し|4byte配列に33MiB lengthを偽指定する廃止済quota試験|media_timing：有効なSIZE_MAX overflow拒否へ訂正、実製品の上限は復活させない|
|旧「EMEなし」試験と追加ClearKeyの衝突|avmedia fixtureが初期assertで停止|root：実API＋非対応key system拒否へ訂正|
|繰り返すinline CSS解析|resource snapshot破棄ごとにASTも破棄|root：共有immutable AST、独立scope/order/media、未参照回収を実装|
|Shakaがvendor.includesで停止|変更前実VMの例外|media_storage：navigator.vendorを実装。最新VMはasset一覧まで表示|
|Shaka画面のdrawer／本文の配置|最新実VMでも左drawerが本文を覆う|root：共通CSS／transform／layout境界の残件、サイト注入しない|
|YouTube本文・おすすめの初期skeleton|最新実VM、停止中はプレーヤー以外が未完成|root＋media_timing：長停止修理後の実UI対象|

## 追加実装中

|不足|担当・製品経路|範囲|
|---|---|---|
|AES-GCM|media_crypto：既存WebCrypto／BearSSLへ追加して梱包済み、実サイト暗号再生は未確認|鍵、IV、AAD、tag、認証失敗を実復号へ接続|
|PSSHがappend chunkを跨ぐ場合のencrypted event|media_crypto：既存EME／MSEへ追加して梱包済み、実サイト暗号再生は未確認|SourceBuffer世代／abort／resetを守る、全動画の再コピーを避ける|
|IndexedDB.databases()|media_storage：native保存層へ追加して梱包済み、実offline再起動は未確認|origin隔離された小catalog、payloadを読まず列挙|
|WebVTT markup・内部timestamp・非既定cue設定|media_storage：native字幕描画へ追加して梱包済み、全cue設定の実表示は未確認|強調／ruby、position／align／size／vertical／regionを実描画に接続|
|Request/Response.formData()|root：既存Fetch Body／FormDataへ追加して梱包済み、guest実経路確認済み|実URL-encoded／multipart解析、File binary、Body消費・不正入力拒否|

## 実装済みだが全面受入には未到達

- playbackRate 0、0.25〜4倍、defaultPlaybackRate、preservesPitch：実PCM変換へ接続。
- MP4 Opus／VP9／HEVC／AC3／EAC3、高bitdepth SDRの変換。
- AES-CBC／CTR、raw/JWK鍵、temporary ClearKey、native CENC sample復号。
- IndexedDB schema／request／transaction／index／cursor、差分payload保存、versionchange／blocked。
- native全画面／floating PiP窓、退出・切断・文書破棄。
- 字幕plain textは既存経路で表示済み。音質聴取・実同期は別の受入。

## まだ未対応、偽対応しない

### 媒体・画面
- AV1 software decoder、HDR tone mapping、10bit physical display。
- 逆再生、0.25未満／4超の速度、混合済みPCMのstream単位取消。
- iframe祖先を含むFullscreen chain、multi-screen選択。
- MPEG-TS HLSのShaka transmux経由の実ネットワーク受入。
- 実ネットワークAES-128 HLS／ClearKey、offline保存後の再起動再生。

### 暗号・保存
- AES-GCM以外の署名／RSA／derive／wrap／unwrap。
- 永続EMEライセンス、商用DRM、WebM暗号、CBCS／CENS／CBC1、seig key rotation group。
- IndexedDBのWorker realm公開、巨大metadataの増分journal／B-tree、全WPT互換。

### 今回ソースで確認したその他の不足
- Module Worker／SharedWorkerのmodule type、追加credentials mode：`js_worker.js`が明示拒否。
- FontFace／FontFaceSet loading API：WebフォントdecoderはあるがJS loading APIは未接続。
- link resource hints：preload／modulepreload／preconnect／dns-prefetch／prefetchは未対応。
- Fetch：manual opaque redirect、referrer override、HTTP response cache、stream uploadは未対応。Window integrityはSHA-256／384／512で応答公開前へ接続済み。script／link／import map側のSRIは別の残件。
- FormData(form, submitter)は既存native collectorと接続済み。disabled／selected／FACE／実File、イベント後のリスト複製と再入防止を追加。通常native GUI送信のformdataイベントによる本文変更は別の残件。
- XHRのstream upload。
- Workerの実POST／FormData送信、Response.formData／blob／body stream／cloneは共用parser・Blob・Streamsとnative binary IPCへ接続済み。Worker Request／stream upload／SRI等は未対応。
- ImageBitmapのvideo/image入力、encoded SVG／EXIF orientation、premultiply／高品質resize。
- Web Animationsのadditive composition／iteration accumulation。
- XML文書生成／解析／fragment parsing。
- native属性値／namespace URI内のNUL。
- Credential provider／credential creation。
- 任意contenteditableのundo／書式編集、Range layoutの未対応部分。
- document.domainの緩和とpublic suffix検証。
- WindowProxyのproperty definition、window.openの追加overload。
- smooth element scroll（現在instantへ明示fallback）。
- CSSのグラフィックtransform／translateと、その描画・hit-test・寸法の共通変換（SVG／Canvasの変換とは別）。Shaka drawerの重なり原因候補。
- Webフォントの完全なGSUB／GPOS shaping・ligature。FreeTypeによる復号／rasterだけでshaping対応とは扱わない。
- import mapのintegrity検証。
- brokered MessagePortの再transfer／キュー内のnested Port transfer。
- TextEncodingのresizable／shared buffer入力。
- WebVTT header extensionと完全なSTYLE cascade。
- WebVTT作者::cue CSS、完全bidi／collision、ruby分節、scroll-upアニメーション、in-band字幕。

## 保全・ライセンス

FreeTypeはFTLを選択し、上流の著作権・二重許諾文書を保持する。
主担当の誤ったTinyCC／FreeType文書削除は復元済み。
別の機能や追加許諾を、ライセンス名のgrepだけで削除しない。
Hyper-Vはユーザー担当。今回QEMU結果をHyper-V成功へ流用しない。
