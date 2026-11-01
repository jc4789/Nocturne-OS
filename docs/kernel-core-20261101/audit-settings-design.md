# 設定の層の設計：敵対的な監査

## 依頼文（そのまま）

````
あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機、GUI が先）の敵対的な設計の監査役。読むだけで、コードも設計も変えない。ビルドも QEMU も動かさない。書くのは報告の一つのファイルだけ。main の今の版を読む。

対象は `docs/kernel-core-20261101/settings-layer-design.md`（OS 全体を通す設定の層の設計）。あなたの仕事は、この設計を壊すこと。作ってから気づくと高くつく欠陥を、作る前に見つける。合格を探さない。

# 持ち主の言葉（そのまま）
"VRR and gsync are settings, one is a os setting one is driver setting. This is something that is an overall issue not just a issue with vrr. we apply overship haphazardly to things, with out finding out what is normal. and we also have like verylittle real support for settings, overall."
"this is not just for vrr or display stuff. we lack a proper settings system. and stuff like this shouldn't be too hardcoded. we should be carefull, and maybe do an adversarial audit first of what we are missing."
"NO, you are not understanding. WE AREMISSING ABSTRACTION THAT NEEDS TO BE THERE. WE HAVE NO PROPER SETTINS SYSTEM ACROSS THE"（ここで切れている）
以前の決定の中の持ち主の言葉（`docs/settings-ownership-20261027.md`）："why are userland PROGRAMs, forced to talk to the kernel to save settings? Why is it hardcoded? What if i port firefox to Nocturne OS, how will it save settings??"、"Why do apps have to share a settings file?"、"Cli should not have to be used!"
規模の例：600 Hz の画面、3 枚の画面、ゲームと YouTube と Discord を同時に開いて、カーネルが原因のコマ落ちが無いこと。

# 読む所
- 規則：`AGENTS.md`、地図 `.claude/rules/map.md`。
- 材料：`docs/kernel-core-20261101/audit-settings-missing.md`、`audit-hardcoded.md`、`settings-ownership.md`、`settings-report.md`、`docs/settings-ownership-20261027.md`。
- 実物：`kernel/src/sys/keep.c`・`keep.h`、`kernel/src/sys/watch.h`・`watch.c`、`kernel/src/sys/channel.c`、`kernel/src/sys/object.c`、`kernel/src/gui/wm.c`・`screenmode.c`、`kernel/src/dev/audio.c`、`kernel/src/fs/store.c`、`kernel/src/init.c`、`kernel/src/sys/proc.c`・`process.h`、`common/abi.h`、`common/display_abi.h`、`docs/nocturne-native-abi.md` §12、`user/apps/settings.ts`、`user/apps/js.c`、`user/libc/js_ui.js` か `user/apps/js_ui.js`、`user/libc/webprefs.c`、`user/apps/browser_settings.h`、`ports/nvidia/display.c`・`nvrm.c`。codegraph（MCP の `codegraph_explore`）と rg で探す。
- 比べる物差しは公式の文書（Microsoft Learn、Apple の文書、NVIDIA・AMD の公式の文書）。`WebSearch`・`WebFetch`。ほかの OS のソースコードは読まない。

# 問い
1. 設計が今のコードの事実と合わない所。起動の順、錠の順、文脈（眠れない所からの変更、`wm.c` の錠の中からの通知）、`store` の能力、`channel` の能力（カーネルからアプリへの送信、流量制御、呼ぶ者の阻止）、見張りの形、プロセスの身元を実際に作れるか。
2. デッドロック・無制限の待ち・資源の枯渇：持ち主が返事をしないとき、窓が固まらないか。持ち主が自分の領域の鍵を読むとき。持ち主どうしが互いを読むとき。死んだ持ち主。巨大な宣言や値。頼みの洪水。変化の輪の大きさ。
3. この形で作れない設定を探す。`audit-settings-missing.md` §6 と `settings-ownership.md` の表の全部の行を、設計の宣言と scope と型で書いてみる。書けない行（モードの選択肢が画面ごとに違う、画面の配置、起動の項目、壁紙の文書、網の口ごとの記録、アプリごとの GPU、ショートカット、keyboard の配列の表、音の出口の一覧、利用者の切替え）はどれか。
4. 型と正規の文字列の欠陥：表示名と値の分離、単位、地域、長さ、文字の escape、保存の行の形、`scope 鍵=値` の曖昧さ、装置の身元に空白や `=` が入るとき、鍵の中の `/`。
5. scope の順の欠陥。一番狭い値が勝つで困る設定はあるか（機械が利用者を強制する政策、装置とプログラムの両方を持つ値、両方の組の値）。`program × device` の組み合わせ。
6. 権限と身元の穴。プログラムの身元の偽り、`C:\Programs\<アプリ>` と `C:\Nocturne\System\` の外のプログラム、Terminal から動かしたプログラム、tcc で組んだプログラム、`Settings` フォルダーの保護が store の他の道（名前の変更、置き換え、host の道具）で破れるか。
7. 試行と戻しの欠陥。期限の間に持ち主が死ぬ、再起動をまたぐ、二つの窓が試行する、画面が映らないのに確定ボタンが押せない。
8. 持ち主の言葉に反する所：プログラムがカーネルに話すことを強いられていないか、中央の表になっていないか、決め打ちが形を変えて残っていないか（例：`device_class` の文字列、`widget` の名、`page` の名、15 秒、`system`・`any`）。shim や一つの呼ぶ側のための包みになっていないか。POSIX・UNIX・Linux の形を写していないか。
9. 普通の OS の設定の仕組みが持っていて、この設計に無い大事な能力（公式の文書を出典に）。真似るためでなく、欠けを見つける物差しとして。
10. 持ち主の監査の層 L2〜L14 の要件がこの設計で本当に載るか。載らないなら、どの部品を今変えておかないと後で作り直しになるか。

# 書き方
各指摘に：どこ（設計の節、コードのファイル:行）、壊れ方の具体的な筋書き、重さ（作り直しになる／当たれば壊れる／きれいさ）、直す案を一つ（言い切りで）。良いと思った所は書かない。無理に数を増やさない。調べて壊せなかった所は、何をどう試したかを一行で書く。

# 報告
`docs/kernel-core-20261101/audit-settings-design.md` へ日本語で全文（この依頼文を先頭に）。コミットしない。返事にも要点を日本語で返す。サブエージェントをさらに出さない。Python で書くなら `newline='\n'`。
````

---

## 0. 結論

この設計のまま書くと、一段目の途中で次の八つに当たり、部品を作り直すことになる。

| 順 | 欠陥 | 指摘 |
|---|---|---|
| 1 | アプリの持ち主への頼みが今のチャンネルで作れない。カーネルはチャンネルから受け取れない。送りは満杯で捨てられる。 | F2 |
| 2 | 値を持たない仲立ちに「読む」の答えが無い。読みが持ち主を待ち、持ち主が固まると窓とゲームが固まる。 | F1、F11 |
| 3 | scope が一本の順である。「誰の値か」と「何についての値か」は別の軸である。保存の場所も、音量も、配置も、利用者の切替えもここで詰まる。 | F27、F14、F17 |
| 4 | `C:\Nocturne\System\` のプログラムは信頼できない。`js`、`tcc`、`clang`、`sh`、`browserjsworker`、`webfetch` がそこにある。設定の窓そのものの身元が `js` である。 | F32、F33 |
| 5 | 改行区切りの値と行の保存の形が矛盾する。実在のアプリ `Media Player` の身元には空白が入る。 | F21 |
| 6 | 機械の文書の置き場所 `C:\Nocturne\Settings` は、媒体の更新のたびに消える。 | F10 |
| 7 | 試行の値が 0.5 秒後に保存される。電源が落ちると映らないモードで起動する。 | F37 |
| 8 | §5 の「両方が入のときに VRR が動く」は、OS の値が driver の値を上書きする形である。§1 と公式の文書に反する。 | F41 |

重さの数は、作り直しになる 19、当たれば壊れる 24、きれいさ 3 である。

証拠の範囲は静的な読みだけである。ビルド、QEMU、実機を動かしていない。読んだ版は main の 388e7e9d である。筋書きはコードの筋から言っていて、再現していない。

## 1. 問い 1：今のコードと合わない所

### F1 値を持たない仲立ちに、読む道が無い

- **どこ**：設計 §2「仲立ちが持つもの」（値は持たない）、§2「保存の場所」（カーネルの部品は `sys/setting.c` の保存の thread）、§3.2 の表の `effective`、§3.2「読むことは `common/setting.c` が持ち主の代わりに行う」、§3.7 `SYS_SETTING_GET`。
- **筋書き**：設計の四つの文は両立しない。窓が `SYS_SETTING_GET("nvidia", "gsync", …)` を呼ぶ。仲立ちは値を持たない。行き先は `nvrm` への頼みか文書の読みの二つである。頼みなら `nvrm` が忙しい間は読みが止まり、§2 の「持ち主が忙しくても窓が開く」は宣言だけで成り立ち、行の値は出ない。文書なら最後の 0.5 秒の変更が見えない。カーネルの持ち主では、`sys/setting.c` の保存の thread が値を書くので、値は仲立ちの中にある。「値は持たない」は初めから偽である。driver が死ぬと「値は残る」と書くのに、残った値を読む口が無い。
- **重さ**：作り直しになる。
- **直す案**：仲立ちは「公開された値の写し」を持つ。持ち主は宣言と一緒に、置かれた値と解いた値を公開する。`SYS_SETTING_GET` は写しだけに答え、持ち主を決して呼ばない。写しは持ち主の死後も残る。§3.2 の `effective` を消す。値の持ち主と効かせる者は持ち主のままで、写しは宣言の写しと同じ扱いにする。

### F2 持ち主への頼みが、今のチャンネルで作れない

- **どこ**：設計 §3 の図「持ち主のチャンネルへ頼みの電文を送り、返事を待つ」、§3.7 `SYS_SETTING_OWN`。`kernel/src/sys/channel.c:338-343`、`:349-399`、`:330-336`、`:200-201`、`kernel/src/sys/channel.h` の `message_channel_post` の説明。
- **筋書き**：壊れる所は四つある。
  1. カーネルは返事を受け取れない。`message_channel_receive` は `user_ok_w`（`:375`）と `copy_to_user`（`:384`）で利用者の番地へ写す。カーネルの緩衝は通らない。
  2. カーネルは待って送れない。`message_channel_post` は締切 0 で `message_queue` を呼ぶ（`:342`）。待ち行列が満ちていれば `N_ERR_NOT_READY` で捨てる。`channel.h` も「満杯なら捨てる」と書く。窓の変更が黙って消える。`message_channel_send` はカーネルの thread では `app->pid` の NULL を読む（`:331-332`）。
  3. 電文に頼みの番号が無い。二つの窓が同時に `SET` すると、一本のチャンネルの返事をどちらの呼び手へ渡すかが決まらない。
  4. カーネルの電文は課金されない（`:200-201`、`message_new` の `app == NULL`）。`SET` の値の大きさは呼び手が決めるので、課金の無いカーネルの heap に入る。F13 を見る。
- **重さ**：作り直しになる。
- **直す案**：チャンネルを使わない。領域ごとの頼みの行列を `sys/setting.c` に持つ。呼び手の thread が頼みの記録を持ち、番号つきで行列へ入れて、自分の締切まで眠る。持ち主は `SYS_SETTING_OWN` の handle を `N_EVENT_RECEIVABLE` で待ち、`SYS_SETTING_TAKE`（頼みを受ける）と `SYS_SETTING_ANSWER`（番号と取った値）で答える。bytes は呼び手のプロセスに課金する。カーネルの呼び手も同じ記録で待てる。

### F3 `SYS_SETTING_SET` の引数がレジスターに入らない

- **どこ**：設計 §3.7 の表（`SET` は 領域、鍵、scope、値、旗、buf、cap の七つ）。`docs/nocturne-native-abi.md` §0（引数は五つ）、`user/libc/sys.c:38`、`user/include/nocturne.h:391`。
- **筋書き**：syscall の引数は `rdi`・`rsi`・`rdx`・`r10`・`r8` の五つである。七つの引数は渡らない。`GET` も五つで満杯なので、F11 の締切と F31 の二つの scope を足す余地が無い。
- **重さ**：当たれば壊れる（書き始めると必ず当たる）。
- **直す案**：`SYS_SETTING_GET`・`SET`・`RESET`・`CONFIRM`・`REVERT` は `struct n_setting_request *` を一つ受ける。欄は 領域、鍵、scope、値と長さ、旗、締切、返しの緩衝と容量である。要求の構造体は一度カーネルへ写す（`.claude/rules/kernel.md`）。

### F4 `wm.c` の錠の下から `setting_set` を呼ぶ

- **どこ**：設計 §3.9「カーネルの中の呼び手は `setting_set` を使う」、§3.2「`apply` は呼ぶ thread で呼ぶ」「眠ってよい文脈」。`kernel/src/gui/wm.c:2715-2735`（`desktop_thread` は `wm_lock` を持ったまま `handle_mouse`・`handle_key` を回す）、`:1403`、`:2457`、`:2696`（音量の変更）、`kernel/src/gui/screenmode.c:21`（画面のモードの錠が先で、WM の錠が後）。
- **筋書き**：`desktop_thread` は一番速い画面の周期の締切で走る（`wm.c:2713-2714`）。この thread が `wm_mutex` を持って音量の面の操作を処理する。`wm_mutex` は合成の thread も取る。設計どおりに `setting_set` を置くと、`apply` は `wm_lock` の下で走る。`apply` は「眠ってよい」ので、一つの持ち主の眠りが入力と全部の画面の合成を止める。これは規模の例の 600 Hz を壊す。タスクバーに画面の項目（配置の切替え、拡大）を足すと、`wm_lock` の下で画面のモードの錠を取り、`screenmode.c:21` の順に逆らう。`mutex_lock` は同じ thread の再入を許す（`kernel/src/sys/wait.c:416-418`）ので、自分自身では止まらない。逆順は別の thread との間で止まる。
- **重さ**：当たれば壊れる。
- **直す案**：`setting_set` は `wm_lock` の下で呼ばないと規則に書く。タスクバーは持ち主の直の関数（今の `audio_set_volume`）を呼び、持ち主が `changed` を言う。持ち主の直の関数は眠らない。ほかの領域へは `setting_post`（行列へ入れて戻る）を使い、層の thread が `apply` を呼ぶ。

### F5 起動の順：網は C: より先に走る

- **どこ**：設計 §4「`store_start` の後、init の前に全部読む」、§6 二段目 `network`。`kernel/src/init.c:44`（`net_init`）、`:49-51`（`store_init`・`store_start`）、`kernel/src/net/net.c:2457-2459`、`:2520-2523`（`net_init` が `dhcp_start` と網の thread を起こす）。
- **筋書き**：静的な IP を選んだ機械が起動する。読み戻しの前に DHCP が走り、IP と DNS を取る。読み戻しの `apply` が来ると、動いている接続の IP が変わる。USB の NIC は `usb_init` から来るので順はさらに揺れる。同じ問題は USB の入力の repeat にもあるが、害は無い。
- **重さ**：当たれば壊れる。
- **直す案**：`net_init` は装置の登録までにする。網の開始は `setting_start` の後に `kmain_late` が呼ぶ。C: が無い起動では既定の値で開始する。

### F6 見張りの形が足りない

- **どこ**：設計 §3.2「閉じれば領域は一覧から消え、見張りが鳴る」、§3.4。`kernel/src/sys/watch.c:8-10`（handle の状態は `seen` 一語）、`:15-21`、`:56-59`、`:61-79`。
- **筋書き**：
  - `watched_gone` は永久である。`gone` を立てると、その handle は `N_EVENT_CLOSED` になり、取り出しは `N_ERR_CLOSED` を返す。これを driver の死に使うと、ゲームの Mesa の見張りは死ぬ。`nvrm` が戻っても鳴らない。
  - 領域が今いないときの `SYS_SETTING_WATCH("nvidia")` の答えが無い。Mesa が `nvrm` より先に起動すると `N_ERR_NOT_FOUND` になり、変化を二度と知らない。
  - `SYS_SETTING_WATCH(NULL)` は全部の領域の輪を読む。handle の状態は `seen` 一語なので、領域ごとの読んだ位置を持てない。後から来た領域の位置も持てない。
  - `n_watch_take` と `SYS_SETTING_CHANGES` の二つの取り出しが同じ handle の準備の状態を動かす。
  - 輪の大きさと一つの記録の長さが設計に無い。タスクバーの音量の引きは一回の引きで何十もの変化を出す。全部の見張りが「全部を読み直す」へ落ちる。
- **重さ**：作り直しになる。
- **直す案**：領域の記録は名前で持ち、持ち主の有無と独立にする。持ち主の死は変化の一つで、`watched_gone` を使わない。`SYS_SETTING_WATCH(NULL)` は領域の一覧の変化だけを言い、窓は領域ごとに見張りを開く。設定の見張りは種類は `N_KIND_WATCH` のまま、自分の `object_ops` を持ち、取り出しは `SYS_SETTING_CHANGES` だけである。輪の記録は固定の大きさ（鍵の番号、実体の番号、世代）にし、同じ鍵の続く変化は一つに畳む。

### F7 持ち主の断りは同期でない

- **どこ**：設計 §3.2「受けるか断るかを持ち主が決める。取った値を返す」、§3.7 `SET` の戻り。`kernel/src/gui/screenmode.c:282-296`、`common/abi.h:454-459`。
- **筋書き**：画面のモードの断りは後から来る。`screen_modes_syscall` は願いを立てて戻る。driver は `n_screen_state` を受けて modeset を試み、後で `n_pci_screen_modes` の `done` で「出せなかった」と言う（`:282`）。`apply` が戻る時に取った値は分からない。待てばカーネルの `apply` が driver を無制限に待つ。待たなければ「取った値」は嘘である。HDR、回転、色の深さ、音の形式も同じである。
- **重さ**：作り直しになる。
- **直す案**：`apply` の答えを三つにする：取った（値）、断った（理由）、受け付けた（結果は後で）。「受け付けた」の鍵は、結果が来た時に `changed` と状態の「断った理由」で知らせる。窓はこの状態を読んで行に出す。

### F8 持ち主の契約の欠け

- **どこ**：設計 §3.2、§4「カーネルの部品 `display`」。`kernel/src/gui/wm.c:1176-1184`、`kernel/src/gui/screenmode.c:339`。
- **筋書き**：
  - `RESET` の後に `apply` へ渡す値が決まっていない。持ち主は `value` を受ける形しかない。
  - `apply(key, scope, value)` の `scope` が狭い値のとき、効いている値は別の scope から来ている。持ち主は自分で解く手段を持たない（`effective` は持ち主の操作で、解くのは `common/setting.c` である）。
  - 新しい画面が繋がったとき、持ち主はその画面の値を問う。関数の表にこの口が無い。
  - `display` の領域の効き目は `wm.c`（`wm_lock`）と `screenmode.c`（自分の mutex）の二つの部品にある。`struct setting_owner` は領域に一つなので、片方へ鍵で振り分ける手書きの `switch` が要る。
  - 読み戻しの `apply` と利用者の `apply` の区別が無い。`keep_changed` の `read` の旗に当たるものが無いと、読み戻しが全部の見張りを鳴らし、保存の印を付ける。
- **重さ**：当たれば壊れる。
- **直す案**：持ち主は `apply(key, target, effective, why)` だけを実装し、層が解いた効く値を渡す。`why` は 変更・読み戻し・試行・戻し である。`RESET` も層が解き直した値で `apply` を呼ぶ。実体が増えたら持ち主は `setting_resolve(key, target)` を呼ぶ。`display` の領域は `display`（モードと配置、持ち主 `screenmode.c`）と `compositor`（VRR の歩調、階調、持ち主 `wm.c`）の二つにする。

### F9 電源の前に、アプリの持ち主は書けない

- **どこ**：設計 §3.6「電源の前に書く（今の `keep.c` の良い所を残す）」、§2「アプリの持ち主は libc の受け取り口」。`kernel/src/dev/pm.c:327-337`、`:115-119`。
- **筋書き**：電源を切る順は `close_windows`、`stop_apps`、`keep_flush`、`store_sync` である。`nvrm` は窓を持たないので、`stop_apps` で止まる。止まる前の 0.5 秒の間の `nvidia/gsync` の変更は libc の中にあり、失われる。`keep_flush` はカーネルの部品しか書かない。
- **重さ**：当たれば壊れる。
- **直す案**：アプリの持ち主の受け取り口は 0.5 秒の遅れを持たず、`ANSWER` の直後に別の thread で保存を始める。`pm.c` は `stop_apps` の前に層の頼み `FLUSH` を全部の持ち主へ出し、一つの締切まで待つ。

### F10 機械の文書は媒体の更新で消える

- **どこ**：設計 §2「文書の置き場所」`C:\Nocturne\Settings\<領域>.conf`。`scripts/layout.py:14-15`、`:80`（`("C:\\Nocturne", "!", "")`）、`host/store/store.c:2308-2309`、`:2342`。
- **筋書き**：`layout.py` は fill のたびに `C:\Nocturne` を丸ごと消してから入れ直す。これは「システムの部分は丸ごと置き換える」という設計どおりの動きである。時間帯、既定の出口、網の設定は更新のたびに既定へ戻る。Hyper-V の `-Update` で `C:\Users` が残るのと食い違う。
- **重さ**：当たれば壊れる（更新のたびに必ず当たる）。
- **直す案**：`layout.py` の `"!"` を `C:\Nocturne` 全体でなく、自分が入れる子（`System`、`SDK`、`Licenses`、`Certificates`、`Firmware` と `rootfs/Nocturne` の各子）へ当てる。`Settings` は fill の対象にしない。`tests/` に「fill の後に `C:\Nocturne\Settings` が残る」host の検査を足す。

## 2. 問い 2：待ち・デッドロック・枯渇

### F11 頼みに締切が無い

- **どこ**：設計 §3.2「待っている頼みは閉じれば `N_ERR_NOT_FOUND`」、§3.7（締切の欄が無い）、§5「見張りで変えた瞬間に効かせる」。
- **筋書き**：
  - **持ち主が返事をしない**：`nvrm` は RM の中で止まれる（`ports/nvidia/display.c:94` の `FLIP_TIMEOUT_MS` は 3 秒である）。頼みの待ちは閉じるまで終わらない。設定の窓は JS の一本の thread なので、窓全体が固まる。「持ち主が忙しくても窓が開く」は、最初の行で崩れる。
  - **自分の領域を読む**：`apply` の中で `n_setting_get` を呼ぶと、受け取り口の thread が自分への頼みを待つ。永久に止まる。
  - **互いを読む**：持ち主 A の `apply` が B を読み、B の `apply` が A を読む。止まる。
  - **ゲームの起動**：Mesa の初期化が `nvidia/*` を読む。`nvrm` が止まっていればゲームも起動中に止まる。
  - **`RESET` の全部**：領域をまたぐので、固まった持ち主が一つあれば全部が止まる。
  - **試行の期限**：カーネルの thread が固まった持ち主へ `apply` すると、期限の thread が止まり、ほかの試行も戻らない。
- **公式の物差し**：`SendMessageTimeout` は呼び手が時間を決め、応答しない相手は待たない旗を持つ（[O1]、[O2]）。
- **重さ**：作り直しになる。
- **直す案**：読みは F1 で持ち主を呼ばなくなり、この筋の半分は消える。`SET`・`RESET`・`CONFIRM`・`REVERT` は要求に締切を持ち、libc の既定は 1 秒にする。時間切れは `N_ERR_TIMEOUT` で、頼みは行列から消える。持ち主が遅れて答えても捨てる。窓は行に「応答なし」を出す。受け取り口の thread からの `SET` は `N_ERR_BUSY` で断る。期限の処理は領域ごとに独立に進める。

### F12 受け取り口が driver の表示を止める

- **どこ**：設計 §3「libc の受け取り口（`n_setting_serve`）が検証、保存、見張りの知らせを担う」、§4 driver の行。`ports/nvidia/nvrm.c:380-397`（主の thread は `nvrm_serve`）、`kernel/src/fs/store.c:924-932`（`store_save` は `N_COMMIT_WAIT` で待つ）。
- **筋書き**：設計は受け取り口の thread を決めていない。`nvrm_serve` の行列で受けると、ゲームの RM の要求と同じ thread で保存が走る。`store_save` の commit の待ちの間、RM の要求が止まる。規模の例のゲームの flip がここで遅れる。別の thread でも、`apply` が表示の錠を長く持てば同じである。
- **重さ**：当たれば壊れる。
- **直す案**：`n_setting_own` は libc の中で自分の thread を二本（受ける、保存する）起こす。`apply` の規則に「flip の道が持つ錠を待たない。表示へは値を渡すだけで、次の commit で効かせる」と書く。

### F13 大きさの上限と課金が無い

- **どこ**：設計 §3.1、§3.6、§3.7（値、宣言、実体の大きさが無い）。`kernel/src/sys/channel.c:255-276`、`kernel/src/sys/keep.c:11`、`:109`。
- **筋書き**：
  - どのプログラムでも自分の領域を取れる（§2）。`SYS_SETTING_PUBLISH` で 1 GiB の文書を送ると、仲立ちの写しがカーネルの heap を取る。毎秒送り直せる。
  - `writer = any` の鍵に、でたらめな `program:<id>` や `device:*` の実体で書き続ける。値は実体ごとに残り（§3.3）、消す者がいない。文書が `READ_MOST` の 64 KiB を超えると、次の起動で読めない文書になり、§3.6 の `.bad` へ退き、全部の値が既定へ戻る。
  - §3.6「知らない行は残す」により、消えた鍵の行も消えない。
  - 同じ鍵の 0.5 秒ごとの書込みは止まらず、SSD に書き続ける。
- **重さ**：当たれば壊れる。
- **直す案**：写しと頼みの bytes を所有のプロセスの `messages` と同じ勘定に入れ、空きメモリーの半分の規則に従わせる。カーネルの持ち主の文書の読みは `READ_MOST` を外し、実際の大きさで読む。実体を作る書込みは、持ち主が受けた実体だけを残す（`apply` が実体を断れる）。窓は残った実体の一覧から消せる（F16）。

## 3. 問い 3：この形で作れない設定

判定は次のとおりである。「書ける」は設計のままで書ける。「欠陥」は書けるが、括弧の指摘で壊れる。「書けない」は宣言と scope と型で表せない。

### 3.1 依頼が名指しした十個

| 設定 | 書いてみた形 | 判定 |
|---|---|---|
| モードの選択肢が画面ごとに違う | `display/mode @ device:monitor:<id>`、`ENUM`、`TRIAL` | 欠陥（F15）。`struct n_setting_decl.options` は静的で、選択肢は状態にしかない。共通の検証は通らない。 |
| 画面の配置 | `display/layout @ ?`、`RECORD` | 書けない（F14、F27）。`settings-ownership.md` の scope は「モニターの組」で、設計に無い。 |
| 起動の項目 | `boot/timeout @ machine`、`INT`、`NEXT_START` | 書けない（F18）。値は `limine.conf` の中にあり、§3.6 の保存の形でない。カーネルに ESP の FAT を書く道が無い（`kernel/src` に FAT の語が 0 個）。 |
| 壁紙の文書 | `appearance/wallpaper @ user`、`STRING` | 欠陥（F24）。文書の型が無い。カーネルの合成が、どのプログラムが書いた道でも開く。 |
| 網の口ごとの記録 | `network/ipv4 @ device:nic:<mac>`、`RECORD` | 欠陥（F21、F24）。接続先ごとの Wi-Fi は `nic × ssid` で書けない（F27、F36）。 |
| アプリごとの GPU | `graphics/gpu @ program:<id>`、`ENUM` | 欠陥（F15、F33、F43）。効かせる者は各プロセスの Mesa で、持ち主の部品が無い。`js` の全アプリが一つの身元になる。 |
| ショートカット | `input/shortcuts @ user`、`RECORD` | 欠陥（F19、F35、L10）。値はプログラムを起こす道なので `writer = any` にできない。 |
| keyboard の配列の表 | 表は値にしない。`input/layout @ user`、`ENUM`、表は `C:\Nocturne\Layouts\` の文書 | 欠陥（F15）。 |
| 音の出口の一覧 | 一覧は状態。`audio/default-output @ machine`、`ENUM` | 欠陥（F15、F26）。 |
| 利用者の切替え | 全部の `user` の値が一度に変わる | 書けない（F17）。 |

### 3.2 `settings-ownership.md` の全部の行

| 分野 | 設定 | 書いてみた形 | 判定 |
|---|---|---|---|
| 画面 | 解像度・周波数 | `display/mode @ device:monitor`、`ENUM`、`TRIAL` | 欠陥（F7、F14、F15） |
| 画面 | 試して戻す | `effect = TRIAL` | 欠陥（F37〜F40） |
| 画面 | HDR | `display/hdr @ device:monitor`、`BOOL`、`TRIAL` | 欠陥（F7） |
| 画面 | 拡大 | `display/scale @ device:monitor`、`INT`、`%` | 書ける |
| 画面 | 配置 | `RECORD`、モニターの組 | 書けない（F14、F27） |
| 画面 | 主画面 | 配置の中 | 書けない（F14、F27） |
| 画面 | 回転 | `display/rotation @ device:monitor`、`ENUM`、`TRIAL` | 欠陥（F14） |
| 画面 | OS の VRR | `compositor/variable-refresh @ device:monitor`、`BOOL` | 欠陥（F41） |
| 画面 | VRR の申告 | 鍵でない。`display` の状態 | 書ける（状態の形は F15） |
| 画面 | Night light | `display/night-light @ user`、`BOOL`、強さ `INT`、予定は `RECORD` | 欠陥（F24：時刻の型が無い） |
| 画面 | 滑らかな階調 | `compositor/smooth-gradients @ user`、`BOOL` | 書ける |
| driver | G-SYNC の入り切り | `nvidia/gsync @ device:monitor`、`BOOL` | 欠陥（F20、F41） |
| driver | 窓か全画面か | `nvidia/gsync-scope @ device:monitor`、`ENUM` | 欠陥（F20） |
| driver | Monitor Technology | `nvidia/monitor-technology @ user\|program`、`ENUM` | 欠陥（F27：「全体」が利用者か機械か決まらない。F43） |
| driver | 垂直同期の上書き | `nvidia/vsync @ user\|program`、`ENUM` | 欠陥（F43） |
| driver | 優先する周波数 | 作らない | 書かない（表の決定どおり） |
| driver | 最大フレーム レート | `nvidia/frame-limit @ user\|program`、`INT` 20〜1000 | 欠陥（F24：「切」が範囲に入らない。F43） |
| driver | 低遅延モード | `nvidia/low-latency @ user\|program`、`ENUM` | 欠陥（F43） |
| driver | トリプル バッファ | `nvidia/triple-buffer @ user\|program`、`BOOL` | 欠陥（F43） |
| driver | 電源管理モード | 効く道が無いので登録しない | 書かない（§1 どおり） |
| driver | FreeSync | `amd/freesync @ device:monitor` と `@ program` | 欠陥（F27：ゲームごとかつ画面ごとが書けない） |
| driver | RM・NVKMS の引数 | `nvidia/<名> @ machine`、`NEXT_START` | 欠陥（F18：driver の再起動を言う形が無い） |
| GPU | アプリごとの GPU | 上の表 | 欠陥（F15、F33、F43） |
| GPU | 窓のゲームの最適化 | 設定にしない | 書かない（表の決定どおり） |
| GPU | Game Mode | `scheduler/game-mode @ user`、`BOOL` | 書ける |
| 音 | 音量と消音 | `audio/volume @ device:audio-output`、`INT`、`audio/muted`、`BOOL` | 欠陥（F26） |
| 音 | アプリごとの音量 | `audio/app-volume @ program`、`INT` | 欠陥（F27：出口ごとが書けない。session は出口ごとである [O3]。F28） |
| 音 | 既定の出口 | `audio/default-output @ machine`、`ENUM` | 欠陥（F15） |
| 音 | 入力の音 | `audio/input-volume @ device:audio-input`、`INT` | 書ける（機能が来たら） |
| 音 | 出口の形式 | `audio/format @ device:audio-output`、`ENUM` | 欠陥（F15） |
| 入力 | ポインターの速さ | `input/pointer-speed @ user\|device:input`、`INT` | 欠陥（F26、F30） |
| 入力 | 加速 | `input/acceleration @ user\|device:input`、`BOOL` | 欠陥（F26、F30） |
| 入力 | ダブルクリックの間 | `input/double-click-ms @ user`、`INT` | 書ける |
| 入力 | ホイールの行数 | `input/wheel-lines @ user\|device:input`、`INT` | 欠陥（F26） |
| 入力 | キーの repeat | `input/repeat-delay-ms`・`repeat-rate-ms @ user\|device:input`、`INT` | 欠陥（F26、F42：`input` の一つの類にマウスも入る） |
| 入力 | 配列 | 上の表 | 欠陥（F15） |
| 入力 | IME | `input/method @ user`、`ENUM` | 欠陥（F15）。機能は層の外 |
| 入力 | 主ボタン・ポインターの大きさ | `input/primary-button`、`input/pointer-size @ user`、`ENUM`・`INT` | 書ける |
| 電源 | 電源の計画（AC・DC） | `power/display-off-ac`・`-dc` のように鍵を二重にする | 欠陥（F27：電源の状態は対象の軸である） |
| 電源 | 画面を消す時間・眠る時間 | `power/display-off-s @ machine`、`INT` | 欠陥（F24：「しない」が範囲に入らない） |
| 電源 | 電源ボタンの動作 | `power/button @ machine`、`ENUM` | 書ける |
| 電源 | 蓋の動作 | `power/lid @ machine`、`ENUM` | 書ける |
| 電源 | 電源の変化の通知 | 読むだけの値 | 書けない（F18） |
| 時刻 | 時間帯 | `time/zone @ machine`、`ENUM` 約 400 語 | 欠陥（F15、F35：権限） |
| 時刻 | 時刻の同期 | `time/sync @ machine`、`BOOL` | 書ける（service は別の設計） |
| 時刻 | 手で時刻を変える | 保存しない操作 | 書けない（F18） |
| 時刻 | 地域の形式 | `locale/format @ user`、`ENUM` | 書ける |
| 時刻 | 表示の言語 | `locale/language @ user`、`ENUM`、`NEXT_START` | 書ける（文字列の表は別の設計） |
| 網 | IP の取り方 | `network/ipv4 @ device:nic`、`RECORD` | 欠陥（F5、F21、F24） |
| 網 | DNS | `network/dns @ device:nic`、`LIST` | 欠陥（F21） |
| 網 | TCP の緩衝 | 設定にしない | 書かない（表の決定どおり） |
| 網 | proxy | `network/proxy @ user`、`RECORD` | 欠陥（F36：資格情報） |
| 網 | Wi-Fi・VPN | 接続先ごとの記録と鍵 | 書けない（F18、F27、F36） |
| 外観 | 壁紙 | 上の表 | 欠陥（F24、F27：画面ごと×利用者） |
| 外観 | 色（テーマ・アクセント） | `appearance/theme`、`ENUM`、`appearance/accent`、`STRING` | 欠陥（F24：色の型が無い） |
| 外観 | 字の大きさ | `appearance/text-size @ user`、`INT` | 書ける |
| 外観 | 字体 | `appearance/font @ user`、`ENUM` | 欠陥（F15） |
| 外観 | アニメーション・透明 | `appearance/reduce-motion`・`reduce-transparency @ user`、`BOOL` | 書ける |
| 外観 | タスクバー | `shell/taskbar-edge @ user`、`ENUM` | 書ける（持ち主は `wm.c`） |
| 外観 | 開始の一覧と机の上の絵 | `shell/pinned @ user`、`LIST` | 欠陥（F19、F35、L10） |
| 外観 | 窓の整列 | `shell/snap @ user`、`BOOL` | 書ける |
| 外観 | 通知 | `shell/notifications @ user\|program`、`BOOL` | 欠陥（F33） |
| 外観 | アクセシビリティ | `appearance/*`、`input/* @ user`、`BOOL`・`INT` | 書ける（持ち主の機能が来たら） |
| 起動 | 起動の項目と待ち時間 | 上の表 | 書けない（F18） |
| 起動 | 起動の旗 | 層に入れない | 書かない（表の決定どおり） |
| 起動 | 起動のときのアプリ | `session/startup @ user`、`LIST` | 欠陥（F19、F33） |
| 起動 | 利用者と機械の分け方 | 仕組みそのもの | 欠陥（F27） |
| 起動 | サインイン | 機能は別の設計 | 書けない（F17、F36） |
| プログラム | プログラムの設定 | 層を使わない | 書ける |
| プログラム | 既定のアプリ | `shell/default-apps @ user`、`RECORD` | 欠陥（F19、L10） |
| プログラム | 起動の一時の値 | 層に入れない | 書かない（表の決定どおり） |
| プログラム | 許可 | `privacy/<権限> @ program`、`ENUM` | 欠陥（F33） |
| プログラム | 保存先・空き容量の管理 | `storage/*`。掃除は操作 | 書けない（F18：操作） |
| プログラム | 更新 | 範囲の外 | 書かない |
| プログラム | 遠隔デスクトップ | 実機の機能が無い | 書かない（§1 どおり） |

### 3.3 `audit-settings-missing.md` §6 に上の表と別に出る項目

| 分野 | 項目 | 書いてみた形 | 判定 |
|---|---|---|---|
| 画面 | 明るさ | `display/brightness @ device:monitor`、`INT` | 書けない（F18：値は画面の中にある） |
| 画面 | 画面ごとのオンとオフ、ミラー | 配置の中 | 書けない（F14） |
| 画面 | モードの選択を消す | `RESET` | 欠陥（F39：試行が付かない） |
| 色 | Auto HDR、夜間の色、RGB の範囲 | `BOOL`・`ENUM` | 書ける |
| 色 | 色の深さ | `display/depth @ device:monitor`、`ENUM`、`TRIAL` | 欠陥（F15） |
| 色 | 色の校正 | 文書の道 | 欠陥（F24：文書の型が無い） |
| 拡大 | 拡大の方式 | `nvidia/scaling @ device:monitor`、`ENUM` | 書ける |
| VRR | モニターの型ごとの有効 | 型は実体でない | 書けない（F26：型を一つの実体にする道が無い） |
| VRR | アプリの上書き | `compositor/variable-refresh @ program` | 欠陥（F28：切の画面で入になる） |
| 音 | アプリごとの出口 | `audio/app-output @ program`、`ENUM` | 欠陥（F15） |
| 音 | 立体音響 | `audio/spatial @ device:audio-output`、`BOOL` | 書ける |
| 入力 | タッチパッド、マウスごとの値 | `device:input` | 欠陥（F26） |
| アクセシビリティ | 高コントラスト、色フィルター、固定キー、ポインターの色、字幕 | `BOOL`・`ENUM`・`RECORD` | 書ける |
| アクセシビリティ | 拡大鏡、読み上げ | 機能は別 | 書ける |
| 電源 | 休止の有効、性能の方針 | `power/hibernate`、`power/profile @ machine` | 書ける |
| 網 | ホスト名 | `network/hostname @ machine`、`STRING` | 欠陥（F24：文字の制約が無い） |
| 網 | IPv6、MAC のランダム化 | `network/* @ device:nic` | 欠陥（F27：接続先ごと） |
| 時刻 | 12 時間と 24 時間、夏時間 | `ENUM`・`BOOL` | 書ける |
| 字体 | 既定の字体 | 上の表 | 欠陥（F15） |
| 外観 | アイコン | `shell/icons @ user`、`LIST` | 欠陥（F19） |
| 通知 | 邪魔しないで、優先 | `BOOL`・`ENUM` | 書ける |
| 記憶域 | 掃除、使用量 | 操作と読むだけ | 書けない（F18） |
| 利用者 | 作成、パスワード | 操作と秘密 | 書けない（F18、F36）。設定の外で正しい |
| 利用者 | 自動ログイン | `session/auto-login @ machine`、`STRING` | 欠陥（F17） |
| プライバシー | アプリごとの権限 | 上の表 | 欠陥（F33） |
| driver | デスクトップの色 | `nvidia/desktop-colour @ device:monitor`、`RECORD` | 書けない（F24：小数が無い） |
| driver | 回転 | 上の表 | 欠陥（F14） |

### F14 複数の鍵を一度に当てられない

- **どこ**：設計 §2「取引」、§6 二段目 `display`。
- **筋書き**：二枚の 4K を一本の DisplayPort MST に繋ぐ。A を 144 Hz にするには、先に B を 60 Hz に落とす必要がある。B を変える前の A の変更は断られ、A を変える前の B の変更は意味を持たない。設計の取引は一つの鍵なので、モードは `device:monitor` ごとに分かれ、配置は別の鍵である。解像度を変えて配置もずらす操作は、15 秒の試行が三本走る。途中の配置は穴が空く。一本だけ確定されると、残りが戻って壊れた形になる。

  公式の文書では、`SetDisplayConfig` は全部の path と mode を一度に受け、検証だけの `SDC_VALIDATE` と一時の適用（`SDC_SAVE_TO_DATABASE` なしの `SDC_APPLY`）を持つ（[O4]）。`ChangeDisplaySettingsEx` は `CDS_NORESET` で溜めて一度に当て、`CDS_TEST` で試す（[O5]）。
- **重さ**：作り直しになる。
- **直す案**：表示の鍵を一つにする。`display/arrangement @ <モニターの組の実体>` の `RECORD` が、組の全部のモニターのモード、位置、回転、主、オンとオフを持つ。一枚の画面は一枚の組である。旗 `N_SETTING_TEST` を足し、持ち主が当てずに答える。`RECORD` の中身は持ち主の形で、窓は `arrangement` の部品で描く。

### F15 選択肢が実体ごと・時々で変わる

- **どこ**：設計 §3.1（`options` は静的）、§2「検証は `common/setting.c`」、§3.1 の状態の項。
- **筋書き**：`ENUM` の検証は宣言の `options` で行う。モードの一覧、音の出口、時間帯、配列、字体、GPU は静的な表に書けない。状態の文書は「使えるか」しか形が書いていない。繋がっていないモニターの値は検証できず、§3.6「範囲の外の値は使わず」で読み戻しの時に消えて見える。今の `displays_line` はモードを検証せずに `kept` へ入れ、`pick` の時に合わなければ使わない（`kernel/src/gui/screenmode.c:156-158`）。この動きは設計の共通の検証では作れない。
- **重さ**：作り直しになる。
- **直す案**：宣言の型に「開いた `ENUM`」を足す。語の形だけを共通のコードが見る。選択肢は実体ごとの状態の欄 `options[語, 表示名]` として公開し、窓はそれを出す。値の検証は持ち主が `apply` で行う。読み戻しの時の共通の検証は形だけにする。

### F16 実体の一覧を得る口が無い

- **どこ**：設計 §3.3「繋がっていない装置の値も残し、窓は一覧に出して消せる」、§3.9「上書きを消せる」、§3.7。
- **筋書き**：`SYS_SETTING_GET` は scope を知っている者しか引けない。窓は、値が残った繋がっていないモニターや、上書きのあるプログラムを列挙できない。
- **重さ**：当たれば壊れる。
- **直す案**：写しの文書（F1）に、鍵ごとの「値のある対象の一覧」を入れる。`SYS_SETTING_DESCRIBE` がそれを返す。接続の有無は持ち主の状態から出す。

### F17 利用者の切替えを表せない

- **どこ**：設計 §3.3、§7「層は前提にしない」、§3.2。`kernel/src/fs/store.c:389-392`、`:397-412`、`kernel/src/sys/proc.c:255-258`、`settings-ownership.md`「層は利用者の切り替えを前提に scope を分けておく」。
- **筋書き**：`scope` の `user` に実体が無い。カーネルの部品の読み戻しは起動で一度だけである。利用者が変わると、全部の領域の全部の `user` の値が一度に変わる。契約の `apply` は鍵ごとの変更しか言わない。`nvrm` は起動で利用者の文書を読むので、利用者の入れ替わりを知らない。ログインの前の画面の値の出所も無い。L12 が来た時に、全部の持ち主の読み戻しと保存の道を書き直す。
- **重さ**：作り直しになる。
- **直す案**：持ち手を `user:<名>` と `machine` の実体にする（F27）。層に「今の利用者」を一つ持ち、変わった時に層が全部の領域へ `reload` を出す。持ち主は `apply(key, target, effective, why = reload)` を鍵ごとに受ける。一段目は利用者が一人のままで、この道だけを通す。

### F18 保存の形に載らない値がある

- **どこ**：設計 §3.2「保存は `common/setting.c` が持ち主の代わりに行う」、§3.1 `effect`。`boot/limine.conf`、`settings.ts:197-206`。
- **筋書き**：
  - 値が別の所にある：起動の待ち時間は `limine.conf`、明るさは画面の中、壁時計は RTC にある。層の文書に書くと、持ち主が別の道で変えた時に食い違う。
  - 読むだけの値：今の IP、電源の状態、使用量。状態の文書にあるが、鍵でないので通知と窓の行の形が無い。
  - 操作：時刻を今に合わせる、Wi-Fi を探す、音を試す、掃除する、繋がっていない装置を忘れる。値が無いので `SET` は嘘になる。
  - 効き方の実行時の答え：同じ `effect` でも、機械の状態で「すぐ」と「driver の再起動」が分かれる。`ChangeDisplaySettingsEx` は結果に再起動の要否を返す（[O5] の `DISP_CHANGE_RESTART`）。
- **重さ**：作り直しになる。
- **直す案**：宣言に三つ足す。`storage`（層か持ち主か）、`access`（読み書きか読むだけか）、型 `ACTION`（`SET` の値なしで持ち主を呼び、保存しない）。`SET` の答えに `N_SETTING_ANSWER_RESTART` の印を持たせる。起動の項目は ESP に書く部品ができるまで登録しない。

### F19 `LIST`・`RECORD` の競りが残る

- **どこ**：設計 §2「複数の窓の競り：鍵ごとの set で解く」。`user/apps/js.c:895-903`。
- **筋書き**：§2 は全部を読んで全部を書く道を消すと言う。`LIST` と `RECORD` の一つの鍵の中では同じ道が残る。二つの窓が `shell/pinned` や `input/shortcuts` に別の項目を足すと、片方の変更が消える。プログラムが自分のショートカットを登録するのも同じ道である。配置の `RECORD`（F14）も同じである。
- **重さ**：当たれば壊れる。
- **直す案**：`GET` は世代を返し、`SET` の要求は `if_generation` を持つ。合わなければ `N_ERR_CHANGED` を返し、呼び手が読み直す。窓の共通の部品がこれを扱う。

### F20 領域に持ち主が一人である

- **どこ**：設計 §3.2、§3.7 `SYS_SETTING_OWN(領域)`、§2「領域の取得」。`user/apps/init.c:133-146`。
- **筋書き**：`init.c` は NVIDIA の GPU ごとに一つの `nvrm` を起こす。二枚の GPU の機械では、二つ目の `nvrm` の `SYS_SETTING_OWN("nvidia")` は `N_ERR_EXISTS` で断られる。二枚目に繋がる画面の `nvidia/gsync` は、領域があるのに届かない。AGENTS.md の的は 2 ソケットの EPYC である。
- **重さ**：作り直しになる。
- **直す案**：領域の持ち主を「対象の範囲」つきにする。`SYS_SETTING_OWN(領域, 実体の接頭辞)` で、`nvrm` は自分の GPU の実体と、自分の画面の実体を持つ。全体の鍵（`user`・`program`）は、最初の持ち主が保存し、写しを全部の持ち主へ配る。

## 4. 問い 4：型と正規の文字列

### F21 保存の行の形と値の形が矛盾する

- **どこ**：設計 §2「列は改行区切り、記録は `名=値` を改行で区切る」、§3.6「`scope 鍵=値` の行」、§3.3 の身元。`scripts/layout.py:33`（`"mediaplayer": "Media Player"`）、`kernel/src/sys/keep.c:122-142`（`trim`、`=` で切る）、`kernel/src/gui/screenmode.c:388-396`（`monitor_writable` が今この問題を避けている）。
- **筋書き**：
  - `LIST` と `RECORD` の値は改行を含む。保存は一行である。`network/dns` の二つ目の DNS は次の行になり、知らない行として残る。
  - `RECORD` の値に `=` が入る。入れ子の配置は書けない。
  - `STRING` の値の両端の空白と `#`。今の `trim` は空白を消すので、読み戻すと変わる。
  - `program:Media Player` の scope には空白がある。実在のアプリである。`scope 鍵=値` を最初の空白で切ると、scope は `program:Media`、鍵は `Player …` になる。
  - USB の serial の文字列は装置が決める。`=`、改行、`#` を入れた USB 機器は、利用者の文書に好きな行を差し込める。悪意のある USB 機器が `time/zone` の行を書ける。
  - 宣言の文書（§2「正規の文書」）の形は設計に無い。同じ問題がある。
- **重さ**：作り直しになる。
- **直す案**：保存と電文の全部の欄を一つの符号で書く。バイトが `0x21`〜`0x7E` のうち `%`、`=`、`#`、`@` 以外、または `0x80` 以上の正しい UTF-8 はそのまま書き、ほかは `%XX` にする。一行は `対象 鍵=値` で、三つの欄はこの符号なので曖昧さが無い。`LIST` と `RECORD` は `common/setting.c` の中では要素の配列として扱い、保存では要素ごとに符号化して `,` で区切る。`RECORD` の入れ子は要素の値の中の符号で表す。

### F22 正規形が決まっていない

- **どこ**：設計 §2「値の形」、§3.2「同じ値の当て直しは変化でない」、§2「既定に戻すは値を消す」。
- **筋書き**：`+5`、`05`、`-0` は同じ整数で違う文字列である。比較を文字列で行うと、見張りが鳴る。`STRING` の空と値なしの区別が `SET` で無い。`LIST` の空と、空の要素が一つの列が同じ文字列になる。`true` と `TRUE` の扱いが無い。不正な UTF-8 の扱いが無い。NUL を含む値は `user_string` で切れる（`kernel/src/sys/syscall.c:125-139`）。
- **重さ**：当たれば壊れる。
- **直す案**：`common/setting.c` に型ごとの `canonical()` を一つ置く。入口で必ず正規形へ直し、直せないものは `N_ERR_INVALID` で断る。比較は正規形でだけ行う。値は長さつきで渡す（F3 の構造体）。空の列は要素数 0 として内部に持つ。

### F23 名前の文法が無い

- **どこ**：設計 §2「領域の取得」「文書の置き場所」、§3.3 の名前。`kernel/src/sys/channel.c:48-61`（名前は `memcmp`）、`common/store/volume.h:38`、`common/store/volume.h:151`（C: は大文字小文字を畳む）。
- **筋書き**：
  - `C:\Nocturne\System\` のプログラムは「空いた領域の名」を取れる。名に制限が無い。`..\..\Users\User\x` を領域名にすると `<領域>.conf` の道が外へ出る。カーネルの部品の保存の thread が、その道を `store_save(path, …, false)` で書く。これはカーネルの権限である（F13 の筋と重なる）。
  - `Display` と `display` は仲立ちでは別の領域で、C: では同じ文書である。一つの文書を二人の持ち主が上書きする。
  - 鍵の `/`、`@`、`.`、空白の扱いが無い。`領域 / 鍵 @ scope` の表記を文字列にすると曖昧である。
  - `C:\Programs\Media Player` の領域名は空白を持つ。
- **重さ**：当たれば壊れる。
- **直す案**：領域と鍵の文法を `[a-z0-9-]+` に固定する。プログラムの領域名は身元から作る（F33 の `app.<符号化した名>`）。文書の名は領域名そのものを文法の検査の後に使う。ABI で名前と scope を別の欄で渡し（F3）、`/` と `@` は文書の中の表記だけにする。

### F24 型が足りない

- **どこ**：設計 §3.1 の `enum n_setting_type`、`min`・`max`・`step`。
- **筋書き**：
  - 小数が無い。gamma、拡大の 1.25、色の補正は書けない。単位で割って整数にすると、窓の表示の桁が宣言に無い。
  - 特別な値が無い。最大フレーム レートは `切` か 20〜1000 である（`settings-ownership.md`）。画面を消す時間も `しない` がある。`min`・`max` では `0` を入れられず、範囲を広げると 1〜19 が通る。
  - 色が無い。アクセントは `STRING` になり、形の検証が持ち主ごとの手書きになる。L2 の色の token で全部に当たる。
  - 文書の道が無い。壁紙、色の校正、字体を `STRING` にすると、窓は文書の選択の窓を出せない。カーネルの合成が、`any` の者が書いた道を自分の権限（`user = false`）で開く。これは `STORE_PRIVATE` も読める代理の穴である（`kernel/src/fs/store.c:615`）。
  - `STRING` に長さと形の制約が無い。ホスト名、IP が手書きの検証になる。
  - 時刻の型が無い。夜間の色、邪魔しないでの予定を書けない。
- **重さ**：作り直しになる。
- **直す案**：型を足す：`NUMBER`（10 進の固定小数点、桁は宣言）、`COLOR`（`#rrggbb`）、`DOCUMENT`（道と、呼び手の権限で開いて持ち主へ handle を渡す）、`TIME`（一日の分）。`INT` と `NUMBER` に `special`（語と値の組）を足す。`STRING` に `max_bytes` と `pattern` の語を足す。`DOCUMENT` の値はカーネルが開かない。`SET` の呼び手が開いた handle を渡し、持ち主はそれを持つ。

### F25 表示の文字と値の分離が半分である

- **どこ**：設計 §3.1（`label`、`help`、`search`、`unit`、`options` の表示名）、§7「宣言の語を鍵にする」。`user/apps/settings.ts:64-69`。
- **筋書き**：`unit` は表示の文字で、`Hz` の 3 桁の桁数や `rateName` の判断（`settings.ts:64`）は宣言に載らない。状態の理由の文（`settings.ts:140-146`）は持ち主が出すので、一つの言語で固まる。§7 は「語を鍵にする」と言うが、宣言の文字そのものを鍵にすると、英語の直しで翻訳が切れる。
- **重さ**：きれいさ。
- **直す案**：`label`・`help`・状態の理由は最初から「文字の id と既定の文字」の組にする。一段目は既定の文字だけを使う。単位は `Hz`、`%`、`ms`、`s`、`px` の語と桁の欄で持ち、窓が書式を作る。

### F26 装置の身元の規則が事実と合わない

- **どこ**：設計 §3.3 の表と規則「機械を換えても同じ物なら同じ、同じ型の二台は別」。`dev/devid.c`、`ports/nvidia/display.c:382-403`、`common/edid.h:60-70`、`common/abi.h:468`（`monitor[32]`）。
- **筋書き**：
  - **持ち主が違う**：モニターの身元は driver が作る（`display.c:396-400`）。カーネルの `dev/devid.c` は EDID も接続口の名も持たない。「一つの部品が作る」は実機の道で成り立たない。
  - **足す規則が値を失う**：「同じ値の二台には接続口の名を足す」は、他の画面に依る。`common/edid.h` のコメントの例 `HPN375b-01010101` のように同じ serial は珍しくない。同じ型の 3 枚の画面（規模の例）で、一枚を抜くと残りの身元が変わり、値を失う。繋ぐ順でも変わる。
  - **長さ**：3 + 4 + 1 + 8 の 16 字に文字列の serial の 13 字と接続口の名を足すと 32 に入らない。切れると別の画面と同じ身元になる。
  - **文字**：EDID と USB の文字列は装置が決める。F21 を見る。
  - **規則と表が矛盾する**：音の出口と GPU は「PCI の位置」を使う。機械を換えると変わる。
  - **複合の装置**：キーボードとマウスを一つにした USB 機器は vendor・product・serial が同じである。マウスの値がキーボードに当たる。
  - **USB の port の道**：serial の無いマウスは、差す口を変えると別物になる。これは規則どおりだが、窓に古い実体が増える（F16）。
- **重さ**：作り直しになる（一度保存された鍵は後で変えられない）。
- **直す案**：身元は装置を知る者が `common/devid.h` の関数で作る。形は固定で、`<類>:<16 進の hash>` とする。hash の材料は次のとおりである。モニターは maker、product、数字の serial、文字列の serial で、二つの serial が空か `0`・`01010101` なら接続口の名を**常に**足す（他の画面を見ない）。USB は vendor、product、serial、interface の番号で、serial が無ければ port の道を足す。GPU は `rm_get_gpu_uuid_raw` の UUID のように装置の識別子があればそれを使い、無ければ PCI の subsystem と位置を使う。規則の文を「取り外せる物は機械をまたいで同じ、作り付けは機械の位置で決まる」に書き直す。表示名は別の欄で持つ。`n_screen_modes.monitor` はこの形に収まる。

## 5. 問い 5：scope の順

### F27 scope は一本の順でなく二つの軸である

- **どこ**：設計 §2「scope の順」、§3.1 `scopes`、§3.3 の名前。`settings-ownership.md`（解像度の scope は「装置」、配置は「モニターの組」、Monitor Technology は「全体とプログラム」）、`audit-settings-missing.md` §7（モードは機械の値）。
- **筋書き**：順「プログラム → 装置 → 利用者 → 機械」には、次の値が無い。
  - 装置の値が機械の値か利用者の値か。§2 の文書の置き場所は機械と利用者の二つで、`device` の行の置き場所が決まっていない。解像度は機械の値で、ポインターの速さは利用者×マウスの値である。`CDS_GLOBAL` は同じ表示の値を全利用者か利用者かで選ぶ（[O5]）。
  - `program × device`：ゲームごとかつ画面ごとの VRR、プログラムごとかつ出口ごとの音量（session は出口ごと [O3]）、ゲームごとの FreeSync で画面が違う場合。
  - `machine × program`：機械の既定のプログラムごとの値。
  - `nic × 接続先`、`AC・DC`、`モニターの組`：装置の類の一つに入らない。
  - 順の「装置 → 利用者」は利用者の値と機械の値の間に割り込むので、機械×装置の値が利用者の全体の値に勝つ。機械が決めた `display/scale @ device` が、利用者の好みの全体の値を消す。
- **重さ**：作り直しになる（名前、文書の形、ABI、`common/setting.c` の解決の全部）。
- **直す案**：scope を次の組にする。
  - 持ち手：`machine`、`user:<名>`。
  - 対象：なし、`<実体>`、`program:<身元>`、`program:<身元>+<実体>`。実体は持ち主が名付ける開いた文字列で、`monitor:…`、`monitor-set:…`、`nic:…+ssid:…`、`power:ac` を同じ形で書ける。

  宣言は受ける組の一覧を持つ。解決の順は `common/setting.c` の表で一度だけ書く：強制（F29）、`user × program+実体`、`user × program`、`user × 実体`、`user`、`machine × program+実体`、`machine × program`、`machine × 実体`、`machine`、既定。文書は持ち手ごとに一つで、対象は行の欄である。

### F28 一番狭い値が勝つと壊れる値がある

- **どこ**：設計 §2「一番狭い値が勝つ」、§6 二段目 `audio`。
- **筋書き**：
  - **音量は掛け合わさる**：装置の音量とセッションの音量は別のつまみで、全体の mix に入る（[O3]、`settings-ownership.md` の [W11]・[W12]）。`audio/volume @ program:Discord` が装置の値に勝つなら、全体の消音で Discord だけが鳴る。
  - **VRR は論理積である**：利用者が画面の VRR を切ると、`compositor/variable-refresh @ program:game` の入がそれに勝つ。監視の周波数を外れた画面でゲームが VRR を要求する。
  - **フレームの上限は最小である**：画面の上限とゲームの上限は小さい方が効く。
  - **能力は上書きできない**：申告の無い画面に狭い値で `入` を書いても、作れない。
- **重さ**：当たれば壊れる。
- **直す案**：解決器は置き換えだけを行う。掛け合わせ、論理積、最小は持ち主の計算で、別の鍵に分ける規則を書く：`audio/volume @ 実体` と `audio/app-volume @ program`、`compositor/variable-refresh @ 実体` と `compositor/variable-refresh-app @ program`（`従う`・`切`）。§3.10 の試験に「同じ鍵の scope は置き換えとして意味を持つか」の点検の表を足す。

### F29 強制の層が無い

- **どこ**：設計 §2「scope の順」。
- **筋書き**：機械の値は利用者の値に常に負ける。子供用の利用者に「音量の上限」「拡大の下限」を機械が強制できない。将来の管理者の層で、順そのものが変わる。Windows は政策の鍵を優先の先頭に置き、政策で決まった値を窓で固定して見せ、そう説明する（[O6]、[O7]）。Apple は値が管理者のものかを問う口を持つ（[O8]）。
- **重さ**：当たれば壊れる。
- **直す案**：持ち手に `forced`（機械の文書の行の印）を足す。解決の先頭である。`GET` の出所は `forced` を返し、窓は行を動かせない形で出す。`SET` は `N_ERR_DENIED` である。一段目は書く窓を作らず、解決と出所と窓の表示だけを通す。

### F30 窓がどの scope へ書くかが決まっていない

- **どこ**：設計 §3.9、§3.1 `scopes`。
- **筋書き**：`input/pointer-speed` の scope が利用者とマウスのとき、Mouse の頁のつまみがどちらへ書くかが無い。マウスの値が既にあると、利用者の値を書いても効かない。窓の行は動いて見えて、何も変わらない。§3.9 の「出所」を出しても、利用者の操作の答えにならない。
- **重さ**：当たれば壊れる。
- **直す案**：宣言に `write_default`（既定で書く組）を持つ。窓は「今効いている値の出所」へ書くのを原則とし、出所が既定か機械なら `write_default` へ書く。狭い scope の値がある間は、行の説明に出所を出す。

### F31 効いている値を一つの scope で問えない

- **どこ**：設計 §3.7 `GET`（scope は一つ）、§3.2 `effective(key, device, program)`。`settings-report.md`「『効いている値』と『その scope に置かれた値』を分けて読める」。
- **筋書き**：Mesa はゲームの中で `nvidia/frame-limit` の効く値を、自分のプログラムと今の画面で問う。`GET` の scope は一つである。`@ program:x` を問うと、置かれた値か、効く値かが決まらない。材料の要件が設計で消えている。
- **重さ**：作り直しになる。
- **直す案**：要求の構造体（F3）に `mode`（`placed`・`effective`）と持ち手と対象を別の欄で持つ。`effective` で対象の `program` を省くと呼び手の身元を使う。

## 6. 問い 6：権限と身元

### F32 `C:\Nocturne\System\` は信頼の境界でない

- **どこ**：設計 §2「権限」「領域の取得」。`scripts/layout.py:92`（APPS に無いものは全部 `System`）、`:31-36`、`kernel/src/gui/wm.c:1064-1065`（設定は `SYSTEM "js"` に `settings.js`）。今の `build/programs` の一覧。
- **筋書き**：`System` には次がある：`js`、`tcc`、`clang`、`clang++`、`ld.lld`、`sh`、`uihost`、`browserjsworker`、`browsermediaworker`、`browsermseworker`、`browserstorageworker`、`webfetch`、`fetch`、`syscallfuzz`、`gotest`、`rusttest`。
  - `js C:\Users\User\evil.js` は、`system` の書き手で全部の鍵を書く。
  - `tcc -run evil.c`（`ports/tcc/runmain.c`）は、`system` の身元で任意のコードを走らせる。Terminal から打てる。
  - `browserjsworker` は Web の Worker の JS を走らせる。QuickJS の穴を突いたページは、`system` の身元で `display/*` と `C:\Nocturne\Settings` を書ける。FFmpeg を走らせる `browsermediaworker` も同じである。
  - 逆に、ブラウザーの本体 `C:\Programs\Browser\browser` は `system` でない。
  - 設定の窓の身元は `js` なので、窓に権限を渡すことと全部の JS に渡すことが同じになる。
- **重さ**：作り直しになる。
- **直す案**：権限をフォルダーでなく、プログラムの文書の属性で決める。`store fill` の属性 `--settings-writer` を、`init`、`nvrm`、`settings`、`volume` の文書にだけ付ける（`layout.py` の表）。アプリは属性を変えられない。`settings` は `js` の実行部と `settings.js` を埋め込んだ専用の ELF にする（`about` も同じ形でよい）。`browser*worker` と `webfetch` を `C:\Programs\Browser\` へ移す（`layout.py` の APPS）。

### F33 プログラムの身元の形が足りない

- **どこ**：設計 §3.3「`C:\Programs\<アプリ>\…` は `<アプリ>`、`C:\Nocturne\System\<名>` は `<名>`」「`n_app_data_folder(name)` の name もこの身元に揃える」、§2「領域の取得」。`kernel/src/sys/proc.c:137-143`、`:201`、`user/libc/sys.c:790-796`、`user/libc/webstorage.c:24`、`user/apps/init.c:77`、`kernel/src/fs/store.c:655-659`。
- **筋書き**：
  - **騙り**：`C:\Programs` に `STORE_SYSTEM` は無い（`layout.py:87` の属性は空）。どのアプリも `C:\Programs\nvidia\x` を作れる。その身元は `nvidia` である。「自分の id と同じ名の領域だけを取れる」により、`nvidia` の領域を取れる。AMD の機械、`nvrm` の前、`nvrm` の死後に、偽の NVIDIA の頁が窓に出る。二段目の `time`、`input` が未実装の間は、`C:\Programs\time\x` が `time` を取る。
  - **同じ名**：`C:\Programs\js\x` は `C:\Nocturne\System\js` と同じ身元で、`program:js` の上書きと共有する。
  - **`js` の全部**：TypeScript のアプリは全部 `program:js` である。`about` と `settings` と利用者のアプリが同じ音量を持つ。
  - **フォルダーの粒度**：`C:\Programs\Steam\gameA` と `gameB` は同じ身元である。ゲームごとの値が書けない。
  - **外のプログラム**：tcc で組んだ `C:\Users\User\a` の身元の規則が無い。`program:` の値を置けない。
  - **ブラウザーの家族**：`webfetch`、`browserjsworker`、`browserstorageworker` は `n_app_data_folder("Browser")` を使う（`web_profile_folder`）。「身元に揃える」を入れると、身元が違うので全部の子が別のフォルダーへ行く。`init` の `"Nocturne"` も同じである。
  - **隔離にならない**：`n_app_data_folder` は道を組むだけで、どのアプリも他のアプリの `AppData` を直接開ける。揃えても §5.4 の欠けは閉じない。
  - **文字列の比べ**：C: は大文字小文字を畳む。`C:\PROGRAMS\…` で起動した時に、文字列で比べた身元は綴りで変わる。
  - **作れるか**：`process_new(program_name(path))` の所で作れる。`name[32]` は短いので別の欄が要る。
- **重さ**：作り直しになる。
- **直す案**：身元は `proc_launch` が開いた文書の親の folder id を辿って作り、`struct process` の文字列の欄に持つ。形は `system/<名>`、`programs/<フォルダー>/<名>`、`users/<利用者からの相対の道>` である。ABI には不透明な文字列として出す（L10 の登録が来たら差し替えられる）。アプリの領域名は `app.<符号化した身元>` に固定し、`nvidia` などの名は `--settings-writer` の者しか取れない。`n_app_data_folder` は変えず、§3.3 の「揃える」の文を消す。`settings.js` 以外の TS アプリの身元は `users/…` や `programs/…` の道を持つ専用の ELF ができるまで `system/js` で、`program` の scope を受ける鍵は窓に「スクリプトは区別しない」と出す。

### F34 `Settings` の保護は道で破れる

- **どこ**：設計 §2「`Settings` の二つのフォルダーへの書込みは store が system のプログラムとカーネルに限る」。`kernel/src/fs/store.c:655-659`（`place_may_change` は親と自分の属性だけを見る）、`:851-917`、`:397-412`、`:436`（`Users` の属性は 0）、`common/store/namespace.h:33-36`、`kernel/src/sys/proc.c:255-258`。
- **筋書き**：
  - `AppData\Nocturne` は保護されない。アプリは `n_entry_move("…\AppData\Nocturne", "…\AppData\x")` を呼べる。親 `AppData` と自分に `STORE_SYSTEM` が無いので通る。次に新しい `AppData\Nocturne\Settings\display.conf` を作る。次の起動でカーネルがこれを読む。`folder_at` は既存のフォルダーに属性を足すだけで（`store.c:366-371`）、中の文書を疑わない。
  - `C:\Users` は属性 0 である。`user_ready` は名前の順で最初のフォルダーを利用者にする（`:403-405`、鍵は畳んだ名の順）。アプリが `C:\Users\A` を作ると、次の起動から利用者が入れ替わる。全部の文書が攻撃者のものになる。
  - `store` の `user` は真偽の一つである。「system のプログラム」の段が無い。`nvrm` は `user = true` で入るので、今は `C:\Nocturne\Settings` に書けない。
  - 利用者の値の文書の場所を決める `USERFOLDER` は起動元が渡せる（`proc.c:255`、`sys.c:782`）。悪いアプリが `--settings-writer` のプログラムを起こして、別の場所の文書を読ませられる。
  - host の道具 `host/store/store.c` は媒体を直接書く。これは機械を手にしている者の話で、走っている系の穴でない。
- **重さ**：当たれば壊れる。
- **直す案**：新しい属性 `STORE_FIXED`（名前と場所を変えられない、中身は変えてよい）を `C:\Users\<名>`、`AppData`、`AppData\Nocturne` に付ける。`C:\Users` は `STORE_SYSTEM` にし、利用者を作るのは系の操作にする。`place_may_change` の `bool user` を `enum store_caller`（kernel、settings-writer、app）に換える。層の libc は道を `n_user_folder` でなく、カーネルが答える `SYS_SETTING_FOLDER` で得る。

### F35 書き手の語が足りない

- **どこ**：設計 §2「権限」、§3.1 `writer`。`docs` の [O3]。
- **筋書き**：
  - 自分だけが書ける語が無い。プログラムは自分のセッションの音量を自分で変えてよい。他のアプリのものは触らない（[O3]）。`system` ではアプリが書けず、`any` では他のアプリを消音できる。
  - `any` は scope を問わない。`nvidia/frame-limit @ program:other` を誰でも書ける。
  - 時間帯、網、起動は「機械」で権限が要る（`settings-ownership.md` [W20]）。利用者の層の前は `system` しかなく、`js` で通る（F32）。
- **重さ**：当たれば壊れる。
- **直す案**：`writer` を `settings-writer`、`self`（対象の `program` が呼び手の身元と同じとき）、`any` の三つにする。判定は持ち手と対象を見る。

### F36 読むのは誰でも、で秘密が漏れる

- **どこ**：設計 §2「読むのは誰でも」、§7「秘密の保管は別の設計」。
- **筋書き**：`network` の領域に Wi-Fi の鍵、VPN、proxy の資格情報が入ると、全部のアプリが `GET` で読める。§7 は別の設計と言うが、宣言に「秘密」の語が無いので、二段目の書き手は `STRING` に書く。保存も平文である。`audit-settings-missing.md` §5.6 の `agent.conf` と同じ形になる。
- **重さ**：当たれば壊れる。
- **直す案**：宣言に `reader`（`any`、`settings-writer`）を足し、`STRING` の鍵に秘密を入れない規則を書く。秘密の保管ができるまで、資格情報を持つ鍵を登録しない。

## 7. 問い 7：試行と戻し

### F37 試行の値が保存される

- **どこ**：設計 §3.5、§3.6「最後の変更から 0.5 秒後に書き」、§3.2 `apply` と `changed`（試行の印が無い）。`kernel/src/gui/screenmode.c:509-516`（今は `KEEP` の時だけ `keep_for` する）。
- **筋書き**：利用者が映らないモードを試す。画面が消える。0.5 秒後に層が文書へ書く。15 秒の前に電源ボタンで切る。次の起動で読み戻しがそのモードを当てる。今のコードではこうならない。`trial_from` の戻しも記録も失われ、画面は消えたままになる。`nvrm` の死も同じで、再起動した `nvrm` は試行の値を文書から読む。`keep_flush` は電源の前にこの値を確実に書く。
- **重さ**：当たれば壊れる。
- **直す案**：試行の値は保存しない。`apply` の `why = try`（F8）で渡し、層の保存は確定まで前の値を書き続ける。写し（F1）の値には `trial` の印を付ける。読み戻しは保存された値だけを当てるので、再起動で試行は消える。

### F38 試行の記録の持ち主が二人になる

- **どこ**：設計 §3.5「仲立ちが前の値と期限を持ち」。`kernel/src/gui/screenmode.c:254`、`:273`、`:289-291`。
- **筋書き**：持ち主は自分で試行を終わらせる。driver が出せないと言えば戻す（`:289`）。モニターが抜けると切る（`:254`）。別のモニターが来ると切る（`:273`）。仲立ちはこれを知らない。仲立ちの記録は残り、15 秒後に前の値を当て直す。その時には別の画面が同じ実体で動いている。窓は「確定」を出し続け、押すと断られたモードが確定する。
- **重さ**：当たれば壊れる。
- **直す案**：持ち主から `trial_end(key, target)` を受ける口を足す。実体が無くなった時と断りの時に呼ぶ。仲立ちは記録を消し、見張りを鳴らす。`CONFIRM` は「試行の値がまだ効いている」時だけ通す。

### F39 試行の競りと抜け道

- **どこ**：設計 §3.5、§3.7 `RESET`・`SET` の旗。`kernel/src/gui/screenmode.c:493`。
- **筋書き**：
  - **旗なしの `SET`**：`effect = TRIAL` の鍵に `N_SETTING_TRY` なしで書く道を設計は禁じていない。アプリが映らないモードを直に保存できる。
  - **`RESET`**：モードの既定へ戻す操作に試行が無い。推奨のモードが映らないモニターで、戻せなくなる。`RESET` の全部は全部のモニターを同時に変える。
  - **二つの窓**：窓 B が窓 A の試行の途中で試行すると、「前の値」が A の試行の値になる設計にすると、期限で A の値へ戻る。今のコードは `trial_on` の間は `trial_from` を変えない（`:493`）。これを設計の文に書いていない。窓 B は窓 A の試行を確定できる。
  - **期限と固まった持ち主**：アプリの持ち主が固まると、戻しが届かない（F11）。
- **重さ**：当たれば壊れる。
- **直す案**：`effect = TRIAL` の鍵は `TRY` か、読み戻し以外の変更を `N_ERR_INVALID` で断る。`RESET` も `TRY` を受ける。試行は鍵と対象ごとに一つで、新しい試行は前の値を引き継いで期限を延ばす。`CONFIRM` は試行を始めたプロセスか `settings-writer` だけが通す。

### F40 試行の期間と残りの時間の欄が無い

- **どこ**：設計 §3.5「試行の期間は 15 秒で宣言に持つ」、§3.1（欄が無い）。`user/apps/settings.ts:205`、`:315-326`、`common/display_abi.h:67`。
- **筋書き**：`struct n_setting_decl` に期間の欄が無い。窓の「Going back in … s」は残りの時間を要る。`GET` の戻りに無い。
- **重さ**：きれいさ。
- **直す案**：宣言に `trial_ms` を足し、写しの試行の印に期限の時刻を入れる。窓は `n_clock` で残りを計算し、状態を読み直さない。

試行で壊せなかったものは §11 に書く。

## 8. 問い 8：持ち主の言葉に反する所

### F41 VRR の「両方が入」は OS が driver を上書きする形である

- **どこ**：設計 §5「OS の値は driver の値を上書きしない。両方が入のときに VRR が動く」、§1「互いを上書きしない」。
- **筋書き**：OS の既定は切である。G-SYNC の検証済みのモニターで driver は入である（`settings-ownership.md` [N5]）。設計どおりでは VRR は動かない。利用者は NVIDIA の頁で G-SYNC を入れても何も起きず、別の頁の OS の値を探す。切の OS の値は driver の入を消しているので、上書きそのものである。

  公式の文書では、OS の切り替えの既定は切で、効果は VRR を持たない全画面のゲームへ足すことである。driver の設定を置き換えず、上書きしない（[O9]）。OS の切替えが切の時に driver の VRR が止まるとは書いていない。
- **重さ**：当たれば壊れる。
- **直す案**：表にして書き直す。

  | OS の値 | driver の値 | 全画面の直接表示で VRR を求めるゲーム | 合成の面 |
  |---|---|---|---|
  | 切 | 切 | 固定 | 固定 |
  | 切 | 入 | VRR | `nvidia/gsync-scope` が窓も含むなら VRR の歩調、ほかは固定 |
  | 入 | 切 | 固定 | 固定 |
  | 入 | 入 | VRR | VRR の歩調（VRR を求めないゲームへも足す） |

  `nvrm` は `compositor` の値を読まない。合成は driver の `shown` の申告（VRR で動いているか）を読み、自分の歩調を決める。

### F42 決め打ちが形を変えて残る

- **どこ**：設計 §3.1 `device_class`、`widget`、`page`、`group`、§3.9「driver の領域は OS の頁と別の場所に出る」、§2 `system`・`any`、§3.5 の 15 秒。
- **筋書き**：
  - **`device_class`**：閉じた語の表で、`dev/devid.c` が知る。タブレット、ゲームパッド、プリンター、AMD の出力を足すと、カーネルを組み直す。アプリの driver は新しい類を作れない。`input` の一つの類にマウスとキーボードが入るので、キーの repeat の頁にマウスが並ぶ。
  - **`widget`**：`mode-list`、`volume` の名は `settings.ts` だけが知る。driver やアプリは新しい部品を足せない。部品の名の表が中央の表である。配置の絵（F14）で必ず増える。
  - **`page`**：文字列が窓の頁に写る。頁の並び、アイコン（`IconName` は閉じた和）、OS の頁か driver の頁かを決める欄が無い。カーネルの持ち主か否かで分けると、`time` の service は driver の側に出る。名前で分けると決め打ちになる。
  - **15 秒**：宣言へ動いただけで、持ち主の表に定数で書く。これは持ち主の方針なので問題は欄が無いこと（F40）だけである。
  - **`system`**：道の接頭辞の決め打ちである（F32）。
- **重さ**：作り直しになる。
- **直す案**：`device_class` を消し、実体の類は開いた語にする（F27）。類の表示名とアイコンは持ち主が状態で出す。宣言に `section`（`system`、`driver`、`app`）を足し、頁の並びは `section` の中で領域が出す `order` の数にする。アイコンは名前でなく領域の文書の画像にする。特別な部品の語は窓の部品の表で、知らない語は型の既定の部品で描くと規則に書く。

### F43 Mesa の値の持ち主がいない

- **どこ**：設計 §5「効かせるのは Mesa の present の道で、見張りで変えた瞬間に効かせる」、§1「値を効かせる者が持ち主である」、§4。`settings-ownership.md`（持ち主は user mode driver）。
- **筋書き**：効かせる者は各プロセスの中の Mesa である。領域の持ち主は `nvrm` である。§1 の定義に合わない。
  - `apply` は `nvrm` で走るが、そこに効き目が無い。`apply` は「受けた」と答えるだけで、Mesa が効かせたかは分からない。
  - `nvrm` が死ぬと、ゲームの CPU の道（`nocturne_gl.c`）に残る設定が読めない。
  - Mesa は窓を持たないプロセスの libc の中で見張りを持ち、待ち行列を別に要る。今の `h_system_watch` は窓の待ち行列に入れる（`js.c:990`）。
  - `present` の中で見張りを見に行くと、一コマごとの syscall になる。§1 に反する。
- **重さ**：作り直しになる。
- **直す案**：`nvidia/*` の 3D の値の持ち主は `nvrm` のまま、保存と宣言を受け持つ。効かせる者は読み手として写し（F1）を読む。読み手は一つの thread で見張りを待ち、変わった値を atomic の変数へ入れる。`present` はその変数だけを読む。Mesa の初期化が libc の `n_setting_cache(領域, 鍵, &var)` を呼ぶ。`§1` の定義の文を「値を効かせる者か、その家族が持ち主である」に書き直す。

### F44 「効かない項目を作れない形」は形でない

- **どこ**：設計 §1「登録が窓の項目になるので、効かない項目を作れない形にする」、§3.10。
- **筋書き**：何もしない `apply` を登録すると、窓に出る。`settingtest` の「当てて読む」は通る。形は何も止めない。

  `settingtest` は全部の領域の全部の鍵を当てる。`display/mode` は画面を変え、`power/*` は電源ボタンの動きを変え、`network/*` は通信を切り、`nvidia/*` は実機の表示を変える。利用者の文書に書く。「再起動で戻る」は一つの試験の実行で確かめられない。
- **重さ**：きれいさ。
- **直す案**：§1 の文を「効く道が通るまで登録しない」という規則に書き直し、形の主張を消す。`settingtest` は試験の領域（`test` の持ち主）で層の全部の道を試す。実物の領域へは、宣言の形の検査、範囲の外の拒否、読むことだけを当てる。効き目は領域ごとの試験（`systemwatchtest` の形）で試す。

### F45 持ち主の承認の記録が無い

- **どこ**：設計 §2「決めたこと」。`audit-settings-missing.md` §2.4（1〜4 は持ち主が決める）、AGENTS.md「カーネルを変えずに済まない案なら書く前にユーザーに聞く」、`docs/nocturne-native-abi.md` §12「全部の設定を並べた表 … はどこにも無い」、`docs/settings-ownership-20261027.md` 決定 1。
- **筋書き**：§2.4 は四つの問いを持ち主へ渡した。設計は同じ四つを「決定」と書く。読んだ文書に持ち主の答えは無い。特に、持ち主の問いは "why are userland PROGRAMs, forced to talk to the kernel to save settings?" である。層のあるプログラムの値は窓からカーネルを通る。カーネルの中に全部の領域の宣言の写しが一覧として並ぶ。これは決定 1 の「全部の設定を並べた表はどこにも作らない」の読み替えである。プログラムが層を使わずに動くことは守られるが、driver と service は読み替えの下にある。
- **重さ**：作り直しになる（答えが違えば仲立ちの置き場所が変わる）。
- **直す案**：書く前に次の四つの文を持ち主へ見せる。「仲立ちはカーネルの小さな部品である」「仲立ちは登録された領域の宣言と値の写しを持つ」「アプリの設定はカーネルを通らない。通るのは、窓に出すと選んだ領域だけである」「機械の文書は `C:\Nocturne\Settings` にある」。

## 9. 問い 9：普通の OS にあって、ここに無い能力

| 番号 | 能力 | 公式の出典 | 無いと何が起きるか | 重さ | 直す案 |
|---|---|---|---|---|---|
| G1 | 値を当てずに試す | `SDC_VALIDATE`（[O4]）、`CDS_TEST`（[O5]） | 窓は押してみるまで出せるか分からない | 作り直しになる | F14 の `N_SETTING_TEST` |
| G2 | 複数の変更を溜めて一度に当てる | `CDS_NORESET` の手順（[O5]） | F14 | 作り直しになる | F14 の一つの鍵 |
| G3 | 強制の層と、窓で固定して見せる | [O6]、[O7]、[O8] | F29 | 当たれば壊れる | F29 |
| G4 | 応答しない相手を待たない送り方 | [O2] | F11 | 作り直しになる | F11 |
| G5 | 設定が原因で起動しないときの回復 | 低解像度の起動と Safe Mode（[O10]） | 悪い値（拡大、配置、HDR）で起動の度に映らないか落ちる。窓に届かない。AGENTS.md は CLI の段を持たないので、利用者は戻せない | 当たれば壊れる | `boot/limine.conf` の起動の項目に、読み戻しをしない項目を足す。カーネルの `setting_start` は旗 `settings-defaults` で全部の読み戻しを止め、文書は残す。§1 の「引数で組まない」の例外で、起動の画面の項目から選ぶ。AGENTS.md に照らして持ち主へ先に見せる |
| G6 | 設定の頁を外から開く | `ms-settings:` の一覧（`settings-ownership.md` [W8]） | ブラウザーやゲームが「VRR の設定を開く」を作れない。`settings.ts:32` は頁の名を `args[0]` で受けるが、頁が宣言から作られると名の約束が無い | 当たれば壊れる | 窓の引数を `領域/鍵` に固定し、ABI §12 に書く。`SYS_GUI_LAUNCH` で開ける |
| G7 | 書き出しと引き継ぎ | Windows Backup の設定の目録（`audit-settings-missing.md` [M6]） | 機械を換えると全部の値を失う。身元の規則（F26）の「機械を換えても同じ」が使い道を持たない | きれいさ | 二段目に回す。文書の形を F21 で符号化するので、後で足せる |
| G8 | 一時の上書き | Apple の argument domain（`settings-ownership.md` [P1]） | `audit-settings-missing.md` §2.2 の「一時の上書き」は設計から消えた。`nosmp` などは窓から見えない。`dns-server=` は消える | きれいさ | 消す判断を §2 に一行書く。設計は起動の旗を層に入れない（`settings-ownership.md`）。診断の旗はこのまま残す |

## 10. 問い 10：L2〜L14 が載るか

| 層 | 載るか | 載らない理由 | 今変える部品 |
|---|---|---|---|
| L2 外観（264 値） | 半分載る | 色の型が無い（F24）。全部のアプリと `wm.c` が多くの token を読む。一つずつの `GET` は遅い。`js_ui.js` は一度読んで凍る。 | F24 の `COLOR`。F43 の `n_setting_cache`。token は一つの `RECORD` で配る。 |
| L3 画面の能力と配置 | 載らない | モニターの組の scope が無い。配置は多鍵の取引である。能力は設定でなく、番号つきの ABI が要る（層の外）。 | F14、F27。 |
| L4 GPU と表示エンジンの能力 | 載らない | §7 は「driver の能力の申告をこの設計で作る」と言うが、§5 の VRR の旗の一つしか書いていない。`n_screen_modes` は固定の構造体である（`abi.h:463-470`）。OS の領域が driver の申告を受ける形が無い。層の状態は領域の中の話で、`display` の行を `nvrm` の申告で出すには別の口が要る。 | §7 の文を直す。能力の問い合わせは `display_abi.h` の別の設計にする。`display` の状態は `screenmode.c` が申告から作る。 |
| L5 入力 | 半分載る | 複合の装置の身元（F26）と類（F42）で詰まる。 | F26、F42、F27。 |
| L6 時刻と地域 | 半分載る | `localtime` は呼ぶたびに読めない。機械の値の権限が `js` で通る（F32）。NTP は system service で別の設計である。文書が更新で消える（F10）。 | F43 の `n_setting_cache`。F10、F32。 |
| L7 音 | 半分載る | 出口の一覧は開いた `ENUM`（F15）である。プログラム×出口の scope が無い（F27）。実機の driver が無い。 | F15、F27、F26。 |
| L8 資源の上限 | 層の外 | 設定にしない。設計自身の上限（`READ_MOST`、輪、値）だけが規則に当たる（F13、F6）。 | F13。 |
| L9 装置と driver の結び付け | 載らない | 一つの領域に持ち主は一人である（F20）。 | F20。 |
| L10 アプリの登録 | 載らない | §3.9「プログラムを選ぶ」は一覧を要る。§7 は「層は登録を前提にしない」と言う。矛盾する。値のあるプログラムしか選べず、表示名とアイコンも無い。身元が道なので、フォルダーの名を変えると値を失う。 | F33 の不透明な身元。窓は一段目で「道を選ぶ」の部品（文書の選択）を使う。 |
| L11 電源と方針 | 半分載る | AC・DC は対象の軸（F27）である。`しない` の値（F24）。 | F24、F27。 |
| L12 利用者とセッション | 載らない | F17。 | F17、F27、F34。 |
| L13 網の口 | 半分載る | 起動の順（F5）。行の形（F21）。接続先（F27）。秘密（F36）。操作（F18）。`net.c:58` の `nif` が一つである。 | F5、F18、F21、F27、F36。 |
| L14 Web へ渡す経路 | 載る | ブラウザーは `appearance/*` を読み手として読む。カーネルの持ち主なので読みは止まらない。`Worker` の子の身元は問わない。 | F1 の読みの形だけが要る。 |

**今変える部品の一覧**（順に、後ろは前に依る）：

1. `sys/setting.c` の頼みの行列と値の写し（F1、F2、F11）。
2. scope の二つの軸と開いた実体（F27）。F31、F17、F20 はここに載る。
3. `common/setting.c` の符号、正規形、文法、型（F21〜F24）。
4. 要求の構造体の ABI（F3）。
5. 持ち主の契約の形（F7、F8、F18、F37、F38）。
6. 身元と書き手（F26、F32、F33、F34、F35）。
7. 表示を一つの鍵にする（F14）。
8. `layout.py`（F10、F32 の移動）。
9. 持ち主へ見せる四つの文（F45）。F41 の表も見せる。

## 11. 調べて壊せなかった所

- **一コマの費用**：`wm.c` の `compositor` と `desktop_thread` を読み、`ws->vrr`・`ws->period_ns` を自分の変数で読む形なので、合成の道に層は入らない。例外は F4 の錠の下の呼出しと F43 の `present` である。
- **変化の通知の文脈**：`watched_changed` は眠らず（`sys/watch.h:7-11`）、`wm_lock` の下でも `desktop_settings_put`（`wm.c:1176-1184`）と同じ形で呼べる。輪の錠を葉の spinlock にすれば `changed` は置ける。
- **`wm_lock` の二度取り**：`mutex_lock` は同じ thread の再入を数える（`sys/wait.c:416-418`）ので、`wm.c` の `apply` が `wm_lock` を取っても自分では止まらない。止まるのは F4 の別の筋である。
- **保存の原子性**：`store_save` は REPLACE と `N_COMMIT_WAIT` を使う（`store.c:924-932`）。`.bad` への退きは `store_entry_move` で作れる。
- **持ち主の死の検出**：`object_ops.close` はプロセスの終わりの handle の閉じで呼ばれる（`object.c:59-73`、`channel.c:69-91` の listener が同じ形）。`SYS_SETTING_OWN` の handle の寿命は作れる。
- **画面が映らず確定が押せない**：カーネルの持ち主では、期限は窓に依らず、`trial_watch`（`screenmode.c:205-234`）の形で戻る。窓が死んでも戻る。壊れるのは F37、F38、F39 の筋だけである。
- **Terminal からの起動**：`sys_app_launch` は呼ぶ者の身元を継がず、道から作るので、Terminal の子は Terminal の権限を持たない。壊れるのは F32 の道の信頼だけである。
- **`system` 属性の書換え**：アプリから属性を変える口は無い（`ns_record_amend` の呼び手はカーネルの `folder_at` と host）。F32 の属性は偽れない。
- **host の道具**：`host/store/store.c` は媒体を直接書く。走っている系からは呼べない。機械を手にしている者の話なので穴に数えない。
- **移植の Firefox**：§3.3 の「揃える」（F33）を消せば、層を呼ばずに `n_app_data_folder` で動く。カーネルの設定の口を一つも通らない。
- **共有の文書**：領域ごとに文書が一つなので、全プログラムが共有する文書は無い。
- **shim**：設計の部品に、ほかの部品の名をかたる代役や偽の関数は無い。`n_setting_serve` の一段目の呼び手は `nvrm` 一つだが、§4 の種類の表に driver と service の二つの使い道があり、一つの呼ぶ側のための包みに当たらない。
- **POSIX の形**：呼出しに fd、パスの open、`ioctl` の形は無い。`領域/鍵` の文字列の get と set は UNIX の sysctl に字面が似る。持ち主の契約、宣言、試行があり、写しでない。
- **休止**：`hibernate_restore` は `store_start` の前で（`init.c:50-51`）、戻る時は RAM の値がそのまま戻る。読み戻しと衝突しない。
- **起動の値の改名**：`n_launch.settings` は `n_launch.values` に換えられる。層と独立である。

## 12. 限界

- 静的な読みだけである。ビルド、QEMU、実機を動かしていない。
- `ports/mesa` の `present` の道は読んでいない。F43 の「一コマごとの syscall」は設計の文から言っている。
- `kernel/src/fs/document.c` は読んでいない。`.bad` の退きと `STORE_FIXED` の実装の細部はそこにも触れる。
- Apple の `objectIsForced` は Apple の頁の説明で読み、構成プロファイルとの結び付きは読めなかった。
- Windows の Safe Mode は利用者向けの案内で読み、低解像度の項の細部は検索の抜粋で読んだ。
- NVIDIA と AMD の control panel の頁はこの監査で開き直していない。材料の `settings-ownership.md` の読みを使った。

## 13. 出典

- [O1] WM_SETTINGCHANGE：https://learn.microsoft.com/en-us/windows/win32/winmsg/wm-settingchange
- [O2] SendMessageTimeoutA：https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendmessagetimeouta
- [O3] Audio Sessions：https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-sessions
- [O4] SetDisplayConfig：https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig
- [O5] ChangeDisplaySettingsExW：https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexw
- [O6] Make a WinUI LOB app policy-aware：https://learn.microsoft.com/en-us/windows/apps/get-started/line-of-business/policy-aware-settings
- [O7] Group Policy preferences in Windows：https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/group-policy/group-policy-preferences
- [O8] objectIsForced(forKey:)：https://developer.apple.com/documentation/foundation/nsuserdefaults/1408635-objectisforcedforkey?language=objc
- [O9] OS Variable Refresh Rate：https://devblogs.microsoft.com/directx/os-variable-refresh-rate/
- [O10] Windows startup settings：https://support.microsoft.com/en-us/windows/experience/startup-boot/windows-startup-settings

[W8]、[W11]、[W12]、[W20]、[N5]、[P1] は `settings-ownership.md` の出典の節、[M6] は `audit-settings-missing.md` の §10 にある。
