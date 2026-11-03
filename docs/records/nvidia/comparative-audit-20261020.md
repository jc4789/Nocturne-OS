# NocturneのNVIDIAドライバーとGPU対応 比較監査

2026年10月20日

## 結論

**Nocturneには、NVIDIAのRMを独自のユーザー空間ドライバーとして動かし、NVKとZinkを経由してWebGLへつなぐ実装がある。ただし、安全に使える汎用NVIDIAドライバーとして受け入れるには、クライアント分離、写像権限、CPUからGPUへの書込み公開順序などに修正が必要である。**

「NVIDIAドライバーが存在しない」も「RMを移植したのでGPU対応は完成」も、現在のソースを正しく表していない。画面合成、表示モード制御、動画復号、一般GPU APIはそれぞれ別の経路であり、NVIDIAの3D描画経路の存在だけでは対応済みにできない。

本監査は特定の初期化不具合に限定せず、起動、PCI、DMA、RMの認可、写像、命令送信、同期、公開能力、ブラウザー接続、梱包、検証範囲を横断した。以下の不備は静的確認に基づく。実機での発症、性能、表示の正常性は今回確認していない。

## 範囲と比較の考え方

- Nocturneの開始時HEAD：`b028b364e51dccfe1610593b65e61d2543223678`。
- 指定基準：[NVIDIA公開資料](<D:/Programes/cloned repos/open-gpu-doc>)と、[615.78.08のkernel-open](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open>)。
- 公開資料から、PCIレジスター、GPFIFOとセマフォ、表示クラスの役割を照合した。kernel-openからはNVIDIA側のOS接続契約、写像保護、PCI設定、書込み公開、同期・解放の扱いを比較した。
- Nocturne側では、[RM接続層](<D:/Programes/llm vibe slop os/ports/nvidia>)、[NVKMD接続層](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne>)、[Mesa接続層](<D:/Programes/llm vibe slop os/ports/mesa>)、カーネルのPCI・共有メモリ・GPU・表示、ブラウザーと梱包経路を追跡した。認可の判断では同梱RMの呼出し先まで確認した。
- 基準が使うLinux API自体をNocturneへ要求していない。必要なのは同じ所有権・保護・ハードウェア契約をNocturneのネイティブ方式で満たすことである。別OS本体のソース取得やコード取り込みは行っていない。
- 主要経路の比較監査であり、巨大な上流ツリーの全ファイルに対する悉皆査読ではない。

優先度は、P1を「隔離・データの正しさ・システム応答に影響し、先に直すべきもの」、P2を「条件付きの互換性・資源管理・公開能力・運用上の不備」とする。未接続機能と、現在の有効な利用経路で到達する不備を分けた。

## 機能ごとの比較

| 領域 | 基準が示す責務 | Nocturneの現在地 |
|---|---|---|
| RMとOS接続 | デバイス管理、OSコールバック、要求元の識別 | RM 615.78.08をユーザーアプリとして接続。ネイティブPCI、共有メモリ、C11スレッドを使う。単なる空APIではない |
| 3Dと計算命令 | 実装置のクラス、チャネル、GPU仮想アドレス、同期 | 実クラス一覧を取得し、NVKからRMへチャネル・メモリ操作を接続。全世代の実機成功は未確認 |
| GPFIFOとセマフォ | 指定フィールド幅、acquire、release、メモリ順序 | 公開クラスに沿う命令構築がある。ただしCPUからのBAR1書込み公開に問題がある |
| WebGL | 言語APIから実描画・表示まで | WebGL→Mesa→Zink→NVK→RMが接続。GPUが得られなければsoftpipe。起動競合と性能属性の判定に問題がある |
| 描画結果の表示 | 描画完了、表示資源、画面への受渡し | 全GPU完了待ちと読み戻し、CPU画素変換を経て表示。ゼロコピー合成ではない |
| NVIDIA表示エンジン | core、head、window、cursor、モード設定 | boot framebufferとCPUコンポジター。NVIDIA固有のモード設定、flip、vblank、表示エンジン接続は未実装 |
| 一般GPU APIとCanvas 2D | 装置への描画振分け | `SYS_GPU_*`はvirtio/virgl専用。NVIDIA RM/NVKとは別系統。512辺・32三角形等の制限もこちらのAPIのもの |
| Vulkanの画面連携 | 拡張の依存関係とWSI初期化 | headless関連ソースは含むが、公開拡張と初期化条件が不整合。headless実用対応済みとは言えない |
| 動画 | NVDEC等のエンジンと復号・表示の接続 | NVKのVDECは拒否、`has_video=false`。FFmpegのCUDA/CUVID/NVDEC/Vulkan支援も無効。ブラウザー動画のGPU復号は未接続 |
| 電源管理 | 装置停止、写像取消、復帰 | PCI claim中はsleep/hibernateを拒否。未対応だが、復帰不能なまま成功させない安全側の扱い |
| 複数GPU | 装置ごとの識別と接続先 | initは複数起動するが、全RMサーバーが同じサービス名を使うため成立しない |
| Hyper-VとRDP | 仮想表示、転送と実GPU処理の区別 | RAM bitmap表示・転送経路。NVIDIA GPU支援の動作証拠にはならない。実機Hyper-Vは今回未確認 |

根拠となる代表箇所：

- クラス取得：[nvkmd_nocturne_rm.c 392–400](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c#L392>)。
- 公開セマフォとGPFIFO定義：[clc36f.h 223–225、267–274](<D:/Programes/cloned repos/open-gpu-doc/classes/host/clc36f.h#L223>)。実装の64bit acquireとWFI→SYS_MEMBAR→release：[nvkmd_nocturne_ctx.c 269–300](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_ctx.c#L269>)。
- 表示クラスの責務：[公開表示資料 8–48](<D:/Programes/cloned repos/open-gpu-doc/classes/display/README.txt#L8>)。現在の画面出力：[wm.c 1632–1636](<D:/Programes/llm vibe slop os/kernel/src/gui/wm.c#L1632>)、[fb.c 115以降](<D:/Programes/llm vibe slop os/kernel/src/dev/fb.c#L115>)。
- WebGL読み戻し：[nocturne_gl.c 343–374](<D:/Programes/llm vibe slop os/ports/mesa/nocturne_gl.c#L343>)、[js_webgl.c 717–740](<D:/Programes/llm vibe slop os/user/libc/web/js_webgl.c#L717>)。
- 動画：[NVK側のVDEC無効化](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c#L399>)、[FFmpeg設定 658–671](<D:/Programes/llm vibe slop os/third_party/ffmpeg/config.h#L658>)、[通常の復号器生成 116–130](<D:/Programes/llm vibe slop os/user/libc/media.c#L116>)。
- 電源：[pm.c 213–226](<D:/Programes/llm vibe slop os/kernel/src/dev/pm.c#L213>)。

## 優先して修正すべき不備

### 1 P1 別アプリがRMの既定共有ポリシー上で同じプロセスになる

`os_get_current_process()`は要求元ではなく、常にnvrm自身のアプリ識別子を返す。RMはこれを各クライアントの`ProcID`として保存する。一方、オブジェクト複製の既定共有ポリシーは、複製元と先のPIDが同じなら許可する。

**条件と影響：** 別アプリが自分のRMクライアントから、既知または推測した他接続のクライアント番号・オブジェクト番号を複製元として指定すると、複製可能な資源について同一プロセス共有の認可を得られる。アリーナ内ポインターの範囲検査では、整数ハンドルの認可を補えない。

- 製品：[os.c 336–338](<D:/Programes/llm vibe slop os/ports/nvidia/os.c#L336>)、[client.c 120–128](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/src/kernel/rmapi/client.c#L120>)。
- 複製先の検査と複製元の検索：[rs_server.c 1710–1718](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/src/libraries/resserv/src/rs_server.c#L1710>)。複製権限検査：[rs_client.c 542–552](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/src/libraries/resserv/src/rs_client.c#L542>)。
- 既定PID共有：[sharing.c 348–355](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/src/kernel/rmapi/sharing.c#L348>)、[client_resource.c 219–230](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/src/kernel/rmapi/client_resource.c#L219>)。
- 基準：[os-interface.c 767–770](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/os-interface.c#L767>)は呼出元を識別する。

**修正方向：** 認証された要求元をネイティブIPCから取得するか、接続を独立したセキュリティ主体にし、異なる主体の共有は明示的に認可する。接続ごとの`nvfp`による厳密照合は既にあるため、「他接続への全control/freeが無制限」とまでは指摘しない。今回確認したのは別経路のPID共有認可である。

### 2 P1 RMの読み取り専用指定が共有ハンドルへ反映されない

RMから渡された`prot`は保存されるが、共有オブジェクトの作成時には使われず、クライアントも常に`N_MEMORY_READ_WRITE`で写像する。

**条件と影響：** RMが書込み不可としたメモリでも、CPU側の書込みを拒否できない。利用者が別途再写像しても拡大できない権利として伝える必要があり、クライアント側だけの修正では不十分である。

- 製品：[os_pci.c 711–717](<D:/Programes/llm vibe slop os/ports/nvidia/os_pci.c#L711>)、[rm_api.c 153–184](<D:/Programes/llm vibe slop os/ports/nvidia/rm_api.c#L153>)、[libnvrm.c 173–178](<D:/Programes/llm vibe slop os/ports/nvidia/libnvrm.c#L173>)。
- カーネルは元ハンドルの権利をviewへ継承する：[syscall.c 468–476](<D:/Programes/llm vibe slop os/kernel/src/sys/syscall.c#L468>)。読み取り専用ハンドルの検査自体は存在するが、元の権利が縮小されていない。
- 基準：[nv-mmap.c 758–763](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-mmap.c#L758>)は書込みと将来の書込み許可を除去する。

**修正方向：** RM属性、共有ハンドルの最大権利、CPUページ属性を一貫させる。

### 3 P1 BAR1書込みの到達確認より先にGPUが命令を開始できる

USERDはsysmemにあり、送信処理はCPUのatomic fence、`GPPut`書込み、再度のfence、ドアベル書込みという順序である。BAR1へのposted writeを排出する読戻しがない。

**条件と影響：** CPUがマップ済みVRAMへdescriptorやデータを書いた後でsubmitすると、sysmemの`GPPut`だけ先にGPUから見え、古いVRAM内容を使う可能性がある。CPUのメモリ順序とPCIe書込みの装置到達は同じ保証ではない。実機での発症は今回未確認。

- 製品：[sysmem確保 908–934](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c#L908>)、[USERD設定 1028–1041](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c#L1028>)、[公開処理 1187–1191](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c#L1187>)。
- VRAMを含めCOHERENT扱い：[nvkmd_nocturne_mem.c 110–118](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_mem.c#L110>)。
- 基準：[uvm_turing_host.c 222–235](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia-uvm/uvm_turing_host.c#L222>)はsysmemの`GPPut`公開前にBAR1を読み、CPU書込みがGPUへ届くことを保証する。

**修正方向：** 既存のRM／チャネル境界で安全な読戻し対象を保持し、装置に対する書込み到達保証をsubmitへ組み込む。

### 4 P1 virglの完了待ちがGUIを実行するBSPを占有する

**条件と影響：** オプションのvirgl経路でホストGPUの完了が遅れると、カーネル内で通常最大2秒、初期化・診断条件では最大10秒まで`pause()`だけで待つ。カーネル継続は非プリエンプティブなので、その間BSPのGUI処理や他のカーネル処理、停止応答が遅延する。これはNVIDIAのsubmit処理とは別の、一般GPU支援の問題である。

- 製品：[gpu.c 180–197](<D:/Programes/llm vibe slop os/kernel/src/dev/gpu.c#L180>)、[sched.c 350–351](<D:/Programes/llm vibe slop os/kernel/src/sys/sched.c#L350>)。
- 比較例：[NVIDIAのfence待機経路](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia-drm/nvidia-drm-fence.c#L123>)。基準の待機時間定数やOS APIをそのまま移植する提案ではない。

**修正方向：** 要求とDMAの寿命を保つ非同期完了・待機・取消を設計し、待機中もGUIを実行可能にする。単なる期限延長では解決しない。

### 5 P2 BARの固定索引が別の実在配置を扱えない

**条件と影響：** PCIの有効BARが`0/1、2/3、4/5`の64bit配置の場合、NocturneのPCI列挙は正しく`bar[0]、bar[2]、bar[4]`に記録する。しかしRMへの接続は固定の`{0,1,3}`を読むため、framebufferとinstance-memoryの基底・サイズが空になる。

- 製品：[pciuser.c 195–214](<D:/Programes/llm vibe slop os/kernel/src/dev/pciuser.c#L195>)、[os_pci.c 311–319](<D:/Programes/llm vibe slop os/ports/nvidia/os_pci.c#L311>)。
- 基準：[nv-pci.c 785–790](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-pci.c#L785>)はHopper以降の当該配置を説明し、[2367–2382](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-pci.c#L2367>)では有効なメモリーBARを順に選ぶ。

**修正方向：** 実際のメモリーBARと幅を使ってRM apertureへ対応付ける。Hopper／Blackwell全機種での発症や、今回のAda実機停止の原因とは断定しない。

### 6 P2 RM呼出しのエラーが成功応答に変わる

**条件と影響：** 未対応escape、引数サイズ不一致など、RMが個別引数のstatusを書き込む前に失敗した場合、サーバーは戻り値を捨てて通信statusを成功にする。ゼロ初期化した引数statusを読むAPIでは、未実行操作や未更新出力を成功と誤認できる。

- 製品：[nvrm_serve.c 113–125](<D:/Programes/llm vibe slop os/ports/nvidia/nvrm_serve.c#L113>)、[libnvrm.c 93–111](<D:/Programes/llm vibe slop os/ports/nvidia/libnvrm.c#L93>)。
- 引数status更新前の拒否例：[escape.c 804–824](<D:/Programes/llm vibe slop os/third_party/nvidia-open/src/nvidia/arch/nvalloc/unix/src/escape.c#L804>)。
- 基準：[nv.c 2982–2983](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv.c#L2982>)はRM呼出し失敗を呼出元へ伝播する。

**修正方向：** 通信エラー、RM関数の戻り値、個別APIのstatusを分離して伝える。

### 7 P2 完了済みtimeline signalが利用回数に比例して残る

**条件と影響：** 長寿命timelineに単調増加値をsignalし、完了確認は別VkFenceやqueue idleで行い、当該timeline自体を照会しない有効な利用で、完了済みpointとfence参照が累積する。後で当該timelineを照会すると、蓄積分の走査も必要になる。

- 製品：[nvkmd_nocturne_sync.c 338–356](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_sync.c#L338>)はtimelineの場合appendのみ。回収は[123–139](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_sync.c#L123>)を呼ぶ照会・待機等に依存する。
- queue idleは別のbinary syncを作って待つ：[vk_queue.c 1475–1490](<D:/Programes/llm vibe slop os/third_party/mesa/src/vulkan/runtime/vk_queue.c#L1475>)。
- 比較例：[NVIDIAの完了entry回収](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia-uvm/uvm_channel.c#L184>)。APIの同一実装を求めるのではなく、完了状態を回収する責務の比較である。

**修正方向：** signal追加時にも完了点を折り畳み、同じfenceで支配される点を安全に集約する。固定上限による拒否で解決しない。メモリ増加量は未測定。

### 8 P2 GPU準備前の最初のWebGL生成でCPU描画が固定される

initはnvrmを非同期起動してそのままGUIへ進む。nvrmはGPU初期化後にサービスを公開する。接続先がまだないと接続は直ちに失敗し、Mesaはsoftpipeを選んだ`fscreen`をプロセス全体で保存する。

**条件と影響：** GSP初期化より先にブラウザーがWebGLを作ると、その後GPUが使えるようになっても、同じブラウザープロセスでは新しいcontextを含めCPU描画を選び続ける。

- 起動：[init.c 41–71](<D:/Programes/llm vibe slop os/user/apps/init.c#L41>)、[nvrm.cのmain](<D:/Programes/llm vibe slop os/ports/nvidia/nvrm.c#L250>)、[サービス公開](<D:/Programes/llm vibe slop os/ports/nvidia/nvrm_serve.c#L129>)。
- 接続失敗：[libnvrm.c 35–57](<D:/Programes/llm vibe slop os/ports/nvidia/libnvrm.c#L35>)、[channel.c 73–84](<D:/Programes/llm vibe slop os/kernel/src/fs/channel.c#L73>)。
- 固定化：[nocturne_gl.c 133–156](<D:/Programes/llm vibe slop os/ports/mesa/nocturne_gl.c#L133>)。

**修正方向：** 未準備・非搭載・初期化失敗を区別し、準備通知や新規contextでの再評価を設計する。既存contextを無断で別backendへ付け替える提案ではない。

### 9 P2 GPUの有無を調べず性能制約付きWebGLを拒否する

**条件と影響：** `failIfMajorPerformanceCaveat=true`なら、GPU画面の作成を試す前に必ずNULLを返す。GPU対応追加前の「常にソフトウェア描画」という前提が残っており、GPU経路が利用可能でもこの指定を使うアプリはWebGLを得られない。

- 製品：[js_webgl.c 649–651](<D:/Programes/llm vibe slop os/user/libc/web/js_webgl.c#L649>)。GPUを試す処理は[nocturne_gl.c 137–146](<D:/Programes/llm vibe slop os/ports/mesa/nocturne_gl.c#L137>)にある。

**修正方向：** 実際に選んだbackendと製品として定義した性能上の問題を評価する。GPUだから必ず高速という判定へ置き換えない。これはGPU実測の遅さを指摘したものではなく、無条件拒否の指摘である。

### 10 P2 headless拡張の広告と必須依存が矛盾する

**条件と影響：** `VK_EXT_headless_surface`は広告されるが、必須依存の`VK_KHR_surface`はNocturneのビルドで広告されない。正しい拡張の組合せを要求したアプリがinstanceを作れない。WSI初期化も同じ条件で無効になる。

- 製品：[nvk_instance.c 29–58](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvk_instance.c#L29>)、[nvk_physical_device.h 107–111](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvk_physical_device.h#L107>)、[nvk_physical_device.c 1732–1736](<D:/Programes/llm vibe slop os/third_party/mesa/src/nouveau/vulkan/nvk_physical_device.c#L1732>)。
- 同梱のAPI定義：[vk.xml 25861–25868](<D:/Programes/llm vibe slop os/third_party/mesa/src/vulkan/registry/vk.xml#L25861>)。拡張非対応の拒否：[vk_instance.c 169–172](<D:/Programes/llm vibe slop os/third_party/mesa/src/vulkan/runtime/vk_instance.c#L169>)。

**修正方向：** Nocturne/headlessの有効化を他OSの窓システム条件から分離し、依存拡張・entrypoint・初期化を揃える。未接続の間は対応を広告しない。NVIDIAハードウェア資料の誤りではなく、Nocturne移植の内部不整合である。

### 11 P2 複数GPUのRMサーバーが同名サービスで衝突する

**条件と影響：** NVIDIA表示機能が複数あると、initはそれぞれのnvrmを起動する。しかし公開名はすべて`nocturne.nvidia.rm`で、2つ目のlistenは既存名エラーとなる。2つ目はGPU初期化後にサーバーを継続できず終了処理へ進む。

- 製品：[init.c 44–64](<D:/Programes/llm vibe slop os/user/apps/init.c#L44>)、[nvrm_protocol.h 14](<D:/Programes/llm vibe slop os/ports/nvidia/include/nvrm_protocol.h#L14>)、[nvrm_serve.c 129–131](<D:/Programes/llm vibe slop os/ports/nvidia/nvrm_serve.c#L129>)、[channel.c 56–71](<D:/Programes/llm vibe slop os/kernel/src/fs/channel.c#L56>)。

**修正方向：** 装置識別付きのサービス発見か、複数装置を管理するサービスを設計する。現状を単一GPU対応として説明することと、起動側が複数装置を同時に初期化する不整合の解消は別である。

### 12 P2 firmware除外指定が既存initrdの内容を無効化しない

**条件と影響：** firmware入りのビルド後、他の入力を変更せず`NVIDIA_FIRMWARE=`へ切り替えると、展開先のfirmwareは削除されるが、その構成変更は既存`initrd.tar`を古くする依存関係になっていない。既存tarを再利用したまま媒体を再作成し、除外指定に反してfirmwareが残る可能性がある。今回、実際の媒体に残っていると確認したものではない。

- 製品：[Makefile 443–463](<D:/Programes/llm vibe slop os/Makefile#L443>)。空になった`NVIDIA_FW_FILES`が前提条件から消えるだけで、除外状態のstampやmanifestがない。[472–478](<D:/Programes/llm vibe slop os/Makefile#L472>)の媒体生成はそのinitrdを使用する。
- 梱包器：[mkinitrd.py](<D:/Programes/llm vibe slop os/scripts/mkinitrd.py>)は呼ばれれば現在のツリーでtarを作り直す。問題は再実行の依存関係にある。

**修正方向：** firmwareの有無・版・入力の変更を梱包依存へ含め、配布用媒体の最終tar内を検査する。全体cleanや常時全再ビルドで隠さない。Makefile読込み時の削除には副作用があるため、この監査ではmakeの試行実行もしていない。

### 13 P2 一般GPU APIに装置能力由来ではない規模制限が残る

**条件と影響：** 一般GPU APIは512×512、32三角形を上限とし、座標にも独自の範囲を課す。固定バッファーと固定ABIに由来し、実装置の能力や利用可能資源から導いた上限ではない。Canvas 2Dの当該支援経路もこの条件に従う。

- 製品：[gpu_abi.h 5–6](<D:/Programes/llm vibe slop os/common/gpu_abi.h#L5>)、[gpu.c 404–414](<D:/Programes/llm vibe slop os/kernel/src/dev/gpu.c#L404>)、[js_canvas.c 393–419](<D:/Programes/llm vibe slop os/user/libc/web/js_canvas.c#L393>)。

**修正方向：** 実資源、overflow検査、分割処理に基づいた要求・資源管理へ拡張する。固定値の巨大化ではない。これはNVIDIAのNVK/WebGLが512辺に制限されるという意味ではない。

## 条件付きの契約不備と未確定のリスク

次は主な通常経路の不備と同じ確度で実害を断定しない。ただし基準との差として解消または対象外の明示が必要である。

### MMIOのキャッシュ属性

装置BARにはUCとWCの双方を許し、レジスター、ユーザードアベル、通常VRAMの意味を写像境界で区別していない。[syscall.c 452–465](<D:/Programes/llm vibe slop os/kernel/src/sys/syscall.c#L452>)、[os_pci.c 186–204](<D:/Programes/llm vibe slop os/ports/nvidia/os_pci.c#L186>)、[rm_api.c 172–173](<D:/Programes/llm vibe slop os/ports/nvidia/rm_api.c#L172>)。

基準は[レジスターとUDを強制UC](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-mmap.c#L590>)にする。危険なWC要求を境界で通せることは確認したが、現在の通常RM要求で必ずその属性が選ばれることは確認していない。BARのprefetchable属性だけでなく、範囲の用途を使った検査が必要である。

### 生存中の既存写像取消

[os_stubs.c 35–49](<D:/Programes/llm vibe slop os/ports/nvidia/os_stubs.c#L35>)は新規map禁止だけで、既存写像を失効しないまま成功と「すべて失効済み」を返す。基準の[全接続の既存写像取消](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-mmap.c#L820>)とは異なる。

確認したRM呼出しは動的電源経路であり、現在のNocturneではその機能を利用不可にしている。したがって通常Ada利用での発症確定ではない。なお、ドライバー終了時のカーネルによるclaim写像失効は別に実装されており、それまで未実装とは言わない。

### 割込み待ちとwait queue

- [pciuser.c 56–65](<D:/Programes/llm vibe slop os/kernel/src/dev/pciuser.c#L56>)の割込み無効化は待ちキューを起こさない。別スレッドが[無期限待機](<D:/Programes/llm vibe slop os/kernel/src/dev/pciuser.c#L343>)していれば、状態変化を再評価できない。現在のnvrmは50ms期限なので、その無期限停止条件には直接該当しない。
- [os.c 598–610](<D:/Programes/llm vibe slop os/ports/nvidia/os.c#L598>)は最初のwaiterが通知を消費し、基準の[全waiter完了](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/os-interface.c#L2286>)とは違う。確認した製品呼出しはfabric経路であり、Ada通常経路の複数waiter問題とは確定していない。

### 初期化と停止のハードウェア前提

[os_pci.c 323–332](<D:/Programes/llm vibe slop os/ports/nvidia/os_pci.c#L323>)はPCI Commandを読み、BMEとIOMMU未確認状態を記録するが、安全なDMA前提を確立したことにはならない。公開資料は[MEMORY_SPACEとBUS_MASTERを別ビット](<D:/Programes/cloned repos/open-gpu-doc/manuals/ampere/ga102/dev_nv_xve3g_fn0.ref.txt#L164>)として定義し、基準は[装置有効化](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-pci.c#L2232>)と[bus masteringの設定](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-pci.c#L2438>)を行う。

NocturneはDMA用に物理アドレスを返し、IOMMU変換状態を確定していない。これは「すべての実機でDMAが動かない」という断定ではなく、環境依存の前提が未確立という指摘である。過去に取り消されたBMEの早期書込みを単純に再導入する提案でもない。

終了時の[停止確認](<D:/Programes/llm vibe slop os/kernel/src/dev/pciuser.c#L71>)は、Transactions Pendingが0またはvendorがFFFFなら安全とみなす。基準には[SHHでBME解除の反映に必要な待機](<D:/Programes/cloned repos/open-gpu-kernel-modules-615.78.08/kernel-open/nvidia/nv-pci.c#L2992>)が明記される。ただし、そのハードウェア条件を現在のx86/Ada対象へそのまま適用できるとは確認していない。一時リンク障害と恒久撤去の区別も含め、異常終了後の再利用条件は実機を含む追加検討が必要である。停止不能時にDMAページを隔離する既存措置は保つべきである。

### ハンドル公開のC評価順依存

[syscall.c 361、382、421、476、1370](<D:/Programes/llm vibe slop os/kernel/src/sys/syscall.c#L355>)には、`new_handle(builder(..., &f), f)`という形がある。builderによる代入より先に別引数の`f`を評価する処理系では、成功した資源のハンドルを正しく公開できない。現行コンパイラーでの発現は確認していないが、builder完了と公開を別の文にして評価順へ依存しないようにすべきである。

## 実装済みの良い点

- RMをユーザー空間アプリへ移し、NocturneのネイティブPCI・共有オブジェクト・メッセージで接続している。Unix/POSIX互換層をOSへ導入する構成ではない。
- RMクライアントごとの`nvfp`、非管理者扱い、アリーナの範囲検査は存在する。認可の問題を理由に、これらも存在しないとは評価しない。
- DMA配列の全アドレス確認、サイズ・整列・失敗時の解放、RAM写像のキャッシュ属性整合が実装されている。
- PCI claimは通常initに限定され、kernel-owned装置との競合を拒否する。claim終了時のBAR失効と、停止確認に失敗したDMAの隔離もある。
- 実装置のクラス、GPC/TPC情報を問い合わせており、固定の架空GPU情報を返すだけの実装ではない。
- virglはCPU描画をGPUと偽らず、返信とfence、描画画素を確認してから能力を公開する。タイムアウト後のDMAを安易に再利用しない。
- GPU不在時のsoftpipeと、PCI claim中のsleep/hibernate拒否は、未対応を成功と偽装しない設計である。
- [Makefileのライセンス配置](<D:/Programes/llm vibe slop os/Makefile#L350>)は既存のCOPYINGとNOTICESを媒体へ入れる。本監査で第三者コードやfirmwareを追加していない。これは依存物全体の法的監査を完了したという意味ではない。

## 検証の射程と残る確認

今回はソースの比較と呼出し経路の確認のみを行った。ビルド、既存ホスト試験、QEMU、NVIDIA実機、Hyper-V、性能測定、画像撮影は実施していない。媒体とソースの一致も今回の成果としては認定していない。

既存試験の射程は次のとおりである。

| 試験 | 確認できる範囲 | 代用できないもの |
|---|---|---|
| [nvrm_dma_hosttest.c](<D:/Programes/llm vibe slop os/tests/nvrm_dma_hosttest.c>) | モックを使ったOS層の範囲、整列、解放処理 | PCI、IOMMU、キャッシュ、GPU DMA、GSP起動 |
| [nvrmtest.c](<D:/Programes/llm vibe slop os/tests/nvrmtest.c>) | QEMU eduに対するRMの初期化と非対応装置拒否 | NVIDIA GPUの初期化、命令実行、描画 |
| [gputest.c](<D:/Programes/llm vibe slop os/tests/gputest.c>) | ネイティブGPU API、virglとCPUの区別 | RM/NVK/Zink経路の成功 |
| 過去のソフトウェアWebGL表示 | CPUフォールバックの表示 | NVIDIAのチャネル・同期・性能 |

修正後の受入は、次のまとまりで行うのが妥当である。ここに記載した試験は今回実行したものではない。

1. **隔離と契約**：異なるアプリのRM資源複製が明示共有なしに拒否されること、読み取り専用写像と再写像の権利、RMエラー伝播。
2. **実装置の送信と寿命**：実BAR配置、CPUからVRAMへ更新して直ちにsubmitする処理、複数queueとtimelineの長時間利用、正常終了と異常終了。
3. **起動と公開能力**：GPU準備前後のWebGL、性能制約付きcontext、広告されたVulkan拡張の依存関係、複数GPU。
4. **表示と応答**：実GPUの画素結果、読み戻し込みの時間、入力・スクロール・窓操作。virglの遅延完了でもGUIが応答すること。GPU実行件数だけで高速化とは判定しない。
5. **梱包**：firmwareありからなし、その逆への差分ビルドで最終initrdの内容が指定と一致すること。

## 監査で変更したもの

この監査報告書のみを追加した。製品ソース、設定、生成物、起動媒体は変更していない。

開始時から存在した[build.ps1](<D:/Programes/llm vibe slop os/build.ps1>)と[scripts/build.py](<D:/Programes/llm vibe slop os/scripts/build.py>)の差分は保全した。監査中に[既存LEDGER](<D:/Programes/llm vibe slop os/docs/nvidia/LEDGER.md>)の差分も現れたが、本監査の担当者は編集しておらず、そのまま保全した。

**総合判定：実装の土台と製品への接続はある。汎用対応・安全な隔離・安定したGPU支援の受入はまだ成立していない。まず既存経路の認可とメモリ契約、送信順序、応答性を直し、その後に表示・動画・世代拡張を進めるべきである。**
