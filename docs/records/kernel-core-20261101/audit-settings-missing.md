# 設定の欠け：敵対的な監査

版は main の 5043e3e6。読むだけの監査で、ビルドも QEMU も動かしていない。

## 依頼文（そのまま）

> あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機、GUI が先）の敵対的な監査役。読むだけで、コードを変えない。ビルドも QEMU も動かさない。main の今の版（5043e3e6 以降）を読む。あなたの仕事は「この OS は設定の面で十分だ」という主張を壊すこと。合格を探さず、欠けを探す。見つからないと言うなら、どこをどう探したかを書く。
>
> # 持ち主の言葉（そのまま）
> "VRR and gsync are settings, one is a os setting one is driver setting. This is something that is an overall issue not just a issue with vrr. we apply overship haphazardly to things, with out finding out what is normal. and we also have like verylittle real support for settings, overall."
> "this is not just for vrr or display stuff. we lack a proper settings system. and stuff like this shouldn't be too hardcoded. we should be carefull, and maybe do an adversarial audit first of what we are missing."
> AGENTS.md の規則：設定の持ち主はシステムで、変えた瞬間に効き、設定アプリは窓だけ。プログラムの設定はそのプログラムの持ち物で、カーネルや共有ファイルに持たせない。標準が利用者の選択を前提にするなら、固定値で済ませず設定とそれが効く経路まで作る。GUI が先。
>
> # 読む所
> `kernel/src/sys/keep.c`、`kernel/src/gui/`、`kernel/src/dev/`（audio、input、power、pm、hibernate）、`kernel/src/net/`、`common/abi.h`、`docs/nocturne-native-abi.md`、`user/apps/settings.ts`、`user/apps/js.c`、`user/apps/browser_settings.h`、`user/libc/webprefs.c`、`user/apps/init.c`、`ports/nvidia`、`rootfs/`。地図は `.claude/rules/map.md`。
>
> # 比べる相手
> 普通の今の OS と driver の利用者向けの設定に何があるかを、公式の文書で調べる（Microsoft Learn と Windows の Settings の文書、DirectX Developer Blog の OS Variable Refresh Rate、NVIDIA と AMD の control panel の公式の文書、Apple の利用者向けの案内。`WebSearch`・`WebFetch`）。ほかの OS のソースコードは読まない。真似るためでなく、欠けを見つける物差しとして使う。
>
> # 問い
> 1. 利用者が普通に変えられるのに、Nocturne で変えられない設定を、分野ごとに全部挙げる（画面、色と HDR、拡大、複数画面の配置、VRR、音の入出力、入力、アクセシビリティ、電源、網と Wi-Fi、時刻と時間帯、言語と地域、字体、外観、通知、既定のアプリ、起動、記憶域、更新、利用者、プライバシーと権限、アプリごとの GPU の好み、driver の設定）。Nocturne に要らないと言えるものは理由を書く。
> 2. 設定の仕組みそのものの欠け：型と範囲の検証、列挙、既定値と既定に戻す、変更の通知、変えた瞬間に効くか、壊れた保存の回復、書込みの原子性、利用者ごとと機械全体の分け、画面ごとや装置ごとの設定の鍵（装置を挿し直したとき同じ値に戻るか）、アプリごとの上書き、権限（誰が変えられるか）、移行、窓から全部に届くか、アプリから読めるか。
> 3. 持ち主の誤り：カーネルが持っているが持つべきでない設定、OS の設定と driver の設定の混同、アプリの持ち物が共有の所にある、持ち主が二人いる値。
> 4. 窓に出ているのに効かない項目、効くのに窓に無い項目、CLI や旗や環境の設定でしか変えられない項目。
>
> 各項目に、ファイル:行（あれば）、壊れ方、普通の持ち主と出典の URL、重さ（毎日当たる／当たれば壊れる／きれいさ）を書く。
>
> # 報告
> `docs/records/kernel-core-20261101/audit-settings-missing.md` へ日本語で全文（この依頼文を先頭に）。返事にも要点を日本語で返す。サブエージェントをさらに出さない。Python で書くなら `newline='\n'`。

作業の途中で届いた指示（そのまま）：

> 持ち主の言葉（そのまま）："NO, you are not understanding. WE AREMISSING ABSTRACTION THAT NEEDS TO BE THERE. WE HAVE NO PROPER SETTINS SYSTEM ACROSS THE"（ここで切れている）。
> 主題を換える：一番の問いは問い 2 で、足りない項目の一覧は例として使う。報告の先頭に、OS 全体を通す設定の抽象化の層に何が要るかを置く：カーネル・driver・system service・アプリの各部品がどう登録し、読み、変更を受けるか。schema（型・範囲・列挙・既定）、scope（機械・利用者・画面や装置・アプリごと）、持ち主と権限、通知、保存、ABI、窓がどう全部に届くか。今のコードにその層が無いことで、部品ごとに何を手で書き直しているかを、ファイル:行で全部示す（`keep.c` の `.conf`、個別の syscall、`js.c` の口、ブラウザーの prefs など）。部品を足すたびに何行、どの層を触ることになるかも数える。設定のほかにも、同じ理由で欠けている抽象化の層が見つかれば別の節に書く。

---

## 1. 結論

設定の層は無い。あるのは、設定ごとに手で通した別々の道である。

- 「設定」という語を知る型・口・登録は、OS のどこにも無い。システムの値は 5 個（ポインターの速さ、滑らかな階調、音量、消音、モニターごとのモード）だけで、口はそれぞれ別の形をしている。
- 共通の部品は二つだけある。`sys/keep.c`（カーネルの部品の文書の保存）と `sys/watch.h`（変化の知らせ）。どちらも値の型も範囲も既定も名前も知らない。カーネルの中の部品しか `keep` を使えない。
- driver は設定を持つ道が無い。`nvrm` の設定の入口は環境の変数だけで、init はそれを渡さない（`user/apps/init.c:142-145`）。VRR と G-SYNC の二つの持ち主を書く場所が、今の構造に無い。
- 窓は値を列挙できない。`settings.ts` の頁は 3 個の決め打ちで（`user/apps/settings.ts:15-20`）、driver もアプリも頁を足せない。
- 足す費用は、既存の部品に値を一つで 8 ファイル・約 36 行、新しい部品で 12 ファイル・約 240 行である（§3.2）。この費用が、設定が 5 個で止まっている理由である。

一番に当たる例：VRR の選択肢は無い。どの driver も `N_SCREEN_MODE_VRR` を立てない（`ports/nvidia` に `N_SCREEN_MODE_VRR` も `vrr_min_mhz` も無い）。`wm.c:2049` は `vrr = on && d.vrr_min_mhz` と、申告だけで方針を決める。OS の切替えと driver の切替えと、アプリごとの上書きを置く所が無い。

## 2. 要る層

### 2.1 守る条件（持ち主が既に決めたこと）

`docs/records/settings-ownership-20261027.md` の決定と持ち主の言葉から、次は層の条件である。

- 値を効かせる者が持ち主である。値を一か所に集めない。全部の設定を並べた決め打ちの表を作らない。
- プログラムは層を使わなくても動く。移植した Firefox は自分の profile を持ち、OS は何も知らなくてよい。
- 全プログラムが共有する文書を作らない。カーネルに利用者の値の保存を強いない。
- CLI の段を持たない。設定は窓から届く。

したがって、作るのは値の倉庫でなく、**持ち主を結ぶ契約**である。登録するのは持ち主で、中央に残るのは「誰がどの名前空間を持つか」だけになる。値と効かせ方は持ち主に残る。

物差しとして、Windows は機械全体（HKLM）と利用者（HKCU）を分け、利用者の値が優先する場面を持つ（[M1][M2]）。Apple の defaults は領域の探索の順（引数、アプリ、全体、登録の既定）を持つ（[A1]）。AMD は全体の設定を継ぐアプリごとの profile を持つ（[D2]）。Windows のアプリごとの GPU の好みは全体の設定より優先し、Reset で既定へ戻る（[M3]）。Nocturne はこのどれも持たない。真似るのでなく、問いの一覧として使う。

### 2.2 層の部品

| 部品 | 要ること | 今 |
|---|---|---|
| 名前 | `持ち主の領域 / 鍵 @ scope の実体`。例：`display/variable-refresh @ monitor:<id>`、`nvidia/gsync-mode @ monitor:<id>`、`browser/languages @ user`。領域は持ち主が登録し、鍵は持ち主が決める。中央に鍵の表を置かない。 | 名前は部品の文書の中の文字列（`keep.h:27-30`）と C の欄の名前で、外から引けない。 |
| schema | データで書く記述。型（真偽、整数、列挙、文字列、順序つきの列、複合）、範囲と刻み、列挙の選択肢と表示名、単位、既定値、とれる scope、効き方（すぐ／試行して戻す／持ち主の再起動）、今使えるかとその理由、変えてよい者、頁と順序の案内、検索の語、schema の版。 | 無い。範囲は各所に書き写してある（§3.1）。 |
| 持ち主の契約 | 五つの操作だけ：記述を返す、値と出所を返す、検証して当てる、既定に戻す、変化を知らせる。カーネルの部品は関数の表、driver とアプリはチャンネルの同じ電文で実装する。 | `struct keep` は `line`・`text` だけで（`keep.h:24-34`）、読み戻しと保存しか表さない。 |
| scope | 既定 → 機械 → 利用者 → 装置の実体 → アプリの上書き、の探索の順と、値がどの scope から来たかの報告。窓は「このアプリの上書きを消す」を出せる。 | 全部が利用者の文書の一種類で（`keep.c:96-100`）、機械の scope も、アプリの上書きも無い。装置の鍵はモニターだけに手で書いてある（`screenmode.c:57-62`）。 |
| 装置の身元 | 安定した装置の id を一つの部品で作る（モニター、音の装置、入力の装置、NIC、GPU）。同じ型の二台を区別する。挿し直しで同じ値へ戻る。 | モニターだけ。EDID の maker・product・serial で（`common/edid.h:60-70`）、シリアルが同じ二台は同じ鍵になる。ほかの装置に鍵は無い。 |
| 権限 | 値ごとに「誰が変えられるか」を記述し、層が呼ぶ者の身元で判定する。 | 判定も身元も無い（§3.3 の権限）。 |
| 通知 | 一つの見張りの口で、領域または鍵の接頭辞を指定する。知らせは「どの鍵が、どの scope で、どの世代へ」を持つ。 | 見張りの種類は一つだが（`N_KIND_WATCH`、`sys/watch.h`）、開く口が部品ごとで（`abi.h:149`、`display_abi.h:66`）、知らせは数だけで鍵を持たない。受け手は全部を読み直す（`settings.ts:281-310`、`:331-349`）。 |
| 保存 | 共通の文書の部品を一つ、カーネルと libc の両方で組む（`common/store` と同じ形）。版の行、知らない行を残す、書けなかったことを持ち主と窓へ返す、再試行、壊れた文書を脇へ退けて残す、scope から道を決める。 | カーネルの `keep.c` とブラウザーの `webprefs.c` と `agent.c` が別々で、振舞いが違う（§3.1、§3.3）。 |
| 試行と戻し | 「効かせて、確かめの窓を出し、期限で戻す」を効き方の一つにする。複数の鍵を一度に当てる取引を持つ。 | 画面のモードにだけ手で書いてある（`screenmode.c:198-234`、`:488-503`、`settings.ts:74-120`、`:197-206`）。 |
| ABI | 一つの族：`n_setting_list`、`_describe`、`_get`、`_set`、`_reset`、`_watch`、登録。値は鍵で引くので、値を足しても ABI の構造体は変わらない。 | 値の族ごとに構造体と番号がある（§3.1）。`struct desktop_settings` は残り 5 バイト（`display_abi.h:39`）で、`sizeof` を厳密に見る（`wm.c:3537`）。 |
| 言語の口 | C（libc の `n_setting_*`）、JS と TS（`nocturne/settings`）、Go と Rust の std は同じ ABI の上。窓の無いプログラムも読める。 | C に口が無く（§3.3）、JS の見張りは窓を要る（`js.c:990`）。 |
| 窓 | Settings は schema から行を作る。型ごとの部品が既定で、特別な部品（モードの一覧）は「部品の案内」で選ぶ。頁は登録された領域から作る。検索は schema から作る。NVIDIA の頁は `nvrm` が登録したときだけ出る。ブラウザーの設定の頁も同じ描き手を使える。 | 頁も行も `settings.ts` に手で書く。ブラウザーは別の言語で別の表を持つ（`browser_settings.h:17-21`、`:618-624`）。 |
| 一時の上書き | cmdline と環境は「引数の領域」の一つとし、出所つきで見える。窓と同じ鍵を変える。 | `cmdline_has`、`cmdline_number`、手書きの走査、`getenv` が別々（§3.1）。 |
| 試験 | 登録された全部の鍵に、型から作る試験を当てる：範囲外を拒む、当てて読む、再起動で戻る、既定に戻す、見張りが鳴る。 | 値ごとに手で書く（`tests/systemwatchtest.c`、`tests/audiotest.c:179-202`）。`keep` のホストの試験は今の木に無い（`tests/keep_hosttest.c` は無い）。 |

### 2.3 部品の種類ごとの動き

| 種類 | 例 | 登録 | 読む | 変更を受ける |
|---|---|---|---|---|
| カーネルの部品 | wm、音、スケジューラーの方針 | `kmain_late` で静的な schema の表と関数の表を登録する | 読み戻しは init の前。部品の値は自分の変数のまま | 層が検証し、部品の `apply` を呼ぶ。眠れない文脈の値は部品が自分で区切る |
| driver（アプリの driver） | `nvrm` | 起動時に自分のチャンネルで schema を送る。装置の実体ごとに登録する | 層が scope を解いて値を渡す。driver は環境を読まない | チャンネルで `apply`。driver が死ねば領域は「使えない」になり、値は残る |
| system service | 今は無い（§5.1）。電源の方針、時刻合わせ、更新 | driver と同じ | 同じ | 同じ |
| 組み込みのアプリ | ブラウザー | 登録は任意。登録すると窓と検索に出る | 自分の文書を自分で読む。値は持ち主の物のまま | 自分の見張り。今の `webprefs` はこの形に近い |
| 移植のアプリ | Firefox | 登録しない。OS は何も知らない | 自分の profile | 自分 |

OS の設定と driver の設定は、**領域が二つあること**で分かれる。VRR なら次の形になる。

- `display/variable-refresh`：持ち主は合成。OS の切替えと、合成の方針（[M4][M3]）。
- `nvidia/gsync-mode`：持ち主は `nvrm`。全画面だけか窓も含むか、モニターの型ごとの有効（[D1]）。AMD なら `amd/freesync`（[D2]）。
- `display/variable-refresh @ app:<id>`：アプリの上書き（[M3][D2]）。
- driver は「この表示はこの範囲を持つ」を能力として報告する。持ち主の違う二つの値を、モードの旗一つに潰さない。

### 2.4 持ち主が決めること

1. 仲立ちを置く場所。私の案はカーネルの小さな部品である。理由：今の持ち主の 3 個はカーネルの中で、init より前に値が要る。チャンネルの反対側へ driver とアプリを置ける。仲立ちは値を持たず、領域の表だけを持つ。AGENTS.md の「カーネルを変えずに済まない案なら書く前にユーザーに聞く」に当たるので、書く前に承認が要る。
2. `docs/records/settings-ownership-20261027.md` の決定 1「全部の設定を並べた表はどこにも作らない」を、「中央に鍵の表を作らない。実行時に持ち主が自分の schema を出す」と読むこと。列挙は実行時の問合せで作る。
3. 機械の scope の文書の置き場所（`C:\Nocturne` の下か）。モードと音量を機械と利用者のどちらへ置くかの既定。
4. カーネルが持っている方針（§5.1）を先に system service へ出すか、層を先に作るか。

## 3. 層が無いことで手で書いているもの

### 3.1 関心ごとの全部

**文書の形式と解析：六つ**

| 場所 | 中身 | 違い |
|---|---|---|
| `kernel/src/sys/keep.c:122-142` | 行、`#`、`=`、空白の切り詰め | 知らない行は次の書込みで消える |
| `user/libc/webprefs.c:359-418` | 版の第 1 行、行、`name=value`、鍵ごとの局所の bool | 知らない行を残す。第 1 行が違えば既定から始め直す（`:416-419`） |
| `user/apps/agent.c:159-188` | `strtok_r` で行、`strcmp` の連なり、`atoi` | 範囲を見ない |
| `ports/nvidia/nvkms_os.c:155-173`、`:108-140` | `;` 区切り、bool の表 | 環境の変数から |
| `ports/nvidia/nvrm.c:270-274`、`os.c:1075-1092` | RM の registry、`NVreg_*` の表 | 環境の変数から |
| `kernel/src/boot.c:70-95`、`net.c:976-983`、`net.c:2486-2509` | `cmdline_has`・`cmdline_number`、DNS の解析、TCP の手書きの走査 | `net.c` は `cmdline_number` があるのに自分で歩く |

**値の読み書きの手書き：部品ごと**

| 部品 | 手で書いた行 |
|---|---|
| 滑らかな階調・速さ | `wm.c:433-437` 変数、`:1176-1184` 当てる、`:1189-1209` 解析（`deep_colour` の別名つき）、`:1211-1219` 書き出し、`:3021` 登録 |
| 音 | `audio.c:68-70` 既定、`:454-482` 当てる、`:485-497` 解析、`:499-508` 書き出し、`:768` 登録 |
| モード | `screenmode.c:57-62` 表、`:109-126` 探す・足す、`:372-386` 値の形、`:390-396` 鍵の検査、`:399-418` 解析と書き出し、`:339` 登録 |
| ブラウザー | `webprefs.c:203-217` 構造体と既定、`:390-418` 解析、`:420-464` 文書の組立て、`:713-785` 編集、`:701-710` と `:795-797` 値ごとの読み書き |
| agent | `agent.c:159-199`、`:13-16` は使い方の文 |
| NVIDIA の表示の選択肢 | `nvkms_os.c:69-140`、`ports/nvidia/README.md:270` |

**範囲の書き写し：同じ規則が別々の層にある**

- ポインターの速さ −2〜2：`display_abi.h:32`、`wm.c:1193`（`keep_number`）、`wm.c:3542`、`wm.c:2609-2610`（表の長さ）、`js.c:899`、`settings.ts:232-233`（選択肢）、`ui.d.ts`。七か所。
- 音量 0〜100：`abi.h:580`、`audio.c:481`、`:489`、`syscall.c:2851`、`settings.ts:50`、`:245`、`wm.c` の面。
- 既定の音量 80：`audio.c:68`、`:70`（二乗した値を手で書く）、`tests/audiotest.c:180`。

**ABI の口：値の族ごと**

| 族 | 口 |
|---|---|
| 速さ・階調 | `SYS_SCREEN_INFO` の第 3 引数の op `SCREEN_SETTINGS_*`（`display_abi.h:23-41`、`wm.c:3533-3553`）。ポインターの速さが「画面の情報」の中にある |
| モード | 同じ syscall の `op \| screen << 8`（`display_abi.h:42-58`、`:78-101`、`screenmode.c:439-527`）。呼ぶ側の版のための `SCREEN_MODES_FIRST_BYTES`（`display_abi.h:87`、`screenmode.c:446`） |
| 音 | 専用の番号 `SYS_AUDIO_OUTPUT`（`abi.h:104`、`:572-583`、`syscall.c:2845-2863`、`:3194`）、set の旗を手で定義 |
| 見張り | `SYS_AUDIO_OUTPUT_WATCH`（`abi.h:149`、`audio.c:85-87`、`:510`）、`SCREEN_WATCH`（`display_abi.h:66`、`wm.c:442`、`:3609`）、`SYS_ENTRY_WATCH`（`webprefs.c:557-590`） |
| driver | `n_screen_modes`（`abi.h:463-470`）と `n_screen_state`（`abi.h:486-496`）に欄を足すしかない |

**言語の口：値ごとの接着**

| 層 | 手書き |
|---|---|
| C | `nocturne.h:283-288`、`sys.c:504-505`、`gui.c:26`。速さ・階調・モードの C の口は無い |
| QuickJS | `js.c:877-904`（`__syscall` を直に呼ぶ）、`:906-975`、`:977-1008`、`:1034-1054`、`:1073-1085`、`:1097`、`:1470-1474`。見張りは「両方開くかどちらも開かないか」の決め打ちで（`:983`）、三つ目で書き直す |
| JS の包み | `js_ui.js:388-405`（`screen`・`sound` の決め打ち）、`:417-436` |
| 型 | `rootfs/Nocturne/SDK/Types/nocturne/ui.d.ts:327-392`、`:405-425`（`SystemChange` は決め打ちの和） |

**窓：三つの描き手**

| 窓 | 場所 | 言語 |
|---|---|---|
| Settings | `settings.ts:15-20`、`:43-56`、`:150-249`、`:281-353` | TS |
| タスクバーの音量の面 | `wm.c:1313` 以降 | カーネルの C |
| ブラウザーの設定 | `browser_settings.h:17-21`、`:42-44`、`:461-491`、`:618-624`（検索の語を手書き）、`:700-745`（作用の switch） | C と `ui_msg` |
| agent | 端末の問いと `conf_save`（`agent.c:191-199`） | CLI |
| 試行の表示 | `settings.ts:197-206`、`:301-305`、`:315-326` | モードにだけ |

**移行：値ごとの手書き（AGENTS.md「互換は持たない」に反する）**

- `keep.c:15-17`、`:199-250`：カーネルが以前の二つの文書の道と引継ぎを持つ。
- `wm.c:1186-1200`：`deep_colour` の別名。
- `form_autocomplete.c:232`：`search-history.enabled` の後始末。
- `scripts/test.py:390-391`：その試験。
- `display_abi.h:87`：古い構造体の大きさ。

### 3.2 足す費用の数え

行数はコメントを含む。今のコードの区間から数えた。

**(a) 既存の部品に値を一つ。** 実物の `smooth_gradients` の足跡は 8 ファイル 36 行である（`rg -c` で数えた）。

| 触る層 | ファイル | 行（実物） |
|---|---|---|
| ABI | `common/display_abi.h` | 3 |
| 持ち主と効き目 | `kernel/src/gui/wm.c`、`kernel/src/dev/fb.c`、`fb.h` | 22 |
| QuickJS | `user/apps/js.c` | 3（位置の引数で渡すので呼び口も変わる） |
| JS | `user/apps/js_ui.js` | 1（同上） |
| 型 | `ui.d.ts` | 4 |
| 窓 | `settings.ts` | 3 |

`pointer_speed` も 8 ファイル 35 行で、そのうち試験が 13 行である。`display_abi.h` に触るので全体の組み直しになる。`desktop_settings_put(speed, smooth)` は位置の引数で、値を足すと全ての呼び口（`wm.c:1195`、`:1203`、`:3545`）が変わる。

**(b) 新しい部品（音と同じ形）。** 実物の音の道で数えた。

| 層 | 区間 | 行 |
|---|---|---|
| 持ち主の保存と当てる | `audio.c:454-510`、`:85-87` | 60 |
| ABI | `abi.h:572-583` と番号 2 | 14 |
| syscall | `syscall.c:2845-2863` と case 2 | 21 |
| libc | `sys.c:504-505`、`nocturne.h:283-288` | 8 |
| QuickJS | `js.c:1032-1054`、`:977-1008`、`:1073-1085` | 68 |
| JS | `js_ui.js:388-405`、`:422-425` | 22 |
| 型 | `ui.d.ts:342-356`、`:367-372` | 21 |
| 窓 | `settings.ts:49-56`、`:237-249`、`:341-347` | 28 |
| 合計 | 9 ファイル（ほかに `audio.h`、`syscallfuzz.c`、文書 §9・§12、試験） | 242 |

試験は別で、`tests/systemwatchtest.c` が 202 行ある。音の見張りと画面の見張りが共有する行を含むので、新しい部品で全部が増えるわけではない。増えないのはその共有の行だけで、`js.c:983` の決め打ちは書き直しになる。

**(c) ブラウザーの好みを一つ。** `search-history` で数えた：`webprefs.h` 2、`webprefs.c` 約 19、`browser_settings.h` 約 40（enum 1、カード 31、表 1、作用 7）。3 ファイル約 60 行と、使う側である。

**(d) driver の設定を一つ。** 道が無い。作るなら次が全部要る：カーネルの部品（(b) の全部）、`n_screen_state` の欄（`abi.h:486-496`、全体の組み直し）、`SYS_PCI_SCREEN_STATE` の配り、`ports/nvidia/display.c` の読み。driver が値を持つのに、持ち主をカーネルに書くことになる。これが持ち主の言う "overship haphazardly" の形である。

**(e) 層があるときの費用。** 持ち主の schema の 1 行と効き目のコードだけである。ABI、libc、QuickJS、JS、型、窓、試験の表に触らない。

### 3.3 問い 2：仕組みの欠け

| 項目 | 今 | 壊れ方 | 普通 | 重さ |
|---|---|---|---|---|
| 型と範囲の検証 | 値ごとに手書きで、読み戻しでは黙って捨てる（`wm.c:1193`、`audio.c:489`）。agent は `atoi` で見ない（`agent.c:180-183`） | 手で壊した値も版の違いも、記録なしで既定に戻り、次の書込みで消える | schema | 当たれば壊れる |
| 列挙 | 無い。システムの設定の一覧を得る口が無い。Settings の検索も無い | 窓も試験もアプリも全部を知らない | Windows は Settings の分類と目録を持つ（[M5][M6]） | 毎日当たる |
| 既定値 | 持ち主の C の初期値で、外から読めない | 窓に「既定」を出せない | [M3] の Reset | 毎日当たる |
| 既定に戻す | システムの値に一つも無い。モードの選択は消せない（`screenmode.c:115-126` は足すか上書き。`SCREEN_MODE_*` に消す操作が無い）。ブラウザーは言語だけ（`webprefs.c:793`） | 一度 Keep すると「モニターの推奨に任せる」へ戻れない | [M3] | 毎日当たる |
| 変更の通知 | ある。ただし数だけで鍵が無い。`SCREEN_WATCH` は速さの変化でも鳴り、モードを読み直す | 受け手が全部読んで比べる（`settings.ts:281-310`）。自分の変更と他人の変更を分けられず、`dragging` の特例が要る（`settings.ts:34`、`:341`） | | きれいさ |
| 変えた瞬間に効くか | 今ある 5 個は効く。モードは試行つき。ブラウザーは見張りで効く | 良い所。ただし `keep_flush` までの間の電源断は最大 0.5 秒の変更を失う（`keep.h:22`） | | 問題なし |
| 全部を書き戻す競合 | `h_desktop_set` は GET してから全部を SET し（`js.c:895-903`）、`settings.ts:44` は古い写しを渡す。比較して当てる形が無い | 二つの窓が別の値を変えると、片方の変更が戻る | 鍵ごとの set | 当たれば壊れる |
| 壊れた保存の回復 | 読めなければ既定のまま `read=true` にする（`keep.c:272`、`:278`）。次の変更が読めなかった文書を上書きする。`READ_MOST` の 64 KiB を超えた文書も同じ（`keep.c:11`、`:109`）。`webprefs` は第 1 行が違うと新しく始める（`webprefs.c:416-419`） | 一時の読み失敗で利用者の値が消える。写しも残らない | | 当たれば壊れる |
| 書込みの原子性 | ある：`store_save` は REPLACE と commit の待ち（`store.c:924-932`）、`n_document_save` | 良い所 | | 問題なし |
| 書けなかったとき | `kprintf` だけで（`keep.c:156`）、窓へ返らない。`retry` は次の変更か `keep_flush` まで眠る（`keep.c:176`、`:288`） | `settings.ts:263` は「Changes apply at once and are kept after a restart.」と出し続ける。書けていなくても同じ文である | | 当たれば壊れる |
| 版 | `keep` の文書に版が無い。知らない行は次の書込みで消える（各 `text` は知る鍵しか書かない） | 版を戻すと新しい値が消える。`webprefs` は残すので振舞いが違う | | きれいさ |
| 利用者と機械の分け | 無い。全部が `C:\Users\<名>\AppData\Nocturne\`。利用者は起動時に一人だけ決まる（`store.c:395-420`）。モードと音量も利用者の文書 | 複数の利用者を作れない。機械の値（網、時刻合わせ、起動の項目、ログイン前の画面）の置き場が無い | HKLM と HKCU（[M1][M2]）、global と application（[A1]） | 当たれば壊れる |
| 装置ごとの鍵 | モニターだけ。挿し直しで戻る（`screenmode.c:156-158`、`:269-281`）。音、入力、NIC は無い。ポインターの速さは全マウス共通 | 同じ EDID の二台は同じ鍵になる（`edid.h:60-70`）。EDID が無ければ接続口の名（`display.c:382-385`）で、口を変えると失う。周波数が 1 mHz でも違う口では合わず、黙って推奨へ戻る（`screenmode.c:81-83`、`:157`） | Windows はモニターの配置と切断時の窓の扱いを持つ（[M8][M13]）、NVIDIA はモニターの型ごとの設定を持つ（[D1]） | 当たれば壊れる |
| アプリの上書き | 無い。アプリの身元の概念も無い | GPU の好み、VRR、音量のアプリごとの値を置けない | [M3][D2][M7] | 毎日当たる（ゲームで） |
| 権限 | 無い。どのアプリも `SCREEN_SETTINGS_SET`、`SCREEN_MODE_SET`、`n_audio_output`、`SYS_POWER`（`syscall.c:3128-3133`）を呼べる。文書は「書くのはその持ち主だけ」と言うが（ABI §12）、`AppData\Nocturne` は `STORE_PRIVATE` でなく（`store_format.h:167`）、init 自身が利用者空間から書く（`init.c:77-82`） | 悪いアプリが音量やモードを変え、次の起動で読まれる文書を書ける | 機械全体の値と利用者の値を分ける（[M1]）、アプリの権限は [M9] | 当たれば壊れる |
| 移行 | 手書き（§3.1） | カーネルに過去の文書の道が残る | schema の版 | きれいさ |
| 窓から全部に届くか | 届かない（§8） | | | 毎日当たる |
| アプリから読めるか | C は速さ・階調・モードの口が無い。JS の見張りは窓を要る（`js.c:990`）のに、ABI §12 は「窓の無い Worker やサービスでも使える」と言う | ゲームは C から画面の好みを読めない。外観の値はそもそも無い | [A1] の global | 毎日当たる |

## 4. 良い所（壊せなかったもの）

- 持ち主が自分の文書を持ち、共有の文書が無いこと。層の条件として残す。
- 置き換えの原子性（`store_save`、`n_document_save`）。
- 見張りの一つの種類（`sys/watch.h`、`N_KIND_WATCH`）。層の通知の土台になる。
- モードの試行と自動の戻し（`screenmode.c`）。層の「試行と戻し」の実物になる。
- モニターの挿し直しで選択が戻ること。
- `webprefs` の知らない行を残す作り。層の保存の見本になる。

## 5. 同じ理由で欠けている、ほかの抽象化の層

設定の層が無い理由と同じで、部品ごとに手で決め打ちしている。

| 層 | 証拠 | 壊れ方 | 重さ |
|---|---|---|---|
| 5.1 system service | 方針がカーネルにある：スタートの一覧 `wm.c:1047-1071`、デスクトップのアイコンの番号 `wm.c:1091`、タスクバー `wm.c:53`、`:1412` 以降、音量の面 `:1313` 以降、壁紙 `desktop.c:107`、通知 `wm.c:3059`、配置 `wm.c:2000-2016`。電源の方針の持ち主は無い | 方針の値を置く持ち主がカーネルしかない。アプリを足すとカーネルの表が変わる | 毎日当たる |
| 5.2 装置の身元 | §2.2。`edid.h:60-70` だけ。音は `audio_card` の名前一つ（`audio.c:394`） | 装置ごとの設定を作れない | 当たれば壊れる |
| 5.3 driver の能力の口 | 表示の契約は `n_screen_modes` だけ。色の深さ、HDR、VRR の範囲の選択、拡大の方式、音の出口の一覧を報告する形が無い | OS が driver の値を決め打ちする。`nvkms_os.c:53`、`:62` の VRR の選択肢は窓に届かない | 毎日当たる |
| 5.4 アプリの身元と登録 | インストールの記録が無い。ファイルの種類と開く先は `files.c:84-92`、`:214-218` の決め打ち。`n_app_data_folder(name)` の name は呼ぶ側が決める（`sys.c:790-796`） | 既定のアプリを選べない。アプリの上書きの鍵が無い。どのアプリも他のアプリのフォルダーを開ける | 毎日当たる |
| 5.5 利用者と権限 | `store.c:395-420` の一人だけ。特権は `init_pid` の判定だけ（`pciuser.c:807`） | 利用者の切替え、管理者、ログイン、ロックが無い | 当たれば壊れる |
| 5.6 秘密の保管 | `api_key` は平文の `AppData\Agent\agent.conf`（`agent.c:16`、`:194`）。5.4 によりどのアプリも読める | 鍵が漏れる | 当たれば壊れる |
| 5.7 地域と文字列 | 表示の文字は各所に直書き：`settings.ts` は英語、`browser_settings.h` は日本語、`wm.c:1464` の曜日は英語。文字列の表が無い。`locale.c:10-16` は C のみ | 言語を変える道が無い | 毎日当たる |
| 5.8 時刻帯 | ABI §8 は「時刻帯は無い」「時計は地方時を持つ」と書き、同じ節で `n_clock_wall` を UTC と書く。`quickjs.c:48993-48998` は差 0 を返す。実機で時刻を合わせる者が無い（`clock_set_wall` の呼び手は `vm/hv/hv_util.c:137` だけ） | RTC が地方時の機械では、壁時計が UTC として誤る。証明書の期限の判定、HTTP の日付、`Date` が地方時の差だけずれる | 毎日当たる |
| 5.9 入力の配列 | `input.c:15` 以降の決め打ちの US 配列がカーネルにある | 日本語の配列のキーボードで記号が違う。入力の方法の層が無い | 毎日当たる |
| 5.10 外観と拡大 | `ui.h:26-33` と `ui.c:189` の定数、`js_ui.js:102-106` は一度読んで凍る。カーネルの色は `wm.c` に別にあり、`fbcon` と `limine.conf` の色も別。拡大の概念が無い（CSS の `resolution` も 1、`css.c`） | 明暗・大きさを変えられない。高密度の画面で全部が小さい | 毎日当たる |
| 5.11 通知 | `wm_notify` はカーネルだけで（`wm.h:27`）、アプリの ABI が無い | アプリは通知を出せない。止める設定も無い | 当たれば壊れる |
| 5.12 電源の方針 | 電源ボタンは決め打ちで切る（`acpi.c:459`、`:464`）。放置で眠る、画面を消すの持ち主が無い。蓋（`PNP0C0D`）を見ない | 蓋を閉じても何も起きない。画面が消えない | 毎日当たる |
| 5.13 音の出口 | 実機の音の driver が無い：`audio_set_card` の呼び手は `vm/ac97.c:300` だけ。入力の口も無い（`abi.h:104` は出力だけ） | 実機の Sound の頁は「No sound device」になる。設定以前の欠けである | 毎日当たる |

## 6. 問い 1：利用者が変えられない設定（例の一覧）

出典の記号は §10。「今」は Nocturne のコード。重さは毎＝毎日当たる、壊＝当たれば壊れる、き＝きれいさ。全部の行で窓の入口が無い。

| 分野 | 変えられないもの | 今 | 普通の持ち主と出典 | 重さ |
|---|---|---|---|---|
| 画面 | 明るさ、画面ごとのオンとオフ、向き、ミラー、モードの選択を消す | 明るさの道は無い（`backlight` の語がカーネルに無い）。`wm.c:2000-2016` | OS（[M8][M10]、[A3]） | 毎 |
| 色と HDR | HDR、Auto HDR、夜間の色、色の深さ、RGB の範囲、色の校正 | `dynamic-range` は `standard` 固定（`css.c:5160-5172`）、`color-gamut` は `srgb` 固定。滑らかな階調だけ | OS（[M11][M10]）、デスクトップの色の調整は driver（[D3]） | 毎 |
| 拡大 | 全体の拡大、文字の大きさ、拡大の方式 | 無い。`ui.h:33` の `fontSize` 14、ポインターは `wm.c:55` の 32 固定 | OS（[M8][M12]）、拡大の方式は driver（[D3]） | 毎 |
| 複数画面 | 配置（並びは番号順で横一列 `wm.c:2000-2016`）、主の画面（画面 0 に決め打ち `wm.c:562`）、タスクバーの置き場所、切断時の窓の扱い | 無い | OS（[M8][M13]） | 毎 |
| VRR | OS の切替え、driver の方式（全画面か窓も）、モニターの型ごとの有効、アプリの上書き、モードごとの選択 | §1。`same()` は旗を見ず（`screenmode.c:81-83`）、保存で旗を落とす（`:124`） | OS（[M4][M3]）、driver（[D1][D2]）、Apple は Refresh rate の選択（[A3]） | 毎 |
| 音 | 出力の選択、入力と入力の音量、アプリごとの音量と出口、形式、立体音響 | 出力は一つの名前（`abi.h:582`）、形式は 48 kHz 固定（`abi.h:569`、`audio.h:21`）、§5.13 | OS（[M7][M14]） | 毎 |
| 入力 | キーボードの配列、繰返しの遅れと速さ（`usbhid.c:18-19`）、加速の有無（`wm.c:2606-2607`）、ダブルクリックの間（`wm.c:486`）、ボタンの入替え、ホイールの量（`ui.c:2563`）、タッチパッド、マウスごとの値 | 速さの 5 段だけ。絶対の座標の装置には効かない（`wm.c:2600-2603`） | OS（[M15][M23]） | 毎 |
| アクセシビリティ | 文字の大きさ、高コントラスト、色フィルター、拡大鏡、読み上げ、固定キー、ポインターの大きさと色、動きを減らす、字幕 | 全部無い。`prefers-reduced-motion`、`prefers-contrast`、`forced-colors`、`inverted-colors`、`prefers-reduced-transparency` は固定（`css.c:5160-5168`）で、AGENTS.md の「標準が利用者の選択を前提にするなら固定値で済ませない」に直接反する | OS（[M12][M5]、[A4]） | 毎 |
| 電源 | 電源ボタンの動作、放置で眠る、画面を消す、蓋、休止の有効、性能の方針 | §5.12。メニューの 4 項目だけ（`wm.c:1066-1069`）。`noresume` は cmdline | OS（[M17]） | 毎 |
| 網 | 手で IP と DNS、代理、NIC の選択、ホスト名、Wi-Fi、IPv6、VPN、MAC のランダム化 | DHCP だけ。DNS は cmdline の `dns-server=`（`net.c:976-983`）。Wi-Fi の語がカーネルとアプリに無い。IPv6 も無い。網の窓が無い（`ifconfig` は表示だけ） | OS（[M19][M20][M21]） | 毎 |
| 時刻 | 時刻帯、夏時間、自動の時刻合わせ、手で合わせる、12 時間と 24 時間、書式 | §5.8。`wm.c:1464` の決め打ち | OS（[M22]） | 毎 |
| 言語と地域 | 表示言語、地域の書式、入力の言語 | §5.7、§5.9。システムの Language の頁は消した（`settings-ownership` §3）。ブラウザーの言語はブラウザーにある | OS（[M23][M22]） | 毎 |
| 字体 | UI の字体、既定の字体、大きさ | `font.c:163` の環境の変数 `FONT_DIR` だけ | OS（[M12]） | き |
| 外観 | 明暗、アクセント、壁紙、タスクバーの動き、デスクトップのアイコン | §5.10、§5.1。`prefers-color-scheme` は `light` 固定（`css.c:5160`）なのに、システムの UI は暗い色（`ui.h:26-31`）で食い違う | OS（[M6]） | 毎 |
| 通知 | アプリごとの有効、邪魔しないで、優先 | §5.11 | OS（[M24]） | き |
| 既定のアプリ | 種類ごとの開く先 | §5.4 | OS（[M5]） | 毎 |
| 起動 | 起動時に開くアプリ、起動の待ち時間（`limine.conf` の `timeout: 3`）、起動の項目 | init は何も読まない（`init.c:155-165`） | OS（[M5]、[A5]） | 毎 |
| 記憶域 | 使用量の表示、掃除、休止の領域 | 窓が無い | OS（[M5]） | き |
| 更新 | 更新の仕組みそのもの | 無い。媒体の入替えだけ | OS（[M25]） | 壊 |
| 利用者 | 利用者の作成、切替え、パスワード、ロック、自動ログイン | §5.5 | OS（[A2][A6]） | 壊 |
| プライバシー | アプリごとの権限 | §5.4。今は音の入力もカメラの driver も無いので、当たる物は保存と網である | OS（[M9]） | 壊 |
| GPU の好み | アプリごとの GPU、既定の GPU、`Optimizations for windowed games` 相当 | 無い（`gpu_abi.h` に好みの欄が無い） | OS（[M3]） | 毎 |
| driver | VRR の方式、デスクトップの色、拡大の方式、回転、3D の全体とアプリごとの値、電力の方針、低遅延、フレームの上限 | 窓も道も無い。`NVKMS_OPTIONS`・`NVRM_REGISTRY_DWORDS` だけ（§8） | driver（[D1][D3][D2][D4]） | 毎 |

**要らないと言えるもの。** 携帯回線と SIM は対象の機械に無い。Windows の互換モード、32 ビットの扱いは「互換は持たない」により持たない。PATH と環境の変数の編集窓は AGENTS.md「PATH を持ち込まない」により持たない。同期と roaming の設定は網の口座が無いので今は要らない。

## 7. 問い 3：持ち主の誤り

| 誤り | 証拠 | 壊れ方 | 普通 | 重さ |
|---|---|---|---|---|
| OS と driver の混同（VRR） | `N_SCREEN_MODE_VRR` はモードの旗で、モニターの能力、driver の有効、OS の方針を一つに潰す（`abi.h:443-447`）。`wm.c:2049` が申告だけで方針を決める | 利用者は切れない。driver が立てれば、OS は問わず合成の方針を変える | OS は切替え（[M4]）、driver は G-SYNC（[D1]）。「OS の機能は driver の機能を置き換えない」（[M4]） | 毎 |
| OS と driver の混同（階調） | 滑らかな階調は OS の合成のディザで、`fb_dither_able` は 8 bpc の xRGB を見る（`fb.c:473-476`）。出力の色の深さを選ぶ窓が無い | 10 bpc の出力を選べず、OS のディザで代わりをする | デスクトップの色の調整は driver も持つ（[D3]） | き |
| カーネルが利用者の文書形式と道を知る | `keep.c:98`、`:17`、`store_user_folder` | カーネルが `AppData\Nocturne` と過去の文書の名を持つ | 持ち主は system service | き |
| カーネルがシェルの方針を持つ | §5.1 | §5.1 | シェル | 毎 |
| カーネルが配列を持つ | `input.c:15` | 配列を変える口が無い | 入力の service（[M23]） | 毎 |
| 機械の値が利用者の文書に | モード、音量（`keep.c:96-100`） | 複数の利用者で矛盾する。ログイン前の値が無い | HKLM・HKCU（[M1]） | 壊 |
| アプリの持ち物が共有の所に | ブラウザーの好みの実装が libc にあり（`user/libc/webprefs.c`）、見出しが SDK に入る（`user/include/webprefs.h`）。見出し自身が「no other program reads or writes them」と言う（`webprefs.h:1-5`） | SDK にブラウザーの内部が出る。AGENTS.md の「共有ファイルに持たせない」に反する | プログラムの中 | き |
| 持ち主が二人 | `fbsim10`（`fb.c:101`）と `deep_wanted`（`wm.c:2754`、`:2763`）が同じ `fb.simulated` を書く。wm が動くと旗は次の回で上書きされる | 旗が黙って無効になる | 一つの出所 | き |
| 持ち主が二人 | DNS：DHCP の `my_dns` と cmdline（`net.c:61`、`:63-64`、`:971-975`）。cmdline が不正なら DNS 全体が止まる（`:972`） | 文字の誤りで名前解決が全部止まる | [M19] | 壊 |
| 持ち主が四人 | 色：`ui.h:26-31`、`wm.c`、`desktop.c`、`limine.conf:7-10` | 外観を一つの値で変えられない | [M6] | き |
| 名前の衝突 | `n_launch.settings` は環境の変数で、「設定」の語が別の物を指す（`init.c:13-14`、`sys.c:388-405`） | 読み手が取り違える | | き |
| 「読むだけの情報」の口に設定がある | ポインターの速さが `SYS_SCREEN_INFO` の中（`display_abi.h:23-33`） | 入力の値が画面の口にあり、持ち主が読めない | | き |

## 8. 問い 4：窓と効き目の食い違い

**窓に出ているのに効かない、または言葉どおりでない**

| 項目 | 証拠 | 壊れ方 | 重さ |
|---|---|---|---|
| 「Variable refresh from … Hz.」 | `settings.ts:159-160`。どの driver も旗を立てない | 実機で出ない。出ても切れない | き |
| 「kept after a restart」 | `settings.ts:263`、`keep.c:156` | 書込みに失敗しても同じ文 | 壊 |
| Pointer speed | `wm.c:2600-2603`：絶対の座標の装置には効かない | その装置で窓だけ動く | き |
| Smooth gradients | 窓は一つで、判定は console の画面（`fb.c:473-476`）。画面ごとの選択が無い | 複数画面で範囲が言葉と違う | き |
| Sound の頁 | §5.13 | 実機で「No sound device」しか出ない | 毎 |

**効くのに窓に無い：cmdline（`boot/limine.conf` を手で編むしかない）**

`dns-server=`（`net.c:976-983`）、`tcp-rx-bytes=`・`tcp-tx-bytes=`（`net.c:2486-2509`）、`noresume`（`hibernate.c:600`）、`noiommu`（`iommu.c:131`）、`noappdrivers`・`appdrivers=any`（`pciuser.c:806-807`）、`appdrivers-unisolated`（`iommu.c:829`、`:859`）、`nosmp`（`smp.c:303`）、`nopcid`（`vmm.c:649`）、`noaml`（`acpi.c:480`）、`fbsim10`（`fb.c:101`）、`fbhold`（`pciuser.c:235`）、`prof`・`prof_ms`（`prof.c:274-275`）、`nogui`（`kernel/src/init.c:53`）、`gpu-*`・`nogpu`（`vm/virtio_gpu.c`、VM のみ）、`e1000-itr-us`（`vm/e1000.c:185`、VM のみ）。多くは診断の旗で窓は要らない。窓が要るのは `dns-server`、`tcp-*`、休止の可否である。重さは網の二つが壊、ほかはき。

**環境の変数でしか変えられない**

| 変数 | 場所 | 届くか |
|---|---|---|
| `NVKMS_OPTIONS` | `nvkms_os.c:158` | 届かない。init は設定なしで `nvrm` を起動する（`init.c:142-145`）。窓も文書も無い。製品の状態で変える道が無い |
| `NVRM_REGISTRY_DWORDS` | `nvrm.c:270` | 同じ |
| `NVreg_*` | `os.c:1085` | 同じ |
| `FONT_DIR` | `font.c:163` | シェルからだけ |

**CLI だけ**

- `agent.conf`（`agent.c:13-16`）：窓が無い。AGENTS.md の GUI が先に反する。アプリの持ち物なので置き場所は正しい。
- `browser --debug-js`、`--log-console`：診断の旗なので窓は要らない（`.claude/rules/browser.md`）。
- `volume`（`user/apps/volume.c`）：窓もある。

**決め打ちで、窓にも旗にも無い**

`wm.c:53`（タスクバーの高さ）、`:55`（ポインター）、`:486`（ダブルクリック）、`:2606-2607`（加速）、`usbhid.c:18-19`（繰返し）、`display_abi.h:67`（試行の 15 秒）、`browser.c:20`、`:1079`（検索の先）、`audio.c:68`（既定の音量）、`pm.c:33-36`（待ち時間）。

## 9. 探した範囲と限界

- 無いと書いたものは `rg` で引いた。語は次のとおり：VRR（`N_SCREEN_MODE_VRR`、`vrr_min_mhz`、`vrr` を全木、`ports/nvidia`）、電源（`dpms`、`screen_off`、`screensaver`、`lock_screen`、`brightness`、`backlight`、`PNP0C0D`、`\blid\b`）、時刻（`clock_set_wall`、`clock_adjust_wall`、`ntp`、`sntp`）、網（`ipv6`、`wifi`、`wlan`、`802.11`、`bluetooth`）、音（`audio_set_card`）、通知（`wm_notify`、`n_notify`、`notification`）、C の口（`SCREEN_SETTINGS`、`screen_modes`、`desktop_settings` を `display.h`、`gui.c`、`nocturne.h`）、配列（`input.c`、`usbhid.c`）。
- 実行していない。静的な読みの結論である。「壊れ方」はコードの筋から言っていて、QEMU でも実機でも再現していない。
- 行数の数えはコメントを含み、共有の行を重ねて数えた区間がある（§3.2 に明記）。
- 公式の頁を引けなかったものが二つある。Windows の既定のアプリの手順の個別の頁は、分類の案内 [M5] だけを使った。出力の色の深さは [D3] に項目が無く、持ち主の出典を持たない。
- 物差しに使った Microsoft の VRR の記事（[M4]）は 1903 の時点の記事で、既定の値はその時点の話である。
- ほかの OS のソースコードは読んでいない。

## 10. 出典

- [M1] Windows registry for advanced users：https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users
- [M2] Predefined Keys：https://learn.microsoft.com/en-us/windows/win32/sysinfo/predefined-keys
- [M3] Navigating the Redesigned Graphics Settings page：https://devblogs.microsoft.com/directx/navigating-the-redesigned-graphics-settings-page/
- [M4] OS Variable Refresh Rate：https://devblogs.microsoft.com/directx/os-variable-refresh-rate/
- [M5] Understand and explore Windows Settings：https://support.microsoft.com/en-us/accessibility/windows/understand-and-explore-windows-settings
- [M6] Windows Backup Settings Catalog：https://support.microsoft.com/en-us/windows/experience/backup-recovery/windows-backup-settings-catalog
- [M7] Fix app audio not working while system sounds work：https://support.microsoft.com/en-us/windows/hardware/audio/fix-app-audio-not-working-while-system-sounds-work-in-windows
- [M8] Change your screen resolution and layout：https://support.microsoft.com/en-us/windows/hardware/display-graphics/change-your-screen-resolution-and-layout-in-windows
- [M9] Windows camera, microphone, and privacy：https://support.microsoft.com/en-us/windows/privacy/windows-camera-microphone-and-privacy
- [M10] Change display brightness and color：https://support.microsoft.com/en-gb/windows/hardware/display-graphics/change-display-brightness-and-color-in-windows
- [M11] HDR settings in Windows：https://support.microsoft.com/en-us/windows/hardware/display-graphics/hdr-settings-in-windows
- [M12] Make Windows easier to see：https://support.microsoft.com/en-us/accessibility/windows/make-windows-easier-to-see
- [M13] How to use multiple monitors：https://support.microsoft.com/en-us/windows/how-to-use-multiple-monitors-in-windows-329c6962-5a4d-b481-7baa-bec9671f728a
- [M14] Fix microphone problems：https://support.microsoft.com/en-gb/windows/hardware/drivers/fix-microphone-problems
- [M15] Change mouse settings：https://support.microsoft.com/en-us/windows/hardware/input-devices/change-mouse-settings
- [M17] Power settings in Windows 11：https://support.microsoft.com/en-us/windows/experience/power-battery/power-settings-in-windows-11
- [M19] Essential Network Settings and Tasks：https://support.microsoft.com/en-us/windows/experience/connectivity-networking/essential-network-settings-and-tasks-in-windows
- [M20] Use a proxy server：https://support.microsoft.com/en-us/windows/experience/connectivity-networking/use-a-proxy-server-in-windows
- [M21] Connect to a Wi-Fi network：https://support.microsoft.com/en-us/windows/experience/connectivity-networking/connect-to-a-wi-fi-network-in-windows
- [M22] Set time, date, and time zone settings：https://support.microsoft.com/en-us/windows/experience/personalization/set-time-date-and-time-zone-settings-in-windows
- [M23] Manage the language and keyboard/input layout settings：https://support.microsoft.com/en-us/windows/hardware/input-devices/manage-the-language-and-keyboard-input-layout-settings-in-windows
- [M24] Notifications and Do Not Disturb：https://support.microsoft.com/en-us/windows/experience/notifications-and-do-not-disturb-in-windows
- [M25] Windows Update Delivery Optimization and privacy：https://support.microsoft.com/en-us/windows/privacy/windows-update-delivery-optimization-and-privacy
- [D1] To set up G-SYNC：https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/nvdsp/To_use_variable_refresh_rates.htm
- [D2] How to Enable AMD FreeSync with AMD Software：https://www.amd.com/en/resources/support-articles/faqs/DH3-013.html
- [D3] NVIDIA Control Panel Quick Start Guide：https://us.download.nvidia.com/Windows/566.36/566.36-nvidia-control-panel-quick-start-guide.pdf
- [D4] Customize Graphics Settings with AMD Software：https://www.amd.com/en/resources/support-articles/faqs/DH3-012.html
- [A1] About the User Defaults System：https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/UserDefaults/AboutPreferenceDomains/AboutPreferenceDomains.html
- [A2] Change Lock Screen settings on Mac：https://support.apple.com/guide/mac-help/change-lock-screen-settings-on-mac-mh11784/mac
- [A3] Displays settings on Mac：https://support.apple.com/guide/mac-help/displays-settings-on-mac-mh40768/mac
- [A4] Change Accessibility settings on Mac：https://support.apple.com/guide/mac-help/change-accessibility-settings-mchlp1400/mac
- [A5] Change Users & Groups Login Items preferences on Mac：https://support.apple.com/en-bh/guide/mac-help/mtusr003/10.15/mac/10.15
- [A6] Lock the screen of your Mac：https://support.apple.com/guide/mac-help/lock-the-screen-of-your-mac-mchl8e8b6a34/mac
