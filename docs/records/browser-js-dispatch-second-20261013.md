# 動的 script 検出の共通修理：独立第二考察

## 単なる局所走査では足りない反例

第一候補の script A が同じ親へ C を追加し、外側 fragment には未処理の B がある場合、旧「全 script 履歴から ready を実行」は内側 C の挿入処理で B まで実行し、A の処理へ不要な再入を起こせる。新しい候補自身だけを prepare／実行する方式では、内側 C が終わってから外側 B に戻る。host `ACB` 条件と native 作者 JS の同条件で境界を固定した。

さらに「候補をすべて prepare してから inline を実行」では、A が B を切断しても B を先に準備済みにしてしまう。静的候補一覧は保ちつつ、各候補の現在の接続を準備直前に確認し、classic inline はその場で実行する。A が B を切断した場合は未準備で残り、後に B を本当に接続すれば一度だけ実行する。

この順序は [DOM の insert と post-connection](https://dom.spec.whatwg.org/#concept-node-insert)、[HTML の script 準備](https://html.spec.whatwg.org/multipage/scripting.html#prepare-the-script-element) に基づく共通の接続境界であり、サイト名・サイト DOM による分岐はない。

## 世代・所有と no-op

- 準備ポリシー、inline 作者 JS、frame 初期化は文書入力世代を変え得る。各候補前と inline 実行前に世代と disabled を確認し、古い sibling の準備を止める。準備中 reset と実行中 reset の別反例を host で確認。
- 一覧はネイティブ node ポインターを保存するが、別 DOM を作らない。移動／接続の wrapper pin と逆順 publication は主担当の既存 `js_nodes.h`／DOM 所有層を使う。実 QuickJS GC／CE／Range を含む寿命受入は host fixture から証明できず、主担当 native `domsubtree` と実サイト確認が必要。
- 最初の候補捕捉には fragment の消費前という境界が必要。OOM／取消では接続前に例外を返し、追加候補領域を解放する。8 個からの grow と失敗時の確保／解放の釣り合いを確認した。
- 凍結前の再考察で空 fragment を Text 挿入と同一扱いする no-op 不備を発見し、実ノード有無 `changed` を保存して修理した。追加 host 反例を含む最終 19 条件が成功。null replace-all の削除だけから新しい script を準備しない。
- 非再帰 walk は deep forest の stack 消費を増やさず、一定件数ごとの既存 native checkpoint を通す。毎要素の高価な時計読み出しは追加していない。

## 残る限界

- `dom_revision` は細粒度の structural revision ではない。style／text mutation で revision が変われば fallback が一度走る。全変更を「構造に無関係」と決めつけて省いてはいない。
- `script_visits` は新方式の実走査 node 数（挿入 root と text も含む）。`script_ms` は文書 fallback の捕捉／準備時間であり、挿入ごとの prepare／inline 実行時間は DOM native／task 時間側に属する。以前の親全走査時間と同じ内訳ではない。
- 実サイトでの CSS HTTP 0、layout／draw の反復、長い posted task の残りは別境界。今回の 19 host 条件、構文確認、仕事量の数学だけでは、それらの解消・全体高速化・GUI 無回帰を主張できない。
- 新しい Worker や別 DOM 管理層、作者 script 差替え、サイト専用例外、期限の無制限化は追加していない。必要だったのは現在の同期接続処理の仕事範囲と再入順序の修理。

製品は第一報の hash で凍結。次は主担当の同媒体 native と同条件実サイト比較で、この限定修理の効果と回帰を区別する。
