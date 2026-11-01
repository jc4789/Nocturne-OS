# mediaclock の報告

枝は `worktree-agent-a2db792d954e915d5`。製品のコミットは古い順に `d3e89239`、`86979720`、`f2c33b74`、`66bf6c6c`、`6eef4835`。土台は main の `12702764`。この後に main へ入った 6 コミットは文書だけで、音と媒体のコードに触れない。

## 依頼の全文

あなたの名は「mediaclock」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の 5043e3e6。自分の作業木が古ければ、まず main を取り込む。QEMU のポートは HMP 45478・QMP 45479。ブラウザーの確かめの手順は `.claude/skills/browser-hikaku/SKILL.md`。

### 欠陥

動画の時計が実時間より遅れる。再現は `https://baike.baidu.com/item/%E6%9C%88%E7%90%83/30767`（自動再生の動画がある頁）。

窓付きの QEMU（q35、4 CPU、WHPX、2560×1440、`-audiodev none,id=snd -device AC97,audiodev=snd`、ブラウザーは `browser --maximized <URL>`、F12 の console を開く、旗なし）で取った `Native media timing` の行：

- @45.608 clock 1998、@47.618 4001、@49.622 5737、@51.625 7737、@53.629 9741、@55.651 11435、@57.655 13432、@59.657 15203、@61.676 16967、@63.691 18980。
- @97.798 clock 49365。@45.608 からの 52.19 s に 47.37 s しか進まない（91%）。
- `presented` は clock に付いて進み、`drop 1`、`starved 0`、`silence 0`。`queued` は多くの行で 8000〜10000 frames、@91.794 と @97.798 の行は `queued 0 frames`・`ahead 0 at -1`。
- 参照（内蔵ブラウザー）の同じ動画は実時間どおりに進む。

### 仕事

層ごとの候補を自分で置き、観察で決める。候補の層は少なくとも：ブラウザーの主ループと媒体の供給（`user/libc/web/avmedia.c`、`doc.c` の tick、子プロセス `browsermediaworker`）、libc の音の口、カーネルの音の stream と渡し（`kernel/src/dev/audio.c`、`kernel/src/vm/ac97.c`）、割り込みと sched（中核は作り直されたばかり）、QEMU の音の出口（`-audiodev none` と `dsound` で比べる。頁の動画は muted なので音はほとんど出ない）。

原因が分かったら、原因を消す形で直す。音の供給が UI や JS の長い仕事に止められる作りなら、それを作りとして直す（媒体の時間の道は、頁の JS・配置・描画に止められないこと。AGENTS.md の「長いネイティブ処理」「性能」の節を読む）。`starved` などの計数が実際の列の尽きを数えていないなら、それも直す。VM の音の装置だけの都合だったなら、そう書いて、製品のコードを変えない。

受入：同じ条件で clock が実時間どおりに進む（52 s の範囲で差を数字で書く）。`drop` が増えない。頁の表示・スクロール・一時停止・再再生を悪くしない。YouTube の `https://www.youtube.com/watch?v=vuYLCMyzFD4`、Bilibili の `https://www.bilibili.com/video/BV1cgYQ6PE8c/` でも同じ行を比べる（動画が出ないなら、その状態をそのまま書く）。報告は `docs/kernel-core-20261101/mediaclock-report.md`。

### 途中で変わった線（コーディネーターの通知。ユーザーの言葉の中継を含む）

- ずれの溜まりは 0。音と映像の差、同じ時計に従う全部の流れの間の差、実時間（TSC の monotonic clock）との差。どんな長さの再生でも 0。"there should never be a build up of drift at all ever." "claude you know there are ways to fix dritf."
- 直し方の例：装置の位置と counter の組から二つの時計の比を測り続け、音の resample の比や時計の補正へ返す閉じた輪。音の高さと音質を耳で分かるほど変えない。補正で音を飛ばさない。
- 輪の持ち主を一つの呼ぶ側に置かず、音を出す全部の道（`player.c`、`mediaplayer.c`、Web Audio、`sound_effect`）へ届く所を選ぶ。
- 比 99.9% は短い窓の最低で、長い再生の合格に使わない。窓を分けて測り、一番悪い窓も書く。"that is not tight enough, we need to set the hardest standards for ourslevs the the easyest. 99.9% accuracy is the absolute bare minimum"
- ±1 ms の対象は時計の読みと音の位置。画面に出るコマの誤差は 1 拍の半分まで。
- 同期の公表の値：ATSC 先 15 ms・後 45 ms、ITU-R BT.1359-1 検知 先 45・後 125、映画 ±22、EBU R37 全体 先 40・後 60、各段 先 5・後 15。音が映像より先の側は 10 ms より厳しくする。
- 報告には線と測った値を並べ、届かないものは届かないと書く。
- YouTube の `author-reset` と「エラーが発生しました」が自分の層か確かめ、そうでなければ一行報告する。
- 要る文書は作り手の公式の場所から `D:\Programes\OS DEV INFO` へ落としてよい。"You can download the docs you need yourself. You dont have to ask me to do it every time."

## 結論

時計の遅れの原因は三つあり、全部を消した。

| 版 | 条件 | 組 | 時計／実時間 |
|---|---|---|---|
| 前（`5043e3e6`） | baike、AC97、none | `@58.752..@136.101` | 71917 ms／77349 ms（92.98%）、遅れ 5432 ms |
| 今（`6eef4835`） | 同じ | `@48.315..@346.469` | 298141 ms／298154 ms（99.996%）、差 13 ms |
| 今（`6eef4835`） | 同じ、動画の終わりまで | `@48.448..@427.447` | 378993 ms／378999 ms（99.998%）、差 6 ms |

届いていない線が三つある。

- 長いネイティブ処理の中で音の列が尽きる（`innerHTML` の 11 s で underrun 10478 ms）。供給は主スレッドの協調のままで、依頼の「作りとして直す」に届いていない。
- ホストの load が高い時に `drop` が増え、コマの差が 34 ms になる（同じコミットで drop 0 の組と 122 の組がある）。
- AC97 の生の位置の報告は 10 ms 刻みで、±1 ms に入らない。濾した値は入る。

## 原因

1. **入力の同期の取り直し。** `user/libc/media_http.c` は 256 KiB ごとに子の中で同期の HTTP を取り直した。その間、子の submit が 570〜1422 ms 空き、音の列が尽き、音に従う時計が止まった。前の版（`base.log`）には列の尽きの計数が無いので、この submit の間隔と `queued 0 frames` の行で決めた。先読みを入れた版で `underrun 0/0`、比 99.99% になった（`fix3`。同じ頁、音の装置の輪はまだ無い）。
2. **計数が尽きを数えていなかった。** `starved` は要素の待ちの状態で、音の列の尽きを数えない。依頼の行の `starved 0` はこのため。カーネルの `n_audio_status` に `underruns`・`underrun_ns` を足し、尽きを出口の側で数えた。`starved` は要素の待ちの状態として残した。
3. **二つの水晶。** 音の出口の水晶と TSC は別の時計で、音に従う時計は装置の比だけ溜まる。下の閉じた輪で消した。

消えた候補：

- 割り込みと sched：入力を直した後の `underrun 0/0` で消えた。
- QEMU の音の出口：none、dsound、intel-hda で同じ線になり消えた。
- `kernel/src/vm/ac97.c`：10 ms 刻みの報告は VM の装置の都合で、変えていない。

## 設計

### 時計の持ち主を分ける

- **音の口の持ち主 `kernel/src/dev/audio.c`** が比を持つ。流れの時間軸 V（`sink_mixed`・`sink_played`）と、出口のフレーム ordinal D（`pace.given`、driver の `*end`、`audio_played(D,at)`、`unissue`）を分けた。流れは V だけを見る。
- **要素の時計 `user/libc/web/avmedia.c`** は monotonic の式（`base_ms`＋経過×rate）にした。音の報告で進むのをやめた。

輪は混ぜ場に一つで、`player.c`、`mediaplayer.c`、Web Audio、`sound_effect` は同じ `mix_unlock` を通る。呼ぶ側のコードは何も持たない。

### 混ぜ場の閉じた輪（`pace`）

- 出口の位置と時計の組（`audio_played`）から、差 `error=(heard-begin_stream)-frames_in(at-begin_at)` を Q8 で測る。
- 最初の 1 秒（`PACE_SETTLE_NS`）は平均で線を置く。装置の初めの一塊と転送の刻みの偏りを線に入れないため。
- 濾過の時定数は約 2 s（`PACE_FILTER_MS`）。
- P は 0.1/s（`PACE_KP`）、I は 0.0025/s²（`PACE_KI_NUM`/`PACE_KI_DEN`）。I があるので、水晶の一定の差は差の残りにならない。
- 窓（2 s から倍々に 64 s）で測る `far` は 2000 ppm を超える装置だけを受ける。
- clamp は ±12.5%（`PACE_MOST`）。50 ms（`PACE_LOST_Q8`）離れたら線を置き直す。全部鳴らし終えたら輪を止める。
- `n_audio_status` の `pace_ppb` に今の比を出す。

### 補間

- `mix_sum` と `mix_pack` の間で、8 タップ、65 行、Q15 の Kaiser 窓 sinc を隣の行と線形補間する。係数は int32。表は `scripts/audio_pace_table.py` が `kernel/src/dev/audio_pace.h` を生成する。
- 遅れは `PACE_DELAY`（5 フレーム）。履歴に残る音は次の block で出す（`pace.ringing`）。音を飛ばさない。
- 比が 0 の間の音は補間の前と同じ値になる（`w3` で -75 dB、親の媒体と同じ）。

### 要素の時計

- hold：開始と、全部鳴らし終えた間だけ止める。開いた時刻より古い報告では解けない。
- resync：50 ms 以上離れて 500 ms 続いたら置き直す。
- 濾した報告へ 2 秒に 1 ms まで寄る（`CLOCK_MOVE_NS`）。始めの一度の差を飛ばさずに埋める。
- `Native media timing` に `underrun`、`sync sound L..H/filtered F ms`、`pace P ppm`、`picture L..H ms`、`hold`、`resync` を足した。

### 入力の先読み（`media_http`）

子の thread が読む。窓は 20 秒分で、4 MiB 以上。音を作る thread は回線を待たない。

### MSE の二つの世代

- `revision`：古い出力を拒む。
- `timeline`：再生の場所が動いた（seek、第一 decoder、`changeType`）。音の流れを閉じるのは `timeline` の変化だけ。
- `nmedia_mse_remove(m,start,end,current)` は今の位置より先の frame が落ちたときだけ seek する。頁の `SourceBuffer.remove` が過去の区間を消しても音を開き直さない。

## 変えたファイル

| 場所 | 中身 |
|---|---|
| `kernel/src/dev/audio.c`、`audio.h` | V と D の分離、輪、補間、`take_back`、`status_of`。 |
| `kernel/src/dev/audio_pace.h`、`scripts/audio_pace_table.py` | 補間の表と生成元。 |
| `common/abi.h`、`docs/nocturne-native-abi.md` | `n_audio_status` の `underruns`・`underrun_ns`・`played_at_ns`・`pace_ppb`。 |
| `user/libc/web/avmedia.c` | 時計、`timeline_seen`、行。 |
| `user/libc/media_http.c`、`media_http_private.h`、`media.c`、`media_alloc.c` | 先読みの thread。 |
| `user/libc/media_rate.c`、`user/include/media_rate_private.h` | `nmedia_trim_*` を消した。 |
| `user/libc/media_mse.c`、`media_mse_worker.c`、`user/apps/browsermseworker.c`、`user/include/media_mse_private.h`、`media_mse_worker_private.h` | `timeline` と `remove` の `current`。 |
| `user/libc/web/html_lexbor.c` | 途中で入れた token ごとの checkpoint を消し、main と同じ形。 |
| `.claude/rules/map.md`、`.claude/rules/browser.md`、`.claude/skills/nocturne-project/references/logs.md` | 生成物の行、行の説明。 |

`kernel/src/vm/ac97.c` は変えていない。

## 消したもの

- 256 KiB ごとの同期の取り直し。
- 音の報告で時計を進める形と、要素の側の積分。
- `nmedia_trim_*`（要素ごとの比。輪が混ぜ場へ移った）。
- 頁の `remove` と decoder の続きによる音の開き直し。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel` と `qemu-image` が通る。媒体は `D:\Programes\llm vibe slop os\build\wt\agent-a2db792d954e915d5\nocturne.img`。
- 変えたファイルに警告は無い。出ている `-Wmissing-field-initializers` は触っていないファイルのもの。
- WPT の `scripts/wpt.py --label mediaclock --accel whpx --cpus 4 --memory 4096 media-source/` と `scripts/test.py audio` は、フックが媒体の数（`build/mae.img`、`build/nocturne.img`）で止めた。走っていない。
- 追加の文書は要らなかった。何も落としていない。

## 線と測った値

条件は全部、窓付き QEMU、q35、4 CPU、WHPX、2560×1440、F12 を開く、`--log-console`。値は `6eef4835` の `k23`（AC97＋none、298 s、別の QEMU が一台ずっと動いていた）と `k20`（AC97＋none、動画の終わりまで 379 s、host の load 3.9）。

| 線 | 値 | `k23` | `k20` | 判定 |
|---|---|---|---|---|
| 短い窓の比 | 99.9% 以上 | 2 s の窓の最悪 99.90% | 2 s の窓の最悪 99.95%（最後の行は 99.59%：終わりで duration に止まる） | `k23` は線の上。`k20` は最後の行を除いて届く |
| 実時間との差の溜まり | 0 | 始めの 26 s で 13 ms へ寄り、以後 298 s まで 13〜14 ms | 370 s の間 -2..-1 ms | 溜まらない |
| 時計の読みと音の位置 | ±1 ms | 濾した値 -0.40..0.46 ms（始めの 26 s を除く）。生の報告 -6.8..2.4 ms | 濾した値 -0.12..0.53 ms。生の報告 -7.4..3.9 ms | 濾した値は届く。生の報告は AC97 の 10 ms 刻みで届かない |
| コマの誤差 | 60 Hz で 8.3 ms まで | -5..8 ms | -5..34 ms（終わりの行 113 ms） | `k23` は届く。`k20` は届かない |
| 音が先の差（コマが遅れる側） | 10 ms より厳しく | 8 ms | 34 ms | `k23` は届く。`k20` は届かない |
| コマが先の差 | EBU R37 各段 15 ms | 5 ms | 5 ms | 届く |
| 音切れ | 0 | `underrun 0/0` | `underrun 0/0` | 届く |
| `drop` | 増えない | 0→0 | 2→122 | `k23` は届く。`k20` は届かない |
| 比の補正 | 耳で分からない | pace -24..73 ppm | pace ±26 ppm | 届く |

### 窓ごとの差（`k23`、約 50 s の窓）

| 窓 | 実時間 ms | 時計 ms | 差 ms | drop | 濾した値 ms | pace ppm | コマ ms |
|---|---|---|---|---|---|---|---|
| @46.315..@96.448 | 50133 | 50120 | 13 | 0→0 | -10.34..0.26 | -8.5..12.6 | -5..7 |
| @96.448..@146.458 | 50010 | 50010 | 0 | 0→0 | -0.08..0.26 | -10.8..10.7 | -5..7 |
| @146.458..@196.460 | 50002 | 50002 | 0 | 0→0 | -0.10..0.24 | -16.6..12.4 | -5..8 |
| @196.460..@246.466 | 50006 | 50006 | 0 | 0→0 | -0.15..0.22 | -14.2..16.4 | -5..5 |
| @246.466..@296.468 | 50002 | 50002 | 0 | 0→0 | -0.40..0.35 | -10.7..73.1 | -5..7 |
| @296.468..@346.469 | 50001 | 50001 | 0 | 0→0 | -0.31..0.46 | -24.2..37.9 | -5..6 |

始めの 13 ms は一度だけの寄りで、溜まりではない。音の流れの始めの位置が時計より 10 ms ほど遅れて出るため、時計が 2 秒に 1 ms でそこへ寄る。この間の音と映像の差は最初の窓の -10 ms で、線（±1 ms）を外れる。

### 窓ごとの差（`k20`、約 50 s の窓）

| 窓 | 実時間 ms | 時計 ms | 差 ms | drop | 濾した値 ms | pace ppm | コマ ms |
|---|---|---|---|---|---|---|---|
| @46.432..@96.566 | 50134 | 50120 | 14 | 0→21 | 0.00..0.53 | -15.7..10.0 | -5..33 |
| @96.566..@146.721 | 50155 | 50155 | 0 | 21→33 | -0.12..0.33 | -12.8..18.5 | -5..34 |
| @146.721..@196.928 | 50207 | 50206 | 1 | 33→46 | -0.03..0.33 | -11.1..12.3 | -5..34 |
| @196.928..@247.006 | 50078 | 50079 | -1 | 46→64 | 0.12..0.44 | -9.4..11.3 | -5..33 |
| @247.006..@297.189 | 50183 | 50183 | 0 | 64→76 | 0.07..0.38 | -13.5..15.4 | -5..33 |
| @297.189..@347.232 | 50043 | 50043 | 0 | 76→93 | -0.07..0.39 | -14.3..26.2 | -5..34 |
| @347.232..@397.366 | 50134 | 50134 | 0 | 93→115 | 0.04..0.51 | -18.2..23.4 | -5..33 |
| @397.366..@427.447 | 30081 | 30073 | 8 | 115→122 | 0.00..0.41 | -20.9..13.7 | -5..113 |

最初の窓の 14 ms は最初の行の clock 0 からの開始、最後の窓の 8 ms は動画の終わりの duration のクランプ。

### 再生の始めと終わりの差

| 組 | 始め（最初の 2 s の後） | 終わり |
|---|---|---|
| `k23` | offset 0 ms、音と時計 -15.4..-11.4 ms | offset 13 ms、音と時計 -4.5..2.0 ms、濾した値 0.16 ms |
| `k20` | offset 0 ms、音と時計 -2.7..3.9 ms | 終わりの前の行で offset -2 ms、音と時計 -5.5..1.7 ms |
| `k2`（`CLOCK` 1 ms/s の前の版） | offset 0 ms | offset 7〜8 ms で一定、濾した値 -0.33..0.09 ms |

溜まらないと言える範囲は、この動画の長さ 380 s まで。これより長い再生は測っていない。輪は I を持つので、水晶の一定の差は差の残りにならない作りで、測った 380 s はその形と合う。

### 音の出口を換えた比べ（どれも `6eef4835` より前の、輪が入った版）

| 出口 | 組 | 実時間／時計 | 最悪の窓 | 終わりの差 | 他 |
|---|---|---|---|---|---|
| AC97＋dsound `k4ds` | @49.008..@241.454 | 192446／192445 | 99.90% | 1 ms | drop 0→0、underrun 0/0。@55 に装置の揺れで pace 834 ppm、濾した値 -5.8 ms まで、20 s ほどで戻る |
| intel-hda＋none `k3hda` | @47.527..@171.798 | 124271／124240 | 98.96%（始めの寄り） | 31 ms（始めの 20 s で寄った後は 10〜11 ms で一定） | drop 0→0、underrun 0/0、生の報告 -1.4..-0.1 ms、pace ±7 ppm |

### drop の決着

同じコミットの `k20` と `k23` で 122 と 0 になる。

- `k23` は別の QEMU が全期間で動いていた（5 秒ごとの数がずっと 2）。それで drop 0、コマ -5..8 ms。QEMU の台数は原因ではない。
- `k20` は host の load 3.9 で、コマ -5..34 ms。コマが 33 ms 遅れる時に drop が増える。
- 近い版の組：`k2` 0、`k22` 2（62 s）、`k1` 32。
- `k23` の間の host の load は取っていない。`k20` の drop の根は、主スレッドの wake の遅れの所までで、それ以上は決めていない。

### 音質（`w3`、`beep -f 1000 -l 60000`、QEMU の wav）

振幅は 7813 で一定。周期は 47.998〜48.001 frames。窓の間の位相の段は 0.0156 rad 以下。100 ms 窓の残りは中央 -60.8 dB、最悪 -46.8 dB（比が 1 の間は -75 dB で親の媒体と同じ。残りは一定の周波数との差を含む上限）。録音の周波数に対する位相の動きは -3.9 ppm。音は飛ばない。

途中の版では係数を int16 に置き、32768 が -32768 になって、小数の位置が境を越えるたびに音が 10 ms ほど消えて逆相になった。`6eef4835` で int32 にして消した（`-0.1 dB` の窓が `w3` の値になった。どちらも同じ `beep` の録音）。

### 長いネイティブ処理（`lg4`、音の供給は主スレッド）

| 処理 | 時間 | underrun | 線 |
|---|---|---|---|
| `'<p>x<b>y</b></p>'.repeat(60000)` の `innerHTML` | 11071 ms | 7 回／10478 ms、hold 4/563、drop 2 | 届かない |
| 100 万要素の配列と JSON | ― | +1 回／427 ms | 届かない |
| 3 s の JS の空回し | 3 s | 増えない | 届く |

時計は実時間に合わせて動くが、音はこの間出ない。作りとしての解は「子が音を自分で sink へ出す」か「親の媒体 thread」で、`pump_impl` の 230 行と JS binding の 157 か所に関わる。この仕事では作っていない。

## QEMU で見たもの

画像と log は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\mediaclock\` にある。自分で開いて見た。

### baike（`6eef4835`）

- `k23.png`：頁と動画（カルーセルの月の映像）が出て、console の行が読める。@346 まで動く。
- `k23.log` の行：
  `[  204.460975 1:24] [browser trace @204.460] Native media timing: generation 1, input Range, clock 158132 ms, presented 3954 at 158120 ms, decoded 3958/drop 0, window-published 3953; audio open, queued 8692 frames, underrun 0/0 ms, written 7598882/silence 0 frames, end 158309/played-estimate 158128 ms, pending 1 at 158290 ms/ahead 4 at 158160 ms, starved 0/ended 0, window-unpublished -@0, sync sound -4.8..1.6/filtered -0.09 ms, pace 5.4 ppm, picture -5..1 ms, hold 0/11 ms, resync 0/0 ms`
- 動画は 380.993 s で終わる（`k20.log` の @427.447、`audio closed`、`starved 0/ended 1`）。

### 操作（`6eef4835`、`k24.log`）

- 一時停止：`v.pause()` で時計が 24361 ms に止まる。
- 再再生：`v.play()` で @85.255 clock 26348 から進む。`underrun 0/0`。
- シーク先：`currentTime=200` で clock 200000 へ移り、@89.259 clock 201680、@97.260 clock 209681。
- 戻るシーク：`currentTime=5` で @99.288 clock 5238 から、@109.291 clock 15240（実時間 10003 ms、時計 10002 ms）。
- @109.291 までの全部で `drop 0`、`underrun 0/0`、`resync 0/0`。開始と再開とシークの hold の合計は 3119 ms。
- wheel の下の後の @110.811 の行で `drop 2`。
- `k24c.png`：wheel の上の後、頁は上端のまま、動画が動く。
- `k24e.png`：wheel の下の後、頁が動く。ヘッダーが固定の形に変わり、本文が読める。@107.290 にコマが 26 ms 遅れる一行が出た。
- @110.811 に `author-pause` が来る。頁自身が見えなくなった動画を止めたもので、時計は 16761 ms で止まる（`k24e.png` の `[16.761,true,2,false]`）。
- 戻りの wheel は `k5f.png`・`k5g.png`（`f2c33b74` の前の版）で見た。上へ戻り、`v.play()` で再開する。

### 同じ動画の参照（内蔵ブラウザー、2560×1000 の表示域）

同じ `vdse.bdstatic.com` の 380.993 s の動画。`currentTime` と `performance.now()` を 1 秒ごとに取り、実時間 64001.2 ms に時計 64001.7 ms、10 秒窓の最悪 99.987%、`droppedVideoFrames` 0（`totalVideoFrames` 2845）。Nocturne の `k23` の窓（差 0 ms、drop 0）と同じ線にある。比べる窓の長さは 10 秒と 2 秒で違う。

### Bilibili（`f2c33b74` の後の版、`k10bl.log`、`k10bla.png`）

- 一つ目の要素：@35.975..@40.039、実時間 4064／時計 4062、drop 1→12、頁が document-free で終える。
- 二つ目の要素：@43.560..@103.628、実時間 60068／時計 60070、最悪の窓 99.95%、差 -2 ms、drop 0→2、underrun 0/0。
- @104.470 に頁の `author-pause`。画面にログインの札が出る。

### YouTube（`6eef4835`、`k21yt.log`、`k21a.png`・`k21b.png`・`k21c.png`）

- 始め：@34.186..@54.618、実時間 20432／時計 20432、最悪の窓 99.95%、drop 0→11（始めの数秒）、underrun 0/0。`k21b.png` で動画の絵が出ている。
- 実マウスの一時停止：@56.552〜@62.040 の間 clock 24245 を保つ。再開で drop 17。
- 再開の後：@64.066..@84.102、offset は 5581→5577 ms（差 4 ms 以内で、実時間どおり）、drop 17→17、underrun 0/0、濾した値 1 ms 未満。
- 終わり：@85.768 clock 47940 で `author-reset`。`k21c.png` で「エラーが発生しました」が出る。`k8yt` では clock 44262 で同じ。

**YouTube のエラーの層：** 時計と音の層ではない。直前まで `underrun 0/0`、濾した値 1 ms 未満で、音の書き手が main の媒体（AC97＋dsound）で同じエラーを報告した。頁が MSE の道で要素を reset しており、根の層は決めていない。

## 残った欠陥と未確認

1. **音の供給は主スレッドのまま。** `innerHTML` の 11 s で underrun 10478 ms。依頼の「作りとして直す」に届いていない。次に作るなら子が音を sink へ出す形。
2. **host の load で drop が増える。** 同じコミットで 0 と 122。根はコマの wake の遅れまでで、決めていない。
3. **生の音の報告は ±1 ms に入らない。** AC97 の 10 ms 刻みは VM の装置の都合。intel-hda でも -1.4 ms で 0.4 ms 届かない。
4. **音の流れの始めの 10 ms。** 最初の 26 s の音と時計の差は線を外れる。寄りは 2 秒に 1 ms で、飛ばさないための速さ。
5. **YouTube の browser の落ち。** `[segfault] … __JS_FreeValueRT +0x1a`、呼び手 `JS_FreeContext +0x8a`、`rt` が壊れている。`token_done` の checkpoint を入れた版で 4 回中 2 回、消した版で 3 回中 0 回（35 秒まで）、親の媒体で 3 回中 0 回。checkpoint は消した。仕組みは解いていない。
6. **`user/apps/beep.c` の `-v`。** `MAX(0, MIN(100, atoi(argv[++i])))` がマクロで複数回評価し、NULL で落ちる。main にもある。直していない。
7. **見つけた別の欠陥。** mixed content の自動の格上げが無く、baike の hover の http の動画が黒い札。Range の readyState が 2 まで。HLS（`media_adaptive`）は区切りごとに同期で取る。動画の終わりの最後のコマが 89〜113 ms 遅れる。
8. **未確認。** 実機の音（HDA）での輪。380 s より長い再生。WPT の `media-source/` とゲストの `audio` の試験（フックが止めた）。`player.c`・`mediaplayer.c`・`sound_effect` は `beep` と同じ混ぜ場を通ることをコードで見たが、別々の録音はしていない。Web Audio の音は録音していない。

## 盤の最終の形

```
# 目的と受入
媒体の時計は TSC の monotonic と同じ速さで進み、音と映像の差も実時間との差も、どんな長さでも溜まらない。音は飛ばさない。線は順に：比 99.9% は短い窓の最低、溜まりは 0、時計の読みと音の位置 ±1 ms、画面に出るコマは 1 拍の半分まで。drop が増えない。スクロール・一時停止・再再生を悪くしない。YouTube・Bilibili も同じ行。

# 範囲と禁止
media_http.c、media.c、media_alloc.c、media_rate.c、avmedia.c、media_mse*、kernel audio.c と abi.h。ac97.c は変えない。音を飛ばさない。

# 候補（層ごと）
| 層 | 主張 | 決める観察 | 状態 |
| 子の入力 media_http | 256 KiB ごとの同期で列が尽きる | k23・k20 の underrun 0/0 | 直した |
| 装置の時計と TSC | 別の水晶で溜まる | k20 の 370 s で offset -2..-1 ms、k23 の 270 s で 13〜14 ms | 直した |
| 補間の係数 | int16 の溢れで音が消える | w3 の振幅一定 | 直した |
| MSE の開き直し | 頁の remove で音が閉じる | k10bl の 60 s で差 -2 ms | 直した |
| 主スレッドの供給 | 長い native の仕事で列が尽きる | lg4 の underrun 10478 ms | 残る |
| コマの遅れ | host の load で主の wake が遅れる | k20 34 ms、k23 8 ms | 残る。根は未確定 |
消えた：sched（underrun 0/0）。QEMU の音の出口（none・dsound・hda で同じ線）。ac97 の 10 ms（VM の都合で輪が受ける）。

# 済んだ手
base 92.98%。read-ahead で 99.99%。閉じた輪で k23 99.996%、k20 99.998%。w3 で音質。k24 で操作。

# 残りの手
主スレッドの外の音の供給の設計（ユーザーの判断を含む）。drop の host の load の切り分け。

# 未確認
実機の音。380 s より長い再生。WPT とゲストの audio（フックが止めた）。
```
