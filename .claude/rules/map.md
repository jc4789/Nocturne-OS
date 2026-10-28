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
| `user/libc/` | ISO C と ABI の包み（`sys.c`）、`gui.c`・`ui.c`、字体・画像、`media*.c`、`net.c`・`http*.c`・`tls.c`・`webnet.c`、`webstorage.c`・`webidb*.c`・`webcache.c`・`webcookie.c`、`webprefs.c`（ブラウザーの好み `AppData\Browser\preferences`：言語の一覧、検索の履歴、サイトのデータの例外と「閉じるときに消す」。各プロセスが見張る）、`websitedata.c`（設定の窓のサイトのデータ：保存先ごとの量と消去をサイトでまとめ、閉じるときに消す。窓の数は一時フォルダーの `windows`）、`parallel.c` | L |
| `user/libc/web/` | ブラウザーのエンジン（約 200 ファイル。下の「ブラウザーの流れ」） | L |
| `user/include/` | 公開の見出し（`nocturne.h`、`web.h`、`webnet.h`、`weburl.h`、`ui.h` …）。SDK にも入る | L |
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
| 解析 | `web/html_lexbor.c`・`html_lexbor_bridge.c`（Lexbor は木構築の状態だけ）、`html.c`（tag ID）、`html_encoding.c`（HTML の符号の嗅ぎ分け）、`xml.c`（XML の解析器、Expat。XMLDocument・DOMParser・XHR の XML と、XML の型の応答の閲覧文脈の文書（台本の終了タグで止まる、少しずつ読む）。どちらの解析器かは doc.c の `doc_parser_*` が隠し、型は `web_document_kind`） | `html_begin`、`html_resume`、`html_fragment`、`html_decode_bytes`、`xml_parser_begin`、`doc_parser_resume` |
| 符号・URL | `web/web_encoding.c`（Encoding Standard の唯一の持ち主：名札・decoder・encoder・BOM・html の誤りの形。表と手続きは Lexbor）、`web_url.c`（WHATWG URL、Lexbor。JS の URL・Location・`a.href` も、native の取得・CSS の `url()`・form の action・文書の base も、この一つの解析器。native の口 `web_url_resolve` はどのスレッドからでもよく、解析器を貸し出しの溜めから借りる。HTML の encoding-parse は doc.c の `doc_encoding_parse_url` が文書の符号を渡し、CSS・Fetch・CSP は UTF-8 の `web_resolve_url_owned`。URL を受けるどの層も、公開の見出し `user/include/weburl.h` の URL の記録 `struct web_url`（直列化と各部分の位置・host の種類・port）を `web_url_read` で受け取り、文字列を切り直さない：要求の層 http.c の `http_target_of`（host・Host の欄・port・要求の対象）、Cookie の瓶、webfetch、webnet、媒体、保存の層、エンジン。URL の origin とその直列化・same origin・保存の鍵の確かめ `web_origin_http`・Secure Contexts の `web_url_potentially_trustworthy`・URL の setter `web_url_change` もここ。same site は PSL を持つ `webcookie.c` の `webcookie_same_site`） | `web_encoding_get`、`web_decode`、`web_url_parse`、`web_url_resolve`、`doc_encoding_parse_url`、`web_url_read`、`web_url_origin` |
| DOM | `web/dom.c`（`node_t` は文書の持ち物）、`html_serialize.c`、`elements.c`、`xpath.c`（XPath 1.0、`js_xpath.js`・`js_xpath_native.h`） | `xpath_compile`・`xpath_evaluate` |
| CSS | `css.c`（解析・selector・cascade）、`cssprop.c`（性質と UA の表）、`cssmath.c`（単位と CSS Values 4 §10 の数式の唯一の持ち主：解析・型・単純化・計算値と使用値への解決・直列化の木 `struct css_calc`、`len_t` の式と長さの補助。長さ・数・時間・角度・形・色の成分がここを呼ぶ）、`color.c`（色の値の型 `struct css_color`：解析・計算値・直列化・色域の写像・補間。描画は装いの 8 ビットの欄、正確な値は `color_inherited`・`color_own`）、`animation.c`（CSS Transitions・CSS Animations・Web Animations の一つの時間の模型。性質ごとの補間の型は cssprop.c の表の `AN_*` の欄と `css_prop_animation`）、`cssom.c`。Anchor Positioning の値は cssprop.c（装いの外の群 `struct css_anchor`、装いの `anchor`、NULL が初期値。`anchor()`・`anchor-size()` は cssmath.c の木の葉 `N_ANCHOR`（引数の解析・直列化は cssprop.c の `css_anchor_function_*`、置き換えは `css_calc_map_anchors`）、try tactic、@position-try が受け入れる性質）、`@position-try` の規則と position fallback origin は css.c の `position_try_styles`。CSS Text と Text Decoration の値も cssprop.c：継がれる群 `struct css_text`（装いの `text`。text-shadow・overflow-wrap・word-break・line-break・hyphens・hyphenate-*・tab-size・text-underline-offset/position。子は親の群を指で借りる）と継がれない群 `struct css_decoration`（装いの `decoration`。text-decoration-color/style/thickness）。飾りを引く箱の一覧 `struct css_decorating`（装いの `decorations`、自分が先、各項は引く箱の使用値）は css.c の `style_decorations` が作る。legacy name alias（word-wrap・grid-*-gap）は表の `aliases` の `legacy` の欄で、CSSOM の口は js.c の `native_css_properties` が足す。要素の inline の宣言は css.c の `css_inline_set`・`css_inline_replace`・`css_inline_text`：CSSOM の変更は宣言を与えられた値のまま要素の `style_cssom` に持ち、style 属性はその直列化（指定値の直列化は丸めることがあるので、cascade は属性の文を読み直さない）。CSS Typed OM は `js_typed_om.js`（類・reify・内部の表現・StylePropertyMap）＋`js_typed_om.c`（native の口）、性質ごとの reify の規則は cssprop.c の `css_prop_reify`、色は color.c の `css_color_reify`、数の型・木は cssmath.c | `css_cascade`、`css_calc_read`・`css_numeric_read`・`css_calc_specified`、`css_color_interpolate`、`web_animations_tick`、`css_inline_set` |
| 箱・配置 | `box.c`／`layout.c`（block・inline・table・flex・grid・abs）。文字の幾何（DOM の Text の位置と行の run・deco の往復）の唯一の持ち主は layout.c の「text geometry」の節：Range・CaretPosition の矩形、caretPositionFromPoint の位置、Element の行内の断片が読む。箱の `frag_first`・`frag_end` が自分の run・deco の範囲、処理後の文字と Text のデータのバイトの対応は box.c の `box_text_source_map`。Anchor Positioning は絶対配置の段の中で一度に解く：`layout_abs`→`anchor_layout`（anchor の箱を探す、position-area の格子、anchor-center、位置の選択）。配置の間の記録 `struct box_anchor`（箱の `anchored`）を描画（`box_anchor_hidden`・`box_anchor_shift`）と getComputedStyle（`box_anchor_style`・`box_anchor_inset`）が読む。自己整列の `normal` は `AI_NORMAL`、他の配置は `ai_used` で読む。行分けの割れ目（css-text-4 §6、UAX #14 の類の近似）の唯一の持ち主は layout.c の `wrap_between`（`wrap_rules_of` が word-break・line-break・hyphens を読む）：`text_items`（語は inline の箱の境をまたぐ。`ibuild` の `word_item`）、`control_text_lines`（input・textarea）、`inline_intrinsic` が呼ぶ。overflow-wrap の非常の割れ目は `wrap_emergency`→`split_word`、soft hyphen の線は `finish_line` が run の `hyphen` に記し、字は `layout_hyphen_string`。hyphenate-limit-chars は `hyphen_limits`、hyphenate-limit-lines は `iline` の `hyphenated`。保った tab は一つの項目（`item` の `tab`）で、幅は置かれた行で `tab_advance`（box.c の `ws_process` は tab と soft hyphen を残す） | `boxes_build`・`boxes_rebuild_local`／`layout_doc`、`layout_abs`、`layout_text_rects`・`layout_inline_rects`・`layout_position_at` |
| counter・リストの印 | 値（counter-reset・increment・set、quotes、list-style-*、content、装いの外の群 `struct css_counters`・`css_list`・`css_content`、::marker の装い `st->marker`）は cssprop.c、ol・li の presentational hint（li value → counter-set、ol reversed・start → counter-reset）は css.c。数える唯一の持ち主は `counter.c`：木の順に counter を継ぎ・作り（CSS Lists 3 §4.4.1 の Issue 5477：親の組が先、兄弟からは親に無い名だけ）、reversed() の初期値（§4.4.2）は一回目の歩きで測って二回目で書く。counter style（定義済みと symbols()）と引用の深さもここ。数える箱の印 `counter_kind`・`counter_tree` と印の箱（inside は inline の箱と字の箱、outside は `box->marker`）は box.c が作り、counter.c が `box_counter_text`・`box_counter_marker` で字を書く（変わった箱だけ配置を無効に）。ホストの検査 `tests/check_counters_native.py` | `web_counters_update`、`css_counter_text` |
| 描画 | `paint.c`（描画・当たり・ページ内検索）。当たりは文字の run も記録し、`doc_caret_at` が caretPositionFromPoint の位置を返す。input・textarea の値の置き場（`input_line_*`・`control_offset_at`・`doc_control_caret_rect`）は描画と caret が共有。文字の描画は `paint_run_text`：影（`paint_text_shadows`、ぼかしは box-shadow と同じ `shadow_gaussian`。塊ごとに全 run の影を先に描く）→下線・上線→字→取消線（`run_lines`→`decoration_line`、外の飾りの箱から） | `web_paint`、`web_hit_test`、`doc_caret_at` |
| JS | `js.c`（QuickJS の host）＋`js_*.c`・`js_*.js`（`js_bootstrap.inc` に埋込み）。Web IDL の形（interface object・prototype・member の名と length・列挙・定数）は束縛が組み上がった後に `js_webidl.c` が `webidl_data.inc` の表から一度に与える。DOM の集まり（NodeList・HTMLCollection と派生）は `js_collections.js`（interface の member と各集まりが読むもの）＋`js_collections_native.h`（QuickJS の exotic な class：添字と名前の内部の手順、DOM の版の比べ）。窓と Worker の両方の realm に入る定義：`js_fetch.js`（Fetch の Headers・Request・Response・fetch・Body の唯一の持ち主 `fetchInterfaces(realm)`。realm が渡すのは AbortSignal の `signals`・native の `transport`・base URL・origin・referrer・data: の口だけ。窓の口は js_bootstrap.js。scheme fetch「blob」は `blobFetch` 一つで、Worker の表に無い blob: URL は `transport.obtainBlob` が親の js.c `worker_blob` に頼む。native の応答のメタ行（cors・redirected・opaque・opaqueredirect）の読み手は `js_worker_wire.h` の `njw_transport_flags` 一つ（js.c・js_worker.c）、redirect の manual の opaque-redirect は webfetch.c の `response_start`）、`js_abort.js`（AbortSignal・AbortController の唯一の持ち主 `abortInterfaces`）、`js_object_url.js`（blob URL の表 `objectURLInterfaces(realm)`）、`js_xhr.js`（XMLHttpRequest・ProgressEvent の唯一の持ち主 `xhrInterfaces(realm)`・`progressEventInterface`。要求はどれも realm の Fetch の口 `xhrFetch`・`xhrFetchSync` を通る。realm が渡すのは URL の解析（窓は文書の base と符号、Worker は API の base URL と UTF-8）・`document` の口（窓だけ。responseXML と "document" の応答、Document の本文）・同期の可否）、`js_geometry.js`（DOMPoint・DOMRect・DOMQuad・DOMMatrix）、`js_typed_om.js`（CSS Typed OM の `typedOMInterfaces(options)`。窓は宣言の塊と計算値の口も渡し、Worker は値の類だけ）、`js_custom_event.js`、`js_clone.js`（`registerSerializable`）、`js_compression.c`（Compression Streams、`deflate.c`・`brotli_codec.h`）。捕まらなかった例外は js.c の `report_exception` が窓へ ErrorEvent を出し（HTML「report an exception」）、取り消されなければ console へ。Range の矩形と `caretPositionFromPoint`・`CaretPosition` は js_range.js（DOM の操作 `rangeRects`・`caretPosition`・`caretRect`、Range の木の歩きは js.c の `range_client_rects`） | `web_js_start`、`web_js_tick`、`web_console_eval` |
| 枠 | `frame.c`、`frame_native.h` | `web_frames_tick`、`web_rendering_now` |
| 媒体 | `avmedia.c` → `user/libc/media*.c` → 子 `browsermediaworker`・`browsermseworker`。`js_canvas.c`、`js_webgl.c` → `ports/mesa/nocturne_gl.c` | `web_avmedia_tick` |
| Web Audio | `webaudio.c`、`webaudio_dsp.c`、`js_webaudio.c`・`.js`。出口は `n_audio_submit` | — |
| Worker・WASM | `js_worker.c` → 子 `browserjsworker`／`js_wasm.cpp`（wabt）。Worker の fetch：`js_worker_runtime.js` の `workerFetchTransport` → NJW_LOAD（mode は kind の旗）・NJW_FRAME・NJW_RESUME（`user/include/js_worker_wire.h`）→ js.c の `worker_request`・`worker_fetch_frame`（作った窓の webnet の要求。opaque は js_worker.c が空にして渡す）。同期の XMLHttpRequest は `workerFetchTransport.fetchSync` → `host.requestWait`（browserjsworker.c の `loaded_wait` が主ループを止め、ほかの記録は後へ回す） | — |
| 時間の計測 | 各要求の時刻は一つの記録 `struct http_fetch_timing`（`http.h`）：tls.c・http.c・webfetch.c が書き、私的な行で webnet.c へ運び、`webnet_response.timing` → browser.c・`frame_native.h` の `frame_host`（文書の航行）／js.c の `web_js_loaded`（資源）。Worker 自身の要求（fetch・importScripts・XHR）の記録は js.c の `worker_timing` が NJW_LOADED_TIMING で Worker へ送り、Worker の `performanceBridge.monotonicResource` が Worker の timeline に入れる。JS の項目は js.c の `timing_value` → `js_performance.js`（Performance Timeline・User Timing・Navigation・Resource・Paint・Server Timing）。first-paint・first-contentful-paint は paint.c の `paint_document` が文書に記し、js.c の `report_paint_timing` が出す | `host.navigationTiming`、`resourceTiming`・`paintTiming`・`navigationTimingQueued` の hook |
| 保存 | `webstorage.c`、IndexedDB は `js_indexeddb.js`＋`js_indexeddb.c`（窓と Worker の両方の realm。Worker は `browserjsworker.c` の主ループ）→ `webidb_async.c` → 子 `browserstorageworker` → `webidb.c`（接続の表で窓と Worker の versionchange・blocked・close が出会う）、`webcookie.c`（永続の Cookie は webnet.c の `cookie_sync` が `AppData\Browser\cookies` へ。窓どうしは見張りで読み直す）、`js_opfs.c`、Cache Storage は `js_cache_storage.js`（窓と Worker の realm。Fetch の記録は窓が `js_fetch.js` の `cache`、Worker が `js_worker_runtime.js` の `workerFetchBridge.cache`）＋`js_cachestorage.c` → IndexedDB と同じ `webidb_async.c` の列 → 子 `browserstorageworker` → `webcache.c`（`AppData\Browser\caches\<origin の SHA-256>`、命令ごとに origin の lock を借りて disk の状態を読む）、まとめは `websitedata.c` | — |
| 文書の方針 | `web/html_policy.c`（CSP の方針の模型と判定、Trusted Types の方針、frame-ancestors・X-Frame-Options、meta の CSP）。判定は各所が呼ぶ：取得は js.c の `send_request`・`web_js_loaded`（転送の後）・`worker_request`・`native_socket_open`・`request_styles`、inline は `queue_script`・`js_bootstrap.js` の handler・doc.c の style 要素・dom.c の style 属性、文字列の compile は `js_html_safety_host.h`、form-action は `web_form_navigate`、base-uri は doc.c。違反の報告は窓が `js_policy_report.h` の `csp_report`、Worker が `browserjsworker.c` の `worker_csp_report` | `csp_request_allowed` |
| F12・ログ | `user/apps/browser_devtools.h`＋`web/inspect.c`。console の行は `web_js_console`（js.c）→ `host_console`→`console_add`（browser.c）。`--log-console`／`--debug-js` で serial へ `[browser <level> @秒.ms] …` | — |

## ビルド・媒体・試験（手順は `nocturne-project` スキル）

- 入口 `python -X utf8 scripts/build.py -j 8 [目標]`（UCRT64 の make）。目標：`all`（img・vhd・vhdx・iso）、`qemu-image`（img だけ）、`run`、`test`・`test-quick`・`test-full`、`kernel`、`user`、`clean`。
- 出力 `build/kernel.elf`、`build/programs/`、`build/sdk/`、`build/c.list`、`build/nocturne.{img,vhd,vhdx,iso}`。C: の中身は `scripts/layout.py` → `build/c.list` → `build/host/store.exe fill`（`mkroots.py` は TLS の根の証明書で、C: とは無関係）。
- 任意の移植は道具があるときだけ：Go `tools/go`、TypeScript `tools/TypeScript`、Rust `tools/rust`、OS の中の LLVM `tools/llvm`、NVIDIA の firmware `local/firmware/`。
- 試験：`scripts/test.py [--quick] [--no-net] [群|群/名]`（`tests/runtests.c` の `tests[]`、117 群 227 件。例 `web/fonts`・`web/engine`・`web/javascript`）。QEMU を窓なしで起動し、`C:\Tests\autorun.sh` から tcc で組んで走らせる。結果 `build/test-serial.log`・`build/test-metadata.json`。`tests/check_*.py` はホストの検査で、まとめる runner は無い。URL を読む層を組む検査は、URL の持ち主（`web_url.c` と Lexbor）を `tests/url_owner.py` の `objects()` で結ぶ。
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
- `scripts/test.py:20` の群の一覧（13 群）→ `tests/runtests.c` は 117 群。
