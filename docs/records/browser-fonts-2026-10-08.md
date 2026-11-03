# 本文書体と欠落文字の改善（2026-10-08）

## 実装したこと

Maple Mono は等幅書体。これを全サイト本文へ強制する処理を外し、CSS の `font-family` を実際の測定・描画へ接続した。

| 指定 | 欧文の主書体 | 主書体に無い CJK の優先候補 |
|---|---|---|
| Sans | 既存 Inter の４書体 | Noto Sans CJK JP Regular |
| Serif | 指定 Noto Serif Living Regular | Noto Serif CJK JP Regular |
| Mono／OS の UI | 既存 Maple Mono の４書体 | 既存 Maple CN、その後 Plangothic |

- CSS の継承、`font` shorthand、初期値と Canvas の書体指定を同じ family 値で処理する。通常本文の初期値は Serif、Canvas の初期値は Sans。
- Canvas の `measureText`／`fillText` と `save`／`restore`／リセットに family を反映した。
- 欠落文字は共通の有限 resolver で探索し、収録判定・幅・描画で同じ実 face を使う。原版の cmap に無い文字を収録済みと偽装しない。
- フォントは必要な時だけ読み、各 face をプロセス内で共有する。UI とコンソールの主書体は変えていない。
- 公開 OS ABI、カーネル、RAM-root と `/data` のモデルは変えていない。UNIX／POSIX 互換は追加していない。

実装の入口は `user/libc/font.c:font_family/glyph_face`、`user/libc/web/cssprop.c:choose_family`、`user/libc/web/layout.c:style_font`、`user/libc/web/js_canvas.c:text_native` と対応する JS。生成済み `js_bootstrap.inc` もビルドへ反映済み。

## 同梱を限定した八書体

指定された統合 Noto は次の４つだけ。

- `NotoSansLiving-Regular.ttf`
- `NotoSansHistorical-Regular.ttf`
- `NotoSerifLiving-Regular.ttf`
- `NotoSerifHistorical-Regular.ttf`

その後の追加許可に基づく CJK は、日本語 Regular OTF の Sans／Serif 各１つのみ。さらに提示された Plangothic の static P1／P2 を追加した。可変版、他地域、他 weight、フォルダー全量は同梱しない。

| 追加資産 | 原版サイズ |
|---|---:|
| 統合 Noto ４つ | 18,929,068 bytes |
| Plangothic P1／P2 | 32,869,912 bytes |
| CJK OTF ２つ | 41,041,600 bytes |
| **合計** | **92,840,580 bytes（約 88.54 MiB）** |

元ファイルを変換・結合・サブセット化せず、そのままコピーした。各ファイルの出所、SHA-256、著作権表記、OFL を `rootfs/usr/share/fonts` の３つの manifest とライセンスに保存した。最終 `initrd.tar` 内の八書体が原版とバイト単位で一致することを確認済み。初回の暫定 Noto 九書体はすべて除去済みで、最終媒体にも残っていない。既存 Maple／Inter とユーザーの原版は削除・変更していない。

## 起動イメージの容量不足も修正

最終資産のビルドは、固定 128 MiB の起動ディスクへのコピーで `Disk full` になった。`scripts/mkimage.sh` を、実 payload と任意の `diskfiles` のサイズに余裕を加え、64 MiB 単位で容量を確保する方式へ修正した。最小値は 128 MiB、今回の生成サイズは 256 MiB。RAM-root の動作や永続データの設計を変える修正ではない。

修正後の `scripts/build.py -j4` は終了 0。QEMU 用 raw、Hyper-V 用 VHD／VHDX、ISO を生成済み。Hyper-V 上での今回のフォント確認は未実施。

## 最終構成の実行結果

すべて専用ディスク、WHPX、４ CPU、2 GiB RAM で実行。個人の `/data` やログイン情報は接続していない。起動媒体から kernel／initrd を取り出して最新 build の SHA と照合した。各 QEMU は終了済み。

| 実行 | 観測 |
|---|---|
| Google＋ネイティブ描画確認、35.4 秒 | `fonttest: 245 checks, 0 failed`。実サイト上の Canvas ７条件がすべて true。画面の本文は比例幅、コンソールは従来の等幅。 |
| SuttaCentral、50.19 秒 | Serif の見出しと Sans の本文・斜体を実画面で確認。既知のサイト側 `setValidity` エラーは残るため、サイト全体の合格とはしない。 |
| 百度、45.15 秒 | ロゴ、検索欄、中国語リンク・本文を実画面で確認。以前の上方への配置崩れは見られない。タイトルバーの非 ASCII は別系統で、まだ `?` になる。 |

Google 上の 24px での `i`／`W`／`Wi` の測定値：

- Sans：5.8125／23.6484375／29.4609375
- Serif：約 7.68／25.128／32.808
- Mono：約 14.4／14.4／28.8

ネイティブ確認は既存 `tests/fonttest.c` の描画・測定確認を拡張したもの。架空のサイト互換テストページではない。Maple／CN の既存契約、実 style の異なる輪郭、Living／Historical／CJK／補助面の欠落文字、CFF OTF の実輪郭、描画幅の一致、クリップを確認した。Canvas の確認は実 Google で行い、偽サイトを作っていない。

証拠は `build/browser-fonts-20261008/verification-final.json` と、`build/nocturne-platform/fonts-{google,sutta,baidu}-01` の serial／metadata／最終画像。これらは今回の最終八書体構成の観測であり、暫定構成の成功を流用していない。

前段のブラウザー修理では Google／Accounts、DuckDuckGo、百度、Reddit、SuttaCentral の５つの実ドメインを確認した。詳細と未完の機能は [前段の記録](D:/Programes/llm%20vibe%20slop%20os/docs/browser-bindings-2026-10-08.md)。今回の限定追加では上の３サイトに絞り、確認だけを繰り返していない。

## 完成した範囲と残る制限

- **完成：** 本文の Sans／Serif／Mono 選択、Canvas への接続、共通の有限欠落文字探索、指定八資産と原版一致、起動容量不足の修正、最終媒体の実行確認。
- **Serif の bold／italic は実 Regular へ戻る。** 指定統合版は Regular しか持たない。存在しない別 style を実装済みとはしない。
- 統合版は実験的で巨大な全字形縦 metrics を持つため、同梱統合 face の行箱には既存 Inter Regular の metrics を使う。原版 bytes、字形の scale／advance／raster は変えていない。特殊な tall glyph の行箱統合は未完成。
- CJK は JP 字形が既定。言語・地域ごとの字形選択、複雑な shaping／結合列、カラー絵文字、可変フォント、`@font-face`、computed style の完全な書体名反映、カーネル bitmap タイトルバーの Unicode は未実装。
- 全 Unicode の完全収録や全サイト互換は主張しない。SuttaCentral／Reddit 等の残るブラウザー機能不足は、フォントだけで解消したことにはしない。
- 今回の実行条件は 2 GiB。追加フォントは遅延読み込みだが RAM-root 自体は増えており、512 MiB で大型サイトを操作できるとは未確認。

親はサブエージェントの最終報告・第二考察を読み、ソースを独立に確認した上で上記実行を担当した。この追加は medium の限定監査として記録し、前段の strict controller 合格を今回のフォント差分の合格として流用していない。
