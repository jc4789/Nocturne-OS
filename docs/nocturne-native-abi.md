# Nocturne ネイティブABI（UNIX型ABIの置き換え）

Nocturneは独自OSであり、UNIX／POSIX／Linuxではない。現行の `common/abi.h` は、
整数fdへの汎用 `read`／`write`／`lseek`／`fcntl`／`dup2`／`poll`、`open` フラグ、
`chdir`／`getcwd`、`spawn`＋`waitpid`／`kill`、`pipe`、`sbrk`／`mmap`、
`ENOENT` 型のエラー名という、UNIXの骨格を持っている。名前を変えても骨格が同じなら
UNIXのままなので、この文書は**骨格そのもの**をNocturne独自の方式へ置き換える仕様を定める。

ISO Cの標準ライブラリ（`stdio.h`、`stdlib.h`、`string.h`、`math.h`、`time.h`、
`threads.h`、`fenv.h`、`setjmp.h`、`errno.h` の `EDOM`／`ERANGE`／`EILSEQ`）は
C言語の一部として、この新ABIの上に実装して残す。POSIXの拡張（`strings.h`、
`fcntl.h`、`unistd.h`、`sys/*.h`、`alloca.h`、`*_r`、`asprintf` 等）は提供しない。
移植物（ffmpeg、QuickJS、Mesa、tcc、lexbor、FreeType、wabt、BearSSL、libc++）は
互換ヘッダーを置かず、各ソースを直接このABIへ書き換える。

## 1. ハンドル

- カーネル資源はすべて**種類つきハンドル**（`n_handle`、非負の `int32_t`）で表す。
- 種類：`N_KIND_DOCUMENT`、`N_KIND_FOLDER`、`N_KIND_CHANNEL`、`N_KIND_WINDOW`、
  `N_KIND_SHARED`、`N_KIND_APP`、`N_KIND_EVENTS`、`N_KIND_DEVICE`（音声等）、
  `N_KIND_CONNECTION`（TCP／UDP）。
- 汎用操作は `n_handle_close(h)` と `n_handle_kind(h)` だけ。読み書きは種類ごとの
  専用関数で行い、種類の違うハンドルを渡すと `N_ERR_WRONG_KIND`。
- 複製（`dup`／`dup2`）と、番号を指定した付け替えはない。子アプリへ渡す資源は
  起動時の引渡し表（§5）で明示する。

## 2. 結果コード

- 失敗は `errno` ではなく負の `n_result` を直接返す。
  `N_ERR_NOT_FOUND`、`N_ERR_EXISTS`、`N_ERR_DENIED`、`N_ERR_NO_MEMORY`、
  `N_ERR_INVALID`、`N_ERR_WRONG_KIND`、`N_ERR_BUSY`、`N_ERR_NOT_READY`、
  `N_ERR_CLOSED`、`N_ERR_TIMEOUT`、`N_ERR_CANCELLED`、`N_ERR_TOO_LARGE`、
  `N_ERR_NOT_EMPTY`、`N_ERR_IO`、`N_ERR_UNSUPPORTED`。
- `n_result_text(r)` が表示用の文を返す。ISO Cの関数が失敗理由を残す必要がある場合、
  libcは `errno` に `n_result` の正の値を入れる（名前は `N_ERR_*` のみ）。
- `nocturne.h`／`gpu.h`／`display.h` のlibc関数も同じで、`errno` は触らない：
  ネットワーク（`net_*`、`udp_*`、`tcp_*`。ソケットは `n_handle`）、窓と画面
  （`win_*`、`screen_*`、`clipboard_*`、`gui_launch`）、GPU（`gpu_*`、`gfx_render3d*`）、
  `proclist`、`sysinfo`、`thread_stack_bounds`、`poweroff`／`reboot`。何かを作って渡す関数は
  出力引数に置いて `n_result` を返す（`win_open(w, h, title, flags, &win)`、
  `n_memory_reserve`。失敗では出力は NULL）。

## 3. 文書とフォルダー（`open`／`lseek`／`readdir`／`chdir` の置き換え）

- ファイルシステムは一つ、ドライブも `C:` の一つだけ（ボリュームの形式は
  `common/store_format.h`、実装は `kernel/src/fs`。二台目のドライブや装置ごとの別の
  ファイルシステムは無い）。パスはドライブからの完全指定：`C:\Users\User\a.txt`。
  区切りは `\`、`/` も同じに扱う。ドライブを省いた `\a`、`/a` も `C:\a`。ほかのドライブ文字は
  `N_ERR_NOT_FOUND`。`.` と `..` の要素はシステムが名前の上で解決する。
- 名前は 1〜255 バイトの UTF-8。制御文字、`\ / : * ? " < > |`、末尾の空白とピリオドは
  使えない（`N_ERR_INVALID`。長すぎれば `N_ERR_NAME_TOO_LONG`）。名前は大文字・小文字を
  区別せずに比べ（Unicode の単純な畳み込み。表はボリュームが持つので、どのカーネルでも同じ）、
  付けられた形のまま保存・表示する。`n_entry_move` で同じ項目の大文字・小文字だけを変えられる。
- 作業ディレクトリはない。カーネルは完全なパスだけを受け付け、`C:` だけ、`C:a`、`a` は
  `N_ERR_INVALID`。シェルは自分の「現在のフォルダー」を持ち、起動するアプリへ設定
  `FOLDER=` として渡す（§5）。
- libc のパスの関数（`nocturne.h`）：
  - `n_path_is_complete(path)`：ドライブの後（無ければ先頭）に区切りがあるか。`C:\…`、`\…`、
    `/…` は完全。`C:` と `C:a` は現在のフォルダーとその中の `a`。
  - `n_path_join(folder, name, out, cap)`：`name` を完全なフォルダー `folder`（完全でなければ
    `C:\`）の中で取る。完全な `name` はそれだけで決まる。結果は常に `C:\a\b` の形
    （大文字のドライブ、`\` 区切り、`.`／`..` 解決済み）。長さを返し、収まらなければ
    `N_ERR_TOO_LARGE`。
  - 利用者が打った名前は `n_path_complete(name, out, cap)` で完全指定にする：設定 `FOLDER`
    （無いか完全でなければ `C:\`）の中で `n_path_join` する。ISO Cの `fopen`／`remove`／
    `rename` もこれを使う。
  - `n_path_name(path)`：最後の名前（根では `""`）。
  - `n_path_from_url(url, out, cap)`／`n_path_to_url(path, out, cap)`：`file:` URL と
    パスの変換（`file:///C:/Users/User/a%20b.html` ⇔ `C:\Users\User\a b.html`。ドライブの
    無い `file:///Users/User` も C:）。
- アプリが自分の現在のフォルダーを変えるのは `n_folder_set(name)`：名前を完全指定にし、
  フォルダーであることを確かめてから（違えば `N_ERR_NOT_FOLDER`）、自分の設定 `FOLDER` を
  置き換える（以後の `n_path_complete` と、libcが起動するアプリへ渡す `FOLDER` に効く）。
- C: の構成（`common/abi.h` の `N_FOLDER_*`）：
  - `C:\Nocturne`：システム。属性 SYSTEM の項目は、アプリは読めるが、それ自体も中身も
    変えられない（作成・削除・名前変更・書込みは `N_ERR_DENIED`）。`System`（システムの
    プログラム、`N_FOLDER_SYSTEM`）、`Fonts`、`Licenses`、`SDK`、`Certificates`、`Web`、
    `Agent`、`Firmware` 等。`C:\Nocturne\Temp` はカーネルだけが使う（差し替え中の内容）。
  - `C:\Programs`：アプリ（`N_FOLDER_PROGRAMS`）。アプリごとに一つのフォルダー。利用者は
    アプリを入れられる。システムと一緒に入るアプリのフォルダーは SYSTEM。
  - `C:\Users\<名前>`：利用者のフォルダー（設定 `USERFOLDER`、`n_user_folder()`）。アプリは
    ここで始まる。`Desktop`、`Documents`、`Downloads`、`Music`、`Pictures`、`Videos`、
    `AppData`。アプリのデータは `n_app_data_folder(name, out, cap)` が作って返す
    `AppData\<name>`。`AppData\Temp` は一時フォルダー（設定 `TEMP`、`n_temp_folder()`）で、
    起動のたびに空になる。
  - UNIX の `/bin`、`/etc`、`/usr`、`/home`、`/tmp`、`/data` と、探索パス `PATH`、`HOME` は無い。
- 丸ごとの操作（通常の用途）：
  - `n_document_load(path, void **data, size_t *size)`：内容を新しい `n_memory` ページで
    受け取る。`n_memory_release(*data, *size)` で返す。
  - `n_document_save(path, data, size)`：新しい内容を別に用意してから原子的に差し替える
    （ボリュームは書込み時複写なので、新しい内容のブロックを書いてから目録を切り替える）。
    書きかけの文書が見えることはない。
  - `n_document_append(path, data, size)`：ログ用の追記。無ければ作る。追記の間は
    文書の排他を持つので、同時に追記しても互いを上書きしない。
- 部分アクセス（大きい媒体、DB）：
  - `n_document_open(path, n_document_access)`。`access` はビットフラグではなく列挙：
    `N_DOCUMENT_READ`、`N_DOCUMENT_EDIT`（既存の読み書き）、
    `N_DOCUMENT_CREATE`（新規作成。既に有れば `N_ERR_EXISTS`）、
    `N_DOCUMENT_REPLACE`（空の新しい内容をメモリー上に作る）。差し替えは保存
    （`n_document_commit` か、`n_handle_close` で明示的に閉じた時）だけで行う。閉じずに
    終了したアプリの内容は捨てられ、元の文書はそのまま残る。
  - 保存：`n_document_commit(h, how)`。`N_COMMIT_WAIT` は媒体に届くまで待つ。
    `N_COMMIT_START` はすぐ戻り、届いたら（失敗も）イベント待ち行列で監視している
    ハンドルに `N_EVENT_SAVED` が来る。`N_COMMIT_RESULT` は最後に始めた保存の結果
    （まだなら `N_ERR_NOT_READY`）。結果を読むと `N_EVENT_SAVED` は報告されなくなる。
    書いていない書込み用ハンドルの保存は、文書のそれまでの記録（作成、名前変更、別の
    ハンドルの保存）が届くのを待つ。`READ` のハンドルの保存は何もせず待たない。
    `n_handle_close` は `N_COMMIT_WAIT` と同じ保存をしてから閉じる。`READ` のハンドルは
    開いた時点で最後に保存された版を読み続ける。媒体への書込みは一つの確定（commit）単位で
    原子的で、電源が切れても最後に確定した状態が残る（`common/store_format.h`）。
  - `n_document_read_at(h, offset, buf, n)`、`n_document_write_at(h, offset, buf, n)`、
    `n_document_size(h)`、`n_document_set_size(h, size)`。**共有カーソル（シーク位置）は無い**。
    `N_DOCUMENT_READ` のハンドルへの書込みは `N_ERR_DENIED`。
  - 排他：`n_document_claim(h, wait)`／`n_document_release(h)`（現行のリースを継承）。
    待たない場合に他が持っていれば `N_ERR_NOT_READY`。
- 情報と操作：`n_entry_info(path, struct n_entry *)`、`n_folder_create(path)`、
  `n_entry_remove(path)`（空でないフォルダーは `N_ERR_NOT_EMPTY`）、
  `n_entry_move(from, to)`、`n_folder_list(path, struct n_entry *out, max, start)`（件数を返す）。
- `struct n_entry` は名前（保存された形）、`kind`、`size`、更新時刻（`modified` の壁時計の秒と
  その秒の中のナノ秒 `modified_ns`）、`id` を持つ。`id` は C: 上の項目そのものの番号で、
  `n_entry_move` で移しても変わらず、別の項目には決して使われない。リンクは無いので、
  綴り（大文字・小文字、`/` と `\`）の違う二つのパスが同じ項目かは `id` で分かる。
- `n_document_info(h, struct n_entry *)` は開いている文書の `struct n_entry` を返す。名前は空
  （ハンドルは名前でなく文書そのものを指し、開いた後に移されても同じ文書のまま）で、`size` は
  そのハンドルが読む長さ（`n_document_size` と同じ）。
- `n_entry_move` は名前の付け替えで、内容は動かない。既存の名前を決して上書きしない
  （`N_ERR_EXISTS`。同じ項目の大文字・小文字の変更は除く）。フォルダーを自分の中へは
  移せない（`N_ERR_INVALID`）。
- `n_entry_replace(from, to)` は `n_entry_move` と同じだが、文書 `from` は既存の文書 `to` の名前を
  取れる。元の `to` は同じ確定（commit）の中で `n_entry_remove` と同じように消える（開いている
  ハンドルは消えた文書を読み書きし続ける）。`to` が文書以外なら `N_ERR_EXISTS`。
- ISO Cの `stdio` は文書ハンドルの上で自分の位置を持つ：`"r"`＝`READ`、`"r+"`＝`EDIT`、
  `"w"`＝既存なら `EDIT` で長さ0にし、無ければ `CREATE`（ISO Cの切り詰め）、`"wx"`＝`CREATE`、
  `"a"`＝`EDIT`／`CREATE` で、書出しのたびに排他を取って末尾へ書く。`fseek`／`ftell` は
  文書だけ（チャンネルは `N_ERR_WRONG_KIND`）。`remove`＝`n_entry_remove`。`rename`＝
  `n_entry_replace`（既存の文書は一つの確定で置き換わる）。
  `tmpfile` は一時フォルダー（`n_temp_folder()`）に作り、閉じるか終了で消す。標準の出力・
  記録が文書なら末尾へ追記する。終了（`exit`、`main` からの戻り）はISO Cのとおり全ストリームを
  閉じたのと同じで、文書のストリームは書き出してから保存する（`N_COMMIT_START`：次に開く
  アプリはその内容を読み、終了は媒体を待たない）。

## 4. チャンネル（`pipe`／標準入出力fd の置き換え）

- チャンネルは**メッセージ単位**の双方向路。一回の送信が一つのメッセージになり、
  受信側は境界を保ったまま受け取る。大きな塊は共有ページを添付して送る。
- `n_channel_pair(n_handle out[2])`：無名の対（現行 `pipe` の置き換え）。
- 名前付きの合流点：`n_channel_listen(key)` が鍵を（ハンドルが閉じるまで）占有し、
  `n_channel_connect(key)` はメッセージチャンネルの片端を返す。もう一端は合流点で
  待ち（`N_EVENT_RECEIVABLE`）、`n_channel_accept(listener)` で受け取る（無ければ
  `N_ERR_NOT_READY`）。合流点が閉じると、待っている接続も閉じる。
- `n_channel_send(h, data, n, attach)`、`n_channel_receive(h, buf, cap, flags, &attached)`。
  - 流量制御：受け手が未だ受け取っていない量が1MiBに達している間、送信は待つ
    （`N_EVENT_SENDABLE` で送れることを知れる）。空の待ち行列へは大きさに関係なく送れる。
  - `N_CHANNEL_WAIT` で待つ。待たない場合は `N_ERR_NOT_READY`。相手が閉じて
    残りも無ければ `N_ERR_CLOSED`。
  - `N_CHANNEL_PEEK` は次のメッセージの大きさだけを返し、取り出さない。
    `cap` より大きいメッセージは `N_ERR_TOO_LARGE` で、待ち行列に残る。長さ0の
    メッセージも一つのメッセージとして届く。
  - `N_CHANNEL_DISCARD` は次のメッセージの先頭 `cap` バイトだけを写してから捨て（添付も閉じる）、その大きさを
    返す。受け取る場所を確保できない大きなメッセージを断るため。`PEEK` とは併用できず、
    未知のフラグと共に `N_ERR_INVALID`。
  - `attach` は文書または共有ページのハンドル（無ければ -1）。メッセージと一緒に運ばれ、
    受信側で新しいハンドルとして `attached` に入る（無ければ -1）。チャンネル自身を
    運ぶと参照の循環になるので `N_ERR_WRONG_KIND`。
- アプリの入出力に渡される文字装置（コンソール、カーネルログ、null）にも
  `n_channel_send`／`receive` が使える。ただし境界と添付は無く、`N_CHANNEL_PEEK` は
  `N_ERR_UNSUPPORTED`。バイト列のパイプは無い。
- `n_channel_receive_from(h,buf,cap,flags,&message)` はメッセージチャンネル専用。
  `n_channel_message` の `attached`、`reserved`（0）、`sender_app` を返す。
  `sender_app` はカーネルが実送信時に記録する `n_app_self()` と同じ識別子で、
  endpoint の作成者・接続者の固定値でも、本文で申告された番号でもない。
  同じアプリの全 thread と別接続で一致し、endpoint を起動先へ引き渡すと起動先の主体になる。
  識別子は再利用せず、送信者終了後の待ちメッセージにも値を保持する。
  `message` は必須。`PEEK` は主体を調べられるが添付を取り出さず、`DISCARD` は添付を閉じる。
  両者の `attached` は -1。旧 receive の番号と形式は変えない。
  添付の設置失敗や出力先の失効は負の結果を返し、設置済み添付も回収する。
  この段階の失敗は既に取り出したメッセージを待ち行列へ戻さない。失敗時の出力値は利用しない。
  接続単位の認証を必要とするサービスは、最初の主体へ bind し各 request の主体も照合する。
- ISO Cの入力は、`cap` より大きいメッセージを `N_CHANNEL_PEEK` で測り、バッファーを
  広げて丸ごと受け取る。長さ0のメッセージは読む内容が無いので次を待つ。
- 端末は、子アプリの「入力」「出力」チャンネルとメッセージをやり取りする一つの
  アプリにすぎない。ハンドルから端末かどうかを調べる手段（`isatty`）は無く、起動元が
  設定 `TERMINAL` に端末へつながるハンドル番号を並べて伝える（例 `TERMINAL=0 1 2`）。
  `n_terminal(h)` がこれを読む。端末はキー入力をそのまま送るので、行の編集と表示は
  アプリ側（libcの `readline`）が行う。ISO Cの `stdout`／`stderr`／
  `stdin` は、起動時に渡された出力／入力チャンネル（無ければ記録先のログ）に結び付く。
  アプリが例外（ページフォルト等）で止められると、カーネルはその報告文を一つの
  メッセージとして記録先のログ（ハンドル2）へ送る。待ち行列が満杯なら待たずに捨てる。
- Ctrl+C：端末はシェルにハンドル3として「割込み」チャンネルを渡し（設定
  `INTERRUPTS=3`）、Ctrl+C ごとにメッセージを一つ送る。シェルは前面のジョブを
  `n_app_stop(h, N_APP_STOP_TREE)` で止め、プロンプトでは入力中の行を取り消す。
  シグナルは無い。
- システムログ：`n_system_log_open()` は、送ったメッセージがシステムログ（とシリアル
  ポート、デスクトップが無いときのテキストコンソール）へ加わるハンドルを返す。`n_system_log(buf, cap)` は最新のログを読む。

## 5. アプリ（`spawn`／`waitpid`／`kill`／`getpid` の置き換え）

- `n_app_launch(path, const struct n_launch *)` がアプリハンドルを返す。
  `n_launch` は引数列、設定の組（ISO Cの `getenv` が読む。`environ` は無い）、
  引き渡すハンドルの表（入力・出力・ログのチャンネル、共有ページ等）を持つ。
  - 設定は `"名前=値"` の形（`=` が無ければ `N_ERR_INVALID`）。起動されたアプリでは
    `n_settings`（NULL終端）として読め、`getenv` もこれを引く。
  - 引数と設定の文字列（各 nul を含む。カーネルが加える設定も数える）と、二つの NULL 終端
    ポインター配列（1 個 8 バイト）は、合わせて `N_LAUNCH_MAX_BYTES`（4 MiB）まで。
    超えると `N_ERR_TOO_MANY_ARGUMENTS`。
  - 引き渡すハンドルは、表の順に子アプリの 0, 1, 2, … 番になる（-1 は空き）。
  - `path` は完全指定（§3）。名前だけのプログラムは libc の `find_program(name, out, n)` が
    `N_FOLDER_SYSTEM`、次に `N_FOLDER_PROGRAMS` の各アプリのフォルダーで探す（`PATH` は無い）。
    それ以外の名前は `n_path_complete` で完全指定にする。
  - 起動されたアプリは作業フォルダーを持たない。利用者の名前を受け取るアプリのために、
    起動元は自分の現在のフォルダーを設定 `FOLDER=C:\…` で渡す（§3の `n_path_complete`）。
    カーネルは、起動元が渡さなければ `USERFOLDER` と `TEMP` を加える。
- 終了はアプリハンドルのイベント（§6）で届く。`n_app_result(h, timeout_ms, int *status)` で
  直接待つこともできる（-1 は無期限、0 は待たずに `N_ERR_NOT_READY`、期限切れは
  `N_ERR_TIMEOUT`）。ゾンビの回収は無く、最後のハンドルを閉じれば記録は消える。
- `n_app_stop(h, N_APP_STOP_TREE か 0)`：停止要求。シグナルは無い。
- スレッド：`thread_create(fn, arg)` は 256 KiB、`thread_create_sized(fn, arg, bytes)` は
  少なくとも `bytes` のスタック（ページは初めて触れたときに割り当てる）で `fn` を動かし、
  0 にならない識別子を返す。終わったスレッドは `thread_join` か `thread_detach` まで残る。
  `thread_detach` 後は終わり次第カーネルが解放し、join はできない（`N_ERR_INVALID`）。
  アプリの終了は全スレッドを止めて回収する。
- `n_app_self()`、`n_app_parent()`：アプリ識別子（共有ページの受渡しの許可判定用）。
- `n_app_open(id)`：アプリ一覧の識別子から、動作中のアプリのハンドルを得る（タスク
  マネージャーや `kill` が `n_app_stop` するため）。`n_app_id(h)`：ハンドルの識別子
  （終了後も同じ。識別子は再利用しない）。
- `n_app_list`、`n_system_info` 等の状態取得は現行の `proclist`（件数）／`sysinfo`（0）を
  引き継ぐ。どちらも失敗では負の `n_result`。

## 6. イベント待ち行列（`poll`／`O_NONBLOCK`／`fcntl` の置き換え）

- `n_events_create()` が待ち行列を作る。各スレッドは任意の数を持てる。
- `n_events_watch(q, h, interest, token)`：ハンドルの関心事を登録。
  `N_EVENT_RECEIVABLE`（メッセージ、窓の入力、接続要求、受信データ）、`N_EVENT_SENDABLE`、
  `N_EVENT_CLOSED`、`N_EVENT_APP_ENDED`、`N_EVENT_SAVED`（`N_COMMIT_START` で始めた保存の
  結果が出た文書）。同じハンドルと同じ token の再登録は関心事の変更。
  待ち行列を待ち行列に入れることはできない（`N_ERR_WRONG_KIND`）。
- 報告は状態に基づく（レベル）。準備ができたままのハンドルは毎回の待ちで報告される。
- 監視はハンドルを保持しない。ハンドルが最後に閉じられると監視も消える。
- 現行のアドレス監視はこの待ち行列へ統合した：`n_events_watch_address(q, address,
  expected, token)`。値が既に違えば `N_ERR_NOT_READY`。通知されると `N_EVENT_ADDRESS`。
- タイマー：`n_events_timer(q, ms, repeat, token)`。`N_EVENT_TIMER` で届く。繰返しの
  タイマーは遅れた分をまとめて報告せず、周期を保つ。
- `n_events_unwatch(q, token)`：その token の監視・タイマー・アドレス監視を外す。
  0＝無かった、1＝外した、2＝アドレスの通知が既に届いていた（取り消した）。
- `n_events_wait(q, struct n_event *out, max, timeout_ms)`：届いた分を一括で返す
  （一回に最大64件。残りは次の待ちで返る）。-1 は無期限、0 は待たない。

## 7. メモリ（`sbrk`／`mmap`／`mprotect` の置き換え）

- `n_memory_reserve(bytes, n_memory_access, &memory)`：ゼロ初期化の領域を確保し、
  `memory` に置く（失敗では NULL と負の `n_result`）。大きさの独自上限は無い：
  空きメモリや予約領域を超えれば `N_ERR_NO_MEMORY`、0 やページ丸めで桁あふれする
  大きさは `N_ERR_INVALID`。
- `n_memory_release(addr, bytes)`、`n_memory_protect(addr, bytes, access)`：0 か負の `n_result`。
- `access` は列挙：`N_MEMORY_READ`、`N_MEMORY_READ_WRITE`、`N_MEMORY_READ_EXECUTE`。
  書込みと実行の同時許可は無い（W^X）。
- 確保時だけ `access` に `N_MEMORY_LOW` を足せる：2GiB未満の低位領域（1GiB〜2GiB、
  プログラム像がそれより大きければ像の直後から）に置く。命令に符号拡張32ビットの
  絶対アドレスを持つ生成コード（`tcc -run` が結合する非PICのlibc等）のため。
  低位領域は通常領域と別の空き一覧・境界を持ち、解放・保護は同じ関数で行う。
- 共有ページ：`n_shared_create(bytes)` は確保済み、`n_shared_reserve(bytes)` は最初に
  触れたページから割り当てる共有オブジェクトのハンドルを返す。`n_shared_map(h, bytes,
  N_MEMORY_READ | N_MEMORY_READ_WRITE, &mapping)` は自分が持つハンドルだけを写す
  （失敗では `mapping` が NULL で、負の `n_result`）。
  他のアプリへはチャンネルの添付で渡す。他アプリのハンドル番号を指す手段や、番号札で
  受け渡す仕組みは無い。ブラウザーの SharedArrayBuffer／共有WebAssemblyメモリは、本文の
  前に添付だけを運ぶ搬送メッセージ（一つに一個）を送って Worker へ渡す。
- `malloc` は `n_memory_reserve` の上に作る：
  ヒープは領域の並び（各領域は先頭ブロックと終端標識を持ち、領域をまたいで結合しない）で、
  領域の大きさはヒープの1/16（256KiB〜8MiB）。最新の領域だけ末尾の空きを一つ分温存し、
  それ以外の領域の末尾の空きはページ単位で返し、全部空いた古い領域は丸ごと返す。
  1MiB以上のブロックは専用の領域で、解放すれば即座に返る。
- アドレス待ち：`wait_on_address(addr, expected, ms)`／`wait_on_address64` は起こされると0、
  語が `expected` でなければ `-N_ERR_NOT_READY`、期限で `-N_ERR_TIMEOUT`、アプリ停止中は
  `-N_ERR_STOPPED` を直接返す（`errno` は触らない）。`wake_address(addr, count)` は起こした数
  （`count == 0` は全員）。`ms == UINT32_MAX` は無期限。共有ページ上でもアプリをまたいで働く。

## 8. 時間

- `n_clock_uptime_ms()`、`n_clock_wall()`（壁時計の秒）、`n_sleep_ms(ms)`（結果は無い。
  旧 `msleep` は削除した。ISO Cの `thrd_sleep` はこれの上に作る）。
- `n_random(buf, n)`：システムのエントロピーから `n` バイト（`/dev/urandom` の置き換え）。
- `gettimeofday`、`struct timeval` は無い。ISO Cの `time`／`timespec_get`／`localtime` は
  これらの上に実装する。

## 9. 装置

- 音声、PCI一覧、カーネルログ等の `/dev` ファイルは、種類つきハンドルと専用の関数へ移す。
  ファイルとして読み書きしない。
- 音声：`n_audio_open()` が混合器の上の自分専用の流れ（`N_KIND_DEVICE`）を返す。全部
  使用中なら `N_ERR_BUSY`。形式は `N_AUDIO_RATE`（48kHz）のステレオ、1フレーム＝int16_t
  二つ（左が先）。
  - `n_audio_submit(a, frames, count, flags)`：入るだけ受け取り、受け取ったフレーム数を返す。
    流れは約170ms分を持つ。`N_AUDIO_WAIT` なら空きを待ち、無ければ満杯で
    `N_ERR_NOT_READY`（空きは `N_EVENT_SENDABLE` で分かる）。
  - `n_audio_status(a, struct n_audio_status *)`：待っているフレーム、装置に渡したフレーム、
    再生済み（推定を含む。`flags` が時計の出所）、世代。
  - `n_audio_flush(a)`：まだ混ぜていない音を捨て、世代を進める。
  - `n_handle_close` は残りを鳴らし終えてから閉じる（停止中のアプリは待たない）。
  - 主音量：`n_audio_volume()`、`n_audio_set_volume(0〜100)`（消音も解く）。
- システムログ：`n_system_log(buf, cap)` で読み、`n_system_log_open()` の
  ハンドルへ `n_channel_send` で書く。
- アプリの中の装置ドライバー（例：NVIDIA の資源管理 `C:\Nocturne\System\nvrm`、`docs/nvidia/LEDGER.md`）：
  - `n_pci_claim(bus, dev, func)` は PCI 機能を動かす権利（`N_KIND_PCI`）を返す。呼べるのは
    init だけで、init は起動するドライバーアプリへハンドルを渡す。カーネルのドライバーが使う
    機能と、既に権利のある機能は `N_ERR_BUSY`。起動オプション `noappdrivers` で全部断る。最後のハンドルが閉じると、カーネルは
    (1) その権利の BAR オブジェクトの写像を全アプリから外し（その範囲は予約のまま、触れると
    全 1 が読める読取り専用のページ。バスから消えた装置と同じ）、(2) 割り込みとバスマスターを
    止め、(3) IOMMUの遮断を確認する。終了処理内で装置の回復を待たず、FLRもしない。
    IOMMUなしでは、clean closeでも汎用PCI読出しだけでDMA停止を保証できないため残存DMAページを保全する。
    PCIeのTransactions Pendingはnon-posted要求だけであり、全TC・RO・IDOのposted DMAの終了を
    一般に証明しない（PCIe 6.0 §6.4、§7.5.3.5）。
    終わったと確かめられない権利の DMA 用メモリは、解放せずに隔離する。アプリの終了時は
    権利のハンドルを他のハンドルとマッピングより先に閉じる。BAR オブジェクトのビュー・写像・
    DMA アドレスの照会は、権利が終わった後は `N_ERR_CLOSED`。
    異常終了した装置の再claimには隔離が必要。非画面機能は次claimでFLRを試し、未準備・取消なら
    保存状態を装置に保持し、次回に復元を再開する。完了までハンドルを返さない。
    起動画面GPUはgeneric FLRをせず、`N_PCI_NEEDS_RECOVERY`でnative driverへ回復責務を渡す。
    フレームバッファー描画の再開はscanoutの復旧保証ではなく、FLR成功も旧DMAのdrain証明ではない。
  - `n_pci_info` は ID、BAR、MSI／MSI-X の数、上流のブリッジ、起動時の画面がその機能の BAR に
    あるならその位置（ドライバーはそこを壊さない）を返す。
  - 構成空間：読みはホストから自分までの経路だけ。自分と同じ装置の他の機能、上流の各ブリッジと
    その装置の他の機能（ルートポートが機能 1 以降で、機能 0 がホストブリッジの機械がある。
    ドライバーは機能 0 が答えない装置を飛ばす）、ホストブリッジ（チップセットの識別）。無関係な装置は
    `N_ERR_DENIED`。読み先がFLRの途中（別のclaimのFLR、または終了したclaimが残し、後のclaimが
    仕上げるFLR）なら、FLR中の機能は要求を捨ててよい（PCIe 6.0 §6.6.2）ので読まずに
    `N_ERR_NOT_READY`。書きは自分だけで、BAR、
    拡張 ROM、MSI／MSI-X の機能はカーネルだけが書く（`N_ERR_DENIED`）。例外は、リセットで消えた
    BAR へ、カーネルが起動時に見た値をそのまま 32 ビットで書き戻すことだけ（戻せるが、動かせない）。
    一部のビットだけがカーネルのものであるレジスターは、そのビットを変える書込みを `N_ERR_DENIED`
    にする（読んで一部を変えて書き戻すなら通る）：Command の MSE・BME・パリティ応答以外（I/O、
    INTx 無効化など）、BIST の開始、Interrupt Line、PMCSR の電源状態（D0 へ戻すことは可）、
    PCIe Device Control の Max_Payload_Size・Phantom Functions・FLR、Link Control の Link Disable、
    Link Control 2 の Compliance、拡張機能の Resizable BAR（VF 用も）の大きさ、SR-IOV の制御と
    NumVFs、ACS・ATS・PRI・PASID の制御。ブリッジ（クラス 06h）は claim できない。
  - ドライバーは装置を止め終えたらバスマスターを切ってから手放す。claim の終わりにバスマスターが
    入ったままなら、ドライバーは途中で終わった（異常終了など）とみなす。装置上のファームウェアが
    古いアドレスを持ったまま止まっていない可能性がある。回復は終了処理ではなく次claimで試す。
    Function Level Reset は §6.6.2 の手順に従う：Command を Interrupt Disable だけにして新しい
    要求を止め、Transactions Pending が消えるか、もう Completion が届き得ない時間（その機能の
    Completion Timeout の最長の範囲、ただし1秒以上：前のドライバーが待ち中に変更・無効化し得る）
    まで待ち、どちらでもFLRを発行する。止められない（バスマスターが切れない・応答しない）機能は
    FLRせず、Commandを元へ戻す（バスマスターは切ったまま）。FLR完了の100ms（有効なReadiness
    Time ReportingのFLR Timeがあればそれ）の後、最初の構成アクセスとしてVendor IDを読み、RRS、
    またはRRSを見せないRoot Complexが再試行を打ち切った失敗読み（全ビット1）をFLR Timeまで、
    無ければ1.0秒まで待つ（§6.6.1・§5.10.1が故障と判断してよいまでの時間。§6.6.2自体に上限は
    無い）。未準備・取消は保存状態を保持し、次の試行で再開する。途中でスリープ（S3）を挟んだ
    FLRは完了とみなさない：Conventional ResetはFLRの代わりにならない（§6.6.2「Other defined
    reset operations do not guarantee that external I/O operations will be stopped」）。復帰時は
    応答を待つだけで、次の試行が改めて止めてFLRする。FLRの途中の機能へは他の経路から構成
    アクセスしない：次のclaimは、どのclaimも持たない途中のFLRを先に完了させてからgroupを調べ、
    FLR・SBRの途中、またはRRSを返している間のAML（PCI_Config領域）の読みは全ビット1、書込みは
    捨てる（RRSの間はAMLのアクセスごとにVendor IDを読み直し、戻った機能を閉ざし続けない）。
    FLRで戻らない機能（上限までにIDを返さない・要求が残る）は、その装置のリンクのDownstream
    Port（Root PortまたはSwitch Downstream Port）のSecondary Bus Reset（§7.5.1.3.13、hot reset）
    で戻し、その後に改めてFLRする。SBRはそのポートの下の全機能をresetするので、下の全機能が
    claim自身（primary、対象、domainのmember）か未使用のときだけ、claimの試行ごとに同じポートは
    1回まで行う。下にカーネルdriverの機能・起動画面の機能・他のclaimやdomainの機能がある、
    ポートがhot-plug slotへ通じる（リンクの変化はfirmwareのhot-plug処理のもの、§6.7.3.3）、
    下に従来PCIバスがある（Trhfa、§6.6.1）、ポートがEnter Complianceの設定、または対象が前回の
    Conventional Reset後に応答しなかった、のいずれかならSBRしない。SBRはTrst（1ms）以上かつ
    リンクが落ちるまで保持し、解除の2ms後（§4.2.7.11）から§6.6.1の待機（5.0GT/s以下のポートは
    100ms、それ以上はData Link Layer Link Activeの100ms後、IDは1.0秒まで）で読む。下の
    Switchのポートのリンクはそのポート自身のreset終了から数える。ポートのLTR Mechanism Enable
    （DL_Downで消える、§7.5.3.16）を先に戻してから各機能の構成を戻し、Common Clock
    Configurationを戻したリンクを再学習させる（§7.5.3.7）。下の未使用機能はバスマスターを
    切った構成で戻る。
    起動画面機能は汎用FLRせず、隔離下でnative driverに回復を要求する。FLR成功だけでは旧DMAを解放しない。
    P2Pで到達できる複数機能groupでは、終了したアプリの状態が全機能へ残り得るため、clean終了でも
    全機能を回復対象にする。次claimは全peerのBME停止を確認し、旧dirty peerのMSI無効化と個別FLRを
    確認してからhandleを返す。FLRは対象Functionだけをresetするので、新primaryだけの成功では不足する。
    FLRとSBRの後も応答しないpeerは、バスマスター停止のままclaimのdomainに閉じ込め（送るものは
    domainの外へ届かない）、カーネルはそのresetの経路以外で触れず（ACSは無いものとしてgroupを
    作る）、回復が必要な機能として残す。primaryが戻らない場合と、回復不能・取消・未準備・
    起動画面のpeerは`N_ERR_NOT_READY`としてgroupを再遮断する。
    native driverへの回復委任は新primaryだけであり、未知peerの回復を暗黙に委ねない。
  - `n_pci_hold_screen(pci, hold)`：起動時の画面がその機能の BAR にあるとき、ドライバーが
    CPU から画面への経路を作り直す間（NVIDIA の RM は GSP が BAR1 を仮想モードにする間）、
    システムは画面へ書かない。`true` は書込み中のものが終わってから戻り、入れ子にできる。最後の
    `false`（または claim の終わり）で画面全体を描き直す。時間切れで描画を再開することはない
    （作り直しの途中で書くことこそ保持が防ぐ競合）。claim の終わりは装置を止めて（BAR の写像の
    取消し、割り込み、バスマスター）から保持を返す。保持中に利用者が Ctrl+Alt+Esc を押すと（デスクトップでもテキストコンソールでも）、
    保持しているドライバーのアプリをシステムが止め、claim の終わりでCPU描画を再開する（scanoutの復旧保証ではない。保持が無ければ
    Ctrl+Esc と同じくメニュー）。起動項目 `fbhold=<秒>` を付けると、それだけ保持したときにも
    同じく止める（時間の既定は無し：RM の待ちは次々に続き、上限を決められない。遅いだけの
    ドライバーを止めると GPU が使えなくなるので、止まったと判断するのは画面を見ている利用者）。
    画面が別の所なら `N_ERR_NOT_FOUND`。パニック画面は保持に関係なく描く。
  - `n_pci_bar(pci, i)` はメモリ BAR を装置メモリの共有オブジェクト（既定は非キャッシュ）にする。
    1 ページ未満や境界の合わない BAR、I/O ポートは扱わない。
  - `n_pci_interrupts(pci, count)` は MSI-X（無ければ MSI）を最大 32 本つなぎ、本数を返す。
    INTx はアプリへ渡さない。届いた割り込みは番号のビットを立て、ビットがある間ハンドルは
    `N_EVENT_RECEIVABLE`。`n_pci_interrupt_take(pci, timeout_ms)` がビットを取り出して消す。
  - `n_dma_create(pci, bytes, flags)` は装置が DMA で読み書きできる 0 埋めの RAM（共有
    オブジェクト）。`N_DMA_CONTIGUOUS` は物理的に一続き、`N_DMA_32BIT` は 4GiB 未満。
    `n_dma_addresses` が各ページのバスアドレスを返す（物理アドレス。IOMMU が隔離していても
    同じ番地に写すので変わらない）。
  - IOMMU（`kernel/src/dev/iommu.c`、AMD-Vi は `iommu_amd.c`、VT-d は `iommu_intel.c`）：起動時に
    ACPI の IVRS か DMAR からユニットを見つけて設定する。claim されていない装置とカーネルの装置は
    素通し（AMD-Vi は 0 の装置表エントリ、VT-d は pass-through の文脈エントリ）。claim した装置は
    専用のドメインに入り、DMA が届くのはその claim で作った DMA オブジェクト（作成から解放まで）と
    ファームウェアの予約範囲（IVMD・RMRR）だけ。割り込みは再マップ表で claim のベクターだけに限る
    （VT-d は互換形式の割り込みを拒み、IOAPIC とカーネルの装置の MSI も表を通る。拡張割り込み
    モードを必須とするユニット（ECAP.EIMER）には、CPU が x2APIC で全ユニットが EIM を持つ時だけ
    x2APIC 形式の宛先を書き、そうでなければ割り込みの再マップを使わない）。再マップを必須と
    するユニット（ECAP.IRREQ）は、切れている間すべての割り込みメッセージを止める（§5.1.4）。
    そのため `noiommu` の時、ファームウェアの記述を保持できなかった時、共通のページ表形式が
    無い時、DMA 変換の起動に失敗した時も、割り込みの再マップだけで起動する（DMA は変換しない）。
    再マップ自体ができなければ、その旨（割り込みが止まること）をログに出す。
    予約範囲は IVMD の読み書きの許可どおりに写す。claim の終了で装置は遮断され、ユニットが
    確かめた後は BME を残した終了でも DMA の RAM は割り当て器へ戻る。確かめられなかった遮断は
    保留され、次の claim のたびにやり直す。確かめられるまで、その装置の再 claim は `N_ERR_BUSY`、
    DMA の RAM とベクターは他へ渡さない。遮断はグループの他の機能にも残る：ドライバーは
    claim した機能の P2P 要求でそれらへ届き、アドレスを渡せたからで、次にどれかが claim される
    まで解かない（グループにカーネルの装置は入らない）。
    - グループ：claim した装置と P2P で互いに届く装置は同じドメインに入れる（そのログが出る）。
      要求元 ID（エイリアスを含む）を共有する装置、ACS の P2P Request Redirect と Completion
      Redirect が効かない多機能装置の他の機能、ACS の効かないスイッチの下流ポートの下の全部、
      ACS の無い・Source Validation の無い・ACS が効かないルートポートなら全装置（PCIe 6.0
      §6.12.1.1：Source Validation は ACS のもので、無ければ下の装置は任意の要求元 ID を名乗れる。
      §1.3.1：ルートポート間の P2P を持つかはルートコンプレックスの選択）。ルートコンプレックス
      内蔵の機能はポートを経ず、その要求は P2P も含めてユニットを通る（VT-d §3.17、AMD-Vi §2.1）。
      ACS は読み戻して確かめ、Egress Control と Direct Translated P2P は切る。試みで変えた ACS の
      制御は、ドメインにならなければ元へ戻す。
    - 隔離できない時：グループにカーネルの装置があるか、IOAPIC・HPET の ID を含むか、どの
      ユニットにも見えない ID があるか、列挙がメモリ不足で全装置を把握していなければ、
      既定では `N_ERR_BUSY`／`N_ERR_UNSUPPORTED`で拒否する。非隔離利用には管理者が明示的に
      起動オプション `appdrivers-unisolated` を選ぶ必要がある。IOMMU不在・`noiommu`にも同じ方針を適用する。
      この設定でも無効化命令失敗、他のclaim、以前のclaimの未確定状態は迂回できない。以前のclaimの遮断がグループに
      残っていれば、その理由に応じて `N_ERR_BUSY`／`N_ERR_UNSUPPORTED`。グループに claim 済みの
      装置か、遮断が未確認の装置・他の claim のドメインの装置があれば `N_ERR_BUSY`。ユニットが
      設定を確かめなければ `N_ERR_IO`（書いた分は遮断する）。
    - ユニットの無効化命令の失敗・時間切れは握り潰さない（その操作は失敗になる）。AMD-Vi は
      不正命令で止まった命令バッファーを再開し、イベントログの溢れは記録が止まったと確かめて
      から仕様の順で立て直す（止まるまでは触らず、次の取出しでやり直す）。VT-d が拒んだ記述子は
      skipしたりIQEを消して再試行せず、IQE/ICE/ITE・完了不明を保持して資源を隔離する（§6.5.2.11）。
      待ち行列を切るのは、wait で空と確かめた後だけ。書込みバッファーを持つ旧ユニット
      （CAP.RWBF）で無効化の要らない新しい写像は、その flush が確かめられなければ失敗になる。
    - AMD-Vi の命令バッファーは CmdBufEn を書いた後、CmdBufRun が立つ（取出し開始）まで
      待って確かめる（§3.4.16・§2.4）。Performance Optimization（PerfOptEn、内蔵装置が IOMMU を
      迂回しうる）は切り、解除を読み返す。ファームウェア由来のExclusionBase全体迂回も解除する。
      MARC は使わない（MarcEn を立てない）。
    - VT-d の通常割込みモードでは、全ユニットの互換形式素通し（CFI）解除を確認してから
      IREを有効にする。EIMEでは互換形式自体が遮断される（§5.1.4）。一時的なCFI有効化は不要。
      起動時はPICとIOAPICをマスクし、ユニット起動とIOAPIC経路変換を確認した後だけPICの
      元のマスクを戻す。経路変換失敗は起動失敗として扱い、ユニット全停止を確認できた時だけ
      通常形式へ戻す。マスクだけの復帰にPIC再初期化を使わず、保留要求を消さない。
      停止が未確認、またはIRREQユニットで必須再マップを起動できない場合は、後からのMSI/IOAPIC
      経路作成・解除確認も失敗させる。停止済みでもIRREQ=1では通常形式が届くとは限らない。
    - スリープが途中で取り消された時は、ユニットを設定し直さない（眠っていないので claim の
      表で動いたまま）。TSC周波数が不明なら推測周波数を使わず、即座にreadyと確認できない操作を失敗にする。
    - 限界：AMD-Vi の装置表は SMI を止められない（SMI フィルターはファームウェアの SMI も止め
      うるので使わない）。claim した装置が SMI のメッセージを送れば SMM へ届く。
    隔離されている装置は `n_pci_info` の `flags` に `N_PCI_ISOLATED`。IOMMU が無い機械、起動
    オプション `noiommu`、隔離できない装置は既定でclaimを拒否する。`appdrivers-unisolated`を明示した
    非隔離claimは全メモリへ届くため、その危険をログに残す。対応幅を超す物理アドレスをPTEへ切り詰めず拒否し、
    DMA確保は実際の変換幅内へ制限する。IVMDのaccess=0/ROをDMAオブジェクトが拡大せず、解放で予約を消さない。
    S3では保存したPIC/IOAPIC経路をマスクし、復帰ファームウェアが再有効化した経路も再び止めてから
    IOMMU→IOAPIC→PIC→PCIの順で再開する。IOAPICの経路は、与えられた要求から毎回作り直す。
    IOMMUが戻らない場合は停止（panic）せず、起動時の失敗と同じ段階で縮退する：ユニットが停止を
    確認し、IRREQも無ければ、IOAPICの経路とPCI機能のMSI/MSI-Xを、保存した再マップ形式ではなく、
    与えられた要求（ベクターと宛先）から通常形式で作り直す。DMAは変換なしで続ける。終わったclaimの
    グループに属する機能はBus Master Enableを切ったままにする。停止が未確認、またはIRREQなら
    装置割込みを全て切ったままにする。どちらの場合も、再起動まで claim・再スリープ・休止を拒否し、
    ユーザーへ通知する。x2APICモードは再マップなしで戻る（起動時と同じ、SDM 11.12.7 からの既知の逸脱）。
    休止復元で停止できない表へメモリを上書きしない。中止sleepではPICを再初期化せずmaskだけ戻す。
    実edge割込みの無損失はこの順序だけでは保証しない。
  - `n_shared_view(shared, offset, bytes, cache)` は共有オブジェクトの一部を、別のキャッシュ
    属性（`N_CACHE_*`）で見る新しい共有オブジェクト。装置メモリは非キャッシュか
    write-combining だけ、DMA 用でも装置でもない共有オブジェクトは元の属性だけ。DMA 用の RAM は
    一つのページに二つの型を持たせないため、オブジェクト全体で型を一つだけ持つ：写像が一つも
    無い間に限り、最初に頼んだ別の型（非キャッシュか write-combining）がオブジェクトの型になり、
    カーネルの直接写像も同じ型に変わる。その後の別の型のビューと、写像がある間の型の変更は
    `N_ERR_DENIED`。`N_CACHE_DEFAULT` のビューと元のオブジェクトの写像はその型を使う。
    装置メモリも、生きている写像の間では一つのページに型を一つだけ許す：あるページが非キャッシュで
    写されている間、そのページを含む write-combining の写像は `N_ERR_DENIED`（逆も同じ。ログに
    出る）。型は写像が全部外れると自由になる。起動時の画面の範囲はカーネルが write-combining で
    写し続けているので、その範囲を含む非キャッシュの写像は常に `N_ERR_DENIED`。PAT の無い CPU では
    write-combining は非キャッシュとして数える。
  - `n_shared_map` の書込み可（`N_MEMORY_READ_WRITE`）は、書込みで開いたハンドルにだけ許す。
    view も共有オブジェクトなので、`n_shared_map` で写し、チャネルの添付で他のアプリへ渡せる。
  - `n_shared_view_access(shared,offset,bytes,cache,access)` は READ または READ_WRITE の
    最大権利を指定する。元ハンドルの権利を拡大できず、RO は map・派生 view・合成でも RW にならない。
    `n_shared_revoke(view)` は発行アプリだけが実行でき、子孫 view とそれに依存する合成の既存 PTE も取り消す。
    元の独立した所有者の写像は取り消さない。受信だけでは取消権限は得ない。
  - `n_shared_compose(ranges,count,access)` は `n_shared_range` の
    `{int32_t shared; uint32_t reserved; uint64_t offset,bytes;}` を配列順に連結した共有ハンドルを返す。
    reserved は0、offset/bytes はページ境界、bytes と count は非0。
    各 source 内の範囲、配列・総サイズの overflow と実確保資源を検査し、source 数の固定上限は設けない。
    新ハンドルの最大権利は全 source の権利内。source を close しても backing と取消依存は保持する。
    非連続 BAR page、RAM、DMA の属性を segment ごとに保ち、ページを複製しない。
    DEFAULT の派生 view は異種 cache を維持し、明示する単一 cache は対象範囲の全 segment と一致する必要がある。
    source view の取消は nested composite/view にも及ぶ。BAR claim 消失時は該当 page が all-ones となり、
    他 segment の独立 RAM は生存する。失効済み BAR を含む合成の新しい map/view/再合成は拒否する。
    DMA の cache pin は PTE/TLB 除去の完了まで保持する。部分失敗では取得した参照を回収する。
    map 構築も取消対象として追跡し、取消済み範囲に後から PTE を設置しない。
    まだ返却していない構築中 VA へ別 thread が release/protect を要求した場合は `N_ERR_NOT_READY`。

## 10. 移行の段階

1. この文書。新システムコールを旧ABIと並べてカーネルへ追加し、`nocturne.h` に宣言する
   （番号は128から。`tests/nativeabitest.c` がゲスト内で実カーネルを確かめる）。
2. libc（stdio、malloc、time、getenv）を新ABIへ移す。
3. OS自身のアプリ（ブラウザー、端末、シェル、ファイル、coreutils 等）を新ABIへ移す。
4. 移植物のソースを新ABIへ直接書き換える。tcc がゲスト内でコンパイルする
   プログラム用のヘッダーも新ABIだけにする。
5. 旧システムコールと `fcntl.h`／`unistd.h`／`sys/*.h`／`alloca.h`、`O_*`、`F_*`、
   `N_POLL*`、POSIX型（`ssize_t`、`pid_t`、`off_t`、`mode_t`）を削除する。

各段階で `make qemu-image` によるビルドと、QEMU実画面でのブラウザー・端末・
ファイル操作の確認を行う。旧ABIの削除は、全利用箇所が新ABIへ移った後に限る。

段階5で削除した番号（1〜18、22〜25、30〜32、35〜37、57、59〜63、90）は欠番のまま
再利用しない。残る番号は意味を変えない（OS内で tcc 等が作ったプログラムのため）。
旧番号を使って作ったプログラムは作り直しが要る。新たに加えたもの：

- `SYS_WIN_RECEIVE`（窓、`struct gui_event *out`、最大数、`timeout_ms`）：待ち行列から
  入るだけ一度に受け取る。件数、期限で0、閉じた窓が最後のイベントを渡し終えると
  `N_ERR_CLOSED`。窓はバイト列として読めない。
- `SYS_MEMORY_RESERVE`／`RELEASE`／`PROTECT`：`n_memory_*` そのもの。`access` が
  列挙外、または書込みと実行の同時指定は `N_ERR_INVALID`。
- `SYS_SHARED_MAP`（共有ハンドル、バイト数、`access`）：自分のハンドルだけ。
- `SYS_DMESG` は `SYS_SYSTEM_LOG`（同じ33番）。

## 11. インターフェースエンジン（`ui.h`）

GUIの部品（ボタン、入力欄、一覧、スライダー等）は、libc の `user/libc/ui.c` にある一つの
エンジンが配置・描画・入力処理を行う。アプリは窓を「部品の木」として記述して送り、
「ボタン3が押された」のようなイベントを受け取る。ホバー、押下、フォーカス、カーソル、
スクロール、動き、配色はエンジン側が持つ。言語ごとに部品を作り直さない：

- C と `js`（TypeScript／JavaScript）は同じプロセス内で `ui_session_send`／
  `ui_session_peek` を呼ぶ。
- C を呼べないプログラム（Go 等）は、メッセージチャンネルの一端をハンドル3として
  `C:\Nocturne\System\uihost` を起動し、同じバイト列をチャンネルの一メッセージずつ送受信する。
  `uihost` は自分の端が閉じられると終わる。

数値はすべてリトルエンディアン。f64 は IEEE の倍精度、str は u32 の長さ＋UTF-8。
色は 0xAARRGGBB（上位バイトが不透明度）。

### アプリからエンジンへ

| 型 | 内容 | 意味 |
|---|---|---|
| `'O'` | u32 窓、u32 幅、u32 高さ、u32 flags、u32 背景（0＝配色の既定）、str 題名 | 窓を開く |
| `'V'` | u32 窓、ノード | この木を表示する |
| `'T'` | u32 窓、str 題名 | 題名を変える |
| `'C'` | u32 窓 | 閉じる |
| `'M'` | u32 要求番号、f64 大きさ、u32 書体（ビット 1 太字、2 等幅）、str 文字列 | 文字列の寸法を測る |
| `'Q'` | なし | 配色と寸法を問い合わせる |
| `'S'` | u32 サーフェス、u32 幅、u32 高さ、［u32 flags］ | サーフェスを作る／大きさを変える |
| `'P'` | u32 サーフェス、［u32 x、y、幅、高さ］ | その画素（矩形を付ければその部分だけ）が変わった |
| `'D'` | u32 サーフェス | 捨てる |

窓の flags は `UI_WINDOW_FLAGS` の範囲だけ：`WIN_RESIZABLE`、`WIN_CENTER`、
`WIN_START_MAXIMIZED`、`WIN_ALWAYS_ON_TOP`、`WIN_DOUBLE_BUFFER`（各画面は空きバッファーを
待ち、全体を描く。表示に合わせて間隔が決まる）、`WIN_DEEP_COLOR`。サーフェスの flags は
`UI_SURFACE_DEEP`（1）だけで、画素の後ろに1画素1バイトの深い面（`win_deep` と同じ並び）を
持つ。10bpc の画面で `WIN_DEEP_COLOR` の窓に置くと、深い面も画面へ写る。範囲外のビットは
不正なメッセージ。

`'P'` に矩形を付けると、エンジンは木を描き直さず、その部分だけを窓へ写して画面へ出す
（動画、カーソル、動く駒など）。サーフェスが配置された大きさと違う、スクロールバーと
重なる、二重バッファーの窓などで直接写せない時は、次の絵でその部分を描き直す。

エンジンは各ノードの見た目（プロパティ、位置、ポインターとキーボードの状態、アニメーション）を
覚えておき、次の絵では変わったノードの箱だけを描き直して画面へ出す（周りはその範囲で描き直すので、
全体を描き直した時と同じ絵になる）。窓の大きさが変わった時と二重バッファーの窓は全体を描く。

### ノード

ノードは：u8 種類名の長さ、種類名、u16 プロパティ数、u16 子の数、プロパティ、子。
プロパティは：u8 名前の長さ、名前、u8 型、値。型は 1＝f64、2＝str、3＝u32 n＋n 個の f64、
4＝u32 n＋n 個の str、5＝u32 n＋n バイト。知らない種類は何も描かず、知らないプロパティは
無視する（新しいエンジンが古いアプリを動かせるように）。ノードの「場所」は、その木を
前順で数えた番号（0から）で、イベントはこの番号で部品を指す。

種類：`column` `row` `scroll` `card` `text` `button` `toggle` `checkbox` `slider`
`progress` `segmented` `field` `item` `icon` `divider` `spacer` `canvas` `surface`。

プロパティ：

- 配置：`width`／`height`（数値、または文字列 `"fill"`）、`minWidth`、`minHeight`、
  `maxHeight`、`grow`、`padding`（数値、または2個［縦、横］か4個［上、右、下、左］）、
  `gap`、`align`／`self`（`"start"` `"center"` `"end"` `"stretch"`）、`justify`
  （それに `"between"`）、`radius`、`key`（数値か文字列。木を送り直してもスクロール位置等を
  保つ同一性）。
- 色：`background`、`border`、`colour`、`hoverBackground`、`pressedBackground`、
  `shadow`（0/1）。
- 文字：`text`、`size`、`weight`（0 通常、1 太字）、`style`（`"heading"` `"title"`
  `"caption"`）、`textAlign`、`lineHeight`、`wrap`、`ellipsis`、`mono`。書体は Noto Sans、`mono` では Maple Mono（どちらも CJK を含む）。
- ボタン：`text`、`icon`、`variant`（`"primary"` `"secondary"` `"ghost"`）、`compact`、
  `disabled`。どの箱も `clickable` で押せる。
- `toggle`／`checkbox`：`on`、`text`。`slider`／`progress`：`value`、`min`、`max`、
  `step`、`wheelStep`、`length`。`segmented`：`options`（型4）、`selected`。
- `field`：`value`（文字列）、`placeholder`、`secret`。`item`：`text`、`icon`、`selected`。
- `icon`：`display` `mouse` `sound` `info` `check` `plus` `minus` `close`
  `chevron-right` `chevron-left` `settings` `folder` `clock` `refresh` `backspace` `home` `search`。
- `canvas`：`draw`（型5の描画命令）、`drawn`（型3の［幅、高さ］：命令を作った大きさ）。
- `surface`：`surface`（サーフェスの番号）、`pointer`（1：ポインターとホイールをアプリへ）。
  `canvas` にも `pointer` を付けられる。

描画命令（`draw`）は u8 命令、その f64 引数、文字列を取る命令は続けて str：

| 番号 | 引数 | 描くもの |
|---|---|---|
| 1 | x、y、幅、高さ、色 | 矩形 |
| 2 | x、y、幅、高さ、半径、上の色、下の色 | 角丸の縦グラデーション |
| 3 | x、y、幅、高さ、半径、線幅、色 | 角丸の枠 |
| 4 | x、y、幅、高さ、半径、ぼかし、色 | 角丸の影 |
| 5 | x0、y0、x1、y1、線幅、色 | 端が丸い線 |
| 6 | x、y、大きさ、書体（`'M'` と同じビット）、色、str | 文字列 |
| 7 | x、y、大きさ、色、str | アイコン |

### エンジンからアプリへ

返信（`'o'` `'m'` `'q'` `'s'`）は要求の順に、待っているイベントより先に届く。

| 型 | 内容 |
|---|---|
| `'o'` | u32 窓、i32 結果（0 か -N_ERR_*）、u32 幅、u32 高さ |
| `'e'` | u32 窓、u32 木（何番目の `'V'` か、1から）、u32 イベント、u32 場所（窓なら 0xFFFFFFFF）、u32 mods（NMOD_*）、u32 flags、f64 a、f64 b、str 文字列 |
| `'m'` | u32 要求番号、f64 幅、f64 行の高さ、f64 ベースライン |
| `'q'` | u32 個数、個数×（u8 名前の長さ、名前、f64 値） |
| `'s'` | u32 サーフェス、i32 結果、u32 幅、u32 高さ。`uihost` 経由では共有ページを添付する（`n_shared_map` で写す） |
| `'x'` | `uihost` だけ：i32 結果、u8 拒否したメッセージの型 |

イベント：

| 番号 | 名前 | 内容 |
|---|---|---|
| 1 | CLICK | ボタン、一覧の項目、`clickable` の箱 |
| 2 | TOGGLE | a＝新しい状態（0/1） |
| 3 | SLIDE | a＝値。ドラッグが終わると flags に `UI_DONE`（1） |
| 4 | CHOOSE | a＝選んだ項目の番号 |
| 5 | TEXT | 文字列＝入力欄が今持つべき値（次の木で `value` として送り返す） |
| 6 | SUBMIT | 入力欄で Enter。文字列＝その値 |
| 7 | KEY | 部品が使わなかったキー。文字列＝`"Escape"` `"Enter"` `"ArrowUp"` … `"F12"` か文字、a＝NKEY_ の番号か文字、離した時は flags に `UI_UP`（8） |
| 8 | CLOSE | 閉じるボタン（`UI_GONE`（2）：窓は既に無い） |
| 9 | CANVAS | `canvas`／`surface` が作った大きさと違う大きさに配置された。a、b＝その大きさ |
| 10 | RESIZE | a、b＝窓の新しい大きさ |
| 11 | POINTER | `pointer` 付きの上で：a、b＝その中の位置。flags に `UI_DOWN`（4）か `UI_UP`（8）（どちらも無ければ移動）、押しているボタンは `(flags >> 8) & 0xFF`（1 左、2 右、4 中）。押した後は全部離すまでその部品が受け取り、外へ出ると外の位置で移動を一つ送る |
| 12 | WHEEL | `pointer` 付きの上で：a＝刻み数（手前が正） |
| 13 | FOCUS | 入力欄がキーボードを得た（a＝1）／失った（a＝0。Escape で手放した時は flags に `UI_CANCEL`（16））。場所が窓（0xFFFFFFFF）なら窓のこと |

入力欄はカーソルと選択範囲をエンジン側で持つ（←→、Home／End、Shift で選択、Ctrl で単語
単位、Ctrl+A／C／X／V、クリックとドラッグ）。入力欄が使わないキーは KEY としてアプリへ
届き、その離鍵も届く。アプリがまだ送り返していない値の上に次のキーを積むので、速い入力が
取りこぼされない。

値が前の木から変わった時だけ働くプロパティがある（もう一度頼む時は数を増やす）：

- `focus`（入力欄）：正の数ならキーボードを取る（カーソルは末尾）。0以下なら手放す。
- `reveal`（どの部品にも）：入っているスクロール箱を、それが見える位置までスクロールする。

`selectOnFocus`（入力欄）は、クリックや `focus` でキーボードを得た時に全選択する。`ellipsis`（文字列）は
一行に収め、入らない部分を「…」で切る。`mono`（文字列、入力欄）は等幅の Maple Mono で描く。
フォーカスのある入力欄が木から消えると、
フォーカスも消える。
