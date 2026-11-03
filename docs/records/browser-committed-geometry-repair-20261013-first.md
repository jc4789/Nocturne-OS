# committed geometry と内部 layout cache の区別・第一報

- 原 17 条件の 1 失敗を保全：`build/browser-xml-geometry-tls-native-20261013/test-serial.log`。期待を変更せず、原試験に before/mutated/after の状態診断だけを加えた。
- 診断 `build/browser-pseudo-geometry-diagnostic-20261013/test-serial.log` を担当自身も読んだ。変更前 root は revision=1、valid=1、dirty=0、post=1、cached/current font=3、viewport=800×600、post/実寸法一致だが layout_cache_valid=0。class 更新後も root と各値は同じ。無条件 layout を通ると revision=2 と cache=1 になった。他 16 条件は成功した。
- 根本境界：layout_cache_valid は入力キーに沿って内部 formatting の再実行を省略できるかを表す。公開済み post-layout geometry 自体の正否とは別である。初期 font 世代変化等で内部 cache が保守的に不許可でも、完了済み post pass と最終 font 世代は有効に公開され得る。
- 製品修理：geometry 再使用条件から内部 layout_cache_valid だけを外した。post_valid、root dirty なし、現在の最終 font 世代一致、post 寸法一致、viewport 同一、recovery 中でない条件は全て維持した。layout/body dirty を強制消去する処理ではない。
- 原 17 条件の期待と診断は保持。target syntax と diff check 成功。まだ修理後媒体で 17 条件を実行しておらず、主担当確認を待つ。未変更 host の再実行はしない。
- 今回 source 変更は既存 doc.c の条件一箇所と説明だけ。freeze 済み。原 paint 12、CSSOM 経路や実 Reddit の成功は前段の支持結果であり、今回 geometry の最新受入に置き換えない。
