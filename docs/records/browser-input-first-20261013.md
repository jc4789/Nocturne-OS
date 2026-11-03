# ロード中入力の製品修理・第一報

最新ユーザーの性能回帰報告を優先し、Window/CSSOM追加は凍結した。
`browser.c:host_script_checkpoint` が wheel を全量FIFOへ保存し、作者task終了後に
通常handlerが既定スクロールを逐次実行することを実ソースで確認した。

既存EventTargetの add/remove/once/abort/inline と native wrapper finalizer に
非passive wheelの取消可否だけをmirrorする。DOM Standardの既定passive値は
Window/Document/documentElement/bodyへ、未指定時だけ適用。明示falseは保全。
`doc.c` の既存scroll整合判定を使い、C checkpoint/active layout/dirty frameでは
paint・layout・作者再入をしない。純JS checkpointと明示snapshot準備済みnative wait
だけ、取消阻害listenerがない入力を即適用し、原target・文書/stream世代を持つ通知を
後から一度だけ送る。後段で同じ既定scrollを再生しない。
FIFOはhead索引と処理済scan位置で反復memmove/再走査を除去し、次timerより入力を先に扱う。

確認: browser/doc/js/jstestのfreestanding clang syntax exit0、正規js_embed再生成、
Event内部状態host14/Worker14成功、CRLF明示diff-check exit0。
新native `jstest input-snapshot`、原WPT `dom/events/passive-by-default.html`（100case）
は実行待ち。wheel取消原WPTは実TestDriver wheel操作を要し未実行。

主担当の旧媒体 `compat-baike-perf-before-20261013/page-full.png` と `page-down.png` を
担当自身も閲覧。後者は下部カード・丸画像・本文が見えている。旧版の表示資料であり、
今回の入力source成功やロード中応答速度の証拠へ昇格しない。
主担当が新ABI完全媒体と同条件の単独百度VMでロード中入力・上下移動を受入する。
