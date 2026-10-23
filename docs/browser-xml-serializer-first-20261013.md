# XMLSerializer 共通 DOM 修理：第一報

## 確定した実製品境界

主担当が取得した実 Reddit の `build/nocturne-platform/compat-public-source-reddit-20261013/serial.log` 307 行で、公開作者 bundle の `xr` は `new XMLSerializer().serializeToString(e)` を呼んでいた。直前の caught TypeError は `not a function`。現製品には serializer が無かった。左欄資源は HTTP 200・正 MIME で返っており、「IntersectionObserver が未起動だから左欄が空」とする旧仮説は撤回済み。公開 bundle の限定 excerpt 以外の私的 URL・Cookie は保存していない。

## 既存責務へ統合した製品修理

- `user/libc/web/js_document.js`：XMLSerializer constructor、receiver/native Node brand、必須引数、標準 method descriptor、interface tag。作者 getter を通さず既存 private DOM binding を使う。
- `user/libc/web/js.c`：既存 native getter に XML serialization 専用接続。通常 Node unwrap・世代・寿命の経路を使い、新 opcode や別 DOM を作らない。主担当の CSSOM 区間は不編集。
- `user/libc/web/html_serialize.c/h`：HTML serializer とは別の XML algorithm。namespace の既定値・prefix scope・属性 namespace・生成 prefix、Document/Fragment/Element/Text/Attr/Comment/PI/doctype、template の inert content と shadow 非公開を扱う。
- XML の出力と namespace/frame storage は一時 heap。OOM・取消で出力を rollback し scope/frame を解放。native 深さに小さい固定上限を追加せず、非再帰の growable frame を使う。
- 既存 HTML innerHTML/outerHTML と raw script/void behavior は変更しない。製品にサイト名や作者 DOM 構造による分岐を入れていない。

[HTML Standard の XMLSerializer](https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-xmlserializer-interface) に従い公開 API は `require well-formed=false`。[XML serialization algorithm](https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization) の strict 条件は native consumer 用の別引数で扱い、Comment/PI の不正文字列を API で一律拒否しない。

## 支持確認と受入の区別

`tests/check_html_serialization.py` は serializer の実製品本文を host C へ組込み、周辺 tree/allocator/checkpoint のみを fixture 化する。初回は 84 条件、namespace・XML escape・HTML rawtext 保全・20,000 深さ・allocator fault・取消 rollback が失敗 0。独立二回目の追加反例と最終 source は第二報を参照。

`tests/js_document_cases.js` に native JS の `runXMLSerializerCases`、`tests/jstest.c` に `xml-serialization` 選択 mode を追加した。WebIDL brand、跨 document/adoption、native template・shadow、属性 namespace、普通の insertAdjacentHTML との往復を確認するための支持試験であり、実サイト成功を偽装する作者関数差替えではない。

対象 C の freestanding 構文確認、JS 構文確認は成功。native JS mode、正規 embed・全 build・実 Reddit の左欄/次 feed/実 scroll は主担当が統合確認中。この報告時点では未受入。性能比較、他サイト回帰、Hyper-V の成功は主張しない。

## 残る実装範囲

既存 native DOM は HTML/SVG/MathML/null namespace の識別と namespaced Attr を持つが、任意 URI の Element、XML Document parsing、CDATA node kind はまだ無い。今回これらを空クラスや固定値で対応したことにしない。HTML の createCDATASection は従来どおり NotSupportedError。XML parser に依存する WPT 原本全体を成功扱いしない。
