# ブラウザー診断・不足境界の修理（2026-10-08）

## `undefined` がスクリプト URL に混入する原因

Baidu が公開している `esl-7410c1d5ab.js` の `toUrl` は、拡張子に正規表現を
一致させたあと、`RegExp.$1` を `extname` に保存して URL に連結する。
Nocturne の QuickJS にはこの旧式静的キャプチャーがなく、値が `undefined`
になるため、たとえば `.js` の代わりに文字列 `undefined` が付いていた。
クエリーを取り出す同じ経路も `RegExp.$1` を利用している。

- 原因は正規表現エンジンの不足。ネイティブの URL 解決器が自発的に文字列を追加する欠陥ではない。
- `third_party/quickjs/quickjs.c` の成功境界に `$1`〜`$9` と関連プロパティを実装。
- 元の文字列への参照と UTF-16 位置だけ保持し、getter 呼出時に必要な部分を生成。
  一致ごとに大きい文脈文字列を複製しない。失敗した一致では前状態を維持。
- realm、GC、メモリー不足の境界を含む有限の native 補助検査 56 項目を実施。
  これは実サイトの表示・操作受入れを代替しない。
- `script.src = undefined` の標準の文字列化は変更せず、URL 内の `undefined` を
  消す・拡張子を推測する・サイト URL を書き換える回避策は使用しない。
- 動的スクリプトの作者指定値が URL 解決前から `undefined` を含む場合、
  元値／解決結果を変更せず、文書ごと最大 8 件の診断警告を出す。
  この警告だけでは URL が不正とは断定しない。

一次資料：
[Baidu 配信 ESL](https://pss.bdstatic.com/static/superman/js/lib/esl-7410c1d5ab.js)、
[ESL のソース](https://github.com/ecomfe/esl/blob/master/src/esl.js)、
[TC39 legacy RegExp draft](https://github.com/tc39/proposal-regexp-legacy-features)。
最後の資料は Stage 3 の draft であり、最終 ECMA-262 本文とは区別する。

## 今回追加したコンソール情報

- 有限のスクロールバック：256 行、1 行最大 1024 byte、幅に応じた UTF-8 折返し。
- メッセージ番号、相対時刻、長いメッセージの継続行表示。
- `PageUp`／`PageDown`、コンソール上のホイール、`Ctrl+Home`／`Ctrl+End`。
  過去を読んでいる間は新しい出力が来ても表示位置を維持する。
- `Ctrl+Shift+C` で保持ログを **Nocturne のクリップボード**へ明示コピー。
  ホスト側のクリップボードは読み書きしない。`Ctrl+K` で消去。
- 例外の task 種別、実行時間、native 待ち、heap、DOM/layout の状態、保存済みソースの抜粋。
  診断のためにページ独自の stack getter を追加実行しない。
- resource 失敗には HTTP 状態、MIME、byte 数、経過時間、失敗理由。
  同一文書の大量失敗は有限件数で抑制する。
- 新しい resource／URL 診断では query、fragment、userinfo 等を省く。
  従来のページ自身の出力や例外 stack 全体まで秘密情報を除去できる、とは主張しない。

## 同じ画像群から修理した主な不足

- SuttaCentral：import された custom element の upgrade/reaction、CJK の折返し、
  画像の definite percentage height、`object-fit`／`object-position`、BODY overflow の viewport 伝播。
- YouTube：CSSOM `style` の PutForwards、Range と欠けていた DOM interface、
  過度に小さい timer 枠、heap／stack 枠。
  Polymer の Object 型 `text` と衝突する非標準 `HTMLElement.text` も除去。
  script／option／anchor 固有の標準 `text` は維持。
- Baidu：フォームの名前付き getter、live control collection、RadioNodeList。
- X：Intl.Segmenter、実ウィンドウ状態に基づく document の focus/visibility、
  plugins／mimeTypes の正しい空 collection。未実装 PDF viewer を偽装しない。
- iframe を実文書へ接続した際、未実装の child browsing context を文書ごと一度だけ警告。
  親 Window を返して対応済みに見せることはしない。

JavaScript heap の上限は空きメモリーに応じて 128／256／512 MiB で、全量を
先取りせず lazy に割り当てる。task budget は既定 15 秒、設定範囲は 1〜30 秒。
timer は lazy な 32 枠から最大 1024 枠まで増やす。watchdog と既存 MLFQ は維持する。

## 受入れと残る境界

隔離 QEMU/WHPX、4 GiB・4 CPU、匿名の実サイトで確認した。
ビルド、console marker、native 補助検査の成功と、サイト全体の利用成功は区別する。

- SuttaCentral：実ホームの Edition 画像と、その下の内容を表示。
  実日本語ページ `an1.82-97/jpn/kansai` の本文と折返しも確認した。
- YouTube：実 loader の例外から不足を修理。匿名ホームの応答は動画カードではなく
  feed nudge を返す場合がある。最新ビルドでは、その案内文と logo／検索欄を
  実画面で確認できた（`diag-youtube-text-final-20261008/screen-final.png`）。
  改善前は同じ案内の DOM／データが存在しても文字が空だった。
  googlevideo から HTTP 403 の応答があり、動画再生成功とは扱わない。
- X：先の不足を越えて、Castle のコードが `iframe.contentWindow` の `Date` を
  必要とするところまで進んだ。子 browsing context は未実装で、全体成功ではない。
- OpenAI：Cloudflare の実コードが子 browsing context の `document` を必要とする。
  ページ全体の表示成功ではない。
- Baidu：実フォームの名前付き control の native identity を確認。
  サーバーが返すページ形態が異なり、ESL を含まないページでは loader の実行経路を検証できない。
  最新ビルドの実検索では captcha ページに転送され、対象 ESL まで到達しなかった
  （`diag-regexp-baidu-search-20261008`）。ローダーのサイト内再受入れは未確認。
  スクリーンショットの HTTP 0 だけで、サーバーが何を返したかまでは断定しない。

最新の配布ビルドは終了コード 0。ログは
`build/browser-diagnostics-20261008/build-regexp-final.log`。
`build/nocturne.img`、`build/nocturne.vhd`、`build/nocturne.iso` を更新した。

詳細な有限のログ・画面は `build/nocturne-platform/diag-*20261008/`、
ビルドログと観測用コンソール入力は `build/browser-diagnostics-20261008/` に保存。
人工の Web ページでサイト対応を成功扱いにする確認は行っていない。
