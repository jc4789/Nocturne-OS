# Nocturne 用 Lexbor 移植

Lexbor 3.1.0、上流リビジョン `f4cbbcd91359a0ec9499e3ce7e263de629482d61`。
ユーザー指定のローカル複製から、`core`、`dom`、`ns`、`tag`、`html`、
`css`、`selectors`、`encoding`、`url`、`unicode`、`punycode` のソース・生成済み
テーブルを採用しています。各ファイルの上流の SHA-256 は
`third_party/lexbor/UPSTREAM.json` に、ブラウザーへつなぐために手を入れたファイルは
`third_party/lexbor/NOCTURNE_PATCHES.json` に記録しています。`vendor.py` は、
前回の取り込みと同じ上流から変えたファイルを残し、それ以外の差分では止まります。
`url`（WHATWG の URL 解析器、IDNA は `unicode` と `punycode`）は
`user/libc/web/web_url.c` から使い、JavaScript の `URL`・`URLSearchParams` と
フォームの URL 検証が同じ解析器を通ります。現行の URL Standard と UTS #46 に
合わせた修理（ASCII のドメインは小文字にするだけ、host 設定子、CheckBidi・
CheckJoiners）は `NOCTURNE_PATCHES.json` に記録し、その表は
`scripts/unicode_idna.py` が `third_party/unicode` の原データから生成します。
上流の `LICENSE`、`NOTICE` と個々の著作権表示を保持しています。

`memory.c` は Nocturne の `malloc`、`realloc`、`calloc`、`free` を直接使用します。
`include/memory.h` は既存 `string.h` を公開するだけの C ヘッダーです。
POSIX / Windows のファイル・性能測定ポート、CLI、ネットワーク、スレッド、
共有ライブラリー、上流の未完成レイアウトエンジンは組み込みません。
`lexbor_fs_*` と `lexbor_perf_*` はリンク対象外です。

既存の freestanding x86-64 ユーザーランド設定で静的オブジェクトとしてビルドし、
通常アプリと TinyCC の `libc.a` に必要な部分だけをリンクします。
TinyCC 用ヘッダーは SDK の `C:\Nocturne\SDK\Include\lexbor` に配置します（`scripts/mksdk.sh`）。
ライセンスと由来情報は `C:\Nocturne\Licenses\lexbor` に配置します。
ファイルシステム、GUI、既存システムコールに変更を加えません。

HTML・DOM・CSS・文字コードのライブラリー移植と、ブラウザーへの利用接続は別です。
この移植だけで JavaScript のブラウザー API や実サイト動作が完成するわけではありません。
現行 HTML の処理命令は独立したネイティブノードとして QuickJS に渡します。
`ProcessingInstruction` の型・対象名・データ・CharacterData 操作・複製・所有文書の
変更に対応し、親の本文取得や描画に処理命令のデータを混入させません。
現行 DOM の生成コンストラクターにも対応しますが、処理命令の疑似属性 API
（`getAttribute`、`setAttribute` 等）はまだ未対応です。
上流のソース更新時は、版・依存・ライセンスを再確認したうえで
`vendor.py` の固定リビジョンを更新します。現在の版の再採用は次の形です。

```
python ports/lexbor/vendor.py LOCAL_LEXBOR_CHECKOUT
```
