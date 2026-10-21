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
  `N_ERR_INVALID`（`.` と `..` の要素はシステムが解決する）。シェルは自分の
  「現在のフォルダー」を持ち、起動するアプリへ設定 `FOLDER=` として渡す（§5）。
- 利用者が打った名前は `n_path_complete(name, out, cap)` で完全指定にする：`/` で
  始まらない名前は設定 `FOLDER` の中（無ければ `/`）の名前とする。長さを返し、
  収まらなければ `N_ERR_TOO_LARGE`。ISO Cの `fopen`／`remove`／`rename` もこれを使う。
- アプリが自分の現在のフォルダーを変えるのは `n_folder_set(name)`：名前を完全指定にし、
  フォルダーであることを確かめてから、自分の設定 `FOLDER` を置き換える（以後の
  `n_path_complete` と、libcが起動するアプリへ渡す `FOLDER` に効く）。
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
- カーネルに作業ディレクトリは無い。`FOLDER` が無ければ `n_path_complete` は `/` を基準にする。

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
- Ctrl+C：端末はシェルにハンドル3として「割込み」チャンネルを渡し（設定
  `INTERRUPTS=3`）、Ctrl+C ごとにメッセージを一つ送る。シェルは前面のジョブを
  `n_app_stop(h, N_APP_STOP_TREE)` で止め、プロンプトでは入力中の行を取り消す。
  シグナルは無い。
- システムログ：`n_system_log_open()` は、送ったメッセージがシステムログ（とシリアル
  ポート）へ加わるハンドルを返す。`n_system_log(buf, cap)` は最新のログを読む。

## 5. アプリ（`spawn`／`waitpid`／`kill`／`getpid` の置き換え）

- `n_app_launch(path, const struct n_launch *)` がアプリハンドルを返す。
  `n_launch` は引数列、設定の組（ISO Cの `getenv` が読む。`environ` は無い）、
  引き渡すハンドルの表（入力・出力・ログのチャンネル、共有ページ等）を持つ。
  - 設定は `"名前=値"` の形（`=` が無ければ `N_ERR_INVALID`）。起動されたアプリでは
    `n_settings`（NULL終端）として読め、`getenv` もこれを引く。
  - 引き渡すハンドルは、表の順に子アプリの 0, 1, 2, … 番になる（-1 は空き）。
  - パスは完全指定。起動されたアプリは作業フォルダーを持たない。利用者の名前を
    受け取るアプリのために、起動元は自分の現在のフォルダーを設定 `FOLDER=/…` で渡す
    （§3の `n_path_complete`）。
- 終了はアプリハンドルのイベント（§6）で届く。`n_app_result(h, timeout_ms, int *status)` で
  直接待つこともできる（-1 は無期限、0 は待たずに `N_ERR_NOT_READY`、期限切れは
  `N_ERR_TIMEOUT`）。ゾンビの回収は無く、最後のハンドルを閉じれば記録は消える。
- `n_app_stop(h, N_APP_STOP_TREE か 0)`：停止要求。シグナルは無い。
- `n_app_self()`、`n_app_parent()`：アプリ識別子（共有ページの受渡しの許可判定用）。
- `n_app_open(id)`：アプリ一覧の識別子から、動作中のアプリのハンドルを得る（タスク
  マネージャーや `kill` が `n_app_stop` するため）。`n_app_id(h)`：ハンドルの識別子
  （終了後は0）。
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
- 共有ページ：`n_shared_create(bytes)` は確保済み、`n_shared_reserve(bytes)` は最初に
  触れたページから割り当てる共有オブジェクトのハンドルを返す。`n_shared_map(h, bytes,
  N_MEMORY_READ | N_MEMORY_READ_WRITE)` は自分が持つハンドルだけを写す（NULL＋`errno`）。
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

- `n_clock_uptime_ms()`、`n_clock_wall()`（壁時計の秒）、`n_sleep_ms(ms)`。
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
- アプリの中の装置ドライバー（例：NVIDIA の資源管理 `/bin/nvrm`、`docs/nvidia/LEDGER.md`）：
  - `n_pci_claim(bus, dev, func)` は PCI 機能を動かす権利（`N_KIND_PCI`）を返す。呼べるのは
    init だけで、init は起動するドライバーアプリへハンドルを渡す。カーネルのドライバーが使う
    機能と、既に権利のある機能は `N_ERR_BUSY`。起動オプション `noappdrivers` で全部断る。最後のハンドルが閉じると、カーネルは
    (1) その権利の BAR オブジェクトの写像を全アプリから外し（その範囲は予約のまま、触れると
    全 1 が読める読取り専用のページ。バスから消えた装置と同じ）、(2) 割り込みとバスマスターを
    止め、(3) 発行済みの DMA が終わるまで待つ（PCIe の Transactions Pending、上限 200 ms）。
    終わったと確かめられない権利の DMA 用メモリは、解放せずに隔離する。アプリの終了時は
    権利のハンドルを他のハンドルとマッピングより先に閉じる。BAR オブジェクトのビュー・写像・
    DMA アドレスの照会は、権利が終わった後は `N_ERR_CLOSED`。
  - `n_pci_info` は ID、BAR、MSI／MSI-X の数、上流のブリッジ、起動時の画面がその機能の BAR に
    あるならその位置（ドライバーはそこを壊さない）を返す。
  - 構成空間：読みはホストから自分までの経路だけ。自分と同じ装置の他の機能、上流の各ブリッジと
    その装置の他の機能（ルートポートが機能 1 以降で、機能 0 がホストブリッジの機械がある。
    ドライバーは機能 0 が答えない装置を飛ばす）、ホストブリッジ（チップセットの識別）。無関係な装置は
    `N_ERR_DENIED`。書きは自分だけで、BAR、
    拡張 ROM、MSI／MSI-X の機能はカーネルだけが書く（`N_ERR_DENIED`）。例外は、リセットで消えた
    BAR へ、カーネルが起動時に見た値をそのまま 32 ビットで書き戻すことだけ（戻せるが、動かせない）。
    一部のビットだけがカーネルのものであるレジスターは、そのビットを変える書込みを `N_ERR_DENIED`
    にする（読んで一部を変えて書き戻すなら通る）：Command の MSE・BME・パリティ応答以外（I/O、
    INTx 無効化など）、BIST の開始、Interrupt Line、PMCSR の電源状態（D0 へ戻すことは可）、
    PCIe Device Control の Max_Payload_Size・Phantom Functions・FLR、Link Control の Link Disable、
    Link Control 2 の Compliance、拡張機能の Resizable BAR（VF 用も）の大きさ、SR-IOV の制御と
    NumVFs、ACS・ATS・PRI・PASID の制御。ブリッジ（クラス 06h）は claim できない。
  - ドライバーは装置を止め終えたらバスマスターを切ってから手放す。claim の終わりにバスマスターが
    入ったままなら、ドライバーは途中で終わった（異常終了など）とみなし、装置上のファームウェアが
    古いアドレスを持ったまま止まっていない可能性があるので、その装置の DMA 用 RAM は二度と
    アロケーターへ戻さない（ログに出る）。
  - `n_pci_hold_screen(pci, hold)`：起動時の画面がその機能の BAR にあるとき、ドライバーが
    CPU から画面への経路を作り直す間（NVIDIA の RM は GSP が BAR1 を仮想モードにする間）、
    システムは画面へ書かない。`true` は書込み中のものが終わってから戻り、入れ子にできる。最後の
    `false`（または claim の終わり）で画面全体を描き直す。保持には時間の上限があり（起動項目
    `fbhold=<秒>`、既定 30、0 で無制限）、過ぎると描画を再開してログに出す。画面が別の所なら
    `N_ERR_NOT_FOUND`。パニック画面は保持に関係なく描く。
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
    （VT-d は互換形式の割り込みを拒み、IOAPIC とカーネルの装置の MSI も表を通る）。claim の終了で
    装置は遮断され、ユニットが確かめた後は BME を残した終了でも DMA の RAM は割り当て器へ戻る。
    従来型 PCI のブリッジの後ろなどで別の装置と要求元 ID を共有する装置の claim は `N_ERR_BUSY`。
    隔離されている装置は `n_pci_info` の `flags` に `N_PCI_ISOLATED`。IOMMU が無い機械と起動
    オプション `noiommu` では従来どおり全メモリへ届き、claim のログでそう知らせる。
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
再利用しない。残る番号は意味を変えない（`/data/bin` のOS内で作ったプログラムのため）。
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

- C と `/bin/js`（TypeScript／JavaScript）は同じプロセス内で `ui_session_send`／
  `ui_session_peek` を呼ぶ。
- C を呼べないプログラム（Go 等）は、メッセージチャンネルの一端をハンドル3として
  `/bin/uihost` を起動し、同じバイト列をチャンネルの一メッセージずつ送受信する。
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
