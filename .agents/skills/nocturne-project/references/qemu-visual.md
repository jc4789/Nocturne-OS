# QEMU画面確認の現場手順

## まず今の画面を取得する

既に起動している作業用QEMUを確認し、利用可能なUI screenshotまたは既存monitor接続で、対象全画面の撮影を一括で要求する。その結果を画像ツールでまとめて開いて目視する。撮影目的でOSを再ビルドしたり、各VMの長い観察期間の終了を順番に待ったりしない。

ネイティブUIツールを使う場合は、対象windowを実際のsnapshotから選び、そのツールの現行APIを読み込む。スクリーンショットのためにwindowを前面化する必要がないツールなら、不要な切替を避ける。

HMPでは既存monitorで次のcommandを送れる。

```text
screendump build/nocturne-platform/<run>/before.ppm
sendkey pgdn
screendump build/nocturne-platform/<run>/after.ppm
```

これは複数commandの例であって、単一画面ごとに工具呼出し／新process／再起動を要求するものではない。利用中の接続で各commandの完了を確認する。入力後の描画は現在のframeで確認し、機械的な固定sleepだけでreadyと断定しない。

- QMPを使える場合は既存QMPの`screendump`を使ってよい。QMPとHMPのcommand形式を混同しない。
- HMP新規接続時は初期の`(qemu) `promptを読み切ってからcommandを送る。古いpromptを撮影完了と誤認しない。
- monitorの既存所有者を確認する。同じTCP monitorへ競合接続しない。既存ハーネスが接続を持つ場合はその接続の撮影機能、またはネイティブUI screenshotを使う。撮影の標準手順としてハーネス／QEMUを強制終了しない。
- HMPの相対保存先はQEMUのcwd基準。絶対pathを使う場合は空白を正しく引用する。画像ファイルの取得完了を確認してから開く。
- 複数VMの独立接続では撮影要求と画像の読込みをbatch化できる。一つの接続に互いのcommand応答を混在させない。
- PPMは対応する画像viewerで直に開くか、可逆のPNG形式変換をすぐ行う。crop、合成、加工した画像だけで実画面の受入をしない。全画面の原本を残す。

## このcheckoutの既存ハーネス

`scripts/platform_qa.py`は隔離boot/data、QEMU条件、撮影・キー入力の補助。**Pythonハーネスを使うこと自体は必須ではない。** 起動が必要な場合だけ現行引数を確認する。例：

```text
python -X utf8 scripts/platform_qa.py --label <unique-run> --url <requested-url> --accel whpx --memory 4096 --resolution 1920x1080x32 --seconds <observation-period>
```

既存runは`build/nocturne-platform/<label>/metadata.json`にPID、起動引数、monitor、image identityがある。記録のPIDだけで操作せず、現在のprocessと専用image/command lineの一致を確認する。ユーザーの既存VMを終了しない。

2026-10-10時点の具体的な罠：

- `--ready-marker`待機中は通常の定期撮影に入らない。markerが来ないまま画面を見ずに待ち続けない。初回はmarkerなしの現画面取得か、独立した即時撮影機能を選ぶ。
- `--defer-screen-png`はPNG化を終了まで保留する。終了前にPPMを開く経路がないなら、目視優先の作業では選ばない。
- `--guest-file observer.js`はFATの`/tests`へコピーされ、OSからの絶対pathは **`/data/tests/observer.js`**。ブラウザー引数は`--console-file /data/tests/observer.js`。`/tests/observer.js`ではない。
- startup console入力はconsoleを開いてfocusする。PgDnはconsole履歴送りになり得る。実画面でconsoleを閉じ、ページへfocusしてからscrollする。
- `--scroll-pages`を指定しただけでは合格にならない。前後画像、本文の移動、必要に応じてread-only scroll位置を自分で確認する。

ハーネスの現行実装が変わっていればソースを必要範囲だけ確認する。この参考を根拠に存在しないAPIや引数を想像しない。

## 目視記録の最小形

```text
サイト／操作：
実URL・run・image identity：
見た画像（絶対path）：
実際に見えた本文と配置：
native操作と前後の変化：
合格／失敗／未確認：
次の製品修正／未実施の境界：
```

HTTP 200、DOMContentLoaded、serial PASS、scrollYだけを「見た本文と配置」の代わりに書かない。参考hostブラウザーとNocturneを区別する。表示変化が読み込みの進行なのかscrollなのかも切り分ける。
