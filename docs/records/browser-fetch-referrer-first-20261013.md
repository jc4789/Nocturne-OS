# 共通 Fetch の参照元・方針接続：第一報

## 実際の破損境界

- 主担当の実 Reddit 検索では、実マウス・文字入力・Enter によって URL は検索へ遷移したが本文は旧 feed のままだった。`build/nocturne-platform/compat-xml-sidebar-reddit-20261013/serial.log` と `search-enter.png` が基準証拠。
- 原例外は `Fetch referrer overrides requiring a Referer header are not supported`。作者の `Referer` 指定を許可する修理ではなく、Request の公開指定を private metadata として既存 native HTTP 責務へ渡す修理とした。
- 製品ソースへサイト名、特定 DOM、URL 特例は追加していない。

## まとまりとして実装した接続

1. Window の Request/Fetch は、明示された参照元の同一 origin 判定、資格情報・fragment 除去、実 client source、文書の effective policy を native request へ渡す。既存 forbidden-header 保護を維持。
2. 文書 policy container は response header と creator inheritance から初期化し、connected HTML meta の処理・属性変更・parser publication で局所更新する。meta 除去で以前の policy に巻き戻さない。Document.referrer の値とは別責務。
3. browser/webnet は所有済み metadata を transaction buffer に copy して publish。既存 64-byte wire prefix は不変更で、opt-in flag と 8-byte extension を使う。旧 framing と無指定呼出しを維持し、未知 flag・不正 length/policy/NUL を拒否。
4. HTTP は各 policy、同一 origin、downgrade、4096-byte を超える source の origin 化、redirect response の policy 更新を適用する。一度縮小・省略された source は次 hop で復活させない。既存 no_referrer flag を優先する。
5. Worker は既存 START/LOAD packet と requester を拡張し、creator/script policy と Fetch options の source/policy を実親 dispatcher へ接続した。世代・取消・binary POST を保持する。新 bridge、並列性の主張、全 Worker Request 対応の主張はない。

規範境界：[Fetch の Request](https://fetch.spec.whatwg.org/#dom-request)、[参照元の決定](https://w3c.github.io/webappsec-referrer-policy/#determine-requests-referrer)。

## 担当差分

- 公開・private 契約：`user/include/web.h`、`webnet.h`、`webnet_wire.h`、`js_worker_wire.h`。
- HTTP/転送：`user/libc/webnet.c`、`user/apps/browser.c` の host_request、`user/apps/webfetch.c`。
- 文書・parser：`user/libc/web/webi.h`、`doc.c`、`dom.c` の meta 局所通知、`html_lexbor_bridge.c` の既存 changed publication。
- JS/Worker：`user/libc/web/js.c` の private Fetch/requester/source getter、`js_fetch.js`、`js_worker.c`、`js_worker.h`、`js_worker_runtime.js`、`user/apps/browserjsworker.c`。
- 支持：`tests/check_fetch_referrer.py`、`referrer_policy_host_cases.c`、`js_fetch_cases.js`、`jstest.c` の Fetch fixture、`worker_stream_hosttest.c`。
- 主担当 CSSOM・scroll_input と他担当の既存変更を保全。生成 inc は主担当が正規再生成し、担当は手編集していない。

## 成立した支持と未成立

- `python -X utf8 tests/check_fetch_referrer.py`：実 product の canonicalization、HTTP policy helpers、native client-source helper を 167 条件で確認、失敗 0。記録は `build/fetch-referrer-host/result.log`。ネットワーク endpoint と QEMU の証明ではない。
- freestanding target の関連 C 9 ファイル構文、JS 2 入力の構文、担当差分の CRLF 対応 whitespace 確認は成功。
- Worker 原 fixture は private `referrerPolicy` getter 未接続により POST 未到達で失敗。`build/fetch-referrer-host/worker-before-private-getter.log` を保全し、実 product native getter へ接続した。
- 接続後の一回はコンパイル成功、最初の 3 条件成功後 `RUNTIME_EXIT 3221226505` で終了。`build/fetch-referrer-host/worker-after-private-getter-failure.log` を保全。限定 marker では FormData 作成完了まで進み、実 cryptoRandom/native_request に未到達のまま同じ終了。`worker-boundary-failure.log` と `worker-native-boundary-failure.log` を別名で保全し、marker と forwarding wrapper は自身の分だけ撤去した。POST・取消・metadata の最終支持は未成立。終了コードだけで fixture 不備／製品不具合を断定しない。
- 既存 exe の debugger 診断は LLDB 起動前に 3221225781 で終了。import dependency の python310.dll が無く、対象 Worker は実行されていない。`build/fetch-referrer-host/worker-debugger-boundary.log` を保全。環境をインストールで広げず、停止 backtrace 未取得とする。
- 主担当の新完全媒体 native 支持は `build/browser-referrer-pseudo-native-20261013/test-serial.log` で担当も読取確認：Fetch API 244、jstest 7/0、既存 pseudo-body 17/0、outer 2 PASS / 0 FAIL / 0 skip。以下の一回 Abort 診断追加前の媒体結果であり、追加後へ流用しない。
- 主担当の最新実検索は左欄正常、検索 URL へ遷移し、元の referrer 未対応例外は出ないが本文は旧 feed のまま。`build/nocturne-platform/compat-referrer-final-reddit-20261013/serial.log` と `search-settled.png`。AbortError が残るため検索内容の受入は未達。全体性能回帰解消も未達。

## 実検索の残る失敗に対する限定診断

主担当許可で `js.c` の既存 error 診断へ、debug_js の時だけ文書一回の P_FETCH 数値 snapshot を追加した。既に文字列化された AbortError prefix を判定し、resource-id、host-accepted、done/aborted/streaming/headers-published、response-event、deadline、既存 now を出す。URL/header/body/signal reason は出さず、作者 getter、追加時計、毎 Fetch の pending 全走査を増やさない。host-accepted は既存 native host_request の受理であり、実通信の完了でも wire-id でもない。追加後の対象 C 構文と差分確認は成功、新媒体実行は主担当統合待ち。

## 凍結と次の行動

製品 source は一回 Abort 診断を含め再凍結。主担当の単独性能 VM 観測中は重い host 検査を保留し、実測条件を汚さなかった。次は主担当が新媒体の同じ実検索で snapshot と実内容を照合して原因を絞る。Worker 未成立と debugger 環境限界を保持し、支持件数で実サイト改善や性能回帰解消を代替しない。
