---
paths:
  - "kernel/**"
  - "common/**"
  - "boot/**"
  - "host/**"
---
# カーネルの領域に触れたとき

- 呼出しは BSP が受け持つ。user の thread は AP で走るが、AP で片付かない呼出しは BSP へ回り（`sched_ap_interrupt` → `isr_dispatch_remote` → `syscall_dispatch`）、BSP が一つずつ行う。長い核の処理（大きなプログラムの読み込み、核の中の回り続け）は、その間の全部の app の呼出しを待たせる。
- ゲストが止まって見えたら、QEMU の HMP で `info registers -a` を取り、CPU0 の RIP と RBP の鎖（`x /2gx`）を `build/kernel.elf` で llvm-symbolizer に引く。
- カーネルの標本は、起動の cmdline に `prof`（または `prof_ms=<n>`）で有効になり、serial に窓（既定 10 秒）ごとに出る：`prof: cpu%u user … kernel … idle …`、`prof: task pid …`、`prof: <標本数> <rip> pid …`、AP から BSP への往復（`prof: … retired to BSP …`）。`scripts/platform_qa.py --cmdline prof`。
- 起動の行：`memory: N MiB usable, M MiB free`、`video: monitor … at … Hz`（EDID が決める）、`store: C: "…" on sata0, …`。メモリー切れは `[memory] pid … no pages for …`（10 秒に 1 回まで）。カーネルの行には時刻が無い。
- VM だけの処理（QEMU・Hyper-V の装置、hypervisor の判定）は一か所の入口の後ろに置き、共通コードに書かない。VM の測定値で実機の既定を決めない。
- ABI は `common/abi.h`（`SYS_*` の番号）・`common/display_abi.h`・`common/gpu_abi.h`。仕様は `docs/nocturne-native-abi.md`。番号で引く呼出しは codegraph の索引に出ないので、呼ぶ側は自分で読む。
- 解像度と拍は EDID とカーネル・表示経路（`kernel/src/dev/fb.c`、`kernel/src/gui/screenmode.c`、`kernel/src/gui/wm.c`）が決める。limine.conf で固定しない。
- 目録（`kernel/src/fs/catalog.c`）の枝の item 0 の鍵は何も区切らない。目録を変えたら `store tree-test <seed> 200000` を幾つかの種で通す（覚え書き）。
- 実機の記録は写真だけ（USB は起動用の一本、Windows から読める）。iGPU・二台目のディスク・BIOS の変更へ逃げない。
