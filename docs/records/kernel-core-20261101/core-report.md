# カーネルの中核：報告

## 依頼文（全文）

````text
あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機）のカーネルの中核を書く書き手。この作業木（main、未コミットの変更なし）で直接書く。ほかの書き手はいない（読むだけの監査が二人、同時に動く）。

# 状況
コミット 63b181ff でカーネルの中核が消え、カーネルが組めない。消えたもの：`kernel/src/arch/percpu.h`・`preempt.h`・`cpuset.h`、`kernel/src/mm/`（bootmem・pmm・vmm・vmalloc・heap）、`kernel/src/sys/` の `thread`・`process`・`sched`・`wait`・`address_wait`・`epoch`。残りのカーネル（約 4 万行：arch・dev・fs・gui・net・sys・vm）は、これらを呼んだままになっている。`python -X utf8 scripts/build.py -j 8 kernel` は `arch/cpu.h` の `#include "arch/percpu.h"` で止まる。

消えた版は `git show 63b181ff^:<path>` で読める。持ち主はそれを理由を記録せずに消した（何が悪かったかは分からない）。丸ごとの `git restore`・`git checkout` は禁止。材料として読んでよいが、各部品を下の要件で自分で判断し、要件を満たさない所は作り直すこと。残すか作り直すかの判断と理由を報告に書くこと。

# 要件（持ち主の言葉から）
- GUI・GPU・合成（compositor）が第一級。カーネルが原因のコマ落ち・かくつきがゼロであること。規模の例：600 Hz の画面、3 枚の画面（それぞれ違う周波数、VRR もある）、ゲームと YouTube と Discord を同時に開いても、カーネルのせいで落ちるコマが無い。この規模に耐えない選択は採らない。
- Linux・UNIX・POSIX を標準とも、良い作りの手本とも見なさない。形は目的から決める。逆に、似ているというだけで良い作りを捨てない。物差しは「最良か」「GUI が先か」「ただ動くか」。
- 対象は今の PC（持ち主の機械は Ryzen 5 7600X・DDR5 32GB・RTX 4070）。その機械だけに合わせない。下限は Zen 4（x86-64-v4、AVX-512、FSGSBASE・INVPCID・SMEP・SMAP・x2APIC・不変の TSC）。Zen 4 に PCID は無い（`arch/cpu.c` が任意の機能として持つ）。上は 2 ソケットの EPYC（論理 CPU 1024、NUMA）。CPU の数・メモリーの量を小さな固定の上限で縛らない。
- shim（何もしない lock、偽の関数、代役）・stub・決め打ちの値・一つの呼ぶ側のための包み・特例の分岐は禁止。きれいで保てるコード。要る所には抽象化の層を置く。
- ネイティブ ABI（`common/abi.h`、仕様 `docs/nocturne-native-abi.md`）を迂回しない。要るなら ABI を Nocturne の形で直し、仕様書も合わせる。
- VM だけの処理は `kernel/src/vm/vm.h` の後ろ。共通コードに hypervisor の判定を書かない。
- コメントは今の形だけを書く（経緯は書かない）。周りのコードの書きぶりと密度に合わせる。

# やること
1. 残りのカーネルが中核に求める口（関数・型・構造体の欄・PERCPU の変数・isr.asm と smp.asm と wake.asm が使う記号・リンカースクリプトの節）を洗い出す。codegraph（MCP の `codegraph_explore`）と rg を使う。`.claude/rules/map.md` が地図、`.claude/rules/kernel.md` があればカーネルの事実。
2. 中核を設計し、`docs/records/kernel-core-20261101/core-design.md` に日本語で書く（部品ごとの責務・持ち主・錠の順・規模が 10 倍になったときにどこが詰まるか）。特に：
   - sched：合成・音・入力の時間の道。画面ごとの周波数（600 Hz なら 1.67 ms）で回る仕事が、ほかの負荷（ゲーム・動画・多くの thread）で締切を落とさないこと。周期の違う画面が何枚あっても成り立つ級（たとえば締切で並べ、予算で守る級）を考え、固定の周期・予算の決め打ちを置かない。起こしてから走るまでの遅れ（idle の CPU を起こす道、MWAIT と IPI）。NUMA と LLC を見た置き場所。tickless（`sys/alarm.c` の CPU ごとの期限）。
   - mm：NUMA を見たページの割り当てと CPU ごとの cache、大きなページ（direct map、framebuffer・GPU の BAR の大きな対応、PAT の WC は `arch/pat.c`）、TLB の shootdown は使っている CPU だけへまとめて送る、カーネルの heap。GPU・合成のための共有の buffer（呼ぶ側が使う形を見て）。
   - 窓の系（`kernel/src/gui/wm.c`）・GPU（`dev/pciuser.c`、`dev/gpu.h`、`vm/virtio_gpu.c`）・音（`dev/audio.c`）・入力（`dev/input.c`、`dev/usbhid.c`）がどの thread でどの級で走るかを読み、中核の口がそれに合うようにする。呼ぶ側を新しい口に合わせて直してよい（互換は持たない）。
3. 書く。`python -X utf8 scripts/build.py -j 8 kernel` を警告なしで通す（目標はいつも書く。目標の無い build.py はフックが止める）。次に `python -X utf8 scripts/build.py -j 8 qemu-image`。
4. 窓付きの QEMU で起こす。手順は `.claude/skills/nocturne-project/SKILL.md` と `references/qemu-visual.md`（q35・UEFI・NVMe・`-cpu host`・`-accel whpx`・USB の tablet と kbd、`-display none`・`-nographic` は禁止、QEMU は同時に二台まで、動いている数を先に見る）。serial.log とスクリーンショットは `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\` に置く。デスクトップが出て、Terminal が開き、`ps`・`free` が動き、ブラウザーで実サイト（たとえば Wikipedia）が開くまで。panic が出たら直す。撮った画像は自分で開いて見る。終わったら自分の QEMU を HMP の `quit` で止める。
5. コミットはしない（主担当が差分を読んでからする）。`git stash`・`git revert`・`git restore`・`git checkout -- <path>` を使わない。

# 道具の注意
- この Windows の Python はテキストモードで LF を CRLF にする。Python でファイルを書くなら `newline='\n'`。
- 上流のソースは `D:\Programes\cloned repos`、仕様書は `D:\Programes\OS DEV INFO`（AMD APM・PPR、Intel SDM、ACPI、PCIe など）。ハードウェアの事実はそこの公式の文書で照らす。どの OS のソースコードも読まない、参照しない。GPLv2 だけのコードを足さない。
- サブエージェントをさらに出さない。

# 報告
最後に `docs/records/kernel-core-20261101/core-report.md` へ日本語で全文を書く（この依頼文の全文を先頭に、続けて：作った部品の一覧、消えた版から残した所と作り直した所とその理由、設計の要点、組みの結果、QEMU で見たもの（画像の絶対 path と serial.log の要る行）、残った欠陥と未確認）。返事にも同じ要点を日本語で返す。
````

## 作った部品

| 部品 | 扱い |
|---|---|
| `kernel/src/arch/percpu.h`・`preempt.h`・`cpuset.h` | 消えた版と同じ中身で置いた |
| `kernel/src/mm/bootmem.{c,h}`・`pmm.{c,h}`・`vmalloc.{c,h}`・`heap.{c,h}`・`vmm.h` | 消えた版と同じ中身で置いた |
| `kernel/src/mm/vmm.c` | direct map を 1 GiB の頁で写すように直した |
| `kernel/src/sys/thread.{c,h}`・`process.{c,h}`・`address_wait.{c,h}`・`epoch.{c,h}` | 消えた版と同じ中身で置いた |
| `kernel/src/sys/sched.{c,h}` | 作り直した（締切の級、EEVDF の公平の級） |
| `kernel/src/sys/wait.{c,h}` | 優先度の継承を締切の貸し借りに換えた |
| `docs/records/kernel-core-20261101/core-design.md` | 設計書 |

呼ぶ側の直し：

| ファイル | 直し |
|---|---|
| `kernel/src/boot.c` | `kmain` に `address_wait_init()` と `epoch_init()` を戻した。23f80428 が部品と一緒に呼出しも消していた。戻す前の版は `epoch_quiet` が NULL を読んで panic した（「QEMU で見たもの」の一回目） |
| `kernel/src/gui/wm.c` | 合成の thread は輪ごとに `frame_period()` を見る。周期が変われば `sched_deadline(周期)` を言い直す |
| `kernel/src/dev/xhci.c` | 開いた割り込みの端点の service interval（`125000 << interval` ns、xHCI 6.2.3.6）の一番短いものを記録する。`usb` の thread はそれを周期にして締切の級で走る |
| `kernel/src/arch/irq.c` | irq の thread は公平の級で、要求は最短 |
| `kernel/src/vm/rdp.c` | `rdpsnd` は音の block（`SND_BLOCK_NS`）、`rdp` は画面を見る間隔（`LOOK_BUSY_NS`）を周期にして締切の級で走る |
| `kernel/src/sys/prof.c` | 標本の行に `throttles` と `late jobs` を足した |

中核の外の直し（`qemu-image` を組むため）：

| ファイル | 直し |
|---|---|
| `third_party/mesa/src/util/os_misc.c`・`.h`、`ports/mesa/README.md` | `DETECT_OS_NOCTURNE` では `os_set_option`・`os_unset_option` を宣言も定義もしない。アプリの設定は起動した側が渡し、アプリの中では読むだけである。mesa の中に呼ぶ側は無い |
| `user/apps/js.c` | `setting_put` の `setenv`・`unsetenv` の二行を消した。js.c は自分の設定の表を持ち、`setting_get` と `run()` はその表を先に読む。二行は同じ値を libc にも写すだけだった |

どちらも、23f80428 が libc から `setenv`・`unsetenv` を消したため組めなくなっていた所である。libc に `setenv` は戻していない。

## 残した所と作り直した所

残した所と理由：

- percpu・preempt・cpuset：CPU の数に上限が無い（`.percpu` 節を CPU ごとに写し、GS 相対で読む）。横取りの数は CPU ごとに持つ。要件に合う。
- bootmem・pmm：buddy、番地の帯、CPU ごとの溜め、node ごとの記録を持つ。記録は 1 GiB の section ごとなので、穴の多い番地でも記録の量は使える記憶に比例する。
- vmm（direct map を除く）：撃ち落としは受け箱と LLC ごとの答えの数で行う。CPU の数が増えても、送り手の待ちは LLC の数に比例する。PCID は任意である。lazy TLB と集め（gather）を持つ。
- vmalloc は purge でまとめて撃つ。heap は magazine の slab と node の depot で動く。
- thread・process・address_wait・epoch：錠の順、kill の意味、epoch の LLC の group が要件に合う。

作り直した所と理由：

- sched：前の版は、固定の優先度で並べる realtime の級（`SCHED_PRIORITY_*`）だった。周期の違う仕事（600 Hz の合成、周波数の違う画面 3 枚、VRR、USB の 125 µs の拍）は、固定の優先度では並べられない。締切の級（EDF）と予算（hard CBS）に換えた。
- wait：前の版の優先度の継承は rank を比べていた。締切の級に合わせ、継承を締切の貸し借りにした（`pi_lend`、`sched_sooner`）。
- vmm.c：前の版は direct map を 2 MiB の頁で写していた。今は 1 GiB の頁を先に試し、次に 2 MiB、最後に 4 KiB で写す。
- mutex の再入（depth）は残した。`wm_mutex` は wm.c の入れ子の道（2017 → 1030 → 2810）で取り直される。C: の呼出しも入れ子で取る。呼ぶ側を直すと窓の管理の構造が変わる。それは合成の持ち主の仕事である。

## 設計の要点

全文は `docs/records/kernel-core-20261101/core-design.md` にある。

- 級は二つ：締切（`SCHED_DEADLINE`）と公平（`SCHED_FAIR`）。締切の thread は周期だけを言う（`sched_deadline(period_ns)`、125 µs〜1 s）。
- 予約は scheduler が測る。仕事ごとに使った時間の頂点を取り、その二倍を [P/64, P−P/16] に収める。頂点は仕事ごとに 1/32 ずつ下がる。呼ぶ側は予算の数を書かない。
- 予約を使い切った締切の thread は、締切まで公平の級へ下がる（throttle）。締切の thread が暴れても、公平の thread は飢えない。
- 起こされた締切の thread は、残りの予約が残りの時間に収まらなければ新しい周期を始める（CBS の規則）。
- 公平の級は EEVDF で、min_deadline を持つ AVL で並べる。
- 置き場所：締切の thread は、締切の thread を持たない CPU（`HINT_FREE`）へ先に置く。探す順は前の CPU、同じ LLC、node とその先である。
- 優先度の継承：mutex を待つ締切の thread は、持ち主へ自分の締切を貸す。連なりの先へも貸す。
- tickless：周期の拍は無い。締切と予約の alarm だけを CPU ごとの heap に立てる。
- 合成：`wm.c` の合成の thread は、表示中の mode の周期を締切にする。画面ごとに合成の thread を分ければ、それぞれの周期で並ぶ。合成の構造（画面の数、flip、面の共有）は合成の持ち主が直す。
- ABI は変わらない。アプリの thread が締切の級を求める口は無い。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：警告 0、誤り 0。`build/kernel.elf` ができた（`kmain` の直しの後に組み直した）。
- `python -X utf8 scripts/build.py -j 8 qemu-image`：`build/programs/rusttest` の link で止まる。`tools/rust/sysroot` の組み済みの libstd が、libc の `setenv`・`unsetenv` を引く（`std::sys::env::nocturne::setenv`・`unsetenv`、`set_current_dir` の中の `setenv("FOLDER")`）。23f80428 がその二つを libc から消した。
- 確かめの媒体は `python -X utf8 scripts/build.py -j 8 RUST_SYSROOT=none qemu-image` で組んだ。入らないのは Rust の app だけである。出力は `IMG build/nocturne.img (1216 MiB)`、`C: 197 folders, 2757 documents`。

## QEMU で見たもの

起動の形：q35、UEFI（edk2）、NVMe、`-cpu host -accel whpx -smp 4 -m 4096M`、`-device VGA,xres=2560,yres=1440`、xHCI の usb-kbd と usb-tablet、e1000。窓付きで起こした。終わりに HMP の `quit` で止めた。

一回目（`kmain` の直しの前）は panic した。

- serial.log：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\serial-panic1.log`
- 報告の行：`*** KERNEL PANIC: Page fault on the app's memory outside user_copy (SMAP) (error 0) at ffffffff8007e80b`、`on CPU 0, thread 1 "idle0"`、`CR2 0000000000000000`
- 番地を記号に引いた呼出しの鎖：`epoch_quiet`（sys/epoch.c:129）← `switch_once`（sys/sched.c:819）← `reschedule` ← `sleep_in` ← `sleep_until` ← `start_cpus`（arch/smp.c）← `smp_init` ← `kmain_scheduled`。
- `epoch.group` が NULL だった。原因は `epoch_init()` の呼出しが無いことである。

二回目（直しの後）は受入を満たした。serial.log は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\serial.log`。

- 起動の行：
  - `[   16.658850 0:1] sched: 4 CPUs in 4 cores, 1 caches, 1 nodes; deadline (EDF, reserved) and fair (EEVDF) classes, preemptible`
  - `[   16.698749 0:1] smp: 4/4 CPUs online, each running the scheduler`
  - `[   16.817870 0:1] store: C: "Nocturne" on nvme0, 1150 MiB, 243 MiB free`
  - `[   16.883035 0:1] wm: compositor started at 2560x1440`
  - `[   16.883047 1:13] wm: without vertical blanks, frames are paced at the mode's 60.000 Hz`
  - `[   17.807783 1:11] net: 10.0.2.15/24 gateway 10.0.2.2 dns 10.0.2.3:53 (DHCP)`
- serial.log に `PANIC`・`] E `・`ASSERT`・`no pages` の行は無い。
- デスクトップ：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\boot1.png`。壁紙、机上のアイコン、タスクバーが出た。
- Terminal：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\term1.png`。二度押しで開き、prompt `nocturne:C:\Users\User$` が出た。
- `ps` と `free`：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\term2.png`（切り出しは同じフォルダーの `term2-crop.png`）。
  - `ps` は核の thread（idle0〜3、epoch0、vmalloc、acpi、power、usb、net、store、compositor、keep）と、init、term、sh、ps を示した。
  - `free` は `Mem: 4134668K total, 91344K used, 4043324K free` と `Kernel heap in use: 3203K` を示した。
- ブラウザー：about:home が開いた（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\br1.png`）。
  - 最大化し、`https://en.wikipedia.org/wiki/Moon` を開いた（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\wiki1.png`）。15 秒後に、記事の本文、目次、infobox と月の画像、Appearance の欄が出た。
  - 記事の中を押すとリンクに当たり、「Ray system」の記事へ移った。本文と画像三枚が出た（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\core\wiki2.png`）。

## 残った欠陥と未確認

- `qemu-image` は `RUST_SYSROOT=none` を付けないと組めない。Rust の std の env（`set_var`・`remove_var`・`set_current_dir`）が、消えた `setenv`・`unsetenv` を引く。直すには三つの手が要る。libc にネイティブの設定の口を設ける。`ports/rust/nocturne.patch` の env をその口に合わせる。std を組み直す。これは ABI と libc の設計の判断なので、主担当と持ち主に渡す。
- 性能は測っていない。fps、合成の時間、`prof` の `throttles`・`late jobs` の値は無い。締切の級が合成のコマ落ちを無くすことは確かめていない。
- 実機と Hyper-V では確かめていない。
- アプリの thread が締切の級を求める ABI が無い。アプリの GPU の提出、音、動画は公平の級で走る。
- 合成は核の thread 一本である。VBLANK の無い画面は mode の拍（ここでは 60 Hz）で回る。画面ごとの thread と VRR の周期は合成の持ち主の仕事である。
- `input_period_ns` は短くなる方へしか動かない。速い装置を抜いても、`usb` の周期は長くならない。
- user の空間に大きな頁が無い。GPU の BAR を 4 KiB の頁で写す。
- INVLPGB を使っていない。撃ち落としは IPI だけで行う。
- `sys/syscall.c` の `shared_lock` を、mm の共有 object と `lib/span` へ移していない（任意の項目）。
- 23f80428 が `init.c` から init の `FOLDER`・`TERMINAL` の設定を消した。Terminal は `C:\Users\User` で開いた。今回の受入に効く欠けは見つからなかった。
