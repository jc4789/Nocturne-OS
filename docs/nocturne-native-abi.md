# Nocturne ネイティブABI（UNIX型ABIの置き換え）

Nocturneは独自OSであり、UNIX／POSIX／Linuxではない。以前の `common/abi.h` は、
整数fdへの汎用 `read`／`write`／`lseek`／`fcntl`／`dup2`／`poll`、`open` フラグ、
`chdir`／`getcwd`、`spawn`＋`waitpid`／`kill`、`pipe`、`sbrk`／`mmap`、
`ENOENT` 型のエラー名という、UNIXの骨格を持っていた。名前を変えても骨格が同じなら
UNIXのままなので、この文書は**骨格そのもの**をNocturne独自の方式へ置き換えた仕様を定める
（旧ABIは §10 の段階5で削除済み）。

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
  `N_KIND_CONNECTION`（TCP／UDP）、`N_KIND_PCI`、`N_KIND_PAGER`、`N_KIND_WATCH`（見張り。下）。
  12 は欠番（§10）。
- 汎用操作は `n_handle_close(h)` と `n_handle_kind(h)` だけ。読み書きは種類ごとの
  専用関数で行い、種類の違うハンドルを渡すと `N_ERR_WRONG_KIND`。
- 見張り（`N_KIND_WATCH`）は、見ている物が変わったことを知らせるハンドル。開く口は見る物ごとに
  あり（C: の `n_entry_watch`（§3）、音の `n_audio_output_watch`（§9）、画面の `n_screen_watch`）、
  取り出しは共通の `n_watch_take(h)`（`SYS_WATCH_TAKE`）一つ。
  - 変化があると `N_EVENT_RECEIVABLE` になり、`n_watch_take` が数を取るまでそのまま（レベル）。
    `n_watch_take` は前の取り出しの後に来た変化の数を返す（無ければ 0）。数えるのはハンドルごとに
    開いた時からで、同じ物を見るほかのハンドルの取り出しは関わらない。`N_WATCH_CHANGES_MAX` で
    止まり、溢れない。
  - 見ている物が無くなると `N_EVENT_CLOSED`。取り出しはまず残っていた数を返し、その後は
    `N_ERR_CLOSED`。
  - 見張りでないハンドルの取り出しは `N_ERR_WRONG_KIND`。見張りはイベント待ち行列（§6）に入れて待つ。
    中身（値や内容）は運ばず、知らせを受けた側が持ち主の読み出し（`n_entry_info`、`n_document_load`、
    `n_audio_output` など）で読み直す。
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
- `struct n_entry` を返すシステムコールは、呼び出し側の `sizeof(struct n_entry)` を受け取り、
  各項目のその長さだけを、その間隔で書く。新しい欄は末尾にだけ加える。長さが
  `N_ENTRY_FIRST_BYTES`（280、`name` から `modified` まで）より短ければ `N_ERR_INVALID`、
  カーネルの知る形より長ければ `N_ERR_UNSUPPORTED`。libc の `n_entry_info`／`n_folder_list`／
  `n_document_info` は `SYS_ENTRY_INFO_SIZED`／`SYS_FOLDER_LIST_SIZED`／`SYS_DOCUMENT_INFO`
  に自分の長さを渡す。`id` より前の SDK で作ったプログラムの `SYS_ENTRY_INFO`／`SYS_FOLDER_LIST`
  は、今も最初の形（280バイト、一覧の間隔も280バイト）を受け取る。その形で 0 だった
  `reserved` の位置は今の `modified_ns`（古いプログラムは読まない）。
- `n_document_info(h, struct n_entry *)` は開いている文書の `struct n_entry` を返す。名前は空
  （ハンドルは名前でなく文書そのものを指し、開いた後に移されても同じ文書のまま）で、`size` は
  そのハンドルが読む長さ（`n_document_size` と同じ）。
- `n_entry_move` は名前の付け替えで、内容は動かない。既存の名前を決して上書きしない
  （`N_ERR_EXISTS`。同じ項目の大文字・小文字の変更は除く）。フォルダーを自分の中へは
  移せない（`N_ERR_INVALID`）。
- `n_entry_replace(from, to)` は `n_entry_move` と同じだが、文書 `from` は既存の文書 `to` の名前を
  取れる。元の `to` は同じ確定（commit）の中で `n_entry_remove` と同じように消える（開いている
  ハンドルは消えた文書を読み書きし続ける）。`to` が文書以外なら `N_ERR_EXISTS`。
- 見張り：`n_entry_watch(path, what)`（`SYS_ENTRY_WATCH`）は C: の見張り（§1）を返す。プログラムは
  自分の設定や好みの文書をこれで見張り、ほかのプログラム（自分の別の窓、設定の窓）が保存すると
  読み直す。
  - `what` は `N_WATCH_NAME`（`path` の最後の名前を、そのフォルダーの中で。その名前の項目が今
    あるかは問わず、大文字・小文字の違う綴りも同じ名前）か `N_WATCH_FOLDER`（`path` のフォルダーの
    直下の全部。中のフォルダーの中は見ない）。それ以外は `N_ERR_INVALID`（64 ビット全体で比べる）、
    `C:\` の名前も `N_ERR_INVALID`。フォルダーが無ければ `N_ERR_NOT_FOUND`、途中や
    `N_WATCH_FOLDER` の相手がフォルダーでなければ `N_ERR_NOT_FOLDER`。
  - 数えるのは、新しく開いた `READ`・`n_entry_info`・`n_folder_list` から見えるようになった変化：
    項目の作成、削除（開いたままの文書も名前はその時に消える）、移動の出入り、名前の変更（大文字・
    小文字だけでも。今と同じ綴りへの移動は変化でない）、`n_entry_replace`、文書に書いた内容が
    保存された版になった時（`n_document_save`、`n_document_commit`、最後の書き手の
    `n_handle_close`、`N_DOCUMENT_EDIT` の書き込みが 16 MiB たまった時、システムの書き手が数秒
    ごとに取り込む時）。`n_document_write_at` だけでは数えない（知らせで読んだ側が古い内容を読み、
    その後に知らせが来なくなる）。書き手が開いている間の `n_entry_info` の `size` は書きかけの長さ
    だが、それも保存までは数えない。
  - 一つの操作は、見張りごとに一回：移動は離れる名前、入る名前、両方のフォルダーに一回ずつ
    （同じフォルダーの中の名前の変更はそのフォルダーに一回）、まだ無い名前への `n_document_save`
    も一回（作成と内容の取り込みの間は誰にも見えない）。`N_DOCUMENT_CREATE` で開いた文書は、
    作った時と、書いた内容を保存した時の二回。
  - 名前で見て、項目の `id` では見ない（`n_entry_replace` や `n_document_save` で名前の指す項目が
    替わっても、同じ名前を見続ける）。フォルダーは項目そのものとして持つので、移されても名前が
    変わっても見続ける。フォルダーが削除されると、それを見る見張りもその中の名前を見る見張りも
    `N_EVENT_CLOSED` になる。同じ名前で作り直したフォルダーは別のフォルダー。`N_WATCH_NAME` の
    名前がフォルダーを指していても、その中の変化は数えない。
  - 権利は `n_entry_info` と同じ：アプリに見えない（PRIVATE の）項目の変化は数えず、PRIVATE の
    フォルダー（`C:\Nocturne\Temp`）は見張れない（`N_ERR_NOT_FOUND`）。SYSTEM のフォルダーは
    読めるので見張れる。
  - 同じ物を見る見張りは一つの物を共有し、数はハンドルごと。C: に見張りが一つも無ければ変化の費用は
    変わらない。見張りがあれば、変化ごとにフォルダーの番号で表を一度引き、そのフォルダーの見張りの
    数だけかかる（ほかのフォルダーの見張りの数にはよらない）。
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
    子アプリは起動元と同じ利用者のために働く：libc の `n_app_launch` は、起動が名指さなければ
    起動元自身の `USERFOLDER` と `TEMP` を渡す（ブラウザーの窓と、その Worker・保存・設定の窓が
    同じ profile を使う）。どちらも無い起動には、カーネルがシステムの利用者のものを加える。
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
- 共有の写像（`n_shared_map`、プログラムの像）のページへの `n_memory_protect` は、範囲を一つの
  写像の中に収め（はみ出せば `N_ERR_NO_MEMORY`）、その写像の権利の内側だけを許す（読取りだけの
  写像への書込みと、共有の写像への実行は `N_ERR_DENIED`。共有メモリは実行権なしで写り、プログラムの
  像は写したときの権利のまま。でなければ、別の写像から書けるページを実行できてしまう。取り消された
  写像は `N_ERR_CLOSED`）。権利はページごとに
  覚え、まだ写っていないページ（最初に触れたとき割り当てるページ、ページャー付きメモリの
  結び付いていないページ、装置が消えた範囲）にも効く：後で触れて写すときもその権利を超えない。
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
- ページャー付きの共有メモリ：`n_pager_create(bytes, &memory)` は、自分のページを持たない
  共有メモリ（`memory`、普通の `N_KIND_SHARED` ハンドル。写像も添付での受け渡しも同じ）と、
  その中身を決めるページャーのハンドル（`N_KIND_PAGER`）を返す。ページャーは作ったアプリだけが
  使える：添付では送れず（`N_ERR_WRONG_KIND`）、起動時に渡された他のアプリの呼出しは
  `N_ERR_DENIED`。
  - 結び付いていないページ、または読取りだけで結び付いたページへの書込みに触れたスレッドは
    （カーネル内のコピーも同じ）、ページャーの答えを待つ。`n_pager_take(pager, out, max,
    timeout_ms)` は待っている触れ方を `struct n_pager_fault {offset; access; app;}` で最大 `max`
    個返す（同じページ・同じ権利への触れ方は一つにまとまる。読取りは同じページで待っている
    書込みとは別の触れ方で、読取りでの結び付けでも答えが出る。`app` は最初に触れたアプリの識別子）。
    `timeout_ms` は -1 で無期限、0 で待たない（`N_ERR_TIMEOUT`）。触れ方が待っている間、
    ハンドルは `N_EVENT_RECEIVABLE`、メモリが無くなれば（ハンドルも写像も無い）`N_EVENT_CLOSED`
    で、take は `N_ERR_CLOSED`。
  - `n_pager_supply(pager, runs, count)` は `struct n_pager_supply {offset, bytes, source_offset;
    source; access;}` の各範囲を、ページャーが持つ RAM の共有オブジェクト `source` のページへ
    `N_MEMORY_READ` か `N_MEMORY_READ_WRITE` で結び付ける（すべてページ境界）。source は
    write-back の RAM（確保済み、最初に触れたとき割り当てるもの、write-back の DMA 用 RAM）か
    その view だけで、装置メモリ、他のページャー付きメモリ、プログラムの像、合成（合成の元の
    オブジェクトを渡す）は `N_ERR_DENIED`。書込み権は書込みで開いた source にだけ許す。ページは
    複製しない：source のフレームそのものが写る。誰も触れていないページを前もって結び付けても
    よい。結び付いたページは、どの写像からの触れ方でもカーネルがページャーに尋ねずに写す。
    だから一つの触れ方の往復は take 一回と supply 一回で、以後は往復しない。別のフレームへの
    結び直しは、古いフレームを全写像から外して TLB を撃ち落としてから行う。結び付いたページは
    渡された source（view ならその view）への参照を持つので、source のハンドルや写像を閉じても
    消えない（DMA 用 RAM のメモリ型も固定される）。同じフレームを別の source で結び直すと、
    そのページは後の source に従う。
  - source の view、またはその祖先の view が `n_shared_revoke` で取り消されると、そこから
    結び付いたページは全アプリの写像（構築中の写像も）から外して TLB を撃ち落としてから結び付きを
    失う（view 自身の写像と同じく、取消が返る時にはどこからも写っていない）。次の触れ方はまた
    ページャーに尋ねる。取り消された source からの supply は `N_ERR_CLOSED`。
  - `n_pager_revoke(pager, offset, bytes, access)`：`access` が 0 ならフレームを外し、
    `N_MEMORY_READ` なら書込み権だけを外す。どちらも全アプリの写像（構築中の写像も）から
    取り除いて TLB を撃ち落としてから返る。次の触れ方はまたページャーに尋ねる。
  - `n_pager_fail(pager, offset, bytes)` は、その範囲で待っている触れ方を失敗させる：
    触れたアプリではアクセス違反、カーネル内のコピーでは `N_ERR_BAD_ADDRESS`。
  - ページャーのハンドルが閉じると、待っている触れ方とそれ以後の結び付いていないページへの
    触れ方は失敗し、結び付いたページはそのまま読み書きできる。メモリが無くなると結び付きは
    すべて外れ、source への参照も返る。
  - ページャー付きメモリの view と合成は `N_ERR_WRONG_KIND`。`n_memory_protect` は結び付いて
    いないページにも効き、写すときはページャーが結び付けた権利と、写像の権利と、`n_memory_protect`
    の権利のうち一番狭いものになる：書込みで結び付いても、読取りだけにしたページへの書込みは
    アクセス違反、カーネル内のコピーは `N_ERR_BAD_ADDRESS` で、ページャーには尋ねない。ページャーが
    書込み権を外したページは、`n_memory_protect` で書込みに戻しても、次の書込みでまたページャーに
    尋ねる。
  - CUDA の統合メモリ（NVIDIA UVM、`ports/nvidia/README.md`）はこれで CPU 側の写像を作る。
    UNIX 系の HMM が前提とする仕組みのうち、無いものは次のとおり：アプリの普通のメモリ
    （`n_memory_reserve`、プログラムの像、スタック）を GPU が直接共有すること（ページャー付き
    メモリとして作った範囲だけが GPU と CPU で同じアドレスを持つ）、アプリのページ表の変化を
    ドライバーへ通知する仕組み（ページャー付きメモリでは中身をページャー自身が決めるので要らない）、
    CPU のページを装置メモリのページとして写すこと（装置私有のページ。GPU の VRAM は CPU からは
    BAR 経由でしか見えないので、VRAM に移ったページは結び付きを外し、触れたら RAM へ戻す）。
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
  - `n_audio_flush(a)`：待っている音を捨て、世代を進める。音の装置へ渡したが装置がまだ読み
    始めていない分も取り戻し、他の流れだけで混ぜ直す。止めた後に聞こえるのは装置が既に持つ分
    だけ（AC'97 は鳴っている塊の残りと先読みの一つ、最大 20 ms）。遠隔の画面の相手へ送った分は
    取り戻せない。
  - `n_handle_close` は残りを鳴らし終えてから閉じる。止められたアプリの流れは待たず、flush と
    同じに捨てる。
  - 主音量と出力先：`n_audio_output(struct n_audio_output *io, set)` 一つ。`set` が 0 なら読むだけ、
    `N_AUDIO_SET_VOLUME` で `io->volume`（0〜100。消音も解く）、`N_AUDIO_SET_MUTED` で
    `io->muted`（0／1）を設定する（両方なら音量が先）。戻ると `io` に今の
    音量・消音と `device`（音を鳴らしている先：音声装置の名前、遠隔デスクトップの利用者がいれば
    "Remote desktop"。鳴らす先が無ければ空文字列。タスクバーのスピーカーと同じもの）が入る。
    鳴らす先が無くても音量は保たれ、後から付いた先に使われる。範囲外・未知のフラグは
    `N_ERR_INVALID`。音量と消音は音の部品（`dev/audio.c`）の持ち物で、アプリが変えても、
    タスクバーのスピーカーで変えても、部品が利用者の `AppData\Nocturne\Sound.conf` に保ち、
    次の起動で init より前に戻す（§12）。
  - `n_audio_output_watch()`（`SYS_AUDIO_OUTPUT_WATCH`）：`n_audio_output` が返すもの（音量、消音、
    `device`）が変わると受け取れる見張り（`N_KIND_WATCH`、§12）。誰が変えても知らせ、同じ値を
    当て直すのは変化ではない。
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
    取消し、割り込み、バスマスター）から保持を返す。起動項目 `fbhold=<秒>` を付けると、それだけ
    保持したとき、保持しているドライバーのアプリをシステムが止め、claim の終わりで CPU 描画を
    再開する（scanout の復旧保証ではない。時間の既定は無し：RM の待ちは次々に続き、上限を
    決められない。遅いだけのドライバーを止めると GPU が使えなくなる）。
    画面が別の所なら `N_ERR_NOT_FOUND`。パニック画面は保持に関係なく描く。
    `n_pci_give_screen` で画面を自分の面へ移したドライバーも、同じく保持できる。
  - `n_pci_give_screen(pci, surface, desc)`：表示機能（PCI クラス 03）のドライバーが自分で
    scanout を組んだら、その面をシステムへ渡す。戻った時からデスクトップはその面へ描かれ、
    起動時の画面（GOP）へは書かない（下の `N_SCANOUT_BOOT` か claim の終わりで戻すまで）。
    `surface` はこの claim で作った書込みできる共有
    オブジェクト：BAR（プリフェッチ可能なもの。システムは書込み結合で写す。PAT が無ければ
    非キャッシュ）、または型が write-back でない DMA オブジェクト（書込み結合か非キャッシュの
    view を作るとその型に決まる）、あるいはそれらの view。`struct n_scanout` の `offset` から
    `pitch × height` バイトが面に収まり、`format` は `N_SCANOUT_XRGB8888`、
    `N_SCANOUT_XRGB2101010`、`N_SCANOUT_XBGR8888`（赤が下位のバイト、NVIDIA の X8B8G8R8）の
    どれか。幅と高さは表示エンジンが出すモードのもの（ドライバーがモードを出した後はシステムが
    選んだモード、`struct n_screen_state` の `mode`。各 65535 まで）で、デスクトップはその大きさに
    なり、全画面・最大化の窓も従う。同じ面を別の大きさで渡し直しても同じく大きさが変わる。起動時の
    画面へ戻すとその大きさで描く。0 や 65535 超・未知の形式・面に収まらない範囲は `N_ERR_INVALID`、
    その大きさのデスクトップがメモリーに収まらなければ `N_ERR_NO_MEMORY`（画面はそのまま）。
    受け付けるとカーネルは面をカーネル空間へ同じ型で写し、そのページの型を固定する
    （1 ページ 1 型。ドライバー自身の書込み結合の写像とは衝突しない。非キャッシュの写像とは
    どちらか後の方が `N_ERR_DENIED`）。進行中の描画が終わるのを待ってから出力先を切り替え、
    全体を描き直す。システムログに 1 行。ほかの claim のオブジェクト・表示機能でない claim は
    `N_ERR_DENIED`、ほかの claim の面が表示中なら `N_ERR_BUSY`。`surface` に `N_SCANOUT_BOOT`
    を渡すと起動時の画面へ戻す。claim の終わり（クラッシュを含む）では、装置を止めてから起動時
    の画面へ戻して描き直す。ただしドライバーが表示エンジンを組み替えていれば、その画面は
    実際には映らないことがある（戻す処理は無い。ログにもそう書く）。保持はこれまでどおり全部の
    書込みを止め、パニック画面はその時の出力先と、`n_pci_screen_image`（下）で登録された面の
    すべてへ描く。スリープと休止は、claim のある間
    始まらない（dev/pm.c）ので、ドライバーの面が表示中に起きることはない。
  - モニターのモード（解像度とリフレッシュレート）：選ぶのはシステムと利用者、列挙・検証・設定は
    ドライバー。`n_pci_screen_modes(pci, modes)` で表示機能のドライバーが、駆動するモニター
    （`struct n_screen_modes` の `monitor`：EDID の製造者・製品・製造番号から作る識別子、`name`：
    EDID のモニター名）と、そこで出せるモード（最大 `N_SCREEN_MODES_MAX`。EDID の推奨モードに
    `N_SCREEN_MODE_PREFERRED`）を知らせる。システム（gui/screenmode.c）は、試行中のモード、無ければ
    利用者がそのモニターに選んだモード、無ければモニターの推奨モード、無ければ推奨の大きさで最も
    近いリフレッシュ、無ければ 60 Hz に近い最大の大きさを選び、`struct n_screen_state` の
    `mode` と `mode_serial`（望むたびに進む。同じモードでも）でドライバーへ伝える。ドライバーは
    それを出し（その大きさで `n_pci_give_screen`）、`done` に `mode_serial`、`shown` に出せたモードを
    入れて知らせ直す。望まれたものと違えば、システムはそのモニターでそのモードを二度と望まず、
    別のものを選ぶ。`done` 0（開始時、モニターの変化・接続時）では選び直す。`count` 0 は、動いた
    ままモニターを駆動しなくなったこと、claim の終わりはドライバーが止まったこと。起動時の画面へ
    戻した（`N_SCANOUT_BOOT`）ドライバーがモードを出したままなら、利用者の試すモードで
    `mode_serial` が進むので、それを見てもう一度モードを出せる。表示機能でなければ
    `N_ERR_DENIED`、件数・0 の大きさやリフレッシュ・65535 超は `N_ERR_INVALID`、ほかの claim の面が
    表示中かモードを出していれば `N_ERR_BUSY`。
  - 設定アプリは `SYS_SCREEN_INFO` の第3引数 `SCREEN_MODES_GET`（`struct screen_modes`：モニター、
    モード、表示中と望まれたモード、試行の残り時間、直前の試行を出せなかったか、画面の出所
    `source`）と `SCREEN_MODE_SET`（`struct screen_mode_set`）を使う（display_abi.h）。
    `SCREEN_MODE_TRY` は一覧のモードを試行として出し、`SCREEN_MODE_TRIAL_MS`（15 秒）以内に `SCREEN_MODE_KEEP` が
    無ければ前のモードへ戻る（`SCREEN_MODE_BACK` は即座に）。試行中の別の `TRY` は戻り先を
    保つ。`KEEP` はそのモニターの利用者の選択にする。選択は画面のモードの部品
    （`gui/screenmode.c`）の持ち物で、部品が利用者の `AppData\Nocturne\Displays.conf` に保ち
    （§12）、次の起動でも、そのモニターが駆動されそのモードを出すときに使う。`struct screen_modes` の `chosen` は `offered` のモニターに
    利用者が選んだモード（無ければ全部 0）。`chosen` より前に作ったプログラムは大きさに
    `SCREEN_MODES_FIRST_BYTES` を渡し、その分だけ受け取る。4（以前の `SCREEN_MODE_SAVED`：
    設定アプリが起動時に保存ファイルから戻していた）は割り当てない。
    モードが 0 件なら `TRY`・`KEEP`・`BACK` は `N_ERR_NOT_FOUND`。そのときも `monitor`・`name` は
    ファームウェアが映すモニターの EDID から入り、起動時の画面が表示中なら `shown` はファームウェアの
    モード（大きさと、EDID のその大きさのタイミングから求めたリフレッシュ。分からなければ 0）。
    `source` は `SCREEN_SOURCE_FIRMWARE`（ドライバーが一度もモードを出していない）、
    `SCREEN_SOURCE_DRIVER`（ドライバーの面が表示中。0 件ならモニターを見つけていない）、
    `SCREEN_SOURCE_HANDED_BACK`（モードを出したドライバーが claim を持ったまま、起動時の画面が
    表示中：画面を返した、または自分の面をまだ出していない。モードが出ていれば `TRY` で
    もう一度望める）、`SCREEN_SOURCE_DRIVER_STOPPED`（モードを出したドライバーの claim が
    終わった）、`SCREEN_SOURCE_REMOTE`（遠隔デスクトップの利用者の窓の大きさ）。`monitor` は
    保存用の識別子で、人に見せる名前ではない。
  - デスクトップの設定は `SCREEN_SETTINGS_GET`・`SCREEN_SETTINGS_SET`（`struct desktop_settings`）：
    ポインターの速さ（-2〜2）と、なめらかな階調（`smooth_gradients`：8 ビットの画面に、
    デスクトップをチャンネルあたり 10 ビットで合成してディザをかけて出す）。後者は利用者の望みで、
    表示中の出力が 8 ビットの xRGB（ドライバーの面を含む）の間だけ効く
    （`smooth_gradients_available`）。マウスの上下の向きはカーネルが自動で決め、設定は無い。
    どちらもデスクトップ（`gui/wm.c`）の持ち物で、変えるとすぐ効き、デスクトップが利用者の
    `AppData\Nocturne\Desktop.conf` に保つ（§12）。
  - 画面の見張り：`SYS_SCREEN_INFO` の第3引数 `SCREEN_WATCH`（第1・第2引数は 0。libc の
    `n_screen_watch()`、display.h）は、`SCREEN_SETTINGS_GET`・`struct screen_format` の返事・
    `SCREEN_MODES_GET` が報告するもののどれかが変わると受け取れる見張り（`N_KIND_WATCH`、§12）。
    デスクトップの大きさと設定、なめらかな階調を出せるか、申し出たモニターとモード、望まれた
    モード、試行の始まりと終わり、拒まれたこと、`source`、利用者の選択。試行の残り時間が減るのは
    変化ではない。第1・第2引数が 0 でなければ `N_ERR_INVALID`、デスクトップの無い起動（`nogui`）では
    `N_ERR_UNSUPPORTED`。
  - `n_pci_screen_vblank(pci, count, timestamp_ns)`：渡した面の垂直帰線を一つ知らせる
    （`count` はドライバーの帰線番号、`timestamp_ns` はドライバーの時計）。届いている間、
    デスクトップは帰線ごとに 1 枚を描き始める。100 ms 届かなければ、表示中のモードの
    リフレッシュ（ドライバーの面ならドライバーが知らせた `shown`、起動時の画面ならその
    ファームウェアのモード、分からなければ 60 Hz）の周期で刻む。どちらで刻んでいるかは `wm:` の
    行が言う。軽い呼出し：描く絵が帰線を待っているときだけ合成スレッドを起こす。画面がこの claim の面でなければ `N_ERR_DENIED`。
  - 直接表示：`n_pci_give_screen` の `format` に `N_SCANOUT_DIRECT` を足したドライバーは、
    全画面の窓の絵を合成を通さずに自分で映せる（デスクトップの合成はこれまでどおり渡した面へ
    続く。ドライバーは映す面を切り替えるだけ）。窓は `win_direct(win, true)` で望み、
    全画面・表示中・入力を持つ窓で、メニュー・音量・通知・大きさ変更の枠・遠隔の利用者が
    無い間だけ許される。許可の出入りは窓へ `EV_DIRECT`（key 1 許可、0 取消し）、今の状態は
    `win_state` の `WIN_STATE_DIRECT`。取り消された窓へは描き直しの損傷も送るので、
    窓は自分の通常の面へ描き直す。
  - `n_pci_screen_state(pci, seen, out, timeout_ms)`：画面をこの claim の面へ描かせているか、
    モードを出したドライバーが、出すべきモード（`mode`、`mode_serial`。上）と、直接表示を受け持つ
    なら許可された窓（`struct n_screen_state` の `app`。無ければ 0）と、その間ドライバーが
    描くポインター（位置・表示の有無・32×32 の ARGB 画像とその要点、画像が変わると
    `pointer_serial` が進む。画像は窓の管理が選んだもの：その形の絵か、窓が `SYS_WIN_POINTER`
    で渡した画像。§10）を読む。`serial` が `seen` と同じ間は `timeout_ms` まで待つ
    （0 は待たない。`N_ERR_TIMEOUT` でも今の状態を書く）。画面がこの claim の面でなく、
    モードも出していなければ `N_ERR_DENIED`。claim の終わりと起動時の
    画面への返却で直接表示は終わり、許可も取り消す。
  - `n_pci_screen_image(pci, surface, desc)`／`n_pci_screen_image_end(pci, image)`：直接表示を
    受け持つドライバーが、渡した面の代わりに表示エンジンへ出すことのある面（アプリの絵）を
    登録する。カーネルが止まると、表示エンジンは最後にフリップされた面を出し続け、渡した面へ
    戻すドライバーはもう動かない。そのためパニック画面は、渡した面と登録された面のすべてへ
    描く。システムがそこへ描くのはこの時だけ。`surface` と `desc` は `n_pci_give_screen` と
    同じ条件（`format` に `N_SCANOUT_DIRECT` は付けない。大きさは面自身のもので、表示エンジンが
    モードの大きさへ拡大縮小してよい。パニック画面はその大きさで描く）で、カーネルは同じく型を固定して
    写す。正の番号を返し、`n_pci_screen_image_end` にその番号を渡すと登録を外す（ドライバーは
    面の memory を解放する前に外す）。`N_SCANOUT_DIRECT` 無しで渡し直すこと、起動時の画面への
    返却、claim の終わりで、登録はすべて外れる。画面がこの claim の `N_SCANOUT_DIRECT` の面で
    ない、またはオブジェクトが `n_pci_give_screen` の受け付けないものなら `N_ERR_DENIED`、0 や
    65535 超の大きさ・未知の形式・面に収まらない範囲、または外す番号が無ければ `N_ERR_INVALID`。登録できない絵は表示エンジンへ
    出さないこと（パニック画面が見えなくなる）。
  - `n_pci_acpi_eval(pci, node, name, args, nargs, data, data_size, out, out_size)`：claim した
    機能に属する ACPI の名前を一つ評価する（カーネルの AML 解釈器で）。`node` は
    `N_ACPI_SELF`（その機能の装置：ホストブリッジの `_SEG`／`_BBN` から `_ADR` をたどる）、
    `N_ACPI_CHILD(i)`（その装置の i 番目の子装置）、`N_ACPI_UPSTREAM`（上流のブリッジ、
    ルートにある機能ならホストブリッジ）だけ。ほかの名前空間へは届かない。`name` は 4 文字の
    名前一つ（`_DSM`、`_ROM` など。3 文字の名前は `_ON_` のように `_` で埋める。上へ探しには
    行かない）。引数は 7 個まで：整数、`data`（64 KiB まで）の中を指すバッファーと文字列
    （`struct n_acpi_value` の `value` が `data` 内の位置、`length` が長さ。文字列の長さに
    終わりの NUL は含めず、AML へはカーネルが NUL を足して渡す）。結果は `out` へ、値の並び
    （幅優先。パッケージの `value` は最初の子の番号、`length` は子の数。文字列とバッファーの
    `value` は値の並びの後ろに続くバイトの位置）とその後のバイトで書き、書いたバイト数を返す
    （1 MiB まで）。下の断る方法は `N_ERR_DENIED`。`name` が 4 文字の名前でない、引数が多すぎる
    か `data` の外を指す、子の番号が最後の子より後（機能自体に名前空間の装置が無ければ子も
    無い）なら `N_ERR_INVALID`、AML が無ければ `N_ERR_UNSUPPORTED`、装置（`N_ACPI_SELF`、
    `N_ACPI_UPSTREAM`）や名前が無ければ `N_ERR_NOT_FOUND`、`out` が足りなければ
    `N_ERR_TOO_LARGE`、評価の失敗は `N_ERR_IO`。
    - 断る方法：システム自身が装置と機械を動かすための定義済みの方法。ドライバーが評価すると、
      カーネルが把握している装置の状態（dev/pm.c、dev/pci.c）が裏で変わり、上流のブリッジでは
      その下の全機能（ホストブリッジなら機械全体）を巻き込む。どの `node` でも、装置を探す前に
      断るので、名前空間の装置の無い機能でも答えは同じ。
      - 電源とリセット：`_PS0`〜`_PS3`、`_ON_`、`_OFF`、`_RST`。D 状態はカーネルのもの（PMCSR
        の D 状態も claim からは書けない）。電源資源はほかの装置と共有されることがある。
      - 取出し・固定・ドッキング：`_EJ0`〜`_EJ4`、`_LCK`、`_DCK`。
      - 資源：`_DIS`、`_SRS`。BAR と同じく、装置のデコードと割当てはカーネルのもの。
      - OS とファームウェアの取決め：`_OSC`、`_OST`。PCIe のホットプラグ・AER・PME などを
        どちらが受け持つかを決め直してしまう。
      - 起床：`_PSW`、`_DSW`。眠りに入る処理（dev/pm.c）の一部。
      - 初期化と処理：`_INI`、`_REG`、`_EVT`。名前空間の作成、領域の準備、GPIO の事象のときに
        カーネルの AML 処理が呼ぶもので、ドライバーが呼び直すとファームウェアの状態が崩れる。
      - システムの睡眠：`_PTS`、`_WAK`、`_TTS`、`_GTS`、`_BFS`。
      - `N_ACPI_UPSTREAM` では、`_` で始まらない名前（ファームウェア独自の方法）もすべて断る。
        ブリッジの独自の方法（スロットの電源やリンクを切るものなど）はその下の全機能に効き、
        ブリッジのレジスターと同じくプラットフォームのもの（ブリッジは claim できない）。
        ブリッジの `_DSM`（PCI Firmware 仕様の、下の装置のための関数）や `_PRT`、`_HID` などの
        照会はこれまでどおり評価できる。
    - 断る一覧にした理由：ドライバーとファームウェアの取決めには装置ごとの独自の名前（RM の
      `MXDS`／`MXDM` など）があり、許す一覧ではドライバーが増えるたびにカーネルを変えることに
      なる。危険な定義済みの方法は ACPI 仕様の定める有限の集まりで、増えるのは仕様の改訂の
      ときだけ。自分の装置とその子の独自の名前は、その装置とファームウェアの取決めとして
      ドライバーに任せる（RM の JT の `_DSM` と同じく、装置全体の電源に触れるものもある）。
      上流のブリッジの独自の名前はその下の全機能（ホストブリッジなら機械全体）に効くので、そこ
      では全部断る。RM（ports/nvidia/os_acpi.c）が使う、自分の装置と上流の `_DSM`、自分の装置の
      `_ROM`・`_DOD`、子の `_ADR`・`_DDC`・`MXDS`・`MXDM` はどれも断る方法に当たらない。
  - `n_pci_acpi_notify_take(pci, timeout_ms)`：その機能の装置へ届いた ACPI の Notify を一つ
    取り出し、値を返す（最初の呼出しで受取りを始め、claim の終わりまで続く。32 個まで溜め、
    溢れたら古いものから捨てる）。`timeout_ms` の間届かなければ `N_ERR_TIMEOUT`、装置が
    無ければ `N_ERR_NOT_FOUND`。
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
旧番号を使って作ったプログラムは作り直しが要る。カーネルが利用者の設定を持った a51c4398 の
203〜206（`SYS_USER_SETTINGS_WATCH`、`SYS_USER_SETTINGS_CHANGES`、`SYS_USER_LANGUAGES`、
`SYS_USER_LANGUAGES_SET`）とハンドルの種類 12（`N_KIND_USER_SETTINGS`）も、設定の持ち主の作り直し
（`docs/settings-ownership-20261027.md`：値を効かせる者が自分の文書に保存する）で欠番にした。振り分けと
宣言は消してあり（カーネルは `N_ERR_UNSUPPORTED` を返す）、番号は再利用しない。新たに加えたもの：

- `SYS_WIN_RECEIVE`（窓、`struct gui_event *out`、最大数、`timeout_ms`）：待ち行列から
  入るだけ一度に受け取る。件数、期限で0、閉じた窓が最後のイベントを渡し終えると
  `N_ERR_CLOSED`。窓はバイト列として読めない。
- `SYS_MEMORY_RESERVE`／`RELEASE`／`PROTECT`：`n_memory_*` そのもの。`access` が
  列挙外、または書込みと実行の同時指定は `N_ERR_INVALID`。
- `SYS_SHARED_MAP`（共有ハンドル、バイト数、`access`）：自分のハンドルだけ。
- `SYS_DMESG` は `SYS_SYSTEM_LOG`（同じ33番）。
- `SYS_ENTRY_INFO_SIZED`（パス、`struct n_entry *out`、`sizeof *out`）、`SYS_FOLDER_LIST_SIZED`
  （パス、`out`、最大数、`start`、`sizeof *out`。件数）、`SYS_DOCUMENT_INFO`（文書、`out`、
  `sizeof *out`）：§3 の長さを渡す `struct n_entry`。`SYS_ENTRY_INFO`（139）と `SYS_FOLDER_LIST`
  （143）は `id` の無い最初の形（`N_ENTRY_FIRST_BYTES`、280バイト）を書き続ける。
- `SYS_WATCH_TAKE`（207、見張り）：§1 の見張りの数の取り出し。`SYS_ENTRY_WATCH`（208、パス、
  `N_WATCH_NAME` か `N_WATCH_FOLDER`）：§3 の C: の見張り。`N_KIND_WATCH`（13）を返す。
- `SYS_WIN_POINTER`（80、窓、`const struct n_win_pointer *`、`sizeof *`。libc の
  `win_pointer`・`win_pointer_shape`）：窓が自分の client 領域の上に望むポインターを窓の管理へ
  伝える。ポインターの持ち主は窓の管理で、形の絵と要点（hot spot）を持ち、描き、何を見せるかを
  決める。窓は望みを言うだけで、次の呼出しまで続く（新しい窓は `N_POINTER_DEFAULT`）。見せるのは
  ポインターが client 領域の上にあり、窓の管理のもの（メニュー、枠、大きさを変える帯、窓のドラッグ）が
  そこに無い間と、client 領域で始めたドラッグの間。形 `shape` は CSS UI 4 の `cursor` の語と
  一対一の `N_POINTER_*`（`N_POINTER_DEFAULT` 矢印、`N_POINTER_NONE` 描かない、`CONTEXT_MENU`、
  `HELP`、`POINTER` 手、`PROGRESS`、`WAIT`、`CELL`、`CROSSHAIR`、`TEXT`、`VERTICAL_TEXT`、`ALIAS`、
  `COPY`、`MOVE`、`NO_DROP`、`NOT_ALLOWED`、`GRAB`、`GRABBING`、`E`・`N`・`NE`・`NW`・`S`・`SE`・`SW`・
  `W`・`EW`・`NS`・`NESW`・`NWSE` の `_RESIZE`、`COL_RESIZE`、`ROW_RESIZE`、`ALL_SCROLL`、`ZOOM_IN`、
  `ZOOM_OUT`。数は `N_POINTER_SHAPES`）。絵は窓の管理のもの。画像（頁の `cursor: url(…)`）は
  `width`・`height` を 1〜`N_POINTER_IMAGE_MAX`（＝`N_SCREEN_POINTER_SIZE`、32）、`pixels` に
  `width × height` の 0xAARRGGBB（乗算済みでない、上の行から）、要点 `hot_x < width`・
  `hot_y < height`。窓の管理は呼出しの時に写し取る。`width`・`height`・要点がどれも 0 なら
  画像なしで `shape` を見せる。画像があっても `shape` は正しい形でなければならない。上限は、ポインターを見せるどの道
  （合成、直接表示のドライバーのポインター、遠隔デスクトップのポインター）でも見せられる大きさで、
  それより大きい画像を断るのは呼ぶ側（頁なら次の候補へ）。大きさの引数が違う・形が範囲外・
  `reserved` が 0 でない・画像の大きさや要点が範囲外は `N_ERR_INVALID`、読めない番地は
  `N_ERR_BAD_ADDRESS`、ほかの app の窓は `N_ERR_DENIED`。

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
| `'F'` | u32 窓 | 何も変わらなくても、表示の次のフレームで FRAME を送る（`win_frame`） |
| `'M'` | u32 要求番号、f64 大きさ、u32 書体（ビット 1 太字、2 等幅）、str 文字列 | 文字列の寸法を測る |
| `'Q'` | なし | 配色と寸法を問い合わせる |
| `'S'` | u32 サーフェス、u32 幅、u32 高さ、［u32 flags］ | サーフェスを作る／大きさを変える |
| `'P'` | u32 サーフェス、［u32 x、y、幅、高さ］ | その画素（矩形を付ければその部分だけ）が変わった |
| `'U'` | u32 サーフェス、u32 形、［u32 幅、u32 高さ、u32 要点 x、u32 要点 y、幅×高さ個の u32 画素］ | `pointer` 付きで置いたこのサーフェスの上のポインター |
| `'D'` | u32 サーフェス | 捨てる |

窓の flags は `UI_WINDOW_FLAGS` の範囲だけ：`WIN_RESIZABLE`、`WIN_CENTER`、
`WIN_START_MAXIMIZED`、`WIN_ALWAYS_ON_TOP`、`WIN_DOUBLE_BUFFER`（各画面は空きバッファーを
待ち、全体を描く。表示に合わせて間隔が決まる）、`WIN_DEEP_COLOR`。サーフェスの flags は
`UI_SURFACE_DEEP`（1）だけで、画素の後ろに1画素1バイトの深い面（`win_deep` と同じ並び）を
持つ。10bpc の画面で `WIN_DEEP_COLOR` の窓に置くと、深い面も画面へ写る。範囲外のビットは
不正なメッセージ。

窓のポインターはエンジンが持つ（`win_pointer`、§10 の `SYS_WIN_POINTER`）。入力欄の上（そこで
選択をドラッグしている間も）は文字の縦棒（`N_POINTER_TEXT`）、`pointer` 付きのサーフェスの上と、
そこで押して離すまでは、アプリが `'U'`（C は `ui_surface_pointer`）で言ったもの、ほか（ボタン、
一覧、メニューの出ている間など）は矢印。変わった時だけデスクトップへ頼む。`'U'` の形は
`N_POINTER_*`、画像は 1〜`N_POINTER_IMAGE_MAX` 四方の 0xAARRGGBB（乗算済みでない）で、要点は
画像の中、形は画像を見せられない所の代わり。範囲外は不正なメッセージ。サーフェスを作り直しても
（`'S'` の大きさの変更）続き、次の `'U'` まで続く。作ったばかりのサーフェスは矢印。

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
`progress` `segmented` `field` `item` `icon` `divider` `spacer` `canvas` `surface` `tabs`
`tab` `menu`。

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
  `disabled`。どの箱も `clickable` で押せる。押すのは左ボタンだけで、`buttons`（2 右、4 中、
  6 両方）を付けた部品はそのボタンの押下と離しでも CLICK になる（付けない部品は今までどおり
  左だけ）。
- `toggle`／`checkbox`：`on`、`text`。`slider`／`progress`：`value`、`min`、`max`、
  `step`、`wheelStep`、`length`。`segmented`：`options`（型4）、`selected`。
- `field`：`value`（文字列）、`placeholder`、`secret`。`item`：`text`、`icon`、`selected`。
- `tabs`：子の `tab` の帯と、その後の「+」。`tabWidth`（一つのタブの幅の上限、既定 240）、
  `tabMinWidth`（これより狭くなると帯が横に送られる。既定 100。送りは両端の矢印とホイール）、
  `add`（0 で「+」を出さない）、`background`（帯）、`selectedBackground`（選んだタブ）、
  `hoverBackground`。タブは帯の幅を等しく分ける。
- `tab`：`text`（題名。入らなければ「…」）、`icon`、`loading`（1：アイコンの代わりに回る印）、
  `selected`（1：選ばれたタブ）、`key`（並べ替えても同じタブだと分かるように、どのタブにも付ける）。
  各タブに閉じるボタン（×）がある（狭いタブでは選んだタブだけ）。
- `menu`：どの部品の子にも置ける浮いた一覧。ほかの子と一緒には配置されず、その部品（錨）の下に
  （入らなければ上に）窓の上へ重ねて出る。`edge`（`"start"` 既定：左端をそろえる、`"center"`、
  `"end"`：右端をそろえる）、`at`（型3の［x、y］：錨の中のその点に出す。右ボタンの一覧など）、
  `minWidth`（既定 220）、`maxHeight`（超えるとスクロール）、`padding`、`background`、`border`、
  `radius`。子は `item`（`text`、`icon`、`shortcut`：右に出すキーの文字、`disabled`、子の `menu`：
  その下の一覧）、`divider`、ほかの部品。
- `icon`：`display` `mouse` `sound` `language` `info` `check` `plus` `minus` `close`
  `chevron-right` `chevron-left` `chevron-up` `chevron-down` `settings` `folder` `clock` `refresh`
  `backspace` `home` `search` `menu`（三本線）`lock`（錠前）`trash`（ごみ箱）。
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
| 1 | CLICK | ボタン、一覧の項目、`clickable` の箱、一覧（`menu`）の項目。押したボタンは `(flags >> 8) & 0xFF`（`UI_BUTTONS`。1 左、2 右、4 中。一覧の項目はどのボタンでも選ばれ、キーで選んだ時は 0）、a、b＝その部品の中のポインターの位置 |
| 2 | TOGGLE | a＝新しい状態（0/1） |
| 3 | SLIDE | a＝値。ドラッグが終わると flags に `UI_DONE`（1） |
| 4 | CHOOSE | a＝選んだ項目の番号。`tab` では、選ばれていないタブが押された（a＝帯の中の番号） |
| 5 | TEXT | 文字列＝入力欄が今持つべき値（次の木で `value` として送り返す） |
| 6 | SUBMIT | 入力欄で Enter。文字列＝その値 |
| 7 | KEY | 部品が使わなかったキー。文字列＝`"Escape"` `"Enter"` `"ArrowUp"` … `"F12"` か文字、a＝NKEY_ の番号か文字、離した時は flags に `UI_UP`（8） |
| 8 | CLOSE | 閉じるボタン（`UI_GONE`（2）：窓は既に無い） |
| 9 | CANVAS | `canvas`／`surface` が作った大きさと違う大きさに配置された。a、b＝その大きさ |
| 10 | RESIZE | a、b＝窓の新しい大きさ |
| 11 | POINTER | `pointer` 付きの上で：a、b＝その中の位置。flags に `UI_DOWN`（4）か `UI_UP`（8）（どちらも無ければ移動）、押しているボタンは `(flags >> 8) & 0xFF`（1 左、2 右、4 中）。押した後は全部離すまでその部品が受け取り、外へ出ると外の位置で移動を一つ送る |
| 12 | WHEEL | `pointer` 付きの上で：a＝刻み数（手前が正） |
| 13 | FOCUS | 入力欄がキーボードを得た（a＝1）／失った（a＝0。Escape で手放した時は flags に `UI_CANCEL`（16））。場所が窓（0xFFFFFFFF）なら窓のこと |
| 14 | DIRECT | 場所は窓（0xFFFFFFFF）。`win_direct` を望んだ窓の直接表示が始まった（a＝1）／終わった（a＝0）（`EV_DIRECT`）。終わるとエンジンは窓を全体から描き直す |
| 15 | FRAME | 場所は窓（0xFFFFFFFF）。`'P'` で見せた絵を画面のフレームが取り込んだ、または `'F'` の後の次のフレームが来た。間に何度変えても頼んでも一つ。a は表示のフレームの周期（ms）で、次のフレームはおよそその後に来る。表示の歩調（垂直帰線、無ければモードのリフレッシュ）で来るので、これを受けて次の絵を描くアニメーションはどのリフレッシュでもフレームごとに一枚描く。窓が表示されていない間は、表示されるまで待つ |
| 16 | DISMISS | `tab`：閉じるボタンか中ボタンで閉じてほしい（a＝帯の中の番号）。`menu`：次の木から外してほしい。項目を選んだ（flags に `UI_DONE`。項目の CLICK の後）、Escape（`UI_CANCEL`）、一覧の外を押した・窓がキーボードを失った（0） |
| 17 | ADD | `tabs`：「+」が押された（新しいタブ） |
| 18 | MOVE | `tab`：帯の上でドラッグされ、別の場所で離された。a＝今の番号、b＝移る先の番号。次の木が新しい順で来るまで、エンジンはその順で見せる |

入力欄はカーソルと選択範囲をエンジン側で持つ（←→、Home／End、Shift で選択、Ctrl で単語
単位、Ctrl+A／C／X／V、クリックとドラッグ）。入力欄が使わないキーは KEY としてアプリへ
届き、その離鍵も届く。アプリがまだ送り返していない値の上に次のキーを積むので、速い入力が
取りこぼされない。

値が前の木から変わった時だけ働くプロパティがある（もう一度頼む時は数を増やす）：

- `focus`（入力欄）：正の数ならキーボードを取る（カーソルは末尾）。0以下なら手放す。
- `reveal`（どの部品にも）：入っているスクロール箱を、それが見える位置までスクロールする。

`selectOnFocus`（入力欄）は、クリックや `focus` でキーボードを得た時に全選択する。キーボードを持つ入力欄の
`value` をアプリが変えると、カーソルは元の位置に残る。ただし前の値が全部選ばれていたときは、新しい値を全部選ぶ。`ellipsis`（文字列）は
一行に収め、入らない部分を「…」で切る。`mono`（文字列、入力欄）は等幅の Maple Mono で描く。
フォーカスのある入力欄が木から消えると、
フォーカスも消える。

`menu` は木にある間だけ出ていて、エンジンは自分では消さない：DISMISS を受けたアプリが次の木から
外す。出ている間はポインターと、↑↓、Home／End、Enter／Space、→（その下の一覧を開く）、←（閉じる）、
Escape を一覧が取り、ほかのキーは KEY としてアプリへ届く。項目に止まると 200 ms でその下の一覧が
開く。一覧はほかのどの部品より上に描かれ、`'P'` の直接の写し（`surface`）とも重なれば写さずに
描き直すので、消されない。一覧を加えると、その後のノードの場所（前順の番号）がずれる。

タブのドラッグは 6 ピクセル動いてから始まり、帯の端の外へ出ると帯が送られる。離すと MOVE が来て、
アプリが新しい順の木を送るまで、エンジンはその順で見せる（タブには `key` が要る）。

## 12. システムの状態の保存と見張り

値を効かせる者がその値の持ち主で、持ち主が自分の文書に保存し、自分で読み戻す。全部の設定を
並べた表、設定のビット、全プログラムが共有する設定の文書はどこにも無い。設定アプリ
（`C:\Nocturne\System\settings.js`）とタスクバーは持ち主へ変更を頼む操作盤で、値も文書も持たない。
端末やコマンドで状態を当てる道は無い。

- プログラムの設定はプログラムの持ち物：`n_app_data_folder(name, out, cap)` の `AppData\<name>`
  （§3）に自分の形式で置き、自分で読む。原子的な保存は `n_document_save`・`n_entry_replace`、
  ほかのプロセスの変更は C: の見張り（`n_entry_watch`、§3）で知る。カーネルも共有の文書も
  通らない（移植したプログラムは自分の profile をそのまま使える）。
- システムの部品の状態（今はどれもカーネルの中）。文書は利用者の `AppData\Nocturne` にある：
  - ポインターの速さとなめらかな階調：デスクトップ（`gui/wm.c`、`SCREEN_SETTINGS_SET`、§9）→
    `Desktop.conf`（`pointer_speed`、`smooth_gradients`）。
  - 音量と消音：音（`dev/audio.c`、`n_audio_output` とタスクバーのスピーカー、§9）→ `Sound.conf`
    （`volume`、`muted`）。
  - モニターごとに選んだモード：画面のモード（`gui/screenmode.c`、`SCREEN_MODE_KEEP`、§9）→
    `Displays.conf`（`mode.<monitor>`）。
- 部品の文書は**インターフェースではない**：形（`キー=値` の行、`#` で始まる行は注釈）も鍵も
  持ち主が決め、版とともに変わりうる。書くのはその持ち主だけ。プログラムは値を §9 の呼出しで
  読み・頼み、見張りで変化を知る。文書を手で直しても、動いている系には効かない（次の起動で
  持ち主が読み、分からない行・範囲外の値は捨てる）。
- 保存：持ち主は値が変わると保存の印を付け、保存の共通の仕組み（`sys/keep.c`。鍵を一つも
  知らない）が最後の変化から 0.5 秒後に持ち主の文書全体を差し替える（`store_save`）。電源を
  切る前、再起動の前、スリープと休止の前には待たずに書く。書けなかった変化は次の変化か電源の前に
  書き直す。
- 起動：各持ち主が登録し、C: と利用者のフォルダーが使えるようになった後（`store_start` と
  `wm_init` の後）、init より前に、各文書を読んで持ち主へ渡す。文書の無い持ち主は既定の値の
  まま。読み戻しと引き継ぎは `keep:` の行でシステムログに書く。
- 以前の系からの引き継ぎ：自分の文書が無い持ち主だけが、以前の系の文書を新しい方から一つ
  （a51c4398 の `AppData\Nocturne\Settings.conf`、無ければ以前の設定アプリの
  `AppData\Settings\desktop.conf`）読んで自分の値にし、すぐ自分の文書を書く。文書を持つ持ち主の
  値は以前の文書に上書きされない。以前の文書は、全部の行がどれかの持ち主のもので、読んだ
  持ち主が全部保存できたときだけ消す（`languages=` の行がある `Settings.conf` は残る）。
- 見張り（`N_KIND_WATCH`、§1）：見張るものが変わると `N_EVENT_RECEIVABLE`（レベル：
  `n_watch_take(watch)` が取るまで）。`n_watch_take` は前の取り出しの後の変化の数を返し（無ければ
  0、上限 `N_WATCH_CHANGES_MAX`）、数を消す。見張りは開いた時から数える。誰が変えても（その
  プログラム自身、ほかのプログラム、タスクバー、遠隔デスクトップの利用者）知らせ、同じ値を
  当て直すのは変化ではない。待ち行列（§6）に入れて待つ。窓の無い Worker やサービスでも使える。
  見張りを閉じれば、入っていた待ち行列からも消える。開く口は用途ごと：
  - `n_audio_output_watch()`（`SYS_AUDIO_OUTPUT_WATCH`）：`n_audio_output` が返すもの。
  - `n_screen_watch()`（`SYS_SCREEN_INFO` の `SCREEN_WATCH`、display.h）：`SCREEN_SETTINGS_GET`・
    `struct screen_format` の返事・`SCREEN_MODES_GET` が報告するもの。
  - `n_entry_watch(path, what)`（`SYS_ENTRY_WATCH`、§3）：C: の名前とフォルダー。
- 言語の一覧はシステムの状態ではない：読むプログラム（ブラウザー）の好みで、その持ち物である。
  a51c4398 の `SYS_USER_SETTINGS_WATCH`・`SYS_USER_SETTINGS_CHANGES`・`SYS_USER_LANGUAGES`・
  `SYS_USER_LANGUAGES_SET`（203〜206）はカーネルが受け付けず（`N_ERR_UNSUPPORTED`）、欠番のまま
  使い直さない（§10）。
- nocturne/ui（TypeScript・JavaScript）では `desktop.watch(listener)`：`listener` は変わったものの
  名前の配列を受け取る。`"screen"` は `desktop.get()`・`desktop.modes()` が報告するもの（画面の
  見張り）、`"sound"` は `desktop.sound()` が報告するもの（音の見張り）。`ScreenModes.chosen` が
  モニターに選んだモード。
