# 同期 CE parser の identity 再結合（2026-10-12）

実サイトの初期化停止修理中、`html_lexbor_bridge.c::rebind_native` が同期 custom-element constructor ごとに hash table 全域を消去し全 binding を再挿入する確定した二乗化経路を発見した。

製品修理は同じ責務内の一関数。open-addressing の旧 identity を cyclic probe path を保つ backshift deletion で除去し、新 native identity を局所挿入する。binding／Lexbor identity、subscription、dirty publication、reaction 順と DOM 所有を保ち、追加確保も不要。全 forest 再走査を行わない。

根拠：[WHATWG HTML の token からの要素作成](https://html.spec.whatwg.org/multipage/parsing.html#create-an-element-for-the-token)。同期 constructor と実 native identity を維持する最適化であり、constructor を省略する変更ではない。

この修理が今回の YouTube skeleton 停止の唯一原因とは確定していない。性能数値・実サイト成功は未測定。主担当の layout cancel 修理／storage の再走査修理とは独立で、主担当へ固定ソースを通知した。
