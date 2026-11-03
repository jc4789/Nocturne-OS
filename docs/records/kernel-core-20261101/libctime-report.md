# libctime の報告：libc に ISO C の `clock()` と C23 の `localtime_r`・`gmtime_r` を足す

枝は `worktree-agent-a8ff26af1d015f8df`。土台は main の c78ffae6（作業木を c78ffae6 へ fast-forward した）。

## 依頼文（全文）

```
あなたの名は「libctime」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。ただし土台は main の c78ffae6（時間の ABI が ns になり、時計は `struct n_clock` の頁 `N_CLOCK_ADDRESS` で読む形。報告は `docs/records/kernel-core-20261101/timeabi-report.md`・`toolchain-report.md`）。自分の作業木が c78ffae6 より古ければ、まず main を取り込む。QEMU のポートは HMP 45472・QMP 45473。

# 仕事：`user/libc` の ISO C の時刻の欠けを埋める
- ISO C の `clock()`（`<time.h>`、`CLOCKS_PER_SEC`）が無い。プロセスの CPU 時間を返す。カーネルが thread・プロセスの CPU 時間を持っている（`kernel/src/sys/sched.h` の `sched_thread_cpu_ns`、`n_procinfo` など）。それをアプリが読む口がネイティブ ABI にあるかを原文で確かめ、あれば使い、無ければ Nocturne の形で最小の口を足す（ABI の仕様書 `docs/nocturne-native-abi.md` も同じコミットで）。
- C23 の `localtime_r`・`gmtime_r`（ISO C23 7.29.3）が無い。足し、`localtime`・`gmtime` はそれの上に。地方時の計算が共有の状態を使うなら、thread で安全に。
- 足したら、`ports/llvm/nocturne.patch` の `Chrono.cpp` の直し（今は共有の `localtime`・`gmtime` の結果を写す）を `_r` の形に変えてよい。ただし LLVM を組み直すのは重い（`ports/llvm/README.md`）ので、patch を変えるなら組み直しまでして、ゲストの clang で確かめる。変えないなら報告に書く。
- POSIX だけの口（`clock_gettime` など）は足さない。

確かめ：組み（`kernel`・`qemu-image`、変数なし）。QEMU で、ゲストの clang と tcc で `clock()`・`localtime_r`・`gmtime_r` を使う小さなプログラムを組み、走らせ、値が `date` と合うこと。
```

## 設計

### `clock()`
- 口の有無を原文で確かめた。`SYS_PROCLIST` の `n_procinfo.cpu_ns` は thread ごとの値で、アプリ全体の合計は無い。終わって記録を手放した thread の時間は、どこにも残らない。呼ぶたびに全 thread の一覧を写す口は `clock()` に向かない。
- 口を一つ足した：`SYS_APP_CPU_TIME`（番号 20、引数なし）。呼んだアプリの thread が走った時間の合計（ns）を返す。
  - 持ち主は `sys/thread.c`。`struct process` に `retired_ns` を足し、thread の記録が process の一覧から外れるとき（`process_unlist`。join・detach・終了時の片付け・`thread_left` の全部がここを通る）に、終わった thread の `sched_thread_cpu_ns` を足す。
  - `process_cpu_ns(p)` は `thread_lock` の下で `retired_ns` と main thread と一覧の thread の `sched_thread_cpu_ns` を足す。走っている thread の分は `charge` を通って今までの分が入る。
  - ほかのアプリと子のアプリの分は入らない。
- libc：`n_app_cpu_ns()`（`nocturne.h`、`sys.c`）と、その上の `clock()`（`time.c`）。`clock_t` は ns の数（`int64_t`）、`CLOCKS_PER_SEC` は 1000000000。ABI が渡す時間は全部 ns なので、換えずに返す。
- 仕様書 §8 に `n_app_cpu_ns` を書いた。`abi.h` の未割当の一覧から 20 を外した。

### `gmtime_r`・`localtime_r`
- 時刻帯は Nocturne に無い（時計は地方時を持つ。`hv_util.c` の注も同じ）。だから `localtime_r` の分解は `gmtime_r` と同じで、共有の状態を使わない。thread で安全。
- `gmtime_r` は年の数え上げをやめ、`days_from_civil` の逆の `civil_from_days`（400 年の era で数える式）にした。今までの実装は年を 1970 から一つずつ数え、大きな `time_t` で終わらなかった。`tm_year` に入らない年は `NULL` と `errno = EOVERFLOW`（C23 7.29.3.3）。
- `gmtime`・`localtime` は `_r` の上に、共有の一つの `struct tm` で置いた（ISO C の定め）。
- `mktime` は `localtime_r` を使い、日数・秒の桁あふれと `tm_year` に入らない結果で `(time_t)-1` を返す。今までは桁あふれを見なかった。

## 変えたファイル

- `common/abi.h`：`SYS_APP_CPU_TIME`。
- `docs/nocturne-native-abi.md`：§8。
- `kernel/src/sys/process.h`・`thread.c`・`thread.h`・`syscall.c`：`retired_ns`、`process_cpu_ns`、dispatch の一行。
- `user/include/nocturne.h`・`user/libc/sys.c`：`n_app_cpu_ns`。
- `user/include/time.h`・`user/libc/time.c`：`clock_t`・`CLOCKS_PER_SEC`・`clock`・`gmtime_r`・`localtime_r`。`gmtime`・`localtime`・`mktime` は書き直し。
- `tests/ctest.c`：`gmtime_r`・`localtime_r`・`mktime`・`clock` の検査。

## 消したもの
- `gmtime` の年を一つずつ数える実装と、共有の static を直接埋める形。`localtime = gmtime` の別実装。
- `mktime` が `gmtime` の共有の結果を写す形。

## 組みの結果
- `python -X utf8 scripts/build.py -j 8 kernel`：rc 0、警告 0。記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\libctime\build-kernel.log`。
- `python -X utf8 scripts/build.py -j 8 qemu-image`（変数なし）：rc 0。`IMG build/nocturne.img (1152 MiB)`。記録は `...\scratchpad\libctime\build-image6.log`。出力は `D:\Programes\llm vibe slop os\build\wt\agent-a8ff26af1d015f8df\nocturne.img`。
  - 警告：`user/libc/font.c` の `-Wmissing-field-initializers` 5 行。変えていない行で、timeabi の報告にもある。
- 作業木で組むために、git の外に次を置いた（コミットに入らない）：
  - `tools` を、`TypeScript` だけ実体の木にして残りは main の `tools` への junction にした。`build` は `build\wt\agent-a8ff26af1d015f8df` への junction にした。`third_party/wabt/src/tools` は main から写した（`.gitignore` の `tools/` が作業木の checkout から外すため）。
  - `tools` 全体を junction にすると、`Makefile` の TypeScript の overlay が効かず `tsc` が `syscall.SIGTERM` で組めなかった（Go が junction を実体の道へ解くため）。それで `TypeScript` を実体にした。
  - 作業木の組みの穴は timeabi の報告と同じ種類：`generate.py`・`build_cxx.py` が BUILD でなく `ROOT/build` へ書く。

## QEMU で見たもの
窓付き一台（q35、UEFI、NVMe、WHPX、4 CPU、4096 MiB、2560×1440、HMP 45472・QMP 45473、`snapshot=on`）。三台目になって一度 `quit` し、他の二台が終わるのを待って起こし直した。終わりに HMP の `quit` で止めた。画像と記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\libctime\` にある。

- `...\libctime\t0.png`：Terminal を開いた。
- `...\libctime\t1.png`：`clang -O2 -o tc t.c && .\tc`（`t.c` は `time` → `gmtime_r`・`localtime_r`、`clock()` で 3000 万回の浮動小数の加算を挟む）。
  - 「gm 2026-11-01 12:38:41 wd0 yd304」「local same 1, static same 1」「clock 67092864 ns, per sec 1000000000」。タスクバーの時計は 12:39。
- `...\libctime\t2.png`：
  - `.\tc && date`：「gm 2026-11-01 12:39:11 wd0 yd304」、直後の `date` は「Sunday 01 November 2026, 12:39:11」。clock 68228661 ns。
  - `tcc -o tt t.c && .\tt && date`：「gm 2026-11-01 12:39:12 wd0 yd304」、`date` は「Sunday 01 November 2026, 12:39:12」。clock 68518153 ns。clang と tcc の二つで、同じ 3000 万回の加算が 67〜69 ms。
  - `tests/ctest.c` に足した検査と同じ内容の `u.c`（`CHECK` で 2000-02-29 の分解・1969-12-31 23:59:59・`localtime_r`・`INT64_MAX` の `NULL` と `EOVERFLOW`・`mktime` の往復・`clock`）を clang と tcc で組んで走らせた。どちらも「timecheck: ok」。
  - `clock()` の検査：20 ms の空回しの間に 20004469 ns（clang）・20005018 ns（tcc）、単調時計は 20000008 ns。20 ms 眠った後の増分は 8350 ns・13311 ns で、眠りは数えない。
- serial.log（`...\libctime\serial.log`）に `fault`・`panic`・`killed` の行は無い。

## `ports/llvm/nocturne.patch` の Chrono.cpp：変えていない
`_r` の形に変えるには、LLVM を組み直す。その組み直しは `tools/llvm`（`sysroot`・`runtimes`・`nocturne`）と main の `build/sdk` を書き換える（`ports/llvm/build.py` は `ROOT/tools`・`ROOT/build/sdk` を使い、`runtimes` は `tools/llvm/sysroot` を消して作り直す）。それは全作業木が共有する物で、この枝の libc で組むと main と合わなくなる。この枝が main へ載ってから、次の形に直して組み直す。

`llvm/lib/Support/Chrono.cpp` の二つの hunk の `#if defined(__nocturne__)` の中身：

```
+#if defined(__nocturne__)
+  struct tm *LT = ::localtime_r(&OurTime, &Storage);
+  assert(LT);
+  (void)LT;
+#endif
```

（二つ目は `::gmtime_r`。hunk の見出しは `@@ -37,6 +37,11 @@` と `@@ -55,6 +60,11 @@`。）手順は `ports/llvm/README.md` の `build.py runtimes`・`nocturne`。今の patch のままでも、ゲストの clang は動いた（`t.c` を組めた）。直す理由は、`localtime`・`gmtime` の共有の結果を別 thread が同時に書き換える競りを消すこと。

## 残った欠陥と未確認
- `tests/ctest.c` の追加分は、ゲストの試験の枠（`scripts/test.py`）では回していない。媒体の数の規則で `build/test.img` を作れないため。同じ内容を手で打ち込んだ `u.c` で clang と tcc の両方を通した。
- `process_cpu_ns` は `thread_lock` の下で全 thread の `sched_thread_cpu_ns`（各 thread の run queue の錠を取る）を呼ぶ。`clock()` を高い頻度で呼ぶアプリで、thread が多いと `thread_lock` を持つ時間が伸びる。測っていない。
- 複数の thread が走る中の `clock()` の合計は見ていない（検査は一つの thread）。
- 実機と Hyper-V では確かめていない。
- 作業木の `tools`・`build` の junction は git の外にある。作業木を消すときは、先に junction を外す（中身を再帰で消すと main の `tools` が消える）。
