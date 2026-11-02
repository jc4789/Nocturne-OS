# Nocturne の地図

今のリポジトリの地図。どこに何があるかを短く書く。

- 探す前に読む。ここに無い所は codegraph と rg で探す。
- 地図とコードが違えば、コードが正しい。気づいたら、その場で地図の行を書き直す。
- 書くのは今の形だけ。日付、コミット、作業の段、変更の経緯、付録は書かない。経緯は git にある。
- 新しい場所ができたら行を足す。消えた場所の行は消す。
- README と docs の日付入りの報告は古いことがある。

## カーネル `kernel/src/`

| 場所 | 役目 |
|---|---|
| `boot.c`・`init.c` | 起動の流れ。`kmain` から `kmain_late` へ進み、`C:\Nocturne\System\services` を根の能力つきで起こす。 |
| `arch/cpu.c`・`tsc_rate.h` | CPUID の唯一の読み手。下限 Zen 4 を照らし、任意の機能（PCID、MWAIT など）の有無と、CPU が申告する TSC の周波数を持つ（書式は社と family ごとに `tsc_rate.h`）。申告が無ければ `sys/clock.c` が測る。どの CPU も `cpu_setup` で同じ初期化をする。 |
| `arch/topology.c` | MADT・SRAT・SLIT から CPU・core・LLC・NUMA node と node の距離を数える。CPU の数に上限は無い。 |
| `arch/smp.c`・`wake.c` | AP の起動、panic のときの NMI による停止、S3 からの復帰。 |
| `arch/spin.h` | MCS spinlock。 |
| `arch/irq.c`・`apic.c`・`ioapic.c`・`lapic_timer.c` | CPU ごとの vector の表、x2APIC と IPI、IOAPIC、LAPIC の timer（CPU に TSC-deadline があればそれ、無ければ one-shot）。 |
| `arch/idt.c`・`gdt.c`・`isr.asm` | IDT・GDT・TSS と、syscall・例外・割り込みの入口。 |
| `arch/fpu.c`・`idle.c`・`pat.c` | XSAVES による FPU の保存、MWAIT か HLT による idle と性能の要求（AMD の CPPC、Intel の HWP）、PAT。 |
| `lib/` | `span`（区間の AVL 木）、`avl`、`idtable`、`lz`、`printf`、`sort`、`string`。 |
| `sys/proc.c` | `proc_launch` が ELF のアプリを起こす。 |
| `sys/clock.c`・`alarm.c` | TSC による三つの時計（monotonic、boot、wall）と、CPU ごとの期限の heap。 |
| `mm/vmm.c`・`pmm.c`・`heap.c`・`vmalloc.c` | 頁表と TLB、frame、heap、核の番地の区間。 |
| `mm/shared.c`・`pager.c`・`program.c`・`space.c` | 共有 object（frame、写像の一覧、fault、revoke、DMA の型、device の pin、scanout）、pager、program の text、アプリの配置（穴、`n_memory_*`）。口は `mm/shared.h`・`mm/space.h`、錠の順は `mm/shared.c` の頭。 |
| `sys/object.c`・`channel.c`・`events.c`・`watch.c` | object と handle の表、channel、event queue、watch。 |
| `sys/syscall.c` | syscall の受け口 `syscall_dispatch`。 |
| `sys/log.c`・`panic.c`・`stream.c`・`prof.c` | log の ring buffer、panic の報告、console と system log の stream、profiler。 |
| `sys/keep.c` | wm・audio・screenmode の設定を `AppData\Nocturne\` の `.conf` に保存し、init の前に読み戻す。 |
| `fs/` | C: の store の実行側。mount とパスの操作は `store.c`、開いた文書は `document.c`、block cache は `cache.c`、変更の watch は `watch.c`。書式のエンジンは `common/store/`。 |
| `dev/` | PCI と `pciuser`（アプリのドライバー）、ACPI（uACPI）、AHCI、NVMe、xHCI と USB、IOMMU、音（`audio` が流れ・混合・出口の選択、`hda`・`hda_codec` が High Definition Audio）、`fb`（画面ごとの面・フリップ・垂直帰線）と `fbcon`、入力（USB HID）、`serial`、`refclock`（HPET か ACPI PM timer と、RTC）、電源と hibernate。 |
| `net/` | `net.c`（Ethernet から TCP・DHCP・DNS まで）と `rtl8125.c`（実機の NIC）。 |
| `gui/` | `wm.c`（窓管理と、画面ごとの合成のスレッド）、`desktop.c`、`screenmode.c`（画面ごとのモードの選択）。 |
| `vm/` | VM だけの処理。下の「境界」を見る。 |

## 共有 `common/`

| 場所 | 役目 |
|---|---|
| `abi.h` | syscall の番号 `SYS_*` と結果 `N_ERR_*`。仕様は `docs/nocturne-native-abi.md`。 |
| `authority_abi.h` | プロセスの身元 `struct n_identity`、級 `enum n_class`、能力 `enum n_cap`。 |
| `services_abi.h` | サービスの接続と制御の記録、管理者・`accounts`・`session`・`sharedworkers` の口の型。 |
| `display_abi.h`・`gpu_abi.h` | 画面と GPU の ABI。 |
| `store_format.h`・`store/` | C: の store の書式とエンジン（`catalog` の B+ 木、`namespace`、`space`、`volume`）。カーネルと `host/store` の両方が組む。 |
| `edid.h`・`gfx.c`・`unifont.bin` | EDID、描画の部品、Unifont。 |

## ユーザー空間 `user/`

| 場所 | 役目 |
|---|---|
| `libc/` | ISO C、ネイティブ ABI の包み（`sys.c`）、サービスの接続と身元（`services.c`、見出しは `nocturne_services.h`）、GUI（`gui.c`、`ui.c`）、字体と画像、媒体（`media*.c`）、網（`net.c`、`http*.c`、`tls.c`、`webnet.c`）、ブラウザーの保存（`webstorage.c`、`webidb*.c`、`webcache.c`、`webcookie.c`、`webprefs.c`、`websitedata.c`）。 |
| `libc/web/` | ブラウザーのエンジン。下の「ブラウザーの流れ」を見る。 |
| `include/` | 公開の header（`nocturne.h`、`web.h`、`weburl.h`、`ui.h` など）。SDK にも入る。 |
| `apps/` | システムのプログラム。サービスの管理者 `services.c`（宣言の表 `declared[]`）とサービス（`accounts.c`、`session.c`、`systemlog.c`、`sharedworkers.c`）、セッションの `desktop.c`、ブラウザー（`browser*.c`）、`sh.c`、`uihost.c`、`webfetch.c`、`agent.c`、小さな道具とゲーム、TypeScript のアプリ（`*.ts`）。 |

## その他

| 場所 | 役目 |
|---|---|
| `ports/<名>/` | 移植の差分と設定。expat、ffmpeg、freetype、go、lexbor、llvm、mesa、nvidia、rust、tcc、typescript、virgl、wabt。 |
| `third_party/<名>/` | 取り込んだ上流のソース。 |
| `host/store/store.c` | ホストの道具。C: を作り、詰め、検査する。 |
| `boot/limine.conf` | 起動の項目は desktop と `nogui` の二つ。 |
| `rootfs/` | C: にそのまま入るもの（`Nocturne\`）と、新しい利用者のフォルダーの元（`NewUser\`、C: の `Nocturne\NewUser`）。 |
| `scripts/` | ビルド、媒体、試験、生成、Hyper-V の道具。 |
| `tests/` | ゲストの試験（`runtests.c`）、試験の媒体のサービス（`testrun.c`）、WPT の runner、ホストの検査 `check_*.py`（`check_hda_codec.py` は codec の表で widget の木を試す）。 |
| `docs/` | 仕様 `nocturne-native-abi.md`、台帳 `*/LEDGER.md`、日付入りの報告。 |
| `build/`・`tools/`・`hyperv/`・`local/`・`.codegraph/` | git の外。ビルドの出力、同梱の道具、Hyper-V のディスク、GSP の firmware、codegraph の索引。 |

## 生成物

手で直さず、生成元を直す。

| 生成物 | 生成元 |
|---|---|
| `user/libc/web/js_bootstrap.inc` | `user/libc/web/js_*.js` と `user/libc/web/js_embed.py` |
| `user/libc/web/js_worker_runtime.inc` | `user/libc/web/js_worker_embed.py` |
| `user/libc/web/webidl_data.inc` | `third_party/webref/idl` と `scripts/build_webidl.py` |
| `js_intl.js`、`js_segmenter.js`、`js_collator.js` | `scripts/build_intl.py`、`scripts/build_segmenter.py`、`scripts/build_collator.py` |
| `unicode_grapheme_data.h`、`wctype_data.h`、`unicode_bidi_data.h` | `scripts/unicode_grapheme.py`、`scripts/unicode_wctype.py`、`scripts/unicode_bidi.py` |
| `common/unifont.bin` | `scripts/unifont2bin.py` |
| `kernel/src/dev/audio_pace.h` | `scripts/audio_pace_table.py` |

## 境界

- syscall の番号と型は `common/abi.h`。画面と GPU は `display_abi.h` と `gpu_abi.h`。
- アプリのドライバーは `SYS_PCI_*`・`SYS_DMA_*` から `dev/pciuser.c` へ。claim は `N_CAP_PCI_CLAIM` の能力で行う。`user/apps/services.c` の `gpu` の項目が NVIDIA の表示の機能ごとに `nvrm` を起こし、`nvrm` が自分で claim する。
- VM の処理は `kernel/src/vm/vm.h` が唯一の入口。実機では何もせずに戻る。裏に `hyperv.c`、`hv/`（VMBus、storvsc、netvsc）、`rdp.c`、VM の装置（ata、ac97、e1000、tulip、vbe）、`virtio_gpu.c` がある。`vm/` の外は `vm/vm.h` だけを include する。
- syscall の道：

```
user/libc/sys.c の SYSCALL
  → arch/isr.asm の syscall_entry（thread のカーネル stack へ移る）
  → sys/syscall.c の syscall_dispatch（アプリのメモリーは copy_from_user・copy_to_user だけで触る）
  → SYSRET
例外は arch/idt.c の isr_exception。NMI・#DB・#MC・#DF は CPU ごとの IST の stack で受ける。
割り込みは arch/irq.c の isr_interrupt。CPU ごとの割り込みの stack で受け、眠らない。
```

## ブラウザーの流れ

各プログラムは libc の全部を lazy な member として link する（Makefile の `LIBC_LINK`）。ファイルは断りが無ければ `user/libc/web/` にある。

| 段 | ファイルと入口 |
|---|---|
| 主ループ | `user/apps/browser.c` の `browser_event`、`document_step`、`redraw`。 |
| 取得 | `user/libc/webnet.c` から子プロセス `webfetch --service` へ。そこから `http.c`、`http2.c`、`tls.c`。応答は `doc.c` の `web_resource_loaded` に戻る。 |
| 文書 | `doc.c` が `web.h` を実装する。`web_parse`、`web_tick`、`web_layout`。 |
| 解析 | HTML は `html_lexbor.c`（Lexbor）、XML は `xml.c`（Expat）。符号の判定は `html_encoding.c`。 |
| 符号と URL | `web_encoding.c`（Encoding Standard）と `web_url.c`（WHATWG URL）。URL を受ける層は `struct web_url` を `web_url_read` で受け取る。 |
| DOM | `dom.c`、`elements.c`、`html_serialize.c`、`xpath.c`。 |
| CSS | `css.c`（解析、selector、cascade）、`cssprop.c`（property の表）、`cssmath.c`（単位と calc）、`color.c`、`animation.c`、`cssom.c`。 |
| box と配置 | `box.c`（box の木）と `layout.c`（block、inline、table、flex、grid、絶対配置、Anchor Positioning、行分け）。 |
| counter | `counter.c`（counter とリストの marker）。 |
| 描画 | `paint.c`（描画、hit test、ページ内検索）と `filter.c`。 |
| JS | `js.c`（QuickJS の host）と `js_*.c`・`js_*.js`。Web IDL の形は `js_webidl.c`、Fetch は `js_fetch.js`。 |
| frame | `frame.c` と `frame_native.h`。 |
| 媒体 | `avmedia.c` から `user/libc/media*.c`、子プロセス `browsermediaworker`・`browsermseworker` へ。canvas は `js_canvas.c`、WebGL は `js_webgl.c` から `ports/mesa/nocturne_gl.c` へ。 |
| Web Audio | `webaudio.c`、`webaudio_dsp.c`、`js_webaudio.c`。出口は `n_audio_submit`。 |
| Worker と WASM | `js_worker.c` から子プロセス `browserjsworker` へ。WASM は `js_wasm.cpp`（wabt）。 |
| 時間の計測 | 要求ごとの `struct http_fetch_timing`（`user/include/http.h`）を `js.c` が受け、`js_performance.js` が Performance API に出す。 |
| 保存 | `user/libc/webstorage.c`。IndexedDB は `js_indexeddb.js`・`js_indexeddb.c` から `user/libc/webidb_async.c`、子プロセス `browserstorageworker`、`user/libc/webidb.c` へ。Cache Storage は `js_cache_storage.js`・`js_cachestorage.c` から `user/libc/webcache.c` へ。ほかに `user/libc/webcookie.c`、`js_opfs.c`。 |
| 方針 | `html_policy.c`（CSP、Trusted Types、frame-ancestors）。 |
| F12 とログ | `user/apps/browser_devtools.h` と `inspect.c`。`--log-console` と `--debug-js` で serial に `[browser …]` の行を出す。 |

## ビルド・媒体・試験

規則は AGENTS.md の「ビルドと媒体」、手順は `nocturne-project` スキルにある。ここは場所だけを書く。

- 入口は `python -X utf8 scripts/build.py -j 8 <目標>`。目標は Makefile の `qemu-image`、`all`、`kernel`、`user`、`clean` など。
- 出力は `build/kernel.elf`、`build/programs/`、`build/sdk/`、`build/c.list`、`build/nocturne.img`・`.vhdx`・`.iso`。
- C: の中身は `scripts/layout.py` が `build/c.list` を作り、`build/host/store.exe fill` が詰める。
- 任意の移植の道具は `tools/go`、`tools/TypeScript`、`tools/rust`、`tools/llvm`、`local/firmware/`。
- ゲストの試験の表は `tests/runtests.c` の `tests[]`。回すのは `scripts/test.py`。
- WPT は `scripts/wpt.py <path>…`。ゲストの側は `tests/wptrunner.c`。
- QuickJS の test262 はホストの `tests/check_test262.py`。

## 記録と道具

- `docs/browser/LEDGER.md` と `docs/nvidia/LEDGER.md` は過去の記録。
- `.claude/oboegaki.md` はファイルに結び付けた覚え書き。触れる前に対象のパスで引く。
- `.claude/ban/` は会話ごとの盤。`.claude/hooks/ban.py` が見せる。
- codegraph の設定は `codegraph.json`、起動は `.mcp.json`。`SYS_*` の番号での呼出しと、表からの呼出しは索引に出ない。
- `scripts/hv-*.ps1` と根の `hyperv.ps1` は Hyper-V の道具。Hyper-V はユーザーの担当。
