# rustenv の報告：`qemu-image` を変数なしで組めるようにする

## 依頼文

> あなたの名は「rustenv」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の b6388a38。QEMU のポートは HMP 45466・QMP 45467。
>
> # 仕事：`qemu-image` を変数なしで組めるようにする
> 今、`python -X utf8 scripts/build.py -j 8 qemu-image` は `rusttest` の link で止まる。`tools/rust/sysroot` の Rust の libstd が libc の `setenv`・`unsetenv` を引く（`std::env::set_var`・`remove_var`、`set_current_dir` の中の `setenv("FOLDER")`）。コミット 23f80428 が libc から `setenv`・`unsetenv` を消した（持ち主が POSIX の口として消した）。POSIX の `setenv` を libc に戻すのは禁止。
>
> やること：
> - Nocturne のプロセスの環境（起動の値、今のフォルダー）がネイティブにどう持たれているかを原文で読む（`common/abi.h`、`docs/nocturne-native-abi.md`、`user/libc/sys.c`・`crt0.asm`、`kernel/src/sys/proc.c`、`getenv` の実装）。今のフォルダーを環境変数 `FOLDER` で持つ形になっているなら、それが最良かを判断する。
> - Rust の std の Nocturne の port（`ports/rust/`、`nocturne.patch` など。取り出し方は `ports/rust/README.md`）の env と今のフォルダーを、Nocturne のネイティブの口に合わせる。std の中だけの値の表で済むならそれでよい。libc に足すなら POSIX の名と形を写さず、Nocturne の形で。Go の port（`ports/go`）にも同じ欠けがあれば同じく直す。
> - std を組み直して `tools/rust/sysroot` を更新する（手順は `ports/rust/README.md`。`tools/` は git の外で、main の作業木の `D:\Programes\llm vibe slop os\tools\rust` にある。作業木から組むならその場所を使う）。
> - 変数なしで `kernel` と `qemu-image` が通ることを確かめる。空の build/ から組んでも通るか（生成物の規則、PATH に無い道具。持ち主の PowerShell に Git の usr/bin は無い）を読んで確かめる。
>
> 確かめ：QEMU で起動し、Terminal から `rusttest`（あれば Rust の試験のプログラム）を走らせ、env と今のフォルダーの扱いが動くこと。

## 今の形（原文で読んだもの）

- 起動の値は「設定」である。`struct n_launch` の `settings`（`"NAME=値"` の列）を、カーネルの `proc_launch` が引数の後ろに積み、`rdx` で渡す（`kernel/src/sys/proc.c`）。`USERFOLDER` と `TEMP` が無ければカーネルが加える。
- libc の `__libc_start(argc, argv, settings)` が `n_settings` に置く（`user/libc/crt0.asm`・`sys.c`）。
- libc の `getenv` は、まず libc 自身の表（`setting_set` が持つ、`static`）を引き、次に `n_settings` を引く。
- 今のフォルダーは libc の持ち物である。設定 `FOLDER` で起動元から受け取る。`n_folder_set` は名前を完全指定にし、フォルダーであることを確かめ、libc 自身の表の `FOLDER` を置き換える。`n_path_complete` は `getenv("FOLDER")` を基準にする。libc が起動するアプリ（`run_here`）へは今の `FOLDER` を渡す。
- カーネルは作業ディレクトリを持たず、完全なパスだけを受ける（`docs/nocturne-native-abi.md` §3）。
- libc に、外から設定を書き換える口は無い。23f80428 が `setenv`・`unsetenv` を消し、c5e0e7b6 が `n_folder_set` 専用の `static` の `setting_set` を残した。

## 判断：今のフォルダーを `FOLDER` で持つ形

この形を保つ。理由は三つ。

- カーネルが作業ディレクトリを持たないので、相対の名前の解決はアプリの中で閉じる。カーネルに thread をまたぐ可変の状態が無い。
- 起動元から子へ渡す道は、起動の設定の列のほかに無い。名前の付いた一行なので、渡すか渡さないかを起動元が決められる。
- libc の中では `n_folder_set` と `n_path_complete` の二つだけが触る。持ち主は一つ（libc）。

型の付いた欄（`struct n_launch` の `folder` など）にすれば名前の決め打ちが消える。これは ABI とカーネルの変更になるので、この仕事では行わない。

## 設計（std の env と今のフォルダー）

- std の環境は std の表である。最初の使用時（`lang_start` の `init`）に `n_settings` から読む。
- `set_var`・`remove_var` はこの表だけを変える。libc へは写さない。libc に口を足さない。
- 名前が空か、`=`・NUL を含むときと、値が NUL を含むときは `InvalidInput` を返す（設定の形 `"NAME=値"` を壊さないため）。
- 今のフォルダーの持ち主は libc のままである。`set_current_dir` は `n_folder_set` を呼び、続けて std の表の `FOLDER` を同じ完全指定の値にする。`current_dir` は `n_path_complete(".")` で libc の今のフォルダーを読む。
- 同じプログラムの C のコードは `getenv` で起動設定と今のフォルダーを読み、std の `set_var` は見ない。`std::process::Command` はまだ無いので、std の表を子へ渡す道は無い。作るときは std の表を `n_launch.settings` に渡す。

Go の port は libc を使わない（cgo なし）。`syscall.Setenv` は Go の runtime 自身の表で、`Chdir` もその表の `FOLDER` を置き換える（`ports/go/nocturne.patch` の `syscall_nocturne.go`）。同じ欠けは無い。変えていない。

## 変えたファイル

- `ports/rust/nocturne.patch`
  - `library/std/src/sys/env/nocturne.rs`：`setenv`・`unsetenv` は std の表だけを変える。名前と値の検査を足した。`abi::setenv`・`abi::unsetenv`・`cvt`・`run_with_cstr` を使わない。
  - `library/std/src/sys/pal/nocturne/abi.rs`：`setenv`・`unsetenv` の宣言を消した。
  - `library/std/src/sys/paths/nocturne.rs`：`chdir` の注釈を今の形にした（呼ぶものは同じ）。
- `ports/rust/apps/rusttest/main.rs`：`set_current_dir` の後に `env::var_os("FOLDER")` が同じフォルダーになる項目を足した。
- `ports/rust/README.md`：環境とフォルダーの行、確かめたことを今の形にした。
- `third_party/tinycc/conftest.c`：git に入れた（下の「作業木で組むときに出た欠陥」）。
- git の外：`tools/rust/src` の上の三つのファイルを同じ内容にした。`tools/rust/sysroot` を組み直した。`tools/rust/target/nocturne.patch` は今の差分になった。

差分は、`tools/rust/dl/rust-src-1.98.1.tar.xz` を作業場に展開し、`git init` して今の `nocturne.patch` を当て、`git diff` が元の差分と一字も違わないことを確かめてから編集し、`git diff` で作った。`tools/rust/src/library` と作業場の木は `diff -rq` で同じ。

## 消したもの

- std の `abi.rs` の `setenv`・`unsetenv` の宣言。
- std の env から libc の表へ写す処理。

## 組みの結果

- std：`ports/rust/build-std.sh`（`RUST_SRC`・`RUST_SYSROOT`・`RUST_TARGET_DIR` を本体の `tools/rust` に向けた）。`Finished release in 15.68s`、`sysroot: …/tools/rust/sysroot (16 libraries)`。警告なし。
- `python -X utf8 scripts/build.py -j 8 kernel`：警告 0、誤り 0（ログ `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\rustenv\build-kernel.log`）。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：通った。`RUST …/programs/rusttest`、`IMG build/nocturne.img (1152 MiB)`、`C: 197 folders, 2744 documents`（ログ `…\scratchpad\rustenv\build-image6.log`）。`rusttest` は `llvm-nm -u` で未解決の記号が無い。`build/c.list` に `C:\Nocturne\System\rusttest` がある。媒体は `D:\Programes\llm vibe slop os\build\wt\agent-a8e92549b332a2e57\nocturne.img`。本線の `build/nocturne.img` には触れていない。
- 組みは空の `build/wt/agent-a8e92549b332a2e57/` から始めた。このとき libc とブラウザーに `-Wmissing-field-initializers` の警告が 51 行出た（`user/libc/font.c` 5、`web/css.c` 1、`web/cssmath.c` 8、`web/cssprop.c` 35、`web/layout.c` 2）。どれもこの仕事の変更の外である。

本線（`D:\Programes\llm vibe slop os`）では `tools/rust/sysroot` を更新したので、次の `qemu-image` で `rusttest` が新しい std で link される。本線の作業木では組んでいない（本線の媒体を他の書き手が使っている）。

## 空の build/ から組むとき（読んで確かめたこと）

- 本線の配置（`BUILD=build`）では、`scripts/build_cxx.py` と `ports/mesa/generate.py` が `build/` に書き、Makefile の `$(BUILD)` と一致する。生成物の規則は order-only の依存（`$(MESA_GEN_STAMP)`、`$(BUILD)/cxx/libc++.a`）で並ぶ。
- `rust.mk` は `tools/rust/sysroot` が無ければ Rust のアプリを組まない。ところが `ports/mesa/nvk.mk` は `--sysroot tools/rust/sysroot` を決め打ちで使い、NVK は常に組まれる。だから `tools/rust/sysroot` の無い木からは `qemu-image` が組めない。README の「`tools/rust/sysroot` が無ければ通常の `make` は変わらない」は今の形と違う。
- PATH に要る道具：`rustc`・`cargo`（`C:\Users\cesta\.cargo\bin`）、`ld.lld`（`C:\Program Files\LLVM\bin`）、`cbindgen`・`bindgen`。build.py は `MSYS2_PATH_TYPE=inherit` で Windows の PATH を受け継ぎ、bash は `tools/msys64` のものを使う。Git の usr/bin は要らない。`build-std.sh` は sh の台本で、手で走らせる（make からは呼ばれない）。

## 作業木で組むときに出た欠陥

作業木では次の三つで止まった。直したのは一つ目だけである。

1. `third_party/tinycc/conftest.c` が git に無かった。tinycc の `.gitignore` の `conftest*` に当たり、取り込みで落ちていた。Makefile の `$(BUILD)/tcc/c2str.exe` がこれを要る。本線の作業木には未追跡のまま在る。新しい clone では組めない。本線のファイルをそのまま `git add -f` した（tinycc は LGPL-2.1）。
2. `scripts/build_cxx.py` と `ports/mesa/generate.py` は出力先を `<木の根>/build/…` に決め打ちしている。作業木では build.py が `BUILD=../../../build/wt/<名>` を渡すので、`-I$(BUILD)/cxx/upstream/libcxx/include` と `$(BUILD)/mesa/gen` が空になり、`cstdint` と `util/format/u_format_gen.h` が見つからない。直していない。確かめのために、作業木の `build` を `build/wt/<名>` への junction にして組んだ（終わりに外した）。
3. 作業木に `tools/` が無い。`nvk.mk` の決め打ちの `tools/rust/sysroot` で止まる。確かめのために作業木の `tools` を本体の `tools` への junction にした（終わりに外した）。このとき `$(PROGRAMS)/tsc` の overlay（`$(BUILD)/typescript/overlay.json`）の鍵が junction の道になり、Go が実の道で読むので当たらず、`syscall.SIGINT` が無いと止まった。`TYPESCRIPT=../../../tools/TypeScript` を渡して組んだ。

## QEMU で見たもの

起動の形：q35、UEFI（edk2）、NVMe、`-cpu host -accel whpx -smp 4 -m 4096M`、`-device VGA,xres=2560,yres=1440`、xHCI の usb-kbd と usb-tablet、e1000、HMP 45466・QMP 45467。窓付きで起こし、終わりに HMP の `quit` で止めた。

- serial.log：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\rustenv\serial.log`
  - `[   17.251149 0:1] store: C: "Nocturne" on nvme0, 1086 MiB, 261 MiB free`
  - `[   17.328129 0:1] wm: compositor started at 2560x1440`
  - `PANIC` の行は無い。
- デスクトップ：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\rustenv\boot.png`。
- Terminal を二度押しで開き、`rusttest` を打った：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\rustenv\term1.png`。最後の行は `rusttest: ok (50 checks)`。
- 遡った画面：`…\scratchpad\rustenv\term2-crop.png`。`vars: 4`、`ok env set_var`、`ok env vars() lists it`、`ok env remove_var`、`ok env missing`、`current_dir: C:\Users\User, temp_dir: C:\Users\User\AppData\Temp, home_dir: Some("C:\\Users\\User")`、`ok set_current_dir`、`ok FOLDER follows set_current_dir`。
- `cd Documents` の後で `rusttest`：`…\scratchpad\rustenv\term4-crop.png` と `term5-crop.png`。`current_dir: C:\Users\User\Documents`。最後は `rusttest: ok (50 checks)`、prompt は `nocturne:C:\Users\User\Documents$`。

## 残った欠陥と未確認

- 上の「作業木で組むときに出た欠陥」の 2 と 3：`build_cxx.py`・`generate.py` が `BUILD` を受けない。`nvk.mk` が `tools/rust/sysroot` を決め打ちし、sysroot の無い木で `qemu-image` が組めない。tsc の overlay が junction の道で当たらない。
- libc の今のフォルダーは thread に対して守られていない。`n_folder_set` の `setting_set` は `env_names`・`env_vals` を `realloc` し、別の thread の `n_path_complete` → `getenv` → `env_find` が同じ表を lock なしで読む。Rust の `set_current_dir` と別の thread のファイル操作が重なると、解放済みの表を読む。直す所は libc（`user/libc/sys.c`）である。
- `tests/navigatortest.c` が消えた `setenv("USERFOLDER", …)` を呼ぶ。ゲストの tcc で組む試験（`tests/runtests.c` の `navigator`）で、組めない。
- 本線の `build/programs/` に、前の link の失敗で残った `rusttest.*.rcgu.o` が十個ある。
- 本線の作業木での `qemu-image` は組んでいない。Hyper-V と実機では走らせていない。
- `set_var("FOLDER", …)` は std の表だけを変え、今のフォルダーは動かさない。今のフォルダーを変える口は `set_current_dir` である。
