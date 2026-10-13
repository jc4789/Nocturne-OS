# フォント取得・形式選択の第一報

## 範囲と実装修理

前回性能実装 `0519429` を保全。担当はフォント容器と資源選択。`css_compat` と合意して `user/libc/web/css.c` の `font_source_hints` と `parse_font_face` の `src` 部分だけを変更した。容器・描画実装 `user/libc/font.c` と通信 `user/libc/webnet.c` は未変更。

- `embedded-opentype` の完全対応を誤って広告していた。実装は EOT-Lite/XOR の生 SFNT 包装には対応するが、MicroType Express 圧縮には対応しない。ヒント付きの非対応項目をダウンロード前に除外し、作者順の次の WOFF2/WOFF/TTF/OTF 項目へ進む。ヒントなし資源は引き続き実バイトの容器判定へ渡す。
- 全項目が非対応／解析失敗の `src` 宣言で、先行する有効な `src` を NULL に上書きしていた。有効項目のないリストを解析失敗として無視し、先行宣言を保つ。
- 根拠は [CSS Fonts 4 §4.3.1/4.3.3](https://www.w3.org/TR/css-fonts-4/#src-desc)。非対応形式／技術項目を除外し、支持項目なしなら宣言は解析エラーという契約。サイト名による製品分岐、UA 偽装、フォント警告の握り潰しは入れていない。

## 原資源の確認

ホストの通常 HTTPS 取得で、実サイト HTML とそこが参照する原 CSS／フォントを保存した。ページの DOM、CSS、関数を注入・修正した受入ではない。

| ファイル（build 配下） | バイト | SHA256 |
|---|---:|---|
| compat-ddg-20261013.html | 20162 | a6d12cac9bfa2868de6584cf0337bd829da26b3861956f64aca18a6e926d5cfd |
| compat-ddg-s-20261013.css | 171882 | b2c8a1a1abb7e0fc856e15b29d80ffea3454e76c39a2ce005a170252540a3d5c |
| compat-ddg-font-20261013.eot | 135409 | 2e1fc2b0892db86807d65e6bbf6ee3897a1e1c5a03f83d2a6a1a21ab74d2df7e |
| compat-ddg-font-20261013.woff2 | 109116 | 3642a61fbc63ad01701f7129c9bb2fb2f75736ace5f48372be62d12cfd6a5f80 |

原 CSS の DuckSans 宣言は EOT → WOFF2 → WOFF → TTF。EOT の総長 135409、FontData 長 135261、version `0x00020002`、flags `0x4` は MicroType Express 圧縮である。`font_open_memory` の `flags & 0x24` 拒否と一致する。HTTP 200 は復号成功ではない。EOT 容器情報は [EOT 仕様提出文書](https://www.w3.org/submissions/EOT/) と照合した。

## 限定検証と失敗の保全

- `python -X utf8 tests/check_cssperformance.py --fonts`：製品の完全な該当 C 関数を抽出して **26 checks / 0 failures**。作者順、非対応ヒント／技術、誤った format、後続 invalid src の保全を確認。`build/compat-font-sources-20261013.log`。
- `python -X utf8 scripts/build.py --log build/compat-font-container-build-20261013.log build/font-container-native.exe`：限定ホストデコーダ試験対象だけのコンパイル成功。OS 全体ビルド／VM 起動ではない。
- 新 `--font` モードで原 WOFF2：**4 checks / 0 failures**。primary A、実描画 ink、描画 advance と計測の一致。raster `0904ac21`。CSS 選択と実サイト受入とは別。
- 原圧縮 EOT の同モード：**1 check / 1 failure**、復号拒否を `build/compat-font-eot-rejection-20261013.log` に保存。未対応形式の切り分けであり、成功扱いしない。
- 元の 5 入力モードへ WOFF2 を全引数に渡した最初の診断は **32 checks / 1 failure**。復号・描画は通ったが、第1引数を生 TTF と仮定する wide-advance 試験の誤適用。期待値を弱めず、単資源モードを別に追加した。
- 正しい既存 TTF/WOFF/WOFF2/CFF/WOFF2-CFF 入力で **34 checks / 0 failures**。無ヒント EOT-Lite と XOR 包装の既存復号、wide advance、切断拒否、元描画一致を保全。`build/compat-font-container-preserved-20261013.log`。
- CRLF-aware `git diff --check` 成功。

## 残る境界

`css/css-fonts/parsing/font-face-src-format.html` と `font-face-src-list.html` の WPT 原本を読み、今回の意味に関連するものだけ主担当へ候補を渡した。原 WPT の CSSOM 宣言シリアライズ／Window named access は今回の native source 選択試験とは別であり、原ハーネスを実行していない担当結果から WPT 合格を主張しない。

検索欄の枠や Reddit 左 sidebar、GitHub の遅延はこの修理で解決したと断定しない。ホスト取得は Nocturne の通信待ち時間の比較でもない。最終媒体の 4 実サイト目視と操作は主担当が担当し、本報はその受入を置き換えない。

次は第二反例として、失敗した無ヒント候補から次候補への native 選択と、CSSOM 表現・ヒントなし EOT の境界を独立に確認する。
