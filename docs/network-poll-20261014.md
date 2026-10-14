# 個別 poll 待機とユーザーバッファ検証

2026-10-14。開始時点は `d962b59`。J-space は指定どおり medium、共同作業。先行の描画改善は保全。

## 製品変更

- `kernel/src/fs/vfs.c` / `vfs.h`: 各 vnode に poll 登録リストを統合。呼出し専用 wait queue と世代を持ち、通知元 vnode を監視している呼出しだけを起こす。登録は vnode の参照を保持し、開いた file の参照は保持しない。相手側の最終 close が EOF/HUP を出すことを妨げない。二重登録・途中登録失敗・解除はリンクと参照を回収する。
- `kernel/src/sys/syscall.c`: `sys_poll` の readiness 判定と登録を同じ IRQ 保護区間に置き、走査中に IRQ を許した場合の通知は呼出し固有の世代で検出。timeout、kill、完了、失敗時に登録を解除する。`close` / `dup2` による descriptor の差替えも元 vnode の待機へ通知する。無効 fd を読み飛ばす既存 ABI は変更していない。
- `pipe.c`, `devfs.c`, `audio.c`, `wm.c`: pipe、console、audio、window の全体通知を個別 vnode 通知へ変更。native channel は wrapper と両方向の既存 pipe に登録する。listener の接続通知も listener vnode だけを対象とする。audio は同じ `/dev/audio` vnode を監視する音声待機者をまとめて通知し、無関係な GUI や pipe の待機者は起こさない。
- `user_str` は各ページを一度検証して、そのページ内だけを走査。終端の次のページを先読みしない。ゼロ容量による `dst[-1]` も防いだ。
- `vfs_read_user` / `vfs_write_user` を既存 VFS に追加。kernel 内の読み書きは従来の入口を維持。pipe、native channel、window、console、audio、kmsg は転送量が決定してから該当バイトを検証する。通常ファイルの user read は EOF を超える要求を先に縮小し、検証した同じ長さを driver に渡す。短い転送に対する巨大な未使用尾部の page walk を避ける。
- fault-fixup 表による全 user copy の置換は行っていない。ページ権限の検証、read-only 拒否、stack fault-in の既存契約は維持する。TCP/UDP の転送量検証は主担当の `net.c` 変更。

## この担当が確認した範囲

1. kernel-target clang `-fsyntax-only`、`-Wall -Wextra -Werror`。syscall、VFS、pipe、devfs、audio、wm、および vnode_ops の参照先 FAT/ramfs をまとめて確認し終了コード 0。
2. `tests/poll_hosttest.c` は製品の `vfs.c`、`pipe.c`、`ramfs.c` と製品の `vfs.h` をそのままコンパイルする。IRQ、scheduler、heap、user permission は明示した host shim。警告をエラー扱いにしたコンパイルと実行は終了コード 0、82 確認・失敗 0。
   - 無関係 vnode の待機者を起こさない。
   - 登録前のデータは readiness で見える。登録後の通知は世代と個別待機に残る。
   - duplicate が残る間の endpoint 寿命、最終 writer close の HUP、file を保持せず vnode だけを保持する寿命。
   - 二重・交互の登録解除、登録割当失敗、全参照と heap の回収。
   - native listener 接続、channel の underlying receive pipe、相手 close の通知。
   - `SIZE_MAX` 要求でも実データだけを検証する短い pipe/file read、copy 前の EFAULT とデータ保全、EOF 時に未使用メモリーを検証しない。
3. `git diff --check` は終了コード 0。

再現コマンド:

```text
tools/msys64/ucrt64/bin/clang.exe -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Itests/poll_host_shim -Ikernel/src tests/poll_hosttest.c -o build/poll-hosttest.exe && build/poll-hosttest.exe
```

## 未確認と統合境界

- host shim は実 `sys_poll` の sleep/timeout、実 AP/BSP、CR3、実ページテーブル、GUI、NIC、WHPX の性能を検証していない。速度向上率や無回帰の証明へ昇格しない。
- 全体 build と QEMU は主担当だけが実施。`nativechanneltest`、`threadtest`、`wxtest`、`audiotest` などが統合後の候補。実行結果は主担当の記録で扱う。
- 古い `tests/pipe_hosttest.c` は固定 `PIPE_SIZE` ring と head/tail 数値を前提にしており、開始 HEAD の動的 segment pipe と既に不整合だった。期待値を弱めたり変更して合格扱いにはせず、今回は実行していない。
- CPU 担当が `sched.c` / `sched.h` の旧 global poll queue と task-exit 一括通知を削除。主担当が syscall の poll 対象でない UDP handle の不要な global 通知を削除する。通常の queue 待機はそれぞれ維持。
