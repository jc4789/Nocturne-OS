# 目的（2026-10-25、ユーザーの言葉）
- 「we lack any meaningful parallelization of work. both kernel and browser side」
- 「our browser performance in general is abysmal … we are like an order of magnitude off」
- 基準：Floorp DDG 検索→リンク CPU 5.44 s／8.28 s、FCP 345 ms。YouTube 7.73 s／14.78 s。Windows の待機 2%
# 形
- 主スレッドの YouTube 読み込みは約 7 割が QuickJS。JS 計測（--debug-js）では、その 6 割が我々の bindings
- 済み：4ddd7914（TextEncoder・Blob・DOM getter）、894423c6（URL を Lexbor native に）
- realm の立ち上げ：最初の realm 536 ms（bootstrap の翻訳が主スレッド上）、以後 53〜87 ms／realm（ReadObject＋実行）。YouTube は 4 realm
# 今の説明（b52 の 1 回目の計測 1002 ms、bindings 606 ms）
- 上位：btoa 65＋atob 14（JS で一文字ずつ）、Intl（js_intl.js の I9・pt・p9・$）約 143、CSS 名の並べ替え（js_bootstrap.js:919、realm ごと）69、createPolicy 51
- 覆る観測：native／遅延にしても bindings の合計が下がらない（計測器の呼び出し税なら関数の数で減る）
# 次の一手
- btoa/atob を native（js_encoding.c、data URL と forgiving-base64 を共有）
- computedLonghands を初回使用まで遅らせる。Intl の locale data を遅らせられるか読む
- 最初の realm の翻訳 536 ms：翻訳スレッドで先に（起動直後、取得と並行）か、版つきのキャッシュ
- 既知の別件：YouTube が再生 44 秒で「Something went wrong」（b47 から同じ、SABR の応答 160 B が続く）