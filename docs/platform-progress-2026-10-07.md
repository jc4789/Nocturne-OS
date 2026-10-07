# CPU・描画・メディアの統合記録（2026-10-07）

## 到達点と前提

3担当の実装を、独自ABI、GUI、RAM rootfs、`/data`永続というNocturneの境界に統合した。UNIX/POSIX実行環境、`fork()`、外部プレーヤーは導入していない。既存のdata媒体とユーザーのHyper-V VMは試験に接続・更新していない。

**日常利用OSの完成ではない。** 一般プロセスのSMP、WebGL、現代の配信サイト全般への対応は未達である。検出、ビルド、自己試験、実サイトの表示、実際の操作を区別する。

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
