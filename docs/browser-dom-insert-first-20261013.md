# DOM挿入・削除の局所更新：第一報

## 製品修理

- `user/libc/web/dom.c` の既存 `indices` を、変更先頭より前の安定した要素位置から後続だけ更新する形へ変更した。末尾追加で過去の全兄弟を読み直さない。Text/Comment の挿入・削除は要素位置を変えないので再番号付けを呼ばない。要素のdetach時は古い位置を0にし、必要な後続だけを更新する。
- `reset_parser_forms` は、移動部分木の全node走査と解除ごとの登録list再検索をやめた。既にparser associationを持つcontrolの登録listだけを調べ、移動対象の祖先照合または従来の普通tree root不一致を判定して現在のlist linkから解除する。普通leafには関連controlもformも無いので処理しないが、外部のfoster-parented controlを持てるleaf formは除外しない。
- 従来の「元ownerにparser association登録が無ければ何もしない」という入口条件を保全した。その条件内で、従来のshadow/template forest traversalが対象としていたcontrolは、main ownerとtemplate inert ownerの登録から到達する。現在の所有者・parser通知を保つ。
- 主担当の `doc_forest_next`、GC pin付き `web_js_nodes_reparented` と `structure_changed_lifetime`、adopt後の同API呼出しを保全した。共有ABI・binding・生成物には触れていない。サイト名・URL・DOM構造の専用分岐は追加していない。

## 確認の範囲

- `python -X utf8 tests/dom_unbounded_hosttest.py --indices`：製品の番号付け・detach・form reset関数を抽出した20条件、失敗0。
- 10000個の末尾要素追加で、実カウンター `index_visits=19999`。元の先頭から全兄弟走査なら計算上50005000訪問になるが、これは同サイトの時間比較ではない。
- 非要素混在、中間挿入、同parent移動、fragment child転送、末尾削除、外部foster control、shadow/template登録、10000 associationの直接解除を確認した。無関係なparser controlは残る。
- `dom.c` のtarget構文検査、限定差分の空白検査は成功。
- 製品全体build、最新媒体の実サイト、ロード中入力・上下scroll、前後時間比較は主担当が調整する。新しいQEMUは起動していない。host支持だけで性能回帰解消とは報告しない。

## 次

主担当の統合buildと同条件の百度記事観測で、挿入・set処理の時間とGUI応答を確認する。既存DOM・parser・form操作のnative回帰も実行媒体と対応させる。
