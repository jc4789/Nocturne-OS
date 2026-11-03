# BIOS 起動と Hyper-V 第 1 世代を媒体と道具から消す（依頼文と報告）

## 依頼文

あなたは Nocturne OS（独自の OS。UNIX でも POSIX でも Linux でもない）の媒体と試験の道具から、古い起動の道を消す書き手です。git の作業木の中で一人で働きます。

### 必ず最初に読むもの（本体の作業木の絶対パスで）
- `D:\Programes\llm vibe slop os\AGENTS.md`：特に「互換は持たない」（BIOS 起動・Hyper-V 第 1 世代は古い道で、見つけたら消す）と「ビルドと媒体」。
- `D:\Programes\llm vibe slop os\.claude\skills\nocturne-project\SKILL.md` と `references\qemu-visual.md`：QEMU の起動の手順。

### 背景
対象は実機（Ryzen 5 7600X、UEFI で起動）。試験の QEMU は今まで `-M pc`（i440FX）と SeaBIOS（BIOS 起動）・AHCI だったが、本体の会話が q35・UEFI（edk2）・NVMe で起動し、デスクトップ・USB の指とキー・Terminal が動くことを確かめた（main 7669f58b の媒体）。その命令：

```text
"C:/Program Files/qemu/qemu-system-x86_64.exe" -M q35 -m 4096M -smp 4 -accel whpx -cpu host -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=32
  -drive "if=pflash,format=raw,readonly=on,file=C:/Program Files/qemu/share/edk2-x86_64-code.fd"
  -drive if=pflash,format=raw,file=<作業場>/vars.fd
  -serial file:<作業場>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait -qmp tcp:127.0.0.1:45455,server,nowait
  -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on -device nvme,serial=nocturne,drive=boot,bootindex=0
  -netdev user,id=qa -device e1000,netdev=qa
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
```

`vars.fd` は `C:/Program Files/qemu/share/edk2-i386-vars.fd` の書ける写し（起動ごとに作業場へ写す。x86_64 の vars の版は QEMU に無い）。起動の確かめは serial の `store: C: "Nocturne" on nvme0`。

### 頼むこと
1. `scripts/mkimage.sh`：BIOS の道（`limine.exe bios-install`、`limine-bios.sys`、ISO の `limine-bios-cd.bin` と `-b` の起動の記録）と、第 1 世代の固定長 VHD（`nocturne.vhd`、`hyperv_disk` の `.vhd` の枝）を消す。残すのは GPT の ESP（Limine の UEFI と kernel）と C:、VHDX（第 2 世代）、ISO（UEFI だけ）。注記も今の形に。
2. `Makefile`：`$(BUILD)/host/limine.exe` の規則とそれへの依存（`image`・`qemu-image`・`run`）を、BIOS の導入にしか使わないなら消す（`tools/limine/limine.c` と `host/limine/seek64.h` の使い道を読んで決める。消すなら `host/limine/` も）。`run` の QEMU の命令を q35・UEFI・NVMe に（vars の写しは `$(BUILD)/run-vars.fd` のように build の中へ、組みの出力として）。注記（`image` の「VHD and VHDX」など）を直す。
3. `scripts/build.py`・`scripts/test.py`・`scripts/wpt.py`：`.vhd` を出す・読む所を消す。test.py と wpt.py の QEMU の命令の機械・firmware・ディスクを上の形（`-M q35`、pflash の二つ、`-device nvme`）にする。vars の写しは各道具の出力の場所（test.py は `build/`、wpt.py は `build/wpt/`）。**窓なしの作り（`-display none`・`egl-headless`・`CREATE_NO_WINDOW`）には触れない**（別の仕事）。
4. `hyperv.ps1`：`-Generation` の第 1 世代の道（`.vhd`、IDE、legacy の網の装置、注記と例）を消し、第 2 世代だけにする。`scripts/hv-*.ps1` に第 1 世代の分岐があれば同じく。
5. `README.md`・`README_EN.md`：第 1 世代の節・表の行・`nocturne.vhd` の行・BIOS 起動の記述を消し、残りを今の形に合わせる。
6. `.claude/skills/nocturne-project/SKILL.md` と `references/qemu-visual.md`：標準の起動を上の q35・UEFI・NVMe の命令にする。AHCI は「SATA の道を試すとき」の追加の装置として一行残す（実機の板は SATA も持つ）。「NVMe で試すときは UEFI で起動する」の落とし穴は標準になったので消す。`if=ide` の落とし穴は消す。
7. `.claude/rules/map.md` の「ビルド・媒体・試験」の行に VHD があれば直す。

### 決まり
- `kernel/` には触れない（`kernel/src/vm/` の装置の driver も残す。VM の中身は別の判断）。
- 消してから建てる。古い道を旗や引数で残さない。
- 編集は Edit・Write。シェルや Python でファイルを書き換えない。シェルは Bash（Git Bash）、探すのは `rg`。
- ビルドの規則を変えたので、空の build/ から組んでも通るかを読んで確かめる（生成物の規則、PATH に無い道具。ユーザーの PowerShell に Git の usr/bin は無い）。作業木の根で `python -X utf8 scripts/build.py -j 8 kernel` が通ることを確かめる（媒体は作業木では作らない。本体が取り込んで作り、QEMU で起動して確かめる）。QEMU は起こさない。
- 作業木の枝にコミットする（題は日本語で「媒体：何をしたか」、本文の最後に `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`）。main へ取り込まない。push しない。

### 報告（最後のメッセージ。日本語。本体がそのまま下へ写す）
1. 消したもの（ファイル・規則・関数・節）。
2. 変えた命令（test.py・wpt.py・Makefile の run の QEMU の命令の全文）。
3. 判断した所（limine.exe を消したか残したか、その理由）。
4. 確かめたことと確かめていないこと。
5. 作業木の枝の名とコミットの一覧。

## 報告（書き手の最後のメッセージ、全文）

## 報告：BIOS 起動と Hyper-V 第 1 世代を媒体と道具から消した

作業木の枝は `worktree-agent-a80b3eab7ac04ce73`。開始時は c53fe992 で、main（7669f58b）より古かった。そのため先に `git merge --ff-only main` で 7669f58b まで進めてから作業した。

### 1. 消したもの
- `scripts/mkimage.sh`
  - `boot_install` の BIOS 導入を消した。対象は `limine.exe bios-install` と ESP への `limine-bios.sys` のコピー。
  - ISO の `limine-bios.sys`・`limine-bios-cd.bin` のコピー、xorriso の `-b … -no-emul-boot -boot-load-size 4 -boot-info-table`、ISO への `bios-install` も消した。
  - 第 1 世代の固定長 VHD を消した。`hyperv_disk` の `.vhd` の枝と `case`、`nocturne.vhd` を作る 2 行。
  - 冒頭と各関数の注記は UEFI だけ・VHDX だけの形に直した。
- `Makefile`
  - `$(BUILD)/host/limine.exe` の規則を消した。
  - `image`・`qemu-image`・`run` からその依存を外した。
  - 注記の「VHD and VHDX」を直した。
- `host/limine/seek64.h` を `git rm` した。`host/limine/` ごと無くなった。
- `host/store/store.c`
  - `store disk` が作る GPT から BIOS boot 区画（`bios_boot_type`、`bios = 2048`、`"BIOS boot"` の項）を消した。
  - ESP は 1 MiB（sector 2048）から始まる。
  - 使い方の注記も直した。
- `hyperv.ps1`
  - `-Generation` 引数を消した。
  - `.vhd` / IDE の枝、`$Ext` / `$Bus`、`Add-Nic` の legacy 網の枝、`Set-VMBios` の起動順、第 1 世代の DVD、「マウスを捕まえる」の案内を消した。
  - `Set-Gen2Boot` は `Set-Boot` に改名した。
  - 注記と例を直した。
  - 既存の VM が第 2 世代でなければ、`-Remove` を含め最初に throw して止まる。
- `scripts/hv-*.ps1`：第 1 世代の分岐は無かった。変更なし。
- `scripts/build.py`：`.vhd` を扱う所は無かった。変更なし。
- `README.md`・`README_EN.md`
  - 第 1 世代の節、`-Generation 1` の表の行、`nocturne.vhd` の行、「PS/2 マウスの縦軸の反転」の項、BIOS 起動の記述を消した。
  - 起動メニューの項目数（3 項目→2 項目）、`ide-hd` の QEMU 命令、ネットワーク欄の第 1 世代の記述、`host/limine` の記述も直した。
- `.claude/skills/nocturne-project/SKILL.md` と `references/qemu-visual.md`
  - 標準の起動を q35・UEFI・NVMe の命令にした。
  - `if=ide` の落とし穴と「NVMe で試すときは UEFI で起動する」を消した。
  - AHCI は「SATA の道を試すとき」の一行にした。
  - `store: C: … on sata0` は `nvme0` にした（SATA の道では sata0）。
  - 二台目のポートは `-monitor` 45456・`-qmp` 45457、vars は `vars-2.fd` に変えた。標準の命令が 45455 を `-qmp` に使うため。
  - PS/2 の相対マウスの項は USB タブレットの記述に差し替えた。
  - 窓なしの印の行番号を今の値（wpt.py 672・677、test.py 552・562）に直した。
- `.claude/rules/map.md`
  - `all`・出力の VHD を消し、`host/` の行から `limine/` を消した。
  - 「古い記述」から README.md:51 の項目を外した。
  - test.py・wpt.py の行番号を直した。
- `.claude/rules/kernel.md` と `references/logs.md`：起動の行を `nvme0` に直した。

### 2. 変えた命令（全文）
`test.py` と `wpt.py` は共通の `test.py` の `vm_machine(qemu, vars_path, disk)` を使う。

`vm_machine` が返す部分：
```text
-M q35
-drive if=pflash,format=raw,readonly=on,file=<QEMU の share>/edk2-x86_64-code.fd
-drive if=pflash,format=raw,file=<vars の写し>
-drive id=boot,file=<disk>,format=raw,if=none
-device nvme,serial=nocturne,drive=boot,bootindex=0
```
- firmware は QEMU の実行ファイルの隣の `share\`、無ければ `..\share\qemu\`（同梱 MSYS2）から探す。無ければ `sys.exit`。
- `edk2-i386-vars.fd` を vars の写しに毎回コピーする。パスは `qemu_file` で絶対・スラッシュ・コンマ二重にする。

`scripts/test.py` の命令（vars は `build/test-vars.fd`。`--output-dir` の下ではその中）：
```text
<qemu> -M q35 -drive if=pflash,format=raw,readonly=on,file=<share>/edk2-x86_64-code.fd
  -drive if=pflash,format=raw,file=build/test-vars.fd
  -drive id=boot,file=build/test.img,format=raw,if=none
  -device nvme,serial=nocturne,drive=boot,bootindex=0
  -m <N>M -smp <n> -accel <tcg|whpx>
  -display <egl-headless|none> -vga none -device VGA,xres=…,yres=… -no-reboot
  -serial file:build/test-serial.log
  -netdev user,id=net -device e1000,netdev=net
  -audiodev wav,id=snd,path=…,out.frequency=48000,out.buffer-length=20000,in.voices=0 -device AC97,audiodev=snd
  (+ -device virtio-gpu-gl-pci（--gpu virgl のとき）)
```

`scripts/wpt.py` の命令（vars は `build/wpt/wpt-vars.fd`）：
```text
<test.QEMU> -M q35 -drive if=pflash,format=raw,readonly=on,file=<share>/edk2-x86_64-code.fd
  -drive if=pflash,format=raw,file=build/wpt/wpt-vars.fd
  -drive id=boot,file=build/wpt/wpt.img,format=raw,if=none
  -device nvme,serial=nocturne,drive=boot,bootindex=0
  -m <N>M -smp <n> -accel <tcg|whpx>
  -display none -vga none -device VGA,xres=…,yres=… -no-reboot
  -serial file:<out>/serial.log -monitor tcp:127.0.0.1:<port>,server,nowait
  -netdev user,id=net -device e1000,netdev=net
  -audiodev none,id=snd -device AC97,audiodev=snd
```

`Makefile` の `run`（`QEMU_SHARE` は同梱 QEMU の `share/qemu` を `cygpath -m` で求める。上書きできる）：
```text
@bash scripts/mkimage.sh --run
@cp -f "$(QEMU_SHARE)/edk2-i386-vars.fd" $(BUILD)/run-vars.fd
@$(QEMU) -M q35 -m $(QEMU_MEMORY) -smp $(QEMU_CPUS) -accel $(QEMU_ACCEL) $(QEMU_GRAPHICS)
  -drive if=pflash,format=raw,readonly=on,file="$(QEMU_SHARE)/edk2-x86_64-code.fd"
  -drive if=pflash,format=raw,file=$(BUILD)/run-vars.fd
  -drive id=boot,file=$(BUILD)/run.img,format=raw,if=none
  -device nvme,serial=nocturne,drive=boot,bootindex=0 -serial stdio -vga std
  -netdev user,id=net -device e1000,netdev=net
  -device qemu-xhci,id=xhci -device usb-kbd,bus=xhci.0 -device usb-tablet,bus=xhci.0
  -audiodev dsound,id=snd -device AC97,audiodev=snd
```

依頼にない追加：
- `-netdev user,id=net -device e1000,netdev=net` を test.py と run に明示した。q35 では既定の網カードが e1000 のままか確かめていない。カーネルが持つ driver は e1000・tulip・RTL8125 だけなので、明示した。wpt.py には元からあった。
- `run` には xHCI と USB の指とキーも足した。PS/2 は無く、入力がそれしか無いため。

### 3. 判断した所
- **`limine.exe` を消した。** 使い手は `mkimage.sh` の `bios-install` の 2 か所だけだった（ISO 用と ESP 用）。UEFI だけになって使い道が無くなったので、規則と `host/limine/seek64.h` も消した。
- **`tools/limine/` は触っていない。** `tools/` は git の外のユーザーの道具。UEFI 用の `BOOTX64.EFI` と `limine-uefi-cd.bin` は mkimage が引き続き使う。
- **`tools/limine/` に残る BIOS 専用ファイル（消すかどうかはユーザーの判断）：** `limine.c`、`limine-bios-hdd.h`、`limine-bios.sys`、`limine-bios-cd.bin`、`limine-bios-pxe.bin`、`limine.exe`。
- **`store.c` の BIOS boot 区画を消した。** 依頼の項目には無いが、BIOS の道の一部なので消した。
  - ESP・C: の検出はどちらも型 GUID 引きで、区画の番号に頼らない（`store esp`、カーネルの `gpt_find`）。
  - 既存の VM のディスク（BIOS boot 区画あり）の `--update` も `store esp` で ESP を引くので動く見込み。
- **QEMU の firmware の探し方。** 標準の命令は `C:/Program Files/qemu/share` 固定。test.py は QEMU の隣の `share` か同梱 MSYS2 の `share/qemu` を順に探すようにした。両方に edk2 のファイルがあることを確かめた。
- **USB タブレットの位置の送り方。** QMP の `input-send-event`（`abs` の x・y は 0〜32767）と書いたが、これは QEMU の入力 API の知識で、実際には試していない。HMP の相対 `mouse_move` が効くかは「未確認」と書いた。

### 4. 確かめたことと確かめていないこと
確かめたこと：
- 作業木の根で `python -X utf8 scripts/build.py -j 8 kernel` が通った（警告 0 行、最後は `LD ../../../build/wt/agent-a80b3eab7ac04ce73/kernel.elf`）。
- `store.exe` をビルドし、`store disk … 192 64` → `esp`（offset 1048576、131072 sectors）→ `format` → `mformat -H … -h 64 -s 32` → `mmd` → `check`（0 problems）が通った。
- `xorriso -as mkisofs -R -r -J -hfsplus -apm-block-size 2048 --efi-boot … -efi-boot-part --efi-boot-image --protective-msdos-label` を小さな材料で実行した。El Torito の起動画像は UEFI の 1 つだけになった。
- `bash -n scripts/mkimage.sh`、`hyperv.ps1` の構文解析（PowerShell の Parser）、`test.py` の `vm_machine` の実行が通った。
  - 引数列を二つの QEMU 位置（C:/Program Files と同梱）で確かめた。
  - firmware の無い QEMU では `sys.exit` になることも確かめた。
  - vars の写しは 540672 byte。
  - `wpt.py` はフックが止めるため実行も構文確認もしていない。差分は読んだ。
- `make -s --eval=…` で `QEMU_SHARE` が `D:/Programes/llm vibe slop os/tools/msys64/ucrt64/share/qemu` に展開されることを確かめた。
- `make -n qemu-image` は、`limine` を参照する行も `limine.exe` の規則欠けも出ない。ただし作業木に `third_party/tinycc/conftest.c` が無く（今回の変更とは無関係）、最後まで評価できなかった。

確かめていないこと（未確認）：
- QEMU の起動（`make run`、test.py、wpt.py の新しい命令での実際の起動と `store: C: "Nocturne" on nvme0`）。QEMU は起こしていない。
- `mkimage.sh` 全体（`qemu-image`、`image`、`--run`、`--update`）の実行。ISO は xorriso の旗だけを小さな材料で確かめた。
- 空の `build/` からの全体ビルド。変更した規則は読んだ限りでは生成物にも PATH にも新しい前提を足していない。
- Hyper-V での `hyperv.ps1` の動作（ユーザーの担当）。
- `Makefile` の `run` の `QEMU_SHARE` が、`QEMU` に別の場所の実行ファイルを渡したときも正しいか。
- test.py の既定 `--accel tcg` では `-cpu host` が付かない。標準の命令にある `-cpu host`（Zen 4 の下限に必要）は test.py・wpt.py・`run` に足していない。tcg では使えないため。
- test.py・wpt.py に xHCI の USB 機器を足していない。GUI の入力を試す試験が USB 機器を要するかは未確認。

### 5. 触れなかった所（本体の判断に残したもの）
- `scripts/platform_qa.py`・`scripts/qtest.py` に `-M pc`＋`ide-hd`/`ide-cd`＋AHCI（BIOS 前提）の命令が残る。スキルが「使わない」道具なので触れていない。消すかどうかはユーザーの判断。
- 作業木にコミットされている `AGENTS.md:83` は「`all`（vhd・vhdx・iso も）」のまま。本体の作業コピー（未コミット）の版は既に VHDX だけの書き方なので、触れなかった。
- `.claude/skills/mokei/SKILL.md:69` に「`all` は vhd・vhdx・iso まで」の記述が残る。使わない旧スキルなので触れていない。
- `kernel/` と `kernel/src/vm/`（ata・tulip・e1000 などの装置の driver）は変更していない。README の `kernel/src/vm/` の置き場所の古い記述（README.md:664–669 周辺）も、この仕事の範囲外として触れていない。

### 6. 作業木の枝とコミット
- 枝：`worktree-agent-a80b3eab7ac04ce73`（main へ取り込んでいない。push していない）
- コミット：`c2f892a3 媒体：BIOS 起動と Hyper-V 第 1 世代の道を消し、QEMU の機械を q35・UEFI・NVMe にする`（一つだけ。親は main の 7669f58b）
- 変更は 14 ファイル（`host/limine/seek64.h` の削除を含む）。コミットの題は「媒体：…」、本文の最後に `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`。
- 本体の `build/wt/agent-a80b3eab7ac04ce73/` に、カーネルと `host/store.exe` の出力が残っている。
