# Shift_JIS の復号表

WHATWG 公式 Encoding Standard の `index-jis0208.txt` を未改変で保存し、
`generate_jis0208.py` が decoder 用の密な 16-bit 配列へ変換する。元の pointer と
Unicode scalar を変更せず、未登録 pointer だけを 0 とする。製品での表は 22208 byte。

取得元の固定 commit、URL、SHA-256 は `source.json`、公式ライセンス全文は
`LICENSE-WHATWG.txt` に保存した。WHATWG のソースコードへの組み込み部分には
BSD 3-Clause License が適用される。生成物にも出典と著作権表示を保持する。

```text
python -X utf8 user/libc/web/encoding/generate_jis0208.py
python -X utf8 user/libc/web/encoding/generate_jis0208.py --check
```

Shift_JIS の正式 decoder、ASCII エラー後の byte 復帰、Windows の EUDC 私用領域は
[Encoding Standard](https://encoding.spec.whatwg.org/#shift_jis-decoder) に従う。
JIS0208 表は CP932 / Windows-31J 拡張も含むが、Python cp932 と WHATWG の全byteの
処理が同じであるとは仮定しない。例えば単独 0xA0 / 0xFD–0xFF は復号エラーになる。

decoder は allocation-free の scalar callback 方式で、chunk 間の先行 byte を保持し、
error replacement と通常 scalar を区別する。HTML は復号後に改行と NUL を正規化する。
今は JavaScript の TextDecoder へ接続しておらず、その対応成功を主張しない。
