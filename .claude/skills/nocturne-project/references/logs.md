# ログの読み方：serial.log と F12 の console

確かめた: 2026-10-27 `88a17463` と未コミットの計測の分離（`user/apps/browser.c`、`user/libc/web/{avmedia,js,js_profile,paint,css,box,doc}.c`、`kernel/src/{boot.c,sys/prof.c,fs/store.c}`）。行の形が違ったら、その場でこの表を直す。

## どこに出るか

| 口 | 場所 | 出る条件 |
|---|---|---|
| ホストの serial.log | 手で起動した QEMU の `-serial file:<path>`。`scripts/platform_qa.py` → `build/nocturne-platform/<label>/serial.log`。`scripts/test.py` → `build/test-serial.log`。`scripts/qtest.py` → `build/serial.log` | カーネルの行は常に。ブラウザーの行は `--log-console` か `--debug-js` のときだけ |
| ゲストの F12 の Console の札 | ブラウザーの窓（F12 で開閉） | 頁の `console.*`、例外、ブラウザー自身の warn・error。旗は要らない |
| ゲストの F12 の Trace の札 | 同じ窓の Console の右の右（Ctrl+] で回る） | ブラウザー自身の計測（`WEB_CONSOLE_TRACE`）：profile 群、`Net #`、媒体の timing、捕まった例外。旗は要らない（出る行の多くは旗が決める） |
| ゲストの system log | `C:\Users\<名前>\AppData\Nocturne\System.log`（前回の起動の分は `System-previous.log`。`kernel/src/sys/stream.c`） | serial と同じ内容。ゲストの Files から読める |

ブラウザーの旗は GUI からは付けられない。付け方は三つ：ゲストの Terminal で `browser --debug-js <URL>`、`C:\Tests\autorun.sh`（test.py・platform_qa の写しの C: にだけ置く）、`scripts/platform_qa.py --label <名> --command "<ゲストのコマンド>"`。

| 旗 | 効き |
|---|---|
| `--log-console` | Console と Trace の両方の行を serial へも書く。費用は行の送出だけ |
| `--net-log` | `Net #N …` の行 |
| `--net-body <URL の一部>` | `--net-log` に加え、その文字列を含む URL の要求の本文の全部と応答の頭 2 KiB を 16 進で（`Net #N request body a-b:`・`response body a-b:`、512 バイトずつ） |
| `--debug-js` | `debug_js = net_log = log_console = true`。2 秒ごとの profile 群、`JavaScript profile`、heap の census、媒体の transition・sink・supply 行が加わる。**製品の速さを歪める**（heap の census は heap 全体を歩く） |
| `--js-console` | F12 を開いた状態で始める |
| `--console-file <path>` | 3 秒後にそのファイルを console へ流し込む（platform_qa の `--guest-file` は `C:\Tests\<名>`） |
| `--maximized`、`--js-budget-ms <n>`、`--console-file-each-document` | 窓を最大化、JS の一仕事の上限、文書ごとに console-file を流し直す |

`--debug-paint`・`--prof` は無い。知らない旗は終了値 2 で止まる。

## 行の形

- ブラウザー：`[browser log @秒.ミリ秒] 本文`。水準は `log`・`warn`・`error`（頁の console と F12 の Console の札）と `trace`（ブラウザー自身の計測、F12 の Trace の札）。1900 字を越える本文は `[browser log+ @…]` の行に続く。秒はゲストの起動からの uptime で、F12 の `@秒` と同じ時計。
- カーネル：接頭辞だけで時刻が無い（`memory:`、`video:`、`store:`、`prof:`、`[memory]`）。
- 計測の行は全部 `[browser trace @…]`。頁の行（`[browser log|warn|error @…]`）と grep で分けられる：`grep -a "\[browser trace" serial.log`、頁だけなら `grep -a "\[browser \(log\|warn\|error\)"`。2026-10-27 までの serial.log では計測の行も `[browser log @…]` だった。

## ブラウザーの計測の行

`--debug-js` のとき、主ループが 2 秒ごとにまとめて出す（browser.c の `report_loop_profile`）。一組の中で見るもの：

| 行 | 読む所 |
|---|---|
| `Browser loop profile: wall W ms, ticks N/T ms/max M, host layout N/T ms, draw N/T ms, update U ms, net N ms, wait W ms, parallel jobs J (inclusive)` | 全面の描画の回数と時間。`wait` が小さく `draw`・`ticks` が大きければ主スレッドが飽和 |
| `Browser damage profile: partial draw N/T ms over K kpx, partial present P ms, off the page N, drawn whole N, video patch N/T ms (…)` | 部分描画の回数と画素。損傷が無い 2 秒は出ない |
| `Browser wait profile: timeout from none/network/storage/document/now/refresh …; shortened by the window engine N; ended by an event N` | 何で待ちが切れたか |
| `Browser memory: process KiB, malloc heap KiB, in use KiB` と `Document memory (KiB, N documents): DOM and sheets, computed styles, boxes, layout, authored sheets, fetched CSS, images, canvases, JavaScript` | 伸び続ければ漏れ |
| `JavaScript heap census (KiB) of <URL>: malloc …, objects …, strings …, functions …` | `Document memory` の次の行。一番大きい JS の heap が 1/8 変わったか 30 秒たったときだけ。heap 全体を歩く |
| `Native paint profile: paints N/T ms, order … walk W ms, video probe V ms, opacity groups …` | 描画の内訳（walk＝箱の木を歩く時間） |
| `Native style profile: cascades N/T ms (full …), bindings …, rescans …, elements … rule candidates …` | cascade の回数と、全体になった理由（index・marked・first・unmarked） |
| `Native box profile: local publications N (splices, regions, escalations), whole builds N by …` | 箱の木を局所で直せたか、全部作り直したか |
| `Document tick profile: turns N; dirtied (layout and paint)/restyle by <段> a/b, …` | どの段が配置・描画を汚したか |
| `Script deadline profile (all/within 16 ms/of those in frames): <理由> a/b/c, …` | JS の締切の理由 |
| `Browser console profile: rect draw N/T ms, rect present P ms (no author page paint)` | F12 の console 自身の描画の費用。計測の行は頁の費用と分けて読む |
| `Native parallel <段>: batches, items/AP, work/AP, wall, workers ms/AP ms, sampled CPUs …` | 並列の段が AP を使えたか。batches が増えたときだけ |

fps を直接出す行は無い。一組（wall ≈ 2000 ms）の `draw` の回数と `partial draw` の回数を足して wall で割る。

その他（js.c・js_profile.c）：

- `JavaScript profile: T ms in N functions; the functions with the most time:` と、続く字下げの行 `  割合 ms 関数名 script`。標本の JS 時間が 1000 ms たまるごと（`--debug-js`）。
- `Net #id <種> <METHOD> <URL>, body N bytes…`（要求）と `Net #id <種> <段階>: HTTP <code>, N bytes, T ms, <type>…`（応答）。`--net-log`。
- `JavaScript stopped: the task exceeded its N second execution budget.`
- `JavaScript caught exception: <name>: <message> at <関数> (<script>:<行>:<列>)`：頁の try/catch（と await の中の例外）が握りつぶした throw。旗は要らない。同じ説明は文書ごとに一度、192 種まで（最後の一つに「last distinct one traced」）。欠けた API や機能の探りの失敗は、ここにしか出ない（QuickJS の `JS_SetCaughtExceptionHook`、js.c の `caught_exception`）。
- `JavaScript promise rejected: <理由の説明>`：Promise の拒否のすべて（後で `.catch` が受けるものも）。async 関数の throw と Promise の連なりの失敗はここに出る。上と同じ表で数える。
- stack の枠：頁から見える `error.stack`（と上の二行・F12 の例外）は、我々の束縛（`nocturne:` で始まる台本、Worker は `nocturne:worker-bindings`）の枠と、そこから呼んだ native の枠を出さない（他のエンジンの束縛と同じ）。束縛の中のどこで投げたかを見るには `--debug-js` で開く（QuickJS の `JS_SetPlatformScriptPrefix`、js.c）。

## 動画と音（avmedia.c）

- `Native media timing: generation G, input MSE|Range|buffer, clock C ms, presented P at T ms, decoded D/drop X, window-published W; audio open|closed, queued Q frames, written/silence …, end/played-estimate … ms, pending …/ahead …, starved S/ended E, window-unpublished <理由>@<行>`
  - 再生中は 2 秒ごと、止まったら一度。**旗は要らない**（F12 の Trace の札には常に。serial へは `--log-console`）。
  - `written/silence`：音の出口へ書いたフレーム数のうち、PTS の隙間を埋めた無音の数。連続した音なら silence はほとんど増えない（2026-10-27 まで、ミリ秒の PTS の丸めを隙間と見て 1 秒に約 300 フレームの無音を挟んでいた）。
  - かくつきは `drop` の増え方と `starved`。`presented` が `decoded` よりずっと少なければ、解読は追い付いて提示が追い付いていない。`window-unpublished` は窓へ直接出せなかった理由と avmedia.c の行番号。
- `Native media transition: reason …, generation a->b, …`、`Native audio sink: …`、`Native media supply: …`：`--debug-js` のときだけ。

## カーネルの行

- 起動：`memory: N MiB usable, M MiB free`、`video: WxH, bpp …`、`video: monitor <名>, at R Hz`（EDID が決めた拍。`the firmware passed no EDID` なら EDID 無し）、`store: C: "<label>" on sata0, N MiB, M MiB free`。`store:` の他の行（`read only from now on`、`damaged`）は C: の異常。
- メモリー切れ：`[memory] pid P … no pages for …`（10 秒に 1 回まで）。
- 標本（起動の cmdline に `prof` か `prof_ms=<n>`。`platform_qa.py --cmdline prof`。窓は既定 10 秒）：
  - `prof: ---- N ms window ----` で一組が始まる。
  - `prof: cpuN user U kernel K idle I[, MHz awake %]`：CPU ごとの配分。
  - `prof: task pid P <名>[ (thread)] total N`：仕事ごとの標本。
  - `prof: <標本数> <rip> pid P <名>`、`prof: caller <標本数> <rip> <呼ぶ側> pid P`：熱い番地。核の番地は `build/kernel.elf`、アプリの番地はその ELF（`build/programs/`）を llvm-symbolizer に引く。
  - `prof: <何> N retired to BSP C times, avg us to BSP …, on BSP …, back …`、`prof: round trips through the BSP waited N ms outside the BSP's work`、`prof: trips pid …`：AP から BSP へ回った呼出しの往復。
  - `prof: quantum/irq retires …`、`prof: TLB shootdowns …, IPIs …`、`prof: kei handoffs …`。

## 取り方と使い方

- 行を拾う：`grep -a "Native media timing" "<serial.log>" | tail -n 20`（`-a` は二進の混入に備える）。時刻の範囲で切るなら `@秒` を見る。
- 製品の速さは `--log-console` だけ（または旗なしで F12 を読む）の回で測る。`--debug-js` は原因を探す回に使い、その回の数字を製品の速さとして書かない。
- 変更前の媒体で同じ行を先に取る。前後は同じサイト・同じ操作・同じ秒数の範囲で並べる。
- F12 を開いたまま撮る（ユーザーの硬い規則、2026-10-27）。撮影の画像の console の行も読む。
- ホスト側にこれらの行を解析する道具は無い。要るときは一度きりの Python を作業場（scratchpad）に置き、リポジトリへ入れない。
