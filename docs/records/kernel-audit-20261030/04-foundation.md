# {"agentType": "Plan", "description": "土台（arch・mm・lib）の監査", "toolUseId": "toolu_01BnURgafzpNHrzf4b9SmQNb", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log・git show を読むのはよい）。Linux や他の OS のソースを開かない（設計の比べは公開された設計の知識で）。third_party は読まない。作業木には未コミットの変更があり（git diff --stat で見える）、今のファイルをそのまま監査する。断片でなくファイルを全部読んで判定する。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。この監査の結果で、どの持ち主の単位を消し、何を（あるなら）残すかを決める。直し方（patch）は提案しない（ユーザー：「Please do not suggest "patching" slop or bad architecture」）。

## あなたの範囲：カーネルの土台
- kernel/src/arch/ の全部：CPU の機能（cpu.c・cpu.h）、topology と NUMA（topology.c）、CPU ごとの変数（percpu.h）、CPU の集合（cpuset.h）、GDT・IDT・入口（gdt.c・idt.c・isr.asm）、APIC・IOAPIC（apic.c・ioapic.c・irq.h）、SMP と起こし（smp.c・smp.asm）、spinlock（spin.h）、FPU（fpu.c）、PAT、P ステート（pstate.c）、S3 の復帰（wake.c・wake.asm）。
- kernel/src/mm/ の全部：bootmem.c、pmm.c（頁の確保）、vmm.c・vmm.h（頁表・アプリの空間・TLB の撃ち落とし・PCID）、vmalloc.c、heap.c（kmalloc）。
- kernel/src/lib/ の全部、kernel/src/boot.c・init.c・kernel.h、kernel/linker.ld、Makefile のカーネルを組む規則と旗。
- 呼ぶ側（sys/sched.c・sys/syscall.c など）は、土台の口の契約を判定するのに要る分だけ読む。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・メモリー・装置・プログラム・利用者の数が増えても成り立つ形（ユーザー：「WHAT IF 20 apps were open at once? … what if the browser had 50 tabs open???」「IF YOUR KERNAL DESGIN IS NOT BETTER THEN LINUX AND NT, THEN DONT EVEN SHOW IT TO ME」）。
- 対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）。CPU の下限は Ryzen 5 7600X（Zen 4）、上は 2 ソケットの EPYC 9996（論理 CPU 1024、x2APIC、NUMA、CXL のメモリーだけの node）。数を決め打ちしない（「As many as the physical cpu / threads has. do not hard code slop」）。
- 後ろ向きの互換は要らない（「treat this as greenfield, with the target being very modern」）。Zen 4 より古い CPU のための道は悪い。
- UNIX・POSIX・Linux ではない。
- shim（代役・何もしない lock・偽の関数）とスパゲッティ（一つの呼ぶ側のための包み、渡すだけの関数、その場の特例の分岐、決め打ちの値、ある環境の都合を別の部品へ持ち込むこと）は禁止。抽象化の層は責務の本当の継ぎ目に要る。
- ユーザーは時間の扱いが全体として根から誤っていると言っている（「our timing is still off. the tolerances youve allowed are way too big.」「THIS IS NOT JUST ABOUT SOUND.. THIS IS ABOUT EVERYTHING」）。土台の中で時間に効く所（割り込みの配り、IPI、TSC、待つ spin）もこの目で見る。

## 判定の物差し
- 規模：CPU 1024・NUMA の node・記憶の量・アプリの空間の数が十倍で何が詰まるか。数に比例する走査、一つの大きな lock、一つの CPU に寄せる仕事、決め打ちの上限と表の大きさ、ポーリング・決め打ちの回数の輪。
- 正しさ：競争、memory ordering、lock の順、割り込み・NMI の文脈、寿命、TLB の一貫性、仕様との違い。仕様は D:\Programes\OS DEV INFO にある（AMD APM 第 1〜5 巻を一冊にした 40332-r4.05.pdf、amd64_arch_programmers_manual-vol2-system_programming.pdf、Intel SDM 325462-093-sdm-vol-1-2abcd-3abcd-4.pdf、ACPI_Spec_6_5_Aug29.pdf、Zen の PPR 55898_B1_pub_0.50）。PDF は大きいので、要る節だけを探して読む。記憶でなく仕様の原文で照らす。
- 持ち主：一つの責務が一つの持ち主にあるか。分断・重複。stub・空の実装。
- Linux・NT の公開された設計と軸ごとに比べて劣る所。

## 報告の形（日本語、平文）
持ち主の単位（ファイルか部品）ごとに：
1. 責務（一行）
2. 判定：「残せる」「消して建て直す」「消すだけ（要らない）」のどれか一つ。「直せば使える」という中間は書かない。
3. 根拠：file:line と、その行が何をしているか。コードで確かめたものだけ。確かめていないものは「未確認」と書く。
4. 十倍・1024 CPU で何が詰まるか。
5. Linux・NT の公開の設計と比べて劣る軸（あれば）。
6. 建て直すなら、正しい形が満たすべき要件（設計の要件。patch ではない）と、他の部品との継ぎ目。
最後に：この範囲で「残せる」と言えるものとその理由、土台を一から建てるなら何から順に建てるべきか（依存の順だけ。設計の中身は書かない）、全体の所感（三行まで）。同じことを繰り返さない。

## reply

Nocturne カーネルの土台（arch・mm・lib・boot/init・linker.ld・Makefile の核の規則）監査の報告

範囲と前提
作業木を今のまま読んだ（vmm.c は約 1500 行の未コミットの書き直しを含む）。範囲の全ファイルを全部読み、呼ぶ側は契約を判定するのに要る所だけ読んだ（sched.c・syscall.c・clock.c・acpi.c・pci.c・rtl8125.c・fb.c）。仕様は SDM 093、AMD APM（40332）、Zen 3 の PPR 55898（Family 19h Model 01h）から pdftotext で該当の節を引いた。
いちばん重いのは時間に効く次の所。
・割り込みの終わりの合図（EOI）を処理の前に出している。
・頁違反の処理は全部、割り込みを閉じたまま走る。
・錠の待ちと並列の batch が、割り込みを閉じた区間を設計として長くしている。
・装置の割り込みはすべて CPU 0 の 8259 の線から入る。
・AMD の実機では LAPIC timer の速さが起動時の一回の測りで決まり、後で直らない。

持ち主の単位ごとの判定

[1] cpu.c の検出の部分（read_leaves・required・optional・cpu_features_same）
1 責務：CPUID から必須と任意の機能を読み、全 CPU で同じかを確かめる。
2 判定：残せる
3 根拠：
・cpu.c:23-44 必須の表（x86-64-v4・SMEP/SMAP・INVPCID・RDPID・XSAVES・x2APIC など）。欠けていれば cpu.c:119 で止める。
・cpu.c:151-159 他の CPU も同じかを確かめる。決め打ちの数は無い。
・傷：mwait（cpu.c:52）と invlpgb・invlpgb_pages（cpu.c:57・131）は読むだけで、使う者がいない（grep で確認）。
4 十倍：問題なし。
5 劣る軸：なし。
6 継ぎ目：[2] の CPU の状態を作る手順が、この表から方針を選ぶ。

[2] CPU の状態を作る手順（cpu_setup・entry_load・pat_init_cpu・mtrr_sync・fpu の setup・pstate_init_ap と、それを呼ぶ三つの道）
1 責務：各 CPU の制御レジスターと MSR を、Nocturne の形にする。
2 判定：消して建て直す
3 根拠：
・同じ「CPU に要るもの」が三つの道に分かれている。BSP の起動（boot.c:101-107）、AP（smp.c:253-269）、S3 の後の BSP（wake.c:156-175・200-204）。
・S3 の後の BSP は手順をやり直さず、保存した CR と MSR の写し（wake.c:179-193）を書き戻すだけ。手順と写しの表が別々に育つ。
・cpu_setup（cpu.c:167-171）が立てるのは NXE・WP・PGE・FSGSBASE・SMEP・SMAP だけ。
・機械検査：CR4.MCE を立てない（Limine が立てて渡すかは未確認）。MCi_CTL を開かず、訂正済みの誤りを読む所も無い（MCG_・MCi_ の使い手なし）。#MC は読まずに panic（idt.c:280）。APM 第 2 巻の CR4.MCE の説明では、0 の間は #MC が起きない。
・投機の対策がどこにも無い：EFER.AIBRSE（APM 第 2 巻 3.1.7）、IA32_SPEC_CTRL、IBPB、SRSO（grep で 0 件）。CR4.UMIP も立てない。
4 十倍：PAT や MTRR が違う CPU では、各 CPU が WBINVD を走らせる（pat.c:24-27・73-80。Limine の PAT の値は未確認）。
5 劣る軸：Linux の mce・NT の WHEA。Zen 4 での AutoIBRS・SRSO の対策。起動・hotplug・復帰で一つの道。
6 要件：手順は一つにし、起動・AP・復帰のすべてが通る。MCA と投機の対策を手順の一部にする。継ぎ目は検出の表・wake・clock（TSC）。

[3] cpu.h
1 責務：命令の包みと、いくつもの持ち主の口の寄せ集め。
2 判定：消して建て直す
3 根拠：
・cpu.h:193-254 に gdt・idt・entry・irq・lapic・pic・fpu の口が並ぶ。本体はそれぞれ別のファイル。
・cpu.h:14-18 MMIO の包みの理由が WHPX の命令の解読という VM の都合。
・struct regs（cpu.h:155-161）は isr.asm の REGS_*（isr.asm:14-23）と手で合わせている。asm 側に検査が無い。
6 要件：口は持ち主ごとの見出しに置く。命令の包みは作り直しが容易。

[4] topology.c・topology.h
1 責務：CPU・core・LLC・die・ソケット・NUMA の node と距離を、MADT・CPUID・SRAT・SLIT から作る。
2 判定：残せる
3 根拠：
・数は表から決め、bootmem に置く（topology.c:147-195・282-379）。上限の定数は無い。
・SLIT を検証する（topology.c:241-255）。
・記憶の無い node の CPU は、記憶のある最も近い node を home にする（topology.c:259-269）。
・事実の欠け：AMD の CPUID 0x80000026 を読まないので、AMD では die がソケットと同じになる（topology.c:57-64）。HMAT・CEDT（CXL の CFMWS）・Generic Initiator を読まない（grep で 0 件）。そのため OS が組む CXL の記憶は node にならず、遅れと帯域は SLIT の距離だけ。hot-add は無い（topology.c:193）。
4 十倍：起動時に一度、O(CPU×node)。問題なし。
5 劣る軸：記憶の階層（Linux の memory tiers は HMAT・CXL から作る）。
6 継ぎ目：bootmem・pmm（home と距離）・sched・vmm（LLC）。

[5] percpu.h と percpu_init・percpu_load（smp.c:50-75）
1 責務：CPU ごとの変数（GS-relative）と、CPU の index。
2 判定：残せる
3 根拠：
・GS の基を写しの距離にして、__seg_gs で一命令で読む（percpu.h:13-20）。
・TSC_AUX に index を入れ（smp.c:53）、IST の入口が RDPID で GS を立て直せる（isr.asm:192-197）。
・写しは CPU の数だけ取る（smp.c:58-63）。
・事実の傷：smp.c:60 は home でなく node を渡す。記憶の無い node の CPU の写しは、bootmem の「最も高い番地」に落ちる（bootmem.c:100-101）。
・動的な CPU ごとの確保の口が無い（vmm.c:573-586 は持ち主が自分で配列を取っている）。
6 継ぎ目：正しさが「核は横取りされない」（percpu.h:14-15）に乗っている。横取りを入れるなら this_cpu の契約はそこで決める。

[6] cpuset.h
2 判定：残せる
3 根拠：64 bit の語の原子的な bitmap で、語数は CPU 数から決める（cpuset.h:9-31）。1024 CPU で 16 語。

[7] spin.h
1 責務：CPU の間の短い排他。
2 判定：消して建て直す
3 根拠：
・xchg の取り合いで、待ちの列が無い（spin.h:22-33）。解放のたびに全員が xchg し、公平さが無い。
・割り込みを閉じてから待つ（spin.h:40-44）。待ちの長さが、そのまま割り込みの遅れになる。【時間】
・待ちの輪の中で vmm_tlb_poll を呼ぶ（spin.h:27）。錠が mm の撃ち落としの約束を知っていて、層が逆。待つ CPU は撃ち落としにしか答えず、VEC_RESCHED と alarm の VEC_TIMER は待ちが終わるまで届かない。【時間】
・錠の順は注釈だけ（spin.h:9）で、機械の検査が無い。
4 十倍：2 ソケット・1024 CPU が一つの線を取り合い、遠い CPU が飢える。その間、割り込みは閉じたまま。
5 劣る軸：Linux の qspinlock（MCS）、NT の queued spinlock。
6 要件：列があり、待ち手が自分の線で回る公平な錠。割り込みを閉じる錠と閉じない錠を分ける。撃ち落としが錠の待ち手に頼らない（継ぎ目は vmm）。錠の順を機械で検査する。

[8] gdt.c
2 判定：残せる
3 根拠：
・CPU ごとの GDT と TSS を percpu の中に置く（gdt.c:18-19・35-71）。
・rsp0 の位置を _Static_assert で確かめる（gdt.c:14）。
・傷：gdt_init は gdt_init_cpu(0) へ渡すだけ（gdt.c:31）。

[9] isr.asm の入口と出口（isr_common・isr_saved・isr_exit・isr_ist・isr_ist_kernel・syscall_entry）
1 責務：割り込み・例外・SYSCALL の出入りと GS の扱い。
2 判定：残せる
3 根拠：
・GS の判断は CS と isr_iret の一か所で行い、lfence を置く（isr.asm:110-122）。
・IST から核に入ったら RDPID で GS を立て直し、元を戻す（isr.asm:188-207）。swapgs の前後に来る NMI・#MC に耐える。
・利用者から来た IST の枠は task の stack へ移す（isr.asm:169-183）。
・SYSRET は、非正規の rip・TF・RF のとき IRET へ回す（isr.asm:235-245）。
・割り込みは CPU ごとの stack で処理する（isr.asm:130-141）。
・同じファイルに別の持ち主の物がある：context_switch とトランポリン（sched）、user_copy（利用者の記憶）、wrmsr_checked（cpu）。
・【時間】核へ戻る割り込みは isr_exit へ直行し、横取りの点が無い（isr.asm:146-148）。
6 継ぎ目：idt（ゲートの型と例外の方針）・sched（横取り）・percpu。

[10] idt.c と irq.h（例外の方針・割り込みの配り・vector の持ち主）
1 責務：IDT、vector の割り当て、例外と割り込みの配り。
2 判定：消して建て直す
3 根拠：
・IDT（idt.c:21）と vec_handlers・vec_owned（idt.c:49・134）は機械全体で一つ。使える vector は 0x50-0xEF の 160 個だけ（idt.c:132-133）で、CPU を増やしても増えない。
・irq.h:4-6 に運転手ごとの決め打ちの vector がある（VMBus 0x31、xHCI、NVMe、RTL8125 0x4C）。使い手は rtl8125.c:608 など。VMBus は Hyper-V のもの。
・IRQ_SHARED 4 の決め打ち（idt.c:47-48）。
・irq_register は irq の範囲を見ない（idt.c:109-116）。rtl8125.c:611 は PCI の Interrupt Line（pci.c:232 で 0-255）をそのまま渡すので、16 以上だと表の外へ書く。
・外す口（idt.c:118-127・155-163）は、走っている処理の終わりを待たない。acpi.c:341-353 は自前の sci_running で塞いでいる。
・【時間・仕様違反】処理の前に EOI を出す（LAPIC は idt.c:346、8259 は idt.c:338）。SDM 第 3A 巻 13.8.5 は「処理の終わり、IRET の前」と定めている。level の線（SCI・INTx）は、戻った直後にもう一度届く。
・【時間】全ゲートが割り込みゲート 0x8E（idt.c:66）。頁違反は idt.c:268 から、割り込みを閉じたまま vmm_handle_user_fault に入る。vmm.h:102 の契約は「割り込みは開いている、眠ってよい」だが、開ける所はどこにも無い（idt.c・vmm.c・syscall.c の shared_fault を確認）。最大 16 頁の 0 埋め（vmm.c:800-816）も、割り込みを閉じたまま走る。user_copy_fixup（idt.c:255）は割り込まれた側の IF を見るだけ。
・NMI は panic で止まる道か panic のどちらか（idt.c:280・317）。NMI を使う標本や見張りは作れない。
4 十倍：vector は 160 が天井で、MSI-X の CPU ごとの列を配れない。装置の割り込みはすべて CPU 0 に来るので、CPU 0 の割り込みを閉じた区間が全装置の遅れになる。
5 劣る軸：Linux（CPU ごとの vector の表、affinity の配り、synchronize_irq、irq thread）、NT（CPU ごとの IDT、DPC）。
6 要件：
・vector は CPU ごとに持つ。
・宛先は x2APIC の 32 bit（IR を通す）で、多くの CPU へ配れる。
・外す口は走り中の処理の終わりを待つ。
・EOI は処理の終わりに出す。
・眠れる例外は割り込みを開いて処理する。
・NMI と #MC に、panic 以外の道を持たせる。
継ぎ目は apic・ioapic・iommu・pci・sched・vmm。

[11] 8259 PIC の道（idt.c:175-233、apic.c:123-126 の LINT0 ExtINT、isr_interrupt の 32-47）
2 判定：消すだけ（残すのは、起動で全部 mask する数行だけ）
3 根拠：
・ISA の線と SCI（acpi.c:361-362）と INTx の後戻りが、8259 から BSP の LINT0 を通って入る（apic.c:2-3・124）。
・ISR の読みと EOI は port I/O（idt.c:230-233・225-228）。
・APIC が無いときの道（apic.c:181-184）は、Zen 4 の下限では起きない。
・acpi.c:357 は GSI 16 以上の SCI を断る。
5 劣る軸：Linux・NT は APIC 方式で、IOAPIC と MSI を使う。

[12] apic.c
1 責務：LAPIC の mode、IPI、EOI、起こしの timer。
2 判定：消して建て直す
3 根拠：
・x2APIC を必須にしている（cpu.c:32）のに、xAPIC の道をまるごと持つ。AMD の 4 bit の ID（apic.c:82-98）、Intel の TME の KeyID（apic.c:165-172）、xAPIC へ戻る遷移（apic.c:71-80・143-154）。
・xAPIC の配達待ちは、時間でなく 100 万回の pause の決め打ち（apic.c:273-279）。
・IPI は宛先一つずつ送る（apic.c:281-303）。省略形も、x2APIC の論理 cluster（SDM 第 3A 巻 13.12.10、一回の ICR で最大 16 CPU）も使わない。
・【時間】TSC-deadline の無い CPU では、一回きりの数えで timer を動かす。Zen 3 の PPR 55898 では CPUID Fn0000_0001_ECX[24] が Reserved（Zen 4 は手元に PPR が無く未確認）。なので AMD の実機ではこの道になると見られる。
・【時間】数えの速さは起動時に 20 ms 一回だけ測る（apic.c:346-355・359-377）。clock は 64 秒後に TSC の速さを測り直す（clock.c:9-10・27）が、LAPIC の速さは測り直さないので、起動時の較正の誤りがずっと残る。そのうえ 1/8192 遅らせ（apic.c:339・374）、数は書く前に読んだ now からの差（apic.c:405-407）。
・VM の値を読む（apic.c:366 の vm_lapic_hz）。
・誤り・温度・性能の LVT は mask したままで、処理が無い（apic.c:115-121）。ARAT は見ない。
4 十倍：IPI の費用が宛先の数に比例する（撃ち落とし・panic・batch の起こし）。量は未確認。
5 劣る軸：Linux の x2APIC cluster の multicast、timer の較正、APIC の誤りの処理。
6 要件：
・x2APIC だけにする。
・宛先の集合へ一度に送る。
・timer の速さは clock と同じ基準を保ち、clock の較正が変わればついて変わる。
・VM の値は VM の持ち主の後ろに置く。
継ぎ目は clock・alarm・smp・vmm・wake。

[13] ioapic.c・ioapic.h
2 判定：消して建て直す
3 根拠：
・ioapic_route を呼ぶ者がいない（grep）。今の役は、全部 mask することと S3 の後の戻しだけ。
・IOAPIC_MAX 8 と given[8][256] の決め打ち（ioapic.c:11・32）。9 個目からは何も言わずに読まない（ioapic.c:51）。
・宛先は 8 bit で、0xFE を超えると断る（ioapic.c:121）。
4 十倍：2 ソケット EPYC の IOAPIC の数は未確認だが、上限そのものが決め打ち。
6 要件：GSI の唯一の持ち主にする。MADT の全 IOAPIC を持ち、IR を通す 32 bit の宛先、SCI と PCI の _PRT を扱う。継ぎ目は iommu・acpi・pci。

[14] CPU を起こす・止める持ち主（smp.c の ap_entry・ap_main・start_wait・park・suspend・resume と、wake.c・wake.h・wake.asm）
2 判定：消して建て直す
3 根拠：
・AP を起こす道が二つ。起動は Limine の goto_address（smp.c:284-289・537-538）、S3 の後は自前の INIT-SIPI（smp.c:379-412、wake.asm:70-128）。
・wake.asm に xAPIC の ID を読む道が残る（wake.asm:88-103）。
・休止の写しの小片が同居している（wake.asm:183-232、wake.c:84-94）。
・IPI の包みは失敗すると panic し、宛先は一つずつ（smp.c:112-127）。
・smp_get_info は「AP の働き手」を ABI に出す（smp.c:209-218）。非対称だった頃の名残り。
・起動のたびの自己試験（smp.c:415-499）。
・良い点：APIC ID で表を引くので、全 AP を一斉に起こせる（wake.asm:104-118）。
4 十倍：smp_suspend は CPU を一つずつ待つ。最悪で 2 秒×CPU 数（smp.c:356-369）。
5 劣る軸：Linux は起動・hotplug・復帰で一つの trampoline を使い、並列に起こす。CPU の hotplug もある。
6 要件：起こしの道は一つにし、[2] の手順を通す。休止の写しは休止の持ち主へ移す。継ぎ目は percpu・sched・clock・alarm・vmm。smp.asm（cpu_switch_stack）は残せる。

[15] cpu_parallel_for と cpu_jobs_poll（smp.c:129-207、sched.c:527）、fpu_kernel_begin/end（fpu.c:102-107）
2 判定：消すだけ
3 根拠：
・batch は機械全体で一つ（smp.c:134-142）。
・【時間】呼び手は batch の間ずっと割り込みを閉じる。他の CPU の終わりを待つ間も閉じたまま（smp.c:171・191-194・203）。手伝う idle の CPU も、割り込みを閉じて走る（smp.h:41-43）。fb.c:397-398 は、その遅れを 128 行ごとに割って逃げている。
・項目の数によらず暇な CPU 全部に IPI を送る（smp.c:187-188）。
・使い手は描画（wm.c:1605、fb.c:401）だけ。
5 劣る軸：Linux は核の SIMD を横取りを止める区間で許し、並列の仕事は眠れる worker で行う。

[16] fpu.c・fpu.h（XSTATE）
2 判定：残せる（ただし fpu_kernel_begin/end は [15] と一緒に消える）
3 根拠：
・compacted の配置を 0xD の葉から計算し、全 CPU で同じか確かめる（fpu.c:46-78）。
・XSAVES と XRSTORS を使う（fpu.c:85-91）。
・事実：切り替えのたびに保存と復元をする（sched.c:465-466）。利用者へ戻るまで復元を遅らせる仕組みは無い。
5 劣る軸：Linux の TIF_NEED_FPU_LOAD（scheduler との継ぎ目の契約）。

[17] pat.c・pat.h
2 判定：残せる
3 根拠：
・SDM・APM の手順で PAT を書く（pat.c:17-32）。vmm の UC・WC の約束と一致する（pat.h:5-7）。
・MTRR の保存は S3 のため。MTRR_VAR_MAX 16 で何も言わずに切る（pat.c:40・50）。

[18] pstate.c・pstate.h
2 判定：消して建て直す
3 根拠：
・全 CPU の最低を nominal、EPP を 0 に固定して変えない（pstate.c:56-59）。
・idle は HLT だけ（sched.c:535）。MWAIT と C 状態の方針が無い。
・VM の判定がある（pstate.c:40・44）。
4 十倍：1024 thread の全 core を nominal 以上に固定する。電力と boost の余地への影響の量は未確認。
5 劣る軸：Linux の amd-pstate EPP・cpuidle（MWAIT・_CST/_LPI）、NT の PPM。
6 要件：性能と idle の方針を一つの持ち主にまとめ、alarm の締め切りと C 状態の出口の遅れを知る。継ぎ目は sched・alarm・apic（ARAT）・ACPI。

[19] bootmem.c・bootmem.h
2 判定：残せる
3 根拠：
・表は自分の最初の確保で、決め打ちの大きさは無い（bootmem.c:47-63）。
・node の境で切り（bootmem.c:65-83）、残りを pmm へ渡す（bootmem.c:104-108）。
・事実：node に無いと、近い node でなく最も高い番地へ落ちる（bootmem.c:100-101）。

[20] pmm.c・pmm.h
1 責務：物理の頁の確保と、frame の記録。
2 判定：消して建て直す
3 根拠：
・node ごとに一つの錠で buddy の全操作をする（pmm.c:71・281・331・345・451・501・550）。
・CPU の溜めは 1 頁だけ（pmm.c:328-341）。多頁と 2 MiB は毎回 node の錠を取る。
・遠い node の解放と確保は、頁ごとに相手の錠を取る（pmm.c:450-453・405-413）。
・足りないときの steal と drain_all は CPU 数に比例する（pmm.c:355-376）。
・回収の口が無い。水位も無い。vmalloc の退いた頁（vmalloc.c:46）と heap の depot（heap.c:247-269）は、別々の場当たりの対処。
・記録の初期化は CPU 0 一つで、RAM の量に比例する（pmm.c:561-576）。
・ブートローダー解放可能領域を戻さない（pmm.c:118-120、bootmem.c:23）。
・hot-add も CXL の階層も無い。
・良い点：1 GiB の section ごとに、node の上に 32 byte の記録を置く。帯で DMA の幅を選べる。
4 十倍：NPS1 では一つの錠を 512 thread で分ける。12 TiB の機械では記録が約 32 億個で、それを CPU 0 だけで初期化する（時間は未確認）。
5 劣る軸：Linux の pcp の多 order・deferred の並列の初期化・kswapd と水位・compaction・hotplug・memory tiers。NT の待機と 0 の頁の列・背景の 0 埋め。
6 要件：
・溜めは多 order にし、錠の区間を短く、遠い解放はまとめる。
・記憶の圧の口を、すべての確保者に持たせる。
・記録は node ごとに並列で初期化する。
・hot-add と CXL の階層を扱う。
継ぎ目は heap（pmm_mark・pmm_marked）・vmm・vmalloc・hibernate（pmm_claim）。

[21] vmm.c・vmm.h
1 責務：核の半分の頁表、アプリの空間、TLB の一貫性。
2 判定：消して建て直す
3 根拠：
・空間の記述（範囲の木）が無い。予約は葉の PTE_DEMAND で、範囲の全部の頁表を先に作る（vmm.c:861-887）。配置の一覧は syscall.c の線形の鎖（syscall.c:1872-1967）にあり、持ち主が二つに分かれている。
・生きている空間の頁表は返さない（vmm.c:972-997）。
・空間ごとに一つの spinlock で、頁違反の 0 埋めもその中で、割り込みを閉じたまま行う（vmm.c:795・800-816）。
・利用者の大きな頁が無い（vmm.c:10-11・386）。直写像は 2 MiB まで（vmm.c:451-469）。
・撃ち落としは宛先ごとに一つの IPI（vmm.c:281-297）。
・INVLPGB を検出しても使わない（cpu.c:57）。CPU ごとに PCID の席を割る作り（vmm.c:74-86・159-180）は、INVLPGB が求める全 CPU 共通の ASID（APM 第 3 巻 INVLPGB）と合わない。
・核の半分は online の全 CPU へ撃ち、32 頁を超えると INVPCID で全部消す。利用者の PCID の訳も消える（vmm.c:138-143・279）。
・答えない CPU は 5 秒で報告するが、待ち続ける（vmm.c:303-310）。
・【時間】長い輪に横取りの点が無い：reserve・protect・free（vmm.c:861-997）。vmm_space_end は 512 GiB の枠ごとに一度だけ（vmm.c:698）。
・配置と閾の決め打ち：vmm.h:28-38（主 stack 8 MiB、ASLR なし）、vmm.c:50・52・808。
・良い点：外してから撃ち、撃ってから frame を返す（gather）。広げる変更は撃たずに頁違反で拾う。答えは LLC ごとに数える。
4 十倍：
・50 タブのブラウザーの大きな予約は、予約の大きさだけ頁表と時間を食う。
・1024 thread のアプリは、一つの錠で頁違反を取り合う。
・1 頁を外すのに 1023 の IPI が要る。
5 劣る軸：Linux（VMA の木・VMA ごとの錠・頁表ごとの錠・THP・munmap で頁表を返す・INVLPGB と global ASID）、NT（VAD・working set・大きな頁）。
6 要件：
・空間の記述を mm に一つ持つ。頁表は触れた所だけ作り、外せば返す。
・錠は範囲か頁表の単位で持つ。
・大きな頁を使う。
・撃ち落としは集合へ一度に送り、PCID の割り方を broadcast と両立させる。
・長い仕事に横取りの点を置く。
継ぎ目は syscall の配置・pmm・sched・spin・idt。

[22] vmalloc.c・vmalloc.h
2 判定：消して建て直す
3 根拠：
・全体で一つの錠（vmalloc.c:34）。頁ごとに核の半分の錠を取る（vmalloc.c:168-176 から vmm.c:471-479）。
・掃除は量の閾（RAM の 1/1024）だけで起き、記憶が足りなくても掃除しない（vmalloc.c:46・221-236）。
・vunmap は毎回全 CPU を撃つ（vmalloc.c:196-202）。
4 十倍：stack の溜め（sched.c:79・85-93、2 個）が外れると、thread の作りが二つの全体の錠に並ぶ。
5 劣る軸：Linux の CPU ごとの vmap block・node ごとの池・失敗時の purge。
6 継ぎ目は vmm・heap・sched。

[23a] heap.c の小さな物（slab・CPU ごとの鎖・node の depot）
2 判定：残せる
3 根拠：
・Bonwick の magazine と depot。遠い home の物は away の鎖でまとめて返す（heap.c:56-98・221-244・300-380）。
・鎖の長さは node の記憶と CPU 数から決める（heap.c:544-563）。
・傷：二重解放の検査は先頭だけ（heap.c:367・375）。CACHE_SHARE を pmm と別々に定義している（heap.c:51、pmm.c:33）。
6 継ぎ目：pmm の frame の印。CPU ごとの排他は「横取りなし」に乗る（heap.c:15-16）。

[23b] heap.c の大きな物（pages_alloc の vmalloc への逃げ、heap.c:384-398・452-454）
2 判定：消して建て直す
3 根拠：三つの契約が食い違う。
・heap.h:4-6 は「NMI と #MC 以外のどこでも」と約束する。
・連続の頁が無いと vmalloc へ逃げる（heap.c:389）。vmalloc の埋め損ないは撃つ（vmalloc.c:159・150 → vmm.c:356）。
・撃ち落としは spinlock の下と割り込みの中を ASSERT で止める（vmm.c:258）。vmalloc.h:10 自身は割り込みの中を禁じている。

[24] lib
・string.c・sort.c・span.c・idtable.c：残せる。span は AVL に最長の長さを足して、O(log n) で最も低い所に合う範囲を探す（span.c:126-134）。idtable は錠の外で倍に伸ばす（idtable.c:10-39）。
・lz.c：残せる。使い手は hibernate.c だけで、伸長は境界を検査する（lz.c:86-111）。
・printf.c の書式（kvsnprintf・ksnprintf）：残せる。
・printf.c の記録と panic（klog・kprintf・panic）：消して建て直す。全 CPU の記録が一つの錠（printf.c:147）を通り、serial と fbcon に同期で書く（printf.c:190-192）。1024 CPU では、一本の錠と UART の速さに並ぶ。Linux の printk は lockless の ring に書き、出力を別にする。

[25] boot.c・init.c・kernel.h
・boot.c・init.c：消して建て直す。消える持ち主の呼び並びで、VM の判定と初期化（boot.c:128、init.c:89・98・110）がある。端末の鍵の訳と cmdline の読みが同居している（init.c:34-82）。boot.c:141 は何もしない。
・kernel.h：残せる。

[26] linker.ld と Makefile の核の規則
2 判定：残せる（規則の形として）
3 根拠：
・text・rodata・data の分け（linker.ld:23-51、vmm.c:612-618）は正しい。
・事実：番地を固定し -mcmodel=kernel -fno-pie なので KASLR が無い（linker.ld:14、Makefile:26）。
・事実：全ての .c を入れるので vm/ も実機の核に入る（Makefile:31）。強化の旗が無い（Makefile:25-28）。KASLR と強化は、建て直す側の安全の設計で決めること。

残せると言えるものとその理由
topology、percpu の仕組み、cpuset、gdt、isr.asm の入口と出口、cpu.c の検出の表、fpu の XSTATE、pat、bootmem、heap の小さな物、lib（string・sort・span・idtable・lz・書式）、smp.asm、kernel.h、linker.ld と Makefile の規則。
理由は四つ。数を CPU と node から決め、決め打ちの上限が無い。仕様の手順どおり。持ち主が一つ。他への依存が薄い。
ただし percpu・heap・isr.asm は「核は横取りされない」に乗っているので、横取りを入れるならその継ぎ目は建て直す側で決める。

土台を一から建てる依存の順
1. 像の配置と起動の受け渡し
2. CPU の機能の検出と、CPU の状態を作る手順（BSP）
3. トポロジーと NUMA
4. 起動の記憶
5. CPU ごとの領域
6. 錠
7. 物理の頁
8. 核の頁表と直写像
9. 例外と割り込みの入口、CPU ごとの vector、x2APIC の IPI
10. TLB の無効化の約束
11. 核の仮想の番地と heap
12. 時間の基（TSC と LAPIC timer）
13. AP の起こし（2 の手順を通る一つの道）
14. 装置の割り込みの経路（IOAPIC・MSI・IR）
15. 利用者の空間
16. 電力・idle と、眠り・復帰

所感
土台の多くは数を表から決めていて、決め打ちは vector 160・IOAPIC 8・IRQ 4 など一部に限られる。
しかし時間と規模を決める所（錠、割り込みの配りと EOI、頁違反の IF、一つずつの IPI、空間の記述）は根から Linux・NT に劣り、割り込みを閉じた区間が設計として長い。
残せるのは表と手順の部品で、それを支える錠・割り込み・空間・CPU の起こしは建て直しになる。

この判定で一番重い所
1. 頁違反の IF の契約の食い違い（idt.c:66・268 と vmm.h:102）
2. 処理の前の EOI（idt.c:338・346、SDM 第 3A 巻 13.8.5）
3. heap・vmalloc・shoot の三つの契約の食い違い
4. 測り直されない LAPIC timer の速さ（apic.c:359-377）

Zen 4 が TSC-deadline を持つかは未確認。手元の PPR は Zen 3 のもので、そこでは Reserved。
