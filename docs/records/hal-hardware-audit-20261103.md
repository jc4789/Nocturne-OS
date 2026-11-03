# HAL の枝を実機の見地で読んだ監査（2026-11-03）

サブエージェント（Opus 5.5）の報告の全文。読むだけで、何も動かしていない。報告の主張は、後でコードと仕様書で照らす。

## 依頼文

Nocturne OS のリポジトリ（`D:\Programes\llm vibe slop os`、枝 `HAL-DEV`）の監査を頼みます。読むだけです。ファイルを変えず、コミットせず、ビルドも QEMU も動かさないでください。返事は全部日本語で書いてください。識別子とパスは元の表記のままです。

### 対象

`git diff main...HAL-DEV`（枝の全部のコミット）。主な範囲は `kernel/src`、`common`、`user`、`ports/nvidia` です。作ったものは次のとおりです。

- 装置の台帳（`kernel/src/dev/hal.c`、`hal.h`、`common/abi.h` の `struct n_hal_device`）
- バスごとの実装の表（`kernel/src/dev/pci_drivers.c`、`usb_drivers.c`、`vm_pci_drivers`）と、起動の順の変更（`kernel/src/init.c`）
- CPU の製造者ごとの分割（`kernel/src/arch/cpu.c`、`cpu_maker.h`、`cpu_amd.c`、`cpu_intel.c`、`mca.c`）
- 必須の機能の表の縮小と、任意にした機能（AVX、AVX-512、RDRAND など）。`arch/fpu.c` の成分の決め方。
- 設定の窓の「装置」の頁（`user/apps/settings.c`）

設計の文書は `docs/hal.md`、規則は `AGENTS.md`、`.claude/rules/architecture.md`、`code.md`、`kernel.md`、地図は `.claude/rules/map.md` です。

`docs/records/hal-diff-review-20261103.md` は読まないでください。別の読みが要るからです。

### 読み方

敵として読んでください。著者の説明とコメントを信じず、コードが誤っていると仮定して探します。文書とコードが違うなら、コードを正として扱います。

差分の行だけで止まらないでください。差分が変えた値、順序、前提に依存している差分の外のコードも読んでください。たとえば、起動の順が変わった部品、必須でなくなった機能を黙って使っているコード（カーネル、libc、ports、組みの旗、アセンブリ）、製造者の分岐が移った先、消えた分岐が持っていた役目です。

### 問い

#### 問い 1：実機で問題を起こすもの、起こしうるもの

実機で問題になるのに、VM では見えないか害が無いものを探します。VM で動いたことは証拠になりません。観点の例です。これに限りません。

- 起動の順、割り込み、タイミング、待ちの上限、装置の準備の遅さ
- MSR、CPUID、レジスターの書き方。製造者、family、世代による違い。hypervisor が隠すか飲み込むが、実物は #GP を出す、あるいは違う動きをするもの
- firmware が残した状態（橋のバス番号、BAR、割り込みの経路、未設定の装置、BIOS handoff）
- DMA、IOMMU、キャッシュ、PCI の階層が深い機械、同じ class の装置が複数ある機械
- 複数のコントローラー、複数の GPU、ホットプラグ、実物の USB 装置の振る舞い（複合装置、答えの遅い装置、規格に沿わない装置）
- 眠りと戻り（S3、休止）
- CPU が多い機械、NUMA、複数 LLC、2 ソケット
- プロセスのドライバー（`nvrm`、`nvk`）への影響：claim、台帳、ABI の世代、起こす条件

#### 問い 2：重要な欠け

この仕事が果たすと言っていることのうち、実は足りないもの、抜けているものを探します。無いと実機で困るものを優先します。

### 実機と VM の事実

- この枝は実機で一度も動いていません。ユーザーの実機は Ryzen 5 7600X、DDR5 32GB、RTX 4070、Resizable BAR 有効です。ただし特定の機械に合わせた設計はしません。目標は、Intel と AMD の新旧の CPU、上は 2 ソケットの EPYC（論理 CPU 1024、NUMA、CXL）までです。実機では画面と GPU を `nvrm`（NVIDIA の RM をプロセスで動かす移植）が持ちます。実機の NIC は `rtl8125` です。C: は NVMe、SATA、USB のどれにもありえます。
- VM で動かした環境：QEMU の WHPX（`-M q35`、`-cpu host` と、機能を隠した CPU）で、NVMe、AHCI、e1000、tulip、HD Audio、AC97、VGA、virtio-vga、USB のキーボード、タブレット、ディスク、網、hub。休止と戻りも動かしました。S3、Hyper-V、`rtl8125`、`nvrm` は動かしていません。

### 使える資料

- 仕様書：`D:\Programes\OS DEV INFO`（PCIe、NVMe、AHCI、xHCI、USB、IOMMU、BKDG、RFC など）。記憶でなくここで照らしてください。AMD と Intel の手引きが要るなら、作った者の公式の場所から取ってかまいません。
- 参照：`D:\Programes\cloned repos\freebsd-src`（答えが出たら読むのを止めてください）。
- 上流のソース：`D:\Programes\cloned repos`（`open-gpu-kernel-modules` など）。
- `codegraph_explore` と `rg`。番号で引く呼出しと表からの呼出しは索引に出ません。
- 読まないもの：Linux のソース、GPL のファイル。

### 報告の形

結論から書いてください。褒めないでください。実況を書かないでください。一文に一つのことを書いてください。

欠陥ごとに次を書いてください。

- 場所：`ファイル:行`
- 順序：何がどの順に起きるか
- 実機で何が起きるか。重さ（起動が死ぬ、データが壊れる、機能が止まる、表示だけが誤る）
- VM で見えない理由
- 確かさ：コードで確かめた、仕様の節で確かめた（文書と節を書く）、見込み、のどれか。見込みは見込みと書いてください。
- `main` にも同じ欠陥があるなら、そう書いてください（この枝が作ったものか、前からのものかを分ける）。

重いものから順に並べてください。その後に次の節を置いてください。

- 重要な欠け（問い 2）
- 好み（欠陥ではないもの）。欠陥と混ぜないでください。
- 読んだ範囲と読まなかった範囲

見つからない観点は「無し」と書き、何を読んでそう言えるかを書いてください。件数を埋めるための所見は要りません。報告は要約せず、全部を最後の返事に書いてください。

## 報告

### 結論

- この枝が新しく作った欠陥で、実機の起動を殺すものは、読んだ範囲で見つからなかった。何も動かしていないので、これは読みの結果である。
- 実機での driver の結びの順（`rtl8125`、`xhci`、`hda`、`ahci`、`nvme`）は `main` と同じである。
- 確かな欠陥は一つある。ABI の世代を上げたのに、Go と LLVM が世代 1 のままである。今の `build/c.list` の 4 本が起動を断られる。
- 実機だけで効く、枝が作った欠陥は二つある。USB の製品名の読出しと、台帳の誤った表示である。
- 目標に対する一番大きな欠けは、CPU の下限である。必須の表はまだ古い CPU を起動時に止める。
- AVX などの任意になった機能を黙って使うコードは、カーネル、libc、ports、組みの旗、アセンブリのどれにも見つからなかった。

### 欠陥（重い順）

#### 1. 世代を 2 に上げたが、Go と LLVM は世代 1 のまま（機能が止まる）

- **場所**
  - `common/abi.h:248`（`N_ABI_GENERATION 2u`）
  - `ports/go/nocturne.patch:102`（`ELF_NOTE_NOCTURNE_GENERATION = 1`）
  - `tools/go/src/cmd/link/internal/ld/elf.go:765`（同じく 1）
  - `ports/llvm/llvm.mk:29`（`$(PROGRAMS)/llvm` は `tools/llvm/nocturne` からの写しだけ）
  - `kernel/src/sys/proc.c:183`（世代が違えば起こさない）
- **順序**
  1. この枝が世代を 2 に上げた。
  2. Go のリンカーは刻印に 1 を書き続ける。
  3. `llvm` は手で組み直さない限り古い像のまま C: に入る。
  4. カーネルが `N_ERR_WRONG_GENERATION` で起動を断る。
- **今の出力での事実**：`build/c.list` の全部の ELF の PT_NOTE を読んだ。世代 2 が 100 本、世代 1 が次の 4 本である。
  - `C:\Programs\Calculator\calc`
  - `C:\Nocturne\System\gotest`
  - `C:\Nocturne\System\llvm`
  - `C:\Nocturne\System\tsc`
- **実機で何が起きるか**
  - 電卓、`gotest`、`tsc` が起動しない。
  - `clang`、`clang++`、`ld.lld` などは `llvm` を起こす道具なので（`ports/llvm/tool.c:1-6`）、ゲストの clang 一式も動かない。
  - Go の patch の定数が 1 なので、組み直しても Go のプログラムは世代 1 になる。
  - `ports/go/README.md:34` は定数を合わせて組み直せと書いている。これが行われていない。
- **VM で見えない理由**：VM でも同じに起きる。動かした一覧にこれらの起動が無いので、見えなかった。
- **確かさ**：コードと今の出力で確かめた。起動の失敗そのものは動かしていない。
- **`main`**：`main` は世代 1 同士で合っている。この枝が作った。
- **根**：C: へ入れる ELF の世代を確かめる検査が無い。`scripts/layout.py` にも `tests/check_*.py` にも見つからなかった。

#### 2. 全部の USB 装置の製品名を、結んだ後に読む（機能が止まる。USB の C: を失いうる）

- **場所**
  - `kernel/src/dev/xhci.c:1400`（`usb_product`）
  - `kernel/src/dev/xhci.c:1756`（`if (!d->hub) usb_product(d);`）
  - `kernel/src/dev/xhci.c:125`（`CONTROL_NS` は 5 秒）
  - `kernel/src/dev/xhci.c:880-889`（`control_recover`、`ctl_broken`）
  - `kernel/src/dev/usbmsc.c:70`（`reset_recovery`）
- **順序**
  1. worker が `usbmsc_attach` を呼ぶ。
  2. `probe_lun` が `blk_register` する。
  3. 別のスレッドの `device_find`（`fs/store.c`）が 100 ms ごとにその disk の GPT を bulk で読み始める。
  4. worker は `usb_product` へ進み、string 0 を 4 バイト、製品名を 255 バイト求める。
- **実機で何が起きるか**
  - 答えない装置なら、worker は最大 10 秒止まる（5 秒の要求が二つ）。
  - その間、同じ controller の他のポートとキーボードの列挙が待つ。
  - `control_recover` が失敗すると `ctl_broken` が立ち、以後その装置の制御の要求は全部失敗する。
  - USB の C: では、後の bulk の失敗で `reset_recovery` の Bulk-Only Reset が届かなくなる。
  - 長さの大きい string の要求で固まる装置が世にあるなら、C: そのものを失う。
  - この読出しの用途は、製品名を台帳へ渡して窓に出すことである（HID は入力の名にも使う）。
- **VM で見えない理由**：QEMU の USB 装置は string に必ず答える。
- **確かさ**：順序、時間、`ctl_broken` の効果はコードで確かめた。実物の装置の振る舞いは見込みである。`xhci.c:1754` のコメント自身が、壊れる装置があることを前提にしている。
- **`main`**：`main` で string を読むのは HID（`string_read`）と `usbnet` の MAC だけである。USB ディスクへの読出しはこの枝が作った。

#### 3. Intel で、休止の戻りの後に CMCI の報告が止まる（機械の検査の報告が止まる）

- **場所**：`kernel/src/arch/cpu_intel.c:86`、`:91`、`:93`、`kernel/src/arch/smp.c:393`、`kernel/src/boot.c:200`
- **順序**
  1. 休止の戻りの起動で、一時のカーネルが CPU 0 の `cmci_on` を走らせ、bank の `CMCI_EN` を立てる。
  2. 像を戻す。CPU 0 に reset は入らない。
  3. 戻ったカーネルの `smp_resume` が `mca_interrupt_on` を呼ぶ。
  4. `cmci_on` は `CMCI_EN` が立った bank を「兄弟の物」と読んで飛ばす。
  5. `mine` が 0 になり、`cmci_banks` も 0 になる。
- **実機で何が起きるか**
  - CPU 0 は自分の bank を見なくなる。
  - 訂正された誤りが報告されない。
  - `cmci_told` は `cmci_banks` の bank しか見ない。
  - 眠りを途中で取りやめた道でも、`smp_resume` を通るので同じになる。
- **VM で見えない理由**：QEMU の環境で CMCI の割り込みと bank の状態は試していない。休止と戻りは動かしたが、CMCI の報告は見ていない。
- **確かさ**：CPU 0 についてはコードで確かめた。SDM の 18.3.2.5 は「processor reset の後に CTL2 が 0 になる」と書く。戻りの道の CPU 0 に reset は無い。AP は INIT で CTL2 が残るかを確かめていないので、見込みである。
- **`main`**：同じ欠陥が `main` の `arch/cpu.c` にある。この枝は移しただけである。

#### 4. 台帳が実機で誤ったことを言う（表示だけが誤る）

- **4a：AMD の IOMMU の機能**
  - **場所**：`kernel/src/dev/iommu_amd.c:897`、`user/apps/settings.c:355-362`
  - **順序**：`iommu_init` が `pci_enable_busmaster(fn)` と割り込みでその機能を動かす。台帳の記録は `N_HAL_UNBOUND` のままである。
  - **実機で何が起きるか**：設定の窓に「動かすドライバーがありません」と出る。`kernel_owned` は true なので、サービスの宣言が合っても claim は `-N_ERR_BUSY` になる。今は合う宣言が無い。
  - **VM で見えない理由**：動かした q35 の構成に AMD の IOMMU の記述が無い。
  - **確かさ**：コードで確かめた。
- **4b：動いた後の失敗を書かない実装**
  - **場所**：`kernel/src/dev/hda.c:817`、`kernel/src/net/rtl8125.c:407`、`docs/hal.md:104`
  - **順序**：`hda_resume` か `rtl_resume` が false を返す。`pm` は「did not come back」と書く。台帳は `N_HAL_RUNNING` のままである。
  - **実機で何が起きるか**：`hal_failed` の呼出しが `hda.c` と `rtl8125.c` の attach の後に無いので、壊れた装置が「動かしています」と出る。文書の決まりと違う。
  - **VM で見えない理由**：VM では HDA が戻りに失敗しない。`rtl8125` は動かしていない。
  - **確かさ**：コードで確かめた。
- **4c：バスから消えた xHCI**
  - **場所**：`kernel/src/dev/xhci.c:2338`
  - **順序**：`USBSTS` が全部 1 のとき、worker は何もしない。
  - **実機で何が起きるか**：台帳は `N_HAL_RUNNING` のままである。
  - **VM で見えない理由**：動かした一覧に、xHCI の機能を PCI から消す試しが無い。
  - **確かさ**：コードで確かめた。
- **`main`**：台帳はこの枝の物なので、三つともこの枝が作った。

### 重要な欠け（問い 2）

#### 1. 新旧の CPU の下限に届いていない（その機械で起動が死ぬ）

- **場所**
  - `kernel/src/arch/cpu.c:48`（INVPCID）
  - `kernel/src/arch/cpu.c:51`（RDPID）
  - `kernel/src/arch/isr.asm:213`
  - `user/libc/sys.c:210`
  - `kernel/src/mm/vmm.c:135`、`:147`
- **順序**：`cpu_features_init` が必須の行の欠けを見つけ、`panic` する。
- **内容**
  - 表の行には本当にコードがあるので、表としては規則どおりである。
  - 欠けているのは、無い CPU のための道である。
  - PCID は任意なのに、INVPCID は必須である。
  - 手元の AMD の PPR は Family 19h だけで、Family 17h は読んでいない。
- **確かさ**：行とコードは確かめた。次の製品の話は私の知識で、見込みである。
  - Intel の Skylake から Comet Lake には RDPID が無い。
  - AMD の Zen 2 以前には INVPCID が無い。
- **`main`**：`main` の下限は Zen 4 である。この枝は下げたが、目標の「新旧」には届いていない。

#### 2. 下限で止まる機械は画面に何も出ない（起動が死に、理由が見えない）

- **場所**：`kernel/src/boot.c:151`、`:174`、`kernel/src/dev/fbcon.c:369`
- **順序**：`cpu_features_init` の `panic` は `fb_init` より先に起きる。`fbcon_panic` は `!ready || !fb.addr` で戻る。
- **実機で何が起きるか**：シリアルの無い機械では、利用者に何も伝わらない。
- **確かさ**：コードで確かめた。
- **`main`**：同じである。

#### 3. PCI の segment が台帳にも ABI にも無い（その装置が無いことになる）

- **場所**
  - `kernel/src/dev/pci.c:18`、`:32`、`:213`
  - `common/abi.h:542`（`N_PCI_TARGET`）
  - `struct n_pciinfo` を埋めた `struct n_hal_device`
  - `kernel/src/dev/iommu_amd.c:324`
  - `kernel/src/dev/iommu_intel.c:323-326`
- **順序**：`pci_init` は segment 0 だけを列挙する。ほかの segment の機能は記録が作られない。
- **実機で何が起きるか**：そこの NVMe に C: があれば、見つからない。窓にも「見つからなかった」と出ない。
- **確かさ**：コードの事実は確かめた。上の目標の機械（2 ソケットの EPYC、CXL）が複数の segment を持つかは見込みである。
- **`main`**：`main` も segment 0 だけである。新しい ABI が同じ形を写した。世代を上げた今回が直す機会だった。

#### 4. 結びつけが同期で、全部の待ちが足し算になる（起動が遅れる）

- **場所**
  - `kernel/src/dev/pci_drivers.c:8-10`、`:27-32`
  - `docs/hal.md:153-154`
  - `kernel/src/dev/ahci.c:45`（`READY_NS` は 10 秒）
  - `kernel/src/dev/nvme.c:947-952`（`CAP.TO`、500 ms 単位）
- **順序**：AHCI の全部のポートを調べた後に NVMe を一台ずつ起こす。`store_init` はその後である。
- **実機で何が起きるか**
  - NVMe の C: の機械が、SATA のディスクの待ちを全部待つ。
  - NVMe が多い機械は台数に比例して遅くなる。
  - `CAP.TO` は 8 bit なので、1 回の待ちの上限は 127.5 秒である。
- **確かさ**：順序はコードで確かめた。実物の時間は見込みである。
- **`main`**：同じ順である。`docs/hal.md` はこの形を決まりとして書いた。

#### 5. 実機で眠りの道が開かない（S3 が使えない）

- **場所**：`kernel/src/dev/fb.c:635-643`、`kernel/src/dev/pm.c` の `sleep_blocker` と `sleep_now`
- **順序と内容**
  - firmware の面がある実機では `display_pm` に `resume` が無いので、S3 は断られる。
  - `nvrm` が claim している機械では、休止も断られる。ユーザーの機械がこれに当たる。
  - だから枝の `hda_resume`、`rtl_resume`、`enable_recorded` の S3 の道は、実機で今は走らない。
- **確かさ**：コードで確かめた。
- **`main`**：同じである。`docs/hal.md:204-210` は未着手と書いている。

#### 6. 既知の欠け（`docs/open.md` にあるもの）

- PCI の抜き差しと AER が無い。USB4 のドックと活線の NVMe は台帳に増えない。
- ノート PC の内蔵キーボードの実装が無い。
- 休止の後に xHCI が poll へ落ちる項目は、順序だけ確かめた。`pm.c` の resume の輪は `smp_resume` より先である。原因そのものは確かめていない。

### 観点ごとの「無し」と、読んだもの

- **起動の順、割り込み**：実機で変わった欠陥は無し。
  - `kernel/src/init.c` の差分を読んだ。
  - `iommu_init` は `main` と同じく全部の実装より先である。
  - `fb_pm_init` が後ろへ動いたが、実機の `display_pm` は `resume` も `quiesce` も持たず、登録だけである。
  - `net_thread` が NIC より先に起きるのは、`net_register` と `rtl_attach` を読み、害が無いと判断した。
- **MSR、CPUID、製造者の分岐**：移した先の誤りは無し（上の欠陥 3 は前からの物）。
  - `cpu_amd.c`、`cpu_intel.c`、`mca.c`、`idle.c`、`topology.c` を `main` の本体と行ごとに比べた。
  - レジスターの番号、順序、`wrmsr_checked` の使い分けは同じである。
  - 消えた `cpu_features.hypervisor`、`vendor`、`pci_find` の読み手は 0 件である。
- **必須でなくなった機能**：黙って使うコードは無し。
  - `Makefile` と `ports/*` の旗は `-msse2` か無指定である。
  - `ports/rust/x86_64-unknown-nocturne.json` は `"cpu": "x86-64"` である。
  - `GOAMD64` の指定は無い。
  - `kernel`、`user`、`ports` の手書きの命令を `rg` した。`rdrand` は `cpu_features.rdrand` で確かめる。`ymm` 系の手書きは `tests` だけである。
  - `third_party` の中までは調べていない。FFmpeg の実行時の選択は `ports/ffmpeg/prepare.py` の記述で知ったが、本体は読んでいない。
- **firmware が残した状態**：新しい欠陥は無し。`pci.c` の `secondary > bus` は未設定の橋の問題を直している。BAR の扱いは `main` と同じである。
- **DMA、IOMMU、深い階層**：新しい欠陥は無し。`kernel_owned` の設定と `group_check`、`hda` の guest の道は変わっていない。
- **複数の controller**：実害のある欠陥は無し。`controllers[]` の読み手は `xhci_quiesce` と `xhci_resume` だけである。
- **CPU が多い機械、NUMA**：新しい欠陥は無し。`hal_lock` は速い道に入らない。`cpu_features_same` の比べ方は AVX と RDRAND の行が増えただけである。
- **`nvrm`、`nvk`**：読んだ範囲で欠陥は無し。動かしていない。
  - `n_pci_matches` の結果は旧 `nvrm_pci_is_gpu` と同じである。
  - GPU の機能に合う kernel の行は無いので、`N_HAL_UNBOUND` で起きる。
  - `hal_bound` と `hal_unbound` は `claim_mutex` の下にある。
  - 今の `build/programs/nvrm` と `build/drivers/nvk` の刻印は世代 2 である。
  - `libnvrm` の `pcilist` の使い方は新しい意味でも合う。

### 好み（欠陥ではないもの）

- `N_CPU_AVX`（`smp.c:168`）は `cpu_features.avx` を見ており、`fpu.c:50` の実際の成分を見ない。ずれるのは hypervisor だけで、実機では起きない。
- `SYS_HAL_LIST` は公開の呼出しなので、どのアプリも USB と NVMe の製品名を読める。`nvk` の像のために公開が要るが、PCI だけの口に絞る案もある。
- `TOKEN_HARDWARE` は頁が違っても `redraw` する（`settings.c:1679-1683`）。
- `usb_drivers` の外に `"usbhub"` の文字がある（`xhci.c:1740`）。
- `nvrm_gpu_functions` が見出しの `static` の配列で、TU ごとに写しができる。
- `mca_msr` が MSR ごとに表を引く。
- AMD の PCI の機能は台帳に全部出るので、窓に PSP などの「そのほかの装置」が並ぶ。人の用事から作る規則 15 に照らして問える。

### 読んだ範囲と読まなかった範囲

- **読んだもの**
  - `git diff main...HAL-DEV` の `kernel/src`、`common`、`user`、`ports/nvidia`、`ports/ffmpeg`、`ports/mesa/README.md`、`docs/open.md`、`docs/nocturne-native-abi.md`、`.claude/rules/map.md`
  - `docs/hal.md`（作業木の版。1 行の未コミットの変更がある）
  - 差分の外の `pm.c`、`fb.c` の眠りの節、`store.c` の `device_find`、`net.c` の `net_register` と `net_thread`、`smp.c`、`wake.c`、`watch.h`、`spin.h`、`pciuser.c` と `iommu.c` の claim の判断、`xhci.c` の制御転送と device の解放、`usbmsc.c`、`libnvrm.c` の `connect_any`
  - SDM の 18.3.2.5 と 18.5.2.1、AMD の PPR Family 19h Model 61h の CPUID の節
- **読まなかったもの**
  - `docs/records/hal-diff-review-20261103.md`（指示のとおり）
  - Linux のソース、GPL のファイル
  - FreeBSD（答えに要らなかった）
  - `usbhub.c` の本体、`hda_codec`、`iommu*.c` の大半、`ports/nvidia` の RM の本体、`third_party`
  - USB の仕様の原文（欠陥 2 は仕様でなくコードと見込みで書いた）
  - `make check` の host の試験が新しい `nvrm.h` で通るか
- **動かしていないもの**：ビルド、QEMU、実機。作業木は変えていない。作業場に `gen.py`（刻印の読み手）と `sdm.txt` を作った。
