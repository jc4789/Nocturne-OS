# placeholder 共通修理・第一報

- 対象：検索欄の native placeholder と作者 label の二重表示。実画像 `build/nocturne-platform/compat-dom-dispatch-net-reddit-20261013/batch-current.png` を担当自身も見て二重表示を確認した。
- 共通の不足をソースで確認：`::placeholder` は `PE_OTHER` として rule index から捨てられ、入力 paint は作者スタイルを読まず灰色固定だった。実サイトの作者宣言がこの不足だけで二重表示を起こしたかは、修理後の実サイトで確認する必要がある。
- 製品修理：既存 selector/cascade に `PE_PLACEHOLDER` を追加。作者宣言が実際に一致した text input/textarea だけ、既存 node-owned style slot に疑似スタイルを算出する。非継承 pointer を同じ世代で回収する。paint は cached font/color/opacity を使い、透明な hint の glyph 作業を省略する。サイト名、特定 DOM、作者 CSS の差し替えはない。
- 性能境界：paint ごとの CSS 走査や新しい style 確保はない。通常スタイルの pointer 追加は 8 bytes。作者疑似宣言のない control は追加 style slot を持たない。
- 支持確認：target syntax と diff check 成功。`tests/check_cssperformance.py --placeholder` の初回は parser・非継承・native色計算の 11 条件で失敗 0。style 世代寿命の 3 条件を追加した最終実行は 14 条件・失敗 0（`build/browser-placeholder-host-20261013.log`）。新条件追加による再実行であり、未変更検査の反復ではない。
- 実サイト受入・最新統合媒体の入力/スクロール・新 native pixel 条件は主担当が担当し、この第一報では成功としない。
- 根拠：CSS Pseudo-Elements 4 §4.3 は input/textarea hint を疑似要素として扱い、§2.1.2 の font/color/opacity を適用する。https://drafts.csswg.org/css-pseudo-4/#placeholder-pseudo

変更配置：`user/libc/web/css.c`、`cssprop.c`、`paint.c`、承認済 `webi.h`。支持試験は既存 `tests/cssperformancetest.c`、`check_cssperformance.py`、`paintperformancetest.c`。
