# カーネルの監査（2026-10-30）：索引

ユーザーの依頼（原文）：「An extreme amount of bad code was added. To the point where i want to delete our kernel. and start over from scratch. I Need an audit done of our kernel in its current state, please do this with the help of subagents. Please do not suggest "patching" slop or bad architecture.. Look at the las session for needed context and do the audit. and then give me the audit before making any changes.」

同じ会話の中でのユーザーの言葉（原文、届いた順）：
- 「"audio issues," are not relevent. its a sytom of a deeper issue with our timing as a whole」
- 「Hyper-V is not wanted. WE TARGET REAL HARDWARE NOT VM'S」
- 「PLEASE DO NOT CHANGE ANY CODE. I NEED TO SEE THE AUDIT. YOU MAKING DECISIONS IS WHY THIS SLOP HAPPENED」
- 「"Hyper-V instruction," REMOVE THat YOU DID NOT UNDER STAD WHAT I SAID.」
- 「VM'S ARE NOT THE TARGET. REAL HARDWARE IS THE TARGET. WRITE ALL CODE FOR REAL HARDWARE. DO NOT MIXIN VM LOGIC WITH REAL HARDWARE CODE EVER」
- 監査は全文をファイルに書くこと、資料をたくさん書くこと。

## 監査した状態

- main の `5f2fabd9`（段①：idle task の stack）に、未コミットの 13-2（空間と TLB）の差分が載った作業木。未コミットのファイル：`kernel/src/arch/{cpu.h,idt.c,smp.c,smp.h,spin.h}`、`kernel/src/gui/wm.c`、`kernel/src/mm/{heap.c,vmalloc.c,vmm.c,vmm.h}`、`kernel/src/sys/{proc.c,prof.c,sched.c,sched.h,syscall.c}`、`.claude/oboegaki.md`、`.claude/rules/{kernel.md,map.md}`。
- 行数（数えた）：arch 3913、mm 2884、lib 747、sys 8569、fs 3112、dev 15471、net 3461、gui 4674、vm 6990、common/store 3857、boot.c・init.c・kernel.h 358。

## ファイル

| ファイル | 範囲 | エージェントの型 |
|---|---|---|
| [01-fs-store-net.md](01-fs-store-net.md) | kernel/src/fs、common/store、kernel/src/net | Explore（Opus） |
| [02-gui-vm.md](02-gui-vm.md) | kernel/src/gui、kernel/src/vm（と dev/fb.c） | Explore（Opus） |
| [03-cross-cutting.md](03-cross-cutting.md) | カーネル全体の横断：決め打ちの数・時間の決め打ち・ポーリング・一つの CPU への寄り・大域の lock・stub・vm/ の外の VM の判定・古い道・時間の単位 | Explore（Opus） |
| [04-foundation.md](04-foundation.md) | kernel/src/arch、mm、lib、boot.c・init.c・kernel.h・linker.ld | Plan（Opus）（届いたら書く） |
| [05-time-and-execution.md](05-time-and-execution.md) | kernel/src/sys と時間の道の全体 | Plan（Opus）（届いたら書く） |
| [06-devices.md](06-devices.md) | kernel/src/dev | Plan（Opus）（届いたら書く） |

各ファイルは、(1) エージェントに渡した依頼文の全文、(2) エージェントの報告の全文（一字も削っていない。通知の中で `&gt;` `&lt;` `&amp;` と化けていた記号だけを `>` `<` `&` に戻した）、(3) 私が原文で照らした行、の順。

## 依頼文に入っている私の誤り（読むときに差し引くこと）

- 全部の依頼文に「対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）」と書いた。このうち「Hyper-V is not wanted」を私は「Hyper-V は製品の要件ではない」と読んで、そう書いた所がある。ユーザーはその読みを「YOU DID NOT UNDER STAD WHAT I SAID」と退けた。ユーザーの言葉は「VM'S ARE NOT THE TARGET. REAL HARDWARE IS THE TARGET. WRITE ALL CODE FOR REAL HARDWARE. DO NOT MIXIN VM LOGIC WITH REAL HARDWARE CODE EVER」。
- 依頼文には、ユーザーの決まり（AGENTS.md の「製品のコード」）から作った物差しを渡した。前の会話の所見と私の結論は渡していない。

## まだ書いていないもの

- 04・05・06 の報告（エージェントが走っている）。
- 六つを通した照らしと、所見の重なり（届いた後に書く）。
