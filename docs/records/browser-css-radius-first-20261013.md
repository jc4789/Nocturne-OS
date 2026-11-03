# 共通丸角・画像・overflow 第一報

## 目的と現在源

DDG の四角い favicon、Reddit/GitHub の四角い portrait/pill を、サイト特例ではなく CSS Backgrounds 3 の丸角・replaced content・overflow・hit 境界で修理する。現在源 snapshot は `build/css-compat-radius-current-source.json`、fingerprint は `3228f2a9ece2328de86cfc51c835675b971ac2f2ad2d7f10c2fdbb2b360fe390`。主担当の他変更が統合されるため、実行媒体一致と実画面受入はこの記録と区別する。

## 製品修理

- `cssprop.c` は 4 corner × 2 axis の length-percentage を `style_t` 内 POD で保持する。1〜4 値の水平/垂直 shorthand と slash、4 longhand、cascade 優先、CSS-wide、var 展開、calc、CSSOM specified/computed canonicalization を接続。普通の `50%` は style arena の新 chunk を要求しない。旧 float は native 内部互換 marker のみ。
- `paint.c` は百分率を border-box の幅/高さで別々に解決し、4 辺の和に基づく単一 overlap factor で全 8 軸を縮小。どちらかの軸が 0 なら square。padding/content edge は対応 border/padding を減算する。1 corner 専用や 50% 専用の近道ではない。
- box/deco 所有の retained A8 geometry を preflight で構築し、通常再描画は再利用。viewport crop、checked allocation、取消前の旧 cache 保持、deco truncate・layout arena retirement・box retirement を接続。影も同じ 4 corner geometry を使う。
- `common/gfx.{h,c}` の borrowed immutable mask chain を solid/blend/image/unifont に統合。`font.c` glyph、native media、canvas の直接書込にも mask 有りのときだけ接続し、mask 無しの既存 fast path は維持。正常 media の時間・世代・復号経路を変更しない。
- 背景・replaced content・普通の overflow descendants・stacked ancestor clip・rounded hit を同じ形状へ統合。fixed containing-block による overflow escape を保ち、native snapshot の丸角迂回を拒否する。

仕様資料は [CSS Backgrounds 3 丸角](https://www.w3.org/TR/css-backgrounds-3/#border-radius)。楕円軸、辺の重なり、内側曲線、replaced/overflow/pointer の曲線はこの節の要件へ対応させた。

## 検証の範囲

- 最新 POD と fragment 影修理後 host geometry: `python -X utf8 tests/check_boxshadow_native.py --radius` → 17 件 / 0 失敗。旧 circular AA の退行を避ける exact fast path を含み、`...check_boxshadow_native.py` → 20 件 / 0 失敗（`build/css-compat-shadow-pod-raster.log`）。これは serial host 分離確認で native site 受入ではない。
- POD/parser の host 40 件 / 0 失敗は、原 WPT valid 23 値 + invalid 11 値、POD/canonical 3 件、既存影 serializer 3 件。testharness を動かした WPT 成功とは呼ばない。
- 最新の製品 C、glyph C、fixture/runner の freestanding syntax と CRLF 対応 diff check は成功。共有 ABI の完全 build は主担当へ依頼した。
- 新 native `boxshadowtest --radius`、修正済み単独 `--damage-only`、原本 WPT 3 ページ 48 cases を既存 runner へ追加。これらの guest 実行はまだ未確認。

## 実画面と残件

私自身が `build/nocturne-platform/compat-shadow-ddg-20261013/shadow-live.png` と提供参照 `codex-clipboard-1972176d-c9fb-456d-ac07-aee3601f468e.png` を見た。これは旧 shadow 版で、検索欄輪郭・影は見えるが favicon は四角。filters と More の矢印は参照の同一 baseline に対して下段へ落ちる。この新半径版の画面ではないので favicon 改善済みとはまだ主張しない。字体、矢印、Reddit の sidebar/二重 placeholder、全サイト入力・上下 scroll、実速度比較は別の未達項目。半径を追加したことでこれらを成功扱いにしない。

次は主担当の完全 ABI build、限定 guest、指定実サイト原画と入力/上下 scroll の確認。
