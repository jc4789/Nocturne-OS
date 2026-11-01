# 監査：一コマが画面に出るまでの道（合成・表示・入力・時間・音）

## 依頼文

あなたは Nocturne OS（`D:\Programes\llm vibe slop os`、独自の OS、カーネルは C、対象は実機）の監査役。読むだけ。コードを変えない。ビルドも QEMU も動かさない。別の書き手が同じ作業木でカーネルの中核（`kernel/src/mm/`、`arch/percpu.h`・`preempt.h`・`cpuset.h`、`sys/thread`・`process`・`sched`・`wait`・`epoch`、これらは今は消えていて書き直し中）を書いている。あなたはそれ以外を読む。

# 持ち主の要件
GUI・GPU・合成（compositor）が第一級。カーネルが原因のコマ落ち・かくつきがゼロ。規模の例：600 Hz の画面、3 枚の画面（それぞれ違う周波数、VRR もある）、ゲームと YouTube と Discord を同時に開いて、カーネルのせいで落ちるコマが無い。この規模に耐えない作りは欠陥。Linux・UNIX・POSIX を標準とも手本とも見なさない。対象は今の PC（Zen 4 以上、上は論理 CPU 1024 の NUMA）。shim・stub・決め打ち・特例の分岐は欠陥。

# 読む範囲
画面に一コマが出るまでの道の全部：入力（`kernel/src/dev/input.c`、`usbhid.c`、`xhci.c`）→ アプリへの事象（`sys/events.c`、`sys/channel.c`、`sys/watch.c`、`sys/object.c`）→ アプリが描いて渡す（`common/display_abi.h`、`common/gpu_abi.h`、`user/libc/gui.c`、`user/libc/ui.c` の該当部）→ 窓の管理と合成（`kernel/src/gui/wm.c`、`desktop.c`、`screenmode.c`）→ 表示の装置（`dev/fb.c`、`dev/gpu.h`、`dev/pciuser.c`、`vm/virtio_gpu.c`、NVIDIA の表示を渡す道 `user/apps/init.c` と `ports/nvidia`）→ 時間（`sys/clock.c`、`sys/alarm.c`、`arch/lapic_timer.c`、vblank・VRR の扱い）。音（`dev/audio.c`）も同じ目で見る。地図は `.claude/rules/map.md`。codegraph（MCP の `codegraph_explore`）と rg を使う。

# 問い
1. 合成は誰が（どの thread・どの文脈）、いつ（何を合図に）走るか。画面ごとの周波数と vblank・VRR に合わせているか、固定の周期や見回り（polling）か。画面が 3 枚で周波数が違うとき成り立つか。600 Hz（1.67 ms）に間に合う形か。
2. アプリの buffer は合成へどう渡るか（写しの回数、共有、同期の印）。一コマの写しの量は画面の数と解像度でどう増えるか。
3. 一コマの道で、大域の錠・全走査・O(窓の数) や O(画面の画素) の仕事・眠る待ち・割り込みを止める長い区間はどこか。
4. 中核（sched・mm・wait）に何を求めているか：thread の級と締切、起こしの道、共有メモリーの対応、WC の対応、大きなページ、DMA。呼んでいる中核の関数名と、その呼び方。
5. GPU（実機の NVIDIA、virtio-gpu）の表示と flip の道は、抽象化の層になっているか、装置ごとの特例か。
各欠陥に、ファイル:行、何が起き、規模の例でどう壊れるか、を書く。直し方の案は短く添えてよい。

# 報告
`docs/kernel-core-20261101/audit-compositor.md` へ日本語で全文（この依頼文の全文を先頭に）。返事にも要点を日本語で返す。サブエージェントをさらに出さない。この Windows の Python で書くなら `newline='\n'`。

---

## 報告

読んだ版は作業木の `c5e0e7b6`。静的に読んだだけで、ビルド・試験・QEMU・実機は動かしていない。中核（`sys/sched`・`wait`・`thread`・`mm/`）は消えているので、その振る舞いは呼ぶ側の注釈と `docs/kernel-rebuild-20261031/2b-S.md` から読んだ。行の番号は `c5e0e7b6` のもの。

### 0. 結論

- 今の作りは「画面は一枚」を前提に全部の層が組んである。カーネルの状態（`sw`・`sh`・`back`・`frame_next`・vblank の状態）、ABI（`struct n_scanout`・`struct n_screen_mode`・`struct n_screen_state`・`SYS_PCI_SCREEN_VBLANK`）、nvrm（一つの head、一つの desk 面）のどれにも画面の番号が無い。3 枚の画面は形として作れない。
- VRR はどこにも無い。モードは `refresh_mhz` 一つだけを持つ。合成は vblank を合図に描き始めるので、VRR では最低の周波数に張り付く。
- 合成はカーネルの一つの thread が CPU で全画素を描く。大域の眠る錠 `wm_mutex` を一コマの描画のあいだ持ち続け、窓の syscall はその間すべて待つ。
- 描いた絵は表示中の scanout へ CPU が直に書く。flip も裏の面も無い。vblank の後に書き始めるので、毎コマ走査の途中に書き込み、裂け（tearing）が出る。
- アプリの絵は CPU のメモリーの窓の buffer を通る。GPU で描いたアプリは VRAM → RAM → 窓 → 合成の裏 → VRAM と、一コマに 2 回 PCIe を渡る。窓の buffer を GPU と共有する口も、同期の印（fence）も無い。
- 全画面のゲームだけは nvrm の直の scanout（NVIDIA 専用の私的な protocol）で flip できる。その flip は 1 ms の眠りで完了を見回るので、600 Hz（1.67 ms）に間に合わない。
- vblank は GPU の割り込み → カーネル → 利用者の nvrm の割り込みの thread → RM → NVKMS の一つの作業の thread → syscall → 合成、と利用者空間を二度回る。どの利用者の thread にも級も締切も無い（ABI に口が無い）。
- EV_FRAME の周期の単位がカーネル（ns）と ABI（µs）とで違う。ブラウザーには画面の周期が届かず、16 ms の決め打ちのまま走る。
- 実機の音の driver が無い（`audio_init` の注釈どおり）。流れの数は 8 本に固定。
- GPU の汎用の口 `dev/gpu.h` の唯一の実装は VM の virtio-gpu で、同期の読み戻しだけの小さな描画器。実機の GPU の抽象化の層ではない。

### 1. 一コマの道（今の形）

```
入力：
  xHCI の MSI（xhci_isr、IMOD 250 µs）
  → usb の thread（全 USB を一つで、実時間 INPUT）poll_all → hid_report
  → usage を scan code set 1 へ訳す（usbhid.c:72）→ input.c（大域の input_lock、割り込みを閉じる）
  → wm.c の input_put：環 inq[256] へ、wake_compositor
  → 合成の thread が次の回の頭で wm_lock を取って handle_key／handle_mouse
  → push_event：窓の ev[128] へ、wq_wake_all ＋ object_notify
  → アプリの SYS_WIN_RECEIVE か SYS_EVENTS_WAIT

アプリが描いて渡す：
  単一 buffer：user_buf へ CPU で描く → SYS_WIN_PRESENT が wm_lock の下で buf へ写す（写し 1）
  二重 buffer：buf か user_buf へ描く → SYS_WIN_PRESENT が pending にする（写しなし）
  GPU（mesa）：VRAM で描く → pipe_texture_map で RAM へ読み戻す → 上のどちらか

合成：
  合成の thread（カーネル、実時間 COMPOSITOR、一本）が
  vblank か「モードの周期」を合図に wm_lock を取り、
  損傷の矩形ごとに scene を取り、壁紙と窓を back（RAM）へ描く（写し 2）
  → wm_lock を放して fb_present：back → 表示中の scanout（VRAM、WC）へ CPU で写す（写し 3）
  → EV_FRAME を各窓へ

vblank：
  GPU の割り込み → claim_isr（カーネル）→ nvrm の irq_thread（利用者、級なし）→ RM
  → NVKMS の timer → NVKMS の作業の thread（一本、級なし）→ vblank_arrived
  → SYS_PCI_SCREEN_VBLANK → wm_vblank（着いた時刻を vblank の時刻にする）→ 合成を起こす
```

### 2. 問い 1：合成は誰が、いつ走るか

- 走るのはカーネルの thread「compositor」一本（`kernel/src/gui/wm.c:2800`）。実時間の級 `SCHED_PRIORITY_COMPOSITOR` を自分に付ける（`wm.c:2424`）。`2b-S.md` によれば `sched_realtime` の既定は周期 10 ms・予算 8 ms。
- 合図は二つ。vblank が 100 ms 以内に来ていれば、vblank の数が前の描画の数から動いたとき（`wm.c:2387-2389`）。来ていなければ、表示中のモードの周期の格子（`frame_next`、`wm.c:2392-2413`）。周波数が分からなければ 60 Hz の決め打ち（`FALLBACK_REFRESH_MHZ`、`wm.c:120`）。
- 合成は vblank が来てから描き始め、描き終えてから scanout へ書く（`wm.c:2509-2522`、`1813-1820`）。次の vblank までに書き終える期限という考えが無い。
- 画面は一枚。`sw`・`sh`・`back`・`wall`（`wm.c:104-105`）、`frame_next`・`frame_mhz`（`121-122`）、vblank の状態（`131-133`）、`screen_shown_refresh_mhz`（`screenmode.c:44`、`276`）が全部一つずつ。3 枚・周波数違いは作れない。
- VRR は無い。`struct n_screen_mode` は `refresh_mhz` だけ（`common/abi.h:354-359`）。合成が vblank を待ってから描くので、VRR の画面では vblank 自体が flip を待って延び、最低周波数で回る。
- 600 Hz：一コマ 1.67 ms。2560×1440 の全面の損傷では、窓を読む 14.7 MB、back へ書く 14.7 MB、`fb_present` が back を読む 14.7 MB、VRAM へ書く 14.7 MB、計 59 MB を一コマで動かす。600 Hz では毎秒 35 GB を一本の thread が動かす。3840×2160 なら毎秒 80 GB。しかもこの間 `wm_mutex` を持つ。周期 10 ms の予算の級では 1.67 ms の締切を表せない。

### 3. 問い 2：アプリの buffer の渡り

- 窓の buffer はカーネルの `vmalloc` の 4 KiB の頁で、作った process だけに `SYS_WIN_MAP` で写像される（`wm.c:679-720`、`2954`）。別の process（ブラウザーの子、GPU の process、画面共有）には渡せない。共有の object（`N_KIND_SHARED`）ではない。
- 単一 buffer の窓も buffer を二つ持つ（`alloc_buffer`、`wm.c:683-686`）。present のたびに矩形を `user_buf` から `buf` へ写す（`wm.c:3050-3057`、wm_lock の下）。
- 二重 buffer の窓は buffer 二つで、合成が次のコマで pending を取るまで、アプリは次の buffer を取れない（`SYS_WIN_MAP` の `WIN_MAP_ACQUIRE` の待ち、`wm.c:2957-2970`）。行列の深さは 1。ゲームが合成より速く描いても、描き捨て（mailbox）も先描きもできない。
- 同期の印は無い。「EV_FRAME は合成が読み終えた印で、vblank でも走査でもない」（`abi.h:519-520`）。GPU の fence を渡す口も、表示された時刻を返す口も無い。
- 一コマの写し：
  - CPU で描く単一 buffer の窓：アプリ → `buf`（写し 1）→ `back`（写し 2）→ scanout（写し 3）。
  - CPU で描く二重 buffer の窓：→ `back` → scanout の 2 回。
  - GPU で描く窓（WebGL、mesa）：VRAM → RAM（`ngl_read_drawing_buffer`、`ports/mesa/nocturne_gl.c:413`、`ST_FLUSH_WAIT` で GPU を待つ）→ 窓 → `back` → VRAM。PCIe を 2 回渡る。
  - 全画面の直の scanout：写しなしで flip。ただし NVIDIA だけ、全画面だけ、焦点のある一つの窓だけ（`wm.c:449-452`）。
- 量の増え方：損傷の面積に比例し、画面の数には比例しない（画面が一枚しか無いから）。損傷の矩形は 32 個を超えると全部の外接矩形に潰れる（`wm.c:276-281`）。四隅で動く窓が 33 個あれば全面を描く。

### 4. 問い 3：一コマの道の錠・走査・待ち

- **大域の眠る錠 `wm_mutex`**（`wm.c:92-102`、`165-167`）。窓・重ね順・損傷・指・直の scanout・遠隔・クリップボードを全部守る。合成は入力の処理から一コマの描画の終わりまで持つ（`wm.c:2466-2528`、放すのは `fb_present` の間だけ）。持っている間に止まるもの：
  - 全アプリの `SYS_WIN_PRESENT`・`MOVE`・`RESIZE`・`SET_TITLE`・`FRAME`・`POINTER`・`STATE`（`wm.c:3256-3262`）。
  - nvrm の直の present（`image_present` が毎回 `n_pci_screen_state` を呼び、`wm_screen_state` が wm_lock を取る。`ports/nvidia/display.c:1463-1468`、`wm.c:523-538`）。
  - 遠隔の画面、`wm_pointer_image`、クリップボード、設定。
- **錠の下の大きな写しと眠り**
  - `SYS_WIN_PRESENT` の矩形の写し（`wm.c:3055-3057`）。4K の全画面の窓なら一回 33 MB。
  - `SYS_SCREEN_GRAB` は全画面を `copy_to_user` で写す（`wm.c:3132-3135`）。頁違反で眠る。画面共有（Discord）が毎コマ呼べば、毎コマ合成が止まる。
  - `SYS_WIN_RESIZE` は確保、写し、`vmm_user_free`（TLB の撃ち落とし）、`map_user`（頁ごとの `vmm_map_page`）を錠の下で行う（`wm.c:3086-3121`、`704-720`）。
  - `set_screen_size` は全面の確保と壁紙の全画素の描画を錠の下で行う（`wm.c:2601-2629`）。
  - `wm_remote_copy` は錠の下で矩形を写す（`wm.c:2735-2745`）。
- **O(窓の数) と O(矩形 × 窓)**
  - `composite_ram` は損傷の矩形ごとに `capture_scene` で全窓を写し取る（`wm.c:1704-1710`、`1532-1549`）。矩形 32 × 窓 64。
  - `frames_asked` は毎回全窓を見る（`wm.c:2403-2407`、`2509`、`2525`）。
  - `composite` は毎コマ全窓を回って EV_FRAME を出す（`wm.c:1791-1804`）。
  - `focused`・`window_at`・`zindex` は全窓を線形に見る（`wm.c:305-338`、`390-394`）。
- **O(画面の画素)**：合成の全描画が一本の thread の上。`render_scene` は CPU の数だけ tile に分けるが、同じ thread で順に回すだけ（`wm.c:1590-1603`）。しかも製品の道（`composite_ram`）は `render_scene_direct` で一つの tile しか使わない（`wm.c:1712`）。`wm_work_budget`・`scene_rows` は試験の `scene_benchmark` でしか使わない（`wm.c:1496-1510`、`1621-1702`）。
- **見回り**
  - `presents_drain` は 100 µs ごとに眠って見回る（`dev/fb.c:205-209`）。
  - nvrm の flip の完了は 1 ms ごとに眠って見回る（`ports/nvidia/display.c:796-819`、`813`）。
  - nvrm の割り込みの thread は 50 ms で目覚めて RM を回す（`ports/nvidia/nvrm.c:44`、`99`）。
  - virtio-gpu は MSI-X が無ければ 1 ms ごとに見回る（`vm/virtio_gpu.c:111-117`）。
  - xHCI は割り込みが来なければ 1 ms・4 ms で見回る（`dev/xhci.c:463-478`、`1504`）。
- **割り込みを止める区間**
  - `input.c` は大域の `input_lock` で割り込みを閉じ、その下で sink を呼ぶ（`dev/input.c:6-10`、`143-147`）。
  - 音の混ぜは `audio_lock` で割り込みを閉じたまま、流れ × 枠の画素ごとの輪で回る（`dev/audio.c:185-240`）。
  - `alarm_interrupt` は来た alarm を全部、割り込みの中で呼ぶ（`sys/alarm.c:177-204`）。

### 5. 問い 4：中核に求めているもの

#### 5.1 呼んでいる中核の関数と呼び方

| ファイル | 関数 | 呼び方 |
|---|---|---|
| `gui/wm.c` | `kthread_create("compositor", wm_thread, NULL)` | 合成の thread を一本作る（2800）。 |
| `gui/wm.c` | `sched_set_policy(thread_current(), sched_realtime(SCHED_PRIORITY_COMPOSITOR))` | 自分を実時間の級へ（2424）。 |
| `gui/wm.c` | `sched_yield()` | 旗 `wmyield` のときだけ毎回（2532）。 |
| `gui/wm.c` | `mutex_lock`／`mutex_unlock`（`wm_mutex`） | **入れ子で取る**。`sys_win_create` の `handle_install` の失敗が `win_close` → `win_destroy` → `wm_lock` を呼ぶ（2927-2928）。`handle_key` → `launch` → `wm_notify` → `wm_lock`（2017、1030、2810）。中核の mutex が再入を許さなければ固まる。 |
| `gui/wm.c` | `wq_wake_all`、`wq_wait_unless`、`wq_wait_locked`、`wq_wait_locked_until` | 合成の眠り（2449）、直の状態の待ち（532）、present の終わりの待ち（2597）、窓の受け箱（644、2963）。 |
| `gui/wm.c` | `vmalloc`／`vfree` | 窓の buffer 二つ、back、wall、深い面（683-686、926-927、2603-2604）。4 KiB の頁。 |
| `gui/wm.c` | `vmm_kernel_translate`、`vmm_map_page(..., PTE_P\|PTE_W\|PTE_U\|PTE_SHARED\|PTE_NX)`、`mmap_reserve`、`mmap_hole_add` | 窓の buffer を頁ごとにアプリへ写像（704-720）。 |
| `gui/wm.c` | `vmm_user_free` | 窓の写像を外す。wm_mutex の下で撃ち落とす（544-546、3108-3111）。 |
| `gui/wm.c` | `vmm_space_get`／`vmm_space_put` | 窓が process の空間を参照で持つ（2930、663）。 |
| `gui/wm.c` | `process_current()->pid`／`->vmm_space`、`thread_current()->killed`、`thread_current()->name` | 窓の持ち主、待ちの中止。 |
| `gui/wm.c` | `cpu_online_count()` | 試験の tile の数（1503）。 |
| `dev/fb.c` | `vmalloc_map_frames(frames, pages, PTE_W \| type \| PTE_NX)` | scanout を WC などの型で核へ写像（266）。 |
| `dev/fb.c` | `vunmap`、`sleep_until` | scanout の取り替え、present の見回り（207、242）。 |
| `dev/pciuser.c` | `mutex_lock(&screen_lock)`、`process_kill`、`kthread_create("screen hold")` | 画面の持ち主の切り替え、握りっぱなしの driver の停止。 |
| `sys/syscall.c` | `shared_scanout_open` | 共有の object（BAR の view）の frame の列を取る（724-757）。 |
| `sys/events.c` | `wq_add`、`wq_remove`、`wq_wake(…, 1)`、`wq_wait_locked_exclusive_until`、`address_watch_arm` | 事象の行列（163-177、140、430、318）。 |
| `sys/object.c` | `epoch_enter`／`epoch_leave`／`epoch_defer` | handle の錠なしの読み。 |
| `sys/alarm.c`・`sys/clock.c` | `per_cpu`、`this_cpu`、`preempt_disable`／`preempt_enable` | CPU ごとの alarm の heap と時計。 |
| `dev/xhci.c` | `sched_set_policy(…, sched_realtime(SCHED_PRIORITY_INPUT))`、`dma_page`、`pmm_alloc_contig` | usb の thread（1453）、DMA の環。 |
| `vm/virtio_gpu.c` | `pmm_alloc_contig`、`vmm_map_mmio`、`wq_wait_unless`、`sleep_until` | VM の GPU。 |

#### 5.2 一コマの道が中核に要るもの（規模の例から）

- **締切の級**：周期と予算と締切を thread ごとに持つ級。周期は画面ごとの周波数（600 Hz なら 1.67 ms）。既定の周期 10 ms・予算 8 ms（`2b-S.md:86`）では表せない。予算を使い切ると公平の級へ落ちる作りは、3 枚の画面で合成が重いコマに全部の画面を遅らせる。
- **利用者の thread の級**：ABI に thread の級・締切の口が無い（`common/abi.h` の `SYS_THREAD_*` に無い）。ゲームの描画の thread、Discord の声の thread、ブラウザーの媒体の worker、nvrm の割り込みの thread、NVKMS の作業の thread は全部、公平の級で走る。
- **起こしの道**：vblank はカーネルの割り込みで受け、合成（画面ごと）を直に起こす道。今は利用者空間を二度回る（§8.1）。起こしから走るまでの遅れの上限を中核が保証する。
- **再入の mutex か、再入しない WM**：§5.1 のとおり wm.c は入れ子で取る。中核の `struct mutex` は再入を約束していない（`kernel.md` は「FIFO の待ち手へ手渡し」だけを書く）。
- **共有メモリー**：窓の面を共有の object にし、handle で他の process・GPU へ渡せること。今は `vmalloc` の頁を作った process だけへ写す。
- **WC**：scanout の写像（`vmalloc_map_frames` の `type`）。今は使っている。窓の面を GPU の VRAM に置くなら、アプリ側の WC の写像も要る。
- **大きなページ**：4K の窓は 33 MB、4 KiB の頁で 8100 枚。合成が毎コマ全部を読むと TLB を外す。2 MiB の頁と、頁ごとでない写像（`map_user` の頁ごとの `vmm_map_page`）が要る。
- **NUMA**：窓の面と back を、合成の thread と GPU の近い node に置く口。今は `vmalloc` に node の引数が無い。
- **DMA**：窓の面を IOMMU で GPU に見せる口（zero copy）。今は無い。

### 6. 問い 5：GPU の表示と flip の層

- **表示の driver の口は層になっている部分がある**。`SYS_PCI_GIVE_SCREEN`・`SYS_PCI_SCREEN_MODES`・`SYS_PCI_SCREEN_STATE`・`SYS_PCI_SCREEN_VBLANK`（`common/abi.h:115-135`、`dev/pciuser.c:708-840`）は特定の装置の名を持たない。どの表示の driver のアプリでも scanout を渡せる。
- **ただし一枚・一 head・一面だけの層**。`screen_owner` は一つ（`pciuser.c:251-257`）、`scanouts[3]` と `shown` は一つの画面の取り替え用（`dev/fb.c:38-39`）。nvrm も一つの display と一つの head を選ぶ（`ports/nvidia/display.c:297-353`、`134-144`）。
- **flip の層は無い**。デスクトップの面は一枚で（`desk_create`、`display.c:495-536`）、カーネルが CPU で直に書く（`fb.c:363-373`）。裏の面、flip の要求、flip の完了の知らせ、表示された時刻は ABI に無い。
- **直の scanout は NVIDIA の私的な道**。アプリ（mesa）は `nvrm.h` を include して `nvrm_scanout_create`・`nvrm_scanout_present` を直に呼ぶ（`ports/mesa/nocturne_gl.c:20`、`615-656`）。カーネルは「どの pid を直に見せるか」と指の絵だけを知る（`struct n_screen_state`、`abi.h:390-400`）。別の GPU（AMD、Intel）が来れば mesa と driver の両方に二つ目の道を書くことになる。
- **`dev/gpu.h` は VM の装置**。唯一の実装は `vm/virtio_gpu.c`（冒頭 1-7 行）。同期の読み戻しだけの三角形と blit、大域の `busy` で一度に一つ（`virtio_gpu.c:82`、`692-738`）、写しは利用者 → 核 → 装置 → 核 → 利用者（`sys/syscall.c:2947-3028`）。合成は使わない。`wm_init` が `gpu_init` を呼ぶ（`wm.c:2761`）ので、共通の合成の初期化から VM の装置の初期化を呼んでいる。

### 7. 入力の道

- usb の thread は一本で、全部の xHCI、全部の USB 装置（HID、hub、大容量記憶、網）、列挙の眠り（100 ms など）を受け持ち、実時間 INPUT の級で走る（`dev/xhci.c:1451-1517`、`1453`）。HID の報告は列挙や記憶の転送の後ろに並ぶ。記憶の仕事が実時間の級で走る。
- HID の usage を一度 PS/2 の scan code set 1 へ訳してから、US 配列の表で文字へ訳す（`dev/usbhid.c:57-74`、`dev/input.c:18-35`）。
- 指の報告一つごとに合成を起こす（`wm.c:1932`）。8 kHz の指なら毎秒 8000 回、合成が wm_mutex を取る回を回す。IMOD 250 µs（`xhci.c:1337`）で割り込みは毎秒 4000 回までに束ねる。
- アプリへの入力は合成の thread だけが配る（`wm.c:2469-2478`）。合成が描いている間（wm_mutex を持つ間）、入力は環に溜まったまま。ゲームの入力の遅れに合成の描画の時間が足される。
- 窓の受け箱は 128 で、満ちたら黙って捨てる（`wm.c:45`、`365`）。キーを放した事象が捨てられれば、キーが押されたままになる。入力の環 256 も満ちたら捨てる（`wm.c:1830`、`1920-1921`）。
- `struct gui_event` に時刻が無い（`abi.h:496-505`）。カーネルは `in_ev.at` に時刻を持つが（`wm.c:1835`）、アプリへ渡さない。`n_event` にも時刻が無い（`sys/events.c:419-421`）。

### 8. 時間の道

#### 8.1 vblank

- GPU の割り込み → `claim_isr`（カーネル）→ nvrm の `irq_thread`（`ports/nvidia/nvrm.c:92-124`、級なし）→ RM → NVKMS の timer → NVKMS の作業の thread 一本（`ports/nvidia/nvkms_os.c:575-610`、級なし、KAPI の事象と全部の timer と共有）→ `vblank_arrived`（`display.c:973-980`）→ `SYS_PCI_SCREEN_VBLANK` → `wm_vblank`（`wm.c:2371-2381`）。
- `wm_vblank` は着いた時刻 `clock_now_ns()` を vblank の時刻にする（`wm.c:2375`）。装置の時刻 `timestamp_ns` は記録するだけ。利用者の二つの thread の寝起きのずれが、そのまま vblank の時刻のずれになる。
- vblank は一つの head のぶんだけ（`display.c:982-989`）。
- 100 ms vblank が来なければ周期の格子へ戻る（`wm.c:130`）。
- NVKMS の timer の待ちは壁の時計 `TIME_UTC` で測る（`nvkms_os.c:598-606`）。壁の時計を合わせると、待ちが一度ずれる。

#### 8.2 alarm と時計

- alarm は CPU ごとの pairing heap、TSC-deadline で張る（`sys/alarm.c:1-15`、`97-101`）。区間（early・latest）を持つ。一コマの道の時間の部品としては足りている。
- EV_FRAME の `y` の単位が違う。ABI は µs（`abi.h:521-522`）。カーネルは `frame_period()` の ns を入れる（`wm.c:1787`、`1795`、`1802`、`2399`）。`ui.c` は µs と読んで 1000 で割り ms とする（`user/libc/ui.c:2653`）。60 Hz で 16666 ms になる。ブラウザーは 1〜1000 ms の外を捨てて既定の 16 ms のままにする（`user/libc/web/js.c:619`、`10509`）。600 Hz でもブラウザーの idle の期限は 16 ms のまま。整数の ms に丸めるので、届いても 1.67 ms は 1 ms になる。

### 9. 音の道

- 実機の音の driver が無い。唯一の card は `vm/ac97.c`。`audio_init` は `vm_audio_init()` を呼ぶだけ（`dev/audio.c:766-770`、注釈「no driver for real sound hardware yet」）。実機では「何も鳴らさない sink」が alarm で時を刻む。
- 流れは 8 本に固定（`MAX_STREAMS`、`dev/audio.c:18`）。ゲーム・YouTube・Discord・ブラウザーの複数のタブで足りなくなる。
- 形式は 48 kHz・16 bit・2 ch に固定（`abi.h:473`）。
- 混ぜは割り込みを閉じた spinlock の下で、標本ごとの scalar の輪（`dev/audio.c:185-240`）。
- 書き手の待ちは全部の流れで一つの `audio_wq` を `wq_wake_all` する（`dev/audio.c:67`、`730`）。
- 音の時刻と画面の時刻をつなぐ口（表示の時刻、音の再生の時刻）が無い。映像と音の同期はアプリの推定になる。

### 10. 欠陥の一覧

| # | ファイル:行 | 何が起きるか | 規模の例でどう壊れるか | 直し方の案 |
|---|---|---|---|---|
| 1 | `gui/wm.c:104-133`、`dev/fb.c:38-39`、`dev/pciuser.c:251-257`、`gui/screenmode.c:44`、`common/abi.h:346-400`、`ports/nvidia/display.c:134-144`、`297-353` | 画面が一枚の前提。画面の番号がカーネルの状態にも ABI にも nvrm にも無い。 | 3 枚の画面は映らない。2 枚目以降は暗いまま。 | 画面（output）を object にし、画面ごとに scanout・モード・vblank・合成の締切を持つ。ABI の scanout・モード・状態・vblank に画面の handle を入れる。 |
| 2 | `common/abi.h:354-359`、`gui/wm.c:2387-2389` | VRR が無い。合成は vblank の後に描き始める。 | VRR の画面で、合成は最低周波数で回る。ゲームの可変の速さが画面に出ない。 | モードに周波数の範囲を持たせる。合成は「描き終えたら flip」、driver は範囲の中で vblank を延ばす。 |
| 3 | `gui/wm.c:1813-1816`、`dev/fb.c:363-373`、`ports/nvidia/display.c:495-536` | デスクトップの面は一枚。CPU が表示中の scanout へ直に書く。flip が無い。 | 毎コマ走査の途中に書き、裂けが出る。600 Hz では一コマの中で必ず走査と重なる。 | 画面ごとに面を二枚以上持ち、裏へ描いて vblank で flip する。flip の要求と完了を ABI に置く。 |
| 4 | `gui/wm.c:2509-2522` | 合成は vblank の数が動いたら描き始める。期限（次の vblank − 描く時間）で始めない。 | 描いた絵は早くて次の次の vblank に出る。入力から画面まで 2 コマ以上遅れる。 | 画面ごとの締切の thread が「次の vblank の前、描く時間の見積もりだけ前」に起きる。 |
| 5 | `gui/wm.c:92-102`、`2466-2528`、`3256-3262` | 大域の眠る錠 `wm_mutex` を一コマの描画の間ずっと持つ。窓の syscall も入力の配りも直の present もその後ろに並ぶ。 | 合成が 3 ms 描けば、その間ゲームの present も入力も止まる。600 Hz の締切 1.67 ms を超える。 | 窓の状態の写しを錠なしで読む（世代つきの scene）。窓ごとの錠と、合成が自分だけで持つ描画の状態に分ける。 |
| 6 | `gui/wm.c:1590-1603`、`1712`、`1496-1510` | 全画素の仕事が一本の thread。tile 分けは同じ thread の順の輪で、製品の道は使わない。 | 2560×1440 の全面で毎秒 35 GB を一本で動かす。3 枚なら三倍。 | 画面ごとの合成、tile を CPU ごとの thread へ配る。GPU で合成する。 |
| 7 | `gui/wm.c:3050-3057` | 単一 buffer の窓の present が wm_mutex の下で矩形を写す。 | 4K の全画面の窓が毎コマ 33 MB を錠の下で写し、合成を止める。 | present は面の取り替えだけにし、写しを無くす（面を共有の object にする）。 |
| 8 | `gui/wm.c:3122-3137` | `SYS_SCREEN_GRAB` が wm_mutex の下で全画面を `copy_to_user` で写す（頁違反で眠る）。 | Discord の画面共有が毎コマ呼べば、毎コマ合成が止まる。 | 合成の出力を共有の面として読ませる。錠の外で、世代つきで。 |
| 9 | `gui/wm.c:3086-3121`、`704-720`、`2601-2629` | 大きさの変更・写像・画面の大きさの変更が、確保・頁ごとの写像・TLB の撃ち落とし・壁紙の全描画を wm_mutex の下で行う。 | 窓を引き伸ばす間、全部の窓と合成が止まる。 | 確保と写像を錠の外で済ませ、取り替えだけを錠の下で行う。 |
| 10 | `gui/wm.c:226-228`、`276-281` | 損傷の矩形は 32 個で、超えると全部の外接矩形一つに潰れる。 | 動く窓が多いと全面を描く。600 Hz の全面は #6 の量になる。 | 損傷を画面ごとの tile の bitmap か区間の木（`lib/span`）で持つ。 |
| 11 | `gui/wm.c:1704-1710`、`1532-1549`、`2403-2407`、`1791-1804` | 矩形ごとに全窓の scene を取る。毎回全窓を走査する。 | O(矩形 × 窓) が毎コマ。窓が増えるほど合成の時間が延びる。 | scene を一コマに一度取る。frame を待つ窓は列で持つ。 |
| 12 | `gui/wm.c:44`、`2893` | 窓は全体で 64 枚に固定。 | ブラウザーの窓、Discord、ゲーム、道具で 64 を超えると窓が開かない。 | 窓を handle の表と同じく伸びる表で持つ。 |
| 13 | `gui/wm.c:45`、`365`、`1830`、`1920-1921` | 窓の受け箱 128、入力の環 256。満ちたら黙って捨てる。 | 重いコマの間にキーを放した事象が捨てられ、ゲームのキーが押されたままになる。 | 押下・解放は捨てない。動きだけを束ねる。満ちたら窓へ「失った」印を渡す。 |
| 14 | `gui/wm.c:2469-2478`、`1932` | 入力は合成の thread が配る。指の報告ごとに合成を起こす。 | 8 kHz の指で毎秒 8000 回 wm_mutex を取る。入力の遅れに合成の描画の時間が足される。 | 入力の配りを合成から分ける（入力の thread、焦点の表を錠なしで読む）。指は画面ごとのハードウェアの cursor で動かす。 |
| 15 | `common/abi.h:496-505`、`gui/wm.c:1835`、`sys/events.c:419-421` | 入力の事象にも `n_event` にも時刻が無い。 | ゲームが入力の時刻で描く時刻を決められない。遅れを測れない。 | `gui_event` と `n_event` に単調時計の時刻を入れる。 |
| 16 | `gui/wm.c:1743-1752`、`ports/nvidia/display.c:585-617` | デスクトップでは指を絵に描き込む。ハードウェアの cursor は直の scanout の時だけ使う。 | 指が動くたびに合成と scanout の写しが要る。指の速さが合成のコマに縛られる。 | 画面ごとの cursor の plane を常に使う。driver の口に cursor の位置と絵を置く。 |
| 17 | `gui/wm.c:2512-2514`、`2957-2970`、`common/abi.h:512-518` | 二重 buffer の行列の深さが 1。合成が次のコマで取るまで、アプリは次の面を取れない。 | ゲームは合成の速さに縛られ、描き捨てができない。描く時間と待つ時間が重なり、コマが落ちる。 | 窓ごとに面の数を選べる行列（mailbox と FIFO）。 |
| 18 | `gui/wm.c:679-720`、`2954` | 窓の面は `vmalloc` の 4 KiB の頁で、作った process だけが写せる。共有の object でない。 | GPU で描くアプリは VRAM → RAM → 窓 → back → VRAM と 2 回 PCIe を渡る。ブラウザーの子の process は窓へ直に描けない。 | 窓の面を共有の object（GPU の VRAM も可）にし、handle で渡す。合成は面を GPU で読む。 |
| 19 | `common/abi.h:519-522`、`gui/wm.c:1787`、`2399`、`user/libc/ui.c:2653`、`user/libc/web/js.c:619` | EV_FRAME の周期を ABI は µs、カーネルは ns で入れる。ui.c は µs と読む。ブラウザーは範囲の外を捨てる。 | ブラウザーは 144 Hz でも 600 Hz でも 16 ms の周期のまま。idle の仕事がコマを越える。 | 単位を一つに決め（ns）、ABI・カーネル・ui.c・js.c を揃える。js.c は ms の整数をやめる。 |
| 20 | `common/abi.h:519-520` | EV_FRAME は「合成が読み終えた」だけで、表示の時刻も次の vblank の時刻も無い。 | ゲームも動画も、次に出る時刻を知らずに描く。動画のコマの間隔が揺れる。 | 表示された時刻（vblank の装置の時刻）と次の締切を EV_FRAME で返す。 |
| 21 | `gui/wm.c:2375`、`ports/nvidia/display.c:973-980`、`nvkms_os.c:575-610`、`nvrm.c:92-124` | vblank が利用者の二つの thread（級なし）を回ってから届き、着いた時刻を vblank の時刻とする。 | vblank の時刻が寝起きのずれで揺れる。3 枚の画面の vblank はそもそも届かない。 | vblank をカーネルの割り込みで受け、装置の時刻のまま合成（画面ごと）へ渡す。driver の thread に締切の級を付ける。 |
| 22 | `ports/nvidia/display.c:796-819`、`1463-1468` | 直の present が前の flip の完了を 1 ms の眠りで見回り、毎回 `n_pci_screen_state`（screen_lock と wm_mutex）を呼ぶ。 | 600 Hz（1.67 ms）では見回りの粒が粗く、flip を一つおきに逃す。合成が描いている間、ゲームの present は wm_mutex を待つ。 | flip の完了を事象（割り込み）で受ける。present の可否は錠なしの状態の世代で見る。 |
| 23 | `ports/mesa/nocturne_gl.c:20`、`615-656`、`gui/wm.c:449-452` | GPU の直の表示は NVIDIA の私的な protocol で、全画面・焦点・一窓だけ。mesa が `nvrm.h` を直に include する。 | 窓のゲームと YouTube は直の表示を使えない。別の GPU では道が無い。 | 面と flip と fence を Nocturne の ABI に置き、driver はその裏に置く。合成の plane に窓を載せる。 |
| 24 | `ports/mesa/nocturne_gl.c:413-422`、`646` | GPU の結果を `ST_FLUSH_WAIT` で CPU が待ってから読み戻す・present する。 | GPU と CPU が交互に止まり、コマの時間が足し算になる。 | fence を面と一緒に渡し、合成か display engine が待つ。 |
| 25 | `dev/gpu.h`、`vm/virtio_gpu.c:1-7`、`82`、`692-738`、`gui/wm.c:2761` | 汎用の名の GPU の口の唯一の実装が VM の装置。同期の読み戻し、大域の `busy`。合成の初期化が VM の装置を起こす。 | 実機では使えない口が ABI（`gpu_abi.h`）に残る。VM の都合が共通の名の後ろにある。 | `dev/gpu.h` と `SYS_GPU_*` を VM の後ろ（`vm/vm.h`）へ下げるか、実機の GPU の層を設計し直す。 |
| 26 | `dev/fb.c:142-178` | 変換の道（深い色・XRGB2101010・XBGR8888）は画素ごとに表を 3 回引く scalar の輪。注釈が「WHPX は SIMD の MMIO を真似ない」を理由に挙げる。 | 10 bit の画面では 600 Hz の全面を描けない。VM の都合が実機の道を縛る。 | 変換は合成の段で RAM の上で SIMD で行い、scanout へは写すだけにする。VM の制約は VM の後ろへ。 |
| 27 | `gui/wm.c:2424`、`dev/xhci.c:1453`、`docs/kernel-rebuild-20261031/2b-S.md:86` | 合成も usb も既定の周期 10 ms・予算 8 ms の実時間の級。 | 1.67 ms の締切を表せない。合成が予算を超えると公平の級へ落ちる。 | 画面の周期に合わせた締切の級を中核に置き、画面ごとの合成の thread に付ける。 |
| 28 | `common/abi.h:54-61` | 利用者の thread の級・締切の口が ABI に無い。 | ゲームの描画、Discord の声、媒体の worker、nvrm の割り込みの thread がビルドの仕事と同じ公平の級で走る。 | thread の級と締切を native ABI に足す（UNIX の名を写さない）。 |
| 29 | `gui/wm.c:2927-2928`、`2017`、`1030`、`2810` | wm_mutex を入れ子で取る。 | 中核の mutex が再入を許さなければ、窓を作る失敗や start menu からの起動の失敗で合成が固まる。 | 錠の下で通知や close を呼ばない形に直す（後回しの列へ積む）。 |
| 30 | `dev/xhci.c:1451-1517` | usb の thread 一本が全装置・列挙・記憶を実時間 INPUT で回す。 | USB の記憶の転送や列挙の眠りの後ろに、指とキーの報告が並ぶ。記憶の仕事が実時間で走る。 | 装置の完了ごとに行き先を分け、HID は自分の環と入力の thread へ。記憶は公平の級へ。 |
| 31 | `dev/usbhid.c:57-74`、`dev/input.c:18-35` | USB の usage を PS/2 の scan code set 1 へ訳し、US 配列の表で文字にする。 | 配列の違うキーボードで文字が違う。usage の情報が落ちる。 | usage をそのまま key code にし、配列は設定の持ち主（システム）が訳す。 |
| 32 | `dev/audio.c:766-770` | 実機の音の driver が無い。 | 実機では音が出ない。 | HDA の driver を置く（装置ごとの実時間の thread、`4-dev-plan.md:610` の設計の線）。 |
| 33 | `dev/audio.c:18` | 流れは 8 本に固定。 | ゲーム・YouTube・Discord・複数のタブで 9 本目が開かない。 | 流れを伸びる表で持つ。 |
| 34 | `dev/audio.c:185-240`、`67`、`730` | 混ぜが割り込みを閉じた spinlock の下の scalar の輪。書き手の待ちは一つの行列を全部起こす。 | 流れが増えるほど割り込みを止める区間が延び、他の装置の割り込みが遅れる。 | 混ぜを音の実時間の thread へ移し、錠の下では環の位置だけを動かす。流れごとに待ちの行列を持つ。 |
| 35 | `dev/fb.c:205-209` | present の終わりを 100 µs の眠りで見回る。 | driver が画面を握るたびに見回りの粒だけ遅れる。 | present の終わりで待ち手を起こす。 |
| 36 | `ports/nvidia/nvkms_os.c:598-606` | NVKMS の timer を壁の時計で待つ。 | 壁の時計を合わせると、timer が一度ずれる。 | 単調時計で待つ口を使う。 |
| 37 | `gui/wm.c:2784-2788`、`1621-1702`、`1490-1510` | 試験の旗（`wmverify`・`wmtrace`・`wmyield`・`wmshadowstrip=off`・`wmocclusion=off`・`wmbench`）と、試験でしか使わない tile の予算が製品の合成のコードにある。 | 製品の道と試験の道が分かれ、試験が製品を確かめない。 | 試験の道を消すか、製品の道そのものを試す形にする。 |
| 38 | `gui/wm.c:120`、`screenmode.c:37` | 周波数の分からない画面を 60 Hz と決め打つ。 | 600 Hz の画面で EDID が読めなければ 60 Hz で合成する。 | 周波数が分からないときは vblank だけで回し、決め打ちしない。 |

### 11. 調べた範囲と限界

- 読んだもの：`kernel/src/gui/wm.c`・`wm.h`・`screenmode.c`・`screenmode.h`・`desktop.c`（関数の一覧）、`kernel/src/dev/fb.c`・`fb.h`・`gpu.h`・`pciuser.c`（画面の部分）・`input.c`・`usbhid.c`（登録と報告）・`xhci.c`（thread と待ち）・`audio.c`（混ぜと submit）、`kernel/src/vm/virtio_gpu.c`（送受と入口）、`kernel/src/sys/events.c`・`channel.c`（頭）・`watch.c`・`object.c`（通知）・`alarm.c`・`syscall.c`（GPU の受け口）、`common/abi.h`・`display_abi.h`・`gpu_abi.h`、`user/libc/gui.c`、`user/libc/ui.c`（EV_FRAME）、`user/include/nocturne.h`（窓）、`user/apps/init.c`、`ports/nvidia/display.c`・`nvkms_os.c`・`nvrm.c`、`ports/mesa/nocturne_gl.c`（読み戻しと直の present）、`docs/kernel-rebuild-20261031/2b-S.md`。
- `sys/clock.c` と `arch/lapic_timer.c` は呼ぶ側と定数だけを見た。一コマの道の欠陥はそこに見つけていない。
- 中核は消えているので、`sched_realtime`・`mutex`・`vmalloc` の振る舞いは注釈と `2b-S.md` に拠る。
- 毎秒の量（35 GB など）は解像度と写しの回数からの算術。実機の写しの速さと、合成の一コマの時間は測っていない。
