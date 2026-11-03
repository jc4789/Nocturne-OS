# 合成と提示の道の監査

## 依頼文（全文）

```text
Nocturne OS（`D:\Programes\llm vibe slop os`）の、画面の合成と提示の道を、読むだけで敵対的に監査してください。返事と報告は全部日本語で書きます。識別子とパスは元の表記のままです。

## 依頼者の言葉（原文）
"Our graphics pipeline needs to be audited. and needs to be assumed wrong. includeing our compositer, and and our lack of disply settings or handleing. ... as well as our compositer seems "heavy" and random things cause a full redraw of the screen after a frame or 2 of black. ... use a couple subagents to do a read only adversarial audit. have them assume that our code base is a mess and that "fake" implmentations of things are rampent."

あなたの担当は合成器と提示の道です。画面のモードの管理（解像度、Hz、bpc の選択）と音は、別の担当が見ます。

## 読むだけ
- コード、設定、媒体、git の状態を変えません。ビルド、QEMU、Hyper-V、スクリプトの実行をしません。OS は動かせないので、証拠はソースと仕様だけです。
- 書いてよいファイルは報告の `docs/audit-compositor-20261102.md` の一つだけです。頭にこの依頼文を全文そのまま写し、続けて報告の全文を書きます。

## 監査の構え
- コードは散らかっていて、偽の実装がはびこっていると仮定します。名前、コメント、文書、コミットメッセージ、試験の合格を信じません。`present` や `flip` や `damage` という名前の関数があっても、画素が実際に画面へ届くまで呼出しを辿って確かめます。表から呼ばれる関数と `SYS_*` の番号での呼出しは codegraph に出ないので、`rg` でも探します。
- 確かめる偽物の形：成功を返して何もしない、何もしない lock や空の口、決め打ちの値、計算したのに誰も読まない値、呼ぶ者のいない道、途中で切れた道、測らずに置いた固定の値や待ち、コメントと食い違う動き、VM でしか成り立たない前提、実機の道が無いまま VM の道だけがある所。
- 証拠の無い所見は出しません。各所見に `file:line` を付けます。数を稼ぎません。読んで本物だと確かめた部分は、本物と書きます。確かめられないことは、確かめられないと書きます。

## 守る線（監査の物差し）
先に `AGENTS.md` と `.claude/rules/map.md` を読みます。`.claude/rules/kernel.md` と `.claude/rules/nvidia.md` も読みます。要点は次のとおりです。
- 層は分かれています。カーネルは GPU を知らず、`nvrm` がアプリのドライバー、GPU の利用者側の像は `C:\Nocturne\Drivers\nvk` です。VM の処理は `kernel/src/vm/vm.h` の後ろだけです。
- 対象は実機です（下限 Zen 4、機械を決め打ちしない）。VM で通ることは実機で動く証拠になりません。実機の GPU は NVIDIA の open-gpu-kernel-modules の RM と nvkms を移植したものです。
- 性能は実時間と fps です。全走査や繰り返す全配置を避け、変わった範囲の管理で設計するのが規則です。
- 文書は古いことがあります。`docs/graphics-20261014.md` は古い報告です。事実は今のコードで確かめます。

## 読む場所
`kernel/src/gui/wm.c`（3370 行、全部）、`kernel/src/gui/pointer.c`、`kernel/src/dev/fb.c`、`kernel/src/dev/fbcon.c`、`common/display_abi.h`、`common/gfx.c`、`docs/nocturne-native-abi.md` の画面の節、`user/libc/gui.c`、`user/libc/ui.c`、`ports/mesa/nocturne_os.c`（swapchain と提示）、`user/libc/vulkan.c`、`ports/nvidia/display.c`（flip と vblank の側）、`kernel/src/vm/` の `rdp.c`・`virtio_gpu.c`・Hyper-V の映像、`user/apps/browser.c` の窓への提示、`user/libc/media_draw.c`。ほかに要る所は辿ってください。NVIDIA の参照は `D:\Programes\cloned repos\open-gpu-kernel-modules` と `open-gpu-doc` です。Linux や他の OS のソースは参照しません。

## 答えてほしいこと
1. 症状「何かのきっかけで黒が 1〜2 frame 出た後、画面全体が描き直される」：黒が画面へ出うる道と、全面の描き直しを起こす条件を全部挙げます。各々、どの操作や事象が引き金か、なぜ黒が先に見えるか（clear してから描く、新しい緩衝を描く前に見せる、flip と内容の順序など）を、呼出しの順で示します。コードで証明できたものと、可能性だけのものを分けます。
2. 症状「合成器が重い」：一 frame に何をするかを実際の手順で書きます。CPU か GPU か、どのスレッドか、何を何度写すか、画面と窓の大きさと数に対して費用がどう増えるか、持っている lock、全走査、毎 frame の確保、起きなくてよいのに起きる所。2560×1440 や 4K、窓が多いとき、動画と同時のときに、どこが詰まるかを述べます。
3. frame の拍：vblank と flip は本物か。拍はどこから来て、VRR と高い Hz でどう動くか。テアリングや一 frame 遅れの所在。実機の NVIDIA の道と VM の道で何が違うか。
4. 窓の緩衝：持ち主、共有の仕組み、同期、窓の大きさを変えるとき、Vulkan や GL の窓の提示の道。
5. 偽物の一覧：上の形に当たるものを全部。

## 報告の形
各所見に、重さ（致命、重い、中、軽い）、種類、`file:line`、呼出しの順の根拠、起きる条件、確信（コードで証明、可能性あり）を書きます。最後に、読んで本物と確かめた部分と、読めなかった範囲を書きます。修理の案は、原因を消す形で短く書きます。

最後の返事は、報告のパスと、重い順の要点を 15 行以内で返します。
```

## 追加の指示（全文）

```text
合成の監査の性能の物差しとして、依頼者の線を渡します。答えの 2 と 3 は、この線で追いつくかを書いてください。

- 1440p の画面三枚（一番速いもので 280 Hz、遅いもので 165 Hz。三枚とも VRR 対応、パネルは OLED と VA の両方）と、消費者向けの 1080p 600 Hz の画面。合成は全部の画面に、それぞれの時刻で毎コマ間に合うか。
- 三枚の画面で AAA ゲーム、動画、SNS を同時に動かして、カクつきも落ちるコマも 0 か。これは最低線で、特別な場面ではない。
- y-cruncher で全コアを使い切る間も、画面と入力は滑らかなままか。8000 Hz の入力に追いつくか。
- 極みは試す線で、決め打ちではありません。拍、上限、間隔、数をコードの定数にしている所は、それ自体が所見です。値は装置の申告と測りから計算するのが決まりです。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

## 追加の指示の二つ目（全文）

```text
性能の線を足します。依頼者によると、消費者向けの 1000 Hz の画面がすでに売られています（LG UltraGear 25G590B：1920×1080、1000 Hz、DisplayPort 2.1 UHBR20、HDR10、G-SYNC Compatible、FreeSync Premium）。

一番速い画面の線を 1080p 1000 Hz とし、一コマの予算は 1 ms として答えの 2 と 3 を見てください。ほかの画面と同時に動く場合も含めます。合成の費用、スレッドの起こしの遅れ、lock の持ち時間、拍の時計の分解能が、1 ms に対してどうかを書きます。これも試す線で、1000 を定数にするという意味ではありません。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

---

## 結論

依頼者の線は、今のコードでは満たせません。

- 一コマ 1 ms（1080p 1000 Hz）に対して、全面の frame は単一のスレッドで 16.6 GB/s 以上の写しを要します（計算）。fair 級のスレッドを一段起こす待ちは、最悪で 3 コマ分です。詳しくは問 2 の「1 ms の線」と問 3 の「拍の時計の分解能」にあります。

- 実機の NVIDIA の道は、画面を一枚しか出しません（C1）。三枚も四枚も届きません。
- Vulkan のプログラムは窓へ絵を出せません。`vkQueuePresentKHR` は成功を返して何もしません（C2）。AAA ゲームは画面に出ません。
- VRR は最初から最後まで偽物です（F1）。立てるコードが一行もありません。
- 合成は全部 CPU で、カーネルの中で、SIMD 無しで走ります（H1）。ブラウザーの本文は画素を 4 回写します（H2）。
- 合成器のスレッドは deadline 級ですが、flip の知らせの鎖は fair 級の 3 つのスレッドを通ります（H9）。全コアを使い切ると、合成器はそこで待ちます。
- 黒の道はコードで三つ証明しました。窓の大きさの変更（B1）、別サイズのモードの変更（B2）、直接表示の終わり（B4）です。三つとも「描く前に見せる」形です。
- 入力の受け取りは 8000 Hz に追いつきます。プログラムへ渡す形（相対の動き、途中の点）がありません（H11）。

合成器の骨組みのうち、画面ごとのスレッド、tile の damage、flip の完了の読み方は本物です（「本物と確かめた部分」）。直す相手は骨組みではなく、CPU の写し、描く前に見せる順序、fair 級の鎖、一枚だけの出力です。

## 範囲と証拠の限界

- 読んだ版は `main` の `d651370a` です。作業木は clean で、このファイルのほかは何も変えていません。
- OS は動かしていません。この報告に実測の値はありません。
- 数は二種類です。「計算」は画素数と周期からの算術です。「見積り」は仮定を置いた値で、仮定を同じ行に書きます。
- 依頼の前提で事実と違った点が二つあります。
  - `ports/mesa/nocturne_os.c` は swapchain ではありません。`os_read_file` など `util/os_file.h` の三つの関数だけです。
  - NVIDIA の参照は `D:\Programes\cloned repos\open-gpu-kernel-modules-615.78.08` にあります。

所見の書式は次のとおりです。

- 重さ：致命、重い、中、軽い。
- 確信：コードで証明、可能性あり。「コードで証明」は、呼出しの順がコードで閉じているという意味です。動かして見たという意味ではありません。

---

## 問 1：黒と全面の描き直し

### コードで証明した黒の道

#### B1 窓の大きさを変えると、新しい部分が黒く出る

- 重さ：重い。種類：新しい緩衝を描く前に見せる。確信：コードで証明。
- 引き金：
  - 最大化と戻す操作（`kernel/src/gui/wm.c:1904-1926`）
  - 全画面の切り替え（`wm.c:3025-3052`）
  - リサイズの離し（`wm.c:2126-2133`）
  - `windows_refit`（`wm.c:1497-1510`）。予約の変更、モードの変更、配置で走ります。
  - シェルの再起動で予約が消えるとき（`wm.c:1513-1527`）
- 呼出しの順：
  1. カーネルが `EV_RESIZE` を送ります。
  2. `user/libc/gui.c:183-193` の `win_event` が、アプリへ渡す前に自動で `win_resize` を呼びます。
  3. `wm.c:2872` が新しい面を作ります。`wsurf_new` は 0 で埋めます（`wm.c:221,225`）。
  4. `wm.c:2874-2879` が古い絵を左上へ写します。残りは黒のままです。
  5. `wm.c:2893-2904` が面を差し替え、`wm.c:2910` が `damage_window` を呼びます。
  6. compositor が起き、`draw_layer` が不透明の `gfx_blit` で 0 の画素を絵へ写します（`wm.c:1186-1238`）。
  7. アプリが描いて present するまで、黒が画面に残ります。
- 黒が延びる条件：`user/libc/ui.c` は canvas の大きさを待つ間 `w->hold` を立て（`ui.c:1403`）、present しません（`ui.c:2910`）。見直しは `w->now + 100` の 100 ms 後です（`ui.c:2840`）。
- ずれの二つ目：`toggle_maximize` は `x`、`y` を先に変え、`cw`、`ch` は変えずに `EV_RESIZE` を送ります（`wm.c:1904-1926`）。窓の位置と飾りが先に動き、中身は後から来ます。受け答え（configure と ack）の形がありません。
- ずれの三つ目：全画面へ入ると panel が先に隠れます（`wm.c:599-604`）。

#### B2 別サイズのモードに変えると、黒が最低 1 blank 出る

- 重さ：重い。種類：新しい緩衝を描く前に見せる、コメントと食い違う動き。確信：コードで証明。
- 引き金：解像度の変更、起動の最初の mode set で firmware のサイズと違うとき。
- 呼出しの順（`ports/nvidia/display.c`）：
  1. `mode_show`（`:1294-1352`）が新しいサイズの buffer を作ります。
  2. `desk_fill(&next[0], ...)`（`:1314`、本体 `:592-620`）が `next[0]` を黒で埋めます（`:610-611`）。
  3. `desk_give`（`:1320`）がカーネルへ渡します。`kernel/src/dev/pciuser.c:435` は `shown` をいつも 0 で渡します。
  4. compositor は「表示していない緩衝」、つまり buffer 1 にしか描きません（`kernel/src/dev/fb.c:513-514`）。
  5. `mode_set`（`:1332`）が `next[0]`、つまり黒い方を載せます。
  6. flip スレッドは `head_lock` で待ちます（`:1532-1553`、`state_thread` が `:1506-1529` で持っています）。
  7. 黒が表示され、flip スレッドが動いた後の blank で初めて絵が出ます。
- コメントとの食い違い：`display.c:34-35` と `:1288-1289` は "the desktop is drawn at the new size by the time the head shows it" と書いています。実際に表示される buffer については偽りです。

#### B3 サイズを大きくする途中の競合で、初期化していない VRAM が出る

- 重さ：中。種類：flip と内容の順序。確信：呼出しの順はコードで証明、時機は動かして確かめていない。
- 呼出しの順：
  1. compositor が古いサイズの絵で `frame()` を走らせています（`wm.c:1430-`）。
  2. 途中で `screen_frame_begin`（`wm.c:1451`）が generation の変化に出会います。
  3. 全 `stale` を埋めます（`wm.c:1453-1456`）。tile の表は古いサイズのままです。
  4. 古いサイズの tile だけを新しい buffer 1 へ書いて flip します（`wm.c:1460-1467`）。
  5. 外側は誰も埋めていない VRAM です。`desk_fill` が埋めるのは `next[0]` だけです（`display.c:592-620`）。`display.c:1624` の注は VRAM が前の持ち主の残りのままだと認めています（これは `image_create` の注です）。
- 条件：大きくするモードの変更と compositor の frame が重なるとき。

#### B4 直接表示の終わりに、黒か古い絵が出てから全部描き直される

- 重さ：重い。種類：新しい緩衝を描く前に見せる。確信：コードで証明。
- 対象：全画面の WebGL で直接表示が効く場合だけです。zink でない描画では効きません（`ports/mesa/nocturne_gl.c:468-660`、`user/libc/web/web_presentation.c:324-350`）。
- 引き金：通知、開始メニュー、音量の overlay（`wm.c:802-809`）、焦点の移動、Alt-Tab、resize の帯（`wm.c:811-814`）。「何かのきっかけで」に一番合う道です。
- 呼出しの順：
  1. 直接表示の間、`user/apps/browser.c:730-747` は窓の面を描きません。窓の面は全画面へ入るときの `win_resize` の 0 と古い絵のままです。
  2. 上の事象で `direct_update`（`wm.c:829-865`）が直接表示を切ります。
  3. `state_thread` → `show(NULL, NULL)` が desktop の buffer へ flip します（`display.c:962-1022`）。
  4. compositor の絵は窓の古い面からできています。黒か古い絵が出ます。
  5. アプリが `EV_DIRECT` の key 0 を受け（`user/libc/ui.c:2646-2652`、`browser.c:3530-3532`）、全部描いて present します。
  6. そこで初めて正しい絵になります。

#### B7 シェルの `desk_fit` の間、壁紙の新しい部分が黒い

- 重さ：中。種類：描く前に見せる。確信：コードで証明。
- 順序：`user/apps/shell.c:316-318` は `win_resize` の後で `desk_paint` を呼びます。その間に compositor が走れば新しい部分は黒です。
- 条件：同じ種類の面のサイズの変更。深さの変更は新しい窓を先に作って黒を避けてあります（`shell.c:301-329`）。この避け方が大きさの変更には入っていません。

#### B8 シェルの窓が追いつくまで、ほぼ黒の BACKGROUND が出る

- 重さ：軽い。種類：既定の色の露出。確信：コードで証明。
- 順序：`compose_rect` は窓に覆われない rect を `BACKGROUND`（`RGB(12,10,26)`、`wm.c:64`）で埋めます（`wm.c:1281-1284`）。
- 条件：シェルの起動と再起動、画面の配置の変更の直後。

### 可能性だけの黒の道

| ID | 重さ | 道 | 根拠 | 確かめられない点 |
|---|---|---|---|---|
| B5 | 重い | flip の timeout から `hand_back` | `display.c:863-889`、`:1239-1259` | 通常の flip の道では timeout に届きません（F2）。届くのは次の `show` が走るときだけです。 |
| B6 | 重い | claim の終わり（ドライバーが落ちたとき） | `pciuser.c:546-553`、ABI の文書は「その画面は実際には映らないことがある」と認めています | 表示エンジンを firmware の形へ戻す道がありません。頻度は不明です。 |
| B9 | 軽い | 最初の present が一部の矩形のとき、残りが黒い | `wm.c:2818-2854`、`wm.c:221,225` | アプリごとの描き方によります。 |
| B10 | 軽い | 直接表示の像が動くと clear されて黒くなる | `ports/mesa/nocturne_gl.c:468-660` | zink の全画面の WebGL だけです。 |

### 黒が出ないと確かめた所

- `compose_rect` の `BACKGROUND` は RAM の絵 `ws->back` の中だけです。tile の書き込みの前に上の窓が描かれるので、窓に覆われた所では見えません。
- `wscreen_resize` の fill（`wm.c:1531-1597`）も RAM の絵の中だけです。
- `screen_frame_write` は表示していない緩衝へ書きます（`fb.c:513-514`）。枚数が 2 以上の道では、途中の絵は見えません。

### 全面の描き直しを起こす条件

合成の全面（`damage_all`、`wm.c:539-547`）：

| 呼び手 | 場所 | 引き金 |
|---|---|---|
| `wscreen_resize` | `wm.c:1581` | モードの変更、`set_deep_simulation`（`wm.c:1790-1810`）、遠隔の viewer |
| `follow` の on/off | `wm.c:1671` | 画面の on と off |
| `win_destroy_locked` | `wm.c:903` | resize の途中の窓の破棄 |
| `wm_screen_arrange` | `wm.c:2439` | 画面の配置の変更 |
| `remote_size` | `wm.c:2501` | 遠隔の viewer のサイズ |
| `wm_repaint` | `wm.c:2533-2538` | hold の解放、claim の終わり、`kernel/src/dev/pm.c:404`（sleep が失敗しても呼びます） |

書き込みの全面（合成はしません）：generation の変化で全 `stale` を埋めます（`wm.c:1453-1456`）。

実質の全面：

- 焦点の移動：`update_focus`（`wm.c:738-752`）は両方の窓の全体と panel を damage します。シェルの全部描きも起こします（`shell.c:1105-1110`）。最大化の窓なら画面のほぼ全部です。
- タイトルの変更：`SYS_WIN_SET_TITLE`（`wm.c:3075-3083`）→ `windows_changed` → タスクバーを全部描きます（`shell.c:406-438`）。一文字の変更でタスクバーの全体が damage になります。
- アプリの whole：`ui.c:2877-2878,2892-2893` で、面積の 3/4 を超えるとき、`WIN_FRAME_QUEUE` のとき、`EV_RESIZE`、`EV_DIRECT` の 0。

---

## 問 2：合成器の重さ

### 線との照合

| 依頼者の線 | 答え | 根拠 |
|---|---|---|
| 1440p 三枚（280〜165 Hz）と 1080p 600 Hz に、それぞれの時刻で毎コマ間に合うか | 間に合いません。実機の NVIDIA の道は一枚しか出しません。 | C1 |
| 一枚の中で、全面の damage が毎コマ来ても間に合うか | 間に合う証拠がありません。設計は間に合わない方へ傾いています。測っていません。 | H1、H2、下の計算 |
| AAA・動画・SNS を同時に動かしてカクつき 0、落ちるコマ 0 か | 満たしません。AAA は窓へ出す道がありません。落ちたコマを数える物差しも製品にありません。 | C2、H7、F15 |
| y-cruncher で全コアを使い切る間、滑らかか | 満たさない見込みです。合成器と desktop は deadline 級で本物です。flip の鎖が fair 級です。遅れの量は測っていません。 | H9、H10 |
| 8000 Hz の入力に追いつくか | 受け取りは追いつきます。プログラムへ渡す形がありません。 | H11 |
| 拍、上限、間隔、数を定数にしていないか | 定数にしている所があります。 | 問 5 の K の表 |
| 1080p 1000 Hz に一コマ 1 ms で間に合うか | 全面の damage では間に合う証拠がありません。ポインターだけの frame は、空いた機械で `commit` が短ければ間に合う見込みです。`commit` の時間を測っていません。全コアの負荷の下では間に合いません（見積り）。 | 下の「1 ms の線」 |

### 一 frame の手順

CPU だけで走ります。GPU は合成に使いません。スレッドは画面ごとのカーネルのスレッド `compositor N` です（`wm.c:1755`）。

毎回の起床（`compositor`、`wm.c:1723-1750`）：

1. `screen_events`。
2. `follow`（`wm.c:1648-1696`）。`screen_mode` と `wm_lock` を取ります。
3. `screen_times`。
4. `wm_lock` の下で `notes_send`（O(窓の数)）と `windows_wait`（O(窓の数)）。
5. 仕事が無ければ眠ります。

frame の本体（`frame`、`wm.c:1430-1489`）：

1. `back_lock` と `wm_lock` を取ります。
2. `capture`（`wm.c:1337-1389`）。全窓を 3 回走査します。窓ごとに `pacing`（O(画面の数)）、タイトルの `memcpy`、面の参照を取ります。
3. `tiles_rects`（`wm.c:1436`）。`kmalloc` が 1 回です（`wm.c:157`）。
4. rect ごとに `compose_rect`（`wm.c:1274-1313`）。
   - 覆われなければ `BACKGROUND` で埋めます。
   - 重なる窓ごとに、`mutex_lock(&l->src->lock)` を取り、`draw_layer` を走らせます（`wm.c:1288`）。
   - 不透明の窓は `gfx_blit`（`common/gfx.c:214-216`、行の `memcpy`）。
   - blend の窓と影とポインターは `gfx_blit_alpha`（`gfx.c:241-249`）で、スカラーの毎画素です。
5. `screen_frame_begin`（`wm.c:1451`、`fb.c:499-526`）。空いた緩衝を最大 1 秒待ちます。
6. `tiles_or` と `tiles_rects`（`wm.c:1459`）。`kmalloc` が 2 回目です。
7. rect ごとに `screen_frame_write` → `present_area`（`fb.c:181-224`）。native の形式は `memcpy` で、BAR1 の WC の写像へ書きます（`fb.c:197-198`）。
8. `SCREEN_BUFFERS_MAX` 回の `tiles_or`（`wm.c:1464-1465`）。緩衝は 2 枚なのに 4 回まで回ります。
9. `screen_frame_end`（`wm.c:1467`、`fb.c:534-549`）。sfence の後で flip を求めます。変化が無くても呼びます。
10. `wm_lock` を取り、`finish`（`wm.c:1406-1415`）。

その後の利用者空間（nvrm）：

11. `flip_thread`（`display.c:1532-1553`）が `n_pci_screen_flips` から起き、`head_lock` を取ります。
12. `desk_flip`（`:1164-1199`）→ `show` → `commit`（`:761-776`）が NVKMS の KAPI で flip を出します。

### 何度写すか

| 窓の種類 | 写し | 回数 |
|---|---|---|
| rect の窓 | アプリの面 → kernel の `stable`（`wm.c:2837-2839`）→ `back`（`gfx_blit`）→ BAR1 | 3 |
| ブラウザーの本文 | view の面 → 窓の面（`ui.c:1376-1387`、`ui.c:3103`）→ `stable` → `back` → BAR1 | 4 |
| queue の窓 | アプリの面 → `back` → BAR1 | 2 |

ラスターと動画の拡縮はこの外です。動画はソフトウェアで解いて、`user/libc/media_draw.c` の SSE2 で拡縮します。ハードウェアの解読の道は grep の範囲で見つかりません（`hwaccel`、`nvdec`、`VK_KHR_video` が `user`、`ports/ffmpeg`、`ports/nvidia` に無い）。

全面の大きさ（計算）：

| 画面 | 一枚の大きさ | 周期 | 一枚の全面の BAR1 への書き込みの速さ |
|---|---|---|---|
| 2560×1440、280 Hz | 14.75 MB | 3.571 ms | 4.13 GB/s |
| 2560×1440、165 Hz | 14.75 MB | 6.061 ms | 2.43 GB/s |
| 1920×1080、600 Hz | 8.29 MB | 1.667 ms | 4.98 GB/s |
| 3840×2160、240 Hz | 33.18 MB | 4.167 ms | 7.96 GB/s |

- 依頼者の四枚が全部毎コマ全面なら、BAR1 への書き込みの合計は 14.0〜15.7 GB/s です（真ん中の一枚を 165〜280 Hz として計算）。
- これは全部 CPU の WC への書き込みです。`back` への写しが同じ量だけ別にあります。
- ブラウザーの全面、1440p、60 fps の写しは 14.75 MB × 4 × 60 = 3.54 GB/s です（計算）。
- 一画素に使える時間（計算、5 GHz と仮定）：280 Hz の 1440p は 4.8 cycle、600 Hz の 1080p は 4.0 cycle です。合成と書き込みの全部で、です。

速さは測っていません。`rep movsb` の WC への速さを確かめていません。上の数は要る速さであって、出る速さではありません。

### 所見

#### H1 合成はカーネルの中のスカラーの CPU の処理

- 重さ：重い。種類：設計の詰まり。確信：コードで証明。
- 根拠：
  - `Makefile:34-36`：カーネルは `-mgeneral-regs-only -mno-sse`。
  - `kernel/src/lib/string.c:4-8`：`memcpy` は `rep movsb`。
  - `common/gfx.c:241-249`、`:123-126`：blend はスカラーの毎画素。
  - 同じ `gfx.c` を組む利用者側は `-msse2` です（`Makefile:118-119`）。
- 条件：blend の面積が大きいとき、全面の frame。RTX 4070 があっても使いません。
- AGENTS.md の「性能に関わる道具は最適化つき」と、GUI が先の線に反します。

#### H2 画素の写しが多い

- 重さ：重い。種類：重複。確信：コードで証明。根拠は上の表です。
- `stable` の写しは「変更の半分が映らない」ための本物の仕組みです。費用は全 rect の窓に毎回かかります。

#### H3 隠れた窓も描く

- 重さ：中〜重い。種類：全走査。確信：コードで証明。
- `first_layer`（`wm.c:1260-1272`）は「合わせた rect 全体を一つの窓が覆う」ときだけ下を省きます。tile ごとの不透明の判定がありません。
- `present_rect` の damage は `visible` だけを見ます（`wm.c:2846`）。隠れた窓の present も合成と flip を起こします。
- 費用は damage の面積 × 重なる窓の数で増えます。窓が多いときの詰まりはここです。

#### H4 変化の無い frame も flip する

- 重さ：中。種類：起きなくてよいのに起きる。確信：コードで証明。
- `windows_wait`（`wm.c:1419-1425`）は `visible && frame_wanted`、または `queue && nqueued` で frame を起こします。`nqueued` の側は `visible` を見ません。最小化の queue の窓でも起きます。
- `screen_frame_end` は変化が無くても呼びます（`wm.c:1467`）。
- アニメーションの窓が `SYS_WIN_FRAME` を出すたびに、絵が同じでも flip が一つ走ります。

#### H5 何も変わらなくても blank ごとに全部が起きる

- 重さ：重い。種類：コメントと食い違う動き。確信：コードで証明。
- 呼出しの順：
  1. NVKMS の RG semaphore の割り込みが blank ごとに来ます。参照の `nvkms-rm.c:5395-5470` で `RgVBlankCallback` が blank ごとに `nvkms_alloc_timer_with_ref_ptr` を呼びます。
  2. timer が queue の worker（`ports/nvidia/nvkms_os.c:620`）で `vblank_arrived`（`display.c:1054-1073`）を呼びます。
  3. `vblank_arrived` は無条件に `n_pci_screen_vblank` の syscall を出します（`:1070`）。
  4. `pci_user_screen_vblank`（`pciuser.c:972-986`）が全体の `screen_lock` の mutex を取ります（`:976-979`）。
  5. `screen_vblank`（`fb.c:564-570`）が `screen_wake` を呼びます。
  6. compositor が一周します（`wm.c:1726-1733`）。`wm_lock` を 2 回取って、窓を全部走査して、眠ります。
- ABI の文（`docs/nocturne-native-abi.md`「ほかの帰線はフリップを待つ間に知らせる」、`common/abi.h:559-563`）と違います。
- `vblank_count` と `vblank_ns` を読む者は居ません（F5）。この wake に用がありません。
- 依頼者の四枚では、何もしなくても毎秒 1210〜1325 回、この鎖が走ります（計算）。

#### H6 ポインターを毎コマ絵に焼く

- 重さ：中。種類：設計の詰まり。確信：コードで証明。
- ハードウェアのカーソルは直接表示のときだけです（`display.c:625-657`）。
- 動くたびに `damage_cursor`（`wm.c:2168-2171`）→ frame → flip の全部が走ります（`wm.c:1303-1311`）。
- 全コアの負荷の下で、ポインターの滑らかさが flip の鎖に縛られます。ハードウェアのカーソルなら flip を待ちません。

#### H7 窓の中の lock を持って全面を写す

- 重さ：中〜重い。種類：lock の持ち方。確信：lock の順はコードで証明、待ちの長さは測っていない。
- 順序：
  - `present_rect`（`wm.c:2837-2839`）はアプリのスレッドで、`copy->lock` を持ったまま矩形の全部を `surf_copy` します。
  - `compose_rect`（`wm.c:1288`）は同じ `lock` を取ります。
- 条件：全面に近い present（動画、ブラウザーのスクロール）。深い面があれば `deep` の分が増えます。
- 動画と同時のときの詰まりはここです。合成器はアプリの写しが終わるまで、その窓の上で待ちます。優先度の継承はあります（`struct mutex`）。継承を受けたアプリのスレッドが写す時間は短くなりません。

#### H8 全体の `wm_mutex` と窓の全走査

- 重さ：中。種類：全走査、全体の lock。確信：コードで証明。
- 毎 frame の走査：`capture`（3 回）、`finish`、`notes_send`、`windows_wait`。全部 `wm_lock` の下です。
- 合成そのものは `wm_lock` の外で走ります（`wm.c:1437-1449`）。画面ごとに並びます。ここは本物です。
- 取る回数（毎コマ）：`follow`、compositor の本体、`capture`、`finish` で最低 4 回です。四枚の合計で毎秒 5000 回を超えます（計算）。
- 入力：`desktop_thread`（`wm.c:2240-2287`）は事象の取り出しの間ずっと `wm_lock` を持ちます。8000 Hz なら毎秒最大 8000 回取ります。
- 窓の数が 10 倍になると、全部 10 倍です。`pacing`（`wm.c:574`）は画面の数も掛かります。
- 毎 frame の確保：`tiles_rects` の `kmalloc`（`wm.c:157`）が 2 回です。

#### H9 合成器は deadline 級で、flip の鎖は fair 級

- 重さ：致命（y-cruncher の線に対して）。種類：スケジューリングの逆転。確信：級はコードで証明、遅れの量は見積り。
- 鎖（毎コマ）：
  1. GPU の割り込み → nvrm の `irq_thread`（`ports/nvidia/nvrm.c:94,140`）。fair 級です。
  2. `rm_isr`（`nvrm.c:66-68`）→ `RgVBlankCallback` → `q_thread`（`nvkms_os.c:620`）。fair 級です。
  3. `vblank_arrived` の syscall → compositor が起きます。ここは deadline 級です。
  4. compositor の flip の要求 → `flip_thread`（`display.c:1826`）。fair 級です。
  5. `head_lock`、`commit`。
- `n_thread_schedule` の呼び手は `user/apps/audio.c:1264`、`user/apps/browser.c:53`、`user/libc/web/webaudio.c:1074` です。`ports/nvidia` に無いことを grep で確かめました。
- `screen_wait` は wake で待つので、優先度の継承が効きません。
- fair 級の起床の横取りは `preempts`（`kernel/src/sys/sched.c:563-567`）で、起こされた側の仮想の期限が早いときだけです。既定の要求は 3 ms です（`sys/sched.h:52`）。全コアが埋まっていれば、起こされたスレッドは動いている者の要求の残りを待ちます。
- 見積り（仮定：各段が要求の 3 ms を待つ、最悪）：3 段で 9 ms です。比べる周期は 1000 Hz の 1 ms、600 Hz の 1.67 ms、280 Hz の 3.57 ms、165 Hz の 6.06 ms です。
- 結果の形：合成器は `asked == shown` になるまで次の frame を始めません（`wm.c:1735`）。画面は周期の整数倍へ落ちます。ポインターも一緒に落ちます。

#### H10 予約が軽い frame に引きずられ、全面の frame で throttle される

- 重さ：重い（負荷の下で）。種類：測りの形の誤り。確信：規則はコードで証明、遅れの量は見積り。
- 規則：
  - 予約は「直近のピークの 2 倍」で、範囲は `[period >> 6, period - (period >> 4)]` です（`sched.c:441-446`、`:74-81`）。
  - ピークは一 job ごとに 1/32 減ります。
  - H5 の無駄な起床も一 job に数えます（`job_end`、`sched.c:812-817`）。
  - 予約を使い切ると、deadline まで fair 級で走ります（`throttle`、`sched.c:466-478`）。
- 計算：半減は約 22 job です。5.9 回半減すると下限に着きます。何もしない時間が 280 Hz で 0.46 秒、600 Hz で 0.22 秒、165 Hz で 0.79 秒あれば、予約は下限です。下限は 55.8 µs、26 µs、94.7 µs です。
- 次の全面の frame は、下限を使い切った時点で fair 級へ落ちます。予約は一周期に 2 倍しか戻りません。上限まで約 6 周期かかります。
- 全コアが埋まっていれば、その間は y-cruncher と同じ級で走ります。黒の後の全面の描き直しが負荷の下で遅く見える原因の候補です。
- 上限も線に当たります。予約の上限は 280 Hz で 3.35 ms、600 Hz で 1.56 ms です。それを超える frame は毎回 fair 級へ落ちます。

#### H11 8000 Hz の入力：受け取りは本物で、渡す形が無い

- 重さ：重い（ゲームに対して）。種類：途中で切れた道。確信：コードで証明。
- 本物の部分：
  - xHCI の周期の interrupter は `IMOD` 0 です（`kernel/src/dev/xhci.c:2030`）。
  - 周期は装置の `bInterval` から計算します（`xhci.c:954`）。
  - 転送は最大 8 個並べます（`xhci.c:1216-1221`）。
  - 報告は割り込みの中で `drain` から `hid_report` → `input_mouse` → `input_put` へ届きます（`xhci.c:596`、`xhci.c:656`、`usbhid.c:274`、`input.c:160-173`）。
  - `desktop_thread` は deadline 級で、周期は一番速い画面のものです（`wm.c:2268-2270`）。
- 全コアの負荷の下でも、この段は遅れません。
- 欠けの部分：
  - `struct gui_event` に相対の動きがありません（`common/abi.h:771-785`）。`x`、`y` は窓の座標です。
  - ポインターは画面の端で止まります（`wm.c:2160-2167`）。視点を回すゲームに要る相対の動きが届きません。
  - `EV_MOUSE_MOVE` は前の事象を上書きします（`wm.c:682-688`）。途中の点と時刻が消えます。
  - `requestPointerLock` は生成された `user/libc/web/webidl_data.inc:1812` にあるだけで、ほかに実装を grep で見つけられません。
  - `getCoalescedEvents` はいつも `[]` です（`user/libc/web/js_pointer_events.js:42`）。
- 無駄：報告ごとに `desk_wake`（`wm.c:1870`）で deadline 級のスレッドを起こします。全コアの負荷の下では y-cruncher を毎秒最大 8000 回横取りします。ただし予約が尽きると周期の終わりまで待つので（H10 の規則）、実際は周期ごとに数回へまとまります。費用は測っていません。
- 上限：入力の輪は固定の `INQ 1024`（`wm.c:1818`）で、8000 Hz では 128 ms 分です（計算）。満杯のときポインターは最後の事象へ合わせ、キーの押下は失います（`wm.c:1851-1869`）。

### 1 ms の線（1080p 1000 Hz、ほかの画面と同時を含む）

周期の計算は定数ではありません。`refresh_mhz` は NVKMS の `RRx1k` から来ます（参照の `nvkms-kapi.c:2484`、`display.c:240-243`）。1000 Hz は 1,000,000 mHz で `uint32_t` に収まり、`period_ns` は 1,000,000 ns ちょうどです（`wm.c:1659`）。周期の表現は 1000 Hz を妨げません。

#### 合成の費用

| 項目 | 値（計算） | 1 ms に対して |
|---|---|---|
| 全面の `back` への写し | 8.29 MB | 下と合わせて単一のスレッドで 16.6 GB/s 以上が要ります |
| 全面の BAR1 への書き込み | 8.29 MB、毎秒 8.29 GB | WC への `rep movsb` の速さを測っていません |
| 一画素の時間（5 GHz と仮定） | 2.4 cycle | 合成と書き込みの全部で、です。スカラーの blend（`gfx.c:241-249`）は入らない見込みです |
| 予約の上限（`dl_most`） | 937.5 µs | 越えた frame は毎回 fair 級へ落ちます（H10） |
| 予約の下限（`dl_least`） | 15.6 µs | 何もしない時間が 0.13 秒あれば、予約はここまで減ります |
| 下限から上限へ戻る時間 | 約 6 周期 | 全面の frame の最初の約 6 コマは、負荷の下で予約が足りません |
| アプリの全面の present の写し | 8.29 MB、毎秒最大 8.29 GB | 別のスレッドの分です |

- ポインターだけの frame は、32×32 の二か所の tile を描くだけです。画素の費用は小さく、`frame()` の手続きと鎖の時間が支配します。
- 緩衝は 2 枚で、flip は一つしか待てません（`display.c:97`、T2）。1 ms に一度でも入らなければ、そのコマを落とします。先に描いておく余地がありません。

ほかの画面と同時の場合（計算、全部の画面が毎コマ全面、真ん中の 1440p を 165〜280 Hz とする）：

| 組み合わせ | BAR1 への書き込みの合計 |
|---|---|
| 1440p 三枚 + 1080p 1000 Hz | 17.3〜19.0 GB/s |
| 1440p 三枚 + 1080p 600 Hz + 1080p 1000 Hz | 22.3〜24.0 GB/s |

比べる値として、PCIe 4.0 x16 の一方向の上限は 16 GT/s × 16 × 128/130 ÷ 8 = 31.5 GB/s です（規格の値からの計算で、TLP の header を含みません）。WC の書き込みを 64 byte の TLP とし、header などを 24 byte とすると、約 23 GB/s です（見積り）。リンクの速さは機械ごとに違うので、これは例です。

CPU から BAR を通して全面を書く形は、この線でバスの上限に届きます。全部の画面が毎コマ全面という最悪の場合でも、です。合成器の工夫では越えられません。GPU が VRAM の中で合成する形でなければ、この線は通りません。

#### スレッドの起こしの遅れ

| 段 | 級 | 空いた機械 | 全コアを使い切った機械 |
|---|---|---|---|
| GPU の割り込み → カーネルの hard handler | 割り込み | 短い（測っていません） | 同じ |
| hard handler → nvrm の `irq_thread`（`n_pci_interrupt_take`、`nvrm.c:101`） | fair、要求 3 ms | MWAIT の語か IPI で起きます | 最悪 3 ms、3 コマ分 |
| `rm_isr` → `RgVBlankCallback` → `q_thread`（`nvkms_os.c:576`） | fair、要求 3 ms | 同上 | 最悪 3 ms、3 コマ分 |
| `vblank_arrived` の syscall → compositor | deadline | 同上 | fair を横取りします（`sched.c:563-567`） |
| compositor → `flip_thread`（`pciuser.c:400-412`） | fair、要求 3 ms | 同上 | 最悪 3 ms、3 コマ分 |
| `flip_thread` → `commit` | fair | 測っていません | 測っていません |

- 深い C-state の待ちはありません。idle は MWAIT の hint 0、つまり C1 だけです（`kernel/src/arch/idle.c:7-8,58`）。起きる費用は短い見込みです。一段あたりの時間は測っていません。
- 割り込みの vector が無い機械では、`IRQ_POLL_MS 1`（`nvrm.c:46,113`）で見に行きます。1 ms は一コマの全部です。blank の知らせが毎コマ遅れます。
- 全コアの負荷の下の結論は H9 のとおりです。3 段の最悪は 9 コマ分です（見積り）。

#### lock の持ち時間

どの lock も持ち時間を測っていません。下は構造と計算です。

| lock | 取る頻度（計算） | 持つ時間の形 | 1 ms に対して |
|---|---|---|---|
| `wm_mutex`（全体） | frame で毎秒 6440〜6900 回（1440p 三枚 + 1000 Hz、一コマ 4 回）。8000 Hz の入力で毎秒最大 8000 回。present で一回あたり 2 回 | O(窓の数)。`capture` は窓ごとにタイトルの 64 byte と面の参照を触ります | 取る間隔の平均は約 68 µs です。持ち時間がこれに近づくと待ちが並びます。窓が数十を超えると見える可能性があります（可能性あり） |
| `copy->lock`（窓ごと） | present ごと | 矩形の大きさ ÷ 写しの速さ。8.29 MB を 20 GB/s（仮定）なら 0.41 ms | 一コマの 41% です。合成器はこの間その窓で待ちます（H7） |
| `back_lock`（画面ごと） | 毎コマ | frame の全部 | 定常では競いません |
| `screen_lock`（`pciuser.c`、全体） | blank ごと、全部の出力で共有 | 短い | 短いですが、用の無い取り方です（H5） |
| `nvkms_lock`（全体の semaphore、`nvkms_os.c:14-17,336`） | `commit` と blank の仕事で、四枚なら毎秒約 3300 回（計算） | NVKMS の `commit` の全部 | 平均の持ち時間が約 300 µs を越えると、この lock だけで溢れます（計算）。`commit` の時間は測っていません |
| `head_lock`（`display.c`） | flip ごと | `commit` の全部 | モードの変更の間は flip が待ちます |

- `nvkms_lock` は NVIDIA の規則です（`nvkms_os.c:14-17`）。作りを変える相手ではありません。一つの頭の `commit` が別の頭の blank の知らせを遅らせます。1000 Hz の頭が 165 Hz の頭の `commit` を待つことになります。
- 複数の出力を実装しても、`q_thread` と `flip_thread` は一本ずつです（`nvkms_os.c:620`、`display.c:1826`）。
- `commit` の時間は `scr.flip_commit_ns` に記録があります（`display.c:877-880`）。失敗のログにしか出ません。この線で一番の不明がこの値です。

### 大きさごとの詰まり

| 場面 | 詰まる所 |
|---|---|
| 2560×1440、最大化の窓が一つ | 全面の present で 3〜4 回の写し（H2）、`copy->lock`（H7）、周期の縛り |
| 4K | 同じ形で面積が 2.25 倍です。`rep movsb` と blend の残り時間が足りません（H1）。 |
| 窓が多い | 隠れた窓の描画（H3）、全走査と `wm_mutex`（H8） |
| 動画と同時 | ソフトウェアの解読と拡縮、4 回の写し、`copy->lock` |
| 何もしていないとき | blank ごとの起床（H5） |

合成の時間と書き込みの時間は `stats_print`（`wm.c:1699-1713`）で測れます。`prof` の窓があるときだけです。測るならここです。

---

## 問 3：frame の拍

### 線との照合

| 依頼者の線 | 答え |
|---|---|
| 画面ごとの時刻で動くか | 合成器の側は動きます。周期はモードの `refresh_mhz` から計算します（`wm.c:1659`）。定数ではありません。実機では一枚だけです（C1）。 |
| VRR の三枚 | VRR は動きません（F1）。固定の refresh で動きます。 |
| 600 Hz | 動く証拠がありません。一つの flip しか待てないので、鎖と合成と commit が 1.667 ms に入らなければ半分の拍へ落ちます（T2）。測っていません。 |
| 落ちるコマ 0 | 落ちたコマを数える物差しがありません（F15）。 |
| y-cruncher の間の滑らかさ | H9 と H10 のとおり、満たさない見込みです。 |
| 1000 Hz、一コマ 1 ms | 拍の時計はカーネルと NVKMS の側で足ります。プログラム側の整数の ms の時計が足りません（下の「拍の時計の分解能」）。鎖と `commit` は測っていません。VRR と 10 bit の HDR10 の絵は出ません（F1、K7）。モードの選択と DisplayPort の帯域は別の担当です。 |

### 実機の NVIDIA の道

本物です。確かめた順序は次のとおりです。

1. flip は NVKMS の KAPI `applyModeSetConfig` で出ます（`display.c:761-776`）。
2. `minPresentInterval 1`、`tearing false` です（`:697-698`）。vsync に載ります。
3. 完了は notifier で読みます（`nvrm_nvkms_flip_done`、`ports/nvidia/nvkms_kapi_rm.c`）。
4. blank は RG semaphore です（参照の `nvkms-modeset.c` の `SetupVBlankRgSemaphoreForApiHead`）。
5. `vblank_arrived` が blank の時刻を NVKMS の時刻で渡します（`display.c:1054-1073`）。参照の `GetVblankTimestamp` は CPU の時間域へ換算します。
6. `EV_FRAME` は flip が表示された後に出ます（`wm.c:1393-1402`）。

拍は blank から来ます。合成の始まりは「前の flip が表示された」知らせです（`wm.c:1735`）。

### 所見

#### T1 最低 1 frame の遅れ

- 重さ：中。種類：設計。確信：コードで証明。
- frame は shown の知らせの直後に始まり、`capture` で damage を切ります（`wm.c:1433-1435`）。
- 描いている間の damage は次の frame へ回ります。
- flip は次の blank で表示されます。入力から表示まで最低 1 周期です。
- 始まりの位相を遅らせる（表示の直前に間に合う時刻を測る）仕組みはありません。

#### T2 flip は一つしか待てず、遅れると半分の拍へ落ちる

- 重さ：重い（高い Hz に対して）。種類：設計の詰まり。確信：順序はコードで証明、時間は測っていない。
- 合成器は `asked == shown` の間だけ次を始めます（`wm.c:1735`）。緩衝は 2 枚です（`display.c:97`）。
- 一周の中身：RG の blank → `irq_thread` → `q_thread` → syscall → compositor → 合成 → 書き込み → `flip_thread` → `head_lock` → `commit`。
- これが周期に入らなければ、その blank を落とします。入らない状態が続けば、半分の拍で安定します。
- `commit` の時間は記録だけで（`scr.flip_commit_ns`、`display.c:877-880`）、失敗のログにしか出ません。

#### T3 `desk_flip` の commit と `wait_serial` の間の blank

- 重さ：中。種類：順序。確信：可能性あり。
- `desk_flip`（`display.c:1164-1180`）は `show` で commit した後で `wait_serial` を置きます。
- その間に flip が載る blank が来ると、`vblank_arrived` は `wait_serial` が 0 なので知らせません。
- 知らせは次の blank まで遅れます。1 周期の無駄です。周期が短いほど当たりやすくなります。

#### T4 RG の trigger は blank の始まりで、notifier より前に読むと 1 周期遅れる

- 重さ：中。種類：順序。確信：可能性あり。
- 参照の `SetupVBlankRgSemaphoreForApiHead` は `rasterLine = rasterBlankStart.y` です（`nvkms-evo3.c:3559-3570`）。
- `vblank_arrived` は `nvrm_nvkms_flip_done` を読みます（`display.c:1059-1060`）。
- notifier の書き込みが読みの後なら、その blank では「まだ」と読みます。ハードの時刻は確かめられません。
- 当たると一周期遅れ、毎回当たると半分の拍です。

#### 拍の時計の分解能

カーネルと NVKMS の側は 1 ms に対して足ります。

| 時計 | 場所 | 分解能 | 1 ms に対して |
|---|---|---|---|
| カーネルの単調時計 | `kernel/src/sys/clock.c:169-176` | TSC からの ns。率は基準の時計で測ります | 足ります。本物です |
| 起こしの装置 | `kernel/src/arch/lapic_timer.c:3-20` | TSC-deadline は TSC の値で正確です。無い CPU は one-shot で、8 count までの刻みです | 足ります。本物です |
| 合成器の周期 | `wm.c:1659` | 整数の ns | 足ります。NVIDIA の道では時計で待たず、blank の知らせで動きます |
| NVKMS の時計 | `ports/nvidia/nvkms_os.c:289` | µs。一コマの 0.1% | 足ります |
| blank の時刻 | 参照の `nvkms-rm.c:5369-5420`、`display.c:1057` | RG の semaphore の ns を `gpuCpuTimeDiff` で CPU の時間域へ換算します | 時間域は合います（`osGetPerformanceCounter` → `os_get_monotonic_time_ns_hr` → `nvrm_now_ns`、`ports/nvidia/os.c:278-279`）。差は一回の標本（`nvkms-evo.c:8822-8842`）で、誤差を確かめていません |

足りないのはプログラム側です。

- T6 ブラウザーの rendering の時刻は整数の ms です。重さ：中。種類：測らずに置いた固定の値。確信：コードで証明。
  - `rendering_due` は `n_clock_monotonic() / N_NS_PER_MS` です（`user/libc/web/js.c:622`、`:9640-9641`）。
  - 周期は `rendering_period_ms + 0.5` で整数へ丸めます（`js.c:9641,9648`）。600 Hz の 1.67 ms は 2 ms に、280 Hz の 3.57 ms は 4 ms になります（計算）。
  - `web_tick` の引数も整数の ms です（`user/include/web.h:355`）。
  - 1000 Hz では時刻の一刻みが一コマの全部です。アニメーションの一コマの差が 0、1、2 ms に揺れます。これは時刻の丸めの形からの判断で、ページの動きを確かめてはいません。
  - 表示の知らせが来る前の周期は 16 ms の固定です（`js.c:10511`）。
- T7 UI の時刻も整数の ms です。重さ：中。種類：同上。確信：コードで証明。
  - `w->now` は `user/libc/ui.c:2403,2865,3297,3333` で ms に切り捨てます。
  - 回る印の拍は 33 ms の固定です（`ui.c:2845`）。
- 本物の部分：`EV_FRAME` の周期は ns で届き、`ui.c:2660` は `double` の ms で渡します（`emit`、`ui.c:454`）。`frame_schedule`（`user/apps/browser.c:50-54`）は表示の周期を丸めずに deadline の周期にします。ブラウザーの描くスレッドは deadline 級です。一プロセスの deadline の分は一 CPU の半分までです（`kernel/src/sys/thread.c:203`、`APP_DEADLINE_SHARE`）。1000 Hz なら一コマ 500 µs までで、越えた分は fair 級です。
- T8 `time_ns` の意味が ABI と違います。重さ：軽い。確信：コードで証明（先の時刻になる点は可能性あり）。
  - ABI の文書は「帰線の始まり」と書いています。
  - 参照の `GetVblankTimestamp` は trigger に `triggerToActiveOffsetNs` を足して、最初の有効な行の時刻にします（`nvkms-rm.c:5415-5420`）。
  - 周期が短いほど、この差は一コマに対して大きくなります。
  - 鎖が blank より短ければ、`EV_FRAME` の `time_ns` が今より先になる可能性があります。

#### T5 VM の道

| 道 | 動き |
|---|---|
| QEMU の VBE（`kernel/src/vm/vbe.c:80-84`） | Y offset のレジスターを書いて、すぐ `screen_flipped` を呼びます。vblank はありません。拍は `wm.c` の `frame_next` です。表示の位相と無関係です。 |
| firmware の framebuffer（一枚、`fb.c:259-262,506-509`） | 表示中へ直接書きます。テアリングが出ます。 |
| refresh が不明（0） | 上限なしに frame を出します（`wm.c:1683-1688`）。 |
| Hyper-V | 映像の driver がありません。`kernel/src/vm/hv/vmbus.c:155` の "video" は名前だけです。firmware の framebuffer と RDP です。RDP は 10 ms と 30 ms で見に来ます（`rdp.c:1794-1795`）。 |
| `virtio_gpu.c` | 画面の供給者ではありません。`screen_set_buffers` の呼び手は `vbe.c:177` と `pciuser.c:435` だけです。 |

VM で滑らかでも、NVIDIA の道の証拠になりません。鎖の長さが全く違います。

#### テアリングの所在

- count が 1 の firmware の道と、VM の一枚の道にあります。
- NVIDIA の道の desktop にはありません。表示中へ書かないことを確かめました（`fb.c:513-514`、`display.c:697-698`）。
- 窓の中のテアリング：rect の窓の複数の present は一つのまとまりにならないので、一枚の絵が別々の frame に割れる可能性があります（可能性あり）。

---

## 問 4：窓の緩衝

- 持ち主：カーネルの `wsurf` です（`wm.c:215-237`、参照の `refs` と `readers`）。
- 共有：`shared_kernel_new` の共有 object を `SYS_WIN_SURFACE` でアプリへ写します（`user/libc/gui.c:55-73`）。
- rect の窓：kernel の写し `stable` と mutex です（`wm.c:2818-2854`）。変更の半分は映りません。本物です。
- queue の窓：`SYS_WIN_ACQUIRE`、`SYS_WIN_PRESENT`、`EV_SURFACE` です（`wm.c:1317-1332`、ABI の文書）。合成は写さずに読みます。本物です。
- 同期：CPU だけです。GPU の fence や semaphore との同期がありません。
- 窓の大きさの変更：B1 のとおりです。受け答えがありません。
- 直接表示：全画面の窓が一つで、overlay が無いときだけです（`wm.c:811-814`）。本物です。ハードウェアのカーソルもここにあります。
- GL の窓：
  - 独立の窓の提示はありません。Mesa の GL は `ports/mesa/nocturne_gl.c` の中で自分の texture へ描きます。
  - 読み戻しは CPU です（`ngl_read_drawing_buffer`、`nocturne_gl.c:383-394`、`PIPE_MAP_READ`）。呼び手は `user/libc/web/js_webgl.c:726,739` です。
  - その後は窓の面 → `stable` → `back` → BAR1 です。
- Vulkan の窓：C2 のとおり、ありません。

### 致命の所見

#### C1 実機の NVIDIA の道は画面を一枚しか出さない

- 重さ：致命（依頼者の線に対して）。種類：途中で切れた道、VM だけの道、決め打ち。確信：コードで証明。
- 根拠：
  - `display.c:375-377`：ログに "the firmware lit %u connected displays; nvrm shows the desktop on display 0x%08x only" と書きます。
  - output は 0 に決め打ちです（`display.c:1057,1111,1549`）。`flips[i].output == 0` 以外の flip は捨てます。
  - `scr` は一つの大域の状態です。
- 食い違い：ABI の文書は「出力ごとに画面を一つ出す」「画面の数に上限は無く」と書いています。合成器（`wscreen_get`、`wm.c:1760-`）は複数の画面に対応しています。二枚目が出るのは QEMU の VBE だけです（`vbe.c:169`）。
- 条件：モニターが二枚以上つながった機械、いつもです。

#### C2 Vulkan のプログラムは窓へ絵を出せない

- 重さ：致命（依頼者の線に対して）。種類：成功を返して何もしない。確信：コードで証明。
- 根拠：
  - `ports/mesa/nvk.mk:200` は "WSI is the headless platform only" と書きます。`:220` が組むのは `wsi_common_headless.c` です。
  - 上流の `src/vulkan/wsi/wsi_common_headless.c:433-445`：`wsi_headless_swapchain_queue_present` は `busy_on_host` を倒して `VK_SUCCESS` を返すだけです。
  - `tests/nvk_contracttest.c:14-31` は headless の surface があることを試します。この合格は表示の証拠になりません。
  - `user/libc/vulkan.c` はローダーだけです。窓の surface の口がありません。
  - ネイティブの Vulkan の窓の surface の拡張は ports と user に見つかりません。
- 条件：Vulkan のプログラムが `vkQueuePresentKHR` を呼ぶと、エラーなしで何も映りません。
- 実機に残る道は、読み戻して `win_update` する CPU の写しだけです。AAA の周期には乗りません。

---

## 問 5：偽物の一覧

| ID | 重さ | 種類 | 場所 | 中身 | 確信 |
|---|---|---|---|---|---|
| F1 | 重い | 呼ぶ者のいない道 | `display.c:240-243`、`:697-698`、`wm.c:1660-1665,1689-1691,1736,1744`、`screenmode.c:102,171`、`user/apps/settings.c:954` | `N_SCREEN_MODE_VRR` を立てるコードがありません。`mode_of` の flags はいつも 0 です。KAPI の VRR も要求しません。`ws->vrr` の道、説明、設定の窓の `W_VARIABLE` は全部死んでいます。`ws->vrr_min_mhz` はログにしか使いません。下限の維持の処理もありません。 | 証明 |
| F2 | 重い | 途中で切れた道 | `display.c:94,863-889` | `FLIP_TIMEOUT_MS` は次の `show` でしか見ません。合成器は shown を待つので次の flip を頼みません。notifier が来なければ timeout に届かず、ポインターごと黙って固まります。 | 証明 |
| F3 | 中 | 文書と食い違う | `abi.h:559-563`、ABI の文書、`display.c:1070` | 「ほかの帰線はフリップを待つ間に知らせる」と書いて、毎回知らせます（H5）。 | 証明 |
| F4 | 中 | コメントと食い違う | `display.c:34-35,1288-1289` | "the desktop is drawn at the new size by the time the head shows it" は偽りです（B2）。 | 証明 |
| F5 | 軽い | 誰も読まない値 | `fb.c:245,566-567`、`fb.h:168` | `vblank_count`、`vblank_ns` は `screen_times` で渡され、`wm.c:1481,1729` は読みません。`kernel`、`user`、`ports`、`common` の grep で確かめました。 | 証明 |
| F6 | 致命 | VM だけの道 | C1 | 複数画面の合成は実機で届きません。 | 証明 |
| F7 | 軽い | 依頼の前提と違う | `ports/mesa/nocturne_os.c:1-45` | swapchain ではありません。ファイルの中身は注のとおりで、偽物ではありません。 | 証明 |
| F8 | 中 | コメントと食い違う | `fb.c:402-417,502`、`ports/nvidia/os_pci.c:572-587` | hold は boot の出力だけを止めます。`fb.c:506-526` の nvrm の緩衝への書き込みは止まりません。RM が電源を切り替える間も BAR1 へ書く可能性があります。ABI の文書は「全部の書込みを止め」と書いています。 | 可能性あり |
| F9 | 軽い | 小さな誤り | `wm.c:1459,1464-1465,2156-2157` | `:1459` の `tiles_rects` の失敗を無視して `stale` を消します。`SCREEN_BUFFERS_MAX` 回の無駄な `tiles_or` があります。`handle_mouse` に空の `else` があります。 | 証明 |
| F10 | 重い | 計算したのに誰も読まない値 | `wm.c:354,2427,3124,3229`、`user/apps/display.c:267`、`user/apps/settings.c:190,243` | 倍率 `scale_percent` は保存され、配られます。読むのは設定の窓だけです。`ui.c` もブラウザーもシェルも読みません（`kernel`、`user`、`common`、`ports` の grep）。カーネルが描くタイトルも `TITLE_H 28` のままです。設定は効きません。 | 証明 |
| F11 | 軽い | 層の確かめ | `wm.c:350-374,813,1379,2477-2505` | wm.c に遠隔の viewer の分岐があります。hypervisor の判定ではなく viewer の有無です。VM の境界の違反とは言いません。 | 証明 |
| F12 | 中 | 空の口 | `user/libc/web/js_pointer_events.js:42`、`webidl_data.inc:1812` | `getCoalescedEvents` はいつも `[]` です。`requestPointerLock` の実装は grep で見つかりません（H11）。 | 証明 |
| F13 | 中 | VM だけの道 | `kernel/src/vm/virtio_gpu.c:1-7`、`user/apps/gpu_demo.c:21` | `dev/gpu.h` の提供者は VM の virtio-gpu だけです。実機では "CPU software 3D fallback" です。`gpu_demo` は実機で GPU を使いません。 | 証明 |
| F14 | 致命 | 成功を返して何もしない | C2 | headless の `vkQueuePresentKHR`。 | 証明 |
| F15 | 中 | 測らない物差し | `wm.c:1477,1699-1713` | `ws->late` は `frame()` の CPU の時間だけを数えます。blank を落としたことを数えません（鎖の遅れは入りません）。`prof` のときしか出ません。落ちたコマの数を製品も試験も持っていません。 | 証明 |

### 定数にしている所（K の表）

依頼者の線により、それ自体が所見です。

| ID | 重さ | 定数 | 場所 | 何を決めているか | 計算できる元 |
|---|---|---|---|---|---|
| K1 | 中 | `DESK_BUFFERS 2` | `display.c:97` | desktop の緩衝の数 | 周期と合成の時間の測り |
| K2 | 中 | `FLIP_TIMEOUT_MS 3000` | `display.c:94` | flip の失敗の判定 | blank の数（refresh から） |
| K3 | 軽い | `STATE_WAIT_MS 1000` | `display.c:96` | 止まる旗を見る間隔 | 待ちを起こす事象に換える |
| K4 | 軽い | `FLIPS_TAKEN 4`、`MODE_TRIES 4` | `display.c:98,100` | 一度に取る flip、試すモードの数 | 出力の数、モードの一覧 |
| K5 | 重い | output 0 | `display.c:1057,1111,1549` | 出力の数 | 装置の申告（C1） |
| K6 | 中 | `minPresentInterval 1`、`tearing false` | `display.c:697-698` | 提示の方針 | 窓と利用者の選択 |
| K7 | 中 | 8 bpc、`XRGB8888` | `display.c:240-243` | 色の深さ（別の担当の領域） | EDID と KAPI の申告 |
| K8 | 中 | `IRQ_POLL_MS 1`、`IRQ_WAKE_MS 50` | `ports/nvidia/nvrm.c:45-46` | 割り込みが無いときの見に行く間隔、待ちの上限 | 1 ms は 1000 Hz の一コマの全部です。refresh から計算します |
| K9 | 中 | `INQ 1024` | `wm.c:1818` | 入力の輪の大きさ、静的な配列 | 装置の報告の速さと周期 |
| K10 | 軽い | `MAILBOX_FIRST 64`、`MAILBOX_MAX 4096`、`RECEIVE_BATCH 16` | `wm.c:59,62,934` | 窓の事象の数 | 入力の速さと受け取りの周期 |
| K11 | 軽い | `clock_after(NS_PER_S)` | `wm.c:1451` | 空いた緩衝を待つ上限 | refresh |
| K12 | 中 | `w->now + 100`、`33` | `user/libc/ui.c:2840,2845` | canvas を待つ間の見直し、回る印の拍 | `EV_FRAME` と実際の完了 |
| K13 | 重い | `SCHED_REQUEST_DEFAULT_NS 3 ms` | `kernel/src/sys/sched.h:52` | fair 級の一回の長さ | 1000 Hz の 3 コマ分で、H9 に効きます |
| K14 | 中 | `DL_LEAST_SHIFT 6`、`DL_SHARE_SHIFT 4`、`DL_DECAY_SHIFT 5`、「2 倍」 | `sched.c:74-81,441-446` | 予約 | 周期あたりの使用の測り（H10） |
| K15 | 軽い | `SCHED_PERIOD_MIN_NS 125000` | `sched.h:57` | deadline の周期の下限（8 kHz） | 今の線には足ります。定数であることは変わりません |
| K16 | 軽い | `N_SCREEN_BUFFERS_MAX 4`、`N_WIN_SURFACES_MAX 8` | `abi.h:541,809` | ABI の枚数の上限 | 配列の大きさの都合 |
| K17 | 中 | `N_SCREEN_POINTER_SIZE 32`、`CURSOR_MIN_PITCH 256` | `abi.h:615`、`display.c:99` | ポインターの大きさ | 倍率と NVKMS の申告 |
| K18 | 軽い | `TITLE_H 28`、`BORDER 1`、`SHADOW 10`、`MIN_CW 120`、`MIN_CH 60` | `wm.c:51-57` | 飾りの寸法 | 倍率（F10） |
| K19 | 軽い | `LOOK_NS 30 ms`、`LOOK_BUSY_NS 10 ms` | `kernel/src/vm/rdp.c:1794-1795` | RDP が見に来る間隔（VM だけ） | damage の知らせ |
| K20 | 中 | 16 ms、`+ 0.5` の丸め | `user/libc/web/js.c:10511,9641,9648` | 知らせの前の rendering の周期、周期の整数化 | 表示の周期を ns のまま持つ（T6） |
| K21 | 中 | `APP_DEADLINE_SHARE`（一 CPU の半分） | `kernel/src/sys/thread.c:203` | アプリの deadline の分の上限 | CPU の数と使い方の測り |

本物の計算（定数でない）と確かめた値は次のとおりです。

- `period_ns`（`wm.c:1659`）はモードの refresh からです。
- `sched_deadline` の周期（`wm.c:1694-1695,2268-2270`）も同じです。
- xHCI の周期（`xhci.c:954`）は `bInterval` からです。

---

## 読んで本物と確かめた部分

- tile の damage と緩衝ごとの `stale`（`wm.c:525-547,1453-1466`）。同じ場所の damage を二重に書きません。
- RAM で合成してから書く順序（`wm.c:1449-1463`）。
- KAPI の flip と notifier の読み（`display.c:761-776,863-889`、`nvkms_kapi_rm.c`）。
- `EV_FRAME` の出し方（`wm.c:1393-1415`）。
- queue の面の `readers`（`wm.c:1317-1332,1362`）。
- `present_rect` の lock と、大きさの変更の後の扱い（`wm.c:2841-2851`）。
- 画面ごとのスレッドと、`wm_lock` の外の合成（`wm.c:1437-1449,1755`）。
- 緩衝を持つ画面で、表示中の面へ書かないこと（`fb.c:513-514`）。
- WC の写像と sfence（`display.c:509-517`、`fb.c:535`）。
- hold と drain（boot の framebuffer について、`fb.c:402-417`）。
- panic の描き込み。
- 深い面の dither は既定が切です（`kernel/src/sys/settings.c:29`、`user/apps/settings_table.h:31`）。既定の道で毎画素の dither は走りません。
- `user/apps/display.c:270-272`：配置が変わらなければ `n_screen_arrange` を呼びません。
- `struct mutex` の優先度の継承。
- `shell.c:301-329`：深さの変更で新しい窓を先に作る順序。
- xHCI から `input_put` までの入力の受け取り（H11）。
- `VBE` のレジスターの flip（VM の道として、`vbe.c:80-84`）。

## 読めなかった範囲

- 動かしていません。実測は一つもありません。依頼者の線の「間に合う」「間に合わない」は、C1、C2、F1 のほかは、構造と計算からの判断です。
- `rep movsb` の WC への速さ、`commit` の時間、鎖の実際の遅れは確かめていません。
- NVKMS の中の flip の時間と、GSP を通るかどうかは読み切っていません。
- `open-gpu-doc` は読んでいません。`open-gpu-kernel-modules-615.78.08` は引用した箇所だけです。
- notifier の書き込みと RG の trigger の前後は、ハードの時刻なので確かめられません（T4）。
- 部分だけ読んだファイル：`user/libc/ui.c`、`user/libc/gui.c`、`user/apps/browser.c`、`user/libc/media_draw.c`、`kernel/src/vm/rdp.c`、`kernel/src/vm/virtio_gpu.c`、`kernel/src/vm/hyperv.c`（grep だけ）、`kernel/src/gui/pointer.c`（絵の生成なので関数の一覧まで）。
- 全部読んだファイル：`kernel/src/gui/wm.c`、`ports/nvidia/display.c`、`kernel/src/dev/fb.c` の画面の部分。
- `kernel/src/dev/fbcon.c` は読みました。合成の問いに関わる所見はありません。
- モードの選択、bpc、HDR、EDID は別の担当です。ここでは K7 だけ触れました。
- 動画の解読の費用、GPU の汎用の層は別の範囲です。

## 修理の案

原因を消す形で書きます。

1. 複数の出力（C1）：`scr` を出力ごとの構造にし、output 0 の決め打ちを消します。ABI の文書の形へ実装を合わせます。
2. Vulkan の窓（C2）：Nocturne の WSI の platform を `wsi_common` に足します。surface は窓です。present は GPU の完了の後で窓の queue へ入れます。headless へ成功を返させる道を窓の道と混ぜません。
3. GPU の合成：合成を GPU で行う利用者空間のサービスへ移します。カーネルは画面の持ち主のままにします。これで `-mno-sse`、写しの回数、BAR の帯域の三つが消えます。カーネルの層を変える設計の決定なので、書く前にユーザーに聞きます。
4. 写しを減らす（H2）：rect の窓の kernel の写しをやめます。全 rect の窓を、面の交換で動く形へ揃えます。
5. 窓の大きさ（B1）：configure と ack にします。新しい面を保留し、アプリの最初の present で位置と大きさを一度に切り替えます。
6. モード（B2、B3）：最初に表示する緩衝へ描き終えてから commit します。`shown` を「無し」で渡し、全部の緩衝を埋めます。
7. 直接表示の終わり（B4）：窓の絵ができてから flip で戻します。overlay は直接表示のままハードウェアの層に載せる形を調べます。
8. flip の鎖（H9、T2）：`irq_thread`、NVKMS の worker、`flip_thread` を deadline 級にします。周期は出力の refresh から計算します。鎖を短くし、blank の処理を一つのスレッドに集めます。
9. 予約（H10）：周期あたりの使用で測ります。無駄な起床を job に数えないようにします。合成器は damage の面積から必要な時間を前もって見積もれます。
10. blank（H5、F3、F5）：flip を待つ間だけ報告します。使わない `vblank_*` の値を消すか、位相の決定に使います。
11. ポインター（H6）：desktop でもハードウェアのカーソルを使います。動いても flip を起こしません。
12. 変化の無い frame（H4）：flip しません。`EV_FRAME` は blank から出します。最小化の窓の queue は進めるだけにします。
13. 隠面（H3）：tile ごとに不透明の窓を見て、下を省きます。隠れた窓の damage を捨てます。
14. 入力（H11）：相対の動きと途中の点を ABI へ足します。ネイティブの形で、窓が求めたときだけ渡します。pointer lock を入れます。入力の輪は装置の速さから大きさを決めます。
15. flip の見張り（F2）：blank の数で動く watchdog にし、flip の待ちと分けます。
16. VRR（F1）：KAPI の VRR を要求して、EDID の範囲を申告し、下限の維持を決めます。それができないなら道を消します。
17. 倍率（F10）：`ui.c` とカーネルの飾りに倍率を通します。
18. 物差し（F15）：blank を落とした数を画面ごとに数えて、製品で読めるようにします。依頼者の線はこの数で試します。
19. 1 ms の線：`commit` の時間、鎖の各段の時間、`wm_mutex` と `copy->lock` の持ち時間を最初に測ります。測りが無いまま緩衝の数や予約を決めません。緩衝の数と予約は、測りと refresh から計算します。
20. プログラムの時計（T6、T7）：`rendering_due` と `w->now` を ns で持ち、周期を丸めません。16 と 33 の固定の値を、表示の周期からの計算に換えます。
21. `time_ns`（T8）：ABI の文を実際に合わせるか、値を文に合わせます。先の時刻を読む側の扱いを決めます。
