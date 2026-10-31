---
paths:
  - "user/libc/web/**"
  - "user/apps/browser*"
  - "user/include/web.h"
  - "user/libc/webnet.c"
  - "user/apps/webfetch.c"
  - "docs/browser/**"
---
# ブラウザーの領域に触れたとき

規則（標準、サイト専用の処理の禁止、実サイトと参照、前の記録、回帰）は AGENTS.md「製品のコード」「確かめ方」。手順は `browser-hikaku`・`nocturne-project`。ここには、この領域のコードとログの事実だけ。

## 流れ（詳しくは .claude/rules/map.md）

取得 `webnet.c`・`webfetch.c` → 解析 Lexbor（`html_lexbor*.c`）→ DOM `dom.c` → CSS `css.c`・`cssprop.c`・`cssom.c` → 箱 `box.c` → 配置 `layout.c` → 描画 `paint.c` → 窓 `user/apps/browser.c`（主ループ、F12 の console と Network）。JS は `js.c` と `js_*.c`・`js_*.js`（QuickJS は `third_party/quickjs`）。動画と音は `avmedia.c`、Web Audio は `webaudio*.c`、枠は `frame.c`。
主ループ（browser.c）は、`web_tick` で文書を一拍進め、要れば `relayout`、`web_paint` で全面を描くか損傷だけを部分描画し、窓へ送り、`webnet_pump` を回し、`web_deadline` まで待つ。締切の源はどれも「-1＝締切なし、それ以外は絶対の uptime_ms」を返す（覚え書き）。

## ログ

- 口は二つ：ホストの serial.log（QEMU の `-serial file:`）と、ゲストの F12。F12 の Console の札は全部の行（頁の行とブラウザー自身の計測）、Trace の札は計測（`WEB_CONSOLE_TRACE`）だけ。計測を Trace の札だけに置くと、要る行が隠れる。どちらも `console_add`（browser.c）から出る。serial へ出るのは `--log-console` か `--debug-js` のときだけ。GUI からは旗を付けられない：ゲストの Terminal で `browser --debug-js <URL>` を打つ。
- 行の形は `[browser log @秒.ミリ秒] 本文`（`warn`・`error` も同じ形）。計測の行は `[browser trace @…]`。秒はゲストの起動からの時間。カーネルの行には時刻が無い。
- 動画のかくつき・落ちるコマ・音：`Native media timing:` の `decoded N/drop M`、`presented`、`queued`、`starved`、`silence`。再生中は 2 秒ごとに出る。旗なしでも F12 の Console と Trace の札には出る。
- 頁が try/catch で握りつぶした例外は `JavaScript caught exception:`、Promise の拒否は `JavaScript promise rejected:`（`trace` の水準。Console と Trace の札の両方）。欠けた API を探す入口。
- 描画と待ち（`--debug-js`、2 秒ごと）：`Browser loop profile:`（wall・ticks・draw・wait）、`Browser damage profile:`（部分描画）、`Native paint profile:`（walk・video probe）、`Document tick profile:`、`Script deadline profile:`。fps を直接出す行は無い。全面の `draw` と部分の `partial draw` の回数を wall で割る。
- JS の重い関数：`JavaScript profile:`（標本の JS 時間が 1000 ms たまるごと）。`JavaScript heap census` は heap 全体を歩き、主スレッドの半分を使うことがあるので計測を歪める（`--debug-js` の回の数字を製品の速さにしない理由）。通信は `Net #N …`。
- 全部の行の意味・出る条件・取り方は `nocturne-project` スキルの `references/logs.md`。

## 触るときの事実

- 計測の行は `web_js_trace`・`WEB_CONSOLE_TRACE` の水準で出し、頁の console の水準（0〜2）を使わない（Trace の札と serial の `[browser trace]` はこの水準で分かれる）。計測を足したら、その道具自身の費用（`Browser console profile` など）を分けて読む。
- 仕様の原文は `fetch.spec.whatwg.org` など、WPT は `D:\Programes\cloned repos\wpt`。
- 箱・run・deco の装い（st）は節の装いの arena を借りる。外す・入れ直す・release の前後は覚え書きを読む。
- 描画の途中の中断点は窓の画布に描くことがある。並列の段に service を渡してよいのは私的な緩衝だけに書く段。
- `docs/browser/LEDGER.md` は記録。今の状態は盤（`ban`）。
