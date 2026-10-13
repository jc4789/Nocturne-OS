# 共通丸角・影 第二考察

第一報 `docs/browser-css-radius-first-20261013.md` を反例・所有権・従来性能の境界から再考察した。snapshot は同じ `build/css-compat-radius-current-source.json`。host 結果を QEMU/実サイト/性能へ昇格しない。

## 実際に見つけた反例と修理

1. **片側だけ開いた inline fragment。** 新背景 clip は first/last によって開いた角を square にする一方、影 cache が style 全 corner を再計算すると途中 fragment の影まで丸くなった。`shadow_cache_prepare_radii` が補正後 used geometry を受け、deco の背景/影で同じ形を共有するよう修理。新 host assert が左 2 角 0・右角 40 を実 cache に確認。17 件 / 0 失敗。
2. **従来 circular AA。** 汎用楕円距離への置換は古い丸角影の edge alpha を変え、既存 host 20 件中 1 失敗した。期待値を緩めず uniform circular の場合に既存 `sdf_rrect` を保ち、その他 4 corner/楕円は一般計算にする製品修理で 20 件 / 0 失敗へ戻した。
3. **極大/画面外 geometry と取消。** viewport intersection の両端を canvas 範囲へ clamp してから integer 化し、画面左外側の巨大座標が失敗して全描画を止めないようにした。offscreen-empty でも checkpoint を落とさず旧 cache publication を保つ。初期 host radius の取消 1 失敗は製品の checkpoint を直し、期待を変更せず現在成功。
4. **style arena の逆戻り。** corner pointer 4 本では ordinary 50% でも可変 arena chunk が増え、以前の style memory 修理を悪化させる。共有 POD 8 `len_t` に移行した。動的 AST だけ既存 arena 所有で、単純 percentage は `arena.head == NULL` を確認。

## 不正 free と damage 失敗の扱い

新 cache 所有導入直後の GH/Reddit startup crash は保存済みで、`struct deco d` が未初期化だった。`d={0}`、truncate の各 cache 解放、arena 解放直後の deco pointer/count 消去を実装修理。主担当が repaired 実 QEMU 2 画面を見て startup crash 解消を確認したが、この新半径 ABI の成功へ流用しない。

原 guest `build/browser-compat-integrated-guest-20261013/test-serial.log:84-86` の shadow 44 件 / 1 失敗は不変保全。命名 atomic の fixture div が絶対配置の blockification で実 B_BLOCK だったことを source/HEAD 両方で確認し、本物 img B_ATOMIC へ訂正。製品の block full-damage fallback を緩めず、元 outside-ink 条件とさらに Gaussian halo 条件を保持した。`--damage-only` 6 件を新限定実行にし、旧 43 PASS 反復を避ける。訂正理由は `docs/browser-css-shadow-damage-fixture-20261013.md`。まだ新 guest 結果は未確認。

## 残る受入の弱い境界

- cache は geometry 変更時だけ確保し、mask 無し primitive は旧 fast path を保つ。ただし新 mask の edge blending と cropped mask の scroll preflight コストは同条件実サイトで未計測。高速化・無回帰を断言しない。
- transformed/offscreen/fixed ancestor、glyph/filter/group opacity、媒体/canvas の mask consumer は source と syntax を照合した。native fixture では decoded SVG、overflow child、fixed escape、curve hit、retained cache、longhand、zero-axis を新条件で確認するが、実 site 操作の代用品ではない。
- 原 WPT valid/invalid/computed の原本 bytes/SHA/helper を runner に固定し、3 ページ 48 cases を要求する。層 4 原本 55 cases と flex text-overflow の original/reference 全画素比較も既存 runner へ追加した。元 DOM/CSS/assert は変更せず、実行基盤失敗を pixel/test failure と分ける。未実行を PASS と報告しない。

次は主担当による source 査読と完全 build、限定 guest、新 DDG/Reddit/GH 原画での半径と細部、入力/上下 scroll、性能の独立確認。
