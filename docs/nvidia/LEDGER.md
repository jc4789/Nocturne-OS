# NVIDIA GPU（RTX 4070）を Mesa へつなぐ作業台帳

目的：実機（RTX 4070、AD104、Ada）で、NVIDIA の open-gpu-kernel-modules 615.78.08 の RM を
Nocturne のドライバーとして動かし、Mesa（NVK＋Zink）でブラウザーの WebGL とコンポジターを
GPU で描く。GPU が無い環境（QEMU、Hyper-V）は今の softpipe のまま。

この台帳は作業の正本。作業を始める前と区切りごとに読み、状態・主張・決定を更新する。
過去の成功は、現在のソースとビルドで確かめるまで証拠にしない。

**2026-10-20の実機状態更新：** 提供写真ではSEC2 FalconのスクラバーIMEM DMAが `0x65` で時間切れ。
`0x56`（`NV_ERR_NOT_SUPPORTED`、`_sysCreateOs` の能力登録）は `os_caps.c` で修理済み（C52）。現在のDMA境界修正は実機復旧を証明していない。
値の照合、修正範囲、未更新媒体は
[実機DMA調査](bare-metal-dma-20261020.md)を参照。以下の過去の「実機未実施」「修理済み」は、
この最新の失敗報告を打ち消すものではない。

**2026-10-24の実機状態更新（3 回目）：** IOMMU（AMD-Vi）が有効なまま `RmInitAdapter` が `RmInitX86Emu`
で止まった（`0xA0000` の写像を断った、`0x32:0xffff:2178`）。原因は `primary_vga` を真にしていたこと。
偽にし、`os_flush_cpu_cache_all` を実装した（C41・C50）。この修理の実機確認は未実施（4 回目待ち）。
GPU が自分の DMA の外へ書いた観測は C51。

## 0. 動かない前提（ユーザーの指示）

- 日本語で報告。UNIX／POSIX 互換を足さない。GPLv2 専用のコードを入れない。Linux や他 OS の
  ソースを参照しない。`kernel-open/` は NVIDIA 自身のコードで比較の基準に含まれる。読んで照合してよいが、
  取り込まない。サイト専用コード禁止。
- GSP ファームウェア（`gsp_ga10x.bin`）は git に入れない・配布しない。手元の gitignore した
  場所に置くのは可。NVIDIA の抽出スクリプト（MIT）は使ってよい。
- Rust の移植方法は任せる。補助に Sonnet 5.5 のサブエージェントを使う。
- QEMU は一台だけ。全体ビルドは必要なとき・役に立つときはいつでもしてよい（2026-10-23）。`build/data.img`・Hyper-V に触らない。
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
| D8 | ~~最初は画面出力（display engine）に触らない~~（2026-10-24、D10 で置換）。起動時のフレームバッファーは `nv_get_screen_info` で RM に知らせて保護する | 実機で画面が消える危険を避ける。ただし RM が GPU を初期化した後は起動時の画面が凍るので、表示を GPU へ移す（D10） |
| D10 | 表示は nvrm の中の NVKMS（nvidia-modeset、MIT）が組む：モード設定、VRAM の scanout 面、flip、垂直帰線。面は `n_pci_give_screen` でカーネルへ渡し、コンポジターはその面へ描いて `n_pci_screen_vblank` の帰線で刻む。claim の終わりで起動時の画面へ戻す | RM の初期化後、CPU の起動時の画面（GOP）は表示エンジンから切られる。GPU が画面をつなぎ直さなければ起動後の画面は凍ったまま |
| D11 | 全画面で前面の窓が求めたら（`win_direct`）、NVKMS がその窓のアプリの画像をそのまま flip で出す（直接表示）。コンポジターは許可（全画面・前面・可視・メニュー／音量／通知／枠の表示なし・遠隔なし）を `n_pci_screen_state` でnvrm へ知らせ、ポインターの形と位置も渡す（nvrm はハードウェアカーソルで出す）。アプリの画像は nvrm が VRAM に作り（`NVRM_SCANOUT_*`）、NVK へ RM の dup で渡す。許可が無い間は従来どおり合成。2026-10-24 追加：画像は作るときに `n_pci_screen_image` でカーネルに登録し、カーネルが止まったときに表示エンジンが出し続ける画像にもパニック画面を描かせる。登録できない画像は作らない（C60） | 全画面の WebGL を毎フレーム CPU へ読み戻して合成するのは 1080p で重い。デスクトップの合成は裏で続けるので、許可が切れた瞬間に戻せる |
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
| P10 | 実機確認（ユーザー） | ユーザー | 進行中：1 回目（2026-10-20、RTX 4070）は rm_init_rm の中で `mov %cr4` の GPF（C35）。2 回目は rm_init_adapter まで進み、BAR0 の二重写像の表明（C36）と上流ポートが見つからない（C37）の後、ログが止まった。どちらも修理済み。3 回目（2026-10-24、IOMMU 有効、USB 起動）は `RmInitX86Emu` が `0xA0000` を写せず停止（C50 で修理）。4 回目は GSP の起動と `RmInitAdapter` が成功し、NVKMS の VT 切替えで nvrm が落ちた（C61 で修理）。5 回目は nvrm が落ちずにクライアントを受け付けたが、最初のモード設定で書かれない完了通知を待って表示を手放し（C62 で修理）、WebGL は softpipe のまま理由がログに無かった（C63 で記録を足した）。6 回目（2026-10-25、2 回起動）は nvrm の面への移行・モード設定・垂直帰線による歩調まで誤りの行なく進んだが、主モニター（Acer X27U Z1、QD-OLED、DP）は暗いまま戻らなかった。原因は不明。モードは起動画面と同じ 1920×1080 59.940 Hz で、設定アプリでは変えられなかった（C65 で修理）。7 回目（2026-10-25、2 回起動）は 2560×1440 59.951 Hz が誤りの行なく点いたことになっているが、どのモニターも暗いままで、何も変わらなかった。0a1d812a（C62）が点けるモード設定の完了を待たなくしたことが原因と仮定して監査し、C62 の却下が誤りと分かった（C66 で修理）。8 回目は点けるモード設定の 3 秒後に「has not completed」で起動画面へ返った。待っていた通知は BEGUN と時刻で、完了の判定が誤っていた（C67 で修理）。暗い画面の第一候補として出力 LUT の倍率 0 も直した（C68）。9 回目（2026-10-25）は X27U Z1 に 2560×1440 59.951 Hz が映り、設定アプリの試用と保持で 120.000 Hz になった（C65・C67・C68 を確認、C69 の表明も出なかった）。WebGL の Zink は NVK の装置の確保が GSP に断られて softpipe のまま（C70 で修理、同じ回にその先の C71・C72 も修理）。10 回目（2026-10-25）は `rpcRmApi*_GSP` の誤りが消え、装置が作れるようになった先で、WebGL を使うページを開くたびにブラウザーが NULL の入口へ跳んで落ちた（C73 で修理）。11 回目（2026-10-25）は WebGL Report の描画器が「zink Vulkan 1.4 (NVIDIA GeForce RTX 4070 (NVK AD10x))」、WebGL City が 54 fps、3D Cube が 280 Hz で 279 fps（C70〜C74 を確認）。12 回目は未実施 |

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
| C14 | MSI-X の表は BAR の中にあるので、BAR を写したドライバーは表を書き換えられる | 受理 → IOMMU のある機械では無害 | IOMMU（C48）の割り込み再マップ表が claim の要求元 ID とそのベクターだけを通すので、表を書き換えても他の割り込みは作れない。IOMMU の無い機械と `noiommu` では危険のまま（DMA も全メモリへ届く）で、claim のログが知らせる |
| C15 | RM は割り込みを MSI-X でも MSI でも受けられ、INTx は不要 | 未確定（実機待ち） | OS 層は `n_pci_interrupts(1)` の後に `NV_FLAG_USES_MSIX`／`NV_FLAG_USES_MSI` を立てる。担当 B の調べでは RM はこの印を読まない。実際に割り込みが来るかは実機で |
| C18 | RM の OS 層が RM の「spinlock」を mutex にしても正しい（割り込み処理もスレッドなので） | 受理 | `rm_isr` は普通のスレッドで動き、`os_is_isr` は `nvrm_in_isr` で真になる。眠ってはならない区間で眠る危険は、割り込み文脈が本物の割り込みでない以上ない。ただし CPU を移りうる点は C69 |
| C19 | 全クライアントが RM には同じ管理者権限のプロセスに見える | 修理済み | 接続ごとの権限（`nvrm_file.admin`）。チャネルは相手の素性を持たないので全クライアントが非特権（通常ユーザー相当）。管理者は nvrm 自身のスレッドだけ。根拠のある接続を `client_is_admin` で足せる |
| C17 | Zink は Vulkan を `util_dl_open(VK_LIBNAME)` と `vkGetInstanceProcAddr` だけで得るので、Nocturne の util の dl 層に静的な表（"vkGetInstanceProcAddr"→NVK の `vk_icdGetInstanceProcAddr`、`vkGetDeviceProcAddr`）を置けば、zink 本体を変えずに静的リンクの NVK を使える | 未確定 | zink_screen.c 3481〜3492 行。ローダー無しで ICD の入口がローダーの代わりになるか（インスタンス拡張の列挙等）は P8 で確かめる |
| C20 | GPU で描いた WebGL を毎フレーム CPU へ読み戻しても、softpipe より速い | 一部修理（実機待ち） | 窓の中の WebGL は今も Zink の staging コピーで読み戻して CPU で合成する。2026-10-24：全画面の canvas は読み戻さない直接表示を足した（D11）。窓の中の読み戻しの重さは実機で測る |
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
| C39 | nvrm は BME（バスマスター）を入れなくてよい | 却下 → 修理済み | カーネルは claim の間 BME を入れない。`nvrm_pci_bus_master` が `rm_init_adapter` の前に Command の BME を入れ（読み戻しで確かめ、書込みが効かない装置は失敗）、初期化の失敗後と `rm_shutdown_adapter` の後に切る。claim の終わりに BME が残っていれば異常終了とみなし、DMA の RAM を隔離 |
| C40 | 画面の取得・解放（`os_disable_console_access` 等）は空でよい | 却下 → 修理済み | RM が BAR1 を仮想モードにする間、システムが起動時の画面へ書くと GPU の外へ書く。新しい `n_pci_hold_screen`（入れ子、パニックは無視して描く、claim の終わりで返し全体を描き直す）。画面が他の所なら NOT_FOUND を無視。2026-10-21 訂正：30 秒で保持を無視して描く上限は、RM がまだ BAR1 を作り直している最中に競合を再開していた（RM の待ちはそれより長い）。時間切れの描画をやめ、保持は RM の解放か claim の終わり（装置を止めてから返す）でだけ終わる。止まったドライバーは `fbhold=<秒>`（既定無し）でアプリを止め、claim の終わりで画面を戻す |
| C41 | `nv_alloc_pages` のキャッシュ型と `os_flush_cpu_cache_all` は無視してよい | 却下 → 修理済み | UC／WC の確保は型つきの view を作って DMA の RAM の型を固定する（直接写像も同じ型）。2026-10-24 訂正：`NOT_SUPPORTED` では足りない。実機で RM は `kbifInitDmaCaps` の前（DMA の能力が 0 でスヌープ不可扱い）にファームウェアの写しの後で求める。`wbinvd` は特権命令なので、nvrm が WB で確保した DMA オブジェクトを一覧に持ち、全行を `clflush`（全 CPU のキャッシュに届く）して `mfence`。GPU は IOMMU でこれらにしか届かないので、これで GPU から見えるメモリは全部書き戻る。nvrm の写像が無いものは一時的に写す。UC／WC は一覧に入らない |
| C42 | 装置メモリは UC と WC の別名があってもよい | 却下 → 修理済み | 生きている写像の間、装置の 1 ページに型は一つ（SDM の PAT 別名の規則）。違う型の写像は DENIED でログに出す。起動時の画面はカーネルが WC で写しているので UC の写像は常に DENIED |
| C43 | 初期化に失敗した GPU も開ける | 却下 → 修理済み | `nv_is_gpu_accessible` は `adapter_ready` のときだけ真。RM 自身の `deviceConstruct` が権限不足で断り、制御装置の診断は使える（実ドライバーと同じ形） |
| C44 | サーバーは一つのループで足りる | 却下 → 修理済み | 接続ごとにスレッド。受付はメインループだけ。RM の順序は RM の錠が決める。終了は `serving_ended` のアドレス監視で全スレッドを起こし、全部が終わるまで待つ。スレッドを作れない接続は閉じてログ |
| C45 | イベントの待ち行列に上限は要らない | 却下 → 修理済み | データつきイベントは接続ごとに 4096 件まで。超えたら新しい方を捨て、接続ごとに一度ログ。取り出すと再び受け付ける。データなしは対象ごとに一つ（C32 を実際に上限つきへ） |
| C46 | `os_dump_stack` は空でよい | 却下 → 修理済み | nvrm はフレームポインターつきでビルドしているので、スレッドのスタック範囲の中だけフレームを辿り、戻り番地をログへ出す（範囲外・逆向き・非整列で止まる） |
| C47 | 構成空間の保護は Command と BAR だけで足りる | 却下 → 修理済み | ABI 文書 §9 の一覧（BIST、Interrupt Line、PMCSR、DevCtl、LinkCtl／2、ReBAR、SR-IOV、ACS・ATS・PRI・PASID）。読んで一部を変えて書き戻すなら通る。ブリッジは claim できない |
| C48 | IOMMU で DMA を隔離している | 却下 → 修理済み | AMD-Vi（`iommu_amd.c`）と VT-d（`iommu_intel.c`）のドライバー、共通層は `iommu.c`。claim した装置は専用のドメインに入り、その claim の DMA オブジェクト（作成から解放まで、解放はユニットの確認の後）とファームウェアの予約範囲（IVMD・RMRR）だけへ届く。割り込みは再マップ表で claim のベクターだけ。claim の終了で遮断。要求元 ID を他と共有する装置は `N_ERR_BUSY`。隔離は `N_PCI_ISOLATED` でアプリに見える。詳細は ABI 文書 §9 |
| C49 | claim の時に BAR の大きさを測ってよい | 却下 → 修理済み | 測るとメモリデコードを切るので、起動時の画面がその BAR にあると壊れる。大きさは列挙時の値（`pci_bar_size`）を使う |
| C50 | 起動時の画面を持つ GPU は `primary_vga` | 却下 → 修理済み | `primary_vga` は VBIOS を通す旧 VGA の画面を意味し、RM は x86emu のために `0xA0000` と option ROM を写そうとする（実機 3 回目で停止）。Nocturne は VGA を使わない。起動画面は `nv_get_screen_info` が返す線形フレームバッファーで、RM はそれを client-managed console として保ち、`RmDeterminePrimaryDevice` で主装置にもなる。`primary_vga` は常に偽。`console_device` は移植側で設定していたが、C61 で RM 自身（`rm_init_dynamic_power_management`）が決めるように戻した |
| C52 | RM の能力（`nv-caps`）は無くてよい | 却下 → 修理済み | `_sysCreateOs: RM Access Sys Cap creation failed: 0x56` は能力の根が無いための `NV_ERR_NOT_SUPPORTED`。`os_caps.c` が能力の木を持ち、ファイルの代わりに接続の権利で判定する |
| C53 | RM が初期化すれば画面は GPU が出す | 却下 → 実装（実機未確認） | RM は表示エンジンを組まない（それは NVKMS の役）。nvrm に NVKMS を移植し（`nvkms.mk`、`nvkms_os.c`、KAPI の客 `display.c`）、`GPU started` の後にモード設定・VRAM の面・`n_pci_give_screen`・帰線の転送まで行う。カーネルは `fb.c` の出力先を切り替え、`wm.c` が帰線で描き始める。QEMU では NVIDIA が無いので、カーネル側の ABI だけを pcitest（VGA の BAR を面にする）で確かめた。2026-10-24 監査：フリップの完了の待ちが働いていなかった（C56）、デスクトップへ戻すフリップの失敗で画面が黒いまま残った（C57） |
| C54 | ACPI（`_DSM`、`_ROM`、`_DOD`、`_DDC`、MUX、Notify）は非対応で返してよい | 却下 → 実装（実機未確認） | 実機の notebook・GPS・JT は `_DSM` を要る。カーネルの uACPI で claim の範囲（機能の装置・その子・上流のブリッジ）だけを評価する `n_pci_acpi_eval` と `n_pci_acpi_notify_take`（ABI 文書 §9）。`os_acpi.c` は nv-acpi.c の移植。claim の外（`_PSR`、電池、NPCF、NVIF／WMMX）は非対応のまま。2026-10-24 訂正：claim の範囲なら何でも評価できたのは誤り、通知のスレッドの止め方はデッドロックしえた（C59） |
| C55 | UVM（nvidia-uvm）は要らない | 却下 → 移植（実機未確認） | `kernel-open/nvidia-uvm` の MIT 部分を nvrm に入れ、RM の割り込みの順序（MMU の障害 → UVM → `rm_isr`）と P2P の排出を元どおりに。ページの貸し借りはカーネルの pager ABI（ABI 文書 §7）。pagertest を足した。2026-10-24 訂正：報告で「Nocturne 本体の ABI には何も足していない」と書いたのは誤り。pager ABI（`SYS_PAGER_CREATE`〜`SYS_PAGER_FAIL`、`N_KIND_PAGER`）はこの移植のためにカーネルへ足した（8acc1589）。また `ports/nvidia/uvm_os.h`（約 1500 行）は、UVM が書かれている Linux カーネルの語彙（`struct page`、`mm_struct`／`vm_area_struct`、`file_operations`、`kmem_cache`、待ち行列など）を nvrm の中で Nocturne の物へ当てる層で、元のコードを無改造で通すための模倣を nvrm の中に持っている。Nocturne 本体へ Linux の API は出していない。2026-10-24 監査：pager ABI の取消しと保護の迂回（C58）、`uvm_mm.c` で写像のファイルの参照が漏れていた（`vm_file` を外した後に `fput` していなかった。今は錠を外してから `fput`） |
| C56 | KAPI の `getFlipPendingStatus` でフリップの完了を待てる | 却下 → 修理済み（実機未確認） | 615.78.08 の `nvKmsKapiGetFunctionsTable` はこの項目を NULL のまま返す。b13cfa79 の待ちはそれに頼っていて、実際には待たずに次のフリップや絵の解放へ進み、表示中の VRAM を解放・再利用しえた。今は KAPI 自身の層ごとの完了通知（`nvkms_kapi_rm.c` の `nvrm_nvkms_flip_arm`／`_done`、`nvKmsResetNotifier`／`nvKmsParseNotifier`）を commit の直前に消し、後で読む。KAPI は通知を名指す前に消さないので、消さなければ二つ前のフリップの書込みが残って「完了」と読める。host 試験（消去の位置、失敗した commit の後は待たない）。実機 8 回目で表示エンジンが通知を書くと確かめた（BEGUN と時刻。完了の判定は C67 で直した） |
| C57 | デスクトップへ戻すフリップが失敗しても、絵を解放すれば画面は戻る | 却下 → 修理済み（実機未確認） | NVKMS は面の破棄で絵を head から外すので、head には何も出ない（黒）。`display.c` は `scr.lost` を立て、絵の解放の直後と画面の状態が変わるたびにデスクトップへ戻すフリップをやり直す（`recover`）。host 試験 |
| C58 | pager ABI の取消しと `n_memory_protect` は供給したページにも効く | 却下 → 修理済み | 供給元の view を `n_shared_revoke` しても枠は結ばれたまま残り、`n_memory_protect(READ)` の後もページャーの経路で書けた。今は各ページが供給元を持ち、その取消し（祖先を含む）で全写像から外す（`paged_revoke`、composite は供給元にならない）。保護は写像ごと・ページごとに覚え、後の障害や再供給でも超えない。共有の写像への実行権は与えない（別の写像から書けるページを実行できたため）。pagertest に試験を追加。QEMU の結果は §7 |
| C59 | ACPI の評価は claim の範囲なら何でもよく、通知のスレッドは RM の ACPI の初期化と終わりで開始・停止してよい | 却下 → 修理済み（実機未確認） | 範囲の中でも `_PS0`〜`_PS3`、`_ON_`、`_OFF`、`_RST`、`_EJ0`〜`_EJ4`、`_OSC`、`_INI`、`_REG`、`_PTS`、`_WAK` などシステムが装置と機械を動かす 28 の方法は、カーネルの電源管理と PCI の把握を裏から変える。今は `N_ERR_DENIED`（上流のブリッジは独自名もすべて）。通知のスレッドは `nv_acpi_methods_uninit`（RM の API と GPU の錠を持ったまま）で join していたので、`rm_acpi_notify` で錠を待つスレッドと互いに待ちえた。今は `nv_start_device` と同じく `rm_init_adapter` の後に開始し、`rm_request_dnotifier_state` を呼び、`rm_disable_adapter` の前に錠の外で止める。RM が扱わない値は渡さない（RM は表明で止まる）。pcitest に拒否の試験 |
| C60 | 直接表示の間も、パニック画面はカーネルの出力先へ描けば見える | 却下 → 修理済み（実機未確認） | 直接表示の間、表示エンジンはアプリの絵を出していて、カーネルが止まると戻す者がいない。カーネルはデスクトップの面にしか描かないので、パニックは画面に出なかった。新しい ABI `n_pci_screen_image`／`_end`（ABI 文書 §9）で、nvrm は絵の BAR1 の写しを登録し、カーネルはパニック画面を登録された面すべてへも描く（`fb.c` の `fb_present_last`、`N_SCANOUT_XBGR8888`）。登録できない絵は作らない。確認：host 試験（登録できないとき・BAR1 のひと続きでないときに絵を作らない）、QEMU の pcitest（登録・終わり・拒否）、QEMU で NMI によるパニックを目視（VGA の第二のバッファーを XBGR8888 の画像として登録して表示、パニック画面がそこに赤と青の入れ替わった色で描かれ、デスクトップの面には元の色で描かれる。§7） |
| C61 | 動的電源管理は移植しなくても、mode が NEVER なら RM は電源管理の状態に触れない | 却下 → 修理済み（実機未確認） | 実機 4 回目、`RmInitAdapter` と GSP の起動は成功したが、NVKMS の読込みの直後に nvrm がページフォールトで落ちた（`rip=0x708784` は `portSyncMutexAcquire`、呼出し元 `0x46ef16` は `RmUpdateGc6ConsoleRefCount`、記号は同じ材料を `-s` 無しで再リンクして引いた）。NVKMS が画面を取るときの VT 切替え（`NV0080_CTRL_OS_UNIX_VT_SWITCH`、SAVE_VT_STATE）は、mode を見ずに `console_device` が真なら電源管理の錠を取る。錠を作る `rm_init_dynamic_power_management` を移植が呼んでおらず、一方で `os_pci.c` が `console_device` を真にしていたので、NULL の錠を取った。修理：`nvrm.c` で上流の probe／remove と同じ位置に `rm_init_dynamic_power_management`（`rm_init_private_state` と `rm_init_event_locks` の後）、`rm_enable_dynamic_power_management`（probe の最後、割り込みと `rm_init_adapter` の前）、`rm_cleanup_dynamic_power_management`（NVKMS と `rm_shutdown_adapter` の後、`rm_free_private_state` の前）を呼ぶ。`console_device` は RM が起動画面（`nv_get_screen_info`）から決める（`os_pci.c` の設定を外した）。`nv_dynamic_power_available` は偽なので mode は NEVER のままで、GPU を止めることは無い。確認：host 試験、全体ビルド、記号付きの再リンクの逆アセンブルで呼出しの順を確かめた。QEMU には NVIDIA の GPU が無く、この経路は通らない |
| C62 | head を点けるモード設定も、層の完了通知でフリップの完了を待てる | 受理（0a1d812a の却下は誤り、C66 で待つ形へ戻した） | 監査（2026-10-25）：KAPI は面を層へ置くすべての commit に完了通知を名指す（`nvkms-kapi.c` の 3050〜3052 行は `surfaceChanged` か `bFromKmsSetMode` のとき `completionNotifier.specified`、3200〜3238 行で通知の面と番号を入れる。NVKMS は `nvkms-hw-flip.c` から `nvAssignCompletionNotifierEvoHwState` で表示エンジンへ渡す）。点けるモード設定も例外ではなく、3 秒で書かれなければ、その面は表示エンジンに届いていない。下の却下の根拠の一つは nvidia-drm（Linux のドライバー）で、参照は禁止（AGENTS.md）。0a1d812a は待たないことで、見えない画面を成功として扱い、5 回目のように起動画面へ戻る道を無くした（6・7 回目の暗いまま）。以下は当時の却下の記録：実機 5 回目、最初のモード設定の後に「the flip on head 0 has not completed after 3000 ms」、続いて「the mode set did not complete」と表示の持ち主の解放。NVKMS が起動画面を復元し、二台のモニター（HP の DP と Acer の HDMI）に同じ画面が出た。表示エンジンが層の完了通知を書くのは、commit の前に点いていた head の、面を出していた層だけ（nvidia-drm も `old_crtc_state->active && old_plane_state->fb` のときだけフリップのイベントを数える）。点けるモード設定には書かれない通知を待っていた。NVKMS のモード設定は同期で、戻った時点で済んでいる。修理：`display.c` は点いている head の面の差し替えだけを待ち、点けるモード設定・面を外す commit（失った head）・失敗した commit は待たない。待ったフリップが 3 秒で完了しなければ、以後はフリップを待たず直接表示もやめる（`scr.flips_unseen`、CREATE と PRESENT は `NV_ERR_NOT_SUPPORTED`、出ていた絵からはデスクトップへ）。host 試験の偽の KAPI に実機の規則を入れた（直す前の `display.c` はこの偽物で `nvrm_display_start` から失敗し、実機の症状を再現した） |
| C63 | Mesa・Zink・NVK が GPU を使わなかった理由はログで分かる | 却下 → 修理済み（実機未確認） | 実機 5 回目、nvrm は「serving clients」まで進んだが、WebGL は「Mesa softpipe」で、System.log に Mesa の行が一つも無かった。机上から起動したアプリには stderr が無く、Mesa の既定の出力先は stderr だけ。`vk_error*` は Vulkan のデバッグ記録が無いと何も出さず、nvkmd の拒否は無言、`nocturne_gl.c` の「ログ一行」は実際には無かった。修理：Mesa の記録の出力先にシステムの記録を足す（`util/log.c`、`MESA_LOG=system`）。`nocturne_gl.c` は GPU で描くか・使えない理由（`nvrm_connect_ready_status` の結果）・Zink が始まらなかったことを、変わったときだけ一行書く。nvkmd は `nnrm_open_device` が止まった段階（RM の制御・物体の名前）と NV_STATUS、または chipset と class を書く。host 試験（mesa_screen、nvk_rm、nvrm_transport） |
| C64 | `NV2080_CTRL_FB_INFO_INDEX_BAR1_SIZE` は MiB | 却下 → 修理済み | RM は KiB で返す（`mem_mgr_gm107.c` は BAR1 の VA 範囲の長さを 1024 で割る）。`nvkmd_nocturne_rm.c` は 1024×1024 を掛けていて、NVK の BAR の大きさが 1024 倍になっていた。RTX 4070 は Resizable BAR で BAR1 が VRAM より大きく、NVK は BAR を VRAM 以上なら別のヒープにしないので、実機では害が出なかったと見られる |
| C65 | nvrm が点けるモードは起動画面と同じ大きさでよく、利用者がモードを選ぶ経路は要らない | 却下 → 修理済み（実機未確認） | 実機 6 回目、nvrm は起動画面の大きさの 1920×1080 から 59.940 Hz を選んだ（X27U Z1 の推奨は 2560×1440 59.951 Hz、31 モード）。カーネルはデスクトップを起動時の大きさに固定し、設定アプリの「Resolution」は何もしなかった。モードを決めるのは OS と利用者、列挙・検証・適用はドライバー（Windows-driver-samples で役割の境界を確かめた）。修理：カーネルにモニターのモードの持ち主（`kernel/src/gui/screenmode.c`）。ドライバーは見えるモードを出し（`n_pci_screen_modes`、推奨に `N_SCREEN_MODE_PREFERRED`）、システムは、そのモニターに保存された利用者の選択、無ければ推奨のモードを望む（`n_pci_screen_state` の `mode_serial`・`mode`）。ドライバーは点けて結果を返し（`done`・`shown`）、点かなかったモードは断られて次の候補が来る。設定アプリの試用（`SCREEN_MODE_SET`）は 15 秒で前のモードへ戻り、保持すればそのモニターの選択として保存される。デスクトップはスキャンアウトの大きさへ追従する（`wm_screen_resized`、`fb.c` はスキャンアウトごとの大きさ）。`display.c` は望まれたモードの大きさの面を作り、NVKMS の検査 → 面の受け渡し → commit → 古い面の解放の順で、commit が失敗すれば前の面（無ければ起動画面）へ戻す。確認：host 試験（偽の EDID と 8 モード、推奨、起動時の利用者の選択、失敗時に何も渡さない、モニターの出入り、直接表示）、QEMU の pcitest（Bochs VGA に 4 モード：推奨、試用が 15 秒で戻る、すぐ戻す、保持、取下げと再提示、他のモニターの選択、点かなかったモードの拒否）と、デスクトップが 1024×768 へ変わって戻ること、設定アプリの試用のカードを目視。実機で 2560×1440 が点くか、6 回目の暗い画面の原因は未確認 |
| C66 | 面が表示に見えなくても、直接表示をやめれば利用者はデスクトップを見られる | 却下 → 修理済み（実機未確認） | 実機 6・7 回目はモニターが暗いまま戻らず、利用者に戻る手段が無かった（C65 の監査の残り「最初のモードが暗いときの逃げ道が無い」）。修理：`display.c` の `hand_back` が「表示を返す」唯一の持ち主になった。順序は、望まれたモードの取下げ、起動画面へ戻す（`n_pci_give_screen` の `N_SCANOUT_BOOT`）、垂直帰線を止める、NVKMS の持ち主を手放す（NVKMS は自分のコンソールを復元する）、head の状態を戻す、直接表示と面の解放。点けるモード設定を含め、待ったフリップが 3 秒で完了しなければ、完了通知の生の語（状態と時刻）、通知の番号、commit にかかった時間を書いて返す（`nvrm_nvkms_flip_words`）。以後 nvrm は自分の面を出さない（PRESENT・CREATE・IMPORT は `NV_ERR_NOT_SUPPORTED`、モードの追従と再走査もしない）。通知は完了を示すのにモニターに何も映らないときは、利用者が Ctrl+Alt+Esc を押す。カーネルは画面を描かせているドライバーへ起動画面を求め（`struct n_screen_state` の `take_back`、ABI 文書 §9、`wm.c`）、nvrm の状態スレッドはそれを見て `hand_back` する。起動画面が戻れば求めは終わる（`pciuser.c` の `screen_give_back` → `wm_screen_given_back`）。ドライバーが画面を保持していれば従来どおりアプリを止め、どちらでもなければメニューを出す。確認：§7 の 2026-10-25（8）。実機で 3 秒の待ちの後に起動画面かコンソールが見えるか、通知の語の値は未確認 |
| C67 | 層の完了通知は、表示エンジンがフリップを終えると「finished」になる | 却下 → 修理済み（実機 9 回目で確認） | 実機 8 回目：点けるモード設定の 3 秒後に「has not completed」、待っていた枠 1 の語は `40000000 00000000 68bd73a0 18e1aa5b`。語 0 の 31:30 が 1 で BEGUN、語 2・3 は表示エンジンが書いた時刻（`nvKmsResetNotifier` の初期値は語 2＝1・語 3＝0 で、枠 0 はそのまま）。window チャンネルの完了通知は、フリップを実行すると NOT_BEGUN から BEGUN に変わり、そこで止まる。NVKMS 自身もそれで完了と判定する（`nvkms-headsurface.c` の `IsPreviousFlipDone`、`HsUpdateClientNotifier`）。FINISHED まで進むのは core チャンネルのモード設定の通知だけ（`nvkms-evo3.c` の `nvEvoWaitForCompNotifierC3`）。`nvrm_nvkms_flip_done` は FINISHED を待っていたので、済んだフリップを毎回「見えない」として起動画面へ返していた（5・8 回目）。修理：commit の前に NOT_BEGUN へ消した枠が NOT_BEGUN 以外なら、そのフリップは実行済み（`nvkms_kapi_rm.c`）。モード設定の後の最初のフリップと、状態が変わったときだけ、状態と時刻を一行書く。host 試験の偽の KAPI を実機の語（初期値と 8 回目の時刻）に合わせた。C56・C62・C66 の「完了」はこの意味に読み替える |
| C68 | モード設定の要求は、触れない項目を 0 にしておけば NVKMS の規定値になる | 却下 → 修理済み（実機 9 回目で確認） | 出力 LUT を持つ装置（Turing 以降、Ada を含む。`nvkms-evo3.c` の `FillLUTCaps`）では、KAPI はモード設定のたびに `headModeSetConfig->olutFpNormScale` を無条件に送る（`nvkms-kapi.c` 3381〜3399 行）。NVKMS は HDR でなければその値のまま `HEAD_SET_OLUT_FP_NORM_SCALE` へ書く（`nvSetupOutputLUT5`、`EvoSetOutputLutC5`）。`display.c` の `head_request` は memset の 0 のまま送っていた。規定値は `NVKMS_OLUT_FP_NORM_SCALE_DEFAULT`（0xffffffff、約 1.0）で、NVKMS が自分のコンソールを戻すときはこれを使う（hand_back の後は映った 5 回目と合う）。0 では全画素が黒へ縮み、信号はあるのに暗い（6・7 回目の症状の第一候補。ハードウェアの文書では未確認）。修理：`head_request` で規定値を入れる（head を消す要求も通る）。モード設定の後と表示を返すときに、head の状態（モード、倍率、垂直帰線の数、KAPI `getCRC32` の合成後・ラスター・出力の CRC）を一行書く。host 試験 |
| C69 | RM の CPU 番号は、いま走っている CPU を返せばよい（C18 の補い） | 却下 → 修理済み（実機 9 回目で確認） | 実機 7・8 回目に `pThreadNode->cpuNum == osGetCurrentProcessorNumber() @ thread_state.c:962`。`threadStateInitISRLockless` は開始時の CPU の枠に節を入れ、`threadStateFreeISRLockless` は終わりの CPU と同じことを表明する。カーネルの割込み処理は CPU を移らないが、nvrm の割込みスレッドは移りうる。修理：`irq_run_rm` の間（`nvrm_isr_begin`／`_end`、`os.c`）は `os_get_cpu_number` が開始時の番号を返す。その枠を埋めるのは割込みスレッド一本だけで、他のスレッドは節のスレッドを照合してから使う。あわせて、割込みの待ちが 50 ms で時間切れになっても `rm_isr` を走らせ（MSI の端を一度失っても RM が止まらない）、5 秒ごとの届いた割込み・RM が処理した数・届かずに見つかった数・垂直帰線の数を、どれが動いているかが変わったときだけ書く（`nvrm.c`） |
| C70 | NVK の装置は `NV_DEVICE_ALLOCATION_FLAGS_RESTRICT_RESERVED_VALIMITS` で RM の VA を決めた範囲へ閉じ込められ、NVK のヒープは [1 MiB, 256 GiB) でよい | 却下 → 修理済み（実機 11 回目で確認） | 実機 9 回目、Zink を起こすたびに `rpcRmApiAlloc_GSP: GspRmAlloc failed … hClass=0x00000080 … status=0x0000001f` と `NVK: … NV01_DEVICE_0 failed: NV_ERR_INVALID_ARGUMENT`（120 Hz への変更とは無関係。8 回目も同じ場所で落ちていたが理由が出ていなかった）。GSP 構成の CPU 側 RM は装置を GSP にも作らせ、その RPC（`NV_RM_RPC_ALLOC_SHARE_DEVICE_FWCLIENT`）は旗を送るが範囲を送らない。GSP の `deviceConstruct` は旗があって範囲が 0 なら断る。旗を外すだけでは足りない：GSP 構成では各 VA 空間の [4 GiB, 4.5 GiB) が GSP-RM の範囲で（`gvaspaceReserveSplitVaSpace`）、CPU 側 RM はチャネルの文脈バッファーを 4 GiB から上の最初の空きへ写す（`dmaAllocMapping_GM107`）。RM の注釈どおり RM の場所は [4 GiB, 8 GiB)（CUDA の約束）。修理：装置は旗なし（UVM・NVKMS と同じ）、NVK のヒープは 8 GiB から（`NNRM_VA_START`、`nvkmd_nocturne_rm.h`）。host 試験の偽の RM に GSP の規則（装置の旗、GSP の範囲、文脈バッファーの写像）を入れた。直す前の `nvkmd_nocturne_rm.c` はこの偽物で NV01_DEVICE_0 から失敗し、実機の症状を再現した |
| C71 | エンジンのクラスは装置のクラス一覧から下位バイトで選べる | 却下 → 修理済み（実機 11 回目で確認） | 監査（2026-10-25）：`NV0080_CTRL_CMD_GPU_GET_CLASSLIST_V2` はエンジンを持たない MIG のクラス（`AMPERE_SMC_CONFIG_SESSION` 0xc639、`AMPERE_SMC_MONITOR_SESSION` 0xc640）も含み、M2MF の 0x40 で選ぶと 0xc640 を拾う。M2MF の物体を頼むチャネルは作れず、NVK の `cls_m2mf` も誤る。修理：グラフィックスとコピーのクラスは、そのエンジンの一覧（`NV2080_CTRL_CMD_GPU_GET_ENGINE_CLASSLIST`、CPU 側 RM の表で答える。一覧は引数の中のポインターで書かれるので libnvrm の引数の領域に置く）から選ぶ。GPFIFO と USERMODE は装置の一覧のまま。host 試験（偽の RM はチャネルの下にエンジンのクラスしか作らせない） |
| C72 | GP の環の空きは USERD の GP_GET で数えられる | 却下 → 修理済み（実機 11 回目で確認） | 監査（2026-10-25）：Host が USERD を書き戻すかはチャネルの RAMFC の `USERD_WRITEBACK` で、RM が GSP の中で決める（CPU 側に呼出しは見えない）。dev_ram.ref（open-gpu-doc、GA100）は、書き戻しは止められるのでセマフォーで進みを追うよう勧めている。書き戻されなければ約 340 回目の flush で 10 秒待って装置を失う。修理：環の空きは実行文脈がフェンスで数える（flush ごとの GP の終わりを持ち、足りなければ古い flush のフェンスを待つ、`nvkmd_nocturne_ctx.c`）。`nnrm_chan_submit` は書いて鳴らすだけ。flush は環に入る大きさに分け、不完全な命令の連なりは切らない（上流の nouveau 版と同じ）。host 試験 `nvk_ctx`（偽の GPU は CPU が待つ間だけ flush を終え、GP_GET は書かない） |
| C73 | NVK と Vulkan の実行時は、物体ごとに libc の遅延メンバーにしてよい | 却下 → 修理済み（実機 11 回目で確認） | 実機 10 回目、WebGL を使うページを開くたびにブラウザーが `rip=0` で落ちた（3 回、スタックの戻り番地 `0x152af0b`）。記号付きで同じ配置に再リンクして引くと `wsi_device_init` の中で、呼んだ `vk_tramp_GetPhysicalDeviceProperties2` が物理装置の入口の表の NULL へ跳んでいた。入口の表は実装を弱い参照で名指しし（上流の生成器、入れない入口は NULL）、弱い参照は遅延メンバーを引き込まない。物体はあるのにリンクされなかった実装が 51 個（`vk_common_GetPhysicalDeviceProperties2`・`CmdCopyBuffer`・`CreatePipelineLayout`・`CreateFramebuffer`・`CreateDescriptorUpdateTemplate` など、NVK の video・生成命令・host copy）。C70 で装置が作れるようになって初めてここまで来た。host 試験はソースを一つずつ組むので、製品のリンクの形を見ていなかった。修理：実行時・WSI・NVK を `ld -r` で一つの物体（`vulkan_nouveau.o`）にして一つの遅延メンバーにする（上流の共有ライブラリーと同じ丸ごとの単位）。`scripts/link_unit.py` は単位から出る弱い参照を他のメンバーが定義していればビルドを止める（実行時の物体を一つ外して試し、止まることを確かめた）。単位に入って初めて分かった MME の試験プログラム（`test_nvk_mme.c`、`main` を持つ）を NVK の一覧から外した。直した後のブラウザーで、物体があるのにリンクされない弱い参照は 0 個 |
| C74 | アプリは自分の 16 ms の時計で描けば、どのリフレッシュでも表示に揃う | 却下 → 修理済み（実機 11 回目で確認） | 実機 10 回目のユーザーの観察：3D Cube は 60 Hz では 60 fps で安定、120・280 Hz では 55〜60 fps で揺れる。System.log では 120・280 Hz でも合成は垂直帰線で歩調を取っていた（「frames paced by the display driver's vertical blank」の後に「no vertical blank」の行が無い）。cube は 16 ms 眠っては描くだけで、表示の拍を受け取る道が無かった。60 Hz では提出が次の垂直帰線まで待たされて位相が揃い、偶然 60 に見える。120・280 Hz では眠りに合成の分の待ちが足されて 17・18 ms になる。修理：窓を `WIN_FRAME_EVENTS` で開くと、提出を取り込んだ合成の後に `EV_FRAME`（key と x は 0、何度提出しても一つ、表示されていない窓には表示された時）が届く（`abi.h`、`wm.c`）。`ui.c` はすべての窓をこれで開き、面が変わった後の分だけを `UI_EVENT_FRAME` にする。cube はその出来事で次の絵を描き、回転は経過時間で進める（Hz で回る速さが変わらない）。Go の ui は受けて捨てる。QEMU（モード 74.998 Hz、垂直帰線なし）で cube は約 75 fps、最小化と復元で再開、Space で形が変わる。ブラウザーの rAF はまだ 16 ms の時計（全画面の直接表示の間だけ表示に従う） |
| C51 | claim の後の GPU の DMA は全部 RM のもの | 未確定（観測のみ） | 実機 3 回目、BME を入れた後に `0x2365440` から 32 バイト刻みの書込みと `0x235b000` の読みが IOMMU で遮断された（domain 2、RM の失敗の後も続いた）。RM の DMA オブジェクトではない。UEFI の GOP などが前に設定したエンジンの残りと推測。IOMMU が無ければこの物理メモリに書かれていた。RM が初期化の中でエンジンをリセットした後に止まるかを 4 回目で見る。実機 4 回目：BME を入れてから nvrm が落ちて解放されるまで（`RmInitAdapter` の 5.1 秒を含む）、IOMMU の遮断の行は一つも無かった |

## 5. 担当と受け渡し

- 主担当（Opus）：設計、カーネル ABI、init、nvrm の骨格、libnvrm、統合、ビルド、QEMU、コミット。
- サブ A（Sonnet）：Rust std の移植（`ports/rust/`）。
- サブ B（Sonnet）：RM の取り込みとビルド（`third_party/nvidia-open/`、`ports/nvidia/`）。
- サブ C（Sonnet）：Mesa の NVK 側（P6 の規約ができてから）。
- サブの報告は受理前に主担当が差分とビルドで確かめる。

## 6. 次の一手

1. P10（ユーザー）：11 回目で Zink＋NVK が RTX 4070 で描いた（WebGL City 54 fps、3D Cube 280 Hz で 279 fps）。
   次の実機では、全画面の WebGL が直接表示になるか（`wm:` の行、C56・C57・C66）、窓を閉じて開き直せるか、
   長く動かしても止まらないか（GP の環とフェンス、C72）を見る。
2. 実機の結果で C15・C21〜C23・C25・C26・C28 を受理か却下へ。失敗した境界から直す。
3. 性能（C9・C20・C25）：同じ WebGL を softpipe と Zink で比べる。全画面は直接表示（D11）になるか（ログの `wm:` の行）、窓の中の読み戻しとポーリングが重ければ直す。
4. 残り：大きいページ、VRAM 使用量の報告以外の kmd 機能（dma-buf は無し）、video は対象外。

## 7. 記録（新しい順）

- 2026-10-25（11）：実機 11 回目（ユーザー、USB 起動、c31dddde の媒体、写真 3 枚と `build/qa/realhw11/`）。
  System.log に「MESA: info: Nocturne GL: drawing on the GPU with zink Vulkan 1.4(NVIDIA GeForce RTX 4070 (NVK AD10x)
  (MESA_NVK))」が 4 回、落ちた行・NVK と ZINK の誤りの行は一つも無く、最後は利用者の電源断で終わった。webglreport の
  WebGL 1・2 とも Unmasked Renderer が同じ値（WebGL 2 の関数は 88 個すべて）。webglsamples の WebGL City は 54 fps
  （softpipe の頃の実機は 4 fps）で描き続け、チャネル・投入・GP の環（C71・C72）も通った。3D Cube（窓の名前は
  「3D Shapes」）は 280 Hz で 279 fps（C74）。設定アプリの 280 Hz の試用と保持、垂直帰線による歩調、nvrm.log に誤りの
  行なし。Zink は一度「PERF WARNING! > 100 copy boxes」を出した（一回の複写に 100 を超える矩形）。残り：GL Version と Shading Language Version は `js_webgl.c` の固定の文字列で「Mesa softpipe」のまま
  （実際の描画器と食い違う）。WebGL City の 54 fps は rAF の 16 ms の時計と窓への読み戻しの分で、表示の Hz には
  揃っていない。

- 2026-10-25（10）：実機 10 回目（ユーザー、USB 起動、5af24747 の作業ツリー）。ユーザーの観察：WebGL を使うサイトを
  開くとブラウザーが落ちる。3D Cube は 60 Hz では 60 fps で安定、120・280 Hz では 55〜60 fps で揺れる。
  `build/qa/realhw10/`（9 回目と同じく管理者の PowerShell と store.exe で取り出した）：X27U Z1 に 2560×1440
  59.951 Hz、設定アプリで 280.000 Hz・120.000 Hz・59.951 Hz の試用・保持・戻るに誤りの行は無く、どの Hz でも合成は
  垂直帰線で歩調を取っていた。`rpcRmApi*_GSP` の誤りは消えた（C70）。ブラウザーは `MESA:`・`ZINK:`・`NVK:` の行を
  一つも書かずに 3 回 `rip=0` で落ちた。同じ配置で記号付きに再リンクして戻り番地を引き、入口の表の NULL と分かった
  （C73）。fps は表示の拍がアプリへ届いていなかった（C74）。確認：host 試験 5 つ（nvk_sync、nvk_ctx、mesa_screen、
  nvk_rm、zink_start）0 件失敗、全体ビルド（img・vhd・vhdx・iso）、起動ディスクの browser・browserjsworker・cube が
  `build/programs` と同じ sha256、QEMU（WHPX、4 コア、4096 MiB、1920×1080、AHCI、NVIDIA 無し）で机上、3D Cube
  約 75 fps（モード 74.998 Hz）、最小化と復元、ブラウザーの get.webgl.org の立方体が softpipe で落ちずに回る
  （1 秒の差で回転を確かめた）、戻る、ホイールで下端と先頭を目視（`build/qa/zink11/`）。実機の Zink と、実機の
  120・280 Hz での fps は 11 回目で確かめる。

- 2026-10-25（9）：実機 9 回目（ユーザー、USB 起動、6e912418 の作業ツリー）。ユーザーの観察：設定アプリで 60 Hz から
  120 Hz に変えた後に ZINK と `rpcRmApiAlloc_GSP` の誤りが出た。`build/qa/realhw9/`（USB の独自のファイルシステムから
  管理者の PowerShell と store.exe で取り出した）：X27U Z1（DP-1）に 2560×1440 59.951 Hz、試用と保持で 120.000 Hz
  （「the user keeps 2560x1440 at 120.000 Hz」、垂直帰線による歩調、CRC の行あり）。その後、ブラウザーが GL を
  起こすたびに `GspRmAlloc failed … hClass=0x00000080 … status=0x0000001f` と
  「NVK: GPU 01:00.0: NV01_DEVICE_0 failed: NV_ERR_INVALID_ARGUMENT」「ZINK: vkEnumeratePhysicalDevices failed」
  「Nocturne GL: … drawing with softpipe」（5 回）。120 Hz との順序は偶然で、原因は NVK の装置の確保の旗（C70）。
  直す前の `nvkmd_nocturne_rm.c` を新しい host 試験で走らせると同じ NV01_DEVICE_0 で止まった。装置より先の経路は
  実機で一度も通っていないので、監査（サブエージェント、模型とコードの場所だけを渡した）で、チャネル・メモリー・写像・
  特権・投入の順序を調べ、止まりうる二つを同じ回に直した（C71、C72）。確認：host 試験 5 つ（nvk_sync、nvk_ctx、
  mesa_screen、nvk_rm、zink_start）0 件失敗、全体ビルド（img・vhd・vhdx・iso）、起動ディスクの browser と nvrm が
  `build/programs` と同じ sha256、QEMU（WHPX、4 コア、4096 MiB、1920×1080、AHCI、NVIDIA 無し）で机上、ブラウザーの
  about:home、get.webgl.org の WebGL の立方体が softpipe で回る（1 秒後の画像と比べて回転を確かめた）、戻る、ホイールで
  下端と先頭、3D Cube 62 fps を目視（`build/qa/zink10/`）。実機の Zink（装置の確保より先の経路）は 10 回目で確かめる。

- 2026-10-25（8）：実機 7 回目（ユーザー、USB 起動、2 回）。ユーザーの観察：C65 の修理の後も何も変わらず、
  モニターは暗いまま。`build/qa/realhw7/` の System.log と nvrm.log：1 回目は X27U Z1（DP-1、0x800）が
  ファームウェアで点灯し、「30 modes offered」「the desktop is shown on display 0x00000800 (head 0) at 2560x1440
  at 59.951 Hz」まで誤りの行は無かった。UVM の開始の後に「display 0x00000800 is disconnected」となり、KG271U X
  （HDMI-3、0x100）へ移って同じ 2560×1440 59.951 Hz を点けた。RM の起動中に
  `pThreadNode->cpuNum == osGetCurrentProcessorNumber() @ thread_state.c:962` の表明が一度出た（表示とは別の件、
  未調査）。2 回目は HP X27q（DP-2、0x200）で 2560×1440 が点いたことになり、UVM の開始の後は何も無い。
  ユーザーの指示で、0a1d812a が nvrm と CPU 描画の両方をモニターへつなぎ直せなくしたと仮定して監査した
  （サブエージェント、0a1d812a の差分と KAPI・NVKMS の該当箇所）。結果：C62 の却下は誤りだった。KAPI は
  点けるモード設定にも完了通知を名指し、NVKMS はそれを表示エンジンへ渡す。0a1d812a は書かれない通知を
  待たなくなったのではなく、表示エンジンに届いていない面を「点いた」として扱うようになった。5 回目は 3 秒の
  待ちの後に持ち主を手放し、NVKMS の復元した起動画面が見えた。6・7 回目はその道が無く、暗いまま残った。
  修理（C66）：点けるモード設定のフリップも再び待つ。3 秒で完了しなければ通知の生の語を書き、`hand_back` で
  表示を起動画面と NVKMS のコンソールへ返す（以後 nvrm は自分の面を出さない）。完了したのに映らないときは、
  利用者が Ctrl+Alt+Esc で返させる（ABI の `take_back`、`wm.c`、`pciuser.c`）。
  残り：通知が完了を示しても映らない場合の原因（DP のリンク、出力の設定、BAR1 への書込みの見え方）は未確定。
  1 回目の切断は、点けたモードを X27U Z1 が受けられずにリンクが落ちたとも読める。
  確認：host 試験（nvrm_display_hosttest：点けるモード設定の通知が来なければ返す、Ctrl+Alt+Esc で返す、
  返した後は CREATE・PRESENT・IMPORT を断る。nvk_sync_hosttest.py の 3 本、nvrm_dma_hosttest.py の全件）。
  `tests/pci_hosttest.c` は今回の前から `pci.c` の API と合わずビルドできない（`pci_resume` の引数、
  `struct pci_reset` の `issued`、`FLR_READY_MS`）。全体ビルド（IMG・VHD・VHDX・ISO）、試験媒体の
  カーネルとシステムのプログラムがビルドと一致、SDK の `abi.h` に `take_back`。QEMU（WHPX、4 コア、4096 MiB、
  1920×1080、AHCI の snapshot、`-vga std`・`-device edu`、`appdrivers=any appdrivers-unisolated`）で
  `pcitest` 0 failed、`pcitest escape=90` 0 failed（HMP の `sendkey ctrl-alt-esc` で System.log に
  「wm: Ctrl+Alt+Esc: the display driver is asked to give the boot screen back」、take_back が立ち、起動画面へ
  返した後の受け渡しでは消える。押した前後の画面はデスクトップのままでメニューは開かない）。ドライバーが
  画面を描かせていないときの Ctrl+Alt+Esc はメニューを開き、Esc で閉じた。マウスの二度押しで設定アプリの
  Display を開いた（ファームウェアの 1920×1080）。panic・ASSERT の行は無い。画像は `build/qa/escape/`。
  QEMU には NVIDIA の GPU が無く、C66 の nvrm 側の経路は host 試験だけ。
- 2026-10-25（7）：実機 6 回目（ユーザー、USB 起動、2 回）。ユーザーの観察：主モニター（Acer X27U Z1、QD-OLED、
  DP、2560×1440 280 Hz、VRR 48〜280 Hz、Windows では 10 ビット）は暗くなったまま戻らなかった。2 回とも
  選ばれたモードは 1920×1080 59.94 Hz。`build/qa/realhw6/` の System.log と nvrm.log：GSP の起動、NVKMS の
  読込み、7 コネクターの列挙（HDMI の Acer KG271U X、DP の HP X27q、DP-1 の X27U Z1 がファームウェアで点灯）、
  「mode 1920x1080 at 59.940 Hz (31 modes, 6 of 1920x1080; preferred 2560x1440 at 59.951 Hz)」、面の確保、
  「the desktop is drawn into nvrm's surface, shown on display 0x00000800」、「serving clients」、垂直帰線による
  歩調まで、誤りの行は一つも無かった。暗い原因はログからは分からない（DP のリンク、VRR、10 ビット、OLED の
  モニター側の都合の仮説はどれも未確認）。
  修理（C65）：モードを決めるのはカーネルと利用者、ドライバーは列挙・検証・適用。カーネルのモードの持ち主
  （`screenmode.c`）、ABI（`n_pci_screen_modes`、`n_pci_screen_state` の `mode_serial`・`mode`、
  `SCREEN_MODES_GET`・`SCREEN_MODE_SET`）、デスクトップの大きさの追従、`display.c` の望まれたモードの表示、
  設定アプリの解像度とリフレッシュレートの一覧・15 秒の試用・モニターごとの保存。既定はモニターの推奨
  （X27U Z1 なら 2560×1440 59.951 Hz）で、280 Hz は設定アプリで選ぶ。合わせて、ファームウェアの画面の
  合成の歩調が ms への切り上げで 120 Hz が約 111 Hz になっていたのを直し（周期の端数を次の開始時刻へ持ち越す）、
  設定アプリの「Desktop frame rate」の説明をモニターのリフレッシュレートと分けた。
  調べもの：Windows-driver-samples（MS-PL、読むだけ、コードは使っていない）と NVKMS で役割の境界を確かめた
  （WDDM の KMDOD・IddCx は OS がモードを選び、ドライバーは列挙・検証・適用。NVKMS も候補と可否だけで、
  自分で選ぶのは所有者がいないときのコンソール復元だけ）。KAPI の preferredMode は EDID の DTD1 だけを指し、
  DisplayID・CTA の推奨は見ない。カーネルの描画・表示・窓管理の監査の残り：消灯・DPMS・OLED の焼付き対策が
  無い、一台のモニターだけ、HDR・10 ビットの面が無い、DPI の倍率が無い、合成は表示中の面へ直接書く（テアリング）、
  GPU の合成は virtio だけ、CURSOR_MOVE が使われない、直接表示中の SCREEN_GRAB、最初のモードが暗いときの
  逃げ道が無い（試用は利用者が試したモードだけ）、OS DEV INFO に EDID・DisplayID・DP・CTA-861 の仕様書が無い。
  確認：host 試験（nvrm_display_hosttest）、全体ビルド（IMG・VHD・VHDX・ISO）。QEMU（WHPX、4 コア、4096 MiB、
  1920×1080、AHCI、`-vga std`・`-device edu`）で `pcitest` 0 failed・`pcitest screen` 0 failed、デスクトップが
  1024×768 になって戻ること、設定アプリの試用のカード（残り秒・Keep・Go back）とモードの一覧を目視。
  合成の歩調：`wmtrace` で 3D Cube（約 62 fps）を出したまま設定アプリで 30 Hz にし、20.004 秒で 600 フレーム
  （ms への切り上げでは 34 ms で約 588）。120 Hz へ戻し、ブラウザーで get.webgl.org の回る立方体を見た。
  実機では未確認。

- 2026-10-24（6）：実機 5 回目（ユーザー、USB 起動）。ユーザーの観察：二台目のモニターに主モニターと同じ画面が出た、
  WebGL は「Mesa softpipe」、3D Cube はいつもの 63 fps。USB の C: から `build/host/store.exe` で System.log と
  nvrm.log を `build/qa/realhw5/` へ取り出した。結果：C61 の修理で nvrm は落ちず、NVKMS の読込み、ディスプレイの
  列挙（HP X27q の DP が起動時に点灯、Acer KG271U の HDMI が接続）、1920×1080 59.940 Hz の面の確保まで進み、
  UVM の開始と「serving clients on nocturne.nvidia.rm.0100」に至った。しかし最初のモード設定の後で書かれない
  フリップの完了通知を 3 秒待ち、表示の持ち主を手放した（C62）。二台のモニターの同じ画面は、NVKMS が持ち主の解放で
  起動画面を復元した結果と見られる。WebGL が softpipe だった理由は、Mesa・Zink・NVK の行が System.log に一つも
  無く分からなかった（C63）。3D Cube は CPU で描く（`gfx_triangle`、16 ms の歩調）アプリで、GPU とは関係なく
  63 fps のまま。その後の「handle 3 is not a PCI right (kind 8)」は二つ目の nvrm（PCI の権利なしで起動されたもの）で、
  一つ目には影響しない。
  修理：C62（点いている head の面の差し替えだけを待つ、見えなかったら以後は待たず直接表示をやめる）、C63（Mesa の
  記録をシステムの記録へ、GL・nvkmd が GPU を使わなかった理由を書く、libnvrm の `nvrm_connect_ready_status`）、
  C64（BAR1 の大きさの単位）。
  確認：host 試験（nvrm_display_hosttest、nvk_sync_hosttest.py の 3 本、nvrm_dma_hosttest.py の全件）。全体ビルド
  （IMG・VHD・VHDX・ISO）は成功し、媒体の nvrm・browser・browserjsworker はビルドの成果物と一致した。QEMU（WHPX、
  4 コア、4096 MiB、AHCI）でデスクトップまで起動し、ブラウザーで get.webgl.org を開いて「Your browser supports
  WebGL」と回る立方体（softpipe）を見た。NVIDIA の GPU が無いので serial.log に `MESA:` の行は無い（nvrm が GPU を
  出さないときは無言の設計どおり）。QEMU には NVIDIA の GPU が無く、C62・C63 の実機の経路は QEMU では通らない。
- 2026-10-24（5）：実機 4 回目（ユーザー、USB 起動、IOMMU 有効）。画面は固まり続けなくなった。USB の C: から
  `build/host/store.exe`（管理者、読むだけの `list`・`get`・`check`）で System.log と nvrm.log を
  `build/qa/realhw4/` へ取り出した（`check` は 0 problems）。結果：GSP の起動と `RmInitAdapter` が初めて成功した
  （5136 ms、「GPU started: NVIDIA GeForce RTX 4070」、VBIOS 95.04.3e.c0.82）。続く NVKMS の読込みの直後、
  VT 切替えの `RmUpdateGc6ConsoleRefCount` が NULL の電源管理の錠を取って nvrm が落ち、カーネルが claim を
  解放してソフトウェア描画へ戻った（C61）。BME の後の IOMMU の遮断は今回は無し（C51）。
  修理：動的電源管理の初期化・有効化・後始末を上流の位置で呼び、`console_device` を RM に決めさせる（C61）。
  上流 kernel-open が呼ぶ RM の入口のうち移植が呼ばないものも洗い直した。残りは Tegra、NUMA、vGPU、
  suspend、BAR の防火壁（GB100 系だけ）、GSP の客では何もしない `rm_isr_bh_unlocked`、NVML 向けの GPU 追加の通知など、
  RTX 4070 の起動で RM が状態を求めないもの。
  確認：host 試験（nvrm_dma_hosttest.py の全件）、全体ビルド（IMG・VHD・VHDX・ISO）、媒体の nvrm がビルドと
  一致、記号付きの再リンク（`build/qa/realhw5/nvrm.sym`、strip すると媒体と同じ配置）の逆アセンブルで呼出しの順。
  QEMU には NVIDIA の GPU が無く、nvrm はこの経路を通らないので QEMU では確かめていない。実機（5 回目）で NVKMS が
  VT 切替えを越えて画面を GPU の面へ移すか（`NVRM display:`・`wm:` の行）を見る。
- 2026-10-24（4）：8acc1589 と b13cfa79 の監査（実機を傷めうるもの、論理が誤っているもの）と修理。致命的 1 件：
  フリップの完了の待ちが NULL の関数に頼り、待っていなかった（C56）。重大 7 件：デスクトップへ戻すフリップの失敗で
  黒いまま（C57）、pager ABI の取消しの迂回と保護の迂回（C58）、ACPI の方法の無制限な評価と通知のスレッドの
  デッドロック（C59）、UVM の写像のファイルの参照の漏れ（C55）、直接表示の間のパニック画面（C60）。
  文書の訂正：README（フリップの完了、DESTROY、UVM の取り込みは条件の追加だけでヘッダーの読替えを含むこと、
  ACPI、パニック画面の登録）、ABI 文書 §7（共有の写像への保護）・§9（XBGR8888、`n_pci_screen_image`、
  起動画面へ「二度と書かない」は誤りで `N_SCANOUT_BOOT` と claim の終わりで戻る、ACPI の拒否）、`nocturne.h`。
  下の（2）の訂正：「`kernel-open` の GPL でない部分をすべて移植」は誤り。動的電源管理（`nv-dynamic-power`）、
  dma-buf の書出し、suspend／resume、`os-mlock` などは移植していない。「元の起動順序」は
  `rm_get_gpu_uuid_raw`・`rm_set_rm_firmware_requested` の位置を上流に合わせたという意味で、起動全体の順序では
  なかった（電源管理の段は無く、ACPI の通知の開始は上流と違う位置で、RM の錠の中だった。今回 C59 で上流の位置へ）。
  確認：host 試験（nvrm_dma_hosttest.py の全件。nvrm_transport_hosttest は b13cfa79 以来
  `nvrm_display_scanout` の偽物が無くリンクできなかったのも直した）、全体ビルド（IMG・VHD・VHDX・ISO）、
  QEMU（WHPX、4 コア、4096MiB、AHCI 起動、edu と `-vga std`）の pc と q35 で pcitest 0 失敗・
  `pcitest screen=10` 0 失敗・pagertest 102 検査 0 失敗。パニック画面：`pcitest screen=40` で第二のバッファーの
  画像を出している間に HMP の `nmi` でパニックを起こし、画面（第二のバッファー）に赤と青の入れ替わった
  パニック画面、デスクトップの面（起動画面の位置）に元の色のパニック画面が描かれたことを目視と HMP の `xp` で確認。
  WHPX の既定（APIC をハイパーバイザー側に置く）では HMP の `nmi` がゲストへ届かず何も起きなかったので、
  この確認だけ `-accel whpx,kernel-irqchip=off` で行った。実機（RTX 4070）ではどれも未確認：フリップの完了の通知、
  戻しのやり直し、BAR1 の絵へのパニック画面、ACPI の通知（0x81・0xC0・0xD1〜0xD5）。
- 2026-10-24（3）：全画面の直接表示（D11・C20）。カーネル：`N_SCANOUT_DIRECT`、`win_direct`、`EV_DIRECT`、`WIN_STATE_DIRECT`、`n_pci_screen_state`（wm.c の許可とポインター）。nvrm：`NVRM_SCANOUT_CREATE／PRESENT／IMPORT／DESTROY`、flip のみの `applyModeSetConfig`、ハードウェアカーソル。Mesa：NVK の`VkImportMemoryNocturneScanoutInfoMESA`、Zink の `WINSYS_HANDLE_TYPE_NOCTURNE_SCANOUT`、ngl_scanout（3 枚、`ST_FLUSH_WAIT` の後に PRESENT）。ブラウザー：全画面の canvas を直接表示、UA の `:not(:root):fullscreen { object-fit: contain; }` と canvas の object-fit。確認：host 試験（nvrm_display、NVK sync、Mesa screen、NVK RM）通過、全体ビルド、QEMU の pcitest 0 失敗（直接表示の許可・取消・ポインター・窓の終わり）、pagertest 73 検査 0 失敗、QEMU の実サイト（get.webgl.org、webglsamples の aquarium、YouTube の再生・全画面の出入り・スクロール）で回帰なし。直接表示そのものは QEMU に NVIDIA が無いので未確認（実機で `wm:` の行と全画面 WebGL）。`tests/pci_hosttest.c` は以前の改名で 39 件のコンパイル誤りがあり古いまま（今回は宣言の足りない 2 関数だけ足した）。
- 2026-10-24（2）：起動後に画面が凍る問題。RM は表示エンジンを組まず、CPU の起動時の画面から GPU へ表示を移す処理が
  無かった（D10・C53）。`kernel-open` の GPL でない部分をすべて移植：nvidia-modeset（NVKMS と KAPI）、
  nvidia（`os_caps.c`＝C52、`os_ioctl.c`、`os_acpi.c`＝C54、BearSSL の SPDM 暗号 `spdm_crypto.c`、レジストリーと
  装置ごとの設定、GPU の除外、`rm_get_gpu_uuid_raw`・`rm_set_rm_firmware_requested` の順序）、nvidia-uvm（C55）。
  カーネル：`n_pci_give_screen`・`n_pci_screen_vblank`（fb.c の出力先、wm.c の帰線）、pager ABI、ACPI ABI。
  確認：host 試験（nvrm_display・dma・contract・transport・ioctl・spdm_crypto）通過、全体ビルド。QEMU の回帰は
  §7 の次の記録または報告。実機（4 回目）で `NVRM display:` と `wm:` の行、画面が GPU の面へ移るかを見る。
- 2026-10-24：実機 3 回目（USB 起動、IOMMU 有効）。USB の GPT から System.log と nvrm.log を読み取り専用で
  取り出した（`build/qa/realhw/`）。RM は `RmInitX86EmuState` で `0xA0000` を写せず停止。その前に
  `os_flush_cpu_cache_all` を求めて `NOT_SUPPORTED` を受けていた。修理：`os_pci.c` で `primary_vga` を常に偽
  （C50）、WB の DMA オブジェクトの一覧と `clflush` による全書戻し（C41 訂正）。GPU の遮断された DMA は
  C51 に観測として記録。
  確認：host 試験（nvrm_dma・contract・transport）0 failed。dma の試験に書戻しの場合（写像の後始末、
  解放で一覧から外れる）を足した。contract の模擬は前の変更（ログを `n_app_data_folder` へ）に
  追い付いておらずリンクで落ちていたので、データフォルダーが無い場合として直した。全体ビルド済み。
  実機での確認は未実施（4 回目：RmInitAdapter が x86emu を越えて GSP の起動まで進むか、SEC2 の
  scrubber の `0x65` が再発するか、C51 の遮断が RM のリセット後に止まるか）。

- 2026-10-21：ドライバー（`ports/nvidia`）と NVK の nvkmd（`nvkmd_nocturne_rm.c`）から `errno` と `msleep` を
  除いた。POSIX の「-1＋errno」ではなく、Nocturne のネイティブ関数が失敗を負の `n_result` で返す形に
  ドライバーが使う関数をそろえた：`n_shared_map(h, bytes, access, &mapping)`、
  `n_memory_reserve(bytes, access, &memory)`、`n_memory_release`、`n_memory_protect`、`thread_create`、
  `thread_join`、`cpu_info`、`clock_info`、`pcilist`（どれも errno に触れない）。待ちは `n_sleep_ms`。
  C11 の `thrd_create` は `thread_create` の結果から `thrd_nomem`／`thrd_error` を決める。全呼出し側
  （libc、malloc、アプリ、wabt・tinycc・Mesa の移植部、Rust std の移植、試験）を同じ形へ直し、
  malloc の errno 退避も不要になったので消した。
  確認：host 試験（nvrm_dma・contract・transport、nvk_rm、malloc の模型）0 failed。全媒体を警告の
  増加なしでビルドし、Rust std を作り直した。QEMU（q35＋VT-d、WHPX、4 コア、4096 MiB、AHCI 起動、
  snapshot）で `pcitest`・`nvrmtest`・`pcitest flr`・`nvrm_servetest`（127 本受け、33 本断る）・
  `sharedpagetest`・`nativeabitest`・`wxtest`・`threadtest` が 0 failed。ブラウザーで JavaScript demo への
  クリック、Alt+Left、ホイールの下と上を撮影して見た。今回と関係なく前からの失敗：`memreservetest` の
  1 TiB 要求（カーネルは 4 GiB を超える要求を N_ERR_INVALID で断り、試験は N_ERR_NO_MEMORY を期待）、
  `cpuinfotest`（scheduler_cpus ≤ 2 を期待、4 CPU では 4）、`malloctrimtest` の物理ページ数の 3 件
  （システム全体の空きを測る）。実機は未実施（ユーザー）。

- 2026-10-21：2d0973be への第二の監査（残件 4、新規 3、重複コード、小さな指摘 7）を AMD 48882 3.10、
  Intel D51397-019、PCIe 6.0、PCI Local Bus 3.0 と照合して修理。
  残件：(a) nvrm のクライアントごとのスレッドは上限（プロセスあたり 128）で断るだけで失敗しない。
  `tests/nvrm_servetest.c`（実 RM の代わりの偽 OS 層で `nvrm_serve.c` を動かす）で 127 本受け、
  33 本断ることを確かめた。失敗した HELLO が arena を漏らす不具合も修理。断る理由はカーネルの
  戻り値どおりに出す（以前は常に「メモリ不足」）。(b) 異常終了後の GPU：BME が残った、止まらない、
  要求が 200 ms 内に終わらない機能を claim の終わりに Function Level Reset する（`pci_function_reset`、
  §6.6.2：100 ms、要求が残れば Completion Timeout、ID が読めるまで最大 1 s（§6.22、§5.9）、設定を
  書き戻す）。起動画面の機能は画面ごと消えるのでリセットしない。(c) 以前のバスマスター凍結は実機
  でしか調べられず未解明。(d) `function_quiet` の数値は `PCIE_DEVCAP/DEVCTL/DEVSTA/DEVCTL2` へ。
  新規：(1) 隔離できない装置（ACS の無い／効かないルートポート等）は claim を拒まず、隔離なしで
  渡してログに出す。グループ内に以前の claim の遮断が残る時だけ拒む。(2) VT-d の IRREQ=1 の
  ユニットは、DMA 変換が使えない（`noiommu`、ページ表の形式が無い、起動失敗）時も割り込みの
  再マップだけで起動し、割り込みを失わない（§5.1.4）。AMD には IRREQ に当たるものが無い。TSC の
  周波数が分からない時の時間切れは 10 GHz を上限に見積もる。(3) 画面の保持は Ctrl+Alt+Esc で
  取り戻せる（保持中のドライバーを止め、機能が止まってから画面が戻る）。`fbhold=` は任意の上限。
  重複：配列を伸ばす 11 か所の写しを `kappend`（mm/heap）へまとめた。
  小さな指摘：ルートポートは ACS と Source Validation があり、要求した P2P の制御が読み戻せる時だけ
  分ける（PCIe 6.0 §6.12.1.1、§1.3.1、VT-d §3.17）。中断した休止ではユニットを設定し直さない。
  AMD の CmdBufRun は有効にした後に確かめ、PerfOptEn を切る。Intel の queue_restart は 2d0973be で
  修理済み。拒んだ claim の ACS の書込みは元へ戻す。同じグループの他の機能は解放後も遮断のまま
  （P2P の到達を理由に文書化）。claim ごとの IOTLB 全体無効化は性能だけの問題で、正しさのため残す。
  従来型 PCI は Bus Master を切って読み返せば要求は終わっている（PCI 3.0 §3.2.5、§3.3.3.3.3）ので、
  IOMMU が無くても正常終了なら DMA の RAM を戻す（以前は常に隔離し、毎回ログに出ていた）。
  確認：警告なしでカーネルと全媒体（img・vhd・vhdx・iso）をビルド。QEMU を一台ずつ（WHPX、4 コア、
  4096 MiB、1920×1080、AHCI 起動、snapshot、`edu`、pcie-root-port の下に virtio-rng）で q35＋AMD-Vi、
  q35＋VT-d、pc 機種（IOMMU 無し）：どれも `pcitest`・`nvrmtest`・`nvrm_servetest`・`pcitest fbhold`
  が 0 failed、IOMMU の 2 構成は `pcitest flr` も 0 failed（BME を残した virtio が「it was reset」、
  BAR は戻り virtio の状態は 0）。VT-d 構成で `fbhold=20` の保持中に Ctrl+Alt+Esc を送り、5.6 s で
  画面が戻ることを撮影して見た。3 構成とも JavaScript demo へのクリック、Alt+Left、ホイールの
  下と上への戻りを実入力で見た（目視）。IRREQ=1、ACS の無いルートポート、FLR の時間切れ、実 GPU の
  リセットは QEMU では起こせず未試験。実機は未実施（ユーザー）。

- 2026-10-21：8131ca1a の IOMMU への追加監査 4 件を仕様と照合し、すべて正しかったので修理。
  (1) VT-d の CFI：§5.1.4 のとおり CFI のままでは互換形式の MSI が IRTE の検査を経ない。起動の最後に
  CFI を切って確かめ、だめなら VT-d を使わず、IOAPIC の項目を通常の形式へ書き戻す（`ioapic.c` に
  与えた項目の控え `given` を持ち、`ioapic_remap` が再マップの入りと切りの両方で書き直す）。
  (2) VT-d の IQE：§6.5.2.11 のとおり、拒まれた記述子（head の位置）を何もしない wait に替え、tail を
  書き直して IQE を消すだけにし、動作中の待ち行列の作り直しをやめた（その操作は失敗になる）。
  §6.5.2 のとおり QIE を切るのは wait で空と確かめた後だけ。切れなければ VT-d を使わない。
  ファームウェアが残した待ち行列は、空でエラーが無い時だけ切る。
  (3) AMD-Vi のイベントログ溢れ：§2.5.1 のとおり EventLogRun が 0 と確かめられるまで EventLogEn、
  HEAD/TAIL、EventOverflow に触らず、次の取出し（割り込みか sync）でやり直す（ログは一度だけ）。
  (4) VT-d の書込みバッファー：§6.8 のとおり、無効化を伴わない新しい写像の WBF の時間切れを
  `intel_flush` の失敗として返す。無効化が自分で flush する経路の重複した WBF は削除。
  確認：警告なしでカーネルと全媒体（img・vhd・vhdx・iso）をビルドし、nocturne.img のカーネルが
  試験したものと一致。QEMU を一台ずつ前回と同じ 3 構成で動かし、すべて `pcitest: 0 failed`・
  `nvrmtest: 0 failed`・`pcitest fbhold: 0 failed`。IOMMU の 2 構成は「isolated: yes」。VT-d は CFI を
  切って確かめたうえで起動した（QEMU は CFI を持たない）。クリック、Alt+Left、ホイールの上下を
  実入力で見た（AMD と Intel の構成。pc 構成はホイールと PgDn）。拒まれた記述子、QIE を切れない
  場合、イベントログが止まらない場合は、QEMU では起こせず未試験。IRREQ=1 のユニットで起動に
  失敗すると IRE を切ることになり、すべての割り込みが止まる（既存の問題。未修理）。
  実機は未実施（ユーザー）。

- 2026-10-21：0d7daeb の IOMMU を二つの監査で照合し、仕様（AMD 48882 3.10、Intel D51397-019、PCIe 4.0）
  に沿って修理。共通層：claim は P2P で互いに届く装置のグループごとにドメインへ入れる（要求元 ID・
  エイリアスの共有、ACS の効かない多機能装置・スイッチ下流ポート・ルートポート。ACS は読み戻して確かめ
  E と T は切る）。グループのカバー（DRHD のスコープ・INCLUDE_PCI_ALL、IVHD の範囲）とカーネルの ID
  （IOAPIC・HPET）を確かめ、だめなら claim を拒む。無効化の失敗・時間切れは握り潰さず `N_ERR_IO`、
  遮断の未確認は保留して次の claim でやり直し、それまでの再 claim は BUSY、ベクターと DMA の RAM は
  渡さない。固定上限は動的配列（`kgrow`）へ、TSC の 3 GHz 仮定は `tsc_hz` へ。IVMD の読み書き許可を
  尊重。AMD-Vi：不正命令で止まった命令バッファーの再開、イベントログ溢れの §2.5.1 の順、INV_ALL が
  無ければ見える ID だけ。VT-d：EIMER・IRREQ・MHMV、x2APIC 形式の宛先、IOAPIC は MADT の id で DMAR の
  スコープと照合、文脈の全体無効化の後に IOTLB の全体無効化、DR/DW、PMEN の予約ビット保持、IQE の
  回復。同意しなかった点：AMD の IRT は cached なので INV_IRT は残す、SMI は SMI フィルターがファーム
  ウェアの SMI も止めうるので止めず文書化、ロック無しの指摘は BSP だけの設計なので該当せず。
  確認：カーネル差分ビルドと全媒体（img・vhd・vhdx・iso）、警告なし。QEMU 一台ずつ（WHPX、4 コア、
  4096 MiB、1920×1080、AHCI 起動、snapshot、`edu`）で q35＋`amd-iommu,intremap=on,dma-remap=on`
  （/data は NVMe）、q35＋`intel-iommu,intremap=on`（同）、pc 機種（IOMMU 無し、/data は IDE）：
  どれも `pcitest: 0 failed`・`nvrmtest: 0 failed`・`pcitest fbhold: 0 failed`。IOMMU の 2 構成は
  「isolated: yes」で、claim 外の DMA は遮断（VT-d は理由 05h）。NVMe の MSI、e1000、DHCP も動いた。
  3 構成とも JavaScript demo へのクリック、Alt+Left、ホイールの下端と上への戻りを実入力で見た（目視）。
  QEMU の「Ignoring request for interrupt vector 0」はカーネル出力前（ファームウェア）に出ており無関係。
  QEMU の amd-iommu が出す page fault イベントは、仕様の書式で読むと「interrupt at 0」になる（遮断は
  されている。QEMU の符号化の問題と見ているが未確認。実機のイベントで確かめる）。
  Ryzen 7600X＋RTX 4070 の実機は未実施（ユーザー）。

- 2026-10-21：837f8789 の残り3件を修理。(1) C40：画面保持の30秒上限を削除（`fb_take_expiry` も削除）。
  保持は RM の解放か claim の終わりでだけ終わり、claim の終わりは装置を止めて（revoke、MSI・バスマスター
  無効、IOMMU 遮断、DMA 隔離）から画面を返す。止まったドライバーには既定無しの `fbhold=<秒>` でアプリを
  止める。(2) C46：`os_dump_stack` は `high - at` の前に `at > high` を弾く。(3) 同じ範囲を A→B と map して
  B を unmap すると A を取り消していた件：client の線形アドレスを bus アドレスから作るのをやめ、map ごとに
  2^63 以上の再利用しない区間を割り当てて `pLinearAddress` を一意にした（sysmem は `os_match_mmap_offset`、
  BAR は `osMapPciMemoryAreaUser`）。確認：ホスト試験3本 0 failed、`scripts/build.py image`、QEMU（WHPX、
  -smp 4、4096MiB、1080p、`fbhold=20`）で pcitest・nvrmtest・`pcitest fbhold=20` が 0 failed。保持中の
  画面2枚はバイト一致で、入力を送っても描かれず、20秒でアプリ停止→装置停止→画面解放の順に戻った
  （目視）。ブラウザーのクリック遷移・戻る・ホイール上下に回帰なし。実機（RTX 4070）は未確認。
  `user/include/nocturne.h` の `n_pci_hold_screen` の説明は、全体再ビルドを避けるため古いまま（30秒）。
- 2026-10-21：C48 を修理。ACPI の読むだけの DMAR／IVRS 表示を消し、AMD-Vi（`iommu_amd.c`）と VT-d
  （`iommu_intel.c`）のドライバーを共通層 `iommu.c` の上に書いた。claim した装置は専用の恒等写像
  ドメインへ入り、DMA は claim の DMA オブジェクトと予約範囲（IVMD・RMRR）だけ、割り込みは再マップ表で
  claim のベクターだけ。claim しない装置は素通し。`N_PCI_ISOLATED` を足したので全体ビルド（img・vhd・
  vhdx・iso）。QEMU 一台ずつ（WHPX、4 コア、4096 MiB、1920×1080、AHCI 起動、snapshot、`edu`）で 3 構成：
  q35＋`amd-iommu,intremap=on,dma-remap=on`、q35＋`intel-iommu,intremap=on`（/data は NVMe）、pc 機種
  （IOMMU 無し）。どれも `pcitest: 0 failed`・`nvrmtest: 0 failed`。IOMMU のある 2 構成では pcitest が
  「isolated: yes」となり、claim 外への DMA が遮断されてユニットのイベント（VT-d は理由 05h）が出た。
  NVMe・xHCI の MSI、e1000（VT-d では IOAPIC の再マップを通る INTx）、DHCP も動いた。3 構成とも、
  リンクのクリックで JavaScript demo へ、Alt+Left で戻り、ホイールで下まで行って上へ戻るのを実入力で
  見た。回帰なし。QEMU 11.1 の癖：`amd-iommu` は `dma-remap=on` を付けないと変換せず全部素通しにする。
  `intel-iommu` は GCMD.CFI を持たないので、CFI は努力目標とした（取れなくても再マップは続ける）。
  Ryzen 7600X と RTX 4070 の実機は未実施（ユーザー）。

- 2026-10-21：3件の静的監査（A1〜A4、B1〜B5、F1〜F3）を照合し、C39〜C49 として修理。新しい ABI
  `n_pci_hold_screen`（`SYS_PCI_HOLD_SCREEN`）のため全体ビルド（img・vhd・vhdx・iso）。ホスト試験
  （dma・contract・transport）は 0 failed：型つき確保、BME の入切と読み戻し、画面保持の入れ子、
  イベント上限（4096 件で止まり、古い順に欠けずに取れ、ログは一度）、サーバー停止の起こし方。
  QEMU 一台（WHPX、4 コア、4096 MiB、1920×1080、AHCI 起動、snapshot、`-vga std -device edu`）：
  `pcitest: 0 failed`（Command・BIST・Interrupt Line・D3 の拒否、ブリッジの claim 拒否、BME を残した
  終了で DMA の RAM が隔離され次の claim で BME が 0、std VGA の画面ページの UC 拒否・WC 可・UC と WC の
  別名拒否、画面保持の入れ子と claim 終了での返却）、`nvrmtest: 0 failed`。保持の解放後に机上と
  ブラウザーが描き直されたこと、ホイールの下と上、JavaScript demo へのリンクと Alt+Left を実入力で
  見た。回帰なし。q35＋`intel-iommu` で DMAR の行（fed90000、48 ビット、変換 off）を確認（q35 には
  旧 IDE が無く /data は読めないので試験は pc 機種で）。従来型 PCI の解放ログが「要求が残っている」と
  誤って言っていたのを「完了を報告できない」に直した。RTX 4070 の実機は未実施（ユーザー）。

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
