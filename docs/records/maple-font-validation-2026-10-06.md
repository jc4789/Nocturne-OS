# Maple Mono 標準フォント化

## 構成

ユーザー指定の `D:/Fonts/MapleMono-NF-unhinted` から Regular / Bold / Italic / BoldItalic、
`D:/Fonts/MapleMono-NF-CN-unhinted` から Regular を、そのまま `rootfs/usr/share/fonts` に同梱。
サブセット化・字体変更・形式変換は行っていない。OFL本文とハッシュを同梱する。

通常の文字はMaple Mono NF、欠落した文字は共有のCN Regularで描く。
CN版4書体を重複してロードせず、必要になるまで約20.6 MBのCN版は読み込まない。
フォールバックの字形選択を、収録判定・一文字の幅・文字列の幅・描画で共通化した。
通常の `font_open` は単一書体のままで、標準ファミリーだけがフォールバックを使う。
不足文字を太字・斜体で表示する場合も、現時点ではCN Regularの字形を使う。

ブラウザー本文とmonospace、ユーザーランドの共通UIがMaple Monoを使う。
UIの既存8×16 / 16×32セルは維持し、全角字形の測定と描画は2セルにする。
ブラウザーの標準行高にはフォントの縦メトリクスを反映する。CSSで明示された行高は維持する。
カーネル・起動時のビットマップ描画やNocturneのABIは変更しない。

## 収録範囲の確認

CN RegularのUnicode cmapから確認：

- ひらがな U+3041–U+3096: 86/86
- カタカナ U+30A1–U+30FA: 90/90
- 統合漢字 U+4E00–U+9FFF: 20,976/20,992
- ハングル音節 U+AC00–U+D7A3: 0/11,172
- サンプルの漢字・仮名は1.2 em、欧文は0.6 em

完全なCJK対応ではない。中国語向け字体であり、日本語向け地域別字形の選択も未実装。
端末の全角セル管理、エディターのUnicode編集、OpenType合字・複雑な文字組み、
ダウンロードWebフォント対応は、この変更に含めない。

## 実行確認

ビルド6の `make all` は終了コード0。initrdは52,960 KiB。
QEMU 512 MiB、専用128 MiBデータディスク、ブートディスクのsnapshotで検証。
ユーザーのHyper-V VM・データディスク・コピー済み媒体は変更していない。

### 実サイト

Nocturne内で公開の `https://ja.wikipedia.org/` を読み込み、リダイレクト後の日本語メインページを描画した。
見出し・リンク・本文の漢字、ひらがな、カタカナと欧文を画面で確認。
HTML試験ページではなく、Nocturne自身のHTTPS取得とブラウザー描画による確認である。

- 画面: `build/nocturne-audit/maple-live-6/wikipedia-initial.png`
- ログ: `build/nocturne-audit/maple-live-6/qa-serial.log`

中国語版の公開メインページも同じNocturneで読み込み、本文・見出し・リンクの中国語描画を確認した。
画面は `build/nocturne-audit/maple-live-6/chinese-wikipedia.png`。
カーネル側のタイトルバーとタスクバーは旧ビットマップ描画のため、中国語タイトルはまだ `?` になる。
同梱イメージ内の5つのTTFもmanifestのSHA-256と一致した
（`build/nocturne-audit/maple-image-hashes.json`）。

これはフォント表示の確認であり、Wikipedia全機能や指定4サイトのJavaScript合格を意味しない。

### 補助回帰

実Nocturne内のTinyCCでコンパイルして実行：

- `fonttest`: 83 checks、0 failed。実Mapleとの画素一致、CNフォールバックの画素一致、
  異なる漢字の画素差、4書体、描画と測定の一致、クリップを確認。
- `webtest`: 99 checks、0 failed。
- media policy: 106/0、media events: 12/0、hover: 447/0。
- Image: forms 47、images 72、0 failures、0 unexpected errors。
- JavaScript全体: 492 checks中2 failed。Intl試験中の1秒タスク制限超過が残る。
  成功したフォント試験でこの失敗を隠さない。

ログ: `build/nocturne-audit/maple-contracts-6/qa-serial.log`。
DeepMindの同ビルドでもカルーセル初期化の時間制限超過が残り、全サイト正常動作は未達成。
