# JavaScript統合と文字コードの実サイト検証・追補

## 判定

**部分修正。指定4サイトの全面動作は未達成。** この文書は同日の
`browser-validation-2026-10-06.md` の後の作業を記録する。古いログの結果を
最新の結果へ書き換えず、到達した境界を区別する。

Nocturneのカーネル、ABI、プロセスやファイルシステムの方式は変更していない。
POSIX互換層を追加せず、QuickJSとブラウザー側を既存Nocturneへ適合させた。
実サイト検証はNocturne自身のブラウザーをQEMU（512 MiB、pc/IDE/e1000）で動かしたもの。
全VMは所有する隔離scratchとsnapshotを使用。既存Hyper-V VM、配布媒体、ユーザーデータは変更していない。
QEMU成功をHyper-V第2世代の動作確認とは扱わない。

## 直接観測した実サイト

| ページ | この追補で確認した進展 | 残る問題・判定 |
|---|---|---|
| `https://www2.5ch.io/5ch.html` | HTTP/metaのShift_JISを誤ってWindows-1252扱いしていた。共通HTML decoderへShift_JISを追加し、**公開ページにQEMUから接続して本文の日本語表示を確認**。 | タイトルバーの日本語はカーネル描画の別課題。一部字形やレイアウト全般の完全性は主張しない。JavaScript合格試験の代用ではない。 |
| `https://deepmind.google/` | build9で実スクリプトがNodeList・観測APIの欠落を越え、Modelsへのホバーで実メニューが開いた。 | localStorage参照、video.load等の例外とCSSの崩れが残る。全体は未合格。 |
| `https://duckduckgo.com/` | **通常版**の入力欄に文字を入れ、サイトのJavaScriptによる候補欄表示まで確認。resource loadがWindowへ誤伝播する不具合、入力選択API、User Timing、旧MouseEvents等を修正。 | 検索結果ページは未合格。build11で正常なHTTP 200のモジュールを`application/x-javascript`未対応により拒否していた。修正後build12でさらに処理が進み、メモリ不足／時間切れ、および例外生成時クラッシュが露呈。 |
| `https://www.youtube.com/` | build10で10.8 MBのスクリプトが64 MiBの**QuickJS quota**に達していたと記録。有限上限を128 MiBへ変更。build12ではcompile 11135 ms、execute 105 msで`HTMLTemplateElement`参照まで進んだ。 | `performance.timing`、Canvas 2D、SVGElement、HTMLTemplateElementの未実装境界。画面は白地と黒線3本のままで、検索・動画一覧・再生は未合格。 |
| `https://chatgpt.com/` | この追補中の実接続でも確認ページのJavaScript例外を記録。 | アプリ本体に到達せず、表示・ログイン・送信は未確認。変動する確認用スクリプトの位置だけから欠落APIを推測していない。認証やアクセス制御の回避はしない。 |

実画面・ログ（リポジトリ内）:

- `build/nocturne-audit/js-live-10/5ch-shift-jis.png`
- `build/nocturne-audit/js-live-9/deepmind-models-hover.png`
- `build/nocturne-audit/js-live-11/ddg-typed.png` と `ddg-results.png`
- `build/nocturne-audit/js-live-12/qa-serial.log`
- `build/nocturne-audit/ddg-fault-12/fault.json`、`fault-stack.bin`、`fault-object.bin`
- `build/nocturne-audit/performance-youtube-10/report.md`、`performance-youtube-11/report.md`、`performance-youtube-12/report.md`

DDGのクラッシュは同一build12バイナリで再現し、既存のユーザー例外分岐にQEMUの診断ブレークポイントを置いて採取した。
カーネルを変更せず、`build_backtrace`→`JS_DefinePropertyValue`→`JS_DefineProperty`で停止。
対象objectはfree_mark=1、class_id=0、shape/prop=NULLで、`free_object`の解放済み状態と一致した。
refcountはobjectより前の別headerにあるため、このdumpでは未測定。
これは通常の未実装API例外ではなく、例外生成中の解放済みobject参照である。
QuickJSの`build_backtrace`が借用していた例外objectは、stack文字列のメモリ不足で
`current_exception`が置き換わると唯一のownerを失っていた。対象objectを保持し、
元の保留例外と割込み属性を保存・復元するよう同関数だけを修正。
途中失敗でもbufferを解放し、内部`JS_EXCEPTION`値をstackプロパティへ格納しない。
実QuickJSへのallocation故障注入は、修正前の8番目の故障で実クラッシュし、修正後は207検査すべて成功した。
このホスト回帰と、Nocturne QEMU／実DDGの再検証は別枠である。

ホストブラウザー、取得したHTML、ローカルfixtureの合格を、この表の実サイト操作の代わりにしていない。

## 共通実装の修正

- NodeListとHTMLCollectionを実装。native DOMにつながるlive collectionと静的querySelectorAllを分離。Arrayの別名で偽装しない。
- MutationObserverをJSからの実DOM更新境界へ接続。属性、文字列、childList、subtree、旧値、microtask通知を扱う。ただしincremental HTML parser/document.writeの全変化通知は未対応。
- 入力欄・textareaの選択状態をUTF-16で保持し、JS APIとnativeキー編集・選択描画を同期。実QEMUで選択置換と青背景の描画を検査。文書全体の選択、マウスdrag、IME、bidi・書記素処理は未完成。
- Performance User Timingとmark/measureのPerformanceObserverを実時計・実通知へ接続。Resource/Navigation/Paint Timingを偽のゼロ値で実装したことにはしない。
- resource `load`はDocumentからWindowへ伝播しないよう修正。以前は各script loadがWindow capture listenerへ入り、DDGがlistenerを増殖させて時間切れになっていた。
- legacy Event/CustomEvent/UIEvent/MouseEventの初期化とprototypeを実装。PointerEventの名前だけを生やす回避はしていない。
- ResizeObserver/IntersectionObserverはnativeレイアウトとスクロールの変化に基づく。固定サイズ・常時visibleの通知はしない。
- module MIMEは標準のJavaScript MIME 16種と、Content-Typeの引用・複数値を処理する。HTML/plain等をJavaScriptとして通す変更ではない。
- source preparationのCOMPILE_ONLYを累積30秒、実行・microtask・native DOMを共有5秒とする。nested callbackやscriptで予算をリセットしない。JSのeval/Functionは実行側の枠。native moduleネット待ちは両枠から除外し、各リクエストの期限は維持する。QuickJS parserは完全な途中割込みに対応していないため、戻った時点の時間検査も行う。
- allocation診断はQuickJS quota拒否とNocturne allocator失敗を区別する。上限増加をサイト対応完了とは扱わない。
- Shift_JISはWHATWG公式の固定JIS0208表に基づくallocation-free decoder。ストリーム境界、半角カナ、Windows-31J拡張、エラー時ASCII復帰を扱う。EUC-JP等の全面対応やJS TextDecoderへの接続は今回未実装。出典・ライセンスをソースとイメージ内`/usr/share/browser/encoding-notices.txt`へ保存。

## 補助回帰と独立レビュー

build12のQEMU内でNocturneのtcc/libcを使い実行した結果:

- fonttest: 83、Shift_JIS: 26、webtest: 99、jstest: 722、media policy: 106、media events: 12、hover: 447、すべて失敗0。
- image試験: 失敗0、予期しない例外0、5要求・5完了。
- 証拠: `build/nocturne-audit/js-contracts-12/qa-serial.log`。

build11のPerformance境界テスト失敗は、固定8msの待機が再入通知より先に終了する競合だった。
製品コードを変えずホストのtimer dispatchを32ms遅延させると旧試験は失敗、新試験は成功した。
有界条件待機へ修正し、通知順序・回数・batch identityとmicrotask checkpointの検査は保持した。
この補助結果と、実DDGの入力操作成功を別の証拠として扱う。

新しいsol担当の入力選択、Performance、Shift_JISの結果は、親がソース・native QEMU統合・実サイトを別途確認した。
MIMEとコンパイル予算の独立レビューは複数値MIME誤受理とnative待ち時間の混入を発見し、親が修正した。
限定モデル検査109成功は、実QuickJS・QEMU合格の代用ではない。

## build13の統合再検証

既存ビルド経路で終了0。起動時に用いたIMG/VHDXと対象ソースのSHA-256を
`build/nocturne-audit/build13-hashes.json`へ保存した。Hyper-Vへコピーはしていない。

`js-contracts-13/qa-serial.log`の実Nocturne結果:

- JavaScript **746件、失敗0**。MIME全16種・複数値・無効値・誤実行拒否と、実行／microtask／heap上限を含む。
- QuickJS OOM故障注入 **207件、失敗0**。Nocturneのtccで実coreへリンク。
- font83、Shift_JIS26、web99、media policy106、media events12、hover447は全失敗0。imageも失敗0。

別担当の実QuickJS独立レビューは、catchable/uncatchable・別ownerの故障群を追加して459検査成功。
その後の実interrupt補助では内部poolのためbackend故障点へ到達せず、補助仮定1件が失敗した。
この未到達を成功へ置き換えず、`qjs-backtrace-independent-review.md`と最終ログに保持した。

実DDGの検索ページを初回と再読み込みで検証。build12のページフォルトは出ず、
native UI操作と次のページへの移動は継続できた。ただし結果表示は依然未合格。
初回は5秒実行枠、再読み込みは128 MiBのruntime quotaへの到達も観測した。
ログのcharged=134204028、limit=134217728、peak=134217467 bytesはQuickJSの課金量であり、
OS全体の物理使用量ではない。これ以上の上限増加で解決済みとはしない。

同じbuild13でユーザー提示の5ch実URLへ移動し、本文の日本語表示を再確認。
画像は`build/nocturne-audit/js-live-13/5ch-shift-jis.png`。
残るタイトルバーの日本語、一部字形、JS有効時のnoscript内容の誤表示、CSS配置は別の未解決課題として保持する。

続けて実DeepMindへ移動。mainはcompile187 ms/execute2081 ms、171回のlayout読み取り1890 ms。
Modelsホバーのメニュー展開を再確認し、`deepmind-initial.png`/`deepmind-models-hover.png`を保存した。
StorageのgetItemとvideo.loadの例外は継続し、正しいレイアウト・全機能の合格とは判定しない。
最後の終了操作後、別の`webfetch`子プロセスのpagefault（rip=0x40090c、addr=0x7fffffeeed88）もserialに記録された。
原因は未特定であり、上で修正したbrowser/QuickJSのUAFと同一原因とは断定しない。
ネットワークワーカー全般の安定性まで確認済みとはしない。

build13で再試験していないYouTube/ChatGPTの状態を最新ビルドの合格とは呼ばない。
YouTubeの実到達点は上記build12、ChatGPTはこの追補中の確認ページまで。

J-spaceの全リポジトリ固定hash gateは、変化し続ける巨大Hyper-V媒体を含むmapのため未合格。
限定ソース・検査の記録を正式gate合格と呼ばない。
全ての担当QEMUは停止済み。親の最新5プロセスの不存在照会は`parent-qemu-cleanup-13.json`に保存。
