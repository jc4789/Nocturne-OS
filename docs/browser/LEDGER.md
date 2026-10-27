# ブラウザーの性能と表示の作業台帳

目的：我々のブラウザーを、表示が正しく、実機で速いものにする。退行を入れない。
この台帳は作業の正本。作業を始める前と区切りごとに読み、状態・主張・決定を更新する。
過去の成功は、現在のソースとビルドで確かめるまで証拠にしない。確かめ方は `browser-hikaku` スキル。

## 0. 動かない前提（ユーザーの指示）

- 日本語で報告。UNIX／POSIX 互換を足さない。Linux や他 OS のソースを参照しない。GPLv2 専用のコードを入れない。
- 壊れているのは我々のブラウザー全体。サイト専用の処理、UA の偽装、注入した CSS・JS で「直った」画面を作らない。サイト名は再現の例。
- 確かめは複数の実サイト（参照フォルダーの `TEST SITES.txt` の全部＋変更が触れた層を通るサイト）を、動いているブラウザーの参照画像と部品ごとに照合する。
- WPT（`D:\Programes\cloned repos\wpt`）は正しい。落ちたら我々が間違っている。試験を変えて通さない。
- 対象は実機（7600X＋RTX 4070）。QEMU は一台だけ。Hyper-V と実機はユーザーの担当。

## 1. 基準

| 名 | 版 | 媒体 | 用途 |
|---|---|---|---|
| 基準 | b2f5b721（10/25 11:52） | `D:\Programes\nocturne-b2f5b721\build\nocturne.img`（別の作業木） | 16 時間の作業の前。退行かどうかの判定 |
| 現在 | main＋作業ツリー | `build\nocturne.img` | 修理の確かめ |

## 2. 16 時間の変更（10/25 12:06〜10/26 03:40）

退行の管理なしで入った。各行は、確かめるまで「未確認」。

| # | 版 | 内容 | 状態 |
|---|---|---|---|
| C1 | 47bb78fb | 待ちを AP で寝かせ、malloc をスレッドごとに、字の代替は cmap で先に、状態の変化は印で再計算 | 未確認 |
| C2 | 99e0686c | 規則の索引をシートの AST に、束縛と計算済みのスタイルを保つ | 未確認 |
| C3 | 6d193d58 | 動きと marquee の締切を絶対時刻に、部分カスケードも並列に、映像の拡大を帯で並列に | 未確認 |
| C4 | fc86bdb3 | AP で読み取り・初めての書き込み・時間付きの待ち・窓の状態 | 未確認 |
| C5 | 2bf13e1f | 角の型を形の表で共有 | 未確認 |
| C6 | ea214856 | 映像の一枚を帯ごとに表示面へ直接描く | 未確認 |
| C7 | e5969375 | 箱の木を変わった所だけ作り直し、配置のキャッシュを保つ | 未確認 |
| C8 | 305fc636 | :has() の無効化を絞り、継承の等しい子のスタイルを作り直さない | 未確認 |
| C9 | 1b0dc75e | キャッシュを使った箱に、最後の子と合流した下の余白を戻す | 未確認 |
| C10 | 3027093e | CSSOM で作り直した様式表の AST が古い領域を指す崩れを直す | 未確認 |
| C11 | 67f7164a | QuickJS の関数の元の文を共有 | 未確認 |
| C12 | 72731aec | 古典スクリプトを翻訳スレッドで bytecode に | 未確認 |
| C13 | c15925ef | 頁の描画を表示の拍に合わせ、課題ごとに描き直さない | 未確認 |
| C14 | 33e5203b | JavaScript の時間を関数ごとに数える計測 | 未確認 |
| C15 | 4ddd7914 | UTF-8 の符号化と Blob の組み立てを native に、DOM の読み取りの回り道を省く | 未確認 |
| C16 | 894423c6 | URL と URLSearchParams を Lexbor の native 解析器に | 未確認 |
| C17 | d0737a57 | btoa/atob を native に、bindings の翻訳を翻訳スレッドへ、Intl の遅延 | 未確認 |
| U1 | 6d503047 ほか（ユーザーの「slop」のコミット） | AP で YIELD・SLEEP・EVENTS_WAIT（世代の語で park）・APP_RESULT を済ませる | K1〜K4 の修理ごとコミット済み。QEMU 4 CPU で組は動く。Hyper-V は未確認 |
| U2 | 同上 | realloc の縮みで尾を返す、QuickJS の qrealloc を realloc に | コミット済み・未確認 |
| U3 | 同上 | http/http2/tls の錠を mtx_t に、HTTP/2 の待ちを条件変数に | コミット済み・未確認 |

## 3. 退行（ユーザーの報告、2026-10-26、Hyper-V、03:40 の媒体）

ユーザーが見た VM は、03:40:29 に `hyperv.ps1` の既定で作り直されたもので、**一 CPU・512 MiB 固定**だった（Get-VM を読むだけで確かめた。VMMS の記録で古い VM は 00:01 に削除、VmSwitch の記録で新しい VM は 03:40:31〜03:46:43 に動いた。既定は 10-05 の 47194194 から 512 MB・一 CPU）。古い VM の CPU と記憶の量は記録に無く、未確認（前の会話の写真では 4 vCPU）。
下の QEMU の比べ（WHPX、1920×1080、`snapshot=on`、同じ手順）は、どれも基準 b2f5b721 と HEAD（00954ae2）の今日のビルドの媒体。

| # | 欠陥（ブラウザーの層） | 再現 | 原因 | 状態 |
|---|---|---|---|---|
| R1 | 見出しの字体と大きさが変わり、場所を取る | YouTube の動画の題名 | 4 CPU・4 GiB では基準も HEAD も太字の Roboto 一行（HEAD は窓を最大化しない回も同じ）。記憶の不足で Web フォントの取得が落ちたときの代替の字体と見るのが一番（推測。512 MiB では頁が出る前に落ちて確かめられない） | 版の差は QEMU で見えない |
| R2 | 頁の部品の一部が組み上がらず、画像の取得が「Response exceeds available receive memory」「Worker response ended early」で失敗 | Reddit の左の案内欄 | 文言は空きの記憶が受け取りの倍に足りないときの守り（`user/libc/webnet.c`）。一 CPU・512 MiB では基準も HEAD も YouTube の動画の頁でブラウザーが約 340 MB を取り、頁が尽きて segfault で落ちる。4 GiB では両方とも左の案内欄が出る | 版の差ではなく VM の記憶の量。記憶が尽きたときに落ちる欠陥は残る（K5） |
| R3 | 毎コマ描く WebGL が 7 fps から 2 fps | webglsamples.org の city | 一 CPU・4 GiB で基準も HEAD も 2 fps、4 CPU・4 GiB で基準 6〜7・HEAD 7 fps | 版の差ではなく VM の CPU の数 |
| R4 | 全体的に遅い | — | 一 CPU の VM（R3 と同じ） | 版の差は未測定。性能の改善は続ける |

## 4. 既知の欠陥

| # | 欠陥 | 根拠 | 状態 |
|---|---|---|---|
| K1 | task_wake_address が address_lock を持ったまま object_notify を呼び、同じ錠を取りに行く（BSP が固まる） | 監査（2026-10-26） | 修理：通知を錠の外へ（U1 と一緒にコミット済み。QEMU 4 CPU で組は動く。Hyper-V・実機は未確認） |
| K2 | 閉じられた待ち行列の語で AP の待ちが park したまま起きない | 監査 | 修理：解放のときに語を起こす（同上） |
| K3 | BSP の経路が取っ手で失敗すると wait_until が残り、次の待ちの期限を狂わせる | 監査 | 修理：BSP の入口で先に消費（同上） |
| K4 | 空間の停止で BSP へ戻った眠りが、はじめから寝直す | 監査 | 修理：期限を wait_until に持つ（同上） |
| K5 | 記憶が尽きると、予約済みの頁の後払いに枠が無く、ブラウザーと映像の Worker が segfault で落ちる | QEMU 一 CPU・512 MiB、基準も HEAD も（serial.log の `[memory] … 0 pages free`） | 未修理 |
| K6 | 頁を移っても記憶が戻らない（7 サイトの後、ブラウザー 125→480〜580 MB、カーネルのヒープ 36→200 MB） | QEMU 4 CPU・4 GiB、基準も HEAD も | 未修理 |
| K7 | 動画が 40〜80 秒で「問題が発生しました」になる | QEMU 4 CPU、基準も HEAD も（再現は YouTube の vuYLCMyzFD4。clh5LrNDj-o は 60 秒進んだ） | 層は媒体の再生ではなく、頁の証明（attestation）の JS。2026-10-26 に F12 で読むだけの fetch の記録で見た：SABR の応答の UMP の STREAM_PROTECTION_STATUS（部の番号 58）が本編では始めから 2（保留）、約 40 秒で 3（要求）になり、以後は媒体の無い 107〜160 バイトの応答だけが返る。広告は 2 の後 1（可）になる。要求の本文には証明の札（StreamerContext の 2、92 バイト、先頭 0x32）が入っている。札は作られているが、配信側が受け入れない。BotGuard の実行のどこで我々の JS・DOM が違うかは未特定。MSE の再生はその間、遅れも飢えも無く進んでいた |
| K8 | 頁がほぼ黒く、画像と本文が出ない | QEMU 4 CPU、基準も HEAD も（再現は anthropic.com/claude-opus-5-5） | 一部：img の crossOrigin の IDL が無く WebGL の texImage2D が SecurityError だった（4001607f）。残り：SVG の text・textPath・clipPath・filter（nanosvg に無い）で大きな題が出ない、HTML の clip-path が無く畳んだ検索欄が見える、案内のボタンが線の引かれた箱になる（原因未特定） |
| K9 | 右の段が出ない | QEMU 4 CPU、基準も HEAD も（再現は news.yahoo.co.jp） | 修理：position: sticky が無かった（f2b3755b）。QEMU で右の段が出た |
| K10 | 格子の 1fr 行が scroll container の中身の高さまで伸び、見出しが画面の外へ出る | QEMU（再現は animate.style） | 修理：css-grid-2 §6.6 の最小の寄与（80b46118） |
| K11 | 幅の float の最後の桁で、収まる字が「comm…」と省かれる | QEMU、基準も（再現は GitHub のファイルの一覧） | 修理：LAYOUT_FIT_SLOP（95bb56b1） |
| K12 | display:none の下の slot に割り当てた子の CSS 動画が止まらず、毎コマ全体を描く | QEMU（再現は Reddit の静かな頁、2 秒に約 70 回・1660 ms） | 修理：e5cce4a3 で 0 回 |
| K13 | 子の文書（iframe）の描き直しが、親の頁の全体の描画になる。隠れた枠・箱の無い枠でも | QEMU 4 CPU、--debug-js（再現は Reddit を 16 頁送った後の静かな間、2 秒に 70〜81 回の全描画） | 修理：枠の箱のインクだけを損傷にし、見えない枠は何もしない。枠の面の準備は描く範囲（clip）に絞る（変形の祖先があれば絞らない）。同じ条件で 0 回 |
| K14 | 止まった動画・画面の外の動画が、毎拍 doc_paint_damage を出し続ける | 同上、K13 の後（2 秒に 122〜134 回の部分描画、各約 11 ms） | 修理：写しを消すと配った枚の数が 0 に戻り「未公開」が永久に真だった。写しの外の数（damaged_frames）で同じ枚の要求を一度にした。同じ条件で 0 回、待ち 1935 ms/2 s |
| K15 | 変形（transform）の面の写しが、その範囲の動画の継ぎ当てを拒む。F12 を押すと変形の面の描画中に落ちる | QEMU（再現は Reddit）。落ちたのは並列の写しに F12 を扱う区切りの処理を渡した版 | 修理：拒むのは動画が私的な面へ描かれたときだけ（snapshot_canvas）。写しは二つの段（被覆・標本）を並列にし、区切りの処理は段の間で一度だけ。再生中の動画での継ぎ当ては未確認 |
| K16 | --debug-js の描画の記録が、毎描画で全節点を 3 回辿る（標本の約 35%） | QEMU、prof の標本 | 修理：媒体の登録から辿る。これより前の --debug-js の数はこの分だけ重い |
| K17 | 音量を変えると volumechange が同期で出て、受け手が同じ値を書き戻すと際限なく呼び返す。有限でない値が IndexSizeError | WPT event_volumechange・volume_nonfinite・volumechange-ratechange-queue-task | 修理（4fe6e4cf） |
| K18 | 自前の試験の失敗 7 組：web/fonts（U+1F0A1 の代替が Plangothic の字形でない 4 件）、web/engine（filter-shadow-fixed-cb・deferred-float-definite-percent）、web/javascript（no-os-exposure、image-lazy-candidate・image-queue-*、mime-rejection-reported、modulepreload、performance）、web/shadow-dom-render、web/message-channels、web/import-maps、web/tcp-capacity-recovery | QEMU 4 CPU、この会話の 4001607f〜の各回で同じ組（test-web-grid・grid2・sticky・frame5） | 片付けた。製品の欠陥は二つ：K19 と K20。残りは試験の期待値が今の仕様・設計より古かった（9785fa80・d6f15b9b）。fonts は合成の太字の代わりの字体も太らせるのが今の設計、tcp は TCB の表が伸びる、message-channels は小さな上限で拒まない |
| K19 | CSS の AST が一規則あたり約 355 B で、shadow の 1.8 MB の CSS が 35.5 MB の AST になる | test-web-k18 の web/shadow-dom-render | 修理（0ef71b96）：@import の欄を規則の外へ、単純選択子の値と a・b を union、索引の項の pseudo を 32 ビットに。同じ試験で 31.6 MB（test-css-k19）。10-25 の 99e0686c が消した style_index_entries の数え方も戻した |
| K20 | module の依存と import() を、主スレッドが一つずつ同期で取得する（再現は Reddit の module の GET が 163.9→166.7 秒に一つずつ並ぶ）。import maps の currentScript の試験もその同期の取得で手前の例外になる | QEMU、--debug-js（clip7）と test-web-k18 | 修理（9b696216）：HTML の fetch the descendants のとおりグラフを並べて取得してから link。QuickJS は失敗の後始末（js_unresolve_broken_modules）を足した。Reddit の取得の時刻は未測定 |
| K21 | ゲストの sh が、全組を並べた一行の autorun で argv を書き潰して落ちる | test.py の全組の一回り | 修理（bad5517d）：単語・引数・展開・脚本の行を固定の上限で切らない |
| K22 | 長い試験の途中でゲスト全体が止まる（k19・k23・再現 hang1 で三度、場所は毎回違う）。目録の B+木の枝の最初の鍵が古いまま子が割れて枝の順が崩れ、在る文書の範囲を見失う。読み込みが長さ 0 の穴で回り続け、呼出しを受け持つ BSP が止まる | QEMU の HMP（CPU0 の RIP・RBP の鎖・version_read の範囲の変数。CPU1〜3 は hlt）。host の store tree-test で前の版は 12000 手以内に崩れる | 修理（304be1a1）：枝の最初の鍵を探しにも検査にも使わない（形式の文どおり）。目録が食い違えば -N_ERR_IO。直した版は tree-test 12 種×200000 手を通る。続き（82f32e43）：枝を併合すると右の枝の最初の鍵がそのまま区切りになり、その子の小さい鍵を見失っていた（10-23 の 540df2f3 から）。親の区切りの鍵で入れる。範囲で消す手元の試験で前の二つの版は鍵を失い、直した版は 6 種を通る。tree-test に範囲で消す手番を足した（直す前は 3 種中 2 種で崩れ、直した版は 12 種×200000 手を通る）。10-23 以後に書いた C: の巻（Hyper-V・実機）は鍵を失っている見込みがある。Windows から `store check \\.\PhysicalDriveN` で読むだけで確かめられる |
| K23 | 一 realm あたり約 5 MB の束縛。Yahoo の 10 文書で JS の heap が約 100 MB | QEMU、--debug-js の heap の census（2 文書で 10.8 MB） | 一部：Intl と CLDR・ICU の表を初めて使う時に作る（427815a1）。一時の計測（HEAPMARK、QEMU、Yahoo の 20 realm）で一 realm 約 15 ms・3.4 MB：bytecode の読み込みと最初 3〜4 ms・1.7 MB、include の合計 6 ms・1.6 MB、末尾の WebIDL の名付け 4〜5 ms（5381 項目を走査し 2026 を改名、委譲 0）。iframe の初期 about:blank も realm を作って遷移で捨てる（20 のうち 6）。残りは未着手 |
| K24 | AP で片付かない呼出しは BSP が一つずつ受け持つ。大きなファイルの読み込みや核の長い処理の間、他の app の呼出しが全部待つ | K22 の調べ（sched_ap_interrupt → isr_dispatch_remote） | 設計の天井。未着手（ブラウザーの Worker が一 CPU 分しか使わないという前の観測と関係しうるが、未確かめ） |
| K25 | 長い試験の一回りで C: が満ち（1342971 塊のうち 1337719）、試験の実行ファイルを書けずに 43 件が落ちる。交互に通るのは、書けなかった上書きが前の版を放すから | test-k25（store check は 0 problems）。試験の実行ファイルは一つ約 90 MB で、runner が一つも消さなかった（51 種で満ちる） | 修理：runner は試験ごとに新しいフォルダーで走らせ、終われば消す。test-k26（全組）で書けない失敗は 0、巻は 909 MB のまま |
| K26 | 自前の試験の期待値が設計より古い：sandboxprofile（10-16 の ff78d7d5 で script を止めた same-origin の枠と CSP の sandbox の合わせを実装した後も、拒む前提）、lineclampreentry（10-17 の 0c1c1cca で絶対の min-height は引き伸ばしで配置し直さない設計にした後も、1px で再配置を期待） | test-k25 | 試験を今の設計に合わせた（CSP の sandbox は合わせて script を止める場合と、空の sandbox で不透明な起点を拒む場合を見る）。intersection-observer/inline-client-rect の 2 件は、既定の serif（Noto Serif、空白 0.26 em）で空白が 4.16 px になり、offsetLeft の丸めから作る期待と合わない。基準 b2f5b721 も同じ字体と同じ送りの計算（コードで比べた。走らせてはいない）。networkpacing/pause-fairness-batch（10-15 04:48 の 14dbdaab）は、同じ日の 75597598 で止めている間の先読みを一つの塊にまとめる設計にした後も、一回の汲み出しで二つ以上の塊を期待していた。4KiB の枠より少ない塊と汲み出しで終わることを見るように直した。test-k27 で三つとも通る（sandboxprofile は検査が 94 になり、runner の期待の数を合わせた） |
| K27 | Web Audio API が一つも無い（AudioContext・OfflineAudioContext・節点・AudioParam）。指紋の script が OfflineAudioContext の無い道で callback(null) を受けて自分の xor で TypeError になり、約 3 秒ごとに 2.5 秒の timer をやり直し続け、主スレッドが止まる（再現は Bilibili の動画の頁。Floorp では同じ script が 45 秒で二回・計 36 ms）。音を Web Audio で作る頁は無音（再現は musewonder.win） | clip7 の serial（372〜387 s に五回の Timer task 2.5 s と TypeError、stack の桁 1:96896・1:90429）、Floorp の記録 | 未着手 |

## 5. 受入表（browser-hikaku の組）

参照：`C:\Users\cesta\OneDrive\Desktop\NOS BROWSER\WEBSITE PICS`（Floorp、窓 2559×1439、頁の表示域は約 2517×1354。Nocturne の 1920×1080 とは幅が違うので、応答型の段組みは同じ幅の参照を自分で撮って比べる）。
操作はどれも、本文へフォーカスして下へスクロールし、上へ戻す。日々変わる中身は数えない。

| サイト | 照合する部品 | 操作・測る値 |
|---|---|---|
| news.yahoo.co.jp | 上の細い帯と検索欄、ロゴ、キーワード欄と右のボタン、青い案内帯、分類のタブ、話題の一覧（NEW の印）と写真、右欄の順位（番号・小さな写真）、ヤフコメ 3×3、ライブの枠、おすすめの一覧 | 最初の本文まで、落ち着くまで |
| youtube.com/watch?v=vuYLCMyzFD4 | 上部（三本線・ロゴ・検索欄とマイク・作成・鈴・顔）、左の大きな再生器、右の絞り込みの帯と関連動画の列（縮小画像と二行の題）、**題名（太字・約 20px・一行）**、チャンネルの行、操作の行、灰色の説明欄、コメント数 | 再生の進み、最初の本文まで |
| github.com/jc4789/Nocturne-OS | 道しるべ、リポジトリのタブ、題と Public、Watch・Fork・Star、枝の選択・Go to file・Code、最新の commit の帯、ファイルの表（印・文・時刻）、右の About・Releases・Contributors・Languages の帯、README の見出し | Floorp はログインした状態（Settings のタブ等はログインで変わるので外す） |
| bilibili.com/video/BV1cgYQ6PE8c | 上部の案内と検索・投稿の桃色のボタン、題と情報の行、再生器、弾幕の帯、操作の行、説明とタグ、右の投稿者の札と「关注」、弹幕列表、合集の一覧、おすすめの列 | Floorp の画面は暗い配色（prefers-color-scheme の扱いを確かめる） |
| baike.baidu.com/item/三体/5739303 | 上の小さな案内、ロゴと検索欄・二つのボタン、青い案内帯、題「三体」と小さなボタン、動画の三枚、要約の本文（リンクと注の番号）、右の本の表紙・TA说・統計、百科图谱、相关星图の札 | Floorp でも読み込みが遅かった回 |
| reddit.com | 上部（ロゴ・検索の丸い欄・登録・ログイン）、**左の案内欄（ホーム・人気・ニュース・探索、リソースの一覧、規則の一覧）**、並び替えの帯、投稿の札（印・名・参加・題・動画や画像・票と注釈と共有）、右の人気のコミュニティ（5 件と表示を増やす）、下の注記 | 画像の取得の失敗をログで見る |
| anthropic.com/claude-opus-5-5 | 最初：左右の切り絵の画像、中央の夕焼けの帯と明朝の「Introducing」、Scroll down。下へ：丸い案内のボタン、日付、大きな明朝の題、目次 (1)〜(4)、生成りの地の大きな明朝の本文 | スクロールで画面が切り替わる |
| webglsamples.org の city | 毎コマ描く WebGL の街 | fps（頁の数え） |

Floorp の性能記録（10/26 04:16、74.5 秒の範囲）は頁を開いた後の範囲が中心で、読み込みの節目はほとんど範囲の外。範囲内の CPU は anthropic 2.7 秒、reddit 2.0 秒、bilibili 0.21 秒、youtube 0.18 秒、baidu 0.17 秒、yahoo 0.10 秒、github 0.09 秒（実機）。読み込みの時間の比べには撮り直しが要る。

## 6. 測定

同じ条件の前後と Floorp を並べる。QEMU は WHPX・1920×1080・`snapshot=on`、媒体は今日ビルドの基準 b2f5b721 と HEAD。画像は `build/qa/hikaku/20261026-*`。

| 測るもの | 条件 | 基準 b2f5b721 | HEAD | Floorp（実機） |
|---|---|---|---|---|
| city の fps（頁の数え） | 4 CPU・4 GiB、起動して最初に開く | 6（一度 7） | 7 | 未測定 |
| 同 | 4 CPU・4 GiB、組の 7 サイトの後 | 6 | 6 | — |
| 同 | 1 CPU・4 GiB、最初に開く | 2 | 2 | — |
| 使用中の記憶（System Monitor） | 4 CPU・4 GiB、起動直後 | 約 245 MiB（ブラウザー 125 MB） | 約 245 MiB（ブラウザー 128 MB） | — |
| 同 | 組の 7 サイトの後 | 821 MiB（ブラウザー 484 MB、カーネルのヒープ 226 MB） | 892 MiB（ブラウザー 578 MB、ヒープ 202 MB） | — |
| YouTube の動画の頁 | 1 CPU・512 MiB | 落ちる（ブラウザー約 340 MB で枠が尽きる） | 落ちる（同じ） | — |


## 7. 決定

| # | 決定 | 理由 |
|---|---|---|
| D1 | 基準 b2f5b721 を別の作業木で全体ビルドし、組を基準と現在で撮り比べる | 退行かどうかは、変更前と並べないと判定できない |
| D2 | AP の読み取り区間で、事象待ちだけは自分の task を park に登録してよい（約束を smp.h に書いた） | 待ち行列の解放が登録より前に来ないことを、読み取り区間の同期で保つため |
