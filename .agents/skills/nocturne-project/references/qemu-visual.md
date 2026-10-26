# QEMU画面確認の現場手順

## まず今の画面を取得する

既に起動している作業用QEMUを確認し、利用可能なUI screenshotまたは既存monitor接続で、対象全画面の撮影を一括で要求する。その結果を画像ツールでまとめて開いて目視する。撮影目的でOSを再ビルドしたり、各VMの長い観察期間の終了を順番に待ったりしない。

ネイティブUIツールを使う場合は、対象windowを実際のsnapshotから選び、そのツールの現行APIを読み込む。スクリーンショットのためにwindowを前面化する必要がないツールなら、不要な切替を避ける。

HMPでは既存monitorで次のcommandを送れる。

```text
screendump "D:/.../build/qa/<run>/before.png" -f png
sendkey pgdn
screendump "D:/.../build/qa/<run>/after.png" -f png
```

これは複数commandの例であって、単一画面ごとに工具呼出し／新process／再起動を要求するものではない。利用中の接続で各commandの完了を確認する。入力後の描画は現在のframeで確認し、機械的な固定sleepだけでreadyと断定しない。

- QMPを使える場合は既存QMPの`screendump`を使ってよい。QMPとHMPのcommand形式を混同しない。
- HMP新規接続時は初期の`(qemu) `promptを読み切ってからcommandを送る。古いpromptを撮影完了と誤認しない。
- monitorの既存所有者を確認する。同じTCP monitorへ競合接続しない。既存ハーネスが接続を持つ場合はその接続の撮影機能、またはネイティブUI screenshotを使う。撮影の標準手順としてハーネス／QEMUを強制終了しない。
- HMPの相対保存先はQEMUのcwd基準。絶対pathを使う場合は空白を正しく引用する。画像ファイルの取得完了を確認してから開く。
- 複数VMの独立接続では撮影要求と画像の読込みをbatch化できる。一つの接続に互いのcommand応答を混在させない。
- PPMは対応する画像viewerで直に開くか、可逆のPNG形式変換をすぐ行う。crop、合成、加工した画像だけで実画面の受入をしない。全画面の原本を残す。

## 一台のQEMUをコマンドラインで直接操作する（標準）

ユーザー指示（2026-10-16）：QEMUは同時に一台だけ。`C:\Program Files\qemu`をWHPXで使い、Python一発スクリプトではなくコマンドラインから直接操作する。

1. 既存のQEMU processがないか確認する。あればそれを使い、二台目を起動しない。
2. 窓付きで起動する。ディスクは一台だけ（ESPとC:を持つGPTの`build/nocturne.img`）。二台目のdataディスクは存在しない。`snapshot=on`ならビルド成果物を汚さないので、毎回コピーしない。

   ```text
   "C:/Program Files/qemu/qemu-system-x86_64.exe" -M pc -m 4096M -smp 4 -accel whpx -vga none -device VGA,xres=1920,yres=1080
     -serial file:<run>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait
     -device ahci,id=ahci
     -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on
     -device ide-hd,drive=boot,bus=ahci.0,bootindex=0
     -netdev user,id=qa -device e1000,netdev=qa
   ```

   画面の大きさは起動設定ではなく、仮想モニターのEDID（`xres`・`yres`）が決める。ゲストのファームウェアとLimineがその推奨モードを選び、カーネルが`video: monitor … at … Hz`と記録する。`-vga std`だけではEDIDの推奨がQEMUの既定の大きさになる。

3. HMPへ一行ずつ送る（PowerShellの`TcpClient`やncなど、接続を保持して送るだけの小さな手段でよい）。

   ```text
   screendump "D:/.../shots/now.png" -f png
   sendkey ctrl-l
   mouse_move 400 300
   mouse_button 1
   mouse_button 0
   ```

4. 保存したPNGをその場で自分で開いて見る。操作→撮影→目視を短く繰り返し、固定秒数の観察期間を設けない。

- **ディスクは標準でAHCIに接続する**（上記の`-device ahci`＋`ide-hd,bus=ahci.0`。実機のSATAと同じ経路）。カーネルはAHCI・NVMe・旧IDE（ATA）のどれからでもC:を読む。起動はserial.logの`store: C: "Nocturne" on sata0`で確かめる。数秒で起動しなければ異常として原因を扱う。
- **NVMeで試すときはUEFIで起動する。** QEMUのSeaBIOSはNVMeディスクを起動できない（`could not read the boot disk`）。`-drive if=pflash,format=raw,readonly=on,file=C:/Program Files/qemu/share/edk2-x86_64-code.fd`と、`edk2-i386-vars.fd`の書込み用コピーを2台目のpflashに付け、`-device nvme,serial=nocturne,drive=boot,bootindex=0`にする。
- HMPの`screendump`の保存先は**スラッシュ区切りの絶対path**にする（`"D:/.../now.png"`）。バックスラッシュのpathは黙って保存されない。
- `C:\Tests\autorun.sh`があれば、initがそれを実行し、終わると電源断する（`scripts/test.py`・`platform_qa.py`のコピーだけに置く）。通常の`build/nocturne.img`にはないので、対話確認ではデスクトップのまま操作できる。
- `snapshot=on`ではゲスト内の変更は残らず、ブラウザーprofileは毎回初回訪問者になる。サイトのA/B表示差もありうるので、参照画像と構成要素を照合して判断する。
- **電源断・再起動をまたぐ保存の確認**は、`snapshot=on`ではなく使い捨てのqcow2 overlayで行う：`qemu-img create -f qcow2 -b "<絶対path>/build/nocturne.img" -F raw build/qa/persist.qcow2`（`tools\msys64\ucrt64\bin\qemu-img.exe`）。ゲストで書いた直後にHMPの`quit`で電源を切り、同じoverlayで再起動して確かめる。nocturne.imgを作り直したらoverlayも作り直し、終わったら消す。
- WHPXでは`system_reset`後にVMが`paused (prelaunch)`のまま止まり、`cont`でも戻らない。再起動は`quit`してから起動し直す。
- 終了はHMPの`quit`。この作業で起動したQEMUだけを止める。

## このcheckoutの既存ハーネス

`scripts/platform_qa.py`は隔離した起動ディスクのコピー（AHCI、`snapshot=on`）、QEMU条件、撮影・キー入力の補助。**Pythonハーネスを使うこと自体は必須ではない。** 起動が必要な場合だけ現行引数を確認する。例：

```text
python -X utf8 scripts/platform_qa.py --label <unique-run> --url <requested-url> --accel whpx --memory 4096 --resolution 1920x1080 --seconds <observation-period>
```

既存runは`build/nocturne-platform/<label>/metadata.json`にPID、起動引数、monitor、image identityがある。記録のPIDだけで操作せず、現在のprocessと専用image/command lineの一致を確認する。ユーザーの既存VMを終了しない。

2026-10-10時点の具体的な罠：

- `--ready-marker`待機中は通常の定期撮影に入らない。markerが来ないまま画面を見ずに待ち続けない。初回はmarkerなしの現画面取得か、独立した即時撮影機能を選ぶ。
- `--defer-screen-png`はPNG化を終了まで保留する。終了前にPPMを開く経路がないなら、目視優先の作業では選ばない。
- `--guest-file observer.js`は隔離コピーのC:の`C:\Tests`へ置かれ、OSからの絶対pathは **`C:\Tests\observer.js`**。ブラウザー引数は`--console-file C:\Tests\observer.js`。
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
