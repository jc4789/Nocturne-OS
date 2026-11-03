# Webフォントの欠字補完と失敗時の組版保持（2026-10-10）

## 製品変更

- `user/libc/font.c`：メモリから開くWeb faceの欠字を既存の同梱フォント群で補完する。収録判定・幅・送り・描画は共通の実face選択を使う。元のLatin字形、一次faceの行メトリクス、単一faceの`font_open`は保持する。Web faceにない私用領域文字は同梱アイコンへ置換しない。
- `user/libc/web/doc.c`、`css.c`、`webi.h`：失敗した資源は完了・失敗状態へ進め、次のレイアウト境界で字体選択だけを再確認する。実faceが変わらなければ全cascade・全box無効化を行わず、SVGのboxと既存inline組版を保持する。次の著者指定srcへの進行は維持する。
- 同時のpartial cascadeで他の利用者のwanted状態が消える場合は、公開後の再選択で復元する。Shadow DOM・割当slotのflat treeを走査し、非活性fallback・未割当light DOMは除外する。実faceの切替、成功した読み込み、スタイル境界の変更では必要な再計測を保つ。

WHATWGの仕様一覧を確認したが、今回の直接の規則はCSSWGの[CSS Fonts 4字体照合](https://drafts.csswg.org/css-fonts-4/#font-matching-algorithm)、[文字処理と私用領域の例外](https://drafts.csswg.org/css-fonts-4/#char-handling-issues)、[src候補](https://drafts.csswg.org/css-fonts-4/#src-desc)である。全family stack・cluster shapingへの対応を今回完了したという意味ではない。既存のWOFF/WOFF2/CFF復号の非対応も変更していない。

## 確認と既存失敗

- 実OSの`web/fonts`：267項目、失敗0。InterのLatinを保持し、BMP／補助面CJKの送りとラスタが同梱CJK faceに一致すること、混在文字列、欠字、私用領域、解放後の同梱faceの健全性を確認した。最終のCSS走査修理で`font.c`は変更しておらず、この検査は繰り返していない。
- 最初の`styleincremental`は105項目中1失敗。失敗直前のroot cacheが必ず有効だとする検査前提が誤りだった。製品の失敗処理はcacheに触れていない。検査を完了前のcache／dirty状態の厳密保持へ修理し、実際のinline run・arenaの存在と保持、cascade件数不変、Shadow／slot内のsrc進行の確認を追加した。最終媒体では初期root cache=0、dirty=0を記録し、115項目すべて成功した。CSSキャッシュの`csscache/completed-external-ast`も23項目すべて成功。最初に指定した`csscache/native-scoped-rule-index`は存在しないtargetで実行されていなかったため、正しいtargetを最終検査で実行した。
- `web/shadow-dom-native`は成功。`web/javascript`は1165項目中19失敗で、`build/tt-fast-before/test-serial.log`の失敗項目と全件一致する。frames、DOM／module順序、watchdogに既存未解決があり、全検査成功とは報告しない。期待値・skip・製品経路を変更していない。

## 実QEMUと媒体

WHPX、4096MiB、4CPU、1920×1080、最大化した２台で、Tofuguのひらがな記事と日本語MDNのfont-family記事を並行観察した。観測JSは読み取り専用で、本文やスタイルを差し替えていない。実マウスのhover／targetを確認して本文へクリックし、PS/2 PageDownで下へ、PageUpで先頭へ戻した。主担当が下の本文と先頭の代表画像を自分で開いて確認した。MDNの日本語、画像、Tofuguの画像・固定ナビ・本文の再描画を確認。MDNの実例領域の空白と既存例外、Tofuguの埋込動画の問題は別の未解決である。両サイトで自然に失敗するWebフォントがある。実サイトの字体補完全件を網羅したとはしない。

最終flat tree修理後も、両サイトの実hover・本文クリック・下／上へのスクロールと先頭への復帰を確認した。本文のスクロールは両方とも899→1798→899→0。主担当は両サイトの`screen-4.png`と`screen-final.png`を自分で開き、下の本文・画像・ナビと、先頭の表示復帰を確認した。代表画面とserial／metadataは`build/nocturne-platform/font-final-{mdn,tofugu}`、関連native検査は`build/web-font-final-tests`へ保存している。VMはすべて終了済み。

正規の`python scripts/build.py -j 8 --log build/web-font-final-build.log all`でIMG／VHD／VHDX／ISOを生成した。最終起動媒体のkernel／initrdは現行buildと一致し、initrd中のbrowser、browserjsworker、cppdemoもbuild/rootのバイナリと一致した。

- kernel SHA-256：`07e2138949e4efa41023080b35aa84044ba323c5daffcb1007378f7e6bbaeb46`
- initrd SHA-256：`240de31115df5a0e8e2fa1d7d25a7a4515be8925dbf8077ee0a97a9ec8f39d31`

ユーザーのHyper-V、通常のdata.img、永続データは操作していない。Hyper-V実機は未確認。停止済みの今回専用VMの一時媒体と重複PPMは回収し、PNG・ログ・metadata・正規媒体を保全する。回収後のディスク自体を再実行可能な保存証拠とは扱わない。
