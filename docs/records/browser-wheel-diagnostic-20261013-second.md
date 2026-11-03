# 記事 wheel 未達・独立第二考察

- native wheel が実ページへ届いているか、作者 default が許可されるか、残差が viewport offset へ反映されるかを別の境界として扱う。HMP ack、pointer ack、通信完了、本文描画だけで wheel 成功とはしない。
- 全子 frame の readiness を parent fastpath の条件にする既存設計は性能上の候補だが、通常 dispatcher の root fallback があるため今回の永久位置不変の確定原因ではない。未検証の変更で listener cancellation を迂回しない。
- EventTarget の通常 listener は戻り値 false を無視している現行コードを確認した。legacy event handler の false は取消しになる別契約であり、それを一般 callback の不具合と誤認して修理しない。
- 診断用 target tag と cancellation count は作者 callback 前に数値だけ取得する。callback が node を外しても、診断で callback 後の古い target を辿らない。作者 callback 本文やサイト専用判定を入れない。
- geometry helper の支持反例は inner cache 不在でも committed post を維持、dirty/new box/font 世代/viewport/寸法/content shift/recovery を拒否する。元条件の期待は保全し、host の条件支持と native/実サイト結果を区別する。
- 最新媒体の数値結果から cancellation、extent、offset 復元のどれが壊れるかを決め、次の製品修理へ進む。今は診断の追加であり、wheel 回帰の修理完了ではない。
