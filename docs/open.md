# 開いているもの

今開いている所見と、決めたがまだ作っていないものの一覧。

- 一行に一件。直したら行を消す。経緯は git にある。
- 前から壊れていた物と、自分が壊した物を分けるために使う。
- ここに無い所見は、`docs/records/` の報告にあることがある。報告は過去の記録で、今のコードと照らす。

## 決めたこと（未着手）

- HAL：形は `docs/hal.md`。試作は枝 `HAL-DEV` にある。受け入れの後に手術の計画を見せる。
- 窓の管理と合成：カーネルの外のサービスへ移す。カーネルに GPU の汎用の層を置く。
- 網：TCP/IP を権限の無いサービスへ移す。NIC の実装はカーネルに残し、輪でつなぐ。ソケットは object にする。網の権限は能力の handle で渡す。
- GPU：virgl と `nvrm` は同じ継ぎ目の後ろの実装にする。試験の門を外す。契約は次の設計 `docs/gpu.md` で決める。
- UVM：ソースを残し、組みと毎回の起動から外す。選んで入れる形にする。
- プログラムの形式：我々の形式を作る。ELF は組みの中間物と、Limine が読むカーネルの像にだけ残す。
- 導入の形式：宣言と中身だけを持つ我々の形式を作る。任意の機能とドライバーも同じ形式で入れる。
- CPU の機能：有れば使い切り、無くても動く。製造者ごとの実装に分ける。
- 媒体：同じ一つの像が NVMe、SATA、USB のどれからでも起動する。
- 試験：試験の媒体を組む目標を一つ作る。製品の宣言の表から `tests` を出す。製品のカーネルに試験の門を残さない。

## 未決

- AMD の GPU：特権の半分を書く材料が、許された範囲に見つからない（`freebsd-src/sys/dev` に無い。GPUOpen の文書の頁は ISA と MES だけ）。AMD を作る日にユーザーの判断が要る。

## 作る道具

- コンパイラーの旗：`-Wno-unused-function` を外し、`-Wswitch-enum` を足す。`ports`、`third_party` は対象にしない。
- `make check` の検査：層の依存、製品の名前、`test_flag` の場所、段の番号の名、共通コードの `hypervisor`。数だけ出す検査：呼ばれない関数、写しの行数、関数の長さ、読み手の無い設定、呼ぶ者の無い syscall。
- `.clang-format`。上流は除く。
- ABI の一覧：`SYS_*` から、カーネルの処理、libc の包み、呼ぶ側まで。
- QEMU の操作の道具：起動、撮影、キー、クリック、ログ、止めるを、それぞれ一つの命令に。
- 走らない試験の整理（`docs/records/audit-structure-quality-20261103.md` の §4.2）。

## 構造の所見

番号は `docs/records/audit-structure-*.md` の番号。

- K1、S11：窓の管理、合成、入力の割り振りがカーネルにある（`kernel/src/gui/`）。
- K2：ソケットが object でない（`kernel/src/net/net.c`）。
- K3：`kernel/src/sys/syscall.c` の受け口に四つの形がある。
- K4：`kernel/src/dev/pciuser.c` に表示と ACPI の役がある。
- K5：USB の実装の状態が `void *` である（`kernel/src/dev/usb.h` の `hub`、`state[]`）。
- K6：RDP がカーネルにある（`kernel/src/vm/rdp.c`）。
- K8：設定の門が持ち主を名指しする（`kernel/src/sys/settings.c`）。
- K9：電源でカーネルが郵便箱になる（`SYS_POWER_SAY`）。
- K10：試験と計測が製品に残る（`SYS_NET_TEST`、`kernel/src/sys/prof.c`）。
- K11：端末と byte stream の形が残る（`kernel/src/sys/syscall.c` の 0・1・2）。
- S2：設定の宣言の表が 6 か所ある（`user/apps/settings_table.h`、`kernel/src/sys/settings.c`、`user/apps/machine.c`、`user/apps/display.c`、`user/apps/audio.c`、`user/libc/webprefs.c`、`user/apps/agent.c`）。`user/apps/settingsd.c:1256` が適用を推測する。
- S3：保存の型が窓の形を決める（`user/apps/settings.c:541`）。
- S4：値の間の関係を書く場所が無い。
- S5：窓がサービスの判断を持つ。配置の計算が 3 か所にある。
- S6：導入の形が無い。身元はパスの hash（`kernel/src/sys/authority.c:51`）。
- S7：窓を閉じる段取りが 3 か所にある。
- S8：能力の絞りが見せかけ（`user/apps/machine.c:369`）。
- S9：`user/apps/session.c` に特権と UI が同居する。
- S10：シェルが主画面を知らない（`list[0]`）。
- S12：UI の事象が場所で宛先を指す。
- S13：文の表がコンパイル時に閉じる。
- S14：製品の宣言に試験と NVIDIA がある（`user/apps/services.c:79`、`:64`）。
- libc S1、S2：libc が持ち主の無い束で、共有の仕組みが無い。
- libc S3：TLS が二通りある（`user/include/nocturne.h:412`）。
- libc S4：起動の ABI に UNIX の形がある（環境、0・1・2、`#!`）。
- libc S5：網の口が BSD ソケットの改名になっている。
- libc S6、S7：`nvrm` に OS の層が三つあり、UVM は Linux の語彙を再現する。
- libc S10：偽の境界（`ports/nvidia/nvkms_os.c:727`）。
- libc S12：`user/libc/sys.c` が九つの責務を持つ。

## 既知の欠陥

- MCE：UC と PCC の bank を飛ばす（`kernel/src/arch/cpu.c:338`）。
- TSC：AMD の判定が family だけ（`kernel/src/arch/tsc_rate.h:43`）。
- TSC：`LockTscToCurrentP0` を見ない（`kernel/src/arch/tsc_rate.h:28`）。
- HD Audio：`link_down` の戻りを捨てて DMA の頁を返す（`kernel/src/dev/hda.c:906`）。
- 共通コードに hypervisor の判定がある（`kernel/src/arch/cpu.c:143`）。
- IBPB：`cpu_ibpb` に呼ぶ者が無いのに、起動のログは "IBPB between apps" と書く（`kernel/src/arch/cpu.c:514`）。
- `nvrm` の帰線とフリップの糸が fair 級である（`ports/nvidia` に `n_thread_schedule` が無い）。
- カーネルの機能は IOMMU を素通りする（`kernel/src/dev/iommu.h:9-10`）。絞る口が無い（`kernel/src/dev/iommu.c:647`）。
- PCI に抜き差しと AER が無い。
- ノート PC の内蔵キーボード（i8042、I2C の HID）の実装が無い。
- AVX-512：カーネルは CPU が持てば有効にするが、FFmpeg は切ってある（`ports/ffmpeg/prepare.py`、`Makefile:190`）。我々のコードは実行時に選ばない。
- 休止から戻ると xHCI が `interrupts never arrive, polling instead` で poll へ落ち、NVMe の `ready again after sleep` が約 5 秒遅れる。変える前の写し（`build/mae.img`）でも同じ。原因の見込みは、戻す callback（`dev/pm.c` の `machine_sleep`）が worker を起こす時に AP がまだ止まっており（`smp_resume` は後）、AP へ向かう MSI-X が失われること。確かめていない。S3 も同じ順である。
- GPU：ドライバーが動作中に落ちると既定の道へ戻らない（`docs/records/audit-gpu-generic-20261102.md` の F1）。
- virgl：製品のカーネルで起きない（`kernel/src/vm/virtio_gpu.c:579`）。
- `build/programs/init` がソースの無い古い出力のまま媒体に載る。
- ブラウザーの音：広告の音が残り本編の音が出ない（ブラウザーの作り直しの中で扱う）。

## 古いコメント

- `user/libc/tls.c:7`：時刻は CMOS と書くが、`n_clock_wall()` を読む。
- `user/include/nocturne.h:396`：スレッドは 128 までと書くが、上限が見つからない。
- `user/apps/services.c:79`：試験の媒体を指すが、作る道が無い。
- `kernel/src/vm/vm.h:18`：第 1 世代の古い装置と書くが、その道は無い。
- `kernel/src/sys/wait.h:86`：`wq_wait` の使い方を書くが、呼ぶ者が無い。
- `tests/runtests.c:1`：消えた `scripts/test.py` を指す（ほかの `tests/` にもある）。
- `README.md:314`：組みの中の検査を指す。
