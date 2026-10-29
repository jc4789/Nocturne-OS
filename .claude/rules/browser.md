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

## 流れ（詳しくは .claude/rules/map.md）

取得 `webnet.c`・`webfetch.c` → 解析 Lexbor（`html_lexbor*.c`）→ DOM `dom.c` → CSS `css.c`・`cssprop.c`・`cssom.c` → 箱 `box.c` → 配置 `layout.c` → 描画 `paint.c` → 窓 `user/apps/browser.c`（主ループ、F12 の console と Network）。JS は `js.c` と `js_*.c`・`js_*.js`（QuickJS は `third_party/quickjs`）。動画と音は `avmedia.c`、Web Audio は `webaudio*.c`、枠は `frame.c`。
主ループ（browser.c）は、`web_tick` で文書を一拍進め、要れば `relayout`、`web_paint` で全面を描くか損傷だけを部分描画し、窓へ送り、`webnet_pump` を回し、`web_deadline` まで待つ。締切の源はどれも「-1＝締切なし、それ以外は絶対の uptime_ms」を返す（覚え書き）。

## ログを先に読む

- 口は二つ：ホストの serial.log（QEMU の `-serial file:`）と、ゲストの F12。F12 の Console の札は頁の行、Trace の札はブラウザー自身の計測（`WEB_CONSOLE_TRACE`）。どちらも `console_add`（browser.c）から出る。serial へ出るのは `--log-console` か `--debug-js` のときだけ。GUI からは旗を付けられない：ゲストの Terminal で `browser --debug-js <URL>` を打つ。
- 行の形は `[browser log @秒.ミリ秒] 本文`（`warn`・`error` も同じ形）。計測の行は `[browser trace @…]`。秒はゲストの起動からの時間。カーネルの行には時刻が無い。
- 動画のかくつき・落ちるコマ・音：`Native media timing:` の `decoded N/drop M`、`presented`、`queued`、`starved`、`silence`。再生中は 2 秒ごとに出る。旗なしでも F12 の Trace の札には出る。
- 頁が try/catch で握りつぶした例外は `JavaScript caught exception:`、Promise の拒否は `JavaScript promise rejected:`（どちらも Trace の札）。欠けた API を探す入口。
- 描画と待ち（`--debug-js`、2 秒ごと）：`Browser loop profile:`（wall・ticks・draw・wait）、`Browser damage profile:`（部分描画）、`Native paint profile:`（walk・video probe）、`Document tick profile:`、`Script deadline profile:`。fps を直接出す行は無い。全面の `draw` と部分の `partial draw` の回数を wall で割る。
- JS の重い関数：`JavaScript profile:`（標本の JS 時間が 1000 ms たまるごと）。`JavaScript heap census` は heap 全体を歩くので計測を歪める。通信は `Net #N …`。
- 全部の行の意味・出る条件・取り方は `nocturne-project` スキルの `references/logs.md`。

## 測るときの約束

- `--debug-js` は原因を探す旗で、製品の速さを歪める（2026-10-27、heap の歩きが主スレッドの半分を使っていた）。製品の速さは旗なしの回で測る。
- 変える前に、今の媒体（`build/nocturne.img` の一つ）で `browser-hikaku` の組の全部を同じ条件（2560×1440、F12 を開いたまま、QEMU は窓付き一台）で測り、値と行を書き残す。写しや旧版の媒体で基準を作らない。参照は Floorp の画像で、前の回の Nocturne ではない。
- 計測の道具を足したら、その道具自身の費用を測る。計測の行は `web_js_trace`・`WEB_CONSOLE_TRACE` で出し、頁の console（level 0〜2）へ混ぜない。
- 「速くなった」は同じ条件の実時間と fps が動いたときだけ。内部の数が減っただけでは言わない。

## 触るときの約束

- W3C・WHATWG の標準（WPT が試すもの）を満たす。欠けは仕様の手順のとおり全部、持ち主の層まで通して入れ、安全の規則を削らない（AGENTS.md「Web の標準を満たす」）。仕様の原文は `fetch.spec.whatwg.org` などを読み、WPT は `D:\Programes\cloned repos\wpt`。
- 壊れているのは我々のブラウザー。サイト名は「再現は◯◯」とだけ添え、ホスト名・URL・特定の DOM での分岐を書かない。
- 実サイトだけで確かめる。自作の HTML・ローカルの試験ページ・example.com は使わない。
- 箱・run・deco の装い（st）は節の装いの arena を借りる。外す・入れ直す・release の前後は覚え書きを読む。
- 描画の途中の中断点は窓の画布に描くことがある。並列の段に service を渡してよいのは私的な緩衝だけに書く段。
- 台帳は `docs/browser/LEDGER.md`。受入表と測定の節を更新する。
