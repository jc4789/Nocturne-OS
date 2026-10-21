# NVIDIA再監査87dd1e2fの修理・独立受入

主担当root、2026-10-20開始。対象は提示されたA1〜A10と、その修理に必要なnative IPC/共有写像境界。
開始時のHEADは87dd1e2f、差分なし。今回 `kernel/src/dev/gpu.c` と `kernel/src/sys/events.c` は未変更。
GPT-6.1 SOL/xhighの三担当の二段階検討を受け、主担当自身が差分・反例・実媒体を確認した。

## 指摘への実装修理

| 指摘 | 処置・維持する契約 |
|---|---|
| A1 書込み専用 | NVOS33が許すRW写像を提供。x86のWRITEABLEを拒否しない。READ_ONLYの不可逆上限は保全。 |
| A2 エラー引数 | dispatcher errorでも検証済みの引数・arena・partial handleを返す。壊れたreplyやnative拒否ではコピーしない。NVKもpartial alloc handleを回収。 |
| A3 一時失効 | 永久取消による偽のRTD3対応を除去。状態・既存写像を変えずNOT_SUPPORTED。safe admissionは独立。通常unmap/free/closeの永久取消は維持。 |
| A4 実プロセス | kernelがsend時のapp IDをmessageへ保存。全thread・別接続で同一主体、別appへ譲渡したendpointは実送信者になる。HELLO後の要求も一致検査。IDは再利用しない。 |
| A5/A6 cache | prefetchable FB/IMEMのWC要求を尊重、USERDのページ丸めoverlapとglobal WC禁止をUCへ反映。BAR0の生存UC aliasと矛盾する別属性は作らない。 |
| A7 非連続 | rangeを動的保存し、native shared composeで連続VAへ並べる。RO・部分cache・source取消・PCI claim寿命を保持。構築途中の取消もPTE公開と直列化。 |
| A8 選別 | NVIDIA VGA/3D/progifの共通predicate、所有PCI snapshot、実RM probed/BDFとHELLOの明示readyで列挙。診断daemonは接続可能なままGPU一覧から除く。後続ready GPUも探す。 |
| A9 失敗種別 | request不在・safe禁止をtokenのNOMEMへ変換しない。非request tokenは別寿命に保持し、native公開時に状態を検査する。 |
| A10 読戻し領域 | channelごとからnnrm/VAspace単位のlazy共有へ変更。channel参照で保持し最後に解放。全USERDがsysmemの現経路で、BAR1読戻し→GPPut→fence→doorbellを維持。 |

GPLv2-onlyコード、POSIX層、新しい人工的資源上限は追加していない。`uvm_common.c`は参照・使用していない。
指定されたNVIDIA独立MIT実装と公開契約を照合し、他OS本体の転記はない。vendor MIT `os.c` の既存寿命hookは維持し説明を正した。

## 親が独立に確認した根拠

- RM担当最新第二報 `review-87dd1e2f-rm-second.md`：client/serverのcopybackとエラー優先順位、HELLO旧echo判定、送信主体のrequest/cleanup TLS、orphan回収、通常map scopeを差分で追跡。
- NVK担当最新第二報 `review-87dd1e2f-nvk-second.md`：private初期化時のprobed登録を親が発見し、readyとの混同を追加修理。snapshotのBDF保持、全失敗経路の一時接続/handle回収、共有BAR1の参照と初期化順、CPU context保持を確認。
- native担当の第二報と追補：flat leaf/cacheとscope依存、構築中VA取消、PTE/TLB後のDMA pin返却、send-time認証、旧receiveの添付・PEEKを差分で追跡。`vmm_user_free`とtask終了のcontinuation/join契約も照合。
- 親が実行したhost検査：`root-rm-host.log`は実7TU target構文＋3群0失敗。`root-nvk-host.log`は同期/screen/RM境界3群0失敗。mockの結果を実RM/GPU成功に昇格しない。

## 実QEMUと失敗の処置

条件はWHPX、4 CPU、4096MiB、1920×1080、AHCI boot、virtio-vga-gl、edu `dma_mask=0xffffffff`。
同時一台、専用boot/dataのsnapshot。通常`build/data.img`とHyper-Vは操作していない。
同一の製品kernel/initrdで実行し、二度の再起動は修正した通信試験だけを実行した。

ログの基準ディレクトリは `build/nvidia-review-87dd1e2f/`。

| 実行 | 結果・意味 |
|---|---|
| `serial.log` pcitest | 0失敗。非連続/混合cache/RO/source取消/claim消失、実free_memサイズのsparse構築中取消反例を含む。2回のchild page faultは取消読取りを拒否する期待結果。 |
| 同 nativeabitest | 222項目、0失敗。既存native ABIの対象操作。 |
| 同 nvrmtest | 0失敗。RM初期化後、eduを非対応として拒否、終了後claim再取得。GSP/実NVIDIAは未実行。 |
| 同 gputest | 55項目、0失敗。既存virgl経路の局所回帰確認でありNVIDIA native支援の証拠ではない。 |
| 初回nativechanneltest | 3MiB送信直後に同threadが空送信し、受信前に流量回復を待つ既存自己待機。画面のpsでpid17 sleepを確認して当該appだけ停止。巨大messageの境界/PEEK/短buffer拒否/全量一致を保持し、空送信を受信後へ移した。 |
| `channel-serial.log` | 235項目中1失敗。既存文書の一回最大64件batchに対し、一回96件を要求していた試験の契約誤読。製品を変更せず、戻り件数ごとに一意token・本文・senderを検査して受信消費する形へ訂正。96件全数は必須のまま。 |
| `channel-final-serial.log` | 修正後333項目、0失敗。同app別thread/別接続、endpoint譲渡と送信者終了後の認証主体、添付、PEEK/DISCARD、旧API、96接続全数を実行。 |

既存eventの64件batchは今回未変更。新しい制限として追加したものではない。文書で保証していない「一回96件」のみを訂正し、全96件到達の期待値を弱めていない。
host fixtureのerrno stub不足も担当第一報に残した。媒体検査でxorrisoのWindows絶対path解釈と抽出時read-only属性に遭遇し、相対pathと確認済み一時fileの属性処理で解決した。検査失敗を製品成功へ読み替えていない。

## 実画面・実操作

主担当がPNGを開いた。すべて実Nocturne画面で、`file:///data/tests/gpu_browser.html` は局所fixtureであり実サイト受入ではない。

- `boot.png`：1080pのdesktop、文字・アイコン・背景を確認。
- `browser-initial.png`：641×97のCanvas二枚の左赤/右青、画素一致、本文・URL・scrollbarを確認。
- `redraw-target.png` → `redraw.png`：実マウスの対象を目視してクリック。左緑/右青へ両Canvas更新、再描画1。
- `scroll-down.png`：実PageDownでWebGL二contextの赤/緑、CPU caveatのnull、入力欄まで移動。
- `input-target.png` → `input-sent.png`：実クリック・`gpu42`入力・Enterで「受信：gpu42」を確認。
- `body-target.png` → `bottom.png` → `returned-top.png`：本文へfocus、PageDownで下端、PageUpで上端へ戻り、緑/青を保持。
- `processes.png` は初回試験の停止診断。`channel-screen.png` / `channel-final-screen.png` は各通信試験後の実ブラウザー表示。

大きいHMP相対mouse移動は狙い通りにならなかったため、クリック前に画像で位置を確認し、小さい移動と実測感度で合わせた。DOM注入で操作成功を偽装していない。

## ビルド・媒体

`scripts/build.py -j 2`で通常依存の増分buildを実施。clean/stash/touchなし。
`product-build.log`、最後のkernel修理を反映した`final-kernel-build.log`、四媒体の`delivery-build.log`はいずれも終了0。
共有ABIの実変更に伴う依存再コンパイルは正規buildに任せた。

`media-verify.log`でIMGと実行scratchのkernel/initrd一致、rawとVHD/VHDXの全内容一致。
`media-payload-verified.log`でISO内部kernel/initrd、initrdのbrowser/browserjsworker/nvrm/libc/native headersを現成果物と照合。

- kernel SHA-256: `4ea57d406ba410037ab2928f285631fa505f15e971fbefe74890a5802d30f8d3`
- initrd SHA-256: `ff1edee21c0ff060ed757de5b7a2ade62a08c13f1323c87d129a0d8e1a5cca69`

四媒体は `build/nocturne.img`、`build/nocturne.vhd`、`build/nocturne.vhdx`、`build/nocturne.iso`。
一時ISO抽出は回収済み。今回のQEMU三回はすべて終了済み。
scratch boot/dataは回収済み（`cleanup.log`、`cleanup-final.log`）。dataの初回削除は共有違反で止め、
Windowsの読取りAPIで保持元を調べたところ、最初の準備で既存bin名への入力待ちになった今回のmmdだった。
pidと引数が専用dataを指すことを確認し、その一プロセスだけを終了して回収した。無関係なprocess・通常data・四成果物は保全。
再実行には保存した設定・autorunと正規媒体からscratchを再作成する。削除済み媒体がそのまま残っているとは報告しない。

## 明示的な限界

NVIDIA実機のGSP起動・VRAM/cache/PCIe posted write・実RM dup・複数GPU・実thread全競合・IOMMU・性能差は未確認。
RTD3/fault復元は未対応のまま。Hyper-V実機はユーザー担当。全アプリ/実サイトの無回帰は証明していない。
受入は上記の修理と検証範囲に限定し、加速や実機完成とは報告しない。
