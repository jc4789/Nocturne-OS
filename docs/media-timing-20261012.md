# 媒体同期・速度・codec 実装引渡し（2026-10-12）

J-space medium。主担当が実QEMU・最大化画面・全媒体梱包を受け持ち、この担当は単一所有のPCM/DMA時計とcodec入力境界を改善した。サイト名の製品分岐、POSIX API、forkは追加していない。

## 製品変更

- `user/libc/web/js_avmedia.js` / `avmedia.c`: playbackRate 0はpausedを変更せず時計と供給を停止、0.25〜4は実PCM変換へ接続。defaultPlaybackRateを独立保持し、loadで適用。ratechangeはtaskで通知。preservesPitchはネイティブ処理の選択に接続。
- `media_rate.c` / `media_rate_private.h`: packet復号・GUIと分離した48kHz stereo PCM責務。pitch維持はupstream atempoのWSOLA、品質範囲0.5〜2のcascade。pitch変更はlibswresample。所有frame・末尾flush・失敗回収を保持。1xは変換を挟まない。
- `avmedia.c`: media時刻とdevice出力framesをrateで対応づけ、音声clockを映像・字幕へ共有。sink実容量8192frames内の7200まで供給、低PCM時に協調collectionを1ms、通常5msに変更。固定10ms throttleの全IPC段階への積算遅延を削減。
- `browsermseworker.c`: STEPで全buffer/ranges metadataを毎回送らず、revision・実info・鍵待ち状態が変化した時だけ配送。APPEND/REMOVE等は必ず配送し、rangeや文書世代を省略しない。
- `kernel/src/dev/audio.c/h`, `ac97.c`: software ring残量と、AC97へ混合済みで未消費の当該streamだけの寄与を区別。実32descriptorに根拠を持つrunを記録し、既存tickでCIV/PICB確認から退役。gettersでport I/Oしない。4byte read互換保持、拡張statusはcommitted/played/generation/推定flagsを返す。flushで旧generationの時計寄与を解除、closeはDMAも待つ。remoteはserver mix推定と明記し、他streamの全DMA待ち時間を加算しない。
- `media_types.c`, `media.c`: MP4 Opus/VP9・HEVC Main/Main10・AC3/EAC3を実decoderと整合。planar 10/12/16bit SDRを8bit physical surfaceへ変換。HDR transfer/BT2020は見せかけの色変換をせず拒否。AV1はsoftware decoderがないので拒否を維持。
- `ports/ffmpeg/prepare.py`, `vendor.py`: 正規browser profile生成。HEVC/AC3/EAC3とabuffer/abuffersink/atempoだけ追加。ネットワークprotocol・encoder・pthread・asmは追加していない。374 C閉包を正規vendorで生成した。CONFIG_GPL=0、LGPL-2.1-or-later。既存許諾文書を削除していない。
- 暗号担当と合意して`emeKeys` snapshotのmedia loadを跨ぐ寿命、最終wipe、MSE再作成時の再送、keyWaiting metadataとwaitingforkey通知を接続。

## 実行した確認

- 正規FFmpeg browser profile configure/library compile/vendor完了（`build/media-browser-20261012`）。
- 対象native codec/DSP・avmedia・kernel音声・workerのsyntax compile成功。初回DSP compileでNocturneにないllroundとMIN依存が発覚し、lroundと独立境界処理へ修理後成功。kernel初回compileでABI include不足が発覚し、明示include後成功。
- diff whitespace確認成功。既存改変前試験の反復は行っていない。
- 引渡し静的再点検で、pitch変更resamplerの通常pullがEOS flush扱いになる問題を発見し、push時に所有PCMへ変換・通常pullはそのspanだけ、NULL入力flushはEOSのみへ修理した。
- 新synthetic fragmented BMFF 6入力、31,860bytesとhost参照を正規生成。最初AC3 empty_moov入力はhost encoderが拒否。delay_moov条件へ修理し別名入力を生成、失敗の`mp4-ac3.mp4`は削除禁止に従って保全し受入には使わない。
- `mediaratetest.c`: 0.5/2xのPCM長・PTS・pitch維持/変更を実DSPへ通す。`media_bmfftest.c`: 直接decoderとMSE同経路へ6codecを通す。**この担当ではguest実行していない。主担当の一括guest確認待ち。**

## 未確認・限界

- 実YouTube/Shakaの同期・速度・連続再生とGUI応答、聴取、seek/rate変更、codec入力のguest結果は主担当の最新build受入が必要。syntax/host encoderを実サイト合格へ昇格しない。
- rate変更はconsumed media位置から同じ保持SourceBufferを再開する。逆再生、0.25未満/4超、AV1、HDR tone mapping/10bit physical displayは未対応。
- 已に全stream混合済みのDMA/remote PCMを1streamだけunmixする機能はない。旧PCMが新generationの時計へ混入するのは防いだが、seek直後の物理残響を完全取消できたとは報告しない。
- WSOLA長の末尾はanalysis windowの有限端点を持つ。新DSP試験はsample長・実pitchを測定し、受入前の品質保証を推測しない。
- 既存媒体/ソース/試験入力の削除なし。既存fixtureの高bitdepth拒否期待は、実pixel変換追加に対応する復号期待へ変更し、AV1/HDR拒否は保持した。

仕様参照: [HTMLMediaElementの速度・pitch](https://html.spec.whatwg.org/multipage/media.html#dom-media-playbackrate)、[FFmpeg atempo](https://ffmpeg.org/ffmpeg-filters.html#atempo)、[VP codec ISO BMFF binding](https://www.webmproject.org/vp9/mp4/)。

## 追加引渡し: native Fullscreen / Picture-in-Picture

- `web_presentation.c/h/js_presentation.js`を明確なpresentation責務として追加。任意の接続済みtop-level Elementをnative top-layerへ載せ、実WMがsw×sh・無装飾・taskbarなしへ移行する。元の通常/最大化geometryを別snapshotへ保全し、Esc・DOM removal・文書破棄で復旧。UA fullscreen寸法/box-sizingと`:fullscreen`を既存cascade/layout/paintへ統合し、DOMを複製しない。
- transient native activation、active document、native node brand、backend成功を検査し、switch時の退出失敗を握りつぶさない。JS Promise/change/error、ShadowRoot/Document retarget、composed fullscreen events、既存handlerTargetのbrand接続を追加。
- PiPは実floating GUI窓。既存owned media pixels/clock/字幕だけをaspect-fit表示し、追加fetch・decoder・media nodeを作らない。WMのalways-on-top群とkeyboard focusを分離、主窓操作時もPiPを保つ。frame/caption revisionが変わった時だけ再描画。窓resize/close、source reset、DOM removal、document退役へ接続。PictureInPictureWindowの寸法/brandはWeakMap保持し、authorの公開fieldを書き換えて偽寸法を作れない。
- 全対象Cのsyntax compileとdiff-check成功。実Fullscreen/PiPの画面/操作は主担当の最新build一括受入待ち。iframeの全ancestor fullscreen chain・複数screen選択・Document Picture-in-Picture APIは未対応として明示拒否し、空API成功にしない。
- Fullscreen namespace許可はHTMLまたはSVG svg/MathML mathに限定し、removal後のfullscreenchangeは切断済み旧nodeではなくDocumentへ送る。一次仕様は[Fullscreen Standard](https://fullscreen.spec.whatwg.org/)と[Picture-in-Picture](https://www.w3.org/TR/picture-in-picture/)を照合した。

## 実受入後の回帰修理

- 主担当のguest一括で新BMFF 232条件、DSP 22条件、CENC 11条件は成功（`build/media-regression-20261012/test-serial.log`）。DSPのpitch維持は0.5x/2xとも約440Hz、pitch変更は220Hz/880Hzで、末尾を含む実PCM長も確認。既存媒体は4カテゴリ失敗したため全面成功にはしない。
- VP9高bitdepthはpixel変換追加後も旧open_decoderのprofile0限定gateが残っていた。現在の実converter/decoderに合わせprofile0〜3へ修理し、HDR拒否は保持。
- codec fixtureのsegfaultは、4byte `tiny`の長さを33MiBだと偽って、既に廃止された32MiB固定上限の拒否を試す旧試験だった。現在のAPIはreal-size ownershipを契約し、JSはArrayBufferの実長を渡す。試験を現行unrepresentable `SIZE_MAX`の拒否へ修理し、製品へ人工上限を戻していない。再実行は主担当の変更後媒体に限定。
- 実YouTubeの84秒layout task内でnative媒体が約64秒占有と主担当phase計測で判明。協調checkpointはMSE/Rangeのproducer handoff/yieldを完全に禁止する非blocking経路へ分離。受信可能spanと送信可能commandだけ扱い、入力/鍵枯渇状態には新decode要求を繰返さない。通常browser loopの有限wait/背圧は維持。単なるthrottle値変更や機能削除で対処しない。
- 直接memory/file decoderのfuture videoが、短い音声の消費時計を超えるとEOFまでdemuxできない境界を修理。実workerと同じowned video lookaheadを適用し、後続EOF確認後に音声末尾からsilent video clockへ移す。映像を早送り表示・偽clock加算・EOF期待緩和で合格にしない。
- `avmedia.c`字幕state/paint_caption/captionCues領域は字幕担当へ責務移譲。pump/clock/seek修理とは編集範囲を分離し、caption_revisionとPiP共有描画を維持する契約。
- 待ちゼロの協調入口はouter pump、mse_next、prefetch、Range budgetの全てに接続し、同millisecond safepointをFD読取前に拒否。既存owned pending/aheadの描画は保持。全対象syntax compile成功、変更後の実受入は主担当へ引渡し。

## 第二受入のA/V不合格からの供給修理

- 主担当の変更後限定guestでは旧媒体4件とFetchが全て成功。成功済みの同じfixtureは反復していない。実YouTube Timer90は85.102秒から10.868秒へ改善したが、音声clock約18.9秒・映像PTS約11.1秒、後で約18.8秒へ飛ぶ実A/Vは不合格のまま。この結果を同期成功へ昇格しない。
- MSE子プロセスの独立音声decoderがVIDEO laneの背圧を無視して先走る供給境界を修理。音声PCM loanは、完全に転送された実画像のPTSと実durationを超える間保持し、映像ownerを進める。画像をdetachしただけ・転送途中ではhorizonを更新しない。実video EOFだけ長いaudio tailを許可し、入力/鍵枯渇では許可しない。音声時計の偽停止・巻戻し・サイト分岐はない。
- ready VIDEOを別audioのBUSYが抑止する旧選択条件を除去。ADD/SEEK/DROP/SELECT、CHANGE/ABORT、REMOVE/APPENDのdecoder revision変更で旧video horizonとin-flight commit資格を失効させる。
- 独立audioは連続PTS・native 4096frame表現内で最大4decoder chunkを一度のPCM responseへまとめる。gap、次のvideo horizon、decoder loan、有限復号量を保持し、packet毎の実IPC/copy回数を削減する。
- wire v5では実decoderが受信した全映像とlate catch-upで破棄した映像の差分を親へ伝える。decoder replacementを跨いで集計し、描画/presented/window-publishedとは区別。drop0という旧出力件数だけの診断で正常性を推測しない。
- native sinkを開いた直後、最初の実PCM投入前にwall時計だけが走るpreroll境界も修理。投入後はdeviceの実消費時計を維持する。
- 最新worker・親wire Cのsyntax compileと差分検査は成功。新wire・供給修理の実YouTube/Shaka受入は、親が全体buildと最新版媒体更新後に行う。実A/V成功、聴取品質、paint所有権はまだ未確認。
- 長いlayout taskでは、次layoutを計算中という理由だけで既存コミット画面の動画patchまで止めていた。layout開始では独立owned rasterを保持し、実paint開始で失効。publishは新box座標を一切読まず旧rasterだけ使い、接続済node/owner/live/generation/source寸法を検査。親のhost側はcanvas/viewport/page bounds/scroll/文書交代/実overlay所有を維持し、未commit layout/paint待ちだけの拒否を除去。最新avmediaのsyntax成功。実画面でwindow-publishedが進むかは一括受入待ち。
- flow媒体の実controls.pngを担当自身で目視。最初の画像/spinner/author-play後、実ログは約76.5秒taskで停止同然になった後に再開し、音声5414ms/映像5200ms、starved1/ahead2で止まるため成功ではない。全worker deadlock確定と決め付けず、協調取得の具体的なowner不整合を修理：`mse_next`のcooperative早期returnが実output slot/revision更新を飛ばしていた。通常/協調の完了境界を共通化し、先読みloanを実SourceBuffer ownerへ帰属させる。
- 空STEP/BUSYが実byte進捗を作らない間、1〜16msの有限readiness予約で同じ非blockingFD probeのhotloop反復を抑える。実VIDEO byte/実output/非STEP ACK進捗で即解除、新APPEND/KEYS/controlも即解除。正常browser loopのwait/背圧は変更せず、協調経路にsleep/yieldを追加しない。予約はprobe終了時刻から取り、probe自身の所要時間で即期限切れになる再試行ループを防ぐ。最新対象Cのsyntax compile成功、新実受入待ち。
- 同じ有限pump内の映像candidate比較は、1回の実device-consumption snapshotを共有してclock read syscall重複を除去。PCM/silence write、sink close/replacementで即失効し、pump終了時に解除。作者currentTime getterや外部観測に古いsnapshotを流用しない。
