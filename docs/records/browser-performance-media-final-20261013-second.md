# 統合第二考察：原 WPT 結果と到達境界

## 実施条件と反証

主担当の標準 server は `127.0.0.1:8766` bind、原 checkout は `D:/Programes/cloned repos/wpt`。第一の準備文書にある `0.0.0.0` とは区別する。guest label は `browser-performance-wpt-guest-20261013`、runner viewport は 1024×768。実結果は `build/browser-performance-wpt-guest-20261013/test-serial.log:65-98` を読んだ。全三 HTML／二 script の受信 hash は第一報の原 hash と一致した。

最初の原本は `const rect = container.getBoundingClientRect()`（原 HTML 46 行）で止まり、続く `assert_greater_than`／`document.elementFromPoint`（49〜52 行）まで到達していない。したがって元 status=1 は保存するが、BVH／clip の回帰または正しさの証拠へ昇格できない。runner の原 status 保存と誤合格防止は、この失敗で実際に機能した。global alias の注入、原本の `getElementById` への差替え、skip はしていない。

## 原 JSON（内容を変えない）

```json
{"name":"Hit testing respects single-axis overflow clipping with negative z-index children","status":1,"message":"'container' is not defined","stack":"    at <anonymous> (http://10.0.2.2:8766/css/cssom-view/elementFromPoint-negative-z-index-inline-overflow-clip.html:3:16)\n    at apply (native)\n    at <anonymous> (http://10.0.2.2:8766/resources/testharness.js:2869:30)\n    at test (http://10.0.2.2:8766/resources/testharness.js:633:34)\n    at <eval> (http://10.0.2.2:8766/css/cssom-view/elementFromPoint-negative-z-index-inline-overflow-clip.html:2:5)\n"}
{"name":"Link should be clickable after hiding a scrollbox with an anonymous table inside","status":0,"message":null,"stack":null}
{"name":"scrollWidth/Height with negative margins: overflow: visible","status":0,"message":null,"stack":null}
{"name":"scrollWidth/Height with negative margins: overflow: hidden","status":0,"message":null,"stack":null}
{"name":"scrollWidth/Height with negative margins: overflow: auto","status":0,"message":null,"stack":null}
{"name":"scrollWidth/Height with negative margins: overflow: clip","status":0,"message":null,"stack":null}
```

三ページの原 harness JSON はそれぞれ `{"status":0,"message":null,"stack":null}`。completion は順に `0 1 1`、`0 1 0`、`0 4 0`。ページ elapsed は 1109／259／276 ms、基盤失敗は全て 0、runner exit status は 1、集計は `限定WPT 3 ページ 6 原ケース 1 失敗 1 ページ失敗`。この elapsed は今回の原本取得・実行時間であり、変更前後の性能比較ではない。

## 追加の静的再考と残件

- 読み取り専用 observer は原 harness を書き換えず、原結果の全六件を受け取った。harness status 0 だけでページ成功にせず、原 status／原ケース数／基盤失敗を分離した。未到達の hit case を、成功した dynamic-anon 一件と negative-margin 四件で相殺しない。
- 最新 `avmedia.c` の caption 二 setter と `doc.c:doc_paint_full`／`webi.h` 宣言を再読した。共通 helper は paint dirty と full damage を owner／frame parent に伝播する。狭い video rect の後に caption が変わる順でも狭い rect へ戻さない。snapshot／readyState／実 audio clock に新しい変更はない。
- この第二考察ではログ／原 source を読むだけであり、製品編集・旧試験再実行・VM 起動はしていない。標準 named globals の不足は元 FAIL として残し、今回の性能修理から全面新機能の実装へ脱線しない。
- 最終 source binding には媒体実装、caption full の material dependency (`user/libc/web/doc.c`／`user/libc/web/webi.h`／`user/libc/web/paint.c`)、限定 runner と原本 hash manifest／実 guest log を含める。実サイトの spinner／controls、初回長停止、実音質／Hyper-V は WPT 五 PASS でも未解決・未確認のまま。
