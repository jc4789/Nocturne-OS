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

## 3. 文書とフォルダー（`open`／`lseek`／`readdir`／`chdir` の置き換え）

- 作業ディレクトリはない。パスは常に `/` からの完全指定。相対パスは
  `N_ERR_INVALID`。シェルは自分の「現在のフォルダー」を持ち、起動前に解決する。
- 丸ごとの操作（通常の用途）：
  - `n_document_load(path, void **data, size_t *size)`：内容を新しい `n_memory` ページで
    受け取る。`n_memory_release(*data, *size)` で返す。
  - `n_document_save(path, data, size)`：新しい内容を別に用意してから原子的に差し替える
    （RAM：内容の付け替え、FAT：新しいクラスター列を書いてから目録の一項目を切替え）。
    書きかけの文書が見えることはない。
  - `n_document_append(path, data, size)`：ログ用の追記。無ければ作る。追記の間は
    文書の排他を持つので、同時に追記しても互いを上書きしない。
- 部分アクセス（大きい媒体、DB）：
  - `n_document_open(path, n_document_access)`。`access` はビットフラグではなく列挙：
    `N_DOCUMENT_READ`、`N_DOCUMENT_EDIT`（既存の読み書き）、
    `N_DOCUMENT_CREATE`（新規作成。既に有れば `N_ERR_EXISTS`）、
    `N_DOCUMENT_REPLACE`（空の新しい内容をメモリー上に作る）。差し替えは
    `n_handle_close` で明示的に閉じた時だけ行う。閉じずに終了したアプリの内容は捨てられ、
    元の文書はそのまま残る。
  - `n_document_read_at(h, offset, buf, n)`、`n_document_write_at(h, offset, buf, n)`、
    `n_document_size(h)`、`n_document_set_size(h, size)`。**共有カーソル（シーク位置）は無い**。
    `N_DOCUMENT_READ` のハンドルへの書込みは `N_ERR_DENIED`。
  - 排他：`n_document_claim(h, wait)`／`n_document_release(h)`（現行のリースを継承）。
    待たない場合に他が持っていれば `N_ERR_NOT_READY`。
- 情報と操作：`n_entry_info(path, struct n_entry *)`、`n_folder_create(path)`、
  `n_entry_remove(path)`（空でないフォルダーは `N_ERR_NOT_EMPTY`）、
  `n_entry_move(from, to)`、`n_folder_list(path, struct n_entry *out, max, start)`（件数を返す）。
- `n_entry_move` は既存の名前を決して上書きしない（`N_ERR_EXISTS`）。同じ装置内は
  名前の付け替え、装置をまたぐ文書は丸ごと複写してから元を消す。装置をまたぐフォルダーは
  `N_ERR_UNSUPPORTED`。フォルダーを自分の中へは移せない（`N_ERR_INVALID`）。
- ISO Cの `stdio` は文書ハンドルの上で自分の位置を持つ：`"r"`＝`READ`、`"r+"`＝`EDIT`、
  `"w"`＝既存なら `EDIT` で長さ0にし、無ければ `CREATE`（ISO Cの切り詰め）、`"wx"`＝`CREATE`、
  `"a"`＝`EDIT`／`CREATE` で、書出しのたびに排他を取って末尾へ書く。`fseek`／`ftell` は
  文書だけ（チャンネルは `N_ERR_WRONG_KIND`）。`remove`＝`n_entry_remove`。`rename` は
  `n_entry_move`、既存の文書が相手なら内容を丸ごと原子的に差し替えてから元を消す。
  `tmpfile` は `/tmp` に作り、閉じるか終了で消す。標準の出力・記録が文書なら末尾へ追記する。
- 移行中（段階3まで）、相対の名前はlibcがアプリの旧作業ディレクトリを前に付けて
  完全指定にしてから渡す。

## 4. チャンネル（`pipe`／標準入出力fd の置き換え）

- チャンネルは**メッセージ単位**の双方向路。一回の送信が一つのメッセージになり、
  受信側は境界を保ったまま受け取る。大きな塊は共有ページを添付して送る。
- `n_channel_pair(n_handle out[2])`：無名の対（現行 `pipe` の置き換え）。
- 名前付きの合流点は現行の `n_channel_listen`／`connect`／`accept` を引き継ぐ。
- `n_channel_send(h, data, n, attach)`、`n_channel_receive(h, buf, cap, flags, &attached)`。
  - `N_CHANNEL_WAIT` で待つ。待たない場合は `N_ERR_NOT_READY`。相手が閉じて
    残りも無ければ `N_ERR_CLOSED`。
  - `N_CHANNEL_PEEK` は次のメッセージの大きさだけを返し、取り出さない。
    `cap` より大きいメッセージは `N_ERR_TOO_LARGE` で、待ち行列に残る。長さ0の
    メッセージも一つのメッセージとして届く。
  - `attach` は文書または共有ページのハンドル（無ければ -1）。メッセージと一緒に運ばれ、
    受信側で新しいハンドルとして `attached` に入る（無ければ -1）。チャンネル自身を
    運ぶと参照の循環になるので `N_ERR_WRONG_KIND`。
- 移行中（段階5まで）、旧来のバイト列のパイプと、アプリの入出力に渡される文字装置
  （コンソール、カーネルログ、null）にも `n_channel_send`／`receive` が使える。
  ただし境界と添付は無く、`N_CHANNEL_PEEK` は `N_ERR_UNSUPPORTED`。
- ISO Cの入力は、`cap` より大きいメッセージを `N_CHANNEL_PEEK` で測り、バッファーを
  広げて丸ごと受け取る。長さ0のメッセージは読む内容が無いので次を待つ。
- 端末は、子アプリの「入力」「出力」チャンネルとメッセージをやり取りする一つの
  アプリにすぎない。端末であるかの判定（`isatty`）は無い。ISO Cの `stdout`／`stderr`／
  `stdin` は、起動時に渡された出力／入力チャンネル（無ければ記録先のログ）に結び付く。

## 5. アプリ（`spawn`／`waitpid`／`kill`／`getpid` の置き換え）

- `n_app_launch(path, const struct n_launch *)` がアプリハンドルを返す。
  `n_launch` は引数列、設定の組（ISO Cの `getenv` が読む。`environ` は無い）、
  引き渡すハンドルの表（入力・出力・ログのチャンネル、共有ページ等）を持つ。
  - 設定は `"名前=値"` の形（`=` が無ければ `N_ERR_INVALID`）。起動されたアプリでは
    `n_settings`（NULL終端）として読め、`getenv` もこれを引く。
  - 引き渡すハンドルは、表の順に子アプリの 0, 1, 2, … 番になる（-1 は空き）。
  - パスは完全指定。起動されたアプリは作業フォルダーを持たない。
- 終了はアプリハンドルのイベント（§6）で届く。`n_app_result(h, timeout_ms, int *status)` で
  直接待つこともできる（-1 は無期限、0 は待たずに `N_ERR_NOT_READY`、期限切れは
  `N_ERR_TIMEOUT`）。ゾンビの回収は無く、最後のハンドルを閉じれば記録は消える。
- `n_app_stop(h, N_APP_STOP_TREE か 0)`：停止要求。シグナルは無い。
- `n_app_self()`、`n_app_parent()`：アプリ識別子（共有ページの受渡しの許可判定用）。
- `n_app_list`、`n_system_info` 等の状態取得は現行の `proclist`／`sysinfo` を引き継ぐ。

## 6. イベント待ち行列（`poll`／`O_NONBLOCK`／`fcntl` の置き換え）

- `n_events_create()` が待ち行列を作る。各スレッドは任意の数を持てる。
- `n_events_watch(q, h, interest, token)`：ハンドルの関心事を登録。
  `N_EVENT_RECEIVABLE`（メッセージ、窓の入力、接続要求、受信データ）、`N_EVENT_SENDABLE`、
  `N_EVENT_CLOSED`、`N_EVENT_APP_ENDED`。同じハンドルと同じ token の再登録は関心事の変更。
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

- `n_memory_reserve(bytes, n_memory_access)`：ゼロ初期化の領域を確保。
- `n_memory_release(addr, bytes)`、`n_memory_protect(addr, bytes, access)`。
- `access` は列挙：`N_MEMORY_READ`、`N_MEMORY_READ_WRITE`、`N_MEMORY_READ_EXECUTE`。
  書込みと実行の同時許可は無い（W^X）。
- 確保時だけ `access` に `N_MEMORY_LOW` を足せる：2GiB未満の低位領域（1GiB〜2GiB、
  プログラム像がそれより大きければ像の直後から）に置く。命令に符号拡張32ビットの
  絶対アドレスを持つ生成コード（`tcc -run` が結合する非PICのlibc等）のため。
  低位領域は通常領域と別の空き一覧・境界を持ち、解放・保護は同じ関数で行う。
- 共有ページ（`n_shared_*`）は現行のまま。`malloc` は `n_memory_reserve` の上に作る：
  ヒープは領域の並び（各領域は先頭ブロックと終端標識を持ち、領域をまたいで結合しない）で、
  領域の大きさはヒープの1/16（256KiB〜8MiB）。最新の領域だけ末尾の空きを一つ分温存し、
  それ以外の領域の末尾の空きはページ単位で返し、全部空いた古い領域は丸ごと返す。
  1MiB以上のブロックは専用の領域で、解放すれば即座に返る。

## 8. 時間

- `n_clock_uptime_ms()`、`n_clock_wall()`（壁時計の秒）、`n_sleep_ms(ms)`。
- `gettimeofday`、`struct timeval` は無い。ISO Cの `time`／`timespec_get`／`localtime` は
  これらの上に実装する。

## 9. 装置

- 音声、PCI一覧、カーネルログ等の `/dev` ファイルは、種類つきハンドルと専用の関数へ移す
  （例：`n_audio_open`、`n_audio_submit`、`n_audio_status`）。ファイルとして読み書きしない。

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
