# 波 4 の一：窓と GPU の層の設計の下書き（依頼文と報告）

## 依頼文

あなたは Nocturne OS の設計の下書きを書く書き手です。Nocturne は独自の OS で、UNIX でも POSIX でも Linux でもありません。ユーザーはその三つを嫌います。GUI が先の OS で、対象は実機（Ryzen 5 7600X、RTX 4070）です。読むだけで、コードもファイルも変えません。

### 目的

窓の系と GPU を、核の第一級の部品として設計する。ユーザーの言葉は三つ。
- 「GUI FIRST MEANS, FIRST CLASS WINDOWING AND GPU SUPPORT」
- 「IMPROVING KERNEL DOESNT MEAN MAKE IT WORSE」
- 「THIS IS NO LINUX」

**支える GPU**
- 実機の NVIDIA（RTX 4070）。ユーザーが移植させた NV のドライバー（nvrm：NVIDIA の RM をアプリにした driver、`ports/nvidia`）と Mesa（`ports/mesa`）の道。
- VM の virtio-gpu の virgl（`vm/virtio_gpu.c`）。

今ある機能を一つも減らさず、遅くしない。

### 必ず最初に読むもの

**決まり**
- `D:\Programes\llm vibe slop os\AGENTS.md`：特に「製品のコード」「GUI が先」「VM の処理は実機と分ける」「UNIX ではない」。
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\00-design.md` §3（全員の決まり）：「GUI が先」「Linux ではない」が先頭にある。
- `D:\Programes\llm vibe slop os\.claude\rules\map.md`、`kernel.md`、`nvidia.md`。

**監査**
- `D:\Programes\llm vibe slop os\docs\kernel-audit-20261030\02-gui-vm.md` の全文（窓・表示・GPU・VM）。
- `06-devices.md` の fb・pciuser・gpu.h の節。

**先の設計**
- `D:\Programes\llm vibe slop os\docs\kernel-rebuild-20261031\3-design.md` §4.5：事象の時刻。入力の時刻、EV_FRAME の表示の時と番号。
- `4-design.md`：装置の層の決定。§2 の 6 と冒頭の「そのまま採らない所」。
- `4-dev-plan.md` の §3.11・§3.12：装置の層の下書きの表示と claim。Linux の形に寄っているので、型にしない。何が要るかの材料としてだけ読む。

**今のコード**
- 窓の系：`kernel/src/gui/wm.c`・`desktop.c`・`screenmode.c`。
- ABI：`common/display_abi.h`・`common/gpu_abi.h`、`common/abi.h` の窓と画面の syscall。
- GPU：`kernel/src/dev/gpu.h`、`kernel/src/vm/virtio_gpu.c`。
- 表示と claim：`kernel/src/dev/fb.c`・`fbcon.c`、`kernel/src/dev/pciuser.c`（claim・画面の hold・vblank）、`user/apps/init.c`（claim の道）。
- VM の表示：`kernel/src/vm/rdp.c`（拡張セッションの表示）。
- libc：`user/libc/gui.c`・`ui.c`（窓と UI の toolkit）、`user/libc/gpu.c`。
- ブラウザー：`user/libc/web/js_canvas.c`・`js_webgl.c`。
- NVIDIA の道：`ports/mesa/nocturne_gl.c`、`ports/nvidia` の README と OS 層（`display.c`・`nvkms_os.c`・`os.c` など）。
- NVIDIA の記録：`docs/nvidia/LEDGER.md` の「0. 動かない前提」「1. 決定事項」「3. 段階と状態」「6. 次の一手」。

### 問い

1. **今ある機能の一覧。** 窓・表示・GPU・入力の経路・画面の取得・クリップボード・指の絵・直接の表示（EV_DIRECT）・拡張セッション・mode の切り替え・panic の画面など、今あるものを全部並べる。建て直しの後もこれを全部持つ。今の速さの物差し（合成の拍、表示までの遅れ）も、測れるものは何で測るかを書く。
2. **窓の系の責務と持ち主。**
   - 合成、窓の木、入力の振り分け、焦点、拍。
   - 今は核の中（wm の thread）にある。この置き場所の是非は、ユーザーが決めること。GUI が先の目的から見た案と理由を示し、推す案を一つ書く。
3. **GPU の層の責務と持ち主。**
   - 名と契約を持つ抽象化の層で、各側に本物の実装がある：NVIDIA（nvrm と Mesa）と virgl。
   - 次を答える：
     - アプリ・ブラウザー（canvas・WebGL）・合成がどう GPU を使うか。
     - 窓の緩衝を GPU とどう分け合うか（写しなしで）。
     - 柵（fence）と時刻（単調の ns）。
     - 表示の時（vblank）と目標の時。
     - 複数の画面と複数の GPU。
     - 眠りと起き（nvrm の claim を持ったまま）。
     - panic の画面。
   - 今の `gpu_abi.h`（virgl だけの、わざと有限にした描画の道）と、NVIDIA の道（アプリの nvrm と Mesa）がどう一つの層になるか。
4. **境界。**
   - 監査 02 の指摘：VM だけの実装が vm.h を通らずに共通コードへ漏れている（wm.c・syscall.c → `dev/gpu.h`）。これを、機能を消さずに層の形で直す。
   - VM の処理は `vm/vm.h` の後ろ（AGENTS.md）。
5. **数が十倍になったら**（窓・画面・GPU・アプリ）、どこが詰まり、呼ぶ側の何が変わるか。
6. **Linux・NT の公開の設計との照らし。**
   - 物差しとしてだけ使う。軸ごとに劣る所が無いかを見る。
   - 型として写さない。Nocturne の部品の名と分け方は、Nocturne の目的から決める。

### 仕様と資料
- 仕様は `D:\Programes\OS DEV INFO`。PDF は pdftotext で要る節だけ引く。
- NVIDIA の資料は `.claude/rules/nvidia.md` の決まりどおり：`D:\Programes\cloned repos` の open-gpu-kernel-modules・open-gpu-doc・mesa。
- Linux・BSD・NT ほか、OS のソースを読まない。`D:\Programes\OS DEV INFO\rtl_bsd_drv_v1102.01\` も読まない。公開された設計の説明はよい。
- 確かめられなかったことは「未確認」と書く。

### 報告（最後のメッセージ。日本語。本体の会話が全文をこの文書の「報告」へ写す）
1. 今ある機能の一覧と、速さの物差し。
2. 今のファイルごとの判定（そのまま／中を直す／建て直す）と理由。機能を消す判定は書かない。
3. 窓の系の設計：責務と持ち主、置き場所の案と推す案、錠と文脈、時間。
4. GPU の層の設計：契約、NVIDIA と virgl の実装、緩衝・柵・時刻・表示の時、複数の画面と GPU、眠り、panic。
5. 境界（vm.h、装置の層、mm の 3-V、時間の 3-T）。
6. 十倍の時の詰まり。
7. Linux・NT との照らしの表（劣る軸の有無）。
8. 書き手への割り方（段、順、QEMU で確かめること、実機だけで見えること）。
9. 未確認。

## 報告

（届いたら全文をここへ）
