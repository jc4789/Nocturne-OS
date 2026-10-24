# Nocturne 用 LLVM 移植（clang・lld、x86_64-unknown-nocturne）

LLVM 23.1.3（`llvmorg-23.1.3`、コミット `0d261d1ca552c95a8f007e061c787ac7132fbcbc`）に
Nocturne を足す差分です。Nocturne の中で C と C++ をコンパイルしてリンクする、システムの
コンパイラーにすることが目的です。同じ版から、Windows で動いて Nocturne 向けに作るホストの
clang・lld も作ります。LLVM は Apache-2.0 WITH LLVM-exception です。Nocturne のディスクには
各部分のライセンスと、別のライセンスのコードの告知（`llvm/lib/Support` の正規表現と BLAKE3）を
`C:\Nocturne\Licenses\llvm` に、llvm-project の中の場所のまま入れます。

Nocturne は UNIX でも POSIX でもないので、`LLVM_ON_UNIX` も `__unix__` も使いません。
clang は `__nocturne__` を定義し、LLVM の土台層（`llvm/lib/Support/Nocturne/*.inc`）は
Nocturne のネイティブ API（`nocturne.h` の `n_*`）と ISO C を直接呼びます。POSIX の互換層は
作りません。他の OS のソースは参照していません。

## 作り方

```
LLVM_PROJECT=<llvm-project の checkout> ports/llvm/setup-src.sh
                             # llvmorg-23.1.3 を tools/llvm/src に取り出して nocturne.patch を当てる
                             # （LLVM_PROJECT が無ければリリースの tarball を tools/llvm/dl に取って使う）
python ports/llvm/build.py host       # Windows で動く clang・lld などを tools/llvm/host に
make                                   # build/sdk（Nocturne の C ライブラリーと SDK）
python ports/llvm/build.py runtimes   # compiler-rt の builtins と libc++ を Nocturne 向けに
python ports/llvm/build.py nocturne   # Nocturne で動く clang・lld を tools/llvm/nocturne に
make                                   # tools/llvm/nocturne があれば C: に入る
```

- `setup-src.sh` は外部の checkout を変えません（`git archive` で取り出すだけ）。テスト、文書、
  ベンチマークは取り出しません。`tools/llvm/src` はそれ自身の git リポジトリーで、コミットが
  リリースそのものなので、そこでの `git diff` がこの差分（`nocturne.patch`）です。新しいファイルも
  含めるため `git add -A -N` してから `git diff` します。
- `build.py host` は Visual Studio 2022 Build Tools の MSVC（静的 CRT）と Ninja で作ります。
  既定のターゲットは `x86_64-unknown-nocturne` です。
- `build.py runtimes` は host の clang で compiler-rt の builtins（host の clang のリソース
  ディレクトリーへ）と libunwind・libc++abi・libc++ を作ります。後の三つは
  `tools/llvm/runtimes` に入れ、`build/sdk` を写した `tools/llvm/sysroot` に重ねます。
- `build.py nocturne` は host の clang で LLVM を Nocturne 向けにクロスビルドし（CMake の
  toolchain は `ports/llvm/cmake/nocturne.cmake`、プラットフォームは `cmake/Platform/Nocturne.cmake`）、
  C:\Nocturne に入る形で `tools/llvm/nocturne` に置きます。`tools/` は git の管理外です。
- Makefile への組み込みは `ports/llvm/llvm.mk` です。`tools/llvm/nocturne` が無ければ何も
  作らないので、通常の `make` は変わりません。

## ディスクに入るもの

| C: の場所 | 中身 |
| --- | --- |
| `C:\Nocturne\System\llvm` | clang、lld と binutils に当たる道具をまとめた一つのプログラム（LLVM の `LLVM_TOOL_LLVM_DRIVER_BUILD`）。起動された名前で道具を選ぶ。 |
| `C:\Nocturne\System\clang`、`clang++`、`ld.lld`、`llvm-ar`、`llvm-ranlib`、`llvm-nm`、`llvm-objcopy`、`llvm-strip`、`llvm-objdump`、`llvm-readobj`、`llvm-readelf`、`llvm-size`、`llvm-symbolizer`、`llvm-addr2line`、`llvm-cxxfilt` | 小さな起動役（`ports/llvm/tool.c`）。`llvm <自分の名前> <引数>` として `llvm` を同じ設定・ハンドル 0〜2 で起動し、その結果で終わる。 |
| `C:\Nocturne\SDK\Lib\clang` | clang のヘッダー（`include`）と実行時ライブラリー（`lib\x86_64-unknown-nocturne\libclang_rt.builtins.a`）。clang のリソースディレクトリー。 |
| `C:\Nocturne\SDK\Include\c++\v1`、`C:\Nocturne\SDK\Lib\libc++.a` | libc++（libc++abi を含む）。 |
| `C:\Nocturne\SDK\Lib\libunwind.a`、`C:\Nocturne\SDK\Include\libunwind.h` など | libunwind。C++ の例外がスタックを戻る。 |
| `C:\Nocturne\Licenses\llvm` | ライセンスと告知。 |

道具を一つのプログラムにまとめるのは、clang と lld がそれぞれ LLVM の大半を含み、別々の
プログラムにすると同じコードを何度もディスクに置くことになるからです。Nocturne の文書には
名前が一つしかなく、リンクも別名も無いので、道具の名前ごとに起動役を置きます。起動役は
`llvm` 自身の完全なパスを最初の引数にするので、clang はコンパイルの仕事（`-cc1`）を起動役では
なく `llvm clang -cc1 …` として起動します。

clang と lld は Nocturne の版であることと、元のリリースの llvm-project のコミットを名乗ります
（`Nocturne clang version 23.1.3 (https://github.com/llvm/llvm-project 0d261d1c…)`）。
`tools/llvm/src` はこの機械の中のリポジトリーで、そのパスとコミットは他の誰にも意味が無いからです。

clang の既定は Nocturne の SDK（`C:\Nocturne\SDK`、`--sysroot` で変更可）です。ヘッダーは
リソースディレクトリーの `include`、SDK の `Include`（C++ では先に `Include\c++\v1`）の順に探します。
リンクは `ld.lld` に `-static --no-dynamic-linker -z max-page-size=0x1000 --gc-sections -T user.ld
--eh-frame-hdr`、`crt1.o`、（C++ なら `-lc++`）`-lc`、compiler-rt の builtins、`-lunwind` を渡します。
プログラムは 0x400000 からの静的 ELF で、位置独立ではありません（`-shared`、`-pie` などは誤り）。
SDK のライブラリーは関数と変数ごとのセクションでできているので、`--gc-sections` で使う物だけが
プログラムに入ります（全部を残すなら `-Wl,--no-gc-sections`）。

## C++ の例外

C++ の例外は使えます（`-fexceptions` は C++ の既定です）。libc++abi の `__cxa_throw` から呼ばれて
スタックを戻すのは libunwind です。

- Nocturne のプログラムは一つの静的な像です。ローダーも共有ライブラリーも無いので、libunwind は
  ベアメタルの形（`LIBUNWIND_IS_BAREMETAL`）で作ります。巻き戻しの表は `user.ld` が名前を付けた
  `.eh_frame`（`__eh_frame_start`、`__eh_frame_end`）と、その整列した索引 `.eh_frame_hdr`
  （`__eh_frame_hdr_start`、`__eh_frame_hdr_end`。`ld.lld --eh-frame-hdr` が作る）で見つけます。
- libunwind が登録した表を守るロックは、C11 の `mtx_t`（`threads.h`）です。どのスレッドでも
  例外を投げられます。
- 表を持つのは、例外が通り得る C++ のコードだけです。clang は Nocturne では非同期の巻き戻し表を
  作りません。C の関数は例外を通さないので、C のコード（`qsort` の比較関数など）を越えて投げた例外は、
  捕まらない例外として `std::terminate` になります。
- OS 自身のプログラムは例外を使わないので、表を持ちません（Makefile の `UCFLAGS` の
  `-fno-asynchronous-unwind-tables`）。Nocturne 版の LLVM も例外を使わないので、表を持ちません
  （`LLVM_ENABLE_UNWIND_TABLES=OFF`）。

## 土台層（llvm/lib/Support/Nocturne）

| 部分 | Nocturne での実装 |
| --- | --- |
| パス（`Path.inc`） | 文書とフォルダーを完全なパス（`C:\a\b`、`/` も区切り）で扱い、名前はアプリのフォルダー（設定 `FOLDER`）の中で解く。大文字小文字を区別しない。リンク、所有者、権限は無い。文書に共有の位置は無いので、順に読み書きする位置は土台層がハンドルごとに持つ。追記は文書を確保してから末尾へ書く。置き換え（`CD_CreateAlways`）は新しい中身が全部保存されるまで古い中身を保つ。 |
| 出力のバッファー（`FileOutputBuffer.cpp`） | 文書を写像しないので、出力は全部メモリーに作って一度に書く。 |
| アプリ（`Program.inc`） | `n_app_launch` で、引数、設定、ハンドル 0〜2 を渡して起動する。何も引き継がないので、フォルダー（`FOLDER`）と端末につながるハンドル（`TERMINAL`）を渡す。PATH は無く、名前だけのプログラムはシステムとアプリのフォルダーで探す（`find_program`）。待つのは `n_app_result`、時間切れは `n_app_stop`。 |
| プロセス（`Process.inc`） | 設定は `getenv`、ページの大きさ、端末（`TERMINAL`、ANSI のエスケープ）、色、乱数（`n_random`）。 |
| メモリー（`Memory.inc`） | `n_memory_reserve`/`n_memory_protect`/`n_memory_release`。ページは書き込みか実行のどちらか一方。 |
| スレッド（`Threading.inc`） | Nocturne のネイティブのスレッド（`thread_create`）。名前、優先度、CPU の指定は無い。CPU 数は `cpu_info`。 |
| シグナル（`Signals.inc`） | Nocturne にシグナルは無い。障害のアプリはシステムが終わらせて場所を報告し、Ctrl+C はアプリを終わらせるだけ。LLVM が諦めるとき（`report_fatal_error`）に消す文書はここで消す。読まれなくなったチャンネルへの書き込みは `EX_IOERR` で終わる。 |
| 動的ライブラリー、COM、監視（`DynamicLibrary.inc`、`COM.inc`、`Watchdog.inc`） | 無い。プログラムは全部まとめてリンクされる。 |
| エラー | errno の値は ABI の `N_ERR_*`（正の数）。`std::error_code` に対応付け、文言は `n_result_text`。 |
| 標準出力・エラー（`raw_ostream.cpp`、`ErrorHandling.cpp`） | 端末なら出力をためずに書く。メモリー不足の報告は C ライブラリーのバッファーの無い `stderr` で、確保せずに書く。 |

## 確認したこと

2026-10-23、QEMU（WHPX、4 CPU、4096 MiB、起動ディスクは AHCI、1920×1080）で。Hyper-V と実機では
まだ確認していません。

- `python scripts/test.py --accel whpx --memory 4096 --cpus 4 clang tcc cxx threads fs sh tools nativeabi mem
  sharedpages gui`：46 件成功、失敗 0（tcc/self-host は `--full` が要るので飛ばす）。clang 群は、C の
  プログラム（`tests/ctest.c`）、エラーの報告、C++23 のプログラム（`tests/cxxtest.cpp`：例外、スレッド、
  `sleep_for` と時計、`<format>`、`<regex>`、`<filesystem>`、ストリーム、`std::exception_ptr`）、スレッド
  （`tests/threadtest.c`）、OS の全アプリを clang でコンパイルしてリンクすること。
- 端末で：`clang --version`（`Nocturne clang version 23.1.3`、`Target: x86_64-unknown-nocturne`）。
  `clang -O2` で作った `tetris.c` と `mandel.c` が窓を開いて動く。`clang++ -std=c++23` の `std::println`
  と、`std::vector::at` が投げた `std::out_of_range` を捕まえるプログラムが動く。`tcc -run` も前のとおり動く。
- `llvm` は 92 MB で、一回のコンパイルで三度起動する（ドライバー、`-cc1`、`ld.lld`）。カーネルは
  プログラムの読み取り専用のセグメントを、それを走らせるタスクと、最近走ったプログラムの次の起動で
  共有する（`kernel/src/sys/syscall.c` の `program_get`/`program_map`/`program_put`）。プログラムは
  文書の中身で見分ける（`store_document_identity`：大きさ、ブロックとその CRC-32C）。測った値：
  ディスクからの最初の起動 576 ms、二度目からの起動 1 ms。`clang -O2` で mandel.c が 271 ms
  （共有の前は 857 ms）、tetris.c が 285 ms（878 ms）、小さな C++23 のプログラムが約 2.0 s（4.2 s）。
- 走っているプログラムを入れ替えた次の起動は新しい中身を読む：大きさの違う入れ替え（echo を date に）
  と、同じ大きさの入れ替え（true と false、どちらも 20552 バイト）。
- `sleep_for(20ms)` が `steady_clock` で 19 ms と測れることがあった（C++ と ISO C は「少なくとも」を
  求める）。アプリの時計（`n_clockinfo` の基準の組）とカーネルの数え方が同じ時刻から数えるように
  し、待ちの期限を最新の時刻から取るようにした（`kernel/src/dev/timer.c`）。後は 20 ms が 20〜21 ms、
  5 ms が 5〜6 ms。
