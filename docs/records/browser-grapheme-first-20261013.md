# 書記素境界：現実装と限定確認

## 変更

- `user/libc/web/layout.c` の既存文字／フォント helper 直前へ
  `wf_grapheme_decode`、`wf_grapheme_break`、`wf_grapheme_next` を統合。
- `user/libc/web/webi.h` に状態と API を宣言。run／style の境界を越えて
  状態を保持し、論理text分断だけで初期化できる。
- Unicode 18.0.0 の GCB、InCB、Extended_Pictographic を公式原UCDから
  `scripts/unicode_grapheme.py` で正規生成。QuickJS Unicode 17 表は不変。
  `third_party/unicode/README.nocturne.md`、原データ、Unicode License V3 を保全。
- ASCII 通常経路は表探索／allocation 不要。非ASCIIは二分探索、
  Hangul LV/LVT は規範的算式。next の at は既知の境界であり、prefixの
  全再走査をせず、全字幅やコード点を「書記素」と称さない。
- UTF-8 は明示 n 以内のみ読み、truncated／overlong／surrogate／
  範囲外符号化は U+FFFD と1byte進行。文字列終端の NUL に依存しない。

## 一次資料

[UAX #29 revision 49](https://www.unicode.org/reports/tr29/tr29-49.html) は
Unicode 18.0.0 の公開安定版。GB9c の18.0規則は
`InCB=Linker InCB=Extend* × InCB=Consonant` であり、17.0規則と混ぜない。
原データの版・SHA256 は生成表に記載。

## 実行結果

- `python -X utf8 tests/check_grapheme.py`：実 layout.c helper のホスト分離検査。
  同版原 `GraphemeBreakTest.txt` **853行**を next と毎コード点別run状態保持の
  2経路へ供給、0失敗。guard-page による末尾read保護も0失敗。
- 原試験 SHA256：
  `b0cf047ee94485bbdc846de2b902f5f8a815f6b674f9d04223cddadd91c9df31`。
- `python -X utf8 scripts/unicode_grapheme.py --check`：生成表と原UCD一致。
- 現 layout.c の freestanding clang syntax-only：exit 0。
  root の新 line metadata に `ellipsis_active` 初期化省略のwarning 1件あり。
- `git diff --check`（CRLFを許容する既存設定）：exit 0。

## 境界と次

これらはホスト・静的確認であり、ellipsis 実描画、実サイト、性能、
QEMU／Hyper-V の成功を証明しない。root が line metadata と paint に
state API を接続して全ビルド・同条件実サイトを確認する。
Reddit 作者 lazy partial の constructor／request 状態は次VM read-only
probe 待ちであり、sidebar が改善済みとは報告しない。
