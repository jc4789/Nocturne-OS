# 設定の層の設計

## 依頼文（そのまま）

````
Nocturne OS（独自の OS。カーネルと C ライブラリーは C。GUI が先。対象は実機。今の PC が基準で、特定の機械に決め打ちしない）の、OS 全体を通す設定の層を設計してください。この仕事は設計の文書までです。製品のコードはまだ書かないでください。設計の後に別の監査役が敵対的に監査し、その後で実装を別に頼みます。返事と文書とコミットメッセージは日本語で書いてください。

ユーザーの言葉：
- "VRR and gsync are settings, one is a os setting one is driver setting. This is something that is an overall issue not just a issue with vrr. we apply overship haphazardly to things, with out finding out what is normal. and we also have like verylittle real support for settings, overall."
- "this is not just for vrr or display stuff. we lack a proper settings system. and stuff like this shouldn't be too hardcoded. we should be carefull, and maybe do an adversarial audit first of what we are missing."
- "WE AREMISSING ABSTRACTION THAT NEEDS TO BE THERE. WE HAVE NO PROPER SETTINS SYSTEM ACROSS THE"
- "linux unix or poisx, should not be treated as the standard or as inherently good architecture."
- 規模の例："600hz moniters, 3 moniters, game, youtube and discord open with no droped frames or stuttering cuased by the kernel its self"

材料（`docs/records/kernel-core-20261101/` の下）：
- `audit-settings-missing.md`：何が欠けているかの監査。
- `settings-ownership.md`：設定ごとの持ち主の表。別の者が作ったので、照らして直してかまいません。
- `audit-hardcoded.md`：決め打ちの監査（層と行の一覧）。
- `settings-layer-design.md`：前の草稿。
- `audit-settings-design.md`：その草稿への敵対的な監査。53 指摘で、前の草稿は作り直しになりました。この監査の指摘の全部に、新しい設計がどう答えるかを書いてください。答えない指摘には理由を書いてください。指摘そのものが誤りと判断したら、コードと文書を引いてそう書いてください。
- 規則：`AGENTS.md`（「製品のコード」の「GUI が先」「shim は禁止」「スパゲッティは禁止」「UNIX・POSIX・LINUX ではない」）、`.claude/rules/map.md`、`docs/nocturne-native-abi.md`、`common/abi.h`。今のコードを読んで事実を確かめてください（`user/apps/settings.ts`、`kernel/src/sys/keep.c`、`user/apps/init.c`、持ち主になる各部品）。

普通は何かを調べること：ユーザーは「普通は何かを調べずに持ち主を決める」ことを問題にしています。持ち主を決める前に、公開の公式の文書（Microsoft、Apple、GPU や音の装置の作り手の文書など）と Web 検索で、その設定が普通どこに属するかを確かめ、出典を書いてください。どの OS のソースコードも読まないでください。一つの OS の形を写さず、Nocturne の形で決めてください。

設計に書くこと：責務と持ち主、部品が設定を登録し・読み・変更を受ける形、窓から全部に届く道、永続と起動の順、複数の画面・GPU・利用者・プログラムの身元、ABI の形、数が十倍になったときに詰まる所、失敗と回復の道、作る順。VRR と G-SYNC のような例だけでなく、`audit-hardcoded.md` の全部の層がこの設計の上でどこへ行くかの表を付けてください。

出し方：作業木の枝に `docs/records/kernel-core-20261101/settings-layer-design-2.md` として書き、コミットしてください（末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。push と main への merge はしないでください。依頼文も文書の先頭に写してください。ビルドと QEMU は要りません。最後のメッセージには、枝の名、コミット、決めきれなかったものを短く書いてください。
````

途中で調整役から届いたユーザーの言葉（そのまま）：

````
"You can download the docs you need yourself. You dont have to ask me to do it every time." 設計に要る文書は、作り手の公式の場所から `D:\Programes\OS DEV INFO` へ落としてかまいません。公式の場所に無いものは取らず、そう書いてください。
````

この設計のために落とした文書は 0 個である。使った公式の文書は Web で読んだ。読めなかったものは §22 に書く。

この文書の番号の読み方は次のとおりである。

- `監査 F1`〜`監査 F45`、`監査 G1`〜`監査 G8` は `audit-settings-design.md` の指摘である。
- `決G1`、`決F1` のように「決」が付く番号は `audit-hardcoded.md` の行である。
- `L0`〜`L14` は `audit-hardcoded.md` の層である。
- `[O1]` などの角括弧は出典である。§23 に全部を書く。

## 0. 結論

1. 設定の層は七つの部品で作る。`sys/setting.c`（仲立ち）、`common/setting.c`（型・符号・解決）、ABI の `SYS_SETTING_*`、libc の `n_setting_*`、JS の `nocturne/settings`、窓の `settings`、身元を作る `proc_launch` である。
2. 値を効かせる者とその家族が持ち主である。持ち主は宣言を公開し、値の正しさを判じ、値を効かせる。
3. 仲立ちは宣言と値の写しを持ち、読みに答え、頼みを運び、試行を数え、文書へ保存する。読みは持ち主を決して呼ばない。
4. 保存は全部を仲立ちが行う。持ち主は保存しない。これで driver の死、電源の前の取りこぼし、flip の道の遅れが一度に消える。
5. 領域を持てるのは、カーネルの部品と、属性 `--settings-owner` を持つプログラムだけである。プログラムの設定はプログラムの持ち物で、カーネルを通らない。
6. scope は「持ち手」と「対象」の二つの軸である。持ち手は `forced`・利用者・機械である。対象は なし・実体・`program`・`program+実体` である。解決は置き換えだけで、順は `common/setting.c` の表に一度だけ書く。
7. 実体は開いた語で、公開した持ち主のものである。モニターの組、モニターの型、接続先、AC・DC も同じ形で書く。
8. 表示は `display/arrangement @ monitor-set:<hash>` の一つの鍵である。全部のモニターのモード、位置、回転、主、オンとオフを一度に試し、一度に当てる。
9. 試行の値は保存しない。保存の文書は確定の値だけを持つ。電源が落ちても映らないモードで起動しない。
10. VRR は OS の値と driver の値を分ける。`nvrm` は `compositor` の値を読まない。合成は `compositor` の値と窓の求めから「VRR を求める」を driver へ渡し、driver の申告（今 VRR で動いているか）を読んで歩調を決める。
11. 権限はフォルダーでなく文書の属性で決める。窓は `js` でなく専用の ELF `settings` である。
12. 機械の文書は `C:\Nocturne\Settings\<領域>.conf`、利用者の文書は `C:\Users\<名>\Settings\<領域>.conf` である。どちらも `STORE_SYSTEM | STORE_PRIVATE` のフォルダーで、読み書きするのはカーネルだけである。
13. 一コマの道に層は入らない。効かせる者は値を自分の変数に持ち、合成と present はその変数だけを読む。
14. 監査の 53 指摘の全部に §19 で答える。誤りと判断した指摘は 0 個である。直す案を換えた指摘は §19 の表に理由を書く。
15. `audit-hardcoded.md` の 91 行の全部の行き先を §15 に書く。

持ち主に見せて決めることを §21 に書く。カーネルに新しい部品を置くので、実装の前に見せる。

## 1. 普通は何か

持ち主を決める前に公式の文書を読んだ。普通は物差しであり、写す形ではない。読んだ結果を、問いごとに一行で書く。

| 問い | 公式の文書が言うこと | Nocturne の決定 |
|---|---|---|
| driver の設定は誰のものか | 装置の状態は driver が持ち、他の部品が変えるなら driver が口を公開する。設定は装置のモードとソフトとの関わりに分かれる。既定は読むだけの場所に置き、上書きを別に置いて後ろへ落ちて読む（[DPI]）。 | `nvrm` の設定は `nvrm` の領域である。OS の領域に混ぜない。 |
| driver の窓は誰のものか | driver に付く UI は driver の本体と別のアプリに分ける（[DCH]）。 | 窓は `settings` 一つで、`nvrm` は窓を持たない。`section = driver` の頁を宣言から作る。 |
| OS の VRR と driver の VRR | OS の切り替えの既定は切で、VRR を持たない全画面のゲームへ足す。G-SYNC と FreeSync を置き換えず、control panel の設定を上書きしない（[VRR]）。G-SYNC の入り切り、全画面か窓も含むか、同じ型の全部の画面へ当てるかは driver の頁にある（[NV]）。検証済みのモニターは driver の導入で自動で入る（`settings-ownership.md` [N5]）。 | 別の領域の別の鍵にする。§13 の表で決める。 |
| 値の軸はいくつか | Apple の preferences は application・user・host の組の 8 個の領域を決まった順に探す（[CF]）。NVIDIA の driver は application の profile が全体の profile に勝つ（`settings-ownership.md` [N3]）。 | 一本の順にせず、持ち手と対象の二軸にする（§3.5）。 |
| 強制はどこに置くか | 順は政策、配備、service、利用者、既定である。政策の値を利用者の値へ写さない。効く値を出して操作を止める（[POL]）。Apple は値が管理者のものかを問う口を持つ（[O8]）。 | 持ち手 `forced` を解決の先頭に置く。 |
| 表示は一つずつか一度にか | 全部の path を一度に渡し、当てずに試す旗と、保存しない一時の適用を持つ（[SDC]）。別の口は溜めて一度に当て、試す旗を持ち、再起動の要否を結果で返す（[O5]）。Apple の表示の構成の取引は、アプリの間、セッションの間、永続の三つの範囲を持つ（[CG]）。 | 一つの鍵、`N_SETTING_TEST`、`N_SETTING_TRY`、答えの `RESTART` にする。 |
| 構成はどう引くか | 保存は今繋がっているモニターの組で引く（[SDC]）。 | `monitor-set:<hash>` を実体にする。 |
| 主画面と配置 | (0,0) の面が主画面で、配置に隙間と重なりを許さない（[SRC]）。 | `arrangement` の検証は持ち主が行い、同じ規則を使う。 |
| モニターの身元 | maker、product、adapter の中の接続の番号で区別する。serial の欄は無い（[TDN]）。 | serial が有効なら serial を、無効なら接続の番号を使う（§11.1）。 |
| 表示の値は機械か利用者か | 同じ表示の変更を、全利用者か利用者かで選ぶ旗がある（[O5]）。 | ログインの前に要るので機械の値にする。拡大は好みなので利用者の値にする。 |
| プログラムの身元 | アプリが明示するかシステムが推定する。host process は中身ごとに別の身元を持てる。連なったプロセスは同じ身元を持つ。長さに上限があり空白を持たない（[AUMID]）。 | 既定はシステムが folder id から作る。家族は起動の旗で同じ身元を継ぐ（§11.2）。 |
| 音量 | 出力の session は一つの出口に結ぶ。音量は再起動をまたいで残る。全体の mix に入る。普通のアプリは他のアプリの session を制御せず、利用者が操作盤で変える（[AS]）。 | 出口の音量とアプリの音量を別の鍵にする。アプリの鍵は対象が `program+出口` である。書き手は `self` と窓である。 |
| 変化の知らせ | 一度の変化を event で知らせ、値の名を運ばない（[RN]）。全体への知らせには時間切れつきの送り方があり、応答しない相手を待たない旗がある。全部へ送ると待ちが相手の数に掛かる（[SMT]、[O1]）。 | 見張りは領域ごとで、輪に鍵と実体を持つ。読みに待ちが無い。変更は呼び手の締切で切る。 |
| 網 | DNS は口ごとである（[DNS]）。無線の profile は 口 + 名 で引く。利用者の profile と全利用者の profile がある。政策の profile は読むだけである。平文の鍵は既定で管理者だけが読める（[WLAN]）。 | `nic:<hash>+ssid:<hash>` を実体にする。秘密は値にしない（§12.3）。 |
| 電源 | 電源ボタンと蓋の設定は電源の設定の一つの群である（[PB]）。値は AC と DC に分かれる（`settings-ownership.md` [W18]）。 | `power` の領域に置く。AC と DC は実体である。 |
| 起動の項目 | 起動の区画の `limine.conf` に `timeout` と `default_entry` がある（[LIM]）。 | 値は `limine.conf` の中にあるので `storage = owner` にする。書く部品ができるまで登録しない。 |
| プログラムの設定 | プログラムは自分の data に保存する（`settings-ownership.md` [W26]、[P1]）。 | 層の外である。`n_app_data_folder` のまま変えない。 |

Nocturne が普通と違う形を選ぶ所は次の三つである。

- **木の中央の倉庫を作らない。** registry のように全部の値が一つの木に並ぶ形にしない。仲立ちが持つのは、システムの部品と driver が登録した領域だけである。
- **OS の VRR を画面ごとにする。** [VRR] の切り替えは一つである。歩調を持つのは画面ごとの合成の thread で、規模の例は周波数と VRR の違う 3 枚の画面である。
- **保存の全部を仲立ちが行う。** [DPI] は driver の状態を driver の持ち物とする。Nocturne でも値の意味と効き目は driver の持ち物である。文書の書込みだけを仲立ちが代わる。理由は、driver の死と flip の遅れから値を切り離すためである（§4.6）。

## 2. 責務と持ち主

### 2.1 六つの役

| 役 | 誰 | 責務 | 持たないもの |
|---|---|---|---|
| 持ち主 | カーネルの部品、`nvrm` などの driver、系のサービス | 宣言の公開、値の正しさの判断、効かせること、状態（選択肢、接続の有無、断りの理由）の公開 | 文書の道、保存、窓 |
| 読み手 | 同じ家族の別のプロセス（Mesa）、libc の `localtime`、`ui.c`、ブラウザー | 効く値を読み、自分の変数へ入れ、効かせる | 書込み（窓の権限が無い限り） |
| 仲立ち | `kernel/src/sys/setting.c` | 領域の記録、宣言と値の写し、頼みの行列、見張りの輪、試行の記録、保存 | 値の意味、選択肢の知識 |
| 共通のコード | `common/setting.c`・`common/setting.h`・`common/devid.h` | 文法、型、正規形、符号、解決の順の表、文書の読み書き、実体の身元の作り方。カーネルと libc と host が組む | 状態 |
| 窓 | 専用の ELF `settings`（`user/apps/settings.ts`） | 宣言と状態から行を作る | 領域ごとの知識 |
| 申告の層 | `dev/fb`、`gui/screenmode.c`、`display_abi.h`、`gpu_abi.h`、`dev/audio.c` の能力の問い合わせ | 装置ができることを出す | 利用者の選択 |

設定は「選択」である。申告は「事実」である。申告を鍵にしない。L3、L4、L7 の多くは申告の ABI の仕事で、層はその結果を状態として窓へ出すだけである。

### 2.2 持ち主の決め方

新しい値の持ち主は、次の問いを順に当てて決める。

1. その値を読むのはそのプログラムだけか。そうなら層の外で、プログラムの持ち物である。
2. 装置の申告か、仕様の値か、メモリーと CPU の数から計算できる値か。そうなら鍵にしない。
3. 開発と試験の旗か。そうなら層に入れない。
4. 値を効かせるのは誰か。常駐の部品なら、それが持ち主である。
5. 効かせる者が各プロセスの中にいるなら、その家族の常駐の者が持ち主で、効かせる者は読み手である（Mesa と `nvrm`）。常駐の者がいなければ、値を配る小さなカーネルの部品が持ち主である（`sys/region.c`）。
6. 公式の文書を読み、普通の持ち主と違うなら理由を書く。

**効く道が通るまで鍵を登録しない。** 形で止めることはできないので、規則として書く。何もしない `apply` を書くことは禁止である。領域の試験が効き目を確かめる（§18）。

### 2.3 領域

領域は持ち主が公開する鍵の組で、一つの領域が窓の一つの頁である。カーネルの部品の領域は短い語である。プログラムの領域の名はその文書の名で、`nvrm` の領域は `nvrm` である（§12.2）。

| 領域 | 持ち主 | section | 登録の段 |
|---|---|---|---|
| `display` | `gui/screenmode.c` | system | 一段目 |
| `compositor` | `gui/wm.c` | system | 一段目 |
| `audio` | `dev/audio.c` | system | 一段目 |
| `nvrm` | `ports/nvidia/nvrm.c` | driver | 二段目 |
| `graphics` | カーネルの GPU の部品 `dev/gpu` | system | 二段目 |
| `input` | `dev/input.c` | system | 一段目は `pointer-speed` だけ、残りは三段目 |
| `power` | `dev/pm.c` | system | 三段目 |
| `time` | `sys/clock.c` | system | 三段目 |
| `region` | `sys/region.c`（新しい小さな部品） | system | 三段目 |
| `network` | `net/net.c` | system | 三段目 |
| `appearance` | `gui/appearance.c`（新しい部品） | system | 四段目 |
| `shell` | `gui/wm.c` | system | 四段目 |
| `session` | 一段目は `sys/setting.c`（`session/reload` だけ）、五段目から L12 の利用者とセッションの部品 | system | 一段目、五段目 |
| `boot` | ESP へ書く部品 | system | 書く部品ができてから |
| `amdrm` など次の driver | その driver | driver | driver が来てから |
| `settingtest` | 試験のプログラム | driver | 一段目（試験の媒体に限らない。効き目は自分の中だけである） |

一つのファイルが二つの領域を持つのは `gui/wm.c` だけである（窓に出す頁が別なので分ける）。`display` と `compositor` を分けるのは、効き目の錠が別で、順が `screenmode.c` → WM だからである（`gui/screenmode.c:21`）。

この表は説明であり、コードの表ではない。領域は登録で生まれる。全部の設定を並べた表はコードのどこにも無い。

### 2.4 なぜカーネルに置くか

三つの案を比べた。

| 案 | 判定 | 理由 |
|---|---|---|
| ユーザー空間のサービス | 採らない | `audio`、`display`、`input`、`network` の値は最初のプログラムの前に要る（今の `keep_start` の位置）。サービスより先に動くカーネルの部品が、サービスを待つことになる。呼び手の身元を確かめる口もカーネルにしか無い。 |
| 仲立ちなしで、窓が持ち主と直に話す | 採らない | 持ち主が固まると窓が固まる。持ち主の死で値が読めない。部品ごとに ABI が増える（今の `SYS_SCREEN_INFO` の副命令と `SYS_AUDIO_OUTPUT`）。 |
| カーネルの小さな仲立ち | 採る | カーネルの部品と driver を同じ形で扱う。持ち主の問いの "why are userland PROGRAMs, forced to talk to the kernel to save settings?" には、プログラムは通らないと答える。通るのはシステムの部品と driver だけである。 |

仲立ちはカーネルの中で方針を持たない。値の意味を知らず、選択肢を知らず、領域の名を知らない。

## 3. 値の形

全部を `common/setting.c` に一度だけ書く。カーネル、libc、`host/store` が同じコードを組む。

### 3.1 名前の文法

- 領域と鍵は `[a-z0-9-]+` で、64 byte までである。文法を外れた名は `N_ERR_INVALID` である。
- `..` も `\` も大文字も文法に無いので、領域名はそのまま文書の名に使える。C: の畳み込みでぶつからない。
- 実体は `<類>:<本体>` で、類は `[a-z0-9-]+` である。組み合わせは `+` で繋ぐ（`nic:…+ssid:…`）。
- プログラムの身元は §11.2 の形を §3.3 の符号にした文字列である。
- ABI では領域、鍵、プログラム、実体を別の欄で渡す。`領域/鍵 @ 対象` は文書の中の表記だけである。

### 3.2 型

| 型 | 正規形 | 宣言の制約 |
|---|---|---|
| `BOOL` | `on`・`off` | なし |
| `INT` | 10 進。`+` なし、先頭の 0 なし、`-0` は `0` | `min`、`max`、`step`、`special`（語と値の組） |
| `NUMBER` | 10 進の固定小数点。小数の桁は宣言の `digits` ちょうど | `INT` と同じ |
| `ENUM`（閉じた） | 語 | `options`（語と表示名） |
| `ENUM`（開いた） | 符号の文字列 | 形だけを共通のコードが見る。選択肢は実体ごとの状態、検証は持ち主 |
| `STRING` | 符号の文字列。空は `%-` | `max_bytes`、`pattern`（語：`hostname`、`ipv4`、`ipv6`、`uri`） |
| `COLOR` | `#rrggbb` の小文字 | なし |
| `TIME` | `HH:MM`（一日の分） | なし |
| `DOCUMENT` | `doc:<hash>` | `pattern`（文書の型の語） |
| `LIST` | 要素の正規形を `,` で繋ぐ。空の列は空の文字列 | 要素の型 |
| `RECORD` | `名=値` を名の順に `,` で繋ぐ。値は正規形 | `fields`（名と型）。無ければ持ち主の形で、形は持ち主が検証する |
| `ACTION` | 値なし、または `STRING`、`TIME` の引数 | 保存しない。`SET` でなく `N_SETTING_ACT` で呼ぶ |

`special` は範囲の外の語である。最大フレーム レートは `min=20, max=1000, special=[off]` で書く。画面を消す時間の `しない` も同じである。1〜19 は通らない。

`canonical(型, 入力)` は一つだけである。入口で必ず正規形へ直し、直せないものは `N_ERR_INVALID` で断る。比較は正規形でだけ行う。同じ値の当て直しは変化でない。

不正な UTF-8 は断る。NUL は `STRING` に許さない。値は長さつきで渡す。

### 3.3 符号

正規形は改行も空白も持たない。値をそのまま行に書ける。

`STRING` と実体とプログラムの身元は次の符号で書く。

- `0x21`〜`0x7E` のうち `%`、`=`、`#`、`@`、`,`、`+`、`:`、`!` 以外はそのまま書く。
- `0x80` 以上は正しい UTF-8 ならそのまま書く。
- ほかは `%XX` である。空の文字列は `%-` である。

`,`、`+`、`:` を符号に入れる理由は、`LIST` の区切りと実体の区切りに使うからである。`RECORD` の入れ子は値の中の符号で表す。

装置が決める文字列（EDID、USB の serial）は身元に入れない。身元は hash である（§11.1）。表示名は状態の欄で、この符号を通る。悪意のある USB 機器は行を差し込めない。

### 3.4 文書の形

一つの領域、一つの持ち手に一つの文書である。一行は次の形である。

```
<対象> <鍵>=<値>
! <対象> <鍵>=<値>
```

- 対象は `-`（なし）、`<実体>`、`program:<身元>`、`program:<身元>+<実体>` である。
- 先頭の `!` は `forced` の印で、機械の文書にだけ意味がある。
- `#` で始まる行は書き手が読まない。
- 行の切り方は固定である。最初の空白で対象を切り、次の `=` で鍵と値を切る。

例：

```
audio-output:9f3a01c2d4e5b607 volume=80
program:programs/Media%20Player/mediaplayer+audio-output:9f3a01c2d4e5b607 app-volume=40
```

持ち主が宣言した後の保存で、宣言に無い鍵の行は消す。持ち主がいない領域の文書は書き換えない。互換は持たない。

### 3.5 scope

scope は持ち手と対象の組である。

- **持ち手**：`forced`（機械の文書の印）、`user`（今の利用者）、`machine`。
- **対象**：なし、`<実体>`、`program:<身元>`、`program:<身元>+<実体>`。

宣言は鍵が受ける組の一覧を持つ。一覧に無い組の `SET` は `N_ERR_INVALID` である。

実体は親を一つ持てる。親は持ち主が状態で公開する。親は既に公開された実体でなければならないので、循環が起きない。例は次のとおりである。

| 実体 | 親 | 意味 |
|---|---|---|
| `monitor:<hash>` | `monitor-model:<hash>` | 同じ型の全部の画面へ当てる値（[NV]） |
| `nic:<hash>+ssid:<hash>` | `nic:<hash>` | 接続先の値が無ければ口の値（[WLAN]、[DNS]） |
| `power:ac` | なし | AC の値 |

**解決の順は次の一つである。** 実体の鎖を `E0`（当の実体）、`E1`（親）… とする。

1. `forced` の中を下の順で探す。
2. `user` の中を下の順で探す。
3. `machine` の中を下の順で探す。
4. 実体ごとの既定（持ち主が状態で出す。モニターの推奨のモードなど）。
5. 宣言の既定。

持ち手の中の順は、`program+E0`、`program+E1`…、`program`、`E0`、`E1`…、なし、である。

ログインの前は今の利用者がいないので、段 2 を飛ばす。

### 3.6 置き換えだけを行う

解決器は置き換えだけを行う。掛け合わせ、論理積、最小は持ち主の計算である。その場合は別の鍵に分ける。

| 値 | 鍵 | 持ち主の計算 |
|---|---|---|
| 音量 | `audio/volume @ 出口`、`audio/app-volume @ program+出口` | 掛け合わせる。全体の消音はアプリの値に負けない（[AS]） |
| VRR | `compositor/variable-refresh @ monitor`、`compositor/variable-refresh-app @ program` | `app` が `off` なら求めない。`app` の `follow` は画面の値に従う |
| フレームの上限 | `nvrm/frame-limit @ program`、`compositor/frame-limit @ monitor` | 小さい方 |
| 能力 | 鍵にしない | 申告の無い画面に `on` を書いても効かない。行を出さない |

一段目の試験に「同じ鍵の複数の scope は置き換えとして意味を持つか」の点検の表を置く（§18）。§14 の全部の鍵がこの点検を受ける。

### 3.7 宣言の形

宣言は `common/setting.h` の構造体で、ABI では文書の形（§3.4 と同じ符号）で渡す。

領域の欄は次のとおりである。

| 欄 | 意味 |
|---|---|
| `section` | `system`・`driver` |
| `order` | section の中の並びの数 |
| `label` | 文字の id と既定の文字の組 |
| `icon` | `C:\Nocturne\Icons` の下の文書の名 |

鍵の欄は次のとおりである。

| 欄 | 意味 |
|---|---|
| `name`、`type`、型の制約 | §3.2 |
| `scopes` | 受ける持ち手と対象の形の組の一覧 |
| `write_default` | 窓が既定で書く組（§8.3） |
| `default` | 宣言の既定 |
| `writer` | `settings-writer`・`self`・`any`（§12.1） |
| `reader` | `any`・`settings-writer`（§12.3） |
| `storage` | `layer`（仲立ちが保存する）・`owner`（値は別の所にある：画面の中、RTC、`limine.conf`） |
| `access` | `read-write`・`read`（読むだけの値。窓の行と見張りを持つ） |
| `effect` | `now`・`owner-restart`・`machine-restart`・`trial` |
| `trial_ms` | `effect = trial` の期間 |
| `widget` | 部品の語（§8.2） |
| `group`、`order` | 頁の中の群と並び |
| `label`、`help`、`search` | 文字の id と既定の文字の組 |
| `unit`、`digits` | 単位の語（`hz`、`percent`、`ms`、`s`、`px`）と表示の桁。窓が書式を作る |

状態の文書は、宣言と別に持ち主がいつでも公開し直す。

| 欄 | 意味 |
|---|---|
| 実体ごとの `name`、`icon`、`present`、`parent`、`default.<鍵>` | 表示名、接続の有無、親、既定 |
| 鍵と実体ごとの `options` | 語と表示名の組。開いた `ENUM` の選択肢 |
| 鍵と実体ごとの `usable`、`reason` | 使えるか、断りの理由。`reason` は文字の id と既定の文字の組 |

## 4. 仲立ち `sys/setting.c`

### 4.1 持つもの

領域の記録は名前で持ち、持ち主の有無と独立である。

```c
struct setting_domain {
    /* 名、宣言の写し、状態の写し。持ち主がいない間も残る */
    /* owners：持ち主の一覧。先頭が全体の鍵に答える（§5.4） */
    /* keys、targets：鍵と対象を番号に引く表。対象の番号は領域の中で生存する間だけ同じ */
    /* values：(持ち手, 鍵, 対象) から値への表。値は不変で参照を数える */
    /* overlays：(鍵, 対象) から試行と保留の記録への表 */
    /* requests：頼みの行列 */
    /* ring：固定の大きさの変化の記録の輪と、通し番号 */
    /* doc[2]：持ち手ごとの文書の状態（読めた、書込みの印、次の書込みの時刻） */
    /* lock：葉の spinlock（arch/spin.h）。この下で持ち主を呼ばない。順は 持ち主の錠 → lock */
};
```

大きさの規則は次のとおりである。

- 値の数、実体の数、領域の数に固定の上限が無い。全部が伸びる表である。
- 宣言と状態の bytes は持ち主のプロセスに課金する（`struct process.messages` と同じ勘定、空きメモリーの半分）。カーネルの持ち主は課金しない。
- 頼みの bytes は呼び手のプロセスに課金する。
- 値は、持ち主が受けたものだけが残る（§4.3）。実体を持つ値は、持ち主が断れば残らない。
- 文書は実際の大きさで読む。`READ_MOST` の 64 KiB は無くなる。

### 4.2 読む道

`SYS_SETTING_GET` は写しだけに答える。持ち主を決して呼ばない。

1. 要求の構造体と名前を一度だけカーネルへ写す。
2. `lock` を取り、鍵と対象を引き、`mode` に従って値を選び、参照を増やし、`lock` を放す。
3. 値、世代、出所、印を呼び手へ写し、参照を減らす。

`mode` は二つである。`placed` はその組に置かれた値だけを答える。`effective` は §3.5 の順で解いた値と出所を答える。`effective` で `program` を省くと、呼び手の身元を使う。

読みの費用は syscall と葉の錠と表の引きである。眠らない。`copy_to_user` の間は錠を持たない。読みは一コマの道に置かない（§5.5）。

持ち主がいない領域でも読める。宣言がまだ無い領域は、文書の値だけを `mode = placed` で答え、`effective` は `N_ERR_NOT_READY` である。Mesa のような読み手は、自分のコードの初期値で動き、見張りで後から受ける。

### 4.3 変える道

`SYS_SETTING_CHANGE` は要求を受け、頼みの記録を領域の行列へ入れ、自分の締切まで眠る。チャンネルを使わない。

```
呼び手（窓の変更の thread）
  → SYS_SETTING_CHANGE（要求の構造体。締切、if_generation）
  → 権限、文法、canonical、閉じた型の検証、TRIAL の規則
  → 頼みの記録を requests へ入れる（番号つき。呼び手へ課金）。呼び手は締切まで眠る
  → 持ち主の受け口が受ける
       カーネルの持ち主：領域の worker の kthread が apply を呼ぶ
       アプリの持ち主：libc の thread が SYS_SETTING_TAKE で受け、apply を呼び、SYS_SETTING_ANSWER で答える
  → 答えの三つ（取った・断った・受け付けた）
       取った：値の写しを替え、世代を増やし、輪へ記録し、書込みの印を付ける
       断った：何も変えない。呼び手へ N_ERR_REFUSED と理由
       受け付けた：overlay に保留として置く（§9）。呼び手へ answer = ACCEPTED
  → 呼び手を起こす
```

カーネルの持ち主の `apply` は領域の worker の上で走る。呼び手の錠の下で走らない。呼び手の締切は worker にも効く。カーネルの呼び手も同じ記録で待てる。

`RESET` は組の値を消す。層は解き直した効く値で `apply` を呼び、取ったら行を消す。

置く値が効く値にならない場合がある（利用者の値があるときに機械の値を置くなど）。この場合、層は置く値を `effective` の欄に入れて `why = TEST` で問い、取れば置く。効き目は変わらない。`TEST` を受けない鍵は、共通のコードの検証だけで置く。

規則は次のとおりである。

- 締切は要求の欄である。libc の既定は今から 1 秒である。時間切れは `N_ERR_TIMEOUT` で、頼みは行列から消える。持ち主が遅れて答えても捨てる。
- 受け取り口の thread（TAKE した頼みを持つ thread、カーネルの worker）からの同じ領域への `CHANGE` は `N_ERR_BUSY` で断る。読みは許す。
- 別の領域への `CHANGE` は締切で切れるので、互いに待っても止まらない。
- 持ち主がいない領域と、`present` でない実体への `CHANGE` は `N_ERR_NOT_READY` である。値の正しさを判じる者がいないので、置かない。窓は行に「応答なし」か「繋がっていない」を出す。
- 例外は `present` でない実体の組の `RESET` である。持ち主を呼ばずに行を消す。窓が古い実体を忘れる道である。
- `if_generation` が 0 でなく今の世代と違えば `N_ERR_CHANGED` である。`LIST` と `RECORD` の競りはこれで解く。

`wm_lock` の下から `setting_set` を呼ぶことは禁止である。`wm_lock` の下で値を変える者は、`setting_post`（行列へ入れて戻る。答えを待たない）を使うか、持ち主の直の関数を呼ぶ（§5.3）。

### 4.4 見張り

設定の見張りは種類 `N_KIND_WATCH` で、自分の `object_ops` を持つ。取り出しは `SYS_SETTING_CHANGES` だけである。`SYS_WATCH_TAKE` は `N_ERR_WRONG_KIND` である。

- `SYS_SETTING_WATCH(領域)` は、領域がまだ無くても名前で記録を作って開く。Mesa が `nvrm` より先に起きても見張れる。記録は宣言も文書も見張りも無くなれば消える。空の記録の課金は開いたプロセスである。
- `SYS_SETTING_WATCH(NULL)` は領域の一覧の変化だけを言う。
- handle の状態は通し番号の読んだ位置である。`N_EVENT_RECEIVABLE` は読んだ位置が輪の先頭より後ろのときである。
- 輪の記録は固定の大きさである：通し番号、種類、鍵の番号、対象の番号、世代。種類は `value`（値が変わった）、`state`（状態が変わった）、`owner`（持ち主が来た、去った）、`declaration`（宣言が替わった。鍵の番号が変わる）、`trial`（試行が始まった、終わった）である。
- 同じ鍵と対象の続く変化は、輪の中で一つに畳む。タスクバーの音量の引きが輪を溢れさせない。
- 輪の大きさは領域の鍵の数と実体の数から決め、伸びる。読み手が遅れて輪から落ちたら `lost` の記録を一つ受け、`DESCRIBE` で全部を読み直す。
- 持ち主の死は `watched_gone` を使わない。`owner` の記録を一つ出すだけである。見張りは生きていて、`nvrm` が戻ったら鳴る。

`watched_changed` と同じく、輪への記録は眠らず、どの文脈からも呼べる。

### 4.5 三つの thread の種類

| thread | 数 | 仕事 | 待つもの |
|---|---|---|---|
| `setting` | 一本 | `setting_note` の処理、試行の期限、頼みの作成 | 持ち主を待たない |
| `setting-save` | 一本 | 文書の書込み（`store_save` の `N_COMMIT_WAIT`） | C: |
| 領域の worker | カーネルの領域ごとに一本 | `apply` の呼出し | 持ち主の錠 |

アプリの持ち主の受け口は libc の thread が一本である。

`setting` の thread は持ち主を呼ばない例外が一つある。`setting_note` の処理で持ち主の `read` を呼ぶ（§5.3）。`read` は自分の小さな錠を短く取るだけで、眠らない。これは契約に書く。

試行の期限は `setting` の thread が頼みを行列へ入れて次へ進む。固まった持ち主があっても、ほかの領域の試行は戻る。

### 4.6 保存を仲立ちが行う理由

仲立ちは値の写しを既に持つので、文書を書ける。これで次のことが同時に解ける。

| 問題 | 解け方 |
|---|---|
| `nvrm` の変更が `stop_apps` で失われる | 仲立ちが持つので失われない。`stop_apps` の前の頼みが要らない |
| `store_save` の commit の待ちが driver の thread を止める | driver は保存しない |
| `nvrm` は `C:\Nocturne\Settings` に書けない | 書くのはカーネルだけである |
| 領域名が道になる | 文法を通った名だけである。呼び手は道を渡さない |
| 利用者の文書の場所を起動元が渡せる | 仲立ちは今の利用者のフォルダーをカーネルから得る。`USERFOLDER` を読まない |

保存の規則は次のとおりである。

- 値が変わったら、領域と持ち手ごとに 0.5 秒後に `setting-save` が書く。
- 続けて変わるなら遅れを 30 秒まで倍々に伸ばす。SSD に書き続けない。止まれば 0.5 秒へ戻る。
- 書くのは確定の値だけである。overlay は書かない。
- `store_save` は REPLACE と `N_COMMIT_WAIT` で書くので、文書は古いか新しいかのどちらかである。
- `pm.c` は今 `keep_flush` を呼ぶ所で `setting_flush` を呼ぶ。遅れを待たずに全部を書く。
- 書込みに失敗したら印を残して次の機会に書き直す。窓は状態に「保存できない」を出し、toast を一度出す。

`storage = owner` の鍵は書かない。持ち主が値を公開し、写しだけを持つ。

## 5. 持ち主の契約

### 5.1 カーネルの持ち主

```c
struct setting_domain_desc {
    const char *name;
    const struct setting_decl *decl;    /* C の表。common/setting.h の形 */
    /* 値を効かせる。領域の worker の上で呼ぶ。眠ってよい */
    void (*apply)(const struct setting_apply *a, struct setting_answer *out);
    /* storage = owner と直の変更の値を読む。眠らない */
    int (*read)(unsigned key, const char *target, char *out, size_t cap);
};
struct setting_apply {
    unsigned key;
    const char *target;      /* 対象の文字列。無ければ "" */
    const char *effective;   /* 層が解いた効く値の正規形。どの段から来たかは問わない */
    enum setting_why why;    /* CHANGE、READBACK、TRY、REVERT、RELOAD、TEST、ACT */
};
struct setting_answer {
    enum setting_took took;  /* TOOK、REFUSED、ACCEPTED */
    const char *reason;      /* REFUSED・ACCEPTED の理由の id */
    bool restart;            /* 効くのは再起動の後：呼び手へ RESTART の印 */
};

struct setting_domain *setting_register(const struct setting_domain_desc *d); /* keep_register の位置で */
void setting_note(struct setting_domain *d, unsigned key, const char *target); /* 眠らない */
int  setting_resolve(struct setting_domain *d, unsigned key, const char *target, char *out, size_t cap); /* 眠らない */
void setting_settle(struct setting_domain *d, unsigned key, const char *target, enum setting_took took, const char *reason);
void setting_trial_end(struct setting_domain *d, unsigned key, const char *target, const char *reason);
void setting_entity_put(struct setting_domain *d, const struct setting_entity *e);
void setting_state_put(struct setting_domain *d, unsigned key, const char *target, const struct setting_state *s);
int  setting_post(struct setting_domain *d, unsigned key, const char *target, const char *value);
```

持ち主は `apply` だけを実装する。`apply` が受けるのは、いつも層が解いた効く値である。`RESET` の後も、狭い値を置いた後も、読み戻しも同じである。持ち主は scope を解かない。

`why` の使い方は次のとおりである。

| why | いつ | 持ち主がすること |
|---|---|---|
| `CHANGE` | 窓やアプリの変更、`RESET` | 検証して効かせる |
| `READBACK` | 起動の読み戻し、持ち主の登録の直後 | 効かせる。断ったら既定で動く。理由を状態へ出す |
| `TRY` | 試行の始まり | 効かせる。`ACCEPTED` を返してよい |
| `REVERT` | 試行の戻し | 確定の値へ戻す |
| `RELOAD` | 今の利用者が替わった | 効かせる |
| `TEST` | `N_SETTING_TEST` | 当てずに答える |
| `ACT` | `ACTION` の鍵 | 操作を行う |

`TEST` を受けられない鍵は宣言で `TEST` なしと書く。呼び手は `N_ERR_UNSUPPORTED` を受ける。

実体が増えたら持ち主は `setting_entity_put` で公開し、`setting_resolve` で値を問う。層から押さない。理由は、実体が来たときの順（モードの候補が揃ってから選ぶ）を持ち主が決めるからである。

### 5.2 効き目の規則

`apply` は次の規則を守る。

- flip の道、合成の道が持つ錠を長く持たない。表示へは値を渡すだけで、次の commit か次のコマで効かせる。
- 値を atomic の変数か、自分の錠の下の短い代入で入れる。
- 自分の領域へ `CHANGE` しない（`N_ERR_BUSY`）。
- 待つなら締切を持つ（`setting_apply` の締切の欄を受ける）。

この規則があるので、`display` の `apply` が `screenmode.c` の mutex を取っても WM の錠の順（`gui/screenmode.c:21`）に沿う。

### 5.3 直の変更

タスクバーの音量は `wm_lock` の下で `audio_set_volume` を呼ぶ。この道は残す。層を通さない理由は、`wm_lock` の下で頼みを待てないからである。

```
wm.c（wm_lock の下）
  → audio_set_volume（持ち主の直の関数。今の sound_put。眠らない）
  → setting_note(audio, KEY_VOLUME, 今の出口)（眠らない。印を立てて setting を起こす）
setting の thread（wm_lock を持たない）
  → audio.read(KEY_VOLUME, 出口) で値を得る
  → write_default の組の写しを替え、輪へ記録し、書込みの印を付ける
```

何十回の引きが印一つに畳まれる。`GET` が古い値を返す間は `setting` の thread が起きるまでである。

`note` が書く組は宣言の `write_default` である。直の変更を持つ鍵は、`write_default` のほかに狭い組を受けないと決める。音量の鍵は `user × 出口` だけを受ける。

### 5.4 アプリの持ち主

```c
struct n_setting_owner *n_setting_own(const struct n_setting_decl *decl,
                                      void (*apply)(const struct n_setting_apply *, struct n_setting_answer *, void *), void *arg);
void n_setting_changed(struct n_setting_owner *, const char *key, const char *target, const char *value);
void n_setting_settle(struct n_setting_owner *, const char *key, const char *target, int took, const char *reason);
void n_setting_trial_end(struct n_setting_owner *, const char *key, const char *target, const char *reason);
void n_setting_entity_put(struct n_setting_owner *, const struct n_setting_entity *);
void n_setting_state_put(struct n_setting_owner *, const char *key, const char *target, const struct n_setting_state *);
```

- `n_setting_own` は libc の中で thread を一本起こす。この thread だけが `TAKE` と `ANSWER` を回す。`nvrm_serve` の行列を通らないので、ゲームの RM の要求と混ざらない。
- 領域の名はプログラムの文書の名である（§12.2）。
- 同じ領域に複数の持ち主を持てる（GPU ごとの `nvrm`）。持ち主の登録は `OWN` の順に並ぶ。
- **実体は公開した持ち主のものである。** その実体の頼みはその持ち主へ行く。ほかの持ち主が同じ実体を `present` で公開していれば `N_ERR_EXISTS` である。モニターは GPU をまたいで動くので、接頭辞では持ち主を決められない。公開で決める。
- **対象に実体の無い鍵の頼みは、並びの先頭の持ち主が答える。** ほかの持ち主は同じ `apply` を `why = RELOAD` で受ける。答えを返さない。先頭が死ねば次が答える。保存は仲立ちなので、誰が先頭でも値は同じである。
- 持ち主の終わり（handle の close）で、`owners` から外し、公開した実体を `present = off` にし、待っている頼みへ `N_ERR_NOT_READY` を返す。`owner` の記録を輪へ出す。値は残る。
- 持ち主が登録すると、層は対象の無い鍵の効く値を `READBACK` で渡す。実体の鍵は、実体の公開の後に持ち主が `n_setting_get` で問う。

### 5.5 読み手と一コマの道

一コマの道（合成、入力、音の mix、present）で syscall を増やさない。読み手は次の形を使う。

```c
int n_setting_cache(const char *domain, const char *key, void *var, enum n_setting_cache_form form);
```

- `var` は呼び手の atomic の変数である。`form` は `BOOL`・`INT`・`NUMBER` を整数に、語を小さな番号に入れる。
- libc はプロセスごとに一本の thread を起こし、領域ごとの見張りを待ち、変わったら `GET`（`mode = effective`、対象は呼び手の身元）で読み、`var` へ入れる。
- `present` は変数だけを読む。
- 大きな値（`appearance/tokens`）は `n_setting_cache_blob` で不変の複製を原子に差し替える。
- 窓を持つプロセスは見張りの handle を自分の待ちに入れ、窓の事象の順で受けてよい。

カーネルの持ち主は自分の変数を持つ。`wm.c` の合成は `ws->vrr` のような変数だけを読む。

`nvrm` の領域の 3D の値の流れは次のとおりである。

```
窓 → SET nvrm/frame-limit @ user × program:… → 先頭の nvrm の apply（値の検証。取った）→ 写しと保存
ゲームの中の Mesa（nocturne_gl.c の初期化）→ n_setting_cache("nvrm", "frame-limit", &limit, …)
nvrm_scanout_present の前 → limit を atomic で読む
```

Mesa は効かせる者で、`nvrm` は持ち主である。Mesa は `nvrm` の死後も写しを読める。

## 6. ABI

### 6.1 呼出し

全部が `common/abi.h` と `docs/nocturne-native-abi.md` の §12 に同じコミットで入る。番号は 210 から続ける。203〜206 と種類 12 は使い直さない。

| 番号 | 名 | 引数 | 呼び手 |
|---|---|---|---|
| 210 | `SYS_SETTING_GET` | `(struct n_setting_request *)` | 誰でも（`reader` に従う） |
| 211 | `SYS_SETTING_DESCRIBE` | `(domain, n, buf, cap)` | 誰でも。`domain = NULL` は領域の一覧 |
| 212 | `SYS_SETTING_CHANGE` | `(struct n_setting_request *)` | `writer` に従う |
| 213 | `SYS_SETTING_WATCH` | `(domain, n)` | 誰でも。handle を返す |
| 214 | `SYS_SETTING_CHANGES` | `(watch, buf, cap)` | 見張りの持ち手 |
| 215 | `SYS_SETTING_OWN` | `(const void *declaration, n)` | 属性 `--settings-owner`。種類 `N_KIND_SETTING_OWNER` の handle を返す |
| 216 | `SYS_SETTING_TAKE` | `(owner, buf, cap)` | 持ち主。無ければ `N_ERR_NOT_READY` |
| 217 | `SYS_SETTING_ANSWER` | `(owner, const struct n_setting_answer *)` | 持ち主 |
| 218 | `SYS_SETTING_PUBLISH` | `(owner, const struct n_setting_publish *)` | 持ち主。値の変化、状態、実体、settle、trial_end |

`SYS_MAX` は 219 である。新しい結果は `N_ERR_CHANGED 34`（世代が合わない）と `N_ERR_REFUSED 35`（持ち主が断った）である。新しい種類は `N_KIND_SETTING_OWNER 14` である。

`GET` と `CHANGE` は引数が多いので要求の構造体を一つ受ける。syscall の引数は五つである（`docs/nocturne-native-abi.md` §0）。構造体は一度だけカーネルへ写す。

```c
struct n_setting_request {
    const char *domain, *key, *program, *entity, *value; /* 長さつき。NUL で終えない。無い欄は NULL */
    uint32_t domain_n, key_n, program_n, entity_n;
    uint64_t value_n;
    uint32_t holder;  /* N_SETTING_USER, N_SETTING_MACHINE。GET の placed は N_SETTING_FORCED も読める */
    uint32_t op;      /* CHANGE：N_SETTING_SET, RESET, CONFIRM, REVERT, ACT */
    uint32_t flags;   /* CHANGE：N_SETTING_TEST, N_SETTING_TRY */
    uint32_t mode;    /* GET：N_SETTING_PLACED, N_SETTING_EFFECTIVE */
    int32_t document; /* DOCUMENT の型：呼び手が開いた handle。ほかは -1 */
    uint32_t reserved;
    uint64_t if_generation; /* 0：問わない */
    int64_t deadline;       /* 既存の deadline と同じ形 */
    void *reply;
    uint64_t reply_cap;
};
struct n_setting_reply {
    uint64_t generation;
    int64_t trial_until;    /* 試行の期限（n_clock）。無ければ 0 */
    uint32_t answer;        /* N_SETTING_TOOK, ACCEPTED */
    uint32_t marks;         /* N_SETTING_MARK_TRIAL, PENDING, FORCED, RESTART */
    uint32_t source_holder;
    uint32_t value_n, source_program_n, source_entity_n, reason_n;
    uint32_t needed;        /* N_ERR_TOO_LARGE のとき要る大きさ */
    /* この後に、値、出所のプログラム、出所の実体、理由の id が並ぶ */
};
```

`program` を `effective` で省くと呼び手の身元である。`entity` を `effective` で省くと実体なしで解く。

### 6.2 消える口

`SYS_AUDIO_OUTPUT`、`SYS_AUDIO_OUTPUT_WATCH` は欠番になる。音量と消音は `audio/*` の鍵で、装置の名は状態の欄である。

`SYS_SCREEN_INFO` の副命令 `SCREEN_SETTINGS_*`、`SCREEN_MODE_*` は消える。`n_screen_watch` のうち設定の変化を言う部分は設定の見張りへ移る。画面の大きさの問い合わせは L3 の画面の ABI に残る。

`struct desktop_settings`、`SCREEN_MODES_FIRST_BYTES`、`sys/keep.c` の `older[]` は消える。互換を持たない。

### 6.3 POSIX でない理由

呼出しに fd も道の open も `ioctl` も無い。値の文字列の取得と変更は sysctl に字面が似る。ここの形は、持ち主の契約、宣言、試行、締切、世代、実体を持つ。名前を換えて足したものではない。

## 7. 言語の口

| 言語 | 口 |
|---|---|
| C | `n_setting_get`、`n_setting_set`、`n_setting_reset`、`n_setting_test`、`n_setting_try`、`n_setting_confirm`、`n_setting_revert`、`n_setting_act`、`n_setting_describe`、`n_setting_watch`、`n_setting_changes`、`n_setting_cache`、`n_setting_cache_blob`、§5.4 の持ち主の口。`user/libc/sys.c` と `user/include/nocturne.h` |
| JS、TS | `nocturne/settings` モジュール：`domains()`、`describe(領域)`、`get()`、`set()`、`reset()`、`test()`、`tryValue()`、`confirm()`、`revert()`、`act()`、`watch()`。変更の全部は Promise を返す。`user/libc/web/` でなく `user/apps/js.c` が host である |
| 型 | `rootfs/Nocturne/SDK/Types/nocturne/settings.d.ts` |

JS の主の thread は `CHANGE` で眠らない。`js.c` は窓ごとに変更の thread を一本持ち、結果を窓の待ち行列へ入れて Promise を解く。固まった持ち主の 1 秒の間も窓は動く。

`js_ui.js` の `desktop.{get,set,sound,setVolume,setMuted,modes,tryMode,keepMode,backMode,watch}` は消える。`js.c` の生の `__syscall` も消える。

libc の口は一つの呼ぶ側のための包みでない。呼ぶ側は `nvrm`、`volume`、Mesa、`ui.c`、ブラウザー、`settingtest`、JS の host である。

## 8. 窓

### 8.1 全部に届く道

```
settings の起動 → domains() → section と order で頁を並べる
頁を開く → describe(領域)（宣言、状態、値のある対象の一覧）→ 行を作る → get(effective) を行ごとに
watch(領域) → 変わった行だけを読み直す
操作 → set / tryValue → Promise の答えで行を更新
```

- `settings.ts` は領域の名を一つも知らない。例外は部品の語（§8.2）だけである。
- 頁を開くことは読みだけなので、持ち主が固まっていても開く。行は写しの値を出し、「応答なし」を添える。
- 繋がっていない実体は「繋がっていない」と出し、値を消せる（`RESET`）。消すと文書の行も消える。
- `program` の対象を受ける鍵は、値のあるプログラムの一覧と、文書の選択で道を選ぶ部品を出す。L10 の登録が来たら一覧の部品を換える。
- `system/js` の身元は「スクリプトは区別しない」と添える。
- プログラム自身の設定は層の外である。L10 の登録が来たら、窓の `section = app` の頁に登録されたプログラムを並べ、押すとそのプログラムの設定の窓を開く（`settings-ownership.md` [W8] の第三者の入口と同じ役）。

### 8.2 部品

`widget` の語は窓の部品の表である。知らない語は型の既定の部品で描く。型の既定は次のとおりである。

| 型 | 部品 |
|---|---|
| `BOOL` | 切り替え |
| `INT`、`NUMBER` | つまみか数の入力。`special` があれば選択を添える |
| `ENUM` | 選択（開いた型は状態の `options`） |
| `STRING` | 文字の入力 |
| `COLOR` | 色の選択 |
| `TIME` | 時刻の入力 |
| `DOCUMENT` | 文書の選択の窓 |
| `LIST` | 要素の一覧 |
| `RECORD` | `fields` があれば欄ごとに描く。無ければ読むだけの文字と「既定に戻す」 |
| `ACTION` | 押す部品 |

特別な部品は `arrangement`（配置の絵）、`mode-list`、`volume` である。driver が新しい部品を足せないのは正しい。既定の部品で必ず動く。

`icon` は領域の文書の画像である。`ui.d.ts` の `IconName` は閉じた語なので、`ui.c` の部品に文書の画像を受ける形を足す。

### 8.3 窓が書く先

`write_default` が既定の書き先である。規則は次のとおりである。

1. 効く値の出所が `user` か `machine` の組なら、その組へ書く。
2. 出所が既定なら `write_default` へ書く。
3. 出所が `forced` なら行を動かせない形で出し、理由を出す。`SET` は `N_ERR_DENIED` である。
4. 狭い scope の値がある間は、行に出所を出す（「このマウスの値」）。

### 8.4 窓を外から開く

`settings` の引数は `領域/鍵` か `領域` だけである。ABI の §12 に書く。ブラウザーやゲームは `SYS_GUI_LAUNCH` で開く。`settings` は同じ窓を前へ出す。

引数は頁を選ぶだけで、値を変えない。

### 8.5 表示の文字

`label`、`help`、`reason`、選択肢の表示名は「文字の id と既定の文字」の組である。一段目は既定の文字を使う。言語の仕組み（§15 の決T3）が来たら id で引く。単位は語と桁なので、窓が書式を作る。

## 9. 試行

試行の記録は仲立ちの overlay である。`screenmode.c` の `trial_on`、`trial_until`、`trial_from`、`trial_watch` は消える。

### 9.1 規則

- `effect = trial` の鍵は `N_SETTING_TRY` か読み戻しだけで変わる。旗なしの `SET` は `N_ERR_INVALID` である。
- `RESET` も `N_SETTING_TRY` を要る。推奨のモードが映らないモニターで戻せる。
- 試行は鍵と対象ごとに一つである。新しい試行は確定の値を引き継ぎ、期限を延ばす。今の `screenmode.c:493` の働きと同じである。
- 試行の値は写しに `trial` の印で見える。文書に書かない。電源が落ちれば消える。
- `CONFIRM` は、試行の値がまだ効いていて（持ち主が `settle` で取ったと言った）、呼び手が試行を始めたプロセスか `settings-writer` のときだけ通る。
- 期限は宣言の `trial_ms`、答えの `trial_until` である。窓は `n_clock` で残りを出し、読み直さない。
- 期限は `setting` の thread が数える。窓が死んでも戻る。

### 9.2 流れ

```
窓 → tryValue（TRY）→ overlay に置く。trial_until = 今 + trial_ms
   → 持ち主の apply(why = TRY)
       取った：印を trial にする
       受け付けた：印を pending にする。呼び手へ ACCEPTED
       断った：overlay を消す。呼び手へ N_ERR_REFUSED
持ち主 → setting_settle(took)：印を trial にし、期限を今 + trial_ms に延ばす（利用者が見る時間を modeset が食わない）
持ち主 → setting_settle(refused, reason) か setting_trial_end(reason)：overlay を消し、見張りを鳴らし、状態へ理由を出す
期限 → setting の thread：頼み REVERT を行列へ入れる。overlay を消す
窓 → confirm：overlay の値を確定の値にし、書込みの印を付ける
```

`trial_end` を持ち主が呼ぶのは、モニターが抜けた時、別のモニターが来た時、driver が出せないと言った時である（今の `screenmode.c:254`、`:273`、`:289`）。

持ち主が固まったまま期限が来ると、`REVERT` は行列で待つ。overlay は消えているので、読みは確定の値を返す。持ち主が戻れば `REVERT` を受ける。持ち主が死ねば再起動の `READBACK` が確定の値を渡す。

非同期に断られる鍵で `effect = trial` でないもの（`RESTART` のない色の設定など）も同じ overlay を保留として使う。期限は無い。`settle` が来るまで文書に書かない。

## 10. 永続と起動の順

### 10.1 場所

| 持ち手 | 文書 | フォルダーの属性 |
|---|---|---|
| 機械 | `C:\Nocturne\Settings\<領域>.conf` | `STORE_SYSTEM`、`STORE_PRIVATE` |
| 利用者 | `C:\Users\<名>\Settings\<領域>.conf` | `STORE_SYSTEM`、`STORE_PRIVATE` |

アプリは文書を直に読めない。読みは全部が `SYS_SETTING_GET` を通るので、`reader` が効く。

`store_start` は次の三つを変える。

- `C:\Users` を `STORE_SYSTEM` にする（`folder_at(STORE_ID_ROOT, "Users", STORE_SYSTEM, &users)`）。
- `user_ready` は利用者のフォルダーの直下に `Settings` を `STORE_SYSTEM | STORE_PRIVATE` で作る。
- `C:\Nocturne\Settings` を `STORE_SYSTEM | STORE_PRIVATE` で作る。

`store_start` は最初のプログラムより前に走るので、アプリが先に作る機会は無い。

`place_may_change` の働きは次のとおりである（`kernel/src/fs/store.c:655-659`）。

- 親が `STORE_SYSTEM` のものはアプリから変えられない。`C:\Users\<名>` は動かせない。`Settings` の中の文書は変えられない。
- 自分が `STORE_SYSTEM` のものはアプリから動かせない。`Settings` は動かせない。
- 親が `Users\<名>` なので `AppData\Nocturne` のような迂回の道が無い。

したがって新しい属性 `STORE_FIXED` も `enum store_caller` も要らない。カーネルは `user = false` で書く。

`C:\Nocturne` のうち fill が消すのは自分が入れる子だけにする。`scripts/layout.py` の `("C:\\Nocturne", "!", "")` を、`System`、`SDK`、`Licenses`、`Certificates`、`Firmware` と `rootfs/Nocturne` の各子へ当てる形に換える。`Settings` は fill の対象にしない。`tests/check_fill_keeps_settings.py` が host で「fill の後に `C:\Nocturne\Settings` の文書が残る」を確かめる。

`DOCUMENT` の値は、文書の中身を `Settings\<領域>\<hash>` へ写したものである。持ち主が `apply` で開いた handle を読み、検証して `n_setting_keep_document`（カーネルは `setting_keep_document`）で写す。値は `doc:<hash>` である。読み戻しは層の持つ写しを開く。カーネルの合成がアプリの渡した道を自分の権限で開くことは無い。どの行からも指されない写しは保存の時に消す。

### 10.2 起動の順

`kmain_late` の順は次のとおりである。太字が変わる所である。

```
entropy → vmalloc → acpi → pm → vm → pci → vm_display → fb_pm → gpu → iommu → usb
→ net_init（**装置の登録まで。DHCP の thread を起こさない**）→ audio_init → ahci → nvme → vm_storage
→ store_init → hibernate_restore → store_start（**Users と Settings の属性**）
→ wm_init → screen_modes_init
→ **setting_start**（登録済みの全部の領域の文書を読み、apply(READBACK)）
→ **net_start**（読み戻しの後に DHCP か静的な IP で開始）
→ vm_session → pm_sleep_check → init を起動
```

`setting_start` の中身は次のとおりである。

1. 今の利用者を `store_user_folder` から得る。
2. `C:\Nocturne\Settings` と利用者の `Settings` の全部の `.conf` を読み、領域の記録を作る。カーネルの領域でない文書（`nvrm.conf`）も値を持つ記録になる。
3. 登録済みのカーネルの領域ごとに、対象の無い鍵と、既に公開された実体の鍵の効く値で `apply(READBACK)` を呼ぶ。この時だけ起動の thread の上で呼ぶ。
4. 値の無い鍵は宣言の既定で呼ぶ。持ち主の初期値は宣言の既定の一か所にする。
5. `setting` と `setting-save` の thread と worker を起こす。

C: が無い起動では文書を読まず、既定の値で動き、書かない。

ハイバネーションからの復帰は RAM の値がそのまま戻るので、`setting_start` を通らない。

### 10.3 アプリの持ち主の順

```
init の start_drivers → nvrm の起動 → n_setting_own（宣言）
  → 層が対象の無い鍵を READBACK で渡す
  → bring_up（owner-restart の鍵はここで n_setting_get で読む）
  → 出力を見つける → n_setting_entity_put(monitor:…) → n_setting_get で gsync を問う
```

`init` は起動設定を渡さない。今の `NVKMS_OPTIONS` と `NVRM_REGISTRY_DWORDS` の環境の道は消す。

### 10.4 文書が壊れたとき

- 読みの結果が「無い」なら既定で動く。次の変更で作る。
- 読みが I/O の失敗なら既定で動き、**書かない**（今の `keep.c:272` の「読めないのに `read = true`」の欠陥を持たない）。窓は状態に出す。
- 一行が文法を外れたら、その行を飛ばし、状態の `bad` の数を増やす。次の保存で消える。
- 文書全体が UTF-8 でないなど読めない形なら、`<領域>.conf.bad` へ `store_entry_move` で退け、既定で動き、toast を一度出す。

### 10.5 既定の設定で起動する

悪い値（拡大、配置、HDR）で映らなくなったときの回復の道である。AGENTS.md に CLI の段が無いので、起動の画面に道を置く。

`boot/limine.conf` に項目「Nocturne OS（既定の設定で）」を足す。`cmdline: settings-defaults` である。

- `setting_start` は文書を読み、値を「休止」の印つきで持つ。解決は休止の値を見ない。全部の鍵が既定で動く。
- 文書は書き換えない。
- 窓は頁の頭に「既定の設定で起動しています」を出す。
- 窓で値を変えると、その鍵の組の休止が解ける。次の起動から普通に読む。

これは「引数で組まない」の規則に当たる。起動の項目の一つとして画面から選ぶ形で、ユーザーに打たせない。§21 で見せる。

## 11. 身元

### 11.1 装置

身元は装置を知る者が `common/devid.h` の関数で作る。形は固定で `<類>:<16 進 16 字の hash>` である。hash は FNV-1a 64 bit で、一度保存された身元のために変えない。`monitor:` と 16 字は `n_screen_modes.monitor[32]` に収まる。表示名は状態の別の欄である。

規則の文は「取り外せる物は機械をまたいで同じ、作り付けは機械の位置で決まる」である。

| 類 | 作る者 | hash の材料 |
|---|---|---|
| `monitor` | driver（`display.c` の `monitor_of`）、`dev/fb.c`（firmware の画面） | maker、product、serial。数字の serial が 0 か `01010101` で、文字列の serial が空なら、接続の名（GPU の身元と接続の番号）を**常に**足す。他の画面を見ない。 |
| `monitor-model` | 同じ | maker、product |
| `monitor-set` | `screenmode.c` | 今繋がっている全部の `monitor` を並べた列 |
| `gpu` | driver | `rm_get_gpu_uuid_raw` のような装置の識別子。無ければ PCI の subsystem と位置 |
| `audio-output`、`audio-input` | 音の driver | PCI の位置と codec の番地と pin（作り付け）、USB は下の規則 |
| `input-device`、USB の音と網 | USB の driver | vendor、product、serial、interface の番号。serial が無ければ port の道を足す |
| `nic` | 網の driver | 永続の MAC。無ければ位置 |
| `ssid` | 網の部品 | 接続先の名 |

繋ぎ合わせた組（`monitor-set`）の順は身元の順で、繋いだ順に依らない。

**同じ serial が二枚繋がる場合。** 無効な値の一覧に無い serial が重複する画面がある。driver は、自分の出力の中で同じ身元を見つけたら、接続の番号の大きい方に接続の名を足す。片方を抜くと残りの身元が戻る。頻度が低く、失うのはその画面の値だけなので、これを限界として受け入れる。

USB のコードは今 serial を読まない（`iSerial` の語が `kernel/src/dev` に 0 個）。三段目の `input` の前に `usb` が読む。

### 11.2 プログラム

身元は `proc_launch` が開いた文書の親の folder id を辿って作り、`struct process` の文字列の欄 `identity` に持つ。`name[32]` と別である。

| 文書の場所 | 身元 |
|---|---|
| `C:\Nocturne\System\<名>` | `system/<名>` |
| `C:\Programs\<フォルダー>\…\<名>` | `programs/<フォルダー>/…/<名>` |
| `C:\Users\<利用者>\…\<名>` | `users/…/<名>`（利用者からの相対の道） |
| ほか | `other/<道>` |

- id を辿り、格納された綴りを使うので、`C:\PROGRAMS\…` のように打っても同じ身元である。
- 中の文字は §3.3 の符号である。ABI には不透明な文字列として出す。L10 の登録が来たら、登録された身元に差し替えられる。
- `C:\Programs\nvidia\x` は `programs/nvidia/x` で、領域 `nvrm` と関係が無い。

**家族。** `n_launch.flags` に `N_LAUNCH_FAMILY` を足す。呼び手と同じフォルダーの文書を起こすときだけ通り、子は呼び手の身元を継ぐ（[AUMID]）。ブラウザーの子（`browserjsworker`、`browsermediaworker`、`browsermseworker`、`browserstorageworker`、`webfetch`）は `C:\Programs\Browser\` へ移り、この旗で起きる。音量も GPU の値もブラウザーのものになる。

**host。** `js` の上の TypeScript のアプリは `system/js` である。専用の ELF を持つアプリは自分の身元を持つ。今それを持つのは `settings` である。ほかのアプリは行に「スクリプトは区別しない」と出す。

`n_app_data_folder` は変えない。身元と揃えない。

### 11.3 利用者

持ち手 `user` の実体は `user:<名>` で、層の中の記録である。層は「今の利用者」を一つ持つ。

```c
void setting_user_changed(const char *name_or_null); /* L12 が呼ぶ */
```

1. overlay の中の `user` の組の試行を全部戻す。
2. 古い利用者の文書を書き切る。
3. 新しい利用者の文書を読む。
4. 全部の領域で、効く値が変わった鍵を `apply(why = RELOAD)` で渡す。頼みを行列へ入れて進むので、固まった持ち主が他を止めない。
5. 全部の見張りを鳴らす。

ログインの前は `name_or_null = NULL` で、段 2 を飛ばして解く。

利用者が今一人である間も、`session/reload` の `ACTION`（書き手は `settings-writer`）でこの道を通して試す。

### 11.4 複数の画面と GPU

- 画面ごとの値は `monitor:<hash>`、組の値は `monitor-set:<hash>` である。
- GPU ごとの `nvrm` は同じ領域の複数の持ち主で、自分の出力の実体を公開する。
- 二枚の GPU をまたぐ配置も一つの `arrangement` である。`screenmode.c` が driver ごとに分けて渡す。
- モニターが GPU を移ると、古い `nvrm` が `present = off` にし、新しい `nvrm` が公開する。値は実体に付くので失わない。

## 12. 権限

### 12.1 書き手

`writer` は三つの語である。判定は持ち手と対象を見る。

| 語 | 通る者 |
|---|---|
| `settings-writer` | カーネルと、属性 `--settings-writer` のプログラム |
| `self` | 上の者と、対象の `program` が呼び手の身元と同じ者（持ち手は `user` だけ） |
| `any` | 誰でも |

カーネルの部品は自分の直の関数と `setting_post` で書く。領域の持ち主は `PUBLISH` で自分の値の変化を言える。

`--settings-writer` を付ける文書は `settings` と `settingtest` である。`settingtest` は決まったコードだけを走らせ、自分の試験の領域と `session/reload` だけを書く。音量の `volume` は属性を持たない。`audio/volume` の `writer` が `any` だからである。

### 12.2 持ち主になれる者

属性 `--settings-owner` のプログラムは、自分の文書の名と同じ名の領域だけを取れる。カーネルが登録した領域は取れない。

`STORE_SETTING_OWNER 8` と `STORE_SETTING_WRITER 16` を `common/store_format.h` に足す。`host/store/store.c` の `attribute_of` が `--settings-owner` と `--settings-writer` を読む。`scripts/layout.py` の表が付ける。

- `proc_launch` は親のフォルダーが `STORE_SYSTEM` のときだけ属性を権限へ写す。
- 属性を変える口はアプリに無い。
- `C:\Nocturne\System` のプログラムに権限は無い。`js`、`tcc`、`clang`、`sh`、`syscallfuzz`、`browser*worker` は普通のアプリと同じである。
- `Terminal` からの起動は呼ぶ者の身元を継がない。
- `--settings-writer` のプロセスは起動元の起動設定（`USERFOLDER`、`FONT_DIR` など）と起動元の handle を受けない。`proc_launch` が捨てる。`settings` は引数を §8.4 の形だけ受ける。

窓は `js` でなく、`js` の実行部と `settings.js` を埋め込んだ ELF `settings` である。`scripts/layout.py` と Makefile が組む。外のスクリプトを読む道を持たない。

### 12.3 読み手と秘密

`reader` は `any` か `settings-writer` である。規定は `any` である。

資格情報を値にしない。Wi-Fi の鍵、VPN、proxy のパスワードは、秘密の保管の部品ができるまで鍵を登録しない（[WLAN]、[POL]）。保存は平文なので、`reader` で秘密を守ったと言わない。`reader = settings-writer` は、ほかのアプリの値（プログラムごとの権限など）を読ませないためにある。

### 12.4 強制

`forced` の行は機械の文書の印である。一段目は書く窓を作らない。解決と出所と窓の固定の表示だけを通す。`CHANGE` は `N_ERR_DENIED` である。カーネルの `N_SETTING_FORCED` を書く口は、管理の部品ができるまで呼び手が無いので作らない。読むだけを作る。

## 13. VRR と G-SYNC

次の五つを分ける。

| もの | 種 | 持ち主 | 形 |
|---|---|---|---|
| モニターの VRR の範囲 | 申告 | driver | `n_screen_mode` の `N_SCREEN_MODE_VRR`、`vrr_min_mhz` |
| G-SYNC の入り切り | driver の設定 | `nvrm` | `nvrm/gsync @ machine × monitor`（親 `monitor-model`） |
| 窓も含むか | driver の設定 | `nvrm` | `nvrm/gsync-scope @ machine × monitor`、`fullscreen`・`fullscreen-and-windowed` |
| OS の VRR | OS の設定 | `wm.c` | `compositor/variable-refresh @ user × monitor`、既定は切 |
| アプリの選択 | アプリ | そのアプリ | 窓の旗 `WIN_VRR` |

`nvrm/gsync` の既定は実体ごとである。`nvrm` は NVKMS の `vrrType` を読み（`third_party/nvidia-open/src/nvidia-modeset/interface/nvkms-api.h:1569-1596`）、`GSYNC`、`GSYNC_V2`、`ADAPTIVE_SYNC_DEFAULTLISTED` なら入を、`ADAPTIVE_SYNC_NON_DEFAULTLISTED` なら切を `default.gsync` で公開する（[N5] の「検証済みは自動で入、未検証は手で」）。KAPI の `vrrSupported` は一つの真偽なので区別できない。`nvrm` は `vrrType` まで読む。

### 13.1 表

| OS の値 | driver の値 | 全画面の直接表示で VRR を求めるゲーム | 全画面の直接表示で VRR を求めないゲーム | 合成の面 |
|---|---|---|---|---|
| 切 | 切 | 固定 | 固定 | 固定 |
| 切 | 入 | VRR | 固定 | `gsync-scope` が窓も含むなら VRR の歩調、ほかは固定 |
| 入 | 切 | 固定 | 固定 | 固定 |
| 入 | 入 | VRR | VRR（OS が足す） | `gsync-scope` が窓も含むなら VRR の歩調、ほかは固定 |

監査 F41 の案の「入・入の合成の面は VRR の歩調」は採らない。[VRR] の OS の切り替えの効果は全画面のゲームに足すことで、合成の面を変えない。合成の面は `gsync-scope`（[NV] の全画面か窓も含むかの選択）で決まる。

OS の切が driver の入を消さないことが要点である。[VRR] は OS の切り替えが driver を置き換えないと書く。

### 13.2 値の流れ

```
compositor/variable-refresh と compositor/variable-refresh-app と窓の WIN_VRR
  → wm.c の変数
  → 画面ごとに計算：
      直接表示：wants = app != off && (WIN_VRR || OS の値)
      合成の面：wants = true
  → n_screen_state の新しい欄 vrr_wanted で driver へ渡す

nvrm/gsync と nvrm/gsync-scope
  → nvrm の出力ごとの変数（apply は atomic で入れるだけ）

nvrm の commit：
  enabled = vrr_wanted && gsync && 申告あり && (直接表示 || scope が窓も含む)
  → KAPI の vrrEnabled
  → n_screen_vblank の新しい欄 vrr（今 VRR で動いているか）

wm.c（kernel/src/gui/wm.c:2050）
  bool vrr = 申告 && n_screen_vblank の vrr
  → 合成の歩調
```

`nvrm` は `compositor` の値を読まない。`wm.c` は `nvrm` の値を読まない。互いに `vrr_wanted` と `vrr` の二つの欄で話す。AMD の driver も同じ二つの欄を使い、自分の `amdrm/freesync` を持つ。driver の設定を知らずに合成が動く。

`N_SCREEN_MODE_VRR` と `vrr_min_mhz` を立てる driver が今 0 個である。`ports/nvidia/display.c` が立てる。これは二段目の仕事で、効く道が通るまで `nvrm/gsync` を登録しない。

同じ形が driver の 3D の値（§14 の `nvrm/*`）の全部に当たる。

## 14. 鍵の表

`settings-ownership.md`、`audit-settings-missing.md` §6、監査 §3 の全部の項目を書く。「機」は機械、「利」は利用者である。`W` は `write_default` の組で、複数の組を受ける鍵は残りを続けて書く。効き方の欄は無印が `now` である。

### 14.1 画面

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 解像度、周波数、配置、主画面、回転、画面のオンとオフ、ミラー、HDR、色の深さ | `display/arrangement` | `RECORD`、部品 `arrangement` | W 機×`monitor-set` | `trial`、15000 ms。組の全部のモニターの `mode`、`x`、`y`、`rotation`、`primary`、`on`、`source`、`hdr`、`depth`。一枚の画面は一枚の組である。選択肢は `monitor` の状態の `options`。検証は持ち主で、隙間と重なりを断る（[SRC]） |
| 試して戻す | 上の `effect` | | | §9 |
| モードの選択を消す | 上の `RESET` と `TRY` | | | §9.1 |
| 既定のモードの選び方 | `display/mode-preference` | 閉じた `ENUM`：`monitor-preferred`、`highest-refresh` | W 機 | 決G6 |
| 拡大 | `compositor/scale` | `NUMBER` 桁 2、`special` `auto` | W 利×`monitor` | 持ち主は合成。窓へ事象（[W6]） |
| 拡大の方式 | `nvrm/scaling` | `ENUM` | W 機×`monitor` | driver の値 |
| OS の VRR | `compositor/variable-refresh` | `BOOL`、既定 `off` | W 利×`monitor` | §13 |
| アプリの VRR の上書き | `compositor/variable-refresh-app` | `ENUM`：`follow`、`off` | W 利×`program` | §3.6 |
| VRR の申告 | 鍵でない | | | §13 |
| Night light | `compositor/night-light`、`-strength`、`-schedule` | `BOOL`、`INT`、`RECORD`（`TIME`） | W 利 | 色の道が来てから登録 |
| 滑らかな階調 | `compositor/smooth-gradients` | `BOOL` | W 利 | 一段目 |
| 明るさ | `display/brightness` | `INT`、`storage = owner` | W 機×`monitor` | 値は画面の中。DDC/CI の道が来てから登録 |
| Auto HDR、RGB の範囲 | `compositor/auto-hdr`、`nvrm/rgb-range` | `BOOL`、`ENUM` | W 利×`monitor`、W 機×`monitor` | 色の道が来てから |
| 色の校正 | `compositor/color-profile` | `DOCUMENT` | W 機×`monitor` | §10.1 |
| driver の色 | `nvrm/desktop-colour` | `RECORD`（`NUMBER`） | W 機×`monitor` | |

### 14.2 driver と GPU

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| G-SYNC の入り切り | `nvrm/gsync` | `BOOL` | W 機×`monitor`、機×`monitor-model` | §13 |
| 窓か全画面か | `nvrm/gsync-scope` | `ENUM` | W 機×`monitor` | §13 |
| Monitor Technology | `nvrm/monitor-technology` | `ENUM` | W 利、利×`program` | 直接表示の flip が読む |
| 垂直同期の上書き | `nvrm/vsync` | `ENUM`：`app`、`off`、`on`… | W 利、利×`program` | Mesa が読む。既定 `app` |
| 最大フレーム レート | `nvrm/frame-limit` | `INT` 20〜1000、`special` `off` | W 利、利×`program` | Mesa が読む |
| 低遅延モード | `nvrm/low-latency` | `ENUM` | W 利、利×`program` | Mesa が読む |
| トリプル バッファ | `nvrm/triple-buffer` | `BOOL` | W 利、利×`program` | Mesa が読む |
| 優先する周波数 | 作らない | | | 上書きする相手が無い |
| 電源管理モード | 登録しない | | | 効く道が RM に無い |
| FreeSync | `amdrm/freesync` | `ENUM` | W 機×`monitor`、利×`program+monitor` | AMD の driver が来たら |
| RM・NVKMS の引数 | `nvrm/hdmi-deep-colour` など要るものだけ | `BOOL` | W 機 | `owner-restart`。起動設定の道は消す |
| アプリごとの GPU | `graphics/gpu` | 開いた `ENUM`（状態が GPU の一覧） | W 利×`program` | 読み手は GL と Vulkan の初期化。GPU が二つ以上か、GPU と CPU の選択があるときだけ行を出す。頼みでありアプリが最後に選べる |
| 窓のゲームの最適化 | 鍵にしない | | | アプリの選択と合成の判断で足りる |
| Game Mode | `scheduler/game-mode` | `BOOL` | W 利 | 持ち主は scheduler。機能が来てから |

### 14.3 音

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 音量、消音 | `audio/volume`、`audio/muted` | `INT` 0〜100、`BOOL` | W 利×`audio-output` | `writer = any`。部品 `volume` |
| アプリごとの音量 | `audio/app-volume` | `INT` | W 利×`program+audio-output`、利×`program` | `writer = self`。出口の音量と掛け合わせる |
| 既定の出口 | `audio/default-output` | 開いた `ENUM` | W 機 | 出口が二つ以上のとき行を出す |
| アプリごとの出口 | `audio/app-output` | 開いた `ENUM` | W 利×`program` | `writer = self` |
| 入力の音 | `audio/input-volume` | `INT` | W 利×`audio-input` | 録音の道が来てから |
| 出口の形式 | `audio/format` | 開いた `ENUM` | W 機×`audio-output` | 実機の driver が来てから |
| 立体音響 | `audio/spatial` | `BOOL` | W 利×`audio-output` | 機能が来てから |
| 音量の確認の音 | `audio/feedback-sound` | `BOOL` | W 利 | |
| 音を試す | `audio/test` | `ACTION` | | |

### 14.4 入力

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| ポインターの速さ | `input/pointer-speed` | `NUMBER` | W 利、利×`input-device` | |
| 加速 | `input/acceleration` | `BOOL` | W 利、利×`input-device` | |
| ダブルクリック | `input/double-click-ms`、`-px` | `INT` | W 利 | アプリが読む |
| ホイールの行数 | `input/wheel-lines` | `INT` | W 利、利×`input-device` | |
| キーの repeat | `input/repeat-delay-ms`、`repeat-rate-ms` | `INT` | W 利、利×`input-device` | 持ち主は `dev/input.c`。実体の状態の `capability` がキーボードの行にだけ出す |
| 配列 | `input/layout` | 開いた `ENUM`（`C:\Nocturne\Layouts` の一覧） | W 利、利×`input-device` | 表は値にしない |
| IME | `input/method` | 開いた `ENUM` | W 利 | 機能が来てから |
| 主ボタン、ポインターの大きさ | `input/primary-button`、`input/pointer-size` | `ENUM`、`INT` | W 利 | |
| タッチパッド、マウスごとの値 | 上の鍵の `input-device` の組 | | | |
| 固定キー、ポインターの色 | `input/sticky-keys`、`input/pointer-colour` | `BOOL`、`COLOR` | W 利 | 機能が来てから |

### 14.5 電源

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 計画、性能の方針 | `power/profile` | `ENUM` | W 機×`power:ac`、機×`power:dc` | 効く所は `cpu_perf_set_policy` |
| 画面を消す時間、眠る時間 | `power/display-off-s`、`power/sleep-after-s` | `INT`、`special` `never` | W 機×`power:ac`、機×`power:dc` | 使っていない時間を見る部品が来てから |
| 電源ボタン | `power/button` | `ENUM` | W 機×`power:ac`、機×`power:dc` | [PB] |
| 蓋 | `power/lid` | `ENUM` | 同じ | 蓋の装置が来てから |
| 休止の有効 | `power/hibernate` | `BOOL` | W 機 | |
| 電源の状態 | `power/source` | `ENUM`、`access = read`、`storage = owner` | | 見張りで通知（`settings-ownership.md` [W19]） |

### 14.6 時刻と地域

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 時間帯 | `time/zone` | 開いた `ENUM` | W 機 | libc の `localtime` は `n_setting_cache_blob` で読む |
| RTC は地方時か | `time/rtc-local` | `BOOL` | W 機 | 決T1 |
| 時刻の同期 | `time/sync`、`time/sync-server` | `BOOL`、`STRING` | W 機 | 読み手はサービス `timesync` |
| 手で時刻を変える | `time/set-now` | `ACTION`（`STRING`） | | `settings-writer` |
| 今の時刻 | `time/now` は作らない | | | 窓は `n_clock` で読む |
| 地域の形式、12 時間と 24 時間、夏時間 | `region/format`、`region/hour-cycle`、`time/dst-auto` | `ENUM`、`BOOL` | W 利、W 利、W 機 | |
| 表示の言語 | `region/language` | 開いた `ENUM` | W 利 | `owner-restart` の扱いを行ごとに。文字列の表は別の設計 |

### 14.7 網

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| IP の取り方 | `network/ipv4`、`network/ipv6` | `RECORD` | W 機×`nic`、機×`nic+ssid` | §10.2 の順で効く |
| DNS | `network/dns` | `LIST`（`pattern`） | 同じ | [DNS] |
| ホスト名 | `network/hostname` | `STRING`、`pattern = hostname` | W 機 | |
| MAC のランダム化 | `network/mac-random` | `ENUM` | W 機×`nic+ssid` | |
| proxy | `network/proxy` | `RECORD`（資格情報なし） | W 利 | 持ち主は網のサービス |
| Wi-Fi・VPN の接続先 | 鍵は装置と秘密の保管が来てから | | | §12.3 |
| 探す、忘れる | `network/scan`、`network/forget` | `ACTION` | | |
| 今の IP | `network/address` | `access = read`、`storage = owner` | `nic` | |
| TCP の緩衝 | 鍵にしない | | | 持ち主が計算する |

### 14.8 外観と作業

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 色 | `appearance/theme`、`appearance/accent` | `ENUM`、`COLOR` | W 利 | |
| 壁紙 | `appearance/wallpaper` | `DOCUMENT` | W 利、利×`monitor` | §10.1 |
| 字の大きさ、字体 | `appearance/text-size`、`appearance/font-ui`、`-mono` | `INT`、開いた `ENUM` | W 利 | |
| アニメーション、透明 | `appearance/reduce-motion`、`reduce-transparency` | `BOOL` | W 利 | |
| 高コントラスト、色フィルター、字幕 | `appearance/high-contrast`、`colour-filter`、`captions` | `BOOL`、`ENUM`、`RECORD` | W 利 | |
| 点滅とメニューの時間 | `appearance/caret-blink-ms`、`menu-delay-ms` | `INT` | W 利 | |
| token の配布 | `appearance/tokens` | `RECORD`、`access = read` | | 持ち主が計算。全部のアプリが `n_setting_cache_blob` で読む |
| タスクバー | `shell/taskbar-edge` | `ENUM` | W 利 | |
| 開始の一覧、机の上の絵 | `shell/pinned`、`shell/icons` | `LIST` | W 利 | `settings-writer`、`if_generation`。L10 |
| ショートカット | `shell/shortcuts` | `RECORD` | W 利 | 値はプログラムを起こす道なので `settings-writer` だけが書く |
| 窓の整列 | `shell/snap` | `BOOL` | W 利 | 機能が来てから |
| 通知、邪魔しないで、優先 | `shell/notifications`、`shell/dnd`、`shell/notification-priority` | `BOOL`、`ENUM` | W 利、利×`program` | |
| toast の時間、音量の刻み | `shell/toast-seconds`、`shell/volume-step` | `INT` | W 利 | |
| 拡大鏡、読み上げ | 機能が来てから | | | |

### 14.9 起動、利用者、プログラム

| 項目 | 鍵 | 型 | 組 | 備考 |
|---|---|---|---|---|
| 起動の項目と待ち時間 | `boot/timeout`、`boot/default-entry` | `INT`、`ENUM`、`storage = owner` | W 機 | ESP へ書く部品ができるまで登録しない（[LIM]） |
| 起動の旗 | 層に入れない | | | §15 の決C1 |
| 起動のときのアプリ | `session/startup` | `LIST` | W 利 | `settings-writer` |
| 自動ログイン | `session/auto-login` | `STRING` | W 機 | L12 |
| サインイン、利用者の作成、パスワード | 鍵にしない | | | 操作と秘密で、L12 の窓 |
| プログラムの設定 | 層の外 | | | プログラムの持ち物 |
| 既定のアプリ | `shell/default-apps` | `RECORD` | W 利 | L10 |
| 起動の一時の値 | 層の外 | | | `n_launch.settings`。名は `n_launch.values` に換える |
| 許可 | `privacy/<権限>` | `ENUM` | W 利×`program` | `reader = settings-writer`。機能が来てから |
| 掃除、使用量 | `storage/clean`、`storage/used` | `ACTION`、`access = read` | | 機能が来てから |
| 更新、遠隔デスクトップ | 作らない | | | 範囲の外、実機の機能が無い |

## 15. 決め打ちの監査の全部の行の行き先

種の語は次のとおりである。

| 種 | 意味 |
|---|---|
| 鍵 | 層の鍵になる |
| 申告 | 装置の申告か ABI の問い合わせになる。設定にしない |
| 計算 | メモリー、CPU の数、窓の大きさなどから計算する。設定にしない |
| 内部 | 持ち主の内部の定数で残す。理由を書く |
| 消す | コードから消す |
| 外 | 層の外の機能の設計へ渡す |

### 15.1 層ごとのまとめ

| 層 | 行き先 |
|---|---|
| L1 | この文書の全体である。値は `keep.c` を消して鍵になる。 |
| L2 | 新しい部品 `gui/appearance.c` が `appearance` の領域を持つ。色と寸法の token を計算し、`wm.c` は関数で、アプリは `appearance/tokens` の複製で読む。拡大率は `compositor/scale`。 |
| L3 | 申告は画面の ABI（番号つきの問い合わせ）である。選択は `display/arrangement` である。 |
| L4 | 申告は `display_abi.h` と `gpu_abi.h` の能力の記述である。driver の設定は `nvrm` の領域である。 |
| L5 | `dev/input.c` が `input` の領域を持つ。driver は usage と時刻だけを渡す。 |
| L6 | `time` と `region` の領域である。`timesync` は読み手である。文字列の表は別の設計である。 |
| L7 | 申告は音の問い合わせの ABI である。選択は `audio` の領域である。 |
| L8 | 層の外である。規則：上限は装置の申告、メモリー、CPU の数、仕様の値のどれかから計算する。口は CPU の数・node・空きメモリーの問い合わせの ABI である。層自身もこの規則に従う（§4.1）。 |
| L9 | 層の外である。driver の表の設計である。設定にしない。 |
| L10 | アプリの登録の設計である。層は `shell/*` の鍵と、差し替えられる身元を用意する。 |
| L11 | `dev/pm.c` が `power` の領域を持つ。 |
| L12 | 利用者の部品が `setting_user_changed` を呼ぶ。 |
| L13 | `net.c` が `network` の領域を持つ。口ごとの `struct netif` と起動の順が先である。 |
| L14 | ブラウザーは読み手である。`css.c` と `js_screen.js` と `webprefs` へ通す。 |
| L0 | その場で直す。 |

### 15.2 全部の行

| 行 | 層 | 種 | 行き先 |
|---|---|---|---|
| 決G1 | L3 | 申告 | `ui.c:2833` の 16 ms を消し、`EV_FRAME` の周期で進める。 |
| 決G2 | L3 | 申告・鍵 | 指の最大は表示エンジンの申告。大きさは `input/pointer-size` × 拡大率。`N_SCREEN_POINTER_SIZE` は画面の ABI の可変の大きさになる。 |
| 決G3 | L3 | 申告・鍵 | `fb.deep`・`fb.bpc` を画面ごとの能力にする。選択は `arrangement` の `depth`。 |
| 決G4 | L4 | 申告 | `nvrm` の出力の表（head ごとの状態）。設定にしない。 |
| 決G5 | L4 | 申告・鍵 | `display.c` が VRR を申告する。§13。 |
| 決G6 | L1 | 鍵 | `display/mode-preference`。 |
| 決G7 | L3 | 鍵 | `display/arrangement`。taskbar は主画面へ（`shell/taskbar-edge`）。 |
| 決G8 | L2 | 鍵・計算 | 寸法は token × `compositor/scale`。`gui/appearance.c`。 |
| 決G9 | L2 | 鍵 | 色は token。`appearance/theme`、`accent`、`wallpaper`。`wm.c` と `desktop.c` の色の直書きを消す。 |
| 決G10 | L2 | 鍵 | `ui.h` の `const` を消し、`appearance/tokens` を `n_setting_cache_blob` で読む。 |
| 決G11 | L2 | 鍵 | `appearance/font-ui`、`font-mono`、`text-size`。`term.c` の `CW`・`CH` は字体から計算。 |
| 決G12 | L8 | 計算 | 窓の大きさとメモリーから。 |
| 決G13 | L8 | 計算 | 窓の題は長さを持つ確保にする。 |
| 決G14 | L4 | 申告 | バッファーの数は申告と遅れの方針。`N_SCREEN_BUFFERS_MAX` を消す。 |
| 決G15 | L3 | 申告 | EDID の読み手が CTA-861、DisplayID、物理の大きさを読む。 |
| 決G16 | L3 | 申告 | 申告の数に従う確保。 |
| 決G17 | L3 | 申告 | 画面の番号つきの ABI。 |
| 決G18 | L3 | 消す | 範囲を `vm/rdp.c` へ移す。 |
| 決G19 | L8 | 計算 | 文書の型とメモリーに従う。クリップボードの持ち主の設計。 |
| 決G20 | L1 | 鍵 | `appearance/caret-blink-ms`、`menu-delay-ms`。 |
| 決G21 | L3 | 申告 | `EV_FRAME` の周期。 |
| 決G22 | L3 | 計算 | 面の数は周期と遅れの方針から。 |
| 決G23 | L8 | 計算 | `wm.c` の内部で画面の大きさから。 |
| 決G24 | L1 | 鍵 | `shell/toast-seconds`。試行の期間は宣言の `trial_ms`。 |
| 決I1 | L5 | 鍵 | `input/repeat-delay-ms`、`repeat-rate-ms`。`usbhid.c` から `dev/input.c` へ移す。 |
| 決I2 | L1 | 鍵 | `input/double-click-ms`、`-px`。`files.c` の値を消して読む。 |
| 決I3 | L5 | 鍵 | `input/acceleration`。曲線は時間で割った速さにする。 |
| 決I4 | L1 | 鍵 | `input/pointer-speed`。 |
| 決I5 | L5 | 申告 | 入力の事象の ABI を 8 個のボタンにする。 |
| 決I6 | L5 | 鍵 | `input/layout`。表は `C:\Nocturne\Layouts` の文書。key の ABI に Unicode を持つ。 |
| 決I7 | L1 | 鍵・内部 | `input/wheel-lines`、`shell/volume-step`、`audio/feedback-sound`。音の高さと `POINTER_SETTLE_MS` は持ち主の内部。 |
| 決I8 | L10 | 鍵 | `shell/shortcuts`。登録は L10。 |
| 決I9 | L5 | 申告 | HID の解析が AC Pan、Resolution Multiplier、Consumer page を読む。 |
| 決I10 | L5 | 計算 | 記述子の長さに従う確保。 |
| 決I11 | L8 | 計算 | 入力の列を伸ばす。黙って失わない。 |
| 決I12 | L3 | 鍵 | `arrangement` の座標へ写す。タブレットの画面は `input/tablet-monitor @ 利×input-device`。 |
| 決I13 | L8 | 内部 | 働きは正しい。窓の受け箱の内部の値で残す。 |
| 決S1 | L7 | 申告・鍵 | HD Audio と USB Audio の driver。`audio/default-output`。 |
| 決S2 | L7 | 申告 | 音の装置の能力の問い合わせの ABI。 |
| 決S3 | L7 | 申告 | 遅れは装置の周期から。`RUNS` の AC'97 の根拠を `vm/` の後ろへ。 |
| 決S4 | L7 | 申告 | 音の問い合わせの ABI。 |
| 決S5 | L8 | 計算 | 流れの数に従う確保。 |
| 決S6 | L7 | 申告 | 能力の層の判定一か所にする。 |
| 決S7 | L1 | 鍵・内部 | 既定は宣言の `default`。v² の曲線は持ち主の内部。利用者に選ばせる普通の例が読んだ文書に無い。 |
| 決T1 | L6 | 鍵 | `time/zone`、`time/rtc-local`。libc、QuickJS、`wm.c` は同じ口で読む。 |
| 決T2 | L6 | 鍵 | `time/sync`。`timesync` が NTP を行い `clock_wall_proof` へ渡す。 |
| 決T3 | L6 | 鍵・外 | `region/format`、`hour-cycle`、`language`。文字列の表は別の設計。`wm.c` の曜日と月の表を消す。 |
| 決T4 | L0 | 内部 | `arch/cpu.c` で family を見て読む。層に載らない。 |
| 決M1 | L8 | 計算 | CPU の数と topology の問い合わせから。 |
| 決M2 | L8 | 計算 | メモリーと buffer の方針から。 |
| 決M3 | L1 | 計算 | 利用者の値にしない。応答の時間から計算する。監査の「設定か応答の時間から」のうち後者を取る。理由：利用者が値を決める材料を持たない。 |
| 決M4 | L8 | 計算 | 遅れの方針から。 |
| 決M5 | L7 | 申告 | 音の周期の申告から。 |
| 決M6 | L2 | 計算 | 寸法は token × 拡大率。 |
| 決K1 | L8 | 計算 | アプリごとの割当を CPU の数から。 |
| 決K2 | L8 | 内部 | `sys/sched` の方針の定数で残す。規模で壊れないと監査が書く。 |
| 決D1 | L4 | 申告 | `gpu_abi.h` を能力の記述に書き直す。互換の 512 と 32 を消す。 |
| 決D2 | L9 | 外 | driver の結び付けの表。設定にしない。 |
| 決D3 | L9 | 外 | PCI の全部の segment。設定にしない。 |
| 決D4 | L8 | 申告 | CAP.MQES とメモリーから。 |
| 決D5 | L8 | 申告 | Get Max LUN の応答から。 |
| 決D6 | L8 | 申告 | MSI-X の Table Size から。 |
| 決D7 | L8 | 計算 | 装置の数と Table Size から。 |
| 決D8 | L8 | 計算 | ACPI の列挙の数から。 |
| 決D9 | L1 | 内部 | `dev/nvme.c` の方針で残す。普通の設定の頁に無い（`settings-ownership.md` [W8] の一覧）。鍵にしない。 |
| 決D10 | L4 | 内部 | `nvrm` の内部で残す。周期から決められるものは周期から。 |
| 決D11 | L8 | 計算 | 各 driver が数から。 |
| 決N1 | L13 | 申告 | `struct netif` を口ごと、ABI に番号。`nic:<hash>` が実体。 |
| 決N2 | L13 | 申告 | 口の MTU を driver と DHCP option 26 から。 |
| 決N3 | L13 | 計算 | 口の速さから。 |
| 決N4 | L8 | 計算 | 窓の大きさとメモリーから。 |
| 決N5 | L13 | 鍵 | `network/hostname`、`network/ipv4`、`ipv6`、`dns`。 |
| 決N6 | L8 | 内部 | `http.c` の内部で残す。 |
| 決F1 | L12 | 鍵・外 | §11.3。利用者の表は L12 の設計。 |
| 決F2 | L8 | 計算 | メモリーと装置の速さから。 |
| 決F3 | L8 | 消す | `READ_MOST` を消す。§4.1。 |
| 決F4 | L8 | 計算 | 一覧の数に従う確保。 |
| 決F5 | L8 | 内部 | 各持ち主の内部で残す。当たる所が狭い。規則に照らして一つずつ計算へ移す。 |
| 決P1 | L11 | 鍵 | `power/profile`、`display-off-s`、`sleep-after-s`、`button`。 |
| 決P2 | L11 | 内部 | 電源の猶予は `pm.c` の内部で残す。利用者に選ばせる普通の例が読んだ文書に無い。 |
| 決P3 | L1 | 鍵 | `boot/timeout`。書く部品ができるまで登録しない。 |
| 決C1 | L1 | 内部 | 起動の旗は層に入れない。監査の「一時の上書き」は採らない。`settings-ownership.md` の決定（開発と試験の旗）を取る。`nosmp` などは窓に出さない。 |
| 決C2 | L1 | 鍵・消す | 要るものだけ `nvrm/*` の `owner-restart` の鍵にする。環境の道を消す。`conceal_vrr_caps` は鍵にしない。 |
| 決C3 | L1 | 消す | `older[]`、`SCREEN_MODES_FIRST_BYTES`、第 2 引数 0 の応答を消す。 |
| 決C4 | L1 | 消す | 範囲は宣言だけに書く。 |
| 決R1 | L10 | 外・鍵 | `wm.c` の `apps[]` を消し、登録から作る。`shell/pinned`、`shell/default-apps`。 |
| 決W1 | L14 | 鍵（読み手） | `css.c` が `appearance/theme`、`reduce-motion`、`reduce-transparency`、`high-contrast` を読む。`hover`・`pointer` は `input` の状態、`color-gamut`・`dynamic-range` は画面の申告。変化は `change` の事象まで通す。 |
| 決W2 | L3 | 申告・鍵 | `resolution` と `devicePixelRatio` は `compositor/scale`、位置は `arrangement` の状態。 |
| 決W3 | L14 | 外 | 既定の字の大きさと頁の拡大はブラウザーの設定（`webprefs`）。層の外。 |
| 決W4 | L14 | 外 | ホームと検索はブラウザーの設定。層の外。 |
| 決W5 | L8 | 外 | Storage の割当はブラウザーの持ち物。 |

## 16. 数が十倍になったとき

| 数 | 今の規模 | 十倍 | 詰まる所 | 呼ぶ側の変化 |
|---|---|---|---|---|
| 領域 | 約 12 | 120 | カーネルの領域の worker は数に比例する。driver の領域は worker を持たない。120 の全部がカーネルの領域になることは無い。 | なし |
| 鍵 | 約 100 | 1000 | 窓の一頁の行が増える。`group` で畳む。 | なし |
| 値の行 | 数十 | 数百〜数千 | 一つの文書の書込みが大きくなる。書込みは領域ごとなので他に掛からない。0.5 秒の遅れが畳む。 | なし |
| 実体（画面 3→30、古いモニターの値が残る） | 数個 | 数十 | `monitor-set` の値が組ごとに増える。窓で消せる。 | なし |
| プログラムの値 | 数個 | 数千 | 表は伸びる。文書の読み込みは起動の一度で、大きさに比例する。 | なし |
| 見張り | 数十 | 数百 | 読みの輪は領域ごとに一本で、handle は位置だけを持つ。変化の一回の費用は起こす数に比例する。ゲームの数だけ起きるが、変化は人の操作の速さである。 | なし |
| 持ち主（GPU 2→8） | 1〜2 | 16 | 対象の無い鍵の `RELOAD` が数に比例する。行列へ入れるだけで呼び手は待たない。 | なし |
| CPU（1024） | | | 葉の spinlock は MCS である。読みは領域ごとの錠で、一コマの道に無い。読みが集中する領域は無い。読みが増えて詰まるなら、値の表を RCU の形にする。今その呼び手は無い。 | なし |
| 利用者 | 1 | 10 | 文書は今の利用者のものだけを読む。切替えの費用は領域の数に比例する。 | なし |
| 変更の速さ | 人の操作 | 悪いアプリの連打 | 頼みは呼び手の課金で、呼び手の thread が眠る。領域の行列は呼び手の thread の数までである。`writer = any` の鍵は `audio` の音量だけで、持ち主は同じ値を変化にしない。保存は遅れを 30 秒まで伸ばす。 | なし |
| 画面の周期（600 Hz×3） | | | 層は合成の道に無い。`wm.c` は自分の変数を読む。`apply` は合成の thread で走らない。 | なし |
| 窓の数（読み手のプロセス） | 数十 | 数百 | `appearance/tokens` の変化で全部のプロセスが一度 `GET` を呼ぶ。人の操作の一回に数百回で、問題にならない。 | なし |

規模の例（600 Hz、3 枚の画面、ゲーム、YouTube、Discord）で層が走るのは、利用者が窓を操作した時と、起動の時と、モニターの抜き差しの時だけである。Mesa の `present` は atomic の読み一つである。

## 17. 失敗と回復

| 失敗 | 何が起きるか | 回復 |
|---|---|---|
| 持ち主が固まる | 読みは答える。変更は締切で `N_ERR_TIMEOUT`。窓は「応答なし」。他の領域は動く。 | 持ち主が戻れば行列の次から。遅れた答えは捨てる。 |
| 持ち主が死ぬ | `owner` の記録、実体が `present = off`、待つ頼みへ `N_ERR_NOT_READY`。値は残る。Mesa は写しを読み続ける。 | 再起動の `OWN` で `READBACK`。見張りは生きている。 |
| 同じ領域の二つ目の持ち主 | 並びに入る。先頭だけが全体の鍵に答える。 | 先頭が死ねば次。 |
| 同じ実体の二重の公開 | `N_ERR_EXISTS`。 | 前の持ち主が `present = off` にした後で公開する。 |
| 自分の領域への変更 | `N_ERR_BUSY`。 | 持ち主の書き直し。読みは許す。 |
| 試行の途中で窓が死ぬ | 期限で戻る。 | |
| 試行の途中で電源が落ちる | 文書は確定の値だけなので、確定の値で起動する。 | |
| 試行の途中でモニターが抜ける | 持ち主が `trial_end`。 | |
| 期限と同時の `CONFIRM` | `lock` の下で一度だけ通る。遅い方は `N_ERR_NOT_FOUND`。 | 窓が読み直す。 |
| 悪い値で映らない | 試行で戻る。確定した後なら起動の項目「既定の設定で」。 | §10.5 |
| 文書の読みが I/O の失敗 | 既定で動き、書かない。 | 次の起動で読む。 |
| 文書の一行が壊れた | その行を飛ばす。 | 次の保存で消える。 |
| 文書全体が壊れた | `.bad` へ退け、既定で動き、toast。 | 窓で設定し直す。 |
| C: が満ちた、書込みの失敗 | 値は写しで効く。状態に「保存できない」。toast を一度。 | 印を残して書き直す。 |
| C: が無い | 既定で動き、書かない。 | |
| 媒体の更新 | `C:\Nocturne\Settings` は残る。`System` などは置き換わる。 | |
| メモリーが足りない | `N_ERR_NO_MEMORY`。値は変わらない。 | |
| 輪から読み手が落ちる | `lost` の記録。 | `DESCRIBE` で読み直す。 |
| 二つの窓の競り | `N_ERR_CHANGED`。 | 窓の部品が読み直して当て直す。 |
| 強制の値 | 行が動かない。`N_ERR_DENIED`。 | |
| プログラムの名が変わる | 古い身元の値が残る。窓の一覧に出て消せる。 | L10 の身元で解ける。 |
| 悪意のある USB 機器の文字列 | 身元は hash。表示名は符号を通る。 | |
| 悪いアプリが属性のあるプログラムを起こす | 起動設定を捨てる。引数は頁の名だけ。値を変えない。 | |
| 悪いアプリが `C:\Users\A` を作る | `C:\Users` が `STORE_SYSTEM` なので `N_ERR_DENIED`。 | |
| 悪いアプリが `Settings` を動かす | `place_may_change` が断る。 | |
| 悪いアプリが `audio/app-volume @ 別のプログラム` を書く | `self` なので `N_ERR_DENIED`。 | |
| 悪いアプリが大きな値を送る | 頼みの bytes は呼び手の課金で、型の検証で断られる。 | |
| 利用者の切替えの途中で持ち主が固まる | 行列で待つ。他は進む。 | |

## 18. 作る順

規則は AGENTS.md の「悪いコードは消してから建てる」である。各段で古い道を消す。各段の後に今の媒体で確かめる。

**零段：持ち主に見せる。** §21 の項目。

**一段目：土台と三つのカーネルの領域。**

1. `common/setting.c`、`common/setting.h`、`common/devid.h`。host の試験 `tests/check_setting_format.py`（正規形、符号、解決の表）。
2. `common/store_format.h` の属性、`host/store` の語、`scripts/layout.py`（fill の子、属性、`settings`、ブラウザーの子の移動）、`tests/check_fill_keeps_settings.py`。
3. `store_start` の `Users` と `Settings`。
4. `proc_launch` の身元と権限、`N_LAUNCH_FAMILY`、ブラウザーの起動の道（`N_FOLDER_SYSTEM` の直書きを `C:\Programs\Browser` へ）。
5. `sys/setting.c`：領域、写し、読み、行列、見張り、試行、保存、`setting_note`。`sys/keep.c` を消す。
6. `common/abi.h`、`docs/nocturne-native-abi.md` §12、libc の口、持ち主の口。
7. `compositor`（`smooth-gradients`）、`audio`（`volume`、`muted`、`feedback-sound`）、`display`（`arrangement`、`mode-preference`）へ移す。`screenmode.c` の試行の機械、`SYS_AUDIO_OUTPUT`、`SCREEN_SETTINGS_*`、`SCREEN_MODE_*` の副命令を消す。`input/pointer-speed` は `wm.c` の値なので、この段で `input` の領域の一つの鍵として持ち主を `dev/input.c` へ移す（今の窓の値を失わない）。
8. `nocturne/settings`、`settings.ts` の書き直し、ELF `settings`、`volume` の書き直し。
9. `settingtest`：試験の領域で層の全部の道を試す（読み、変更、断り、受け付け、締切、自分への変更、死と戻り、見張り、輪の畳み、`lost`、試行の全部の道、世代、`forced` の解決、`RELOAD`、`self`、`TEST`、壊れた文書）。実物の領域へは、宣言の形の検査、範囲の外の拒否、読むことだけを当てる。
10. 点検の表（§3.6）を `tests/check_setting_replace.py` にする。
11. 効き目は領域ごとの試験（`systemwatchtest`、`audiotest` の書き直し）。

**二段目：driver と VRR。** `n_screen_state.vrr_wanted`、`n_screen_vblank.vrr`、`WIN_VRR`。`display.c` の VRR の申告と `vrrType`。`nvrm` の領域、Mesa の読み手（`n_setting_cache`）、`graphics/gpu`。§13.1 の表の確かめは実機の担当で、QEMU では申告の無い道だけを確かめる。

**三段目：機械の値と入力。** `net_init` と `net_start` の分離。`input`（`usb` の serial、repeat の移動、配列）。`power`。`time` と `region`、`localtime`、`timesync`。`network` は `struct netif` の後である。

**四段目：外観。** `gui/appearance.c`、`compositor/scale`、`ui.c`、`wm.c`、`desktop.c`。L14 の読み手。`shell` の領域。

**五段目：L10、L12、書き出し。** アプリの登録、`session`、利用者の切替え、設定の書き出し（監査 G7）。秘密の保管の設計の後に Wi-Fi・VPN。

## 19. 監査の 53 指摘への答え

「案のまま」は監査の直す案を採る。「案を換えた」は理由を書く。誤りと判断した指摘は 0 個である。読んだコードと文書に反する筋書きは無かった。

| 指摘 | 答え | 節 |
|---|---|---|
| F1 | 案のまま。仲立ちが宣言、値、状態の写しを持つ。`GET` は写しだけに答え、持ち主を呼ばない。 | §4.2 |
| F2 | 案のまま。チャンネルを使わず、領域ごとの行列、`OWN`・`TAKE`・`ANSWER`、呼び手へ課金。カーネルの呼び手も同じ記録で待つ。 | §4.3、§6 |
| F3 | 案のまま。`GET` と `CHANGE` は `struct n_setting_request *` を一つ受ける。`RESET`・`CONFIRM`・`REVERT` は `op` の欄である。形が同じなので番号を分けない。 | §6.1 |
| F4 | 案のまま。`wm_lock` の下の `setting_set` を禁止する。直の関数か `setting_post`。カーネルの `apply` は worker で走るので、呼び手の錠の下で走らない。 | §4.3、§5.3 |
| F5 | 案のまま。`net_init` は登録まで、`net_start` は `setting_start` の後。 | §10.2 |
| F6 | 案のまま。領域の記録は名前で持ち、死は変化の一つ。`NULL` は一覧の変化だけ。専用の ops。固定の大きさの輪と畳み。輪の大きさは鍵と実体の数から決める。 | §4.4 |
| F7 | 案のまま。答えは 取った・断った・受け付けた の三つ。`settle` で後から言う。 | §4.3、§9.2 |
| F8 | 案のまま。`apply(key, target, effective, why)`、`setting_resolve`、`display` と `compositor` の二領域。`why` に `RELOAD`、`TEST`、`ACT` を足した。読み戻しは保存の印を付けない。 | §5.1 |
| F9 | 案を換えた。アプリの持ち主は保存しない。仲立ちが値を持つので `stop_apps` で失われず、`FLUSH` の頼みが要らない。`pm.c` は `setting_flush` を呼ぶ。理由：頼みを増やさず原因を消す。 | §4.6 |
| F10 | 案のまま。`"!"` を子へ当てる。host の検査を足す。 | §10.1 |
| F11 | 案のまま。締切の既定 1 秒、`N_ERR_TIMEOUT`、受け取り口からの同じ領域の変更は `N_ERR_BUSY`、期限は `setting` の thread が行列へ入れるだけなので独立に進む。 | §4.3、§4.5 |
| F12 | 案を換えた。libc の thread は受ける一本だけである。保存の thread は要らない（§4.6）。`apply` の規則はそのまま。 | §5.2、§5.4 |
| F13 | 案のまま。宣言と状態は持ち主へ、頼みは呼び手へ課金。`READ_MOST` を消す。実体は持ち主が受けたものだけ残す。宣言に無い鍵の行は保存で消す。保存の遅れは 30 秒まで伸ばす。ほかに、領域を取れる者を属性で限るので、任意のプログラムが写しを送る筋が無い。 | §4.1、§4.6、§12.2 |
| F14 | 案のまま。`display/arrangement @ monitor-set` の `RECORD`、`N_SETTING_TEST`。色の深さと HDR と回転もこの中に入れる。 | §14.1 |
| F15 | 案のまま。開いた `ENUM`、状態の `options`、検証は持ち主。読み戻しは形だけを見る。 | §3.2、§3.7 |
| F16 | 案のまま。`DESCRIBE` が値のある対象の一覧を返す。 | §6.1、§8.1 |
| F17 | 案のまま。`user:<名>`、今の利用者、`RELOAD`。`session/reload` で一人の間も試す。 | §11.3 |
| F18 | 案のまま。`storage`、`access`、`ACTION`、答えの `RESTART`。起動の項目は書く部品ができるまで登録しない。 | §3.7、§14.9 |
| F19 | 案のまま。世代、`if_generation`、`N_ERR_CHANGED`。 | §4.3 |
| F20 | 案を換えた。接頭辞の `OWN` でなく、実体を公開した持ち主が持つ。理由：`monitor:<hash>` は GPU を含まず、モニターは GPU を移るので接頭辞で持ち主を決められない。全体の鍵は先頭の持ち主が答える。 | §5.4 |
| F21 | 案を換えた。監査の案の符号は `,` を残すので、要素の区切りと曖昧になる。符号に `,`、`+`、`:`、`!` を足し、空の文字列を `%-` にする。行の形は同じである。 | §3.3、§3.4 |
| F22 | 案のまま。`canonical()` が一つ。値は長さつき。空の列と空の要素は別の文字列である。 | §3.2 |
| F23 | 案を換えた。文法は案のまま。`app.<符号>` の領域は作らない。理由：プログラムの設定は層の外なので要らない（AGENTS.md「プログラムの設定はそのプログラムの持ち物」）。 | §3.1、§12.2 |
| F24 | 案を換えた。`NUMBER`、`COLOR`、`TIME`、`special`、`max_bytes`、`pattern` は案のまま。`DOCUMENT` は handle を渡すだけでは再起動の後に残らないので、検証の後に層のフォルダーへ写す。カーネルが他者の道を開かない。 | §3.2、§10.1 |
| F25 | 案のまま。文字の id と既定の文字、単位の語と桁。 | §3.7、§8.5 |
| F26 | 案のまま。`common/devid.h`、`<類>:<hash>`、無効な serial は接続の名を常に足す。interface 番号、GPU は UUID。無効な値の一覧に無い重複の限界を §11.1 に書いた。 | §11.1 |
| F27 | 案のまま。持ち手×対象。解決の表は一か所。文書は持ち手ごと。実体の親を足した（同じ型の画面、接続先から口へ）。 | §3.5 |
| F28 | 案のまま。解決は置き換えだけで、別の鍵に分ける。点検の表を試験にする。 | §3.6 |
| F29 | 案のまま。`forced`、出所、窓の固定、`N_ERR_DENIED`。書く窓は作らない。 | §12.4 |
| F30 | 案のまま。`write_default`、出所へ書く。 | §8.3 |
| F31 | 案のまま。`mode`（`placed`・`effective`）、持ち手と対象は別の欄、省くと呼び手の身元。 | §4.2、§6.1 |
| F32 | 案を換えた。属性で決めることと専用の ELF は案のまま。属性を二つに分ける（持ち主と書き手）。`init` と `nvrm` と `volume` に書き手の属性を付けない。`init` は設定を書かない。`nvrm` は自分の領域の持ち主で、他の領域を書く理由が無い。`volume` は `any` の鍵だけを書く。書き手の起動設定を捨てる規則を足した。 | §12 |
| F33 | 案を換えた。folder id で辿る身元は案のまま。`app.<符号>` は作らない。領域名は文書の名に限る。家族の旗 `N_LAUNCH_FAMILY` を足した（[AUMID]）。 | §11.2、§12.2 |
| F34 | 案を換えた。文書の場所を `C:\Users\<名>\Settings` に移し、`C:\Users` を `STORE_SYSTEM` にする。これだけで迂回の道が消えるので、`STORE_FIXED` と `enum store_caller` と `SYS_SETTING_FOLDER` は要らない。書くのはカーネルだけである。`USERFOLDER` を層は読まない。 | §10.1、§4.6 |
| F35 | 案のまま。`settings-writer`、`self`、`any`。判定は持ち手と対象を見る。 | §12.1 |
| F36 | 案のまま。`reader`、秘密の保管ができるまで資格情報の鍵を登録しない。 | §12.3 |
| F37 | 案のまま。試行の値は overlay で、文書に書かない。写しに `trial` の印。 | §9.1 |
| F38 | 案のまま。`setting_trial_end`、`CONFIRM` は効いている時だけ。`screenmode.c` の試行の機械を消すので持ち主は一人である。 | §9 |
| F39 | 案のまま。`TRY` か読み戻しだけ、`RESET` も `TRY`、前の値を引き継ぐ、`CONFIRM` の制限。 | §9.1 |
| F40 | 案のまま。`trial_ms`、`trial_until`。 | §9.1、§6.1 |
| F41 | 案を換えた。OS 切・driver 入で全画面の VRR が動く形は案のまま。入・入の合成の面は `gsync-scope` で決める。[VRR] は OS の切り替えの効果を全画面のゲームに置くので、合成の面を動かさない。`nvrm` は `compositor` を読まず、`vrr_wanted` と `vrr` の二つの欄で話す。 | §13 |
| F42 | 案のまま。`device_class` を消す。`section`、`order`、領域の画像、知らない部品は型の既定。`section` の `app` は宣言に入れず、窓が L10 の登録から作る。 | §3.7、§8.2 |
| F43 | 案のまま。持ち主は `nvrm`、読み手は写しと `n_setting_cache`、present は atomic の変数だけ。定義の文を「値を効かせる者か、その家族」に書き直した。`nocturne_gl.c` の present は `nvrm_scanout_present` の所である。 | §2.2、§5.5 |
| F44 | 案のまま。規則に書き直した。`settingtest` は試験の領域だけを当てる。 | §2.2、§18 |
| F45 | 案のまま。四つの文に、変わった点と VRR の表と起動の項目を足して見せる。 | §21 |
| G1 | `N_SETTING_TEST`。 | §5.1、§6.1 |
| G2 | 一つの鍵に溜めて当てる（F14）。 | §14.1 |
| G3 | `forced`（F29）。 | §12.4 |
| G4 | 締切と `N_ERR_TIMEOUT`、読みに待ちが無い（F11）。 | §4.3 |
| G5 | 起動の項目「既定の設定で」。文書は残す。 | §10.5 |
| G6 | 窓の引数を `領域/鍵` に固定する。 | §8.4 |
| G7 | 五段目に回す。文書の形が符号化されていて、身元が機械をまたぐので後で足せる。今は作らない。理由：今の的（一コマ、持ち主、効き目）に効かず、当たる所が狭い。 | §18 |
| G8 | 作らない。起動の旗は層に入れない。診断の旗は残す。理由：`settings-ownership.md` の決定と AGENTS.md「システムの内部を引数や旗の起動で組まず」に沿い、利用者が使う口にしない。 | §15 の決C1 |

監査 §10 の「今変える部品」の 9 項目はこの設計に全部入っている。項目 1 は §4、項目 2 は §3.5、項目 3 は §3、項目 4 は §6、項目 5 は §5 と §9、項目 6 は §11 と §12、項目 7 は §14.1、項目 8 は §10.1、項目 9 は §21 である。

監査 §11 の「壊せなかった所」は、この設計でも同じ形で保つ。一コマの費用（§5.5）、`watched_changed` と同じ文脈の自由（§4.4）、保存の原子性（§4.6）、handle の close での死の検出（§5.4）、期限が窓に依らないこと（§9.1）、`Terminal` の子が権限を継がないこと（§12.2）、host の道具（機械を手にした者）、移植の Firefox が層を通らないこと（§14.9）、共有の文書が無いこと（§3.4）、shim が無いこと、休止（§10.2）である。

## 20. `settings-ownership.md` の直した行

材料の文書は書き換えない。この設計が取る形は次のとおりである。

| 行 | 材料 | この設計 | 理由 |
|---|---|---|---|
| 言葉：持ち主 | 値を効かせる部品 | 値を効かせる者か、その家族 | Mesa の値（監査 F43） |
| 言葉：scope の表 | 機械、利用者、装置、プログラム、起動 | 持ち手×対象。起動は層の外 | 監査 F27 |
| 解像度・周波数、配置、主画面、回転、HDR | 別々の行、持ち主は `screenmode.c` と `wm.c` | 一つの鍵 `display/arrangement`、持ち主は `screenmode.c` | [SDC]、[O5]、監査 F14。`wm.c` は読み手 |
| 主画面の出典 | 読めなかった | (0,0) の面が主画面 | [SRC] |
| 拡大 | scope は装置 | 利用者×モニター | 好みである。持ち手を足した |
| OS の VRR | 既定は切、scope は装置 | 同じ。§13 の表を足す | [VRR] |
| G-SYNC の入り切り | scope は装置 | 機械×モニター、親はモニターの型 | [NV] の同じ型の全部へ当てる選択 |
| Monitor Technology ほか driver の 3D | 全体とプログラム | 利用者と利用者×プログラム | 持ち手を決めた。好みの値で、ログインの前に要らない |
| 垂直同期、フレームの上限、低遅延、トリプル バッファ | 持ち主は user mode driver | 持ち主は `nvrm`、Mesa は読み手 | 監査 F43 |
| RM・NVKMS の引数 | 消す範囲は二段目で決める | 環境の道は消す。要る値だけ `owner-restart` の鍵 | 決C2 |
| アプリごとの GPU | 持ち主は OS | `graphics`、持ち主は `dev/gpu`、読み手は GL と Vulkan の初期化 | 持ち主の部品を名指しした |
| 音量と消音 | scope を装置へ | 利用者×出口 | 持ち手を決めた |
| アプリごとの音量 | scope はプログラム | 利用者×プログラム+出口 | [AS] の session は一つの出口に結ぶ |
| ポインターの速さ、加速、ダブルクリック | 持ち主は `wm.c` | 持ち主は `dev/input.c` | L5。入力の値を一つの部品に集める。`wm.c` は読み手 |
| 電源ボタン | 文書で確かめていない | [PB] で読んだ | [PB] |
| 電源の計画 | 持ち主は `dev/pm.c`、scope は機械 | 同じ。AC と DC は実体 | 監査 F27 |
| 電源の変化の通知 | 層の通知 | 読むだけの鍵 `power/source` | 監査 F18 |
| 手で時刻を変える | 持ち主は `sys/clock.c` | 同じ。`ACTION` | 監査 F18 |
| 地域の形式、言語 | 持ち主は OS | 持ち主は `sys/region.c` | 持ち主の部品を名指しした |
| 字体 | 起動設定 `FONT_DIR` は残すか決める | `appearance` の鍵。`FONT_DIR` は消す | AGENTS.md「引数や旗の起動で組まない」 |
| 起動の項目 | 持ち主は起動の部品 | 書く部品ができるまで登録しない | 監査 F18、[LIM] |
| 利用者と機械の分け方 | 機械の値は利用者のフォルダーに置かない | 同じ。場所は `C:\Nocturne\Settings` と `C:\Users\<名>\Settings` | §10.1 |
| Wi-Fi・VPN | 来たら同じ層へ | 秘密の保管の後。接続先は `nic+ssid` の実体 | [WLAN] |
| 通知の仕組み | 開く口を一つに | 領域ごとの見張り | 監査 F6 |

`audit-hardcoded.md` の決C1「設定の層の一時の上書き」は `settings-ownership.md` の「層に入れない」と食い違う。この設計は後者を取る（§15）。

## 21. 持ち主に見せて決めること

AGENTS.md「カーネルを変えずに済まない案なら書く前にユーザーに聞く」に当たる。実装の前に次を見せる。読んだ文書に持ち主の答えは無い。

1. 仲立ちはカーネルの小さな部品 `sys/setting.c` である。
2. 仲立ちは登録された領域の宣言と値の写しを持ち、文書へ保存する。持ち主は保存しない。
3. アプリの設定はカーネルを通らない。領域を持てるのはカーネルの部品と属性を持つシステムのプログラムだけである。これは `docs/records/settings-ownership-20261027.md` の決定 1「全部の設定を並べた表はどこにも作らない」の読み替えを含む。仲立ちの中に、登録された領域の宣言が並ぶ。
4. 持ち主の答え：この案は否決。持ち主の言葉は「Both are bad. Settings are not files.」である。§10 ほかの文書の場所と文書の形は作り直す。
5. §13.1 の VRR の表である。
6. §10.5 の起動の項目「既定の設定で」である。起動の画面に項目が一つ増え、カーネルの起動引数を一つ読む。
7. `C:\Users` を `STORE_SYSTEM` にする案は、4 の文書の場所のためのものなので、4 と一緒に作り直す。

持ち主が足した条件。この設計の全部がこれに従う。

- 設定は、適用のボタンが押された時に効く。持ち主の言葉は「Settings are applied when the apply button is pressed.」である。
- 再起動を要る設定は例外である。持ち主の言葉は「A reboot should never be needed in 99%of cases.」である。
- 全部をファイルとして扱う考えは禁止である。持ち主の言葉は「everything as a file is a BANNED concept.」である。

持ち主の指摘：「multiple layers are getting blurred right now. One is ui. One is system process, one is kernel. 3 different things」。この設計は三つを混ぜている。§0 の 3・4・12 と §2 の仲立ちは、値の写し・保存・試行・文法をカーネルに置く。作り直しでは、窓、システムのプロセス、カーネルの責務を分けて書き、適用のボタンは窓だけの事とする。

次は設計の中で決めきれなかったことである。

- `nvrm/*` の 3D の値を利用者の持ち手に置いた。NVIDIA の公式の文書に、全体の profile が機械か利用者かの記述を読めなかった。
- 無効な値の一覧に無い serial が重複する画面で、片方を抜くと残りの身元が変わる（§11.1）。これより良い形を見つけられなかった。
- 文字列の表の設計、秘密の保管の設計、アプリの登録（L10）の設計、利用者とセッション（L12）の設計はこの文書の外である。層はその受け口だけを持つ。

## 22. 限界

- 証拠の範囲は静的な読みと公式の文書である。ビルド、QEMU、実機を動かしていない。
- 読んだ版は `f2a489bd` である。
- `kernel/src/fs/document.c` は `place_may_change` の呼び手の 3 か所だけを読んだ。
- `ports/mesa/nocturne_gl.c` は present の場所だけを読んだ。
- NVKMS の `vrrType` は `nvkms-api.h` の型の宣言だけを読み、`nvrm` から読む道は読んでいない。
- `third_party/` の中はこの宣言のほかに読んでいない。どの OS のソースコードも読んでいない。
- Apple の Keychain の頁は本文を取れなかった。秘密を設定と分ける普通の出典は [WLAN] と [POL] を使った。
- Apple の音の装置の UID の持続は、公式の文書を見つけられなかった。音の出口の身元は `settings-ownership.md` の読みと [AS] で決めた。
- `CGBeginDisplayConfiguration` の頁は本文を読めなかった。[CG] は範囲の三つの頁で読んだ。
- AMD の頁は開いていない。`settings-ownership.md` の読みを使った。
- [HSA] は検索の抜粋で読んだ。
- [PB] の頁は短く、AC と DC の別はこの頁に書いていない。AC と DC は [W18] で読んだ。

## 23. 出典

- [DPI] Driver Package Isolation：https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/driver-isolation
- [DCH] DCH Design Principles and Best Practices：https://learn.microsoft.com/en-us/windows-hardware/drivers/develop/dch-principles-best-practices
- [HSA] Hardware Support App, Steps for Driver Developers：https://learn.microsoft.com/en-us/windows-hardware/drivers/devapps/hardware-support-app--hsa--steps-for-driver-developers
- [CF] CFPreferences, Preference Domains：https://developer.apple.com/library/archive/documentation/CoreFoundation/Conceptual/CFPreferences/Concepts/PreferenceDomains.html
- [CG] CGConfigureOption：https://developer.apple.com/documentation/coregraphics/cgconfigureoption.md ; forSession：https://developer.apple.com/tutorials/data/documentation/coregraphics/cgconfigureoption/forsession.md ; permanently：https://developer.apple.com/tutorials/data/documentation/coregraphics/cgconfigureoption/permanently.md ; forAppOnly：https://developer.apple.com/documentation/coregraphics/cgconfigureoption/forapponly.md
- [TDN] DISPLAYCONFIG_TARGET_DEVICE_NAME：https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_target_device_name
- [SDC] SetDisplayConfig：https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig
- [SRC] DISPLAYCONFIG_SOURCE_MODE：https://learn.microsoft.com/en-us/windows/win32/api/wingdi/ns-wingdi-displayconfig_source_mode
- [AUMID] Application User Model IDs：https://learn.microsoft.com/en-us/windows/win32/shell/appids
- [AS] Audio Sessions：https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions
- [RN] RegNotifyChangeKeyValue：https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regnotifychangekeyvalue
- [PB] Power button and lid settings：https://learn.microsoft.com/en-us/windows-hardware/customize/power-settings/power-button-and-lid-settings
- [LIM] Limine CONFIG.md：https://raw.githubusercontent.com/limine-bootloader/limine/v8.x/CONFIG.md
- [DNS] SetInterfaceDnsSettings：https://learn.microsoft.com/en-us/windows/win32/api/netioapi/nf-netioapi-setinterfacednssettings
- [WLAN] WlanGetProfile：https://learn.microsoft.com/en-us/windows/win32/api/wlanapi/nf-wlanapi-wlangetprofile
- [VRR] OS Variable Refresh Rate：https://devblogs.microsoft.com/directx/os-variable-refresh-rate/
- [NV] NVIDIA Control Panel, To set up G-SYNC：https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/nvdsp/To_use_variable_refresh_rates.htm
- [POL] Make a WinUI LOB app policy-aware：https://learn.microsoft.com/en-us/windows/apps/get-started/line-of-business/policy-aware-settings
- [SMT] SendMessageTimeoutW：https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeoutw

[O1]、[O5]、[O8] は `audit-settings-design.md` の §13 にある。[O5] はこの設計でも開いて読んだ（ChangeDisplaySettingsExW）。[W6]、[W8]、[W18]、[W19]、[W26]、[N3]、[N5]、[P1] は `settings-ownership.md` の出典の節にある。
