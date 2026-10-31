# QEMU の起動・撮影・操作

## 窓付きで起動する（標準。二台まで）

`C:\Program Files\qemu` を WHPX で使い、コマンドラインから直接操作する。4 コア、4096 MiB、2560×1440（規則は AGENTS.md「確かめ方」）。

1. 動いている QEMU の数を見る（`powershell -c "Get-Process qemu-system-x86_64"`）。一台あればそれを使うか、二台目を別のポートで起こす（下）。二台あれば三台目を起こさない。撮影のために再ビルド・再起動しない。
2. 起動する。serial.log の置き場所は、この会話の作業場（システムプロンプトにある scratchpad の場所）。build の下に置かない。

   ```text
   "C:/Program Files/qemu/qemu-system-x86_64.exe" -M q35 -m 4096M -smp 4 -accel whpx -cpu host -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=32
     -drive "if=pflash,format=raw,readonly=on,file=C:/Program Files/qemu/share/edk2-x86_64-code.fd"
     -drive if=pflash,format=raw,file=<作業場>/vars.fd
     -serial file:<作業場>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait
     -qmp tcp:127.0.0.1:45455,server,nowait
     -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on
     -device nvme,serial=nocturne,drive=boot,bootindex=0
     -netdev user,id=qa -device e1000,netdev=qa
     -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
   ```

   機械は実機の形：q35（PCIe の ECAM）、UEFI（edk2）、NVMe。`<作業場>/vars.fd` は起動ごとに `C:/Program Files/qemu/share/edk2-i386-vars.fd` を写した UEFI の変数（写す命令は `nocturne.img` と別の行に。同じ行だとフックが媒体の写しと読んで止める）。SATA の道を試すときは NVMe の代わりに `-device ahci,id=ahci` と `-device ide-hd,drive=boot,bus=ahci.0,bootindex=0`。入力は xHCI の USB のキーボードと tablet（PS/2 は無い）。キーは HMP の `sendkey`、指は QMP の `input-send-event`（下の 4）。

   `-display none`・`-nographic` を付けない。ユーザーはその窓を見て、自分でも触る。

   画面の大きさは起動設定ではなく仮想モニターの EDID（`xres`・`yres`）が決める。ファームウェアと Limine がその推奨モードを選び、カーネルが `video: monitor … at … Hz` と記録する。`-vga std` だけでは EDID の推奨が QEMU の既定の大きさになる。

3. **窓が出たことを確かめる。** `Get-Process qemu-system-x86_64 | Select-Object Id,MainWindowHandle,MainWindowTitle`。MainWindowHandle が 0 なら窓は無い。`quit` で止めて、起動の引数を見直す。
4. HMP へ一行ずつ送る（PowerShell の `TcpClient` など、接続を保持して送る小さな手段でよい）。新規接続では初期の `(qemu) ` prompt を読み切ってから送り、各応答の `(qemu)` を待つ。

   ```text
   screendump "<作業場>/now.png" -f png
   sendkey ctrl-l
   sendkey pgdn
   info registers -a
   ```

   指は QMP（TCP 45455）へ。一行目の挨拶を読み、`{"execute":"qmp_capabilities"}` を送ってから、各事象を一行の JSON で送り、応答を一行ずつ読む：

   ```text
   {"execute":"input-send-event","arguments":{"events":[{"type":"abs","data":{"axis":"x","value":2047}},{"type":"abs","data":{"axis":"y","value":2047}}]}}
   {"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":true,"button":"left"}}]}}
   {"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"down":false,"button":"left"}}]}}
   ```

   値は 0〜32767 に画面を写したもの（画素 x なら x×32767/幅）。二度押しは押し・放しを 0.1 秒ほどの間で二回。起動の直後の最初の事象は落ちることがあるので、先に一度動かす。

5. 保存した PNG をその場で自分で開いて見る。操作→撮影→目視を短く繰り返し、固定秒数の観察期間を設けない。固定秒数の一発スクリプト（`platform_qa.py --seconds 330` のような）や、デスクトップの computer-use の撮影で代用しない（黒画面・待ち過ぎになった）。

## 二台目を起こすとき

- 前後を同じ時に並べる（前の写し `build/mae.img` と今の `build/nocturne.img`）、または同じ媒体の二つの頁を並べるとき。`-monitor tcp:127.0.0.1:45456`、`-qmp tcp:127.0.0.1:45457`、`-serial file:<作業場>/serial-2.log`、vars は `<作業場>/vars-2.fd` にし、他は同じ。`snapshot=on` なので同じ img を二台が読んでよい。
- 速さを測る回は一台だけ。もう一台は `quit` してから測る（RAM と CPU を取り合うと値が歪む）。
- 二台とも、この作業が起動したものは終わりに `quit` する。

## 媒体を組み直したとき

`qemu-image` は同じ `build/nocturne.img` を上書きする。動いている QEMU は古い中身のまま動き続けるので、HMP の `quit` で止めて、同じ命令で起動し直す。前を残すなら、組み直す前に `build/mae.img` へ写す（二つ目の媒体。三つ目は作らない。前の写しが要らなくなったら上書きで使い回す）。

## 落とし穴

- 起動は serial の `store: C: "Nocturne" on nvme0` で確かめる。UEFI と Limine の待ちを含めて十数秒で出なければ、待たずに原因を見る。
- `screendump` の保存先は**スラッシュ区切りの絶対 path**。バックスラッシュの path は黙って保存されない。
- 同じ TCP monitor へ競合して接続しない。一回の接続で命令を順に送り、終わったら閉じる。
- `C:\Tests\autorun.sh` があれば init がそれを走らせ、終わると電源を切る。通常の `build/nocturne.img` には無い（試験の道具が写しの媒体に置いていたもの）ので、デスクトップのまま操作できる。
- `snapshot=on` ではブラウザーの profile は毎回初回訪問者になる。サイトの A/B の違いもありうるので、参照画像の部品と照合して判定する。
- **再起動をまたぐ保存の確認**は、使い捨ての qcow2 の overlay を作業場に作って行う：`qemu-img create -f qcow2 -b "<絶対path>/build/nocturne.img" -F raw <作業場>/persist.qcow2`（`tools\msys64\ucrt64\bin\qemu-img.exe`）。`snapshot=on` を外し、`-drive id=boot,file=<作業場>/persist.qcow2,format=qcow2,if=none` で起こす。差分だけが overlay に入り、`build/nocturne.img` は変わらない。書いた直後に HMP の `quit` で切り、同じ overlay で起動し直す。nocturne.img を組み直したら overlay は捨てる。build の下には置かない。
- WHPX では CPU の reset が戻らない：`system_reset` も S3 からの起こし（`system_wakeup`）も、stderr に `failed to get xsave state` と `WHPX: Unexpected VP exit code 4` を出して `paused` で止まり、`cont` でも戻らない。再起動は `quit` してから起動し直す。S3 からの復帰は WHPX では確かめられない（Hyper-V か実機）。
- S3 の眠りの間、窓の題に `[Paused]` は出ず、最後の画面が残る。眠ったかは QMP の `query-status` が `{"status":"suspended","running":false}` を返すかで見る。
- ゲストが固まって見えたら `info registers -a` で各 CPU の RIP と RBP の鎖（`x /2gx <rbp>`）を取り、`build/kernel.elf` を llvm-symbolizer に引く（CPU に役割の差は無いので、全部の CPU を見る）。
- 終了は HMP の `quit`。この作業で起動した QEMU だけを止める。

## 目視記録の最小形

```text
サイト／操作：
実URL・媒体を組んだ時刻とコミット：
見た画像（作業場の絶対path）：
実際に見えた本文と配置：
操作と前後の変化：
console・serial の行（要点）：
合格／失敗／未確認：
次の製品修正／未実施の境界：
```

HTTP 200、DOMContentLoaded、serial の PASS、scrollY だけを「見た本文と配置」の代わりに書かない。表示の変化が読み込みの進行なのかスクロールなのかも切り分ける。
