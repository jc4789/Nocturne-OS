# data Fetch 共通経路の再考察

初回の実装方針から独立して、既存の輸送・寿命・観測との接続を再確認した。

1. **同じ facade を Worker が使うという前提は誤りだった。** Worker は独自 workerFetchBridge を持つ。Window の変更だけで両realm対応とせず、共有する責務を C の data URL processor / QuickJS wrapper に限定し、既存 Worker の Response と Abort へ接続した。
2. **同期成功 Promise は Abort 競合を落とす。** 復号と公開を task にし、同task・直後microtaskの Abort を先に処理する。公開後は既存 body/clone の signal 検査で本文読出しを拒否する。
3. **observer が存在しても headers/chunk/end だけでは不十分だった。** streaming host 上の POST XHR は upload 完了通知も必要である。この具体的反例を source 固定前に修理し、実 XHR の upload-end→response-load とバイナリ応答を両 host 条件で確認した。最初の51項目成功を最終53項目成功と混同しない。
4. **MIME 原文を header に連結しない。** canonical MIME record の serialization だけを immutable Headers に渡す。パラメーターを捨てて Wasm streaming の MIME 検査を誤って通すことも避ける。
5. **decoder の成功は site の成功ではない。** Puter の実 zstd loader 再試行、JSPI の警告/Launch 状態、その後の memory / Worker / graphics は別の境界である。JSPI の名前だけ追加、強制 Launch、代替画面は実施しない。

runtime 課金 ArrayBuffer の生成と tuple 構築が失敗すれば、processor の malloc 結果と一時 JS 値を解放し例外を返す。source/画像/CSS の既存decoderも同じ厳格な復号を使い、不正な base64 を黙って成功へ変換しない。画像の失敗長は0へ初期化し、CSS の malformed と OOM を区別した。

独立 cross-compile が検出した `doc.c` の `hexv` 未宣言は実際の退行だった。data decoder の除去時に fragment lookup の別利用を見落としていた。navigation の既存責務に hex helper を残し、修理後は doc.o だけを実 target へ再コンパイルした。構文補助の成功を全 object build 成功へ昇格しなかったことで、この失敗を別に検出できた。

新 guest / 最新媒体 / 実サイト / TCC backend の結果はこの補助記録へ流用せず、実行した担当の後続証拠を待つ。
