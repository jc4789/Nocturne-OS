# プログラムと OS の境界の調べ：動的リンクと静的リンクと IPC の費用（サブエージェントの報告の全文）

モデルは Sonnet 5.5。判定は [app-boundary-20261102.md](app-boundary-20261102.md) にある。

## 依頼文

読むだけの調べものです。ファイルを変えません。Web の検索と取得を使ってください。

問い：共有ライブラリー（動的リンク）と静的リンクの費用と利点を、測った数字と一次の資料で知りたい。良い面も悪い面も同じだけ調べてください。

調べる項目：
1. 読込みの費用：起動の時間、再配置、ASLR と頁の共有の関係、prelink、dyld shared cache、Windows の preferred base と ASLR の頁の共有。測定の数字。
2. メモリー：共有で本当にどれだけ節約できるか。測定の数字。Drew DeVault の "Dynamic linking" の測定（公式でなければそう書く）、Android、ChromeOS、macOS の資料など。
3. 安全の修正を届けること：共有ライブラリーの修正で一度に届くという利点の実際。Heartbleed などの実例、静的リンクと Go・Rust の世界の修正の届け方、ライブラリーを入れ替えて動きが壊れた実例。
4. C の ABI の凍結：構造体の形、inline、マクロが ABI になること。libstdc++ の dual ABI、glibc の time64、Windows の FILE と CRT の境界、Hyrum's Law。Rust と C++ が安定した ABI を持たない理由。
5. 表面の大きさ：公開の関数の数と維持の費用。Win32 と libc の実例。小さな境界を選んだ例と結果。
6. 安定の境界を「関数の呼出し」でなく「プロセス間のメッセージ」に置く費用：IPC の遅延の測定の数字、速い IPC の実例（L4 系、Binder、Mach、XPC）、Windows の ALPC、共有メモリーの輪の使い方。
7. syscall の入口の小さなライブラリーの費用：vDSO の呼出しの費用の数字。

書き方：
- 日本語で書く。識別子と固有名詞は元の表記。一文に一つのことを書く。
- 主張ごとに出どころの URL を付ける。一次の資料を優先する。公式でない資料は「公式でない」と書く。数字には、何の機械で何を測ったかを添える。他の OS のソースコードは読まない。文書と記事と論文だけを読む。
- 読んで確かめたことと推測を分ける。取得できなかったページは、取得できなかったと書く。
- 自分の評価は最後に分けて書く。どれが良いかを決める必要はない。
- 最後の返事が報告の全文です。ファイルには書きません。

## 報告

# 共有ライブラリーと静的リンクの調べ（読むだけ。ファイルは変えていない）

## 先に

- 道具は WebSearch と WebFetch だけ。他の OS のソースコードは読んでいない。
- 次の項目は、測った数字を取れなかった。
  - prelink（実測）
  - dyld shared cache（Apple の数字）
  - Windows の ASLR の頁の共有（数字）
  - XPC と ALPC の往復の時間
  - Win32 の公開関数の総数
  - Drepper の "How To Write Shared Libraries" の本文。PDF は圧縮されていて、取得で本文を読めなかった。
- 検索の要約に頼った所は「検索の要約」と書いた。ページ本体を開いたものは「読んだ」と書いた。

## 1. 読込みの費用

読んで確かめたこと:
- 小さな C プログラムの起動の平均は、動的 137,263 ns、静的 64,048 ns（約 2.1 倍）。
  - 条件は、Arch Linux（2020-06-25 時点）、musl-gcc、1000 回、CLOCK_MONOTONIC、機械は一台。ハードウェアの記載は無い。
  - 出どころ: https://drewdevault.com/dynlib/
  - 公式でない個人ブログ。取得した結果では著者名が無く、ホストは redacted.moe と出た。Drew DeVault 本人かどうかは、この取得では確かめられていない。
  - 測ったのは自明なプログラムだけ。大きなプログラムでは差の割合が小さくなる見込みだが、これは推測。
- Allegro（公式でないブログ）の iOS アプリ。dylib が 57 個から 31 個に減ると、起動が 12〜26% 速くなった（iPhone 5s: 3.14 秒から 2.75 秒、iPad 2 の cold: 11.75 秒から 7.27 秒）。
  - ストップウォッチとスローモーション撮影で、6 回の平均。
  - 同じ記事の macOS 試験は、MacBook Pro Retina 13 インチ（2015 年前半）、Core i7 3.1 GHz、macOS 10.13.4。warm の起動は dyld2 0.737 秒、dyld3 0.726 秒、静的 0.676 秒。cold（通常のディスク）は dyld2 1.166 秒、dyld3 1.094 秒、静的 0.871 秒。
  - 出どころ: https://blog.allegro.tech/2018/05/Static-linking-vs-dyld3.html
  - 記事の本文は静的リンクのライブラリー数を 26、表は 31 と書いていて、食い違っている。
- 再配置と prelink（検索の要約。原文は未確認）。
  - Drepper は 1998 年の libc-hacker の投稿で「起動時のシンボル再配置は lazy binding でも起動時間の 70〜80%」と書いた。出どころ: https://sourceware.org/ml/libc-hacker/1998-12/msg00351.html
  - 2001 年の GNOME の投稿では、Red Hat 7.2 の既定で再配置の費用が prelink 無しでも大きく減ったと書いた。出どころ: https://lists.gnome.org/archives/gnome-hackers/2001-November/msg00222.html
  - prelink の欠点は、ライブラリーの場所が予測できることと、更新のたびにやり直すこと。
  - 数字は古く、後の glibc と GCC（hash 表の改善）で変わっている。
- dyld shared cache（Apple DTS の Quinn、公式の forum）。
  - 「よく使う dylib を macOS の製造で一つのファイルに pre-link する。起動を速くする多くの最適化ができる」。数字は無い。
  - 「全部が実装の詳細で、変わりうる」と書いている。出どころ: https://developer.apple.com/forums/thread/692383
- dyld3 の launch closure は、WWDC 2017 Session 413 で説明されている。システムの app の closure は shared cache に入り、第三者の app の closure は install のときに作られる。
  - 出どころ: https://wwdcnotes.com/notes/wwdc17/413
  - これは検索結果の要約から。スライドの数字は未確認。
- Windows の preferred base と ASLR（Raymond Chen は Microsoft の社員のブログ。公式の仕様書ではない）。
  - preferred base に載れば、fixup 無しで backing store から頁を読めて、プロセス間で共有できる。
  - 載れないと image 全体が再配置され、page file が裏になる。NT 3.1 では、同じ番地に再配置された二つのプロセスでも、複製が別々に作られた。
  - ASLR では、読込みの途中で fixup した頁だけが private になり、残りは共有される。カーネルは選んだ番地を覚えて、二つ目のプロセスにも同じ番地を使おうとする。
  - ASLR で再配置した場合の再配置の罰は無い。罰があるのは、別の割り当てが番地を取っていたときなど。
  - 出どころ: https://devblogs.microsoft.com/oldnewthing/20170120-00/?p=95225 と https://devblogs.microsoft.com/oldnewthing/20160413-00/?p=93301
  - Google の記事（公式でない）は、Windows の DLL は PIC でないため、番地が同じでないと code を共有できないと書き、再起動でだけ全 image の番地が新しくなると書いた。出どころ: https://cloud.google.com/blog/topics/threat-intelligence/six-facts-about-address-space-layout-randomization-on-windows
  - 上の二つは検索の要約。ASLR 下の頁の共有の測定値は取れなかった。
- 静的 PIE でも ASLR は効く（DeVault のページの主張。試験結果は無い）。

## 2. メモリー

- FreeBSD 6.2、2007 年、Spinellis の個人ブログ（公式でない、推定）。
  - Web サーバー: 共有 object 98 個、共有 16,790,901 バイト、mapping 1,002、静的なら約 198,815,270 バイトの推定（約 12 倍）。
  - ログインサーバー: 共有 object 58 個、11,285,262 バイト、静的なら約 515,107,268 バイト（約 46 倍）。
  - 方法は、Perl で fstat -m を読み、静的の大きさを足し合わせた推定。静的に組んだものを測ってはいない。本文は「50 倍」と書くが、表からは約 46 倍。機械は 2 台だけ。
  - 出どころ: https://www.spinellis.gr/blog/20071006
- DeVault: 動的な実行ファイルは、依存先の export する symbol の平均 4.6% しか使わない。
  - /usr/bin の 5688 個のうち、libc を使うのは 4496 個。半分以上のライブラリーは 0.1% 未満の実行ファイルにしか使われない。
  - これは頁の共有の測定ではない。静的リンクで本当に増える量は測っていない。
  - 出どころ: https://drewdevault.com/dynlib/
- Android の audioserver（AOSP の commit、検索の要約）。HAL の interface のライブラリーを静的にして、PSS が 15515 KB から 14997 KB に減った（Android 7.1、約 3%）。
  - 出どころ: https://android.googlesource.com/platform/bionic/+/81aad00%5E%21/
  - 静的にしたほうが小さくなった実例。
- Android の公式資料: zygote が framework の class と resource を前読みし、アプリは fork で copy-on-write のまま共有する。PSS は共有頁をプロセス数で割る。
  - 出どころ: https://developer.android.com/topic/performance/memory/guide/concepts
  - 検索の要約。節約量の数字は無い。
- ChromeOS の資料は調べていない。macOS は shared cache の説明だけで、節約量の数字は取れなかった。
- 査読つきの「共有と静的のメモリー」の論文は、この検索では見つからなかった。

## 3. 安全の修正

- Heartbleed。
  - Apache の指針: OpenSSL が mod_ssl に静的に入っているなら、httpd と mod_ssl を作り直す。出どころ: https://emptyhammock.com/blog/apache-http-server-and-cve-2014-0160-the-so-called-heartbleed-bug.html
  - openSUSE の投稿: 配布元が OpenSSL を静的に入れない方針なので、ライブラリーを入れ替えてサービスを再起動すれば済んだ。出どころ: https://lists.opensuse.org/archives/list/users@lists.opensuse.org/thread/ECGSUWMXAEARI36QLN5VOCTW2SH3B4J7
  - 公式 nginx の package は静的で、別に作り直しが要った（gist、公式でない）。出どころ: https://gist.github.com/robbyt/10502435
  - 報道（公式でない）: Secunia の記録で、100 社の 590 製品が脆弱だった。出どころ: https://www.bankinfosecurity.com/heartbleed-update-fixes-plateau-a-6981
  - どれも検索の要約。
- DeVault の算定（Arch、2019 年の CVE）。
  - libc を除いて、100 個超の binary に効く critical か high の CVE があるのは dbus、gnutls、cairo、libssh2、curl。
  - 全部を入れ替えて配ると 3.8 GiB、glibc を除くと 1.0 GiB。
  - 本人が書く限界は二つ。CVE が binary の作成より後のものを含む。多くの CVE が特定の code の道にしか効かない。どちらも量を出していない。
- Go の世界。
  - oss-security の投稿に、配布元が stdlib や依存の CVE のたびに全 Go binary を作り直す例がある（Alpine の 2026 年 4 月、Chainguard）。symbol 表を残すと govulncheck が使われる symbol を判定できる。出どころ: https://openwall.com/lists/oss-security/2026/04/20/1
  - Mageia の bug 報告: Go の binary の依存を追っていない。`go version -m` で外部依存は見えるが、標準の package は見えない。出どころ: https://bugs.mageia.org/show_bug.cgi?id=33940
  - Go 公式の脆弱性管理の文書は読めなかった。Rust については、この調べで一次の資料を取れていない。
- ライブラリーを入れ替えて壊れた例。
  - 実例の一次資料は、この検索では取れなかった。次の二つは、入れ替えが壊れうる仕組みの資料。
  - libstdc++ の dual ABI（下の 4）。
  - Microsoft の「CRT の物を DLL の境界で渡す誤り」（下の 4）。
- 新旧のライブラリーを入れ替えて動きが壊れた個別の事件（CVE の修正で挙動が変わった件など）は、確かめられていない。

## 4. C の ABI の凍結

- libstdc++ の dual ABI（GCC 公式）。
  - GCC 5.1 で `std::string` と `std::list` の新しい実装を入れた。C++11 が copy-on-write の string を禁じ、list に size の記録を要求したため。
  - soname を保つため、新しい版は inline namespace `std::__cxx11` に入れ、古い版と同じ library に同居させた。
  - `_GLIBCXX_USE_CXX11_ABI` の値が違う object を混ぜると、`std::__cxx11` や `[abi:cxx11]` の undefined reference になる。
  - 出どころ: https://gcc.gnu.org/onlinedocs/libstdc++/manual/using_dual_abi.html
- glibc の time64。
  - glibc 2.34 は旧 ABI と新 ABI を両方持つ。強制はせず、各 package が `-D_TIME_BITS=64` で選ぶ。`_TIME_BITS=64` は `_FILE_OFFSET_BITS=64` を要る。
  - Debian は armel と armhf の ABI を変える。i386 は除く。移行は 2024-02-27 に始まった。armhf を現地で移すのは難しく、全部を組み直す案（新しい triplet）が出た。
  - 出どころ: https://wiki.debian.org/ReleaseGoals/64bit-time と https://inbox.sourceware.org/libc-alpha/20221112042052.GL27919@mail.wookware.org/
  - 検索の要約。最終の結論は未確認。
  - ここでは `time_t` の大きさがマクロで決まり、ABI になる。
- Windows の CRT（Microsoft Learn）。
  - VS 2015 以降の UCRT（ucrtbase.dll）は Windows の部品として一つ。
  - CRT の copy が違うと（`/MT` で別々に static リンク）、`FILE*` を境界で渡して access violation になる例を載せている。同じ `/MD` なら通る。
  - CRT の heap をまたぐ確保と解放は heap の破壊を起こしうる。
  - 出どころ: https://learn.microsoft.com/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries
  - FILE 構造体の中身が非公開かどうかは、この資料では確かめられていない。
- Hyrum's Law（作者の公式ページ）: 利用者が十分いれば、観測できる振る舞いは全部、誰かに頼られる。性能、バグ、実装がそのまま interface になる、の例がある。出どころ: https://www.hyrumslaw.com/
- Rust に安定した ABI が無い理由。
  - 検索の要約: コンパイラーの team が、性能のために data の配置と Rust 同士の呼出し規約を変える自由を持ち続けている。`repr(Rust)` の配置は一つのコンパイラー内では安定だが、版をまたぐと変わりうる。`repr(C)` は型ごとに要る。
  - 出どころ: https://internals.rust-lang.org/t/when-is-the-abi-stable/10420 と https://viruta.org/index28.html
  - 公式の book ではなく forum と個人ブログ。C++ が ABI を持たない理由の一次の資料は、dual ABI のほかは取れていない。
- macOS の安定の境界（Apple の技術者の投稿）。
  - 「kernel の ABI は安定でない。安定を保証するのは動的リンクの境界、つまり libSystem の公開 symbol」。出どころ: https://lists.llvm.org/pipermail/llvm-dev/2017-June/114509.html
  - Go が libSystem 経由に変えた件は、Go の issue から検索で確かめた。出どころ: https://golang.org/issue/17490
  - コメントの内容で、リリースノートは未確認。

## 5. 表面の大きさ

- Win32 の kernel32.dll: 「2,000 に近い」export がある。数え方による（Geoff Chappell の個人の解析、公式でない）。出どころ: https://www.geoffchappell.com/studies/windows/win32/kernel32/api/
- Windows 1.0 は 450 未満、現在は数千（Wikipedia 系の要約。一次ではない）。
- Microsoft が仕様外の使い方まで支える互換の努力は、Wikipedia 系の要約と Raymond Chen の発言の引用だけ。
- libc の公開関数の数と維持の費用は、数字を取れなかった。
- 小さな境界を選んだ例: Fuchsia の Zircon は、syscall を vDSO だけから通す（下の 7）。
  - 公式文書が理由に挙げるのは、kernel に入る口を限る、kernel と vDSO の間が事実上の ABI になるのを防ぐ、の二つ。
  - 結果の測定（保守の費用など）は、この文書に無い。

## 6. プロセス間のメッセージの費用

- Android Binder（Android の公式文書）。
  - Android 8.0 の試験（日付 2017-05-17、4 CPU、cpu scaling 有効、機種名なし）。
  - `BM_sendVec_binderize` の往復は、payload 4〜1024 バイトで約 70 µs（CPU 時間は約 32.7 µs）。2 KB で 75 µs、64 KB で 215 µs。
  - hwbinder の latency 試験（3 組、各 5000 回）は、通常の scheduler の平均が 0.19〜0.2 ms、RT-fifo で 0.15〜0.16 ms。最悪は 4.8 ms。
  - 出どころ: https://source.android.com/docs/core/tests/vts/performance
  - 新しい機種の数字は取れなかった。
- L4 系（検索の要約。原論文の本文は取れていない）。
  - UNSW の講義資料は、i486 で L4 の IPC が約 250 cycle、Mach は約 5,750 cycle。出どころ: https://cgi.cse.unsw.edu.au/~cs9242/08/lectures/01-intro.pdf
  - Liedtke ほかの HotOS 1997 は、直接の IPC が Alpha で 45 cycle、Pentium で 121 cycle。出どころ: https://trustworthy.systems/publications/papers/Liedtke_ESHHIJ_97.abstract
  - Yale の講義資料は、1990 年代前半の典型を約 100 µs とし、L4 を 10〜20 倍速いとしている。出どころ: https://www.cs.yale.edu/flint/cs428/doc/L3toseL4.pdf
  - 機械の世代が違う。同じ資料の中でだけ比べられる。
- Mach: XNU の MPMMTest の README に、1 server と 4 client で平均 12.1 µs、約 83,000 message/秒とある。機械と大きさは不明。出どころ: https://git.saurik.com/apple/xnu.git/blame/7ddcb079202367355dddccdfa4318e57d50318be:/tools/tests/MPMMTest/README
  - 検索の要約。XPC の往復の測定は取れなかった。
- Windows ALPC: 測定値は取れなかった。Vista で LPC を書き直し、非同期、completion port、message の batch 化を持つ。内部で、公開の文書が少ない。出どころ: https://learn.microsoft.com/en-us/archive/blogs/ntdebugging/lpc-local-procedure-calls-part-1-architecture
- 共有メモリーの輪。
  - io_uring の設計: SQ は app だけが書き、CQ は kernel だけが書く。head と tail の持ち主が分かれ、追加の syscall を要らなくする（Axboe の 2019 年の論文を二次で確認。論文の本文と数字は未確認）。
  - Linux の io_uring は、kernel ソースの検索結果が出たが、読んでいない。

## 7. syscall の入口だけの小さなライブラリー（vDSO）

- man7 の vdso(7)。
  - kernel が全プロセスに写す小さな共有ライブラリー。clock_gettime、gettimeofday、time、getcpu など。時刻のデータを読むだけにして、kernel に入らない。
  - symbol に版がつく。base 番地は execve ごとに乱数。呼出しは strace と seccomp に見えない。
  - 数字は載っていない。出どころ: https://man7.org/linux/man-pages/man7/vdso.7.html
- 数字（検索の要約。どれも公式でない）。
  - Harvard CS161 の 2018 年の講義の実験で、100 万回の loop。getpid は約 235 ns/回、`clock_gettime(CLOCK_REALTIME)` は約 28 ns/回。出どころ: https://read.seas.harvard.edu/cs161/2018/lectures/lecture18
  - 開発者の gist では gettid が 44〜49 ns、vDSO の clock_gettime が約 20 ns。出どころ: https://gist.github.com/azat/622fa1f9a5d8e7d546ee9d294501961d
  - 別の substack は syscall 100〜200 ns、vDSO 約 5 ns と書くが、方法が無く、他と合わない。
  - 機械の型は、検索の要約では確認できなかった。
- Fuchsia Zircon の公式文書。vDSO が syscall の唯一の道。各 syscall は C の関数 ABI の ELF symbol。書込みの segment と dynamic relocation を持たない。kernel は呼出し元の PC を確かめ、不正なら例外にする。
  - 出どころ: https://fuchsia.dev/fuchsia-src/concepts/kernel/vdso
  - 数字は無い。
- 呼出しの費用そのものは、読んだ範囲では関数呼出し 1 回分と、データ読みの数回だけ。一つの代わり、kernel に入る費用が消える。

## 私の評価（推測を含む）

- 読込みの速さは、動的が不利。静的との差は、小さな program では大きい（DeVault で約 2 倍、ただし 1 台の一つの試験）。大きな program では割合が小さいはずだが、数字は取っていない。
- メモリーの節約は、古い推定（FreeBSD 2007）が最も大きく、新しい実測（Android の audioserver）は約 3% と小さい。使われる symbol が 4.6% という数字は、共有の利益が「少数のライブラリーに集中する」ことと合う。
- 安全の修正で「一度に届く」のは、OS の package が全部動的で、修正が届くまで再起動が要らないとき。Heartbleed では、静的な第三者の build が別に作り直しを要した。静的の側は、依存の目録を持てば作り直しで届けられるが、その目録の弱さを Mageia の報告が示している。
- ABI の凍結の費用は、C ABI の側にある。マクロ、構造体の形、`time_t` の大きさ、`FILE*` の持ち主が境界をまたぐ。Apple と Zircon は、境界を「関数名の集まり」に絞り、中を隠すことで、この費用を抑えている。
- IPC の費用は、設計で大きく動く（Mach の 5,750 cycle から L4 の 250 cycle まで。世代が違う）。Android の約 70 µs は、vDSO の約 20〜200 ns の三桁上。頻繁な小さな呼出しを IPC に置くと不利で、共有メモリーの輪に寄せるのが、io_uring の方向。
- 取れていない数字（prelink、ALPC、XPC、ASLR 下の頁共有、ChromeOS）は、決める前にもう一度、一次の資料を探す必要がある。
