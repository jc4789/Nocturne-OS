# third_party/nvidia-open：NVIDIA GPU の Resource Manager（RM）615.78.08

NVIDIA の open-gpu-kernel-modules **615.78.08**（`version.mk` の `NVIDIA_VERSION`）から、
RM のビルドに要る部分だけを取り込んだもの。Nocturne ではカーネルではなくユーザー空間の
ドライバーアプリ `/bin/nvrm` として動かす（`ports/nvidia/`、台帳は `docs/nvidia/LEDGER.md`）。

**RM のソースは一行も変更していない。** 取り込み元（`D:\Programes\cloned repos\open-gpu-kernel-modules-615.78.08`）
の `src/nvidia` と `src/common` の取り込んだ部分とは `diff -rq` で差がない（3126 ファイル、約 98 MB）。
OS ごとの違いは、RM が呼ぶ `os_*`／`nv_*` を `ports/nvidia/` で実装することで吸収している。

## 取り込んだもの

| パス | 内容 |
|---|---|
| `src/nvidia/` | RM 本体（`src/kernel`・`src/lib`・`src/libraries`）、OS 向けの `arch/nvalloc/unix`、`inc`、`interface`、`generated`（生成済みのヘッダーと C）、`srcs.mk`（ソース一覧の正本）、`Makefile` |
| `src/common/inc` `sdk` `shared` `src` `uproc` | RM が include するヘッダーと共通 C（SDK のクラス定義・`nvSha256.c` など） |
| `src/common/nvlink` `nvswitch` | RM が `INCLUDE_NVLINK_LIB`／`INCLUDE_NVSWITCH_LIB` でリンクする NVLink／NVSwitch ライブラリー |
| `COPYING` `version.mk` | ライセンスとバージョン |

## 取り込まなかったもの（と理由）

| 元の場所 | 理由 |
|---|---|
| `kernel-open/` | Linux カーネル向けの層（GPL の関連を含む）。参照も取り込みもしない（台帳 D7、作業指示） |
| `nouveau/` | 同上（Linux の nouveau 向け） |
| `src/nvidia-modeset/` | Linux のモードセット層。最初は画面出力に触らない（台帳 D8） |
| `src/common/displayport` `modeset` `softfloat` `unix` | 上の modeset 層と Linux ユーザー側ライブラリー用。RM 本体のビルドに要らない（1100 個のオブジェクトが警告なしで通ることで確認済み） |
| ルートの `Makefile` `utils.mk` `nv-compiler.sh` `README.md` ほか | Linux モジュールのビルド用。Nocturne のビルドは `ports/nvidia/nvrm.mk` が `srcs.mk` を読んで行う |

## ライセンスの確認結果

- 本体は **MIT**（`COPYING`、各ファイルの `SPDX-License-Identifier: MIT`）。
- `COPYING` には「Linux カーネルモジュールとしてリンクしたとき、そのモジュールは MIT／GPLv2 の
  デュアルライセンス」という但し書きと GPLv2 の全文が付いている。これは **Linux モジュールに
  リンクした場合の条件**で、個々のファイルは MIT。Nocturne はモジュールを作らず、GPLv2 専用の
  コードも入っていない（`grep` で SPDX が GPL のファイルは無い）。ただし `COPYING` は MIT の表示義務の
  ため原文のまま残している。
- **MIT 以外のもの**
  - `src/nvidia/src/libraries/libspdm/`（DMTF libspdm 3.5.0）：**BSD-3-Clause**（各ファイルの
    ヘッダーが「License: BSD 3-Clause License」と書く。ただし LICENSE 本文はこのツリーに無い）。
    再配布するなら BSD-3-Clause の条件（著作権表示・条件・免責の保持）を満たすため、
    ライセンス全文を同梱する必要がある。NVIDIA の `openspdm.mk` と `nvidia/` 以下の NVIDIA 製ファイルは MIT。
  - `src/common/inc/nvSha256.h`／`src/common/src/nvSha256.c`：**BSD-3-Clause**（Olivier Gay、本文にヘッダーあり）。
  - `src/common/inc/nv_list.h`：MIT 系の許諾文（Intel、Francisco Jerez、NVIDIA の表記）。
  - `src/common/inc/prbrt.h`、`src/nvidia/inc/lib/protobuf/prb.h`、`src/nvidia/src/lib/protobuf/prbenc.c`：
    元は Apache 2.0 だが、ヘッダーの記述では NVIDIA は原著者から **MIT の条件で**受け取っている。
  - `src/nvidia/inc/libraries/nvport/clang/stdatomic.h`：**Apache-2.0 WITH LLVM-exception**（LLVM の複製）。
    GPLv2 とは別系統の許諾的ライセンス。Nocturne のビルドでは clang の組込みヘッダーが先に使われうるが、取り込み済み。
- 要約：GPLv2 専用のものは無い。MIT 以外は BSD-3-Clause（libspdm、nvSha256）、Apache-2.0
  （LLVM の stdatomic.h、元 Apache の protobuf 補助）。BSD-3-Clause の全文は `NOTICES` にまとめ、
  `COPYING` とともに媒体の `/usr/share/licenses/nvidia-open/` へ入れる（Makefile）。

## `NV_BSD` について

`ports/nvidia/nvrm.mk` は RM を `-DNV_UNIX -DNV_BSD -DNV_X86_64` で作る。

- `NV_UNIX` は RM 内部で「OS 向けの層（`arch/nvalloc/unix`）の形」を選ぶスイッチで、Unix 互換を
  意味しない。
- `NV_BSD` を定義する理由は一つだけ。`src/common/inc/nvVer.h`・`nvBldVer.h`・`nvUnixVersion.h` が
  バージョン文字列（`NV_VERSION_STRING` など）を作る分岐を `NV_LINUX`／`NV_BSD`／`NV_SUNOS` などの
  どれかが定義されたときだけ有効にするため。ここで取り込んだソースの中で `NV_BSD` が変えるのはこの分岐
  だけ（BSD 向けのカーネル層は取り込んでいない）。
- `NV_LINUX` は意図して定義しない（cgroup や dmem のフックが有効になる）。
- Nocturne は Unix でも POSIX でもなく、`NV_BSD` を定義しても BSD の API は一つも使っていない。

## 更新するとき

別のバージョンに差し替えるときは、`src/nvidia` と `src/common` の同じ部分を丸ごと置き換え、
`version.mk` と `ports/nvidia/nvrm.mk` の `NVRM_VERSION`、ファームウェアの置き場
（`NVRM_FIRMWARE_VERSION`）を合わせる。RM 側を直して合わせるのではなく、`ports/nvidia/` の OS 層で合わせる。
`.gitignore` の `hyperv/` は無視されるため、`src/nvidia/src/kernel/virtualization/hypervisor/hyperv/`
だけ `!` で例外にしてある。
