# ネットワーク改善：CPU 局所状態と AP の退避通知

## 対象と変更

- 開始時 `d962b59`、担当は提案 5。先行描画変更を保全。J-space は medium、旧ミッションの台帳は再開していない。
- `kernel/src/arch/smp.c/h`、`cpu.h`、`gdt.c`：各 CPU の GDT 読込み後に GS の kernel 局所状態を初期化。`cpu_current_index()` は単 CPU なら即座に 0、それ以外は GS 相対の一命令読取り。LAPIC 読取りと CPU 一覧の走査を除去。
- `kernel/src/sys/sched.c/h`：`current_task` の読取りは GS 相対の一命令、書込みは専用 setter。FS のユーザー TLS、syscall 番号、既存 task の移動・XSTATE 保存を変更しない。
- `kernel/src/arch/isr.asm`：ユーザー入口／復帰の `swapgs`、kernel 起源例外の実 GS base 確認、推測実行用 `lfence`、復帰に必要な交換状態の私有 stack word を追加。公開 `struct regs` の配置は不変。呼出し stack は C ABI の整列へ合わせる。
- FSGSBASE は各 CPU で無効のままにする。ユーザーは GS selector を変更できるが、ユーザー用 GDT の base は 0。ユーザーが高位 GS base を作り、例外入口で kernel 状態と誤認させる経路は認めない。
- AP は旧 task stack を離れた後の `RUN_RETURNED` 公開に続き、BSP へ専用 vector `0x34` を送る。同時退避は一つの pending flag に集約。BSP は flag の atomic exchange に続いて mailbox を acquire 読取りし、ready queue へ回収する。EOI は既存 IDT 経路の LAPIC EOI を利用する。
- BSP kernel continuation の非プリエンプト契約を維持する。通知 IRQ 内で任意の `schedule()` はしない。ring3 からの IRQ は通常の user-return、idle の HLT は通常の HLT 復帰後の安全な切替点を利用する。kernel が IRQ を閉じて待つ既存 quiesce は明示回収で進める。
- poll 担当との合意に基づき、`sched.c/h` の全体 poll queue と終了時の全体通知を除去。個別 provider の通知実装は別担当。
- 既存 `tests/smptest.c` に、GS selector を 0／ユーザー DS へ再読込みした後の read-only identity syscall・BSP sleep continuation・FS TLS 保持・CPU index 範囲の確認を統合した。

## 第二考察で修正した競合

pending flag の解除が release store だけだと、BSP の store buffer に解除が残ったまま mailbox の読取りが先行し得る。AP が旧 pending=1 を見て IPI を省略し、その公開を BSP の走査が見落とす組合せを否定できなかった。解除を acquire/release の atomic exchange に変更し、解除後の走査を順序化した。コンパイル済み出力にも `xchgl` が存在する。

NMI／#DB／#MC や復帰時例外は、保存 CS が kernel でも GS がまだユーザー側の狭い区間へ入れる。そこで kernel 起源の例外だけは実 GS base を読取り、交換の要否を stack word へ保存する。通常 user syscall と通常 kernel IRQ にこの MSR 読取りを入れていない。現行 NMI の診断・終了方針そのものは変更しない。

## 実行した確認

- Clang の正規 kernel flags による `gdt.c`／`smp.c`／`sched.c` の構文確認：成功。
- `smptest.c` の正規 user flags による構文確認：成功。
- NASM による `isr.asm` のアセンブル：成功。listing で CS offset 152、vector offset 128（私有 word 込み）、`swapgs`／`lfence` と stack 整列を確認。
- Clang の assembly 出力で CPU index が `movl %gs:8`、current task が `movq %gs:16`、pending の送信／解除が `xchgl` であることを確認。
- 最小の 2 AP／1 BSP 競合モデル：順序化した解除は 1,680 通りで取りこぼし 0。store buffer を許す旧候補の解除は 4,200 通り中 862 通りで未回収／IPI なしの反例。これは局所的なモデル確認であり、実ハードウェア試験ではない。
- GS の入口／復帰モデル：ユーザー、通常 kernel、例外の kernel GS、例外の交換前後 GS の 5 有効クラスで C 入口の kernel base と復帰前の元 base を確認。
- `git diff --check`：成功。

## 確認の境界

この担当は全体 build、QEMU、Hyper-V、実ネットワーク throughput を実行していない。実ゲストの `smptest`／thread TLS、BSP が user 実行中・idle HLT 中の通知、例外診断は主担当の統合確認へ引き渡す。AP syscall の待ち時間が常に 0 になる保証はしない。長い非プリエンプト kernel 区間や IRQ 禁止区間は安全な接点まで待つ。LAPIC/MSR が何 µs の VM exit になるか、今回の速度向上率は実測していない。

## 参照した一次資料

- [Linux kernel の入口資料](https://www.kernel.org/doc/html/v5.12/x86/entry_64.html)：保存 CS と実 GS base の判定を使い分ける必要性、NMI 等の交換区間。
- [Intel の FSGSBASE ガイダンス](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/best-practices/guidance-enabling-fsgsbase.html)：CR4 の許可と GS の高位判定を組合せる際の所有権境界。
- [Intel の SWAPGS 推測実行資料](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/speculative-behavior-swapgs-and-segment-registers.html)：交換判定後の GS 相対アクセスを fence で区切る根拠。
