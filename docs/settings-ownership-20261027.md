# 設定の持ち主の作り直し（2026-10-27）

a51c4398（カーネルが言語の一覧まで含む利用者の設定を、決まった鍵と決まった文書で持つ）を、ユーザーは否定した：「why are userland PROGRAMs, forced to talk to the kernel to save settings? Why is it hardcoded? What if i port firefox to Nocturne OS, how will it save settings??」。続けて、全プログラムが共有する好みの文書の案に「Why do apps have to share a settings file?」、750fecbd の自分の desktop.conf と起動時の `--apply` に戻す案に「Cli should not have to be used!」「CLI IS BANNED」。求めは「proper well designed architecture that works with scale」。

設計は二つのサブエージェントが別々に作った（システムの設定／プログラムとブラウザーの設定）。下はその二つを照らし合わせて決めたこと。二つの報告は `docs/settings-ownership-20261027-system.md`（システムの設定）と `docs/settings-ownership-20261027-browser.md`（プログラムとブラウザーの設定）。報告と下の決定が食い違う所は、下が勝つ（番号、既定の言語、伝え方）。

## 決めたこと

1. **値を効かせる者が持ち主。** 持ち主は自分の文書に自分で保存し、自分で読み戻す。全部の設定を並べた表・ビット・共有の文書はどこにも作らない。
2. **プログラムの設定はプログラムの持ち物。** `n_app_data_folder(name)` の `AppData\<name>` に自分の形式で置く。カーネルも共有の文書も通らない。移植した Firefox は自分の profile に prefs を持ち、OS は何も知らない。OS が用意するのは一般の道具だけ（自分のデータのフォルダー、原子的な保存 `n_document_save`・`n_entry_replace`、claim、チャンネル、C: の見張り）。
3. **システムの部品の状態**（今はどれもカーネルの中）：
   - ポインターの速さ・滑らかな階調（`kernel/src/gui/wm.c`）→ `AppData\Nocturne\Desktop.conf`
   - 音量・消音（`kernel/src/dev/audio.c`。タスクバーの部品は wm.c、値の持ち主は audio.c）→ `AppData\Nocturne\Sound.conf`
   - モニターごとに選んだモード（`kernel/src/gui/screenmode.c`）→ `AppData\Nocturne\Displays.conf`
   - 保存の共通の仕組み `kernel/src/sys/keep.c` は鍵を一つも知らない。持ち主が自分の行を書き、keep は最後の変更から 500 ms 後に `store_save` で差し替え、眠り・終了の前（pm.c）には待たずに書く。
   - 起動時：kmain_late で、各持ち主が登録し、`keep_start` が各文書を読んで持ち主へ渡し、読み終わってから init を起動する。init は何もしない。CLI の段は無い。SCREEN_MODE_SAVED は戻さない（4 は空けたまま）。
   - Settings の窓は値も文書も持たない操作盤で、タスクバーと同じ立場。頁は Display・Mouse・Sound。「Language」頁は外す（システムの中に使い手が無い）。500 ms の poll は見張りに替える（試行の秒読みの描き直しだけ timer）。
4. **ブラウザーの好み**（言語の一覧、検索の履歴の有効）はブラウザーの持ち物：
   - 文書 `AppData\Browser\preferences`（`web_profile_folder(false)` の中）。形式と API は `user/include/webprefs.h`。実装は libc の `user/libc/webprefs.c`。
   - 各ブラウザーのプロセス（窓ごとに別のプロセス、Worker の子）は自分の待ち行列で文書を見張り、変われば読み直す。中継のプロセスは置かない（窓どうしに共通の親が無い）。
   - webfetch は設定を知らない。Accept-Language は fetch が始まる時（`webnet_submit`・`webnet_socket_open`）に一度決め、要求の wire の別の欄で渡す。redirect のどの跳びでも同じ値。作者が付けた Accept-Language が勝つ。
   - navigator.languages は HTML の手順どおり：写しが変わったら各 Window（iframe を含む）と各 WorkerGlobalScope に languagechange の task を一つ立て、その task の始めで凍った配列を替える。
   - 設定の窓は別のプログラム `browsersettings`（C と ui.h）。頁は Languages と Privacy。入口はツールバーの歯車。今のモーダルの検索の履歴の窓（`web_autocomplete_settings`）と `search-history.enabled` の印は消し、Privacy の頁と好みの文書の一行へ移す。
   - 言語は目録（今の settings.ts の表を C へ移す）の tag だけ。一覧は Accept-Language が 128 バイト（CORS-safelisted）に収まる長さまで。既定は `ja,zh`（日本語、次に中国語。2026-10-28 ユーザー：「jp or zh. fuck en.」）。値は `webprefs.h` の `WEBPREFS_DEFAULT_LANGUAGES` 一か所。
5. **見張り**は一つの種類 `N_KIND_WATCH`（13）と一つの取り出し `n_watch_take`（`SYS_WATCH_TAKE` 207）。開く口は用途ごと：`n_entry_watch(path, N_WATCH_NAME|N_WATCH_FOLDER)`（`SYS_ENTRY_WATCH` 208）、`n_audio_output_watch()`（`SYS_AUDIO_OUTPUT_WATCH` 209）、`n_screen_watch()`（`SYS_SCREEN_INFO` の `SCREEN_WATCH` 5）。カーネルの共通の部品は `kernel/src/sys/watch.h`（`struct watched`、`watched_changed`・`watched_gone`・`watched_open`・`watch_take`）。
   - C: の見張りは名前で見る（id では見ない：`n_entry_replace` で id が変わる）。フォルダーは id で持つ。数えるのは、新しい opening・`n_entry_info`・`n_folder_list` から見えるようになった時（名前の操作、EDIT の work が目録に入る flush、REPLACE の take）。write_at だけでは数えない。PRIVATE は数えない。目録を変える全ての所で拾うため、`kernel/src/fs/namespace.c` の操作の成功時に volume の観測の口から呼び、host/store ではその口を NULL にする。本体は `kernel/src/fs/watch.c`。見張りの表はフォルダーの id で引き、見張りが無ければ即座に戻る。
6. **欠番。** a51c4398 の 203〜206 と種類 12 は使い直さない（ABI の文書 §10「欠番のまま再利用しない」）。古い宣言（`SYS_USER_*`、`N_KIND_USER_SETTINGS`、`N_USER_SETTING_*`、`N_USER_LANGUAGES_MAX`、`n_user_settings_*`・`n_user_languages*`）は、使い手が全部離れた統合の最後に消す。
7. **引き継ぎ。** ユーザーの Hyper-V は `-Update` で `C:\Users` を残す。`keep_start` は、自分の文書が無い持ち主に古い `AppData\Nocturne\Settings.conf`（a51c4398）、無ければ `AppData\Settings\desktop.conf`（750fecbd）を読ませ（鍵は同じ：`pointer_speed`・`smooth_gradients`・`deep_colour`・`volume`・`muted`・`mode.<monitor>`）、すぐ自分の文書を書く。古い文書を消すのは、全部の行がどれかの持ち主のもので、読んだ持ち主が全部保存できたとき（`languages=` が残る Settings.conf は消さない）。

## 土台（main に先に入れたもの）

`common/abi.h`（207〜209、`N_KIND_WATCH`、`N_WATCH_*`）、`common/display_abi.h`（`SCREEN_WATCH`）、`kernel/src/sys/watch.h`・`watch.c`、`kernel/src/sys/syscall.c` の `SYS_WATCH_TAKE`、`user/include/nocturne.h`・`user/libc/sys.c`（`n_watch_take`・`n_entry_watch`・`n_audio_output_watch`）、`user/include/display.h`・`user/libc/gui.c`（`n_screen_watch`）、`user/include/webprefs.h`。208・209 と `SCREEN_WATCH` の振り分けは実装の塊が足す（それまでは N_ERR_UNSUPPORTED）。

## 実装の塊（同じファイルに触れない。重なる所は統合で合わせる）

| 塊 | ファイル |
|---|---|
| K1 C: の見張り | `kernel/src/fs/watch.c`・`watch.h`（新）、`fs/volume.h`、`fs/namespace.c`、`fs/store.c`、`sys/syscall.c`（`SYS_ENTRY_WATCH` の case）、`tests/entrywatchtest.c`（新）、`tests/runtests.c`（`nativeabi/entry-watch`）、`user/apps/syscallfuzz.c`（207・208）、ABI の文書 §1・§3・§10 |
| K2 システムの部品の状態 | `kernel/src/sys/keep.c`・`keep.h`（新）、`sys/usersettings.c`・`.h`（消す）、`sys/syscall.c`（`SYS_USER_*` の case を消し 209 を足す）、`gui/wm.c`・`wm.h`、`gui/screenmode.c`・`.h`、`dev/audio.c`・`.h`、`dev/pm.c`、`kernel/src/init.c`、`common/display_abi.h` の説明、`tests/systemwatchtest.c`・`keep_hosttest.c`（新）、`tests/usersettings*`（消す）、`tests/pcitest.c`、`tests/runtests.c`（`nativeabi/user-settings` の差替え）、`user/apps/syscallfuzz.c`（209）、ABI の文書 §9・§12 |
| J Settings の窓 | `user/apps/settings.ts`、`user/apps/js.c`、`user/apps/js_ui.js`、`rootfs/Nocturne/SDK/Types/nocturne/ui.d.ts`、README.md・README_EN.md の設定の段落 |
| B webprefs | `user/libc/webprefs.c`（新）、`tests/check_webprefs.py`・`tests/webprefs_host_cases.c`（新） |
| C ブラウザーの配線 | `user/libc/web/js_navigator_native.h`、`js_navigator.c`（消す）、`web/doc.c`、`web/js.c`、`user/apps/browserjsworker.c`、`user/libc/webnet.c`、`user/include/webnet_wire.h`、`user/apps/webfetch.c`、`tests/fetch_metadata_host_cases.c`・`check_fetch_metadata.py`・`check_navigator_native.py`・`navigatortest.c` |
| D ブラウザーの設定の窓 | `user/apps/browsersettings.c`（新）、`user/apps/browser.c`、`user/libc/web/form_autocomplete.c`・`.h`、`user/include/web.h`、新しいプログラムをビルドと C: へ入れる一覧 |

統合（主担当）：古い宣言の削除、`.claude/rules/map.md`、台帳、全体ビルド、ゲストの試験、QEMU。
