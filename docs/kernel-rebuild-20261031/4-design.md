# 波 4 の一：装置の層（kernel/src/dev/）の設計の決定

元は `4-dev-plan.md` の報告（Plan の型の書き手の下書き、全文）。この文書には、下書きを照らした結果と、下書きが本体に残した判断と、下書きから変えた所だけを書く。

**下書きをそのまま採らない所**（ユーザー、2026-10-31「GUI FIRST MEANS, FIRST CLASS WINDOWING AND GPU SUPPORT」「THIS IS NO LINUX」。00-design.md §3）
- 下書きは Linux の形に寄っている：driver core の bus・device・driver と probe、KMS に似た connector・CRTC・plane、blk-mq、dpm の名と分け方。
- 表示・GPU・窓（下書き §2.8 の「表示 ↔ 窓」、§3.11、§3.12、§5 の表示の行）は採らない。
  - ここは GUI が先の設計（窓と GPU の層の下書き、`4-gui-gpu-plan.md`）から決める。
  - 装置の層はそれに仕える。
- 装置の木・寿命・割り込み・DMA・電源・通知の無い待ちの中身は採る。ただし名と分け方は Nocturne の目的から決め直す。各段の依頼文を書く時に、Linux の名をそのまま使っていないかを照らす。

## 1. 照らした結果

**コード**（今の main、`223af19e`）
- `dev/pci.c`：segment 0 だけ（17・31・204）、`ecam_active[65536 / 8]`（26）、`cf8_lock`（48）、`COMPLETION_FLOOR_MS 1000`（759）、全体の `reset_mutex`（821）。下書きのとおり。
- `dev/iommu_amd.c`：`RING_BYTES 4096`（85）、全体の `amd_lock`（160）、錠の下で割り込みを閉じた 100 ms の待ち（457）。下書きのとおり。
- `gui/wm.c:2761` の `gpu_init()`。下書きのとおり（監査 02 の 2845 は行がずれた）。
- 分割（GPT）の今の持ち主は `fs/store.c`（`gpt_read`・`gpt_find`・`gpt_room`、C: の区画を伸ばす所も）。休止の像は C: の中のファイル（`dev/hibernate.c` の `store_raw_place`・`store_raw_find`）。
- `common/gpu_abi.h` と `SYS_GPU_*` を使うのは virgl の道だけ：`vm/virtio_gpu.c`、`sys/syscall.c`、`user/libc/gpu.c`・`user/include/gpu.h`、`user/libc/web/js_canvas.c` の blit と clear、`user/apps/gpu_demo.c`、`tests/gputest.c`・`tests/canvastest.c`、`scripts/mksdk.sh`、Makefile。nvrm と Mesa（`ports/mesa/nocturne_gl.c`）は使わない。

**仕様**（`D:\Programes\OS DEV INFO`、pdftotext で原文を読んだ）
- AMD IOMMU（48882 r3.10）：COMPLETION_WAIT の i の旗は MMIO 2020h の `ComWaitInt`（RW1C）を立てる。MMIO 0018h bit 4 の `ComWaitIntEn` が 1 なら割り込みになる。下書きのとおり。
- NVMe Base 2.4：
  - CAP.TO は CSTS.RDY の変わりを待つ「worst-case」の時間。500 ms 単位で、最大は FFh（127.5 s）。下書きのとおり。
  - §3.6：shutdown は RTD3E 以上待つことを勧め、RTD3E が 0 なら最低 1 秒。ただし RTD3E は「typical」の時間で、上限ではない。下書き §3.3 の「CSTS.SHST を RTD3E の期限で読む」は直す（§2 の 1）。
- xHCI：R/S を消してから 16 ms 以内に halt する（§5.4.1・§5.4.2）。MSI では、MaxIntrs が 32 を超えても使えるのは interrupter 0〜31 だけ（§4.17 "Interrupters and PCI Interrupt Mechanisms"）。下書きのとおり。
- PCIe 6.0：
  - Conventional Reset を出てから 100 ms は設定の要求を送らない。1.0 s を過ぎても答えなければ故障と判断する。
  - FLR は 100 ms 以内に終える。
  - 下書きのとおり。
- HDA：WALCLK は Offset 30h（§3.3.16）。下書きのとおり。

## 2. 決めたこと

### 1. 通知の無い状態の bit（下書き §2.6 の 3）

仕様が上限だけを言う bit がある：NVMe の CSTS.RDY（CAP.TO・CRTO）、xHCI の CNR と SSS/RSS、IOMMU の制御の bit。これらの読み方を決める。

**読む時刻**
- 上限から逆に数えた倍の間隔で読む。k 回目の読みは「上限 × 2^(k−K)」の時（k = 0, …, K）。
- 最初の読みの時刻：
  - 装置が典型の値を言うなら、その値より前には読まない。
  - 言わないなら K = 10。読みは一つの立ち上げで高々 11 回。

**読み方**
- 各読みは alarm で起き、読みの間は thread が眠る。spin も、回数で回る輪も無い。
- 読むのは装置の start・reset・resume・shutdown の時だけ。定常の道には置かない。
- 装置の start は池の上で他の装置と並ぶので、遅い装置が起動の全体を止めない。

**この形を選んだ理由**
- 上限の時に一度だけ読むと、CAP.TO が 127.5 s の装置では起動がそれだけ遅れる。
- ms ごとに読むのは、回数の spin と同じ。
- 倍で読むと、読む数は log の数に収まる。完了から気づくまでの遅れは、真の完了の時刻の倍（または最初の読みの時刻）を超えない。

**下限だけを言う待ち**（NVMe の CSTS.SHST：RTD3E か、0 なら 1 s）
- 下限の時に一度読み、そこから倍で読む。
- 期限は電源の段の期限（下書き §2.5）。越えた時：
  - 電源断なら進める。
  - 眠りなら取りやめる。

### 2. `dev/iommu_intel.c` を消す

下限は Zen 4、上は EPYC で、Intel には手間をかけない（ユーザーの言葉）。`struct iommu_hw` の口は AMD の実装一つで残す。この口は「ユニットの実行」という責務の継ぎ目にある。

### 3. 休止の像の暗号化

**今は暗号化しない**
- 像は C: の中のファイル。C: そのものに暗号化は無い。
- 像の鍵の源は C: の鍵と同じであるべきなので、二つは一つの設計にする。
  - 鍵は TPM（AM5 の fTPM）で封じる見込み。TPM の仕様は手元に無く、未確認。
  - この設計は、波 4 の格納庫の設計で行う。
- 段 H がすること：
  - 像の完全さを暗号の hash で見る。これは壊れの検出で、改ざんの検出は鍵と一緒に来る。
  - 戻した後に像の block を返す。これは今の形。
- 今の像も同じく平文なので、新しい汚れではない。

### 4. 分割（GPT）

- 今の持ち主は格納庫（`fs/store.c`）で、それを保つ。
- blk は disk の全体を出し、格納庫が自分の区画を探して伸ばす。
- `fs/` の中で GPT を別の部品に分けるかは、波 4 の格納庫の設計で決める。

### 5. AHCI

- 下書きは D0 で消すとした。決めた所：
  - D0 では消さない。
  - 消すのは段 B。実機の C: が SATA なら、段 B が下書き §3.4 の形で建て直す。
- 理由：D0 と B の間に `all` の媒体を渡すと、実機が disk を失いうるため。
- 実機の C: が NVMe か SATA かは、ユーザーへの実機の一覧に入れる。

### 6. GPU（virgl の道）は残す

**経緯**
- 一度、消すと決めて消した（791b1980）。理由は、監査 02 が「VM だけの実装が vm.h を通らずに共通コードと ABI とブラウザーへ漏れている」と判定したこと。
- ユーザーの指示（2026-10-31「undelete the gpu」）で戻した。下書きの D0 の「`dev/gpu.h`・virtio の 3D・`gpu_init`・`SYS_GPU_*` を消す」は取り下げる。

**残る物**
- `dev/gpu.h`、`vm/virtio_gpu.c`、`common/gpu_abi.h`、`SYS_GPU_*`、libc の gpu、canvas の GPU の blit と clear、`gpu_demo`、試験、`ports/virgl`。

**後の段への影響**
- 3-S は `SYS_GPU_*` の手続きも、他と同じく持ち主へ移す。
- 段 D（表示）は、この道を消さない形で設計する。
- `vm/virtio_gpu.c` の中身は virgl の描画だけで、2D の scanout は無い。下書き §2.7 の「2D の scanout を表示の提供者に」は、今のコードには当たらない。

**NVIDIA の GPU を支える**（ユーザーの指示、2026-10-31「We need to SUPPORT gpus」「WE SUPPORT NVIDIA GPUS」）
- Nocturne は NVIDIA の GPU を支える。実機の RTX 4070 を、移植した NV のドライバー（nvrm、`ports/nvidia`）と Mesa（`ports/mesa`）で動かす。
- 装置の層のどの段も、この道を壊さない。下書きが「消して建て直す」とした部品のうち、NVIDIA の道が通るもの：
  - `pciuser`（claim・BAR・DMA・割り込み・ACPI・画面の hold）→ 段 C。
  - IOMMU（nvrm の claim の domain）→ 段 I。
  - `fb`（画面の渡し、panic の面）→ 段 D。
  - `pm`（claim を持ったままの眠り）→ 段 P。
  - 建て直しは中身を新しい形にすることで、nvrm が使う機能を一つも減らさない。各段の依頼文に「nvrm が使う口の一覧と、その後も同じに動くこと」を書く。
- 波 0〜2 で、NVIDIA の道の核の側を変えた（`pciuser.c`・`iommu.c`・`iommu_amd.c`・`fb.c`・`init.c`）。中身：割り込みを CPU ごとの vector へ、claim ごとの錠、handle の権利、process と thread の分け。
  - nvrm は組めて link も通る。
  - RTX 4070 の実機では、まだ動かしていない（QEMU に NVIDIA の GPU は無い）。
- VM：virtio-gpu の virgl も残す。
- 表示の模型（§3.11）と claim（§3.12）は、NVIDIA の GPU を支える道として設計する。消す・痩せさせる側に倒さない。
- 監査 02 の境界の指摘は残っている。`gui/wm.c` と `sys/syscall.c` が vm.h を通らずに `dev/gpu.h` を呼んでいる。
  - 直す向きは、機能を消すことではない。GPU の口を本当の抽象化の層にする（名と契約を持ち、各側に本物の実装がある：VM の virgl と、実機の GPU）。
  - この層の設計は段 D の前に書く。

### 7. `vm/ac97.c`

- 段 S で、試験の機械を HDA（`-device ich9-intel-hda -device hda-output`）に替える時に消す。
- Hyper-V 第 2 世代の音は RDP なので、AC'97 を使うのは QEMU の古い機械だけ。

### 8. `vm/ata.c`・`vm/tulip.c`

- D0 で消す。媒体は UEFI・q35・第 2 世代だけになった（main 463abe7f）。

## 3. 段と順（下書き §6 を採る。変えた所）

**前提**
- 波 3（3-V、3-S、3-T）。

**段の順**
1. D0：装置の核と PCI の核。AHCI は残す（§2 の 5）。
2. 一組目：A・I・B・U。
   - B は AHCI を消す。要るなら建て直す。
   - I は `iommu_intel.c` を消す。
3. 二組目：S（ac97 を消す）・D・C・M・R。
4. 三組目：P → H。H は §2 の 3 の範囲。

**一度に走らせる書き手**
- 5 未満。一組目の四人は作業木で並べる。

**格納庫・網・窓の下書き**
- 装置の口が決まった後に頼む。blk の要求の口、netdev、表示の模型、入力の router が継ぎ目。
- 休止と C: の暗号化（§2 の 3）は、格納庫の下書きに入れる。

## 4. 劣る所と後の段（盤の「設計の借り」へ）

- polling の I/O が無い。
- APST・HMB、runtime PM、S0ix。
- PCIe の hot-plug・AER・DPC・SR-IOV。
- UAS、USB Audio、USB の網。
- EC と熱の zone（_TZP の見回りを求める zone は扱わない）。
- 実機の NIC。
- 休止の像と C: の暗号化。

## 5. 実機で見ること（`all` を渡す時に、ユーザーへの短い一覧に入れる）

- RTX 4070：nvrm が claim し、画面が nvrm へ渡り、窓とブラウザーの WebGL が動くか。波 0〜2 の後、まだ見ていない。
- C: が NVMe か SATA か（§2 の 5）。
- root port の DRS・FRS・Readiness Time Reporting・Immediate Readiness。
- firmware に S3 があるか（Low Power S0 Idle だけではないか）。
- 板の音の codec と、それが HDA 1022:15E3 に付くか。
- RTX 4070 の HDA の関数の IOMMU の群（ACS）。
- IOMMU のユニットの数。
