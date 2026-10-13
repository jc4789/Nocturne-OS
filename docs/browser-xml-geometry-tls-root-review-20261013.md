# XML・style-only geometry・TLS回帰の統合確認

## 独立したソース確認

- 実Redditの公開Source25:19284にXMLSerializer.serializeToString呼び出しがあり、未実装APIで挿入前に停止していた。主担当が実原画の左欄空白と公開source excerptを確認した。HTML serializerの代用ではなく既存html_serialize.c/hにXML専用処理を統合し、既存js_document.js/native_getから接続。namespace、属性、Fragment、template、doctype、HTML rawtext保全、OOM/取消rollbackの実装を独立して読んだ。XML parser/CDATA作成は別の未対応範囲であり、全XML対応とはしない。
- 新XML支持関数は53条件と数え、native集約側も53 exactへ強化。従来documents群を消さず併走する。
- 非生成before/afterの計算済みbodyを保持した版は、原paint12と疑似CSSOM原24＋20経路がnative成功。しかし新12条件のgeometry世代維持が失敗。担当がdoc.cでcascade後の確定geometry再使用を修理した。主担当はpost/cache/font/viewport/dirty条件、通常commit出口、paint/hit別世代と画像intrinsic invalidatorを確認した。原失敗期待は維持、5反例を追加して17条件にした。最新nativeはこれから確認する。
- 単独百度の新位相では、homeのTCP3.296秒＋TLS16.705秒、TCP15.278秒＋TLS4.722秒が新共有20秒予算で失敗した。従来成功取得も拒否する回帰として扱い、TCPとTLS予算を元caller契約へ分離。TLS全体は一操作の絶対期限であり、recordごとの延長には戻さない。既存HTTPの長いcaller予算を新規容量policyと呼ばない。debugのcallback内wallと外wallを分け、外を暗号CPU時間とは呼ばない。
- TLS担当は容量エラーで最終報告を返せなかったため、主担当が残った製品差分を引き継ぎ、TLS/HTTP/private metadataを読んで構文確認した。既存生成済み支持実行物の再実行は43/11/12条件で失敗0だが、実TLS認証・実サイト速度の代替にしない。private行の数値検証、旧形式互換、native callback前除去も確認。

## 実画面と残る受入

最新単独百度のphase-early/phase-homeを主担当が見た。記事はantispamのTCP20秒失敗、homeは上の共有期限失敗により白のまま。HTTP状態やdeadlineで早く失敗したことを高速化・表示成功としない。IPv4のhost HEAD対照はTCP3.466秒、TLS確立9.936秒、header10.686秒で200。hostは別経路なのでNocturne性能の証拠ではない。

XML・geometry・TLS最終凍結を正規embed/allへ統合し、IMG/VHD/VHDX/ISO更新後に原native失敗と実Reddit・単独百度を確認する。GitHub/DDG/HTML5testの先行成功を新sourceの回帰なしへ流用しない。

仕様：XMLSerializerの現行interfaceは https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#the-xmlserializer-interface 、serialization algorithmは https://w3c.github.io/DOM-Parsing/#xml-serialization 。疑似計算値は https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle 。
