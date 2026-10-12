# Nocturne の Web フォント復号

FreeType 2.14.3（上流タグ `VER-2-14-3`）をメモリ専用で使用する。
取得元: https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz
ライセンスは FreeType Project License (`docs/FTL.TXT`) を選択する。

コンパイル対象は Makefile の `FT_C` に列挙した SFNT / TrueType / CFF と
通常のグレースケールラスタライザのみ。ホストファイルシステム、CLI、
プラットフォームフォント列挙、追加形式ドライバは組み込まない。
`ports/freetype` の構成ヘッダと `user/libc/third_party_ttf.c` のメモリ割当境界を使う。
WOFF は同梱 zlib、WOFF2 は同梱 Brotli の限定 decoder を使用する。
展開した TrueType は既存の厳密な検証と stb 描画経路へ戻し、既存の描画を保つ。
CFF は FreeType の境界検査されたメモリ face を寿命中保持する。

上流からの局所修正: `src/sfnt/sfwoff2.c` の `reconstruct_glyf` では、
呼出元がローカルコピーへだけ設定した `dst_offset` を共有 `glyf_table` から
誤って読むことがあり、終端 `loca` が相対長でなく SFNT 内の絶対位置になる。
`glyf_start` を共有記録へ設定し、実際の再構築長を出力ディレクトリへ反映する。
厳密な既存 SFNT gate を緩めずに、変換付き WOFF2 を正常に復元する。

根拠と限定確認:
- `FT_Load_Sfnt_Table(face, 0, 0, ...)` は公式 API と `tt_face_load_any` の
  tag 0 分岐が定義する「メモリ stream 全体」のコピーである。
  https://freetype.org/freetype2/docs/reference/ft2-truetype_tables.html#ft_load_sfnt_table
- 同梱 Inter-Regular.ttf を FontTools の正規 WOFF2 CLI で変換した入力で、
  修理前の FreeType 出力は glyf 開始 139312、長さ 213740、終端 loca 353052。
  353052 = 139312 + 213740 であり、最後だけ絶対位置になっていた。
- 同一入力の FontTools 独立復号は全 17 table を正常に解析し、glyf 長と
  終端 loca がともに 210512。decoder により outline の符号化・長さは変わりうる。
  W3C WOFF2 の §4.1 / §5.3 に従い、元の origLength を正解値として強制しない。
  https://www.w3.org/TR/WOFF2/#glyf_table_format
- 局所修理後の製品 `font_open_memory` による native 確認は 30 項目失敗 0。
  TrueType の raw / WOFF / WOFF2 と EOT の実 raster が一致し、CFF の raw / WOFF2
  も一致。切断入力と未対応 MicroType EOT を拒否する。実サイト受入は別。
