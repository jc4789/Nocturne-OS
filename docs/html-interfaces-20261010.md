# HTML要素インターフェース追加と実サイト確認

## 実装

依頼された26インターフェース、別名を含む34タグを既存のネイティブDOMへ接続した。通常・旧式要素20種は `user/libc/web/js_missing_html_elements.js`、テーブル6種は `user/libc/web/js_html_tables.js`。既存のprototype番号を変更せず、Cの分類とJS配列の末尾に同じ順序で追加した。解析、createElement、clone、import、adopt、HTML以外の名前空間を扱い、空のクラスやinstanceofの上書きで対応を作っていない。

- HTMLConstructor、受け手のブランド、IDL属性の型変換・URL反映・旧式属性、globalの記述子を実装。
- Map.areas、table.rows/tBodies、section.rows、row.cellsはSameObjectのライブHTMLCollection。入れ子のテーブルを混ぜず、見出し・本文・フッターの順序を維持する。
- caption、thead、tfoot、tbody、行、セルの生成・挿入・削除は実DOMを変更し、例外と必要なcustom element反応を既存経路へ接続。
- body/framesetのWindowイベント属性は所有文書のWindowへ連携。解析属性の登録、document.write、document.openによる旧ハンドラー破棄、Windowのonerrorの5引数と取消条件を修理。
- marqueeの開始・停止、方向、scroll/slide/alternate、ループ、遅延をネイティブ描画・ヒット判定へ接続。通常文書で全DOMを毎tick走査しない。
- HTMLEmbedElement.getSVGDocumentは既存の同一生成元の生きたSVG子文書を返し、それがなければnullを返す。embedからのSVG文書ロード全体は今回未検証。

GitHubのCodeボタンを実クリックすると、Reactが未実装のElement.getClientRectsを呼んで本文を失っていた。実レイアウトの行内断片・境界箱・スクロール・テーブルcaptionを返すネイティブ経路とDOMRectListを追加した。修理後は同じボタンからCloneメニューとDownload ZIPが表示され、本文を維持した。既存レイアウトが未対応のCSS変換やSVG内部の幾何まで完全対応したとは扱わない。

仕様は [WHATWGの要素インターフェース一覧](https://html.spec.whatwg.org/multipage/indices.html#element-interfaces)、[HTMLConstructor](https://html.spec.whatwg.org/multipage/dom.html#htmlconstructor)、[テーブル](https://html.spec.whatwg.org/multipage/tables.html#htmltableelement)、[marquee](https://html.spec.whatwg.org/multipage/obsolete.html#the-marquee-element)、[イベント属性](https://html.spec.whatwg.org/multipage/webappapis.html#event-handler-attributes)、[Web IDLのglobal定義](https://webidl.spec.whatwg.org/#define-the-global-properties)、[CSSOM ViewのgetClientRects](https://drafts.csswg.org/cssom-view/#dom-element-getclientrects)と照合した。

## 検査結果

最終製品ビルドは `build/html-interfaces-geometry-build.log`。IMG、VHD、VHDX、ISOを更新し、initrd内のbrowser/browserjsworkerとbuild/rootの実行ファイル一致を確認した。

- kernel SHA-256: `07e2138949e4efa41023080b35aa84044ba323c5daffcb1007378f7e6bbaeb46`
- initrd SHA-256: `16f78e08039fa6a5d1822b0662f28b6e0f947bc73077e1b7849bf7df20a501d6`
- 最終 `web/html-interfaces`: JSの1023項目すべて成功、Cのまとめは7 checks / 0 failed。
- 最終 `web/javascript`: 1171 checks / 19 failed。変更前は1165 checks / 19 failedで、失敗名と分類も一致する。全合格とは扱わない。
- 既存のattribute-nodes、shadow-dom-nativeは成功。marqueeを含むネイティブsemantic検査は155 checks / 0 failed。その後の修正はイベント連携と矩形APIで、marqueeの製品経路は変更していない。

既存19失敗は、inactive Timerとframeの保存メソッド・detach境界、foreign interface、DOMContentLoaded/document.write/late-writeの順序、3条件の実行予算停止・報告・解析生存である。今回追加のbody属性検査は、既存のTimer失敗より先に実行して確認した。

検査側の固定512件のmarker配列が最終api-doneを捨てる不備も修理した。動的配列にしてメモリ不足を明示的失敗にし、期待値を緩めていない。生成物は正規のjs_embed.pyとbuild.pyで再生成した。未変更の同条件検査を繰り返して合格を作っていない。

証拠: [最終検査ログ](../build/html-interfaces-complete-regression/test-serial.log)、[marqueeを含む検査ログ](../build/html-interfaces-final-regression/test-serial.log)。

## 実QEMUの確認

WHPX、4 CPU、4096MiB、1920×1080、主ブラウザー最大化。ホストRAMの都合で独立VMを最大2台ずつ動かした。全サイトは上記の最終kernel/initrdを使用し、専用boot/dataを隔離した。主担当自身が画面を閲覧した。観測JSは読み取り専用で、DOM・CSS・関数の差し替えやJS click/scrollToによる操作代用は行っていない。クリックは現在のhitとhoverを確認したPS/2入力、スクロールはPageDownを2回、PageUpを2回で行った。ハーネスのmarker成功と、サイト全体の正常動作は区別する。

| サイト | 実表示・操作の結果 | 残る問題 |
| --- | --- | --- |
| [百度百科・三体](https://baike.baidu.com/item/%E4%B8%89%E4%BD%93/5739303) | 中国語本文と下の人物表を表示し上下移動。「刘慈欣」の実リンクから別ブラウザー窓が開き、著者記事の本文まで到達。 | AxiosのNetwork Error、空のカード、装飾SVGの崩れが残る。子窓は既定サイズ。 |
| [GitHub・deepseek-harness](https://github.com/deepseek-ai/deepseek-harness) | ファイル一覧とREADMEを表示し上下移動。Code実クリックでClone/Download ZIPを表示し、本文が消える例外を解消。 | 右側の日付・commit列の見切れなど、表示の完全一致は未達。 |
| [MDN・font-family](https://developer.mozilla.org/ja/docs/Web/CSS/font-family) | 日本語本文と構文表示、上下移動。実リンクからCSS解説のURL・本文へ遷移。HTMLPreElement未定義の例外を解消。 | デモのpostMessage周辺でTypeError、未定義値のtoString例外。デモ欄が空で、全体正常ではない。 |
| [OpenAI・GPT-6 Astra](https://openai.com/ja-JP/index/gpt-6-astra/) | チャレンジ後に記事へ到達。日本語本文と下の評価説明を表示し、上下入力を受け付けた。 | WebGL生成失敗で埋込みゲームが動かない。先頭へ戻した後にヒーロー部分が空白になるため、スクロール再描画の受入は未達。第2のメニュー操作は確認できなかった。 |
| [Reddit・r/codex](https://www.reddit.com/r/codex/) | 投稿本文・画像・コミュニティ情報を表示し、下から先頭へ戻る画面変化を確認。 | 次ページ取得失敗、Timed out、検索欄の重なり。第2クリックはRPL-TOOLTIPで確認条件を満たさず、遷移成功に数えない。 |
| [Amazon.co.jp](https://www.amazon.co.jp/) | 日本語本文・商品画像・下部の商品列を表示し、上下移動。ロゴを実クリックすると/ref=nav_logoへ遷移し、ホーム本文を再表示。 | 未実装の暗号処理によるNotSupportedError、一部カテゴリ画像の空欄が残る。検索送信・購入・ログインは未確認。 |

各サイトの画面とログは `build/nocturne-platform/html-interfaces-{baike,github,mdn,openai,reddit,amazon}-verified/`。代表画面は各ディレクトリのscreen-final.png、下へ移動した画面はscreen-3.png。OpenAIでは最終画面の空白も失敗証拠として保存した。

この結果をサイト全体の互換性完成、性能改善、回帰なしの証明にはしない。以前報告された速度低下は同条件の変更前後比較を行っておらず未解決扱い。Hyper-V実機はユーザー担当で今回未確認。

## 後片付け

今回のVMと検査プロセスの終了を確認し、使い捨てboot/data、PPM、検査用展開媒体を回収した。最後の回収では終了済み7 VMの117ファイル、Amazonと最終検査の21ファイルを削除した。これ以前の途中確認の一時媒体も回収済み。最後に確認したDドライブ空き容量は160.42GiB。PNG、ログ、metadata、ソース、最新配布媒体、通常の永続data.imgは保全した。回収済みVMコピーをそのまま再実行可能な保存媒体とは扱わない。
