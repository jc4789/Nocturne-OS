# placeholder selector 値衝突・修理第一報

- 原 native 失敗を保全：`build/browser-tls-placeholder-native-20261013/test-serial.log` の `paintperformance --placeholder` は 12 条件中 7 失敗。CSSOM は作者疑似色ではなく UA 灰色になった。初回 host 14 成功を実製品成功に昇格しない。
- 原因は製品ソースで確定：初回追加した `PE_PLACEHOLDER` は値 5 だったが、`css.c` の内部 `#define PE_SLOTTED 5` と衝突した。rule collection が疑似宣言を assigned-element 宣言として `PE_NONE` に変換し、control 本体へ色・opacity・font を誤適用した。
- 修理：既存 `PE_NONE`～`PE_BACKDROP` の値 0～4 を維持。既存 slotted marker の 5 を共有 `webi.h` の enum へ統合し、新 placeholder は 6 に分離。css.c の別定義を除去し、非衝突を compile-time と host 条件で確認する。
- 初回 host の不足：parser は疑似 kind を保持するか、paint helper と slot 回収は動くかを検査したが、collection に入る slotted marker と比較していなかった。この不足を追加条件で補った。既存の期待値は弱めていない。
- 最新支持：target syntax、diff check 成功。`build/browser-placeholder-shared-kind-host-20261013.log` は 15 条件、失敗 0。原 native 12 条件と主担当追加 CSSOM 20 条件の実媒体再確認は主担当が担当する。
- `.d` で css.o が `webi.h` に依存することも確認した。今回の作者色欠落は値衝突で説明でき、根拠なく媒体不一致だけを原因にはしない。
- ソースを凍結済み。主担当全 build と実サイト再確認を待つ。修理後の native・実サイト成功はこの報告では未主張。
