# Nocturne 用 Rust 移植（x86_64-unknown-nocturne）

Rust 1.98.1（コミット `48a229ceaefd4985c50990b14116b6d856af0985`）の標準ライブラリーに
`target_os = "nocturne"` を足す差分です。コンパイラー本体は変えません。Mesa の Rust
コード（NAK シェーダーコンパイラー、`nil`）を Nocturne 向けに作ることが目的で、
Nocturne のアプリをRustで書くためにも使えます。Rust は MIT または Apache-2.0 です。
標準ライブラリーが入った実行ファイルには、初期 RAMfs の `/usr/share/licenses/rust`
（`COPYRIGHT`、`LICENSE-APACHE`、`LICENSE-MIT`）を添えます。

Nocturne は UNIX でも POSIX でもないので、`target_family = "unix"` も `std::os::unix` も
`libc` クレートも使いません。標準ライブラリーは Nocturne の C ライブラリー（ISO C の
`malloc`、`exit`、`getenv`、`timespec_get`、`fwrite` と、ネイティブ API の `n_*`）を
`extern "C"` で直接呼びます（`library/std/src/sys/pal/nocturne/abi.rs`）。他の OS の
ソースは参照していません。

## 作り方

```
ports/rust/setup-src.sh      # rust-src 1.98.1 を tools/rust/src に展開して nocturne.patch を当てる
ports/rust/build-std.sh      # 標準ライブラリーを作り、tools/rust/sysroot に置く
make                         # tools/rust/sysroot があれば ports/rust/apps/* が /bin/<名前> になる
```

- 安定版の `rustc` / `cargo` 1.98.1 が要ります。`RUSTC_BOOTSTRAP=1` で `-Zbuild-std` を使います
  （スクリプトが設定します）。ネットワークは要りません（依存クレートは rust-src の `library/vendor`）。
- `build-std.sh` の結果は `tools/rust/target`（cargo のビルド）と `tools/rust/sysroot`
  （`lib/rustlib/x86_64-unknown-nocturne/lib/*.rlib`）です。`tools/` は git の管理外です。
- 一つのプログラムだけ作るとき：
  `ports/rust/rustc-nocturne.sh ports/rust/apps/rusttest/main.rs build/rust/rusttest`
  （直前の `make` が作った `build/u/user/libc/crt0.asm.o` と `build/libc-objects.list` を使います）。
- Makefile への組み込みは `ports/rust/rust.mk` です。`tools/rust/sysroot` が無ければ何も作らないので、
  通常の `make` は変わりません。Makefile には Go のブロックの後（`LIBC_RSP` の定義より後）に
  `include ports/rust/rust.mk` を足し、`$(BUILD)/initrd.tar` の依存に `$(RUST_FILES)` を加えます。
- 差分は `tools/rust/src` の `git diff` から作ります（`nocturne.patch`）。新しい rust-src に
  当てて同じ内容になることを確認済みです。

アプリは `ports/rust/apps/<名前>/main.rs` に置くと `/bin/<名前>` になります。C のアプリと同じく
`user/user.ld`、`crt0.asm.o`、C ライブラリーの object（`--start-lib @build/libc-objects.list`）で
`ld.lld` が静的リンクします（0x400000 から。`--gc-sections`）。C の `main` から呼ぶ場合は
`--crate-type staticlib` でも作れます。この場合は Rust の起動処理（`lang_start`）が走らないので、
`std::env::args()` は空で、それ以外（環境、スレッド、標準出力、ファイル）は使えます。

## 対応の形

| 標準ライブラリーの層 | Nocturne での実装 |
| --- | --- |
| 起動（`sys/pal`） | C ライブラリーの `__libc_start` が TLS・stdio・起動設定を用意して `main` を呼ぶ。`std::rt` は `argc/argv` を受け取る。 |
| メモリー（`sys/alloc`） | `malloc`/`calloc`/`realloc`/`free`。16 バイトを超える配置は多めに確保して前に元のポインターを置く。 |
| 引数・環境（`sys/args`、`sys/env`） | 引数は `argv`。環境は起動設定（`n_settings`、`NAME=値`）を最初の使用時に読んだ表で、`set_var`/`remove_var` は C ライブラリーの `setenv`/`unsetenv` にも反映する（C 側だけの後の変更は見えない）。 |
| スレッド（`sys/thread`） | `thread_create`/`thread_join`。スタックはカーネルが決める固定 256 KiB で、`Builder::stack_size` は効かない。1 プロセス 128 本まで。終了時に Rust の TLS デストラクターを走らせる。`available_parallelism` は `cpu_info`。 |
| ロック・条件変数・Once・RwLock・park（`sys/sync`） | Rust の futex 実装を `wait_on_address`/`wake_address` の上に置く（ミリ秒切り上げ、`u32::MAX-1` ms を超える待ちは早く戻る）。 |
| スレッドローカル | ネイティブの `#[thread_local]`（C ライブラリーが各スレッドの FS を設定する）。 |
| 時刻（`sys/time`） | `timespec_get` の `TIME_MONOTONIC`（Instant）と `TIME_UTC`（SystemTime）。分解能は 1 ms。 |
| 標準入出力（`sys/stdio`） | 出力は C の `stdout`/`stderr`（チャンネルでも文書でも C ライブラリーが扱う）。入力はハンドル 0 を直接読み、チャンネルは 1 メッセージずつ、大きいメッセージの残りは次へ持ち越す。文書なら自分で位置を持つ。 |
| ファイル（`sys/fs`） | `n_document_*`、`n_entry_*`、`n_folder_*`。共有カーソルが無いので `File` が位置を持つ。追記は排他を取って末尾へ書く。`rename` は C ライブラリーの `rename`（既存の文書を原子的に置き換える）。パスは `n_path_complete`（`FOLDER` を基準に `.`/`..` を解決）で完全指定にしてから渡す。 |
| パス（`sys/paths`） | `current_dir` は設定 `FOLDER`（無ければ `/`）、`set_current_dir` は `n_folder_set`。`temp_dir` は `/tmp`、`home_dir` は `HOME`（既定 `/home`）。区切りは `/`、`PATH` 区切りは `:`。 |
| エラー（`sys/io/error`） | OS エラーの値は ABI の `N_ERR_*`（正の数）。`ErrorKind` に対応付け、文言は `n_result_text`。 |
| 乱数（`sys/random`） | `n_random`（`HashMap` の鍵もこれ）。 |
| プロセス | `process::id` は `n_app_self`、`process::exit` は `exit`、`abort` は `abort`。 |
| パニック | `panic=abort` のみ。メッセージは標準エラーに出て `abort()`（終了コード 134）。`catch_unwind` は何も捕まえず閉じを実行する。 |

## 確認したこと

ホスト（Windows）での確認だけです。Nocturne 上ではまだ動かしていません。

- 標準ライブラリー（core、alloc、std、panic_abort）が警告なしにビルドできる。
- `ports/rust/apps/rusttest` が `rustc` でコンパイルされ、`ld.lld` で `user/user.ld` と
  C ライブラリーの object にリンクされる。結果は静的 x86-64 ELF（`llvm-objdump -f`：
  `elf64-x86-64`、開始アドレス `0x400000`。`llvm-nm -u`：未解決シンボルなし。PT_LOAD は
  r-x、r--、rw- の 3 つと PT_TLS。584 KB、`strip` 前）。
- C の `main` から Rust の `staticlib`（`HashMap`、`OnceLock`、`env::var`、`eprintln!`）を呼ぶ
  リンクも、未解決・重複シンボルなしで通る。

`rusttest` は println/eprintln、Vec/HashMap/BTreeMap/HashSet/BinaryHeap/VecDeque、8 MiB の確保、
64 バイト配置、`OnceLock`、4 本のスレッド（`Arc<Mutex<Vec>>`、アトミック、スレッドローカル、
`join`）、`Condvar`、`RwLock`、`thread::scope`、`park`/`unpark`、`env`（`var`、`vars`、
`set_var`、`remove_var`、`args`）、`/tmp` での `fs`（作成、追記、シーク、`copy`、`rename`、
`read_dir`、`remove_*`、`NotFound`）、`Instant`/`SystemTime`、`CString`、`RandomState`、
`catch_unwind` を確かめ、すべて通れば `rusttest: ok (N checks)` を出して終了コード 0 です。
Nocturne 上では端末から `rusttest` を実行して確かめます。

## まだできないこと・制限

- `std::process::Command`（アプリの起動）と子プロセス、パイプ：未対応（`n_app_launch` への接続はこれから）。
- `std::net`：未対応（TCP/UDP は `n_net`/`TCP*` 系への接続がこれから）。
- シンボリックリンク、ハードリンク、権限、`set_times`、作成時刻：無い（`set_permissions` は何もせず成功する）。
  更新時刻は秒単位。`File::duplicate` は同じ文書を開き直す。
- `is_terminal` は常に偽（`n_terminal` への接続はこれから）。`current_exe` は未対応。
- スレッドのスタックサイズ指定、スレッド名、優先度：効かない。ファイルのロックは文書の排他（共有ロックと排他ロックの区別は無い）。
- `panic=unwind`、バックトレース：無い。
- Windows のホストで `ld.lld` が PATH に要ります（`rustc-nocturne.sh` が `.exe` を探します）。
