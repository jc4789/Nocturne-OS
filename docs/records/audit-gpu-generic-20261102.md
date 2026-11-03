# GPU と画面の汎用の層と、Mesa と NVIDIA の決め打ちの監査

## 依頼文（原文のまま）

Nocturne OS（`D:\Programes\llm vibe slop os`）の、GPU と画面の汎用の層と、Mesa と NVIDIA の決め打ちを、読むだけで敵対的に監査してください。返事と報告は全部日本語で書きます。識別子とパスは元の表記のままです。

## 依頼者の言葉（原文）
"Our graphics pipeline needs to be audited. and needs to be assumed wrong. ... use a couple subagents to do a read only adversarial audit. have them assume that our code base is a mess and that "fake" implmentations of things are rampent."

"OS settings and driver settings should NOT get confused. and there will be setting over lap."

"the overlaped settings will change one another if one is changed.  we have no generic/default gpu drivers or display handing and are randomely hard codeding mesa and nvidia stuff. that is a big issue"

ほかの担当が、合成器、画面のモードの管理、音を見ています。あなたの担当は、汎用の層の有無と、決め打ちの全体の棚卸しです。

## 読むだけ
- コード、設定、媒体、git の状態を変えません。ビルド、QEMU、Hyper-V、スクリプトの実行をしません。OS は動かせないので、証拠はソースだけです。
- 書いてよいファイルは報告の `docs/records/audit-gpu-generic-20261102.md` の一つだけです。頭にこの依頼文を全文そのまま写し、続けて報告の全文を書きます。

## 監査の構え
- コードは散らかっていて、偽の実装がはびこっていると仮定します。名前、コメント、文書、コミットメッセージ、試験の合格を信じません。依頼者の「we have no generic/default gpu drivers」も、そのまま受け入れず否定もせず、道を辿って確かめます。「汎用」「抽象」と名の付く口があっても、中身が一社の形をしていないか、実装が一つしかないのに口だけあるのではないかを確かめます。表から呼ばれる関数と `SYS_*` の番号での呼出しは codegraph に出ないので、`rg` でも探します。
- 証拠の無い所見は出しません。各所見に `file:line` を付けます。数を稼ぎません。本物だと確かめた部分は、本物と書きます。確かめられないことは、確かめられないと書きます。

## 守る線（監査の物差し）
先に `AGENTS.md` と `.claude/rules/map.md` を読みます。`.claude/rules/nvidia.md` も読みます。要点は次のとおりです。
- 層は分かれています。カーネルは GPU を知らず、`SYS_PCI_*`・`SYS_DMA_*` を出すだけです。`nvrm` がアプリのドライバー、GPU の利用者側の像は `C:\Nocturne\Drivers\nvk` です。VM の処理は `kernel/src/vm/vm.h` の後ろだけです。
- GPU の道は決め打ちせず、継ぎ目（抽象化の層）で支えます。
- 書く前の問いは「これは誰のものか。二つ目が来たら呼ぶ側は変わるか」です。一つの呼ぶ側のための包み、決め打ちの値、ある環境の都合を別の部品へ持ち込むことは禁止です。
- 対象は実機で、機械を決め打ちしません。依頼者の機械は RTX 4070 ですが、それに合わせた設計は誤りです。VM で通ることは実機で動く証拠になりません。
- 文書は古いことがあります。事実は今のコードで確かめます。

## 読む場所
`rg` で全体を探す語の例：`nvidia`、`NVIDIA`、`nvrm`、`nvk`、`nvkms`、`nouveau`、`mesa`、`llvmpipe`、`lavapipe`、`gallium`、`0x10de`、`10de`。`third_party/` と `ports/` の中の、そのドライバーの本体は棚卸しの対象外ですが、`ports/` から外へ出る口は対象です。
次は必ず読みます。`common/gpu_abi.h`、`common/display_abi.h`、`user/libc/vulkan.c`、`user/libc/driver_image.c`、`user/include/nocturne_gpu.h`、`user/apps/services.c`（宣言の表 `declared[]` と `gpu` の項目）、`user/apps/display.c`、`kernel/src/dev/fb.c`、`kernel/src/gui/screenmode.c`、`kernel/src/gui/wm.c`（`screen_ops` の使い方）、`kernel/src/dev/pciuser.c`、`kernel/src/mm/shared.c`、`ports/nvidia/nvk_image.c`、`ports/nvidia/display.c`、`ports/mesa/nocturne_os.c`、`ports/mesa/nocturne_gl.c`、`ports/mesa/nvk.mk`、`Makefile`、`scripts/layout.py`、`user/libc/web/js_webgl.c`、`user/apps/settings.c`。ほかに要る所は辿ってください。

## 答えてほしいこと
1. 決め打ちの棚卸し：ドライバーの外（`kernel/`、`common/`、`user/`、`scripts/`、`Makefile`、設定の表、ABI の文書）に現れる NVIDIA、Mesa、nvk、nvkms、llvmpipe の全部を一覧にします。各行に、`file:line`、何を決め打ちしているか、そこに在ってよいか、誰の持ち物であるべきかを書きます。名前に出なくても一社の形をしている口（NVIDIA の語の値、NVIDIA の手順を前提にした ABI、`0x10de` の照合）も含めます。
2. 汎用の層の有無：次の機械で、起動、画面、モードの変更、GL、Vulkan、計算がどうなるかを、それぞれ呼出しの順で確かめます。AMD の GPU だけ、Intel の GPU だけ、対応外の古い NVIDIA の GPU だけ、GPU のドライバーの起動に失敗した機械、GPU が無い機械、NVIDIA と AMD の両方がある機械。
3. 既定のドライバー：firmware の framebuffer の道は既定のドライバーとして成り立っているか。ベンダーのドライバーが居ないときの画面の扱い（解像度、Hz、複数画面）。ベンダーのドライバーが起動中や動作中に落ちたときに既定の道へ戻るか。ソフトウェアの GL と Vulkan は既定として居るか、選ばれ方は実行時か組みの時か。
4. 継ぎ目の検査：`struct n_gpu_driver`、`screen_ops`、サービスの `gpu` の項目、像の選び方について、二社目を足すと呼ぶ側の何が変わるかを具体的に書きます。口はあるが実装が一つで、形が一社に合っている所を挙げます。GPU の機能を列挙する者、どのドライバーを起こすかを決める者は誰で、どこに書かれているかを述べます。
5. ドライバーの設定と OS の設定：ドライバーの固有の設定を置く場所と、OS の設定へつなぐ継ぎ目があるか。ここは画面の管理の担当も見ているので、汎用の層の観点からだけ書きます。
6. 偽物の一覧：何もしない口、呼ぶ者のいない道、実装の無い抽象、組みの時の選択を実行時の選択に見せている所。

## 報告の形
各所見に、重さ（致命、重い、中、軽い）、種類、`file:line`、根拠、起きる条件、確信（コードで証明、可能性あり）を書きます。最後に、本物と確かめた部分と、読めなかった範囲を書きます。修理の案は、原因を消す形で短く書きます。依頼者は今すぐ多くのベンダーを動かすことを求めていないので、案は継ぎ目と既定の道を中心に書きます。

最後の返事は、報告のパスと、重い順の要点を 15 行以内で返します。

---

# 報告

証拠はソースだけです。OS は動かしていません。実機と QEMU の結果はありません。

## 結論

依頼者の「we have no generic/default gpu drivers or display handing」は、半分が事実で、半分は違います。

- **画面の層は本物の汎用です。** カーネルにベンダーの判定はありません。`struct screen`、`n_pci_*screen*`、`gui/screenmode.c`、サービス `display` にベンダーの語はありません。
- **ただし、画面の層の実機の使い手は nvrm の一つです。** nvrm は出力 0 だけを使います。多出力の道は、実機では GPU が二つあるときにしか通りません。
- **GPU の層に汎用はありません。** 起こすドライバーの選択は根のサービスに決め打ちです。ローダーは像を一つしか持ちません。`struct n_gpu_driver` の中身は Vulkan の ICD と Mesa の log と nvrm の直接表示です。OS 全体の GPU の一覧を持つ者は居ません。
- **既定の道は片方だけあります。** firmware の framebuffer は、ベンダーのドライバーが居ない機械の既定として働きます。ソフトウェアの GL も実行時に選ばれます。ソフトウェアの Vulkan と計算の既定はありません。
- **既定の道は、落ちたときの戻り先になっていません。** ドライバーが動作中に落ちると、カーネルは firmware の面へ描き直します。表示エンジンを戻す者は居ないので、モニターに映らない可能性があります。コードのコメントとログ文が自らそう書いています。
- **決め打ちは「でたらめ」というより、一箇所に集まっています。** カーネルと `common/` に現れるベンダーの名前はコメントだけです。根のサービスの一行、ローダー、私的な直接表示の口、firmware の一覧が、実害のある決め打ちです。

## 所見（重い順）

### 致命

#### F1. ドライバーが動作中に落ちると、既定の道へ戻らない

- 種類：既定の道の欠け。
- 場所：`kernel/src/dev/pciuser.c:287-297`、`kernel/src/dev/pciuser.c:475-480`、`kernel/src/dev/pciuser.c:504-559`、`kernel/src/dev/pciuser.c:679`、`kernel/src/dev/pciuser.c:757-770`、`kernel/src/dev/fb.c:342-343`、`common/abi.h:484`、`ports/nvidia/os_pci.c:356-358`、`ports/nvidia/display.c:1239-1259`、`user/apps/services.c:65-67`。
- 根拠：
  - claim が終わると出力の面が外れ、画面 0 は firmware の面へ戻ります（`fb.c:342-343`）。
  - 表示エンジンを元へ戻す者は居ません。`pciuser.c:296-297` のコメントは「the driver that left reprogrammed it and nothing sets it back」と書きます。
  - `pciuser.c:477-479` は同じ内容をログへ出します。
  - 画面の BAR を持つ機能には FLR をしません（`pciuser.c:757-770`）。SBR もしません（`pciuser.c:679`）。
  - 次のドライバーへの知らせ `N_PCI_NEEDS_RECOVERY` は、`os_pci.c:356-358` でログになるだけです。
  - 戻る道は二つだけです。正常な終わりの `hand_back`（`display.c:1239-1259`）と、起こし直しの成功です。
  - 起こし直しは 2 回で（`services.c:66` の `on_failure={2,2000,60000}`）、尽きると FAILED のままです。
- 起きる条件：NVIDIA の出力で画面を出している機械で、nvrm が異常終了し、起こし直しが失敗する。起こし直しが成功する場合も、その間の画面は映らない可能性があります。
- 確信：戻す処理が無いことはコードで証明。モニターに実際に何が映るかは可能性あり（実機で確かめていません）。

### 重い

#### F2. 起こすドライバーの選択が、根のサービスの中に決め打ちされている

- 種類：決め打ち、層の混ざり。
- 場所：`user/apps/services.c:24`、`user/apps/services.c:59`、`user/apps/services.c:65-67`、`user/apps/services.c:273`、`ports/nvidia/nvrm.mk:144`、`ports/nvidia/include/nvrm.h:33-35`。
- 根拠：
  - サービスの管理者が `nvrm.h` を include します。
  - `nvrm.mk:144` は `services.o` の組みにベンダーの見出しの道を足します。
  - 判定は `vendor == 0x10de` と class の組です（`nvrm.h:33-35`）。
  - `function_matches` を持つ項目は `gpu` の一つだけです。
  - 別の会社の項目を足すには、根のサービスの見出し、関数、表を変えて組み直します。
  - PCI の device の番号も世代も見ないので、対応外の古い NVIDIA にも nvrm を起こします。
- 起きる条件：どの機械でも。二社目を足すとき、対応外の NVIDIA があるとき。
- 確信：コードで証明。

#### F3. Vulkan のローダーは像を一つしか持たず、次の像へ進まない

- 種類：実装が一つの抽象、誤った選択。
- 場所：`user/libc/vulkan.c:19-20`、`user/libc/vulkan.c:37`、`user/libc/vulkan.c:45-62`、`user/include/nocturne_gpu.h:6-10`、`common/services_abi.h:67`。
- 根拠：
  - `driver` は静的な変数の一つです（`vulkan.c:19`）。
  - `n_gpu_driver_get` の繰り返しは `r == -N_ERR_NOT_FOUND` の間だけ続きます（`vulkan.c:54`）。
  - 最初の像の読み込みが成功すると、その像の装置が無くても止まります。
  - 読み込みが `NOT_FOUND` 以外で失敗しても、次の像を試さずに止まります。
  - 飛ばすのは `N_SERVICE_ABSENT` だけです（`vulkan.c:55`）。FAILED のサービスの像も読みます。
  - 失敗は `driver_failure` に残り続け、そのプロセスでは二度と試しません（`vulkan.c:20,37`）。
  - 装置を束ねる処理はありません。
- 起きる条件：像を持つサービスが二つあるとき。サービスが FAILED のとき（無駄な読み込みになります。費用は測っていません）。
- 確信：コードで証明。

#### F4. `struct n_gpu_driver` は GPU の口を名乗るが、中身は Vulkan と Mesa と nvrm の形

- 種類：一社の形の抽象。
- 場所：`common/gpu_abi.h:60-89`、`ports/nvidia/nvk_image.c:15-25`、`ports/nvidia/nvk_image.c:41-55`、`user/libc/vulkan.c:33-35`。
- 根拠：
  - `negotiate`、`get_instance_proc_addr`、`get_device_proc_addr` は Vulkan の ICD の三つです。
  - `log_hold`、`log_release`、`log_free` は Mesa の `mesa_log_hold` の形です。
  - `ready` は有無だけで、名前も装置の一覧も持ちません。
  - `scanout_open(void)` は GPU も画面も指定しません。実装は「最初の ready な GPU」です（`nvk_image.c:25`、`ports/nvidia/libnvrm.c:97-131`）。
  - 計算の口はありません。像の口の abi は表全体で一つです。
  - 実装は `nvk_image.c` の一つです。
- 起きる条件：二社目、計算、Vulkan でない道を足すとき。
- 確信：コードで証明。`docs/records/gpu-compute-20261102.md` も同じことを書いています。

#### F5. 直接表示は NVK 専用の私的な口で、画面も形式も渡さない

- 種類：一社の形、黙った前提。
- 場所：`common/gpu_abi.h:81-88`、`ports/mesa/nocturne_gl.c:477`、`ports/mesa/nocturne_gl.c:502-551`、`ports/mesa/nocturne_gl.c:614`、`ports/nvidia/display.c:1596`、`user/libc/web/js_webgl.c:753-767`、`third_party/mesa/src/gallium/include/frontend/winsys_handle.h:22-23`、`third_party/mesa/src/vulkan/util/vk_internal_exts.h:128-140`、`third_party/mesa/src/gallium/drivers/zink/zink_resource.c:57-58`、`third_party/mesa/src/gallium/drivers/zink/zink_resource.c:1016-1018`、`third_party/mesa/src/gallium/drivers/zink/zink_resource.c:1139-1146`、`third_party/mesa/src/nouveau/vulkan/nvk_device_memory.c:151-153`、`third_party/mesa/src/nouveau/vulkan/nvkmd/nvkmd.h:482-484`。
- 根拠：
  - `scanout_create` は寸法と pitch と大きさを返します。形式は返しません（`gpu_abi.h:85-86`）。
  - `image_copy_buffer` は GL の RGBA8 のバイトの並びをそのまま写します（`nocturne_gl.c:614`）。X8B8G8R8 を黙って前提にしています。
  - その形式は nvrm が決めます（`display.c:1596`）。ABI の文書も「NVIDIA の X8B8G8R8」と書きます（`docs/nocturne-native-abi.md:638`）。
  - `WINSYS_HANDLE_TYPE_NOCTURNE_SCANOUT` と `VkImportMemoryNocturneScanoutInfoMESA` は、Zink と NVK の中に足した私的な import です。`winsys_handle.h:22-23` は「nvrm direct-scanout image」と書きます。
  - wm は画面ごとに direct を持ちます（`kernel/src/gui/wm.c:811-814`）。GL の口は画面を指定せず、`js_webgl.c:765-767` は大きさだけを比べます。
- 起きる条件：二社目の Vulkan が来たとき。形式の違う画面。GPU が二つの機械。
- 確信：コードで証明。

#### F6. OS に GPU の一覧の持ち主が居ない

- 種類：持ち主の欠け。
- 場所：`common/gpu_abi.h:1-18`、`common/abi.h:60`、`common/abi.h:62-63`、`common/abi.h:136`、`user/apps/services.c:273`、`third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c:549`、`third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c:565`、`user/apps/settings.c`（GPU の頁は無い）。
- 根拠：
  - `SYS_GPU_INFO` の答えは virgl か CPU だけです（`gpu_abi.h:6-18`）。製品では意味がありません（F16）。
  - NVK は自分のプロセスの中で RM に聞きます。
  - 「この機械に何の GPU があり、どれが動き、なぜ動かないか」に答える口がありません。
  - 分かるのはサービスの状態と system log だけです。
  - 設定の窓に GPU の頁はありません（`rg` の `gpu|vulkan|driver|nvidia` で画面の鍵しか出ません）。
- 起きる条件：どの機械でも。
- 確信：コードで証明。

#### F7. ソフトウェアの Vulkan と計算の既定が無い

- 種類：既定の道の欠け。
- 場所：`user/libc/vulkan.c:72`、`user/libc/vulkan.c:78`、`user/libc/vulkan.c:87-89`、`ports/mesa/nocturne_gl.c:269-272`、`ports/mesa/nocturne_gl.c:311-313`、`scripts/mksdk.sh:52-55`。
- 根拠：
  - 像が無ければ `VK_ERROR_INCOMPATIBLE_DRIVER` で、版は 1.0、拡張は 0 個です。これは正直な答えです。
  - `lavapipe` は `user`、`scripts`、`Makefile`、`kernel`、`common` のどれにも現れず、`ports/mesa/mesa.mk` の源の一覧にもありません。
  - SDK は Vulkan の見出しを配ります（`mksdk.sh:52-55`）。AMD と Intel と GPU の無い機械では、そのプログラムは動きません。
  - GL は ES 3.0 までで、計算シェーダーがありません（`nocturne_gl.c:269-272,311-313`）。
- 起きる条件：動く NVIDIA が無い全部の機械。
- 確信：コードで証明。

#### F8. firmware の framebuffer が無いと、全部が立たない

- 種類：既定の道の前提。
- 場所：`kernel/src/boot.c:172-175`、`kernel/src/dev/fb.c:102`、`kernel/src/dev/fb.c:109`、`kernel/src/dev/fb.c:617`、`kernel/src/gui/wm.c:2611-2614`、`kernel/src/gui/wm.c:2621-2624`、`kernel/src/gui/wm.c:2631`。
- 根拠：
  - `fb_buffer_check` は `!fb.addr` で断ります（`fb.c:109`）。ドライバーが自分の面を渡せません。
  - wm は画面が無いと `return` します（`wm.c:2611-2614`）。
  - `screen_changed_hook` はその後です（`wm.c:2631`）。後から画面が来ても desktop は立ちません。
  - panic の画面も出ません（`fb.c:617`）。
- 起きる条件：Limine が framebuffer を渡さない機械。そういう機械の実例は確かめていません。
- 確信：コードで証明。

#### F9. 既定の道の力が足りない

- 種類：既定の道の限界。
- 場所：`kernel/src/boot.c:173`、`kernel/src/dev/fb.c:44-69`、`kernel/src/dev/fb.c:259-263`、`kernel/src/dev/fb.h:65-66`、`kernel/src/dev/fb.h:81`、`kernel/src/gui/wm.c:1659-1664`、`kernel/src/gui/screenmode.c:184`、`user/apps/display.c:153-157`、`user/apps/display.c:356`、`kernel/src/dev/fb.c:630-642`、`kernel/src/dev/pm.c:250`。
- 根拠：
  - `framebuffers[0]` だけを使います（`boot.c:173`）。Limine が二つ渡しても一つです。
  - モードの申し出はありません。解像度は Limine が決めたものに固定です。
  - 拍は EDID から推します（`fb.c:44-69`）。同じ大きさに複数の拍があれば 0 です。
  - 拍が 0 なら周期がなく、頼まれたときだけ描きます（`fb.h:81`、`wm.c:1659-1664`）。
  - バッファーは一枚で、見えているものへ描きます（`fb.h:65-66`）。垂直帰線はありません。
  - 実機の display に `resume` はなく、眠りは出しません（`fb.c:630-642`、`pm.c:250`）。これは NVIDIA の機械でも同じです。
- 起きる条件：ベンダーのドライバーが居ない機械。眠りは実機の全部。
- 確信：コードで証明。Limine の解像度の選び方は読んでいません。

#### F10. 多画面の口の唯一の使い手が、出力 0 だけを使う

- 種類：実装が一つの口。
- 場所：`ports/nvidia/display.c:321-378`、`ports/nvidia/display.c:713`、`ports/nvidia/display.c:1111`、`ports/nvidia/display.c:1660`、`kernel/src/dev/pciuser.c:358-370`、`kernel/src/vm/vbe.c:169`。
- 根拠：
  - `choose_display` は接続された最初の一つを選びます。`display.c:375-377` は「nvrm shows the desktop on display 0x%08x only」とログへ書きます。
  - `numDisplays = 1` です（`display.c:713`）。
  - カーネル、`screenmode.c`、サービス `display` の多画面の論理は、実機では GPU が二つあるときだけ通ります。
  - VM の vbe は二つ目の画面を作れますが（`vbe.c:169`）、実機の証拠になりません。
- 起きる条件：一つの GPU に二つのモニターをつないだ機械。二つ目は映りません。
- 確信：コードで証明。詳しくは画面の管理の担当の範囲です。

#### F11. ドライバーが起動の途中で止まると、画面が止まり続ける

- 種類：既定の道へ戻らない。設計判断。
- 場所：`kernel/src/dev/fb.c:407-410`、`kernel/src/dev/fb.c:502`、`kernel/src/dev/pciuser.c:182-185`、`user/apps/settings_table.h:28-29`、`docs/nocturne-native-abi.md:612-618`。
- 根拠：
  - `n_pci_hold_screen` の間は合成が止まります（`fb.c:502`）。
  - 時間では終わりません（`fb.c:407-410`）。
  - 時間切れの設定 `display.hold-seconds` の既定は 0 です（`settings_table.h:28-29`）。
  - 理由は ABI の文書にあります。RM が BAR を切り替える間、CPU が firmware の面へ触れると止まるからです。
- 起きる条件：nvrm が hold の間に、死なずに止まる。
- 確信：既定で解けないことはコードで証明。止まる頻度は可能性あり。この判断を覆すべきかは決めません。問題は、止まったことを別の者が見つける道が無いことです。

#### F12. GL は GPU の喪失を知らない

- 種類：既定の道へ戻らない。
- 場所：`ports/mesa/nocturne_gl.c:103`、`ports/mesa/nocturne_gl.c:141`、`ports/mesa/nocturne_gl.c:186`、`user/libc/web/js_webgl.c:4399`。
- 根拠：
  - `gpu_fscreen` は一度できると、そのプロセスで以後ずっと使います（`nocturne_gl.c:141,186`）。
  - `ports/mesa/nocturne_gl.c`、`nocturne_gl.h`、`js_webgl.c` に `reset_status`、`DEVICE_LOST` はありません。
  - `gl->lost` を真にするのは `WEBGL_lose_context` だけです（`js_webgl.c:4399`）。
  - nvrm が落ちても、WebGL の頁へ `webglcontextlost` が届く道が見えません。CPU へ移る道もありません。
- 起きる条件：動作中に nvrm が落ちる、または起こし直される。
- 確信：`nocturne_gl.c` と `js_webgl.c` に道が無いことはコードで証明。Zink の中で何が起きるかは読んでいません。

#### F13. ドライバーの設定に置き場が無く、在る口を埋める者も居ない

- 種類：持ち主の欠け、何もしない口。
- 場所：`ports/nvidia/nvrm.c:222-239`、`ports/nvidia/nvrm.c:271`、`ports/nvidia/nvkms_os.c:46-48`、`ports/nvidia/nvkms_os.c:180-211`、`ports/nvidia/os.c:1060-1096`、`user/apps/services.c:363-365`、`common/abi.h:357-363`、`ports/mesa/mesa.mk:108`、`third_party/mesa/src/util/os_misc.c:245`、`third_party/mesa/src/util/os_misc.c:248`、`user/apps/settingsd.c:889`、`common/settings_abi.h:17-26`。
- 根拠：
  - nvrm は設定を `getenv("NVRM_REGISTRY_DWORDS")` で読みます（`nvrm.c:271`）。
  - NVKMS は `NVKMS_OPTIONS` で読みます（`nvkms_os.c:46-48`）。
  - サービスの管理者の `n_launch` は `environment` を渡しません（`services.c:363-365`）。`services.c` に `.environment` はありません。だから管理者が起こした nvrm では、どちらも常に空で、全部が既定値です。
  - Mesa は `WITH_XMLCONFIG=0` で（`mesa.mk:108`）、`os_get_option` は `getenv` です（`os_misc.c:245,248`）。起こす者が環境を渡さない限り空です。窓から起こすプログラムへ環境を渡す者が居るかは確かめていません。
  - `ports/nvidia`、`ports/mesa`、`nvkmd/nocturne` に `n_setting` はありません。
  - 設定のサービスの領域に GPU はありません（`settings_abi.h:17-26`）。
  - `gpu` は `N_CLASS_USER` なので、領域の鍵を宣言できません（`settingsd.c:889`）。
- 起きる条件：ドライバーの固有の値を変えたいとき。
- 確信：コードで証明。

### 中

#### F14. firmware の一覧が RTX 4070 の世代に合わせてある

- 種類：機械に合わせた設計。
- 場所：`scripts/layout.py:28`、`scripts/layout.py:101`、`Makefile:585-594`、`tests/layout_firmware_test.py:28-29`、`third_party/nvidia-open/src/nvidia/arch/nvalloc/common/inc/nv-firmware.h:104`、`third_party/nvidia-open/src/nvidia/arch/nvalloc/common/inc/nv-firmware.h:117`、`ports/nvidia/os.c:1014-1031`。
- 根拠：
  - `FIRMWARE_FILES` は `gsp_ga10x.bin` と `ucodes_ga10x.bin` と `LICENSE` に固定です。
  - `local/firmware/nvidia/615.78.08/firmware/` には `gsp_tu10x.bin` と `ucodes_tu10x.bin` もあります。C: に入りません。
  - RM は世代で firmware の名前を選びます（`nv-firmware.h:104,117`）。無ければ `os.c:1014-1031` が「firmware not found」を出します。
  - `third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_pdev.c:35-38` の `chipset_family` は TU10x を知っています。
  - `Makefile:588` のコメントは「the RTX 4070」と機種を書きます。
- 起きる条件：Turing の世代の機械。診断の状態で RUNNING になり（F15）、GL は CPU へ行きます。
- 確信：コードで証明。その世代が NVK で実際に動くかは確かめていません。

#### F15. GPU の起動に失敗しても、サービスは RUNNING になる

- 種類：偽の成功。
- 場所：`ports/nvidia/nvrm.c:324-329`、`ports/nvidia/nvrm.c:405`、`user/apps/settings.c:299-302`、`user/apps/settings.c:309`。
- 根拠：
  - `init_adapter` が失敗すると `bring_up` は `return true` です（`nvrm.c:328`）。コメントは「still served for diagnosis」と書きます。
  - その後 `n_service_ready()` を呼びます（`nvrm.c:405`）。
  - 設定の窓のサービスの頁は、動いていると見せます。
  - 使えないことは `nvrm_device_ready` を聞く者にしか分かりません。
- 起きる条件：firmware が無い、GSP が起きないなど。
- 確信：コードで証明。

#### F16. `SYS_GPU_*` は製品で動かないのに、ブラウザーがその費用を払う

- 種類：偽の口、費用。
- 場所：`common/abi.h:60`、`common/abi.h:62-63`、`common/abi.h:136`、`kernel/src/kernel.h:71-77`、`kernel/src/vm/virtio_gpu.c:576`、`kernel/src/vm/virtio_gpu.c:580`、`kernel/src/vm/virtio_gpu.c:692-696`、`kernel/src/sys/syscall.c:487-498`、`kernel/src/sys/syscall.c:1151-1165`、`user/libc/web/js_canvas.c:402-403`、`user/libc/web/js_canvas.c:669-682`、`tests/gputest.c:50`。
- 根拠：
  - 製品のカーネルでは `test_flag` は `false` です。`virtio_gpu.c:580` は `gpu-enable` の旗を要るので、`ready` になりません。
  - 自らの名は「CPU display; no verified virgl pipeline」です（`virtio_gpu.c:576`）。
  - `js_canvas.c:669-682` は 64×64 以上の不透明な矩形ごとに `malloc(w*h*4)` と `gpu_render` の syscall を払います。その後でソフトウェアの道へ進みます。
  - カーネルは頁ごとに確かめてから `kmalloc(bytes)` します（`syscall.c:487-498,1151-1165`）。
  - `gputest.c:50` は「使えないことが正直」を合格にします。
- 起きる条件：全部の実機で、canvas の矩形を描くたび。
- 確信：コードで証明。実時間は測っていません。

#### F17. `dev/gpu.h` が VM の境界を迂回する

- 種類：層の混ざり。
- 場所：`kernel/src/init.c:8`、`kernel/src/init.c:42`、`kernel/src/sys/syscall.c:28`、`kernel/src/vm/virtio_gpu.c`、`tests/check_boundaries.py:50`、`tests/check_boundaries.py:91`。
- 根拠：
  - 実装は `vm/virtio_gpu.c` です。呼ぶ側は `vm/vm.h` を通らず、`dev/gpu.h` を include します。
  - `check_boundaries.py` は `"vm/..."` の include しか見ないので、これを通します。
- 起きる条件：常に。
- 確信：コードで証明。

#### F18. framebuffer が無いと `screen_new` が範囲外へ書く

- 種類：不具合。
- 場所：`kernel/src/dev/fb.c:253-254`、`kernel/src/dev/fb.c:276-281`、`kernel/src/dev/fb.c:288-289`、`kernel/src/dev/fb.c:303`、`kernel/src/dev/pciuser.c:364`、`kernel/src/dev/pciuser.c:1011`、`kernel/src/mm/heap.c:426-429`。
- 根拠：
  - `screens_boot` は `fb_init` からしか呼ばれません。framebuffer が無いと `screens_room` は 0 のままです。
  - `nscreens == screens_room` で `room = 0` になり、`kmalloc(0)` が最小の slab を返します。
  - `screens_room` は 0 のままなので、次から伸ばさずに `screens[nscreens]` へ書きます。
  - `n_pci_screen_modes` の道（`pciuser.c:1011` から `:364`）がここへ来ます。
- 起きる条件：framebuffer が無く、NVIDIA の機能がモードを申し出る。
- 確信：範囲を外れる計算はコードで証明。何番目で slab を超えるかは最小の大きさによります（確かめていません）。

#### F19. llvmpipe の有無は組みの時に決まる

- 種類：組みの時の選択。
- 場所：`ports/mesa/llvmpipe.mk:17`、`Makefile:344-345`、`Makefile:350-351`、`ports/mesa/mesa.mk:94`、`ports/mesa/mesa.mk:106`、`ports/mesa/nocturne_gl.c:116-123`。
- 根拠：
  - `LLVMPIPE` は `$(LLVM_BUILD)/lib/libLLVMMCJIT.a` があるかどうかで決まります。
  - 無ければ黙って softpipe だけの OS ができます。
  - 実行時の `cpu_screen` は実行時の選択に見えますが、片方は組みの時に消えています。
  - ソフトウェアの GL の速さが、組む機械の道具の有無で変わります。
- 起きる条件：LLVM の移植を組んでいない機械で組むとき。
- 確信：コードで証明。

#### F20. Mesa の語が公開の見出しと libc に入り込む

- 種類：持ち物の混ざり。
- 場所：`user/include/nocturne.h:418-420`、`third_party/mesa/src/util/u_call_once.c:23`、`third_party/mesa/src/mesa/glapi/shared-glapi/core.c:247`、`user/libc/web/js_webgl.c:10`、`user/libc/web/js_webgl.c:13-15`、`user/libc/web/js_webgl.c:981`、`ports/mesa/mesa.mk:127-133`、`Makefile:346-352`、`scripts/mksdk.sh:8-9`。
- 根拠：
  - 公開の見出しの `N_TLS_GL_DISPATCH`、`N_TLS_GL_CURRENT`、`N_TLS_CALL_ONCE` は Mesa の名です。SDK に入ります。
  - `js_webgl.c` は Mesa の内部の `util/half_float.h` を使います（`_mesa_float_to_half`、`js_webgl.c:981`）。
  - そのために `WEBGL_OBJ` の組みに `MESA_DEFS` と `-I$(MESA)/src` が要ります（`mesa.mk:127-133`）。
  - `nocturne_gl.h` の呼ぶ者はブラウザーの `js_webgl.c` と試験 `tests/nvk_contracttest.c` だけです。
  - SDK に GL の見出しはありません（`scripts/mksdk.sh` が写すのは Vulkan の見出しだけです）。公開の GL の口はありません。
- 起きる条件：常に。Mesa を入れ替えるとき。
- 確信：コードで証明。lazy な member なので、GL を使わないプログラムに Mesa は入りません（`Makefile:346` のコメント。組みの結果は確かめていません）。

#### F21. 読み手だけがいる口

- 種類：呼ぶ者のいない道。
- 場所：`kernel/src/gui/screenmode.c:102`、`kernel/src/gui/screenmode.c:171`、`kernel/src/gui/wm.c:1660`、`kernel/src/gui/wm.c:1665`、`user/apps/settings.c:954`、`kernel/src/dev/fb.c:112`、`kernel/src/dev/fb.c:124`、`user/apps/settings.c:757`、`ports/nvidia/display.c:241-243`、`common/abi.h:484`、`ports/nvidia/os_pci.c:356-358`。
- 根拠：
  - `N_SCREEN_MODE_VRR` を立てる者は、`ports`、`user`、`kernel/src`、`tests` の `rg` にありません。カーネルと設定の窓は読むだけです。
  - `N_SCANOUT_XRGB2101010` を出す者もありません。`display.c:241-243` は 8 bit の `XRGB8888` だけです。
  - `N_PCI_NEEDS_RECOVERY` はログだけです（F1）。
- 起きる条件：VRR や 10 bit のモニターでも、その値は選ばれません。
- 確信：コードで証明。

#### F22. 後から来た GPU に実例ができない

- 種類：汎用の層の欠け。
- 場所：`user/apps/services.c:303-310`、`user/apps/services.c:831`。
- 根拠：
  - `instances_from_table` が PCI を読むのは管理者の開始の一度です。
  - `SCOPE_FUNCTION` の実例はその時にしかできません。
- 起きる条件：PCIe の hotplug、外づけの GPU。
- 確信：コードで証明（この関数については）。別の道で実例を足す者は見つかりません。

#### F23. 試験が既定の道と二社目の道を試さない

- 種類：試験の穴。
- 場所：`tests/drivertest.c:37`、`tests/runtests.c:94-95`、`tests/runtests.c:131-132`、`tests/nvk_rm_hosttest.c:22-25`、`tests/nvk_contracttest.c:1-2`、`tests/nvrmtest.c`。
- 根拠：
  - `drivertest.c:37` は `N_DRIVER_FOLDER "\\nvk"` に決め打ちです。
  - `nvk_rm_hosttest.c` は偽の RM の上で動きます。`nvk_contracttest.c:1-2` は GPU 不在の成功が何も証明しないと自ら書きます。これは正直です。
  - ドライバーが落ちる試験、既定の道へ戻る試験、二つの像の試験は、`rg` の範囲で見つかりません。
  - `tests/nvrmtest.c` は `Makefile`、`scripts`、`tests/runtests.c`、`tests/testrun.c`、`tests/*.py` に名前が出ません。呼ぶ者が居るか確かめられていません。
- 起きる条件：常に。
- 確信：試験の表についてはコードで証明。`nvrmtest.c` が組まれないかは可能性あり（Makefile の組みの一覧の全部は読んでいません）。

### 軽い

#### F24. 古いコメントと文書

- 種類：文書の誤り。
- 場所と内容：
  - `ports/mesa/nocturne_gl.c:1-2`、`ports/mesa/zink.mk:1-2`：NVK を「プログラムに結合する」と書きます。今は像です。
  - `third_party/mesa/src/gallium/drivers/zink/zink_screen.c:82-83`：同じです。
  - `user/libc/web/js_webgl.c:1`、`user/libc/web/js_webgl.c:4020`：llvmpipe を書き落としています。
  - `kernel/src/dev/pciuser.c:1`：「Only init claims a PCI function」と書きます。今は能力です。
  - `kernel/src/dev/fb.c:410`、`kernel/src/dev/pciuser.c:182`、`ports/nvidia/os_pci.c:577`、`docs/nocturne-native-abi.md:615`：`fbhold=` の語が残ります。製品のカーネルは起動の引数を読みません。
- 確信：コードで証明。

#### F25. 小さな決め打ちと費用

- 種類：形の残り。
- 場所と内容：
  - `user/libc/web/js_webgl.c:636-647`、`user/libc/web/js_webgl.c:1504`：`powerPreference` は読んで返すだけです。今は選ぶ相手がありません。
  - `ports/mesa/nocturne_gl.c:477`：汎用の口の欄の名が `rm` です。
  - `common/display_abi.h:59-68`：`SCREEN_SOURCE_*` の `HANDED_BACK` は nvrm の手順から来た語です。
  - `user/libc/vulkan.c:51`：像が無い間は、呼ぶたびにサービスの一覧を取り直します。GL の文脈を作るたびの費用です。
  - `Makefile:561`：SDK の組みの依存に `ports/nvidia/include` があります。`scripts/mksdk.sh` はそれを写しません。
- 確信：コードで証明。費用は測っていません。

## 問い 1：決め打ちの棚卸し

`rg -i "nvidia|nvrm|\bnvk|nvkms|nouveau|\bmesa|llvmpipe|lavapipe|gallium|softpipe|\bzink|10de"` を `kernel`、`common`、`user`、`scripts`、`Makefile`、`boot`、`rootfs` へかけ、全部の行を見ました。`common/store/fold_table.h:97` と `user/libc/web/counter.c:74` の `0x10DE` は文字の番号で、GPU と関係ありません。

**カーネルと `common/` に `0x10de` はありません。** カーネルの名前はコメントだけです。

### カーネル

| 場所 | 決め打ちしているもの | 在ってよいか | 持ち主 |
|---|---|---|---|
| `kernel/src/dev/fb.h:38`、`kernel/src/dev/fb.c:403` | コメントが RM の BAR の切り替えを例に挙げる。機構 `fb_hold` は汎用 | よい | カーネル |
| `kernel/src/sys/stream.c:54` | コメントが nvrm を例に挙げる | よい | カーネル |
| `kernel/src/dev/pciuser.c:184`、`:591`、`:1085` | コメントが RM に触れる | よい | カーネル |
| `kernel/src/dev/pciuser.c:757-770` | 画面の BAR の機能を FLR しない。ベンダーの語は無い | よいが F1 | カーネルと管理者の段取り |
| `kernel/src/init.c:8`、`:42`、`kernel/src/sys/syscall.c:28` | `dev/gpu.h`（virtio-gpu）を共通コードが呼ぶ | いけない（F17） | `vm/vm.h` の後ろ |
| `kernel/src/vm/vbe.c:190-191`、`kernel/src/vm/virtio_gpu.c:578` | VM の装置の番号 | よい | VM |

### common

| 場所 | 決め打ちしているもの | 在ってよいか | 持ち主 |
|---|---|---|---|
| `common/gpu_abi.h:1` | 「native GPU interface」と名乗る | いけない（F6、F16） | 消す |
| `common/gpu_abi.h:6-18` | `N_GPU_BACKEND_VIRGL`、`N_GPU_BACKEND_CPU` | いけない | 消すか VM |
| `common/gpu_abi.h:65` | `N_DRIVER_FOLDER` | よい | ABI |
| `common/gpu_abi.h:68-89` | Vulkan の ICD、Mesa の log、nvrm の直接表示の形（F4、F5） | 形が問題 | ABI。分ける |
| `common/abi.h:60`、`:62-63`、`:136` | `SYS_GPU_*`（F16） | いけない | 消す |
| `common/abi.h:484` | `N_PCI_NEEDS_RECOVERY`。語は一般、読み手は nvrm（F1） | よいが働かない | 段取りの持ち主 |
| `common/abi.h:521-527`、`:541`、`:574-575`、`:592`、`:615` | 形式、面の数、モード、ポインター。一般の語 | よい | ABI |
| `common/services_abi.h:67` | サービスごとに `client_image` が一つ | 形が問題 | 台帳 |
| `common/display_abi.h:59-68` | `HANDED_BACK` は nvrm の手順の語（F25） | ほぼよい | ABI |
| `common/edid.h:4-5` | コメントが `ports/nvidia/display.c` を挙げる | よい | 共有 |
| `common/settings_abi.h:17-26` | 領域に GPU もドライバーも無い（F13） | 欠け | 設定のサービス |
| `common/settings_abi.h:53`、`user/apps/settings_table.h:25-29` | `N_DISPLAY_HOLD_SECONDS`、`boot.unisolated-drivers`：ドライバーの事情から来た OS の鍵 | よい（OS の方針） | OS |

### ABI の文書 `docs/nocturne-native-abi.md`

| 場所 | 内容 | 在ってよいか |
|---|---|---|
| `:11`、`:534`、`:803` | nvrm を例に挙げる | よい |
| `:475` | UVM の口の説明 | よい |
| `:612-618` | RM と GSP の BAR1 が hold の理由 | よい（`:615` の `fbhold=` は古い） |
| `:638` | 「NVIDIA の X8B8G8R8」 | 形が問題（F5） |
| `:1352-1355` | `gpu` の項目の nvrm と nvk | 今の形としては正しい。形が問題（F2） |

### user

| 場所 | 決め打ちしているもの | 在ってよいか | 持ち主 |
|---|---|---|---|
| `user/apps/services.c:24`、`:59`、`:64-67` | 根のサービスが `nvrm.h`、`nvidia_display`、`.program="nvrm"`、`.client_image="nvk"` を持つ（F2） | **いけない** | ドライバーの宣言のデータ |
| `user/libc/vulkan.c:19`、`:45-62` | 像は一つ（F3）。名前は無い | 形が問題 | ローダー |
| `user/include/nocturne_gpu.h:6-10` | 「A process has one」 | 形が問題 | ローダー |
| `user/include/nocturne.h:418-420` | Mesa の TLS の枠（F20） | いけない | 移植の側 |
| `user/include/nocturne.h:561` | コメントが nvrm を挙げる | よい | 公開の見出し |
| `user/libc/web/js_webgl.c:1`、`:10`、`:13-15`、`:981`、`:1928`、`:4020` | Mesa の内部の見出しとコメント（F20、F24） | 内部の見出しはいけない | Mesa の移植の口 |
| `user/libc/web/js_webgl.h:2`、`js_canvas.js:1`、`js_offscreen_canvas.js:8` | コメントが Mesa を挙げる | よい | ブラウザー |
| `user/libc/web/js_webgl_constants.py:2`、`:9` | GL の定数を `third_party/mesa/include` から読む | よい（生成の材料） | ブラウザー |
| `user/libc/web/js_canvas.c:402-403`、`:669-682` | `SYS_GPU_*` の呼出し（F16） | いけない | 消す |

### scripts と Makefile

| 場所 | 決め打ちしているもの | 在ってよいか | 持ち主 |
|---|---|---|---|
| `scripts/layout.py:5`、`:28`、`:101` | `C:\Nocturne\Firmware\NVIDIA\`、ga10x だけ（F14） | 道はよい。一覧がいけない | ドライバーの宣言 |
| `scripts/layout.py:88` | `C:\Nocturne\Drivers` は `build/drivers` の全部 | よい（汎用） | 媒体 |
| `scripts/mkimage.sh:23`、`:84`、`:101` | `nvidia-firmware.json` | ほぼよい | ドライバーの宣言 |
| `scripts/mksdk.sh:8-9` | `libc.a` に Mesa と LLVM の JIT（F20） | 形が問題 | 移植 |
| `scripts/mksdk.sh:52-55` | Vulkan の見出しを Mesa の木から写す | よい（Khronos の見出し） | SDK |
| `scripts/mksdk.sh:112`、`scripts/link_unit.py:5` | コメント | よい | 組み |
| `Makefile:12`、`:17` | コメント | よい | 組み |
| `Makefile:31` | `QEMU_GRAPHICS` の virgl | 確かめの道具のみ | 組み |
| `Makefile:335-338` | nvrm と Mesa の include。「無い機械ではファイルを持つだけ」 | 形が問題（F2） | 移植ごとの宣言 |
| `Makefile:339-352` | `CXX_RUNTIME_LINK` と `LIBC_LINK` が `MESA_OBJ`、`ZINK_OBJ`、`VK_CLIENT_OBJ` を持つ | 形が問題（F20） | 移植 |
| `Makefile:360`、`:375-383`、`:492`、`:500-503`、`:578` | NVK と nvrm の組みと試験 | よい | 組み |
| `Makefile:561` | SDK の依存に `ports/nvidia/include`（F25） | いけない | 組み |
| `Makefile:582-594` | 版 `615.78.08`、`:588` のコメントの機種名（F14） | 版はよい。機種名はいけない | 移植の側 |
| `Makefile:598-599` | `DISK_FILES` に `NVRM_FILES`、`NVK_IMAGE` が必須 | 形が問題 | ドライバーの宣言 |

### `ports/` から外へ出る口と、`third_party/` に足した私的な口

| 場所 | 決め打ちしているもの | 在ってよいか | 持ち主 |
|---|---|---|---|
| `ports/nvidia/nvrm.mk:144` | 根のサービスの組みにベンダーの見出し（F2） | **いけない** | 消す |
| `ports/nvidia/include/nvrm.h:33-35` | `0x10de` と class の照合 | よいが、呼ぶ場所がいけない | ドライバーの宣言 |
| `ports/nvidia/nvk_image.c:15-25`、`:41-55` | `n_gpu_driver` の唯一の実装 | よい | ドライバーの像 |
| `ports/mesa/nocturne_gl.c:116-123`、`:138-189` | Zink か CPU を実行時に選ぶ | よい（本物） | GL の口 |
| `ports/mesa/nocturne_gl.c:477`、`:502-551`、`:614` | 直接表示の私的な道（F5） | 形が問題 | GL の口と ABI |
| `ports/mesa/llvmpipe.mk:17`、`ports/mesa/mesa.mk:94`、`:106` | 組みの時の選択（F19） | 形が問題 | 組み |
| `ports/mesa/mesa.mk:108`、`:111` | `WITH_XMLCONFIG=0`、`NOCTURNE` | 設定の道が無い（F13） | 移植 |
| `ports/mesa/mesa.mk:127-133` | WebGL の組みに Mesa の内部の旗（F20） | いけない | 移植 |
| `ports/mesa/zink.mk:17` | `-DGALLIUM_ZINK` は常に付く。だから `nocturne_gl.c:632-648` の空の口は製品で使わない | 空の口が要らない | 移植 |
| `ports/mesa/nvk.mk:272-289` | NVK の像の組み | よい | 移植 |
| `ports/mesa/nocturne_os.c` | OS の小さな道具の口だけ | よい | 移植 |
| `third_party/mesa/src/gallium/include/frontend/winsys_handle.h:22-23` | `WINSYS_HANDLE_TYPE_NOCTURNE_SCANOUT` | 形が問題（F5） | 移植の差分 |
| `third_party/mesa/src/vulkan/util/vk_internal_exts.h:128-140` | `VkImportMemoryNocturneScanoutInfoMESA` | 形が問題（F5） | 移植の差分 |
| `third_party/mesa/src/gallium/drivers/zink/zink_screen.c:3533-3535` | 汎用の Zink の中の「nvrm's GPU」 | コメントがいけない | 移植の差分 |
| `third_party/mesa/src/nouveau/vulkan/nvkmd/nocturne/nvkmd_nocturne_rm.c:549`、`:565` | NVK が `nvrm_pci_is_gpu` を使う | よい（ドライバーの本体） | ドライバー |

### 試験

名前の出るファイルは `tests/` に 22 個あります。ドライバーの試験なので、在ること自体は問題ありません。問題は F23 です。

### 名前に出ない一社の形

| 場所 | 形 |
|---|---|
| `common/gpu_abi.h:83-88` | `scanout_*`：GPU と画面と形式を渡さない。NVKMS の面の形 |
| `common/gpu_abi.h:78-80` | `log_*`：Mesa の `mesa_log_hold` |
| `common/display_abi.h:59-68` | `HANDED_BACK`：`hand_back` の手順 |
| `common/abi.h:484` | 「RM が回復する」前提の知らせ |
| `kernel/src/dev/fb.c:403-410` | hold：RM の BAR の切り替えの必要から来た機構。語は一般で、形は他社でも使えます |

## 問い 2：機械ごとの道

全部の機械に共通の頭です。

1. `kernel/src/boot.c:172-175`：`framebuffers[0]` があれば `fb_init` と `fbcon_init`。
2. `kernel/src/dev/fb.c:102`：`screens_boot`。
3. `vm_display_init` は実機で何もしません。`gpu_init`（`kernel/src/init.c:42`）は実機で戻ります。
4. `kernel/src/gui/wm.c:2608-2631`：画面があれば desktop が立ちます。
5. `user/apps/services.c:831`：`instances_from_table`。`:273` で `nvidia_display` を各 PCI 機能に問います。

### AMD だけ、Intel だけ

二つの道は同じです。両社の GPU のドライバーの語（`amdgpu`、`radeon`、`radv`、`i915`、`anv`、`iris`）は、`kernel/src`、`common`、`user/apps`、`user/include`、`user/libc/*.c`、`scripts`、`Makefile`、`ports/mesa` の `.mk` と `.c` にありません（`rg` で確かめました）。

| 項目 | 呼出しの順と結果 |
|---|---|
| 起動 | 共通の頭で立ちます。`gpu` の実例はできません。 |
| 画面 | 画面 0 は firmware の面で、`boot_flip`（`fb.c:259-263`）です。firmware が出した一画面だけです。 |
| モード | `user/apps/display.c:153-157` は申し出が無く、何もしません。`screenmode.c:184` は `-N_ERR_NOT_FOUND` で、`display.c:356` が同じ扱いをします。設定の窓は理由 `W_R_FIRMWARE` を言います（`user/apps/settings.c:746-750`、`user/apps/settings_words.h:153-156`）。 |
| GL | `js_webgl.c` から `ngl_*`、`get_screen`（`nocturne_gl.c:138-189`）、`n_gpu_driver_get`、NOT_FOUND、ログなしで `cpu_screen`（llvmpipe、なければ softpipe）。動きます。ES 3.0 までです。 |
| Vulkan | `VK_ERROR_INCOMPATIBLE_DRIVER`、版 1.0、拡張 0 個（`vulkan.c:72,78,87-89`）。 |
| 計算 | 道がありません。 |
| 眠り | 出しません（F9）。 |

### 対応外の古い NVIDIA だけ

| 項目 | 呼出しの順と結果 |
|---|---|
| 起動 | `nvidia_display` が真で実例ができ、nvrm が claim します。`nvrm.c:282-285` の `rm_is_supported_device` が断り、`:401-404` で status 1 です。2 秒おきに 2 回起こし直し、FAILED になります。 |
| 画面 | firmware のままです。表示エンジンに触れる前に断るので、画面は残ります（読んだ道の範囲）。 |
| モード | AMD と同じです。 |
| GL | `vulkan.c:55` が FAILED を飛ばさないので NVK の像を写します。`ready` が失敗し、理由をログへ書いて CPU です。 |
| Vulkan | 像を読み、instance は立ちます。装置の数の細かな道は確かめていません（0 になるはずです）。 |
| 計算 | 上と同じです。 |

### Turing の世代（RM は知っているが firmware が無い）

`init_adapter` が失敗し（`ports/nvidia/os.c:1014-1031`）、診断の状態でサービスは RUNNING です（F15）。画面は firmware のままです。GL は `ready` が NOT_READY で CPU です。

### GPU のドライバーの起動に失敗した機械

| 失敗の場所 | 結果 |
|---|---|
| claim（隔離できない、`kernel/src/dev/iommu.h:16-18`、`iommu.c:830-836`） | status 1、FAILED、firmware のまま。理由は system log です。 |
| `rm_is_supported_device` | 上の「対応外」と同じです。 |
| `init_adapter` | RUNNING（F15）。GL は CPU。Vulkan の装置は使えません。 |
| NVKMS か画面（`nvrm.c:341-344`） | ログだけで firmware のまま。GL と Vulkan は GPU で動きえます。これは良い分離です。 |
| hold の間の hang | 画面が止まり続けます（F11）。 |
| 画面を取った後の異常終了 | F1。 |

### GPU が無い機械

AMD だけと同じです。framebuffer が無ければ desktop も panic の画面も立ちません（F8）。

### NVIDIA と AMD の両方

| 項目 | 呼出しの順と結果 |
|---|---|
| 起動 | 実例は NVIDIA の機能の分だけです。AMD の機能に何も起きません。 |
| 画面、firmware が AMD 側 | 画面 0 は firmware の面（AMD）です。NVIDIA の出力 0 は画面 1 です（`pciuser.c:358-370`）。 |
| 画面、firmware が NVIDIA 側 | 画面 0 は NVIDIA の出力 0 です。AMD のモニターは使いません。 |
| モード | NVIDIA の出力だけが申し出ます。AMD の画面は固定です。 |
| GL、Vulkan、計算 | NVK だけです。`scanout_open` は最初の ready な GPU です。 |

IOMMU は claim しない機能を素通しにします（`kernel/src/dev/iommu.h:1-18`、`kernel/src/dev/iommu_intel.c:5`）。firmware の画面を遮りません。AMD の IOMMU の道は読みませんでした。

## 問い 3：既定のドライバー

- **firmware の framebuffer の道は既定として成り立っていますか。** ベンダーのドライバーが居ない機械の画面としては成り立ちます。名前のある持ち主は無く、`boot_ops` と `show_boot` が `dev/fb.c` にあるだけです。
- **解像度。** Limine が決めたものに固定です（`boot/limine.conf` は決めません。Limine の中は読んでいません）。
- **Hz。** EDID から推します。分からなければ 0 で、頼まれたときだけ描きます。垂直帰線はありません。
- **複数画面。** ありません。`framebuffers[0]` だけです。
- **起動中に落ちたとき。** 表示エンジンに触れる前なら firmware のままで戻ります。これは本物です。hang は F11 です。
- **動作中に落ちたとき。** 戻りません（F1）。
- **ソフトウェアの GL。** 居ます。llvmpipe と softpipe のどちらかです。Zink か CPU かは実行時の選択で、本物です。llvmpipe の有無は組みの時です（F19）。
- **ソフトウェアの Vulkan。** 居ません（F7）。
- **文脈が CPU へ移るか。** 文脈を作るたびに GPU の画面が無ければ試し直します。一度 GPU の画面ができると CPU へ戻りません（F12）。

## 問い 4：継ぎ目の検査

### 二社目を足すと呼ぶ側の何が変わるか

| 継ぎ目 | 変わるもの |
|---|---|
| `user/apps/services.c` の `gpu` | 見出しの include、判定の関数、項目を足し、根のサービスを組み直します。`nvrm.mk:144` の同類を足します。 |
| `user/libc/vulkan.c`、`user/include/nocturne_gpu.h` | 像を一つから複数へ変えます。装置を束ねます。FAILED を飛ばします。`driver_failure` を像ごとにします。 |
| `struct n_gpu_driver` | `scanout_*` に画面と形式を足します。`log_*` を一般にします。`ready` に情報を足します。ABI の版が変わります。 |
| `ports/mesa/nocturne_gl.c` | 私的な import を二社目の Vulkan が実装しないと、直接表示が動きません。RGBA の前提を消します（`:614`）。 |
| `n_pci_*screen*` | **変更は要りません。** 本物です。 |
| `screen_ops` | **変更は要りません。** `flip` の一つで、実装は `boot_ops`、`vbe_ops`、`output_ops` の三つです。呼ぶのは `fb.c:546` だけです。wm は知りません。 |
| `screenmode.c`、サービス `display` | **変更は要りません。** |
| `scripts/layout.py`、`Makefile` | firmware の一覧と道を足します。`DISK_FILES` に足します。 |

### 口はあるが実装が一つで、形が一社に合っている所

- `struct n_gpu_driver`：実装は `ports/nvidia/nvk_image.c` の一つ。
- `client_image`：値は `"nvk"` の一つ。
- `SCOPE_FUNCTION`、`function_matches`：項目は `gpu` の一つ。形は一般です。
- `n_gpu_scanout`：実装は一つ。
- `n_pci_give_screen` の系統：アプリのドライバーの使い手は `ports/nvidia/display.c` の一つ。形は一般で、一社に合っていません。

### 誰が決めるか

- **GPU の機能を列挙する者：OS には居ません。** NVK が自分のプロセスの中で RM に聞きます（`nvkmd_nocturne_rm.c:549,565`）。`SYS_GPU_INFO` は virgl だけです。
- **どのドライバーを起こすかを決める者：`user/apps/services.c:59,65-67` の一行です。** 表はコードの中にあります。
- **どの像を読むかを決める者：** サービスの一覧の順で最初のものです（`vulkan.c:54-58`）。

## 問い 5：ドライバーの設定と OS の設定

- **ドライバーの固有の設定の置き場：ありません（F13）。** 在る口は `getenv` で、起こす者が埋めません。
- **OS の設定へつなぐ継ぎ目：** 今あるのはモードの往復だけです。
  - OS が望みを書きます（`n_screen_state.mode`）。
  - ドライバーが実効を言います（`n_screen_modes.shown` と `done`）。
  - `SCREEN_WATCH` で知らせます。
  - この形は、重なる値を OS とドライバーの間で持つ正しい型です。
- **ドライバーの発の変更を OS の設定へ戻す道：** このモードの報告のほかにありません。
- **重なりの例（今は片方しか居ません）：**
  - 色深度：`ports/nvidia/nvkms_os.c` の `max_output_color_depth` と `hdmi_deepcolor`、OS の 10 bit の語（`user/apps/settings.c:757`）。
  - VRR：`nvkms_os.c` の `disable_vrr_memclk_switch` と `conceal_vrr_caps`、OS の VRR の語（F21）。
  - dither：`N_WINDOW_SMOOTH_GRADIENTS`（`kernel/src/gui/wm.c:2264`、`kernel/src/dev/fb.c:474-488` は画面 0 の形式だけを見ます）と表示エンジンの dither。
- **`N_SCOPE_PROGRAM` の鍵：** 口としてはドライバーにも開いています（`user/apps/settingsd.c:883-888`）。ドライバーは身元の `program` が要ります。使っている者はありません。nvrm の身元に `program` が入るかは確かめていません。
- **設定を変える窓を持つ者：** 無ければ、その値は利用者に届きません。

詳しくは画面の管理の担当の報告に任せます。

## 問い 6：偽物の一覧

| 種類 | 場所 | 内容 |
|---|---|---|
| 何もしない口 | `SYS_GPU_*`（F16） | 製品で常に使えない。呼ぶ費用だけがある |
| 何もしない口 | `N_PCI_NEEDS_RECOVERY`（F1） | ログだけ |
| 何もしない口 | `NVRM_REGISTRY_DWORDS`、`NVKMS_OPTIONS`（F13） | 起こす者が渡さない |
| 何もしない口 | `powerPreference`（F25） | 記録だけ。選ぶ相手が無い |
| 呼ぶ者のいない道 | `N_SCREEN_MODE_VRR`、`N_SCANOUT_XRGB2101010`（F21） | 読み手だけ |
| 呼ぶ者のいない道 | `ports/mesa/nocturne_gl.c:632-648` | `#else` の空の口。`zink.mk:17` が常に `-DGALLIUM_ZINK` を付ける |
| 実装の無い抽象 | `struct n_gpu_driver`、`client_image`、`n_gpu_scanout`（F3、F4、F5） | 実装が一つで、形が一社 |
| 組みを実行時に見せる | `cpu_screen`（F19） | 片方が組みの時に消える |
| 偽の成功 | nvrm の RUNNING（F15） | 使えない GPU でも RUNNING |
| 名前の偽り | `common/gpu_abi.h:1` | 「native GPU interface」の中身は virgl のデモと Vulkan の像 |

**偽物でなかったもの。** `ports/mesa/nocturne_gl.c:75-79` の `mem_displaytarget_display` と `:257-261` の `drawable_flush_front` は何もしないように見えます。表示は呼ぶ側の写しで行うので、正当です。

## 修理の案

カーネルに触れる案は、設計を見せてから書きます。

1. **落ちたときの段取りを一つ決めます（F1、F11、F12）。** 問いは「落ちても画面が見えるか」です。正常な `hand_back` に頼りません。止める、機能を reset してよいかを確かめる、起こし直す、尽きたら既定の道へ、の順を、管理者とカーネルの持ち物にします。reset の後に表示エンジンがどうなるかは、作り手の文書で確かめてから決めます。GL は喪失を受けて `webglcontextlost` を出し、CPU へ移ります。
2. **firmware の framebuffer を、名前のある既定のドライバーにします（F8、F9、F18）。** `screens_boot` を `fb_init` から分けます。framebuffer が無くても画面の層は立ちます。`fb_buffer_check` の `!fb.addr` を消します。wm は「画面が来たら立つ」へ変えます。Limine の全部の framebuffer を受けます。
3. **ドライバーの選択をデータの宣言へ移します（F2、F14、F22）。** PCI の vendor、class、device、program、像、firmware の一覧を、ドライバーごとの宣言に書きます。管理者はそれを読むだけです。`services.c` からベンダーの見出しを消します。hotplug もこの表を通ります。
4. **GPU の台帳の持ち主を一つ置きます（F6、F15）。** 機能ごとにドライバー、状態、理由を返します。「起きている」と「使える」を分けます。設定の窓はこれを読みます。
5. **`n_gpu_driver` を分けます（F4）。** 共通の頭（abi、名前、装置、記録）と、名前で引く口（Vulkan、直接表示、計算）にします。`docs/records/gpu-compute-20261102.md` の案と同じ向きです。
6. **ローダーを複数の像にします（F3）。** FAILED を飛ばします。失敗で次へ進みます。装置を束ねます。
7. **直接表示の口に画面と形式を渡します（F5）。** Mesa の私的な import は、二社目が実装できる形を ABI の文書に書きます。
8. **ソフトウェアの Vulkan を既定として置くかどうかを、依頼者が決めます（F7）。** 置くなら、llvmpipe の上の lavapipe が候補です。LLVM の有無を組みの時に隠しません（F19）。
9. **`SYS_GPU_*`、`user/libc/gpu.c`、`js_canvas.c` の呼出しを消します（F16、F17）。** VM の道は `vm/vm.h` の後ろへ移します。
10. **ドライバーの設定の継ぎ目を一つ作ります（F13）。** 領域と、ドライバーが宣言する固有の鍵を分けます。重なる値は「OS が望み、ドライバーが実効を言う」の往復を、モードと同じ形で広げます。`getenv` の口はこの継ぎ目に置き換えます。
11. **Mesa の語を公開の見出しから外します（F20）。** `js_webgl.c` が Mesa の内部を読まないようにします。

## 本物と確かめた部分

- カーネルにベンダーの判定はありません。`kernel/src/dev/pciuser.c` の判定は `cls != 0x03` だけです（`:894`、`:1003`）。
- `screen_ops`（`kernel/src/dev/fb.h:72-77`）は `flip` の一つで、実装は三つです。呼ぶのは `fb.c:546` だけです。`wm.c` は `screen_frame_*` だけを使います。
- `n_pci_give_screen`、`n_pci_screen_*`（`kernel/src/dev/pciuser.c:891-1067`）にベンダーの語はありません。
- `kernel/src/mm/shared.c:945-973` の `shared_scanout_open` は BAR でも DMA でも受けます。
- `kernel/src/gui/screenmode.c` と `user/apps/display.c` にベンダーの語はありません。画面ごとの配置、倍率、断られたモードの論理があります。
- `user/libc/driver_image.c` と `SYS_IMAGE_MAP`（`kernel/src/sys/proc.c:242`）は汎用の像の読み込みです。
- `scripts/layout.py:88` の Drivers のフォルダーは汎用です。
- `kernel/src/dev/pciuser.c:358-370` の `output_screen_of` は GPU が二つでも筋が通ります。
- `kernel/src/dev/pciuser.c:1086-1094` の ACPI の断る表は system の持ち物で汎用です。
- `ports/mesa/nocturne_gl.c:138-189` の GPU か CPU の選択は実行時で、失敗を記録します。
- `ports/nvidia/nvrm.c:341-344`：NVKMS が失敗しても RM を提供し続けます。
- `kernel/src/dev/iommu.h:1-18`：claim しない機能を素通しにし、隔離できないと claim を断ります。
- `README.md:76-77`、`:270`、`:466`、`:623`：NVIDIA だけと正直に書いています。
- `tests/nvk_contracttest.c:1-2`：何を証明しないかを正直に書いています。

## 読めなかった範囲

- OS を動かしていません。実機と QEMU の結果はありません。「映らない」「動く」は、全部コードから読んだ話です。
- `ports/nvidia/display.c`、`kernel/src/gui/wm.c`、`user/apps/settings.c`、`kernel/src/mm/shared.c` は要る所だけを読みました。
- NVK の本体、RM の本体、Zink の中の喪失の扱いは読んでいません。
- Limine の中は読んでいません。
- `kernel/src/dev/iommu_amd.c` の素通しの道は読んでいません。
- 費用（F3 の像の読み込み、F16 の矩形ごとの syscall）の実時間は測っていません。
- `Makefile` の組みの一覧の全部は読んでいません。`tests/nvrmtest.c` が組まれるかは確かめられていません。
- 合成器、モードの管理、音は別の担当です。重なる所は汎用の層の観点からだけ書きました。
