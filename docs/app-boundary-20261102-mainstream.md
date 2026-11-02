# プログラムと OS の境界の調べ：Windows・Apple・Android の境界（サブエージェントの報告の全文）

モデルは Sonnet 5.5。判定は [app-boundary-20261102.md](app-boundary-20261102.md) にある。

## 依頼文

読むだけの調べものです。ファイルを変えません。Web の検索と取得を使ってください。

問い：次の OS で、第三者のプログラムが OS の更新の後も動くために、プログラムと OS の境界はどこに、どんな形で置かれているか。その形の費用と失敗は何か。

調べる OS と項目：
- Windows：syscall の番号の扱い（公式の立場）、ntdll と Win32 の DLL、KnownDLLs、DLL の探す順と DLL hijacking、API sets（api-ms-win-*）、Side-by-side と manifest、DLL hell の歴史、UCRT、アプリの互換のシム（Application Compatibility）、MSIX、COM と WinRT の版の持ち方、WoW64。
- macOS と iOS：syscall の扱い（公式の立場）、libSystem、dyld と dyld shared cache、two-level namespace、weak linking と availability、Swift の library evolution と ABI の安定、Mach-O の minos と SDK の版で動きを変える仕組み（linked-on-or-after）。
- Android：Bionic、NDK の API level、targetSdkVersion による動きの変更、linker namespaces、APEX（更新できるシステムの部品）、syscall のフィルター。

各項目について：
- 何をしているか。
- 何が得られ、何を払っているか（読込みの時間、メモリー、API の表面の大きさ、永久に保つ費用、安全の問題）。作り手自身か一次の資料が述べる費用と失敗を優先する。数字があれば数字を出す。

書き方：
- 日本語で書く。識別子と固有名詞は元の表記。一文に一つのことを書く。
- 主張ごとに出どころの URL を付ける。作った者の公式の文書を優先する。公式でない資料は「公式でない」と書く。
- 読んで確かめたことと推測を分ける。取得できなかったページは、取得できなかったと書く。
- 自分の評価は最後に分けて書く。どれが良いかを決める必要はない。
- 最後の返事が報告の全文です。ファイルには書きません。

## 報告

# 調査報告：プログラムと OS の境界（Windows、macOS と iOS、Android）

## 範囲と限界

- 読んだのは Web の公式ページと、一部の公式でない資料。確かめた内容と推測は分けて書く。
- 次は取得できなかった、または公式の文書が見つからなかった。
  - Windows の syscall の番号について Microsoft が述べる公式の立場の文書。検索でも出なかった。
  - Windows の KnownDLLs の専用ページ。DLL の探す順のページにある説明だけを使った。
  - Apple の dyld shared cache の公式の数字。
  - Apple の「linked-on-or-after」を説明した公式の文書。公開された `dyld_priv.h` の写しだけ。
  - Microsoft の `application-compatibility-database` ページ。404 だった。代わりに古い TechNet の記事を読んだ。
  - Android の linker namespace を app 向けに述べた記述。取れたページは HAL と VNDK が中心だった。
- WebFetch は小さなモデルで要約された。引用符で囲んだ文は原文の引用ではなく、要約中の語句の可能性がある。数字は出どころのページ上の表記のまま書いた。

---

## 1. Windows

### 1.1 syscall の番号

- 何をしているか
  - Microsoft の文書は Win32 と ntdll の関数だけを公開している。syscall の番号は公開していない。
  - 番号が公式に約束されていない証拠は、公式でない資料からしか取れなかった。
- 公式でない資料が述べること
  - DEF CON の講習の資料は「Microsoft は番号が変わらないと保証しない」と書く。 https://media.defcon.org/DEF%20CON%2034/DEF%20CON%2034%20workshops/DEF%20CON%2034%20-%20Workshops%20-%20Bramwell%20-%20Bw3ll%20-%20Brizendine%20-%20Creating%20Shellcode%20for%20Hackers/ShellWasp/Additional%20Notes%20-%20Windows%20Syscalls%20and%20WoW64.md （公式でない）
  - 研究者の記事は、番号が版ごと、service pack ごとに違うと書く。 https://www.evilsocket.net/2014/02/11/On-Windows-syscall-mechanism-and-syscall-numbers-extraction-methods/ （公式でない）
  - Wine の開発者の投稿は、`NtGdiAlphaBlend` が win32syscalls.h では 4 だが、Windows 11 の win32u.dll では 0x1075 だと書く。 https://list.winehq.org/archives/list/wine-devel@list.winehq.org/message/U7EKLAUAVR7ODG3AJN22AUBNH4KJTWEQ （公式でない）
- 推測（確かめていない）
  - 公式の立場は「番号の下の層は公開しない。公開するのは DLL の関数だけ」だと読める。

### 1.2 ntdll と Win32 の DLL、API sets

- 何をしているか
  - API set の名前は DLL のファイル名ではなく「契約」の名前。ローダーが実行時にスキーマで実体の DLL に引き直す。
  - 実装 DLL を分割、統合、改名しても、アプリは同じ契約名で届く。 https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets
  - 旧来の DLL 名で import したアプリには「reverse forwarder」が使われる。同じ名前の互換 DLL が、API set へ転送する。 https://learn.microsoft.com/en-us/windows/win32/apiindex/api-set-loader-operation
- 得るもの
  - 実装の置き場所を変える自由。
  - HoloLens や Xbox のように Win32 の一部だけを持つ機器で、「その契約があるか」を問える（`IsApiSetImplemented`）。
- 払うもの（Microsoft 自身の記述）
  - 契約が無いと、静的 import のプロセスは起動に失敗する。遅延 import なら起動し、呼んだ時に失敗が出る。
  - ロードに成功しても、その機能がある証拠にならない。契約の中の個別の機能は、ローダーが調べない。「静的 import では拘束が致命的になるので、ローダーは許す側に倒す」と書く。
  - reverse forwarder は、API set に対応がある export だけを持つ。持たない関数を import すると「入口が見つからない」で失敗する。
  - reverse forwarding は Win32 の一部にしか効かない。デスクトップ向けの binary をすべての機器で動かすものではない。
  - `api-` と `ext-` の接頭辞に意味は無い。ローダーは区別せず、後から役割が変わっても名前は変わらない。接頭辞から存在を推測してはいけない。
  - 存在する関数が、エラーを返すだけの stub のこともある（`GetProcAddress` の注意）。 https://learn.microsoft.com/en-us/windows/win32/sysinfo/operating-system-version

### 1.3 DLL の探す順、KnownDLLs、DLL hijacking

- 何をしているか
  - 順序は次のとおり。DLL redirection、API sets、SxS manifest、読込済みの一覧、Known DLLs、パッケージの依存、exe のフォルダー、system フォルダー、16 ビットの system フォルダー、Windows フォルダー、カレントフォルダー、PATH。
  - Known DLLs は、レジストリ `...\Session Manager\KnownDLLs` に載る DLL。OS が持つ写しを使う。
  - 出どころ： https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order
- 払うもの
  - 攻撃者が探索経路のフォルダーを握ると、偽の DLL を置ける。「DLL preloading attack」または「binary planting attack」と呼ぶ。
  - アプリが管理者権限なら、権限昇格に至り得る。
  - 緩和策は利用者側の責務として書かれている。完全パスの指定、`LOAD_LIBRARY_SEARCH` 系の旗、`SetDefaultDllDirectories`、`SetDllDirectory("")`。 https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security
  - `SetDllDirectory` はプロセス全体に効く。複数 thread で別の値を呼ぶと動作が未定義になる。サードパーティの DLL とは互換性を試す必要がある。
  - 依存 DLL は、最初の DLL を完全パスで読んでも、モジュール名だけで探される。
  - ロード結果で OS の版を推測するのも危ない。DLL が正当に無い環境で、偽物が読まれ得る。
  - `SafeDllSearchMode` は Windows XP SP2 から既定で有効。カレントフォルダーが後ろに下がる。

### 1.4 DLL hell、Side-by-side と manifest、DLL redirection

- 何をしているか
  - 共有 DLL が別のアプリの導入で入れ替わり、動いていたアプリが壊れる問題が DLL hell。
  - Side-by-side assembly は、DLL と manifest の組。同じ部品の複数の版を同時に置き、アプリごとに使う版を選ぶ。バインドと COM 起動の情報を、レジストリの代わりに manifest に持つ。 https://learn.microsoft.com/en-us/windows/win32/sbscs/about-isolated-applications-and-side-by-side-assemblies
  - Comctl32.dll 6.0 の見た目（テーマ）は、このしくみで選ばれる、と同ページにある。
  - 導入が古い歴史の根拠は公式でない。Windows 2000、ME、98 で side-by-side 共有と COM の redirection が入ったと、ServerWatch が書く。 https://www.serverwatch.com/guides/side-by-side-component-sharing-in-windows-2000/ （公式でない）
  - 古い MSDN 記事「The End of DLL Hell」の本文は取れなかった。Microsoft Learn の別ページが、その名前を挙げている。
  - DLL redirection（`.local` ファイル）は、アプリを変えずに、exe のフォルダーの DLL を優先させる。 https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-redirection
- 払うもの
  - COM は問題が残る。登録の場所が一つしかなく、互換でない二つの COM server を入れても、最後に登録したものだけが起動される（同ページ）。
  - redirection は、アクセスできないドライブがあると、そこで探索を止める（通常は飛ばして続ける）。
  - 公式でない評価として、「Side-by-side assemblies bring DLL hell 2.0」という議論がある。 https://developers.slashdot.org/story/09/10/04/1956225/side-by-side-assemblies-bring-dll-hell-20 （公式でない。本文は確認していない）

### 1.5 UCRT

- 何をしているか
  - VS 2015 で CRT を分割した。標準 C の部分を Universal CRT として Windows の部品にし、コンパイラー固有の部分を vcruntime に分けた。
  - UCRT は Windows 10 以降の OS の一部。安定した ABI を持ち、コンパイラーの版に縛られない。更新は Windows Update が行う。
  - 出どころ： https://learn.microsoft.com/en-us/cpp/porting/upgrade-your-code-to-the-universal-crt
- 得るもの
  - Visual Studio を上げるたびに、CRT の版を追いかける必要が無くなる。
- 払うもの
  - ヘッダーと lib と再配布ファイルの名前と場所が変わり、導入方法も変わった。既存のビルドの更新が要った。
  - 一部の関数や macro で、互換を壊す変更があった。
  - アプリ内同梱（app-local）も可能。ただし、性能と安全のために推奨されない。`ucrtbase.dll` と `api-ms-win-*.dll` の転送 DLL が要り、OS ごとに必要な組が違うので全部を入れる、と書く。

### 1.6 target 版の指定と版の確認

- 何をしているか
  - ヘッダーが `WINVER`、`_WIN32_WINNT`、`NTDDI_VERSION` で宣言を出し分ける。 https://learn.microsoft.com/en-us/windows/win32/winprog/using-the-windows-headers
  - 実行時の版は、manifest の宣言に依存する。Windows 8.1 と 10 向けに manifest していないアプリには、Windows 8 の版 6.2 が返る。 https://learn.microsoft.com/en-us/windows/win32/sysinfo/operating-system-version
- 払うもの
  - サービスパックで追加された関数があるので、target を一つ上げないと宣言が見えない場合がある（`GetDllDirectory` の例）。
  - Microsoft は、版ではなく機能の有無で確認するよう勧める。

### 1.7 アプリの互換のシム

- 何をしているか
  - 動作の変わった関数のため、Windows 本体に分岐を入れる代わりに、アプリ単位の修正を OS の外に持つ。
  - PE の import address table（IAT）の関数アドレスを、シムの関数のアドレスに差し替える。動的に読む DLL は `GetProcAddress` を hook して扱う。
  - 出どころ： https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-7/dd837644(v=ws.10) （古い、保管済みの記事）
- 払うもの（同記事の記述）
  - シムの中身は、OS から見るとアプリのコード。UAC などの安全機構は越えられない。
  - ユーザー空間で動くため、カーネル側のコード（ドライバー、一部のセキュリティー製品）の互換は直せない。
  - 同じ効果は、アプリを直しても得られる。
  - 本体に分岐を入れる案は、OS の保守性と信頼性の長期の課題になる、と書く。
- 公式でない補足：データベース（SDB）の解析と、悪用の研究がある。 https://www.blackhat.com/docs/eu-15/materials/eu-15-Pierce-Defending-Against-Malicious-Application-Compatibility-Shims-wp.pdf （公式でない。本文は未確認）

### 1.8 MSIX

- 何をしているか
  - package identity を OS が与える。導入と削除をきれいにし、ファイルとレジストリを仮想化する container で動かす。
  - 差分更新は 64 KB のブロック単位。
  - 既存のアプリ向けに、Package Support Framework で、コードを変えずに互換の修正を当てられる。
  - 出どころ： https://learn.microsoft.com/en-us/windows/msix/overview
- 得るもの（Microsoft の数字）
  - 導入の成功率 99.96%（数百万回の導入）。削除の後に、ファイルもレジストリも残らない。
  - この数字の測定条件は、取得したページには無かった。
- 払うもの
  - 全パッケージに署名が要る。
  - container のため、書込みが仮想化または転送される。既存アプリの互換には、PSF の修正が要ることがある、と読める（推測）。
- パッケージ化したアプリの探索順に、パッケージの依存グラフが入る。 https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-search-order

### 1.9 COM と WinRT の版の持ち方

- COM
  - COM は呼出しの binary 標準。インターフェイスは IID（GUID）で識別する。`QueryInterface` で、実行時に対応を問う。
  - 登録情報の台帳（レジストリ）が、CLSID と DLL または EXE の場所を結ぶ。呼び出す側は CLSID だけ知ればよい。
  - 出どころ： https://learn.microsoft.com/en-us/windows/win32/com/com-technical-overview
  - 「公開したインターフェイスは変えない」という明文の規則は、取得したページには無かった。IID と QueryInterface の仕組みからの推測。
- WinRT
  - システムの struct、delegate、interface は、一度定義すると不変。以後の Windows で変更してはならない。
  - enum と runtime class は「加える方向」に版を上げる。enum は値を足せる。class は実装する interface を足せる。
  - すべての型に Version 属性が要る。サードパーティの WinRT 部品はアプリに同梱され、アプリと別に版が変わらない。
  - 出どころ： https://learn.microsoft.com/en-us/uwp/winrt-cref/winrt-type-system
- 払うもの
  - 不変の interface は永久に残る。拡張は新しい interface の追加で行う（これは上の記述の帰結で、推測を含む）。

### 1.10 WoW64

- 何をしているか
  - 32 ビットのアプリを 64 ビット Windows で動かすエミュレーター。OS の部品として有効化は要らない。
  - 32 ビットと 64 ビットを隔離し、ファイルとレジストリの衝突を避ける。
  - 出どころ： https://learn.microsoft.com/en-us/windows/win32/winprog64/running-32-bit-applications
- 払うもの
  - 32 ビットのプロセスは 64 ビットの DLL を実行用に読めない。逆も同じ。
  - 64 ビット Windows は 16 ビットのアプリを動かせない。理由は、ハンドルが 32 ビット有効で、16 ビットのアプリへ切り詰めると失われるから。起動は `ERROR_BAD_EXE_FORMAT` で失敗する。

---

## 2. macOS と iOS

### 2.1 syscall の扱い（公式の立場）

- Apple の Technical Q&A QA1118（2011 年、更新されていない）が、静的リンクの binary をサポートしない理由を書く。
  - 静的リンクの binary は、カーネルの syscall を直接呼ぶ。
  - Apple は syscall の境界の binary 互換を保証しない。互換を保つのは、動的にリンクするシステムの library と framework。
  - 「互換の重大な危険がある。強く勧めない」と書く。
  - 出どころ： https://developer.android.com/library/archive/qa/qa1118/_index.html は誤り。正しい URL は https://developer.apple.com/library/archive/qa/qa1118/_index.html
- 実例：Go 1.12 は Darwin で libSystem 経由に切り替えた。
  - 狙いは、将来の macOS と iOS との互換。
  - 副作用として、App Store の私的 API の検査が増えた。iOS で `syscall.Getdirentries` が常に `ENOSYS`。`syscall.Setrlimit` が時々 invalid argument。
  - 出どころ： https://go.dev/doc/go1.12
- QA1118 は libSystem の名前を挙げない。libSystem が境界という説明は、一般知識で、上の資料からの推測。

### 2.2 dyld と dyld shared cache

- 何をしているか
  - 共有キャッシュは、よく使うシステム library を一つのファイルに事前にリンクして作る。起動を速くする最適化に使う。iOS 3.1 と macOS Snow Leopard で導入され、prebinding の後継。
  - dyld 3 は、プロセスの外で解析して「closure」を作り、プロセス内の小さな部品がそれを使う。システムアプリの closure は共有キャッシュに入り、サードパーティのものは導入時に作る。
  - 出どころ（公式の WWDC の発表資料を要約した二次の資料）： https://wwdcnotes.com/documentation/wwdc17-413-app-startup-time-past-present-and-future/ （公式でない）
  - Apple の DTS の技術者の回答： https://developer.apple.com/forums/thread/692383 （公式の forum）
- 払うもの
  - Big Sur 11.0.1 から、システムの dylib が個別のファイルとして disk に無い。キャッシュの中にだけある。`dlopen` でファイルの有無を調べる道具（Python の `find_library` など）が壊れた。出どころ： https://bugs.python.org/issue44689 、https://bugzilla.mozilla.org/show_bug.cgi?id=1683758 （公式でない。Apple の release note の引用は Python 側のスレッドにある）
  - 起動時間の数字：Allegro の測定で 31 個の dylib のアプリが約 7.3 秒から 1.75 秒。これは一社の特定の構成の結果で、Apple の数字ではない。 https://blog.allegro.tech/2018/05/Static-linking-vs-dyld3.html （公式でない）
  - Apple 自身の「キャッシュ有りと無し」の比較数字は見つからなかった。

### 2.3 two-level namespace

- 何をしているか
  - 記号の名前を「library の名前」と「記号の名前」の二段で持つ。OS X 10.1 で導入され、標準の linker で作る Mach-O の既定。
  - 利点は、記号の引きが速くなることと、library 同士の名前の衝突が起きないので前方の互換が良くなること。
  - 条件：クライアントは、記号を持つ library（または umbrella framework）に直接リンクする必要がある。
  - 出どころ： https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/MachOTopics/1-Articles/executing_files.html
- 払うものを述べた記述は、取れたページに無かった。

### 2.4 weak linking と availability

- 何をしているか
  - base SDK は、コンパイルで使える API を決める。deployment target は、動かしたい最古の OS。
  - 弱くリンクした class、関数、framework は、無い時に nil か NULL になり、起動失敗にならない。
  - 出どころ： https://developer.apple.com/library/archive/documentation/DeveloperTools/Conceptual/cross_development/Using/using.html （2010 年の記述で、Swift の `@available` 以前）
- 払うもの（同ページの「危険」）
  - 弱い記号を確認せず使うと、動的リンカーのエラーで process が終わり得る。
  - 新しい SDK でビルドして古い OS で動かすと、実行時の確認が無い限り失敗する。
  - deprecated の API は将来の OS で消え得る。
  - 版の確認は最後の手段、と書く。

### 2.5 Swift の library evolution と ABI の安定

- 何をしているか
  - Swift 5 で、Apple のプラットフォームの標準ライブラリーの ABI が安定した。OS が Swift の runtime と標準ライブラリーを持つので、アプリは同梱せずに済み、サイズが縮む。 https://www.swift.org/blog/abi-stability-and-more/
  - library evolution は選択制。既定は切。切の library は、変更のたびにクライアントを再コンパイルする必要がある。 https://www.swift.org/blog/library-evolution/
- 払うもの（Swift の公式 blog が述べる）
  - public と `@usableFromInline` の宣言は、消せず、署名も変えられない。
  - `@frozen` の付け外しは、binary 互換を壊す。早く決める必要がある。
  - 型の大きさと配置を、境界の外から仮定できない。型情報を使う間接の扱いになり、性能の費用が出る。property の access は accessor 経由、protocol と class の呼出しは thunk 経由。code size も増え得る。
  - `@inlinable` は、関数の本体を永久の約束にする。
  - frozen でない enum の `switch` には `default` か `@unknown` が要る。
  - Apple 以外では、全 binary を同じ compiler の版でビルドする必要が残る。
- 性能の定量的な数字は、取得したページに無かった。

### 2.6 linked-on-or-after（SDK の版で動作を変える）

- 何をしているか
  - `dyld_program_sdk_at_least()` など。プログラムがどの SDK でビルドされたかで、新しい動作か古い動作かを出し分ける。`dyld_program_minos_at_least()` は最低 OS の版を見る。
  - 出どころ： https://opensource.apple.com/source/dyld/dyld-852/include/mach-o/dyld_priv.h.auto.html （公開された private ヘッダー。公式の利用者向け文書ではない）
  - WebKit は `linkedOnOrAfterSDKWithBehavior` を持ち、OS の年ごとの `linkedBefore` の連鎖で、動作を個別に切る。 https://webkit.googlesource.com/WebKit/+/e7faecc4fcb24350646e1305ac734c97b2abfc44/Source/WTF/wtf/cocoa/RuntimeApplicationChecksCocoa.cpp
- 払うもの
  - 古い動作を残す分岐が、OS 側に年ごとに積み重なる（WebKit のコードから読み取れる。推測を含む）。
  - これは private ヘッダーなので、公式の約束の範囲かは不明。Apple の公式文書は見つからなかった。

---

## 3. Android

### 3.1 Bionic と syscall のフィルター

- Bionic の方針
  - Bionic は、カーネルが提供する Linux の syscall の wrapper を追加していく（`statx`、`renameat2` が R、`pidfd_*` が S）。
  - 動作の変更は、多くが targetSdkVersion と無関係に全アプリへ適用される。target で切り分けた例は少数。API 24 の `sem_wait` の EINTR、API 26 の不正な `pthread_t` で abort、API 28 の破棄済み mutex の使用が FORTIFY 失敗。
  - 出どころ： https://android.googlesource.com/platform/bionic/+/master/docs/status.md
- seccomp フィルター
  - Android O で zygote に入れ、全アプリが継承する。既存のアプリを壊さない方針。許可の基準は、Bionic の `SYSCALLS.TXT`、起動に要るもの、人気アプリが使うもの。
  - arm64 で 271 のうち 17、arm で 364 のうち 70 を禁止する。禁止の syscall を呼ぶアプリは SIGSYS で殺される。
  - 出どころ： https://android-developers.googleblog.com/2017/07/seccomp-filter-in-android-o.html
  - 許可と禁止の一覧の最新の中身は読んでいない。

### 3.2 NDK の API level と minSdkVersion

- 何をしているか
  - minSdkVersion より新しい NDK の API は、既定では呼べない。`dlopen` と `dlsym` で使う必要がある。 https://developer.android.com/ndk/guides/stable_apis
- 払うもの（NDK の公式ページが述べる）
  - `libz.so` は端末のものが使われ、ヘッダーの版と一致しない。struct の配置が時代で変わっており、今後も変わる。静的な `libz.a` を使えば回避できるが、APK が大きくなる。
  - NDK にあるのは OpenGL ES などの library で、「ある」ことと「その機能に対応している」ことが別。実行時に問い合わせる必要がある。

### 3.3 targetSdkVersion による動作変更

- 何をしているか
  - 一部の変更は、その API level を target にしたアプリにだけ適用される。例：Android 14 の foreground service の型の必須、JobScheduler の callback の時間超過で ANR、暗黙の intent の制限、動的に読むコードの読取り専用の義務。 https://developer.android.com/about/versions/14/behavior-changes-14
  - 全アプリに適用される変更の一覧は別にある（同ページが示す）。
- Google Play の規則
  - 2026 年 8 月 31 日から、新規と更新は API 36 以上を target にする。既存のアプリは API 35 以上でないと、新しい OS の新規の利用者に配られない。延長は 2026 年 11 月 1 日まで。 https://developer.android.com/google/play/requirements/target-sdk
- 払うもの
  - 古い target のアプリは、新しい OS の新規の利用者に配られなくなる。
  - 古い動作は OS 側に残り続ける。しかし Play の規則で、新しい target への更新が強制される。
  - 古い target（API 22 以下）のアプリを初めて起動すると、警告が出る（Android 10 から）。

### 3.4 非 SDK インターフェイスの制限

- Android 9 から、SDK に無い interface の利用を制限する。一覧は、blocked、max-target-x（その API level 以下を target にする時だけ許可）、unsupported（使えるが予告なく変わる）に分かれる。
- ブロックされると、`NoSuchMethodError` などで失敗する。JNI は NULL を返す。
- OEM は一覧に足せるが、AOSP の一覧から外せない。CDD と CTS が強制する。platform 署名のアプリと一部のシステムアプリは免除。
- ネイティブコード向けには Android 7（API 24、ページ内で「26」と書かれた箇所と食い違う。後述）から、NDK 以外の library への link を禁止した。
- DEX の形式や dex2oat に、安定の約束は無い。
- 出どころ： https://developer.android.com/guide/app-compatibility/restrictions-non-sdk-interfaces
- 食い違い：同ページの Q&A は「Android 7（API level 26）」と書くが、Android 7.0 は API 24。別のページ（target-sdk のページ）は「Android 7 (API level 24)」で「非 NDK の library へ link を禁じる」と書く。後者が正しいと見る。

### 3.5 linker namespaces

- 何をしているか
  - 動的リンカーで library を分離し、同名で記号が違うものを共存させる。`ld.config.txt`（INI 形式）で、namespace、探す道、link を定める。Android 11 から `/linkerconfig` に boot 時に生成される。
  - fallback の link は推移しない。A→B と B→C があっても、A は C を探さない。
  - 出どころ： https://source.android.com/docs/core/architecture/vndk/linker-namespace
- 払うもの
  - ページに費用の節は無い。次は取得ページからの推測。設定が複雑になること。`libcutils.so` のように、namespace ごとの複製が起き得ること。
  - アプリ向けには `libnativeloader` が `public.libraries.txt` で同様の絞りをする、と書く。

### 3.6 APEX

- 何をしているか
  - Android 10 で入った、低位のシステム部品（native のサービスと library、HAL、ART、class library）を、OS 全体の更新なしに更新する容器。署名つきの zip で、dm-verity 付きの ext4 の中身を持つ。`apexd` が boot の早くに検証して mount する。
  - 出どころ： https://source.android.com/docs/core/ota/apex
- 払うもの（同ページ）
  - 更新の反映に再起動が要る。
  - 部品の置き場所が `/system/lib/` から `/apex/<名前>/` に動く。呼び出す側が新しい道を使う必要がある。道の転送は、Bionic を静的にリンクするアプリがあるため実現できないとして却下された。
  - 組込み版が disk に残り、容量を使う。圧縮は Android 12 から。ただし bootstrap、更新不可、動的 library の APEX には効かない。OTA の展開には空きの確保が要る。
  - カーネルの対応（loop driver と dm-verity など）、署名の鍵の運用がある。
  - 部品間の安定した interface をどう保つかは、取得したページに書かれていない。

---

## 4. 評価（私の見方。調べた資料の外の判断を含む）

どれが良いかを決める必要は無いので、観察だけを書く。

- 境界の位置は、三つの OS で共通している。カーネルの syscall より上の「システムの library」に置く。Windows は ntdll と Win32 の DLL、Apple は libSystem、Android は Bionic と framework。syscall の番号を公式の境界にしている OS は、ここでは見つからなかった。ただし、Windows の公式文書で番号を明示的に否定したものは見つかっていない。
- 一度公開した型は変えない、という規則が、各所に現れる。WinRT の struct と interface、Swift の public な宣言、COM の IID、API set の versioned な名前。公式の文書で明言されているのは WinRT、Swift、API set。COM は推測。
- 版による動作の切分けが、三者とも別の形で現れる。Windows は manifest の宣言と版の偽装（6.2 を返す）、Apple は linked-on-or-after、Android は targetSdkVersion。古い動作を OS が残すので、積み重なる。Android は Play の規則で、更新を強制する。
- 失敗の形は共通して「存在の確認が無いまま呼ぶ」。静的 import で起動失敗（API set）、弱くない記号で process 終了（Apple）、NDK の新しい API が古い端末に無い（Android）、export が無い forwarder（Windows）。
- 探索の自由度が、そのまま安全の穴になる。Windows の DLL の探す順の問題が典型。Android の linker namespace と非 SDK の制限、Apple の共有キャッシュは、探索の幅を狭くする方向。
- 更新の単位を小さくする動きが、三者にある。UCRT（Windows）、APEX（Android）、共有キャッシュ（Apple）。どれも、境界の安定と引換えに、配置の変更（道の変更、ファイルの消失）を払っている。
- 数字は少ない。公式の数字は MSIX の 99.96% と、Android の seccomp の禁止数だけ。起動時間などの数字は、公式でない一社の測定のみ。

## 5. 未確認

- KnownDLLs の実際の一覧と、セキュリティー上の効き方。
- Windows の WinRT と COM の版管理の細部（class の版管理の節を読んでいない）。
- Apple の `minos` と SDK の版が Mach-O のどのコマンドに入るか。
- Android の `ld.config.txt` を app がどう受けるか。
- 各 OS の「永久に保つ費用」の定量的な値。
