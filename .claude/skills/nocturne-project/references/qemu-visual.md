# QEMU の起動・撮影・操作

## 一台を窓付きで起動する（標準）

ユーザーの指示（2026-10-16・2026-10-27）：QEMU は同時に一台。`C:\Program Files\qemu` を WHPX で使い、コマンドラインから直接操作する。4 コア、4096 MiB、2560×1440。

1. 動いている QEMU が無いかを見る。あればそれを使い、二台目を起動しない。撮影のために再ビルド・再起動しない。
2. 起動する。ディスクは一台（ESP と C: を持つ GPT の `build/nocturne.img`）。`snapshot=on` なら媒体を汚さないので、毎回写さなくてよい。

   ```text
   "C:/Program Files/qemu/qemu-system-x86_64.exe" -M pc -m 4096M -smp 4 -accel whpx -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=32
     -serial file:<run>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait
     -device ahci,id=ahci
     -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on
     -device ide-hd,drive=boot,bus=ahci.0,bootindex=0
     -netdev user,id=qa -device e1000,netdev=qa
   ```

   画面の大きさは起動設定ではなく仮想モニターの EDID（`xres`・`yres`）が決める。ファームウェアと Limine がその推奨モードを選び、カーネルが `video: monitor … at … Hz` と記録する。`-vga std` だけでは EDID の推奨が QEMU の既定の大きさになる。

3. HMP へ一行ずつ送る（PowerShell の `TcpClient` など、接続を保持して送る小さな手段でよい）。新規接続では初期の `(qemu) ` prompt を読み切ってから送る。

   ```text
   screendump "D:/.../shots/now.png" -f png
   sendkey ctrl-l
   sendkey pgdn
   mouse_move 400 300
   mouse_button 1
   mouse_button 0
   info registers -a
   ```

4. 保存した PNG をその場で自分で開いて見る。操作→撮影→目視を短く繰り返し、固定秒数の観察期間を設けない。

## 落とし穴

- **AHCI に接続する**（上の `-device ahci` と `ide-hd,bus=ahci.0`。実機の SATA と同じ経路）。起動は serial の `store: C: "Nocturne" on sata0` で確かめ、数秒で出なければ異常として原因を扱う。
- **NVMe で試すときは UEFI で起動する。** SeaBIOS は NVMe を起動できない。`-drive if=pflash,format=raw,readonly=on,file=C:/Program Files/qemu/share/edk2-x86_64-code.fd` と `edk2-i386-vars.fd` の書込み用の写しを二つ目の pflash に付け、`-device nvme,serial=nocturne,drive=boot,bootindex=0`。
- `screendump` の保存先は**スラッシュ区切りの絶対 path**。バックスラッシュの path は黙って保存されない。
- 同じ TCP monitor へ競合して接続しない。既存のハーネス（platform_qa）が接続を持つなら、その撮影機能を使う。撮影のためにハーネスや QEMU を強制終了しない。
- `C:\Tests\autorun.sh` があれば init がそれを走らせ、終わると電源を切る（test.py・platform_qa の写しにだけ置く）。通常の `build/nocturne.img` には無いので、デスクトップのまま操作できる。
- `snapshot=on` ではブラウザーの profile は毎回初回訪問者になる。サイトの A/B の違いもありうるので、参照画像の部品と照合して判定する。
- **再起動をまたぐ保存の確認**は使い捨ての qcow2 overlay で：`qemu-img create -f qcow2 -b "<絶対path>/build/nocturne.img" -F raw build/qa/persist.qcow2`（`tools\msys64\ucrt64\bin\qemu-img.exe`）。書いた直後に HMP の `quit` で切り、同じ overlay で起動し直す。nocturne.img を作り直したら overlay も作り直す。
- WHPX では `system_reset` の後に `paused (prelaunch)` のまま止まり、`cont` でも戻らない。再起動は `quit` してから起動し直す。
- ゲストが固まって見えたら `info registers -a` で CPU0 の RIP と RBP の鎖（`x /2gx <rbp>`）を取り、`build/kernel.elf` を llvm-symbolizer に引く（呼出しは BSP が受け持つので、CPU0 を先に見る）。
- 終了は HMP の `quit`。この作業で起動した QEMU だけを止める。

## platform_qa.py を使うとき

```text
python -X utf8 scripts/platform_qa.py --label <一意の名> --accel whpx --memory 4096 --cpus 4 --resolution 2560x1440 --url <URL> --seconds <秒>
```

- 既定は tcg・2048 MiB・1920×1080・60 秒・画面の間隔 30 秒。`--accel whpx` を忘れると遅い回になる。
- `--command "<ゲストのコマンド>"`（複数可）、`--key <QEMU のキー>`、`--timed-key 秒:キー`、`--cmdline prof`（カーネルの標本）、`--guest-file <path>`（写しの `C:\Tests\<名>` へ置く。ブラウザーには `--console-file C:\Tests\<名>`）、`--expect`・`--reject`（serial の文字列で判定）、`--stop-when-expected`。
- 出力は `build/nocturne-platform/<label>/`（serial.log、画面、`metadata.json` に PID・引数・monitor・媒体の識別）。記録の PID だけで操作せず、今の process と一致するかを見る。
- `--ready-marker` 待ちの間は定期撮影に入らない。marker が来ないまま画面を見ずに待ち続けない。`--defer-screen-png` は PNG 化を終了まで保留するので、目視を先にする作業では使わない。
- `--scroll-pages` を指定しただけでは合格にならない。前後の画像と本文の移動を自分で確かめる。startup の console 入力は console を開いて focus するので、PgDn は console の履歴送りになりうる。
- 引数が変わっていればソース（`scripts/platform_qa.py` の `add_argument`）を見る。無い引数を想像しない。

## 目視記録の最小形

```text
サイト／操作：
実URL・run・媒体の版：
見た画像（絶対path）：
実際に見えた本文と配置：
操作と前後の変化：
console・serial の行（要点）：
合格／失敗／未確認：
次の製品修正／未実施の境界：
```

HTTP 200、DOMContentLoaded、serial の PASS、scrollY だけを「見た本文と配置」の代わりに書かない。表示の変化が読み込みの進行なのかスクロールなのかも切り分ける。
