# ブラウザー実サイト検証（2026-10-06）

## 結論

**部分修正。指定4サイトの正常動作は未達成。** QuickJSを組み込めたこと、ビルド成功、単体試験成功を、実サイト対応の証明として扱わない。

Nocturneの既存カーネル・ABIは変更していない。POSIX互換層の追加、外部ブラウザーへの置換、サイト別の例外回避は行っていない。Hyper-Vはユーザーが検証する。今回の最終検証はQEMUのみで、Hyper-V第2世代の動作確認とは区別する。

## 実サイトでの結果

最終コードをビルドした `build/nocturne.img` を、各サイトごとに独立したQEMUで起動した。各VMは512 MiB、pc/IDE/e1000、ブート媒体はsnapshot、データは新規の隔離ディスク。ユーザーの永続データディスクは使用していない。

Nocturne自身の `browser --debug-js URL` が公開HTTPSからHTMLとJavaScriptを取得・実行した。ローカルHTML、保存ページ、ホストのブラウザーは受け入れ試験の代用にしていない。

| 実URL | 画面・実行結果 | 判定 |
|---|---|---|
| `https://deepmind.google/` | 修正前のgzipの文字化けを解消。さらに背景に隠れていたナビ、見出し、Read blogが表示された。ただし書体・配置に差があり、主スクリプトは `history.scrollRestoration` 参照時に停止。Promiseの `split` 例外も残る。 | 不合格 |
| `https://duckduckgo.com/` | Liteへ切り替えていない。ロゴ・検索欄の一部は出るが、サイドメニューの表示が崩れる。Cookie読み出しの `split`、`URLSearchParams`、`URL` で例外。 | 不合格 |
| `https://www.youtube.com/` | 本文内に混入していたHTTPチャンクサイズ文字列と、それによるJS構文エラーを解消。しかし白い画面と巨大なメニューアイコンのまま。`responseStart`、`Image`、複数のDOM/API呼び出しで例外、その後 `out of memory`。 | 不合格 |
| `https://chatgpt.com/` | 公開アクセスの確認ページで `crypto` 未定義の例外。アプリ本体に到達しておらず、その描画・ログイン・送信は検証できていない。認証回避は行っていない。 | 不合格 |

実行時のJSファイル名・行・列はシリアルログに保存した。確認ページなどの配信内容は時点・アクセス条件で変わる。

今回のローカル証拠は `build/nocturne-audit/` にある。各 `*-final/` の `page.png`、`console.png`、`qa-serial.log` は同じ最終ビルドの実Nocturne画面・ログである。

- `deepmind-final/`
- `ddg-full-final/`
- `youtube-final/`
- `chatgpt-final/`

直前のDeepMind実操作では、Modelsへのホバーでメニューは展開せず、クリックは通常リンクとして `/models/` に遷移した。これはカスタム要素によるJSナビゲーションの成功とは判定していない。証拠は `deepmind-stacking-fixed/models-hover.png` と `models-click.png`。後者は遷移直後であり、遷移先の読み込み完了の証拠ではない。

## 修正した共通不具合

### HTTP転送と圧縮

- YouTubeの実応答ではヘッダーが4568バイト、`Transfer-Encoding` が4538バイト目にあり、4096バイトの公開ヘッダー保存領域から落ちていた。そのためチャンク境界がHTML/JSへ混入していた。
- 転送形式と内容エンコーディングをヘッダー受信中に解析し、保存領域の長さから独立させた。
- DeepMindのgzip応答を復号する。既存のDEFLATE実装を利用し、サイズ制限、CRC、ISIZE、連結メンバー、切断・不正入力を検査する。未対応エンコーディングは失敗として報告する。
- カーネル、TLS、サイト別処理は変更していない。gzip応答は検証後に渡すため、圧縮されたストリームの逐次通知までは対応していない。

対象: `user/libc/http.c`、`user/libc/third_party_img.c`、`tests/httptest.c`。

### DOMとイベント

- native DOMラッパーに実体に対応したprototypeを与え、DocumentがElement専用属性を誤って継承する不整合を修正した。
- remove/once後のイベントリスナー再登録を修正した。古いAbortSignalが新しい登録を削除しないよう、登録単位で寿命を管理する。
- DOMの同一性とnativeブランドを保持する。Custom Elements等を空の関数で偽装していない。

対象: `user/libc/web/js.c`、`js_bootstrap.js`、生成物 `js_bootstrap.inc`、`tests/jstest.c`。

### 描画順とクリップ

- staticのflex/grid itemにも明示的なz-indexを適用した。
- 全positioned子孫を平坦化していた描画順を、明示的なスタッキングコンテキストごとの順序へ修正した。
- 独立レビューで、親のoverflowがviewport固定子・外側を配置基準とするabsolute子まで切る退行を発見。QEMUで再現し、描画順と配置基準のクリップ計算を分離して修正した。
- ピクセルと同じ位置のヒット判定を検査した。sticky、transform、opacityのみのコンテキスト等を含むCSS全面対応ではない。

対象: `user/libc/web/paint.c`、`tests/webtest.c`。

### 診断とビルド

- `browser --debug-js URL` で例外を標準エラーにも出す。画面のコンソールはF12。
- HTTPワーカー起動失敗をpipe/open/spawnとerrnoで区別する。プロセスABIは変えていない。
- `Makefile` の既定目標を `all` にした。
- `build.ps1` は呼出し元ディレクトリに依存せず自身のリポジトリを使い、makeの終了コードを返す構成へ修正した。別ディレクトリからBash部分を実行し、正常ビルドの終了0と存在しない目標の終了2を確認した。外側のPowerShellランチャー自体の実行は未検証。
- `build/nocturne.img`、`build/nocturne.iso`、`build/nocturne.vhdx` を再生成した。Hyper-V用コピーや稼働VMの媒体を更新したとの主張ではない。

## 補助的な回帰検査

これは上記の実サイト受け入れ試験とは別枠。

```
python scripts/test.py --quick --no-net --timeout 600 web
```

最終ビルドのQEMU内でNocturneのtcc/libcを使用した結果:

- webtest: 99 checks、0 failed
- jstest: 423 checks、0 failed
- httptest: 72 checks、0 failed
- HTTPワーカーの試験も成功
- TESTS DONE: pass=4、fail=0、skip=0
- FAT検査成功

記録: `build/nocturne-audit/integrated-web-regressions-final.log`。
クリップ修正前には追加ケースの6検査が失敗し、修正後は成功した。修正前ログは `paint-clip-regression-before.log`。

HTTPについては、4サイトの取得本文を独立したPythonのチャンク/gzip復号と比較した。加えてホスト側の723ケースのDEFLATE境界検査を通した。これも画面・操作の合格の代わりにはしない。

## 残る実装課題

QuickJSはJavaScript言語の実行エンジンであり、URL、Cookie、History、Custom Elements、観測API、画像コンストラクター、暗号API等のブラウザー機能はNocturne側の実装が必要となる。今回実際に停止した境界は上表のとおり。列挙したAPIを実装すれば全サイトが動くと保証しているわけではない。

YouTubeではJavaScriptの64 MiBヒープ制限にも達した。単に上限を増やして対応済みにせず、実際の消費とリソース寿命の検証が必要。動画再生を成功確認した事実はない。

以後もNocturneの既存機能へ適合させる。ダミーAPI、空Cookie、固定の計時値、弱い乱数等で例外だけを消して、実装完了と扱わない。
