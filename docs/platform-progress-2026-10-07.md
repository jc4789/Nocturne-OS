# CPU・描画・メディアの統合記録（2026-10-07）

## 到達点と前提

3担当の実装を、独自ABI、GUI、RAM rootfs、`/data`永続というNocturneの境界に統合した。UNIX/POSIX実行環境、`fork()`、外部プレーヤーは導入していない。既存のdata媒体とユーザーのHyper-V VMは試験に接続・更新していない。

**日常利用OSの完成ではない。** 一般プロセスのSMP、WebGL、現代の配信サイト全般への対応は未達である。検出、ビルド、自己試験、実サイトの表示、実際の操作を区別する。

## 第2段階の現在の到達点

以下の第1段階の記録を削除せず、最新の統合結果をここに区別して示す。旧媒体の「VP9/Opus/Vorbis未対応」「GPU能力非公開」「40成功」は、この第2段階の現在値ではない。

最新payloadは kernel `238aac650cdf8015e7d6b33082bf2ce7f5af100d7f1cd8d894fd5ca0ec79ba78`、initrd `06d72dc677077e3826fbc2139c90d02b52668ce43dc6cf4593889d9b845fe7b7`。各実行の内部payloadと専用scratchを照合し、旧候補・失敗・修理後の証拠を混ぜない。

### CPU：実GUIのRAM合成へ接続

- 不変のgeometry/title/focus等のsnapshotと借用client RAMを、限定APの非重複行cellで合成する。私有clip、同期join、batch間IRQ復帰を維持し、AP内で割当・I/O・scheduleをしない。overlayとVRAM転送はBSPのまま。
- 行数を幅・layer密度の仕事量へ正規化し、5/6/9行のceil分割がAP APIの逐次fallbackへ落ちた反例を、比例cell境界だけで修理した。強制4/5/6/9行と実GUIでmask/仕事数/画素/guardを確認した。
- 同じ仕事の合成fixtureの多層4/16/64ではdirect比約1.77/1.97/1.99倍。担当の実16窓操作では約1.73～2.00倍。ただし**RAM合成部分の測定**であり、全frame・入力latency・音声同時負荷・browser全体の高速化ではない。一窓の利益は揺れる。
- 実28→16→0窓、move/title、resize/free、42client画素、UP/nosmp、実callback fault/context保持を検査した。MAX_WIN64だけを見て63窓を期待した親/担当の判断は、MAX_FDS32による29窓上限で反証された。試験だけ28へ限定し、kernel/ABI容量を増やしていない。合成64layerと実63窓を混同しない。
- **既定は逐次のまま**。`wmparallel`はopt-in、`wmverify`/`wmbench`は比較検査。担当run最大batch1.337/1.341ms、親の独立28窓run最大1.763ms、後にも1ms超が残る。仕事量縮小はheuristicで、hard1msや無欠落を保証しない。一般scheduler SMP・user thread・AVX・一般TLB shootdownは未達。将来kernel preemption/SMPを広げるなら借用bufferのpin/refcountが必要。

### 描画：実3Dの期限内動作、速度改善は未達

- QEMUを介さないvirgl/ANGLE対照でも255～256pollの空tail fenceを再現した。追加glFlushやMT指定だけでは解決しない。同梱ANGLEの固定commitはflags無しをD3D11のDONOTFLUSHへ写す。正規`GL_SYNC_FLUSH_COMMANDS_BIT`の一行修理で、ダミー描画や期限延長なしに解決した。256の内部定数の所在は断定しない。
- `ports/virgl`の追跡builderは固定revision `2cb2065b6a0515c5accfa3e44bcb7ce57d2f9983`/1.1.0をworkspace内だけに構築する。親が空の新stageから未修正/修理版を全再構築し、元QEMUの全対象hash不変を確認した。旧新DLLの差はPE timestamp/checksum領域だけだが、byte-identical再現とは呼ばない。
- 最新OS＋親fresh修理runtimeでは全25transaction、通常期限の画素検査、**GPU19 checks/0失敗、Canvas22/0・実paint・GPU submissions2**。全体最大30.070ms、通常/validation最大16.923ms。未修理対照は通常2秒期限でcap0/DMA保持へ撤回し、CPU fallbackの画素を維持した。
- 同入力hash・400×400三角形3回のupload/syscall/readback/copy込みは **GPU47ms、CPU5ms（GPUが9.4倍遅い）**。したがって実3D機能は成立したが実用加速は未達。旧同媒体対照の47/6msは別記録として保持する。shader/512²/同期readback/Canvas opaque clearの限定は変えず、WebGL・全Canvas・Hyper-V GPUは未対応/未検証。修理runtimeと`gpu-enable`を明示しない既定起動はCPU描画。

### メディア：WebM/OggとVP9/Opus/Vorbis

- FFmpeg 9.0.2閉包を**333C・749 manifest file・9,463,614bytes**へ拡張した。原文とlicenseは元木の全hash一致、生成構成だけLF等を正規化。Opus SILK内部のgeneric swresample9Cを追加し、thread/assembler/外部codec/network/FFmpeg CLIは導入しない。
- VP9 profile0・8bit YUV420・SDR、Opus CELT/SILK、Vorbis、WebM/Matroska・Oggを実decoderへ追加した。高bitdepth/HDR・VP8/AV1等の存在しない出力能力を宣言せず、圧縮負例と`canPlayType`文字列を検査した。
- 最初のnative試験は全Opusが9600ではなく9312framesとなり失敗した。`pkt_timebase`欠落と非active seek sentinel/負PTSによる二重trimをadapter2箇所だけで修理。元decoder・圧縮fixture・9600期待を変えず、先頭PTS0とseekを確認した。
- 最新全体回帰の旧codec73982/0・旧browser36/0、新codec148113/0・新browser51/0。file/所有memory、VP9画素、Opus9600、Vorbis、EOF後seek、doc lifetime、pause/seek/flush・拒否を通過。
- 最新の実HTTP `/bin/browser`は自作HTML＋VP9/Opus WebM＋Vorbis Oggの3GETを取得し、各bytes/SHAを照合。96×64の最終frame/controls、ENDED0.4/0.4と0.2/0.2、DONE、JSエラー無しを画面とconsoleで確認。AC97録音は左右444/656Hz・0.20秒を**2回**、厳密解析で確認した。短い自作clipの正例であり、人間の聴感・実サイト配信・長時間品質ではない。
- BSP software decode、簡易resample/先頭2ch、4decoder/文書、64MiB入力/文書、32MiB/所有memory入力、既存fetch実効16MiB、総decoder heap予算未保証を維持する。MSE/DRM/HLS/DASH/Range/live、HEVC/AV1等と長時間高解像度品質は未達。

### 最新全体回帰と再現

WHPX4/2GiB、`scripts/test.py --quick --no-net`全groupを**69成功・0失敗・9省略**で完走。FAT118files整合、strict8音録音、QEMU終了0。`jstest`927 checks/0失敗を含むnative web経路も検査した。省略はquickのOS内全アプリ再compile/tcc self-host2件と、明示除外した外部network7件。ローカルTCP/worker/容量回復は実行済み。bootはsnapshot、回帰dataは新規専用の書込可能scratch（FAT検査用）で、既存dataではない。他のplatform QAはboot/scratch両snapshot。証拠は`build/nocturne-platform/stage2-release-regression`、`root-proportional-independent.json`、`root-stage2-current-acceptance.json`。

親の最新単CPU/512MiB `stage2-root-up512`でもCPU情報detected=online=scheduler=1、jobs=0、旧新codec73982/148113・旧新browser36/51・Canvas22の全失敗0、実paint、QEMU終了0を確認した。4CPU/2GiBの全groupと音声8音検査を512MiBでも全部実行した意味ではない。

```text
python -X utf8 scripts/build.py all
python -X utf8 scripts/test.py --quick --no-net --accel whpx --cpus 4 --memory 2048
python -X utf8 ports/ffmpeg/check-vendor.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2"
python -X utf8 ports/ffmpeg/prepare.py --profile webm --stage build/media-reproduce-new --source "D:/Programes/cloned repos/ffmpeg-9.0.2"
python -X utf8 scripts/platform_qa.py --label own-ram --accel whpx --cpus 4 --cmdline wmverify --command "tcc -o /home/wmramtest /data/tests/wmramtest.c && /home/wmramtest --many" --command "sleep 10" --seconds 180
```

stageとlabelは新規名にする。host修理の再構築/依存/BIOS読み取り専用指定は`ports/virgl/README.md`に記録した。自動installerや元配置へのDLL上書きはしない。最終Hyper-V実機・Enhanced Session・日常OS完成は引き続き未検証/未達である。

### 最新の指定実サイト（各75秒、実HTTPS・Nocturne自身）

`stage2-latest-{baike,amazon,openai}`は全て上記238aac/06d72を起動し、QEMU終了0と画面を親が確認した。期待markerを指定しないサイトharnessの受理は**サイト互換成功ではない**。

| 指定URL | 実画面・通信/JSの観察 | 未達 |
|---|---|---|
| `https://baike.baidu.com` | 4,117,334bytesのscriptを実行、検索欄・中国語本文/画像card表示 | 未処理promiseとAxios API404が継続、検索/全記事操作の受入れ未 |
| `https://amazon.com` | `www.amazon.com`へredirect、header・検索欄・広告/商品画像を部分表示、native MP4経路も呼ばれる | 左cardの緑の画像崩れ、not-a-function、未実装crypto等。購入/loginは実行せず、全面操作成功ではない |
| `https://openai.com/ja-JP/index/gpt-6-astra/` | exact URL、OpenAI logoとCloudflare待機画面 | document参照例外、本文未到達。challenge迂回はしない |

Amazon画像候補4件はhostとNocturne decoderの寸法/ARGB hashおよび実HTTP入力が一致したが、緑cardの実assetとの対応は特定できていない。これをAmazon画像修理と呼ばない。API404/DOM/CSS/crypto/iframe/実画像の境界は、CPUやcodecの追加だけでは閉じなかった。次は採取した実サイト失敗を狭いnative負例へ落として原因を証明する。一般SMPや配信機能を同時に際限なく追加しない。

## 第1段階の記録（以下は旧媒体の履歴）

## CPU

- Limine MPでCPUを検出し、BSPを含め最大16 CPUまでAPを同期型の純計算workerとして利用する。APごとのGDT/TSS/例外stack、SSE2、kernel CR3、HLT/IPI待機を用意した。
- `cpu_parallel_for`のcallbackは信頼するkernelコードだけ。割当・解放・I/O・syscall・待機・schedule・再入は禁止。BSPがjoinを終えるまでcontextと入力を保持する。
- task scheduler、通常プロセス、IRQ、allocator、QuickJS、FFmpegは引き続きBSPで動く。AVX、一般TLB shootdown、x2APIC、user thread SMPは未対応。
- `cpu_info`とSystem Monitorは検出CPU、online CPU、AP worker、scheduler=1を区別する。既存の`n_sysinfo`を改変せずsyscallは末尾に追加した。
- 担当の同一kernelによるWHPX4/1、nosmp4、TCG4、AP誤使用負例では、実仕事・SSE2・端数/ゼロ仕事・joinを検査した。AP fault後はcontextを保持したpanicで停止し、危険な逐次fallbackに戻らない。
- framebufferの同一RAM→VRAM仕事は、WHPX4で並列4916435 ticks、逐次2051691 ticksだった。**約2.4倍遅いため既定は逐次**。`fbparallel`/`fbbench`だけが並列転送を有効にする。自己試験のserial/parallel ticksは仕事量が違うため速度比較に使わない。

## 描画

- secondary modern PCI virtio-gpu/virglへの有限driverを追加した。固定shaderのclear/色付き三角形、最大512×512、fenced readbackだけ。任意shaderやcommand streamは受け付けない。
- Limine framebufferとHyper-V/RDPの表示経路を置換しない。GPU失敗時はDMA領域を保持し、能力を撤回してCPU表示を維持する。
- `gpu-enable`は明示opt-in。cold自己試験だけでなく通常2秒の期限で画素検査が成功するまでGPU能力を公開しない。
- 同じdriverでbootとuserから実clear/三角形/readbackの画素を確認したが、ホストのfence転送に約2.6～4秒の待ち周期を採取した。待ち時間を延長するだけでは加速完成としない。virglのhost GL実装・同期方式と物理GPU利用は別の検証事項である。
- capsetが返すhost renderer名はANGLE/NVIDIA RTX4070/Direct3D11だった。ただし実用性能は証明しない。QEMU子プロセスだけで`VIRGL_DISABLE_MT=1`とした対照でも約2.63秒周期は変わらず、通常期限検査で能力を非公開にした。最終の通常MT対照も同じ結果。GPUなし/撤回後の同一400×400三角形3回はCPUで5ms、画素/canary成功だが、使えないGPUとの速度倍率は算出しない。
- `gfx_render3d`の返すbackendはvirglとCPUを区別する。CPU rasterizerはGPU対応の証拠ではない。
- Canvasは実RAM bitmap、native paint、限定Canvas2Dを追加した。矩形/path/curve、transform/save、ImageData、寸法/同値寸法変更とresetを検査する。1 Mpixel/bitmap、16 MiB/document、256 path点が上限。WebGL/WebGL2は`null`。drawImage、text、clip、Path2D、exportなどは未対応。

## メディア

- ローカルFFmpeg 9.0.2の限定C実装280 sourceとinclude閉包659 fileを取り込んだ。元FFmpeg木は読み取りのみ、vendor原文hashを再照合した。FFmpeg CLI、network/protocol、threads、assembler、encoder/filter/deviceは含めない。
- 実decoder: PCM、MP3、FLAC、AAC、8bit H264、MJPEG、rawvideo。container: WAV/MP3/FLAC/ADTS/MP4/AVI。
- custom AVIOはNocturneのread/lseekまたは所有メモリーを使う。出力は48 kHz/16bit stereoとopaque ARGB。GUI Media Playerとブラウザのaudio/videoが同じnative decoderを使う。旧WAV Sound Playerは維持した。
- ブラウザは既存fetch/CORS→全体取得→native decoder→audio/frameの経路。Audio/HTMLMediaElement/HTMLVideoElement、play/pause/seek/volume、controlsを限定実装した。世代、再入イベント、pending play取消、document解放、未来frame先読みの所有権を検査した。
- 音声のpause/seek/unloadは専用`audio_flush(fd)`で未混合queueを捨てる。無効fd/非audio/dup共有queueを実検査する。すでにAC97 DMAやremote clientへ送った約60～70ms分は取り消せない。普通のcloseのdrain挙動は変えない。
- FLAC seektable無しのEOF後seekを、入力先頭から再生成する限定fallbackで修理した。audio/videoのseek targetは別々に扱う。最後のvideo frameは、既知durationの残り1秒以内だけ保持し、不正な巨大durationを無期限に待たない。
- browser入力は1つ32 MiB、documentは64 MiB/4 decoder/64 media状態。8 streams、約2.36M pixels、8 audio channelsが上限。native fileの逐次IOに32 MiB制限はない。64 MiBのallocation上限はdecoder heapの総量保証ではない。
- resamplingは簡易線形、surroundは先頭2chで本格downmixではない。MSE、DRM、HLS/DASH、Range/live、autoplay、字幕、1x以外、10bit H264、HEVC/AV1/VP9/Opus/Vorbisは未対応。長時間・高解像度・同期品質は未検証。

### ソースと再リンク

`third_party/ffmpeg/manifest.json`、`sources.mk`、`COPYING.LGPLv2.1`、`LICENSE.md`と`ports/ffmpeg`に限定構成・由来を保存した。sysrootにもライセンス、manifest、`README.nocturne.md`を同梱する。IJG由来部分のcreditはこのREADMEに記載している。静的再リンク用objectを`build/libc-objects.list`とsysrootのlibc.aに用意する。配布時はこのソース・構築手順・ライセンスを省かない。

```text
python -X utf8 scripts/build.py all
python -X utf8 ports/ffmpeg/check-vendor.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2"
python -X utf8 ports/ffmpeg/prepare.py --source "D:/Programes/cloned repos/ffmpeg-9.0.2"
```

`prepare.py`は空白を含む元パスを直接upstream configureへ渡さず、workspace内のstageで再生成する。元木を変更しない。既定stageの生成configはvendor構成とbyte一致を確認した。既存のstageを消さず再生成前に保存する。

## 実行した試験と残る課題

実QEMUは`C:/Program Files/qemu/qemu-system-x86_64.exe`、11.1.0。WHPXを実際に起動し、4 CPU/2 GiBを使用した。TCGは明示指定の対照で、WHPX失敗時に黙って切り替えない。hostのCPU clock、driver、Windows設定は変更していない。

証拠は`build/nocturne-platform`以下に保持する。QEMU媒体からkernel/initrdを取り出し、現在のbuildとのSHA256一致を確認する。専用scratch diskとsnapshotだけを接続する。

- 初期統合でFLAC seek、Canvas同値reset、GPU待機が実際に失敗した。失敗ログを残し、修理後のnative経路を再実行した。
- 中間総合は38 PASS/1 FAIL/2 SKIP。TCP枠試験は単独36 checks/0 failureだが、直前worker試験後に15枠までしか確保できなかった。kernelの既存20秒FIN生存期間より準備時の5秒待ちが短い試験順序依存と再現した。準備だけ25秒待ちとし、満杯時の150ms期限・取消・kill・回復のassertionは維持した。
- 中間のcodec/browsing試験は通過したが、browser FLAC fixtureの30ms再生音が既存8音の録音試験へ混入した。fixtureをmuteし、既存の期待音数を緩めず再試験する。前の録音はnative decode→AC97到達の証拠として保存した。人間の聴取やA/V同期品質とは別である。
- EOF負例の追加では、試験が4 decoder上限を超えて失敗した。完了した要素を明示unloadして修理し、製品上限は緩めなかった。
- **最終影響範囲: WHPX4/2 GiB、40 PASS・0 FAIL・2 SKIP、FAT整合成功、厳密な8音の実録音成功、QEMU終了0。** `build/nocturne-platform/release-regression`に不変ログ・録音・媒体hashを保存した。内訳はcodec 73982 checks/0 failure、browser生命周期36 checks/0 failure、Canvas22 checks/0 failure/painted=1、TCP容量36 checks/0 failure。2 SKIPはquickで省く全アプリin-OS再compileとtcc self-host。hostでの全アプリ構築は成功している。
- 直前の全group試験は66 PASS/1 FAIL/2 SKIPで、shell/tools/filesystem/agentも実行した。1 FAILは上記のdecoder枠を超えた試験。修理後は影響範囲を最終媒体で再実行し、全group一式をもう一度通したとは主張しない。
- rootの独立UP/512 MiBでCPU stress・codec73982/0・Canvas22/0、nosmp4でdetected4/online1、AP誤使用でfault6/context retainedを確認した。Hyper-Vの実VMとは別である。
- browser生命周期試験はfixture callbackを使うnative DOM/JS/decoder/paint試験で、TCPの代わりではない。別の`final-media-http`では実HTTP→Nocturne browser→H264/AAC→GUI frame/AC97を確認し、終了time=duration=0.4、録音444/656Hz・0.20秒を得た。native Media Playerでも実frameと初回再生を画面・同じ録音で確認した。キーによる再生再開は録音に2回目の音がなく、操作成功は確認できていない。合成clipの限定正例で、実サイト配信や長時間品質とは区別する。

### 指定実サイト

実ブラウザでHTTPSとsite scriptを実行し、QEMU画面を採取した。外部ブラウザでの成功や静的HTML取得だけをNocturneの成功にしていない。

| サイト | 観察 | 未達 |
|---|---|---|
| 百度百科 | 4 MB級bundle実行、本文・画像・検索欄が表示。`document.referrer`欠落による前の例外を修理 | API 404、全検索/記事操作の受入れは未確定 |
| Amazon | header、検索欄、商品/広告の部分表示。追加native MP4経路も実際に呼ばれた | 画像/配置の崩れ、CSS/JSのnot-a-function、未実装crypto操作。購入・loginは試していない |
| 指定OpenAIページ | exact URLに接続しOpenAI logoとCloudflare待機画面を表示 | challengeでdocument参照例外、本文未到達。challengeを迂回しない |

license同梱後の最終媒体でも`release-baike`、`release-baike-article`、`release-amazon-search`、`release-openai`を実行し、全QEMUの終了0と画面を確認した。百度のLinux記事では見出しと本文冒頭まで表示するが、動画枠は黒く、未対応constructor/関数に関する例外が残る。Amazonの検索URLはBooks storeへredirectし、header/footerは表示するが商品cardが空でlength参照例外が残る。検索が正常だったとはしない。OpenAIは同じchallenge未到達である。ホームのpartial表示より強い操作経路でも、全面互換の未達を確認した。

表示の部分成功を全面動作とは呼ばない。Hyper-V Gen2/Enhanced Session、実物GPU、実サイト動画の安定再生は未検証で、ユーザーのHyper-V検証と分ける。

## 再試験

```text
python -X utf8 scripts/test.py --quick --no-net --accel whpx --cpus 4 --memory 2048 mem tcc gui audio media web
python -X utf8 scripts/platform_qa.py --label own-baike --accel whpx --cpus 4 --url https://baike.baidu.com --seconds 90
python -X utf8 scripts/platform_qa.py --label own-gpu --accel whpx --gl --cmdline "gpu-enable gpu-timing" --command "tcc -run /data/tests/gputest.c --require-gpu --bench" --seconds 120 --expect "gputest:" --reject "FAIL"
```

ラベルは未使用のものを選ぶ。`harness_accepted`は指定marker・終了コード・panic等の検査であり、実サイトや可聴性の受入れ判定ではない。GUIの画像と操作結果は別に読む。

次は一般scheduler SMPを急いで広げるより、実サイトのDOM/CSS/crypto/iframe、画像の描画不具合、streaming media、実GLの待機原因を、それぞれnative実経路の負例付きで進める。OS全体の日常利用完成は未達として保持する。
