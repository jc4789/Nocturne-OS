# 動的 script 検出の共通修理：第一報

## 範囲と確定した原因

- 担当は `user/libc/web/js.c` の動的 script／meta CSP／frame 接続処理。DOM、CSS、通信、描画の担当差分は変更していない。
- 旧 `native_dom_complete` は普通の append／replace ごとに挿入先の親部分木を再走査し、その後、実行済みも含む `s->scripts` 全履歴を走査していた。葉を順に追加する処理で、必要な挿入部分木より累積親の大きさに仕事量が依存した。
- 完了済み文書でも `web_js_tick` は DOM 変更のない timer／rAF ごとに文書全体を再走査していた。

## 製品変更

- `inserted_roots_capture` は移動前の実挿入 forest から script／未処理 meta／frame の静的候補だけを保持する。fragment が空になっても候補は失われない。
- shadow を含む順序の非再帰走査。template の内容へ入らない。通常の葉では候補用のヒープ確保なし。8 個はインライン初期領域であり上限ではなく、アドレス可能量まで動的確保する。
- `dynamic_inserted` は候補ごとに現在の接続と入力世代を確かめ、prepare 直後に classic inline を同期実行する。既存 script 全履歴は探索しない。module／外部 script の従来の準備・非同期処理は保全する。
- script 自身へ Text を挿入したときの children-changed 境界も同じ準備経路へ接続。空 fragment／null の no-op は移動前の `changed` で区別する。
- parser／ネイティブ API からの変更を拾う全体 fallback は残し、`dom_revision` が同じ idle tick でのみ省く。stream reset／encoding restart ではキャッシュを無効化する。
- CE／Range／MutationObserver／slot の既存フック位置は変更していない。既存 debug 用 DOM opcode サンプリングと wrapper lifetime の主担当差分を保全した。

## 支持確認と受入境界

実行：`python -X utf8 tests/check_js_dynamic_queues.py --insertion-only`。

- 製品の candidate capture／prepare／insert helper 本文を抽出し、allocator／tree／prepare callback／clock だけを fixture とした host 確認で **19 条件、0 失敗**。
- 1500 葉 append の走査件数が 1500 で追加確保なし、A が C を再入 append する `ACB`、A が B を切断、prepare／execute 中の世代変更、静的候補順、shadow／template、CSP／frame、33 候補の grow、OOM／取消、12000 深さ、空 fragment を確認。
- ログ：`build/goal-20261009/script-insertion-host.log`。ログの製品 hash は UTF-8 読み出し後の改行正規化された抽出元。実ファイル hash は下記。
- freestanding target の `js.c`／`scriptlifecycletest.c` 構文確認、`diff --check` は成功。native lifecycle の追加作者 JS は `node --check` による構文確認のみ成功。
- 既存 `script-lifecycle` native group の従来 15 条件を保持して 7 条件を追加し、成功 marker を **22** へ強化。native 実行・完全媒体・実サイト時間・入力／scroll 受入は主担当待ちで、ここでは成功扱いしていない。

## 凍結ファイル hash

```text
user/libc/web/js.c ed02401597d85ebbe1d9228a5876e09cd04f0011e8750b846db654a0005fe87a
tests/scriptlifecycletest.c 90f2cc2e94cca1f470dfe8d69b55b2aeb39fdbda9f6f74e92527756c0d6b558e
tests/check_js_dynamic_queues.py 77c92ba69d439a826079b669722fcb6fedf2b7d7c591680ab96908deec347cc9
tests/runtests.c d0fe34edc75b71aefc7b28c2f6ec24a1d3a3379ccac776001bc4a0526bd866e4
```

次は主担当の native `script-lifecycle` と同条件の百度実操作・長い callback 実時間で確認する。host の仕事量削減を実サイト全体の速度改善へ昇格させない。
