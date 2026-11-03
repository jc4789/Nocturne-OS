# 敵対的な仕様の監査：CPU とプラットフォーム

## 依頼文

Nocturne OS（独自の OS。カーネルは C）の差分を、公式の文書に対して敵対的に監査してください。コードは変えないでください。ビルドと QEMU は動かさないでください。返事と文書は日本語で書いてください。

ユーザーの言葉："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."
対象の前提：今の PC を基準にした一般の OS で、特定の機械に決め打ちしない。各社の対応は消さない。

対象の差分：`git diff 5cd02f38 ba6a9138 -- kernel/ tests/check_tsc_rate.py`（リポジトリは D:\Programes\llm vibe slop os）。差分が触れるレジスター・MSR・CPUID・ACPI の表・タイマーの全部を、差分の周りの今のコードも読んで照らしてください。コードの注釈は書き手の主張なので、真実と見なさず文書で確かめてください。

文書：`D:\Programes\OS DEV INFO`（AMD の APM、PPR、Intel SDM、ACPI など）。`.pdf` の文字が要るなら、会話の作業場に起こしたものがあります：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\ppr\`（`v1.txt`、`v2.txt`、`v7.txt`、`apm.txt`、`acpi.txt`、`sdm.txt`）。無い文書は無いと書き、そこは照らせなかったと書いてください。推測で合否を書かないでください。

見ること：ビット位置、MSR の番号、式、有効な値の範囲、手順の順、読む前に確かめるべき機能のビット、family・model の範囲の取り違え、社ごとの違い、文書の無い機械での振る舞い、競り（複数 CPU）、S3 の復帰、VM の処理が共通コードへ漏れていないか。

報告：全文を `docs/records/kernel-core-20261101/audit-spec-cpu.md` に書いてください（この依頼文を先頭に）。指摘ごとに、場所（ファイル:行）、文書の節（または起こした文字の行番号）、日本語の要旨、どう壊れるか、重さ（データや安全を失う／当たれば壊れる／きれいさ）を書いてください。原文の文を長く写さないでください。合っていた手順も領域ごとに一行書いてください。読み切れなかった範囲を書いてください。コミットはしないでください。

## 結論

差分が加えたビット位置・MSR の番号・式で文書に反するものは見つからなかった。

手順と場合分けに 5 件の欠陥がある。重さの内訳は、データや安全を失うものが 1 件、当たれば壊れるものが 4 件である。5 件のうち 3 件は Intel の machine check の道にある。きれいさの指摘は 10 件である。

- F1：Intel の CMCI の割り込みが UCNA を記録も消去もしない。データの破損の知らせが消える。
- F2：Intel で reset を通らずに `cpu_mca_interrupt_on` へ戻ると、CMCI の持ち主が全部消える。休止からの復帰は毎回これに当たる。
- F3：Intel で UCNA と spurious の記録を見た `#MC` の処理が panic する。文書は続行を定める。
- F4：TSC の申告を family だけで読む。手元の PPR は Family 19h の Model 01h・61h と Family 1Ah の Model 02h だけである。
- F5：`HWCR[LockTscToCurrentP0]` を読まずに PStateDef[0] を TSC の周波数とする。

IA-PC HPET の文書は手元に無い。HPET の手順は照らせなかった。

## 文書の所在

行番号は次の起こした文字のものである。列がずれて起こされた表は、文書の順に読み直した。

| 略号 | 文書 | 起こした文字 |
|---|---|---|
| `sdm` | Intel SDM 325462-093（結合版） | `...\scratchpad\ppr\sdm.txt` |
| `acpi` | ACPI 6.5 | `...\scratchpad\ppr\acpi.txt` |
| `apm` / `apm2` | AMD APM 40332 r4.05 / Vol. 2 24593 r3.39 | `...\scratchpad\ppr\apm.txt`、`apm2.txt` |
| `v1` | PPR 57238 Vol. 1（Family 1Ah Model 02h） | `...\scratchpad\ppr\v1.txt` |
| `p56713` | PPR 56713（Family 19h Model 61h） | `...\scratchpad\56713-B1_3.05.pdf.txt` |
| `p55898` | PPR 55898 Vol. 1（Family 19h Model 01h） | `...\scratchpad\audit-spec-cpu\55898-1.txt`（この監査で起こした） |

`...` は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448` である。

コードの行番号は `ba6a9138` のものである。`gui/wm.c` だけは今の main の `d44c44b4` のものである。ほかの対象ファイルは二つの版で同じである。

## 指摘：データや安全を失う

### F1：CMCI の処理が UCNA を記録も消去もしない

- **場所**：`kernel/src/arch/cpu.c:338`（`mca_threshold`）、`kernel/src/arch/cpu.c:385`（`cmci_on`）。
- **文書**：SDM vol. 3B 18.11.4.2（`sdm:221847`）、Example 18-5（`sdm:221876`）、18.6.3 と Table 18-7（`sdm:220491`）、18.6.4（`sdm:220564`）、18.5.2.1（`sdm:220326`）。
- **要旨**：
  - コードは `UC` か `PCC` の立つ bank を飛ばす。
  - UCNA は `UC`=1、`PCC`=0、`S`=0、`AR`=0 の誤りで、CMCI で届く。
  - 文書は UCNA の記録と消去を CMC の処理の責任と定める（`IA32_MCG_CAP[24]` が 1 のとき）。
  - 18.5.2.1 は持ち主の bank の STATUS を全部記録して消せと定める。`cmci_on` は UC の bank を残す。
- **どう壊れるか**：
  - メモリーの中のデータが壊れた知らせが来ても、log に何も出ない。利用者は破損を知らない。
  - 18.6.4 では、UCR の誤りは後の UCR に上書きされない。訂正の誤りも UCR を上書きしない。消さない bank は以後の誤りの記録を失う。
  - 残った UCNA は F3 の条件で panic を起こす。
- **重さ**：データや安全を失う。
- **直す向き**：CMCI の道で、`IA32_MCG_CAP[24]` が 1 かつ `S`=0・`AR`=0 の bank を記録して消す。`S`=1 の bank と `PCC`=1 の bank は 18.11.4.2 のとおり消さない。

## 指摘：当たれば壊れる

### F2：reset を通らない再開で CMCI の持ち主が全部消える

- **場所**：`kernel/src/arch/cpu.c:372-379`（`cmci_on`）、`kernel/src/arch/smp.c:391`（`smp_resume`）、`kernel/src/dev/pm.c:297`、`kernel/src/dev/hibernate.c:698`、`kernel/src/init.c:50`（`hibernate_restore`）。
- **文書**：SDM vol. 3B 18.3.2.5（`sdm:220112`）、18.5.2.1（`sdm:220326`）、vol. 3A Table 12-1（`sdm:206202`）。
- **要旨**：
  - `cmci_on` は `CMCI_EN` の立つ bank を兄弟の thread のものとして飛ばす。これは初期化の手順として文書どおりである。
  - 文書が `IA32_MCi_CTL2` を 0 にするのは reset だけである。Table 12-1 は Machine-Check Banks を INIT で不変とする。`CTL2` がこの行に入るかは表に明記が無い。
  - `cmci_on` は `this_cpu(cmci_banks)` を、持ち主が前に誰だったかを見ずに上書きする。
  - 次の三つは CPU 0 が reset を通らずに `smp_resume` へ来る。
    - firmware が眠らずに戻った S3（`enter_s3` が戻る、`dev/pm.c:294`）。
    - 止まらない CPU や装置で打ち切った眠り（`dev/pm.c:277`、`dev/pm.c:205`）。
    - 休止の snapshot の後に image の書込みが失敗したとき（`dev/pm.c:282`）。
  - 休止からの復帰も reset を通らない。起動した核が `cmci_on` で立てた `CMCI_EN` が残ったまま、image の核の `smp_resume` が走る（`dev/hibernate.c:698`）。
- **どう壊れるか**：
  - CPU 0 の `mine` は 0 になる。`mca_threshold` は全部の bank を飛ばす。
  - `machine_sleep` を通った場合は `apic_init_cpu` が LVT を mask したままにする。`mine` が 0 なので開かない。
  - INIT で `CTL2` が保たれる機械では、AP も同じになる。
  - 訂正の誤りと UCNA の割り込みは、次の本当の reset まで止まる。log には `0 banks` の行が CPU 0 について出るだけである。
- **重さ**：当たれば壊れる。
- **直す向き**：眠りの前と `restore_jump` の前に、各 CPU が自分の bank の `CMCI_EN` を落とす。

### F3：Intel の UCNA と spurious の記録で `#MC` の処理が panic する

- **場所**：`kernel/src/arch/cpu.c:441-448`（`cpu_machine_check`）、`kernel/src/arch/cpu.h:123-127`。
- **文書**：SDM vol. 3B Example 18-4（`sdm:221659`）、18.6.3 と Table 18-7（`sdm:220491`）。
- **要旨**：
  - コードは `UC` か `PCC` の立つ valid な bank を全部 uncorrected と数え、panic する。
  - Example 18-4 の場合分けは、`MCG_CAP[24]` が 1 のとき次のとおりである。
    - `S`=0・`AR`=0 は UCNA で、CMCI の処理へ任せて飛ばす。
    - `EN`=0・`OVER`=0 は spurious で、記録して消す。
    - SRAO は回復を選べて、再開できる。
  - この処理は差分で変わっていない。差分の前は Intel の実機が起動しなかったので、差分がこの道を Intel へ開いた。
  - Example 18-4 は broadcast の `#MC` を一度に一つの CPU が調べる形である。コードに順番待ちは無い。
- **どう壊れるか**：
  - F1 で残った UCNA がある機械で、別の bank の回復可能な `#MC` が来ると、続行できるのに panic する。
  - Intel の `#MC` は broadcast なので、誤りの無い CPU も全部の bank を読む。兄弟の thread と共有の bank は二つの CPU が同時に読み、訂正の bank は同時に消す。
- **重さ**：当たれば壊れる。
- **直す向き**：Intel の道に `MCG_CAP[24]`・`EN`・`S`・`AR`・`OVER` の場合分けを Example 18-4 の順で入れる。AMD の道は APM のまま分ける。

### F4：TSC の申告を family だけで読む

- **場所**：`kernel/src/arch/tsc_rate.h:43`（`tsc_amd_family_known`）、`kernel/src/arch/tsc_rate.h:32-38`、`kernel/src/arch/cpu.c:146-147`、`kernel/src/arch/cpu.c:137`、`kernel/src/sys/clock.c:830`。
- **文書**：PPR 56713 の表紙と MSRC001_006[4...B]（`p56713:10556`）、PPR 55898（`p55898:11400`）、PPR 57238（`v1:12048`）。Revision Guide 57095（Family 19h Models 10h-1Fh）に PStateDef の書式は無い。
- **要旨**：
  - 手元の PPR が書式を定めるのは Family 19h の Model 01h・61h と Family 1Ah の Model 02h だけである。
  - コードは family 全部の model へこの書式を当てる。
  - 注釈は Family 1Ah の他の model を認めている。Family 19h の他の model（EPYC の Model 11h など）は注釈に無い。`cpu.c:137` は文書のある family だけ読むと書くが、model の範囲は確かめていない。
  - この MSR は IDT の前に読む。文書の無い model で MSR があることを文書で言えない。
- **どう壊れるか**：
  - 書式が違う model は誤った周波数で時計を始める。
  - 参照がある機械では、最初の窓で正しい周波数へ換わる。それまでの誤差は残り、`clock:` の行に出る。
  - HPET も PM timer も無い機械では、誤った周波数のまま走り続ける（`clock.c:851`）。
  - MSR が無い model では、IDT の前の fault で何も言わずに止まる。
- **重さ**：当たれば壊れる。
- **直す向き**：`tsc_amd_family_known` を family と model の組にする。文書の無い model は申告なしとして計測の道へ送る。計測の道は既にある。

### F5：`HWCR[LockTscToCurrentP0]` を読まない

- **場所**：`kernel/src/arch/tsc_rate.h:28-29`、`kernel/src/arch/tsc_rate.h:50-52`、`kernel/src/arch/cpu.c:147`。
- **文書**：PPR 56713 MSRC001_0015（`p56713:10144`、bit 21）、PPR 57238 MSRC001_0015（`v1:11632`）、MSRC001_006[4...B]（`p56713:10556`、読み書きできる）。
- **要旨**：
  - bit 21 が 1 なら、TSC はその bit を立てた時の P0 の周波数に固定される。P0 の定義が後で変わっても TSC は変わらない。
  - bit 21 の reset は 0 であるが、読み書きできる。firmware が立てられる。
  - PStateDef[0] も読み書きできる。
  - 注釈は 0 を前提に、ここでは何も変えないと書く。firmware の分を見ていない。
- **どう壊れるか**：
  - firmware が bit 21 を立てた後に P0 を変えた機械では、PStateDef[0] の値は TSC の周波数と違う。
  - 参照がある機械では最初の窓で直る。参照の無い機械では誤った周波数のまま走る。
- **重さ**：当たれば壊れる。
- **直す向き**：`tsc_rate_amd` で bit 21 を見て、立っていれば申告なしを返す。`tests/check_tsc_rate.py` にその場合を足す。

## 指摘：きれいさ

### F6：文書の場所の誤り

- **場所**：`kernel/src/arch/tsc_rate.h:68`。
- **文書**：SDM の leaf 15H は vol. 1 の Table 21-53（`sdm:30299`）。22.7.3 は vol. 3B の 22-149 頁（`sdm:240367`）。この版の Table 3-8 は比較の命令の疑似命令の表である（`sdm:36464`）。
- **要旨**：注釈の「vol. 2A, Table 3-8」と「vol. 1 22.7.3」は、この版の SDM の場所と合わない。
- **どう壊れるか**：動作に影響しない。次に照らす者が場所を探せない。
- **重さ**：きれいさ。`kernel/src/arch/cpu.h:108` の「vol. 2A CPUID leaf 7」も同じである。

### F7：Table 22-95 の model が要件より下という主張

- **場所**：`kernel/src/arch/tsc_rate.h:70-72`。
- **文書**：SDM vol. 3B 22.7.3 Table 22-95（`sdm:240367`）。
- **要旨**：
  - 表の 06_55H（Xeon Scalable）と Xeon W は注釈の言う下限との関係を SDM で判定できない。
  - 06_55H が `required[]` の全部を満たすかは、手元の文書に model ごとの一覧が無いので分からない。
  - 満たす場合は、ECX が 0 のとき表の crystal を使うのが文書の形である。コードは申告なしとする。
- **どう壊れるか**：参照がある機械では計測で正しく動く。参照も無い機械では panic する。
- **重さ**：きれいさ。注釈の断定を消すか、表を入れる。

### F8：APERF・MPERF を 0 から始める根拠の場所

- **場所**：`kernel/src/arch/idle.c:130`。
- **文書**：APM vol. 2 17.3（`apm2:36529`）は差を取る手順で、0 を書く手順は無い。PPR 57238 2.1.6（`v1:2735`）と SDM vol. 3B 17.2（`sdm:215891`）に 0 を書く手順がある。
- **要旨**：手順は正しい。根拠に挙げた節が違う。
- **どう壊れるか**：動作に影響しない。
- **重さ**：きれいさ。

### F9：CPPC の文書の場所

- **場所**：`kernel/src/arch/idle.c:12`。
- **文書**：PPR 56713 MSRC001_02B0-02B3（`p56713:11520`）。
- **要旨**：注釈は PPR 57238 だけを挙げる。Family 19h Model 61h の PPR にも同じ書式がある。
- **どう壊れるか**：動作に影響しない。読み手は Family 19h の根拠が無いと誤解する。
- **重さ**：きれいさ。

### F10：Intel の MPERF と TSC の関係

- **場所**：`kernel/src/arch/idle.c:168-170`。
- **文書**：SDM vol. 3B 22.7.2（`sdm:240341`）。
- **要旨**：
  - 注釈は Intel の文書が MPERF と TSC を結び付けないと書く。
  - 22.7.2 は、どちらも最大の非 turbo の周波数の近くで動くと書く。
  - 17.2 が比だけを定めるのは正しい。
- **どう壊れるか**：動作に影響しない。
- **重さ**：きれいさ。

### F11：Intel の DisplayModel

- **場所**：`kernel/src/arch/cpu.c:190-195`。
- **文書**：SDM vol. 1 CPUID の Family と Model の規則（`sdm:27567`）。
- **要旨**：文書は Family 06H でも Extended Model を合わせる。コードは 0FH のときだけ合わせる。
- **どう壊れるか**：Intel の Family 06H の model が log で誤る。今 `model` を読むのはこの log の行だけである。model で分ける道を足すと誤った分岐になる。
- **重さ**：きれいさ。

### F12：topology の注釈と、leaf 8000_0026h の非対称のビット

- **場所**：`kernel/src/arch/topology.c:4-5`、`kernel/src/arch/topology.c:70-79`。
- **文書**：APM vol. 3 E.4.24（`apm:92202`）。
- **要旨**：
  - 注釈は各 CPU が起動時に shift を確かめると書く。`read_levels` は最初の CPU で一度走るだけで、AP の確かめは無い。
  - 文書は CPU ごとに違う値がありうると書く。EAX[31:29] が 0 でないときの読み方も定める。コードは読まない。
- **どう壊れるか**：非対称の機械で `topology.dies` が誤る。今 `die` を読むのは数と log だけである。
- **重さ**：きれいさ。

### F13：HPET と PM timer の同じ水晶という主張

- **場所**：`kernel/src/dev/refclock.c:20-21`。
- **文書**：ACPI 6.5 Table 4.14（`acpi:8484`）。HPET の文書は手元に無い。
- **要旨**：ACPI は PM timer について 14.31818 MHz の clock を述べる。HPET の周期は `COUNTER_CLK_PERIOD` から読むもので、同じ水晶である根拠は手元に無い。
- **どう壊れるか**：動作に影響しない。HPET を先に選ぶ理由の一部が確かめられない。
- **重さ**：きれいさ。

### F14：VM の判定と語が共通コードに残る

- **場所**：`kernel/src/arch/cpu.c:143`、`kernel/src/arch/cpu.c:183`、`kernel/src/arch/cpu.c:7`、`kernel/src/arch/idle.c:21`、`kernel/src/gui/wm.c:2602`。
- **文書**：AGENTS.md「VM の処理は実機と分ける」。
- **要旨**：
  - `cpu.c:143` は共通コードで `hypervisor` のビットを判定する。
  - `cpu_features_init` は `vm_detect` より前に走る。`vm/vm.h` の口を今は使えない。
  - 実機ではこの分岐は何もしない。
- **どう壊れるか**：実機の動作には影響しない。規則から外れる。
- **重さ**：きれいさ。

### F15：試験の範囲

- **場所**：`tests/check_tsc_rate.py:25-68`。
- **文書**：PPR 56713 の ValidValues（`p56713:10600`）。
- **要旨**：
  - 依頼文の「文書の書式の例」は、文書にある計算例ではない。期待値はコードと同じ式から作られている。
  - DID の 00h・2Dh・30h と FID の FFh は試していない。
  - `LockTscToCurrentP0` の場合（F5）が無い。
- **どう壊れるか**：範囲の端の誤りを見逃す。
- **重さ**：きれいさ。

## 合っていた手順

### TSC の周波数

- HWCR は MSR C001_0015、`TscFreqSel` は bit 24 で読み出し専用である（`p56713:10144`、`p55898:11006`、`v1:11632`）。
- PStateDef[0] は C001_0064、`PstateEn` は bit 63 である（`p56713:10556`）。
- Family 19h は CpuFid 7:0（10h-FFh、値 × 25 MHz）、CpuDfsId 13:8（08h-1Ah と偶数の 1Ch-2Ch）で、式は FID / DID × 200 MHz である。CpuFid × 5 MHz の行が食い違うという注釈も正しく、用語集と値の表が FID × 25 の側に揃う（`p56713:1376`、`p56713:10600`、`p55898:11400`）。
- CpuDfsId の本文は 30h まで述べるが、ValidValues は 2Ch までである。コードは ValidValues に従う。
- Family 1Ah は CpuFid 11:0（010h-FFFh）、CoreCOF は値 × 5 MHz である（`v1:2027`、`v1:12048`）。
- 周波数の欄がコヒーレントな fabric の全部のコアで同じという注釈は正しい（`p56713:10556`）。CpuVid だけが別である。
- Intel の式は ECX × EBX / EAX で、0 の扱いと `MAX_LEAF` の確かめも文書どおりである（`sdm:30293`、`sdm:240367`）。
- CPU ごとの周波数の比べは、同じレジスターの読みなので `num` と `den` の一致で足りる。

### 参照

- FADT の位置は PM_TMR_BLK 76、PM_TMR_LEN 91、Flags 112、X_PM_TMR_BLK 208 で、構造体と一致する（`acpi:10204`、`acpi:10258`、`acpi:10609`）。
- TMR_VAL_EXT は bit 8、HW_REDUCED_ACPI は bit 20 である（`acpi:10826`、`acpi:11066`）。
- 64 bit の欄を優先し、使えないときだけ 32 bit の欄を使う手順は文書どおりである（`acpi:9897`、`acpi:10609`）。
- HW_REDUCED_ACPI でこれらの欄を無視する手順は文書どおりである（`acpi:9893`）。
- PM timer は 3.579545 MHz、読み出し専用、32 bit で読み、始めの値は未定義で、I/O か memory にある（`acpi:7603`、`acpi:8466`）。
- 24 bit の timer の上位 8 bit は 0 で、差を mask する読みと矛盾しない（`acpi:8484`）。
- S3 の後に数え直す手順は、Table 4.14 の Sx の記述と合う（`acpi:8484`）。
- `vmm_map_mmio` は頁の途中の番地を保つので、memory の PM timer に使える。

### 性能の要求

- AMD の CPUID ビットは 8000_0008h EBX[27] である（`p56713:4827` 付近、`p55898:5851`、`v1:6142` 付近）。
- CPPC_CAP1 の順と CPPC_REQ の順は文書どおりである（`p56713:11520`、`p56713:11550`、`v1:13095`、`v1:13134`）。
- CPPC_ENABLE は Write-1-only で、何度書いても再開で問題ない（`p56713:11545`）。
- HWP の CPUID は EAX[7] と EAX[10]、MSR は 770h・771h・774h である（`sdm:216159`、`sdm:216247`）。
- HWP の有効化を他の MSR より先に行う（`sdm:216266`）。
- HWP_ENABLE は一度きりで、後の書込みは無視される。各 CPU と再開で書いて問題ない（`sdm:216247`）。
- HWP_CAPABILITIES と HWP_REQUEST のビット位置は文書どおりである（`sdm:216302`、`sdm:216371`）。
- 範囲は Lowest から Highest、Desired 0、Activity Window 0 で、17.4.10 の共通の推奨と一致する（`sdm:216945`）。
- EPP が無い機械で 0 を書くのは無視される（`sdm:216453`）。
- MPERF・APERF は E7h・E8h で、CPUID.06H:ECX[0] で確かめ、割り込みを閉じて 0 を書く（`sdm:215891`、`v1:2735`）。
- AMD の MPERF が P0 の周波数で C0 の間だけ進むという注釈は正しい（`v1:7460`）。

### CMCI

- `MCG_CMCI_P` は bit 10 である（`sdm:219475`）。AMD の MCG_CAP は bit 63:9 が予約である（`apm2:17905`）。
- `IA32_MCi_CTL2` は 280h から 29Fh で、CMCI_EN は bit 30、閾値は bit 14:0 である（`sdm:220068`、`sdm:285108`、`sdm:285289`）。
- LVT は 2F0h（x2APIC は 82Fh）で、Max LVT Entry 6 は 7 個の機械の値である（`sdm:220068`、`sdm:287775`、`sdm:209917`）。
- 初回の手順の順は、bank の主張、handler、LVT、記録と消去で 18.5.2.1 どおりである。
- 主張の全体を `cmci_lock` の下で行うので、CPU ごとに直列という要件を満たす。
- 閾値 1 は 18.5.2.2 の初期値である。
- 持ち主の bank だけを見る handler は 18.5.2.3 どおりである。
- CMCI の無い bank は文書上誤りを報告しないので、その bank の poll は要らない（`sdm:220284`）。
- `apic_init_cpu` の時に `cpu_features.cmci` は既に立っている。`cpu_setup(0)` が `irq_init` より先に走るからである。
- AP は `percpu_load` の後に `cmci_on` へ来るので、lock と CPU ごとの変数が使える。

### speculation の制御

- CPUID.7 EDX[26]・[27]・[29] と、IA32_SPEC_CTRL 48h の bit 0・1、IA32_ARCH_CAPABILITIES 10Ah の bit 1 は文書どおりである（`sdm:28885`、`sdm:283047`、`sdm:283689`）。
- 10Ah は CPUID.7 EDX[29] で確かめてから読む（`sdm:283681`）。
- AMD の CPUID.7 EDX の 26・27・29 は予約で、誤読は起きない（`v1:4531`）。
- Intel の機械で IBRS_ALL を一度立てることの可否は、下の「読み切れなかった範囲」に書く。

### topology

- leaf 8000_0026h の LevelType 3 は Die で、MaskWidth はその階層の ID の shift である（`apm:92202`、`p56713:5803`、`v1:7254`、`v1:7326`）。
- PPR の二版は語が少し違うが、同じ ID を持つ CPU が今の階層を共有するという結論は同じである。
- 0xB の AMD の level type は Thread と Processor だけで、die が無いという注釈は正しい（`v1:4622`）。
- 終わりの条件は LevelType 0 と EBX の 0 の両方を見る。APM と PPR の両方を満たす。

### タイマー

- PPR 55898 と 57238 の 2.1.12 に、APIC timer の 1 から 8 の刻みの記述がある（`p55898:2562`、`v1:2957`）。
- ARAT は CPUID 6 EAX[2] である（`v1` の CPUID_Fn00000006_EAX）。

### 時計の道

- 一周をまたぐ数えは mask の差で、読む間隔は一周の 4 分の 1 までに抑えている。文書の値（24 bit、32 bit、3.579545 MHz）から `span_ns` は正しい。
- `first_measure` は `data` の公開より前に走り、`clock_tsc_read` は `data` を読まない。
- `first_measure` の後の窓は `discipline` が新しい参照として始める。
- `clock_resume` の `ref->begin()` は差分の前の `hpet_resume` と同じ位置である。

### 眠りの復帰と競り

- `cpu_perf_setup` の `way` は最初の CPU が決めてから AP が読む。
- `cmci_on` は再開でも `spin_lock` で割り込みを閉じて主張する。
- `cpu_features_same` は required を先に見てから MSR を読むので、AP の IDT の前の fault を避ける。

### VM の分離

- RDP の大きさの範囲は `vm/rdp.c` へ移り、`gui/wm.c` の共通の道から消えた。
- `wm.c` の absolute pointer の注釈は USB tablet を含む形になった。

## 読み切れなかった範囲

- **IA-PC HPET 1.0a**：文書は `D:\Programes\OS DEV INFO` に無い。`refclock.c` の HPET の手順と、注釈の 2.3.3・2.3.4・2.3.5・2.3.7・2.4.1・2.4.7・3.2.4 は照らせなかった。
- **MS-RDPEDISP**：文書は手元に無い。`vm/rdp.c` の `disp_caps` の欄は照らせなかった。
- **Intel の speculation の緩和の文書**：SDM vol. 4 にはビットの名だけがある。IBRS_ALL を一度立てる手順が「IBRS always on」に当たるという注釈（`cpu.c:453-455`）は照らせなかった。STIBP を立てる条件、RSBA（10Ah bit 2）のある機械で return の予測を消さないという注釈（`cpu.c:469`）も同じである。Intel の機械で `stibp` のフラグは読まれるが、`stibp_always_on` が立たないので STIBP は立たない。これが正しいかは分からない。
- **文書の無い機械**：Family 19h の Model 01h・61h の外、Family 1Ah の Model 02h の外、Hygon の書式は照らせなかった。
- **実機**：レジスターの実際の値は見ていない。依頼のとおり、ビルド・QEMU・`tests/check_tsc_rate.py` は動かしていない。
- **差分の外**：`mca_setup` と AMD の threshold の道は、差分が関数を切り出しただけなので手順の全部は照らしていない。`lapic_timer.c` は注釈の差分だけを照らした。`clock.c` の窓の算術は文書の対象ではないので、文書では照らしていない。
- **起こした文字**：PPR の表は列がずれて起こされる。CPPC の bit 27、HWCR の bit 24、PStateDef の位置は順番を数えて読み、PPR 55898 の整った起こしと照らした。CPUID 8000_0008h EBX の PPR 57238 の行も同じである。
