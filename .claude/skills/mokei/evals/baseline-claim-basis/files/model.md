# 目的（2026-10-26、ユーザーの言葉）
- 「our browsers performance Is not the best, and is fairly slow over all. and there are an undetermined amount of regressions in layout and performance, do to a lack of proper regression control, and a lack of proper ledger usage over the last 16 hours」
- 「investigate, with the help of subagents, and implement proper fixes（refactoring, adding missing features or completing current feature implementation）」
- 「use D:\Programes\cloned repos\wpt … if we fail a test that means our browser is in the wrong, not the test」
# 形
- 16 時間＝10/25 12:06 以降：コミット 17（47bb78fb〜d0737a57）＋未コミット（AP の events park・YIELD・SLEEP、realloc の縮み、http/http2/tls の錠と待ち、js_memory など）。基準＝b2f5b721
- 物差し：browser-hikaku の組（TEST SITES.txt の 7＋WebGL）と Floorp の参照画像・性能記録。WPT は http.server 8766 と tests/wptperformancetest.c の限定 runner
- 台帳：docs/browser/LEDGER.md を作業の正本にする（変更・退行・確かめ・未確認）。散らばった docs を増やさない
# 今の説明
- 報告された退行三つ（どれも原因未確定）：見出しの字体と大きさ（再現 YouTube）、部品が組み上がらず取得が受け取りの記憶不足・Worker 応答切れで失敗（再現 Reddit）、WebGL 7→2 fps（再現 WebGL City、Hyper-V）
- 一番：16 時間の変更のどれかが共通の層を壊した。覆る観測：基準 b2f5b721 の媒体でも同じ症状
- 既知の欠陥：未コミットの events park に、自己デッドロック・閉じた待ち行列で永久に寝る・wait_until が残る（監査で確定）
# ありか
- 我々の変更から：events.c/sched.c/object.c（AP park）、malloc.c（realloc）、http2.c/tls.c、js_* の native 化（URL・UTF-8・Blob・btoa・Intl）、style/layout の部分再計算、33e5203b の JS 計測、c15925ef の描画の拍
# 次の一手
- 基準 b2f5b721 を別の作業木でビルド（背景）し、組を基準と今で撮り比べる。並行してサブエージェントで 16 時間の差分を層ごとに洗い、欠陥と WPT の該当を出させる
