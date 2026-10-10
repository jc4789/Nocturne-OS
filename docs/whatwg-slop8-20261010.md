# WHATWGを根拠にした8件の製品修正

2026-10-10。調査だけという旧依頼は、8件すべての実装・最新媒体での実サイト確認へ変更された。以下はその作業結果であり、ブラウザー全体の仕様適合を主張するものではない。

## 製品コード

| 対象 | 修正した境界 | 主な実装 | 根拠 |
|---|---|---|---|
| localStorageの別窓更新消失 | 古い全量保存で別窓の別キーを消していた | 同一originの共有状態、差分の反映、原子的な容量検証、ファイル所有者に結び付く排他、別realmへのStorageEvent。保存時も最新共有状態を基準にする | [HTMLの保存領域](https://html.spec.whatwg.org/multipage/webstorage.html#the-localstorage-attribute) |
| 遅延外部scriptのdocument.write | 解析終了済みの文書を破壊していた | 外部script実行と直後のmicrotask完了まで破壊的書込み抑制を維持。解析中の挿入点による書込みは維持 | [HTMLの文書書込み](https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#document-write-steps) |
| moduleのトップレベルawait | DOMContentLoadedを待つmoduleが同イベントを阻止していた | 解析用待機の解除とEvaluate Promise終了を分離。後続defer、DOMContentLoaded、文書loadをPromise終了待ちにしない | [HTMLの解析終了](https://html.spec.whatwg.org/multipage/parsing.html#the-end)、[module実行](https://html.spec.whatwg.org/multipage/webappapis.html#run-a-module-script) |
| Fetch/XHRの逐次応答 | 本文全量の受信まで応答ヘッダー・途中状態を公開していなかった | native通信でheaders/chunk/end/uploadを通知。pullによる流量制御、休止応答の待避、取消、逐次gzip復号を接続。XHRは2→3→4の状態と途中本文・進捗を公開 | [Fetch](https://fetch.spec.whatwg.org/#fetch-method)、[XHRのsend](https://xhr.spec.whatwg.org/#the-send()-method) |
| replaceChildrenの失敗時破壊 | 妥当性検証の前に既存childrenを消していた | nativeのreplace-allへ接続。文書・fragment全体と所有権移行の検証・必要領域確保を削除前に完了。custom element反応、MutationObserver、Rangeも接続 | [DOMのreplaceChildren](https://dom.spec.whatwg.org/#dom-parentnode-replacechildren) |
| Eventの公開属性改変 | 書換え可能な公開状態が内部配送と信頼性に影響していた | private状態と読み取り専用IDLを使用。公開dispatchはisTrusted=false、native入力・内部配送は内部経路でtrue。Worker側にも接続 | [DOMのEvent](https://dom.spec.whatwg.org/#interface-event) |
| MutationObserverの削除済み子監視 | 最初の通知前に全observerの一時監視を解除し、後続observerの記録を落としていた | observerごとの通知直前にnative記録を回収し、そのobserverの一時監視だけを解除してcallback。作成順を維持 | [DOMの通知手順](https://dom.spec.whatwg.org/#notify-mutation-observers) |
| Locationなしのリダイレクト状態 | Locationのない302等を通信エラーにしていた | 元のHTTP応答を正常公開。リダイレクト回数の最終境界も修正 | [FetchのHTTPリダイレクト](https://fetch.spec.whatwg.org/#http-redirect-fetch) |

主な変更先は `user/libc/web/js.c`、`dom.c`、`js_storage.js`、`js_fetch.js`、`js_streams.js`、`js_xhr.js`、`js_mutations.js`、`js_bootstrap.js`、`js_worker_runtime.js`、`user/libc/webstorage.c`、`webstorage_shared.h`、`webnet.c`、`http.c`、`http_gzip.h`、`user/apps/webfetch.c` と関連ヘッダー。共有保存の排他を `kernel/src/fs/vfs.c` とsyscallへ接続した。既存ABIの通信ヘッダー長は維持した。

`user/apps/browser.c` に `--maximized` を追加し、試験では最初から最大化した。内蔵コンソールはF12で開閉して例外と本文を両方見た。生成済みbindingはMakefileの正規依存関係から再生成し、生成物だけの手修正は行っていない。

## 確認と途中失敗

実OS内で `web_live` を通る解析・DOM/Event/MutationObserver・Fetch・XHR・Worker・保存・別process保存の確認は、それぞれ修正に対応する実行で通過した。これらは実サイト受入の代替ではない。

- `build/slop8-runtime-1/test-serial.log`：解析、DOM/Event/observer、XHR、Workerが通過。逐次通信のfixtureルートを誤ってOPTIONS内へ置いたためGETが404となった。正しいAPIルートへ移して再確認した。Fetchの必須引数検証の欠落は製品wrapperを修正した。
- `build/slop8-runtime-final/test-serial.log`：逐次通信、Fetch、保存が通過。別窓保存用の試験コードはゲストTCC非対応のatomic組込みでコンパイル失敗した。
- `build/slop8-storage-final/test-serial.log`：試験のphase通知を整列済みvolatile値に直し、別process保存・イベント・排他・破棄の41条件が失敗0。期待値を弱めていない。
- `build/slop8-xhr-final/test-serial.log`：最終版の逐次通信とXHRが通過。実Amazonで発見した旧通知の配送問題を修正した後の限定確認。

実Amazonで `abort()` / `open()` 後に予約済み旧XHR通知が実行され、消去済みヘッダーを参照する例外を発見した。通知予約時だけでなく、実行直前にも要求世代を検証する共通queue修理を入れた。例外の握り潰しではない。最終版Amazonでは当該例外は記録されなかった。根拠は [XHRのopen](https://xhr.spec.whatwg.org/#the-open()-method) と [abort](https://xhr.spec.whatwg.org/#the-abort()-method)。

## 媒体

最後の正規ビルドは `python -X utf8 scripts/build.py -j 8 --log build/slop8-final-build.log all`、終了0。IMG、VHD、VHDX、ISOを更新した。最終QEMU起動媒体のkernel/initrd抽出hashと現在のbuildは一致し、initrd内browser、browserjsworker、webfetchもbuild/rootと一致した。

- kernel SHA-256：`6614ef3888494269107c0116fbf9c09b319fe0cc845c9f5ce5c56207dd8b53c3`
- 最終initrd SHA-256：`181fc5e6c5fb291212f785dda0adf17f70eda89362e5dbc45c241414b6a676cf`
- 最後のXHR配送修理前initrd：`f506a18f6cb7d371c52480d5424678e20ad52db2153e2bd357e203c1b31a0045`。`slop8-final-baike/openai/amazon/reddit` はこの版。最終版とは区別する。

QEMUはWHPX、4096MiB、4CPU、1920×1080。本文viewportはゲスト全画面と異なり、通常は1908×947、コンソールを開くと縮む。ホスト空きRAMを考慮して同時2VMまでとした。ユーザーの永続data・Hyper-V VMは接続・操作していない。Hyper-V実機の動作は未確認。

変更前の通常窓と変更後の最大化窓ではviewportが異なるため、速度改善・性能回帰なしとは判定しない。以前の速度低下報告の原因と解消は、今回の8件の通過から証明できない。

## 実サイトで主担当が見た結果

最終版Amazonは `build/nocturne-platform/slop8-final-amazon-search`、最終版百度百科・Reddit・OpenAIは `slop8-latest-baike`、`slop8-latest-reddit`、`slop8-latest-openai`。同時に動く対象の画面を撮影し、主担当自身が画像閲覧した。PS/2入力を使用し、直前のtrusted hoverと実targetを確認してクリックした。観測補助は読み取り専用で、サイトDOM・style・関数を差し替えていない。ハーネスの終了成功とサイトの正常動作は別に判定した。

| 実サイト | 目視・実入力で成立したこと | 未達・残る問題 |
|---|---|---|
| Amazon | 最大化。実検索欄クリック、キーでqemu入力、Enter送信、40件の商品検索結果の本文・表紙画像を表示。PageDown後に別商品、PageUp後に先頭商品を表示。戻るでホーム、進むで検索結果のURLと本文へ戻った。F12で内蔵コンソールを開閉 | ホームの大きな空白。暗号処理未対応と参照元関連の例外。旧XHR通知のIllegal Headers receiverは最終版ログにないが、それだけでホーム正常とは判定しない |
| 百度百科 | 最大化。本文への実クリック、PageDownで下部のカード・footer、PageUpで本文先頭と画像へ戻った。F12でコンソールを開閉 | 右側の見切れとAxiosの404が残る。最終版の安全な同窓リンク候補は選べず、リンク・履歴成功とは判定しない。XHR最終修理前には別窓リンク先の辛亥革命の記事本文と地図画像を見たが、最終版の証拠と混同しない |
| Reddit /r/codex/ | 最大化。投稿本文・画像・sidebarを表示。本文への実クリック、PageDownで下部投稿、PageUpで先頭へ戻る。プロフィールリンクへtrusted hoverと実クリック。F12でコンソールを開閉 | 検索欄の重なり、アイコン配置不良。追加投稿の読み込み失敗とプロフィール本文への遷移未達。Fetchの参照元指定未対応例外が残る。URL変更だけで遷移合格としない |
| OpenAIの指定記事 | 最大化。チャレンジから記事URLへ自然に移り、ヘッダーを表示したところまで目視 | 本文は黒い領域。WebGL初期化失敗とCSS・JavaScript・画像取得403が記録され、その後サイト自身の「読み込めません」画面へ移った。目的の記事本文・操作・スクロールは未達。XHR最終修理前の装飾なし記事表示を最終版の成功証拠に流用しない |

Amazonで見た代表画像は `slop8-final-amazon-search/screen-2.png`（検索結果）、`screen-4.png`（下の別商品）、`screen-5.png`（上へ戻る）、`screen-7.png`（内蔵コンソール）。百度百科とRedditは各 `slop8-latest-*/screen-0.png`、`screen-2.png`、`screen-4.png`、`screen-7.png`、`screen-final.png` を主担当が見た。

OpenAIは `slop8-latest-openai/screen-0.png` のヘッダーと黒い本文領域、`screen-4.png` の読込み失敗画面、`screen-10.png` の失敗画面と内蔵コンソールを見た。遅い時刻のPageDown/PageUp入力後も記事へ到達していない。403やチャレンジをUA偽装、例外握り潰し、代替HTML注入で回避していない。

全4サイトの最終版起動metadataは上記の最終kernel/initrd hashと一致する。すべての今回起動VMは停止済み。専用試験ディスクとPNG化済みPPMだけを、解決済み絶対パス・リンクでないこと・非使用状態を確認して回収した。正規配布媒体、通常の `build/data.img`、ソース、PNG、ログ、metadataは保全した。回収した試験媒体そのものは残っておらず、metadataのhashを保存済みディスクと取り違えない。

今回修理した8件の限定確認は通過したが、実サイト受入は上表の成立した操作に限定される。OpenAIの記事、Redditの追加投稿・プロフィール、Amazonホームを正常動作と報告しない。WorkerのFetchは別の全量受信経路が残り、今回のWindowの逐次受信修理をWorkerの完全対応と報告しない。
