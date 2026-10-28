# Nocturne の地図

探す前に読み、ここに無い所だけを codegraph・grep で探す。README と docs の報告より、この表と今のコードを先に信じる。違いを見つけたらその場で直す。確かめた: 2026-10-28 `5d099ff7`。

## 置き場所（K=カーネル、L=libc、A=アプリ、P=移植・上流、H=ホストの道具）

| 場所 | 役目 | 主 |
|---|---|---|
| `kernel/src/arch/` | GDT・IDT・`isr.asm`、APIC、SMP、FPU、PAT、P ステート、S3 復帰 | K |
| `kernel/src/mm/` | `pmm`・`vmm`（空間ごとの 4 段ページ表）・`heap` | K |
| `kernel/src/sys/` | `proc`（ELF）、`sched`（AP の実行キュー）、`kei`（継の選択核）、`syscall`、`object`、`channel`、`events`、`stream`（console・system log）、`watch`（見張りの共通の部品 `struct watched`、`n_watch_take`）、`keep`（システムの部品の状態の保存：持ち主の wm・audio・screenmode が自分の行を出し、`AppData\Nocturne\{Desktop,Sound,Displays}.conf` へ最後の変更の 500 ms 後に書き、init より前に読み戻す。鍵は知らない）、`prof` | K |
| `kernel/src/fs/` | C: の格納庫（`store`・`volume`・`catalog` の B+木・`namespace`・`document`）、`watch`（C: の見張り `n_entry_watch`：名前空間の変更を volume の口から聞く）。`host/store` も同じコード（見張りの口は NULL） | K |
| `kernel/src/dev/` | PCI と `pciuser`（アプリのドライバー）、ACPI（uACPI）、AHCI、NVMe、xHCI・USB、IOMMU、音、`fb`・fbcon、入力、timer、電源 | K |
| `kernel/src/net/` | `net.c`（Ethernet〜TCP・DHCP・DNS）、`rtl8125.c`（実機の NIC） | K |
| `kernel/src/gui/` | `wm.c`（窓管理と合成）、`desktop.c`、`screenmode.c` | K |
| `kernel/src/vm/` | VM だけの処理のすべて（下の「境界」） | K |
| `kernel/src/{boot,init}.c` | `kmain`（boot.c）→ `kmain_late`（init.c）→ `C:\Nocturne\System\init` | K |
| `common/` | 核と libc の共有：`abi.h`、`display_abi.h`、`gpu_abi.h`、`store_format.h`、`edid.h`、`gfx.c`、Unifont | K+L |
| `user/libc/` | ISO C と ABI の包み（`sys.c`）、`gui.c`・`ui.c`、字体・画像、`media*.c`、`net.c`・`http*.c`・`tls.c`・`webnet.c`、`webstorage.c`・`webidb*.c`・`webcookie.c`、`webprefs.c`（ブラウザーの好み `AppData\Browser\preferences`：言語の一覧、検索の履歴、サイトのデータの例外と「閉じるときに消す」。各プロセスが見張る）、`websitedata.c`（設定の窓のサイトのデータ：保存先ごとの量と消去をサイトでまとめ、閉じるときに消す。窓の数は一時フォルダーの `windows`）、`parallel.c` | L |
| `user/libc/web/` | ブラウザーのエンジン（約 200 ファイル。下の「ブラウザーの流れ」） | L |
| `user/include/` | 公開の見出し（`nocturne.h`、`web.h`、`webnet.h`、`ui.h` …）。SDK にも入る | L |
| `user/apps/` | システムのプログラム（`browser*.c`（タブは `browser.c` の `struct tab`、設定の頁 `about:settings` は `browser_settings.h`）・`webfetch.c`・`uihost.c`・`init.c`・`sh.c`・`agent.c`、`*.cpp`、`*.ts`→`System\*.js`） | A |
| `ports/<名>/` | 移植の差分・設定：ffmpeg、freetype、go、lexbor、llvm、mesa（`nocturne_gl.c`）、nvidia（RM をアプリ `nvrm` にする OS 層）、rust、tcc、typescript、virgl（ホストの QEMU の DLL だけ）、wabt | P |
| `third_party/<名>/` | 取り込んだ上流：webref（仕様の IDL）、quickjs、lexbor、bearssl・ca、ffmpeg、freetype・brotli・img、mesa、nvidia-open（615.78.08）、tinycc、wabt、uacpi、unicode・formatjs、unifont、web_streams | P |
| `host/` | `store/store.c`（C: を作る・詰める・検査する）、`limine/` | H |
| `boot/limine.conf` | 起動の項目は 2 つ（desktop と `nogui`）。解像度は持たない | H |
| `rootfs/` | C: へそのまま入る（`Nocturne\`、`Users\User\` の見本） | — |
| `scripts/` | ビルド・媒体・試験・Hyper-V の道具 | H |
| `tests/`、`docs/` | 試験、仕様・台帳・日付入りの報告 | — |
| `build/`、`tools/`、`hyperv/`、`local/`、`.codegraph/`（無視） | 出力、同梱の道具、Hyper-V のディスク、GSP の firmware、codegraph の索引 | — |

生成物（手で直さず生成元を直す）：`web/js_bootstrap.inc` ← `js_*.js`＋`js_embed.py`、`js_worker_runtime.inc` ← `js_worker_embed.py`（どちらも `user/libc/web/` にある）、`web/webidl_data.inc` ← `third_party/webref/idl`＋`scripts/build_webidl.py`、`js_intl.js`・`js_segmenter.js`・`js_collator.js` ← `scripts/build_*.py`、`unicode_grapheme_data.h`・`wctype_data.h`・`web/unicode_bidi_data.h`（`:dir()` の強い双方向の型）← `scripts/unicode_*.py`、`common/unifont.bin` ← `scripts/unifont2bin.py`。

## 境界

- syscall の番号：`common/abi.h` の enum `SYS_*`（結果は `N_ERR_*`）。仕様は `docs/nocturne-native-abi.md`。画面・GPU は `display_abi.h`（`struct screen_format`）、`gpu_abi.h`（OpenGL の ABI ではない）。
- アプリのドライバー：`SYS_PCI_*`・`SYS_DMA_*` → `dev/pciuser.c`。claim するのは init だけで、NVIDIA の表示機能を `nvrm` へ渡す（`user/apps/init.c`）。
- syscall の道（AP はアプリを走らせ、核の共有物は BSP の持ち物）：

```
user/libc/sys.c int $0x80 → arch/isr.asm → isr_dispatch (arch/idt.c)
  BSP: → syscall_dispatch (sys/syscall.c) → sched_user_return
  AP:  → sched_ap_interrupt (sys/sched.c) → syscall_ap（時刻・TCP・事象待ち・窓の状態など）
       → 片付かなければ文脈を BSP へ → isr_dispatch_remote (idt.c) → syscall_dispatch
```

- VM と実機：`kernel/src/vm/vm.h` が唯一の入口（`vm_detect`、`vm_hosted()`、`vm_init`・`vm_storage_init`・`vm_net_probe`・`vm_audio_init`・`vm_session_init`。実機では即座に戻る）。裏に `hyperv.c`、`hv/`（VMBus・storvsc・netvsc）、`rdp.c`（拡張セッション）、`devices.h` の装置（ata・ac97・e1000・tulip・vbe）、`virtio_gpu.c`。`vm/` の外は `vm/vm.h` しか include しない。

## ブラウザーの流れ

各プログラムは libc の全部（web・QuickJS・Lexbor・FFmpeg・Mesa…）を怠惰な member として link する（Makefile の `LIBC_LINK`）。

| 段 | ファイル | 入口 |
|---|---|---|
| 主ループ | `user/apps/browser.c` | `browser_event`（`webnet_wait`→`web_wait_events`）、`document_step`（`web_tick`→`relayout`→`web_layout`、`redraw`→`draw_page`→`web_paint`）、`webnet_pump`、`web_deadline` |
| 取得 | browser.c → `user/libc/webnet.c` → 子 `webfetch --service` → `http.c`・`http2.c`・`tls.c` | `navigate_inner`、`host_request`→`webnet_submit`。戻りは `web_resource_loaded`（doc.c） |
| 文書・公開 API | `web/doc.c`（`web.h` の実装） | `web_live`・`web_parse`、`web_tick`（媒体→`web_js_tick`→`web_frames_tick`）、`web_layout`（`css_cascade`→`boxes_build`→`layout_doc`） |
| 解析 | `web/html_lexbor.c`・`html_lexbor_bridge.c`（Lexbor は木構築の状態だけ）、`html.c`（tag ID）、`html_encoding.c`（HTML の符号の嗅ぎ分け）、`xml.c`（XML の解析器、Expat。XMLDocument・DOMParser・XHR の XML） | `html_begin`、`html_resume`、`html_fragment`、`html_decode_bytes` |
| 符号・URL | `web/web_encoding.c`（Encoding Standard の唯一の持ち主：名札・decoder・encoder・BOM・html の誤りの形。表と手続きは Lexbor）、`web_url.c`（WHATWG URL、Lexbor。HTML の encoding-parse は文書の符号で query を符号化）。native の取得・CSS の `url()`・form の action は `util.c` の手書きの `web_resolve_url_owned` を通る（台帳 K80、二つ目の解析器） | `web_encoding_get`、`web_decode`、`web_url_parse` |
| DOM | `web/dom.c`（`node_t` は文書の持ち物）、`html_serialize.c`、`elements.c`、`xpath.c`（XPath 1.0、`js_xpath.js`・`js_xpath_native.h`） | `xpath_compile`・`xpath_evaluate` |
| CSS | `css.c`（解析・selector・cascade）、`cssprop.c`（性質と UA の表）、`color.c`（色の値の型 `struct css_color`：解析・計算値・直列化・色域の写像・補間。描画は装いの 8 ビットの欄、正確な値は `color_inherited`・`color_own`）、`animation.c`（CSS Transitions・CSS Animations・Web Animations の一つの時間の模型。性質ごとの補間の型は cssprop.c の表の `AN_*` の欄と `css_prop_animation`）、`cssom.c` | `css_cascade`、`css_color_interpolate`、`web_animations_tick` |
| 箱・配置 | `box.c`／`layout.c`（block・inline・table・flex・grid・abs） | `boxes_build`・`boxes_rebuild_local`／`layout_doc` |
| 描画 | `paint.c`（描画・当たり・ページ内検索） | `web_paint`、`web_hit_test` |
| JS | `js.c`（QuickJS の host）＋`js_*.c`・`js_*.js`（`js_bootstrap.inc` に埋込み）。Web IDL の形（interface object・prototype・member の名と length・列挙・定数）は束縛が組み上がった後に `js_webidl.c` が `webidl_data.inc` の表から一度に与える。窓と Worker の両方の realm に入る定義：`js_geometry.js`（DOMPoint・DOMRect・DOMQuad・DOMMatrix）、`js_custom_event.js`、`js_clone.js`（`registerSerializable`）、`js_compression.c`（Compression Streams、`deflate.c`・`brotli_codec.h`）。捕まらなかった例外は js.c の `report_exception` が窓へ ErrorEvent を出し（HTML「report an exception」）、取り消されなければ console へ | `web_js_start`、`web_js_tick`、`web_console_eval` |
| 枠 | `frame.c`、`frame_native.h` | `web_frames_tick`、`web_rendering_now` |
| 媒体 | `avmedia.c` → `user/libc/media*.c` → 子 `browsermediaworker`・`browsermseworker`。`js_canvas.c`、`js_webgl.c` → `ports/mesa/nocturne_gl.c` | `web_avmedia_tick` |
| Web Audio | `webaudio.c`、`webaudio_dsp.c`、`js_webaudio.c`・`.js`。出口は `n_audio_submit` | — |
| Worker・WASM | `js_worker.c` → 子 `browserjsworker`／`js_wasm.cpp`（wabt） | — |
| 保存 | `webstorage.c`、IndexedDB は `js_indexeddb.js`＋`js_indexeddb.c`（窓と Worker の両方の realm。Worker は `browserjsworker.c` の主ループ）→ `webidb_async.c` → 子 `browserstorageworker` → `webidb.c`（接続の表で窓と Worker の versionchange・blocked・close が出会う）、`webcookie.c`（永続の Cookie は webnet.c の `cookie_sync` が `AppData\Browser\cookies` へ。窓どうしは見張りで読み直す）、`js_opfs.c`、まとめは `websitedata.c` | — |
| F12・ログ | `user/apps/browser_devtools.h`＋`web/inspect.c`。console の行は `web_js_console`（js.c）→ `host_console`→`console_add`（browser.c）。`--log-console`／`--debug-js` で serial へ `[browser <level> @秒.ms] …` | — |

## ビルド・媒体・試験（手順は `nocturne-project` スキル）

- 入口 `python -X utf8 scripts/build.py -j 8 [目標]`（UCRT64 の make）。目標：`all`（img・vhd・vhdx・iso）、`qemu-image`（img だけ）、`run`、`test`・`test-quick`・`test-full`、`kernel`、`user`、`clean`。
- 出力 `build/kernel.elf`、`build/programs/`、`build/sdk/`、`build/c.list`、`build/nocturne.{img,vhd,vhdx,iso}`。C: の中身は `scripts/layout.py` → `build/c.list` → `build/host/store.exe fill`（`mkroots.py` は TLS の根の証明書で、C: とは無関係）。
- 任意の移植は道具があるときだけ：Go `tools/go`、TypeScript `tools/TypeScript`、Rust `tools/rust`、OS の中の LLVM `tools/llvm`、NVIDIA の firmware `local/firmware/`。
- 試験：`scripts/test.py [--quick] [--no-net] [群|群/名]`（`tests/runtests.c` の `tests[]`、114 群 225 件。例 `web/fonts`・`web/engine`・`web/javascript`）。QEMU を窓なしで起動し、`C:\Tests\autorun.sh` から tcc で組んで走らせる。結果 `build/test-serial.log`・`build/test-metadata.json`。`tests/check_*.py` はホストの検査で、まとめる runner は無い。
- WPT：`scripts/wpt.py --label <名>`（ホストの wptserve と QEMU 一台、ゲストの `tests/wptrunner.c` が枠ごとにブラウザーを開き、`tests/wpt/testharnessreport.js` が結果を返す。`--resume`、`--summary-only`）。結果 `build/wpt/<名>/results.jsonl`・`summary.txt`。QuickJS だけの test262 はホストの `tests/check_test262.py`。
- 画面で確かめる：`scripts/platform_qa.py`（ラベル専用の写し、`build/nocturne-platform/<label>/`）、`scripts/qtest.py`、または手で起動した QEMU。

## 記録と道具

- `docs/browser/LEDGER.md`：ブラウザーの表示と性能の正本（基準版、退行、未確認）。`docs/nvidia/LEDGER.md`：RTX 4070 を RM と Mesa へつなぐ作業の正本。
- `docs/` のそれ以外（約 250 ファイル、ほとんど名に日付）はその日の記録で、今の仕様は `nocturne-native-abi.md` だけ。
- `.claude/oboegaki.md`：ファイルに結び付けた覚え書き。`.claude/hooks/mokei.py` が触れた時に出す。`.claude/mokei/`：会話の記録。
- codegraph：`codegraph.json`（third_party を外す）、`.mcp.json` が `codegraph serve --mcp` を起こす。`SYS_*` の番号の呼出しと登録の表からの呼出しは索引に出ない。
- `scripts/hv-*.ps1`（Hyper-V はユーザーの担当）：`hv-boot`・`hv-esm`・`hv-input`・`hv-serial`・`hv-shot`・`hv-sock`。VM の作成は根の `hyperv.ps1`。

## 古い記述（直すまで信じない）

- README.md:51 の起動メニュー 3 項目と解像度 → `boot/limine.conf` は 2 項目で解像度を持たない。README.md:268 の tools 一覧 → llvm・rust・go・TypeScript もある。README.md:660 の `hv/`・`rdp/`・e1000・tulip・ATA の置き場所 → どれも `kernel/src/vm/`。README.md:668 の ports 一覧に llvm が無い。
- `scripts/test.py:20` の群の一覧（13 群）→ `tests/runtests.c` は 114 群。
