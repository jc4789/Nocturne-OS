# toolchain の報告：git の外の道具を今の ABI で組み直す

働いた所は main の作業木（`D:\Programes\llm vibe slop os`）。コミットは main の 02e8e89a・f76bbb12 と、この報告のコミット。

## 依頼文（全文）

```
あなたの名は「toolchain」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。ただし、あなたは作業木でなく main の作業木（`D:\Programes\llm vibe slop os`）で働く。理由：仕事の大半が git の外の `tools/` と `build/sdk` を組み直すことだから。QEMU のポートは HMP 45470・QMP 45471。

# 状況
main の 8837d5bd で時間の ABI が ns になった（`docs/records/kernel-core-20261101/timeabi-report.md`）。ms の口（`SYS_UPTIME`・`SYS_TIME`・`SYS_CLOCK_INFO`、`uptime_ms`、`n_sleep_ms` など）が消え、`struct n_clock` の頁（`N_CLOCK_ADDRESS`）で時計を読む形になった。その前のコミットで libc から `setenv`・`unsetenv` も消えている。Rust の std（`tools/rust/sysroot`）は主担当が今の patch で組み直した。古い ABI で組まれたまま残っている道具：
- `tools/llvm` の runtimes（compiler-rt の builtins、libc++）と `tools/llvm/sysroot`、ゲストで動く clang・lld（`tools/llvm/nocturne`、媒体の `C:\Nocturne\System\llvm`）。手順は `ports/llvm/README.md`（`python ports/llvm/build.py runtimes`、`python ports/llvm/build.py nocturne`。`make` で `build/sdk` を先に）。
- Go（`tools/go` は Go のソースへの link。手順は `ports/go/README.md`。patch は timeabi の書き手が gofmt でしか確かめていない）。
- ほかに libc の ABI を静的に取り込む git の外の道具があれば、それも（`ports/*/README.md` を読む）。

# 仕事
1. 上の道具を、今の main のソースと patch から組み直す。組めない所は、移植の接着（`ports/llvm`、`ports/go` の patch と Nocturne の部分）を新しい ABI に直す。POSIX の口を libc に戻すのは禁止。直しは `ports/` の中だけにし、カーネル・`common/`・`user/libc` を変えなければ済まないなら、変えずに報告する。
2. `python -X utf8 scripts/build.py -j 8 qemu-image` を変数なしで通す。
3. 窓付きの QEMU で起動し、ゲストの Terminal で clang（小さな C と C++ のプログラムを組んで走らせる。時計と眠りを使うものを含む）、tcc、Go のプログラム（`ports/go/apps/*`）、Rust の `rusttest` を走らせる。ゲストの試験 `scripts/test.py --accel whpx --memory 4096 --cpus 4 clang tcc cxx threads fs sh tools nativeabi mem`（`ports/llvm/README.md` の行）が回せるなら回す。フックが媒体の数で止めるなら、`build/mae.img` は主担当のものなので消さずに、止まったことを報告する。
4. ほかの書き手（「compositor」「iopath」）が作業木で並べて働いていて、主担当がそれらを main へ載せることがある。あなたが main の作業木で変えるのは `ports/llvm`・`ports/go`・`ports/` の中の要る所と `docs/records/kernel-core-20261101/toolchain-report.md` だけにし、直したら早めに main へコミットする（日本語、末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。主担当の載せが作業木を変えたら、それを取り込んで続ける。

# 報告
`docs/records/kernel-core-20261101/toolchain-report.md` に日本語で全文（依頼文の全文を先頭に）。組み直した道具、直した patch、組みの結果、ゲストで走らせた結果（画像の絶対 path と serial の行）、残った欠陥。
```

## 組み直した道具

| 道具 | 場所 | 結果 |
|---|---|---|
| LLVM のソース | `tools/llvm/src` | `ports/llvm/nocturne.patch` の今の形に合わせた（timeabi の二か所：`libcxx/src/atomic.cpp` の期限、`Support/Nocturne/Path.inc` の眠り）。下の直しも入れた。`git diff` は patch と `index` の行のほかは一字も違わない。 |
| compiler-rt の builtins、libunwind・libc++abi・libc++ | host の clang のリソースディレクトリー、`tools/llvm/runtimes`、`tools/llvm/sysroot` | `python ports/llvm/build.py runtimes`。rc 0。 |
| ゲストで動く clang・lld（`llvm`）と MCJIT の庫 | `tools/llvm/nocturne`、`tools/llvm/build-nocturne/lib` | `python ports/llvm/build.py nocturne`。2917 個を組んで rc 0。直しの後に増分でもう一度 rc 0。 |
| Go のソース | `D:\Programes\cloned repos\go`（`tools/go` の先） | 枝 `nocturne` を `260501a1`（go1.27.2 の基準）＋ `ports/go/nocturne.patch` と同じ木にした（コミット d451a0eb）。下の直しをコミット（c55507db）。`go.exe` は組み直していない：変わったのは runtime・syscall・os・net だけで、`cmd` は同じ。標準ライブラリーは `go build` がソースから組む。 |
| TypeScript の `tsc`（Go） | `build/programs/tsc` | `qemu-image` が `ports/go/nocturne.patch` の変化で組み直した。 |
| Rust の std | `tools/rust/sysroot` | 主担当が組み直した物（06:09）をそのまま使った。 |

git の外の道具で libc の ABI を静的に取り込むのは、上の LLVM・Go・Rust のほかに無い。Mesa・FFmpeg・FreeType・Lexbor・Expat・TinyCC・wabt は `third_party/` から `make` が組む。`tools/mesa-tools` はホストの bison・flex、`tools/msys64` はホストの道具で、Nocturne の ABI を持たない。

## 直した patch

### `ports/go/nocturne.patch`（02e8e89a）
- runtime（`src/runtime/sys_nocturne_amd64.s`）と syscall（`src/syscall/asm_nocturne_amd64.s`）の呼出しが `INT $0x80` だった。今の ABI に `int $0x80` の入口は無い（`docs/nocturne-native-abi.md` §0、一般保護例外でアプリが終わる）。全部 `SYSCALL` にした。CX と R11 は命令が使う。
- `settls` が消えた `SYS_THREAD_TLS`（52）を呼んでいた。`WRFSBASEQ` で FS の基底を書く形にした（§0：FS と GS の基底はアプリのもの）。
- Go の全ての `SYS_*` の番号（66 個と asm の 4 個）を `common/abi.h` と `_Static_assert` で照らした。違いは無い。
- `ports/go/README.md` の「`INT $0x80` を使っていて最初の呼出しで止まる」の注意を消した。

### `ports/llvm/nocturne.patch`（02e8e89a・f76bbb12）
- libc++ の `fstream`：libc から POSIX の `fileno`・`fdopen` が消えた（23f80428）ので、runtimes が組めなかった（`fstream:280`・`801` の未宣言）。Nocturne の C の `FILE` は名前でだけ開き、裏の handle を出す口が無い。だから Nocturne では：
  - `basic_filebuf`・`basic_ifstream`・`basic_ofstream`・`basic_fstream` の C++26 の `native_handle_type`・`native_handle()` を持たない。`version` の `__cpp_lib_fstream_native_handle` も定義しない。
  - handle で開く内部の `__open(int)` を持たない。Nocturne の `copy_file` はこれを使わない（`operations.cpp` の Nocturne の道）。
- `llvm/lib/Support/Chrono.cpp`：Nocturne では `getStructTM`・`getStructTMUtc` が `struct tm` を埋めずに返していた（警告 `unused variable 'OurTime'` で見えた）。ISO C の `localtime`・`gmtime` の結果を写す。
- `clang/lib/Frontend/PrecompiledPreamble.cpp`：Nocturne が「Unknown platform」の `#warning` の道に落ちていた。メモリー上の PCH の名を `C:\__clang_tmp\…` にした。

## 組みの結果

- runtimes：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\toolchain\llvm-runtimes2.log`。誤り 0。警告は上流の `libcxx/include/__thread/support/c11.h:151` の `-Wcast-function-type-mismatch` が 18 回（libc++ の C11 thread の型の写し。移植の patch の外）。
- clang・lld：`…\scratchpad\toolchain\llvm-nocturne.log`・`llvm-nocturne2.log`。直しの後の増分は警告 0。初回の全体の組みに残った警告は、上の二つの直しが消した物と、上流の `SimpleRemoteEPCUtils.h:138` の `-Wunused-private-field`（Nocturne に fd の通信が無いので使われない欄）。
- `python -X utf8 scripts/build.py -j 8 qemu-image`（変数なし）：rc 0。`IMG build/nocturne.img (1216 MiB)`、`C: 197 folders, 2747 documents`。記録は `…\scratchpad\toolchain\build-image.log`。Go の `calc`・`gotest`・`tsc`、`rusttest`、全ての C++ のプログラムと browser が組み直され、`build/programs/llvm` は `tools/llvm/nocturne/System/llvm` と同じ中身。
  - 警告 1 行：cargo の「failed to connect to jobserver … `--jobserver-auth=-2,-2`」。make の MAKEFLAGS を Rust の組みが読めないという知らせで、`rust.mk` の外の形。この仕事では変えていない。

## ゲストで走らせた結果

窓付きの QEMU を一台（q35、UEFI、NVMe、`-cpu host`、WHPX、4 CPU、4096 MiB、2560×1440、HMP 45470・QMP 45471、`build/nocturne.img` を `snapshot=on`）。終わりに HMP の `quit` で止めた。プログラムは Terminal の `echo '…' >> 名` で打ち込んだ（打ち込み役は `…\scratchpad\toolchain\hmp.py`）。画像はすべて `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\toolchain\` にある。

- `desk.png`：デスクトップ。時計 11:48。
- `clang-c.png`：`clang --version` は「Nocturne clang version 23.1.3 (https://github.com/llvm/llvm-project 0d261d1c…)」「Target: x86_64-unknown-nocturne」「Thread model: nocturne」。`clang -O2 t.c` が通った。
- `clang-cpp.png`：
  - C（`timespec_get(TIME_MONOTONIC)`、`thrd_sleep` 250 ms、`time`・`ctime`）：「slept 250 ms, Sun Nov  1 11:49:53 2026」。直後の `date` は「Sunday 01 November 2026, 11:49:53」。
  - C++23 を `clang++ -std=c++23 -O2` で組んで走らせると「libc++abi: terminating due to uncaught exception of type std::out_of_range: vector」。下の「残った欠陥」の一番目。
- `exc-crop.png`：`throw 7; catch (int)` も捕まらない（「uncaught exception of type int」）。
- `unw-crop.png`：`_Unwind_Backtrace` が 0 枚の frame で `_URC_END_OF_STACK`。`.eh_frame_hdr` の中身は読める（版 1、符号 0x1b・0x03・0x3b）。
- `unw2-crop.png`：`unw_get_proc_info` は main の FDE を見つける（start 400010）。
- `exc2-crop.png`：同じ二つを `-lunwind` を入力の前に置いて組むと：
  - 「ok1 7」「ok2 x」「ok3 vector」。
  - 「sleep 20ms wait_for 30ms fstream ok, catch ok now 2026-11-01 11:59:24」（`sleep_for(20ms)`、`condition_variable::wait_for(30ms)`、`std::atomic::wait`・`notify_one` と thread、`fstream` の書き読み、例外、`system_clock` の書式）。
- `tcc-rust.png`：
  - `tcc -run t.c`：「slept 250 ms, Sun Nov  1 12:00:03 2026」。`tcc -o tt.exe t.c` の `tt.exe`：「slept 251 ms」。
  - `rusttest`：「slept 50 ms, measured 50.056452ms」、「rusttest: ok (50 checks)」。
- `gotest.png`：「GOOS/GOARCH: nocturne amd64 CPUs: 4 GOMAXPROCS: 4」「now: 2026-11-01T12:00:37Z」「goroutines: 175000000 in 177ms」「sleep 250ms took 250ms」、文書・一覧・チャンネル・GC・exec の行、「total 562ms」。
- `gonet-crop.png`：`gotest net`：「lookup example.com: [104.20.23.154 172.66.147.243] <nil> 17ms」「read deadline: true 300ms」「GET https://example.com/ 200 OK 577 bytes TLS 1.3 Example Domain <nil> 72ms」。`tsc --version`：「Version 7.1.0-dev」。
- `calc.png`：Go の `calc` が Calculator の窓を開いた。

serial.log（`…\scratchpad\toolchain\serial.log`）の行：

```
[   18.170580 0:1] store: C: "Nocturne" on nvme0, 1150 MiB, 243 MiB free
[   18.258598 0:1] wm: compositor started at 2560x1440
```

`fault`・`panic`・`killed`・`abort`・`terminat` の行は無い（C++ の terminate はアプリの出力で、カーネルの行ではない）。

ゲストの試験 `scripts/test.py …` はフックが止めた：「媒体は 2 つまで。build に既に mae.img、nocturne.img があり、道具の写しが三つ目になる」。`build/mae.img` は主担当の物なので消していない。

## 残った欠陥

1. **SDK の C++ の例外が捕まらない。** 原因は `scripts/mksdk.sh` にある（`ports/` の外なので変えていない）。
   - `strip_obj`（88 行）は `objcopy --strip-debug -R .eh_frame …` で、C++ の実行時の庫（`*.a` の道、113 行あたり：libc++.a・libunwind.a・LLVM の JIT）の部材にもかかる。282785b0 から、SDK の `libc.a` はこれらの部材を `.eh_frame` 無しで持つ。
   - clang の link は `-lc++ -lc … -lunwind` の順。lld は先に見た庫の定義を取るので、`_Unwind_RaiseException`・`__unw_getcontext` などは `libc.a` の表の無い部材から入る。libunwind が自分の frame を巻き戻せず、全ての `throw` が `std::terminate` になる。ホストの clang で `--sysroot=tools/llvm/sysroot` で組んでも同じ（`-Wl,--why-extract` で `libc.a(UnwindLevel1.c.obj)` が選ばれ、出力の `.eh_frame` に libunwind の FDE が一つも無い）。
   - 直し方は二つある。どちらも主担当の判断が要る：(a) `mksdk.sh` で、C++ の実行時の庫の部材から `.eh_frame` を消さない。(b) clang の Nocturne のドライバー（`ports/llvm` の `Nocturne.cpp`）で `-lunwind` を `-lc` より前に置く。(a) は原因を消す。(b) は `libc.a` に表の無い部材を残す。TinyCC が `.eh_frame` を持つ部材をどう扱うかは、確かめていない。
2. libc に ISO C の `clock()` が無い（`time.h` に宣言が無い）。C の試しから外した。持ち主は `user/libc`。
3. libc に C23 の `localtime_r`・`gmtime_r` が無いので、LLVM の `Chrono.cpp` は ISO C の `localtime`・`gmtime` の共有の結果を写す。別の thread が同時に呼ぶと競る。持ち主は `user/libc`。
4. C++26 の `fstream::native_handle` は Nocturne に無い。libc が `FILE` の handle を返す Nocturne の口を持てば足せる。持ち主は `user/libc`。
5. 共有の `tools/` を組み直した。`build.py runtimes` は `tools/llvm/sysroot` と `tools/llvm/runtimes` を消してから作り直すので、組んでいる間（06:18〜06:25 と再組み）に別の作業木の組みが `TOOLS=../../../tools` でそこを読むと失敗しうる。今の `tools/` は新しい ABI（8837d5bd）の libc の header で組まれている。df893170 を土台にした作業木の組みは、`tools/llvm/sysroot`・`runtimes` と合わない。
6. ゲストの試験（clang・tcc・cxx・threads・fs・sh・tools・nativeabi・mem）は回していない（上のフック）。
7. Hyper-V と実機では確かめていない。
