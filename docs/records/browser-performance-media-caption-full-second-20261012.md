# 媒体 caption 全体damage接続 第二考察

- 第一報の二 callsite と `doc_paint_full` の現定義を読むだけで再考した。既存 video damage valid の前後どちらでも full flag が true となるため、`web_take_paint_damage` の `!paint_damage_full` 条件を満たさず、狭い動画 rect を返さない。helper は既存 rect を消して成功を偽装するのではなく、未知変更を full 安全fallback と分類する。
- rich cue の完全一致 early return と plain text/既存richの変更判定を維持しているため、不変captionで新しい全体 repaint を毎frame増やさない。変更したcaptionの revision と snapshot 無効化は従来通りで、新しい表示と古いoverlayの所有者を混ぜない。
- frame 子で更新した場合も親 composite の全体更新へ伝播する。helper は描画・layout・JS callback・割当・待機を行わず、native pump の寿命や clock 条件を変更しない。
- parent の mixed-damage / nested scroll の新guestに対して、既存19媒体bindingのうち変更は `avmedia.c` のみ。他の媒体源と限定host証拠は保存し、古い無変更試験を再実行しない。helperを所有する `doc.c` / `webi.h` の他の編集は親/layout担当のreviewで確認する。
- 主担当が観測したspinner残留は未解決、原因未確定。基準との差異とF12後の消失、実audio input-starvationの回復、最終372333msのEOSは区別して扱う。dirtyだけで動画snapshotを止めたり、readyState固定/架空clockを導入したりはしていない。
