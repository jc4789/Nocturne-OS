# 音の道の監査（読むだけ）

## 依頼文（全文）

Nocturne OS（`D:\Programes\llm vibe slop os`）の、音の道を、読むだけで敵対的に監査してください。返事と報告は全部日本語で書きます。識別子とパスは元の表記のままです。

### 依頼者の言葉（原文）
"Also our audio is messed up(yt will keep playing an ads audio after it ended and the main video started playing). use a couple subagents to do a read only adversarial audit. have them assume that our code base is a mess and that "fake" implmentations of things are rampent."

あなたの担当は音の全部です。ブラウザーの媒体要素と MSE から、デコード、流れ、サービス `audio`、カーネル、ハードウェアまでです。画面の担当は別にいます。

### 読むだけ
- コード、設定、媒体、git の状態を変えません。ビルド、QEMU、Hyper-V、スクリプトの実行をしません。OS は動かせないので、証拠はソースと仕様だけです。
- 書いてよいファイルは報告の `docs/audit-audio-20261102.md` の一つだけです。頭にこの依頼文を全文そのまま写し、続けて報告の全文を書きます。

### 監査の構え
- コードは散らかっていて、偽の実装がはびこっていると仮定します。名前、コメント、文書、コミットメッセージ、試験の合格を信じません。`stop` や `flush` や `abort` という名前の関数があっても、すでに下へ送った音が実際に止まるかを、輪の中身まで辿って確かめます。表から呼ばれる関数と `SYS_*` の番号での呼出しは codegraph に出ないので、`rg` でも探します。
- 確かめる偽物の形：成功を返して何もしない、決め打ちの値、計算したのに誰も読まない値、呼ぶ者のいない道、途中で切れた道、測らずに置いた固定の値や待ち、仕様の手順の一部だけの実装、取り消しのない非同期の処理、古い世代の結果を新しい文書や流れへ渡す所、コメントと食い違う動き、VM の道だけがある所。
- 証拠の無い所見は出しません。各所見に `file:line` を付けます。数を稼ぎません。読んで本物だと確かめた部分は、本物と書きます。確かめられないことは、確かめられないと書きます。

### 守る線（監査の物差し）
先に `AGENTS.md` と `.claude/rules/map.md` を読みます。`.claude/rules/browser.md` と `.claude/rules/kernel.md` も読みます。要点は次のとおりです。
- 層は分かれています。カーネルは音の出口の一覧と一つの open と HDA を持ち、混合と選択はサービス `audio` の持ち物です。VM の処理は `kernel/src/vm/vm.h` の後ろだけです。
- Web の標準を満たすのが規則で、照らすのは仕様の原文と WPT（`D:\Programes\cloned repos\wpt`）です。サイト専用の処理は認めません。主語はブラウザーの欠陥で、YouTube は再現の例です。
- 遷移や破棄では古い結果を新しい文書へ渡さず、要求、タイマー、子処理を取り消すのが規則です。
- 対象は実機です。VM で通ることは実機で動く証拠になりません。文書は古いことがあり、事実は今のコードで確かめます。`docs/media-*` と `docs/browser-*media*` は古い報告です。

### 読む場所
`user/libc/web/avmedia.c`、`user/libc/web/js_*` の媒体要素と MSE の結びつき（`rg` で探す）、`user/libc/media.c`・`media_mse.c`・`media_mse_parser.c`・`media_mse_worker.c`・`media_worker.c`・`media_adaptive*.c`・`media_rate.c`・`media_http.c`、`user/apps/browsermediaworker.c`・`browsermseworker.c`、`user/libc/web/webaudio.c`・`webaudio_dsp.c`・`js_webaudio.c`、`user/libc/audio.c`、`user/apps/audio.c`、`common/audio_abi.h`、`docs/nocturne-native-abi.md` の §17、`kernel/src/dev/audio.c`・`hda.c`・`hda_codec.c`、`kernel/src/vm/` の音（ac97、`rdp.c`）、設定の音の頁。ほかに要る所は辿ってください。

### 答えてほしいこと
1. 症状「YouTube で広告が終わり本編の動画が始まっても、広告の音が鳴り続ける」：広告から本編へ移るときに頁が取りうる操作（`src` の変更、`load()`、`MediaSource` の取り外しと付け直し、別の `<video>` への切り替え、`SourceBuffer.abort`・`remove`・`timestampOffset`・`appendWindow`、`pause`、要素の除去や GC）を全部挙げ、それぞれについて、古い音の流れが止まるかを呼出しの順で辿ります。止まらない道、古い音の数え方の残り、子プロセスが生き残る道、流れの所有者が居なくなっても鳴り続ける道を示します。コードで証明できたものと、可能性だけのものを分けます。HTML の media element load algorithm と Media Source Extensions の手順に照らします。
2. 音の時計と A/V 同期：動画の提示が何の時計に従うか、古い音の位置が新しい流れへ混ざらないか、seek、rate、停止、underrun の扱い。
3. 流れの寿命：作成、停止、flush、破棄、プロセスの終了、頁の遷移、窓を閉じるとき。輪に残った音はどうなるか。下へ送った後に取り消せるか、遅延はどれだけか。
4. サービス `audio`：混合、変換（標本化率、チャンネル、形式）、音量、消音、出口の切り替えと、失敗の扱い。本物か、固定の前提か。
5. カーネルと HDA：輪と位置の報告、codec の経路、形式の選択、割り込み、underrun。実機の道と VM の道の違い。
6. 偽物の一覧：上の形に当たるものを全部。

### 報告の形
各所見に、重さ（致命、重い、中、軽い）、種類、`file:line`、呼出しの順の根拠、起きる条件、確信（コードで証明、可能性あり）を書きます。最後に、読んで本物と確かめた部分と、読めなかった範囲を書きます。修理の案は、原因を消す形で短く書きます。

最後の返事は、報告のパスと、重い順の要点を 15 行以内で返します。

---

# 報告

## 結論

- 音の土台は本物です。`flush` はサービスの混合と装置の輪まで届いて音を取り戻します。
- 同じ要素での `src`、`load()`、`pause()`、seek、`remove`、`abort`、`removeSourceBuffer` は、どれも `close_audio` に届いて止まります。
- 欠陥は上の層にあります。媒体要素と MSE の手順が欠けていて、音を止める呼出しが来ない道があります。
- 古い音が止まらないとコードで証明できた道は三つです。
  - 再生中の要素を DOM から除く（A1）。
  - `srcObject` を代入する（A2）。
  - 同じ `MediaSource` に音声の `SourceBuffer` を足して、古い方を除かない（A3）。
- 音を増やす道が一つあります。`<video muted>` が音を出します（A4）。
- 同じ `MediaSource` で広告から本編へ移る道は別の形で壊れます。`changeType` と track ID の検査が再生を誤りで止めます（A5）。
- YouTube が実際にどの操作をするかは確かめていません。どの道が今回の症状の原因かは、コードだけでは決まりません。

### 症状の候補と、見分け方

| 順 | 候補 | 症状との合い方 | 見分け方 |
|---|---|---|---|
| 1 | A1：広告の要素を除いて、`pause()` を呼ばない | 広告の音が終わりまで続く | `web_avmedia_tick` は要素ごとに `timing_profile` を呼ぶ（`avmedia.c:1406`）。本編の間に `Native media timing:` が二つの要素の分だけ出れば合う見込みです。 |
| 2 | A3：音声の `SourceBuffer` が二つある | 広告と本編の音が交互に混ざる | F12 で `document.querySelector('video').audioTracks.length` が 2 以上なら合います。 |
| 3 | A4：本編の前に別の要素を `muted` で読み込む | 消音のはずの音が出る | 同じ行の数と、要素の `muted` 属性で分かります。 |
| 4 | A2：`srcObject` で切り替える | 代入が何も起こさない | F12 で `'srcObject' in HTMLMediaElement.prototype` を読むと false です。これは候補の否定にならないので、頁の操作を別に見る必要があります。 |

この表の「見分け方」は、まだ実行していません。

## 証拠の範囲

- 証拠はソースの読みだけです。OS は動かしていません。音を聞いていません。
- 仕様は `https://www.w3.org/TR/media-source-2/` と `https://html.spec.whatwg.org/multipage/media.html` を WebFetch で読みました。WebFetch は小さなモデルの要約を返す道具で、原文の一行ずつの照合ではありません。
- 要約に出ず、記憶で照らした項目には「記憶」と書きます。
- HDA の仕様書の PDF（`D:\Programes\OS DEV INFO\high-definition-audio-specification.pdf`）は開いていません。HDA の判定はコードの読みです。
- WPT は実行していません。名前と中身の一部を読みました。

## 問い 1：広告から本編へ移る操作ごとの辿り

止まるまでの時間の「HDA の遅延」は、`hda_status` の `guard_frames`（FIFO の内容 + 1 frame + codec の遅延）と、次の塊の境までです（`hda.c:522`、`hda.c:644-660`、`apps/audio.c:437`、`apps/audio.c:931-964`）。塊は 5 ms の望みです。FIFO の実際の大きさは測っていません。

| 頁の操作 | 呼出しの順 | 古い音 | 止まるまで | 標準との差 |
|---|---|---|---|---|
| `video.src = x` | `js_avmedia.js:205` → `reset` :125 → `mseBridge.detach` → `native('reset')` → `avmedia.c:958` → `unload` :269 → `close_audio` :262 | 止まる | 同期。HDA の遅延 | `abort` と `emptied` が無い（A6） |
| `setAttribute('src', x)` | 属性の変更の口は無い。`monitor` の 40 ms の輪（`js_avmedia.js:96`）で `load` | 遅れて止まる | 40 ms 以上。JS が混めば延びる | 仕様は即座に load。再生中なら勝手に `play` する（A6） |
| 停止中の要素へ `setAttribute('src', x)` | `active` から外れている（`js_avmedia.js:120`）ので誰も気づかない | 鳴っていない | 該当なし | load が走らない（A6） |
| `removeAttribute('src')` だけ | `monitor` が気づいて `load`、空なので `emptied` | 止まる | 40 ms 以上 | 仕様は何もしない。害は小さい |
| `video.load()` | `js_avmedia.js:258` → `load` :133 → `reset` | 止まる | 同期。HDA の遅延 | `abort`、`emptied` が無い（A6） |
| `MediaSource` の取り外しと付け直し | `reset` → `js_mse.js:216` の `detach`。新しい方は `attach` :210 → `mseCreate`（`avmedia.c:781`）→ `unload` | 止まる | 同期 | 本物 |
| `URL.revokeObjectURL` | 媒体に触れない | 鳴り続ける | 止まらない | 仕様どおり |
| `srcObject = x` | プロパティが無い。ただの JS の値になる | **止まらない** | 止まらない | **load が走らない（A2）** |
| 別の `<video>` へ。古い方に `pause()` か `load()` がある | 上の道 | 止まる | 同期 | 本物 |
| 別の `<video>` へ。古い方を放置 | 何も呼ばれない | **鳴り続ける** | 頁の遷移まで | 仕様どおり。頁の誤りの扱い |
| 古い要素を DOM から除く | `dom.c:1601` は枠だけを切り離す。媒体への口が無い | **止まらない** | 頁の遷移まで | **仕様は内部の一時停止（A1）** |
| 親や枠の除去 | 枠なら `web_frame_retire` → `web_avmedia_free`（`doc.c:1765`）。枠でない親なら上と同じ | 枠は止まる。枠でない親は止まらない | - | 枠は本物。親は A1 |
| 参照を捨てる（GC） | finalizer の呼出しが無い。`s` は `streams` の鎖と `active` が持つ | 鳴り続ける | 頁の遷移まで | 仕様は音が終わるまで GC を許さない。音は正しい。止まった要素の子プロセスが残るのは A14 |
| `SourceBuffer.abort()` | `js_mse.js:157` → `mseAbort`（`avmedia.c:838`）→ 子の `NMSW_ABORT`（`browsermseworker.c:194`）→ `nmedia_mse_abort`（`media_mse.c:487`）→ `nmedia_mse_seek`、`timeline++` → `pump_impl` の `moved`（`avmedia.c:1112-1114`）→ `close_audio`、`audio_open` | 止まる。今の位置から鳴り直す | 子の応答まで。HDA の遅延 | 仕様は再生に触れない（A9） |
| `remove(a, b)`。今より先を含む | `media_mse.c:472` で `changed` → `nmedia_mse_seek` → 同じ `moved` | 止まる | 子の応答まで | 位置が消えた範囲なら仕様は `HAVE_METADATA` で止める。ここは下の frame があれば進む |
| `remove(a, b)`。全部が今より前 | `changed` が立たない | 動かない | - | 正しい。次の行の前提 |
| 全部を `remove` した後、前の時刻へ append | `initial`（`media_mse.c:365`）。seek も `timeline` も動かない | 流れが続く | 流れが続く | **A7** |
| `timestampOffset`、`appendWindow*` | 次の append にだけ効く。重なれば `replaced` → `nmedia_mse_seek`（`media_mse.c:365`）→ `moved` | 重なれば止まる。重ならなければ今の音が続く | 子の応答まで | 仕様の splice は無いが、切れ目の扱いは本物 |
| `changeType` | 違う型で frame が残れば誤り（`media_mse.c:497`） → `operation` の外で `NotSupportedError`（`js_mse.js:166`） | 動かない | - | **A5** |
| 同じ型の `changeType` | `nmedia_mse_abort`（`media_mse.c:496`） | 止まる。鳴り直す | 子の応答まで | 仕様は再生に触れない（A9） |
| 新しい init segment | `configuration_matches`（`media_mse.c:269`）。track ID か codec_tag が違えば誤り → `avmedia.c:1101` で `close_audio` | 止まる。要素は誤りになる | 子の応答まで | **A5** |
| 同じ世代の frame を続けて append | 切れ目で `replace_decoder`。`revision` だけが動く（`avmedia.c:1111-1113`） | 続く | - | 本物。継ぎ目なしの道 |
| `removeSourceBuffer` | `js_mse.js:185` → `mseDrop` → `command` が epoch を増やす（`media_mse_worker.c:95`）→ `timeline` が動く（:350）→ `moved` | 止まる | 次の pump。古い PCM が 1 束混ざりうる（軽い所見） | 本物 |
| 音声の `SourceBuffer` を足し、古い方を残す | 種類で一つの `enabled[0]`（`browsermseworker.c:221`）。両方が同じ流れへ pts 順に並ぶ（:247） | **両方鳴る** | 止まらない | **A3** |
| `video.pause()` | `js_avmedia.js:301` → `avmedia.c:1024` → `mse_pause` :484 → `close_audio` | 止まる | 同期 | 本物。`pause` の事象も本物 |
| 要素を終わりまで再生 | `monitor` :103-113 | 尽きる | - | `pause` の事象が無い（A6） |
| 頁の遷移 | `web_avmedia_free`（`doc.c:1765`、`avmedia.c:1645`）→ `unload` | 止まる | 同期 | 本物 |

### 分け方

- **コードで証明（止まらない）**：A1、A2、A3。
- **コードで証明（音が増える）**：A4。
- **コードで証明（古い数が残る）**：A7 の床 `audio_played_ms`。
- **コードで証明（子が残る）**：A14。止まった要素の子プロセスと coded frame が、文書の終わりまで残ります。
- **所有者が居なくなっても鳴る道**：アプリのプロセスが落ちると、サービスが残りを鳴らして流れを消します（`apps/audio.c:1018-1039`）。残りは 170 ms 以下です。文書にも書いてあり、長くは続きません。
- **可能性だけ**：YouTube が上のどの操作をするか。これは未確認です。

## 問い 2：時計と A/V 同期

- 動画は `current()`（`avmedia.c:354-379`）に従います。
- `current()` は monotonic の時計に `base_ms` と `started` を足した値です。
- 音があるとき、`audio_clock_track`（`avmedia.c:316-353`）が装置の報告へ寄せます。
  - 50 ms 超の差が 500 ms 続くと合わせ直します。
  - それ以下は 2 秒のフィルターで、2 秒に 1 ms ずつ動かします。
  - 音が尽きると `clock_hold` で止まります。
- 音が無いときは monotonic だけで進みます。出口が無いときは、サービスが時計で数えた位置を返します（`N_AUDIO_CLOCK_ESTIMATED`）。
- 古い位置の混入：`close_audio` が `audio_written`、`audio_played_valid`、`clock_*` を全部消します（`avmedia.c:262`）。次の流れの基準は `audio_fill_gap` が `base_ms` から作り直します（:404-407）。**これは本物です。** 例外は A7 の、`close_audio` を通らない道です。
- seek：`close_audio`、`clear_ahead`、`pending` の消去のあと、子の応答まで `mse_seeking` で待ちます（:462、:1121）。応答で `output_ready` が無効になります（`media_mse_worker.c:144`）。**本物です。**
- rate：`nmedia_rate`（FFmpeg の atempo と swr）を通します。`close_audio` が `rate_decoder` も閉じるので、古い PCM は残りません（:262）。**本物です。**
- 停止：`mse_pause` が位置を残して流れを閉じます。再開は `mse_prepare_play` で位置から鳴らし直します。**本物です。**
- underrun：`NMEDIA_AGAIN` で輪も空なら `starved` になり、時計が止まります（:1148）。**本物です。** 隙間だけは別の扱いで、A8 です。
- 注意：`audio_clock_track` は `current()` の中で動きます。読むだけの口が状態を変えます。誤りの証拠は見つけていません。

## 問い 3：流れの寿命

| 段 | 動き | 輪の音 | 根拠 |
|---|---|---|---|
| 作成 | `n_service_connect` と往復で待つ。答えの添付が共有の輪 | - | `libc/audio.c:33-49`、`apps/audio.c:969-1014` |
| 停止（`pause`） | flush してから閉じる | 捨てる | `avmedia.c:484-489`、`:262` |
| flush | 装置がまだ読まない塊まで全部の流れを戻し、他の流れだけで混ぜ直す | 捨てる | `apps/audio.c:931-964` |
| close | flush の後は `played` が `head` に並ぶので待たない。flush が失敗すると残り + 1 秒まで主スレッドが待つ | 鳴らしてから消える | `libc/audio.c:189-205`、A12 |
| プロセスの終了 | 接続が切れ、サービスが残りを鳴らして流れを消す | 170 ms 以下を鳴らす | `apps/audio.c:1018-1039` |
| 頁の遷移 | `web_avmedia_free` → `unload` | 捨てる | `avmedia.c:1645-1650` |
| 窓を閉じる | プロセスの終了と同じ道と読みました。窓を閉じる段取りは読み切っていません | 170 ms 以下の見込み | `apps/audio.c:1018-1039` |
| Web Audio の閉じる | flush してから閉じる | 捨てる | `webaudio.c:1129` |
| Web Audio の suspend | 流れを開けたまま待つ。flush しない | 170 ms 以下を鳴らす | `webaudio.c:1077-1080` |

取り消しの遅延は出口で違います。

- HDA：FIFO の内容 + 1 frame + codec の遅延と、次の塊の境まで（`hda.c:522`、`hda.c:654`、`apps/audio.c:437`）。10 ms 前後の見込みで、測っていません。
- AC'97：`remaining/2 + buf_frames`（`kernel/src/vm/ac97.c`）。VM だけの道です。
- RDP：送った音は取り戻せません（`kernel/src/vm/rdp.c`、`guard_frames = snd_taken - snd_played`）。`docs/nocturne-native-abi.md` §17 にも書いてあります。VM と遠隔の道だけです。

文書（§17）とコードは、上の範囲で合っています。

## 問い 4：サービス `audio`

- 混合：本物です。`stream_mix`（`apps/audio.c:321-387`）、`block_mix`（:424-435）。
- 変換：本物です。流れごとの率、形式、channel を `stream_decode`（:273-311）と `matrix_build`（:198-219）で扱います。
  - 固定の 48 kHz ではありません。形式は申告から選びます（`format_for` :810-829）。
  - ただしブラウザーは手前で 48 kHz S16 の 2ch へ落とします（A15）。サービスの変換はブラウザーの音に使われていません。
- 音量と消音：本物です。設定の値を混合で掛けます。要素ごとの音量は別の持ち方です（軽い所見）。
- 選択：本物です。`device_rank`（:759-766）と `output_choose`（:832-847）。§17 の順と合っています。
- 出口の切り替え：`output_close` と開き直しです。混ぜてあった音は聞かせたものと数えます。切り替えで、装置の輪へ先に混ぜた分（`AHEAD_HIGH` の 16 塊、望みの塊で 80 ms 以下）の音が失われます。誤りとは言えません。
- 失敗：出口を開けないと時計で数え（`layout_null` :656-664）、再試行は一覧の変化を待ちます。流れは時計の上で進み、音は失われます。
- 設定の音の頁（`user/apps/settings.c:801-891`、`:1383-1398`）：一覧、選択、率、大きさ、channel を実際に読み書きします。**本物です。** 値は申告の表から出します。
- 欠点：`matrix_build` の記憶切れは無音になり、誤りを返しません（:204）。
- 欠点：サービスが遅れると古い音が鳴ります（A13）。

## 問い 5：カーネルと HDA

- 層：カーネルは出口の一覧と一つの open と輪だけを持ちます（`kernel/src/dev/audio.c:45-54`、`:188-267`）。混合はありません。**本物です。**
- VM の分離：共通コードは `vm_audio_init` の一呼出しだけです（`audio.c:274-278`）。**本物です。**
- 輪：BDL の塊で回ります。`hda_open` が答えの形を返します（`hda.c:570-614`）。
- 位置：LPIB を読み（`hda.c:534-548`）、周回の取りこぼしは経過時間で補います。位置の緩衝（DMA position buffer）は使いません。LPIB の精度は実機で確かめられません。
- 守り：`guard` は FIFOS の内容 + 1（`hda.c:522`）。codec の遅延を足して報告します（:653-654）。
- codec の経路：`find_path`（`hda_codec.c:229-278`）、ELD、DIP、電源の復帰を実際に扱います。
- 形式：率と大きさは converter の申告から出します（`hda_codec.c:212-220`）。**channel は 2 までに決め打ちです（`hda_codec.c:352`）。** 5.1 の出口は申告されません。
- 割り込み：塊の終わりで handle が読めるようになります（`hda.c:427-442`）。
- underrun：`STS_FIFOE` と `STS_DESE` を数えます（`hda.c:434`）。サービスは `out.errors` に写すだけで、誰も読みません（`apps/audio.c:872`）。
- 実機と VM の差：実機は HDA だけです。AC'97 は 48 kHz 16 bit だけで、VM の後ろにあります。RDP は `REMOTE` で、`device_rank` が一番に選びます（`apps/audio.c:759-766`）。
- 実機では一度も確かめていません。

## 所見（重い順）

### A1 再生中の要素を DOM から除いても止まらない
- 重さ：重い。種類：仕様の手順が無い。確信：コードで証明。
- `file:line`：`user/libc/web/dom.c:1560-1561`、`dom.c:1601`、`user/libc/web/frame.c:77-92`、`user/libc/web/avmedia.h`（口が無い）、`avmedia.c:1403-1407`、`avmedia.c:1438-1439`、`avmedia.c:1347`。
- 根拠：
  1. `doc_node_remove` は `web_frames_detach_tree` を呼びます。これは枠と押下中の要素だけを扱います。
  2. `avmedia.h` に除去の口はありません。
  3. `web_avmedia_tick` と `web_avmedia_service` は `owner` だけを見ます。除いても `owner` は変わりません。
  4. `doc_node_connected` の使い手は絵の写し（`snapshot_publish`）だけです。
  5. `js_avmedia.js` に `isConnected` の読みはありません（`rg` で 0 件）。
- 標準：要素が文書から除かれると、安定状態を待ち、まだ文書の外なら内部の一時停止を走らせます。
- WPT：`html/semantics/embedded-content/media-elements/playing-the-media-resource/pause-remove-from-document.html` は `v.paused` が true になることを試します。今のコードでは通らない読みです。実行はしていません。
- 起きる条件：再生中の要素を除き、`pause()` を呼ばない頁。
- 修理の案：`dom.c` の除去の口から、媒体の子孫を持ち主へ知らせます。安定状態で接続を確かめ、外なら `pause` の手順を事象ごと走らせます。`doc_node_move_preserving` と文書内の移動は止めません。

### A2 `srcObject` が無い
- 重さ：重い。種類：欠け。確信：コードで証明。
- `file:line`：`user/libc/web/js_avmedia.js`（`rg` で 0 件）、`user/libc/web/js_mse.js`（同じ）。
- 根拠：`rg` で `srcObject` は `user/libc/web/*.js` と `*.c`（`webidl_data` を除く）に無いです。`webidl_data.inc:3051` には名前がありますが、`js_webidl.c:21` のとおり、bindings に無い名前は無いままです。代入はただの JS の値になり、load が走りません。古い音は続き、新しい媒体は始まりません。
- 標準：代入で load が走ります。
- WPT：`html/semantics/embedded-content/media-elements/src_object_blob.html`。
- 同じ形の欠け：`createMediaElementSource`、`captureStream`、`setSinkId`、`fastSeek` も `rg` で 0 件です。
- 修理の案：load の手順に、割り当てられた object を第一の候補として入れます。`MediaSource` と `Blob` は既存の道へ通します。

### A3 音声の選択が種類で一つで、二つの `SourceBuffer` の音が一つの流れへ並ぶ
- 重さ：重い。種類：途中で切れた道、決め打ち。確信：コードで証明。
- `file:line`：`js_mse.js:67`、`js_mse.js:79`、`avmedia.c:752`、`avmedia.c:775-778`、`media_mse_worker.c:87`、`browsermseworker.c:198`、`browsermseworker.c:221`、`browsermseworker.c:243-247`、`avmedia.c:1238-1267`。
- 根拠：
  1. `AudioTrack.enabled` の setter は `mseSelect` を種類ごとに呼びます。track の識別がありません。
  2. 子の `enabled[0]` は音声全部の一つの旗です。
  3. 全部の音声の枠が `needed` になり、pts の小さい方が選ばれます。
  4. 親は選ばれた PCM を一つの `n_audio_stream` へ順に書きます。
  5. 混合はありません。二つの音が交互に並びます。
  6. 輪の消費は実時間なので、音は動画から遅れていく見込みです。遅れの大きさは確かめていません。
- 標準：複数の有効な音声 track は同期して混ぜます（記憶）。MSE の init segment の手順では、すでに有効な音声 track があれば、新しい track は既定で無効です（記憶）。
- 起きる条件：同じ `MediaSource` に音声の `SourceBuffer` を足し、古い方を除かない頁。
- 修理の案：`enabled` を track（枠）ごとの旗にします。新しい track の既定を標準どおりにします。複数の有効な track は枠ごとの `n_audio_stream` にして、混合はサービスへ任せます。

### A4 `muted` 属性を読まない。`autoplay` が動かない
- 重さ：重い。種類：反映だけで働きが無い。確信：コードで証明。
- `file:line`：`js_avmedia.js:19`、`js_avmedia.js:243-246`、`js_avmedia.js:303`。
- 根拠：
  1. 状態の初期値は `muted:false` です。
  2. `muted` の getter は状態だけを返します。
  3. `defaultMuted` は属性の反映だけで、状態へ入りません。
  4. `native('volume', ...)` に属性の値は渡りません。
  5. `autoplay` は反映だけです。`readyState` の変化から `play` する道がありません。
- 標準：muted の状態が既定のままで属性があれば、音は消えています。`HAVE_ENOUGH_DATA` で autoplay の適格な要素は再生を始めます。
- 起きる条件：`<video muted>` を再生する頁。`<video autoplay>` の頁は始まりません。
- WPT：`audio_muted_present-manual.html`（手で試す）。`autoplay-*`、`event_*`（`autoplay` を使う試験）。
- 修理の案：muted の状態に「既定」を持ち、属性を読みます。autoplay の flag を load の手順と `readyState` の変化へ入れます。

### A5 `changeType` と track ID の検査が、同じ `MediaSource` での切り替えを止める
- 重さ：重い。種類：仕様の手順の一部だけ。確信：コードで証明。
- `file:line`：`media_mse.c:497`、`media_mse.c:207`、`media_mse.c:269`、`js_mse.js:165-167`、`avmedia.c:1101`。
- 根拠：
  1. 違う型の `changeType` は、frame が残っていると `remove retained coded frames before changing MSE codec` を返します。
  2. `js_mse.js` はこれを `NotSupportedError` にします。
  3. 後の init segment は `s->id`、`codec_id`、`codec_tag` の違いで誤りになります。誤りは `avmedia.c:1101` で要素の再生を止めます。
- 標準：`changeType` の手順に frame を消す段はありません。track ID の一致は、一つの種類に複数の track があるときだけ求められます。codec は `changeType` か `addSourceBuffer` の型に基づいて断ってよいとされます。`s->id` の検査は標準に無いです。
- 既存の部品：`coded_frame` は自分の `configuration` を持ち、`replace_decoder` は切れ目で decoder を替えます（`media_mse.c:238-245`）。frame を残す土台はあります。
- WPT：`media-source/mediasource-changetype-play.html`。`mediasource-changetype-util.js` の `trimBuffered` は後ろだけを消すので、frame は残ったままです。今のコードでは通らない読みです。
- 起きる条件：同じ `MediaSource` で、広告と本編の codec か track ID が違う頁。症状は音が残ることではなく、再生の誤りです。
- 修理の案：`media_mse.c:497` の拒否を消します。`nmedia_mse_change_type` は parser の状態だけを戻します。`configuration_matches` から `s->id` の検査を外します。

### A6 load の手順の欠けと、属性の見張りの輪
- 重さ：中。種類：仕様の手順の一部だけ、測らずに置いた固定の待ち。確信：コードで証明。
- `file:line`：`js_avmedia.js:10`、`js_avmedia.js:88-124`、`js_avmedia.js:96`、`js_avmedia.js:103-113`、`js_avmedia.js:120`、`js_avmedia.js:125-136`。
- 根拠：
  - `mediaEvents` に `abort`、`stalled`、`progress` はあります。出す所が無いです。
  - `emptied` は URL が空の時だけ出ます（:136）。
  - 終わりの手順は `timeupdate` と `ended` だけです。`paused` は `s->playing` から true になります（`avmedia.c:663`、`:1293`）。`pause` の事象がありません。
  - ネイティブが自分で止めた時（誤りや失敗）も、`paused` だけが変わり、`pause` の事象がありません。
  - `setAttribute('src')` は 40 ms の `setTimeout` の輪でだけ気づきます。JS が混むと延びます。その間、古い音は `web_avmedia_service` で供給され続けます。
  - 停止中の要素は輪から外れるので、`setAttribute('src')` に気づきません。
  - 気づいた時、再生中なら自動で `play` します。標準は `paused` を true にします。
- 標準：load の手順は `abort`、`emptied` を queue し、`paused` を true にして、待ちの `play` を拒否します。終わりは `timeupdate`、`pause`、`ended` の順です。
- WPT：`html/semantics/embedded-content/media-elements/event_*`、`error-sequence.html`。
- 修理の案：属性の変更の口を持ち主（DOM の属性の変更）から呼びます。輪を消します。load と終わりの手順を標準の順で書き直します。

### A7 全部を `remove` した後の append が、位置を無視して鳴り、古い床が残る
- 重さ：中。種類：古い数の残り。確信：コードで証明。条件つき。
- `file:line`：`media_mse.c:472`、`media_mse.c:355`、`media_mse.c:365`、`avmedia.c:372-376`、`avmedia.c:1112-1114`。
- 根拠：
  1. 位置が全部の frame より後なら、`remove` の `changed` は立ちません（`media_mse.c:472`）。
  2. 次の append は `initial` なので `m->cursor=m->frames` だけで、seek も `timeline++` もありません（:365）。
  3. decoder と流れは続き、前の時刻の PCM がそのまま輪へ入ります。
  4. `current()` の床 `audio_played_ms` は `audio_played_valid` が消えるまで残り、`currentTime` が前の位置に留まります（`avmedia.c:372-376`）。
  5. その間、前の時刻の絵は全部期限内として出ます。
- 標準：位置は動かず、その位置に frame が無いので `HAVE_METADATA` で止まります。頁が seek して初めて進みます。
- 起きる条件：広告を最後まで再生し、全部を `remove` し、seek せずに小さい時刻へ append する頁。
- 修理の案：カーソルを dts の線形から、位置に基づく選びへ変えます。A8 と同じ根です。

### A8 MSE の隙間で時計が止まらず、無音で埋めて進む
- 重さ：中。種類：仕様の手順の一部だけ。確信：コードで証明。
- `file:line`：`avmedia.c:402-422`、`avmedia.c:636-640`、`media_mse.c:113-171`。
- 根拠：
  1. `packet_read` のカーソルは dts の順に次の frame へ進み、隙間で止まりません。
  2. `audio_fill_gap` は次の PCM の pts まで無音を書きます。
  3. `ready_state` は隙間で 1 を言いますが、再生は止まりません。
- 標準：位置に媒体が無ければ `readyState` が `HAVE_METADATA` まで下がり、再生は待ちます。MSE の coded frame removal の手順に書いてあります。隙間そのものの扱いは HTML の手順で、記憶で照らしました。
- 修理の案：隙間に当たったら `starved` にして時計を止めます。無音の埋めは標準の小さな隙間（音声 1 frame 未満）だけにします。

### A9 `abort()` が再生を止める。取り消せない APPEND。誤りの出ない `abort`
- 重さ：中。種類：仕様と違う動き、取り消しのない非同期。確信：コードで証明。
- `file:line`：`media_mse.c:487`、`browsermseworker.c:194`、`avmedia.c:1112-1114`、`js_mse.js:157-160`、`media_mse.c:496`。
- 根拠：
  - `nmedia_mse_abort` は `nmedia_mse_seek` を呼びます。decoder と音の流れが flush され、`moved` で流れが開き直されます。
  - 標準の `abort` は parser の状態と append window を戻すだけで、再生に触れません。
  - 動いている `remove` の間の `abort` は `InvalidStateError` を投げるのが標準です。`js_mse.js:157-160` は `updating` を区別せず、投げません。
  - JS の `epoch` は結果を捨てるだけです。子で動いている APPEND は取り消されません。
- WPT：`media-source/SourceBuffer-abort-updating.html`、`SourceBuffer-abort-removed.html`、`SourceBuffer-abort-readyState.html`。
- 修理の案：`nmedia_mse_abort` を parser の状態だけにします。seek を消します。remove の判定を `js_mse.js` に入れます。

### A10 quota、duration、`sequenceOffset`
- 重さ：中。種類：決め打ち、仕様と違う動き。確信：コードで証明。
- `file:line`：`js_mse.js:107-133`、`avmedia.c:817`、`avmedia.c:788-797`、`js_mse.js:177`、`avmedia.c:758`、`js_mse.js:119`。
- 根拠：
  - 標準は `appendBuffer` の準備で evict し、足りなければ同期で `QuotaExceededError` を投げます。ここは非同期の `error` 事象です（:125-131）。頁は例外で次の手を打てません。
  - `mseQuota` は `INFINITY` の決め打ちです。
  - `ms.duration` を短くすると、`mseDuration` が暗黙に `NMSW_REMOVE` を送ります。標準では duration は buffered の終わりより短くなりません。
  - sequence mode の `timestampOffset` に `group_end` を入れます。標準は group start と frame の pts の差です（記憶）。fragment の pts が 0 でなければ値が違います。
- WPT：`media-source/mediasource-sequencemode-append-buffer.html`、`mediasource-duration.html`。
- 修理の案：evict を準備の段へ移します。上限は実際の持ち主の値にします。

### A11 `currentTime` と `buffered`、`seekable`
- 重さ：中。種類：仕様の手順の一部だけ。確信：コードで証明。
- `file:line`：`js_avmedia.js:222-238`、`avmedia.c:462`、`js_avmedia.js:211`、`js_avmedia.js:213`。
- 根拠：
  - `HAVE_NOTHING` で既定の開始位置へ入れる道がありません。`native('seek')` が false なら投げます。
  - 負の値で `TypeError` を投げます。標準は丸めます。
  - `seekable` への丸めがありません。
  - MSE で metadata の前の seek が `s->ready=1` にし、`loadedmetadata` が早く出ます。
  - `buffered` と `seekable` は MSE 以外で空です。
- 修理の案：setter を標準の手順で書き直します。Range の範囲を `buffered` と `seekable` へ出します。

### A12 主スレッドが音の往復で止まる
- 重さ：中。種類：止まらない同期処理。確信：コードで証明。起きるのはサービスが詰まった時です。
- `file:line`：`libc/audio.c:33-49`、`libc/audio.c:145-152`、`libc/audio.c:179-205`、`avmedia.c:262`、`avmedia.c:423-433`。
- 根拠：
  - `close_audio` と `audio_open` は `pause`、seek、`abort` のたびに主スレッドで走ります。
  - `n_audio_flush` は `N_CHANNEL_WAIT` で答えを待ちます。期限がありません。`peer_gone` は死を見るだけで、詰まりは見ません。
  - flush が失敗すると、`n_audio_close` は残り + 1 秒まで待ちます。
  - `n_audio_status` は `sequence & 1` の間回り続けます（:150-152）。サービスが書く途中で死ぬと抜けません。窓は小さいです。
- 測っていません。
- 修理の案：flush と open を非同期の要求にして、答えを pump で受けます。`n_audio_status` の輪に回数の上限と `peer_gone` を入れます。

### A13 サービスが遅れると、装置は古い音を鳴らす
- 重さ：中。種類：古い音の残り。確信：コードで証明。
- `file:line`：`apps/audio.c:596`、`apps/audio.c:700-725`。
- 根拠：
  - `fill` のコメントのとおり、過ぎた塊には古い音が残ります。
  - 装置の輪は終わりなく回るので、遅れている間は 1 周前（32 塊、160 ms 以上前）の音が繰り返されます。
  - 先の塊を無音で埋める道がありません。`output_start` も輪の中身を消しません。
  - `out.ahead` は増えるだけで、`output_start` まで戻りません（:607-608）。
- 影響：長く続く広告の音の説明にはなりません。かくつきと音の繰り返しの原因になります。
- 修理の案：まだ読まれない塊を無音で埋めます。`ahead` を戻す条件を作ります。

### A14 止まった要素の子プロセスと coded frame が、文書の終わりまで残る
- 重さ：中。種類：子処理の残り。確信：コードで証明。
- `file:line`：`avmedia.c:240-249`、`avmedia.c:269-294`、`avmedia.c:1645-1650`、`user/libc/web/js.c`（`web_avmedia_*` の呼出しに finalizer が無い）。
- 根拠：
  - `web_avmedia_free` は文書の終わりにだけ走ります。
  - ラッパーが GC されても `streams` の鎖の項目は残ります。
  - 要素ごとに `browsermseworker` の子と coded frame が残ります。
- 影響：同じ頁で広告の要素を作って捨てる頁では、子プロセスが増えていきます。
- 修理の案：ノードの finalizer か除去の口から、再生していない要素の資源を手放します。A1 と同じ持ち主です。

### A15 音声を手前で 48 kHz S16 へ線形補間で落とす
- 重さ：中。種類：固定の前提、変換の重複。確信：コードで証明。
- `file:line`：`user/libc/media.c:417-452`、特に `:429-443`、`user/include/nocturne.h:599-601`。
- 根拠：
  - decoder の出力は `audio_output` で `SOUND_RATE`（48 kHz）の 16 bit 2ch に線形補間されます。
  - サービスは sinc の変換と F32 を持ちますが、ブラウザーの音はそこを通りません。
  - 44.1 kHz の音は折り返しを持ちます。5.1 は 2ch に落ちます。
- 修理の案：decoder の形式のまま `n_audio_open` で開き、変換はサービスへ任せます。

### A16 Range の固定の方針
- 重さ：中。種類：固定の方針。確信：コードで証明。
- `file:line`：`js_avmedia.js:265-270`、`avmedia.c:434-446`。
- 根拠：Range の `play` は他の Range の要素を止めます。`pause` は Range の子を殺します。標準に無い制限です。一つの頁で二つの Range の要素を同時に鳴らせません。
- 修理の案：子の数を資源として数え、方針を外します。

### 軽い所見

| 種類 | 内容 | `file:line` |
|---|---|---|
| 古い世代 | `removeSourceBuffer` の直後に、集めてあった古い PCM を 1 束（85 ms 以下）渡しうる。`step` は取り出す時に revision を見ない。可能性あり | `media_mse_worker.c:293`、`avmedia.c:699` |
| 遅れ | 要素の音量と `muted` は提出時の掛け算なので、輪にある 170 ms 以下は元の音量で鳴る | `avmedia.c:1257-1258` |
| 誰も読まない | `out.errors` | `apps/audio.c:872` |
| 誰も読まない | `started_at` | `hda.c:204`、`hda.c:527` |
| 嘘の値 | `playing` は時計で数えている時も 1 | `apps/audio.c:1052` |
| 決め打ち | `corruptedVideoFrames` が 0 | `js_avmedia.js:322` |
| 決め打ち | HDA の channel が 2 まで | `hda_codec.c:352` |
| 古いコメント | "the kernel's mixer" など。混合はサービスの持ち物 | `avmedia.c:295`、`avmedia.c:165-167`、`avmedia.c:1381` |
| 誤りの隠れ | `matrix_build` の記憶切れが無音になる | `apps/audio.c:204` |
| 流れの残り | Web Audio の suspend は flush しない | `webaudio.c:1077-1080` |

## 問い 6：偽物の一覧

| 形 | 当たるもの |
|---|---|
| 成功を返して何もしない | `srcObject` の代入（A2）。`autoplay`、`defaultMuted` の setter（A4）。 |
| 決め打ちの値 | `mseQuota` の `INFINITY`（A10）。`corruptedVideoFrames: 0`。HDA の 2ch。`SOUND_RATE` への固定（A15）。 |
| 計算したのに誰も読まない値 | `out.errors`、`started_at`。 |
| 呼ぶ者のいない道 | `mediaEvents` の `abort`、`stalled`、`progress`（A6）。 |
| 途中で切れた道 | 音声の track の選択（A3）。`AudioTrack.enabled` は枠に届きません。 |
| 測らずに置いた固定の待ち | `monitor` の 40 ms の輪（A6）。 |
| 仕様の手順の一部だけ | load の手順（A6）。除去の手順（A1）。`changeType`（A5）。`abort`（A9）。`currentTime`（A11）。 |
| 取り消しのない非同期 | 子の APPEND（A9）。 |
| 古い世代の結果を渡す所 | `initial` の append の床（A7）。`removeSourceBuffer` の 1 束（軽い）。 |
| コメントと食い違う動き | "the kernel's mixer"。`js_avmedia.js:5-6` は `autoplay` の無さを書いており、これは合っています。 |
| VM の道だけがある所 | 見つけていません。実機の道（HDA）は本物の形です。実機で動く証拠は持っていません。 |

## 修理の順の案

1. A1、A4：止まらない音と増える音を消します。標準の手順で書きます。
2. A3、A5：同じ `MediaSource` での切り替えを正します。
3. A2、A6、A9：load と `abort` の手順を標準に合わせます。
4. A7、A8：カーソルを位置に基づく形へ変えます。

## 読んで本物と確かめた部分

- `close_audio`（`avmedia.c:262`）：flush、close、時計の欄の消去、`rate_decoder` の破棄まで実際に行います。
- `unload`（`avmedia.c:269-294`）：子の停止と回収まで行います（`media_mse_worker.c:49-57`、`:333-334`）。
- サービスの `stream_flush`（`apps/audio.c:931-964`）：装置がまだ読まない塊まで全部の流れを戻し、他の流れで混ぜ直します。名前のとおりです。
- `moved` と `timeline`、`revision`、epoch の守り：seek と `remove` と `removeSourceBuffer` で働きます。
- 動画の世代の守り（`media_mse_worker.c:144`、`:308`、`media_worker.c:241`、`:261`）。
- 子プロセスは音の装置を持ちません（`browsermediaworker.c:2`）。
- 混合、sinc の変換、閉ループ（`pace_report` :538-589）、輪の確かめ（`streams_look` :494-524）、サービスの再起動の続き（`libc/audio.c:73-103`）。
- `client_event` は一つの事象に一通です。事象はレベルなので、取りこぼしません（`kernel/src/sys/events.c:2`）。
- `media_rate.c`：FFmpeg の atempo と swr を実際に使います。
- HDA のリセット、CORB と RIRB、BDL、SRST、FIFOS の guard、割り込み（コードの読み。仕様書とは照らしていません）。
- codec の経路探索、ELD、DIP、電源の復帰。
- `vm/vm.h` の分離。
- 設定の音の頁。
- Web Audio の閉じる時の flush。
- §17 の文書は、読んだ範囲でコードと合っています。

## 読めなかった範囲

- YouTube の実際の操作。JS を読んでいません。OS も動かしていません。
- `media_mse_parser.c`、`media_adaptive*.c`、`media_http.c` は `rg` で要点だけを見ました。
- `webaudio_dsp.c` は読んでいません。`webaudio.c` は流れの部分だけです。
- 窓を閉じる段取りの全部（`machine.c` と窓の管理）は読んでいません。
- 親の realm から枠の中の要素を操作する道：`avmedia.c:928` の拒否を読みました。`contentDocument` は子の realm の文書を返します（`js_frames.js:85`）。ここで投げる証拠はありません。全部の道は辿っていません。
- 実機の HDA の振る舞い（LPIB の精度、FIFO の大きさ、codec の遅延の実際）。
- 実際の遅延の数は全部、コードからの見込みです。測っていません。
- WPT の結果。実行していません。「通らない読み」は読みの判定です。
