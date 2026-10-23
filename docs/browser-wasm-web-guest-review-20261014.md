# Wasm Web 担当：修理後ゲスト結果の独立確認

2026-10-14、`build/wasm-repaired-native-20261014/test-serial.log` と `test-metadata.json` を担当自身が読んだ。新しい製品変更・試験は実行していない。

- serial 134–147行: 基本 Wasm **168**、native 境界 **41**、実 HTTP Worker **29**。各完了行と各 jstest の `0 failed` を確認した。
- serial 148–176行: DOM/events/observers、network stream、Fetch **244**、XHR **89**、Worker events、policy inheritance の成功を確認。script lifecycle と storage windows も含み、177行は **11群成功・失敗0・skip0**。
- metadata: WHPX、4096MiB、4CPU、実解像度1920×1080、ユーザーdata未接続、終了code0、timeoutなし、試験VM停止済み。headless試験であり、実サイトGUIの受入ではない。
- 使用媒体の metadata: `nocturne.img` SHA-256 `569d2d623ead1dfc16954692edfda18a0385b82a70210b70ff404f51b196a05c`、kernel `5b89bb9f238adc5441f43a34f116fbeeece09ed7d1ca977d0e62931ce60e63fb`、initrd `a0ee472681223652330d4fe0347bf7b3541d8e80a491a4553423012e743b0493`。

これは初回の grow 返値失敗と boundary 完了ログ欠落を保存した過去の記録を変更せず、修理後の別実行として追記する。host Node の168件や Worker補助検査と混同しない。指定三実サイトの最大化・console・実操作、性能、動画音声、Hyper-V実機、配布媒体の最終一致は本ログから認定しない。

製品 facade と tests の変更はここでは行わない。最終 map 同期後に担当自身の map view / source receipts を更新する。
