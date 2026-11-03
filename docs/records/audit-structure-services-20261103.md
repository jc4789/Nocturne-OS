# サービス、設定、シェル、GUI の土台の構造の監査（2026-11-03）

結論：

- 「サービスが全部見せかけ」とは、今のコードでは言えません。12 の項目のうち 9 つは本物の仕事をしています。
- 見せかけなのは「持ち主」という名乗りです。どのサービスも物を持っていません。持っているのは設定の鍵か、一つの機構です。
- 一番の根は三つです。機械全体の物の台帳が無いこと（S1）、保存の表が窓の形を決めること（S3）、値の間の関係を書く場所が無いこと（S4）。
- プログラムの導入の形はありません（S6）。今の台帳は C の表とパスを打つ欄です。プログラムの身元はパスの hash です。
- 窓の方針はカーネルに残っています（S11）。シェルは主画面を知りません（S10）。
- 全部が読むだけの判断です。ビルド、QEMU、実機では確かめていません。

## 1. 依頼文

あなたは Nocturne OS（作業場所 `D:\Programes\llm vibe slop os`）の構造の監査を一人で担当します。読むだけの監査です。今日は 2026-11-03 です。

### このプロジェクトの持ち主の言葉（原文）

"what we need to do now, is to continue to audit, so that we know what the problems are structurally. ... there are to many layers that keep geting blured or done halfassed and its become an issue. things are allways added on top, nothing is ever refracted when needed or anything ... everything seems likes its dont with a LOT of shortsightedness and no consideration as for why. ... this is a pervasive issue with how implementations are done, how code is written, and lack of consideration as to what purpose the code is serving."

"our serves seem to be for nothing more then theater."

### あなたの範囲：システムのサービス、設定、シェル、GUI の土台

- `user/apps` の `services.c`、`settingsd.c`、`settings.c`、`settings_table.h`、`session.c`、`shell.c`、`machine.c`、`display.c`、`audio.c`、`network.c`、`timesync.c`、`programs.c`、`accounts.c`、`systemlog.c`、`files.c`、`uihost.c`、`sysmon.c`
- `user/libc` の `services.c`、`settings.c`、`gui.c`、`ui.c`、`audio.c`、`language.c`、`keymap.c`
- `common/services_abi.h`、`settings_abi.h`、`display_abi.h`、`input_abi.h`、`audio_abi.h`、`authority_abi.h`
- 境界として `kernel/src/gui`、`kernel/src/sys/settings.c`、`service.c`、`authority.c`

答える問い：
1. サービスごとに：名乗っている持ち物。実際に決めていること。同じ決定がカーネルや別のサービスや窓に残っていないか。このサービスが無くなったら何が困るか。何も困らないなら、何のためにあるのか。消す、畳む、広げるのどれか。
2. 人が持っている物（モニター、スピーカー、マウス、キーボード、USB メモリー、網の装置、プログラム、文書、利用者）を持つ者は誰か。一つの物が複数の持ち主にまたがるとき（モニターは画面と音の両方に出る。USB の装置は入力にも音にもディスクにもなる）、どう扱っているか。機械全体の装置の一覧を持つ者はいるか。
3. プログラムの導入。入れる、消す、更新する、権利に同意する、という形（model）があるか。無いなら、今のプログラムの台帳は何か。
4. 重なる設定。OS の値、ドライバーの値、装置自身が変える値（モニターの OSD）が互いに変わるとき、それを書く場所はあるか。
5. 設定の層。宣言の表が何か所にあるか（`settings_table.h`、カーネルの `sys/settings.c`、`machine.c`、`display.c` など）。同じ値の持ち主が割れていないか。窓の形が保存の表からどう決まるか。
6. GUI の層。`ui.c`（3585 行）、`gui.c`、シェル、カーネルの `gui/wm.c` の間の持ち物の分け方。アプリの UI の形（`ui_msg`、`uihost`、TypeScript のアプリ）。同じ UI の仕組みが複数ないか。文の表（`settings_words.h`、`shell_words.h`）は言語が増えたらどうなるか。
7. 継ぎ足しの跡。`settings.c`、`shell.c`、`services.c`、`settingsd.c`、`ui.c` の履歴を `git log` で読む。古い道と新しい道が両方残っている所を挙げる。

### 守ること

- コードを変えない。ビルド、QEMU、Hyper-V を動かさない。git の状態を変えない。書いてよいのは下の報告のファイル一つだけ。
- 先に `AGENTS.md` と `.claude/rules/map.md` を読む。
- 今あるコードに敬意を払わない。コメント、ファイルの頭の説明、docs、関数の名前は主張であって証拠ではない。「〜の持ち主」という説明はとくに疑う。証拠は動くコードで、誰が呼ぶか、誰がその値を読むかで確かめる。
- 本物の仕事をしているサービスは、本物と書く。構造は正しいのに読みにくいだけの所と、構造が間違っている所を分ける。
- 行ごとの不具合を並べない。書くのは構造の問題（層、持ち主、目的、重複、継ぎ足し）だけ。
- codegraph（MCP の `codegraph_explore`）と rg で探す。`SYS_*` の番号での呼出しと表からの呼出しは索引に出ない。
- Linux や他の OS のソースは読まない。
- 対象の外：ブラウザーの中（`user/libc/web`、`user/apps/browser*`）。firmware の有無は話題にしない。特定のマシンに決め打ちした判断をしない。Windows の窓を写す案を書かない。問いは窓の見た目ではなく、裏の持ち物と目的。
- 既にある報告：`docs/records/audit-settings-20261103.md`、`docs/records/audit-display-20261102.md`、`docs/records/audit-gpu-generic-20261102.md`、`docs/records/audit-compositor-20261102.md`。同じ所見は節で引くだけにする。自分で読んで違っていれば、違うと書く。
- 全部を読めないなら、読んだ範囲と読まなかった範囲を正直に書く。推定は推定と書く。

### 報告

全文を `docs/records/audit-structure-services-20261103.md` に日本語で書く。Python で書くなら `newline='\n'` を付ける。節は次のとおり。

1. 依頼文（この文を全部そのまま）
2. 読んだ範囲と読まなかった範囲
3. サービスごとの判定の表（名乗り、実際、無くなったら困ること、判定）
4. 構造の問題。一つずつ：題、このコードの目的、今の持ち主とあるべき持ち主、証拠（`file:line`）、継ぎ足しの跡（コミットを見たなら hash）、消す範囲と書き直す範囲（ファイル、行数、数え方）、先に変える必要があるもの、確かさ（証明／可能性）
5. 残してよいもの（読んで構造が正しいと判断したものと、その理由）
6. 集計：部品ごとに、残す・書き直す・消すの行数の見積り
7. 依存の順序：どれを先に変えないと、どれを変えられないか

最後の返事は 15 行以内：ファイルのパス、重い問題の題、行数の集計。全文は返事に書かない。

## 2. 読んだ範囲と読まなかった範囲

動かしたものはありません。ビルド、QEMU、Hyper-V、実機のどれでも確かめていません。「証明」は、コードを読んで呼出しと読み手を辿れたという意味です。動きの観測ではありません。

全部を読んだファイル：

- `user/apps`：`services.c`、`settingsd.c`、`settings.c`、`settings_table.h`、`session.c`、`shell.c`、`shell_words.h`、`machine.c`、`display.c`、`timesync.c`、`programs.c`、`accounts.c`、`systemlog.c`、`uihost.c`
- `user/libc`：`services.c`、`settings.c`、`gui.c`
- `common`：`services_abi.h`、`settings_abi.h`、`display_abi.h`、`input_abi.h`、`authority_abi.h`
- `kernel/src/sys`：`settings.c`、`service.c`、`authority.c`
- `docs`：`audit-settings-20261103.md`、`boundaries-services-20261102.md`

一部だけ読んだファイル：

| ファイル | 読んだ所 |
|---|---|
| `user/apps/audio.c` | 1-260、739-800、1090-1320 |
| `user/apps/network.c` | 1-150、1200-1327 |
| `user/apps/files.c` | 1-80 と、rg で見た行 |
| `user/apps/sysmon.c` | 1-80 |
| `user/apps/settings_words.h` | 1-60、139-175、255-303 |
| `user/apps/shell_art.h` | 1-20 と、関数の名前 |
| `user/libc/ui.c` | 1-60、181-200、280-330 と、関数の一覧 |
| `user/libc/language.c` | 頭の部分と関数の一覧 |
| `user/libc/keymap.c` | 120-142 |
| `user/libc/audio.c` | 1-60 |
| `common/audio_abi.h` | 93-113 |
| `kernel/src/gui/wm.c` | 1124-1148、1596-1650、1950-2040、2396-2466、2644-2664、3092-3112、3163-3300 と、関数の一覧 |
| `kernel/src/gui/screenmode.c` | 1-40 |
| `docs/nocturne-native-abi.md` | §12 の頭。ほかの節は見出しだけ |
| `docs/records/audit-display-20261102.md` | 結論、D-09、D-10 |
| `docs/records/audit-gpu-generic-20261102.md` | F2、F6 |
| `docs/records/audit-compositor-20261102.md` | 結論、H1 |
| `docs/records/app-boundary-20261102.md` | 頭の 60 行 |
| `scripts/layout.py` | 25-40、76-110 |

読まなかったもの：

- `kernel/src/gui/pointer.c` の全部。`screenmode.c` と `wm.c` の残り。
- `user/libc/ui.c` の本体。ここの判断は関数の一覧によります。
- `audio.c` と `network.c` の中ほど。混合と DHCP を「本物」と書くのは、頭の部分と呼出しの形からの判断です。
- `ports/go/ui/ui.go`、`user/libc/web/js_ui.js`、`user/apps/agent.c`、`user/libc/webprefs.c`。rg と宣言の所だけを見ました。
- `kernel/src/dev` の中身。`hda_codec.c`、`usbmsc.c`、`input.c`、`pciuser.c`、`pm.c` は引いた行の近くだけを見ました。

git の履歴は `git log` と `git show --stat` と `git log -S` で読みました。差分の中身は読んでいません。継ぎ足しの跡の「何を変えたか」は、題と増減と `-S` の結果からの判断です。

前の監査と違う点：

- `audit-settings-20261103.md` は `accounts`、`timesync`、`machine` を「先頭の説明と宣言しか読んでいない」としていました。全部を読みました。この三つの説明は中身と合っています。`audio` は一部だけ読みました。
- 同じ監査の「`display.hold-seconds` は 0〜86400」は窓の表の値です。カーネルは `INT32_MAX` まで受けます（S2）。
- `docs/nocturne-native-abi.md` §12 の持ち主の表には、サービス `network` の鍵と `N_DOMAIN_TIME` がありません。同じ節の「誰が変えても知らせる（タスクバーのスピーカーも）」は今のコードと合いません（S15）。

## 3. サービスごとの判定の表

宣言の表 `user/apps/services.c:61-98` の 12 項目と、管理者です。

| サービス | 名乗り | 実際に決めていること | 同じ決定が別の所に | 無くなったら困ること | 判定 |
|---|---|---|---|---|---|
| 管理者 `services` | 全サービスの起動と管理 | 依存の順、backoff、能力の切り出し、名前の道。本物。 | 無い。確かめはカーネルの `authority.c` と分担で、重複ではない。 | 何も起きない。 | 残す。表の決め打ちを外へ出す（S14）。 |
| `systemlog` | log を C: へ | 69 行で一つの仕事。本物。 | 無い。 | `System.log` が書かれない。 | 残す。 |
| `gpu` | GPU のサービス | NVIDIA の表示の機能ごとに `nvrm` を起こす項目。GPU の一覧も選択も持たない（F2、F6）。 | 無い。選択の決定がそもそも無い。 | NVIDIA の GPU が使えない。 | 畳む。ドライバーの結びつけの表へ移す。GPU の持ち主は S1 の台帳。 |
| `accounts` | 利用者の持ち主 | PBKDF2、slot の記録、フォルダーの用意。本物。 | 無い。 | ログインができない。 | 広げる。口に削除、改名、合言葉の変更が無い（`services_abi.h:81-88`）。 |
| `settings` | 設定の持ち主 | 型と範囲の確かめ、crash に耐える保存、試し、見張り。保存は本物。「持ち主」は違う。 | 宣言と範囲がカーネル、`machine`、`display`、`audio` に割れている（S2）。 | 選択が全部消える。 | 残す。宣言と窓の形の部分を書き直す（S2、S3、S4）。 |
| `machine` | 電源の方針 | ボタン、蓋、放置、窓を閉じる段取り。本物。画面の読取りも同じ口にある。 | 段取りがシェルと `session` にもある（S7）。許可の確かめがシェルにもある。 | ボタンも放置も効かない。窓を閉じる段取りが無くなる。 | 残す。画面の読取りを出す（S8）。 |
| `programs` | プログラムの台帳 | C の表、admin が打った行、文書の種類ごとの既定。 | 置き場は `scripts/layout.py` が別に持つ。起こす道が別にある（S6）。 | 開始メニューと「開く」が空になる。 | 広げる。導入の形が要る（S6）。 |
| `tests` | 試験の媒体だけ | 試験の間だけ動く。製品の媒体には無い。 | 無い。 | 製品は困らない。 | 畳む。製品の表から出す（S14）。消すかは持ち主の判断。 |
| `session` | ログインとシェル | ログイン、シェルの起こし直し、停止。本物。一度に一つのセッション。 | 停止の段取りが三か所（S7）。 | 誰も入れない。 | 残す。ログインの窓を別のプロセスへ（S9）。 |
| `network` | 網の持ち主 | DHCP、固定の住所、経路、名前引き。本物（一部を読んだ判断）。 | TCP の容量はカーネル。proxy の読み手は `libc/net.c:248-252` で、サービスではない。窓は装置の一覧をカーネルから読む（S5）。 | 住所が取れない。 | 残す。領域の割れを閉じる（S2）。 |
| `timesync` | 時計を合わせる | SNTP、時計と RTC。本物。 | 時間帯は読む側が足す（`libc/time.c:66`、audit-settings）。 | 時計がずれる。 | 残す。口が無く、状態を問えない。口を足す。 |
| `display` | 画面の持ち主 | ドライバーが申し出たときのモードの選択と配置。倍率は運ぶだけ（D-15）。主画面はシェルに届かない（S10）。 | 配置の計算が窓とカーネルにもある。選び方が窓にもある（S5）。 | モードを申し出るドライバーのある機械で、モードが選ばれない（D-01）。無い機械では、配置の保存だけが無くなる。 | 広げる。口を持ち、判断を一か所に集める（S5）。 |
| `audio` | 音の持ち主 | 出口と形式の選択、変換と混合、音量。本物（一部を読んだ判断）。 | 無い。`N_AUDIO_*` の置き場だけが `settings_abi.h` のカーネルの節にある。 | 音が出ない。 | 残す。設定の接続の手書きを畳む（S2）。 |

シェルは宣言の項目ではありません。`session` が利用者の身元で起こす普通のプロセスで、タスクバー、開始メニュー、音量、通知、デスクトップの絵を持ちます。

持ち主の言葉「theater」への答え：

- 本物の仕事をしている項目は `systemlog`、`accounts`、`settings`、`machine`、`session`、`network`、`timesync`、`audio`、`programs` です。
- 口が全く無い項目は `display`、`timesync`、`systemlog`、`gpu`、`tests` です（`clients_from = CLIENTS_NONE`）。誰もこの五つに何も聞けません。
- `systemlog` に口が無いのは正しいです。`display` と `timesync` に口が無いので、窓はカーネルを直接読みます（S5）。
- 見せかけと呼べる名乗りは次の六つです。
  - `display` の倍率：描く側に読み手がありません（D-15）。
  - `display` の主画面：シェルが読みません（S10）。
  - `gpu` という名前：一社のドライバーの項目です（F2）。
  - `programs` の台帳：入れる、消す、更新するがありません（S6）。
  - `settings` の「持ち主」：自分で「領域の意味を持たない」と書いています（`settingsd.c:1`）。
  - `machine` と能力の絞り：能力を持たないアプリにも同じ力を配っています（S8）。

## 4. 構造の問題

問いと節の対応：

| 問い | 節 |
|---|---|
| 1 サービスごと | 3 節 |
| 2 物の持ち主 | S1 |
| 3 プログラムの導入 | S6 |
| 4 重なる設定 | S4 |
| 5 設定の層 | S2、S3 |
| 6 GUI の層 | S10、S11、S12、S13 |
| 7 継ぎ足しの跡 | 4 節の終わり |

行数の数え方：範囲は `file:line` の差か、関数の開始行の差です（`rg` で関数の頭を出して引きました）。「新しく書く」の行数は見積りで、確かな根拠はありません。

### S1. 機械全体の物の台帳が無い

- **目的：** 人が持っている物を一つの身元で数え、その物に何ができるかを一か所で答える。
- **今の持ち主：** いません。物は種類ごとの口に分かれ、互いにつながりません。
- **あるべき持ち主：** 一つのシステムのサービス。カーネルは発見と出来事を渡すだけにします。`display`、`audio`、入力、`network` は、台帳の身元の上に自分の面を持ちます。
- **証拠：**

| 物 | 一覧の口 | 身元 | 設定の持ち主 |
|---|---|---|---|
| モニター | `SYS_SCREEN_INFO` の `SCREEN_MODES_GET` | EDID の文字列に `#n`（`libc/gui.c:229-247`、D-17） | サービス `display` |
| スピーカー | `SYS_AUDIO_DEVICE_LIST`（能力が要る）、サービスの `N_AUDIO_OP_OUTPUTS` | `n_audio_device_key` | サービス `audio` |
| マウス、キーボード | `n_settings_effective(N_DOMAIN_INPUT, N_INPUT_DEVICE)`（`settings.c:613-647`、`sys/settings.c:37`） | `usb:vvvv:pppp:hash:i`（`input_abi.h:65`） | カーネルの `dev/input.c` |
| USB メモリー | 無い | 無い | 無い |
| 網の装置 | `SYS_NET_ADAPTERS` | MAC の文字列（`settings.c:360-369`） | サービス `network` とカーネル |
| GPU | 無い（F6） | 無い | `nvrm` |
| PCI の機能 | `SYS_PCILIST` | 無い | 無い |
| プログラム | サービス `programs` | パスの hash（S6） | サービス `programs` |
| 文書 | 無い。store のフォルダーと、種類ごとの既定 | パス | 無い |
| 利用者 | サービス `accounts` | 番号 | サービス `accounts` |

  - 身元の型は四つあり、どれも `n_setting_device`（`libc/settings.c:57-61`）の FNV-1a の 64 bit へ落ちます。身元の持ち主は設定の口です。
  - 入力の装置の一覧は、設定の実効値の形をしています。一覧と設定の区別がありません。
  - USB の装置の一覧はありません。`common` と `user` に USB の語が出るのは `input_abi.h` と `lspci.c` だけです。
- **一つの物がまたがるとき：**
  - モニターの音：HDA は ELD の名前を読みます（`hda_codec.c:319-324`、`382-414`）。画面の身元とのつながりはありません。人にとって同じモニターが、二つの無関係の項目になります。
  - USB メモリー：`usbmsc.c:313` が `blk_register` します。C: は起動の媒体を一つだけ mount します（`store.c:300-343`）。mount、取り外し、知らせの道はありません。差しても人に届きません。
  - USB の複合装置：入力は `usbhid`、ディスクは `usbmsc` で、同じ装置だと言う者がいません。USB の音の出口は `dev/` にありません。
  - AGENTS の「ディスクは一台」は起動の媒体の話と読めます。USB メモリーの扱いを決めた場所はありません。これは持ち主の判断が要ります。
- **継ぎ足しの跡：** 身元は領域ごとに作られ、最後に `n_setting_device` で一つの番号に揃えられました。物の型ではなく、設定の鍵の欄 `device` です。
- **消す範囲と書き直す範囲：**
  - 書き直す：`libc/gui.c:229-247`（19 行）、`libc/settings.c:57-61`（5 行）、`settings.c:360-369`（10 行）、`settings.c:613-737`（125 行）。計 159 行。
  - 新しく書く：サービスと libc の口と ABI で 1000〜2000 行（見積り）。
- **先に変えるもの：** モニターの身元（D-17）。ドライバーがつながりの手がかりを渡す口（ELD の名前と EDID、USB の装置の親）。
- **確かさ：** 一覧の持ち主が無いことは証明。USB メモリーの mount の道が無いことは証明（`store.c` と rg）。台帳の設計は提案です。

### S2. 宣言の表が六か所にあり、領域の中で持ち主が割れている

- **目的：** 一つの値について、型、範囲、持ち主、適用の仕方を一か所で決める。
- **今の持ち主：** 表ごとに別です。カーネルの鍵は二つの表に書かれ、値の範囲が違います。
- **あるべき持ち主：** 値を持つ者です。領域ごとに持ち主を一つにし、その者が宣言します。カーネルが持つ値の宣言は一つの生成元から作ります。
- **証拠（表の場所）：**

| 場所 | 件数 | 中身 |
|---|---|---|
| `user/apps/settings_table.h` | 31 | カーネルの鍵と、適用の無い鍵 |
| `kernel/src/sys/settings.c:22-41` | 18 | カーネルの鍵の型と範囲 |
| `user/apps/machine.c:36-45` | 8 | 電源の方針 |
| `user/apps/display.c:22-32` | 4 | 画面 |
| `user/apps/audio.c:1106-1118` | 6 | 音 |
| `user/libc/webprefs.c:356-`、`user/apps/agent.c:160-` | 中身は未読 | プログラムの鍵 |

- **証拠（同じ鍵の範囲の違い）：**

| 鍵 | カーネル | サービスの表 |
|---|---|---|
| `display.hold-seconds` | 0〜`INT32_MAX`（`sys/settings.c:28`） | 0〜86400（`settings_table.h:28-29`） |
| `input.layout` | INT、0〜`INT32_MAX`（`sys/settings.c:36`） | ENUM、最大 2（`settings_table.h:51`） |
| TCP の容量 | 下限 1（`sys/settings.c:39-40`） | 下限 4096（`settings_table.h:70-73`） |

  `tests/check_settings.py` は表を正規表現で読んでカーネルの enum と照らします。範囲の違いは調べません。

- **証拠（領域の割れ）：**
  - `N_DOMAIN_DISPLAY`：`hold-seconds` はカーネルの `pciuser.c`、残りはサービス `display`。
  - `N_DOMAIN_POWER`：`performance` は `arch/idle.c`、残りはサービス `machine`。
  - `N_DOMAIN_NETWORK`：TCP はカーネル、DNS と固定の住所はサービスが見張りで読む、proxy は `libc/net.c:248-252` が読む（サービスの `settings_event` は DNS と固定だけを見る）。
  - `N_DOMAIN_TIME`：時間帯は読む側が足します。
  - 鍵の番号を手で続けています：`N_DISPLAY_MODE = N_DISPLAY_KEY_COUNT`（`settings_abi.h:313`）、`N_POWER_BUTTON = N_POWER_KEY_COUNT`（`:320`）、`N_NETWORK_DNS_SERVER = N_NETWORK_KEY_COUNT`（`:171`）。カーネルの鍵とサービスの鍵が同じ領域に入った跡です。
  - `N_AUDIO_*` はサービスの鍵なのに、カーネルの節に残っています（`settings_abi.h:41-49`）。
- **証拠（持ち主の推測）：** `settingsd.c:1256` は `k->kernel = d->apply != N_APPLY_NONE` です。カーネルが持ち主かどうかを適用の型から推測しています。`apply_start`（`:644-652`）と `current_bytes`（`:814-836`）がこの推測で分かれます。
- **証拠（持ち主の側の作法が四通り）：**
  - `display` と `machine`：`n_setting_owner_open` を使い、設定の接続が切れると死んで起こし直されます（`display.c:429`、`machine.c:269`）。
  - `audio`：`n_setting_owner_open` を使わず、`settings_link`、`settings_relink`、`settings_handle`、`settings_event` を手で書きます（`audio.c:1123-1235`）。
  - `network`：見張りを開き直します（`network.c:1253-1258`）。
  - `timesync`：見張りを開き直します（`timesync.c:213-216`）。
- **継ぎ足しの跡：**
  - 74708779（11-01）で `settingsd.c` が 1178 行で入りました。
  - その後の d2ca6cee、6d1fa363、bb5286ac が、鍵をカーネルからサービスへ移すたびに持ち主の側の宣言を足しました（題と stat からの判断）。
  - 78d65017 が音の方針をカーネルから消しました（`git log -S N_AUDIO_VOLUME`）。
  - `n_setting_owner_open` は d2ca6cee で入りました（`-S`）。`settings_relink` は bb5286ac で入りました（`-S`）。d2ca6cee は bb5286ac の祖先ではありません（`git merge-base`）。音の枝は共通の口が無い所から建ち、6be72a3d で統合されました。統合の後も手書きのままです。
- **消す範囲と書き直す範囲：**
  - 書き直す：`settings_table.h` の全部（103 行）を持ち主の宣言へ。`sys/settings.c:22-41`（20 行）を生成へ。`settingsd.c` の推測の道（`:644-652`、`:814-836`、`:1256`、計 33 行）。
  - 消す：`audio.c:1123-1235` のうち約 100 行（113 行を `n_setting_owner_open` へ畳む見積り）。
- **先に変えるもの：** S15 の掃除。`display`、`network` の領域の持ち主の決定。
- **確かさ：** 表の数、範囲の違い、推測の行、`audio.c` の手書きの由来は証明。

### S3. 保存の表が窓の形と文を決める

`docs/records/audit-settings-20261103.md` の「根」を引きます。自分で読んで同じでした。`setting_card`（`settings.c:541-605`）が型で部品を決めます。ここにはその節に無いことだけを書きます。

- **目的：** 人が物と用事から設定を見つけて変える窓を作る。
- **今の持ち主：** 保存の表です。頁は領域の番号で分かれます（`settings.c:30-47`）。
- **あるべき持ち主：** 窓の形は窓が持ちます。物の情報は S1 の台帳とサービスの口から来ます。保存の表は窓を知りません。
- **証拠：**
  - `struct n_setting_decl` は `label`、`about`、`options` という窓の文を持ちます（`settings_abi.h:224-239`）。カーネルの値の範囲、保存の型、窓の文が一つの型に入っています。
  - `N_SETTING_DECL_CUSTOM`（`settings_abi.h:222`）と除外（`settings.c:1292-1293`）は、この形で出せない鍵を隠すための旗です。
  - 窓を作る道が二つあります。表から作る道と、手で書いた頁です：`screen_card`（`:896`）、`sound_cards`（`:839`）、`link_card`（`:1011`）、`services_page`（`:1060`）、`users_page`（`:1080`）、`programs_page`（`:1198`）、`input_cards`（`:680`）。
  - 手で書いた頁は、物を扱う所に集まっています（画面、音、網、入力、利用者、アプリ）。宣言の形では物を扱えないという読みです。
- **継ぎ足しの跡：** 74708779 が `settings.ts` 353 行を消して `settings.c` 831 行を入れました。88b12f11、d2ca6cee、6d1fa363、bb5286ac、e73494ae、e242325a、38b08a8a で二日のうちに 1617 行になりました。サービスを建てるたびに頁を足した形です。
- **消す範囲と書き直す範囲：**
  - 書き直す：`settings.c` の約 1200 行（`setting_card` 65 行、`show` 49 行、`on_event` 152 行、手で書いた頁と読み手の約 930 行）。
  - `settings_abi.h` の `n_setting_decl` から窓の文を出す（約 16 行）。
- **先に変えるもの：** S1、S2、S4、S5、S12、S13。窓の作り直しは最後です。
- **確かさ：** 証明。

### S4. 値の間の関係を書く場所が無い（重なる設定）

- **目的：** OS の値、ドライバーの値、装置が自分で変える値が互いに変わるとき、誰の値が今効いているかを持つ。
- **今の持ち主：** いません。D-09（正本が無い）、D-10（ドライバーの設定の置き場が無い）、同じ監査の 8 節と 10 節のとおりです。
- **あるべき持ち主：** 設定のサービスの値の型です。一つの鍵に「望み」「実際」「変えた者」を持たせ、宣言に他の鍵との関係を書けるようにします。装置の側の値は S1 の台帳から来ます。
- **証拠：**
  - `struct n_setting_decl` に他の鍵を指す欄がありません（`settings_abi.h:224-239`）。
  - `struct n_settings_changed` に変えた者の欄がありません（`settings_abi.h:304-308`）。
  - サービスが持つのは「望み」だけです。実際の値を読むのは持ち主ごとの手書きで、形がありません（`display.c` の `monitor_look`、`settings.c` の `screen_read`）。
  - 適用の外で変わる値の唯一の機構が `kernel_domain_changed`（`settingsd.c:1178-1206`）です。これは製品では値を動かしません（S15）。
  - カーネルの `settings_changed` の呼び手は、`wm.c:1133`、`idle.c:161`、`net.c:2169`、`pciuser.c:240`、`input.c:240`、`input.c:265` が全部 put の中です。残りは `input.c:75`、`86` の抜き差しです。
  - 階層があるのは入力だけです。`N_INPUT_SAME` の装置の記録が利用者の既定に従います（ABI §12）。領域ごとの手書きで、他の領域は使えません。
  - DDC や明るさの語が出るのは `ports/nvidia` と `pciuser.c` だけです（中身は未読）。OS に明るさの持ち主はありません。
- **継ぎ足しの跡：** 74708779 の `kernel_domain_changed` は音量がカーネルにあった時の形で、音量はその翌日にサービスへ移りました（78d65017）。
- **消す範囲と書き直す範囲：**
  - 書き直す：`settingsd.c` の値の記録と知らせ、`settings_abi.h` の二つの型、各持ち主の適用の答え。約 200 行（見積り）。
  - 新しく書く：300〜600 行（見積り）。
- **先に変えるもの：** S1（装置の値の出どころ）、S2（宣言の場所）。ドライバーの側は D-09 と D-10 の直し。
- **確かさ：** 場所が無いことは証明。モニターの OSD の変化が HPD や EDID を伴うかはメーカー次第で、確かめていません（D-09）。

### S5. 窓がサービスの判断を持ち、口の無いサービスの代わりにカーネルを読む

- **目的：** 画面の選択と配置の判断を一か所に置き、窓はそれを見せて頼むだけにする。
- **今の持ち主：** 窓、サービス `display`、カーネルの三者です。
- **あるべき持ち主：** サービス `display`。カーネルは指示された位置へ置くだけにします。窓はサービスの口へ聞きます。形の手本はサービス `audio` の `N_AUDIO_OP_OUTPUTS` と `N_AUDIO_OP_WATCH`（`audio_abi.h:97-98`）です。
- **証拠：**
  - 大きさの選び方：`settings.c:219-235` の `size_try` と、`display.c:100-134` の `nearest`、`mode_for`。
  - 配置の計算：`settings.c:254-282` の `screen_place`、`display.c:194-276` の `place_one`、`arrange`、`wm.c:1601-1628` の `layout`（置かれていない画面を右に並べる）。
  - 試しの期限：窓の `trial_start`（`settings.c:203-211`）と見張り（`:1562-1568`）の両方で持ちます。サービスも持ちます（`settingsd.c:64-70`）。
  - 窓がカーネルを直接読みます：`n_screen_modes_get`、`n_net_adapters`。`display` は口が無いので、聞く先がありません。
  - シェルも電源の許可を自分で確かめます（`shell.c:527-535` と `machine.c:180`）。
  - 事情を述べる文も窓にあります：`W_R_DRIVER`、`W_R_HANDED_BACK`、`W_R_STOPPED`、`W_R_REMOTE`（`settings_words.h`）。ここまでは窓の仕事として正しいです。判断だけが窓にあるのが問題です。
- **継ぎ足しの跡：** d2ca6cee が `display.c` を足し（`--diff-filter=A`）、カーネルの `screenmode.c` の方針をサービスへ移しました（230 行の変更）。正しい移動です。窓の判断が今も窓にあるので、このとき移らなかったか、後で足されたかのどちらかです。差分は読んでいません。
- **消す範囲と書き直す範囲：**
  - 消す：`settings.c` の `size_try`（17 行）、`screen_place`（29 行）、`trial_start`（9 行）、`mode_try`（7 行）、`scale_try`（13 行）、`place_set`（5 行）、`screen_make_main`（11 行）。計 91 行を口の呼出しへ置き換えます。
  - 書き直す：`display.c` に口を足す。`wm.c` の `layout` の第二の輪（約 7 行）は、未配置の画面の既定を残すか決めます。
  - 新しく書く：`display` の口で 300〜500 行（見積り）。
- **先に変えるもの：** S1（画面の身元）、S4（望みと実際）。
- **確かさ：** 重複は証明。`wm.c` の第二の輪を消せるかは未確認です。サービスが立つ前の画面に要る見込みです。

### S6. プログラムの導入の形が無い

- **目的：** プログラムを入れる、消す、更新する、権利に同意する。プログラムの身元、持ち物、設定、文書の種類を一つの記録にする。
- **今の持ち主：** いません。今の台帳は次の組み合わせです。
  - `programs.c:20-46` の `system_programs[]`：C の表です。
  - `settings.c:1176-1196` と `programs.c:322-345`：admin が名前とパスと種類を打つ行です。パスは `C:\Programs` の中だけです（`:324`）。
  - `scripts/layout.py:31-36` の `APPS`：ビルドの時に置く場所の表です。`programs.c` と手で揃えます。
- **あるべき持ち主：** サービス `programs` を導入の持ち主へ広げます。ファイルを置く、記録する、消す、更新する、版と出どころと権利を持つ、という仕事です。C の表は媒体に入れる記録へ置き換えます。
- **証拠：**
  - 記録の型は `RECORD_PROGRAM`、`CLAIM`、`CHOICE` です（`programs.c:50-56`）。版、出どころ、権利の欄はありません。
  - `program_remove`（`programs.c:356-378`）は行を消すだけです。ファイルも設定も消しません。
  - `rg` で `install`、`uninstall`、`license`、`manifest`、`package` を探しても、無関係の語だけです。`C:\Nocturne\Licenses` はビルドが置く文書のフォルダーです（`layout.py`）。
  - プログラムの身元はパスの hash です：`store_path_number`（`kernel/src/fs/store.c:668-688`、FNV-1a）を `authority.c:51` が使います。フォルダーを動かすと別のプログラムになります。`N_SCOPE_PROGRAM` の値（`settingsd.c:995`）はその番号で引くので、選択を失います。
  - 名前からパスを探す `find_program`（`libc/sys.c:632-643`）は `C:\Programs` の全フォルダーを歩きます。同じ名前が二つあれば、先に見つかった方です。登録と無関係です。
  - 起こす道が二つあります：`gui_launch`（`libc/gui.c:199-209`、`shell.c:785`、`files.c:276`、`js.c:774`）と `n_program_run`（`libc/services.c:270-278`）。
  - 印は `enum n_icon`（`services_abi.h:122-127`）です。第三者は `N_ICON_GENERIC` しか使えません。プログラムが自分の印を持てません。
  - プログラムの名前の文は `programs.c:21` の列で、システムのプログラムだけが言語ごとの名を持てます。
  - 世代の刻印 `.note.nocturne` と `N_ERR_WRONG_GENERATION`（b8f0448a、`docs/records/app-boundary-20261102.md`）は、古いプログラムを拒みます。拒まれた後に更新する道がありません。入口の境界だけが先に建っています。
  - 普通の利用者が `C:\Programs` に置けるかは未確認です。
- **継ぎ足しの跡：** `programs.c` は e73494ae（11-02）で入りました（`--diff-filter=A`）。題は「プログラムの台帳とセッションの停止を入れ」です。読み手は開始メニュー、「開く」、設定の頁です。b8f0448a の入口の境界はその後の変更で、導入の形と結ばれていません（順序は `shell.c` の履歴の並びからの判断です）。
- **消す範囲と書き直す範囲：**
  - 書き直す：`programs.c` の約 307 行（表 27 行、記録 7 行、足すと消す 57 行、読み手の残りの見積り 216 行）。`layout.py` の `APPS`（6 行）。`find_program`（12 行）。
  - 新しく書く：2000〜4000 行（見積り。形式、検査、置き場、更新、権利の窓を含みます）。
- **先に変えるもの：** プログラムの身元をパスから離す（`authority.c:51`、カーネルの変更）。S13（プログラムが自分の文を持つ）。何を権利とするか、誰が入れてよいかは持ち主の判断です。
- **確かさ：** 形が無いことは証明。身元とパスの関係は証明。

### S7. 窓を閉じる段取りが三か所にあり、電源の文がカーネルを通る

- **目的：** 電源の断やログアウトの前に、開いている窓へ閉じる機会を与え、答えない者を人に見せる。
- **今の持ち主：** シェル、`machine`、`session` の三者です。
- **あるべき持ち主：** `machine` を唯一の持ち主にします。ログアウトも同じ段取りを呼びます。文はサービスからシェルへ channel で届けます。カーネルは窓の数と閉じる知らせの機構だけです。
- **証拠：**
  - シェル：`logout_begin`、`logout_follow`、`logout_stalled`（`shell.c:984-1028`）、`LOGOUT_STALL_MS`（`:36`）。
  - `machine`：`closing_look`、`closing_deadline`、`closing_end`（`machine.c:103-176`）。
  - `session`：`n_session_stop` の待ちの poll（`session.c:169-172`）。
  - カーネルの機構：`wm_window_count`、`wm_close_windows`（`wm.c:2644-2662`）。
  - 文の道：`SYS_POWER_SAY`（`abi.h:196`、`pm.c:157`）から `SYS_POWER_STATE`、`SYS_POWER_WATCH`（`abi.h:185-186`）を通ってシェルの `power_changed`（`shell.c:945`）へ届きます。カーネルが人への文を中継します。シェルには channel の受け口があります（`peer_add`、`inlet_event`、`shell.c:1029-1069`）。
- **継ぎ足しの跡：** `closing_look` と `SYS_POWER_SAY` は d2ca6cee で入りました（`-S`）。`session` の停止は e73494ae の題にあります。シェルの `logout_begin` の由来は `-S` で引けませんでした。どの順で重なったかは確かめていません。
- **消す範囲と書き直す範囲：**
  - 消す：`shell.c:984-1028`（45 行）、カーネルの `SYS_POWER_SAY` の中継（見積り 40 行。`pm.c` は引いた行の近くだけ読みました）。
  - 書き直す：`machine.c:103-176` を両方の呼び手に使える形へ（約 30 行）。`session.c:169-172`。
- **先に変えるもの：** 無し。独立に直せます。
- **確かさ：** 三か所あることは証明。

### S8. `machine` の寄せ集めと、能力の絞りが見せかけ

- **目的：** 電源の断と画面の読取りを、許された者だけに許す。設定の鍵の持ち主を守る。
- **今の持ち主：** `machine` が同じ口で全部を判定します。設定の鍵の持ち主は最後の宣言者です。
- **あるべき持ち主：** 画面の読取りは画面の持ち主（`N_CAP_SCREEN_READ` を持つ者）で、許可の持ち主は呼ぶ者の能力です。鍵の持ち主は最初の宣言か管理者の表で決めます。
- **証拠：**
  - `machine.c:369`：`allowed = peer.session != 0 || cls >= ADMIN`。セッションの中の全プロセスが電源を切れて、画面を読めます。
  - `N_MACHINE_SCREEN`（`machine.c:282-302`）の呼び手は `agent.c:660` と `screenshot.c:35` です。電源の方針と関係がありません。
  - カーネルの能力は本物です（`authority.c`）。その手前でサービスが能力を持たないアプリへ同じ力を配ります。
  - `answer_declare`（`settingsd.c:871-906`）：`N_CLASS_ADMIN` 以上なら宣言でき（`:889`）、断るのはカーネルの鍵だけです（`:894`）。`k->owner = c`（`:903`）なので、admin の級のプロセスが既存の鍵の持ち主を奪えます。
- **継ぎ足しの跡：** 差分は未読です。
- **消す範囲と書き直す範囲：**
  - 消す：`machine.c` の画面の読取り（`screen_pixels` 22 行と受け側で約 40 行）。
  - 書き直す：`machine.c:369` の許可（約 15 行）、`answer_declare`（36 行）。
  - 呼び手：`agent.c`、`screenshot.c`。
- **先に変えるもの：** 無し。画面の読取りの新しい持ち主だけを先に決めます。
- **確かさ：** `machine` の許可は証明。持ち主を奪えることはコードを読んだ可能性で、動かしていません。

### S9. `session` に特権と UI が同居する

- **目的：** 誰かを認証して、身元を与えてシェルを起こす。
- **今の持ち主：** `session` が `N_CAP_IDENTITY_GRANT` と `N_CAP_SHELL` を持ち、同じプロセスでログインの窓を描きます。
- **あるべき持ち主：** 身元の付与は `session` に残します。ログインの窓は特権の無い別のプロセスにし、入力だけを `session` へ渡します。
- **証拠：**
  - ログインの窓が `session.c:196-324` にあり、文の表が `:20-32` にあります。
  - システムの級のプロセスに、`ui.c`（3584 行）と字体の処理が同居します。
  - 事象の宛先を場所で計算します（`session.c:204,217,298`、S12）。
  - 一度に一つのセッションです。複数の利用者の切り替えは、今の形では足せません。これは今の目的の外です。
- **継ぎ足しの跡：** `session.c` は 16310d67（11-01）で入りました（`--diff-filter=A`）。ログインの窓が最初から同居していたかは、差分を読んでいません。
- **消す範囲と書き直す範囲：**
  - 移す：`session.c:196-324`（129 行）と文の表（13 行）を新しいプロセスへ。
  - 新しく書く：`session` との文で約 100 行（見積り）。
- **先に変えるもの：** 無し。
- **確かさ：** 同居は証明。危険の大きさは測っていません。可能性です。

### S10. シェルがエンジンを使わず、主画面を知らない

- **目的：** タスクバーとデスクトップを、人が選んだ画面の配置に合わせて出す。
- **今の持ち主：** 主画面はカーネルの `primary_screen`（`wm.c:506`、`:2428`）です。タスクバーの場所はシェルの `list[0]` です。
- **あるべき持ち主：** 主画面の持ち主はサービス `display` です。シェルはそれを読みます。
- **証拠（主画面）：**
  - `struct n_shell_screen`（`display_abi.h:133-138`）に主画面の欄がありません。`shell_screens`（`wm.c:3217-3234`）も入れません。
  - シェルは `list[0]` にタスクバーを置きます（`shell.c:847-851`、`bar_fit` `:454-476`、`:277`、`:805`）。
  - カーネルの `screen_main()`（`wm.c:509-513`）は `primary_screen` を使います。読み手は `work_area`（`:589`）、`layout`（`:1621`）、`handle_mouse` の中（`:2153`）です。
  - 別の口 `struct screen_modes` には `primary` があります（`wm.c:2459`）。シェルはこの欄を読みません。
  - だから主画面を選ぶと、窓の戻り先は動き、タスクバーは動きません。これはコードからの判断です。動かしていません。
- **証拠（二系統）：**
  - 印：`ui.c:1262` の `draw_icon`（文字列）と `shell_art.h:208` の `art_icon`（enum）。
  - 色：`ui.h` の固定の `ui_theme` と、シェルの `RGB()` の決め打ち（`shell.c:417-419`、`:493`）。テーマは設定にありません。
  - 字：シェルは `gfx_text`、`ui.c` は `font.h` です（`ui.c` の本体は未読で、推定）。
  - 直接 `win_open` と `gfx` を使うのは `shell.c`、`term.c`、`mandel.c`、`gpu_demo.c`、`browser.c` です。残りはエンジンです。
- **判断：** シェルが自分で描くこと自体は、タスクバーと壁紙については誤りと言えません。開始メニュー、音量、通知は普通の UI で、色と印と字が二系統になる原因です。
- **継ぎ足しの跡：** d383c2d3、88b12f11、d2ca6cee、bb5286ac、e73494ae（150 行）、e242325a（60 行）、b8f0448a。シェルは `desktop.c`（16310d67）から来た見込みで、未確認です。
- **消す範囲と書き直す範囲：**
  - 書き直す：`screens_changed`（39 行）、`bar_fit`（23 行）、`menu_*`（157 行）、`volume_*`（89 行）、`toast_*`（60 行）、印と色の見直しで残りの約 22 行。計約 390 行。
  - `struct n_shell_screen` に欄を足す。
- **先に変えるもの：** S5（`display` の口）。エンジンへ移すなら S12。
- **確かさ：** 主画面の割れは証明。エンジンへ移す価値は持ち主の判断です。

### S11. カーネルが窓の方針を持つ

- **目的：** 題の棒、ボタン、Alt+Tab、ドラッグ、最大化など、人が見て触る窓の振る舞いを決める。
- **今の持ち主：** カーネルの `gui/wm.c` です。合成も同じ場所です（audit-compositor の H1）。
- **あるべき持ち主：** 方針はユーザー空間です（シェルか、窓の管理のプロセス）。カーネルに残すなら、面の管理と提示の機構だけです。AGENTS の「層を分ける」に従います。
- **証拠：**
  - 題の棒とボタンの描画：`draw_layer_shadow`、`draw_layer`（`wm.c:1152-1242`）。アプリの文を核が描きます（`text_trunc` `:1056`）。
  - 鍵の方針：`handle_key` の Alt+Tab と Alt+F4（`:1972-1992`）、`hotkey_take`（`:1955`）。
  - 窓の操作：`toggle_maximize`（`:1904`）、`cycle_windows`（`:1928`）、`count_click`（`:2059`）、`left_press`、`left_release`（`:2070-2139`）。
  - クリップボード：`:2299-2342`。カーネルは文書の貼り付けの持ち主になっています。
  - 遠隔の大きさ：`:2469-2596`。`vm/rdp.c` が呼びます。実機の道にある VM の都合かは判定していません。
  - シェルの口 `SYS_SHELL_*`（`:3166-`）は、窓の一覧と配置を渡しています。シェルは「見る者」で、方針を決める者ではありません。`SYS_SHELL_HOTKEY` だけが鍵を渡します。
  - 地図の `gui/` の行は「文を言わず」と書きます。題の棒の文を核が描くのは、アプリの文の中継で、自分の文ではありません。線としては保たれています。
- **継ぎ足しの跡：** 差分は未読です。今の位置は最初の窓の管理から続いていると推定します。
- **消す範囲と書き直す範囲：**
  - 移す：計約 322 行（描画 91、`text_trunc` の周り 55、最大化 24、Alt+Tab 27、鍵 44、押下とドラッグ 81）。
  - クリップボード 44 行を持ち主の決定に従って移します。
  - 合成の本体は S11 の範囲に入れません。
- **先に変えるもの：** 合成をカーネルから出すかの設計（H1）。S10（シェルの口）。これは一番後です。
- **確かさ：** カーネルに方針があることは証明。移す先と行数は見積りです。

### S12. UI の事象が場所で宛先を指す

- **目的：** アプリが UI の木を送り、人の操作がどの部品に起きたかを受け取る。
- **今の持ち主：** エンジン `ui.c` の pre-order の場所です。対応の計算はアプリごとにあります。
- **あるべき持ち主：** エンジンです。部品の `key` を事象に載せて返します。
- **証拠：**
  - 事象の `e.node`、`e->node` が 89 か所あります（rg）。
  - `settings.c` の対応表：`sent[VIEWS]`、`act_at`、`action_of`（`:17`、`:61-65`、`:489-498`、`:1360-1365`）。
  - `files.c`：`ROW_NODES`、`first_row`（`:20`、`:41`）。
  - `session.c`：`first_item`（`:204`、`:217`、`:298`）。
  - `key` は木の差分に使いますが、事象に戻りません。
  - 木に node を一つ足すと、後ろの全部の宛先が動きます。Go（`ui.go`）と JS（`js_ui.js`）も同じ文の形式を使います（中身は未読）。
- **継ぎ足しの跡：** 1dd72cf3（10-05）から 33fbe945（旧 imgui を消して一つのエンジンへ）の後も場所の形が続きました。差分は未読です。
- **消す範囲と書き直す範囲：**
  - 書き直す：`ui.c` の事象の部分（約 100 行の見積り）、`settings.c` の対応表（約 40 行、S3 に含む）、呼び手 89 か所のうち場所を計算している所。
  - ABI §11 の文の形式を変えます。
- **先に変えるもの：** 無し。S3 より先に行います。
- **確かさ：** 形は証明。`ui.c` の本体は未読で、書き直しの行数は見積りです。

### S13. 文の表がコンパイル時に閉じている

- **目的：** 人の言語で全部の文を出す。
- **今の持ち主：** 各プログラムの C の表と、宣言の中の日本語です。
- **あるべき持ち主：** プログラムごとの文の記録で、実行時に読みます。言語の番号は ABI の enum ではなく、言語の札にします。読む側は `libc/language.c` です。
- **言語を一つ足すときに変える所：**
  - `enum n_language`（`settings_abi.h:190`）：ABI の変更。`enum n_region`（`:191`）も同じです。
  - `shell_words.h`（`[N_LANGUAGE_COUNT]`、`:16`、44 行）。
  - `settings_words.h`（`:139`、130 行）。
  - `settings_english`（`settings_words.h`、28 行）と `english_row`（`settings.c:507-512`）。英語だけの特例です。
  - `session.c:21` と `programs.c:21` の列。
  - `language.c:41`（札）、`:89`（`n_locale_intl`）、`:92-107`。
  - 選択肢の文：`settings_table.h:85`、`:100` の `"日本語\0English"` と `.max=1`。
  - 宣言の `label` と `about`：`settings_table.h`、`machine.c`、`display.c`、`audio.c`、`webprefs.c`、`agent.c`。全部日本語だけです。
- **証拠：**
  - 宣言は 49 件以上あり（S2）、英語の行は 28 件です。無い鍵は英語の画面でも日本語で出ます（`setting_words`、`settings.c:515-519`）。
  - 英語の行に `browser.languages` があります。システムの窓の表がプログラムの文を持っています。プログラムが自分の宣言の文を訳す道はありません。
  - 足さなかった言語の列は NULL になる見込みです（C の初期化からの推定）。
  - `n_language_now` を使うのは `language.c`、`services.c`、`settings.c`、`shell.c` だけです。`session` は機械の鍵を読みます。
  - `files.c`（`:52` など）、`sysmon.c` の文は英語の固定です。`n_result_text` も英語です（`boundaries-services-20261102.md`）。
  - `nocturne_language.h:4` は「文の表はプログラムごとに持つ」と言います。第三者が訳を足す道はありません。
- **継ぎ足しの跡：** 差分は未読です。
- **消す範囲と書き直す範囲：**
  - 書き直す（文は残り、形が変わる）：`settings_words.h` 303 行、`shell_words.h` 62 行、`session.c` 13 行、`programs.c` 表、`language.c` の約 57 行、`libc/settings.c` の約 18 行。
  - `settings_table.h:85`、`:100` を言語の一覧の読み手へ。
  - 新しく書く：300〜500 行（見積り）。
- **先に変えるもの：** S2（宣言から文を出す先）。S3 より先です。
- **確かさ：** 証明。NULL の件は推定です。

### S14. 製品の宣言に試験と NVIDIA が入っている

- **目的：** 起こすサービスと、その能力と依存を決める。
- **今の持ち主：** 管理者の C の表です（`services.c:61-98`）。
- **あるべき持ち主：** 媒体にある宣言の記録です。製品の媒体の記録に試験は入りません。ドライバーは装置との結びつきの記録で選びます。
- **証拠：**
  - F2 を引きます：`services.c:24`、`:59`、`:65-67`。自分で読んで同じでした。
  - `tests`：`services.c:80-82`。`N_CAP_POWER`、`N_CAP_PCI_CLAIM`、`N_CAP_SCREEN_READ`、`N_CAP_IDENTITY_GRANT` を持ちます。
  - 製品の `session` が `tests` に依存します（`services.c:84`）。`optional` なので無ければ満たされたとみなします（`:45`、`:319`、`:340`）。
  - コメントは「C:\Nocturne\System の中にあるので、アプリは置けない」と言います。`C:\Nocturne\System` に置ける者は、製品でも四つの能力を得ます。可能性です。
  - `instances_from_table` は起動の一度だけです（`services.c:303-310`）。装置が後から現れても増えません。
  - `change_instance`（`:625-645`）は依存も重要さも見ません。管理者の確かめは `:695` です。
- **継ぎ足しの跡：** 661db797 が「呼ぶ者の無くなった試験の部品を消す」、d927e533 がその「削除を戻す」です。同じ 11-02 です。何が戻ったかの差分は読んでいません。
- **消す範囲と書き直す範囲：**
  - 消す：`services.c:24`、`:59`（`nvrm.h` と `nvidia_display`）、`:80-82`、`:84` の `tests`。約 15 行。
  - 書き直す：表 38 行を記録と読み手へ。約 70 行。
- **先に変えるもの：** 試験の媒体の起こし方の決定。F2 の直し。
- **確かさ：** 証明。

### S15. 死んだ道の残り

- **目的：** 無し。移動の後の掃除が無かった跡です。
- **証拠：**

| 残り | 場所 | 読んだこと |
|---|---|---|
| `kernel_domain_changed` | `settingsd.c:1178-1206` | 「タスクバーの音量など」の値は 78d65017 でサービスへ移りました。カーネルの `settings_changed` は put と抜き差しだけなので、今の値と保存の値が違わない限り何もしません。違えば、カーネルの今の値を利用者の選択として保存します。試験の旗（audit-settings）が値を変えると、そこで保存される可能性があります。 |
| `settings_syscall_describe` | `sys/settings.c:113-118`、`libc/sys.c:588`、`nocturne.h:330`、ABI §12 | 呼び手は宣言と包みだけです（`rg`）。 |
| ENDPOINT の道 | `sys/settings.c:19-20`、`:89-92` | カーネルの表に ENDPOINT の項目がありません。 |
| `session_changed` の第二の輪 | `settingsd.c:923-934` | 第一の輪が `apply != NONE` の鍵を全部当てます。`apply == NONE` の鍵は `apply_start` が何もせず 0 を返します（`:645`）。第二の輪は同じ既定をもう一度送るだけです。 |
| `SCREEN_SETTINGS_GET`、`struct desktop_settings` | `display_abi.h:24-33`、`wm.c:3099-3107` | `user/` に読み手がありません（`rg`）。 |
| `SYS_SCREEN_INFO` の多重 | `display_abi.h:13-15`、`:28`、`:50`、`:57`、`libc/gui.c:12,17,25,28,226` | 引数で四つの口に分かれます。「existing programs are unaffected」は互換の跡で、AGENTS の「互換は持たない」と合いません。 |
| 空の `enum n_time_key` | `settings_abi.h:94-96` | 中身がありません。 |
| `N_AUDIO_*` の置き場 | `settings_abi.h:41-49` | カーネルの節にあります。S2 です。 |
| ABI §12 の文 | `docs/nocturne-native-abi.md` | 「タスクバーのスピーカーも」と書かれています。`settings_abi.h:9-10` も同じです。 |

- **継ぎ足しの跡：** `kernel_domain_changed` は 74708779（11-01）に入りました（`-S`）。`N_AUDIO_VOLUME` は 78d65017（11-02）でカーネルの表から消えました（`-S`）。
- **消す範囲と書き直す範囲：**
  - 消す：`kernel_domain_changed` と呼出しと見張りの開き（約 35 行）、`describe` の道（約 12 行）、ENDPOINT（6 行）、第二の輪（12 行）、`desktop_settings`（約 20 行）、空の enum（3 行）。計約 88 行。
  - 書き直す：`SYS_SCREEN_INFO` を四つの番号へ（約 30 行）。
- **先に変えるもの：** 無し。最初に行います。ただし `kernel_domain_changed` を消す前に、S4 で「適用の外で値が変わる」道の形を決めます。形が決まるまでは残しても害は小さいです。
- **確かさ：** 呼び手が無いことは `rg` で証明。`SYS_*` の番号で直接呼ぶ者は `SYS_SETTINGS_DESCRIBE` も `rg` で探しました。

### 継ぎ足しの跡（問い 7）

「何も作り直されない」は、履歴とは合いません。作り直しはあります。

| コミット | 作り直したもの |
|---|---|
| 16310d67 | init を消してサービスの管理者を建てた |
| 33fbe945 | 旧 imgui を消して UI のエンジンを一つにした |
| 74708779 | `settings.ts` を消してサービス `settings` と窓を建てた |
| d2ca6cee | `screenmode.c` の方針をサービスへ移した |
| 78d65017 | 音の方針をカーネルから消した |
| 0e7b7465 | 文字の持ち主を `keymap.c` にした |

足りないのは、移動の後の掃除と、呼ぶ側の作り直しです。

- 機構を移した後に、古い説明と道が残ります（S15）。
- 判断は移っても、窓の中の同じ判断は残ります（S5）。
- 新しいサービスを建てるたびに、同じ窓のファイルへ頁を足します（S3）。
- 別の枝で建てたサービスが、統合の後も共通の口へ揃いません（S2 の `audio`）。

古い道と新しい道が両方ある所：

| 仕事 | 道 |
|---|---|
| プログラムを起こす | `gui_launch` と `n_program_run` |
| 設定の持ち主の接続 | `n_setting_owner_open` と `audio.c` の手書き、見張りの開き直し |
| 窓を閉じる段取り | シェル、`machine`、`session` |
| 配置の計算 | 窓、`display`、`wm.c` |
| 画面の情報 | `SYS_SCREEN_INFO` の四つの形 |
| 窓の形 | 表から作る頁と手で書いた頁 |
| 印 | `draw_icon` と `art_icon` |
| 色 | `ui_theme` と `RGB()` の決め打ち |
| 言語の文 | `settings_words` の列と `settings_english` の名前引き |
| 試験の部品 | 661db797 と d927e533（消して戻した） |

`git log` で見た主なコミット：

- `settings.c`：18be8422、33fbe945、74708779、88b12f11、d2ca6cee、6d1fa363、bb5286ac、e73494ae、e242325a、38b08a8a。
- `settingsd.c`：74708779（1178 行）、3a4e8df8、0b8d6f63、88b12f11、d2ca6cee、6d1fa363、bb5286ac、e242325a、c51812ff。
- `services.c`：16310d67（788 行）、d2ca6cee、6d1fa363、bb5286ac、e73494ae、c51812ff、661db797、d927e533、b8f0448a。
- `shell.c`：d383c2d3、88b12f11、d2ca6cee、bb5286ac、e73494ae、e242325a、b8f0448a。
- `ui.c`：1dd72cf3、33fbe945、09b19376、b3ccf549、97b7d802、b13cfa79、c31dddde、b2f5b721、a51c4398、ea966154、a3285076、8837d5bd、5461059d、8a776a7c、0e7b7465。

## 5. 残してよいもの

構造が正しいと読んだものです。

- **名前の道と能力の切り出し**（`services.c`、`sys/service.c` 60 行、`sys/authority.c` 121 行）。カーネルは名前を解きません。能力は handle で、持つことが権限です。
- **管理者の依存と backoff**（`services.c`）。表の置き場だけが問題です。
- **設定の保存と試し**（`settingsd.c:308-320`、`420-493`、`64-70`、`671-681`）。二つの版の枠と CRC と、期限つきの試しです。
- **鍵の scope**（`N_SCOPE_MACHINE`、`USER`、`DEVICE`、`PROGRAM`）。値の範囲の分け方として正しいです。
- **`accounts` の合言葉の処理**。PBKDF2、slot の記録です。足りないのは口です。
- **`systemlog`**。69 行で一つの仕事です。
- **`timesync` の SNTP**。足りないのは口です。
- **`network` の DHCP と名前引き**。カーネルの網は機構で、住所の決め方はサービスです。層の分け方が正しいです（一部を読んだ判断）。
- **`audio` の混合と変換と、`N_AUDIO_OP_OUTPUTS`、`N_AUDIO_OP_WATCH` の口**。窓がサービスに聞く形の手本です（一部を読んだ判断）。
- **`machine` の電源の方針**。ボタン、蓋、放置の決定はここが正しい場所です。
- **`screenmode.c` が選ばないこと**。カーネルは運ぶだけです（頭の 40 行と `display.c` からの判断）。
- **`keymap.c` の文字の持ち主**（0e7b7465）。カーネルは番号を写すだけです。
- **シェルが普通のプロセスであること**。`N_CAP_SHELL` と `SYS_SHELL_*` の形は正しいです。
- **全言語で一つの UI の文の形式**（33fbe945）。C、Go、JS が同じ形を使います。`uihost.c`（78 行）は Go のために同じバイト列を運ぶだけです。同じ UI の仕組みは複数ありません。
- **`language.c` の読み手と書式**。文の表の持ち方だけが問題です。

構造は正しいのに読みにくいだけの所：

- `user/libc/ui.c`（3584 行）。形、字、解析、配置、描画、hit test、入力、damage、session の口が一つのファイルにあります。持ち主の問題ではなく、ファイルの分け方の問題です。関数の一覧での判断で、本体は未読です。
- `user/apps/shell.c`（1196 行）の `dispatch` と各 `*_event`。
- `settings.c` の `on_event`（152 行）は S3 で書き直します。単独では読みにくいだけです。

## 6. 集計

見積りです。範囲は関数の開始行の差と引いた `file:line` から数え、残りを丸めました。問題の間の重なりは一度だけ数えました。「残す」は、この報告が構造の変更を求めないという意味です。良いという意味ではありません。読まなかった部分は「残す」に入れました。`wm.c` の合成の本体も `audit-compositor` の対象なので「残す」に入れました。

| 部品 | 行 | 残す | 書き直す | 消す |
|---|---:|---:|---:|---:|
| `user/apps/services.c` | 845 | 760 | 70 | 15 |
| `user/apps/settingsd.c` | 1293 | 1050 | 200 | 43 |
| `user/apps/settings.c` | 1617 | 350 | 1200 | 67 |
| `user/apps/settings_table.h` | 103 | 0 | 103 | 0 |
| `user/apps/settings_words.h` | 303 | 0 | 303 | 0 |
| `user/apps/session.c` | 397 | 265 | 120 | 12 |
| `user/apps/shell.c` | 1196 | 760 | 390 | 46 |
| `user/apps/shell_words.h` | 62 | 0 | 62 | 0 |
| `user/apps/shell_art.h` | 383 | 383 | 0 | 0 |
| `user/apps/machine.c` | 440 | 340 | 60 | 40 |
| `user/apps/display.c` | 438 | 300 | 138 | 0 |
| `user/apps/audio.c` | 1320 | 1200 | 20 | 100 |
| `user/apps/network.c` | 1327 | 1290 | 37 | 0 |
| `user/apps/timesync.c` | 232 | 222 | 10 | 0 |
| `user/apps/programs.c` | 547 | 240 | 307 | 0 |
| `user/apps/accounts.c` | 380 | 380 | 0 | 0 |
| `user/apps/systemlog.c` | 69 | 69 | 0 | 0 |
| `user/apps/files.c` | 467 | 390 | 77 | 0 |
| `user/apps/uihost.c` | 78 | 78 | 0 | 0 |
| `user/apps/sysmon.c` | 337 | 327 | 10 | 0 |
| `user/libc/services.c` | 394 | 370 | 24 | 0 |
| `user/libc/settings.c` | 258 | 240 | 18 | 0 |
| `user/libc/gui.c` | 279 | 240 | 29 | 10 |
| `user/libc/ui.c` | 3584 | 3450 | 134 | 0 |
| `user/libc/audio.c` | 259 | 259 | 0 | 0 |
| `user/libc/language.c` | 327 | 270 | 57 | 0 |
| `user/libc/keymap.c` | 142 | 142 | 0 | 0 |
| `common/services_abi.h` | 220 | 180 | 40 | 0 |
| `common/settings_abi.h` | 332 | 220 | 90 | 22 |
| `common/display_abi.h` | 180 | 130 | 35 | 15 |
| `common/input_abi.h` | 79 | 79 | 0 | 0 |
| `common/audio_abi.h` | 170 | 170 | 0 | 0 |
| `common/authority_abi.h` | 48 | 48 | 0 | 0 |
| `kernel/src/gui/wm.c` | 3370 | 2985 | 370 | 15 |
| `kernel/src/gui/wm.h` | 74 | 74 | 0 | 0 |
| `kernel/src/gui/pointer.c` | 381 | 381 | 0 | 0 |
| `kernel/src/gui/screenmode.c` | 246 | 246 | 0 | 0 |
| `kernel/src/gui/screenmode.h` | 25 | 25 | 0 | 0 |
| `kernel/src/sys/settings.c` | 147 | 62 | 60 | 25 |
| `kernel/src/sys/service.c` | 60 | 60 | 0 | 0 |
| `kernel/src/sys/authority.c` | 121 | 121 | 0 | 0 |
| **計** | **22530** | **18156** | **3964** | **410** |

割合は、残す 81%、書き直す 18%、消す 2% です。読み方の注意：

- 問題の重さは行数に出ません。S1、S4、S6 の中心は、今あるコードではなく、まだ無いコードです。
- 書き直しの半分近くは設定の窓とその文です（`settings.c`、`settings_words.h`、`settings_table.h` の計 1606 行）。

新しく書く量の見積り（上の表の外。確かな根拠はありません）：

| 仕事 | 行 |
|---|---:|
| 物の台帳（S1） | 1000〜2000 |
| 値の層（S4） | 300〜600 |
| `display` の口（S5） | 300〜500 |
| 導入の形（S6） | 2000〜4000 |
| ログインのプロセス（S9） | 約 100 |
| 文の記録（S13） | 300〜500 |

上の表の範囲の外で変わるもの（行数は数えていません）：`kernel/src/sys/authority.c:51` の呼び手の先、`kernel/src/fs/store.c`、`scripts/layout.py`、`tests/check_settings.py`、`kernel/src/dev/pm.c`、`user/apps/agent.c`、`user/apps/screenshot.c`、`user/libc/webprefs.c`、`ports/go/ui/ui.go`、`user/libc/web/js_ui.js`、`docs/nocturne-native-abi.md`。

## 7. 依存の順序

先に変えないと変えられないものです。

| 変えるもの | 先に要るもの | 理由 |
|---|---|---|
| S2 | S15 の一部 | 死んだ道が宣言の形を読みにくくします。 |
| S4 | S1、S2 | 装置の値の出どころは台帳です。宣言の場所が一つでないと欄を足せません。 |
| S5 | S1、S4 | 窓が聞く口は物の身元と、望みと実際の形を使います。 |
| S10 | S5 | シェルは主画面を `display` の口か欄から読みます。 |
| S3 | S1、S2、S4、S5、S12、S13 | 窓は全部の上に載ります。先に作り直すと、もう一度作り直します。 |
| S6 | プログラムの身元をパスから離すカーネルの変更、S13 | 更新で身元が変わらないこと、プログラムが自分の文を持つことが前提です。 |
| S11 | H1 の設計の決定、S10 | 方針を動かす先が決まりません。 |
| S13 | S2 | 宣言から文を出す先が要ります。 |

独立に先へ進められるものです：S7、S8、S9、S12、S14、S15。

順序の案：

1. S15、S14、S8、S7。短く、他を止めません。
2. S12、S2、S13。UI の事象と宣言と文の形を決めます。
3. S1。物の台帳と身元です。ここが根です。
4. S4、S5、S10。値の層と、口の無いサービスの穴を塞ぎます。
5. S3。設定の窓を作り直します。
6. S6。プログラムの導入です。3 と独立に並べられます。
7. S9。いつでも行えます。S12 の後なら書き直しが一度で済みます。
8. S11。合成の設計が決まった後です。

持ち主の判断が要ることです：

- 物の台帳を新しいサービスにするか、`machine` を広げるか。
- USB メモリーを C: の外の文書としてどう扱うか。AGENTS の「ディスクは一台」との関係です。
- 導入の形：誰が入れてよいか、何を権利の同意とするか、出どころを確かめるか。
- シェルの開始メニュー、音量、通知をエンジンへ移すか。
- 窓の方針の移す先（シェルか、別のプロセスか）。
- `tests` の製品の表からの出し方。
