# プログラムと OS の境界の設計

対象は、OS と別に配られるプログラムと OS の間の境界です。[設計の読み直し](app-boundary-20261102-review.md)に、元の案から直した点と理由があります。

## 結論

- **Windows の DLL の形は最良ではありません。** 長く動いている確かな形ですが、更地で選ぶ理由は薄いです。
- **DLL は五つの仕事を一つの仕組みで行っています。** 五つを分ければ、DLL の欠点の多くは起きなくなります。
- **推奨は四つの道具です。**
  1. **入口：** カーネルが全プロセスに写す小さな像です。syscall 命令と時計の形はこの中に閉じます。
  2. **プログラムの中：** libc、C++、UI の部品は、今と同じく静的にリンクします。
  3. **システムのサービスとプログラム：** 普通の OS がサービスにする機能は、channel の文で呼びます。
  4. **GPU のドライバーの像：** 利用者側のドライバーだけが、プロセスの中に入る別の像です。探す道も共有の名前空間もありません。公開の口は Vulkan です。
- **古い形は残しません。** 互換の約束はまだありません。形を変えたら世代を上げ、プログラムを組み直します。
- **開発を遅くする物は作りません。** 凍結の検査、版の交渉、古い世代の訳は作りません。
- **一番大きな費用は、プログラムの中の libc の欠陥が OS の更新では直らないことです。** 下の「費用と弱い所」に書きます。
- **現行の OS がやらない良い形はあるか：** 新しい発明はありません。部品の多くは Fuchsia、Android、OpenHarmony、OpenBSD にあります。Windows・macOS・Linux はこの組み合わせを採っていません。
- **一番良い設計：** 境界の形では Fuchsia です。Nocturne は、その版の仕組みを今は採りません。

## DLL がしている五つの仕事

| 仕事 | DLL での形 | 要るか | 推奨の置き場 |
|---|---|---|---|
| カーネルを隠し、番号を変えられるようにする | `ntdll.dll`、`kernel32.dll` | 要る | 入口 |
| OS の更新で修正を届ける | `ucrtbase.dll`、`schannel.dll` など | 要る。ただし届けたいのは全部ではない | サービス |
| メモリーを共有する | 同じファイルの頁を共有 | 小さい。同じファイルなら今も共有している | 今の `mm/program.c` |
| 機械ごとのコードをプロセスに入れる | GPU のドライバーの DLL | 要る | GPU のドライバーの像 |
| 後から機能を足す | `LoadLibrary` と差し込み | 第三者の側で要る | 今は別プロセスと channel |

三つ目だけは新しい物が要りません。`kernel/src/mm/program.c` は読取り専用の segment を全プロセスで共有し、頁は最初の触りで読みます。

## DLL の欠点

| 欠点 | 中身 | 出どころ |
|---|---|---|
| 探す道が穴になる | 名前で探すので、探す先を取られると別の DLL が入ります。管理者で動くアプリなら権限が上がります。緩和は開発者の手順です。 | [Dynamic-Link Library Security](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-security) |
| 版が強制されない | Microsoft 自身が「DLL にはシステムが強制する版が無い」と書いています。読み込みの順とアプリの動きが脆くなるとも書いています。 | [Best Practices](https://learn.microsoft.com/en-us/windows/win32/dlls/dynamic-link-library-best-practices) |
| 読み込みの錠 | `DllMain` は loader lock の中で走ります。`LoadLibrary`、`CreateThread`、`User32` の関数などを呼ぶと、固まるか落ちることがあります。 | 同上 |
| C の物が境界を越えられない | CRT の写しごとに状態と heap が別です。`FILE*`、環境変数、locale、`malloc` の番地を渡すと壊れます。 | [Potential Errors Passing CRT Objects](https://learn.microsoft.com/en-us/cpp/c-runtime-library/potential-errors-passing-crt-objects-across-dll-boundaries) |
| 公開の面が巨大で凍る | `kernel32.dll` は約 2,000 の export を持ちます（公式でない数）。公開した物は消せません。 | [costs の報告](app-boundary-20261102-costs.md) |
| 互換の仕掛けが積もる | API sets、SxS、manifest、互換のシム、manifest が無いアプリへの偽の版番号です。読み込めても機能があるとは限りません。 | [Windows API sets](https://learn.microsoft.com/en-us/windows/win32/apiindex/windows-apisets)、[mainstream の報告](app-boundary-20261102-mainstream.md) |
| 版を推測する | Windows は対象の ABI を推測しています。Fuchsia の RFC はこれを「開発者の大きな苦痛」と評しています。 | [RFC-0002](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0002_platform_versioning) |
| 起動が遅くなる | 小さな C で動的 137 µs、静的 64 µs です（Arch 2020、公式でない）。iOS で dylib を 57 から 31 に減らすと起動が 12〜26% 速くなりました（公式でない）。 | [costs の報告](app-boundary-20261102-costs.md) |

## DLL の利点

利点も本物です。推奨の形は、これを別の道で受けなければなりません。

- OS の更新で修正が全アプリへ届きます。Heartbleed では、共有の OpenSSL はライブラリーの入替えと再起動で済みました。静的に持つ製品は作り直しが要りました。
- 長く保たれた実績があります。推奨の形に同じ長さの実績はありません。
- プロセスの中なので、呼出しが速いです。

## 比べた形

| 形 | 代表 | 凍る面 | 探す穴 | 修正の届き方 | 判定 |
|---|---|---|---|---|---|
| 多くの DLL | Windows | とても大きい | ある | OS の更新 | 動く。欠点は上のとおり |
| 一つの大きなシステムライブラリー | Apple の `libSystem` と shared cache | 大きい | 小さい | OS の更新 | **次点。** 確かな形。libc の面が凍る |
| 版と機能を宣言するシステムライブラリー | OpenHarmony | 大きい | 名前空間で狭める | OS の更新 | 形は採らない。機能の宣言を借りる |
| syscall を保つ | Linux | 番号と動きの全部 | 動的なら glibc 側にある | 配布ごとに違う | ユーザーの判断で採らない |
| 静的とサービスだけ | Plan 9 | サービスの文 | 無い | サービス側 | GPU で成り立たない |
| 入口とサービス | Fuchsia | 入口と protocol | 無い | サービスと package | **一番近い** |
| 中間形式と型のある部品 | WASM の Component Model | WIT | 無い | 実行環境 | 重い。1.0 が未達 |

WASM の呼出しは、同期の道でも約 3.5 倍の overhead があると作り手が書いています（[Bytecode Alliance](https://bytecodealliance.org/articles/the-road-to-component-model-1-0)）。

**Apple の形を次点にした理由は一つです。** libc を OS の中の像に入れると、面が大きくなり、約束を始めた後の改善を遅くします。小さな入口なら、後から関数を足して Apple の形へ寄せることもできます。

## 推奨の形

### 入口

入口を置くと、カーネルの番号と実体を、プログラムを変えずに動かせます。互換を持たない今の効き目は小さく、効くのは第三者のプログラムが増えてからです。

- カーネルは読取り専用の小さな像を全プロセスに写します（`kernel/src/sys/entry.c`）。frame は全プロセスで共有します。
- **像は溝の並びです。** 溝は一つ 16 バイトで、番号の順に並びます。関数の番地の表は再配置が要るので持ちません。
- `struct n_start` の `entry` が像の番地です。呼ぶ番地は `entry + 番号 × N_ENTRY_SLOT_BYTES` で、C の呼び方で呼びます。動的リンカーと名前の解決は要りません。起動の費用は写す一回だけです。
- **公開の約束は溝の番号です。** 番号はプログラムに残ります。カーネルが受ける番号と実体は、溝の中身で動かせます。今は溝の番号とカーネルの番号が同じです。
- 番地はプロセスごとに変えます。範囲は主の stack の下の 1 TiB です。
- 溝の中身は三種類です。
  - カーネルへ入る溝：`mov r10, rcx; mov eax, 番号; syscall; ret`。
  - 拒む溝：`-N_ERR_DENIED` を返します。落ちません。
  - 像の中のコードへ跳ぶ溝：時計です（`kernel/entry/clock.c`）。時計のデータは像の直後に写します。`struct n_clock` の形も番地もプログラムからは見えません。
- **カーネルは番号と溝を結びます。** `syscall_dispatch` の頭で `rip == entry + 番号 × 16 + 10` を比べ、違えばプロセスを止めます。範囲の比較だけなら、別の溝の `syscall` 命令へ別の番号で跳べるからです。Fuchsia の文書と OpenBSD の `pinsyscalls` も位置を強制します（[vDSO](https://fuchsia.dev/fuchsia-src/concepts/kernel/vdso)、[pinsyscalls](https://man.openbsd.org/pinsyscalls.2)）。
- **`%fs:0` は `struct n_thread_block` です。** 頭の語は自分の番地で、次の語が像の番地です。プロセスの中の libc の写し（`tcc -run`、ドライバーの像）は、ここから像の番地を知ります。
- Go と Rust は C を介さず、溝を直接呼べます。Go が macOS と OpenBSD で苦労した点は、libc を読み込まないと呼べないことでした（[Go 1.12](https://go.dev/doc/go1.12)、[Go 1.16](https://go.dev/doc/go1.16)）。この形では libc が要りません。

**システムと第三者を分けます。**

- **決めるのは置き場所です。** `C:\Nocturne` の中のプログラムの像は全部の溝がカーネルへ入ります。そこはシステムしか書けません（`kernel/src/fs/store.c`）。
- それ以外のプログラムの像は、公開の溝だけがカーネルへ入ります。`C:\Programs` の付属のアプリも同じです。
- 身元の級は見ません。`ls` は USER の級で動くシステムのプログラムで、第三者のプログラムは admin の身元で動けます。刻印も見ません。自己申告だからです。
- 公開の一覧は `common/abi.h` の `N_PUBLIC_CALLS` です。一覧に無い呼出しは内部です。
- `abi.h` の「古い形を保たない」はそのまま続きます。
- 呼べるかどうかの本当の検査は、これからも能力と身元で行います。表を分けるのは、面を小さくするためです。

**開発を遅くしません。**

- 溝はカーネルが起動のときに `SYS_*` の列挙から作ります。生成の道具はありません。呼出しを足す手順は、公開なら `N_PUBLIC_CALLS` に一語足すだけです。
- 形を変えたら、今と同じく組み直します。審査、凍結の検査、適合の試験は足しません。

約束を始めるかどうかは、後で決めます。OpenHarmony の公開の口の約束は 5 版で、適合の試験が守ります（[API Governance](https://github.com/openharmony/docs/blob/master/en/design/OpenHarmony-API-governance.md)）。始めるときの参考です。

**入口は実装を変えられます。** 今カーネルにある窓や TCP/IP をサービスへ出しても、入口の中身を channel の呼出しに変えるだけで、プログラムは変わりません。

### プログラムの中

ISO C、C++、文字列、描画の部品、字体、UI の部品は、今と同じく静的にリンクします。

- 使う member だけが入ります。手元の build で `echo` は 0.04 MiB、`notepad` は 0.78 MiB です。
- OS との間で `malloc` の番地や `FILE*` を渡しません。CRT の問題は起きません。
- 名前の衝突も DLL hell もありません。
- OS を更新しても、プログラムの中身は変わりません。世代が同じ間は、組まれたときのまま動きます。

OpenHarmony の文書が、この分け方の裏付けです（[C++ Standard Library](https://github.com/openharmony/docs/blob/master/en/application-dev/reference/native-lib/cpp.md)）。

- システムの `libc++.so` は OS と一緒に、アプリの `libc++_shared.so` は SDK と一緒に更新されます。両者は major の版が離れることがあり、ABI の問題になります。
- そこで二つの C++ の名前空間を分け、互いに使えなくしています。OS の公開の native の口は C だけです。
- 共有ライブラリーが一つのアプリには `c++_static` を勧めています。

### システムのサービス

置き場の線は普通の OS と同じです。普通の OS がサービスにする物はサービスに、ライブラリーにする物はプログラムの中に、カーネルに置く物はカーネルに置きます。`uihost` と `webfetch --service` は既に channel の形です。

| 機能 | 置き場 | 理由 |
|---|---|---|
| メモリー、thread、待ち、handle、channel、文書、時計 | 入口 | カーネルの口 |
| 窓、TCP/IP | 入口（実体はカーネルの `gui/wm.c`、`net/`） | 今のまま。カーネルから出す仕事はこの設計の外 |
| ISO C、C++、文字列、UI の部品、字体、画像、TLS | プログラムの中 | 普通はライブラリー。呼出しが頻繁で、往復に向かない |
| 音、網の設定、名前引き、時刻、画面の構成、設定、アカウント、セッション、プログラムの台帳、記録 | サービス（今ある） | 持ち主がシステム |
| 開く窓と保存の窓 | システムのプログラム。呼ぶ者の身元で動く | どのプログラムも同じ窓を出す。結果は完全なパス一通。宣言のサービスは利用者 0 の身元で動くので、他の利用者のファイルを見せてしまう |

サービスの文に版は持ちません。文の形も世代に入り、プログラムとサービスは同じ世代で動きます。文の宣言のファイルも今は作りません。Go の手写しが開発を遅くすると分かったら作ります。

頻繁な呼出しは往復させず、共有メモリーの輪で運びます。Binder の往復は約 70 µs です（8 byte、[Android の文書](https://source.android.com/docs/core/tests/vts/performance)。機種は書かれていません）。

### GPU

Nocturne は GUI が先の OS です。GPU は後から足す機能ではなく、入口と同じ順位の公開の口にします。

**ドライバーには二つの半分があります。**

| 半分 | 置き場所 | 中身 |
|---|---|---|
| 特権の半分 | 別プロセス `nvrm`（今のまま） | RM、GSP、画面、PCI の claim |
| 利用者側の半分 | プロセスの中の像 | NVK と NAK。コマンドを組みます |

利用者側をプロセスの中に置く理由は速さです。全部を別プロセスへ出す案（Mesa の Venus）と比べました。[読み直し](app-boundary-20261102-review.md)に理由があります。

**道具は四つです。**

- **公開の口は Vulkan です。** プログラムは `vkGetInstanceProcAddr` から入ります。SDK に Khronos の見出しが入ります。
- **ローダーはプログラムの中です**（`user/libc/vulkan.c`）。小さく、静的にリンクします。今は入る像を一つ選びます。一台に作り手の違う二つの GPU があるときは、最初に装置を出した像を使います。呼ぶ側は `vkEnumeratePhysicalDevices` を呼ぶだけなので、全部を並べる形へ変えてもプログラムは変わりません。
- **どの像かは宣言の表が決めます。** `user/apps/services.c` の `gpu` の項目が、装置ごとに `nvrm` を選んでいます。同じ項目が像の名前を持ちます。新しい作り手の GPU は表の一行です。ローダーは管理者の一覧を読むだけで、名前を探しません。Fuchsia でも、どの ICD かは GPU の装置が答えます（[RFC-0205](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0205_vulkan_loader)）。
- **カーネルは写すだけです。** `SYS_IMAGE_MAP` は文書の handle から像を写します。GPU の銘柄を知りません。書込みのない segment はプログラムと同じ道で共有します。

**像の規則は次のとおりです。**

- **依存しません。** 像は `-shared` の ELF で、自分の libc を中に持ちます。他の像に依存しません。
- **出す口は一つです。** ELF の入口の関数が `struct n_gpu_driver`（`common/gpu_abi.h`）を返します。中身は Vulkan の driver の口（[Loader Driver Interface](https://vulkan.lunarg.com/doc/view/latest/windows/LoaderDriverInterface.html)）の三つと、GL が要る準備の確かめ、記録の預かり、画面へ直接出す口です。版の交渉は Vulkan の口の手順です。
- **読み込みのときにコードを走らせません。** 写すだけです。再配置と初期化は、初めて入口の関数を呼んだときに像が自分で行います。loader lock は要りません。
- **状態を境界に渡しません。** `malloc` の番地や `FILE*` を渡しません。確保した側が解放します。Vulkan の C の口と共有メモリーだけが境界を越えます。
- **`%fs` は共有します。** スレッドの区画に libc の写しごとの記録を繋ぎます。像の TLS は global-dynamic で、像の中の `__tls_get_addr` が記録から引きます。
- **権限は呼ぶプログラムと同じです。** 像の libc も、そのプロセスの溝を呼びます。公開の溝で足りないことは `nvrm` に頼みます。像を入れて権限が増えることはありません。

**プログラムの中に残るものがあります。**

- OpenGL ES（Mesa の state tracker と Zink）は Vulkan の上のライブラリーで、静的です。
- CPU の描画（llvmpipe）も静的です。機械ごとのコードではないからです。今の直結の速さを保ちます。
- GPU が無い機械では、Vulkan は装置なしと答えます。CPU の Vulkan（lavapipe）は後の項目です。

**画面へ出す口も公開に入れます。** 窓の面、フリップ、可変のリフレッシュレートです。今の `ngl_scanout_*` が種で、`nvrm` への直結をやめ、像の表を通します。

**効き目は次のとおりです。**

- ポータブルも同じ道です。一つのファイルの中にドライバーは入りません。世代が同じなら、新しい GPU でもその時の OS の像で動きます。
- OS の更新でドライバーの修正が届きます。
- 像の text は全プロセスで共有します。
- ブラウザーは NVK、NAK、`libnvrm` を持たなくなります。

Go のプログラムは FS の基底を自分で持つので、今は像を呼べません。

### 世代の刻印

- SDK が組むときに、世代の番号を ELF に自動で刻みます。作る人の手間はありません。ポータブルのプログラムも同じです。
- 足すだけの変更は、同じ世代の表の末尾に足します。プログラムは要る表の長さも刻みます。
- 形を変えるときは世代を上げます。古い世代は残しません。
- `proc_launch` が読み、合わないプログラムは `N_ERR_WRONG_GENERATION` で起こしません。起こした側が理由を言います。シェルとファイルの窓は通知で出します。
- 刻印は `.note.nocturne` の PT_NOTE で、libc が全プログラムに入れます。Go はリンカーが入れます。

この形は `AGENTS.md` の shim の禁止と「互換は持たない」に合います。入口は他の部品になりすましません。

### 古いプログラムを保つと決めたとき

今は作りません。古い形を保つことが正しいと決めた口があれば、そのときの候補は世代の鎖です。

- カーネルとサービスは最新の世代だけを実装します。
- 古い世代の像は、一つ次の世代の表だけを呼び、古いプログラムのプロセスにだけ写します。
- 前例は Stripe の Web API の鎖（[Stripe](https://stripe.com/blog/api-versioning)）と、Windows の WOW64 の一段の訳（[WOW64](https://learn.microsoft.com/en-us/windows/win32/winprog64/wow64-implementation-details)）です。OS の互換の仕組みとしての鎖は見つかりませんでした。
- 副作用のある変更は訳しにくいと、Stripe も書いています。

### 機能の宣言

インストール形式を作るときに入れます。世代だけでは、機能の有無が分かりません。OpenHarmony の SysCap がこの問題を解いています（[SystemCapability](https://github.com/openharmony/docs/blob/master/en/application-dev/reference/syscap.md)）。

- 機能には名前があります。
- インストール形式は、**要る機能**の集合を持ちます。インストールのときに OS の持つ集合と比べ、足りなければ入れません。
- プログラムは動いている間に、**あれば使う機能**を問えます。
- Nocturne では、宣言の対象を OS の機能にします。USB の装置のように後から繋がる物は、起動のときの有無で拒みません。これは OpenHarmony の文書に無い、私の判断です。
- ポータブルは宣言を持ちません。

### 三種類のプログラムでの見え方

| 種類 | 入口 | 刻印 | サービスを呼ぶ | サービスを登録する |
|---|---|---|---|---|
| システムのプログラム（`C:\Nocturne`） | 全部の溝 | OS と同じ | できる | `declared[]` |
| インストールされたアプリ | 公開の溝 | ある | できる | インストールのときに宣言する |
| ポータブル | 公開の溝 | ある | できる | しない |

ポータブルは一つのファイルで動きます。登録も宣言も要りません。Windows の単体の `.exe` が `kernel32.dll` を呼ぶのと同じで、公開の溝を呼ぶだけです。

## 費用と弱い所

| 費用 | 大きさ | 受け方 |
|---|---|---|
| プログラムの中の libc の欠陥が OS の更新で直らない | **一番大きい。** | 受け入れます。TLS と画像の解読もプログラムの中なので、修正は組み直しで届きます。第三者のプログラムには、作った者の更新で届きます。 |
| 異なるプログラムの間で libc の頁を共有しない | 小さい見込み。`notepad` 級 100 個を全部触っても 78 MiB です。**実測はしていません。** | 大きな部品を各アプリが持つ形（アプリごとの Web エンジンなど）では効きます。Web の表示は別プロセスのサービスにできます。 |
| 溝の呼出しの費用 | syscall ごとに間接の呼出し一回と比較一回です。syscall は約 235 ns です（公式でない）。 | 無視できる見込みです。 |
| 世代を上げると、古いプログラムが動かなくなる | 組み直しが要ります。 | 受け入れます。今は配られた第三者のプログラムがありません。 |
| ドライバーの像の欠陥がプログラムを巻き込む | 同じプロセスの中だからです。 | 受け入れます。Windows、macOS、Linux、Fuchsia も同じです。 |
| 像の中の libc の写し | Mesa と Rust の分だけメモリーが増えます。 | 受け入れます。頁はプロセスの間で共有します。 |
| 実績 | デスクトップの一般の OS で、この組み合わせの長い実績は見つかりませんでした。 | 入口は Fuchsia の実績があります。 |

## 現行の OS がやらない形か

**既にあるもの：**

| 部品 | ある所 | Windows・macOS・Linux |
|---|---|---|
| syscall を小さな入口に閉じて強制する | Fuchsia、OpenBSD（位置の強制） | 無い。Linux の vDSO は速さのためで、命令も ABI |
| 組んだ版の刻印と支える窓 | Fuchsia、Android、OpenHarmony（公開の口は 5 版） | Windows は manifest が任意で、無ければ推測する |
| 要る機能をインストールのときに照らす | OpenHarmony の SysCap | 系全体の形ではない |
| アプリとシステムの C++ を分け、OS の口を C だけにする | OpenHarmony | 系全体の形ではない |
| 版の付いた型のある文 | Fuchsia の FIDL、Android の AIDL、OpenHarmony の HDI | 系全体の形ではない |
| 静的とサービス | Plan 9（公式でない資料）、Fuchsia は許す | 無い。macOS は静的を支えない（[QA1118](https://developer.apple.com/library/archive/qa/qa1118/_index.html)） |
| GPU の ICD を装置が答え、探さずに読む | Fuchsia（[RFC-0205](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0205_vulkan_loader)） | 無い。探す道か一覧のファイルで読む |

**調べた範囲で見つからなかったもの：**

0. **世代の鎖を OS の互換の仕組みにすること。** 一段の訳は WOW64 と WASI にあります。鎖は Stripe の Web API にあります。検索は一回の広い問いで、網羅はしていません。
1. **サービスの文の版をインストールのときに照らすこと。** Fuchsia の検査は package ごとに一つの ABI revision です（[RFC-0188](https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0188_component_abi_compatibility)）。OpenHarmony の SysCap は機能の名前を照らします。文書に、文の版を照らすとは書かれていません。Nocturne は文に版を持たないので、今は要りません。
見つからなかったことは、無いことの証明ではありません。Windows などが採らない理由は、古いプログラムを保つ義務だと推測します。これは確かめていません。

**調べて、採らないと決めたもの：**

- **WASM を必須にすること。** 費用が重く、ネイティブの移植（Mesa、LLVM、Go、Rust）の道を捨てます。
- **Flatpak や Nix の、古い runtime を並べる形。** 保守されないアプリに修正が届きません。複雑にもなります。
- **インストールのときの再リンク。** ポータブルに効かず、署名と合いません。
- **何でもサービスにすること。** GPU のドライバーを全部別プロセスにする案は、[読み直し](app-boundary-20261102-review.md)のとおり、速さを測れないので今は採りません。
- **一般の差し込みの仕組み。** GPU と必要が違います。今は別プロセスと channel で作れます。
- **OpenHarmony の全体の形。** カーネルは Linux で、libc は musl の共有ライブラリーです。Nocturne の「UNIX・POSIX・Linux を足さない」に合いません。syscall の制限は番号による seccomp で、番号そのものは隠れません。文書は C と C++ だけのアプリを勧めておらず、ArkTS が先です。Nocturne はネイティブのプログラムを第一級にします。

## プログラムが OS に頼る量と、権限と隔離

### 頼る量

手元の build の `build/symbols` を逆アセンブルし、プログラムの中にある syscall の語を数えました。全部で 182 語あります。

| プログラム | 語の数 |
|---|---|
| `true`、`echo` | 14 |
| `ls` | 18 |
| `cp` | 19 |
| `cube` | 31 |
| `notepad` | 32 |
| `term` | 33 |
| `files` | 36 |

これは静的な数で、走った回数ではありません。各プログラムに、番号を読めない呼出しが 5〜6 か所あります。`browser` は数えていません。`true` の 14 語の多くは、いつも入る libc の始末と文書の部品です。

OS でなければできない仕事は少ないです。

| 仕事 | OS が要る理由 | Nocturne の語の例 |
|---|---|---|
| メモリー | 頁表は特権です | `MEMORY_RESERVE`、`MEMORY_RELEASE`、`SHARED_MAP` |
| 時間 | CPU の割当ては特権です | `THREAD_CREATE`、`WAIT_ADDRESS`、`WAKE_ADDRESS` |
| 外との通信 | 隔離を越えられるのはカーネルだけです | `CHANNEL_SEND`、`CHANNEL_RECEIVE`、`EVENTS_WAIT`、`HANDLE_CLOSE` |
| 終わり | プロセスの片付けです | `EXIT` |

計算だけなら、走っている間は OS を呼びません。文書、窓、音、網は、原理上は通信の相手です。遅さの都合でカーネルの口にすることはあります。

### 権限と隔離は別のもの

| | 権限 | 隔離 |
|---|---|---|
| 問い | 誰が何をしてよいか | 一つのプログラムを、利用者の権限より狭く閉じ込めるか |
| 決める物 | 利用者の身元の級（user、admin、system）、道の権限、能力 | プログラムごとに渡す物 |
| 今の形 | ある（`kernel/src/fs/store.c:624`、`kernel/src/sys/authority.c`） | 無い |

プログラムは、起こした利用者の権限で動きます（`identity_for_child`）。普段の操作に確認は出ません。止まるのは、利用者自身に権利が無いときだけです。

### 全部のプログラムを隔離できるか

これは問いへの答えで、やる案ではありません。

**技術としてはできます。** プロセスを空で始め、起こす側が渡した handle だけを持たせます。Fuchsia の新しいプロセスはこの形です（[Sandboxing](https://fuchsia.dev/fuchsia-src/concepts/process/sandboxing)）。Nocturne の `proc_launch` は起動のときに handle を渡せます（`kernel/src/sys/proc.c:261`）。閉じるのは、道で開く `DOCUMENT_OPEN`、名前で繋ぐ `SERVICE_CONNECT`、`TCP_CONNECT` のような、何も持たずに世界を引く呼出しです。

**費用は二つです。**

- 実行の費用は小さいです。Capsicum の能力つきの呼出しは数 % 遅い程度で、`gzip` は一定の 2.4 ms 増えました（[Capsicum](https://www.usenix.org/legacy/event/sec10/tech/full_papers/Watson.pdf)）。FreeBSD の 2010 年の値で、Nocturne の値ではありません。
- 使う人の手間は大きくなりえます。素早く書いたプログラムが好きな道を開けなくなり、確認の窓が要ります。摩擦は小さくするので、これは既定に向きません。

**隔離で解けないものもあります。** カーネルの欠陥、Spectre のような CPU の漏れ、資源の使い尽くし、渡した文書の持出しです。

## Nocturne で変わる所

1. `SYS_*` を公開と内部に分けます（`N_PUBLIC_CALLS`）。
2. 入口の像を作り、`proc_launch` が写し、`struct n_start` で渡します。`user/libc/sys.c` の `__syscall` と時計を溝の呼出しにします。
3. `syscall_dispatch` に位置の検査を足します。
4. ELF の刻印と `proc_launch` の確かめと、起こした側の理由の文を足します。ファイルの窓はプログラムを中身で判定します。
5. `ports/go/nocturne.patch` を溝の呼出しに直し、リンカーが刻印を入れます。Go の時計も溝を呼びます。
6. 開く窓と保存の窓のプログラムを作ります。
7. `%fs` の区画に libc の写しの記録を繋ぎ、`SYS_IMAGE_MAP`、NVK の像、`user/libc/vulkan.c`、公開の見出しを作ります。ブラウザーをこの道へ移します。
8. インストール形式を作るときに、機能の名前と宣言を足します。

[インストールの監査](install-audit-20261102.md)との関係は次のとおりです。

- 欠陥 1（プログラムが番号を持つ）は 2、3、5 で解けます。
- 欠陥 2（システムのライブラリーと読む道が無い）は 2 と 7 で解けます。動的リンカーは第三者の配布の前提から外れます。
- 「決めてほしいこと」の 1 は、下の「決まったこと」のとおりです。

## 決まったこと

ユーザーの判断です。

1. **小さな入口から始めます。** 将来の改善と開発を遅くしない形で作ります。
2. **古い形は残しません。** 古い道は、それが正しいと決めたときだけ持ちます。
3. **サービスへ出す範囲は普通の OS と同じです。**
4. **GPU の公開の口の土台は Vulkan です。**
5. **GPU の案は私に任されました。** 一般の「封じた module」をやめ、上の形にしました。

## 確かめの範囲

- **読みました：** Nocturne の境界のコード（上に場所を書いた所）。
- **読んでいません：** 他の OS のソース。
- **測っていません：** Nocturne の channel の往復、起動の費用、静的な libc のメモリーの費用。
- **自分で原文を確かめました：** Microsoft の三つの頁の本文、OpenHarmony の文書の本文、Fuchsia の vDSO、RFC-0002、RFC-0239、QA1118、`pinsyscalls`、Go 1.12、Android の Binder の値、Bytecode Alliance の 3.5 倍、Vulkan の口。Microsoft と OpenHarmony 以外は、取得の道具の要約を通した引用です。
- **OpenHarmony で読んでいないもの：** ソース、`targetAPIVersion` で動きを切り替える範囲、GPU のドライバーの読み込みの道。読んだ頁は[調べの記録](app-boundary-20261102-openharmony.md)にあります。
- **サブエージェントの報告のままで、自分では確かめていません：** 表の中の公式でない数字、RFC-0188、Stable AIDL、Plan 9、Go 1.16。
- **報告の誤りを一つ直しました：** QA1118 は静的リンクを支えないと書いていますが、`libSystem` の名を出していません。
- **確かめられなかったこと：** Windows の番号が変わることの公式の文書、Fuchsia の libc の今の置き場所、Plan 9 の作り手の発言、Apple の shared cache の公式の数字、ALPC と XPC の往復。

## 調べの全文

- [OpenHarmony の境界](app-boundary-20261102-openharmony.md)（自分で読んだ記録）
- [Windows・Apple・Android の境界](app-boundary-20261102-mainstream.md)
- [Fuchsia ほかの境界の形](app-boundary-20261102-others.md)
- [動的リンクと静的リンクと IPC の費用](app-boundary-20261102-costs.md)
