# NVIDIA GPU（RTX 4070）を Mesa へつなぐ作業台帳

目的：実機（RTX 4070、AD104、Ada）で、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM を
Nocturne のドライバーとして動かし、Mesa（NVK＋Zink）でブラウザーの WebGL とコンポジターを
GPU で描く。GPU が無い環境（QEMU、Hyper-V）は今の softpipe のまま。

この台帳は作業の正本。作業を始める前と区切りごとに読み、状態・主張・決定を更新する。
過去の成功は、現在のソースとビルドで確かめるまで証拠にしない。

## 0. 動かない前提（ユーザーの指示）

- 日本語で報告。UNIX／POSIX 互換を足さない。GPLv2 専用のコードを入れない。Linux や他 OS の
  ソースを参照しない（`kernel-open/` は読まない、取り込まない）。サイト専用コード禁止。
- GSP ファームウェア（`gsp_ga10x.bin`）は git に入れない・配布しない。手元の gitignore した
  場所に置くのは可。NVIDIA の抽出スクリプト（MIT）は使ってよい。
- Rust の移植方法は任せる。補助に Sonnet 5.5 のサブエージェントを使う。
- QEMU は一台だけ。全体再ビルドを避ける。`build/data.img`・Hyper-V に触らない。
- コミットは main、push しない。AGENTS.md・.agents・`docs/real hardware pics/` はコミットしない。

## 1. 決定事項（理由つき）

| # | 決定 | 理由 |
|---|---|---|
| D1 | RM はカーネルに入れず、ユーザー空間のドライバーアプリ `/bin/nvrm` で動かす | 100 万行規模の RM が落ちても OS と画面が残り、実機でログを読める。描画命令の投入は Mesa が GPFIFO＋ドアベルで直接行うので、RM がプロセス内でも速度はほぼ落ちない |
| D2 | カーネルは「PCI 機能の権利」を種類つきハンドル（`N_KIND_PCI`）で渡す。取得できるのは init だけで、init がドライバーアプリへ起動時の引渡し表で渡す | 暗黙の特権を作らない。init は起動時に最初に動く信頼の根 |
| D3 | BAR・DMA メモリは既存の共有オブジェクト（`N_KIND_SHARED`）に「物理範囲」「部分の写し（view）」「キャッシュ属性」を足して表す。クライアントへはチャンネルの添付で渡す | 共有ページの仕組みを一つのまま使う（分断しない）。Mesa はこれを `n_shared_map` で写すだけ |
| D4 | RM 呼出し（escape）の引数は、クライアントと nvrm が共有する「引数の場」に置く。RM の `os_memcpy_from_user` は、その場の中の番地だけを nvrm 側の番地へ読み替える | 他アプリのメモリを読む汎用の特権を作らない |
| D5 | Mesa は NVK（Vulkan）＋Zink（GL）を本線にし、`nvkmd/nocturne/` を足す。DRM の uAPI は作らない | 上流の既定（Turing 以降は Zink＋NVK）。nvkmd は DRM を必須にしない抽象 |
| D6 | NAK などの Rust 部分のため、Rust の `std` を Nocturne へ移植する（Go の `GOOS=nocturne` と同じ形） | 上流の Mesa を書き換えずに済み、他の Rust プログラムにも使える |
| D7 | RM の取り込みは `src/nvidia` と `src/common` のうち、ビルドに要るものだけ（MIT）。`kernel-open/`・`nouveau/`・GPL の物は入れない | ライセンスと参照の制約 |
| D8 | 最初は画面出力（display engine）に触らない。起動時のフレームバッファーは `nv_get_screen_info` で RM に知らせて保護する | 実機で画面が消える危険を避ける |
| D9 | ファームウェアは gitignore した `local/firmware/nvidia/615.78.08/` に置く。存在するときだけ手元のイメージへ入れる | git に入れない・配布しない |

## 2. 構成（予定）

```
カーネル  : N_KIND_PCI（取得・情報・config・BAR・MSI/MSI-X・割り込み待ち）
            共有オブジェクト：物理範囲・view・キャッシュ（WB/WC/UC）・DMA の bus 番地
init      : NVIDIA の表示装置（10de, class 03）を見つけたら権利を取り、/bin/nvrm を起動
/bin/nvrm : RM 本体＋Nocturne の OS 層（os_*／nv_*）。割り込みスレッド、作業キュー、
            チャンネル "nvidia" でクライアントを受け、escape を rm_ioctl へ渡す
libnvrm   : クライアント側（接続、引数の場、alloc／control／free／map）
Mesa      : nvkmd_nocturne（pdev/dev/mem/va/ctx、同期）→ NVK → Zink → WebGL
Rust      : x86_64-unknown-nocturne の std（NAK・nil 用）
```

## 3. 段階と状態

状態：未着手／進行中／済（何で確かめたか）／保留

| 段階 | 内容 | 担当 | 状態 |
|---|---|---|---|
| P0 | 調査（RM の OS 層、カーネル、Mesa、Rust） | 主担当 | 済（下の主張表） |
| P1 | GSP ファームウェアの入手（.run を実行せずに抽出） | 主担当 | 済（`local/firmware/nvidia/615.78.08/`、sha256 を §7 に記録、`git check-ignore` で除外を確認） |
| P2 | カーネル ABI（D2・D3）と init の起動 | 主担当 | 済：QEMU（edu 装置、`appdrivers=any`）で `pcitest: 0 failed`。NVIDIA の無い機械では init は nvrm を起動しない |
| P3 | RM の取り込みとビルド（静的ライブラリー）、未定義の OS 関数の洗い出し | サブ B | 済：`third_party/nvidia-open`（無変更、`diff -rq` で一致と担当報告）、`ports/nvidia/nvrm.mk`。1100 オブジェクトがエラー 0。主担当が `NV_BSD` の影響（版名の文字列だけ）とファームウェア名の形を確認 |
| P4 | Nocturne の OS 層と /bin/nvrm（GSP 起動まで） | 主担当＋サブ B | 書いた：`os.c`・`os_pci.c`・`rm_api.c`・`os_stubs.c`・`nvlink_os.c`・`spdm_stubs.c`・`nvrm.c`。RM が要る 397 シンボルすべてに定義あり。主担当の修正 2 件（割り込みの時間切れの符号、`ucodes_ga10x.bin` の同梱漏れ）。全体ビルドで `/bin/nvrm`（13 MB）がリンク。実機の GSP 起動は未確認（P10） |
| P5 | Rust std の Nocturne 移植 | サブ A | ホストで済：std（rust-src 1.98.1 へのパッチ `ports/rust/nocturne.patch`）がビルドでき、`rusttest` が静的 ELF にリンクできる（未定義シンボル 0）。主担当がパッチ対象・ターゲット定義・C 構造体との一致を確認し、`rust.mk` を Makefile へ接続。QEMU の Nocturne 上で `rusttest: ok (38 checks)` |
| P6 | libnvrm（クライアント）と通信規約 | 主担当 | 済（ビルド）：`libnvrm.o` は nvrm.mk の `NVRM_CLIENT_OBJ` として libc の遅延メンバーに。実機での往復は P10 |
| P7 | Mesa：NVK＋nvkmd_nocturne＋NAK／nil のビルド | 主担当（サブ C は 1 時間で成果がほぼ無く、主担当が引き取り） | 済（ビルド）：NAK・nil・nvk_rs の Rust は `libnvk_mesa_rs.a`（rustc の staticlib）、C は Vulkan 共通 runtime＋NVK＋`nvkmd/nocturne/`（pdev・dev・mem・va・ctx・sync・rm）がエラー 0。実機で未実行 |
| P8 | Zink と WebGL（nocturne_gl.c）への接続、softpipe への後退 | 主担当 | 済（リンク）：`nocturne_gl.c` の強い参照で GL を使うプログラムにだけ Zink＋NVK＋Rust＋libnvrm が入る。ブラウザーの試しリンクは未定義 0、41.5→46.8 MB。sysroot の libc.a には Rust の .a の各メンバーを展開して入れる（TinyCC 用） |
| P9 | 全体ビルド、QEMU で回帰が無いこと、文書、コミット | 主担当 | 済：NVK・Zink を含む差分ビルド（img・vhd・vhdx・iso）、QEMU で机上・3D Cube（63 fps）・ブラウザー（about:home、get.webgl.org の WebGL の立方体が softpipe で回る、戻る、ホイールで下端と先頭）を目視 |
| P10 | 実機確認（ユーザー） | ユーザー | 進行中：1 回目（2026-10-20、RTX 4070）は rm_init_rm の中で `mov %cr4` の GPF（C35）。2 回目は rm_init_adapter まで進み、BAR0 の二重写像の表明（C36）と上流ポートが見つからない（C37）の後、ログが止まった。どちらも修理済み。3 回目はチップセットの失敗が消え、GSP 起動前のスクラバーの Falcon DMA が時間切れ（C39）。修理済み、4 回目は未実施 |

## 4. 主張の表

状態：受理（根拠を確かめた）／却下（誤り）／未確定（要確認）

| # | 主張 | 状態 | 根拠・確かめ方 |
|---|---|---|---|
| C1 | `src/` は MIT。GPLv2 との二重ライセンスは Linux のカーネルモジュールとしてリンクした時だけ | 受理 | COPYING の冒頭。`src/` に GPL-2.0 専用の SPDX は無かった（rg） |
| C2 | RM が呼ぶ OS 関数のうち `src/` で定義されていないものは 273 個。大半（Tegra、cgroup、NUMA、vGPU、ACPI）は非対応で返せる | 受理（数）／未確定（非対応で足りるか） | os-interface.h／nv.h の宣言と `src/` の定義を突き合わせた。実際に要る物はリンクと実行で確かめる |
| C3 | RM は 1099 個の C ファイル。`nvoc` の生成物は同梱 | 受理 | srcs.mk、`src/nvidia/generated` |
| C4 | Turing 以降は GSP ファームウェアが必須。Ada は `gsp_ga10x.bin` | 受理 | kernel-open は読まず、`nouveau/` の抽出スクリプトと .run の中身で確かめる |
| C5 | NVK の nvkmd は DRM を必須にしない（DRM の操作は省ける） | 受理 | ローカルの mesa `src/nouveau/vulkan/nvkmd/nvkmd.h`（`get_drm_fd` 等は NULL 可） |
| C6 | NAK は Rust 約 6 万行で、`std` の collections・sync・io・fs を使う | 受理 | ローカルの mesa を rg |
| C7 | gallium nvc0 は chipset 0x190（Ada）を受け付ける | 受理（一覧に有る）／未確定（実際に描けるか） | nvc0_screen.c の chipset の switch |
| C8 | Nocturne カーネルは ECAM、64 ビット BAR、MSI／MSI-X、連続ページ、WC の PAT を持つ | 受理 | pci.h、pmm.h、pat.h |
| C9 | ユーザー空間の RM でも性能上の問題は小さい | 未確定 | 投入はクライアント直、RM は確保と制御だけ、という設計上の推論。実機で測る |
| C10 | RAM を WB の直接写像と WC／UC の写しで二重に持っても実害は小さい | 却下 → 修理済み | x86 は一つのページに二つの型を許さない。DMA の RAM は型を一つだけ持つ：最初の UC／WC のビューで直接写像（`vmm_direct_type`、2 MiB ページは分割、TLB 撃墜と clflush）も同じ型へ変え、別の型のビューと、写像が残る間の型変更は拒む。解放時に WB へ戻す |
| C11 | RM の `arch/nvalloc/unix` は NVIDIA 自身の OS 中立層（MIT）で、使っても UNIX 互換を Nocturne に足すことにはならない | 受理 | Nocturne 側へ POSIX の API を足さず、RM の OS 層を Nocturne の API で実装する |
| C12 | RTX 4070 は ReBAR 有効なら BAR1 が 16GB になり得る。BAR 全体を 4K で一度に写さない | 受理 | 16GB÷4K＝400 万 PTE。view で必要な部分だけ写す |
| C13 | アプリ終了時は fd をすべて閉じてから共有マッピングを外すので、PCI の権利（fd 3）が DMA メモリの解放より先に終わり、バスマスターが先に止まる | 却下（順序だけでは足りない） → 修理済み | バスマスターを消しても発行済みの DMA は残る。クレームの解放は BAR の写像を全プロセスから失効 → 割り込み停止 → BME 消去 → Device Status の Transactions Pending が消えるまで待つ（上限 200 ms）。DMA オブジェクトはクレームを参照で持ち、静止を確かめられなかったクレームの RAM は解放せず隔離する。終了処理は PCI クレームを他の fd より先に閉じる。複製されたハンドルも参照数で同じ扱い |
| C14 | MSI-X の表は BAR の中にあるので、BAR を写したドライバーは表を書き換えられる | 受理（危険として） | ドライバーアプリは init が起動する信頼済みのもの。IOMMU も無いので DMA でも同じ。権限分離の目標外と明記 |
| C15 | RM は割り込みを MSI-X でも MSI でも受けられ、INTx は不要 | 未確定（実機待ち） | OS 層は `n_pci_interrupts(1)` の後に `NV_FLAG_USES_MSIX`／`NV_FLAG_USES_MSI` を立てる。担当 B の調べでは RM はこの印を読まない。実際に割り込みが来るかは実機で |
| C18 | RM の OS 層が RM の「spinlock」を mutex にしても正しい（割り込み処理もスレッドなので） | 受理 | `rm_isr` は普通のスレッドで動き、`os_is_isr` は `nvrm_in_isr` で真になる。眠ってはならない区間で眠る危険は、割り込み文脈が本物の割り込みでない以上ない |
| C19 | 全クライアントが RM には同じ管理者権限のプロセスに見える | 修理済み | 接続ごとの権限（`nvrm_file.admin`）。チャネルは相手の素性を持たないので全クライアントが非特権（通常ユーザー相当）。管理者は nvrm 自身のスレッドだけ。根拠のある接続を `client_is_admin` で足せる |
| C17 | Zink は Vulkan を `util_dl_open(VK_LIBNAME)` と `vkGetInstanceProcAddr` だけで得るので、Nocturne の util の dl 層に静的な表（"vkGetInstanceProcAddr"→NVK の `vk_icdGetInstanceProcAddr`、`vkGetDeviceProcAddr`）を置けば、zink 本体を変えずに静的リンクの NVK を使える | 未確定 | zink_screen.c 3481〜3492 行。ローダー無しで ICD の入口がローダーの代わりになるか（インスタンス拡張の列挙等）は P8 で確かめる |
| C20 | GPU で描いた WebGL を毎フレーム CPU へ読み戻しても、softpipe より速い | 未確定（実機待ち） | 画面は起動時のフレームバッファー（D8）で、合成は CPU。読み戻しは Zink の staging コピーと待ち。遅ければ、読み戻しを非同期にするか表示側を GPU へ寄せる |
| C21 | GSP 下でも USERD とエラー通知を sysmem（CPU から見えるだけのページ）に置いてよい | 未確定（実機待ち） | `NV_CHANNEL_ALLOC_PARAMS` の `hUserdMemory[0]` と `hObjectError` に物理メモリ（sysmem）を渡す。NVIDIA の SDK の型と RM の受付けまで。RM が拒めば `nnrm_chan_create` が失敗を返す |
| C22 | GP エントリーの `SYNC_WAIT` が nvkmd の `no_prefetch` に当たる | 未確定 | clc36f.h の GP_ENTRY1 の SYNC。前のエントリーの完了まで次を先読みしない、という意味として使った |
| C23 | ホストのメソッド（セマフォ・WFI・MEM_OP）は GR チャンネルでは subchannel 0、コピー専用では 4 で出せる。初回に SET_OBJECT が要る | 未確定 | ctx.c の `host_subc`。ホストメソッドは subchannel を問わないはずだが、実機で確かめる |
| C24 | 4 KiB ページだけで足りる（大きいページは使わない） | 受理（制約として） | `bind_align` 4096。性能は実機で見る。大きいページは後で |
| C25 | フェンスは割り込みを使わずポーリング（2 ms は yield、その後 1 ms ずつ眠る）でよい | 未確定（性能） | 実機で待ちの遅れと CPU 使用率を測る。悪ければ nvrm の非停止割り込み通知を足す |
| C26 | 解放済みメモリの対応付けを外す時、RM が `NV_ERR_INVALID_OBJECT_HANDLE`／`OBJECT_NOT_FOUND` を返すのは成功扱いでよい（メモリの解放が対応付けも外す） | 未確定 | `nnrm_virt_unmap`。実機で RM の挙動を確かめる |
| C27 | チャンネル内部用 VA（3<<37〜1<<39、1 MiB ずつ、再利用しない）は使い切らない | 受理（上限として） | 131072 個。超えれば `VK_ERROR_OUT_OF_DEVICE_MEMORY` 相当で失敗し、黙って壊れない |
| C28 | VRAM の CPU 写像は BAR1 経由で RM の MAP_MEMORY が返す共有オブジェクトで得られる | 未確定（実機待ち） | libnvrm の `nvrm_map`。ReBAR 無しでは BAR1 が 256 MiB で、写せる量が限られる |
| C29 | QEMU（NVIDIA 無し）では `nvrm_connect` がすぐ失敗し、Zink は 1 行のログで softpipe へ後退する | 未確定 → P9 の QEMU で確かめる | `n_channel_connect` が相手無しで即失敗する前提 |
| C16 | 1 バイトの構成空間書き込みを 16 ビットの読み書きで行うと、隣の状態レジスターの RW1C ビットを消すことがある | 修理済み | RM に呼び元が無くても、カーネルの `pci_write16` 自体が 32 ビットの読み書きで Status を消していた（BME の操作、uACPI、ドライバーの 1／2 バイト書込み）。`pci_write16`／新しい `pci_write8` は本当の 16／8 ビット幅で書く（ECAM、CF8 とも） |
| C30 | 古い BAR の写像は、クレームの所有者が替わった後も使える | 却下 → 修理済み | クレームの最後のハンドルが閉じると、そのクレームの BAR オブジェクトの写像を全プロセスで外し、触れると全 1 の読取り専用ページ（バスから消えた装置と同じ）を見せる。新しいビュー・写像・DMA アドレスの照会は `N_ERR_CLOSED` |
| C31 | `n_shared_map` は読取り専用のハンドルからも書込みの写像を作れる | 却下 → 修理済み | 書込みの写像は書込みで開いたハンドルにだけ許す |
| C32 | イベント（`nv_post_event`／`nv_get_event`）は不要 | 却下 → 修理済み | 接続ごとの `nvrm_file` に上限つきの待ち行列。GOP の画面は `nv_get_screen_info` で RM へ渡し、RM がその範囲を保つ。nvrm の死亡は NVK がチャネルの照会で `DEVICE_LOST` にする（`nvrm_alive`） |
| C33 | RM が BAR を構成空間へ書き戻す必要は無い | 却下 → 修理済み | RM はリセット後（`kbifRestoreBarsAndCommand`、GC6 からの復帰の `kbifRestoreBar0`）に起動時の BAR の値を 32 ビットで書き戻す。カーネルは、起動時に見た値と同じ書込みだけを通す（戻せるが動かせない）。GC6 の経路は BAR0 だけ戻してデコードを入れるので、デコードには条件を付けない。MSI／MSI-X がリセットで消えたときの再設定は実機で未確認 |
| C34 | 他機能のトークン、BAR 写像の長さ、セマフォの破棄は問題ない | 却下 → 修理済み | 他の機能のトークンは機能ごとに一つを使い回す（応答する機能だけ）。BAR 写像の長さはページに丸めたビューの長さを返す。`os_free_semaphore` は `cnd`／`mtx` を破棄する。`TIME_UTC` の期限は、Nocturne の `TIME_UTC` が戻らないので、壁時計が進んでも早く起きて単調時計で測り直すだけ |
| C35 | RM の中にユーザーモードで実行できない命令は無い | 却下 → 修理済み | 実機で `nv_rdcr.c` の `osNv_rdcr4`（`mov %cr4`）が GPF（rip 0x46ec34、`RmInitCpuInfo` から）。nvrm.mk で同ファイルを外し、`os.c` が OSFXSR（常に有効）と CPUID.1:ECX 27（CR4.OSXSAVE の写し）から同じ値を返す。記号付きで再リンクした nvrm の逆アセンブル全体で、CR・DR・MSR・in／out・cli／sti・hlt・invlpg・wbinvd は他に 0 |
| C36 | nvrm が BAR0 の写像を `nv->regs->map` に置いてよい | 却下 → 修理済み | RM の `RmSetupRegisters` は自分で写像し、`aperture->map == NULL` を表明する（実機で osinit.c:168）。nvrm は BAR0 全体の写像を自分だけで持ち、`os_map_kernel_space` がその一部を返す。`nv->regs->map` は RM が設定する |
| C37 | 構成空間は自分の装置と上流のブリッジだけ読めればよい | 却下 → 修理済み | RM の `clStoreBusTopologyCache` は装置ごとに機能 0 が答えなければ他の機能を飛ばす。AMD ではルートポートが 00:01.1 で、00:01.0 はホストブリッジ。読めないと上流ポートも FHB（ホストブリッジ、チップセットの識別）も見つからない（実機で chipset.c:463／558 の表明と「Unable to get PCI port handles」）。カーネルは、上流の各ブリッジの装置の全機能とホストブリッジ（06/00）の読みも許す。無関係な装置は今まで通り `N_ERR_DENIED` |
| C38 | rm_init_adapter が止まっても分かる | 却下 → 修理済み | GSP の起動はうまくいくと何も出さないので、固まったのか遅いのかが見分けられない。nvrm は rm_init_adapter の間 10 秒ごとに経過と処理した割り込みの数を、終わると所要時間と成否をログへ出す |
| C39 | 装置のバスマスターは誰かが入れている | 却下 → 修理済み | RM はコマンドレジスターのメモリデコードとバスマスターを、リセット後の構成空間の復元でしか入れない（普段は OS 側のドライバーが装置を取るときに入れる前提）。カーネルの権利は入れず、終わるときに切るだけ。実機 3 回目で `s_dmaPoll_GA102: Error while waiting for Falcon DMA`（スクラバーの IMEM 読込み、4 秒で時間切れ）が約 60 秒ごとに繰り返した。nvrm が attach で 0x6 を立てる。C38 の行の間隔が 20 秒になっていたのも、早く起きた待ちが 10 秒を数え直していたためで、単調時計の決まった時刻で出すように直した |

## 5. 担当と受け渡し

- 主担当（Opus）：設計、カーネル ABI、init、nvrm の骨格、libnvrm、統合、ビルド、QEMU、コミット。
- サブ A（Sonnet）：Rust std の移植（`ports/rust/`）。
- サブ B（Sonnet）：RM の取り込みとビルド（`third_party/nvidia-open/`、`ports/nvidia/`）。
- サブ C（Sonnet）：Mesa の NVK 側（P6 の規約ができてから）。
- サブの報告は受理前に主担当が差分とビルドで確かめる。

## 6. 次の一手

1. P10（ユーザー）：RTX 4070 の実機で起動し、システムログの nvrm（GSP 起動）と、ブラウザーの WebGL が
   Zink で動くか（失敗ならログの `ZINK:`／`nvkmd`／RM の状態名）を見る。
2. 実機の結果で C15・C21〜C23・C25・C26・C28 を受理か却下へ。失敗した境界から直す。
3. 性能（C9・C20・C25）：同じ WebGL を softpipe と Zink で比べる。読み戻しとポーリングが重ければ直す。
4. 残り：大きいページ、VRAM 使用量の報告以外の kmd 機能（dma-buf は無し）、video は対象外。

## 7. 記録（新しい順）

- 2026-10-20：実機 3 回目（ユーザー）。FHB と上流ポートの失敗は消えた。rm_init_adapter の中で、スクラバーの
  ucode を Falcon の IMEM へ DMA で読む `s_dmaTransfer_GA102`（kernel_gsp_falcon_ga102.c:242）が
  NV_ERR_TIMEOUT、`kgspExecuteScrubberIfNeeded_AD102` 失敗（kernel_gsp_tu102.c:406）が 180 秒の間に
  3 回。割り込みは 0。原因はバスマスターが切れたまま（C39）。修理後、QEMU で `nvrmtest: 0 failed`、`pcitest: 0 failed`。

- 2026-10-20：実機 2 回目（ユーザー）。C35 の修理で rm_init_rm を越え、rm_init_adapter の中まで進んだ。
  BAR0 16 MiB at 0xfb000000、BAR1（ReBAR）16 GiB at 0xf800000000、ファームウェア 2 本を読込み。
  osinit.c:168 の表明（C36）、FHB が無い（chipset.c:463／558）、「Unable to get PCI port handles」（C37、
  RM はこれを無視して続ける）の後、nvrm のログは止まり、WebGL は softpipe のまま。止まった場所は
  ログから分からないので、経過の行（C38）を足した。

- 2026-10-20：実機 1 回目（ユーザー）。RTX 4070（AD104、10de:2786、副 19da:2714、PCIe 4.0 x16、
  Above 4G・ReBAR 有効、画面は GPU に接続、BIOS 95.04.3E.C0.82）、AMD の iGPU 1002:164e も有り。
  nvrm.log は「GPU 10de:2786 at 01:00.0, BAR0 16 MiB at 0xfb000000」と `_sysCreateOs` の 0x56
  （無害、RM は続行）の後、nvrm が GPF（C35）。WebGL は softpipe（9 fps）。NOCTDATA が無く /data は RAM。
  修理後、新しい `tests/nvrmtest.c`（edu 装置で nvrm を起動し、rm_init_rm を通って RM が装置を断り
  状態 1 で終わるか）を QEMU で：修理前は状態 141 で 2 failed、修理後は `nvrmtest: 0 failed`、
  `pcitest: 0 failed`。nvrm のシステムログの行に改行が無く、画面で繋がっていたのも修理。

- 2026-10-20：GLM の静的監査の指摘を修理。BAR の書き戻し（C33）、他機能のトークンの使い回し、
  BAR 写像の長さ、セマフォの破棄（C34）。pcitest に「BAR へ自分の値を書き戻せる」「BAR を動かせない」を
  追加。割り込みが取れないと 1 ms のポーリングへ落ちる設計（nvrm.c）は指摘通り維持。
- 2026-10-20：Codex の静的監査（97b7d802）の指摘を修理。カーネル：クレームの参照数と静止（BME 消去後に
  Transactions Pending を待つ、確かめられなければ DMA の RAM を隔離）、BAR 写像の失効（C30）、DMA RAM の
  型を一つに（C10）、読取り専用ハンドルの写像（C31）、本当の 8／16 ビットの構成空間書込み（C16）、
  `n_memory_release` の VA 再利用、終了時に PCI クレームを先に閉じる（C13）。RM の OS 層：大ページの
  散布への後退をやめる、割り込み・タイマー・ワーカーの生成失敗を失敗として返し停止を join、イベント
  （C32）、接続ごとの権限（C19）、終了時に全クライアントを終える。NVK：sparse の pieces を mtx と事前確保
  で守る、query_gr の失敗を失敗に、nvkmd_mem_map の参照数、host copy の失敗処理、nvrm の死亡で
  DEVICE_LOST。WebGL：bufferData(size)・texStorage・copyTexImage2D の外側・renderbuffer を 0 で初期化、
  描画バッファーを作った時に色 0・深度 1・ステンシル 0 で初期化。差分ビルド（img・vhd・vhdx・iso）。
  QEMU 一台（WHPX、4 コア、4096 MiB、1920×1080、AHCI、snapshot）：edu で `pcitest: 0 failed`（型の固定、
  UC ビューで DMA の写しが見える、クレーム終了後の BAR は全 1 を新たに確認）。通常の媒体で机上、3D Cube
  （63 fps）、ブラウザー（about:home、get.webgl.org の立方体が softpipe で回る、Alt+Left で戻る、ホイールで
  下端と先頭）を実マウスで操作して画面を見た。回帰なし。NVIDIA 実機の経路（RM・NVK・Zink）は未実行。

- 2026-10-19：P7・P8 をビルドとリンクまで。主担当が NVK 側を引き取り、`nvkmd/nocturne/` の 7 ファイル
  （rm・pdev・dev・mem・va・ctx・sync）を書いた。直した誤り：rm.c が Mesa の `nvtypes.h` を拾う（RM の
  include を前に置く対象別の変数）、ファイル直下の `STATIC_ASSERT`、存在しない状態名。nvk.mk の生成規則の
  上書き警告を除いた。libnvrm を nvrm.mk の `NVRM_CLIENT_OBJ` に、`u_dl.c` を mesa.mk に、Zink・NVK・
  Rust の .a・libnvrm を `LIBC_LINK` に。nvrm.mk の include を mesa.mk より前へ移した。ブラウザーは
  41.5→46.8 MB、browserjsworker 42.6→47.9 MB、sysroot の libc.a 95.8 MB、initrd 592 MB（手元の
  ファームウェア 85 MB を含む）。QEMU 一台（WHPX、4 コア、4096 MiB、1920×1080、AHCI、snapshot）で
  机上、3D Cube 63 fps、get.webgl.org の回る立方体（GPU 無しなので softpipe）、about:home の戻る・
  ホイールの下端と先頭を目視。回帰なし。Zink の後退ログはシリアルには出ない（アプリのログ）。

- 2026-10-19：P8 着手。取り込み済み Mesa はクローンと同じ 745709a（＋Nocturne の改修）と確認し、Zink を
  LF で取り込み。Nocturne に fd は無いので `ZINK_USE_DMABUF` と sync file の取込みを外した（POSIX の
  close 等は作らない）。Zink の Vulkan は `u_dl.c` の Nocturne 分岐（弱参照の NVK 入口）で得る。GPU では
  描画バッファーの消去を `clear_texture` に（毎フレーム CPU で塗らない）、softpipe は従来のまま。
  担当 C へ：vulkan util のオブジェクトを一つの変数で共有、`os_read_file` は nocturne_gl.c にある、
  ICD 入口 2 つの名前を保つ、と送付。
  未解決（C20）：GPU の描画結果は毎フレーム `pipe_texture_map` で CPU へ読み戻して合成する（D8 の起動時
  画面に出すため）。1080p では読み戻しが重い可能性。実機で測る。

- 2026-10-19：QEMU 一台（WHPX、4 コア、4096 MiB、1920×1080、AHCI 起動、`-device edu,dma_mask=0xffffffff`）。
  `pcitest: 0 failed`、`rusttest: ok (38 checks)`。pcitest の誤り 2 件を直した：edu の DMA バッファー
  （0x40000）は MMIO から見えないので WC view の確認を liveness レジスターで行う、edu の既定の DMA
  範囲は 28 ビットなので QEMU 側で 32 ビットへ広げる（32 ビット確保が範囲外なら失敗として数える）。
  通常の媒体で机上、ブラウザー（about:home、JS デモへのリンク、戻る、ホイールで下端と先頭）、3D Cube
  （63 fps）を実マウスで操作して画面を見た。回帰なし。

- 2026-10-19：担当 B の RM と OS 層を確認して受理。主担当の修正：`nvrm.c` の割り込み待ちの時間切れ判定の
  符号、Makefile のファームウェアに `ucodes_ga10x.bin` を追加（RM の osinit.c は GSP と ucodes の両方を
  必須として読む）。Makefile に `include ports/nvidia/nvrm.mk` と `/bin/nvrm` のリンク（`NVRM_FILES`）。
  BSD-3-Clause（libspdm、nvSha256）の全文を `third_party/nvidia-open/NOTICES` にまとめ、`COPYING` と
  ともに `/usr/share/licenses/nvidia-open/` へ。全体ビルドを開始。

- 2026-10-19：担当 A の Rust std を確認して受理（ホストでのビルドまで）。`include ports/rust/rust.mk` と
  initrd の依存 `$(RUST_FILES)` を Makefile に追加。sysroot（`tools/rust/`、git 対象外）が無ければ何も
  しない。rustc はユーザーの `~/.cargo/bin/rustc` 1.98.1。Nocturne 上の rusttest は未実行。

- 2026-10-19：nvrm のサーバー `ports/nvidia/nvrm_serve.c` と境界 `nvrm_serve.h` を作成（接続の受付、
  hello で引数領域を写す、要求を 1 つずつ OS 層の `nvrm_rm_ioctl` へ、MAP_MEMORY の返事に
  `nvrm_rm_mapping_object` の共有オブジェクトを添付、`nvrm_user_translate`）。単独コンパイル済み。
  OS 層側の 4 関数は担当 B へ依頼。
- 2026-10-19：`tests/pcitest.c`：QEMU の edu 装置（1234:11e8）で権利・構成空間・BAR と view・MSI・
  DMA（edu の DMA で往復）・解放後のバスマスター停止を確かめる試験。起動オプション
  `appdrivers=any`（init 以外のアプリにも claim を許す。起動設定を書ける者だけが使える）を追加。
  コンパイル済み、未実行（全体ビルドの後に QEMU で）。

- 2026-10-19：Makefile に GSP ファームウェアの規則（`NVIDIA_FIRMWARE`、既定は local/ の複製。
  無ければ入れない。gsp_ga10x.bin と LICENSE だけを `/usr/share/firmware/nvidia/615.78.08/` へ）。
  `make -n` で規則を確認。
- 2026-10-19：担当 C（Sonnet）を起動：NVK の `nvkmd/nocturne/` を libnvrm の上に書き、NVK の C 部分を
  コンパイルできるところまで（NAK・nil は担当 A の後）。
- 2026-10-19：担当 B へ OS 層の規約を送付：nv_alloc_pages は 1 確保 1 DMA オブジェクト、BAR は
  n_pci_bar と view、`nv_add_mapping_context_to_file` は待ち枠に保存して `nvrm_take_mapping` で
  取り出す、`os_memcpy_*_user` は `nvrm_user_translate` を通す、割り込みは n_pci_interrupt_take。
- 2026-10-19：libnvrm を作成（`ports/nvidia/include/nvrm.h`、`nvrm_protocol.h`、`libnvrm.c`）。
  チャネル "nocturne.nvidia.rm"、hello で 4MiB の引数領域（予約型の共有オブジェクト）を添付、
  要求は RM の escape の引数構造体そのまま、MAP_MEMORY の返事は共有オブジェクトの添付。
  単独コンパイル済み。サーバー側は担当 B の nvrm.c の形を見てから。
- 2026-10-19：init が NVIDIA の表示機能を claim して `/bin/nvrm bb:dd.f` を起動（handle 3、
  ログはシステムログ、init は複製を閉じる）。`/bin/nvrm` が無い媒体では何もしない。
  起動オプション `noappdrivers` でカーネルが claim を断る。

- 2026-10-19：カーネル側の ABI を実装。`kernel/src/dev/pciuser.c`（権利、構成空間、MSI／MSI-X、
  解放でバスマスター停止）、`syscall.c` の共有オブジェクトに範囲・view・キャッシュ属性・DMA を
  追加、`VT_PCI`→`N_KIND_PCI`、`init_pid`、`pci_device.kernel_owned`。変更した 6 ファイルを
  カーネルの CFLAGS で単独コンパイルし警告 0。実行は未確認。`docs/nocturne-native-abi.md` §9 を更新。
- 2026-10-19：ファームウェアを抽出（`tail -c +160326 | zstd -dc | tar`、スクリプトは実行せず）。
  gsp_ga10x.bin 27,043,992 B（sha256 `a20100ca…d58d584`）、gsp_tu10x.bin 22,901,632 B、
  ucodes_ga10x.bin 58,215,936 B、ucodes_tu10x.bin 8,837,632 B、LICENSE。

- 2026-10-19：.run（529,834,675 バイト、SHA-256 `3e3f6ddd…d07ffe`）を
  `D:\Programes\cloned repos\nvidia-driver-615.78.08\` に取得。makeself の頭（1018 行）の後ろが
  zstd の tar。スクリプトは実行しない。
- 2026-10-19：調査を終え、D1〜D9 を決定。
