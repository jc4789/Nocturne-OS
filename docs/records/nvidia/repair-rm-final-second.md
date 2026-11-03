# RM最終整合・新しい第二検討（2026-10-20）

新第一報を「host成功が統合成功を保証する」という反例で再検討した。

- 統合で実際に生じたheader不足は製品側の直接includeで修理し、fixtureへ定数を足して隠していない。
- 7TUを別々にtarget構文確認することで、他の `.c` の推移includeを借りるfixtureの穴を分離した。ただし構文確認は正規linkの代わりではなく、linkは親の `integration-build-linked.log`、梱包は `integration-build-pack.log` へ根拠を分ける。
- QEMU eduのRM拒否・claim再取得は `after-serial.log` の `nvrmtest: 0 failed` まで。NVIDIA実装置の認可、DMA、cache、GSP、複数GPU実行へ昇格しない。
- 旧48byte／Protocol1／default aliasは維持するが、旧clientがRO viewをRWとしてmapする場合は安全に失敗する。RO権利を緩める互換処理は入れていない。
- 同じ `after-serial.log` に残るvirgl末端画素の失敗はRM受入とは別で、親の修理後再検証を待つ。全体を完了・無回帰とは報告しない。

結論：RM担当の追加include修理、独立TU確認、host3群、親の正規link／梱包、実QEMU非対応装置拒否は、それぞれの範囲で根拠が揃った。実NVIDIAと親の末端画素修理の最終結果は未確認。今回は記録整合のみで、再試験・build・repo fingerprint走査は追加していない。
