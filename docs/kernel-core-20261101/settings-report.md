# settings の報告：一段目（持ち主の表）

枝は `worktree-agent-a563ee79bd72b29cc`。土台は main の 5043e3e6。
表は同じフォルダーの `settings-ownership.md`。
二段目のコードは書いていない。組みも QEMU の確かめも、コードが無いので回していない。

## 依頼文（全文）

```
あなたの名は「settings」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。ただし土台は main の 5043e3e6（中核・USB・時間の ABI・合成と表示・I/O が載った形。報告は同じフォルダーの `compositor-report.md` など）。自分の作業木が古ければ、まず main を取り込む。QEMU のポートは HMP 45474・QMP 45475。

# 持ち主の言葉（そのまま）
"VRR and gsync are settings, one is a os setting one is driver setting. This is something that is an overall issue not just a issue with vrr. we apply overship haphazardly to things, with out finding out what is normal. and we also have like verylittle real support for settings, overall."
持ち主が見せた画像は二枚。Windows の「設定 > グラフィックス設定」に Variable refresh rate の入り切り（"Optimize games for variable refresh rate"）と、アプリごとの Graphics performance preference がある（Microsoft の DirectX Developer Blog の OS Variable Refresh Rate の記事。この切り替えは G-SYNC・FreeSync・Adaptive-Sync の control panel の設定を上書きしない、と書いてある）。NVIDIA コントロール パネルの「3D 設定の管理」に、グローバル設定とプログラム設定があり、モニター テクノロジ（G-SYNC との互換性、ULMB、固定リフレッシュ レート）、優先的に使用するリフレッシュ レート、垂直同期、低遅延モード、最大フレーム レート、トリプル バッファリング、電源管理モードがあり、別に「G-SYNC の設定」の頁がある。

# 仕事：何が普通かを調べて持ち主を決め、本物の設定の層を作り、VRR と driver の設定を通す

## 一段目：普通を調べて、持ち主の表を作る
設定ごとに、普通の OS と driver で誰が持つかを、公式の文書で調べる（Microsoft Learn と DirectX Developer Blog、WDDM の文書、NVIDIA・AMD の公式の control panel の文書、VESA。`WebSearch`・`WebFetch` か内蔵ブラウザーで読む。ほかの OS のソースコードは読まない）。今の Nocturne で誰が持っているかを原文で読み（`kernel/src/sys/keep.c`、`gui/wm.c`・`screenmode.c`・`desktop.c`、`dev/audio.c`、`user/apps/settings.ts`、`user/apps/js.c` の設定の口、`user/apps/browser_settings.h`、`ports/nvidia`）、二つを並べる。範囲は設定全部：画面（解像度・周波数・HDR・拡大・配置・主画面・回転）、VRR、driver の表示の設定（G-SYNC 相当、垂直同期の上書き、最大フレーム レート、低遅延、電源）、アプリごとの GPU の好み、音、入力（指の速さ、キーの repeat、配列）、電源、時刻と時間帯、網、外観、起動。`docs/kernel-core-20261101/settings-ownership.md` に表で書く：設定、普通の持ち主（出典の URL）、今の Nocturne の持ち主、合っているか、Nocturne の決定。普通と違う持ち主を選ぶなら、その理由を書く。AGENTS.md の「設定の持ち主はシステムで、変えた瞬間に効き、設定アプリは窓だけ。プログラムの設定はそのプログラムの持ち物で、カーネルや共有ファイルに持たせない」は持ち主の規則。driver の設定は driver の持ち物で、OS の設定と分ける。

## 二段目：作る
- 設定の層：今の設定は部品ごとの `.conf` と個別の syscall（`keep.c`、`js.c` の口）に散っている。読み、書き、検証（範囲・列挙）、変更の通知（変えた瞬間に効く）、既定に戻す、保存を一つの形にする。部品が持ち主のまま登録する形で、大きな共有の表にしない。アプリから読める口はネイティブ ABI に Nocturne の形で（仕様書も同じコミットで）。driver のようなアプリの持ち物の設定も同じ層で持てること（持ち主が driver なら、driver が居ないときは出ない）。
- VRR：`gui/wm.c` は今、画面の mode に `vrr_min_mhz` があれば無条件に VRR で回す。OS の設定として画面ごとの入り切りにする。装置の申告は「できる」であって「使う」ではない。切なら固定の周期で回す。既定は普通の既定に合わせる。OS の設定は driver の設定を上書きしない。
- driver の設定：G-SYNC 相当（monitor technology）、垂直同期の上書き、最大フレーム レート、低遅延は driver の持ち物として `ports/nvidia` の側に置き、全体の値とプログラムごとの値を持つ。効く道まで実装できるものは通す。実機の NVIDIA でしか確かめられない道は、NVIDIA の公式の文書と `D:\Programes\cloned repos\open-gpu-doc`・`open-gpu-kernel-modules` で照らして書き、hosttest で試す（ソースは NVIDIA の公開物として既に移植に使っている範囲で）。効かせられない項目を窓に出さない。stub の項目は禁止。
- 窓：`user/apps/settings.ts` に画面ごとの VRR の入り切りと、ここで作った設定を出す。driver の設定は OS の設定と別の頁（driver が居るときだけ出る）。CLI や旗の起動にしない。

範囲の外：中核の中身、`sys/syscall.c` の `shared_lock`（別の書き手が同時に mm へ移す）。`common/abi.h` は自分の部分だけを変える。

確かめ：組み（`kernel`・`qemu-image`、変数なし）。QEMU で設定の窓を開き、指で切り替え、画面に効くこと（`wm:` の行）、再起動をまたいで残ること（再起動の確かめの手順は `references/qemu-visual.md`）。VRR の道は QEMU に VRR の画面が無いので、`tests/` の host の検査か `wmframetest` の形で、切のとき固定の周期で回ることを試す。報告は `docs/kernel-core-20261101/settings-report.md`。
```

調整役の途中の指示（全文）：

```
持ち主の追加の言葉（そのまま）："this is not just for vrr or display stuff. we lack a proper settings system. and stuff like this shouldn't be too hardcoded. we should be carefull, and maybe do an adversarial audit first of what we are missing."
依頼を変える：一段目（持ち主の表 `docs/kernel-core-20261101/settings-ownership.md`）を、画面に偏らず設定の全部で書いて、作業木の枝にコミットし、そこで止まって報告を返す。二段目（設定の層・VRR・driver の設定・窓）はまだ書かない。敵対的な監査が二本走っていて、その結果と表を合わせてから、主担当が二段目を頼む。すでに二段目のコードを書き始めていたら、コミットせず、報告にそう書く。
```

```
持ち主の言葉（そのまま）："NO, you are not understanding. WE AREMISSING ABSTRACTION THAT NEEDS TO BE THERE. WE HAVE NO PROPER SETTINS SYSTEM ACROSS THE"（ここで切れている）。
仕事の主題は持ち主の表でなく、OS 全体を通す設定の抽象化の層。表は、その層が扱う scope と持ち主の分け方を決める材料として書く。表を書き終えて止まる指示は同じ。報告の終わりに、表から読める層の要件（部品の登録の形、scope、持ち主、通知、ABI）を書く。コードはまだ書かない。
```

## 結果の数

表は 75 行。判定の数は次のとおり。

| 判定 | 行 |
|---|---|
| 無い | 43 |
| 決め打ち | 19 |
| 違う | 5 |
| 合う | 4 |
| 範囲が違う | 1 |
| 普通に無い | 1 |
| 比べられない | 1 |
| 普通と同じ場所 | 1 |

普通と合うのは、解像度と周波数、ポインターの速さ、プログラムの設定、起動の一時の値の四つだけである。

## 表から分かった事実

依頼の前提と違う事実を先に書く。

1. **実機の driver で VRR の道に入らない。** `ports/nvidia/display.c` は `N_SCREEN_MODE_VRR` も `vrr_min_mhz` も立てない。NVKMS の KAPI には `vrrSupported` と `vrrEnabled` があるが、`ports/nvidia` は使っていない。`wm.c` の VRR の分岐が動く画面は、今の driver には無い。VRR の設定を足すだけでは実機で何も変わらない。driver が申告し、`vrrEnabled` を commit する道が先に要る。
2. **Windows の OS の切り替えは画面ごとと書いていない。** DirectX Developer Blog は Graphics Settings の一つの toggle と書く。既定は切である。効果は、VRR を持たない DX11 の全画面のゲームに足すことである。依頼の「OS の設定として画面ごとの入り切り」は普通と scope が違う。表は画面ごとに決め、理由を書いた（歩調を持つのは画面ごとの合成の thread で、規模の例は周波数の違う 3 枚の画面）。
3. **`NVKMS_OPTIONS` と `NVRM_REGISTRY_DWORDS` は入らない。** `nvrm` は両方を起動設定から読む。`user/apps/init.c` は `nvrm` を起動設定なしで起こす。
4. **`struct keep` はカーネルの中だけである。** アプリの `nvrm` は登録できない。`user/libc/webprefs.c` は同じ仕事を自分で書いている。
5. **機械の scope の置き場が無い。** `keep.c` は全部を利用者のフォルダーへ書く。時間帯・起動・網・既定の出口に置き場が無い。
6. **利用者の値が起動引数にある。** `dns-server=`、`tcp-rx-bytes=`、`tcp-tx-bytes=`（`net/net.c`）。AGENTS.md は旗の起動を禁じる。
7. **同じ知識が二か所にある。** 色は `user/include/ui.h` と `gui/wm.c`・`desktop.c` の直書き。ポインターの範囲はカーネルと `settings.ts`。
8. **決め打ちの場所が持ち主の層と違う。** キーの repeat は USB の driver（`dev/usbhid.c`）の定数で、ほかの鍵盤の driver には効かない。開始の一覧はカーネルの `apps[]`。既定のアプリは `files.c`。

表の中で、私が読み違えて直した行は二つある。全画面の窓の直接表示（`N_SCANOUT_DIRECT`）が無いと初めに書いた。原文で読んで直した。`init.c` が決まったアプリを起こすと初めに書いた。実際は試験の `autorun` だけで、直した。

## 残った欠陥と未確認

- 頁を開けず検索の抜粋で読んだ文書がある。一覧は表の「公式の文書で分からなかったこと」にある。AMD の頁は取得が時間切れになる。
- 電源ボタン・蓋・主画面を説明する Windows の公式の文書は読めなかった。Monitor Technology の既定も読めなかった。
- `D:\Programes\OS DEV INFO` に VESA の仕様書は無い。共通の依頼の「VESA の DisplayPort・EDID など」と違う。
- 走っている二本の敵対的な監査の結果は見ていない。表はその結果と合わせる前の形である。

## 表から読める層の要件

### 部品の登録の形

- 登録するのは値を効かせる持ち主で、中央の表は無い。カーネルの部品も、アプリの driver も、OS のサービスも同じ形で登録する。今の `struct keep` はカーネルの部品しか登録できない。
- 登録は持ち主の宣言である：名前空間（`display`、`audio`、`input`、`nvidia` など）と、設定ごとの鍵、型、範囲か列挙、既定、受ける scope、効く時。範囲を窓に写さないために、宣言だけが知識の場所である。
- 登録は持ち主の寿命に結ぶ。アプリの持ち主は handle で登録し、プロセスか claim が終わると設定は見えなくなる。保存された値は消えない。「driver が居ないときは出ない」はここから出る。
- 出るかどうかは今の事実で変わる。VRR はモニターの申告がある間だけ、音の出口の選択は二つ以上ある間だけ。この変化も通知する。
- 書くことは持ち主への頼みである。層は宣言で確かめ、持ち主が受けるか断るかを決め、持ち主が取った値を返す。持ち主の断りは層の検証より強い（mode が出せないなど）。
- 試して戻す（今の `SCREEN_MODE_TRY`・`KEEP`・`BACK`）は層の書く形に一般に持つ。mode だけの特例にしない。

### scope

- scope は五つある：機械、利用者、装置、プログラム、起動。表の全部の行はこのどれかに入る。
- 起動（`n_settings`）は層に入れない。保存せず、別の仕組みのままにする。語が重なるので、層の名は「起動設定」と紛れないものにする。
- 宣言ごとに受ける scope を持つ。VRR の OS の値は装置、G-SYNC は装置、Monitor Technology は全体とプログラム、音量は装置、アプリの音量はプログラム、時間帯は機械。
- 解決の順は宣言で一つに決める：プログラムの値、全体の値、登録の既定（DRS の三段と Apple の domain の検索順）。解決は持ち主の外で一度だけ書く。
- 装置の scope は安定した身元で引く。モニターは `edid_monitor_id`。音の出口と網の装置の身元は新しく決める。繋がっていない装置の値も残し、一覧に出せる。
- プログラムの scope はプログラムの身元が要る。今 Nocturne に身元の定義は無い。候補は `C:\Programs\<アプリ>` の場所で、層の設計で決める。
- 機械の値の置き場を `C:\Nocturne` の下に作る。利用者の値は今の `AppData\Nocturne`。

### 持ち主

- 持ち主の表は `settings-ownership.md` のとおり。OS の部品、driver、プログラムの三つに分ける。
- OS の設定と driver の設定は名前空間が別で、互いを上書きしない。アプリごとの GPU だけは、OS の値が driver の control panel より勝つ（Windows の文書）。その値も頼みであり、プログラムが最後に選べる。
- 層は vendor を知らない。NVIDIA の項目も AMD の項目も、それぞれの driver が登録する。
- プログラムの設定はプログラムの持ち物のままで、層は使ってよい口であり、強制しない。移植したプログラムの profile に手を入れない。ブラウザーの `webprefs` を層へ移すかは別に決める。
- 文書は持ち主だけが読み書きする今の約束を保つ。層が持ち主に代わって文書を書いても、形はインターフェースにしない。

### 保存と既定

- 保存は `keep.c` の良い部分を使う：最後の変更から 0.5 秒後に書く、電源の前に書く、原子的に差し替える。
- 既定に戻すは三段ある：設定ごと、持ち主ごと、全部（NVIDIA の DRS）。戻すは「値を消す」であり、既定を書き込まない。版で既定が変わればそれに従う。
- `keep.c` の `older[]` の引き継ぎは互換の残りで、AGENTS.md の「互換は持たない」と照らして消すかを決める。

### 通知と効く時

- 通知は一つの形にする。`N_KIND_WATCH` は良い形なので使う。話題ごとの開く口（`n_screen_watch`、`n_audio_output_watch`）を増やさず、名前空間か鍵で開く。今の `n_watch_take` は変化の数だけを返す。層では何が変わったかが要る。
- 誰が変えても知らせる。同じ値の当て直しは変化でない。今の約束のままである。
- 通知は保存より先で、保存を待たない。
- カーネルの持ち主は眠らない道で受ける（今の `keep_changed` の約束と同じ）。合成の一コマの中で層を引かない。持ち主は効いた値を自分で持つ。一コマの費用は設定の数に比例しない。
- 変えた瞬間に効くのが AGENTS.md の規則である。普通の driver はプロセスの初期化でだけ読む（NVIDIA の案内）。Nocturne は user mode driver の present が通知を聞いて効かせる。
- 効かせられない値は宣言に「次の起動で効く」と書く。起動の項目がそれに当たる。窓はそう表示する。

### ABI

- `common/abi.h` に新しい番号の塊を作る：一覧、宣言を読む、読む、書く、既定に戻す、見張る。値は型付きで、固定の構造体を設定ごとに作らない。
- 読むと書くは scope、装置の身元、プログラムの身元を引数に取る。「効いている値」と「その scope に置かれた値」を分けて読める。
- 登録の口はアプリの持ち主のために ABI に要る（名前空間の object と、書く頼みを受ける事象）。
- 誤りは今の `N_ERR_*` で言う：範囲の外は `N_ERR_INVALID`、持ち主が居ないのは `N_ERR_NOT_FOUND`。
- 使い手を新しい口へ移し、古い口を消す：`SCREEN_SETTINGS_GET`・`SET` と `struct desktop_settings`、`n_audio_output` の書く側、`SCREEN_MODE_SET` の設定の部分。`js.c` の `h_desktop` などと `ui.d.ts` の個別の型も一般の形に置き換える。mode の一覧のように読み物である口は残る。
- 仕様は `docs/nocturne-native-abi.md` の §12 を同じコミットで書き直す。今の §12 の「全部の設定を並べた表、設定のビット、全プログラムが共有する設定の文書はどこにも無い」は保つ。層は共有の表でなく、持ち主の登録の集まりである。

### 窓

- `settings.ts` は登録の宣言から頁を作る。設定を足すときに窓のコードを足さない。
- 宣言だけで描けない頁（mode の一覧と試行、音量のつまみ）は、持ち主の型の部品として窓が持つ。
- 頁の分け方と表示の文字列も持ち主の宣言に入る。文字列の言語は表示の言語の仕組みと一緒に決める。
- driver の頁は OS の頁と別の場所に出る（NVIDIA の control panel と同じ分け方）。driver が居ないと出ない。
- 効く道が通っていない項目は登録しない。登録が窓の項目になるので、stub の項目は作れない形になる。

### 表の決定のうち、層の設計で解くもの

- プログラムの身元の定義。
- 機械の scope の文書の場所と、書いてよい者（今は利用者が一人で、権限の仕組みが無い）。
- 音の出口と網の装置の安定した身元。
- 宣言の字句（鍵の形、型の種類）。
- 知らない行の扱い（今の `keep.c` は捨て、`webprefs.c` は残す）。一つに決める。
