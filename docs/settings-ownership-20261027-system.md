# システムの設定の設計（サブエージェントの報告、2026-10-27）

決定は `docs/settings-ownership-20261027.md`。食い違う所（番号は 203〜206 を欠番にして 207 から、SYS_MAX 210、`N_KIND_WATCH` 13）はそちらが勝つ。

## 0. 結論

設定の値は、その値を当てている部品（持ち主）が持つ。持ち主は自分の C: の文書へ自分で保存し、起動時に自分で読み戻し、自分が報告する値に見張りを付けて変化を知らせる。Settings の窓は値も文書も持たない操作盤で、タスクバーと同じ立場。カーネルは、プログラムのための設定の置き場を一切持たない。プログラムの設定は、既にある `n_app_data_folder(name)` の `AppData\<名前>` に自分で持つ（docs/nocturne-native-abi.md:85、ブラウザーも webstorage.c:23 で `AppData\Browser`）。「Language」頁はシステムの Settings から外す。起動時の復元は、持ち主が init より前に自分で行う。

## 1. 持ち主（HEAD 44a30ab2 のコードで確かめた位置）と保存先

| 値 | 持ち主 | 変える経路 | 保存 | 知らせ |
|---|---|---|---|---|
| ポインターの速さ | wm.c:119 `pointer_speed`。効くのは :2196 の相対移動だけ | `SCREEN_SETTINGS_SET`（wm.c:2881〜） | `AppData\Nocturne\Desktop.conf` | `SCREEN_WATCH` |
| 滑らかな階調（望み） | wm.c `deep_wanted`。表示できるかは `fb_dither_able` | 同上 | Desktop.conf | `SCREEN_WATCH` |
| 音量・消音 | audio.c:58〜59。変えるのは `audio_set_volume`／`audio_set_muted`（:278／:286）だけ | `SYS_AUDIO_OUTPUT`（syscall.c:2409）と、タスクバーの wm.c :1109（つまみ）、:2051（消音）、:2274（ホイール） | `Sound.conf` | `SYS_AUDIO_OUTPUT_WATCH` |
| モニターごとのモード | screenmode.c:43〜47 `choices[8]`、`choose_for`（:90）、`pick`（:128） | `SCREEN_MODE_TRY`／`KEEP`／`BACK` | `Displays.conf` | `SCREEN_WATCH` |

`AppData\Nocturne` には前例がある：カーネルは `System.log` をここへ残す（stream.c:108〜）。

## 2. 流れ

- **変更**：Settings の窓、タスクバー、どのプログラムも持ち主の ABI で頼む。持ち主は値を確かめてその場で効かせ、値が実際に変わったときだけ `keep_changed(&持ち主の keep)`（保存の印）と `watched_changed(&その報告の watched)`（見張りへの知らせ）を呼ぶ。
- **保存**：`kernel/src/sys/keep.c` が、最後の変更から 500 ms 後に持ち主の `text()` が書いた行を `store_save` で原子的に差し替える。眠り・休止・再起動・電源断の前には待たずに書く（pm.c:244・:306 の位置）。keep はキーを一つも知らない。キーと値の検査は持ち主の中。
- **起動時の復元**（kmain_late、init.c:83〜）：`audio_init` が登録（store_init より前）→ `store_start` の後に `wm_init` が登録 → `screen_modes_init`（新）が登録 → `keep_start` が kthread で各文書を読み、持ち主の `line()` が値を入れる → boot の task は読み終わるまで待つ（a51c4398 の usersettings.c:429〜431 と同じ形。boot task は wait queue で眠れない）→ init を起動。nvrm（init が起動する）の最初の申し出の時点で、モニターの選択が既に入っており、モードの設定は一回で済む。
- **他所の変更の知らせ**：見張りのハンドル（`N_KIND_WATCH`）を窓の事象の待ち行列に入れ、受け取れたら `n_watch_take` で取り、持ち主の読み出しで読み直す。500 ms の poll は無くし、試行中（15 秒まで）の秒読みの描き直しの timer だけ残す。

## 3. Settings の窓

- Display：モニターの欄（名前、今のモード、理由）、解像度とリフレッシュの一覧、試行、拒否、「Kept for this monitor」（`struct screen_modes.chosen`）、Smooth gradients。
- Mouse：Pointer speed。
- Sound：装置名、音量、消音。
- Language は外す（システムの中に使い手が無く、ブラウザーだけが使う）。今の言語の表（settings.ts:229〜342）はブラウザーの設定の窓へ移す。
- 任意の改善：Hyper-V の合成マウスは絶対座標で Pointer speed が効かない（wm.c:2186〜）。`desktop_settings.reserved` に読むだけの印を一つ足して Mouse の頁で理由を言う。タスクバーの音量の窓から Sound の頁を開く入口。

## 4. 規模

- 設定を一つ足す：持ち主の値・検査・`line`／`text`・ABI と、Settings の頁だけ。全部の設定を並べた表やビットは無い（a51c4398 の `N_USER_SETTING_*` と `apply()` は全部の鍵を知る中心だった）。
- 持ち主を一つ足す（キーの繰返し、時刻帯、配色、電源の時間、ネットの固定アドレスなど）：自分の `struct keep` を登録し、報告に `struct watched` を一つ付け、自分の文書を持つ。
- 持ち主が将来 userland に移るとき、保存も一緒に移る。
- 移植したプログラム：自分の設定は `n_app_data_folder` と `n_document_save`。システムの状態は持ち主の ABI と見張りで読む。システムの文書は読まない。

退けた案：750fecbd の desktop.conf と `--apply`（CLI の段、窓が置き場、窓を閉じた状態でタスクバーで変えた音量が再起動で失われた）／a51c4398（プログラムの設定がカーネルを通る、全部の鍵を知る中心、共有の一つのファイル、AP の近道 `user_languages_ap` まで要った）／共有の好みの文書／userland の常駐の部品（値の写しが二つ、入れ直す口が設定ごとに要る、nvrm より前の競り合い）／SCREEN_MODE_SAVED を戻す（どのプログラムでも利用者の選択を黙って変えられる口）／500 ms の poll／WM が全窓へ「設定が変わった」を配る形（話題の列挙が中心に戻る、窓の無い使い手に届かない）。

## 5. カーネルの中の形（ABI ではない）

`sys/watch.h` は土台で入った（`struct watched`、`watched_init`・`watched_open`・`watched_changed`・`watched_gone`・`watch_take`）。

`sys/keep.h`：
```c
struct keep {
    const char *name;   /* "Desktop", "Sound", "Displays" */
    /* A line of the part's document, or of an older system's: whether key is the part's; with
       apply, the value checked and put in place. */
    bool (*line)(const char *key, const char *value, bool apply);
    size_t (*text)(char *out, size_t cap);   /* the part's state now, as its document's lines */
    struct keep *next; bool read, dirty; uint64_t dirty_at;   /* keep's own */
};
void keep_register(struct keep *k);
void keep_start(void);
void keep_changed(struct keep *k);
void keep_flush(void);
bool keep_number(const char *s, int64_t low, int64_t high, int64_t *out);
```
- `keep_changed` は、その持ち主の文書を読み終わるまでは無視する。
- 文書の鍵（750fecbd・a51c4398 と同じで古い行がそのまま読める）：Desktop：`pointer_speed`（-2〜2）、`smooth_gradients=on|off`、昔の `deep_colour`（当てる）・`pointer_y`・`refresh`（捨てる）。Sound：`volume`（0〜100）、`muted`。Displays：`mode.<monitor>=<w>x<h>@<mHz>`（`=`・改行・前後の空白を含むモニター名は書かない）。

## 6. 消すもの・残すもの（a51c4398）

- 消す：`kernel/src/sys/usersettings.c/.h`、203〜206 の振り分けと AP の近道、`N_KIND_USER_SETTINGS`・`N_USER_SETTING_*`・`N_USER_SETTINGS_ALL`・`N_USER_LANGUAGES_MAX`、`n_user_settings_*`・`n_user_languages*`（ブラウザーが離れた後）、`wm_desktop_settings(_set)`（wm.h）・`screen_modes_choose/choice`（screenmode.h。内部の callback にする）、settings.ts の Language 頁、usersettings の試験二つ。
- 残す：`struct screen_modes.chosen`、`SCREEN_MODES_FIRST_BYTES`、SCREEN_MODE の 4 を空けておくこと、audio.c・wm.c の「変わったときだけ」の判定、pm.c の 2 か所の flush（呼ぶ先を `keep_flush` に）、ui.c の `chevron-up`・`chevron-down`・`language` の icon、pcitest の chosen の確かめ。
- init.c:106 の `user_settings_start` → `screen_modes_init(); keep_start();`。
- js.c :967〜1008・:1073〜1082・:1135、js_ui.js :358〜377・:399〜410、ui.d.ts :278〜319：見張りは名前を替えて残し（"screen"・"sound"）、言語の分は消す。
- 作業木 agent-aef0937b87f066f42 の分は取らない（置き換え無しに消すだけ、SCREEN_MODE_SAVED の復活、未実装の SYS_ENTRY_WATCH）。

## 7. 引き継ぎ

`hyperv.ps1` の `-Update` は C:\Users を残す（hyperv.ps1:8〜11）。keep_start の一時的なコード：自分の文書が無い持ち主だけが古い文書を読む（新しい方の `Settings.conf` を先に、無ければ `desktop.conf`）、`line(apply=true)` で入れ、すぐ書く。古い文書の各行は文書を持つ持ち主にも `apply=false` で見せ、誰のものかだけを問う。消すのは、読んだ持ち主が全部保存できて、全部の行がどれかの持ち主のものだったとき（desktop.conf は消え、`languages=` が残る Settings.conf は残る）。読み・保存・削除は `keep:` の行で serial に書く。

## 8. ui.d.ts の形（J）

```ts
/** What changed:
    - "screen": anything get() or modes() reports (the desktop's size and settings, whether smooth
      gradients can be shown, the monitor and its modes, a trial beginning or ending, the mode kept
      for it); a trial counting down is no change.
    - "sound": anything sound() reports (the volume, muting, what plays the sound). */
export type SystemChange = "screen" | "sound";
    /** Calls listener after something get(), modes() or sound() reports changed, whoever changed it
        (this program, another, the taskbar). Returns the function that stops it. Watching alone
        does not keep the program running. */
    watch(listener: (changed: readonly SystemChange[]) => void): () => void;
```
js.c：`h_user_languages`・`h_user_languages_set`・`h_settings_watch`・`deliver_settings` を `systemWatch(on)` に替える（`n_screen_watch` と `n_audio_output_watch` を ui の待ち行列に二つの token で入れ、`n_watch_take` で取って `internal.systemChanged`）。run_loop の条件（:1135）も直す。settings.ts：poll（:466〜473）を消し `desktop.watch` で読み直す。試行中だけ秒の境目で `modes()` を読み直す。ドラッグ中は音の更新を無視する（今の `dragging`）。

## 9. K2 の関数（案）

- wm.c（Desktop の持ち主）：`desktop_keep` を wm_init で登録。`SCREEN_SETTINGS_SET` で値が変われば `keep_changed`。`screen_watched` を持ち、`wm_screen_info_changed()`（wm.h）を出す。設定の変更、`wm_screen_resized`／大きさの変化、遠隔表示の始まりと終わり、scanout の差し替え（階調の可否）で `watched_changed`。`wm_syscall` の先頭（wm の lock の外）に `SCREEN_WATCH` を足す（今の分岐のままだと c=5 は screen_info の返事に落ちる）。
- screenmode.c：`displays_keep` を `screen_modes_init()` で登録。KEEP で選択が変われば `keep_changed`。`publish()` の後と KEEP で `wm_screen_info_changed()`。
- audio.c：`sound_keep` を audio_init で登録。setter で値が変われば keep と watched の両方へ。`audio_set_card`、`audio_remote_attach`／`detach` でも watched へ（attach／detach の呼び手が BSP で走るかは未確認）。`audio_output_watch()` を足す。
- pm.c の 2 か所は `keep_flush()`、init.c は `screen_modes_init(); keep_start();`。

## 10. 試験

- ゲスト `nativeabi/system-state`（tests/systemwatchtest.c）：見張りでないハンドルは `N_ERR_WRONG_KIND`、無いハンドルは `N_ERR_NO_HANDLE`、kind が `N_KIND_WATCH`、開いた直後は受け取れず take は 0。音量を変えると 1000 ms 以内に受け取れ、take が 1 以上、その後 0。同じ値は変化でない。消音でも受け取れる。音の変化で画面の見張りは受け取れない。`SCREEN_SETTINGS_SET` でポインターの速さを変えると画面の見張りだけ。3000 ms 以内に Sound.conf に `volume=<値>`、Desktop.conf に `pointer_speed=<値>`。閉じた見張りは待ち行列から消える。最後に始めの値へ戻す。
- 起動時の復元と引き継ぎ（scripts/test.py が試験の写しの C: に `store fill` で置く）：`AppData\Nocturne\Desktop.conf` に `pointer_speed=9`（捨てられる）と `pointer_speed=1`、`AppData\Settings\desktop.conf` に `pointer_speed=-2`・`volume=79`・`muted=off`・`refresh=60`。ポインターの速さが 1（自分の文書が古い文書に勝つ）、Sound.conf がある、desktop.conf が消えている、`dmesg | grep keep:` に引き継ぎと削除の行。音量 79 は既定 80 に近く音の試験を乱さない。他の試験がポインターを動かすかは guitest.c と wm*test.c で確かめる。
- pcitest：KEEP の後 3 秒以内に Displays.conf に `mode.pcitest-vga=` の行。TRY・KEEP・BACK・申し出で画面の見張りが受け取れる。
- ホスト tests/keep_hosttest.c（clang で keep.c を include し、格納庫・時計・kthread・wq を差し替える）：行の解析（注釈、空行、CR、前後の空白、`=` の無い行、上限）、文書を持つ持ち主は古い文書を当てない、Settings.conf が desktop.conf より先、削除の条件、読む前の `keep_changed` を無視、連続した変更は最後から 500 ms で一回、flush はすぐ、保存の失敗は印を残して次で書き直す、起動後の登録はすぐ読む。
- QEMU：qcow2 の overlay（qemu-visual.md:43）、スタートメニューの Shut Down で切ってから起動し直す（WHPX の system_reset は止まる）。Settings の頁が三つ、Sound の頁を開いたままタスクバーのホイールでつまみが追う、Settings を二つ開いて両方が追う、値を変えて Shut Down・起動で同じ値、serial に `keep:` の読み込み、古い desktop.conf の引き継ぎ。
- Hyper-V（ユーザー）：第 2 世代はモードの一覧が無い、拡張セッションでは装置名「Remote desktop」、`-Update` で引き継ぎが起きる、合成マウスでは Pointer speed が効かない。実機：Keep したモード（例 2560×1440@120）が再起動の後に一回の切り替えで出る。
