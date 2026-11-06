# Nocturne ネイティブABI

Nocturne のカーネルとアプリの間の約束。番号と型は `common/abi.h`、画面と GPU は
`common/display_abi.h` と `common/gpu_abi.h` にある。

ISO Cの標準ライブラリ（`stdio.h`、`stdlib.h`、`string.h`、`math.h`、`time.h`、
`threads.h`、`fenv.h`、`setjmp.h`、`errno.h`）は C言語の一部として、このABIの上に実装する。
`errno.h` は ISO C++ の `<cerrno>` の名（`ENOENT` など）も持つ。そのほかに `string.h` は
`strnlen`・`strlcpy`・`strlcat`・`strcasecmp`・`strncasecmp`・`strtok_r` を、`math.h` は
`M_PI` などの定数を持つ。`strings.h`、`fcntl.h`、`unistd.h`、`sys/*.h`、`alloca.h` は無い。
移植物（ffmpeg、QuickJS、Mesa、tcc、lexbor、FreeType、wabt、BearSSL、libc++）は
互換ヘッダーを置かず、各ソースを直接このABIへ書き換える。

## 0. 呼出しの形

- カーネルへ入る道は入口の像だけ。プログラムは `SYSCALL` を書かない（`tests/check_boundaries.py` が見る）。
  カーネルは全プロセスに読取り専用の像を一つ写す。frame は全プロセスで共有し、番地はプロセスごとに違う。
  番地は `struct n_start` と `struct n_thread_block` の `entry` が教える。
- 番号 n の呼出しは `entry + n × N_ENTRY_SLOT_BYTES`（16 バイト）の関数で、C の呼び方で呼ぶ。引数は
  `rdi`・`rsi`・`rdx`・`rcx`・`r8`、結果は `rax`（負の値は §2 の `N_ERR_*`）。libc の `__syscall` がこの形で、
  `n_entry_slot(n)` が番地を返す。`rcx` と `r11` の戻ったときの中身は決まらない。
  `int $0x80` の入口は無い（一般保護例外でアプリが終わる）。
- カーネルは、`SYSCALL` がその番号の溝から来たことを確かめる。違えばプロセスを結果 `128 + 13` で終わらせ、
  serial に `[crash]` の行を書く。
- `C:\Nocturne` の中のプログラムの像では、全部の溝がカーネルへ入る。ほかのプログラムの像では、`abi.h` の
  `N_PUBLIC_CALLS` の溝だけが入り、ほかは `-N_ERR_DENIED` を返す。表は面の広さを決めるだけで、呼べるかどうかの
  本当の検査は能力と身元で行う（§13）。
- 溝のうち `SYS_MAX` から上はカーネルへ入らない像の中のコードで、今は時計（§8）。
- 刻印：プログラムと像は ELF の PT_NOTE に、組んだときの `N_ABI_GENERATION` と `N_ENTRY_SLOTS` を持つ（`struct
  n_program_note`。`user/libc/sys.c` が全プログラムに入れ、Go はリンカーが入れる）。世代が違うか、溝がカーネルより
  多ければ、起こさずに `N_ERR_WRONG_GENERATION` を返す。起こした側が理由を言う。
- スレッドの FS と GS の基底はアプリのもの：`WRFSBASE`・`WRGSBASE` で直に書き、
  `RDFSBASE`・`RDGSBASE` で読む。カーネルはスレッドごとに保ち、切り替えのたびに戻す。
  libc は FS を自分の TLS（x86-64 の variant II）に使う。`%fs:0` は `struct n_thread_block`（`abi.h`）で、
  頭の語が自分の番地、次の語が像の番地。ドライバーの像（§7）の libc は FS を変えず、同じスレッドの区画に
  自分の区画を繋ぐ。Go は FS を自分で持ち、溝を番地で呼ぶ。
- `RDTSCP` は ECX に、そのスレッドが今走っている CPU の番号（0 から数える）を返す。CPU が `RDPID` を持つなら、それでも同じ値を読める。
  読んだ直後に別の CPU へ移ることがあるので、目安にだけ使う（libc の `cpu_index`）。

## 1. ハンドル

- カーネル資源はすべて**種類つきハンドル**（`n_handle`、非負の `int32_t`）で表す。
- 種類：`N_KIND_DOCUMENT`、`N_KIND_FOLDER`、`N_KIND_CHANNEL`、`N_KIND_WINDOW`、
  `N_KIND_SHARED`、`N_KIND_APP`、`N_KIND_EVENTS`、`N_KIND_DEVICE`（開いた音の出口。§17）、
  `N_KIND_CONNECTION`（TCP／UDP）、`N_KIND_HAL_CLAIM`、`N_KIND_PAGER`、`N_KIND_WATCH`（見張り。下）、
  `N_KIND_CAPABILITY`（能力。§13）、`N_KIND_CONSOLE`（コンソール。§4）。12 は欠番（§10）。
- 汎用操作は `n_handle_close(h)` と `n_handle_kind(h)` だけ。読み書きは種類ごとの
  専用関数で行い、種類の違うハンドルを渡すと `N_ERR_WRONG_KIND`。
- 見張り（`N_KIND_WATCH`）は、見ている物が変わったことを知らせるハンドル。開く口は見る物ごとに
  あり（C: の `n_entry_watch`（§3）、画面の `n_screen_watch`）、
  取り出しは共通の `n_watch_take(h)`（`SYS_WATCH_TAKE`）一つ。
  - 変化があると `N_EVENT_RECEIVABLE` になり、`n_watch_take` が数を取るまでそのまま（レベル）。
    `n_watch_take` は前の取り出しの後に来た変化の数を返す（無ければ 0）。数えるのはハンドルごとに
    開いた時からで、同じ物を見るほかのハンドルの取り出しは関わらない。`N_WATCH_CHANGES_MAX` で
    止まり、溢れない。
  - 見ている物が無くなると `N_EVENT_CLOSED`。取り出しはまず残っていた数を返し、その後は
    `N_ERR_CLOSED`。
  - 見張りでないハンドルの取り出しは `N_ERR_WRONG_KIND`。見張りはイベント待ち行列（§6）に入れて待つ。
    中身（値や内容）は運ばず、知らせを受けた側が持ち主の読み出し（`n_entry_info`、`n_document_load`、
    `n_screen_info` など）で読み直す。
- ハンドルの複製と、番号を指定した付け替えはない。子アプリへ渡す資源は
  起動時の引渡し表（§5）で明示する。

## 2. 結果コード

- 失敗は `errno` ではなく負の `n_result` を直接返す。
  `N_ERR_NOT_FOUND`、`N_ERR_EXISTS`、`N_ERR_DENIED`、`N_ERR_NO_MEMORY`、
  `N_ERR_INVALID`、`N_ERR_WRONG_KIND`、`N_ERR_BUSY`、`N_ERR_NOT_READY`、
  `N_ERR_CLOSED`、`N_ERR_TIMEOUT`、`N_ERR_CANCELLED`、`N_ERR_TOO_LARGE`、
  `N_ERR_NOT_EMPTY`、`N_ERR_IO`、`N_ERR_UNSUPPORTED`、`N_ERR_WRONG_GENERATION`（§0）。
- `n_result_text(r)` が表示用の文を返す。ISO Cの関数が失敗理由を残す必要がある場合、
  libcは `errno` に `n_result` の正の値を入れる。ISO C++ の `<cerrno>` の名は同じ値を指す。
- `nocturne.h`／`gpu.h`／`display.h` のlibc関数も同じで、`errno` は触らない：
  ネットワーク（`net_*`、`udp_*`、`tcp_*`。ソケットは `n_handle`）、窓と画面
  （`win_*`、`screen_*`、`clipboard_*`、`gui_launch`）、GPU（`gpu_*`、`gfx_render3d*`）、
  `proclist`、`sysinfo`、`thread_stack_bounds`、`poweroff(cap)`／`reboot(cap)`（`N_CAP_POWER`）。何かを作って渡す関数は
  出力引数に置いて `n_result` を返す（`win_open(w, h, title, flags, &win)`、
  `n_memory_reserve`。失敗では出力は NULL）。

## 3. 文書とフォルダー

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
  `N_ERR_INVALID`。シェルは自分の「現在のフォルダー」を持ち、起動するアプリへ
  `struct n_launch` の `folder` で渡す（§5）。
- libc のパスの関数（`nocturne.h`）：
  - `n_path_is_complete(path)`：ドライブの後（無ければ先頭）に区切りがあるか。`C:\…`、`\…`、
    `/…` は完全。`C:` と `C:a` は現在のフォルダーとその中の `a`。
  - `n_path_join(folder, name, out, cap)`：`name` を完全なフォルダー `folder`（完全でなければ
    `C:\`）の中で取る。完全な `name` はそれだけで決まる。結果は常に `C:\a\b` の形
    （大文字のドライブ、`\` 区切り、`.`／`..` 解決済み）。長さを返し、収まらなければ
    `N_ERR_TOO_LARGE`。
  - 利用者が打った名前は `n_path_complete(name, out, cap)` で完全指定にする：今のフォルダー
    （`n_folder()`）の中で `n_path_join` する。ISO Cの `fopen`／`remove`／
    `rename` もこれを使う。
  - `n_path_name(path)`：最後の名前（根では `""`）。
  - `n_path_from_url(url, out, cap)`／`n_path_to_url(path, out, cap)`：`file:` URL と
    パスの変換（`file:///C:/Users/User/a%20b.html` ⇔ `C:\Users\User\a b.html`。ドライブの
    無い `file:///Users/User` も C:）。
- アプリが自分の現在のフォルダーを変えるのは `n_folder_set(name)`：名前を完全指定にし、
  フォルダーであることを確かめてから（違えば `N_ERR_NOT_FOLDER`）、今のフォルダーを
  置き換える（以後の `n_path_complete` と、libc が起動するアプリへ渡す `folder` に効く）。
- C: の構成（`common/abi.h` の `N_FOLDER_*`）：
  - `C:\Nocturne`：システム。`System`（システムのプログラム、`N_FOLDER_SYSTEM`）、`Fonts`、
    `Licenses`、`SDK`、`Certificates`、`Web`、`Agent`、`Firmware` 等。`C:\Nocturne\Temp` は
    カーネルだけが使う（差し替え中の内容）。
  - `C:\Programs`：アプリ（`N_FOLDER_PROGRAMS`）。アプリごとに一つのフォルダー。
  - `C:\Users\<名前>`：利用者のフォルダー（`n_user_folder()`、身元から `accounts` が引く。§15）。アプリは
    ここで始まる。`Desktop`、`Documents`、`Downloads`、`Music`、`Pictures`、`Videos`、
    `AppData`。アプリのデータは `n_app_data_folder(name, out, cap)` が作って返す
    `AppData\<name>`。`AppData\Temp` は一時フォルダー（`n_temp_folder()`）で、
    起動のたびに空になる。
- パスの権限は `kernel/src/fs/store.c` の入口の一か所で決まる。決めるのは呼ぶ側の身元（§13）で、
  パスの文字列でなく項目の番号で比べる。誤りは `N_ERR_DENIED`。

  | 場所 | 読む | 書く |
  |---|---|---|
  | `C:\Nocturne` の下 | 誰でも | `N_CLASS_SYSTEM` |
  | `C:\Programs` の下 | 誰でも | `N_CLASS_ADMIN` 以上 |
  | `C:\Users\<名前>` の下 | 持ち主と `N_CLASS_ADMIN` 以上 | 持ち主と `N_CLASS_ADMIN` 以上 |
  | その他（根の直下の項目） | 誰でも | `N_CLASS_ADMIN` 以上 |

  `C:\Nocturne`、`C:\Programs`、`C:\Users` の三つのフォルダー自身を消す・移すのは
  `N_CLASS_SYSTEM` だけ。`C:\Users` の直下を一覧するのは誰でも、そこに作るのは `N_CLASS_ADMIN` 以上。
- 項目は持ち主の利用者の番号を持つ（`struct n_entry` の `owner`。`STORE_OWNER_SYSTEM` は 0）。
  作った項目は親のフォルダーの持ち主を継ぎ、移すと行き先の持ち主になる。
  `n_entry_set_owner(path, user)`（`SYS_ENTRY_SET_OWNER`）が変える。`N_CLASS_ADMIN` 以上。
- 私的な領域：名前の木の外の文書。パスは無く、`n_entry_info`・`n_folder_list`・`n_entry_watch` に
  現れない。`SYS_STORE_PRIVATE_OPEN(cap, region, user, access)` が番号で開く。
  - `cap` は `N_CAP_STORE_PRIVATE` の能力（§13）。無ければ `N_ERR_DENIED`。
  - `region` は領域の番号（`enum n_private_region`、`common/settings_abi.h`：`N_REGION_SETTINGS`、
    `N_REGION_USERS`、`N_REGION_SECRETS`。`N_REGION_FIRST_FREE` から上はシステムの部品が割り当てる）。
  - `user` は 0 なら機械の領域、それ以外はその利用者の領域。
  - `access` は `n_document_open` と同じ。0 は `N_DOCUMENT_EDIT`。無い領域は `EDIT`・`REPLACE` が作る。
  - 開いた文書は普通の文書で、`N_DOCUMENT_REPLACE` と保存で原子的に差し替わる。読みは
    `READ` のハンドルが保存された版を読む。
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
  その秒の中のナノ秒 `modified_ns`）、`id`、持ち主 `owner` を持つ。`id` は C: 上の項目そのものの番号で、
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
    フォルダー（`C:\Nocturne\Temp`）は見張れない（`N_ERR_NOT_FOUND`）。読めないフォルダーは
    `N_ERR_DENIED`。
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

- 文書を選ぶ窓：`n_document_ask_open(start, out, n)`、`n_document_ask_save(start, out, n)`（`nocturne_services.h`）。
  `start` は最初に見せるフォルダーか、保存のときは提案の名を含むパス。結果は完全なパス一通で、取り消しは
  `-N_ERR_CANCELLED`。libc は呼ぶ者の身元で `C:\Nocturne\System\files` を子として起こし、channel の片端を
  handle `N_DOCUMENT_WINDOW_HANDLE`（3）で渡す。文は `struct n_document_window_request` と、パスの一通。窓は
  サービスではない：宣言のサービスは利用者 0 の身元で動き、他の利用者のファイルを見せてしまう。結果を handle に
  しないのは、`n_document_save` がパスで atomic に置き換えるから。

## 4. チャンネル

- チャンネルは**メッセージ単位**の双方向路。一回の送信が一つのメッセージになり、
  受信側は境界を保ったまま受け取る。大きな塊は共有ページを添付して送る。
- `n_channel_pair(n_handle out[2])`：無名の対。
- 受け口：各メッセージが新しいチャンネルの端を一つ運ぶチャンネル。`n_inlet_connect(inlet)` は
  対を作って一方の端を受け口へ送り、もう一方を返す。`n_inlet_accept(inlet, flags)` は次の端を
  受ける（無ければ `N_ERR_NOT_READY`、`N_CHANNEL_WAIT` で待つ。送る側が全部閉じれば `N_ERR_CLOSED`）。
  受ける側が閉じると、待っている端も閉じる。
- 名前で相手を引くのはサービスだけ（§14）。カーネルにチャンネルの名前は無い。
- `n_channel_peer(h, &identity)`：もう一端の身元。カーネルが証明する（§13）。
- `n_channel_send(h, data, n, attach)`、`n_channel_receive(h, buf, cap, flags, &attached)`。
  - 流量制御：受け手が未だ受け取っていない量が1MiBに達している間、送信は待つ
    （`N_EVENT_SENDABLE` で送れることを知れる）。空の待ち行列へは大きさに関係なく送れる。
  - 送ったメッセージは受け取られるまでカーネルの記憶にあり、送ったアプリの分として数える。
    一つのアプリが全部のチャンネルに待たせているメッセージの合計は、送るときの空きメモリーの
    半分まで。超える送信は `N_ERR_NO_MEMORY` で、送られない。
  - `N_CHANNEL_WAIT` で待つ。待たない場合は `N_ERR_NOT_READY`。相手が閉じて
    残りも無ければ `N_ERR_CLOSED`。
  - `N_CHANNEL_PEEK` は次のメッセージの大きさだけを返し、取り出さない。
    `cap` より大きいメッセージは `N_ERR_TOO_LARGE` で、待ち行列に残る。長さ0の
    メッセージも一つのメッセージとして届く。
  - `N_CHANNEL_DISCARD` は次のメッセージの先頭 `cap` バイトだけを写してから捨て（添付も閉じる）、その大きさを
    返す。受け取る場所を確保できない大きなメッセージを断るため。`PEEK` とは併用できず、
    未知のフラグと共に `N_ERR_INVALID`。
  - `attach` は文書、共有ページ、能力、チャンネルの端のハンドル（無ければ -1）。ほかの種類は
    `N_ERR_WRONG_KIND`。自分の channel の端は運べず `N_ERR_BUSY`。メッセージと一緒に運ばれ、受信側で新しいハンドルとして `attached` に入る
    （無ければ -1）。送った側のハンドルは開いたまま残る。
  - チャンネルの端を運ぶとき、自分のチャンネルの端と、運ばれている途中の端へ送るものと、別の端を
    運んでいる端は `N_ERR_BUSY`。端が端を運ぶ輪はできない。
- アプリの入出力に渡される文字装置（コンソール、カーネルログ、null）にも
  `n_channel_send`／`receive` が使える。ただし境界と添付は無く、`N_CHANNEL_PEEK` は
  `N_ERR_UNSUPPORTED`。バイト列のパイプは無い。
- コンソール（`N_KIND_CONSOLE`）は、窓の管理が動かない機械のキーの押下を渡す。`n_channel_send` は文字を
  出す。`n_channel_receive` は `struct n_console_key`（`common/input_abi.h`：HID の usage、host が打った文字、
  配列の番号、修飾）を丸ごと受け取る。カーネルは文字を作らず、`cap` が一件より小さければ `N_ERR_TOO_LARGE`。
  打ったまま読まれていない押下は 256 件まで持ち、溢れた分は落とす。
- `n_console_receive(h,buf,cap,flags)`（libc）は同じ道を文字で読む。押下を `n_key_translate` で文字にし、
  Ctrl と英字は制御文字に、移動のキーは `ESC [ A` などの列にして返す。`stdin` はこの道で読む。
- `n_channel_receive_from(h,buf,cap,flags,&message)` はメッセージチャンネル専用。
  `n_channel_message` の `attached`、`reserved`（0）、`sender_app` を返す。
  `sender_app` はカーネルが実送信時に記録する `n_app_self()` と同じ識別子で、
  endpoint の作成者・接続者の固定値でも、本文で申告された番号でもない。
  同じアプリの全 thread と別接続で一致し、endpoint を起動先へ引き渡すと起動先の主体になる。
  識別子は 2^31 個を配り終えるまで再利用せず、送信者終了後の待ちメッセージにも値を保持する。
  `message` は必須。`PEEK` は主体を調べられるが添付を取り出さず、`DISCARD` は添付を閉じる。
  両者の `attached` は -1。旧 receive の番号と形式は変えない。
  添付の設置失敗や出力先の失効は負の結果を返し、設置済み添付も回収する。
  この段階の失敗は既に取り出したメッセージを待ち行列へ戻さない。失敗時の出力値は利用しない。
  接続単位の認証を必要とするサービスは、最初の主体へ bind し各 request の主体も照合する。
- ISO Cの入力は、`cap` より大きいメッセージを `N_CHANNEL_PEEK` で測り、バッファーを
  広げて丸ごと受け取る。長さ0のメッセージは読む内容が無いので次を待つ。
- 端末は、子アプリの「入力」「出力」チャンネルとメッセージをやり取りする一つの
  アプリにすぎない。起動元は、端末へつながるハンドル番号を `struct n_launch` の
  `terminals` に並べて伝える。
  `n_terminal(h)` がこれを読む。端末はキー入力をそのまま送るので、行の編集と表示は
  アプリ側（libcの `readline`）が行う。ISO Cの `stdout`／`stderr`／
  `stdin` は、起動時に渡された出力／入力チャンネル（無ければ記録先のログ）に結び付く。
  アプリが例外（ページフォルト等）で止められると、カーネルはその報告文を一つの
  メッセージとして記録先のログ（ハンドル2）へ送る。待ち行列が満杯なら待たずに捨てる。
- Ctrl+C：端末はシェルに「割込み」チャンネルを渡し（`struct n_launch` の
  `interrupts` がそのハンドル番号、`n_interrupts()` が読む）、Ctrl+C ごとにメッセージを一つ送る。シェルは前面のジョブを
  `n_app_stop(h, N_APP_STOP_TREE)` で止め、プロンプトでは入力中の行を取り消す。
- システムログ：`n_system_log_open()` は、送ったメッセージがシステムログ（カーネルの記録の輪。
  シリアルポートと、デスクトップが画面を取るまでの起動の画面が読む）へ加わるハンドルを返す。
  同じハンドルを `n_channel_receive` で読むと、ログの文字が来る：ハンドルごとに読んだ所を持ち、
  最初の受け取りの時に輪が持つ一番古い記録から始まる。各行の頭は `[秒.マイクロ秒 CPU:task] `
  （重さが情報でなければ task の後に ` E`・` W`・` D`）。読む前に上書きされた分は、その所に
  `[log: … bytes …]` の一行で言う。`N_CHANNEL_WAIT` は新しい記録が来るまで待つ。
  `n_system_log(buf, cap)` は最新のログを読む。C: への保存はサービス `systemlog` の仕事
  （`AppData\Nocturne\System.log`、前の起動の分は `System-previous.log`）。

## 5. アプリ

- `n_app_launch(path, const struct n_launch *)` がアプリハンドルを返す。
  `n_launch` は引数列、引き渡すハンドルの表（入力・出力・ログのチャンネル、共有ページ、能力等）、
  起動の属性、子の身元を持つ。起動の属性は型のある欄で、文字列の組ではない：
  - `folder`：今のフォルダー。
    `terminals`・`terminal_count`：端末へつながるハンドル番号。`interrupts`：Ctrl+C の知らせを受ける
    チャンネルのハンドル番号（0 は無し）。
  - カーネルはこれを新しいアプリの stack に写し、入口の第三引数 `const struct n_start *` で渡す。
    libc が `n_folder()`、`n_terminal(h)`、`n_interrupts()` で読ませる。
  - `environment`・`environment_count`：移植のプログラムのための `"名前=値"` の列
    （`=` が無ければ `N_ERR_INVALID`）。ISO C の `getenv` と `__libc_environment` だけが読む。
    システムの部品はこれを読まず、これで組まない。
  - `identity` が NULL なら、子の身元は親と同じ。`identity` を渡すときの規則は §13。
  - 引数と環境の文字列（各 nul を含む）と、二つの NULL 終端ポインター配列（1 個 8 バイト）は、
    合わせて `N_LAUNCH_MAX_BYTES`（4 MiB）まで。超えると `N_ERR_TOO_MANY_ARGUMENTS`。
  - 引き渡すハンドルは、表の順に子アプリの 0, 1, 2, … 番になる（-1 は空き）。0, 1, 2 番が空きなら、
    カーネルはそこへ何も読めず書いても捨てられる stream を置く。子が開くチャンネルや窓がその番号に来ない。
  - `path` は完全指定（§3）。名前だけのプログラムは libc の `find_program(name, out, n)` が
    `N_FOLDER_SYSTEM`、次に `N_FOLDER_PROGRAMS` の各アプリのフォルダーで探す。
    それ以外の名前は `n_path_complete` で完全指定にする。
  - 起動されたアプリは作業フォルダーを持たない。利用者の名前を受け取るアプリのために、
    libc の `n_app_launch` は `folder` が NULL なら起動元の今のフォルダーを入れる（§3 の `n_path_complete`）。
    起動元が持たなければ子も持たず、子は身元の利用者のフォルダーで始まる。
    子アプリは起動元の身元を継ぐ（§13）。ブラウザーの窓と、その Worker・保存の窓は同じ profile を使う。
- 終了はアプリハンドルのイベント（§6）で届く。`n_app_result(h, deadline, int *status)` で
  直接待つこともできる（期限は §8。呼出しの時点で過ぎた期限は待たずに `N_ERR_NOT_READY`、
  待って期限が来れば `N_ERR_TIMEOUT`）。最後のハンドルを閉じれば記録は消える。
- `n_app_stop(h, N_APP_STOP_TREE か 0)`：停止要求。
- スレッド：`thread_create(fn, arg)` は 256 KiB、`thread_create_sized(fn, arg, bytes)` は
  少なくとも `bytes` のスタック（ページは初めて触れたときに割り当てる）で `fn` を動かし、
  0 にならない識別子を返す。終わったスレッドは `thread_join` か `thread_detach` まで残る。
  `thread_detach` 後は終わり次第カーネルが解放し、join はできない（`N_ERR_INVALID`）。
  アプリの終了は全スレッドを止めて回収する。
- スレッドの級：`n_thread_schedule(tid, schedule, value)`（`SYS_THREAD_SCHEDULE`）。`tid` は同じ
  アプリのスレッド（0 は呼んだスレッド）。ほかのアプリのスレッド、終わったスレッドは
  `N_ERR_NOT_FOUND`。範囲の外の `schedule`・`value` は `N_ERR_INVALID`。
  - `N_SCHEDULE_FAIR`：`value` は重み（1〜`N_SCHEDULE_WEIGHT_DEFAULT`）。走りたいスレッドの間で、
    重みに比べた CPU の分を受ける。スレッドは初め、既定の重みでこの級にいる。アプリは既定より
    重くできない。
  - `N_SCHEDULE_DEADLINE`：`value` は周期の ns（`N_SCHEDULE_PERIOD_MIN_NS`〜
    `N_SCHEDULE_PERIOD_MAX_NS`）。装置の周期（画面のフレーム、音の塊）ごとに一つの仕事をする
    スレッドのため。起きたスレッドは、周期ごとにシステムが取り置く時間の内で、公平の級のどの
    スレッドより先に走る（締切の早いものから）。取り置きを使い切ると、次の周期まで公平の級として
    走る。取り置きは周期ごとに測った仕事の時間で、一つのアプリの締切の級のスレッドを合わせて
    一つの CPU の半分まで（等分）。だから一つのアプリが CPU を一つ占めることは無い。
    呼び直しは周期の残りの取り置きを満たさない。
  - ブラウザーは絵を作るスレッド（主ループ）を表示のフレームの周期で、Web Audio の描き手を
    描画の量子（128 フレーム）の周期でこの級に置く。
- `n_app_self()`、`n_app_parent()`：アプリ識別子（共有ページの受渡しの許可判定用）。
- `n_app_open(id)`：アプリ一覧の識別子から、動作中のアプリのハンドルを得る（タスク
  マネージャーや `kill` が `n_app_stop` するため）。触れてよい相手だけ（§13）で、ほかは
  `N_ERR_DENIED`。`n_app_id(h)`：ハンドルの識別子
  （終了後も同じ。識別子は 2^31 個を配り終えるまで再利用しない）。
- `n_app_list`、`n_system_info` 等の状態取得は現行の `proclist`（件数）／`sysinfo`（0）を
  引き継ぐ。`proclist` に出るのは核の thread、SYSTEM のアプリ、触れてよいアプリ（§13）。どちらも失敗では負の `n_result`。

## 6. イベント待ち行列

- `n_events_create()` が待ち行列を作る。各スレッドは任意の数を持てる。
- `n_events_watch(q, h, interest, token)`：ハンドルの関心事を登録。
  `N_EVENT_RECEIVABLE`（メッセージ、窓の入力、接続要求、受信データ）、`N_EVENT_SENDABLE`、
  `N_EVENT_CLOSED`、`N_EVENT_APP_ENDED`、`N_EVENT_SAVED`（`N_COMMIT_START` で始めた保存の
  結果が出た文書）。同じハンドルと同じ token の再登録は関心事の変更。
  待ち行列を待ち行列に入れることはできない（`N_ERR_WRONG_KIND`）。
- 報告は状態に基づく（レベル）。準備ができたままのハンドルは毎回の待ちで報告される。
- 監視はハンドルを保持しない。ハンドルが最後に閉じられると監視も消える。
- アドレス監視もこの待ち行列で行う：`n_events_watch_address(q, address,
  expected, token)`。値が既に違えば `N_ERR_NOT_READY`。通知されると `N_EVENT_ADDRESS`。
- タイマー：`n_events_timer(q, due, period, token)`。`due` は最初の時刻（§8 の期限）、
  `period` は繰返しの周期の ns（0 は一度だけ）。`N_EVENT_TIMER` で届く。繰返しのタイマーは
  遅れた分をまとめて報告せず、`due` から周期の格子を保つ。
- `n_events_unwatch(q, token)`：その token の監視・タイマー・アドレス監視を外す。
  0＝無かった、1＝外した、2＝アドレスの通知が既に届いていた（取り消した）。
- `n_events_wait(q, struct n_event *out, max, deadline)`：届いた分を一括で返す
  （一回に最大64件。残りは次の待ちで返る）。期限は §8（`N_CLOCK_NEVER` は無期限、過ぎた期限は
  待たない）。`struct n_event {token; events; reserved; time_ns;}` の `time_ns` は単調時計の ns：
  タイマーはその格子の上の時刻、ほかは事象をカーネルが知った時刻。

## 7. メモリ

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
- ドライバーの像：`n_image_map(document, &image)`（`SYS_IMAGE_MAP`、公開）。読むために開いた文書の handle から、
  番地 0 から組んだ ET_DYN の ELF を呼ぶ者へ写す。`image.base` が番地 0 の写った先、`image.entry` が入口。節は
  番地の順に並び、頁を共有せず、書込みと実行を同時に持たない。そうでなければ `N_ERR_NOT_PROGRAM`。刻印は
  プログラムと同じ（§0）。読取りだけの節は同じ文書を使う全プロセスで frame を共有し、最初に触れたときに読む。
  カーネルは再配置もコードの実行もしない：入口を呼ぶのは呼ぶ者で、像は自分で再配置する（`user/libc/driver_image.c`）。
  像はプロセスの終わりまで残り、その権限はプロセスと同じで、使える溝もプロセスと同じ。GPU のドライバーの表は
  `gpu_abi.h` の `struct n_gpu_driver`、読み込むのは libc の Vulkan ローダー（`nocturne_gpu.h`、§15）。
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
    deadline)` は待っている触れ方を `struct n_pager_fault {offset; access; app;}` で最大 `max`
    個返す（同じページ・同じ権利への触れ方は一つにまとまる。読取りは同じページで待っている
    書込みとは別の触れ方で、読取りでの結び付けでも答えが出る。`app` は最初に触れたアプリの識別子）。
    期限（§8）が来れば `N_ERR_TIMEOUT`。触れ方が待っている間、
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
    無いものは次のとおり：アプリの普通のメモリ
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
- アドレス待ち：`wait_on_address(addr, expected, deadline)`／`wait_on_address64` は起こされると0、
  語が `expected` でなければ `-N_ERR_NOT_READY`、期限（§8）で `-N_ERR_TIMEOUT`、アプリ停止中は
  `-N_ERR_STOPPED` を直接返す（`errno` は触らない）。`wake_address(addr, count)` は起こした数
  （`count == 0` は全員）。`N_CLOCK_NEVER` は無期限。共有ページ上でもアプリをまたいで働く。

## 8. 時間

- ABI が渡す時間は全部 ns。期限は単調時計の上の時刻（ns）で、`N_CLOCK_NEVER` は期限なし、
  呼出しの時点で過ぎた期限は待たずに見るだけ。`n_clock_after(ns)` が今から `ns` 後の期限を返す
  （桁あふれは `N_CLOCK_NEVER`）。
- 時計は三つ。どれもカーネルへ入らず、入口の像の溝（§0、`N_ENTRY_CLOCK_*`）が読む。
  - `n_clock_monotonic()`：TSC が数え始めてからの ns。機械が眠る間は止まる。期限の時計。
  - `n_clock_boot()`：単調時計に眠った時間を足したもの。
  - `n_clock_wall()`：壁時計（1970 年からの UTC の ns、`int64_t`）。時刻合わせの差は飛ばずに
    ゆっくり埋まる（slew）。
- 時計のコードとデータは像の中で、プログラムはデータの形を知らない（`kernel/entry/clock.c`、形は
  `common/clock_data.h`）。像は `seq` を読み（奇数の間はカーネルが書いている）、RDTSCP で TSC と CPU の番号
  （TSC_AUX、§0）を読み、欄とその CPU の offset を読み、`seq` を読み直す。違えば読み直す。
  monotonic = `base_ns + ((t - base_tsc) * mult >> shift)`（t は TSC から offset を引いたもの）。TSC は不変
  （invariant）であることを前提にする。
- `n_sleep_until(deadline)`（`SYS_SLEEP`、結果は無い）：期限まで眠る。ISO C の `thrd_sleep` は
  これの上に作る。
- `n_random(buf, n)`：システムのエントロピーから `n` バイト。
- `n_app_cpu_ns()`（`SYS_APP_CPU_TIME`、引数なし）：呼んだアプリの thread が走った時間の合計（ns）。
  終わった thread の分も入る（カーネルは thread の記録を手放すとき、その時間を process に足す）。
  開始から今までで、ほかのアプリや子のアプリの分は入らない。ISO C の `clock()` はこれの上に作る
  （`clock_t` は ns の数で、`CLOCKS_PER_SEC` は 1000000000）。
- `gettimeofday`、`struct timeval` は無い。ISO Cの `time`／`timespec_get`／`localtime`／`gmtime`
  （と C23 の `localtime_r`／`gmtime_r`）はこれらの上に実装する（`timespec_get` の
  `TIME_MONOTONIC` は単調時計、`TIME_UTC` は壁時計で、どちらも ns まで）。時計は UTC を持つ。
  `localtime` は設定 `N_TIME_ZONE_MINUTES` を足して分解する。`tm_year` に入らない年は
  `NULL`（`errno` は `EOVERFLOW`）。
- `proclist` の `struct n_procinfo` の `cpu_ns`（走った時間）と `start_ns`（始まった単調時計の
  時刻）、`sysinfo` の `struct n_sysinfo` の `uptime_ns`（単調時計）も ns。

## 9. 装置

- 音声、装置の台帳、カーネルログはファイルではない。種類つきハンドルと専用の関数で扱う。
- CPU：`cpu_info(struct n_cpuinfo *)`（`SYS_CPU_INFO`）。`detected_cpus` はファームウェアが
  示す CPU の数、`online_cpus` は動いている数、`scheduler_cpus` はアプリの thread を走らせる数で、
  動いている全部。`flags` は全 CPU が持つ機能（`N_CPU_SSE2`・`N_CPU_AVX`・`N_CPU_USER_THREADS`）。
  カーネルは自分の仕事を CPU に配らないので、`worker_cpus`・`parallel_jobs`・`worker_chunks` は 0
  で、`N_CPU_AP_WORKERS` は立たない。
- 音声：サービス audio の流れで鳴らす（§17）。
- システムログ：`n_system_log(buf, cap)` か `n_system_log_open()` のハンドルの
  `n_channel_receive` で読み、そのハンドルへ `n_channel_send` で書く（§4）。
- アプリの中の装置ドライバー（例：NVIDIA の資源管理 `C:\Nocturne\System\nvrm`、`docs/records/nvidia/LEDGER.md`）：
  - 装置は台帳（下の「装置の台帳」）の `id` で指す。どのバスの装置も同じ呼出しで claim でき、バスごとの呼出しは
    `n_hal_bus_call` を通る。`N_PCI_TARGET(segment, bus, dev, func)` は PCI の機能の住所（`uint32_t` の全部）で、
    台帳では `N_PROP_PCI_ADDRESS` の値である。`N_PCI_SELF` は `struct n_hal_pci_config` の `target` で
    「claim した機能そのもの」を言い、32 bit の外にある。上にブリッジがある機能は `N_PCI_HAS_UPSTREAM` が立ち、
    `upstream` がその住所である。
  - `n_hal_claim(cap, device)` は台帳の装置 `device`（`id`）を動かす権利（`N_KIND_HAL_CLAIM`）を返す。`cap` は
    `N_CAP_HAL_CLAIM`（§13）で、サービスの管理者が宣言のあるドライバーアプリにだけ渡す。持っている実装がある装置
    （カーネルのドライバーが動かしているもの）と、既に権利のある装置は `N_ERR_BUSY`。プロセスの実装を許さない
    装置（PCI のブリッジ）は `N_ERR_UNSUPPORTED`。同じ group の機能をカーネルのドライバーが使っていても、それが
    手放せるもの（`struct iommu_guest`：GPU の音の機能）なら、その機能は止まり、権利ができた後に domain の中で
    動き直す。権利が終わると、次の権利まで止まったまま。
    権利を持つ間、台帳のその装置は `N_HAL_RUNNING` で、`driver` はアプリの名になる。権利は
    最後のハンドルが閉じたとき（アプリの終わりを含む）と、装置がバスから去ったときに終わる。
    終わるとカーネルは (1) その権利の写像（`n_hal_map`）を全アプリから外し（その範囲は予約のまま、触れると
    全 1 が読める読取り専用のページ。バスから消えた装置と同じ）、(2) 割り込みとバスマスターを止め、
    (3) IOMMUの遮断を確認し、(4) 提供（`n_hal_offer`）を台帳から外す。終了処理内で装置の回復を待たず、FLRもしない。
    IOMMUなしでは、clean closeでも汎用PCI読出しだけでDMA停止を保証できないため残存DMAページを保全する。
    PCIeのTransactions Pendingはnon-posted要求だけであり、全TC・RO・IDOのposted DMAの終了を
    一般に証明しない（PCIe 6.0 §6.4、§7.5.3.5）。
    終わったと確かめられない権利の DMA 用メモリは、解放せずに隔離する。アプリの終了時は
    権利のハンドルを他のハンドルとマッピングより先に閉じる。写像のビュー・
    DMA アドレスの照会は、権利が終わった後は `N_ERR_CLOSED`。
    異常終了した装置の再claimには隔離が必要。非画面機能は次claimでFLRを試し、未準備・取消なら
    保存状態を装置に保持し、次回に復元を再開する。完了までハンドルを返さない。
    起動画面GPUはgeneric FLRをせず、`N_PCI_NEEDS_RECOVERY`でnative driverへ回復責務を渡す。
    フレームバッファー描画の再開はscanoutの復旧保証ではなく、FLR成功も旧DMAのdrain証明ではない。
  - 権利の呼出し（`N_CAP_HAL_CLAIM` を持つアプリが、持った権利に対して使う）：
    - `n_hal_claim_resources(claim, out, max)`：装置が持つ資源（`struct n_hal_resource`：メモリ、ポート、GSI）の
      一覧を書き、全部の数を返す。`slot` はバスが決める番号で、PCI は BAR の番号である。
    - `n_hal_map(claim, slot)`：メモリの資源を装置メモリの共有オブジェクト（既定は非キャッシュ、プリフェッチ可能なら
      書込み結合も許す）にする。1 ページ未満や境界の合わない資源、ポートは `N_ERR_UNSUPPORTED`。
    - `n_hal_interrupts(claim, count)`・`n_hal_interrupt_take(claim, bits, words, deadline)`：下の割り込み。
    - `n_hal_bus_call(claim, op, in, in_size, out, out_size)`：バスの持つ呼出し。`op` と `in`・`out` の形はバスの
      見出し（`common/hal_buses.h`）が決め、入力は 128 KiB、出力は 1 MiB まで。バスが持たなければ `N_ERR_UNSUPPORTED`。
      PCI の呼出しは `n_hal_pci_info`・`n_hal_pci_config_read`・`n_hal_pci_config_write`・`n_hal_pci_hold_screen`・
      `n_hal_pci_acpi_eval`・`n_hal_pci_acpi_notify_take`（libc の包み）で、以降の項がその意味を言う。
    - `n_hal_offer(claim, kind, instance, label)`・`n_hal_offer_withdraw(claim, kind, instance)`：この権利の実装が
      装置で提供するもの（`N_OFFER_GPU`、`N_OFFER_SCREEN` など）を台帳に載せ、外す。同じ `kind` と `instance` は
      `N_ERR_EXISTS`。権利が終わると全部外れる。
    - `n_hal_power_wait(claim, deadline)`・`n_hal_power_done(claim, ok)`：眠りと休止（dev/pm.c）の前に、権利を持つ
      アプリへ止める時を知らせる。`n_hal_power_wait` は眠りの手順が始まるまで待ち（期限は §8）、`N_HAL_POWER_SUSPEND`
      か `N_HAL_POWER_RESUME` を返す。アプリは装置を止め終えたら `n_hal_power_done` で知らせる。止めない実装は
      `n_hal_power_wait` を呼ばず、その権利がある間、眠りと休止は始まらない（NVIDIA の `nvrm` は呼ばない）。
  - `n_hal_pci_info`（`N_PCI_CALL_INFO`）は ID、BAR、MSI／MSI-X の数、上流のブリッジ、起動時の画面がその機能の BAR に
    あるならその位置（ドライバーはそこを壊さない）を返す（`struct n_hal_pci_info`）。
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
  - `n_hal_pci_hold_screen(claim, hold)`：起動時の画面がその機能の BAR にあるとき、ドライバーが
    CPU から画面への経路を作り直す間（NVIDIA の RM は GSP が BAR1 を仮想モードにする間）、
    システムは画面へ書かない。`true` は書込み中のものが終わってから戻り、入れ子にできる。最後の
    `false`（または claim の終わり）で画面全体を描き直す。時間切れで描画を再開することはない
    （作り直しの途中で書くことこそ保持が防ぐ競合）。claim の終わりは装置を止めて（BAR の写像の
    取消し、割り込み、バスマスター）から保持を返す。起動項目 `fbhold=<秒>` を付けると、それだけ
    保持したとき、保持しているドライバーのアプリをシステムが止め、claim の終わりで CPU 描画を
    再開する（scanout の復旧保証ではない。時間の既定は無し：RM の待ちは次々に続き、上限を
    決められない。遅いだけのドライバーを止めると GPU が使えなくなる）。
    画面が別の所なら `N_ERR_NOT_FOUND`。パニック画面は保持に関係なく描く。
    `n_screen_give` で画面を自分の面へ移したドライバーも、同じく保持できる（画面 0 の面）。
  - 画面の呼出し（`n_screen_give`・`n_screen_flips`・`n_screen_report_vblank`・`n_screen_offer_modes`・
    `n_screen_read_state`・`n_screen_image_add`・`n_screen_image_end`、`SYS_SCREEN_*`）は claim の handle を取り、
    その claim の装置の画面を扱う。画面を出す装置のドライバーは出力（コネクター・head・表示エンジンの
    pipe など、ドライバー自身の番号）ごとに画面を一つ出す。起動時の画面が通る機能の出力 0 が
    画面 0（タスクバーのある画面）で、ほかの出力はそれぞれ自分の画面を持つ。同じ機能の同じ出力は、
    どのドライバーが駆動しても同じ画面になる。画面の数に上限は無く、画面は消えない（ドライバーが
    去った画面は、次に面を渡されるまで消えている）。画面ごとにモード・リフレッシュ・可変
    リフレッシュの範囲・垂直帰線・フリップを持ち、合成も画面ごとのスレッドがその画面の周期の
    締切で行う。
  - `n_screen_give(claim, output, buffers)`：ドライバーが出力の scanout を組んだら、その面
    （`struct n_screen_buffers`：一つのモードと形式の面を `count` 枚、1〜`N_SCREEN_BUFFERS_MAX`）を
    システムへ渡す。戻った時からその出力の画面は渡した面へ描かれ、その大きさになり、画面 0 なら
    起動時の画面（GOP）へは書かない（`buffers` に NULL を渡して返すか claim の終わりまで）。
    戻った時に表示中なのは一枚目。各面は、この claim で作った書込みできる共有オブジェクトと、
    その中の最初の行の `offset`：BAR（プリフェッチ可能なもの。システムは書込み結合で写す。
    PAT が無ければ非キャッシュ）、または型が write-back でない DMA オブジェクト（書込み結合か
    非キャッシュの view を作るとその型に決まる）、あるいはそれらの view。`offset` から
    `pitch × height` バイトが面に収まり、`format` は `N_SCANOUT_XRGB8888`、
    `N_SCANOUT_XRGB2101010`、`N_SCANOUT_XBGR8888`（赤が下位のバイト、NVIDIA の X8B8G8R8）の
    どれか。幅と高さは表示エンジンが出すモードのもの（ドライバーがモードを出した後はシステムが
    選んだモード、`struct n_screen_state` の `mode`。各 65535 まで）で、画面はその大きさに
    なり、その画面の全画面・最大化の窓も従う。システムはコマを、出力が表示していない面へだけ描き、
    描き終えたらその面のフリップを求める（`n_screen_flips`、下）。表示中の面へ CPU が書くことは
    無い。一枚だけなら、ほかに面が無いので表示中の面へ描き、書いたとおりに映る（フリップの知らせは
    届くが、待たずに表示されたものとする）。戻り値は、その出力の前の面への最後のフリップの番号
    （無ければ 0）：その番号までのフリップは前の面のもので、まだ取っていないものは消える。
    受け付けるとカーネルは各面をカーネル空間へ同じ型で写し、そのページの型を固定する
    （1 ページ 1 型。ドライバー自身の書込み結合の写像とは衝突しない。非キャッシュの写像とは
    どちらか後の方が `N_ERR_DENIED`）。進行中の描画が終わるのを待ってから出力先を切り替え、
    全体を描き直す。システムログに 1 行。0 枚・`N_SCREEN_BUFFERS_MAX` 超・0 や 65535 超の大きさ・
    未知の形式・面に収まらない範囲・返す面が無いことは `N_ERR_INVALID`、その大きさの画面が
    メモリーに収まらなければ `N_ERR_NO_MEMORY`（画面はそのまま）、ほかの claim のオブジェクト・
    claim の終わり（クラッシュを含む）では、装置を止めて
    から画面を消し、画面 0 は起動時の画面へ戻して描き直す。ただしドライバーが表示エンジンを
    組み替えていれば、その画面は実際には映らないことがある（戻す処理は無い。ログにもそう書く）。
    保持はこれまでどおり全部の書込みを止め、パニック画面はすべての画面の面と、
    `n_screen_image_add`（下）で登録された面のすべてへ描く。スリープと休止は、claim のある間
    始まらない（dev/pm.c）ので、ドライバーの面が表示中に起きることはない。
  - `n_screen_flips(claim, out, max, deadline)`：claim の出力の画面が求めるフリップ
    （`struct n_screen_flip`：出力、面の番号、出力ごとに 1 から数える `serial`）を `max` 件まで
    読む。無ければ期限（§8）まで待ち、期限で 0。取る前の出力のフリップは、
    その出力の次のフリップに置き換わる。ドライバーはその面を次の垂直帰線から出し、表示した帰線を
    `n_screen_report_vblank` の `flip_serial` で知らせる。画面の次のコマはその知らせを待つ。
  - `n_screen_report_vblank(claim, vblank)`：渡した面の出力の垂直帰線（`struct n_screen_vblank`）を
    知らせる。`count` はドライバーの帰線番号、`time_ns` は表示エンジンが測った帰線の始まりを
    システムの単調時計（§8、ナノ秒）へ直したもの、`buffer` はその帰線から表示中の
    面、`flip_serial` はその帰線が表示する最後のフリップ（無ければ 0）。フリップを表示する帰線は
    表示エンジンの知らせのとおりに、ほかの帰線はフリップを待つ間に知らせる。合成はその時刻を
    窓の `EV_FRAME` の `time_ns`（表示された時刻）にする。画面がこの claim の面でなければ
    `N_ERR_DENIED`。
  - モニターのモード（解像度、リフレッシュレート、色の深さと形式、可変リフレッシュ）：選ぶのはサービス `display` と利用者、
    列挙・検証・設定はドライバー。カーネル（gui/screenmode.c）は一覧と望みを運ぶだけで、選ばない。起動の時は
    ファームウェアの面で動き、望みが来るまでそのまま。`n_screen_offer_modes(claim, output, modes)` で画面を出す装置のドライバーが、
    出力ごとに、駆動するモニター（`struct n_screen_modes` の `monitor`：EDID の製造者・製品・製造番号から作る
    識別子、`name`：EDID のモニター名）と、そこで出せるモード（最大 `N_SCREEN_MODES_MAX`。EDID の
    推奨モードに `N_SCREEN_MODE_PREFERRED`、可変リフレッシュで出すモードに `N_SCREEN_MODE_VRR`。
    `bits_per_channel` と `format`（`N_SCANOUT_*`）はそのモードで出す色で、0 は言わない。
    `vrr_min_mhz` はモニターの可変リフレッシュの下限、無ければ 0）を知らせる。
    `n_screen_mode_apply(cap, screen, mode)`（`SYS_SCREEN_MODE_APPLY`、`N_CAP_DISPLAY`）が望みを渡す：提供に無いモードは
    `N_ERR_INVALID`、提供が無ければ `N_ERR_NOT_FOUND`、全部 0 は望みなし。カーネルはこれを `struct n_screen_state` の
    `mode` と `mode_serial`（望むたびに進む。同じモードでも）でドライバーへ伝える。ドライバーは
    それを出し（その大きさで `n_screen_give`）、`done` に `mode_serial`、`shown` に出せたモードを
    入れて知らせ直す。望まれたものと違えば、カーネルは `refused_serial` にその番号を入れて報告するだけで、次を決めるのは
    サービス。`done` 0（開始時、モニターの変化・接続時）は `offer_serial` を進める。`count` 0 は、動いた
    ままモニターを駆動しなくなったこと、claim の終わりはドライバーが止まったこと。面を返した
    （`buffers` NULL）ドライバーがモードを出したままなら、次の望みで
    `mode_serial` が進むので、それを見てもう一度モードを出せる。件数・0 の大きさやリフレッシュ・65535 超は `N_ERR_INVALID`。
  - 誰でも `SYS_SCREEN_INFO` の第3引数 `SCREEN_MODES_GET`（`n_screen_modes_get`、`struct screen_modes`：モニター、
    モード、表示中と望まれたモード、ドライバーが出せなかった最後の望み `refused_serial`（モニターが来てから無ければ 0）、
    画面の出所 `source`、画面の数 `screens`、デスクトップの中の位置と大きさ、`placed`、`primary`、`scale_percent`）で読む
    （display_abi.h）。画面ごとで、第3引数の 8 ビット目から上が画面の番号（`op | screen << 8`）。無い画面は `N_ERR_NOT_FOUND`。
    モードが 0 件のときも `monitor`・`name` は
    ファームウェアが映すモニターの EDID から入り、起動時の画面が表示中なら `shown` はファームウェアの
    モード（大きさと、EDID のその大きさのタイミングから求めたリフレッシュ。分からなければ 0）。
    `source` は `SCREEN_SOURCE_FIRMWARE`（ドライバーが一度もモードを出していない）、
    `SCREEN_SOURCE_DRIVER`（ドライバーの面が表示中。0 件ならモニターを見つけていない）、
    `SCREEN_SOURCE_HANDED_BACK`（モードを出したドライバーが claim を持ったまま、起動時の画面が
    表示中：画面を返した、または自分の面をまだ出していない。モードが出ていれば適用で
    もう一度望める）、`SCREEN_SOURCE_DRIVER_STOPPED`（モードを出したドライバーの claim が
    終わった）、`SCREEN_SOURCE_REMOTE`（遠隔デスクトップの利用者の窓の大きさ）。
  - 配置：`n_screen_arrange(cap, screen, &arrange)`（`SYS_SCREEN_ARRANGE`、`N_CAP_DISPLAY`、`struct n_screen_arrange`）。
    `x`、`y` は負でない。`N_ARRANGE_PLACED` はそこへ置く。無ければ置かれた画面の右に続く。`N_ARRANGE_PRIMARY` は主画面にする：
    新しい窓、`struct screen_format` の作業領域、絶対ポインターの基準がそこになる。`scale_percent` は 0 でなく、
    `struct screen_format` と `struct n_shell_screen` でプログラムへ報告する。カーネルは倍率で描かない。
    届く位置の上限は、部品の端が `int` に収まること（幅と高さは 65535 以下）だけが決める。
  - サービス `display`（`user/apps/display.c`）：画面の見張りで一つのスレッドが眠り、設定の値をカーネルへ渡す。モニターは
    `n_screen_device(screen, &device)`（EDID の身元の番号。空か、前の画面と同じなら画面の番号を足す）で引く。鍵は §12 の
    `N_DISPLAY_MODE`・`N_DISPLAY_PLACE`・`N_DISPLAY_SCALE`・`N_DISPLAY_PRIMARY`。モードの選び方は、利用者の選択、
    モニターの推奨、推奨の大きさで最も近いリフレッシュ、最大の大きさで推奨に最も近いリフレッシュ（推奨が無ければ最も高い）の順で、
    出せないと言われたモードはそのモニターがつながっている間は選ばない。試しの中の断りは試しを取り消す。
    試して戻す（`SCREEN_MODE_TRIAL_MS`、15 秒）のは設定のサービスの仕事で、`display` は適用の依頼に従うだけ。
    申し出の無いモードの値は選択として残り、出せるようになるまで望まない。止まっている間は最後の望みと配置がそのまま残り、
    起こし直しは今の表示を変えない。
  - デスクトップの設定は `SCREEN_SETTINGS_GET`（`struct desktop_settings`）が読む：
    なめらかな階調（`smooth_gradients`：8 ビットの画面に、デスクトップをチャンネルあたり 10 ビットで合成して
    ディザをかけて出す）。利用者の望みで、表示中の出力が 8 ビットの xRGB（ドライバーの面を含む）の間だけ効く
    （`smooth_gradients_available`）。デスクトップ（`gui/wm.c`）の持ち物で、変える口は §12 の
    `N_WINDOW_SMOOTH_GRADIENTS`。変えるとすぐ効く。
  - 画面の見張り：`SYS_SCREEN_INFO` の第3引数 `SCREEN_WATCH`（第1・第2引数は 0。libc の
    `n_screen_watch()`、display.h）は、`SCREEN_SETTINGS_GET`・`struct screen_format` の返事・
    `SCREEN_MODES_GET` が報告するもののどれかが変わると受け取れる見張り（`N_KIND_WATCH`、§12）。
    デスクトップの大きさと設定、なめらかな階調を出せるか、申し出たモニターとモード、望まれた
    モード、拒まれたこと、`source`、配置と倍率。第1・第2引数が 0 でなければ `N_ERR_INVALID`、
    デスクトップが動いていなければ `N_ERR_UNSUPPORTED`。
  - コマの拍：合成は画面ごとに、その画面のリフレッシュの周期を締切にしたスレッドで描く
    （`sched_deadline`）。周期はドライバーが出しているモード（`shown`）か、起動時の画面なら
    EDID のそのモードのタイミングから取る。二枚以上の面の画面では、フリップの表示（上の
    `n_screen_report_vblank`）ごとに 1 枚を描き始める。フリップがすぐ表示される画面（一枚の面）では、
    周期の次の時刻より前には描かない。可変リフレッシュの画面は描けしだい 1 枚、最も速い
    リフレッシュまで。周期が分からない画面は、絵が変わったときに描く。どれで刻んでいるかは
    `wm: screen` の行が言う。
  - 直接表示：`n_screen_give` の `format` に `N_SCANOUT_DIRECT` を足したドライバーは、
    全画面の窓の絵を合成を通さずに自分で映せる（デスクトップの合成はこれまでどおり渡した面へ
    続く。ドライバーは映す面を切り替えるだけ）。窓は `win_direct(win, true)` で望み、
    全画面・表示中・入力を持つ窓で、メニュー・音量・通知・大きさ変更の枠・遠隔の利用者が
    無い間だけ許される。許可の出入りは窓へ `EV_DIRECT`（key 1 許可、0 取消し）、今の状態は
    `win_state` の `WIN_STATE_DIRECT`。取り消された窓へは描き直しの損傷も送るので、
    窓は自分の通常の面へ描き直す。
  - `n_screen_read_state(claim, output, seen, out, deadline)`：出力の画面をこの claim の面へ描かせているか、
    モードを出したドライバーが、出すべきモード（`mode`、`mode_serial`。上）と、直接表示を受け持つ
    なら許可された窓（`struct n_screen_state` の `app`。無ければ 0）と、その間ドライバーが
    描くポインター（位置・表示の有無・32×32 の ARGB 画像とその要点、画像が変わると
    `pointer_serial` が進む。画像は窓の管理が選んだもの：その形の絵か、窓が `SYS_WIN_POINTER`
    で渡した画像。§10）を読む。`serial` が `seen` と同じ間は期限（§8）まで待つ
    （過ぎた期限は待たない。`N_ERR_TIMEOUT` でも今の状態を書く）。画面がこの claim の面でなく、
    モードも出していなければ `N_ERR_DENIED`。claim の終わりと起動時の
    画面への返却で直接表示は終わり、許可も取り消す。
  - `n_screen_image_add(claim, output, surface, desc)`／`n_screen_image_end(claim, image)`：直接表示を
    受け持つドライバーが、出力に渡した面の代わりに表示エンジンへ出すことのある面（アプリの絵）を
    登録する。カーネルが止まると、表示エンジンは最後にフリップされた面を出し続け、渡した面へ
    戻すドライバーはもう動かない。そのためパニック画面は、渡した面と登録された面のすべてへ
    描く。システムがそこへ描くのはこの時だけ。`surface` と `desc`（`struct n_scanout`：幅・高さ・
    `pitch`・形式と、面の中の最初の行の `offset`）は `n_screen_give` の一枚と
    同じ条件（`format` に `N_SCANOUT_DIRECT` は付けない。大きさは面自身のもので、表示エンジンが
    モードの大きさへ拡大縮小してよい。パニック画面はその大きさで描く）で、カーネルは同じく型を固定して
    写す。正の番号を返し、`n_screen_image_end` にその番号を渡すと登録を外す（ドライバーは
    面の memory を解放する前に外す）。`N_SCANOUT_DIRECT` 無しで渡し直すこと、起動時の画面への
    返却、claim の終わりで、登録はすべて外れる。出力の画面がこの claim の `N_SCANOUT_DIRECT` の面で
    ない、またはオブジェクトが `n_screen_give` の受け付けないものなら `N_ERR_DENIED`、0 や
    65535 超の大きさ・未知の形式・面に収まらない範囲、または外す番号が無ければ `N_ERR_INVALID`。登録できない絵は表示エンジンへ
    出さないこと（パニック画面が見えなくなる）。
  - `n_hal_pci_acpi_eval(claim, node, name, args, nargs, data, data_size, out, out_size)`：claim した
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
  - `n_hal_pci_acpi_notify_take(claim, deadline)`：その機能の装置へ届いた ACPI の Notify を一つ
    取り出し、値を返す（最初の呼出しで受取りを始め、claim の終わりまで続く。32 個まで溜め、
    溢れたら古いものから捨てる）。期限（§8）まで届かなければ `N_ERR_TIMEOUT`、装置が
    無ければ `N_ERR_NOT_FOUND`。
  - `n_hal_interrupts(claim, count)` は MSI-X（無ければ MSI）を、機能と系が出せる本数まで
    つなぎ、本数を返す。INTx はアプリへ渡さない。届いた割り込みは番号のビットを立て、ビットが
    ある間ハンドルは `N_EVENT_RECEIVABLE`。`n_hal_interrupt_take(claim, bits, words, deadline)` は
    先頭 `64 × words` 本のどれかが届くまで待ち（期限は §8）、それらのビットを `bits` へ書いて
    消し、書いた語の数を返す。番号 n は `bits[n / 64]` のビット `n % 64`。後ろの番号のビットは残る。
  - `n_dma_create(claim, bytes, flags)` は装置が DMA で読み書きできる 0 埋めの RAM（共有
    オブジェクト）。`N_DMA_CONTIGUOUS` は物理的に一続き、`N_DMA_32BIT` は 4GiB 未満。
    `n_dma_addresses` が各ページのバスアドレスを返す（物理アドレス。IOMMU が隔離していても
    同じ番地に写すので変わらない）。
  - IOMMU（`kernel/src/dev/iommu.c`、AMD-Vi は `iommu_amd.c`、VT-d は `iommu_intel.c`）：起動時に
    ACPI の IVRS か DMAR からユニットを見つけて設定する。claim されていない装置とカーネルの装置は
    素通し（AMD-Vi は 0 の装置表エントリ、VT-d は pass-through の文脈エントリ）。claim した装置は
    専用のドメインに入り、DMA が届くのはその claim で作った DMA オブジェクト（作成から解放まで）と
    ファームウェアの予約範囲（IVMD・RMRR）だけ。割り込みは再マップ表で claim のベクターだけに限る
    （VT-d は互換形式の割り込みを拒み、IOAPIC とカーネルの装置の MSI も表を通る。CPU が x2APIC に
    いるか、x2APIC へ移るのを待つ時（Intel の CPU で、ファームウェアが xAPIC で渡し、DMAR が
    X2APIC_OPT_OUT を求めない時。SDM 13.12.7 は拡張割り込みモードの再マップを x2APIC より先に
    求める）、全ユニットが EIM を持てば拡張割り込みモードで 32 ビットの宛先を書く。待つ CPU は、
    ユニットが立った後、IOAPIC の経路とドライバーの MSI が表を通る前に、割り込みを閉じて揃い、
    全部一度に x2APIC へ移る。再マップが立たない、または EIM の無いユニットがある時、待つ CPU は
    xAPIC のままで、その理由をログに一行出す。拡張割り込みモードを必須とするユニット（ECAP.EIMER）
    は、xAPIC の宛先では再マップを使わない）。割り込みの再マップは DMA の変換と別のもので、
    再マップを必須とするユニット（ECAP.IRREQ）は切れている間すべての割り込みメッセージを止める
    （§5.1.4）。そのため起動の記録で DMA の変換を切った時、ファームウェアの記述を保持できなかった
    時、共通のページ表形式が無い時、DMA 変換の起動に失敗した時も、ユニットが再マップできれば
    再マップだけで起動する（DMA は変換しない）。再マップを必須とするユニットが再マップできなければ、
    その旨（割り込みが止まること）をログに出す。
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
    隔離されている装置は `n_hal_pci_info` の `flags` に `N_PCI_ISOLATED`。IOMMU が無い機械、起動
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
    ユーザーへ通知する。CPU は眠る前の APIC のモードで戻り、Intel の CPU が x2APIC へ移った後なら
    再マップなしの x2APIC になる（SDM 13.12.7 からの逸脱。宛先は 255 未満の APIC ID）。
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

### 装置の台帳

`SYS_HAL_LIST`（`n_hal_list(out, bytes)`）は、機械で見つかった物の木を `struct n_hal_device` の並びで `out` へ写し、全部の大きさを返す。`bytes` が足りなければ何も写さずに要る大きさだけを返す。親の下に子が続く順で、先頭は根である。装置の木は、バスの制御器と、その下の装置（PCI の機能、USB の装置、VMBus の channel、ACPI の装置、固定のポートの制御器など）からなる。`n_hal_snapshot(&snapshot)` は全部を malloc の領域へ写し、`first` と `count` を書いて件数を返す。`first` は呼ぶ者が `free` する。

記録は `struct n_hal_device` の後ろに、性質（`n_hal_prop`）、提供（`n_hal_offer`）、依存先の `id`、性質のバイトの区画が続く。読む口は `n_hal_device_props`・`n_hal_device_offers`・`n_hal_device_depends`・`n_hal_device_number`・`n_hal_device_next`。`id` は起動の間、二度と使わない。`parent` は親の `id` で、根の `parent` は 0 である。`bus` は `N_HAL_PROP_BUS` と同じ名で、台帳はバスの名の意味を知らない。性質の鍵はバスが決める（`common/hal_buses.h`）：PCI の機能は `N_PROP_PCI_VENDOR`・`N_PROP_PCI_DEVICE`・`N_PROP_PCI_CLASS`・`N_PROP_PCI_ADDRESS` などを持つ。

`state` は `N_HAL_UNBOUND`（持っている実装が無い）、`N_HAL_ATTACHING`（実装が立ち上げている）、`N_HAL_RUNNING`（`driver` が持っている）、`N_HAL_FAILED`（`driver` が持てなかった、または止まった。理由は `reason`）。`name` は装置が言う名で、言わないなら空である。`location` はバスの中の場所の字で、バスが書く。`driver` は持っている実装の名で、プロセスの実装ではプログラムの名である。提供（`n_hal_offer`）は、実装がその装置で出すもの（ディスク、網、音の出口、入力、画面、GPU）の種類 `kind`、番号 `instance`、人に見せる名 `label` である。

規則 `struct n_hal_rule` は項（`struct n_hal_term`）の並びで、全部の項が装置の性質に合えば合う。`n_hal_device_rule_score(device, rule)` は合えば項の数、合わなければ 0 を返す。サービスの管理者は、サービスの宣言の規則に合う `N_HAL_UNBOUND` の装置ごとに実例を一つ起こし、実例の引数に装置の `id` を 16 進数で渡す。

`SYS_HAL_WATCH`（`n_hal_watch()`）は、台帳が変わると知らせる見張りの handle を返す（`n_watch_take`）。

## 10. 番号と、ほかの節に無い呼出し

欠番：1〜18、20〜25、31〜32、36〜37、44、51〜52、60〜63、90、164、203〜206。ハンドルの種類 12 も
欠番。ABI は呼出しの古い形を残さない：プログラムは動かすシステムの `abi.h` で組む。

ほかの節に無い呼出し：

- 窓の面：窓の画素は面（共有オブジェクト、`N_KIND_SHARED`）で、幅×高さの 0xAARRGGBB
  （`WIN_DEEP_COLOR` ならその後ろに深い面）。`SYS_WIN_SURFACE`（65、窓、番号）が面のハンドルを
  返し、アプリはそれを写す（`n_shared_map`）か、代わりに描く別のアプリへ渡す。合成は同じ頁を読む。
  libc の `win_open` は面を写してからハンドルを閉じる（写像はハンドルより長く生きる）。
  窓の大きさは縦横とも 1〜`N_WIN_SIZE_MAX`（16384）で、画面より大きくてよい（映るのは収まる分）。
  カーネルは大きさを丸めない：範囲外の `SYS_WIN_CREATE`・`SYS_WIN_RESIZE` は `N_ERR_INVALID`。
  - `WIN_FRAME_QUEUE` の無い窓は面を一枚持ち続け、その一部へ描く。`SYS_WIN_PRESENT`（66、窓、
    x、y、幅 << 32 | 高さ、0）は変えた矩形を名指し、システムはその矩形の写しを取る（変更の半分が
    映ることはない）。
  - `WIN_FRAME_QUEUE` の窓は `SYS_WIN_CREATE` の第5引数の枚数（2〜`N_WIN_SURFACES_MAX`、0 は 2）の
    面を持ち、一枚が一コマ全部。`SYS_WIN_ACQUIRE`（81、窓、期限（§8））が描いてよい面の
    番号を返し（無ければ期限まで待つ）、`SYS_WIN_PRESENT` の第5引数に番号＋1 を渡すと、その面
    全体を行列へ入れる。合成は写さずにその面を読む。画面に出ている面は、別の面が出るまで
    システムのもので、出なくなると `EV_SURFACE`（`buttons` のビット i が面 i）で返る。行列は一コマに
    一つずつ出る。`WIN_MAILBOX` では、新しい present がまだ待っている present を置き換え、置き換え
    られた面はすぐ `EV_SURFACE` で返る（画面より速く描くゲームが最新のコマを出す）。present から返る
    までその面へ書いてはならない。表示中の面を present すると `N_ERR_BUSY`。
  - `SYS_WIN_PRESENT` は present の番号を返す。`EV_FRAME` は、画面のコマが窓の出したものを取り込んで
    表示したことを知らせる：`y` は画面のコマの周期（ナノ秒。可変リフレッシュの画面は最も短い周期）、
    `time_ns` は表示された時刻（表示装置が測った垂直帰線。帰線の無い画面では写しの終わり）、
    `serial` はそのコマが表示した最後の present の番号、`key` は表示した面（`WIN_FRAME_QUEUE`）。
    二つの画面にまたがる窓は、中心のある画面の歩調で来る。
  - `SYS_WIN_RESIZE`（72、窓、幅、高さ）は新しい大きさの面を作る。アプリは面を写し直す
    （libc の `win_resize`。`EV_RESIZE` を受けた `win_event` も同じことをする）。
- 入力の型は `common/input_abi.h`。キーは HID の usage（HID Usage Tables の Keyboard page）で届く。
  カーネルは文字を作らず、配列を読まない。
  - `EV_KEY`・`EV_HOTKEY`：`usage` は押された鍵、`mods` は `NMOD_*`（`NMOD_SHIFT`・`CTRL`・`ALT`・`SUPER`・`CAPS`・
    `RIGHT_ALT`）、`layout` はその装置の `enum n_key_layout`。カーネルにとって `layout` は意味のない数で、装置の値を
    事象に写すだけ。
  - 文字の持ち主は libc の `n_key_translate(&event)`（`user/libc/keymap.c`）。`win_event` がキーの事象ごとに呼び、`key` に
    文字（Unicode）か `NKEY_*` を入れる。`NKEY_*` は Unicode の外（`0x110000` から）で、文字かどうかは
    `N_KEY_IS_TEXT(k)` で分ける。`SYS_WIN_RECEIVE` を直に読むアプリは自分で呼ぶ。
  - usage の無いキー事象（`usage` が 0）は host が鍵なしで打った文字で、`key` に初めから文字が入っている。
  - `EV_MOUSE_DOWN`：`clicks` は続けて押された回数（1 から）。同じボタンを `N_INPUT_DOUBLE_CLICK_MS` の内に、ほぼ同じ場所で
    押すと増える。数えるのは `gui/wm.c` で、アプリはダブルクリックの時間を読まない。
  - ポインターの速さ、加速、ホイールの向きは `dev/input.c` が装置ごとの値で当てる。`EV_MOUSE_*` と `EV_WHEEL` は結果。
  - キーの繰返しはキーボードのドライバーが装置の値で作る（USB は alarm。Hyper-V と遠隔デスクトップは host が繰り返す）。
- `struct gui_event` の `time_ns` はシステムの単調時計（§8、ナノ秒）の時刻：入力は
  装置の報告がシステムに届いた時、`EV_FRAME` は表示された時。窓が受け取らずに入力が溜まり、
  システムが窓に持つ数を超えると、黙って捨てず `EV_INPUT_LOST` を一つ入れる：それより前に押されて
  いたキーとボタンは放されたものとし、次のイベントはその後に来たもの。
- `SYS_WIN_RECEIVE`（窓、`struct gui_event *out`、最大数、期限（§8））：待ち行列から
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
`WIN_START_MAXIMIZED`、`WIN_ALWAYS_ON_TOP`、`WIN_FRAME_QUEUE`（コマごとに空いた面を待ち、
全体を描く。表示に合わせて間隔が決まる。§10）、`WIN_MAILBOX`、`WIN_DEEP_COLOR`。サーフェスの flags は
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
| 1 | CLICK | ボタン、一覧の項目、`clickable` の箱、一覧（`menu`）の項目。続けて押した回数は `flags >> 16`（`UI_CLICKS`）。押したボタンは `(flags >> 8) & 0xFF`（`UI_BUTTONS`。1 左、2 右、4 中。一覧の項目はどのボタンでも選ばれ、キーで選んだ時は 0）、a、b＝その部品の中のポインターの位置 |
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

## 12. 設定と見張り

設定はファイルではない。変えて適用する値である。カーネルは値の持ち主へ渡し、今の値を覚え、
保存しない。保存は設定のサービスの仕事で、サービスは名前の木の外の私的な領域（§3）に自分の形式で
置き、起動のたびに適用し直す。カーネルの中に `.conf` は無い。

- 型のある値は領域と鍵の番号で引く（`common/settings_abi.h`）：`enum n_setting_domain` と領域ごとの
  鍵。文字列は解析しない。値の型は `N_SETTING_BOOL`、`N_SETTING_INT`（`int32_t`）、`N_SETTING_ENDPOINT`
  （`struct n_sockaddr`）、`N_SETTING_MONITOR_MODE`（`struct n_setting_monitor_mode`）、
  `N_SETTING_MONITOR_PLACE`（`struct n_setting_monitor_place`）、`N_SETTING_DEVICE`（`uint64_t`、`n_setting_device` の番号）。

  | 領域 | 鍵 | 持ち主 |
  |---|---|---|
  | `N_DOMAIN_BOOT` | `N_BOOT_IOMMU`、`N_BOOT_ACPI_AML`、`N_BOOT_SMP`、`N_BOOT_RESUME`、`N_BOOT_UNISOLATED_DRIVERS` | `fs/bootrecord.c` |
  | `N_DOMAIN_AUDIO` | `N_AUDIO_VOLUME`（0〜100）、`N_AUDIO_MUTED`、`N_AUDIO_OUTPUT`（出口の key。空は一番良いもの）、装置ごとの `N_AUDIO_FORMAT_RATE`・`N_AUDIO_FORMAT_BITS`・`N_AUDIO_FORMAT_CHANNELS`（0 は既定） | サービス audio（§17） |
  | `N_DOMAIN_DISPLAY` | `N_DISPLAY_HOLD_SECONDS` | `dev/pciuser.c` |
  | `N_DOMAIN_DISPLAY` | `N_DISPLAY_MODE`（試し）、`N_DISPLAY_PLACE`、`N_DISPLAY_SCALE`（試し）、`N_DISPLAY_PRIMARY` | サービス `display`（§9） |
  | `N_DOMAIN_WINDOW` | `N_WINDOW_SMOOTH_GRADIENTS` | `gui/wm.c` |
  | `N_DOMAIN_POWER` | `N_POWER_PERFORMANCE_PREFERENCE`（0〜255。AMD の CPPC、Intel の HWP へ渡す） | `arch/idle.c` |
  | `N_DOMAIN_POWER` | `N_POWER_BUTTON`、`N_POWER_LID`、`N_POWER_IDLE_MINUTES`、`N_POWER_IDLE_ANSWER`、`N_POWER_ALLOW_SLEEP`、`N_POWER_ALLOW_HIBERNATE`、`N_POWER_CLOSE_SECONDS`、`N_POWER_AGAIN_SECONDS` | サービス `machine`（§15） |
  | `N_DOMAIN_NETWORK` | `N_NETWORK_TCP_RX_BYTES`、`N_NETWORK_TCP_TX_BYTES` | `net/net.c` |
  | `N_DOMAIN_INPUT` | `N_INPUT_POINTER_SPEED`（-2〜2）、`N_INPUT_POINTER_ACCEL`、`N_INPUT_SCROLL_NATURAL`、`N_INPUT_DOUBLE_CLICK_MS`、`N_INPUT_REPEAT_DELAY_MS`、`N_INPUT_REPEAT_INTERVAL_MS`、`N_INPUT_LAYOUT`（`enum n_key_layout`）、`N_INPUT_DEVICE`（装置ごとの記録） | `dev/input.c` |
  | `N_DOMAIN_LANGUAGE` | `N_LANGUAGE_UI`、`N_LANGUAGE_REGION`、`N_LANGUAGE_DATE_ORDER`、`N_LANGUAGE_CLOCK`、`N_LANGUAGE_NUMBER`、`N_LANGUAGE_LOGIN` | カーネルに無い。`libc/language.c` が読む |

- 装置ごとの値 `N_INPUT_DEVICE` は `N_SETTING_INPUT_DEVICE` の記録（`struct n_setting_input_device`、`N_SETTING_RECORDS`）。
  頭の後ろに装置の id と名が続く可変長で、次の記録は `N_INPUT_RECORD_NEXT(size)` の先。id は `usb:vvvv:pppp:hash:i`
  の形で、`hash` は serial か、無ければ差した場所。値の欄の `N_INPUT_SAME` は利用者の既定に従う。全部の欄が
  `N_INPUT_SAME` の記録は選択を消す。`n_settings_effective` はつながっている装置（`N_INPUT_ATTACHED`）と、覚えている
  選択を全部返す。装置の抜き差しは `n_settings_watch(N_DOMAIN_INPUT)` に届く。

- `n_settings_apply(cap, domain, key, value, size)`（`SYS_SETTINGS_APPLY`）：持ち主へ渡す。`cap` は
  `N_CAP_SETTINGS_APPLY` の能力（§13）で、持つのは設定のサービス。プログラムはサービスへ頼む。
  知らない領域と鍵は `N_ERR_NOT_FOUND`、`size` が型と違うか範囲の外は `N_ERR_INVALID`。一覧の鍵は一件ずつ
  適用する。
- `n_settings_effective(domain, key, out, cap)`（`SYS_SETTINGS_EFFECTIVE`）：今の値。全体のバイト数を返し、
  `cap` に入る分を写す。誰でも読める。
- `n_settings_describe(domain, key, out)`（`SYS_SETTINGS_DESCRIBE`）：`struct n_setting_desc`（型、一つの値の
  大きさ、旗 `N_SETTING_LIST`・`N_SETTING_AT_START`、範囲）。
- `n_settings_watch(domain)`（`SYS_SETTINGS_WATCH`）：その領域の今の値が変わると受け取れる見張り。
  誰が変えても知らせる（タスクバーのスピーカーも）。
- 起動より前の値（`N_DOMAIN_BOOT`）は起動の記録に置く。起動の区画の `/boot/settings.rec`（8 KiB）が
  Limine の module "settings" として届き、カーネルは起動の初めに読む。記録は `struct n_boot_record` の
  二つの枠で、世代の大きい方の CRC-32C の合うものが効く。全部 0 は既定。`SYS_SETTINGS_APPLY` は
  古い枠へ上書きする（FAT32 の区画の中の番地を辿る。大きさは変えない）。
  効くのは次の起動から（`N_SETTING_AT_START`）。`SYS_SETTINGS_EFFECTIVE` は記録の値を返す。
- 起動の引数は製品の値を決めない。`kernel/src/boot.c` の `test_flag` などの読み手は試験の組み
  （`make kernel-test`、`NOCTURNE_TEST_FLAGS`）だけにある。
- 設定のサービス `settings`（`user/apps/settingsd.c`）：選択の保存、宣言の表、型と範囲の確かめ、権限の
  確かめ、見張りを持つ。領域の意味を持たない。呼ぶ側の口は `nocturne_settings.h`（`user/libc/settings.c`）、
  channel の文は `common/settings_abi.h` の `enum n_settings_op`。
  - `n_setting_get`・`n_setting_get_int`：値。`n_setting_set`・`n_setting_set_int`：検証、適用、保存、見張りへの
    知らせ。適用が断られれば何も変えず、保存に失敗すれば適用を戻す。理由は `enum n_setting_reason` で返る。
    `n_setting_set_if`：今の値が読んだ値と同じときだけ書く。
  - `n_setting_try`・`n_setting_commit`・`n_setting_revert`：`N_APPLY_TRIAL` の鍵（画面のモード）の期限つきの試し。
    期限はサービスが数える。試した者が死んでも値は戻る。戻す先は適用より先に保存の版へ書くので、
    サービスが試しの最中に落ちても、起き直したサービスが前の値へ戻す。
  - `n_setting_watch`・`n_setting_watch_take`：見張り。`struct n_settings_changed` が届く。
  - `n_setting_list`：見える宣言の一覧。設定の窓（`user/apps/settings.c`）はこれで頁を組む。
  - `n_setting_declare`：宣言を足す。プログラムの設定は `N_SCOPE_PROGRAM`（利用者ごと、プログラムごと）で
    宣言し、読み書きするのはそのプログラムだけ。編むファイルを置かない。
  - 権限：機械と装置の値（`N_SCOPE_MACHINE`・`N_SCOPE_DEVICE`）を変えるのは `N_CLASS_ADMIN` 以上。利用者の値は
    本人。身元は `n_channel_peer` で確かめる。
  - 保存は私的な領域 `N_REGION_SETTINGS` の一つの文書で、版つきの二進。頭に二つの版の記録があり、新しい本体を
    古い版の場所へ書いて、媒体へ届いてから記録を書く。壊れていれば前の版へ戻る。
  - 時刻の鍵 `N_TIME_ZONE_MINUTES`（`N_DOMAIN_TIME`）はカーネルに持ち主が無く、libc の `localtime`・`mktime` が読む。
- 見張り（`N_KIND_WATCH`、§1）：見張るものが変わると `N_EVENT_RECEIVABLE`（レベル：
  `n_watch_take(watch)` が取るまで）。`n_watch_take` は前の取り出しの後の変化の数を返し（無ければ
  0、上限 `N_WATCH_CHANGES_MAX`）、数を消す。見張りは開いた時から数える。誰が変えても（その
  プログラム自身、ほかのプログラム、タスクバー、遠隔デスクトップの利用者）知らせ、同じ値を
  当て直すのは変化ではない。待ち行列（§6）に入れて待つ。窓の無い Worker やサービスでも使える。
  見張りを閉じれば、入っていた待ち行列からも消える。開く口は用途ごと：
  - `n_audio_output_watch()`：`n_audio_outputs` が返すものが変わると一通届く channel（§17）。音量と消音は設定の見張りで受ける。
  - `n_screen_watch()`（`SYS_SCREEN_INFO` の `SCREEN_WATCH`、display.h）：`SCREEN_SETTINGS_GET`・
    `struct screen_format` の返事・`SCREEN_MODES_GET` が報告するもの。
  - `n_entry_watch(path, what)`（`SYS_ENTRY_WATCH`、§3）：C: の名前とフォルダー。
  - `n_settings_watch(domain)`（`SYS_SETTINGS_WATCH`）：領域の今の値。
  - `n_power_events_watch(cap)`（`SYS_POWER_EVENTS_WATCH`、§15）：電源の事象。
  - `n_power_windows_watch(cap)`（`SYS_POWER_WINDOWS_WATCH`、§15）：アプリの窓の数と題。
- 言語と地域は領域 `N_DOMAIN_LANGUAGE` の利用者の値で、`nocturne_language.h`（`user/libc/language.c`）が読む。
  `n_locale_now`、`n_language_now`、`n_language_changed`、`n_locale_date`・`time`・`day`・`number`、`n_locale_weekday`・`month`。
  読むプログラムだけが見張りを一つ持ち、読むたびにサービスへ頼まない。文の表はプログラムごとに持つ
  （`words[][N_LANGUAGE_COUNT]`）。設定の宣言の言葉は日本語で、英語は設定の窓の表（`settings_words.h`）が名から引く。
- C の locale：プログラムは `"C"` で始まる。`setlocale(cat, "")` がそのカテゴリーを今の言語と地域にする。名はタグ（`"ja"`、
  `"en-US"`、`"en-GB"`、`"en"`）。効くのは `printf`・`strtod` の小数点、`localeconv`、`strftime` の名と書式。
- `Intl` の既定と `navigator.language` は同じ値から来る。ブラウザーの言語の一覧はプログラムの設定で、空なら
  `n_locale_languages` に従う。
- nocturne/ui（TypeScript・JavaScript）では `desktop.watch(listener)`：`listener` は変わったものの
  名前の配列を受け取る。`"screen"` は `desktop.get()`・`desktop.modes()` が報告するもの（画面の
  見張り）、`"sound"` は `desktop.sound()` が報告するもの（音の見張り）。

## 13. 身元と能力

書式は `common/authority_abi.h`。

- どのプロセスも身元（`struct n_identity`：`user`、`session`、`cls`、`program`）を持つ。起動の時に
  決まり、変わらない。アプリは書けない。`n_identity_self(&out)` で読む。
- 級（`cls`）は `N_CLASS_USER`、`N_CLASS_ADMIN`、`N_CLASS_SYSTEM`。上の級ほど広い。
- 子の身元（`struct n_launch` の `identity`）：
  - NULL なら親と同じ `user`・`session`・`cls`。
  - `cls` は親を超えられない。超えれば `N_ERR_DENIED`。
  - `user`・`session` を親と変えるには、`identity_grant` に `N_CAP_IDENTITY_GRANT` の能力を
    渡す。無ければ `N_ERR_DENIED`。
- プログラムの番号（`program`）はカーネルが決める。起こす文書のパスから作り、パスの書き方と大小によらず、
  起動をまたいで同じ。`identity` の `program` は 0 で渡す（ほかは `N_ERR_INVALID`）。
  - `n_launch` の `flags` の `N_LAUNCH_SAME_PROGRAM`：子を呼ぶ側と同じ番号にする。プログラムの部品（ブラウザーの
    `webfetch`、`browserjsworker`）が親の設定を読むために付ける。呼ぶ側が自分の番号を渡すだけなので、能力は要らない。
  - スクリプトの番号は、それを走らせる解釈系の文書から決まる。
  - この番号は設定の名前の範囲で、同じ利用者のプログラムどうしの権限の壁ではない。
- 触れてよい相手：SYSTEM は全部。ADMIN は SYSTEM 以外。USER は同じ `user` で同じ `session` の USER。
  `n_app_open`、`proclist`、`clipboard_get` がこの規則に従う。
- 能力（`N_KIND_CAPABILITY`）は特権を一つ持つハンドル。持つことが権限の証明で、渡すことが委譲。
  起動の表か、メッセージの添付で渡す。
  - 種類：`N_CAP_ROOT`、`N_CAP_SERVICE_ROOT`、`N_CAP_IDENTITY_GRANT`、`N_CAP_HAL_CLAIM`、`N_CAP_POWER`、
    `N_CAP_SCREEN_READ`、`N_CAP_SETTINGS_APPLY`、`N_CAP_STORE_PRIVATE`、`N_CAP_SHELL`、`N_CAP_NETWORK`、`N_CAP_CLOCK`、`N_CAP_DISPLAY`、`N_CAP_AUDIO_DEVICE`。
  - カーネルが作るのは `N_CAP_ROOT` を一つだけで、最初のプロセスがハンドル `N_INITIAL_ROOT_HANDLE`
    （3）で受ける。最初のプロセスの身元は `user` 0、`session` 0、`N_CLASS_SYSTEM`、`program` 0。
  - `n_cap_derive(cap, kind)`：`N_CAP_ROOT` は全部の種類を、ほかの能力は同じ種類だけを切り出す。
    ほかは `N_ERR_DENIED`。
  - `n_cap_kind(cap)`：種類。`n_cap_find(kind)`：自分が持つその種類のハンドル（無ければ
    `N_ERR_NOT_FOUND`）。`n_caps_list(out)`：自分が持つ能力の全部。
  - 特権の呼出しは種類の合う能力を引数に取る。`N_CAP_ROOT` でも種類が違えば `N_ERR_DENIED`。

| 呼出し | 要るもの |
|---|---|
| `n_hal_claim` | `N_CAP_HAL_CLAIM` |
| `poweroff`、`reboot`、`n_power_request`、`n_power_events`、`n_power_events_watch`、`n_power_windows`、`n_power_windows_watch`、`n_power_say` | `N_CAP_POWER` |
| `n_screen_mode_apply`、`n_screen_arrange` | `N_CAP_DISPLAY` |
| `screen_grab` | `N_CAP_SCREEN_READ` |
| `n_service_register` | `N_CAP_SERVICE_ROOT` |
| `n_shell_*`、`n_power_state`、`n_power_watch` | `N_CAP_SHELL` |
| `n_audio_device_list`・`n_audio_device_open`・`n_audio_device_watch` | `N_CAP_AUDIO_DEVICE` |
| `n_system_log`、`n_system_log_open` の受信 | 級が ADMIN 以上（書くのは誰でも） |
| `SYS_NET_TEST` | 級が ADMIN 以上 |
| `n_net_address_set`、`n_net_routes_set`、`udp_socket_on` | `N_CAP_NETWORK` |
| `n_clock_wall_step`、`n_clock_wall_slew`、`n_rtc_write` | `N_CAP_CLOCK` |

- チャンネルの端の身元：`n_channel_peer` が読む値。
  - `n_channel_pair` の両端は作った者の身元で始まる。カーネルが作る端はシステムの身元で始まる。
  - 端が別のアプリの手に渡ると（起動の表、添付の受信）、受け手が今の身元を超えないときだけ、受け手の
    身元へ下がる。渡した者も端を持ち続けられるので、上がる向きには動かない。報告される身元は、端を
    持つ者の権限を超えない。
- `gui_launch` は普通の `n_app_launch` で、起こすアプリの身元は呼ぶ側と同じ。

## 14. サービス

名前で相手を引く道はこれだけ。カーネルは名前を解かず、覚えない。名前の持ち主はサービスの管理者。

- `n_service_register(cap)`（`N_CAP_SERVICE_ROOT`）：接続の要求を受けるチャンネルを返す。管理者は
  一度に一つで、開いている間は `N_ERR_EXISTS`。閉じれば次の登録が替わる。
- `n_service_connect(name)`：カーネルがチャンネルの対を作り、一方の端を返す。もう一方の端を添付した
  要求を管理者のチャンネルへ送る。管理者がいなければ `N_ERR_NOT_FOUND`。管理者の列が満ちていれば待つ。
  要求の大きさは呼ぶ側に課金する（§4）。
- 要求のメッセージは `struct n_service_request`（`from`：カーネルが書く依頼者の身元、`name_length`）の
  後ろに名前のバイト列が続く形。管理者は要求を次のどれかにする：
  - 受け付ける：同じメッセージをサービスの受け口へ送る（添付の端も一緒に）。
  - 断る：添付の端を閉じる。依頼者のチャンネルは `N_ERR_CLOSED` になる。
- `n_service_listen(name)`：名前 `N_SERVICE_BIND`（`"Nocturne.Service.Bind"`）のサービスへつなぎ、名前を
  本文に、受け口へ送る端を添付にしたメッセージを送る。管理者は `int32_t` を一つ返す（0 か
  `-N_ERR_*`、使われている名前は `-N_ERR_EXISTS`）。成功すると受け口の受ける側を返す。
  - `N_SERVICE_BIND` を受けるのは管理者自身。誰にどの名前を許すか、同じ名前を利用者やセッションで
    分けるかは、管理者が `from` で決める。
  - 受け口へ送る端に `N_EVENT_CLOSED` が来たら、管理者はその名前を消す。
- サービスは `n_inlet_accept(inlet, flags)` で端を受け、`n_channel_peer` で依頼者の身元を読む。

## 15. サービスの管理者、利用者、セッション

型は `common/services_abi.h`。libc の口は `nocturne_services.h`。

- サービスの管理者：`C:\Nocturne\System\services`（`user/apps/services.c`）。カーネルが起こす最初のプロセス。
  宣言は C の型のある表 `declared[]` で、プログラムに組み込む。項目は名前、プログラム、級、範囲（機械か PCI の機能）、
  渡す能力、依存、失敗の扱い（起こし直す回数、間隔、健康とみなす時間）、接続してよい相手。
  起動の順は依存から解き、準備は相手の知らせ（`n_service_ready`）で知る。決まった時間を待たない。
- 宣言の項目 `client_image` は、サービスを使う者が自分の中へ読み込む像の名（`C:\Nocturne\Drivers` の中）で、
  `struct n_service_status` に出る。`gpu` の項目の `nvrm` は特権の半分で、`nvk` が利用者側の半分。libc の Vulkan
  ローダー（`user/libc/vulkan.c`）が一覧からこの名を読み、初めて要るときに写して入口を呼ぶ。一プロセスに一つ。
  呼ぶ者の身元で動くので、NVK は公開の溝と `nvrm` への channel だけを使う。
- 宣言のサービスは handle `N_SERVICE_HANDLE_CONTROL`（3）に管理者との channel を受け、能力は `n_cap_find` で引く。
  受ける向きは受け口で、`n_inlet_accept(N_SERVICE_HANDLE_CONTROL, flags)` が相手の端を返す。
  送る向きは準備の知らせ一通。管理者がこの channel を閉じると `N_ERR_CLOSED` になる。これが止める指示で、
  5 秒の後に `n_app_stop` で止める。
- 名前の範囲：サービスの管理者が起こしたプログラム（`user` が 0）か `N_CLASS_SYSTEM` が申し込んだ名前は機械全体で引ける。
  ほかは申し込んだ者と同じ `user` と `session` の相手にだけ見える。別の利用者は同じ名前を別に申し込める。
  宣言の名前と管理者の名前は申し込めない（`N_ERR_DENIED`）。
- 管理者の口：`n_service_connect(N_SERVICE_MANAGER)` の記録は `struct n_services_request` と `struct n_service_status`。
  一覧、状態、起こし直し、止める、見張る。起こし直しと止めるは `N_CLASS_ADMIN` 以上。
- 利用者：サービス `accounts`。私的な領域に型のある記録で一覧を持つ。合言葉は salt つきの PBKDF2-HMAC-SHA256。
  一人目は誰が作っても admin。`n_user_folder` と `n_temp_folder` は身元から `accounts` が引く。
  新しい利用者のフォルダーの中身の元は `C:\Nocturne\NewUser`。
- セッション：サービス `session`。ログインの窓を出し、選ばれた利用者の身元と `N_CAP_SHELL` でシェル
  `C:\Nocturne\System\shell`（§16）を起こす。シェルが落ちたら起こし直す。画面が無く、自動の利用者も無ければ待つ。
  ログインの窓の文は機械の値 `N_LANGUAGE_LOGIN` に従う。
  `n_session_end`（`N_SESSION_END`）でセッションのプログラムを止め、ログインの窓へ戻る。
- 網：サービス `network`（`user/apps/network.c`）。DHCP（RFC 2131）、固定の住所、経路の表、名前引きと cache を持つ。
  カーネルは adapter の一覧（`n_net_adapters`、`n_net_adapter_name`、見張りは `n_net_watch`）、IP、TCP、UDP を持つ。
  `net_resolve*` は `N_NETWORK_RESOLVE`、`n_network_links` は `N_NETWORK_LINKS` の記録を送る。数字の住所と `localhost` は libc が答える。
  `net_set_setup_cancel_channel` の相手が閉じると、接続を閉じて `N_ERR_CANCELLED` で戻る。同じ名前の要求は一つの問いを待ち、
  待つ者が全部消えると問いは止まる。設定は `N_NETWORK_DNS_SERVER`、`N_NETWORK_FIXED_ADDRESS`（adapter ごと、`struct n_setting_ipv4`）、
  `N_NETWORK_PROXY_HOST`、`N_NETWORK_PROXY_PORT`。proxy は HTTP の接続に効き、平文の HTTP は absolute-form、ほかは CONNECT で通る。
- 時刻：サービス `timesync`（`user/apps/timesync.c`）。SNTP（RFC 4330）で時計と RTC を合わせる。聞く先は DHCP の言う NTP のサーバー、
  無ければ `N_TIME_SERVER`。入切は `N_TIME_SYNC`。差が 128 ms 以上なら step、未満は 500 ppm の slew。カーネルの時計は UTC だけ。
- 機械：サービス `machine`。`N_CAP_POWER` と `N_CAP_SCREEN_READ` を持ち、利用者のアプリの電源と画面の依頼を受ける
  （`n_machine_power`、`n_machine_screen`）。能力をアプリへ配らない。`n_machine_choose(op)` は利用者がメニューで
  選んだ操作で、`N_MACHINE_SLEEP`・`N_MACHINE_HIBERNATE` も受ける。結果を待たずに戻り、段取りは `n_power_state`（§16）で届く。
  電源の方針の持ち主でもある。カーネルは事象を知らせ（`n_power_events`、`struct n_power_events`：ボタンの押下、蓋の変化と今の状態、
  起床、ホストの依頼の累積の数、最後の起床と入力の時刻、眠れるか。`n_power_events_watch` で待つ）、頼まれたことを実行する
  （`n_power_request(cap, action)`）だけ。ボタン、蓋、放置に何で答えるか、眠りと休止を許すかは §12 の `N_DOMAIN_POWER` の鍵。
  窓を閉じる段取りもここにある：`n_power_windows(cap, ask_to_close, title, size)` が窓の数と一番上の窓の題を返し、機械が
  `n_power_windows_watch` で変化を待ち、`n_power_say` で `n_power_state` を言う。窓が一つも閉じないまま `N_POWER_CLOSE_SECONDS` が
  過ぎると取りやめ、そこから `N_POWER_AGAIN_SECONDS` の間にもう一度選ぶと窓を待ち切って止まる。
  規模：一つのスレッドが事象の行列で眠る。接続はスレッドを持たないので、接続が数百あっても待つ間の費用は行列の項目で、依頼一通の
  仕事はその接続だけに触れる。起きるのは事象と放置の期限のときだけで、放置が 0 なら期限を持たない。電源の変更は一度に一つで、
  待つ呼び手の全部に一度答える。

- プログラムの台帳 `programs`（`user/apps/programs.c`）：起こせるプログラムと、文書の種類（小文字の拡張子）ごとの既定を持つ。
  システムのプログラムはサービスの表にある。管理者は `n_program_register` で `C:\Programs` の中のプログラムを足し、
  `n_program_claim` で開ける種類を言う。利用者は `n_program_choose` で自分の既定を選ぶ。種類 `*` はどの種類も開ける。
  名前、パス、一覧の長さは決まっていない。要求も応答も一通で、形は `common/services_abi.h`。
  `n_program_open_document(path)` が文書を既定のプログラムで開く。
- セッションの終わり：`session` はシェルが終わった後に `n_session_stop(N_CAP_IDENTITY_GRANT の cap, session)` を呼び、
  そのセッションの身元のアプリを全部止める。親が先に終わったアプリもここで止まる。
- `n_processes(&list)` はプロセスの一覧を全部読む。`n_procinfo` の `app` はその thread のアプリ、`user`、`session`、`cls` はアプリの身元。

## 16. シェル

シェルは普通のプロセス（`user/apps/shell.c`）で、セッションが利用者の身元で起こす。タスクバー、開始メニュー、音量、通知、
デスクトップの絵と印を持つ。文は言語の設定に従う表（`shell_words.h`）で持つ。窓の合成と管理はカーネル（`gui/wm.c`）が持つ。カーネルは文を言わず、絵を描かない。
型は `common/display_abi.h`。`n_shell_*` は第一引数に `N_CAP_SHELL` の能力を取り、無ければ `N_ERR_DENIED`。

- `n_shell_place(cap, win, &place)`：自分の枠なしの窓（`WIN_NO_DECOR`）を置く。枠つきの窓は `N_ERR_INVALID`。
  - `layer`：下から `N_LAYER_DESKTOP`、`N_LAYER_NORMAL`（アプリの窓）、`N_LAYER_PANEL`、`N_LAYER_OVERLAY`。
    `N_LAYER_PANEL` の窓は焦点を取らず、同じ画面の全画面の窓に焦点がある間は描かれない。
  - `screen`、`x`、`y`：画面の番号と、その画面の中の位置。無い画面は `N_ERR_NOT_FOUND`。
  - `reserve_edge`（`N_EDGE_*`）と `reserve`：その画面の縁を、窓がある間だけ `reserve` 画素取っておく。
    アプリの窓の最大化はその残りの中に収まる。窓の領域が変わると開いた窓を入れ直す。
  - `flags`：`N_PLACE_NO_FOCUS` は焦点を取らない。`N_PLACE_BLEND` は窓の画素の alpha で下の絵に混ぜる。
- `n_shell_screens(cap, out, max)`：画面の数を返し、`struct n_shell_screen` を `max` まで書く。電源の切れた画面は `on` が 0。
- `n_shell_windows(cap, out, max)`：アプリの枠つきの窓で、一度出たものの数を返し、古い順に `struct n_shell_window`
  （`id`、アプリの番号、`N_SHELL_WINDOW_MINIMIZED`・`FOCUSED`・`FULLSCREEN`、画面、題）を `max` まで書く。
- `n_shell_window(cap, id, op)`：`N_SHELL_RAISE`（元に戻して焦点を渡す）、`N_SHELL_MINIMIZE`、`N_SHELL_CLOSE`（`EV_CLOSE` を送る）。
- `n_shell_watch(cap)`：窓、窓の順、焦点が変わると知らせる watch（`n_watch_take`）。
- `n_shell_hotkey(cap, win, usage, mods)`：鍵（HID の usage、§9）と修飾の組を `win` が受ける。配列に左右されない。`mods` は `NMOD_SHIFT`・`NMOD_CTRL`・`NMOD_ALT`。
  その組はアプリへ届かず、`win` へ `EV_HOTKEY` で届く。組が既に取られていれば `N_ERR_EXISTS`。窓が閉じると組も消える。
- 層のある窓はポインターが離れると `EV_MOUSE_LEAVE` を受ける。

電源の段取りはカーネルが型のある値で言い、文にしない。シェルが文にする。

- `n_power_state(cap, &state)`：`struct n_power_state`（`serial`、`stage`、`action`、`reason`、`forceable`、`can_sleep`、
  `can_hibernate`、`detail`）。`stage` は `enum n_power_stage`、`reason` は `enum n_power_reason`。`detail` は窓の題か装置の名。
  窓の題を持つので `N_CAP_SHELL` だけが読める。
- `n_power_watch(cap)`：`n_power_state` の変更を知らせる watch。
- 閉じない窓があれば機械のサービス（§15）が `N_POWER_CLOSING` を言い、`forceable` ならもう一度の選択で強制する。アプリの電源の変更は機械のサービスを通す。

通知はサービス `N_SHELL_SERVICE`（`"shell"`）のチャンネルで受ける。名前の範囲は同じ利用者の同じセッション（§14）。

- `n_shell_notify(title, text)`：`struct n_shell_notice` を一通送る。シェルは数秒の間それを出す。シェルの音量の表示は §12 の設定を読み、変更は `n_setting_set_int` で設定のサービスへ頼む。

## 17. 音

型は `common/audio_abi.h`。カーネルは音の方針を持たない。混合、変換、音量、消音、出口と形式の選択はサービス `audio`（`user/apps/audio.c`）が持つ。

### アプリの流れ

- `n_audio_open(const struct n_audio_format *, n_audio_stream **)`：形式を宣言して流れを開く。率は 0 より大きい Hz、channel は 1 以上、標本は `N_AUDIO_S16`・`N_AUDIO_S32`・`N_AUDIO_F32`。channel の順は WAVE と同じ。出口と数が違えば左右へ混ぜ、名前の無い位置は混ぜない。流れの数に上限は無く、記憶が無ければ `N_ERR_NO_MEMORY`、輪の大きさが表せなければ `N_ERR_TOO_LARGE`。
- `n_audio_submit(s, frames, count, flags)`：共有の輪へ写す。受け取った frame の数を返す。流れは約 170 ms を持つ。`N_AUDIO_WAIT` なら空きを待ち、無ければ満杯で `N_ERR_NOT_READY`。続けて鳴る流れにはシステムコールが無い。眠った流れだけ `N_AUDIO_OP_KICK` を一通送る。
- `n_audio_status(s, struct n_audio_status *)`：輪から読む。待っている frame、出口へ渡した frame、再生済みとそれを聞いた時刻、世代、音切れ、`flags`（`N_AUDIO_CLOCK_DEVICE`、`N_AUDIO_CLOCK_ESTIMATED`、`N_AUDIO_CLOCK_REMOTE`）、`pace_ppb`。流れは出口の水晶によらず monotonic の時計で進む。音切れは、音を全部聞かせた後に次の音が来たこと。
- `n_audio_flush(s)`：待っている音を捨て、世代を進める。出口がまだ読まない塊まで全部の流れを戻し、他の流れだけで混ぜ直す。遠隔の相手へ送った分は取り戻せない。
- `n_audio_close(s)`：残りを鳴らし終えてから閉じる。アプリが落ちて接続が切れても、サービスが残りを鳴らして流れを消す。
- サービスが死ぬと、次の呼出しで接続し直し、まだ混ぜていない音を新しい輪へ写し、数と世代を続ける。死の確かめは、輪が動かない時と満杯の時だけ。その間、driver は止まらない。

### 出口と設定

- `n_audio_outputs(struct n_audio_output **)`：出口の並びと、今の決め（`playing`、`format`）。一件は `bytes` の長さで、`n_audio_output_device` が申告を指す。`n_audio_output_name()` は鳴らしている出口の名前。`n_audio_output_watch()` は変化ごとに 1 バイトが届く channel で、`n_audio_output_watch_take` で取る。
- 選択：`N_AUDIO_OUTPUT` の key の出口が使えればそれ。無ければ遠隔の相手、刺さった出口、刺さり方の分からない出口の順で、同じなら HEADPHONE、SPEAKER、LINE、DISPLAY、DIGITAL、OTHER の順。同点は今のものを続ける。
- 形式：`N_AUDIO_FORMAT_*`（`id.device` は `n_setting_device(key)`）が申告にあればそれ。無ければ 48 kHz に一番近い率、一番大きい標本、一番多い channel。申告に無い値は `N_ERR_INVALID` で断る。
- 宣言：サービスが起動のたびに宣言し、接続を持ち主にする。settings はその時に保存された値を当て直す。利用者が替わると、その人の値か既定を当てる。音量を変えても消音は解けない。
- 接続は流れごとか見張りごとで、音の無い流れは活動中の列に居ない。流れが数百あっても、拍の費用は鳴っている流れの数で決まる。全部が静かになって 1 秒たつと装置を止め、拍が止まる。一度も鳴らなければ出口を開かない。

### カーネルの口（`N_CAP_AUDIO_DEVICE`）

能力はサービス audio にだけ渡す。

- `n_audio_device_list(cap, out, bytes)`：全部のバイト数を返す。足りなければ何も書かない。一件は `struct n_audio_device`（`bytes`、id、種類、刺さり方、channel の範囲）の後に率、大きさ、name、key が続き、`n_audio_device_rates`・`n_audio_device_sizes`・`n_audio_device_name`・`n_audio_device_key` で読む。
- `n_audio_device_open(cap, id, format, ring)`：申告にある形式で開く。`ring` は望みの `block_frames` と `blocks` を渡し、答えは装置が取れる形（High Definition Audio は記述子 2〜256 個、塊は 128 バイトの倍数。AC'97 は記述子 32 個）。輪の共有 object を `ring->memory` で返す。開けるのは一つの出口に一つで、同じ controller の別の出口が開いていれば `N_ERR_BUSY`。ハンドルを閉じると装置は止まる。
- `n_audio_device_run(device, run)`：輪の始めから流す、止める。リンクが下りていれば `N_ERR_NOT_READY`。周期の知らせでまた見る。
- `n_audio_device_status(device, status)`：位置と時刻の組、`guard_frames`（ここまでは装置が読んだか読みかけ）、世代（電源の変化で輪の中身が失われると増える）、流れの誤りの累積。塊の終わりごとに handle が `N_EVENT_RECEIVABLE` になり、読むと消える。
- `n_audio_device_watch(cap)`：一覧の見張り（`N_KIND_WATCH`）。
- 新しい音の装置（USB Audio など）は `kernel/src/dev/audio.h` の `struct audio_output` を作り `audio_output_add` するだけで、サービスとアプリは変わらない。
