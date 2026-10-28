# プログラムとブラウザーの設定の設計（サブエージェントの報告、2026-10-27）

決定は `docs/settings-ownership-20261027.md`。食い違う所（番号：`SYS_WATCH_TAKE` 207・`SYS_ENTRY_WATCH` 208、`n_entry_changes` ではなく共通の `n_watch_take`、`N_WATCH_CHANGES_MAX`、既定の言語は今の `en-US` のまま）はそちらが勝つ。API は `user/include/webprefs.h`（土台で入った）。

## 1. プログラム一般

今ある道具：`n_app_data_folder(name)`（sys.c:692）、`n_document_save`（REPLACE で作り commit を WAIT、store.c:672）、`n_entry_replace`（名前を上書きする改名、同じ commit）、`n_document_claim`（文書ごとの協調の排他、document.c:727）、チャンネルと `n_channel_listen`（鍵を一つのプロセスだけが持てる：一つだけ起動・profile の錠）、共有ページ。足すのは C: の見張りだけ。設定に固有の ABI は作らない。各プログラムは自分のフォルダーに自分の形式で書き、起動時に読み、動いている間は見張る。起動の時に当て直す段は無い。

Firefox を移植した場合：prefs.js は `AppData\Mozilla\…`、安全な書き出しは `n_entry_replace`、親が設定を持ち子へは IPC、profile の錠は `n_channel_listen` の鍵か claim、監視は `n_entry_watch`。Go の `os.UserConfigDir` はすでに AppData（ports/go/nocturne.patch:2100）。

見張りを既に要している所：localStorage の storage 事象が 100 ms ごとに journal を見回る（js.c:4599 `storage_run_event`、`storage_poll_due=now+100`）。検索の履歴の有効を操作のたびに C: から読み直す（form_autocomplete.c:147）。

## 2. C: の見張りの意味

- 二つの型：`N_WATCH_NAME`（フォルダーの中の一つの名前。その名前の項目が今あるかは問わない）、`N_WATCH_FOLDER`（フォルダーの直下の全部。再帰しない）。
- 見る相手は名前で決め、項目の id では見ない（`n_entry_replace` で id が変わる：store.c:630 → ns_replace）。フォルダーは id で持つので改名されても続く。フォルダーが除かれたら `N_EVENT_CLOSED`。
- 数える時点：新しい READ の opening・`n_entry_info`・`n_folder_list` から見えるようになった時。名前の操作（作成、削除、移動の出入り、大小文字だけの改名、`n_entry_replace`）、EDIT の work が目録に入る `flush`（document.c:376：commit、最後の書き手の close、4096 ページ、書き手のスレッドの 5 秒）、REPLACE の `ns_take`（document.c:762）。write_at だけでは数えない（数えると知らせを受けた側が古い内容を読み、その後は知らせが来ない）。
- 数だけを持つ（飽和、レベルで RECEIVABLE、溢れない）。
- 権利は `n_entry_info` と同じ：アプリに見えない PRIVATE の変化は数えない。SYSTEM のフォルダーも読めるので見張れる。
- 費用：見張りの表はフォルダーの id で引く。見張りが一つも無ければ即座に戻る。目録への書込みの費用は変えない。
- 置き場所：目録を変える全ての所で漏れなく拾うため、`kernel/src/fs/namespace.c` の操作（ns_create・ns_remove・ns_move・ns_replace・ns_update・ns_take）の成功時に `struct volume` の観測の口から呼ぶ（ns_update は record.parent から folder を取る）。host/store ではこの口を NULL。本体は `kernel/src/fs/watch.c`。「store.c に集める」案は document.c の flush と replace を取りこぼす。

## 3. ブラウザー：持ち主と流れ

- 文書 `C:\Users\<名>\AppData\Browser\preferences`（`web_profile_folder(false)` の中）。読み書きの持ち主は `user/libc/webprefs.c`（webstorage・webcookie・webidb と同じ層）。検索の履歴の有効も同じ文書の一項目（今の印 `search-history.enabled` は廃止。履歴の中身は今のとおり form_autocomplete の二つの slot）。
- 形（UTF-8、LF、64 KiB まで）：
  ```
  Nocturne browser preferences 1
  languages=ja,en-US,en
  search-history=on
  ```
  名前は `[a-z0-9-]+`。最初の行が違えば全部既定。知らない行は書き直すときに残す。知っている項目の値が不正ならその項目だけ既定。文書が無ければ全部既定で、読むだけでは書かない。
- 変更：設定の窓で一回クリックすると `webprefs_set_*` が一つの項目を変える。文書を EDIT で開く（無ければ CREATE）→ `n_document_claim(wait)` → 読み直してその一行だけ替える → `n_document_save` で丸ごと差し替え → release。書き手が二人いても別の項目の変更を失わない。
- 伝達：差し替えが目録に入ると、ブラウザーの全プロセスの見張りが RECEIVABLE。各プロセスは主ループの待ちで受けて `webprefs_events` を呼び、文書を読み直して検査し、一覧が違えば `webprefs_languages_generation()` を進める。
  - ブラウザーの窓（窓ごとに別のプロセス、browser.c:1388）：見張りは `web_wait_queue()`（doc.c:1262）が登録し `web_wait_events` が渡す（今の言語の見張りと同じ道）。各 Window の realm と各 iframe（frame.c:257 で tick）は世代のずれを見て languagechange の task を立てる（js.c:8300）。`webnet_submit` は要求を受けた時点の値で Accept-Language を決める。
  - 取得の子（webfetch）：設定を知らない。値は要求の wire の一つの欄で届く。
  - Worker の子（専用・SharedWorker のサーバー）：自分の待ち行列（browserjsworker.c:612）に自分の見張りを置き、WorkerGlobalScope に languagechange（:658）。Worker の要求は持ち主の窓へ `NJW_LOAD` で渡る（:466）ので、Accept-Language は窓の webnet が付ける。
  - 保存の子・媒体の子：今は使わない。
  - 設定の窓のプロセス：書き、自分も見張る。
- 退けた案：カーネルが持つ（a51c4398）、共有の設定の文書（registry の型）、ブラウザーの設定の仲介プロセス（窓どうしに共通の親が無い）、設定の窓が各プロセスへチャンネルで配る（登録簿が無く後から起動したプロセスを取りこぼす）、見回り（100 ms）、要求ごとにカーネルへ問う（16b8490d：webfetch.c:928、prepare_outgoing は 1544 で跳びごとに呼ばれ、redirect の途中で値が変わりうる。Fetch では header list は fetch の段で一度決まる）、browser.c の ui session の中の窓（頁の長い task の間は応答しない。今の検索の履歴の窓はモーダルで主ループを止める：form_autocomplete.c:354、`ui_session_wait(s,-1)`）、about:preferences のような特権付きの頁、id で見張る、媒体に届いた時に数える。

## 4. ABI（webnet_wire と web.h）

webnet_wire.h（UA が付ける Accept-Language は作者の欄と別の欄で運ぶ。webfetch は CSP の報告の欄（webfetch.c:2133）と WebSocket の欄（:1987）を厳密に検査しており、混ぜるとこの境界が崩れる）：
```c
/* The request's Accept-Language as the browser chose it when the fetch began (Fetch "fetch",
   step 14): a struct webnet_wire_language after struct webnet_wire_fetch, its bytes after the
   top-level URL's. Sent on every hop unless the author's fields have Accept-Language; at most 128
   bytes of [0-9A-Za-z,;=.-] (CORS-safelisted), else the request fails. */
#define WEBNET_WIRE_ACCEPT_LANGUAGE /* a free bit; add it to WEBNET_WIRE_REQUEST_FLAGS */
struct webnet_wire_language { uint32_t length, reserved; };
```
web.h：
```c
- void web_autocomplete_settings(void);
+ /* Turns the search history off (webprefs), then removes every stored search. */
+ bool web_autocomplete_clear(void);
```
web.h:289 の注は「Atomics の待ちとブラウザーの設定の見張り」に直す。

16b8490d から残すもの：Fetch Metadata、destination ごとの Accept、移動の一本化、referrer、F12 の送った頭、js_navigator.js の凍った配列と languagechange の task の形（js.c:8300、browserjsworker.c:658）、WorkerGlobalScope の onlanguagechange、q の規則（rank ごとに 0.1 下がり 0.1 で止まる：webprefs へ移す）。替えるもの：`js_navigator_native.h`（host.languages を webprefs から）、`js_navigator.c`（消す。見張りは webprefs に入る）、doc.c の `web_wait_queue`・`web_wait_events`、js.c と browserjsworker.c の世代の源、webfetch.c の `append_accept_language`（`n_user_languages` を呼ばず wire の欄の値）と `ws_handshake`、試験（fetch_metadata_host_cases.c、check_fetch_metadata.py、check_navigator_native.py、navigatortest.c）。

## 5. ブラウザーの設定の窓

- 別のプログラム `C:\Nocturne\System\browsersettings`（`user/apps/browsersettings.c`、C と ui.h）。約 760×560、大きさを変えられる。左に頁の一覧「Languages」（icon `language`）と「Privacy」、右に scroll と card（form_autocomplete の窓と同じ部品）。待ちは `ui_session_queue(s)` に `webprefs_watch` を登録して受ける。
- 入口：ツールバーの時計（`c.history = tool(&m,"clock",…)`、browser.c:332）を歯車 `settings` に替え、`CMD_HISTORY` を `CMD_SETTINGS` にする。押すとそのプログラムを起動し、起動した app handle を窓が持ち、まだ動いていれば二つ目は起動しない。Ctrl+Shift+H（browser.c:2512）は今のモーダルの窓の入口なので除く。既にある窓を前へ出す ABI は無い（後で）。
- Languages の頁：card「Preferred languages」（番号、自分の名前（太字）、caption「英語の名前 · tag」、上・下・削除。最後の一つは削除できない）、その下に「Restore default」（既定と同じなら押せない）。card「Add a language」（検索の欄：自分の名前・英語の名前・tag の頭で絞る、目録のうち未選択を並べ、押すと末尾に足す。`webprefs_languages_fit` が偽になるものは押せなくし、理由「Websites accept at most 128 bytes of languages」）。card「What websites receive」（`Accept-Language: …` の実際の値を等幅で）。操作は即座に保存・即座に効く。保存に失敗したら頁の下に理由。
- Privacy の頁：今のモーダルの窓の中身をそのまま移す（Search history の toggle（既定 off）、説明の 4 行、置き場所の文、「Clear and disable」）。モーダルの窓（`web_autocomplete_settings`）は消す。form_autocomplete では `HISTORY_PREF` を除き、`history_enabled` は `webprefs_sync()` と `webprefs_search_history()`、`history_enable` は `webprefs_set_search_history`、`web_autocomplete_clear` を公開（中身は今の `history_clear`、古い印の文書があればそれも除く。前の印の「on」は引き継がない：off の側へ倒れるので安全）。

## 6. 仕様の手順

- languagechange（HTML NavigatorLanguage）：「queue a global task on the DOM manipulation task source given global to fire an event named languagechange at global, and wait until that task begins to be executed before actually returning a new value」。プロセスの写しが新しい一覧になった時点で、各 Window（各 iframe を含む）と各 WorkerGlobalScope が自分の次の機会に task を一つ立て、その task の始めで凍った配列を替え、isTrusted の Event を投げる。「The same object must be returned until …」は js_navigator.js の作りのまま。一覧が元に戻って見えた間はその realm は投げない。
- Accept-Language（Fetch「To fetch」の段 13・14）：「If request's header list does not contain Accept-Language, then user agents should append (Accept-Language, an appropriate header value)」。値は `webnet_submit`・`webnet_socket_open` の時点で一度決めて要求に入れ、redirect のどの跳びでも同じ。作者の値は残す（WPT fetch/api/basic/accept-header.any.js の "bzh"、xhr/send-accept-language.htm の "x-GameSpeak"）。CORS-preflight には付けない。値は常に CORS-safelisted（128 バイト以下、許された bytes だけ）。HTML の「user agents should use the same list … as for the HTTP Accept-Language header」を満たすため、一覧そのものを 128 バイトに収まる長さに限る。
- 検証（RFC 5646）：HTML は「valid BCP 47 language tag」を求め、§2.2.9 の valid は登録簿にあることまで含む。一覧は目録の tag だけ（どれも登録された subtag、§2.1.1 の大小）。読む時は ASCII で大小を無視して目録と照らし目録の形にし、重複を除き、目録に無いものは捨て、空なら既定。値は HTTP の頭へ入るので、この検査が頭への注入を防ぐ。webfetch も wire の欄の bytes を検査する（二重の守り）。
- WPT：`html/…/navigatorlanguage.html`（languages[0] と language が同じ）、`workers/interfaces/WorkerUtils/navigator/language.html`（Worker と窓の language が同じ）。languagechange を自動で試す WPT は無い（`interfaces/html.idl` の onlanguagechange と手動の get-navigatorlanguage-manual.html だけ）ので、ゲストの試験で補う。

## 7. 試験

- K1：`scripts/test.py nativeabi/entry-watch`：まだ無い名前の作成、`n_document_save`、`n_entry_replace`、削除、移動の出入り、大小文字だけの改名、EDIT の write は commit まで数えない、知らせの後の READ で新しい内容が見える、フォルダーを除いた後の CLOSED、`N_ERR_WRONG_KIND`・`N_ERR_INVALID`、待ち行列でのレベル、1000 回が 1000、見張りの有無で保存の速さが変わらない（ms を出す）。namespace.c に触れるので `build/host/store.exe tree-test <seed> 200000` を三つの種で。
- B：ホストの試験（製品の webprefs.c を n_document_*・n_entry_*・claim を記憶の上で真似る stub と組む。check_fetch_metadata.py と同じ作り）：文書が無い・最初の行が違う・値が不正→既定、知らない行が残る、大小のそろえ、重複、目録に無い tag、128 バイトの限り、q の規則、二人の書き手が別の項目を変えても両方残る、世代が変化の時だけ進む、割り当ての失敗の注入。
- C：ホストの fetch_metadata で wire の値が全部の跳びで同じ、作者の値が勝つ、不正な値を拒む、WebSocket にも付く。navigator_native の stub を webprefs に。ゲストの navigatortest で USERFOLDER を試験用のフォルダーに替え、`webprefs_set_languages` で窓・iframe・Worker のそれぞれに languagechange がちょうど一回、task の前は配列が同じ物、Worker と窓の language が同じ。
- QEMU（主担当が統合の後）：二つの窓と Worker に languagechange が一回ずつ、設定の窓で日本語を足して先頭へ、F12 Network の送った頭、再起動で残る、Privacy の検索の履歴、設定の窓を開いたまま動画が止まらない、目録の各文字が描けるか。

## 8. 統合の後の一行ずつ（台帳）

preflight に `Accept: */*` が無い（webfetch.c:1214、Fetch の CORS-preflight の段 2）。CORS-unsafe の判定に 1024 バイトの合計の規則が読んだ範囲（webfetch.c:476〜905）に無い。媒体の取得に Accept-Language が無い（media_http.c:166）。storage 事象の 100 ms の見回り。
