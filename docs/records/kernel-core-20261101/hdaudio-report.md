# hdaudio の報告

## 依頼の全文

あなたの名は「hdaudio」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の今の先頭（5043e3e6 の上に記録だけ）。自分の作業木が古ければ、まず main を取り込む。QEMU のポートは HMP 45480・QMP 45481。

### 欠陥

実機で音が出ない。`kernel/src/dev/audio.c` の `audio_init` は `vm_audio_init()` だけを呼ぶ（注釈「no driver for real sound hardware yet」）。`audio_set_card` の呼び手は `kernel/src/vm/ac97.c` だけ。今の PC の音は HD Audio（マザーボードの codec、GPU の HDMI・DisplayPort の音）と USB Audio。

### 仕事

Intel High Definition Audio の driver を実機のために書く（仕様は `D:\Programes\OS DEV INFO\high-definition-audio-specification.pdf`）。

- 制御器：CORB・RIRB、stream descriptor、BDL、MSI または MSI-X（`arch/irq.c` の口）、DMA は `dev/iommu.h` を通す。制御器の数と codec の数に固定の上限を置かない。AMD の Zen の制御器と NVIDIA の GPU の HDMI の制御器の両方が同じ道で動く形。class code で探し、製品の ID の決め打ちをしない。
- codec：widget の木を歩いて、出力の道（DAC → mixer・selector → pin）を探す。pin の configuration default、jack の検出（unsolicited response）、音量の amp の capability は装置の申告から取る。
- 複数の出口：今の `audio_set_card` は card を一つと決めている。複数の card と複数の出口が持てる形に `audio.c` の抽象化を直す（AC97 も同じ口で）。形式も装置の申告から（48 kHz の決め打ちは `audio.h:21`・`abi.h:569` にある）。出口の選択そのものの設定の窓は設定の層の書き手が後で通すので、今は、音の出口を列挙できる内側の口と、既定の選び方（jack の刺さった出力を先に）までを作る。
- 時間の道：割り込みの中で mix する今の形（`audio_card_mix`）が、割り込みを止める区間として大きくないかを読み、音が締切を落とさない形にする。監査 `audit-compositor.md` の音の項を読む。別の書き手「mediaclock」が音の供給と時計の遅れを調べている（`kernel/src/dev/audio.c` を触る可能性がある）ので、played の報告の意味（`audio_played`）は変えない。
- 電源：`dev/pm.h` の quiesce と resume。

確かめ：QEMU に `-audiodev dsound,id=snd -device intel-hda -device hda-output,audiodev=snd` を付けて起動し、`audio:` の行、ブラウザーで実サイトの動画（`https://www.youtube.com/watch?v=vuYLCMyzFD4`）か Sound Player の音が進むこと（出口の played の数で。ホストのスピーカーから音が出る設定で一度確かめる）。AC97 の道も壊さないこと。実機の codec は QEMU に無いので、`hda-duplex`・`hda-micro` でも起動を見る。widget の木の解析は host の検査で実機の形（複数の pin、複数の DAC）を試す。報告は `docs/records/kernel-core-20261101/hdaudio-report.md`。

### 途中で来た指示

1. ユーザーの言葉 "Here is some grounding info for your subagents that are working on video and sound"。画像の値：音の sample clock は ppm 以下、多くの DAC は受けた信号を自分で打ち直す。映像は 1 コマの精度でずれを溜めない。音と映像の同期は音が先なら 15〜22 ms まで、後なら 45 ms まで、人は音が先に敏感で 10〜15 ms でも不自然に感じる。媒体の内部の時計と PTS は ±1〜±10 ms で、ずれが長い再生で溜まらないこと。今の作業の物差しにこの値を入れて、報告に測った値と並べる。
2. ユーザーの言葉 "that is not tight enough, we need to set the hardest standards for ourslevs the the easyest. 99.9% accuracy is the absolute bare minimum"。値に幅があれば一番厳しい端を線にする（音の sample clock は ppm 以下、媒体の時計へ渡す音の位置は ±1 ms）。音の位置は装置が実際に出した sample の位置を公式の文書の手順で読んで渡し、送った量からの推定で代えない。線に届かないものや測れないものはそのとおり書く。
3. ユーザーの言葉 "there should never be a build up of drift at all ever."。溜まりはどんな長さの再生でも 0 が線。99.9% は短い窓の最低の線。確かめる形：報告する位置と装置が実際に出した sample の差が時間の経過で増えず、決まった幅の中に留まること。実機でしか確かめられない部分は確かめていないと報告する。
4. Media Foundation の Presentation Clock と IAudioClock::GetPosition の事実。位置の読み方は対象の装置の公式の文書で決める。
5. ユーザーの言葉 "claude you know there are ways to fix dritf."。音の装置の時計と TSC の差の溜まりも 0 が線。直し方の例は、装置の位置と counter の組から二つの時計の比を測り続け、音の resample の比へ返す閉じた輪。driver が持つのは位置と counter の組を正しく読んで渡す所。比の補正の持ち主は音の道のコードを読んで決める。`mediaclock` の書き手と同じ責務を二か所に作らない。
6. `dev/audio.c` の `RUNS` 32 の根拠は AC'97 だと別の書き手の報告にある。真偽はコードと文書で確かめ、実機の音の driver が必要とする形へ決める。

## 結論

- 実機のための HD Audio の driver を書いた。QEMU の `intel-hda` で動画の音が出口まで進む。実機では動かしていない。
- 音の層は複数の出口を持つ形になった。AC97 も同じ口を通る。
- 比の補正の持ち主は mediaclock の `audio_clock_track`（`user/libc/web/avmedia.c`）と決めた。この枝に輪は作っていない。driver は LPIB と時計の組を `audio_played` に渡す。
- 溜まり 0 と ±1 ms の線は、QEMU では確かめられない。QEMU の LPIB は実際に出した sample と一致しない。実機の LPIB は確かめていない。

## 設計

### 制御器（`kernel/src/dev/hda.c`）

- PCI class 04h・subclass 03h の全部の function を `hda_init` が拾う。製品の ID で分ける所は無い。AMD の制御器も NVIDIA の HDMI の制御器も同じ `controller_add` を通る。
- 制御器ごとに `struct hda` を持つ。制御器の数に上限は無い。codec は仕様の番地の数（15）だけ見る。
- verb は CORB と RIRB を一つずつ通し、答えは polling で待つ。RIRB の割り込みは、pin の知らせ（unsolicited response）を制御器の thread へ伝える。
- 流れは制御器ごとに一本で、32 block（各 256 frame、5.33 ms）の輪を歩く。block の終わりごとに割り込みが来て、音の thread を起こす。
- 割り込みは MSI か MSI-X、無ければ INTx、どちらも無ければ alarm で補う。
- DMA の記憶は `pmm_alloc_contig` の 4 GiB より下から取る。64OK の機能は任意だからである。

### codec（`kernel/src/dev/hda_codec.c`・`hda_codec.h`）

- verb だけで widget を読む。制御器を知らないので、host で模擬の codec を相手に試せる。
- 出口は configuration default が出力と言う pin で、そこから mixer と selector を辿って 48 kHz PCM を受ける converter までの一番短い道を持つ。
- 同じ association の pin は一つの流れの channel なので、種類ごとに一番小さい sequence だけを出口にする。association 15 と 0、ヘッドホン、別々の表示器への pin はまとめない。
- amp は capability の申告で 0 dB にする。形式の bit 数は converter の申告から取る。
- 仕様 7.3.4.7 の R7 により全部の codec に 48 kHz があるので、resampler は持たない。
- jack は presence detect で読み、表示器の pin は ELD が有効なときを刺さったと数える。知らせない pin は 1 秒ごとに見る。
- widget の遅れは 7.3.4.5 の group の Output Delay、0 なら 7.3.4.6 の道の各 widget の和で取る。

### 音の層（`kernel/src/dev/audio.c`・`audio.h`）

- `struct audio_sink` と `audio_sink_ops{open,close,fill,unissue}` が driver の口である。
- 列挙は `audio_sinks`、選択は `audio_sink_choose`。既定の順は `sink_rank` で、刺さったものが先、次に刺さる要らないものや分からないもの、最後に空の jack。同じ順位ではヘッドホン、スピーカー、line、表示器、digital の順。
- 混ぜは割り込みの中でなく `audio_thread` で行う。`audio_lock` は位置だけに持ち（`mix_claim`）、合計は錠の外で 64 frame ずつ行う（`mix_sum`・`mix_pack`）。`stream_close` は `held` が 0 になるのを待つ。
- `AUDIO_RATE` はストリームと混ぜの速さとして残す。出口の形式は装置の申告である。
- `audio_played(ordinal, at)` の意味は変えていない。

### `RUNS`

- 真偽：`RUNS` 32 の根拠は AC'97 だという報告は、コードの値と合う。`kernel/src/vm/ac97.c` の `NBUF` が 32 で、BDL の 32 entry は AC'97 の輪そのものである。HD Audio の `AHEAD_MOST` は 16、RDP の `snd_sent` は 256 で、どちらも 32 を決める者ではない。`#define RUNS` の行に根拠の注は無く、git の履歴の 4b597384 より前は辿っていない。
- 一本の流れの run は、隣り合えば一つに合わさるので、出口が持つ block 一つにつき多くて一つである。要る数を決めるのは出口なので、固定の定数をやめた。
- 形：`struct audio_sink.blocks` と `struct audio_remote.blocks` を持ち主が申告する（AC'97 は `NBUF`、HD Audio は `BLOCKS`、RDP は `snd_sent` の数）。`runs_grow` が全部の流れの `issued` を 2 の冪で広げる。確保は錠の外で行い、差し替えだけを `audio_lock` の下で行う。
- 開始の数は `NULL_RUNS` 2 である。音の無い sink は次の block を取る前に報告するので、再生中の block とその次で足りる。そのために `null_play` の順を直した。
- 記憶が足りないとき、その流れは run が空くまで block を飛ばす。流れが止まるだけで、panic はしない。

### 時間の道と位置の組

- 位置は `position_update` が `SD_LPIB` を読み、その直後に `clock_now_ns` を読む。二つの読みは `engine` の spinlock の下で割り込みを閉じて行う。
- 周回の数だけ、半周以上遅れたときに時計で補う。位置そのものを時間や送った量から足し上げない。
- 報告の値は `audio_played(ordinal_at(pos - delay), last_at)` である。`delay` は pin までの widget の遅れ（`hda_codec.c`）である。
- ELD の Aud_Synch_Delay は映像と音の差の値で、今は使っていない。

### 比の補正の持ち主

- `user/libc/web/avmedia.c` の `current` の媒体の時計は、ストリームの `queued` と `device_pending` から求める。この値は `audio_played` の報告から来る。
- mediaclock の作業木（`agent-a2db792d954e915d5`）を読んだ。`audio_clock_track` が単調時計を媒体の時計とし、`played_frames` と `played_at_ns` の組から差を求め、`nmedia_trim` で PCM を ppm 単位で伸ばす閉じた輪を持つ。ABI に `played_at_ns` を足している。
- よって輪は mediaclock の一つとし、カーネルの混ぜに二つ目を作らない。この枝は位置の組を正しく読んで渡す所までを持つ。
- この形で輪が届かないもの：ブラウザーの媒体の道の外の流れ（`user/apps/player.c`、`user/apps/mediaplayer.c`、Web Audio、`sound_effect`）は装置の時計のまま進む。線をここにも当てるなら、混ぜの層か各々の持ち主へ輪を置く判断が要る。

### 電源と claim

- `hda_quiesce` と `hda_resume` を `pm_register` で登録する。quiesce は `engine` の下で `power_gen` を進めて止める。各 thread は書く前に世代を見るので、止めた後に装置を動かさない。
- GPU の音の function が表示の function と同じ isolation group にあるとき、`struct iommu_guest` で「claim の間止まり、claim の domain に記憶を写して再開する」形にした。`iommu_guests_stop`・`iommu_guests_start` を `attach`・`claim_form`・`claim_release` が呼ぶ。`bus_reset` は `kernel_owned` の相手がいるとき `!ours` を見る。

## 変えたファイル

| ファイル | 内容 |
|---|---|
| `kernel/src/dev/hda.c` | 新規。制御器。 |
| `kernel/src/dev/hda_codec.c`・`hda_codec.h` | 新規。codec の解析と道の設定。 |
| `kernel/src/dev/audio.c`・`audio.h` | 複数の出口、`audio_thread`、錠の外の混ぜ、`blocks`、`runs_grow`。 |
| `kernel/src/vm/ac97.c` | 新しい口へ書き直し。hard handler は SR を消して起こすだけ。 |
| `kernel/src/vm/rdp.c` | `snd_remote.blocks` の申告。 |
| `kernel/src/dev/iommu.c`・`iommu.h`・`pci.h`・`pciuser.c` | `iommu_guest`。 |
| `common/abi.h` | `n_audio_output` のコメントだけ。 |
| `docs/nocturne-native-abi.md` §9、`.claude/rules/map.md` | 今の形へ直した。 |
| `tests/hda_codec_hosttest.c`・`tests/check_hda_codec.py` | 新規。host の検査。 |

## 消したもの

- `audio_set_card` と `card_*` の一式、割り込みの中で mix する `audio_card_mix`。
- `audio_trace_*` と `trace`。
- `audio_init` の注釈「no driver for real sound hardware yet」。
- `RUNS`。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：カーネルの警告は 0。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：成功。`abi.h` を直した全体の組み直しで警告は 57 で、カーネルと今回のファイルには 0 である。見本は `user/libc/font.c:305` の `-Wmissing-field-initializers` で、今回の変更の外である。全文は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\hdaudio\build_final.log`。
- 出力は `D:\Programes\llm vibe slop os\build\wt\agent-a33c8b225fc600edd\nocturne.img`。
- host の検査：`python -X utf8 tests/check_hda_codec.py --out <場所>` は `HD Audio codecs: all cases hold`。試した形は desktop（複数の pin、複数の DAC、mixer と selector、jack、association）、graphics（表示器の pin が複数）、loops、formats、silent の 5 つ。遅れの計算も入る。

## QEMU で見たもの

ファイルの場所は全部 `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\hdaudio\` の下である。同じ動画 `vuYLCMyzFD4` を、窓付き、q35、UEFI、NVMe、`-cpu host`、`-accel whpx`、4 CPU、4 GiB で開いた。

依頼のポート 45480・45481 は他の python が持っていたので、45490・45491 を使った。

### 最後の媒体

| 回 | 音の装置 | serial.log の行と値 | 画像 |
|---|---|---|---|
| final.log | `intel-hda` + `hda-output` + wav 48 kHz | `audio: output "line out", 48000 Hz, 16 bits, 2 channels`、`audio: HD Audio at 00:05.0 (8086:2668): codecs 1, 1 outputs, message interrupts`、`audio: output "line out" stopped: played 2212864 frames, link at 48463 Hz, 1 late, 0 stream errors`。wav は 2264053 frames で、音の途中の無音は 4 ms×1、9 ms×1、14 ms×1、162 ms×1。動画の画が進む。 | final.png |
| ac97only.log | `AC97` + dsound | `audio: output "AC'97", 48000 Hz, 16 bits, 2 channels`。guest の 10.03 s に clock 10.04 s（100.1%）、48034 frames/guest s。動画の画が進む。 | ac97only.png |
| ac97final.log | `AC97` と `intel-hda` + `hda-micro` + dsound | AC'97 の次に `audio: output "Speakers", 48000 Hz, 16 bits, 2 channels` へ移る。スピーカーが line より先という既定の順のとおり。guest の 10.01 s に clock 10.26 s（102.5%）。 | ac97final.png |
| two2.log | `intel-hda` 二つ、codec 計 3、`AC97`（audiodev none） | `audio: HD Audio at 00:05.0 (8086:2668): codecs 1, 1 outputs, message interrupts`、`audio: HD Audio at 00:06.0 (8086:2668): codecs 3, 2 outputs, message interrupts`、名前の重複は `line out 2`・`line out 3`。 | two2.png（デスクトップまで） |

### 前の媒体（RUNS を直す前で、`position_update` を `engine` の下にする前を含む）

| 回 | 音の装置 | serial.log の行と値 | 画像 |
|---|---|---|---|
| hda.log | `intel-hda` + `hda-output` + dsound（依頼の形） | `audio: output "line out"`。clock 13332 ms@73.27 → 45135 ms@103.307（105.9%）。played 2260736 frames、7 late、0 stream errors。 | yt1.png、yt2.png |
| ac97.log | `AC97` + dsound | written 47975 frames/guest s（99.95%）、clock 4.00 s/4.00 s。 | ac97.png |
| hdawav.log | `intel-hda` + wav 44.1 kHz | clock 11028@43.87 → 41603@73.93（101.7%）。played 2195968、0 late。 | なし |
| wav48.log | `intel-hda` + wav 48 kHz | `stopped: played 2216192 frames, link at 48379 Hz, 0 late, 0 stream errors`。clock 13987@49.06 → 38137@73.08（100.5%）。wav は 2248693 frames、最後の非ゼロは frame 2202368。 | なし |
| duplex.log | `hda-duplex` | `line out`。pause の 1 s 後に stop（played 1522176、2 late）。再開し、`l` で 10 s シーク（clock 33875 → 45788）。 | duplex1.png、duplex2.png |
| micro.log | `hda-micro` | `"Speakers"` で起動。 | micro.png |
| iommu.log | `-device intel-iommu,intremap=off` + `intel-hda` + dsound | `iommu: VT-d on, 1 unit, …: every function passes through until an app …`。VT-d が有効でも HD Audio が起動し、動画が進む。clock 27167 ms@61.54 → 37271 ms@71.56（100.9%）。 | iommu.png |
| two.log | 複数の制御器の前の回 | 最初の回は serial 47 行で止まり、HMP の quit も効かず、自分の pid を止めた。音の backend を一つ共有した形だった。backend を三つに分けた後の two.log と two2.log は止まっていない。原因は分からない。 | なし |

### ホストのスピーカー

依頼の dsound の設定で走らせた。自分は音を聞けない。音の内容は wav の出口の peak で確かめた。

## 線と測った値

| 線 | 値 | 測った値 | 判定 |
|---|---|---|---|
| 音の sample clock | ppm 以下 | QEMU の LPIB は guest の時計に対し 48379〜48463 Hz（wav 48 kHz）、clock で 105.9%（dsound）、101.7%（wav 44.1 kHz）、100.5%（wav 48 kHz）。この比は QEMU の出口を替えると変わるので、QEMU の intel-hda の進みである。実機は測っていない。 | QEMU は届かない。実機は未確認。 |
| 短い窓の時計の進み | 実時間の 99.9% 以上（上の線は下の溜まり 0） | AC'97：100.1%（10 s）、99.95%（前の媒体）。HD Audio：final.log で 4691 ms@42.300 → 38436 ms@76.380 は 99.0%。この窓には 162 ms の無音と 1 late が入り、内訳は分けていない。 | AC'97 の 99.95% は届く。HD Audio は QEMU で届かない。 |
| 媒体の時計へ渡す音の位置 | ±1 ms | 読みは LPIB と時計を `engine` の下で続けて読む。報告の間隔は 5.33 ms で、その間の位置は `played_at_ns` の組で mediaclock が扱う。組の読みの間隔は測っていない。wav と比べる回では、author-reset のとき clock 46143 ms（2214864 frames）に対し wav の最後の非ゼロ 2202368 frames で、差は 12496 frames（260 ms）だった。この差は QEMU の LPIB が出した sample と一致しないことを含む。 | QEMU は届かない。実機は未確認。 |
| 位置と実際の sample の差の溜まり | 0 | 設計：位置は毎回装置から読み、足し上げない。QEMU の 260 ms は 46 s の 1 回で、時間とともに増えるかは測っていない。実機は確かめていない。 | 証明していない。 |
| TSC と音の装置の時計の差の溜まり | 0 | 輪は mediaclock の `audio_clock_track` が持つ。この枝に輪は無いので、枝の単体の値は補正なしの上の値である。mediaclock の `CLOCK_TRIM_PPM` は 1000 で、QEMU の intel-hda の 100.5%〜105.9% はこの幅の外にある。 | この枝の単体では届かない。mediaclock と合わせた測りは未確認。 |
| 音と映像の差 | 音が先 10〜15 ms の感知の値より厳しい ±1 ms | 媒体の時計を音の位置から読むので映像は音に従う。presented と clock の差は iommu.log で 0〜27 ms（`clock 37271 ms, presented … at 37267 ms`、`clock 35227 ms, … at 35200 ms`）。これは表示した PTS と時計の差で、耳と目の差ではない。 | 測った範囲で 27 ms の行があり、線に届かない。 |

## 残った欠陥と未確認

- 実機の codec と実機の制御器では一度も動かしていない。LPIB の精度、jack の検出の実物、MSI と MSI-X の実物は実機でしか確かめられない。
- NVIDIA の HDMI は未確認。表示の driver が audio を有効にする手順、GPU の reset が HD Audio に及ぶこと、ELD の中身を見ていない。
- `iommu_guest` は QEMU で試していない。`tests/pcitest.c` は媒体に載らず、`appdrivers=any` を起動の項目に入れる道具も無かった。
- HDMI と DisplayPort の InfoFrame の値（CEA-861 と DP）は手元の文書に無く、知識で書いた。実物で確かめていない。
- 音量は amp を 0 dB に固定する。音量は混ぜの `gain` が持つ。
- `MAX_STREAMS` 8 本と `QUEUE_BYTES`・`RING_BYTES` の固定は残した。監査 `audit-compositor.md` の #33 と、`audit-hardcoded.md` の S3 の残りである。流れごとの待ち行列（#34 の後半）も mediaclock と衝突するので触れていない。
- `audio_stream_flush` のとき、途中の block は取り戻すが制御器の FIFO に入った分は出る。
- YouTube の「エラーが発生しました」と `author-reset`：再生 clock の 46〜54 s で出る。main の媒体 `build\nocturne.img`（AC97 + dsound、`mainac97.log`、`mainac97.png`）でも clock 47846 ms で同じ画面が出たので、この枝の HD Audio が持ち込んだものではない。音の道の外の欠陥で、原因は調べていない。
- 枝を合わせるときの衝突：mediaclock の `audio.c` の `status_of` は旧名の `card_fill` を使う。この枝では `card_fill` は無い。`played_locked` の `dry_since`、`stream_drop` の周りも同じ所を触る。
- 枝の土台は e67f52ed である。main の先に進んだ分は取り込んでいない。
