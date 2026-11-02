# プログラムと OS の境界の調べ：Fuchsia ほかの境界の形（サブエージェントの報告の全文）

モデルは Sonnet 5.5。判定は [app-boundary-20261102.md](app-boundary-20261102.md) にある。

## 依頼文

読むだけの調べものです。ファイルを変えません。Web の検索と取得を使ってください。

問い：次の OS と仕組みで、プログラムと OS の境界はどこに、どんな形で置かれているか。その形の費用と失敗は何か。古いプログラムを、OS の進化の後も動かすために、何をしているか。

調べる対象：
- Fuchsia：vDSO だけが syscall の入口であること（libzircon）、syscall の呼出し元の制限、libc の置き場所（パッケージの中か）、FIDL の版と進化の規則、API level と ABI revision、components とパッケージ、capability routing、実際に保つ窓の長さ。
- OpenBSD：pinsyscalls、msyscall、syscall は libc からだけ、libc.so の版を上げる方針、static の扱い。
- Go の runtime が macOS と OpenBSD で syscall の直接呼出しをやめた理由と経緯。
- Linux：syscall を保つ方針、vDSO、glibc の symbol versioning と「新しい機械で組むと古い機械で動かない」問題、静的リンクの実際、Flatpak のランタイム。
- WebAssembly：WASI と Component Model、WIT、言語に依らない境界、能力の渡し方、性能の費用、現実の成熟度。
- その他、参考になるもの：Plan 9 の静的リンク、Haiku の ABI、Redox の relibc、Genode の ABI 分離（ld.lib.so の ABI ダミー）、seL4 の上の仕組み、Nix の内容で引く保存、Android の Binder の AIDL の安定な版。
- 能力（capability）と型のある channel の上で OS のサービスを呼ぶ形は、API の版の問題をどう変えるか。

各項目について：
- 何をしているか。
- 何が得られ、何を払っているか。作り手自身か一次の資料が述べる費用と失敗を優先する。数字があれば数字を出す。
- まだ広くは行われていない形、研究の段階の形があれば、そう書く。

書き方：
- 日本語で書く。識別子と固有名詞は元の表記。一文に一つのことを書く。
- 主張ごとに出どころの URL を付ける。作った者の公式の文書を優先する。公式でない資料は「公式でない」と書く。他の OS のソースコードは読まない。文書と記事だけを読む。
- 読んで確かめたことと推測を分ける。取得できなかったページは、取得できなかったと書く。
- 自分の評価は最後に分けて書く。どれが良いかを決める必要はない。
- 最後の返事が報告の全文です。ファイルには書きません。

## 報告

# 調査報告：プログラムと OS の境界の形、その費用、古いプログラムの保ち方

## 0. 読み方の注意
- 取得は WebFetch（小さなモデルが頁を要約して返す）と WebSearch で行った。頁の原文を直接は読んでいない。引用に見える文は要約器の出力で、一字一句の原文とは限らない。
- 「確認」は、その URL の要約にそう書いてあったこと。「推測」と書いた所は、資料に無いこと。
- 取得できなかった頁：
  - Fuchsia `concepts/versioning/abi-revisions`、`rfcs/0246_api_level_data_type`、`rfcs/0235_...`、`reference/platform-spec/versions`：404。
  - Fuchsia FIDL の `development/languages/fidl/guides/compatibility`：頁が「古い。新しい手引きを書く予定」とだけ言う。
  - OpenBSD `man.openbsd.org/msyscall.2`：404。OpenBSD FAQ は libc の版の方針を載せていなかった。
  - Plan 9 の FAQ と「なぜ動的リンクが無いか」の頁：404。Haiku の docs 頁：404。
  - glibc マニュアルの静的リンクの頁：リダイレクトのみ。sourceware.org の wiki：bot 対策で拒否。
- 他の OS のソースコードは読んでいない。

## 1. Fuchsia

### 1.1 境界の置き場所：vDSO だけが syscall の入口
- Zircon の vDSO は、カーネルが提供する読み取り専用の仮想 ELF 共有ライブラリ。各 syscall は ELF の symbol になっている。文書は vDSO を「syscall に入る唯一の手段」と書く。
  https://fuchsia.dev/fuchsia-src/concepts/kernel/vdso
- カーネルに入る機械語命令は ABI ではない。ユーザーコードが直接実行するのは不正。
- 呼出し元の制限は、カーネルが次のように強制する。
  - 実行可能な vDSO の mapping は一つだけ。offset と大きさが一致しなければならず、一度作ると外せない。違反は `ZX_ERR_ACCESS_DENIED`。
  - syscall 命令の PC を、既知の vDSO 内の位置と照らす。違えば、そのスレッドに未定義命令に似た同期例外が起きる。
- プログラムの loader が、新しい process ごとに vDSO を map し、先頭スレッドに base address を渡す。process を起こす process は vDSO の VMO handle を持つ必要がある。
- 費用として文書が述べること：
  - 定数を返す呼出しは vDSO の読み取り専用データから返す。カーネルに入る費用を払わない。
  - カーネルは、ユーザー空間からの全ての入り方に頑丈でなければならない。強制の検査は危険の軽減で、除去ではない。
  - vDSO のコードは状態を持たず、再入可能でなければならない。
- 性能の数字は文書に無い。

### 1.2 libc の置き場所
- Fuchsia は `libc.so` を提供し、プログラムは動的に link できる。プログラムは使わなくてよい。自前の libc を持つことも、無しにもできる。
  https://fuchsia.dev/fuchsia-src/concepts/kernel/libc
- libc 自身は file I/O をしない。弱い symbol（`open`、`write` など）が失敗するだけで、実際の file I/O と socket は `fdio.so` が channel IPC で行う。process の起動は `fdio_spawn`。
- libc が package の中に入るのか、platform が持つのかは、取得できた公式文書に書いていなかった。
- 古い build 資料（公式の googlesource の古い版）は次を示す。musl 由来の libc は link 時 `libc.so`、実行時は `ld.so.1` の名で入る。`dist/lib/ld.so.1` として package に載る。
  https://fuchsia.googlesource.com/fuchsia/+/cc6f71c/zircon/system/ulib/c/BUILD.gn
  https://fuchsia.googlesource.com/fuchsia/+/8b2bb8a5d1a0/zircon/system/ulib/c/BUILD.zircon.gn
- 推測：この資料からは、libc が各 package に同梱される形に見える。現行の配置は確認していない。
- 古い docs（googlesource の旧版）は「静的リンクの libc は非対応で、全てが libc.so を動的に link する」と書くと、検索の要約は述べる。現行の文書では未確認。
  https://fuchsia.googlesource.com/zircon/+/422991c6a7b3c1d555a7b862e80609124354b55c/docs/libc.md

### 1.3 API level と ABI revision
- API level は番号で、milestone に同期する。公開されたら不変。SDK の利用者は一つの level を選ぶ。
  https://fuchsia.dev/fuchsia-src/concepts/versioning/api_levels
- build 時、SDK が対象 level の ABI revision を package に刻印する。起動時に platform が刻印を確かめる。platform がその level をまだ支えていれば動き、でなければ Component Manager が起動を止める。
- level の局面は、Supported（動く。SDK で組める）、Sunset（動く。SDK では組めない）、Retired（動かない）の順。
- ABI revision は、不透明で無作為な 64 bit の識別子。API level は人のための番号。
  https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0002_platform_versioning
- 窓の長さ：
  - RFC-0239 は、level の sunset と retire に決まった日程は無いと言う。
  - sunset は「partner がもうその level で組まなくなったら」、retire は「partner がもうその binary を動かしたくなくなったら」。
  - 「もっと正式な方針が要るが、今はこれでよい」と書く。
  - 具体の数は一つだけ：F15 の milestone が level 11 から 15 の五つを同時に支えた。
  https://fuchsia.dev/docs/contribute/governance/rfcs/0239_platform_versioning_in_practice
- 古い数字として、RFC-0015 は 2020-11-09 時点で「6 週間の互換窓が必要。強制の仕組みは無い」と書く。現行ではない見込み。
  https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0015_cts
- ABI revision の検査の道は、resolver が package の `meta.far` から ABI revision の file を読み、Component Manager へ渡す。不支持なら resolve が失敗する。
  - 失敗した capability の routing は channel を epitaph つきで閉じる。原因は log を見ないと分からない。
  - 一つの package の中の全 component が同じ ABI revision を共有する。
  https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0188_component_abi_compatibility
- 費用（作り手自身の記述）：
  - RFC-0239：「古い API level との後方互換の維持には性能の費用がある」。binary が大きくなる。platform の component は、支えている全 level で同時に正しく動かなければならない。method は、それを含む全 level が retire されるまで実装し続ける。
  - 開発中の変更にも互換を保つ案は、API level ごとに約 168 個、release 全体で 800 個超の ABI 刻印が要るので退けた。
  - RFC-0002：運用の複雑さ、試験の行列の拡大、悪意のある app が古い ABI revision を名乗って古い経路に入る危険。
  - API level を HEAD や NEXT に向ける利用者は、互換の保証を失う。
- 古い level を支えきれなくなった重要な component は、RFC-0002 の「quirk」（互換の shim の表）で動かし続ける。RFC は、Windows が対象の ABI を推測する方式を「開発者の大きな苦痛」と評し、それを避けるために早い段階で明示の刻印を埋めると述べる。

### 1.4 FIDL の版と進化の規則
- `@available(added, deprecated, removed, replaced, renamed)` で要素ごとの版を注釈する。値は整数、`NEXT`、`HEAD`。文書は「これは API の版付けで、ABI の版付けではない」と明言する。実行時に版を問う手段は無い。
  https://fuchsia.dev/fuchsia-src/reference/fidl/language/versioning
- fidlc は ABI 互換を保証しない。互換は別に守る規則による。
- 非推奨から削除までに、少なくとも一つの API level を挟む（FIDL API evolution 文書、検索結果の要約）。
- RFC-0083 は、新しい変更を `HEAD` に注釈し、履歴を書き換えず、非推奨を経てから削除し、strict と flexible の変更は API Council が個別に見ると定める。
  - 欠点は、FIDL 言語と fidlc の複雑さ。安全な小変更が面倒になる。
  - 別の platform の版が混ざる場合の分解が、未解決の問題。
  https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0083_fidl_versioning
- 未知の欄の扱いは RFC-0033。`flexible`（既定）は未知の tag を持つ message を受け入れ、未知の handle は閉じる。`strict` は未知の欄を持つ message を無効とする。費用として、復号側が未知の欄を処理する分の小さな性能の損と、binding の追加の間接やメモリー。
  https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0033_handling_unknown_fields_strictness

### 1.5 components、package、capability routing
- package は content addressed。各 BLOB は Merkle root を名前に持ち、同じ内容は一度だけ保存される。複数 package に共通の共有ライブラリも一つで済む。base package は実行中に変えられず、OTA でだけ変わる。
  https://fuchsia.dev/fuchsia-src/concepts/packages/package
- component は、namespace の中の capability と数個の番号つき handle を通してだけ、外とやり取りする。manifest の `offer`、`expose`、`use`、`capabilities` で route を宣言する。route が成り立つ時だけ、実行時に capability が与えられる。
  https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities
- 費用として文書が述べるのは、循環の検出と、その解き方三つ（weak な依存、component の分割、依存の反転）。それぞれに manifest の増加や欠けた時の対処が要る。
- handle は 32 bit の整数で、syscall のたびにカーネルが型と権利を検査する。channel は datagram 型で、message の大きさと handle の数に上限がある。
  https://fuchsia.dev/fuchsia-src/concepts/kernel/concepts
- Starnix は、Linux の ABI をユーザー空間のプログラムで実装し、無改造の Linux プログラムを動かす runner。仕組みの詳細（syscall の捕まえ方）は取得した頁に無かった。
  https://fuchsia.dev/fuchsia-src/concepts/starnix

## 2. OpenBSD

- `pinsyscalls(2)` は、各 syscall の命令がある位置の表（pintable。syscall 番号で引く、`start` からの offset）をカーネルに渡す。別の位置の syscall 命令で呼ぶと、process は `SIGABRT` で終わる。呼ぶのは `ld.so` だけ。複数スレッドになる前でなければならない。static binary からの呼出しは `EPERM`。OpenBSD 7.5 で初出。
  https://man.openbsd.org/pinsyscalls.2
- LWN の記事（公式でない）：
  - 2019 年に syscall を四つの領域（static binary の text、signal trampoline、ld.so、libc.so）に限った。そのための `msyscall()` は libc.so の位置をカーネルに知らせる。
  - 2023 年に `execve()` の呼び元を限る `pinsyscall()` が加わった。
  - 新しい `pinsyscalls()` では、`ld.so`、`libc.so`、`libc.a` が `openbsd.syscalls` という ELF section を持つ。dynamic linker がそれから pintable を作る。利用者の build は変わらない。
  - 旧 `msyscall()` と `pinsyscall()` は、一、二 release の後に何もしない形にし、その後消す予定。
  - 記事は、これができるのはカーネルと libc を一緒に開発しているからだと述べる。
  - 批判は二つ。ROP なら libc の包みを呼べるので効果が限られること。`pledge()` がすでに似た制限を持つこと。
  https://lwn.net/Articles/959562/
- static の扱い：カーネルが、`ld.so` と static executable の分は自動で登録する。LWN は、保護が最も効くのは static binary で、動的 link のプログラムでは libc の syscall 位置が全部 address 空間にあると述べる。
- libc.so の版を上げる方針：OpenBSD 公式の文書は取得できなかった。公式でない資料は、非互換の変更ごとに major 番号を上げると示す。
  - Debian の mailing list（2014）：libc の major は当時 50 前後。
  - 古い commit の履歴：flag day で全ライブラリの major を一斉に上げた例。
  - 公式 FAQ は、約六か月ごとの release（目標は 5 月と 11 月）と「kernel と userland が同期した完全な system」とだけ述べる。
  https://www.openbsd.org/faq/faq1.html
- 古いプログラムをどう動かすかは、OpenBSD では「release ごとに全体を組み直す前提」。互換の仕組みは取得資料に見つからなかった。推測：古い static Go binary は、新しい kernel で動かなくなりうる（下の 3 を参照）。

## 3. Go が macOS と OpenBSD で syscall の直接呼出しをやめた経緯

- macOS：
  - 2016 年 10 月に issue #17490 が開かれた。理由は、Apple が syscall ABI を約束しておらず、libSystem の利用を求めていること。libSystem は全ての macOS にあること。
  - issue の議論に、時刻の取得が壊れた件（#16570）への言及がある。時刻取得は性能が重要なので、独自のままにする案も出た。
  - 古い golang-nuts の投稿は、カーネルの更新で binary がときどき壊れたと述べる。
  - Go 1.11 から、runtime が `libSystem.dylib` を使う。この時点で `syscall` package はまだ直接呼出し。
  - Go 1.12 で `syscall` も libSystem 経由。その結果、iOS では `syscall.Getdirentries` が常に `ENOSYS`、`Setrlimit` が従来成功した所で invalid argument。リリースノートは、これは Go 固有でなく libSystem の挙動との一致だと述べる。App Store の private API 検査が増えたとも書く。
  https://github.com/golang/go/issues/17490
  https://go.dev/doc/go1.11
  https://go.dev/doc/go1.12
- OpenBSD：
  - Go 1.11 のノート：OpenBSD 6.4 のカーネルの変更のため、古い Go は 6.4 で動かない。変更の中身はそのノートには無い。
  - 2020 年 1 月に issue #36435 が、「libc.so からでなければ syscall できなくなる」カーネルの変更に備える提案として開かれた。
  - Go 1.16 のノート：openbsd/amd64 と arm64 で、syscall を `SYSCALL`/`SVC` 命令でなく libc 経由にする。OpenBSD 6.9 以降は非 static の Go binary に libc 経由を要求する。
  - 議論では、static binary（`CGO_ENABLED=0`）は例外の範囲に入るとされた。Go は OpenBSD で、cgo を使えば動的 link になる。反対意見は、全 syscall に費用が付き、カーネルが安定した API を持つべきだというもの。
  https://go.dev/doc/go1.16
  https://github.com/golang/go/issues/36435
- LWN（公式でない）は、Go が最大の障害だったと述べ、Joel Sing が対応したとする。
- 費用の整理：確認できたのは iOS の `ENOSYS` などの挙動の差と、cgo を伴う動的 link 化。性能の数字は取得資料に無い。

## 4. Linux

- 方針：「退行を起こさない」がカーネル開発の第一の規則。ユーザー空間への interface が対象で、カーネル内部（module API）は対象外。例外は、とても稀な安全上の修正、影響が小さく回避策がある場合、とても古い装置の対応の削除、staging、5% 程度の微小な性能低下。
  https://www.kernel.org/doc/html/latest/admin-guide/reporting-regressions.html
- vDSO：カーネルが全 process に map する小さな共有ライブラリ。`gettimeofday` のような頻繁な呼出しで、カーネルに入る費用を避けるため。
  - 主な呼び手は C ライブラリー。直接使うのは「たぶん間違い」と man page が書く。
  - symbol に版が付き、カーネルが関数の型を変えても古い ABI を保てる。
  - `strace` にも `seccomp` の filter にも見えない。
  - Linux では、syscall の命令自体も ABI として保たれる。だから Go のような libc 無しの実行が成り立つ。これは Fuchsia や OpenBSD と対照的。
  https://man7.org/linux/man-pages/man7/vdso.7.html
- glibc の symbol versioning：検索結果は公式の glibc 文書ではなく mailing list と解説記事。
  - 古い binary は新しい glibc で動く。逆は動かない。
  - glibc 2.34 では、`__libc_start_main@GLIBC_2.34` が常に link されるため、古い header で組んでも古い glibc では確実に動かない。理由は Bugzilla 23323 の硬化（新しい binary が古い libc で ELF constructor を実行しない問題）。
  - 実際の対処は「対応したい最古の glibc で組む」（古い Docker image など）。
  https://patchwork.sourceware.org/comment/116352/
  https://inbox.sourceware.org/libc-help/4224ac85-83b7-6fe8-6781-b8e8f514924a@linaro.org/
- 静的リンクの実際：glibc の static binary は、NSS（`getaddrinfo`、`getpwnam` など）で実行時に共有ライブラリを `dlopen` する。そのため警告が出る。bug 17250 では、static binary が既定の library path を探さなくなり NSS が壊れ、これは意図した挙動とされた。CVE-2025-4802 は、static な setuid binary の `dlopen` が `LD_LIBRARY_PATH` を探す件。対処は動的 link、musl、`--enable-static-nss`。
  https://inbox.sourceware.org/glibc-bugs/bug-17250-131-QHqVv6bG8w@http.sourceware.org%2Fbugzilla%2F/t
  https://seclists.org/oss-sec/2025/q2/161
- Flatpak（公式）：
  - app は一つの runtime に対して組まれ、その版が入っていなければならない。複数の runtime と版が共存する。
  - runtime は OS の版に依らない安定した土台になる。
  - OSTree に保存し、同じ file は一度だけ。hard link で写す。
  - sandbox、portal で host とやり取りする。
  - 費用の数字は公式文書に無かった。
  https://docs.flatpak.org/en/latest/basic-concepts.html
  https://docs.flatpak.org/en/latest/under-the-hood.html
- runtime の寿命（公式でない、KDE の方針と forum）：KDE は Qt の最新二つの minor を支え、新しい minor が出て一か月後に二つ前を EOL にする。EOL の runtime を使い続ける app が出て、更新の時に警告が出る。保守されない app は古い runtime に残り、安全の修正を受けない。
  https://community.kde.org/Policies/Flatpak_Runtime_Update_Policy

## 5. WebAssembly（WASI と Component Model）

- 目的：core module は `i32`、`i64`、`f32`、`f64` しか使えず、文字列などは offset と長さで表す。型の安全が無く、言語ごとのデータ配置が違う。WIT と Canonical ABI が、言語に依らない契約を与える。Go の component が C や Rust の component を、互いの言語を知らず呼べる。
  https://component-model.bytecodealliance.org/design/why-component-model.html
- 制約：component は memory を export できない。関数の import と export だけで通じる。sandbox は強くなるが、設計の自由は減る。
- WIT：`record`、`variant`、`enum`、`flags`、`list`、`option`、`result`、`stream`、`future`。`resource` は外の実体への handle で、`own` と `borrow` がある。world が import と export の集合。package の ID は semver つき。package の解決の仕方は仕様が定めない。
  https://component-model.bytecodealliance.org/design/wit.html
- 能力の渡し方：Wasm に syscall 命令も組み込みの I/O も無い。能力は二種。
  - link 時の能力は、import された関数や instance で、host や中間の component が別物を供給して仮想化や弱めができる。
  - 実行時の能力は handle で、偽造できない。handle を持てない言語では、instance ごとの table の `i32` の添字になる。
  - 文書は、二つの方式の切替が難しいことを述べる。
  https://github.com/WebAssembly/WASI/blob/main/docs/Capabilities.md
- 成熟度（公式の roadmap）：WASI 0.2.0 は 2024 年 1 月。0.3.0 は 2026 年 6 月 11 日で、async（`async func`、`stream<T>`、`future<T>`）を Component Model に入れた。0.3.1 が 2026 年 8 月、以後 2 か月ごとの release を予定。WASI 1.0 の日付は無く、roadmap の目標は「暫定で改訂されうる」。
  https://wasi.dev/roadmap
- 性能の費用（Bytecode Alliance の記事）：
  - 現在の実装は、同期だけの呼出し経路でも約 3.5 倍の overhead がある。Nick Fitzgerald の Plumbers Summit の測定による。async の task の管理を host call で行うため。
  - 目の前の eager な ABI は、heap の断片化、大きな確保の失敗の扱い、list の値ごとの host から guest への呼出し、独自の allocator の使いにくさが問題で、「lazy ABI」を 0.3.x で任意、1.0 で既定にする計画。
  - list を返すと、host が呼び元の `cabi_realloc` で要素ごとに確保して copy する。
  - Mozilla の Ryan Hunt の実験では、DOM を多く変える Wasm の VDOM 処理で、browser API の直接呼出しにすると「2 倍に近い」速さになりうる。workload による。
  - Component Model 1.0 は未リリース。browser の native 実装は未コミット（二つ以上の engine が必要）。jco による変換は動く。P1 の module と P2 の component は引き続き動く。
  https://bytecodealliance.org/articles/the-road-to-component-model-1-0
- P1 から P2 へは、adapter（`wasi_snapshot_preview1`）が P1 の ABI を P2 に写す。toolchain が P2 を直接出せない間の橋（公式でない crate の説明）。
  https://docs.rs/wasi-preview1-component-adapter-provider
- 広くは行われていない形、研究段階：Component Model の async、lazy ABI、1.0 は途中。実際の本番の広がりを示す数字は取得資料に無い。

## 6. その他

### Plan 9
- 公式の論文の頁（compiler）は、loader が object と library をまとめて実行 binary を作ると述べる。`#pragma lib` で header が必要な library を名指す。library は「たまに compile し、頻繁に load される」。
  https://plan9.io/sys/doc/compiler.html
- 「全て static」「機能は file server として提供し、修正は server 側で全 client に効く」という説明は、公式でない投稿（FreeBSD の mailing list、OSDev forum、Hacker News）による。作り手の発言は取得できなかった。
  https://lists.freebsd.org/pipermail/freebsd-current/2003-November/014531.html

### Haiku
- 公式でない集約（検索の要約）：Haiku の公式の案内は、gcc2 の ABI を BeOS との binary 互換のため、R1 までは「安定で将来にわたる」唯一の ABI として扱う。
  - 「hybrid」image は、二つの ABI の system library を重複して持つ。primary と secondary の architecture（x86_gcc2）。
  - 2026 年の RFC は、LLVM を修正して gcc2 の ABI を出す案を示す。これは提案で、決定ではない。
  - beta 同士の互換は forum 発言で、正式な方針ではない。
  https://www.haiku-os.org/guides/building/gcc-hybrid
  https://discuss.haiku-os.org/t/rfc-revitalization-of-the-x86-gcc2-ecosystem/19355

### Redox の relibc
- Redox Book：当面、安定を約束するのは libredox の ABI だけ。relibc は開発中で不安定。`redox_syscall` は 1.0 の後も不安定。`redox_syscall` を直接呼ばず libredox を使えと書く。libredox 自体はまだ SemVer に従わない。
  https://doc.redox-os.org/book/libraries-apis.html
- FOSDEM 2025 のスライド：syscall の ABI は意図して不安定にし、安定の層を redox-rt に移す。
- 2023 の優先事項の記事：relibc を動的 library にして安定の境界にする計画。移行の完了は確認していない。
  https://redox-os.org/news/development-priorities-2023-09/
- 注意：Redox は POSIX 風の OS で、relibc は POSIX の包み。Nocturne の目的とは違う。

### Genode の ld.lib.so の ABI ダミー
- 出どころは公式でない（個人の Gitea の mirror の commit 説明）。
  - アプリは、dynamic linker の symbol だけを持つ薄い stub `ld.lib.so`（ABI）に link する。
  - 本物は kernel ごとの実装（`ld-linux.lib.so`、`ld-nova.lib.so` など）で、integration の時に `ld.lib.so` の名で boot image に入る。
  - ABI の symbol file は `base/lib/symbols/ld`。`<lib>.abi.so` は code も data も持たず、symbol だけ。
  - 後の commit で、この仕組みを共有 library 一般に広げ、libc のような動きの遅い ABI の中間の ABI にできるようにした。
  https://gitea.c3d2.de/ehmry/genode/commit/3d7b92ea5051c65db14ba25c44715675e1bf7db8
- Genode の公式 manual：RPC は同期で、capability の委譲を運ぶ。message は最大 1 KiB の生データと 4 個の capability。RPC interface ごとに capability の型が暗に決まる。server は client の身元を受け取らず、capability を持つこと自体が権限。
  https://genode.org/documentation/genode-foundations/25.05/architecture/Inter-component_communication.html

### seL4
- seL4 の discourse の Pre-RFC（提案で、決定ではない）：libseL4 だけを安定の入口と想定し、raw の syscall ABI は安定にしない。version を確かめる syscall だけ例外。破る時は major 版（約十年に一度と見積もる）か experimental の機能。
  https://sel4.discourse.group/t/pre-rfc-specify-a-fixed-kernel-abi-for-each-architecture/145/19
- 2026 年 7 月の release：seL4 16.0.0 は seL4 API を直接使う者に破壊的変更を含む。
  https://sel4.discourse.group/t/july-2026-releases-of-sel4-microkit-camkes-capdl-and-rust-support/1069
- 公式 docs：libsel4 系の user-level C library は試作向けで、本番は Microkit を勧める。
  https://docs.sel4.systems/projects/user_libs/

### Nix の内容で引く保存
- 各 package は、build の依存の graph の hash を前置した store path に入る。宣言した依存だけで組めるので、成功は宣言の完全さを示す。版が違えば path が違い、並んで共存できる。古い版は rollback のため garbage collection まで残る。
  https://nixos.org/guides/how-nix-works/
- 費用の例（公式でない）：glibc の更新は普通は全体の再 build を招く。2016 年の CVE-2015-7547 の時は `system.replaceRuntimeDependencies` で代用し、投稿者の体感で約 25 分。2026 年の issue は、nixpkgs の pin が安全の修正の無い glibc 2.40 を含み、閉包の 54 個の store path が影響を受けた例を報告する。
  https://releases.nixos.org/nix-dev/2016-February/019571.html

### Android の Binder の stable AIDL（公式）
- 変更の許可は限る。末尾への method の追加、parcelable の末尾への要素（既定値つき）、定数、列挙子（Android 11 以降）、union の末尾への欄（Android 12 以降）だけ。他は禁止。
- version を凍結し（`aidl_api/<name>/<V>` と `.hash`）、build が凍結版同士と先端との互換を検査する。
- client は `getInterfaceVersion()` と `getInterfaceHash()` で、相手の版を実行時に問える。古い相手には新しい method の既定実装を持てる。
- 限界：新しい method を古い server に呼ぶと `UNKNOWN_TRANSACTION`（C++/NDK）か `RemoteException`（Java）。union の新しい欄は古い受け手で失敗する。一つの process に一つの版だけ。
- vintf の stability を付けた interface は、使われている間、安定の約束になる。
  https://source.android.com/docs/core/architecture/aidl/stable-aidl
- Android NDK の文書：libc や libm は端末のもの。zlib は `libz.so` が端末の版で、header は NDK の最新と一致しないことがあり、static の `libz.a` にすれば避けられるが APK が大きくなる。
  https://developer.android.com/ndk/guides/stable_apis

## 7. 能力と型のある channel の上で OS のサービスを呼ぶ形は、API の版の問題をどう変えるか
これは資料の要約ではなく、読んだ資料からの整理で、推測を含む。

- 資料が示す事実：
  - Fuchsia は、handle（権利つき）と channel と FIDL を境界にし、syscall は vDSO に閉じる。版は API level と ABI revision で扱い、FIDL の版は API の版付けで、wire の互換は別の規則で守る（1.1、1.4）。
  - Android の stable AIDL は、型のある呼出しの interface を凍結し、実行時に相手の版を問える（6、Binder の項）。
  - WASM の component は、能力の import と export だけで通じ、memory を共有しない（5）。
  - Genode は、capability の型が RPC interface ごとに決まる。
- 整理（推測）：
  1. 境界が「syscall の番号の表」から「サービスごとの型つき protocol」に変わる。互換の単位が、一つの巨大な表から、サービスごとの protocol に分かれる。
  2. 能力は、与えるかどうかを決める。与えた能力が何を許すかは protocol の型が決める。能力が無ければ呼べないので、古いプログラムに新しい能力を暗黙に与えずに済む。Fuchsia の「古い ABI revision を名乗る攻撃」の危険（RFC-0002）は、能力の与え方の側で扱える。
  3. 版の問題が無くなるわけではない。protocol の進化の規則（追加だけ、未知の欄を許す、凍結）が要る。Android の「古い server への新 method は `UNKNOWN_TRANSACTION`」のように、失敗の形が変わる。
  4. 呼出しの先を OS が差し替えられる。能力の先が別の実装（仮想化、弱めた版、古い版の変換）でも、呼ぶ側は気づかない。WASI の link 時の能力の仮想化がこの形。Starnix や Fuchsia の quirk も、古い interface を新しい実装の上に置く試み。
  5. 費用が移る。呼出しごとの検査（Fuchsia は handle の型と権利を syscall ごとに検査）、message の copy（WASM は 3.5 倍の overhead を自ら述べる）、サービスの数だけ protocol の版を保つ手間。

## 8. 自分の評価（決める必要はない。参考として）
- 境界の置き場所は三通りに分かれる。
  - 命令を ABI とする（Linux、約束するのはカーネル）。
  - 命令を ABI にせず、小さな library に閉じる（Fuchsia の vDSO、OpenBSD の libc、macOS の libSystem、seL4 の libseL4、Redox の libredox）。
  - 言語に依らない型つきの契約に閉じる（WASM の Component Model、Android の stable AIDL、FIDL）。
- 境界が一番下の命令で守られない系（macOS、OpenBSD、Fuchsia）では、他の言語の runtime（Go）が真っ先に痛む。この三つで、Go が直接 syscall をやめたことが共通の結果。
- Fuchsia が一番、窓の仕組みを明文化している（API level、ABI revision、phase、quirk）。それでも窓の長さは「partner 次第」で、数としては固定されていない。費用（性能、binary の大きさ、試験の行列）を作り手が書いている点が、他より具体的。
- 静的リンクは、境界を「プログラムの中」に持ち込むので、OS が変わっても動く。ただし NSS のような実行時の読み込み（glibc）や、syscall の位置の検査（OpenBSD）では例外や制約が出る。
- 古いプログラムを保つ道は、(a) 境界を不変にする（Linux）、(b) 版を刻印して支える窓を決める（Fuchsia）、(c) 古い runtime を同梱して共存させる（Flatpak、Nix）、(d) 古い ABI を新しい実装の上に写す（Starnix、WASI の P1 adapter）、(e) 組み直しを前提にする（OpenBSD、Plan 9）。(c) は安全の修正が届かない費用（Flatpak の EOL runtime、Nix の古い glibc）、(b) は窓の管理と試験の行列の費用がある。
- 資料の弱い所：Fuchsia の libc の現行の配置、OpenBSD の libc の版の方針の公式文書、Plan 9 の作り手の言葉、Haiku の現行の約束、Genode の公式文書での ABI の記述は、確認できていない。必要なら一次資料を探し直す。
