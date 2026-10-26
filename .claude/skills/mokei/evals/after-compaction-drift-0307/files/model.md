# 目的（2026-10-25、ユーザーの言葉）
- 「we lack any meaningful parallelization of work. both kernel and browser side」
- 「our browser performance in general is abysmal … we are like an order of magnitude off」
- 基準：Floorp DDG 検索→リンク CPU 5.44 s／8.28 s、FCP 345 ms。YouTube 7.73 s／14.78 s。Windows の待機 2%
# 形
- 済み（コミット）：:has() の無効化（305fc636）、配置のキャッシュの下余白（1b0dc75e）
- 書いた（未コミット）：js_compile.c/h（翻訳スレッド2本、SHA-256(origin,URL,中身) で記憶、64 KiB 未満は文書で翻訳）。paint.c の不透明な背景の白塗り省略
- b40 の観測：戻る・進むで 10.8 MB 台本の取得完了→実行開始が 570 ms → 105 ms（記憶が効く）。主の翻訳は消えた。初回は翻訳スレッドが約 1.1 s（b38 の主の翻訳 0.5 s の倍）で、取得完了→実行が 1.38 s に伸びた
# 今の説明
- 翻訳スレッドの遅さ：memcpy が 57%（qrealloc の伸長 158、最後の写し 68、dbuf_put 48、js_strndup 23）と頁の割り当て。QuickJS が関数ごとに元の文を写すので、入れ子の深さぶん同じ文が何重にも bytecode に入る → 共有の元の文＋位置にすれば、翻訳・書き出し・読み戻し・実行環境のメモリーが全部減る
- 読み込み中の主：描画が約 45%（巻き戻しの写し 283、窓への写し 239、白塗り 225、映像 317）、JS は約 20%。描画が 2 秒に 272 回（136 fps）＝表示の拍に合わせていない
# 次の一手
- QuickJS：入れ子の関数の元の文を一つの共有の文字列＋位置で持つ（README.nocturne.md に記す）。覆る観測：bytecode が元の文の数倍でない
- browser.c：頁の描画を表示の拍（UI_EVENT_FRAME）一回に一度へ。覆る観測：描画回数が減らない、入力の遅れが出る
- 記録の条件：翻訳スレッドが 250 ms 以上でも「Script task profile」を出す