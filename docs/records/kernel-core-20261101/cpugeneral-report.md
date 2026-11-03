# cpugeneral の報告

## 依頼文（全文）

```
あなたの名は「cpugeneral」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の今の先頭。自分の作業木が古ければ、まず main を取り込む。QEMU のポートは HMP 45482・QMP 45483。

# 持ち主の言葉（そのまま）
"I want to point out that that is for zen 5 epyc, and is not for my zen 4 7600x. and that we should also not be hard coding things to only support one specfic processor."
"again we are not targeting a specific processer, or hard coding things to my system. The kernel should be made with the intent of making the kernel a general OS"
"Again we are targeting MODERN SYSTEMS."
"we are taking modern systems as a baseline. What i keep saying tho, is to make sure you are not hard coding the kernel to my specfic MODERN zen4 machine."
"this does not MEAN TO REMOVE VENDER SPECFIC SUPPORT"

# 仕事
カーネルが CPU とプラットフォームをどう扱っているかを読み、持ち主の言葉に合う形へ直す。何が欠陥で、どう直すかは、あなたがコードと文書から決める。ほかの誰かの案を前提にしない。

入口の一つは TSC の周波数の道である：`kernel/src/arch/cpu.c`、`kernel/src/arch/cpu.h`、`kernel/src/sys/clock.c`、`kernel/src/sys/clock.h`、`kernel/src/dev/refclock.c`、`kernel/src/dev/refclock.h`。監査 `docs/records/kernel-core-20261101/audit-hardcoded.md` の行 T4 と §6 はこの種の指摘で、静的な読みなので原文で照らす。入口はそれだけではない。`kernel/src/arch/` と `kernel/src/dev/` を読み、一つの機種や一つの世代にしか成り立たない所を探す。

# 文書（`D:\Programes\OS DEV INFO`）
- `40332-r4.05.pdf`：AMD64 Architecture Programmer's Manual Volumes 1-5。
- `amd64_arch_programmers_manual-vol2-system_programming.pdf`、`AMD64 Architecture Programmer's Manual - Volume 3 ...pdf`。
- `56713-B1_3.05.pdf`：PPR 56713。何の family と model の文書かは表紙で確かめる。
- `57238_C1_pub_0.49\`：PPR 57238（7 分冊）。何の family と model の文書かは表紙で確かめる。
- `325462-093-sdm-vol-1-2abcd-3abcd-4.pdf`：Intel SDM。
- `ACPI_Spec_6_5_Aug29.pdf`、HPET の文書。
ほかにも必要な文書はこのフォルダーを探す。手元に無い文書が要るなら、報告にそう書く。

# 確かめ
組み（`kernel`・`qemu-image`、変数なし、カーネルの警告なし）。窓付きの QEMU で起動し、デスクトップとブラウザーが動くこと、`clock:` の行を読む。直した道のうち QEMU を通らない道は、host の検査でどう試したかか、試せないことを報告に書く。報告は `docs/records/kernel-core-20261101/cpugeneral-report.md`。
```

## 結論

カーネルは「AMD の Family 19h の実機か、周波数を申告する hypervisor」でしか正しく起動しなかった。これを次の形に直した。

- TSC の周波数は、hypervisor の登録、CPU の申告、参照に対する計測の順に取る。どれかがあれば起動する。
- 申告は社と family ごとに、その社の文書の書式で読む。
- 参照は HPET と ACPI PM timer の二つを持つ。
- 性能の要求、訂正の machine check、speculation の制御、die の読みは、両社の道を持つ。
- AMD の支えは一つも消していない。下限（x86-64-v4）は AGENTS.md の決定のままである。

## 見つけた欠陥

| 場所 | 欠陥 | 根拠 |
|---|---|---|
| `arch/cpu.c` の `tsc_p0` | AMD でない実機は起動で panic する。Intel は全部止まる。 | 原文の分岐 |
| `arch/cpu.c` の `tsc_p0` | Family 1Ah の PStateDef を Family 19h の書式で読む。CpuFid 258h は DID 2 と読まれ、無効として panic する。 | PPR 57238：CpuFid[11:0]、CoreCOF = CpuFid × 5 MHz |
| `arch/cpu.c`・`sys/clock.c` | 文書の無い family、HWCR[TscFreqSel] が 0、PstateEn が 0、周波数を登録しない hypervisor は panic する。計測すれば動く機械である。 | 原文 |
| `dev/refclock.c` | HPET の無い機械は名目の周波数のまま走る。ACPI PM timer を読まない。 | ACPI 6.5 4.8.3.3 |
| `arch/idle.c` | 性能の要求が AMD の CPPC だけで、Intel の HWP が無い。 | SDM vol. 3B 17.4 |
| `arch/cpu.c`・`apic.c` | Intel の CMCI が無く、訂正の誤りが割り込みで届かない。 | SDM vol. 3B 18.3.2.5、18.5 |
| `arch/cpu.c` | CPUID 7 EDX[26] を IBPB だけとして読み、EDX[27] の STIBP と拡張 IBRS（IA32_ARCH_CAPABILITIES bit 1）を読まない。 | SDM vol. 2A、vol. 4 |
| `arch/topology.c` | AMD の die は leaf 8000_0026h にあり、leaf 0xB には無い。読まないので die の数が誤る。 | APM vol. 3 E.4.24、PPR 57238 |
| `dev/fb.c` | 「WHPX does not emulate SIMD MMIO」という VM の都合の注釈。本当の理由は核が FPU を使わないことである。 | 原文 |
| `gui/wm.c` の `remote_size` | RDP の大きさの範囲 640..3840・480..2160 を共通の窓管理に持つ。 | AGENTS.md の VM の分離 |

監査 §6 の残りは次のとおり判じた。

- `gui/wm.c` の absolute pointer は VM 専用ではない（`dev/usbhid.c` が absolute を立てる）。注釈だけ直した。
- `dev/input.c` の `input_unicode` は入力の一般の種類である。呼ぶのが今 VM だけでも残した。
- `dev/audio.c` の `RUNS` 32 は根拠が AC'97 である。実機の音の sink が無いので、今は比べる第二の持ち主が無い。実機の sink を入れるときに決める。別の書き手が音の driver を書いているので、そちらへ渡す事項である。

## 設計

### TSC の周波数

`clock_init` は次の順で最初の周波数を決める。

1. `clock_register_tsc_hz` で hypervisor が登録した値。参照には触れない。
2. `arch/tsc_rate.h` が解釈する CPU の申告。AMD の Family 19h は FID[7:0] / DID[13:8] × 200 MHz、Family 1Ah は CpuFid[11:0] × 5 MHz、Intel は CPUID 15H の ECX × EBX / EAX。
3. 参照に対する初めの計測（`first_measure`）。
4. どれも無ければ、理由を述べて panic する。

2 と 3 の後は、参照が窓を倍にしながら測り直す（`discipline`）。この道は前からあった共通の道で、変えていない。

`tsc_rate.h` はレジスターの意味だけを持つ純粋な関数である。MSR と CPUID を読むのは `arch/cpu.c` である。これで文書の例を host で試せる。

文書の無い family の MSR は読まない。IDT の前で読むので、fault すると何も言わずに止まるからである。

Family 19h の書式には文書の内部の食い違いがある。PPR 56713 のレジスターの行は「CpuFid * 5MHz」と書くが、同じ文書の用語集の CoreCOF と PPR 55898（Model 01h）は FID / DID × 200 MHz である。用語集の式を取った。理由は `tsc_rate.h` の注釈に書いた。

### 参照

`refclock_rate_reference` が HPET、次に PM timer を探す。どちらも数えていることを見てから返す（`rate_counting`）。`struct clock_rate_reference` の口は `read`（初めの計測）と `begin`（起動と復帰の両方、`resume` を置き換えた）である。`clock.c` は参照の種類を知らない。`rate_kind` の分岐は消えた。

### 性能の要求

`struct perf_way` が一社の MSR の組を持ち、`cppc_way` と `hwp_way` が裏である。呼ぶ側の `cpu_perf_setup`、`cpu_perf_set_policy` は変わらない。

### MSR でしか分からない機能

`msr_features` が CPU 0 の `entry_load` の後に読む。`wrmsr_checked` の fault の受け手が要るからである。CMCI は `cmci_on` が bank を claim し（SDM 18.5.2.1）、自分の bank だけを割り込みで見る。

## 変えたファイル

コミットは二つ：`7bc62961`（本体）と、この報告のコミット（注釈の折り返しと報告）。

- `kernel/src/arch/tsc_rate.h`（新規）、`arch/cpu.c`、`arch/cpu.h`、`arch/apic.c`、`arch/apic.h`、`arch/idle.c`、`arch/idle.h`、`arch/lapic_timer.c`（注釈）、`arch/topology.c`
- `kernel/src/sys/clock.c`、`sys/clock.h`、`dev/refclock.c`、`dev/refclock.h`、`boot.c`（注釈）
- `kernel/src/dev/fb.c`（注釈）、`gui/wm.c`、`vm/rdp.c`
- `tests/check_tsc_rate.py`（新規）
- `.claude/rules/map.md`

ネイティブ ABI は変えていない。

## 消したもの

- `tsc_p0`、`did_valid`、`PSTATE_FID`、`PSTATE_DID`、`p0_fid`、`p0_did`、`CPU_COF_UNIT_HZ`
- `cpu_features_init` の panic「states no TSC rate Nocturne can use」、`clock_init` の panic「the hypervisor registers none」
- `rate_kind`、`refclock_hpet_start`、`hpet_resume`、`hpet_fire`、`hpet_sample`（`rate_*` へ）、`clock_rate_reference.resume`
- `apic.c` の `cmci` の static（`cpu_features.cmci` へ）
- `gui/wm.c` の RDP の範囲の clamp（`vm/rdp.c` の `viewer_size` へ）

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel` と `qemu-image`：変数なしで通り、カーネルの警告は 0 である（CC・LD・IMG の行のほかに何も出ない）。
- `python -X utf8 tests/check_tsc_rate.py`：`ok`。
- 媒体は `D:\Programes\llm vibe slop os\build\wt\agent-a921564b7633775e7\nocturne.img`。

作業木の土台は `5cd02f38` である。main はその後に 4 コミット進んだ（先頭 `f2a489bd`）。`git merge-tree` で衝突が無いことを見た。main を取り込んだ組みは試していない。

## QEMU で見たもの

四つの道を、それぞれ窓付きの QEMU（q35、UEFI、NVMe、`-cpu host`、WHPX、4 CPU、2560×1440）で起動した。どの回もデスクトップが出て、Web Browser で en.wikipedia.org の Main Page が開いた。

二・三・四の道は、一時の編集を入れた媒体で試した。一時の編集は戻し、コミットに入っていない。

- 「hypervisor が周波数を登録しない」機械：`vm/hyperv.c` の登録の行を外した。
- 「CPU が申告する」機械：上に加え、`tsc_stated` の hypervisor の分岐で `tsc_rate_amd(0x19, TSC_AMD_TSC_FREQ_SEL, EN | 0x08BC)` を返した。実機の 7600X が通るのと同じ `clock_init` の枝と窓の道である。レジスターの読みだけが通らない。

画像と log は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\` にある。

### 一：最終の版（一時の編集なし、Hyper-V の登録）

画像：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\final-desktop.png`、`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\final-browser.png`。log は同じフォルダーの `final-serial.log`。

```
[   16.757401 0:-] clock: TSC 4699.887621 MHz by Hyper-V
[   16.772953 0:1] clock: the TSCs of 4 CPUs agree
[   16.774316 0:1] smp: 4/4 CPUs online, each running the scheduler
[   21.677238 0:1] clock: the RTC's second found within 2340 us; the wall clock on by 80242 us
cpu: AMD Ryzen 5 7600X 6-Core Processor, family 19h model 61h stepping 2, ...
cpu: no CPPC or HWP: the firmware's performance policy stays
cpu: no automatic IBRS, IBPB between apps, STIBP, UMIP
cpu: 4 present, APIC IDs 0-3: 4 cores, 1 LLCs, 1 dies, 1 sockets
cpu: APIC ID shifts (leaf 0xb) core 0, die 2, package 2; LLC no cache leaf, the package's, shift 2
```

### 二：計測、参照は HPET（登録なし）

画像：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\measured-hpet.png`。log は `measured-hpet-serial.log`。

```
refclock: HPET at fed00000, 64 bits, 100.000000 MHz
clock: the CPU states no TSC rate here: a hypervisor runs this CPU
[   17.152457 0:-] clock: TSC 4699.990603 MHz by the HPET, a first measure
clock: the TSC measured against the HPET over windows that double from 17081 us
[   17.188463 0:1] clock: TSC 4699.892001 MHz within 117810 ppb of the HPET over 35 ms (-20979 ppb); the clock within 4232231 ns of it
[   57.022876 0:1] clock: TSC 4699.887546 MHz within 143 ppb of the HPET over 39870 ms (-18 ppb); the clock within 4281467 ns of it
```

### 三：計測、参照は ACPI PM timer（登録なし、`-M q35,hpet=off`）

画像：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\measured-pm.png`。log は `measured-pm-serial.log`。

```
refclock: ACPI PM timer at port 608, 24 bits, 3.579545 MHz
clock: the CPU states no TSC rate here: a hypervisor runs this CPU
[   16.784627 0:-] clock: TSC 4699.908718 MHz by the ACPI PM timer, a first measure
[   16.784646 0:-] clock: the TSC measured against the ACPI PM timer over windows that double from 12084 us
[   16.809474 0:1] clock: TSC 4699.906304 MHz within 130392 ppb of the ACPI PM timer over 24 ms (-513 ppb); the clock within 4481512 ns of it
[   70.810037 0:1] clock: TSC 4699.887628 MHz within 61 ppb of the ACPI PM timer over 54025 ms (+2 ppb); the clock within 4521833 ns of it
```

24 bit の timer は 4.7 秒で一周する。54 秒の窓が通ったので、一周をまたぐ数えが働いている。

### 四：CPU の申告（FID BCh、DID 08h の 4700 MHz を与えた）、参照は HPET

画像：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\cpugeneral\stated-hpet.png`。log は `stated-hpet-serial.log`。

```
refclock: HPET at fed00000, 64 bits, 100.000000 MHz
[   16.880927 0:-] clock: TSC 4700.000000 MHz by the P0 P-state
[   16.880976 0:-] clock: the TSC measured against the HPET over windows that double from 17039 us
[   16.898687 0:1] clock: TSC 4699.946741 MHz within 387266 ppb of the HPET over 17 ms (-11331 ppb); the clock within 6735780 ns of it
[   56.983156 0:1] clock: TSC 4699.887606 MHz within 107 ppb of the HPET over 40102 ms (+0 ppb); the clock within 6796133 ns of it
```

### 四つの比べ

比べはすべて同じ host の同じ日の QEMU である。QEMU の HPET と PM timer は host の時計から作られるので、実機の参照の確かさの証拠にならない。

| 道 | 最初の周波数 | 最後の窓の周波数 | 最初の値と 4699.887606 MHz の差 |
|---|---|---|---|
| Hyper-V の登録 | 4699.887621 MHz | 測らない | +3.2 ppb |
| 計測、HPET | 4699.990603 MHz | 4699.887546 MHz（39870 ms） | +21.9 ppm |
| 計測、PM timer | 4699.908718 MHz | 4699.887628 MHz（54025 ms） | +4.5 ppm |
| 申告（与えた値）、HPET | 4700.000000 MHz | 4699.887606 MHz（40102 ms） | +23.9 ppm |

どの最初の値も、最初の窓（数十 ms）の後で参照の値へ寄った。

## QEMU を通らない道

| 道 | どう試したか |
|---|---|
| `tsc_rate_amd` の Family 19h・1Ah の解釈 | `tests/check_tsc_rate.py`：host の clang で `tsc_rate.h` を組み、文書の書式の例と無効の値を照らす。 |
| `tsc_rate_intel` の CPUID 15H の解釈 | 同じ検査。 |
| HWCR と PStateDef の実機の MSR の読み | 試せない。hypervisor の下では読まない。実機の 7600X の起動で通る。 |
| Intel の 15H の実機の読み | 試せない。host が AMD である。 |
| Intel の HWP（770h、771h、774h） | 試せない。 |
| Intel の CMCI（280h+i、LVT 2F0h、claim） | 試せない。 |
| 拡張 IBRS（10Ah bit 1） | 試せない。 |
| AMD の CPPC を `perf_way` へ移した道 | 試せない。QEMU では「no CPPC or HWP」と出る。実機の 7600X の起動で「cpu: CPPC highest …」の行が出る。行の書式は lowest nonlinear が floor に変わった。 |
| AMD の leaf 8000_0026h の die | 試せない。QEMU の行は「leaf 0xb … die 2」で、この leaf は隠れている。 |
| 参照の S3 からの `begin` | 休止の往復を試していない。`clock_resume` の呼ぶ先の名が変わっただけで、中身は前の `hpet_resume` と同じ手順である。 |

## 残った欠陥と未確認

- 下限 x86-64-v4 を AGENTS.md の決定のまま変えていない。AVX-512 の無い Intel（Alder Lake 以降の一般向け）は required の検査で起動が止まる。社を問わず「今の機械」を的にするなら、ここは持ち主の判断の対象である。
- Family 1Ah の Model 02h の PPR しか手元に無い。コードは Family 1Ah の全部の model にこの書式を当てる。書式の違う model は、参照が最初の窓で測り直す。参照の無い機械では誤った周波数のままになる。Family 19h の EPYC（Genoa など）の PPR も手元に無い。
- 手元に無い文書：Family 1Ah の Model 02h 以外の PPR、Family 19h の Genoa の PPR、IBRS_ALL の使い方（一度立てる）を述べた Intel の文書。SDM にはビットの名だけがある。
- Intel の MDS などの緩和は調べていない。
- `dev/pci.c` は segment 0 だけを見る（`struct pci_device` に segment が無い）。複数 segment の機械（大きな EPYC を含む）のデバイスは見えない。呼ぶ側が広いので、この仕事では直していない。下限の「2 ソケットの EPYC」に効く欠陥である。
- S0 低電力（s2idle）が無く、S3 の無い今の機械は眠れない（`dev/pm.c`）。
- 参照も申告も無い機械は panic する。RTC は秒しか数えないので率の参照にしていない。
- PM timer の読みの誤差を ACPI は述べない。jitter を 0 とした。
- `arch/idle.c` の APERF/MPERF の値は、Intel で MPERF の周波数が TSC と違えばその分ずれる。Intel の文書は比だけを定める。注釈に書いた。
- 7600X の実機での確かめはしていない。実機で見るのは次の行である：`clock: TSC … MHz by the P0 P-state`、`clock: the TSC measured against the HPET …`、`cpu: CPPC highest …`、`cpu: … dies`。

## 手順の外れ

PM timer の回を起こすとき、待ちの道具 `wq.py` が「まだ二台」と出して終わった後に起動の命令が続けて走り、三台目の QEMU が一分足らず動いた。フックが次の命令を止めて気づき、自分の一台を HMP の `quit` で止めた。台数を見て起こす `wqs.py` に換え、以後は二台を超えていない。その間にほかの書き手の QEMU で測っていた値があれば歪んでいる。
