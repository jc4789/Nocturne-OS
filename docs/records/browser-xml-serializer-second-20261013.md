# XMLSerializer 共通 DOM 修理：独立第二考察

第一報の「存在しない API の追加だけで十分」を前提にせず、namespace identity・取消の所有者・出力寿命を再検討した。source は第二考察完了後に凍結し、主担当へ統合を依頼した。

## 反証で追加修理した境界

1. **空 prefix の誤用**：`xmlns:p=""` を null namespace の子 Element の別名に使うと、本来必要な `xmlns=""` を出さず名前空間を誤る。空の prefix binding は preferred alias から除外し、既定 namespace を正しく reset。strict では prefixed empty namespace を拒否する。
2. **省略した XML namespace 宣言の継承**：予約 XML namespace の宣言は出力で省略するので、これを実際に出力した default namespace として継承しない。
3. **strict の不正値**：no-namespace の `xmlns` 属性、および長さ付き public doctype identifier 内の NUL を拒否。strlen だけで後半を無視しない。公開 API の false flag と native strict を区別する。
4. **inert/template の取消所有者**：DOMParser が作る inert owner は JS host を持たない。node owner だけを checkpoint しては caller の入力取消へ届かない。native node の owner を変更せず dom_family を checkpoint owner に選ぶ。既存の 64 回ごとの低頻度 native checkpoint が通信/媒体/入力をサービスするため、毎文字の高価な syscall を追加しない。

## 最終支持結果

- `python -X utf8 tests/check_html_serialization.py`：**93 条件・失敗 0**。
- namespace scope、default reset、author/生成 prefix の衝突回避、Attr NS、template/shadow、sized doctype、false/strict の Comment/PI 差、20,000 深さの非再帰 storage、全 allocator fault の scope 解放、取消 rollback、inert family checkpoint を実製品 serializer で確認。
- 既存 HTML raw script と HTML void serialization の期待は不変更。
- serializer/js.c の freestanding target 構文、JS 構文、CRLF を正しく許容した対象限定差分検査は成功。
- 最終 `user/libc/web/html_serialize.c` SHA256：`83e42cdca3257d3da2fa77238238a22de72a9caf7c658e2db4fe3cf58f3328aa`。
- 詳細支持ログ：`build/xml-serialization-host/result.log`（371 byte）。生成 host C（40,995 byte）と実行媒体（69,676 byte）も同ディレクトリにのみ置いた。QEMU boot/data コピーは作っていない。

## 規格・原期待の扱い

[DOM Parsing の namespace serialization](https://w3c.github.io/DOM-Parsing/#dfn-xml-serialization) と local WPT 原本 `D:/Programes/cloned repos/wpt/domparsing/XMLSerializer-serializeToString.html` を読んだ。原 WPT には、既存 `ns1` と衝突する生成 prefix、祖先 alias を一致する default declaration より優先するなど、本文にも歴史的 issue として記載された期待がある。現在の namespace round-trip を壊して古い期待へ退行させず、原本や期待を改変しない。原 WPT 全成功という報告もしない。

## まだ証明していないこと

native JS mode の結果と最新媒体の実 Reddit 左欄/次 feed/スクロールは主担当の受入範囲。API 欠落という実観測原因を修理したが、sidebar が既に表示された、ページ全体が速くなった、他サイト無回帰とはまだ言わない。任意 namespace Element、XML parsing、CDATA 自体の未対応は第一報どおり残る。既存寿命・世代契約を迂回する JS DOM は作っていない。
