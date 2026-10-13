# 媒体 caption 全体damage接続 第一報

- 親の新反例は既知 video rect damage の後に未分類の表示変更を通知すると、rect の有効フラグが残って狭い更新へ誤分類されるというもの。媒体側では rich cue 更新と plain caption 更新の二通知を対象にした。
- `user/libc/web/avmedia.c` の二箇所のみ、`d->paint_dirty=true` を共同 `doc_paint_full(d)` へ置換。caption storage、revision、変換時の文書/世代ガード、変更なしの early return、`snapshot.valid=false` は維持。
- helper は `user/libc/web/doc.c` の `doc_paint_full` と `webi.h` 宣言を読むことで確認した。owner と各 `frame_parent` に `paint_dirty=true` / `paint_damage_full=true` を設定する。
- `avmedia.c` 一個のみ正規 native clang flags の `-fsyntax-only` を実行し exit0 / warningなし。owned diff check exit0。前の境界試験、guest、VM、全buildは実行していない。
- MSE ready、時計、共有転送、snapshot publication、旧coherent sceneの保持は変更していない。spinnerの原因を特定・修理したという成果ではない。
- 新 mixed-damage guest は親が他の producer と統合して確認する。本書時点ではこの二通知の実guest効果は未確認。
