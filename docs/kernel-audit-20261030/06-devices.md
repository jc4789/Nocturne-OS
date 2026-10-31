# {"agentType": "Plan", "description": "装置（dev）の監査", "toolUseId": "toolu_01CDerZSQ8fLTetf47BwacT3", "spawnDepth": 1, "requestShape": "background", "requestNonInteractive": true, "model": "opus", "finished": true}

## prompt

あなたは Nocturne OS（D:\Programes\llm vibe slop os）のカーネルの監査の一人です。読むだけの仕事です。サブエージェントを出さない。ファイルを書かない・変えない。ビルド・QEMU・git を変える命令をしない（git diff・git log・git show を読むのはよい）。Linux や他の OS のソースを開かない（設計の比べは公開された設計の知識で）。third_party（uACPI の上流など）は読まない。作業木には未コミットの変更があり、今のファイルをそのまま監査する。断片でなくファイルを全部読んで判定する。

## 目的
ユーザーはカーネルを消して一から建て直すことを考えている。この監査の結果で、どの持ち主の単位を消し、何を（あるなら）残すかを決める。直し方（patch）は提案しない（ユーザー：「Please do not suggest "patching" slop or bad architecture」）。

## あなたの範囲：kernel/src/dev/ の全部
pci.c・pci.h（列挙・設定空間・MSI/MSI-X）、pciuser.c（アプリのドライバー：SYS_PCI_*・SYS_DMA_*。NVIDIA の RM をアプリ nvrm として動かすのに使う。user/apps/init.c が claim する）、acpi.c（uACPI とのつなぎ。上流は読まない）、iommu.c・iommu_amd.c・iommu_intel.c、ahci.c、nvme.c、blk.c、xhci.c・usb.h・usbhid.c・usbhub.c・usbmsc.c・usbnet.c、audio.c・audio.h、fb.c・fbcon.c、input.c、power.c・pm.c・hibernate.c、entropy.c、serial.c、gpu.h。呼ぶ側と下の層（arch/apic.c・ioapic.c の割り込みの配り、mm の DMA の記憶、sys/syscall.c の該当の口、common/abi.h）は、つながりを判定するのに要る分だけ読む。

## 問う軸
- 装置と driver の型：装置と driver の抽象の継ぎ目があるか（二つ目・三つ目の driver が来たら呼ぶ側は変わるか）。
- 割り込み：割り込みの処理と後の仕事の分け方、宛先の CPU、MSI-X と queue の数、ポーリングに頼る所。
- 記憶装置の queue：NVMe の queue を CPU ごとに持つか、要求の経路の lock。
- DMA と IOMMU：DMA の記憶の持ち主、IOMMU の領域、アプリのドライバーへの写像の安全。
- 電源：S3・休止・装置の止めと戻し。
- 時間：装置の待ち（時間切れ・spin の回数・sleep の輪）の単位と値。ユーザーは時間の扱いが全体として根から誤っていると言っている（「our timing is still off. the tolerances youve allowed are way too big.」「THIS IS NOT JUST ABOUT SOUND.. THIS IS ABOUT EVERYTHING」）。音（audio.c）は、装置へ送る位置と時刻の扱いをこの目で見る。

## 製品の前提（ユーザーの言葉と決まり）
- 汎用の OS。CPU・装置・プログラムの数が増えても成り立つ形（「IF YOUR KERNAL DESGIN IS NOT BETTER THEN LINUX AND NT, THEN DONT EVEN SHOW IT TO ME」）。
- 対象は実機だけ（ユーザー、今日：「WE TARGET REAL HARDWARE NOT VM'S」「Hyper-V is not wanted」）。ユーザーの PC：Ryzen 5 7600X（Zen 4、AM5 の platform、AMD IOMMU）、RAM 32 GB、RTX 4070（Resizable BAR 有効）、NIC は RTL8125。上は 2 ソケットの EPYC 9996（論理 CPU 1024、NUMA、PCIe の装置が多い）。実機の記憶装置・USB・音の装置の型は、コードと資料から読める範囲で書き、分からなければ未確認と書く。
- 後ろ向きの互換は要らない。古い装置・古い方式のための道は悪い。ディスクは一台。
- UNIX・POSIX・Linux ではない（ネイティブ ABI：common/abi.h、仕様 docs/nocturne-native-abi.md）。GUI が先。
- shim（代役・何もしない lock・偽の関数）とスパゲッティ（一つの呼ぶ側のための包み、渡すだけの関数、その場の特例の分岐、決め打ちの値、ある環境の都合を別の部品へ持ち込むこと）は禁止。抽象化の層は責務の本当の継ぎ目に要る。

## 判定の物差し
- 規模：装置・queue・CPU・アプリのドライバーが十倍で何が詰まるか。数に比例する走査、一つの大きな lock、CPU 0 や一つの thread に寄せる仕事、決め打ちの上限と表の大きさ、ポーリング。
- 正しさ：仕様との違い。仕様は D:\Programes\OS DEV INFO にある（PCIe・PCI・NVMe・AHCI・xHCI・USB・VT-d・AMD IOMMU・ACPI 6.5・high-definition-audio-specification.pdf など）。PDF は大きいので要る節だけを探して読む。記憶でなく原文で照らす。競争、memory ordering、DMA の一貫性、寿命。
- 持ち主：一つの責務が一つの持ち主にあるか。分断・重複。stub・空の実装。
- Linux・NT の公開された設計（driver の型、割り込みの後の仕事、block の層、DMA の API）と軸ごとに比べて劣る所。

## 報告の形（日本語、平文）
持ち主の単位（ファイルか部品）ごとに：
1. 責務（一行）
2. 判定：「残せる」「消して建て直す」「消すだけ（要らない）」のどれか一つ。「直せば使える」という中間は書かない。
3. 根拠：file:line と、その行が何をしているか。コードで確かめたものだけ。確かめていないものは「未確認」と書く。
4. 十倍・1024 CPU で何が詰まるか。
5. Linux・NT の公開の設計と比べて劣る軸（あれば）。
6. 建て直すなら、正しい形が満たすべき要件（設計の要件。patch ではない）と、他の部品との継ぎ目。
最後に：この範囲で「残せる」と言えるものとその理由、実機（7600X の PC）で要る装置のうち driver が無いもの、全体の所感（三行まで）。同じことを繰り返さない。

## reply

kernel/src/dev/ の監査（読むだけ。git status で dev/ に未コミットの変更は無く、今の木をそのまま判定した。全ファイルを全部読んだ）
パスは D:\Programes\llm vibe slop os\kernel\src\ からの相対で書く。README・AGENTS・user/ はリポジトリの根からの相対。

====================================================================
共通の事実（どの単位にも効くので、ここに一度だけ書く）
====================================================================
A. 装置の割り込みは全部 CPU 0 に来る。
   根拠：dev/pci.c:200（interrupt_dest = lapic_current_id()）、dev/pci.h:94、dev/pciuser.c:1079。
   これは規則として書かれている（.claude/rules/kernel.md:15）。

B. 割り込みベクターの表は全 CPU で一つ。
   - 手で決めた番号：xHCI 0x40–0x47（dev/xhci.c:19）、NVMe 0x48–0x4B（dev/nvme.c:19）、RTL8125 0x4C（net/rtl8125.c:50）。
   - 残りは 0x50–0xEF の 160 個を、一つの vec_owned[256] で配る（arch/idt.c:132-153）。

C. ISA の線（ACPI の SCI、PS/2）は 8259 PIC を通る。
   - pic_unmask は 8259 のマスクをそのまま書く（arch/idt.c:215-218）。
   - ioapic_route は定義だけで、呼ぶ所が無い（arch/ioapic.c:118）。
   - PCI の INTx は使わない。

D. 実機の今の構成（README_EN.md:262-270）：
   - USB スティックから起動し、C: もスティックの上。
   - キーボードとマウスは USB。LAN は RTL8125B。音は出ない。
   - つまり実機では、C: の I/O もキーもマウスも、一本の usb スレッドを通る（9 の項）。
   - 内蔵ディスクの型（NVMe か SATA か）は、コードにも文書にも無い：未確認。

E. 実機では眠り（S3）も休止も出ない。止め具が二つあり、両方とも効いている。
   - アプリが PCI 機能を握っている間は拒否（dev/pm.c:251-254）。init が RTX 4070 を nvrm に握らせる（user/apps/init.c:71-80）。
   - USB のディスクがある間も拒否（dev/xhci.c:1544-1547）。実機の C: は USB スティック。

F. 実機のコードに VM の処理が混じっている所（dev/ の中）。ユーザーの「DO NOT MIXIN VM LOGIC WITH REAL HARDWARE CODE EVER」に反する。
   - audio.c:15, 769：vm_audio_init。カードは vm/ac97.c にしか無い。
   - audio.c:75-80, 435：リモートデスクトップの音の出口。実体は vm/rdp.c:1327, 1374。
   - audio.h:128-135、audio.c:143-147：AC'97 の trace。
   - fb.c:12, 146, 391-393, 438, 446：WHPX での測定を理由にした分岐と、vm_display_pm。
   - gpu.h：実装は vm/virtio_gpu.c:576-731 だけ。
   - hibernate.c:26, 146, 637：vm_hibernate_blocker、vm_suspend。
   - input.c:9, 201-207, 239, 290 と input.h:42：vm_ps2_flips_y、Hyper-V 第 2 世代の判定。
   - power.c:1, 8, 89：vm_power_off。
   - pm.c:23, 220：vm_resume。
   - pciuser.c:634-636：QEMU の edu 装置の試験のための appdrivers=any。
   - blk.h:1：Hyper-V SCSI。usbhid.c:3：Hyper-V のキーボード。
   - 文書の側：AGENTS.md:49 は今も「Hyper-V 第 2 世代と拡張セッションは要件のまま」と書いている。ユーザーの「Hyper-V is not wanted」と食い違い、エージェントが VM の道を足し続ける元になっている。

G. 時間の扱い（ユーザーの「tolerances way too big」に当たるもの）。数字は各項に書く。型は六つ。
   (1) 失った割り込みを隠す保険の poll：NVMe の 10 ms、xHCI の 10 ms と、4 ms の poll への退避。
   (2) 時間でなく回数で数える待ち：power.c の io_wait の輪、serial.c の 100000 回、entropy.c の空の輪。
   (3) 割り込みを閉じたまま秒の単位で回る待ち：
       - NVMe の停止は 1 台あたり最大 10 s。AHCI は 10 s。
       - PCI の復帰は機能ごとに 1 s。
       - IOMMU は lock を持ったまま 100 ms。
   (4) 事象に装置の時刻が付かない：
       - 入力の事象には時刻の欄が無い。
       - 音の位置には、読んだ時の clock の時刻を付けるだけで、それも ABI に出ない。
       - GOP の画面の拍は EDID からの推測。
   (5) 遅れを大きな緩衝で吸う：音の書き手は最大 170 ms 先行できる。無音の出口は 10 ms の塊で進む。
   (6) 規格の値を丸める：NVMe の RTD3E を 10 s で切る。

====================================================================
単位ごとの判定
====================================================================

1. PCI の核（pci.c, pci.h）
責任：PCIe の列挙、構成空間、BAR、MSI/MSI-X、FLR・SBR・RRS のリセット、構成の保存と復帰。
判定：消して建て直す

証拠：
- セグメント 0 しか見ない（pci.h:47、pci.c:15、acpi.c:186）。
- 列挙は 256×32×8 の総当たり（pci.c:210-252）。上流とのつなぎは O(n²)（pci.c:254-257）。
- pci_find・pci_find_class・pci_find_function は線形に探して最初の一つを返す（pci.c:260-302）。
- driver を装置に結ぶ仕組みが無い。各 driver が pci_at を自分で回す（例：nvme.c:445-452）。
- MSI を MSI-X より先に使い、ベクターは一本（pci.c:497-507）。APIC ID が 255 を超える CPU は拒否（pci.c:500）。
- pci_enable_busmaster が I/O デコードまで開ける（pci.c:272-277）。
- リセットは機械全体で一つの reset_mutex（pci.c:757, 952-964）。
- AML が構成空間に触れるたびに、spinlock の下で pci_find_function の線形探索をする（pci.c:986-1004）。
- 復帰の待ち：
  - 割り込みを閉じた pause の輪（pci.c:1334-1341）。
  - 眠れない時は wait_step が回る（pci.c:163-166）。
  - 機能ごとに最大 1 s の ready_id を、一つずつ順に（pci.c:1356）。
  - ポートごとに最大 1 s の links_retrain。COMPLETION_FLOOR_MS 1000（pci.c:695）。
- カーネルは _OSC を評価しない。_OSC が出てくるのはアプリに禁じる名の表だけ（pciuser.c:863）。それでもリンクの再訓練と SBR は OS が行っている。

10 倍の装置・1024 CPU で壊れるもの：
- 全装置の割り込みが CPU 0 の一つに集まる。ベクターは機械全体で 160 個しかなく、NVMe を CPU ごとのキューで使うことは原理的にできない。
- APIC ID が 255 を超える CPU には送れない。2 ソケットの EPYC 9996（論理 CPU 1024）では大半の CPU がこれに当たる。
- セグメントが複数ある機械では、セグメント 0 以外の装置が見えない。
- 復帰は「機能の数 × 1 s」を一つの CPU で順に回し、その間ほかの CPU は止まったまま。

Linux／NT より悪い軸：
- Linux：bus_type と driver の照合、MSI-X を CPU に散らす managed affinity、CPU ごとのベクター表、装置ごとのリセットの lock、親子の順の非同期の resume、ホストブリッジでの _OSC の交渉。
- NT：PnP の装置木、IoConnectInterruptEx のグループ affinity、MSI-X のメッセージごとの affinity、pci.sys の _OSC。
- ここにはそのどれも無い。

建て直すなら（設計の要件と継ぎ目）：
- MCFG の全セグメントを使い、ブリッジをたどって列挙する。親子の装置木を持つ。
- 照合表で driver を結ぶ（probe と remove）。
- MSI-X を先に使い、キューごとに一本ずつ、CPU ごとのベクター空間から取って affinity を付ける。arch の割り込みの層との継ぎ目。
- ホストブリッジで _OSC を交渉し、AER・DPC・hotplug・PME・LTR の持ち主を決める。ACPI との継ぎ目。
- リセットの lock は装置ごと。
- suspend/resume は部分木ごとに並列で、眠れる文脈で行う。

2. アプリのドライバーの道（pciuser.c, pciuser.h, sys/syscall.c の SYS_PCI_*・SYS_DMA_*）
責任：アプリに PCI 機能を握らせ、構成空間・BAR・割り込み・DMA の頁・ACPI の一部を渡す。
判定：消して建て直す

証拠：
- 握れるのは init だけで、試験のための抜け道 appdrivers=any も同じ行にある（pciuser.c:636）。
- 機械全体で一つの claim_mutex を、秒の単位のリセットの間ずっと持つ（pciuser.c:81, 647）。
- 割り込み：
  - claim_of_vector[256] と一つの claim_irq_lock、ISR は CPU 0（pciuser.c:86-106）。
  - 一つの握りにベクター 32 本まで（pciuser.c:39, 1078）。宛先は CPU 0（1079）。INTx は無い（1074-1107）。
- 画面の都合が PCI の握りのファイルの中にある（pciuser.c:143-840）：
  - screen hold・scanout・mode・image・vblank、wm_* と fb_* の呼出し、fbhold を見張るスレッド。
  - ABI の番号も SYS_PCI_GIVE_SCREEN と SYS_PCI_SCREEN_* として PCI の名前空間に入っている（sys/syscall.c:2861-2870）。
- どの握りでもホストブリッジを全部読める（pciuser.c:976-981）。
- DMA：
  - IOVA は物理番地そのもの（pciuser.h:28-29、iommu.h:5）。
  - 物理番地をそのままアプリへ返す（sys/syscall.c:895-917）。
  - 量の上限は pmm_free_pages との比較だけ（sys/syscall.c:863）。
  - 頁を一枚ずつ確保してゼロで埋める（881-886）。
- 電源：眠りの知らせがアプリのドライバーに届かない（pm.c:251-252 のコメント）。だから握りがあるだけで眠りも休止も拒否される（pm.c:253-254）。

10 倍・1024 CPU で壊れるもの：
- GPU 級の装置（1 台 32 本）が 5 台あれば、160 本のベクターが尽きる。
- 全部の握りの割り込みが CPU 0 で一つの lock を取る。
- 握りの開始と終了は、機械全体で一度に一つずつ。

Linux／NT より悪い軸：
- Linux の VFIO：IOMMU のグループ、利用者が選ぶ IOVA（物理番地は見せない）、eventfd に結んだ割り込み（ホスト側の IRQ の affinity が効く）、runtime PM。
- NT：GPU は WDDM のカーネル側の miniport と、表示の持ち主である dxgkrnl に分かれる。表示を PCI の ABI に入れない。
- ここは物理番地を見せ、表示の方針を PCI の ABI に混ぜ、割り込みは CPU 0。

建て直すなら（設計の要件と継ぎ目）：
- 握りは「機能＋自分の IOVA 空間を持つ IOMMU のドメイン＋割り込みの物」にする。割り込みの物は待ちの物に結び、受けるスレッドの CPU へ向ける。
- DMA の量は、握り（とアプリ）ごとに数える。
- 電源の段階（D0→D3→D0）を、期限つきでアプリへ知らせる約束を持つ。
- 表示は表示の部分系の ABI として切り離す（scanout、mode、装置の時刻つきの vblank）。GPU の driver はそこへ登録する。

3. ACPI の表の登録簿（acpi.c:1-154 と、acpi.h の表の形）
責任：RSDP から XSDT/RSDT をたどり、検査和と置き場所を確かめて、表を引けるようにする。
判定：残せる

証拠：
- 検査和（acpi.c:41-46）。表がファームウェアの範囲にあるかの確かめ（50-65）。
- XSDT が壊れていれば RSDT を使う（73-104）。
- 有効な表に印を付けるビットの表（106-137）。acpi_find と、子の項目のたどり（139-154）。
- 構造体の大きさを _Static_assert で確かめている（25-30）。
- VM の分岐も、大域の lock も無い。

10 倍・1024 CPU：表は数十なので、線形の acpi_find で困らない。MADT と SRAT の項目は CPU 数に比例して増えるが、それをたどるのは呼ぶ側（arch）。

Linux／NT より悪い軸：無い（同じ程度の小さな仕事）。

残すときの継ぎ目：Limine の rsdp_req と memmap の型に依る所は三か所だけ（acpi.c:32, 50-59, 75-77）。起動の約束を変えるなら、そこだけ変わる。

4. ACPI の AML の継ぎ目（acpi.c:156 以降：uACPI の kernel API、SCI、GPE、アプリへの評価）
責任：uACPI にカーネルの役務を渡し、SCI と GPE を回し、アプリに名前空間の一部を評価させる。
判定：消して建て直す

証拠：
- SCI は 8259 を通って CPU 0 へ（acpi.c:160-161, 355-365）。IOAPIC の経路は使わない（共通の C）。
- uacpi_kernel_unmap は何もしない（acpi.c:177）。AML が触った MMIO は、直写像に恒久に残る（mm/vmm.c:495-517）。
- セグメント 0 以外の PCI は開けない（acpi.c:186）。
- GPE と Notify の仕事は、一本の「acpi」スレッドで回す（acpi.c:382-446）。コメントは CPU に縛れないと言うが、kthread_create_on はある（kernel.md:11）。
- 凍結中の待ちは sti/pause/cli で回る（acpi.c:278-312）。LINKS_MAX 32（acpi.c:507）。
- _OSC・EC（PNP0C09）・熱・起こし（_PRW）を扱う所が、カーネルのどこにも無い（grep で _OSC は pciuser.c:863 の禁止の表だけ、ほかは 0 件）。
- 名前空間から装置を数えて driver を結ぶ道も無い。
- アプリ向けに PCI の node を探して評価する処理が、このファイルにある（acpi.c:581-799）。

10 倍・1024 CPU：SCI と GPE が、CPU 0 と一本のスレッドに詰まる。平台の装置（GPIO・I2C・UART など）が増えても、どれにも driver が付かない。

Linux／NT より悪い軸：
- Linux：SCI を GSI として IOAPIC に載せ、GPE は kacpid で CPU 0 に縛って回す（firmware の SMI の都合）。_OSC・EC・thermal・wake の driver があり、名前空間の _HID/_CID から platform device を作る。
- NT の acpi.sys も同じ範囲を受け持つ。

建て直すなら（設計の要件と継ぎ目）：
- FADT の SCI_INT を、MADT の override を通して IOAPIC に載せる。
- GPE と Notify は専用の仕事場で回し、CPU 0 に縛る。
- _OSC は PCI の核との継ぎ目。
- 名前空間から装置を数え、装置木に入れる。
- アプリへの評価は「握った機能の範囲」の約束として、アプリのドライバーの道の側に置く。
- uACPI そのものを残すかは、third_party を読んでいないので未確認。

5. IOMMU（iommu.c, iommu.h, iommu_amd.c, iommu_intel.c）
責任：AMD-Vi と VT-d の装置の表・ドメイン・無効化・割り込みの付け替え、眠りの前後の扱い。
判定：消して建て直す

証拠：
- 守るのはアプリが握った機能だけで、カーネルの driver の DMA は素通り（iommu.h:9-10、iommu_amd.c:9-11）。AMD では核の装置の DTE はゼロで、割り込みも付け替えない。
- ドメインは恒等写像（iommu.h:5）。
- 機械全体で一つの attach_mutex（iommu.c:40）。
- グループの計算は全機能を二重に回る（iommu.c:559-639）。domain_id_new は 65535 まで線形に探す（iommu.c:760-770）。
- AMD：
  - 全コマンドが一つの amd_lock の下で、割り込みを閉じて走る。同期ではユニットごとに最大 100 ms 回る（iommu_amd.c:161, 581-609）。
  - コマンドを全ユニットへ同報する（516-518）。
  - unmap のたびにドメイン全体を無効化する（521）。
  - 握り一つに IRTE は 32 個（88）。事象の ISR は CPU 0（572-577）。
  - amd_ioapic は何もせずに true を返す（745-748）。
- Intel：
  - IRT は 4096 項目。IOAPIC は GSI 256 まで。MSI の塊は 120 機能分で、線形に探す（iommu_intel.c:131-136, 767-780）。
  - 全体で一つの units_lock の下で 100 ms 回る。
  - DMAR が二つ以上の機械と、セグメントが 0 でない機械は拒否する。
- 故障の報告は毎秒 8 件まで（iommu.c:185-200）。

10 倍・1024 CPU で壊れるもの：
- ユニットが増えると、同報と同期の待ちがユニット数に比例して伸びる。しかも一つの lock の下で、割り込みを閉じたまま。
- APIC ID が 255 を超える CPU（x2APIC）へ核の装置の割り込みを向けるには付け替えが要るが、核の装置は付け替えの外にいる。
- unmap のたびに全体を無効化するので、DMA の頁の出入りが多いと装置が止まり続ける。

Linux／NT より悪い軸：
- Linux：全装置に既定のドメイン（DMA API）、CPU ごとの溜めを持つ IOVA の確保、無効化をまとめる flush queue、ユニットごとの命令列の lock、全装置の割り込みの付け替え。
- NT：Kernel DMA Protection で、核の driver の DMA も区切る。
- ここは核の DMA が無防備で、無効化も lock も粗い。

建て直すなら（設計の要件と継ぎ目）：
- 起動の最初から、全装置（核のものもアプリのものも）をドメインに入れる。
- ドメインごとの IOVA の確保を持ち、CPU ごとの溜めを付ける。
- 無効化は、その装置を受け持つユニットだけに、範囲でまとめて送る。
- ユニットごとの命令列を持ち、完了は眠って待てるようにする。
- 全装置の割り込みを付け替え、x2APIC の ID を使う。
- グループは ACS をたどって一度だけ、O(n) で計算する。
- 継ぎ目：PCI の核（装置木と ACS）、mm（DMA の頁）、arch の割り込みの層。

6. ブロック層（blk.c, blk.h）
責任：ディスクの登録と、読み・書き・永続化の入口、それに眠りの間の門。
判定：消して建て直す

証拠：
- read・write・persist の同期の関数ポインターだけ。要求の列も非同期も scatter-gather も無い。MAX_BLK 16（blk.c:8）。
- 要求のたびに一つの gate_lock を取り、holder と inflight を数える（blk.c:36-56）。
- 一時の写しの手助けがある（blk.c:83-110）。

10 倍・1024 CPU で壊れるもの：
- 1024 の CPU からの I/O が、毎回一つの lock を通る。
- 下の driver も一件ずつなので、深さ 1 の同期 I/O しか出ない。
- 17 台目のディスクは登録できない。

Linux／NT より悪い軸：
- Linux の blk-mq：CPU ごとの送り列をハードのキューへ対応させ、tag、非同期の完了、SG、plug を持つ。
- NT の StorPort：コアごとのキューと MSI-X、DPC での完了。
- ここにはどれも無い。

建て直すなら（設計の要件と継ぎ目）：
- SG を持つ要求の物と、非同期の完了。
- CPU（か NUMA の node）ごとの送り口を、ハードのキューへ対応させる。tag を持つ。
- FUA と flush の意味を要求に持たせる。
- 眠りの門はキューごとの凍結にして、毎要求の大域の lock にしない。
- 継ぎ目：上は fs/store、下は NVMe などの driver、横は IOMMU（DMA の写像）。

7. NVMe（nvme.c）
責任：NVMe の制御器の起動、名前空間、I/O、停止。
判定：消して建て直す

証拠：
- MAX_CTRL 4、MAX_NS 8（nvme.c:17-18）。ベクターは 0x48＋番号の手決め（19）。
- キュー：
  - I/O キューは一組、IO_ENTRIES 64（20-21）。それなのに c->lock の mutex の下で一度に一件（76）。
  - Number of Queues に 1 を設定する（210-211）。
- 128 KiB の一時の写しに、毎回 memcpy する（22, 300）。
- 待ち：MSI があっても 10 ms ごとに見に行く保険。MSI が無ければ 1 ms ごと（168）。
- ISR は CPU 0 で、全部の制御器を回る（105-112）。
- 時間切れ：I/O 30 s、admin 5 s。時間切れからの回復は、制御器全体のリセット（270-272）。ready_ns は少なくとも 2 s（368）。
- 停止（414-429）：
  - RTD3E を 1 s〜10 s に丸め、割り込みを閉じて pause で回る。制御器ごとに順番に。
  - 手元の NVMe Base 2.4（NVM-Express-Base-Specification-Revision-2.4、114 頁）は、「a minimum of the RTD3 Entry Latency」を待つよう勧めている。
  - 10 s の上限は、それより長い RTD3E を報告する装置で、停止を途中で打ち切ることになる。
- 眠りの後は、次の I/O の時に遅れて全体をリセットする（434-439）。

10 倍・1024 CPU で壊れるもの：
- 機械全体で、制御器 1 台あたり同時に 1 コマンドしか出ない。CPU が増えても I/O は一件ずつ。
- 5 台目の制御器は見えない。
- 停止は「台数 × 最大 10 s」を、割り込みを閉じて順に回す。

Linux／NT より悪い軸：
- Linux：CPU ごとのキュー対（制御器の上限まで）、キューごとの MSI-X と affinity、要求の頁から直接 PRP/SGL（写しなし）、APST、全制御器を並列に停止。
- NT の stornvme も、コアごとのキューと MSI-X を持つ。

建て直すなら（設計の要件と継ぎ目）：
- Number of Queues は、CPU 数（か node ごと）と制御器の上限の小さい方にする。
- CQ ごとに MSI-X を一本、受け持つ CPU へ向ける。
- PRP/SGL は IOMMU の写像から直接作り、写しをしない。tag はキューごと。
- 時間切れは Abort から段階を踏み、いきなり全体をリセットしない。
- 停止は RTD3E を下限として、全台を並列に、眠れる文脈で行う。APST で電源状態を使う。
- 継ぎ目：blk、IOMMU、PCI の割り込みの割り当て。

8. AHCI（ahci.c）
責任：SATA の HBA。
判定：消すだけ（要らない）

証拠：
- MAX_HBA 4。スロット 0 だけで一度に一件、NCQ は無い、128 KiB の写し（ahci.c:1-5, 239-282）。
- 10 ms と 1 ms の poll。
- 停止では STANDBY IMMEDIATE を出し、割り込みを閉じて最大 10 s 回る（458-473）。
- IPM を常に active に固定（176-179）。hba_take は 2 s 眠ることがある（404）。
- kernel.md:21 の起動の行「store: C: … on sata0」は QEMU の ICH9 の構成。

理由：
- ユーザーは「一台のディスク」「後ろ向きの互換は不要、とても新しい機械」と言っている。
- 内蔵ディスクが SATA かは未確認。だが SATA だったとしても、この実装（一件ずつ、NCQ 無し）から残せるものは無い。

10 倍：深さ 1 のまま。

Linux／NT より悪い軸：Linux の libahci も NT の storahci も、32 スロットと NCQ を使う。

建て直すなら（SATA が要ると決まった時だけ）：32 スロットと NCQ、blk の非同期の要求、あればポートごとの MSI。

9. USB の核（xhci.c, usb.h, usbhub.c）
責任：xHCI の制御器、リング、事象、port、hub、列挙、class driver への転送の口。
判定：消して建て直す

証拠：
- スレッド：
  - 一本の「usb」スレッドが、全部の制御器と全部の class driver を回す。ISR は起こすだけ（usb.h:3、xhci.c:478-491, 1447-1508）。
  - 待ちの中で事象を回す、入れ子の輪がある（xhci.c:461-476）。bulk の待ちの間に、HID の報告や hub の仕事が入り込む。
  - キーの繰り返し（usbhid_tick）と usbhub_work は外側の輪でしか回らない（1485, 1495）。
- 割り込み：
  - ベクターは 0x40＋番号の手決め。MAX_HC 8（xhci.c:15, 19, 1336）。
  - interrupter は一つ、事象のリングは 1024 TRB、IMOD は 250 µs（17-18, 1335）。
  - MSI が届かなければ 4 ms の poll に落ちる（1465-1471, 1498）。状態は 1 s ごとに見直す。
- isochronous が無い（714）。USB の音とカメラは扱えない。
- 電源：眠りの後は全体をリセットして列挙し直す（1527-1542）。USB のディスクがあれば眠りを拒否（1544-1547）。
- 転送：usb_bulk は usb スレッドの上だけで動く同期の転送で、最大 64 KiB の写しを通る（822-850）。
- 時間の値：
  - control 2 s、command 3 s＋abort 5 s。
  - port の debounce 100 ms。reset は 500 ms を 1 ms ごとに poll。
  - hub：HUB_PORTS 15、10 ms ごとの poll を最大 500 ms、debounce 100 ms。
  - 停止は 20 ms を pause で回る（1519）。起動の落ち着き待ちは 500 ms（1450）。

実機での意味：共通の D の通り、C:（USB スティック）・キーボード・マウスが同じスレッドを通る。BOT の転送を待つ間は、キーの繰り返しも hub の変化も止まる。

10 倍・1024 CPU で壊れるもの：
- 9 台目の制御器は見えない。
- USB の仕事が全部、一つのスレッドと CPU 0 の割り込みに一列に並ぶ。
- interrupter が一つなので、MSI-X を散らせない。

Linux／NT より悪い軸：
- Linux：URB の非同期の完了（割り込みを受けた CPU の BH で返す）、hub_wq、class driver ごとの文脈、isochronous、眠りを越えて装置を残す USB persist。
- NT の usbxhci＋UCX：端点ごとの列、DPC での完了、isoch、selective suspend。
- ここは一本の糸と、入れ子の待ち。

建て直すなら（設計の要件と継ぎ目）：
- 非同期の転送要求と完了の callback。callback は、割り込みを受けた CPU の遅延の文脈で走らせる。
- interrupter を複数持ち、MSI-X を散らす。isochronous を持つ。
- hub は独立の class driver にする。class driver は制御器のスレッドで走らせない。
- 眠りの後は記述子を確かめ直して装置を残し、列挙し直さない。
- 継ぎ目：PCI の割り込み、IOMMU（転送の DMA）、blk（USB のディスク）、入力。

10. USB HID（usbhid.c）
責任：USB のキーボード・マウス・タブレットの報告を読み、入力の層へ送る。
判定：消して建て直す

証拠：
- HID の usage を、PS/2 の scan code set 1 に訳して送る（usbhid.c:56-75）。
- キーの繰り返しはカーネルが作る。500 ms と 33 ms（15-16）。
- ボタンは 8 まで読むが、送るのは 3 つだけ（196 の buttons & 7）。
- キーの欄は 16 まで（273-276）、usage の一覧も 16 まで（244, 355）。
- 読む page は、keyboard（0x07）と X・Y・wheel・button 1–8 だけ（278-322）。
  - consumer page（音量や再生のキー）、横の wheel（AC Pan）、Resolution Multiplier は読まない。
- Caps の LED は全キーボードで一つ。

10 倍：キーボードを何台つないでも、修飾キーの状態は一つ（18 の項）。

Linux／NT より悪い軸：
- Linux の hid-core：全部の usage を時刻つきの input event にし、繰り返しは input 層の timer で装置ごとに作る。
- NT の hidclass＋kbdhid も、usage のまま上へ渡す。
- ここは一度、1980 年代の scan code に落としてから戻している。

建て直すなら（設計の要件と継ぎ目）：
- 報告の記述子を完全に解析する（全 page、Push/Pop、配列と変数、複数の報告 ID）。
- usage のまま、時刻つきの事象として入力の層へ送る。
- 繰り返しは入力の層（か GUI）で、装置ごとに作る。LED も装置ごと。
- 継ぎ目：USB の核と入力の層。

11. USB の大容量記憶（usbmsc.c）
責任：USB のディスク（BOT）を blk に登録する。
判定：消して建て直す

証拠：
- BOT だけで、UAS は無い。usb_run で usb スレッドの上で同期に動く（usbmsc.c:210-238）。
- TEST UNIT READY を 250 ms × 40 回、合わせて 10 s まで繰り返す。
- 転送は usb_bulk の 64 KiB の写しを通る（xhci.c:822-850）。
- 今の実機の C: はこれ（README_EN.md:262-270）。

10 倍：スティックが 2 本でも、同じスレッドで一件ずつ。

Linux／NT より悪い軸：
- Linux：usb-storage（BOT）と uas（UAS はストリームで多重）を、SCSI 層の上で非同期に動かす。
- NT：usbstor と uaspstor。

建て直すなら（設計の要件と継ぎ目）：
- UAS を先に使い、BOT は UAS の無い装置だけに使う。
- blk の非同期の要求に直接つなぐ。LUN ごとに登録する。
- 抜き差しを blk と store に知らせる約束を持つ。

12. USB のネットワーク（usbnet.c）
責任：CDC ECM/NCM の USB LAN。
判定：消すだけ（要らない）
証拠：
- CDC ECM/NCM だけで、一台まで（usbnet.c:3-4）。
- 一つの NTB に一つの datagram しか入れない。stack の上に 1024 バイトの頭の配列を置く。
- 対象の PC の LAN は基板の RTL8125B（README_EN.md:263）。

13. 音（audio.c, audio.h）
責任：アプリの流れを混ぜて一つの出口（カード、リモート、無音の出口）へ渡し、再生した位置を返す。
判定：消して建て直す

証拠：
- 実機の音の driver が無い。audio.c:769 に「no driver for real sound hardware yet」とあり、カードは vm/ac97.c だけ。README_EN.md:267 も音は出ないと書いている。
  - だから実機での音の時計は「無音の出口」になる。alarm で 10 ms の塊を回し、位置は clock_now_ns から作る（audio.c:21, 303-348）。
- 形式と上限：
  - 48 kHz・S16・ステレオに固定（audio.h:21-22）。流れは MAX_STREAMS 8 まで（audio.c:17）。
  - 書き手の先行は最大 8192 フレーム、つまり 170 ms（18）。
  - RUNS 32 の根拠は「AC'97 の環の全部」（20）。
- lock：一つの audio_lock と card_lock を、割り込みを閉じて取る。混ぜる仕事は、カードの割り込みの中でその lock の下で行う（audio.h:67-68、audio.c:47-57, 184-238）。
- 位置の時刻：
  - カードは位置を読んだ直後の clock_now_ns を付けるだけ（vm/ac97.c:94, 108）。装置の時計も、時計のずれの模型も無い。
  - その時刻は ABI に出ない（audio.c:736-738「the ABI has no field for that time yet」、status_of 739-746）。
  - だからアプリは、played_frames がいつの値か分からない。無音の出口なら最大 10 ms、AC'97 なら buffer 一つ分古い。
- 流れごとに飽和の足し算をするので、音が割れる所が混ぜる順で変わる（audio.c:222-223）。
- 共通の口に VM の物が入っている：AC'97 の trace（audio.h:128-135、audio.c:143-147）、リモートデスクトップの出口（75-80, 435）。
- 流れを閉じるとき、最大 1 s 待つ（641）。

10 倍・1024 CPU で壊れるもの：
- 流れは 8 本まで。
- 全部の流れの混ぜが、一つの lock と一つの CPU（カードの割り込みは CPU 0）で、割り込みを閉じたまま走る。
- 出口は一つだけ。HDMI の音と基板の音を同時に扱えない。

Linux／NT より悪い軸：
- NT：音の engine は利用者側の専用の高優先スレッドで、周期ごとに混ぜる。IAudioClock は「装置の位置＋QPC の時刻」を組で返す。
- Linux：ALSA の snd_pcm_status も位置と時刻を組で返し、HDA では装置の wall clock から audio_tstamp を作れる。PipeWire は時計のずれを測って再標本化する。
- ここは時刻を返さず、装置の時計も持たない。

規格：HDA 1.0a §3.3.16 の Wall Clock Counter（オフセット 30h）は、24 MHz で数える 32 ビットの数で、約 179 秒で一周する。位置と装置の時計を組にするための物で、実機の音を作るなら必要。

建て直すなら（設計の要件と継ぎ目）：
- 装置の driver（HDA）が、位置（LPIB か DMA の位置の buffer）と装置の時計（WALCLK）を組で読む。システムの clock との対応とずれを模型にする。
- ABI は「位置、その時刻、時計の出所」を返す。
- 混ぜ手は割り込みの外の実時間のスレッドで走り、周期は装置が決める。
- 形式の変換と再標本化を持つ。出口は複数（装置の一覧と既定の選び）。
- 継ぎ目：sys の clock（時刻の約束）、PCI（HDA の制御器）、GUI の映像との同期。

14. 画面（fb.c, fb.h）
責任：ファームウェアの GOP の framebuffer、アプリのドライバーの scanout への切替、hold/release、色の LUT。
判定：消して建て直す

証拠：
- scanout は 3 つまで（fb.c:40）。present は、画面一枚分を CPU で写す。
- hold は、presents_drain が 100 µs ごとに眠って見に行く（208-212）。
- GOP の拍は EDID からの推測（fb.c:50-73）：
  - 同じ大きさの最初の detailed timing、無ければ 60 Hz に近いものを選ぶ。
  - GOP には vblank が無いので、GUI はこの推測の数字で、帰りを確かめないまま拍を刻む。
  - EDID とカーネルが拍を決めるのは、規則にもなっている（kernel.md:24）。
- VM の都合：fb.c:12、146（WHPX が SIMD の MMIO を真似ないので scalar）、391-393（WHPX の fbbench の測定で並列をやめた）、438、446（vm_display_pm）。
- 実機では、表示の眠りから戻る道が無い（435-448）。
- 実機の RTX 4070 の scanout は、アプリ（nvrm）から pciuser の道で入ってくる（2 の項）。

10 倍：画面は三つまで。複数の画面や複数の GPU の模型が無い。

Linux／NT より悪い軸：
- Linux の DRM/KMS：CRTC・plane・connector の模型と、装置の vblank の時刻つきの事象。
- NT の dxgkrnl：VidPN の模型と vsync の時刻。
- ここは一つの framebuffer と、推測の拍。

建て直すなら（設計の要件と継ぎ目）：
- 表示の部分系（画面、mode、plane、装置から来る vblank の時刻）を持ち主にする。GPU の driver は、核のものでもアプリのものでも、そこへ登録する。
- ファームウェアの framebuffer は、起動と panic の退避にだけ使う。
- 継ぎ目：GUI（wm）、アプリのドライバーの ABI、panic。

15. 文字の console（fbcon.c, fbcon.h）
責任：起動の文字の console と、panic の画面。
判定：消して建て直す

証拠：
- panic の画面を持っている（fbcon.c:271-370）。
- canvas は画面全体の連続の確保。
- 起動の旗に「a hobby OS dreamt up overnight」の文（50）。
- fbcon_write_kernel は fbcon_write をそのまま呼ぶだけの包み（252-254）。

建て直す物：
- GUI が先の OS なので、文字の console は要らない。
- 要るのは panic の画面だけ。確保も lock もせず、今出ている scanout（アプリの GPU の scanout も含む）へ描く、別の小さな物として作る。

Linux／NT との比べ：NT の bugcheck の画面は表示の driver に依らない最小の描き手で、Linux の DRM panic も同じ考え方。

16. gpu.h
責任：gpu_init・gpu_render などの口。
判定：消すだけ（要らない）
証拠：
- 実装は vm/virtio_gpu.c:576-731 だけ。一般の名前の口の後ろに、VM の装置が隠れている。
- 実機の GPU は nvrm（アプリ）が受け持つ。

17. PS/2（input.c の i8042 の部分：input.c:123-305）
責任：PS/2 のキーボードとマウス。
判定：消すだけ（要らない）

証拠：
- 実機のキーとマウスは USB（README_EN.md:263）。
- 8042 があるかを、port 0x64 が 0xFF を返すかで推測している（input.c:290）。
  - 有無を言うのは ACPI 6.5 の表 5.11、FADT の IAPC_BOOT_ARCH の bit 1（「8042」）だが、それを見ない。
- IRQ 1 と 12 は 8259 を通って CPU 0 へ（298-302）。
- 待ちは、1 回につき 100 ms までの pause の poll（178-186）。復帰では十数個の命令を、それぞれ最大 100 ms で回す（217-263, 265-285）。
- VM の判定（vm_ps2_flips_y、201-207）と、Hyper-V の判定（290）がある。

18. 入力の層（input.c の残り：input.c:11-121, 307-331、と input.h）
責任：全部の入力 driver のキーとポインターを、一つの受け口（wm）へ渡す。
判定：消して建て直す

証拠：
- 表現：
  - 内側の表現が PS/2 の scan code set 1（input.h:43-47）。
  - 英語配列の表で文字に変える（input.c:31-46）。0x80 以上の文字は KEY_UNKNOWN になる（318）。
- lock：
  - 一つの input_lock を割り込みを閉じて取り、受け口をその下で呼ぶ（input.h:38-40、input.c:11-17）。
  - 修飾キーと E0 の前置きは、全キーボードで一つ。input.h:44-45 は、二つの driver の前置きが混ざりうると自分で書いている。
- 事象に時刻の欄が無い（input.h:19-32）。ボタンは 3 つ（29）。

10 倍・1024 CPU で壊れるもの：
- 入力装置が増えても、lock は一つ、修飾の状態も一つ。
- 受け口は割り込みを閉じて走るので、wm の受け口を重くできない。

Linux／NT より悪い軸：
- Linux：input_event は装置ごとの状態と時刻を持ち、配列は利用者側（xkb）が受け持つ。
- NT：raw input に装置の handle と時刻があり、配列は win32k と利用者側が受け持つ。

建て直すなら（設計の要件と継ぎ目）：
- 状態は装置ごとに持つ。
- 事象は、装置に依らない符号（usage か同等の物）と、時刻（割り込みの時刻か装置の時刻）を持つ。
- 配列は GUI の側。lock は装置ごとにし、事象の列は lock の無い輪にする。
- 継ぎ目：usbhid と GUI。

19. 電源の段取り（pm.c, pm.h）
責任：電源の変化（切る、再起動、S3、休止）の順序と、装置の quiesce・resume の呼出し。
判定：消して建て直す

証拠：
- 段は種類で決めた三つ（IO、STORAGE、BUS：pm.h:41-46）で、装置木の親子の順ではない。
- 待ちの約束（pm.h:25-37）：
  - quiesce は割り込みを閉じて、poll しかできない。
  - resume は CPU 0 で、ほかの CPU を止めたまま、眠れない。
  - その結果、各 driver の待ちが全部 busy-wait になる（NVMe 10 s、AHCI 10 s、PCI 1 s／機能、PS/2 は 100 ms × 命令の数）。
- 電源のスレッドが GUI の仕事をしている：
  - 窓の数を数え、閉じるよう頼み、toast を出す（pm.c:79-108）。50 ms ごとの poll（99）。
  - アプリの停止も 20 ms ごとの poll（113）。
  - GRACE 5 s・STALL 15 s・FORCE 60 s・STOP 3 s（28-31）。
- 眠りと休止は、アプリの握りがあるだけで拒否される（251-254）。
- 復帰では、装置の resume を一つずつ順に呼ぶ（229-236）。
- vm_resume が共通の道にある（220）。

10 倍・1024 CPU で壊れるもの：
- 復帰には「装置の数 × それぞれの待ち」の和がかかり、それを CPU 0 一つで回す。ほかの 1023 の CPU は止まったまま。
- 窓が増えると、閉じたかの判定のために wm の数え上げを 50 ms ごとに繰り返す。

Linux／NT より悪い軸：
- Linux の dpm：装置の親子と device link の順に、非同期の suspend/resume を並列に、眠れる文脈で回す。
- NT：PnP の装置木と電源の IRP（子から親へ）、PoFx。
- アプリへの終了の知らせは、NT では利用者側（winlogon と csrss）、Linux でも systemd が行う。カーネルが窓を閉じて回ることはしない。

建て直すなら（設計の要件と継ぎ目）：
- 装置木の順に、部分木ごとに並列で、眠れる文脈で suspend/resume する。
- quiesce と resume には期限と結果を持たせ、待ちは割り込みで行う。
- アプリのドライバーへ電源の段階を知らせる（2 の項との継ぎ目）。
- 窓とアプリの終了は GUI の側の仕事にし、カーネルには「止まってよいか」を問うだけにする。

20. 電源を切る・再起動する（power.c）
責任：S5 と再起動の最後の一歩（と、acpi_init の入口）。
判定：消すだけ（要らない）

証拠：
- uACPI の acpi_sleep_enter(5) と acpi_reset が先にある（power.c:74, 96）。ほかは全部、その代わりの道：
  - DSDT をバイトで走査して _S5_ を探す（43-61）。
  - SMI_CMD で ACPI を有効にする（81-83）。
  - 8042 に 0xFE を書く（101-102）。FADT の 8042 の印は見ない。
  - IDT を空にして三重違反を起こす（104-109）。
  - vm_power_off（89）。
- 待ちは時間でなく、io_wait の回数（78, 83, 87, 96, 99, 101, 103）。

要る物の行き先：uACPI の二つの呼出しと acpi_init の入口は、ACPI の継ぎ目（4）と電源の段取り（19）の建て直しの中に入る。

21. 休止（hibernate.c, hibernate.h）
責任：RAM の像を圧縮して C: へ書き、次の起動で戻す。
判定：消して建て直す

証拠：
- 像は、割り込みを閉じてほかの CPU を止めた CPU 0 一つで取る。使っている全部の頁を圧縮する（hibernate.c:293-320、pm.c:186-187）。
- 圧縮した像を全部入れる空きの頁の池が要る（236-250）。像の前にメモリーを空ける段が無いので、RAM の半分より多く使っていると「not enough memory」で失敗する。
- 像は暗号化されない（hibernate.h:20-21）。検査に使う hash も暗号ではない（88-95）。
- 表示の mode が同じでないと戻せない（522-524）。
- VM の都合が共通の道にある（146, 637）。復帰の道で 8259 の 16 本をマスクする（630）。
- 実機では握りがあるので使えない（共通の E）。

10 倍・1024 CPU：RAM の大きな機械では、像を取る時間が RAM の量に比例する。それを CPU 一つで行い、その間ずっと割り込みは閉じたまま。

Linux／NT より悪い軸：
- Linux：像の前にメモリーを詰め（image_size）、圧縮と書込みを複数のスレッドで行う。
- NT：hiberfil も複数の CPU で圧縮し、戻すのは専用の起動の読み手。

建て直すなら（設計の要件と継ぎ目）：
- 像の前にメモリーを詰める。
- 圧縮は全 CPU で行う。
- 像を暗号化する。鍵の持ち主は未確認の範囲。
- 電源の段取り（19）の、装置木の順の上に載せる。

22. 乱数（entropy.c, entropy.h）
責任：カーネルの乱数（SYS_RANDOM と、TLS の鍵の元）。
判定：消して建て直す

証拠：
- 読み出しのたびに、一つの pool_lock を割り込みを閉じて取り、全 CPU の feed を 4 語ずつ畳み込む（entropy.c:80-97）。CPU 数に比例する仕事を、大域の lock の下でしている。
- 混ぜは掛け算と回転で、暗号の抽出ではない（28-38）。
- 「種が十分か」の状態が無い。起動で rdtsc を 64 回と空の輪を回して「ready」と書く（113-120）。空の輪は回数の待ち。
- RDRAND の失敗（いつも同じ値を返す）を確かめない（49-56）。
- 装置の割り込みは全部 CPU 0 なので、装置の揺らぎは CPU 0 の feed にしか入らない（arch/idt.c:337, 345 と共通の A）。

10 倍・1024 CPU：一回の読みが、大域の lock の下で 4096 回の混ぜになる。

Linux／NT より悪い軸：
- Linux：BLAKE2s で集め、CPU ごとに ChaCha の crng を持ち、読みの道に大域の lock が無い。
- NT も CPU ごとの生成器を持つ。

建て直すなら（設計の要件と継ぎ目）：
- 暗号の hash で蓄える。
- 生成器は CPU ごとに持ち、大域の鍵から周期的に種をもらう。
- 種が十分になるまでの状態を持つ。
- RDSEED を使い、失敗を確かめる。

23. 記録の port（serial.c, serial.h）
責任：COM1（16550、port 0x3F8、115200 baud）へ kprintf を流す。
判定：消すだけ（要らない）
証拠：
- port と速さは手で決めてあり（serial.c:16, 19, 31-47）、ACPI の SPCR/DBG2 で探さない。
- flush は、100000 回の poll という回数で打ち切る（111-112）。
- 実機の記録は写真だけ（.claude/rules/kernel.md:26）。この port は QEMU の serial.log のための道。
- 記録の部分系を建て直すのは、lib/printf の監査の範囲：CPU ごとの lock の無い輪と、実機で使える出口（画面、ディスクの記録、要れば DBG2）。

====================================================================
最後に
====================================================================

残せるもの
- ACPI の表の登録簿（acpi.c:1-154 と acpi.h の表の形）。
  - ACPI 6.5 第 5 章の通りの小さな仕事で、検査和と置き場所を確かめている。
  - 大域の lock も VM の分岐も無く、規模で壊れる所が無い。
  - 起動の約束（Limine）に依るのは三か所だけ（acpi.c:32, 50-59, 75-77）。
- dev/ のほかの物は、全部消す（建て直すか、消すだけ）。

7600X の PC に足りない driver（コードと文書で確かめた範囲）
- 音：HDA の制御器と codec（基板の音と、RTX 4070 の HDMI/DP の音）。音の driver は一つも無い（audio.c:769、README_EN.md:267）。codec の型は未確認。
- 使える NVMe：driver はあるが、深さ 1・写しつき・CPU 0（7 の項）。内蔵ディスクの型は未確認。
- USB：isochronous（音とカメラ、xhci.c:714）と UAS（usbmsc.c）。
- AMD IOMMU で核の装置の DMA を守ること（iommu.h:9-10）。
- ACPI：_OSC、EC、熱、起こし（_PRW）、名前空間からの平台の装置の結び付け（4 の項）。
- 表示：核の側の表示の部分系が無く、GOP の拍は推測（14 の項）。GPU は nvrm（アプリ）だけ。
- 眠りと休止：driver はあるが、実機では出ない（共通の E）。
- LAN：RTL8125B の driver は net/rtl8125.c にあるが、ベクター 0x4C の手決め（net/rtl8125.c:50）で、CPU 0 の約束に乗っている。中身は net の監査の範囲。

全体の印象（三行以内）
dev/ は「装置の割り込みは CPU 0」「キューは一つで一件ずつ、写しつき」「待ちは割り込みを閉じた poll」の三つの前提の上に積まれていて、どの driver もその前提ごと作り直しになる。
実機の構成（USB の C:、USB の入力、nvrm の GPU、音なし）で見ると、要の道ほど VM で試しやすい形に寄っていて、実機では一本の usb スレッドと推測の時刻に頼っている。
残せるのは ACPI の表の登録簿だけ。

### Critical Files for Implementation
- D:\Programes\llm vibe slop os\kernel\src\dev\pci.c
- D:\Programes\llm vibe slop os\kernel\src\dev\pciuser.c
- D:\Programes\llm vibe slop os\kernel\src\dev\pm.c
- D:\Programes\llm vibe slop os\kernel\src\dev\xhci.c
- D:\Programes\llm vibe slop os\kernel\src\dev\audio.c
