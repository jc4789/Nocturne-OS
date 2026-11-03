# SharedWorker と既存 Worker 改善：実装引継ぎ

## 現在の状態

2026-10-11 の担当実装。親担当が総合ビルド・梱包・実 QEMU／実サイト操作を実施する。以下のホスト検査を、その実製品受入や性能比較へ読み替えない。

## 製品の変更

- `user/libc/web/js_worker.js`：SharedWorker コンストラクター、名前／オプション、専用 MessagePort、共有インスタンスへのエラー配送。専用 Worker と共有 Worker のブランドを分離し、借用した terminate／postMessage が共有接続を破壊しない。
- `user/libc/web/js_worker.c`、`js_worker.h`：同一プロセスの文書共有、文書別のネイティブ port 配送、文書離脱時のポート切断・要求取消・残存所有者へのキュー移管。Nocturne の named duplex channel による別ブラウザープロセス共有も接続する。
- `user/apps/browserjsworker.c`：独立 QuickJS／ネイティブタスクを保つ共有サーバー。複数 client の非ブロッキング接続、部分 packet 受信、内部 port ID と client の元 ID／generation の相互変換、通信所有者の再選出、途中要求の拒否、最後の接続終了時の回収。
- `user/include/js_worker_wire.h`、`user/libc/web/js_worker_port_lifetime.h`：接続操作・共有開始情報・ネイティブ client 所有者を追加。既存の 24 バイト packet header は維持。
- `user/libc/web/js_worker_runtime.js`：実 connect MessageEvent、source／ports、SharedWorker 内の専用 postMessage 非公開、WorkerLocation の URL 各属性。タイマーを安定した作成順付き最小ヒープにし、次期限取得を全走査から定数時間へ、追加／取消を対数時間へ変更。期限が同じ timer の順序は ID の巻き戻りに影響されない。
- QuickJS 割込の時計 syscall を 16 回単位へまとめる。閉鎖状態は毎回確認し、ネイティブ task 終了による取消を保つ。実サイトの高速化率は未測定。

共有キーはネイティブ文書の完全な origin を使用する。名前が空なら URL、名前が非空なら名前を識別子とし、URL は接続時にサーバーの canonical URL と照合する。キーを hash だけで識別しない。名前は長さと 16 進表現を含め、埋込み NUL を切り捨てない。異なる URL で同じ名前へ接続して別の静かな realm を作らない。

START より早い CONNECT は開始前専用キューへ保持し、スクリプトの開始後に配送する。START 前に通信元が消えた場合は、次の生存 client にネイティブ開始要求を送る。稼働済み共有タスクは最初の窓の終了では殺さず、SPAWN_DETACH と接続寿命で管理する。client の回収は port／出力／deferred packet／要求の参照数を使用し、各 poll で全キューを client ごとに再走査しない。

## 共通 ABI の分担

別担当の Nocturne 専用 API `n_channel_listen`／`n_channel_connect`／`n_channel_accept`、動的 FD 表、動的 poll 表を使用する。UNIX socket、fork、ファイルへの packet 保存・読戻しは追加しない。`js.c` の native operation／origin／CSP 統合は親担当。生成済み `js_worker_runtime.inc` は正規 `js_worker_embed.py` で更新した。最終 browser binding の生成と全梱包は親担当。

## 実施した焦点確認

- `tests/sharedworker_hosttest.c`：製品 `js_worker.c` を複数の QuickJS context で使用。文書共有、origin 分離、名前／URL 拒否、port client 所有者、他文書の capture 拒否、START の前置、元所有文書破棄後の生存・取消・キュー料金移管・最後の所有者回収。下記 stream 修理後には、退役する非所有文書の実 packet と synthetic packet を並べ、実 packet の credit だけを 1 回返す確認も追加。最終実行 13 項目成功。
- `tests/sharedworker_runtime_hosttest.c`：正規生成済み実 Worker bootstrap、実 MessagePort／clone／native transfer commit。connect の trusted イベントと source／port 同一性、返信経路、ArrayBuffer の実 detach、完全 WorkerLocation、タイマー優先順位・大量取消・ヒープ回収。最終実行 12 項目成功。
- `tests/sharedworker_server_hosttest.c`：製品の別 client ID 変換、返信の client／local ID／generation、二重変換防止、通信所有者切断と生存 client、保留要求拒否、対象 port だけ閉鎖、切断出力回収、開始前 source 再依頼、名前付き URL 不一致。参照寿命修理後の最終実行 9 項目成功。
- 最新製品の `js_worker.c`／`browserjsworker.c` は、Nocturne 用 include と target で clang 構文確認成功。

これらの host fixture では Nocturne の実 spawn／channel／ネットワークはモデルまたは置換であり、OS 内の通信受入ではない。最初の fixture 接続にはホストヘッダー衝突、未配置 alloca、trusted host の不足、未リンク host stub の失敗があった。各検査不備を直した後の結果を上記に示す。期待値を弱めたり skip を追加したものではない。

## 受入と残る制限

親担当が WHATWG 公式の `https://html.spec.whatwg.org/demos/workers/shared/002/test.html` 等と実ブラウザー窓で受入する。Hello World／pong だけで、別窓共有・先頭窓の終了後の状態保持・origin 分離・GUI 応答まで合格にしない。

module Worker、Worker 内からの nested Worker、既存の brokered MessagePort の再転送、Worker 境界の Blob structured clone は今回の実装でも未対応。開始済み Blob Worker は共有 realm に source を保持するが、初期所有プロセスが START を届ける前に終了したケースで、別プロセスから失効済み Blob URL を再取得できるとは保証しない。

実サイト全体の性能改善、QEMU の実ポート通信、複数窓共有・再接続、実サイト回帰なし、Hyper-V 実機はこの担当だけでは確認していない。最後の追加修理は、peer EOF と queued port-close の間で postMessage が誤って例外にならず、複製／転送を行って宛先を破棄する処理、および SharedWorker console を架空の PID 0 表示にしない処理。これらを含む最新 source の梱包は親担当へ通知済み。

仕様照合：WHATWG HTML Living Standard の Workers／SharedWorker／MessagePort 接続処理。
https://html.spec.whatwg.org/multipage/workers.html#shared-workers-and-the-sharedworker-interface

## 実サイトで判明した専用 Worker の送信停止と追加修理

親担当は実 WHATWG primes ページが正しい見出しまで表示されても数値が空であることを報告した。対象は `https://html.spec.whatwg.org/demos/workers/primes/page.html`、ログは `build/nocturne-platform/ddg-font-csp-first/final-interactive.log`。同じ観察段階の共有公式デモでは複数窓の接続番号と pong が報告された。これは後続 source の受入へ流用しない。

専用 Worker の publication は出力キューへの追加だけで、flush は JS task の終了後だった。公式の `while(true)`＋`postMessage` では終了へ戻らず、相手 realm の message 配送が止まる。修理対象は `browserjsworker.c`、`js_worker.c`、`js_worker_wire.h`。

- transfer の不可逆 commit は 1 回だけ行い、その後に有限コピー量で非ブロッキング送信する。copy quantum より大きい record も、次の author task 終了を待たず、ネイティブ輸送だけを pump して完送する。
- 64KiB の byte-credit window で producer に背圧を掛ける。これは message サイズの拒否上限ではなく、単独の大きい record は実サイズのまま許容する。
- 親が実 child event を dequeue／廃棄したときだけ、対象 generation の private CREDIT を返す。synthetic／fanout コピー、終了・閉鎖済み child に二重 credit を作らない。文書退役時も、生存 shared child の実 event を捨てる前、shared 参照を解放する前に 1 回だけ返す。
- native wait は private credit だけを消費し、author message、LOADED、connect は通常 checkpoint へ延期する。author callback は再入させない。共有では各 client 別の credit を維持し、全 client のネイティブ入出力を pump する。broadcast の宛先も待機中に寿命を pin する。
- consumer 死亡・文書遷移による endpoint 閉鎖は native poll 待機を解除する。待機中に既存 task の絶対期限を書き換えない。

新規 `tests/worker_stream_hosttest.c` は実製品 bootstrap で、単一の未終了 JS task 内の 2,500 回送信、credit 背圧、author 再入なし、LOADED の保全、checkpoint で callback 1 回、credit 処理で transfer を再 commit しないこと、待機中の ArrayBuffer 転送、100KB record の完送、peer 死亡での待機終了を確認し、10 項目成功。退役 credit 修理を含む文書共有 fixture は上述の 13 項目成功。最新 product target の clang 構文確認も成功。host fixture の `SYS_OPEN` macro 衝突は host shim で修正し、期待値は弱めていない。以前の共有 server 9 項目／bootstrap 12 項目は stream 修理前の結果であり、今回は同じ検査を無変更で再走していない。

最新 source の全ビルド・梱包と実 primes／共有デモ受入は親担当が進行中。追加実装・検査を止める指示に従い、この担当は記録だけで終了する。

## 純 CPU 共有 Worker の最後の所有者終了

上記の固定後、host 呼出しも task 終了もない純 CPU 無限ループでは、last-client EOF を読む通常 pump へ戻らず、SPAWN_DETACH の task が残る静的不具合を親へ報告した。親の追加指示により、この寿命境界だけを修理した。製品変更は `browserjsworker.c` のみ。

通常 pump と interrupt の native accept helper を共通化し、1 回 16 peer までだけを受け入れて C の client metadata／FD を保持する。interrupt は既存の 16 回単位で、固定 stack 64 FD の batch と `poll(timeout=0)` で listener と active client の HUP を判断する。全 peer が消え、listener の確認前後にも pending 接続がなければ closed にして QuickJS を中断する。未 accept のまま死亡した peer も有限量ずつ受け入れて HUP を判断するので、pending tombstone が永久に detached task を保持しない。生きた新 peer や scan 中に到着した pending は保護し、未解析の bytes は通常 loop へ残す。

この interrupt helper は QuickJS API、deserialize、JS 値生成、`shared_disconnect`、author callback を呼ばない。同期待機、毎 bytecode の syscall、要素ごとの時計 syscall は追加しない。FD／キュー／JS context の解放は通常 shutdown に任せる。

新規 `tests/worker_shared_cancel_hosttest.c` の焦点確認 1 まとまりは 8 項目成功。初期 bootstrap の保護、15 回の interrupt では poll しないこと、生存 client の保護、80 client の全 HUP、live pending の未解析保持、96 dead pending の 16 個単位回収、scan 中の新 pending の保護、実 QuickJS の `for(;;){}` が最後の peer EOF で中断することを確認した。モデル内の read／write は一度も使わず、native ownership のみを判断した。Nocturne target の clang 構文確認も成功。

これらは endpoint readiness を置換した host 確認であり、OS 内の実 EOF／task 回収そのものの受入ではない。親は直前の配送媒体で実素数表示の連続更新と共有公式 2 窓の成功を報告しているが、最新 interrupt 修理媒体の実受入は親担当に委ねる。最新 source を固定して build ready を通知した。ここで担当実装と検査を終了する。
