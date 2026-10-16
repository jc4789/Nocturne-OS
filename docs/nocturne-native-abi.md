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
  - `n_document_load(path, n_shared *out)`：内容を共有ページで受け取る。
  - `n_document_save(path, data, size)`：一時領域へ書いてから原子的に差し替える。
    書きかけの文書が見えることはない。
  - `n_document_append(path, data, size)`：ログ用の追記。無ければ作る。
- 部分アクセス（大きい媒体、DB）：
  - `n_document_open(path, n_document_access)`。`access` はビットフラグではなく列挙：
    `N_DOCUMENT_READ`、`N_DOCUMENT_EDIT`（既存の読み書き）、
    `N_DOCUMENT_CREATE`（新規作成。既に有れば `N_ERR_EXISTS`）、
    `N_DOCUMENT_REPLACE`（空の新しい内容で作り、閉じた時点で原子的に差し替え）。
  - `n_document_read_at(h, offset, buf, n)`、`n_document_write_at(h, offset, buf, n)`、
    `n_document_size(h)`、`n_document_set_size(h, size)`。**共有カーソル（シーク位置）は無い**。
  - 排他：`n_document_claim(h, wait)`／`n_document_release(h)`（現行のリースを継承）。
- 情報と操作：`n_entry_info(path, struct n_entry *)`、`n_folder_create(path)`、
  `n_entry_remove(path)`（空でないフォルダーは `N_ERR_NOT_EMPTY`）、
  `n_entry_move(from, to)`、`n_folder_list(path, struct n_entry *out, max, start)`。

## 4. チャンネル（`pipe`／標準入出力fd の置き換え）

- チャンネルは**メッセージ単位**の双方向路。一回の送信が一つのメッセージになり、
  受信側は境界を保ったまま受け取る。大きな塊は共有ページを添付して送る。
- `n_channel_pair(n_handle out[2])`：無名の対（現行 `pipe` の置き換え）。
- 名前付きの合流点は現行の `n_channel_listen`／`connect`／`accept` を引き継ぐ。
- `n_channel_send(h, data, n)`、`n_channel_receive(h, buf, cap, flags)`
  （`N_CHANNEL_WAIT` で待つ。待たない場合は `N_ERR_NOT_READY`）、
  `n_channel_attach(h, shared)`／`n_channel_take(h)`（共有ページの受渡し）。
- 端末は、子アプリの「入力」「出力」チャンネルとメッセージをやり取りする一つの
  アプリにすぎない。端末であるかの判定（`isatty`）は無い。ISO Cの `stdout`／`stderr`／
  `stdin` は、起動時に渡された出力／入力チャンネル（無ければ記録先のログ）に結び付く。

## 5. アプリ（`spawn`／`waitpid`／`kill`／`getpid` の置き換え）

- `n_app_launch(path, const struct n_launch *)` がアプリハンドルを返す。
  `n_launch` は引数列、設定の組（ISO Cの `getenv` が読む。`environ` は無い）、
  引き渡すハンドルの表（入力・出力・ログのチャンネル、共有ページ等）を持つ。
- 終了はアプリハンドルのイベント（§6）で届く。`n_app_result(h, timeout_ms, int *status)` で
  直接待つこともできる。ゾンビの回収は無く、最後のハンドルを閉じれば記録は消える。
- `n_app_stop(h, N_APP_STOP_TREE か 0)`：停止要求。シグナルは無い。
- `n_app_self()`、`n_app_parent()`：アプリ識別子（共有ページの受渡しの許可判定用）。
- `n_app_list`、`n_system_info` 等の状態取得は現行の `proclist`／`sysinfo` を引き継ぐ。

## 6. イベント待ち行列（`poll`／`O_NONBLOCK`／`fcntl` の置き換え）

- `n_events_create()` が待ち行列を作る。各スレッドは任意の数を持てる。
- `n_events_watch(q, h, interest, token)`：ハンドルの関心事を登録。
  `N_EVENT_RECEIVABLE`、`N_EVENT_SENDABLE`、`N_EVENT_CLOSED`、`N_EVENT_APP_ENDED`、
  `N_EVENT_WINDOW_INPUT`、`N_EVENT_CONNECTION`、`N_EVENT_TIMER`。
- 現行のアドレス監視（`n_address_watch`）はこの待ち行列へ統合する。
- `n_events_wait(q, struct n_event *out, max, timeout_ms)`：届いた分を一括で返す。
- タイマー：`n_events_timer(q, ms, repeat, token)`。

## 7. メモリ（`sbrk`／`mmap`／`mprotect` の置き換え）

- `n_memory_reserve(bytes, n_memory_access)`：ゼロ初期化の領域を確保。
- `n_memory_release(addr, bytes)`、`n_memory_protect(addr, bytes, access)`。
- `access` は列挙：`N_MEMORY_READ`、`N_MEMORY_READ_WRITE`、`N_MEMORY_READ_EXECUTE`。
  書込みと実行の同時許可は無い（W^X）。
- 共有ページ（`n_shared_*`）は現行のまま。`malloc` は `n_memory_reserve` の上に作る。

## 8. 時間

- `n_clock_uptime_ms()`、`n_clock_wall()`（壁時計の秒）、`n_sleep_ms(ms)`。
- `gettimeofday`、`struct timeval` は無い。ISO Cの `time`／`timespec_get`／`localtime` は
  これらの上に実装する。

## 9. 装置

- 音声、PCI一覧、カーネルログ等の `/dev` ファイルは、種類つきハンドルと専用の関数へ移す
  （例：`n_audio_open`、`n_audio_submit`、`n_audio_status`）。ファイルとして読み書きしない。

## 10. 移行の段階

1. この文書。新システムコールを旧ABIと並べてカーネルへ追加し、`nocturne.h` に宣言する。
2. libc（stdio、malloc、time、getenv）を新ABIへ移す。
3. OS自身のアプリ（ブラウザー、端末、シェル、ファイル、coreutils 等）を新ABIへ移す。
4. 移植物のソースを新ABIへ直接書き換える。tcc がゲスト内でコンパイルする
   プログラム用のヘッダーも新ABIだけにする。
5. 旧システムコールと `fcntl.h`／`unistd.h`／`sys/*.h`／`alloca.h`、`O_*`、`F_*`、
   `N_POLL*`、POSIX型（`ssize_t`、`pid_t`、`off_t`、`mode_t`）を削除する。

各段階で `make qemu-image` によるビルドと、QEMU実画面でのブラウザー・端末・
ファイル操作の確認を行う。旧ABIの削除は、全利用箇所が新ABIへ移った後に限る。
