# compositor の報告：合成と表示の道を、規模の例に耐える形に作り直す

枝は `worktree-agent-a8613a050aec779b7`。土台は main の b6388a38。途中で main の 7c1fd4ac（USB と入力）と 8837d5bd（時間の ABI の ns 化）を取り込んだ。

## 依頼文（全文）

```
あなたの名は「compositor」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の b6388a38（df893170 の上に記録だけ）。QEMU のポートは HMP 45460・QMP 45461。
# 仕事：合成と表示の道を、規模の例に耐える形に作り直す
持ち主は窓の系の部品：`kernel/src/gui/wm.c`・`desktop.c`・`screenmode.c`、`kernel/src/dev/fb.c`・`fbcon.c`、`common/display_abi.h`・`common/abi.h` の画面と窓の部分、`common/gpu_abi.h`・`kernel/src/dev/gpu.h`、表示の driver の口（`dev/pciuser.c` の `SYS_PCI_*SCREEN*`）と NVIDIA の表示（`ports/nvidia` の display、`user/apps/init.c` の渡し）、`vm/virtio_gpu.c`、利用者の側（`user/libc/gui.c`・`ui.c`、`user/libc/web/js.c` の EV_FRAME の受け、`ports/mesa/nocturne_gl.c` の present）。監査 `audit-compositor.md` を全部読むこと。
必ず入れる形：
- 画面は複数。画面ごとに mode・周波数・VRR の範囲・vblank・flip を持つ。ABI と driver の口に画面の番号を通す。3 枚の画面で周波数が違っても、画面ごとに締切を守る（合成の仕事を画面ごとの締切で回す。中核の `sched_deadline` を画面の周期で使う。VRR の画面は装置の申告する範囲で、固定の拍を強いない）。周期が分からない画面の決め打ちの 60 Hz を消し、装置の申告（EDID・driver の mode）から取る。
- 裂けない表示：表示中の scanout に CPU が直に書かない。裏の面と flip（装置が flip を持たない道でも、写しを vblank に合わせる層の形で）。present の終わり・flip の完了を見回り（100 µs・1 ms の眠り）で待たず、割り込み・事象で待つ。
- 窓の面は共有の object（handle）にし、アプリと合成が同じ frame を見る。一コマの写しの回数を減らす。描き捨て（mailbox）の行列を持てるように。fence（同期の印）と、表示された時刻を ABI で返す。
- 大域の `wm_mutex` を一コマの描画の間握らない。入力の配り・窓の syscall・present が合成の描画を待たない錠の分け方にする。錠の下の大きな写し（present・SCREEN_GRAB の copy_to_user・大きさの変更）を外へ出す。
- 損傷の扱いを O(矩形 × 窓) と 64 枚・32 矩形の固定の上限から外す。
- 監査の「目立つ不具合」：EV_FRAME の周期の単位の食い違い（ABI は µs、カーネルは ns）を ns に統一し、受け手（ui.c・js.c）も直す。窓の受け箱と入力の環が満ちたとき黙って捨てない（特にキーを放す事象）。入力の事象に時刻を付ける（時刻は単調時計の ns）。
- vblank を利用者の thread を二つ回ってから届いた時刻にしない。装置（driver）が測った時刻を ABI で渡す。
範囲の外：中核（`kernel/src/mm/`・`sys/sched`・`thread`・`wait` など）の口は使うだけ。足りない口があれば中核の持ち主の形で足してよいが、最小にし、報告に書く。USB・xHCI の中は別の書き手（「usbinput」）、時間の ABI（SYS_SLEEP・timer・n_event の一般の時刻）は別の書き手（「timeabi」）が持つ。`common/abi.h` は三人が触るので、自分の部分だけを変え、ほかの行を並べ替えない。
確かめ：QEMU で、デスクトップ、窓を開く・動かす・大きさを変える・閉じる、Terminal、ブラウザーで実サイト（Wikipedia）を開いてスクロール、3D Cube（あれば）、が前と同じか良く動くこと。QEMU で二枚の画面を試せるなら試す（`-device VGA` を二つ、または virtio-gpu の `max_outputs=2`）。コマの時間は `prof:` の行と合成の行で見て、報告に数字を書く。
```

途中の調整役の指示：
- 入力の時刻は input.h の key_event・mouse_event の `time_ns` をアプリへ渡す。
- main の 8837d5bd（時間の ABI の ns 化）を取り込み、EV_FRAME の単位と入力の時刻の受け渡しをその形に重ねる。

## 設計

### 画面（`kernel/src/dev/fb.c`・`fb.h`）
- 画面（`struct screen`）は一つの表示。mode（幅・高さ・周波数、または VRR の範囲）と、表示エンジンが一つを映す 1〜`N_SCREEN_BUFFERS_MAX`（4）枚の面を持つ。画面の数に上限は無い。画面 0 は起動時のファームウェアの面。画面は消えない：driver が去った画面は、次に面を渡されるまで off。
- 一コマは表示されていない面へ描く。`screen_frame_begin` が空いた面を待ち（前に求めたフリップの表示まで）、`screen_frame_write` が矩形を書き、`screen_frame_end` が次の垂直帰線からの表示をフリップで求める。表示は driver から戻る（`screen_flipped`、表示エンジンが測った垂直帰線の時刻つき）。垂直帰線も同じ（`screen_vblank`）。どちらも画面の待ち手を起こす（`screen_wait`）。待ちは事象で、見回りの眠りは無い。
- 面が一枚の画面には裏が無い。その画面のコマは表示中の面へ書き、書いたとおりに映る（裂けうる）。今これに当たるのは、二コマ分の記憶を持たない VGA と、ファームウェアの面のままの画面 0。
- `screen_set_buffers` は古い面へ最後に求めたフリップの番号を返す。それより後のフリップは新しい面のもの。

### 合成（`kernel/src/gui/wm.c`）
- 画面ごとに合成の thread が一つある。画面は左から順に並ぶデスクトップの一部（画面 0 にタスクバー）。
- thread の級は画面の歩調から決める：
  - 周波数の分かる画面：`sched_deadline(周期)`。周期の次の時刻より前には描かない。
  - VRR の画面：最も速いリフレッシュの周期の締切の級で、描けしだい一コマ（装置の申告する範囲。固定の拍を強いない）。
  - 周期の分からない画面：`sched_fair`。絵が変わったときに描く。決め打ちの 60 Hz は消した。周期は EDID・driver の mode から取る。
  - 二枚以上の面の画面は、次のコマを前のフリップの表示（`asked_serial == shown_serial`）まで始めない。
- 一コマ：`wm_lock` の下で、その画面の部分の変わった区画（tile）と、絵の材料の写し（capture）を取り、錠を放す。区画を画面の絵（RAM の canvas）へ描き、表示されていない面へ書き、フリップを求める。入力・窓の呼出し・present は描かれているコマを待たない。
- 損傷は画面ごとの区画の bitmap（`TILE` 32×32 の一区画に 1 bit）。窓の数と矩形の数の固定の上限（64 枚・32 矩形）と、O(矩形 × 窓) の走査を消した。
- 錠の順（wm.c の頭に書いた）：画面の `back_lock`（その絵）→ fb.c の画面の使用 → `wm_mutex`（窓・重なり・焦点・損傷・ポインター。短く握る。コマの描画と書込み、窓の画素の写しの間は握らない）→ 面の錠（矩形で present する窓のカーネルの写し）→ 窓の `ev_lock`・入力の環の `in_lock`（どちらも spinlock で、二つを同時に持たない）→ 待ち行列と object の錠。
- 錠の下の大きな写しを外へ出した。present の矩形の写しは面の錠の下だけ。`SYS_SCREEN_GRAB` は画面 0 の絵を行の帯ごとにカーネルの記憶へ写し、錠の外で `copy_to_user` する。大きさの変更は新しい面を錠の外で作ってから差し替える。

### 窓の面（`common/abi.h`、`user/libc/gui.c`）
- 窓の画素は面：共有の object（`N_KIND_SHARED`、`sys/syscall.h` の `shared_kernel_new`）。`SYS_WIN_SURFACE`（65、窓、番号）が面のハンドルを返し、アプリはそれを写す（`n_shared_map`）か、描く別のアプリへ渡す。合成はカーネルの写像で同じ頁を読む。
- 矩形で present する窓（`WIN_FRAME_QUEUE` 無し）：面一枚。`SYS_WIN_PRESENT` はその矩形を面の錠の下でカーネルの写しへ写す（変更の半分が映ることはない）。
- `WIN_FRAME_QUEUE` の窓：2〜`N_WIN_SURFACES_MAX`（8）枚の面。`SYS_WIN_ACQUIRE`（81、窓、期限）が描いてよい面の番号を返す。present した面は行列へ入り、合成は写さずにその面を読む（一コマの写しが一回減る）。画面に出た面は別の面が出るまでシステムのもので、出なくなると `EV_SURFACE` で返る。`WIN_MAILBOX` では新しい present が待っている present を置き換え（描き捨て）、置き換えられた面はすぐ返る。
- `SYS_WIN_PRESENT` は present の番号を返す（同期の印）。`EV_FRAME` は画面のコマがその present を取り込んで表示したことを知らせる：`y` は画面のコマの周期（ns）、`time_ns` は表示された時刻（driver が測った垂直帰線。帰線の無い画面では写しの終わり）、`serial` はそのコマが表示した最後の present の番号、`key` は表示した面。
- 窓の大きさは縦横 1〜`N_WIN_SIZE_MAX`（16384）。範囲外は `N_ERR_INVALID`（丸めない）。

### 入力（`kernel/src/gui/wm.c`）
- `struct gui_event` に `time_ns`（単調時計の ns）と `serial` を足した。入力の時刻は input.h の `key_event`・`mouse_event` の `time_ns`（装置の報告がシステムに届いた時）。
- 窓の受け箱は `MAILBOX_MAX`（4096）まで育つ。満ちたら黙って捨てず `EV_INPUT_LOST` を一つ入れる：それより前に押されていたキーとボタンは放されたものとし、次の事象はその後のもの。
- `SYS_WIN_RECEIVE` は `wm_lock` を取らず、`ev_lock` の下で一度に区切って取り出し、錠の外で写す。期限は main の時間の ABI の形（単調時計の ns の絶対の期限）。

### driver の口（`dev/pciuser.c`、`common/abi.h`）
- 出力（output）：driver の connector・head・pipe の番号。表示の機能の出力 0 が画面 0。ほかの出力はそれぞれ自分の画面を持ち、同じ driver が同じ出力を受け持つたびに同じ画面になる。
- `n_pci_give_screen(pci, output, const struct n_screen_buffers *)`：出力の面（1〜4 枚）を渡す（NULL で返す）。最初の面が表示中のもの。戻り値は、その前の出力の最後のフリップの番号（0：無し）。それ以下のフリップは前の面のもので、まだ取られていないものは消える。
- `n_pci_screen_flips(pci, out, max, deadline)`：画面が求めるフリップ（`struct n_screen_flip`：出力、面、出力ごとの `serial`）を読む。出力ごとに一つの枠で、まだ取られていないフリップは次のものに置き換わる。
- `n_pci_screen_vblank(pci, const struct n_screen_vblank *)`：出力、表示中の面、帰線番号、表示エンジンが測った時刻（単調時計の ns）、その帰線が表示する最後のフリップ。画面の次のコマはフリップの帰線を待つ。
- `n_pci_screen_modes`・`_state`・`_image` は出力を取る。`n_screen_mode` に VRR の旗と範囲。mode の既定の選び方は「モニターの推奨、無ければ最も大きく最も速いもの」。
- `SCREEN_MODES_GET`・`SCREEN_MODE_SET` は `op | screen << 8` で画面を選び、`screens` に画面の数を返す。設定アプリ（`settings.ts`）は画面を選んで mode を変える。

### NVIDIA の表示（`ports/nvidia/display.c`）
- デスクトップの面を二枚にした（`DESK_BUFFERS`）。フリップの thread が `n_pci_screen_flips` を受け、head の錠の下で `desk_flip` が表示エンジンへ commit する。
- 垂直帰線のコールバック（NVKMS の錠の下で走る）が帰線を数え、待っていたフリップの完了を通知の記憶（`nvrm_nvkms_flip_done`）で確かめて表示に上げ、`n_pci_screen_vblank` で知らせる。時刻は表示エンジンの時刻を単調時計へ直したもの。
- フリップの完了の待ちは 1 ms の眠りを消し、帰線のコールバックが起こす条件変数（`blank_wait`）で待つ。コールバックの無い間は mode の一周期ずつ待つ。
- 古いフリップ：カーネルは give の番号以下の枠のフリップを消し、driver は `given_serial` 以下のフリップを捨てる（give とフリップの処理はどちらも head の錠の下）。
- 大きさの変更の commit が失敗したときは、head に載っている面を先頭にして渡し直す（`desks_regive`）。表示中の面へ描かせない。

### 受け手（`user/libc/ui.c`、`user/libc/web/js.c`、`user/apps/browser.c`）
- `EV_FRAME` の周期は ns。`ui.c` は ns を ms に直して `UI_EVENT_FRAME` の `a` に入れる（`ui.h` の約束どおり ms）。ブラウザーの主ループの `frame_schedule`（main の 8837d5bd）はその ms を ns に直して締切の級に置く。`js.c` は同じ値を描画の機会の周期にする。
- `ui.c` は `WIN_FRAME_QUEUE` の窓で、次の面の古い絵を見て全部を描き直す。

### 中核へ足した口（最小）
- `sys/prof.h` の `prof_window_ns`：標本の窓の長さ。合成の行を `prof:` と同じ窓で出すため。
- `sys/syscall.h` の `shared_kernel_new`・`shared_kernel_end`・`shared_kernel_handle`：カーネルも読み書きする共有の object（窓の面）。
- `kernel/src/init.c`：`vm_display_init`（VM の表示を画面にする。`vm/vm.h` の後ろ）と `gpu_init` の呼出し。

## 変えたファイル

コミットは枝 `worktree-agent-a8613a050aec779b7` の三つ。

- 5461059d：合成と表示の本体。40 ファイル、+4846 −3073。
- a60e224f：main の 8837d5bd（時間の ABI の ns 化）の取り込み。画面と窓の待ちを ns の絶対の期限に合わせた。
- 最後のコミット：設定の文、`display_abi.h` の注、この報告。

| 区分 | ファイル |
|---|---|
| ABI と文書 | `common/abi.h`、`common/display_abi.h`、`docs/nocturne-native-abi.md`、`user/include/nocturne.h`、`user/include/ui.h`、`rootfs/Nocturne/SDK/Types/nocturne/ui.d.ts`、`.claude/rules/map.md` |
| 画面 | `kernel/src/dev/fb.c`・`fb.h`・`fbcon.c`、`kernel/src/gui/screenmode.c`・`screenmode.h` |
| 合成と窓 | `kernel/src/gui/wm.c`・`wm.h` |
| driver の口 | `kernel/src/dev/pciuser.c`・`pciuser.h`、`kernel/src/sys/syscall.c`・`syscall.h` |
| 中核と起動 | `kernel/src/init.c`、`kernel/src/sys/prof.c`・`prof.h` |
| VM の表示 | `kernel/src/vm/devices.h`・`vbe.c`・`vm.c`・`vm.h` |
| NVIDIA | `ports/nvidia/display.c`・`README.md` |
| 利用者の側 | `user/libc/gui.c`・`sys.c`・`ui.c`・`web/js.c`、`user/apps/js.c`・`js_ui.js`・`mediaplayer.c`・`settings.ts`・`syscallfuzz.c` |
| 試験 | `tests/nvrm_display_hosttest.c`・`pcitest.c`・`wmframetest.c`・`wmramtest.c` |

変更が要らなかった持ち物は二つある。

- `kernel/src/vm/virtio_gpu.c`：画面のコードが無く、GPU だけを持つ。
- `ports/mesa/nocturne_gl.c`：nvrm への直の present だけを持つ。

## 消したもの

- `SYS_WIN_MAP`、`WIN_DOUBLE_BUFFER`、`N_SCANOUT_BOOT`。
- `map_user`・`unmap_user`・`buffers_set`・`alloc_buffer`。
- 損傷の 64 枚・32 矩形の固定の表。
- 決め打ちの 60 Hz（`frame_period`、`settings.ts` の 60000）。
- `scene_benchmark`。
- フリップの完了の 1 ms の見回り（`ports/nvidia/display.c`）。
- modes の `N_ERR_BUSY`。

`docs/records/graphics-20261014.md` と `audit-compositor.md` は日付入りの記録で、消した名がそのまま残っている。

## 組みの結果

- 条件は `python -X utf8 scripts/build.py -j 8 GO= TYPESCRIPT= RUST_SYSROOT=none qemu-image`。作業木の出力は `D:\Programes\llm vibe slop os\build\wt\agent-a8613a050aec779b7\` に置いた。
- 変数なしの `qemu-image` は、この枝の作業木では組んでいない。理由は二つある。
  - 作業木の組みの問題：`scripts/build_cxx.py`・`ports/mesa/generate.py`・`scripts/layout.py` が ROOT/build に書き、make は `$(BUILD)` を見る。Makefile の `tools/`・`local/` も作業木に無い。本式の直しは I/O の書き手の作業木にある。ここでは作業木に `build`・`tools`・`local` のジャンクションを作って回避し、終わりに消した。
  - 取り込んだ時点の `tools/go` で tsc が `undefined: syscall.SIGINT` で止まった。道具の鎖の直しは main の 02e8e89a にあり、この枝は 8837d5bd までを取り込んでいる。
- 上の条件なので、媒体に Go と Rust のプログラムと tsc は無い。TypeScript のアプリは `build/host/tsc.exe -p user/apps/tsconfig.json` を直に回して入れた（終了値 0、型の誤りなし）。変数なしの組みは、main へ載せたときに確かめること。
- 警告：カーネルは 0。最後の組みの `qemu-image`（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\compositor\build_image7.log`）に 1 件ある：`user/libc/web/css.c:8544` の `-Wmissing-field-initializers`（`computed_text`）。全体の組みでは同じ種類が `user/libc/font.c`・`web/cssmath.c`・`cssprop.c`・`layout.c` にも出る。これらは私が触れていないファイルで、main との差は 0 行。依頼の「警告を残さない」に対して、この分は残っている。
- `tests/nvrm_display_hosttest.c`：`nvrm_display_hosttest: ok`。
- `tests/wmframetest.c`・`wmramtest.c`・`pcitest.c`：ホストの clang で翻訳が通る。ゲストでは回していない（下の「残った欠陥と未確認」）。

## QEMU で見たもの

機械は全部同じ：q35、4 CPU、4 GiB、WHPX、NVMe、xHCI の tablet と鍵盤、窓付き。cmdline に `prof`。画像と serial.log の置き場は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\compositor\`。下の表の名は全部この場所の中のもの。撮った画像は全部開いた。

### 比べた媒体

- 今：この枝の媒体。serial は `serial-after-1vga.log`。
- 本体 A：main の c78ffae6 の組み（`build/nocturne.img`、07:10）。引きずりと 3D Cube。serial は `serial-main2.log`。
- 本体 B：同じ `build/nocturne.img` の 06:47 の組み（`wm: compositor started` の版）。Wikipedia。serial は `serial-main-wiki.log`。

### 一枚の VGA（2560×1440）

起動の行：

```
video: emulated display 00:01.0: 2560x1440 at 74.998 Hz, 2 buffers, screen 0, monitor QEMU Monitor
wm: desktop started at 2560x1440 on 1 screens
wm: screen 0: frames at its 74.998 Hz
```

本体 A は `wm: without vertical blanks, frames are paced at the mode's 60.000 Hz` と出す。今は EDID の 74.998 Hz で回る。

| 見たこと | 画像 | 結果 |
|---|---|---|
| デスクトップ | `a1.png` | 正しく出る |
| Terminal と `ps` | `a2.png` | 正しく出る。応答する |
| 窓の引きずり | `a3.png` | 窓は指の位置に来る |
| 大きさの変更 | `a4.png` | 中身が新しい大きさで描き直される |
| 窓を閉じる | `a5.png` | 消える |
| 3D Cube | `a6.png` | 75 fps と表示する |
| ブラウザー、Wikipedia | `a7.png`・`a8.png`・`a9.png` | 頁が出る |
| スクロール | `a10.png`・`a11.png`・`a12.png` | a10 は wheel の直後でまだ動いていない。a11・a12 は動いた後 |
| 設定 | `a13.png`、組み直した媒体の `f1.png`（serial は `serial-final.log`） | `QEMU Monitor`、`2560 × 1440 at 75 Hz`、`The display driver offers only this mode for this monitor.` |

本体 B の同じ記事は `m1.png`・`m2.png`・`m3.png`、本体 A の窓は `n0.png`〜`n3.png`。

### コマの時間と標本

今の合成の行：

```
引きずり  wm: screen 0: 447 frames in 10000 ms, drawing avg 419 max 2117 us, writing avg 242 max 4989 us, 0 longer than a refresh
引きずり  wm: screen 0: 436 frames in 10000 ms, drawing avg 409 max 2306 us, writing avg 185 max 4335 us, 0 longer than a refresh
3D Cube   wm: screen 0: 750 frames in 10000 ms, drawing avg 177 max 335 us, writing avg 621 max 4189 us, 0 longer than a refresh
静止      wm: screen 0: 20 frames in 10000 ms, drawing avg 27 max 30 us, writing avg 35 max 44 us, 0 longer than a refresh
```

同じ機械、同じ手順で本体 A と今を比べた（本体 A には合成の行が無いので、比べられるのは `prof:` の標本と画面の fps）。

| 手順 | 本体 A（c78ffae6） | 今 |
|---|---|---|
| Terminal の同じ 21 回の引きずりの busy の標本の合計 | 789（287＋400＋102） | 500（245＋255） |
| 3D Cube の画面の fps | 59 | 75 |
| 3D Cube の 10 秒の busy の標本 | 716、716 | 809、834、867、847 |
| 3D Cube の一コマの標本（busy ÷ コマ） | 約 1.21（716 ÷ 590、59 fps からの見積り） | 1.08〜1.16（÷ 750） |
| 3D Cube の 10 秒の late jobs | 1、1、3 | 3〜5 |
| throttles | 0 | 0 |

- 引きずりは同じ操作の台本で、本体 A の 789 と今の 500 は、標本の窓の区切りが同じでない合計を比べている。両方とも引きずりの前後の窓は静止の値（一桁）。本体 A の 102 は引きずりの終わりと静止の混ざった窓。
- 3D Cube は 10 秒の合計で今のほうが多い（716 に対し 809〜867）。コマが 59 fps から 75 fps へ増えたからで、一コマあたりは今のほうが小さい。今の標本には VM の Y offset の書込み `vbe_write` の 179 が入る。これは VM の VGA のフリップの費用で、実機の費用ではない。
- 3D Cube の late jobs は今のほうが多い。本体 A の合成は 60 Hz の拍で、今は 75 Hz の締切で回る。条件が違うので、この数だけでは悪化とも改善とも言えない。

Wikipedia のスクロールは本体 B と比べた。同じ記事 Moon、同じ wheel の列で、今の busy は 10760 と 18218、本体 B は 9939 と 17216。throttles はどちらも 0。限界：今の回は最大化の後で読み込み、本体 B の回は読み込みの後で最大化した。wheel から絵が動くまでの実時間は測っていない。標本の多い所は browser の `memcpy`、`hit_partition`・`hit_compare_y`（`paint.c`）、`paint_runs`、`stacking_context`、`style_font` で、合成の行は 18 秒に 3 frames ほど。

### 二枚の VGA（2560×1440 と 1920×1080）

serial は `serial-two-final.log`。

```
video: emulated display 00:02.0: 1920x1080 at 74.998 Hz, 2 buffers, screen 1, monitor QEMU Monitor
wm: desktop started at 2560x1440 on 2 screens
wm: screen 0: frames at its 74.998 Hz
wm: screen 1: frames at its 74.998 Hz
```

| 見たこと | 画像 | 結果 |
|---|---|---|
| 画面 1 のデスクトップ | `t1.png` | デスクトップの続きが出る |
| 画面をまたぐ窓の左右 | `t2.png`・`t3.png` | 窓が二枚にまたがり、y が合う |
| 設定の Screen 1 / Screen 2 | `t4.png`・`t5.png` | 画面ごとに選べる。Screen 2 は `1920 × 1080 at 75 Hz` |

またぐ窓を引きずった間の行：

```
wm: screen 1: 174 frames in 10581 ms, drawing avg 321 max 945 us, writing avg 560 max 10812 us, 0 longer than a refresh
wm: screen 0: 469 frames in 10005 ms, drawing avg 684 max 5905 us, writing avg 1056 max 21756 us, 4 longer than a refresh
```

同じ窓の busy は 591、late jobs 5、inherited deadlines 14。画面 0 の 4 コマが一周期を超えた。一枚の回の 0 より悪い。

## 残った欠陥と未確認

欠陥：

- Wikipedia のスクロールの browser の CPU（`hit_partition` など）。本体 B でも同じ程度で、この仕事の範囲の外。
- 面が一枚の画面は裂けうる。ファームウェアの面のままの画面 0 と、二コマ分の記憶の無い VGA が当たる。
- `image_present`（NVIDIA の道）が present ごとに `n_pci_screen_state` を呼ぶ。
- `vbe.c` は mode の一覧を出さない。QEMU の VGA では設定で mode を選べない（`f1.png` の文のとおり）。
- `WIN_FRAME_QUEUE` の窓の `ui.c` の `render` は次の面へ全部を描き直す。
- tablet は絶対座標で画面 0 の上だけに写る（`handle_mouse`）。QEMU の tablet では指を画面 1 へ動かせない。相対の mouse では試していない。
- 開いた直後の窓に `730 frames … writing max 141868 us, 1 longer than a refresh`、別の窓に `53 frames … writing max 40775 us, 1 longer than a refresh`、開始の全面の書込みに `writing max 25163 us` が出た。どれも VM の VGA への書込みの区間の値で、外れの原因は分からない。
- 警告：`css.c` ほか（上の「組みの結果」）。

未確認：

- WPT は回していない。`scripts/wpt.py html/webappapis/update-rendering html/webappapis/animation-frames` をフックが止めた（「媒体は 2 つまで。build に既に mae.img、nocturne.img があり、道具の写しが三つ目になる」）。`build/mae.img` は私のものではないので消さず、フックも迂回していない。`user/libc/web/js.c` の描画の機会の周期に触れたので、mae.img が片付いた後にこの二つを回すこと。
- ゲストの試験（`tests/wmframetest.c`・`wmramtest.c`・`pcitest.c`）も回していない。`scripts/test.py` は同じように媒体の写しを作るので、同じフックに当たる。main へ載せた媒体で回すこと。
- 周波数の違う複数の画面、VRR、3 枚の画面。QEMU の VGA は両方とも 74.998 Hz で、この三つは試していない。画面ごとの thread と締切は 74.998 Hz の二枚でしか動かしていない。
- 裂けないことは目で確かめていない。screendump は一つの面を読むので、裂けは写らない。言えるのは、二面の画面で表示中の面へ書くコードの道が無いことまで。
- wheel から絵が動くまでの実時間は測っていない。
- NVIDIA の表示は hosttest だけで、実機は未確認。Hyper-V には触れていない。
- Go・Rust・tsc の無い媒体で確かめた。
