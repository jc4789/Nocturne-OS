# HTML IDL／live collection の通信・走査修理

主担当の実 YouTube `media-layout-video-controls-20261012/serial.log` では Timer native profile に `213109 calls / 4120ms`、別 Timer に `1257ms / layout1002ms / 21flushes / DOMrev358` がある。これらは重い native 境界の実証であり、以下の二経路だけが全時間の原因とは断定していない。

- `js_bootstrap.js::htmlElementBrand` の各 IDL 呼出しで繰り返していた nodeType／namespaceURI／localName の三 native call を、主担当が追加した private `get(htmlElementBrand)` の一 call に統合。native unwrap・HTML namespace と寿命を保ち、JS 側の永続 brand cache で権限を飛ばさない。
- `js_collections.js` の HTMLCollection `ownKeys` と各 named descriptor／namedItem の反復全走査を修理。DOM reader の既存 owner／uint64 revision snapshot が同じ間、id／HTML name → tree-order 最初の要素を Map で共有する。owner／revision が変わると snapshot と index を交換し、旧世代は保持しない。ownKeys の重複排除は Set で線形化する。
- generic form／option／control state reader は DOM revision なしで変わるため named cache を適用しない。native読取りの例外は握り潰さず、新 index は完了時だけ公開する。native DOM、CE/MO反応、全体の dirty 更新は省略していない。

根拠：[WHATWG DOM HTMLCollection](https://dom.spec.whatwg.org/#interface-htmlcollection) の live tree-order と namedItem／supported names の規則。

今回の製品変更後、既存 `tests/js_collections_hosttest.js` を一度実行して `274 checks, 0 failed; native snapshot reads 18, version checks 4355`。2048 要素の全 ownKeys と全 descriptor の id／name 読取りは 4096 回で、一つの線形 snapshot に限られた。owner交換、属性変更、同じ backing array の generic control 変更、反復読取り失敗も対象。fixture の旧 rawDom 関数へ現行 typed `.get` 契約を接続した。

これは Node 上で実製品 bridge を評価する支持確認であり、native guest／実サイトの速度・GUI応答・正しさの受入ではない。主担当の次回最新媒体で実 Timer native call／時間を比較する。
