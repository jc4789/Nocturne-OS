# WebAssembly Promise / Web API 第二考察（2026-10-14）

## 独立に問い直した点

初回の「捕捉済み API だから作者変更から独立」という主張を、次 task までの変更と下位の Fetch / Streams 依存へ分解した。

1. **import getter の時期**: 現行 JS API の Module overload は Promise を返す前に imports を読む。同期 Instance 全体を単に timer へ送るだけでは getter も遅れる。このため Module.imports 順の即時 snapshot と native の各 namespace/name 読取に対応する再生 queue を実装した。重複名で getter が異なる関数を返す場合も試験した。
2. **options snapshot の再変換**: native Module は次 task で builtins iterable を再変換する。private 配列だけのコピーは Array.prototype の iterator 改変を受けるため、private own iterator へ修理した。未知 builtin 名と task 間 iterator 置換の実挙動を追加確認した。
3. **realm と資格判定の捕捉**: globalThis / Symbol / TypeError、setTimeout、Module、Headers.get、Response の getter / arrayBuffer を作者が置換しても、コンパイルと真正な MIME / HTTP status 検査は捕捉した経路を使用する。本文の消費自体は既存 Fetch / Streams に委ねる。
4. **下位依存まで無条件に強いとは限らない**: RegExp.prototype.exec を壊す独立試験では Node の Headers.get 自身が失敗した。Object.create を壊したままの試験では正規 upstream Streams の ReadableStream 構築が失敗した。これらは Wasm MIME trim の処理とは別の依存である。Wasm 専用の迂回は入れず、compile の捕捉を確認後 Object.create を戻し、Response / Headers の作者 override が MIME / status を偽れないことを別に検査する条件へ訂正した。この訂正を下位依存の全面耐性の成功とは扱わない。

## 追加された命令境界

主担当の libc nearbyint 変更に対応し、実 Wasm の f32.nearest / f64.nearest で ties-to-even、正負ゼロ、最小 subnormal、NaN、正負無限大を26件追加した。単に libc 関数を直接呼ぶテストではない。最終 full suite は168件で、下記二経路とも終了0。

- Node v26.8.1 native Wasm + Node Response / Streams: `WASM facade host checks 168`。
- Node VM native Wasm + 製品 js_fetch.js + 製品 js_streams.js / 正規 ponyfill: `WASM real Fetch/Streams bridge checks 168`。
- `node --check` による js_wasm.js / js_wasm_cases.js の構文確認も終了0。

## 変わった結論と残る限界

- Promise facade 単体の snapshot と、Fetch / Streams 全体の作者による全組込み変更への耐性を分ける。後者を保証したという主張はしない。
- streaming は全 body 読取後の buffered compile である。仕様上許容されるが、バックグラウンド並列実行や処理時間改善は未検証。
- 同期 Module / validate の dictionary / sequence 変換は native 担当へレビュー結果を送った。追加の native string builtins / imported constants、CSP の wasm-unsafe-eval / unsafe-eval、structured clone 等の現行仕様全体の対応をこの担当の host 成功から推定しない。
- Worker の bootstrap include 時期と worker 実 Wasm は主担当の統合範囲。window と worker で共通の捕捉 setTimeout を使うよう修理したが、ここでは Nocturne Worker の成功を主張しない。
- WABT native の Nocturne jstest、QEMU 実画面・実入力・性能・媒体をまだこの担当では実行していない。主担当の指定三サイト（百度百科、指定 YouTube、CNN、最大化と JS console 表示）での結果を別の受入証拠として必要とする。Photopea / Squoosh は受入に含まない。

## 引継ぎ

製品コード二ファイルは親担当による独立 review と正規生成・build・Nocturne 実行へ引き渡す。試験失敗があれば期待を弱めず native / facade / Fetch / Streams の境界へ戻って原因を修理する。

## native / Worker 追加依頼の再考察

同期 Module / validate の compile options は、Node v26.8.1 を現行 WebIDL の完全な oracle とみなせない。Node は primitive 辞書と重複 builtin を許容する一方、指定現行 draft は非 object dictionary を TypeError、重複名を無効とする。この差を隠すための条件分岐や skip は設けず、厳密な native suite と基本 facade suite を分けた。native suite が host で失敗する箇所と、その前に通過した16境界を初回報告へ明記した。

外側 Instance 変数を失った function / Table と externref の確認は到達可能性の検査であり、強制 GC API を公開していないためメモリ回収の証明ではない。Wasm memory.grow は JS Memory.grow と別の実命令経路で旧 ArrayBuffer の detach を確かめる。埋込み NUL 名は descriptor、native 同期 import/export、Promise snapshot の三層を通す。再入テストでは内側 trap を JS import が捕捉した後の外側実行を確認する。

Worker script の隔離 Node VM 確認は byte encoding と script 自身の検査に限られる。本物の HTTP Worker は `runWasmWorkerCases(url)` が生成し、root の jstest `wasm-worker` selector で別途確認する必要がある。postMessage で Module 自体を送らないため structured clone の未対応を隠さず、Worker 自身のコンパイル・import callback・Memory・Response streaming を検査する。

## 転送安全性反例への独立再考察

ArrayBuffer の detach を一律禁止すると Memory.grow も壊れ、一律許可すると Wasm の所有メモリを作者が移動でき UAF になる。根拠は WebAssembly JS API の特別な ArrayBufferDetachKey と ECMAScript のキー照合である。したがって、作者転送の拒否と host detach の成功を同じ memory の byte 保存とともに検査する構成へした。transferToFixedLength と structuredClone を迂回手段に残さず、Worker 双方向も別経路として検査する。

失敗する transfer list の先頭に通常 ArrayBuffer を置く否定対照は重要である。Wasm buffer を後から拒否した時に先頭だけ detach してはならず、全要素の preflight が先行する必要がある。また通常 ArrayBuffer の転送と、転送しない Wasm buffer の独立 clone は引き続き通す。これにより、すべての transfer / clone を禁止するだけの修理を成功としない。

Node 実 Worker / MessagePort で追加後21項目が成功したことは、単なる postMessage mock と隔離 VM より強い補助証拠だが、Nocturne の worker / clone / frame preflight と engine フラグの実接続の証明ではない。初回 Nocturne grow 失敗と、ログ filter による boundary 完了行欠落を修理後の成功へ繰り上げない。最終は親担当の更新媒体で boundary41 / Worker29 と基本168を実行する必要がある。
