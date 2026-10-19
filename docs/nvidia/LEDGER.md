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
| P6 | libnvrm（クライアント）と通信規約 | 主担当 | 進行中：クライアント側・サーバー側（`nvrm_serve.c`）は済、OS 層は担当 B |
| P7 | Mesa：NVK＋nvkmd_nocturne＋NAK／nil のビルド | サブ C | 進行中 |
| P8 | Zink と WebGL（nocturne_gl.c）への接続、softpipe への後退 | 主担当 | 未着手 |
| P9 | 全体ビルド、QEMU で回帰が無いこと、文書、コミット | 主担当 | P0〜P6 分は済：全体ビルド（img・vhd・vhdx・iso）、QEMU で机上・ブラウザー（表示、リンク、戻る、ホイールで下と上）・3D Cube（63 fps）を目視。NVK・Zink 分は未 |
| P10 | 実機確認（ユーザー） | ユーザー | 未着手 |

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
| C10 | RAM を WB の直接写像と WC／UC の写しで二重に持っても実害は小さい | 未確定 | x86 は型の混在を避けるよう求める。RM が非キャッシュを求めるメモリだけに限る。問題が出たら直接写像の属性も変える |
| C11 | RM の `arch/nvalloc/unix` は NVIDIA 自身の OS 中立層（MIT）で、使っても UNIX 互換を Nocturne に足すことにはならない | 受理 | Nocturne 側へ POSIX の API を足さず、RM の OS 層を Nocturne の API で実装する |
| C12 | RTX 4070 は ReBAR 有効なら BAR1 が 16GB になり得る。BAR 全体を 4K で一度に写さない | 受理 | 16GB÷4K＝400 万 PTE。view で必要な部分だけ写す |
| C13 | アプリ終了時は fd をすべて閉じてから共有マッピングを外すので、PCI の権利（fd 3）が DMA メモリの解放より先に終わり、バスマスターが先に止まる | 受理（順序）／未確定（nvrm が権利のハンドルを複製した場合） | sched.c の終了処理（fd を昇順に閉じた後 `sys_shared_cleanup`）。nvrm は権利を複製しない規約にする |
| C14 | MSI-X の表は BAR の中にあるので、BAR を写したドライバーは表を書き換えられる | 受理（危険として） | ドライバーアプリは init が起動する信頼済みのもの。IOMMU も無いので DMA でも同じ。権限分離の目標外と明記 |
| C15 | RM は割り込みを MSI-X でも MSI でも受けられ、INTx は不要 | 未確定（実機待ち） | OS 層は `n_pci_interrupts(1)` の後に `NV_FLAG_USES_MSIX`／`NV_FLAG_USES_MSI` を立てる。担当 B の調べでは RM はこの印を読まない。実際に割り込みが来るかは実機で |
| C18 | RM の OS 層が RM の「spinlock」を mutex にしても正しい（割り込み処理もスレッドなので） | 受理 | `rm_isr` は普通のスレッドで動き、`os_is_isr` は `nvrm_in_isr` で真になる。眠ってはならない区間で眠る危険は、割り込み文脈が本物の割り込みでない以上ない |
| C19 | 全クライアントが RM には同じ管理者権限のプロセスに見える | 受理（制約として） | `nvrm_thread_admin` は既定で真。クライアントごとの隔離は無い。グラフィックスを使うアプリはすべて信頼する前提。後で接続ごとに権限を分けられる |
| C17 | Zink は Vulkan を `util_dl_open(VK_LIBNAME)` と `vkGetInstanceProcAddr` だけで得るので、Nocturne の util の dl 層に静的な表（"vkGetInstanceProcAddr"→NVK の `vk_icdGetInstanceProcAddr`、`vkGetDeviceProcAddr`）を置けば、zink 本体を変えずに静的リンクの NVK を使える | 未確定 | zink_screen.c 3481〜3492 行。ローダー無しで ICD の入口がローダーの代わりになるか（インスタンス拡張の列挙等）は P8 で確かめる |
| C16 | 1 バイトの構成空間書き込みを 16 ビットの読み書きで行うと、隣の状態レジスターの RW1C ビットを消すことがある | 受理（影響なし） | RM の `osPciWriteByte` には呼び元が無い（rg で 0 件）。1 バイト書き込みは起きない |

## 5. 担当と受け渡し

- 主担当（Opus）：設計、カーネル ABI、init、nvrm の骨格、libnvrm、統合、ビルド、QEMU、コミット。
- サブ A（Sonnet）：Rust std の移植（`ports/rust/`）。
- サブ B（Sonnet）：RM の取り込みとビルド（`third_party/nvidia-open/`、`ports/nvidia/`）。
- サブ C（Sonnet）：Mesa の NVK 側（P6 の規約ができてから）。
- サブの報告は受理前に主担当が差分とビルドで確かめる。

## 6. 次の一手

1. 担当 B（RM と OS 層）・担当 C（NVK）の報告を差分とビルドで確かめて受理。nvrm.mk を Makefile へ。
2. P8：Zink を静的な dl 表で NVK へつなぎ、`nocturne_gl.c` で Zink → softpipe の順に選ぶ。
3. 全体ビルド（`common/abi.h` の変更で一度は全体）。担当がコンパイル中の間は走らせない。
4. QEMU 一台で：pcitest（`-device edu`、`appdrivers=any`）、rusttest、既存の画面・ブラウザーの回帰。

## 7. 記録（新しい順）

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
