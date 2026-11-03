# インストールの調べ：実行形式と配布の道（サブエージェントの報告の全文）

モデルは Sonnet 5.5。判定は [install-audit-20261102.md](install-audit-20261102.md) にある。

## 依頼文

D:\Programes\llm vibe slop os は Nocturne という独自の OS のリポジトリです。AGENTS.md と .claude/rules/map.md を先に読んでください。

読むだけの仕事です。ファイルを変えず、ビルドも QEMU も走らせず、コミットもしません。

問い：OS と別に組んで配った実行形式は、今の Nocturne で動くか。動くとして、次の版の OS でも動く保証はどこにあるか。特に次を、コードの事実として調べてください。

1. 実行形式：kernel/src/sys/proc.c の proc_launch と ELF の読み方。受ける形式（静的、PIE、動的）。動的リンカーや共有ライブラリーの有無。kernel/src/mm/program.c の program の text。部品の探し方（プログラムと同じ場所の .so など）。
2. libc の結び方：Makefile の LIBC_LINK、crt0。各プログラムは libc を静的に持つか。syscall の番号と構造体（common/abi.h、user/libc/sys.c）はアプリの中に焼き込まれるか。
3. ABI の安定：ABI の版の番号、版の確かめ、古いプログラムを動かす方針。docs/nocturne-native-abi.md の記述。syscall の番号の付け直しの歴史は git log -p の軽い確認で分かる範囲。起動時にプログラムが必要な版を宣言する形はあるか。
4. SDK：scripts/mksdk.sh、build/sdk、C:\Nocturne\SDK。ゲストの中の tcc、clang の使い方。ホストの上で OS の外で組む道（クロスコンパイル）はあるか。
5. 言語の実行系：ports/go、ports/rust、ports/typescript、ports/llvm。組んだものの置き場所（`user/apps/*.ts` の扱いを含む）。スクリプトのプログラムの起こし方（解釈系の指定）。
6. 配る形式：パッケージ、アーカイブ、展開の道具、インストーラー、更新、署名、ハッシュの確かめ。repo 全体を zip、tar、msi、installer、package、manifest、signature、updater などの語で探す。libc にある展開の部品（deflate など）。ブラウザーのダウンロードの道（user/apps/browser*.c、user/libc/webnet.c）はどこへ保存し、保存した実行形式を起こせるか。
7. OS 自身の更新：動いている機械の OS を新しい版へ上げる道はあるか。boot/limine.conf、settings.rec、host/store/store.c の役目。
8. プログラムの宣言：能力や権限をプログラムが宣言する manifest の形はあるか。一般のプログラムはどの能力を持ち、何に触れるか（N_CLASS_*、N_CAP_*、common/authority_abi.h）。同じ利用者のプログラムどうしの分離の有無。

書き方：
- 日本語で書く。識別子とパスは元の表記。一文に一つのことを書く。結論から書く。
- 主張ごとに file_path:line を付ける。読んだ原文で確かめた事実だけを「事実」と書き、推測は「推測」と書く。確かめていないことは確かめていないと書く。
- 「ある」「無い」は、探した語と場所を添える（無いことの根拠）。
- 良し悪しの判定より、何がどうなっているかを詳しく書く。
- 最後の返事が報告の全文です。ファイルには書きません。

## 報告

## 結論

- OS と別に組んだ実行形式は、今の Nocturne で動く。条件は、その OS の `common/abi.h` と同じ版に対して静的リンクした ELF64 の ET_EXEC であること。
- 次の版の OS でも動く保証は、コードにも文書にも無い。むしろ「古い形を持たない」と abi.h に明記してある。
- ABI の版の番号、プログラムが必要な版を宣言する形、manifest、署名、パッケージ、インストーラー、アプリの更新の道は、探した範囲では無い。

## 1. 実行形式

- 事実：受ける形式は ELF64、x86-64、`eh.type == 2`（ET_EXEC）だけ。`kernel/src/sys/proc.c:184-187`。
- 事実：PT_LOAD だけを読む。PT_INTERP、PT_DYNAMIC、再配置の処理は無い。`proc.c:34`（定義は PT_LOAD のみ）と `proc.c:210`。
- 事実：PIE（ET_DYN）は `type != 2` で `N_ERR_NOT_PROGRAM` になる。`proc.c:184`。
- 無いことの根拠：`rg "dlopen|PT_INTERP|ld\.so|shared librar|dynamic link"` を `.c/.h/.md/Makefile` にかけた。自前の部品には当たらなかった。当たったのは `user/libc/web/paint.c:26` のコメントと `README_EN.md:612` の "No dynamic linking and no swap. Every program is linked statically." だけ。
- 事実：リンクは `-static --no-dynamic-linker`、`-fno-pic -fno-pie`。`Makefile:103,107`。
- 事実：PT_LOAD の `vaddr` が `USER_MMAP_BASE` 以上だと失敗する。`proc.c:211`。
- 事実：エントリが実行可でない segment には、古い tcc の組みのために VM_X を足す。`proc.c:215-219`。
- 事実：読み取り専用で他の segment と頁を共有しない segment は、`program_map` で同じプログラムを走らせる全プロセスと一つの frame の組を共有する。それ以外はコピーする。`proc.c:221-233`。
- 事実：`mm/program.c` は、文書の identity（`store_document_identity`）と `entry.id` が同じ間だけ cache を再利用する。文書が変われば読み直す。`program.c:123-151`。
- 事実：cache から外れるのは、idle のプログラムが全メモリーの 1/16 を超えるとき、または空きが 1/8 を割るとき。`program.c:76-99`。
- 事実：部品の探し方（同じ場所の .so など）は無い。カーネルは一つの文書だけを読む。
- 事実：プログラムの名前（パスの最後の部分）は `program_name` が決める。`proc.c:141`。
- 事実：開始のとき、`rip=eh.entry`、`rdi=argc`、`rsi=argv`、`rdx=struct n_start*`。`proc.c:319-322`。
- 事実：`proc_launch` は `store_document_open(path, N_DOCUMENT_READ, false, &doc)` で開く。`proc.c:177`。
- 事実：`place_find` は第二引数が false のとき `who = NULL` にする。`kernel/src/fs/store.c:624`。
- 推測：そのため、この開き方は呼ぶ側の身元でパスの権限を確かめていない。`store_document_open` の本体までは読んでいないので、確定ではない。
- 事実：ELF でない文書の launch は `-N_ERR_NOT_PROGRAM` を返す。そのとき libc の `launch_program` が、文書の先頭行を読む。`user/libc/sys.c:429-448`。
- 事実：先頭が `#!` なら、その行の解釈系で起こす。引数は `[interp, path, 残り]`。`sys.c:54-68,433-446`。
- 事実：`#!` が無ければ `C:\Nocturne\System\sh` で起こす。`sys.c:68`。
- 事実：この道はカーネルでなく libc の中にある。つまり、別の言語系が自前で `SYS_APP_LAUNCH` を呼ぶと、この解釈系の道は働かない（推測）。

## 2. libc の結び方

- 事実：各プログラムは `crt0.asm.o` と自分の `.o` を、`--start-lib @$(LIBC_RSP) --end-lib` で libc の全 member に lazy にリンクする。`Makefile:349,363,394,495,525`。
- 事実：`LIBC_LINK` は libc、BearSSL、QuickJS、Lexbor、Expat、FFmpeg、FreeType、wabt、C++ 実行系、Mesa、Zink、NVK などの object の一覧。`Makefile:328-329`。
- 事実：使われた member だけが入る。ほかの member は入らない。
- 事実：`crt0` は `_start` から `__libc_start` を呼ぶだけ。`user/libc/crt0.asm:1-12`。
- 事実：syscall の番号は `__syscall` の `rax` に即値で焼き込まれる。`user/libc/sys.c:36-43` と `common/abi.h` の `enum`。
- 事実：構造体（`n_launch`、`n_entry`、`n_start` など）の配置と大きさも、アプリの中のコンパイル済み code に焼き込まれる。
- 事実：クロック（`N_CLOCK_ADDRESS` の `struct n_clock`）はアプリの中の code が直接読む。`sys.c:76-100`。
- 推測：したがって、ABI の番号や構造体が一つでも変われば、古い実行形式は静かに壊れうる。

## 3. ABI の安定

- 事実：`common/abi.h` の syscall の番号の前に次の文がある。「The ABI keeps no older form of a call: a program is built against the abi.h of the system it runs on.」`abi.h:53-55`。
- 事実：同じ箇所に、未割り当て番号の一覧がある（1-18、21-25 など）。
- 事実：ABI の版の番号は無い。`rg "ABI_VERSION|abi_version|N_ABI|ABI version"`（third_party、ports、LEDGER を除く）の当たりは `user/include/c++/__config_site:5` の `_LIBCPP_ABI_VERSION` だけ。
- 事実：`struct n_sysinfo` の `os` は文字列 `OS_NAME " " OS_VERSION`（`kernel/src/kernel.h:9` の `0.9 "Moonrise"`）。`syscall.c:221`。数値の版ではなく、比べる形にもなっていない。
- 事実：起動時にプログラムが必要な版を宣言する形は無い。ELF の読み方（`proc.c:184-189`）に版の項目は無い。
- 事実：`n_start`（`abi.h:299-305`）にも版は無い。
- 事実：部分的な互換の例が一つある。`docs/nocturne-native-abi.md:170-172` に、`n_entry_info`/`n_folder_list` は `SYS_ENTRY_INFO_SIZED`/`SYS_FOLDER_LIST_SIZED` に自分の長さを渡し、「カーネルの知る形より長ければ `N_ERR_UNSUPPORTED`」とある。`id` より前の SDK で作ったプログラムの旧呼出しについても書いてある（その先は読んでいない）。
- 事実：番号の付け直しの歴史。リポジトリは 717 コミット、最初のコミットは 2026-10-05。
- 事実：`common/abi.h` の履歴は 91 コミット。直近 12 件のうち 7 件で `SYS_` の行が削除または変更されている。例は `c51812ff`、`54ec4f87`、`6be72a3d`、`78d65017`（5 行消し 7 行足し）。
- 事実：`78d65017` と `fa54dda3` は、音や網の方針をカーネルからサービスへ移したコミット。口が実際に消えたり変わったりしている。
- 推測：「付け直し」と言える番号の移動が何件かは、`git log -p` を細かく見ていないので確かめていない。「消す・足す・変える」が頻繁に起きていることだけが確か。

## 4. SDK とクロス

- 事実：`scripts/mksdk.sh` が `build/sdk`（`Makefile:545`）を作り、`scripts/layout.py:85` が `C:\Nocturne\SDK` に入れる。
- 事実：中身は、`Include`（abi.h、authority_abi.h、services_abi.h、settings_abi.h、audio_abi.h、input_abi.h、gpu_abi.h、display_abi.h、user/include、BearSSL、Lexbor、QuickJS、nvidia の header）。
- 事実：`Lib\crt1.o`、`crti.o`、`crtn.o`、`libc.a`（上の全 member）、`user.ld`、`libwasm-rt.a`、`Lib\tcc`、`Source\Apps`、`Source\libc`、`Source\wasm2c`。`mksdk.sh:1-24`。
- 事実：`--llvm` を渡すと clang の header と runtime、libc++ 23、libunwind も入る。`mksdk.sh:19-24`。
- 事実：OS の中の tcc の使い方は `tcc -o C:\Programs\Hello\hello hello.c`。「Nocturne の libc と静的にリンク」と README にある。`README.md:511-516`。
- 事実：ゲストの中の clang の組み方は `mksdk.sh` の説明（`user.ld` を clang の driver が使う）。実際に OS の中で clang を走らせる手順の全体までは読んでいない。
- 事実：ホストで OS の外で組む道はある。ただし、公開された「SDK を取り出して使う道」ではなく、このリポジトリのビルドに組み込まれた道だけ。
  - C は `clang --target=x86_64-unknown-none-elf`、`ld.lld` で、`user/user.ld` と `crt0.asm.o`。`Makefile:103-107,349,363`。
  - C++ は `scripts/build.py cxx`。`README.md:524-527`。
  - Go は `ports/go/README.md` に「ホストでツールチェーンを作り、静的 ELF をクロスコンパイル」とある。
  - Rust は `ports/rust/rustc-nocturne.sh` が `build/u/user/libc/crt0.asm.o` と `build/libc-objects.list` を使う。`ports/rust/README.md`。
- 推測：どれもこのリポジトリのビルドの出力（crt0.asm.o と libc-objects.list）に依存している。ビルド済みの SDK だけを別の Windows 機に置いて組む道が文書化されているかは、確かめていない。

## 5. 言語の実行系

- Go：`ports/go`。`GOOS=nocturne GOARCH=amd64`、静的 ELF（ET_EXEC、0x400000 から）をクロスコンパイル。`ports/go/README.md`。
  - 事実：Go は Nocturne の syscall を直接呼ぶ（`syscall` パッケージ）。
  - 事実：アプリは `ports/go/apps/<名前>/main.go` に置くと `C:\Nocturne\System\<名前>`（デスクトップのアプリは `C:\Programs\<アプリ>\<名前>`）に入る。
  - 事実：`GO` を渡さない通常のビルドは Go のアプリを作らない。
- Rust：`ports/rust`。標準ライブラリーが Nocturne の C ライブラリー（`n_*`）を `extern "C"` で呼ぶ。C と同じ `user/user.ld`、`crt0`、libc の object で `ld.lld` が静的リンク。
  - 事実：`tools/rust/sysroot` が無ければ何も作らない。`ports/rust/README.md`。
- TypeScript：`ports/typescript` は tsc（Go で書かれた）を `GOOS=nocturne` でビルドして `C:\Nocturne\System\tsc` に入れる。
  - 事実：`user/apps/*.ts` は `Makefile:446-457` で、ホストの `build/host/tsc.exe` が型検査とコンパイルをして `C:\Nocturne\System\<名前>.js` になる。
  - 事実：起こし方は `programs.c` の例で `SYSTEM "js"` に引数 `SYSTEM "about.js"`（`user/apps/programs.c`、about の行）。つまり `js`（QuickJS のプログラム）が解釈系で、プログラム台帳の項目に解釈系を指定している。
  - 事実：`#!` 行の道（`sys.c:54-68`）とは別に、台帳が `path=js, argument=about.js` を持つ。
- LLVM：`ports/llvm`（`build.py`、`llvm.mk`、`nocturne.patch`）。`mksdk.sh --llvm` で SDK に入る。
- 事実：組んだものの置き場所の規則は、`C:\Nocturne\System\<名前>` がシステム、`C:\Programs\<アプリ>\<名前>` がアプリ。`abi.h:222-223`。`ports/go/README.md`。
- 事実：WASM は `wasm2c` で C にして SDK のランタイムと一緒に組む。`README.md:575`。

## 6. 配る形式

- 探した語：`zip`、`tar`、`msi`、`installer`、`package`、`manifest`、`signature`、`updater`、`sha256`、`inflate`、`deflate`、`.nup`、`.npk`。範囲は `user kernel common scripts host Makefile boot rootfs`（`.c .h .py .sh Makefile`）。
- 事実：当たったファイルの大半は web エンジン、TLS、HTTP、PNG、フォントなど。`deflate.c` は HTTP の content coding 用の「システムで唯一の DEFLATE」。`user/include/deflate.h:1-11`。
- 事実：`deflate.h:11` の `DEFLATE_RAW/ZLIB/GZIP` を圧縮・展開の両方向で持つ。zip や tar の読み手は、当たった中には見ていない。
- 無いことの根拠：アーカイブを展開してアプリを入れる道具、パッケージの形式、インストーラー、ハッシュや署名の確かめを実装した箇所は、上の語で見つからなかった。当たった `accounts.c` の語は、アカウントの標準フォルダーの名前の一覧など。
- 事実：ブラウザーのダウンロードの道は見つからなかった。`rg -i "download|content-disposition|save as"` を `user/apps/browser*.c`、`user/libc/webnet.c`、`user/libc/web/*.c` にかけた。実装の当たりは無い（`webnet.c:20,1022,1359` はコメント）。
- 事実：`Downloads` は、新しい利用者の標準フォルダーの名前として作られる（`user/apps/accounts.c:175`）。
- 推測：ブラウザーが応答をファイルに保存して実行形式を起こす道は無い。`browser.c` 全体を読んではいない。
- 事実：`user/apps/fetch.c` は「download a URL over HTTP or HTTPS」と頭にある。CLI の道具で、実行形式の取得に使えるかもしれない（推測）。
- 事実：実行形式を置く場所は `C:\Programs\<アプリ>`。書くのは admin 以上。`kernel/src/fs/store.c:603`（`AREA_PROGRAMS: return !write || admin`）。
- 事実：台帳へ足すのは `N_PROGRAMS_REGISTER`、admin 以上。`user/apps/programs.c:484-485`、`common/services_abi.h:117`。
- 事実：台帳は、開始メニューと「開く」の既定のためのもの。起こす権限を決めるものではない（`SYS_APP_LAUNCH` は台帳を見ない。`syscall.c:807-880`）。

## 7. OS 自身の更新

- 事実：`boot/limine.conf` の項目は一つ。`kernel.elf` と、module として `settings.rec`（8K の起動の記録）。`boot/limine.conf`、`scripts/mkimage.sh:41-43,60-61`。
- 事実：`host/store/store.c` はホストの Windows 用の道具で、C: を作り、詰め、検査する（`put`、`fill`、`check` など）。OS の中の更新の道具ではない。`host/store/store.c:1-40`。
- 事実：更新の道は `hyperv.ps1 -Update` だけ。VM を止め、新しいビルドの「起動部分、`C:\Nocturne`、付属アプリのフォルダー」を VM のディスクへ入れる。ユーザーのファイルと `C:\Programs` のユーザーのアプリは残る。`README.md:230`、`hyperv.ps1` の頭。
- 事実：Hyper-V はユーザーの担当で、私は触れていない。
- 無いことの根拠：`user/apps/*.c` に `kernel.elf` を書く処理は無い（`rg kernel.elf user/apps`）。動いている機械の中から OS を新しい版へ上げる道、ロールバック、署名の確かめは、探した範囲では見つからなかった。
- 推測：実機は、`build/nocturne.img` などの媒体を新しく作って入れ直す形。

## 8. プログラムの宣言と権限

- 事実：manifest のような宣言の形は無い。`rg "manifest"` は実行系のコードに当たらなかった。
- 事実：`struct n_identity` は `user`、`session`、`cls`、`reserved`、`program` を持つ。`common/authority_abi.h:6-13`。
- 事実：`enum n_class` は `N_CLASS_USER(0)`、`N_CLASS_ADMIN(1)`、`N_CLASS_SYSTEM(2)`。`authority_abi.h:5`。
- 事実：能力は `N_CAP_ROOT`、`SERVICE_ROOT`、`IDENTITY_GRANT`、`PCI_CLAIM`、`POWER`、`SCREEN_READ`、`SETTINGS_APPLY`、`STORE_PRIVATE`、`SHELL`、`NETWORK`、`CLOCK`、`DISPLAY`、`AUDIO_DEVICE`。`authority_abi.h:16-30`。
- 事実：子の身元は、親と同じ user・session・cls が既定。`cls` は親を超えられない。user や session を変えるには `N_CAP_IDENTITY_GRANT` の handle が要る。`kernel/src/sys/authority.c:35-52`、`abi.h:288-290`。
- 事実：能力は launch の `handles` の列で渡す。子は `SYS_CAP_FIND` で引く。`abi.h:290`。
- 事実：`program` の番号は、パスの各部分を畳んだ名前で FNV-1a をとった値。`store_path_number`。`store.c:659-677`。
- 事実：`N_LAUNCH_SAME_PROGRAM` が無ければ、起こした文書のパスで決まる。`authority.c:49-51`。
- 事実：同じパスなら同じ番号。内容の署名ではない。
- 事実：一般のプログラムは、親（普通は利用者のシェル）から cls=USER の身元を受け、能力は渡されたものだけ。
- 推測：一般のプログラムは、原則として能力を持たない。シェルが何を渡すかは `user/apps/shell.c` や `session.c` を読んでいない。
- 事実：C: への権限は身元の cls と利用者で決まる。`place_allows`（`store.c:600-609`）。
  - `AREA_SYSTEM`（`C:\Nocturne`）は読めるが、書くのは SYSTEM だけ。
  - `AREA_PROGRAMS` は読めるが、書くのは admin 以上。
  - `AREA_HOME`（`C:\Users\<名前>`）は admin か、持ち主の利用者だけ。
  - `AREA_OTHER` は admin だけ。
- 事実：同じ利用者の USER 級のプログラムどうしは、`identity_may_reach` で互いに届く。`authority.c:30-34`。「同じ user かつ同じ session の USER 級」は同じ範囲。
- 事実：同じ利用者のプログラムどうしの身元の区別は、`identity_covers` の `identity_same`（`program` も比べる）だけ。`authority.c:20-26`。
- 事実：アドレス空間はアプリごとに別。`README_EN.md:604-605` は「Every app is isolated in its own address space」とある。
- 事実：同じ README_EN.md の「No users or permissions」「CPU 0 does the kernel's work」の二行は、今のコード（`authority.c`、`kernel.md` の対称カーネル）と食い違っている。古い記述（`README_EN.md:604-610`）。
- 事実：サービスは `user/apps/services.c` の `declared[]` に、名前、プログラム、cls、能力（`CAPS(...)`）、`clients_from` を宣言する。例 `services.c:61-96`。
- 推測：この宣言はシステムのサービスのためのもので、配布するプログラムが自分で宣言する形ではない。
- 事実：特権の syscall は `cap_check`（例 `syscall.c:1041,1219-1280,1360`）か `identity_at_least`（`syscall.c:944,1286`）で確かめる。

## 配るのに足りないもの（事実の並び）

- ABI の版の番号と、起動時の版の確かめ。
- 動的リンクまたは共有ライブラリー。
- manifest、署名、ハッシュ、パッケージ、インストーラー。
- OS の中からの更新と、ロールバック。
- ブラウザーからのダウンロード（探した範囲では未実装）。

## 確かめていないこと

- `store_document_open` の本体（`launch` が権限を確かめるか）。
- `browser.c` 全体。
- `shell.c` と `session.c` が子に渡す能力。
- ゲストの中の clang の実行手順の全体。
- `git log -p` による番号の付け直しの詳細（件数のみ）。

主に読んだファイル：
- `D:\Programes\llm vibe slop os\kernel\src\sys\proc.c`
- `D:\Programes\llm vibe slop os\kernel\src\mm\program.c`
- `D:\Programes\llm vibe slop os\common\abi.h`
- `D:\Programes\llm vibe slop os\common\authority_abi.h`
- `D:\Programes\llm vibe slop os\user\libc\sys.c`
- `D:\Programes\llm vibe slop os\user\libc\crt0.asm`
- `D:\Programes\llm vibe slop os\Makefile`
- `D:\Programes\llm vibe slop os\scripts\mksdk.sh`
- `D:\Programes\llm vibe slop os\kernel\src\sys\authority.c`
- `D:\Programes\llm vibe slop os\kernel\src\fs\store.c`
- `D:\Programes\llm vibe slop os\user\apps\programs.c`
- `D:\Programes\llm vibe slop os\user\apps\services.c`
- `D:\Programes\llm vibe slop os\boot\limine.conf`
- `D:\Programes\llm vibe slop os\README.md`
- `D:\Programes\llm vibe slop os\README_EN.md`
- `D:\Programes\llm vibe slop os\ports\go\README.md`
- `D:\Programes\llm vibe slop os\ports\rust\README.md`
- `D:\Programes\llm vibe slop os\ports\typescript\README.md`
