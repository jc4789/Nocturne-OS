# ブラウザー性能改善・レイアウト担当 第1報

## 目的・対象

- 基準HEAD: `9184615eb205e2e65b44c5fe0dfd12b494b34c5f`。共同の作業木へ限定変更。
- `user/libc/web/layout.c`、`layout_text_cache.h`、`webi.h`の関連定義、`doc.c`の破棄経路。
- レイアウトの安全性、絶対配置の登録、浮動要素/BFC、相対位置、overflow、font世代、文字/スタイル所有権を保ちながら無変更枝の走査を除去する。

## 製品実装

- `layout_cache_prepare`の安全性/float輸出/stretch依存を保存し、dirty祖先経路だけ再集計。`layout_invalidate`とfont失効はprepare/postも失効。
- 絶対子のある祖先は再利用しない。旧実装の祖先再利用が`abs_boxes`再登録を抑止する問題を回避し、影響しない兄弟の再利用は保つ。
- intrinsicジョブ収集は既存の確定済みpost結果とfont世代を確認し、無変更枝を省略。intrinsic幅自身もfont世代で失効し、遅延fallback読込を跨ぐ結果を確定cacheにしない。
- 相対位置はボックス自身の実cb寸法で更新してから、内側のサイズ/content shiftが不変な安全枝を省略。
- overflowの安全枝はローカル範囲を再利用し、現在の配置で親へ再publish。文書最下端も同じpostorderで集計し、`max_bottom`の別全木走査を除去。
- runのdescentを既存の行組みmetricsから保存。overflow側のrun毎`wf_metrics`は除去。
- uncached長処理へ既存のcoalesced native checkpointを接続。中断したpost処理は確定cacheやdoc extentとして公開しない。
- 文書所有の文字計測cacheは文字値、長さ、字体、太字、寸法、spacing、font世代、所有コピーしたfont choice/range/resource face/failed状態で一致確認。CSS arenaの退役ポインタを保持しない。
- cache hitでも未読込unicode subsetの需要を維持。読み取り専用probeでは需要を変更しない。
- 文字計測が実際に行われた世代で古いLRU項目を回収し、bucket配列もhysteresisで縮小。内側レイアウトが全再用された回では未訪問の文字cacheを無意味に老化させない。font世代変更で旧項目を解放。
- OOMは正しい非cache計測へfallback。同文書の再入は一時table、異文書は独立table。arena longjmp時もbusy/trap/TLSを復元。文書破棄でtableを解放。
- 動画natural size変更の`layout_invalidate(node->box)`を媒体担当へ連絡し、媒体担当が実装。

## 実行と結果

- native target clangの`-fsyntax-only`を`layout.c`/`doc.c`へ実行: 成功。最終layout.cの新規警告なし。doc.cには既存sandbox未使用関数警告2件。
- `python -X utf8 tests/check_layout_text26.py`: 最終62件成功、0失敗。実製品cache/layout_docを抽出した限定host検査。キー値/衝突、変更済み文字の所有、2049項目、OOM、font世代とwrap、再入、longjmp、fontchoices値、需要、LRU回収/配列縮小を確認。
- `python -X utf8 tests/check_layout_cache_perf.py`: 最終17件成功、0失敗。実製品prepare/失効/relative/overflow/max-bottom関数を抽出。無変更時の走査省略、dirty祖先のみ、abs登録安全性、float/BFC、親寸法変更とcached配置、clip/clamp、保存descent、中断の未commitを確認。
- 自担当ファイルの`git diff --check`: 成功。
- 初回host試験はNocturne headerとhost setjmpの`ssize_t`衝突でcompile失敗。不要headerの取り込みを除去し、製品TLS文脈はそのまま抽出した。
- 新しいoverflow fixtureの初回はtranslateのフィールド推測がcompile失敗。現定義`xy`へ訂正。clamp fixtureはdirty通知無しの直接変更をして失敗したため、実製品と同じ`layout_invalidate`を通知するよう訂正。期待寸法は弱めていない。
- 全作業木のdiff検査は主担当編集中のbrowser.c CRLF行を指摘したため、主担当へ通知し自担当を限定確認した。

## 未確認・次

- 本報の検査は実font decoder・実サイトJS・実GUI・guest描画・性能時間比較ではない。VM/全buildは主担当が所有し、この担当は起動/全buildをしていない。実サイトの速度改善/回帰なし/Hyper-V成功を主張しない。
- 次は主担当の独立確認に従い、外側cbへoverflowが漏れる境界とfontchoices/reentryの反証を第2報で確認する。全treeの媒体/入力受入は主担当に残る。
