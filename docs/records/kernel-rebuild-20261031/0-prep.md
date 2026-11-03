# 波 0：下準備（cpu.h の口を持ち主の見出しへ、LAPIC の timer を分ける）

書き手：Sonnet 5.5 のサブエージェント（本体の作業木、コミットなし）。動きを変えない機械的な整理。波 1 の四人が arch/ の同じファイルを取り合わないため。

## 依頼文

Nocturne OS（独自の OS、C のカーネル）のリポジトリ `D:\Programes\llm vibe slop os` で、動きを一切変えない機械的な整理を一つしてほしい。この後、四人の書き手が arch/ を並べて書き直すので、その前に一つのファイルを取り合わないようにするのが目的。

### やること
1. `kernel/src/arch/cpu.h` は命令の包みと、ほかの持ち主の口の寄せ集めになっている。ほかの持ち主の口を、その持ち主の見出しへ移す：
   - gdt（`gdt_init`・`gdt_init_cpu`・`tss_set_rsp0`・`tss_set_ist`、IST の enum、セグメントの定数はそのままでよいか判断）→ 新しい `kernel/src/arch/gdt.h`
   - 入口（`idt_init`・`entry_load`・`entry_stacks_init`・`irq_nesting`・`irq_handler_t`・`irq_register`・`irq_unregister`・`vector_register`、8259 の `pic_*` と `VEC_TIMER`）→ 既にある `kernel/src/arch/irq.h` へ（ベクターの割り当ての口が既にそこにある）
   - LAPIC（`lapic_present`〜`lapic_ident`、`struct lapic_ident`、`LAPIC_INIT_WAIT_NS` など、ただし下の 2 の timer の口を除く）→ 新しい `kernel/src/arch/apic.h`
   - `fpu_init` → 既にある `kernel/src/arch/fpu.h`
   - `wrmsr_checked`、`struct cpu_features` と MSR・CR の定数、命令の包み、`struct regs`、FS/GS の包み、`cpu_switch_stack` は cpu.h に残す。
2. `kernel/src/arch/apic.c` の起こしの装置（`lapic_wake_init`・`lapic_wake_at`、それだけが使う static の変数と関数、`WAKE_LATE_SHIFT` など。`lapic_resume` のうち timer の部分）を、新しい `kernel/src/arch/lapic_timer.c` と見出し `kernel/src/arch/lapic_timer.h` へそのまま移す。`lapic_resume` が LAPIC の他の状態と timer の両方を戻しているなら、timer の分を `lapic_timer_resume` として分け、呼ぶ側（`rg -n lapic_resume kernel/src`）を両方呼ぶように直す。中身の論理は一字も変えない。apic.c の中の他の関数を timer 側が使うなら、apic.h に口を出す。
3. 移した口を使うファイルに、新しい見出しの `#include` を足す。`rg` で使う所を全部探す（`kernel/src` の下全部、vm/ も含む）。要らなくなった include は消さなくてよい。
4. `kernel/Makefile` か根の `Makefile` がファイルを自動で集めるか確かめ（新しい .c が入るか）、必要なら足す。

### 決まり
- 動きを変えない。名前を変えない。注記は移した先にそのまま持っていく。新しい注記は書かない（見出しの最初に一行の説明が要るなら、その持ち主の責務を一行だけ）。
- 編集は Edit・Write で。シェルや Python でファイルを書き換えない。
- コミットしない。作業木を作らない。
- 組んで確かめる：`cd "D:/Programes/llm vibe slop os" && python -X utf8 scripts/build.py -j 8 kernel`。警告もエラーも増やさないこと。QEMU は起こさない。
- 報告は日本語で：移した口の一覧（どの見出しへ）、新しいファイル、include を足したファイル、ビルドの結果（最後の数行）。

## 報告

書き手の報告の本文は、本体の会話の圧縮で失われた（出力のファイルは空）。以下は本体が差分を読んで書いたもの。

- 新しい見出し：`arch/gdt.h`（GDT・TSS の口、IST の enum、セグメントの定数）、`arch/apic.h`（LAPIC の口、`struct lapic_ident`、INIT・STARTUP の待ちの定数。`lapic_read`・`lapic_write` を static から外して出し、timer の側が使う `LAPIC_LVT_TIMER`・`LVT_MASKED`・`LVT_UNUSED`）、`arch/lapic_timer.h`（`lapic_wake_init`・`lapic_wake_at`）。
- `arch/irq.h` へ移した口：`idt_init`・`entry_load`・`entry_stacks_init`・`irq_nesting`・`irq_handler_t`・`irq_register`・`irq_unregister`・`vector_register`・`VEC_TIMER`・`pic_*`。irq.h は cpu.h と percpu.h を読む。
- `cpu.h` から重なっていた `fpu_init` の宣言を消した（fpu.h にある）。
- `arch/lapic_timer.c`：apic.c の起こしの装置（`measure_lapic_hz`・`lapic_wake_init`・`lapic_wake_at`、`wake_mult`・`wake_tsc`・`wake_vector`、`IA32_TSC_DEADLINE`・`LVT_TSC_DEADLINE`・`WAKE_LATE_SHIFT`、timer の register の定数）をそのまま移した。`lapic_resume` は分けていない。
- include を足したファイル：boot.c、arch の gdt.c・idt.c・ioapic.c・smp.c・wake.c、sys の sched.c・proc.c・alarm.c、mm/vmm.c、dev の acpi.c・hibernate.c・input.c・iommu.c・iommu_amd.c・iommu_intel.c・nvme.c・pci.c・pm.c・xhci.c、net/rtl8125.c、vm の ac97.c・e1000.c・hv/hv.c・tulip.c。
- 差分：28 ファイル、65 行足し、164 行消し。
- 本体の確かめ：`python -X utf8 scripts/build.py -j 8 kernel` が `LD build/kernel.elf` まで通った。`MSR_GS_BASE` の桁揃えが崩れていたのを本体が戻した。
- 残り：`apic_init` の宣言は boot.c に残る（A2 が apic.h で持つ）。
