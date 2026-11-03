# CSS共通影実装・第一報

## 製品原因と修理

DDG現検索欄の外輪郭は `build/css-compat-ddg-wpl.css` の三重 `box-shadow` であり、古い `.search` の border ではない。原変数は `0 10px 20px 0 rgba(0,0,0,.02),0 2px 6px 0 rgba(0,0,0,.06),0 0 0 1px rgba(0,0,0,.08)`。最後の spread 影が輪郭を作る。修理前の native property table と描画には box-shadow が存在しなかった。実Nocturne `build/nocturne-platform/compat-after-ddg-20261013/after-live.png` は自身で確認し、検索欄白背景だけで輪郭・影が欠けていた。

製品にはサイト名・class名・上記変数を入れていない。`cssprop.c` に本物の comma list 文法、連続する2～4長さ、色、inset、計算長さ、負spread、blur非負範囲、computed arena所有とCSSOM指定値/計算値直列化を接続した。`webi.h` の style 本体は list pointer、可変listは必要時だけarenaへ置く。初期値none、非継承とexplicit inheritは既存property copy経路を使う。

`paint.c` は outset/inset、spreadと角丸、透明border-boxのknockout、first shadowが上になる順、背景/境界に対する積層、色alphaを実合成する。Gaussianは標準偏差blur/2の分離FIR、3σ外の切捨て。geometry-only A8 masksをbox/inlinefragmentで保持し、色だけの更新はmaskを再作成しない。大きいauthorboxはviewport＋畳込みhalo分だけを確保し、scrollで新sliceを再構築する。実確保・整数overflow失敗はpreflightで処理し、普通のrepaintでviewport Gaussian確保を繰り返さない。

影をink bounds/known atomic damageへ追加し、scroll領域やhit領域を広げない。offscreenの原boxが見えなくても影がviewportへ入れば描画する。既存style retirement、derived inline-flex等のdecorations、fragment reflow、box廃棄へcache解放を接続した。Gaussian構築はnative checkpoint＋既存parallel raster経路であり、host確認だけからCPU並列の速度改善は主張しない。

規格根拠: [CSS Backgrounds 3 §6.1](https://www.w3.org/TR/css-backgrounds-3/#box-shadow)。各角の独立/楕円radiusやCSS mask全体合成等の従来の未対応を、この修理で全面対応したとは主張しない。

## 限定確認と元の失敗

- `python -X utf8 tests/check_boxshadow_native.py --parser`: 81件/0。ローカルWPT原本 valid/invalid の宣言値を78件そのまま実製品parserへ入力し、specified/computedの3反例を追加したhost確認。browserのWPT testharness実行ではない。
- 初回81件は独立fixtureの `calc(-1px)` 文字列長を19としていたため1件失敗。実際の18を `strlen` に訂正した。期待する未clamp指定値文字列は変更していない。
- WPT invalid原本を読んで「長さ群の途中へcolor/insetを入れてはいけない」反例を発見し、cascade/CSSOM/supportsすべてのparserを厳格化。token数を越えるtailもcascadeで黙って捨てない。
- `python -X utf8 tests/check_boxshadow_native.py`: 20件/0。実paint/gfxを抽出、host dispatcherだけ直列化した算術/cache/失敗所有検査。DOM・guest・実サイト受入の代用品ではない。初回20件/2失敗は第二報に保全する。
- `clang --target=x86_64-unknown-none-elf -ffreestanding -fsyntax-only` は shadow関連製品5ファイル、新fixture、js.cとLexbor正規includeを確認。diff checkも成功。これは梱包・実行の証拠ではない。
- rootが新媒体でGitHub/Redditを開いた結果、両方で不正free crashが発生した。製品回帰でありhost合格では相殺しない。原因と最小修理は第二報。修理後の実サイト/guest結果はこの第一報時点で未確認。

## ソース束縛（不正free最小修理後）

全木fingerprint: `b68460fcb3d22999f041a1d35d26ba536005b1bec83c1a45268ea8317e2bf521`。共有他担当のdiffを含み、これだけで担当外変更を確認済みとはしない。

|主材料|SHA256|
|---|---|
|user/libc/web/cssprop.c|8838883f9cbf3453a9b441f6543258a3455b344f373b469e07be211c989ab788|
|user/libc/web/paint.c|18fad5d0401b5e3053230e0b7909682713cadd84623f9c5ba8f86e35add34b78|
|user/libc/web/box.c|90949c606d8eae7376933151068ff6f4a041f73e876604497b7ec4bcabd45a43|
|user/libc/web/webi.h|3a4098bc6dbe6b7cb12d9120cbcbf418c362bb40d32ef38dc0ee7e89c7b7f412|
|user/libc/web/js.c|894a203afcf3910e889deea576e28eda96151a1edf0517bfea948a3d8da815da|
|user/libc/web/layout.c|35740aaab161a0254c100c9c8479b208c0bf27a63d4f5fd5e627debbb1906b68|
|tests/boxshadowtest.c|0e9fb01626f278138ee9f8ed09466bb903e96a0e2a00da810f9564305eb39d5f|

次はrootの同2実サイト起動と新 native fragment退役群・WPT3原本を確認し、失敗を修理する。DDG影だけで提供全比較画像の細部を受入にしない。
