# 設定の層の設計

OS 全体を通す設定の抽象化の層の設計である。材料は `audit-settings-missing.md`、`settings-ownership.md`、`settings-report.md`。

## 1. 守る条件

- 値を効かせる者が持ち主である。値を一か所に集めない。中央に鍵の表を作らない。
- 層は持ち主を結ぶ契約である。カーネルの部品、driver、system service、アプリが同じ形で登録する。
- プログラムは層を使わなくても動く。移植したプログラムは自分の profile を持ち、OS は何も知らなくてよい。プログラムにカーネルへの保存を強いない。
- CLI の段を持たない。窓は宣言から作り、値を足しても窓のコードを足さない。
- 一コマの中で層を引かない。持ち主は効いている値を自分の変数で持つ。一コマの費用は設定の数に比例しない。
- 効く道が通っていない項目は登録しない。登録が窓の項目になるので、効かない項目を作れない形にする。
- 層は vendor を知らない。OS の設定と driver の設定は領域で分かれ、互いを上書きしない。

## 2. 決めたこと

| 問い | 決定 | 理由 |
|---|---|---|
| 仲立ちの置き場所 | カーネルの小さな部品 `sys/setting.c`。 | 今の持ち主の三つ（wm、audio、screenmode）はカーネルの中で、init より前に値が要る。呼ぶ者の身元を偽れない所が仲立ちになる。仲立ちが service なら、カーネルの部品が利用者空間に頼る逆向きの依存になる。 |
| 仲立ちが持つもの | 領域の表、各領域の宣言の写し、変化の輪、試行の記録。値は持たない。 | 宣言の写しがあれば、持ち主が忙しくても窓が開く。 |
| 値の形 | 正規の文字列（UTF-8）。真偽は `true`・`false`、整数は十進、列挙は語、文字列はそのまま、列は改行区切り、記録は `名=値` を改行で区切る。 | 値を足しても ABI の構造体が変わらない。保存、JS、Go、Rust へ同じ形で渡る。検証は宣言と共通のコードで一度だけ書く。 |
| 宣言の形 | C では `struct n_setting_decl` の静的な表。電文と窓では、共通のコードがそれから作る正規の文書。 | データで書く。カーネルの部品もアプリも同じ表を書く。 |
| 検証と解決の場所 | `common/setting.c`（カーネルと libc の両方が組む）。仲立ちは検証しない。持ち主の受け取り口が宣言で検証し、持ち主が受けるか断るかを決め、取った値を返す。 | `common/store` と同じ形で、解析と解決を一度だけ書く。 |
| 保存の場所 | 持ち主の側。カーネルの部品は `sys/setting.c` の保存の thread、アプリの持ち主は libc の受け取り口。文書の形と読み書きは `common/setting.c` の一つ。 | driver の値は driver の持ち物である。カーネルにアプリの値の保存を強いない。 |
| 文書の置き場所 | 機械は `C:\Nocturne\Settings\<領域>.conf`、利用者は `<利用者>\AppData\Nocturne\Settings\<領域>.conf`。プログラムが自分の領域を登録するなら `AppData\<プログラム>\Settings.conf`。 | 機械の値と利用者の値を分ける（[M1]、[A1]）。 |
| scope の順 | 探す順は プログラム → 装置 → 利用者 → 機械 → 既定。一番狭い値が勝つ。宣言が受ける scope を持つ。 | NVIDIA の DRS の三段（[N3]）と Apple の domain の順（[A1]）を問いの一覧として使う。一つの順に決める。 |
| 起動の値 | 層に入れない。`n_launch.settings`・`n_settings` は `n_launch.values`・`n_launch_values` へ改名する。 | 「設定」の語が二つの物を指している。起動の値は保存しない別の仕組みである。 |
| 取引 | 一つの鍵の変更が一つの取引である。一度に当てる値は記録の型の一つの鍵にする（画面の配置）。 | 複数の鍵の取引は持ち主をまたぐと解けない。 |
| 権限 | 宣言ごとに書いてよい者（`system`・`any`）を持つ。`system` はカーネルと `C:\Nocturne\System\` のプログラム。読むのは誰でも。`Settings` の二つのフォルダーへの書込みは store が system のプログラムとカーネルに限る。 | 利用者の層が来るまでの二段で、宣言の語を増やせば広げられる。 |
| 領域の取得 | `C:\Nocturne\System\` のプログラムは空いた領域の名を取れる。ほかのプログラムは自分のプログラムの id と同じ名の領域だけを取れる。 | 悪いアプリが `display` を騙れない。 |
| 複数の窓の競り | 鍵ごとの set で解く。全部を読んで全部を書く道を消す。 | `js.c:895-903` の欠陥を形で消す。 |
| 移行 | 持たない。`keep.c` の `older[]`、`deep_colour`、`SCREEN_MODES_FIRST_BYTES`、`search-history.enabled` の後始末を消す。 | AGENTS.md「互換は持たない」。 |

## 3. 部品

```
窓（settings.ts）・タスクバー・アプリ
   │ n_setting_*（libc）／ nocturne/settings（JS・TS）
   ▼
SYS_SETTING_*（common/abi.h、§12）
   ▼
sys/setting.c（領域の表、宣言の写し、変化の輪、試行、呼ぶ者の身元）
   ├─ カーネルの持ち主：struct setting_owner の関数を呼ぶ thread で呼ぶ
   └─ アプリの持ち主：持ち主のチャンネルへ頼みの電文を送り、返事を待つ
         libc の受け取り口（n_setting_serve）が検証、保存、見張りの知らせを担う
common/setting.c：宣言の文書、型の検証、scope の解決、保存の文書（カーネルと libc が組む）
```

### 3.1 宣言

```c
struct n_setting_decl {
    const char *key;        /* 領域の中の鍵。"variable-refresh" */
    enum n_setting_type type;  /* BOOL, INT, ENUM, STRING, LIST, RECORD */
    const char *def;        /* 既定の正規の文字列 */
    int64_t min, max, step; /* INT */
    const char *unit;
    const struct n_setting_option *options; /* ENUM の語と表示名 */
    uint32_t scopes;        /* N_SCOPE_MACHINE | USER | DEVICE | PROGRAM */
    const char *device_class; /* "monitor"・"audio-output"・"input"・"nic"・"gpu" */
    enum n_setting_effect effect; /* NOW, TRIAL（試して戻す）, NEXT_START（次の起動で効く） */
    enum n_setting_writer writer; /* SYSTEM, ANY */
    const char *page, *group, *label, *help, *widget, *search;
};
```

- `widget` は窓の部品の案内である（既定は型から決まる。`mode-list`・`volume` のように型だけで描けないものを選ぶ）。
- `page` と `group` が窓の頁と見出しになる。頁の置き場所は領域が決める。driver の領域は OS の頁と別の場所に出る。
- 鍵ごとの今の状態（今使えるか、使えない理由、装置の実体ごとの選択肢）は持ち主が公開する。宣言の文書と一緒に `SYS_SETTING_PUBLISH` で仲立ちへ渡す。VRR の項目はモニターの申告がある間だけ使える。モードの選択肢はモニターごとに違う。公開が変わると見張りが鳴る。

### 3.2 持ち主の契約

カーネルの部品は次の関数の表、アプリの持ち主は同じ意味の電文を実装する。

| 操作 | 意味 |
|---|---|
| `apply(key, scope, value)` | 宣言で検証した値を効かせる。受けるか断るかを持ち主が決める。断りは層の検証より強い。取った値を返す。 |
| `effective(key, device, program)` | 今効いている値を読む。 |
| `publish` | 宣言と今の状態を仲立ちへ渡す（持ち主から）。 |
| `changed(key, scope)` | 値が変わったと知らせる（持ち主から）。誰が変えても知らせ、同じ値の当て直しは変化でない。通知は保存を待たない。 |

- 読むこと、既定に戻すこと、scope の解決、保存は `common/setting.c` が持ち主の代わりに行う。持ち主が書くのは宣言と `apply` だけである。
- 既定に戻すは「その scope の値を消す」である。既定を書き込まない。段は鍵、領域、全部の三つ。
- 寿命：アプリの持ち主は `SYS_SETTING_OWN` で得た handle で領域を持つ。閉じれば（プロセスの終わりを含む）領域は一覧から消え、待っている頼みは `N_ERR_NOT_FOUND` で返り、見張りが鳴る。保存された値は残る。driver が戻れば同じ値に戻る。
- カーネルの持ち主の `apply` は眠ってよい文脈で呼ばれ、持ち主の錠だけを取る。割り込みの文脈からの変更（なし）を作らない。

### 3.3 scope と身元

- 名前：`領域 / 鍵 @ scope`。例：`display/variable-refresh @ device:monitor:<id>`、`nvidia/gsync @ device:monitor:<id>`、`nvidia/frame-limit @ program:<id>`、`input/key-repeat-delay @ user`、`time/zone @ machine`。
- **装置の身元**は一つの部品 `dev/devid.c` が作る。規則は「機械を換えても同じ物なら同じ、同じ型の二台は別」である。

| 種類 | 身元 |
|---|---|
| モニター | EDID の maker・product・serial と文字列の serial。同じ値の二台には接続口の名を足す。 |
| 音の出口 | PCI の位置と codec の vendor・subsystem と pin の番号。USB は vendor・product・serial。 |
| 入力 | USB の vendor・product・serial。serial が無ければ port の道。 |
| NIC | MAC。 |
| GPU | PCI の subsystem と位置。 |

- 繋がっていない装置の値も残し、窓は「繋がっていない装置」を一覧に出して消せる。
- **プログラムの身元**はカーネルが読み込んだ道から作る。`C:\Programs\<アプリ>\…` は `<アプリ>`、`C:\Nocturne\System\<名>` は `<名>`。`struct process` に持ち、子へ継がない。呼ぶ者が名乗ることはできない。`n_app_data_folder(name)` の name もこの身元に揃える（§5.4 の欠け）。

### 3.4 通知

- 見張りは今の `N_KIND_WATCH`（`sys/watch.h`）の一つの種類を使う。`SYS_SETTING_WATCH(領域 または NULL)` が handle を返す。
- 領域ごとの変化の輪が（鍵、scope の実体、世代）を持つ。`SYS_SETTING_CHANGES(watch, buf, cap)` が前の取り出しの後の変化を返す。輪を追い越した見張りには「全部を読み直す」の印を返す。話題ごとの口を増やさない。
- `n_screen_watch`・`n_audio_output_watch` を消す。読み物の変化（画面の接続）は領域の公開の変化で鳴る。

### 3.5 試行と戻し

- `effect = TRIAL` の鍵に `N_SETTING_TRY` で書くと、仲立ちが前の値と期限を持ち、持ち主へ当てる。`SYS_SETTING_CONFIRM` で確定し、期限か `SYS_SETTING_REVERT` で前の値へ当て直す。
- 試行の期間は 15 秒で宣言に持つ。今の `screenmode.c` の試行の手書きと `settings.ts` の表示はこの一般の形へ移す。

### 3.6 保存

- 文書の形は版の行、`scope 鍵=値` の行、`#` の注釈である。形はインターフェースでない。
- 知らない行は残す（版を戻しても新しい値が消えない）。範囲の外の値はログの行を出して使わず、文書には残す。
- 読めなかった文書を上書きしない。壊れた文書は `.bad` の名で残して既定から始める（`keep.c:272`・`:278` の欠陥を消す）。
- 書けなかったことは持ち主の状態になり、窓へ出る。「kept after a restart」の文は事実のときだけ出る。
- 最後の変更から 0.5 秒後に書き、電源の前に書く（今の `keep.c` の良い所を残す）。

### 3.7 ABI

番号は一つの族にする。値を足しても ABI は変わらない。

| 呼出し | 引数 | 返すもの |
|---|---|---|
| `SYS_SETTING_DOMAINS` | buf, cap | 今いる領域の名 |
| `SYS_SETTING_DESCRIBE` | 領域, buf, cap | 宣言と今の状態の文書 |
| `SYS_SETTING_GET` | 領域, 鍵, scope, buf, cap | 値と出所（どの scope から来たか） |
| `SYS_SETTING_SET` | 領域, 鍵, scope, 値, 旗, buf, cap | 持ち主が取った値 |
| `SYS_SETTING_RESET` | 領域, 鍵 または NULL, scope | |
| `SYS_SETTING_CONFIRM`・`REVERT` | 領域, 鍵, scope | |
| `SYS_SETTING_WATCH`・`CHANGES` | §3.4 | |
| `SYS_SETTING_OWN` | 領域 | 持ち主のチャンネル |
| `SYS_SETTING_PUBLISH`・`NOTIFY` | 持ち主, 文書／鍵 | |

- 消すもの：`SCREEN_SETTINGS_GET`・`SET`、`struct desktop_settings`、`SCREEN_MODE_SET`・`TRY`・`KEEP`・`BACK`、`SYS_AUDIO_OUTPUT` の書く側、`SYS_AUDIO_OUTPUT_WATCH`、`SCREEN_WATCH`、`js.c` の `h_desktop` の族、`ui.d.ts` の個別の型、`SystemChange` の和。
- 読み物は残す：画面の情報、`n_screen_modes` の driver から OS への申告。
- 誤りは `N_ERR_*`：範囲の外は `N_ERR_INVALID`、持ち主が居ないのは `N_ERR_NOT_FOUND`、権限は `N_ERR_DENIED` に当たるもの。
- 仕様は `docs/nocturne-native-abi.md` §12 を同じコミットで書き直す。

### 3.8 言語の口

- C：`n_setting_get`・`set`・`reset`・`watch`・`changes`・`describe`・`domains`、持ち主の `n_setting_own`・`n_setting_serve`（`user/include/nocturne.h`、`user/libc/setting.c`）。窓の無いプログラムも読める。
- JS・TS：`nocturne/settings`（`js.c` の一つの汎用の口と `js_ui.js`、`rootfs/Nocturne/SDK/Types/nocturne/`）。
- Go と Rust の std は同じ ABI の上にあり、今は要らない。

### 3.9 窓

- `settings.ts` は `domains` と `describe` から頁と行を作る。検索は宣言の `search`・`label` から作る。
- 型ごとの部品が既定で、`widget` が特別な部品を選ぶ。行には出所（既定か、利用者か、装置か、プログラムか）と「既定に戻す」がある。使えない項目は理由を出す。
- 装置の scope の頁は装置を選ぶ。プログラムの scope の頁はプログラムを選び、上書きを消せる。
- driver の領域は OS の頁と別の場所に出る。driver が居なければ出ない。
- タスクバーの音量の面も層を通して読み書きする。カーネルの中の呼び手は `setting_set` を使う。
- ブラウザーの設定の頁は今の窓のままとし、値の鍵を層へ登録するかは §6 の二段目で決める。

### 3.10 試験

- `tests/settingtest.c`：登録された全部の領域の全部の鍵に、宣言から作る試験を当てる。範囲外を拒む、当てて読む、既定に戻す、見張りが鳴る、再起動で戻る。
- host の試験：`common/setting.c` の解析、解決、知らない行、壊れた文書。

## 4. 部品の種類ごとの動き

| 種類 | 例 | 登録 | 読み戻し | 変更 |
|---|---|---|---|---|
| カーネルの部品 | `display`、`audio`、`input`、`time`、`power`、`network` | `kmain_late` で `setting_register(&owner)` | `store_start` の後、init の前に全部読む | 呼ぶ thread で `apply` |
| driver | `nvidia` | 起動時に `n_setting_own` と `n_setting_serve` | 受け取り口が文書を読み、`apply` を呼ぶ | チャンネルで `apply`。driver が死ねば領域は消え、値は残る |
| system service | 今は init の仲間に無い。`time` の時刻合わせ、`power` の放置の方針が来たら | driver と同じ | 同じ | 同じ |
| 組み込みのプログラム | `browser` | 任意 | 自分の文書 | 自分の見張り |
| 移植のプログラム | Firefox | しない | 自分の profile | 自分 |

## 5. VRR と driver の設定

- VRR の四つを分ける（`settings-ownership.md`「VRR で普通に分かれる四つ」）。
- モニターの申告：`ports/nvidia/display.c` が KAPI の `vrrSupported` から `N_SCREEN_MODE_VRR` と `vrr_min_mhz` を立てる。申告は「できる」である。
- OS の設定：`display/variable-refresh @ device:monitor:<id>`。持ち主は合成。既定は切（[W1]）。`wm.c` は設定が入のときだけ VRR の歩調で回す。切なら固定の周期で回す。画面ごとにする理由は、歩調を持つのが画面ごとの合成の thread で、規模の例が周波数と VRR の違う 3 枚の画面だからである。
- driver の設定：`nvidia/gsync @ device:monitor:<id>`（持ち主は `nvrm`、効く所は KAPI の `vrrEnabled` の commit）。`nvidia/gsync-scope`（全画面か窓も）。OS の値は driver の値を上書きしない。両方が入のときに VRR が動く。
- 3D の設定（垂直同期の上書き、最大フレーム レート、低遅延）は `nvidia` の領域で、全体とプログラムごとの値を持つ。効かせるのは Mesa の present の道で、見張りで変えた瞬間に効かせる。効く道が通るものだけ登録する。
- `NVKMS_OPTIONS`・`NVRM_REGISTRY_DWORDS`・`NVreg_*` の起動の値の道は消す。要るものは `nvidia` の鍵にする。

## 6. 段

一段目は層と今ある値の全部を移す。二段目は層の上に持ち主を足す。

| 段 | 中身 |
|---|---|
| 一段目 | `common/setting.c`、`sys/setting.c`、`dev/devid.c`、プログラムの身元、ABI と §12、libc と JS の口、`keep.c` を層へ畳む、`pointer_speed`・`smooth_gradients`・音量・消音・モードを移す、古い口を消す、`settings.ts` を宣言から作る、`settingtest.c`、`n_launch.values` への改名、VRR の OS の値と driver の申告と commit、`nvrm` の領域。 |
| 二段目 `input` | 持ち主を `dev/input.c` へ：加速、ダブルクリック、キーの repeat（`usbhid.c` から移す）、配列（JIS と US）、ホイール、ボタンの入替え、装置ごとの値。 |
| 二段目 `time` | 時間帯（機械）、手で合わせる、libc の `localtime`、QuickJS の地方時の差、タスクバーの時計の書式。 |
| 二段目 `power` | 電源ボタンの動作、放置で画面を消す、放置で眠る、休止の可否。放置の見張りは入力の道から。 |
| 二段目 `network` | NIC ごとの IP の取り方と DNS。`dns-server=`・`tcp-*` の起動引数を消す。 |
| 二段目 `appearance` | 明暗、アクセント、文字の大きさ、動きを減らす、透明を減らす、コントラスト。持ち主は一つ。`ui.h`・`wm.c`・`desktop.c` の色の二重を消す。CSS の `prefers-*`（`css.c:5160-5168`）をこの値につなぐ。画面の拡大は `display/scale @ device:monitor`。 |
| 二段目 `display` | 配置と主の画面を記録の型で。タスクバーの置き場所。 |
| 二段目 `audio` | 音量を装置の scope へ、既定の出口、アプリごとの音量。実機の driver（`hdaudio`）の上に。 |

`git` の上で二段目は別々の書き手が並べられる。触るファイルが重なる所（`wm.c`、`settings.ts` は宣言から作るので触らない）は順に載せる。

## 7. 同じ理由で欠けた層の扱い

| 層 | 扱い |
|---|---|
| 装置の身元、プログラムの身元、driver の能力の申告、音の出口 | この設計で作る。 |
| 時刻帯、入力の配列、外観と拡大、電源の方針 | 二段目で作る。 |
| system service、シェルの方針（`wm.c` の `apps[]`）、地域と文字列、通知の ABI、利用者と権限、秘密の保管 | 別の設計が要る。層はこれらを前提にしない。一段目の宣言の文字列は一つの言語で書き、文字列の表が来たときに宣言の語を鍵にする。 |

## 8. 出典

[M1]・[A1] は `audit-settings-missing.md` の §10、[N3]・[W1] は `settings-ownership.md` の出典の節。
