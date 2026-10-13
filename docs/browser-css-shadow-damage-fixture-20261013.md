# 影damage fixtureの対象分類訂正

元実guest結果は `build/browser-compat-integrated-guest-20261013/test-serial.log:84-86` の44件/1失敗。失敗は「known atomic damage includes shadow ink outside its box」。この失敗を保全し、成功した他43件で相殺しない。

fixtureのdivは `display:inline-block;position:absolute` だった。現在sourceと基準HEAD両方で `css_style_finish` はこの表示をD_BLOCKへblockifyするため、実箱はB_BLOCKである。一方 `doc_paint_damage` はB_ATOMICだけがknown rectで、一般blockの未知descendant inkは安全に全描画へfallbackする。元fixtureが「atomic」と命名していた前提が誤りであり、製品damage経路は変更しない。B_BLOCKまで無条件に部分damageへ拡大すればescaping descendantやblurを欠落させる。

訂正fixtureには本物のreplaced imgをnormal parserで追加し、definite100×40、原位置50,50、shadowを設定する。実box.kind==B_ATOMICを先にassertし、元の部分damage要求は同じ厳しさで維持する。さらに offset10/blur4/spread6 の影を使い、旧2px余白だけでは通らない x<=47/y<=37/right>=173/bottom>=103 を追加。元blockified paneの全damagefallbackも別assertで固定する。一般blockの誤小rectを許さず、影inkを本物atomic経路で確認する。

訂正は `tests/boxshadowtest.c` のみ。native再実行はroot担当で未確認。既存第一/第二報は不変に保ち、この訂正をその後のsource変更として扱う。
