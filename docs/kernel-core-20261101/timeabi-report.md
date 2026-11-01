# timeabi の報告：時間と締切を ns でアプリまで通す

枝は `worktree-agent-a0f0ad79836d456f0`。土台は main の b6388a38。

## 依頼文（全文）

```
あなたの名は「timeabi」。まず `D:\Programes\llm vibe slop os\docs\kernel-core-20261101\common-brief.md` を読み、その全部に従う。土台は main の b6388a38。QEMU のポートは HMP 45464・QMP 45465。

# 仕事：時間と締切をアプリまで ns で通し、アプリの thread が締切の級を求められるようにする
監査 `audit-kernel.md` の要点 7 を読むこと。今の時間の ABI は ms だけ（`kernel/src/sys/syscall.h`、`sys_events_timer`、`SYS_SLEEP`、`SYS_UPTIME`）で、600 Hz の拍 1.667 ms を表せない。`n_event` に時刻が無い。カーネルの内側には ns の期限と alarm（`sys/alarm.c`、`sys/clock.c`）がある。

必ず入れる形：
- 時間を受け渡す ABI は全部 ns。期限は単調時計の絶対の ns（相対の ms でなく）。眠り・timer・事象の待ちの期限・時計の読み（monotonic・boot・wall）。ms の口は消す（互換は持たない）。呼ぶ側（`user/libc/`、`user/apps/`、移植の接着 `ports/*` の Nocturne の部分、Go・Rust・QuickJS の時計と眠り、`user/libc/web/` の timer）を全部新しい口に直す。
- 時計の読みを syscall なしでできるなら、その形を考える（たとえばカーネルが読むだけの頁で TSC の換算の値を渡す）。入れるなら、不変の TSC を前提にしてよい（下限は Zen 4）。VM の都合を共通コードに入れない。
- `n_event` に時刻（単調時計の ns）の欄を持たせ、事象を作る所で入れる。窓と入力の事象の中身（画面・窓の欄）は「compositor」の書き手が触るので、あなたは `n_event` の共通の時刻の欄と、それを入れる共通の道だけを持つ。
- アプリの thread が締切の級（周期 ns、中核の `sched_deadline`）と公平の級の重みを求める ABI を、Nocturne の形で一つ足す（たとえば thread の handle に対する一つの syscall）。中核の受入の範囲（`SCHED_PERIOD_MIN_NS`〜`MAX_NS`）を守り、一つのアプリが CPU を取り切れないこと（中核の CBS がそれを持つか、原文で確かめる）。libc に包みを置き、ブラウザーの requestAnimationFrame の thread や媒体の出力の thread のような周期の仕事が使えるようにする（どこで使うかは原文を読んで決め、使う所まで入れる）。
- 仕様書 `docs/nocturne-native-abi.md` を同じコミットで合わせる。

範囲の外：中核の中身は使うだけ（口が足りなければ最小に足す）。`common/abi.h` は三人が触るので、自分の部分だけを変え、ほかの行を並べ替えない。

確かめ：組み（`kernel`・`qemu-image`。`qemu-image` が Rust の std の `setenv` で止まるなら、`RUST_SYSROOT=none` を付けて組んでよい。その件は別の書き手が直す）。QEMU で起動、Terminal の `uptime`・`ps`、時計の表示、ブラウザーで実サイト（Wikipedia）を開き、JS の timer と requestAnimationFrame が動くこと。ゲストの試験の表（`tests/runtests.c`）に時間の試験があれば `scripts/test.py` で回す。
```

## 設計

### 時間の受け渡し
- ABI が渡す時間は全部 ns。期限は単調時計の上の絶対の時刻（ns）。`N_CLOCK_NEVER`（`UINT64_MAX`）は期限なし。呼出しの時点で過ぎた期限は待たずに見るだけ。
- 期限を取る呼出し：`SYS_SLEEP`、`SYS_EVENTS_TIMER`（最初の時刻と周期の ns）、`SYS_EVENTS_WAIT`、`SYS_WAIT_ADDRESS`・`WAIT_ADDRESS64`、`SYS_APP_RESULT`、`SYS_PAGER_TAKE`、`SYS_WIN_RECEIVE`、`SYS_PCI_INTERRUPT_TAKE`・`SCREEN_STATE`・`ACPI_NOTIFY_TAKE`、網の待つ呼出し（`TCP_CONNECT`・`TCP_RECV`・`TCP_ACCEPT`・`TCP_SEND_UNTIL`・`UDP_RECV`・`PING`）。カーネルは受けた期限をそのまま alarm へ渡す。ms から換える層（`syscall_deadline`・`address_deadline`）は消した。
- `n_procinfo` の `cpu_ns`・`start_ns`、`n_sysinfo` の `uptime_ns`、`net_ping` の往復も ns。

### 時計を呼出しなしで読む
- カーネルの時計のデータ（`sys/clock.c` が持つ seqlock の組：`base_tsc`・`base_ns`・`mult`・`shift`・`asleep_ns`・壁時計と slew・CPU ごとの TSC の offset）を、`struct n_clock` として `common/abi.h` に置いた。カーネルはその頁を全アプリの `N_CLOCK_ADDRESS`（主スタックの上、`USER_TOP` の下）に読取りだけ（`PTE_SHARED_RO`、NX）で写す（`clock_map`、`proc_launch` から）。
- libc は RDTSCP（TSC_AUX が CPU の番号）と LFENCE で TSC を読み、カーネルと同じ式で monotonic・boot・wall を出す（`n_clock_monotonic`・`n_clock_boot`・`n_clock_wall`）。不変の TSC を前提にする。VM の判定は共通コードに無い。
- 消した：`SYS_UPTIME`・`SYS_TIME`・`SYS_CLOCK_INFO`、`struct n_clockinfo`・`N_CLOCK_USER_TSC`、libc の `uptime_ms`・`uptime_us`・`n_clock_uptime_ms`・`n_sleep_ms`・`clock_info`。
- Go の runtime も同じ頁を読む（`nanotime1`・`walltime`、RDTSCP は `sys_nocturne_amd64.s` の `nocturneTSC`）。Rust の std と QuickJS・FFmpeg・Mesa・Expat・TinyCC の Nocturne の部分は libc の時計と期限を使う。

### `n_event` の時刻
- `struct n_event` に `uint64_t time_ns`（単調時計）を足した。タイマーはその格子の上の時刻（`timer.due`）、ほかは事象をカーネルが知った時刻（pending の `at`）。`sys/events.c` が報告を作る所で入れ、核の中の別の型（局所の `struct event`）は消して `struct n_event` をそのまま写す。

### アプリの thread の級
- `SYS_THREAD_SCHEDULE`（57）：`n_thread_schedule(tid, schedule, value)`。`tid` 0 は呼んだ thread、同じアプリの thread だけ。
  - `N_SCHEDULE_FAIR`：重み 1〜`N_SCHEDULE_WEIGHT_DEFAULT`（1024）。アプリは既定より重くできない。
  - `N_SCHEDULE_DEADLINE`：周期 `N_SCHEDULE_PERIOD_MIN_NS`（125 µs）〜`N_SCHEDULE_PERIOD_MAX_NS`（1 s）。中核の `sched_deadline` そのもの。定数は中核の `SCHED_PERIOD_MIN_NS`・`MAX_NS`・`SCHED_WEIGHT_DEFAULT` と等しいことを `_Static_assert` で縛る。
- 中核の CBS を原文で確かめた結果：thread ごとの取り置きは `[P>>6, P−P>>4]` で抑えられていたが、(1) 一つのアプリが締切の thread を幾つでも作れば、合わせて CPU を取り切れた。(2) `sched_set_policy` が呼ばれるたびに取り置きを満たし直していたので、呼び直すだけで CBS を抜けられた。
- 中核へ足した最小の口：
  - `struct sched_policy` の `share`（取り置きが周期に占める上限、`SCHED_SHARE_ONE` 分の）。`dl_most` がこれで抑える。カーネルの締切の thread は `SCHED_SHARE_ONE`（今までと同じ）。
  - `dl_keeps`：CBS の起こしの規則（`now < due` かつ `left × P ≤ (due − now) × reserve`）。`dl_release` と `sched_set_policy` が使う。呼び直しは残りを満たさない。
- `thread.c`：一つのアプリの締切の thread を合わせて一つの CPU の半分（`APP_DEADLINE_SHARE`）を等分する。級を変えたときと、締切の thread が終わったときに配り直す（`app_shares`）。
- 使う所：
  - ブラウザーの主ループの thread（requestAnimationFrame の機会と、avmedia の音の出力を回す thread）。`UI_EVENT_FRAME` の `a`（表示のフレームの周期）で締切の級に置き、周期が変われば置き直す（`frame_schedule`）。
  - Web Audio の描き手（`webaudio.c` の `realtime`）。描画の量子（128 フレーム）の周期。

## 変えたファイル

`git diff --stat`（b6388a38 との差分）：

```
 common/abi.h                               |  88 ++++++++++++++++++++++++++++++++++++-----------
 docs/nocturne-native-abi.md                |  83 ++++++++++++++++++++++++++++++++-------------
 kernel/src/gui/wm.c                        |   6 ++--
 kernel/src/net/net.c                       |  24 +++++--------
 kernel/src/sys/clock.c                     |  47 ++++++++++++++-----------
 kernel/src/sys/clock.h                     |  40 ++++++----------------
 kernel/src/sys/events.c                    |  23 +++----------
 kernel/src/sys/proc.c                      |   1 +
 kernel/src/sys/sched.c                     |  43 +++++++++++++++--------
 kernel/src/sys/sched.h                     |  14 ++++++--
 kernel/src/sys/syscall.c                   |  67 ++++++++++++++----------------------
 kernel/src/sys/syscall.h                   |  11 ------
 kernel/src/sys/thread.c                    |  67 ++++++++++++++++++++++++++++++++++++
 kernel/src/sys/thread.h                    |   4 +++
 ports/go/README.md                         |   2 +-
 ports/go/nocturne.patch                    | 279 ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++-----------------------------------------------------
 ports/llvm/nocturne.patch                  |  16 ++++-----
 ports/llvm/tool.c                          |   2 +-
 ports/mesa/README.md                       |   2 +-
 ports/nvidia/README.md                     |   4 +--
 ports/nvidia/display.c                     |   6 ++--
 ports/nvidia/libnvrm.c                     |   4 +--
 ports/nvidia/nvrm.c                        |   4 +--
 ports/nvidia/nvrm_serve.c                  |   4 +--
 ports/nvidia/os.c                          |  55 +++++-------------------------
 ports/nvidia/os_acpi.c                     |   2 +-
 ports/nvidia/uvm_mm.c                      |   2 +-
 ports/nvidia/uvm_os.c                      |  22 ++++++------
 ports/nvidia/uvm_os.h                      |   8 ++---
 ports/nvidia/uvm_rm.c                      |   2 +-
 ports/nvidia/uvm_serve.c                   |   7 ++--
 ports/nvidia/uvm_shared.h                  |   2 +-
 ports/rust/README.md                       |   4 +--
 ports/rust/nocturne.patch                  |  50 +++++++++++----------------
 rootfs/Nocturne/Agent/system.md            |   2 +-
 tests/ata_pio_io_cases.c                   |   4 +--
 tests/audiotest.c                          |  50 +++++++++++++--------------
 tests/automationtest.c                     |  10 +++---
 tests/avmediatest.c                        |  18 +++++-----
 tests/boxshadowtest.c                      |  16 ++++-----
 tests/browserfreezetest.c                  |   2 +-
 tests/canvascontextprobetest.c             |   2 +-
 tests/canvasgradienttest.c                 |   2 +-
 tests/canvastest.c                         |   8 ++---
 tests/check_animation_model_native.py      |   4 +--
 tests/check_fetch_metadata.py              |   2 +-
 tests/check_media_count_native.py          |   2 +-
 tests/check_navigator_native.py            |   2 +-
 tests/check_webnet_pacing.py               |   2 +-
 tests/check_xml_parser.py                  |   6 ++--
 tests/child_module_sync_native_cases.c     |   2 +-
 tests/collections_native_cases.c           |   4 +--
 tests/css_cascade_hosttest.c               |   2 +-
 tests/css_selectors_native_cases.c         |   2 +-
 tests/cssanimationpixeltest.c              |   2 +-
 tests/cssanimationregressiontest.c         |  18 +++++-----
 tests/cssanimationtest.c                   |  28 +++++++--------
 tests/csscachetest.c                       |  22 ++++++------
 tests/cssrelationaltest.c                  |   4 +--
 tests/csssupportstest.c                    |   4 +--
 tests/currentscripttest.c                  |   6 ++--
 tests/dom_unbounded_hosttest.py            |   2 +-
 tests/entrywatchtest.c                     |  14 ++++----
 tests/fetch_keepalive_lifetime_new.c       |   2 +-
 tests/fetch_redirect_manual_hosttest.c     |   2 +-
 tests/fixedvisualtest.c                    |   6 ++--
 tests/frame_embedding_policy_hosttest.c    |   2 +-
 tests/fuzz/browser/net_stub.c              |   4 +--
 tests/gputest.c                            |   4 +--
 tests/guitest.c                            |   4 +--
 tests/hovertest.c                          |   2 +-
 tests/html_csp_transport_hosttest.c        |   2 +-
 tests/http2_queue_hosttest.c               |   4 +--
 tests/http_body_limit_hosttest.c           |   2 +-
 tests/http_dynamic_headers_hosttest.c      |   2 +-
 tests/http_header_batch_native.c           |   2 +-
 tests/http_hostname19_new.c                |   2 +-
 tests/http_method18_new.c                  |   2 +-
 tests/http_mock_lifecycle.h                |   2 +-
 tests/http_origin20_new.c                  |   2 +-
 tests/http_owned_span21_new.c              |   2 +-
 tests/httptest.c                           |   2 +-
 tests/imagebitmappromisetest.c             |   6 ++--
 tests/imagebitmaptest.c                    |  16 ++++-----
 tests/imageownershiptest.c                 |  12 +++----
 tests/imagetest.c                          |   8 ++---
 tests/js_dynamic_queues_prefix.c           |   2 +-
 tests/js_frames_native.h                   |  14 ++++----
 tests/js_importmaps_native.c               |   2 +-
 tests/jstest.c                             |  32 ++++++++---------
 tests/lineclamppredicatetest.c             |   2 +-
 tests/lineclamptest.c                      |   2 +-
 tests/media_http_cors_hosttest.c           |   2 +-
 tests/mediatest.c                          |   2 +-
 tests/memreservetest.c                     |   4 +--
 tests/metadatatest.c                       |   4 +--
 tests/mlfqtest.c                           |  28 +++++++--------
 tests/nativeabitest.c                      |  28 +++++++--------
 tests/nativechanneltest.c                  |   8 ++---
 tests/navigatortest.c                      |  28 +++++++--------
 tests/nettransporttest.c                   |  62 ++++++++++++++++-----------------
 tests/network_scheduler_hosttest.c         |   4 +--
 tests/network_url_hosttest.c               |   2 +-
 tests/nvrm_contract_hosttest.c             |   2 +-
 tests/nvrm_display_hosttest.c              |   3 +-
 tests/nvrm_servetest.c                     |  30 ++++++++--------
 tests/nvrmtest.c                           |   2 +-
 tests/observerfairnesstest.c               |   4 +--
 tests/pagertest.c                          |  10 +++---
 tests/pagevisibilitytest.c                 |  35 ++++++++++---------
 tests/paintperformancetest.c               |  22 ++++++------
 tests/paralleljstest.c                     |   4 +--
 tests/paralleltest.c                       |   4 +--
 tests/parserdeduptest.c                    |   8 ++---
 tests/pcitest.c                            |  84 ++++++++++++++++++++++-----------------------
 tests/retired_tasks_native_prefix.c        |   2 +-
 tests/runtests.c                           |  18 +++++-----
 tests/same_forest_native_cases.c           |   2 +-
 tests/sandboxprofiletest.c                 |   4 +--
 tests/scriptlifecycletest.c                |   8 ++---
 tests/semanticstest.c                      |   4 +--
 tests/shadowtest.c                         |  10 +++---
 tests/sharedpagetest.c                     |   6 ++--
 tests/sharedworker_host/nocturne.h         |   2 +-
 tests/sharedworker_server_hosttest.c       |   2 +-
 tests/smptest.c                            |   6 ++--
 tests/stdiotest.c                          |   4 +--
 tests/storage_snapshot_native_cases.c      |   4 +--
 tests/storagetest.c                        |   8 ++---
 tests/storagewindowtest.c                  |  14 ++++----
 tests/styleincrementaltest.c               |   2 +-
 tests/svggeometrytest.c                    |   2 +-
 tests/svgtest.c                            |   4 +--
 tests/systemwatchtest.c                    |   8 ++---
 tests/tcpcapacitytest.c                    |  40 +++++++++++-----------
 tests/tcptest.c                            |  14 ++++----
 tests/threadtest.c                         |  61 +++++++++++++++------------------
 tests/tls_hosttest.c                       |   4 +--
 tests/translatetest.c                      |   2 +-
 tests/transport8.c                         |  36 ++++++++++----------
 tests/transport8_host.c                    |   4 +--
 tests/typed_om_hosttest.c                  |   2 +-
 tests/unifont_guest_test.c                 |   2 +-
 tests/waitingstreamtest.c                  |   6 ++--
 tests/web_form_validation_cases.h          |   2 +-
 tests/webfetch_body_limit_hosttest.c       |   4 +--
 tests/webfetch_cache_hosttest.c            |   2 +-
 tests/webnet_cookie_store_host.c           |  12 +++----
 tests/webnet_url22_new.c                   |   2 +-
 tests/webnettest.c                         | 126 +++++++++++++++++++++++++++++++++----------------------------------
 tests/webtest.c                            |  16 ++++-----
 tests/wmframetest.c                        |  10 +++---
 tests/wmholdtest.c                         |   4 +--
 tests/wmramtest.c                          |  10 +++---
 tests/worker_host_include/nocturne.h       |   2 +-
 tests/worker_origin17_new.c                |   2 +-
 tests/wptperformancetest.c                 |  30 ++++++++--------
 tests/wptrunner.c                          |   8 ++---
 tests/wxtest.c                             |   2 +-
 third_party/expat/lib/xmlparse.c           |   4 +--
 third_party/ffmpeg/libavutil/random_seed.c |   6 ++--
 third_party/ffmpeg/libavutil/time.c        |  15 ++++----
 third_party/mesa/src/util/os_time.c        |   8 +++--
 third_party/quickjs/quickjs.c              |  28 ++++++---------
 third_party/tinycc/tcc.c                   |   2 +-
 user/apps/agent.c                          |  18 +++++-----
 user/apps/browser.c                        |  79 +++++++++++++++++++++++-------------------
 user/apps/browser_devtools.h               |  10 +++---
 user/apps/browser_settings.h               |   2 +-
 user/apps/browserjsworker.c                |  42 +++++++++++------------
 user/apps/browsermediaworker.c             |   2 +-
 user/apps/clock.c                          |   6 ++--
 user/apps/cppdemo.cpp                      |   4 +--
 user/apps/cube.c                           |   4 +--
 user/apps/fetch.c                          |   4 +--
 user/apps/files.c                          |   2 +-
 user/apps/fortune.c                        |   2 +-
 user/apps/gpu_demo.c                       |   4 +--
 user/apps/host.c                           |   2 +-
 user/apps/init.c                           |   8 ++---
 user/apps/js.c                             |  16 ++++-----
 user/apps/life.c                           |   8 ++---
 user/apps/mandel.c                         |   6 ++--
 user/apps/mediaplayer.c                    |   6 ++--
 user/apps/mines.c                          |  10 +++---
 user/apps/neofetch.c                       |   2 +-
 user/apps/paint.c                          |   2 +-
 user/apps/piano.c                          |   4 +--
 user/apps/ping.c                           |  23 +++++++------
 user/apps/player.c                         |   6 ++--
 user/apps/poweroff.c                       |   2 +-
 user/apps/ps.c                             |   2 +-
 user/apps/reboot.c                         |   2 +-
 user/apps/screenshot.c                     |   2 +-
 user/apps/sh.c                             |   4 +--
 user/apps/sleep.c                          |   3 +-
 user/apps/snake.c                          |  10 +++---
 user/apps/syscallfuzz.c                    |  16 ++++-----
 user/apps/sysmon.c                         |  26 +++++++-------
 user/apps/term.c                           |  12 +++----
 user/apps/tetris.c                         |  12 +++----
 user/apps/uihost.c                         |   3 +-
 user/apps/uptime.c                         |   2 +-
 user/apps/webfetch.c                       |  28 +++++++--------
 user/include/http.h                        |   6 ++--
 user/include/js_worker_wire.h              |   2 +-
 user/include/nocturne.h                    |  71 ++++++++++++++++++++------------------
 user/include/web.h                         |   4 +--
 user/libc/cxxabi.cpp                       |   8 ++---
 user/libc/font.c                           |   2 +-
 user/libc/gui.c                            |  10 +++---
 user/libc/http.c                           |  31 ++++++++---------
 user/libc/http2.c                          |  16 ++++-----
 user/libc/malloc.c                         |   2 +-
 user/libc/media.c                          |  12 +++----
 user/libc/media_mse_worker.c               |  10 +++---
 user/libc/media_worker.c                   |  14 ++++----
 user/libc/net.c                            |  28 +++++++--------
 user/libc/parallel.c                       |  14 ++++----
 user/libc/stdio.c                          |   2 +-
 user/libc/sys.c                            | 146 ++++++++++++++++++++++++++++++++++++++++++------------------------------------
 user/libc/third_party_img.c                |   2 +-
 user/libc/threads.c                        |  40 ++++++++++------------
 user/libc/time.c                           |  31 ++++++-----------
 user/libc/tls.c                            |  71 +++++++++++++++++++-------------------
 user/libc/ui.c                             |  10 +++---
 user/libc/web/animation.c                  |   2 +-
 user/libc/web/animation.h                  |   2 +-
 user/libc/web/avmedia.c                    |  52 ++++++++++++++--------------
 user/libc/web/css.c                        |   8 ++---
 user/libc/web/doc.c                        |  34 +++++++++----------
 user/libc/web/dom.c                        |   4 +--
 user/libc/web/form_file_picker.c           |   2 +-
 user/libc/web/form_validation.c            |   4 +--
 user/libc/web/form_value.c                 |   2 +-
 user/libc/web/frame_native.h               |   6 ++--
 user/libc/web/html_lexbor.c                |   4 +--
 user/libc/web/js.c                         | 156 +++++++++++++++++++++++++++++++++++++++++------------------------------------------
 user/libc/web/js_activation_native.h       |  10 +++---
 user/libc/web/js_atomics.c                 |  24 ++++---------
 user/libc/web/js_compile.c                 |   8 ++---
 user/libc/web/js_inner_text.h              |   4 +--
 user/libc/web/js_profile.c                 |   4 +--
 user/libc/web/js_wasm.cpp                  |  13 ++++---
 user/libc/web/js_webaudio.c                |   8 ++---
 user/libc/web/js_webgl.c                   |   2 +-
 user/libc/web/js_webidl.c                  |   4 +--
 user/libc/web/layout.c                     |   4 +--
 user/libc/web/marquee.c                    |   2 +-
 user/libc/web/paint.c                      |  16 ++++-----
 user/libc/web/paint_media_debug.h          |   2 +-
 user/libc/web/web_presentation.c           |   2 +-
 user/libc/web/webaudio.c                   |   6 ++--
 user/libc/web/webaudio.h                   |   2 +-
 user/libc/web/webi.h                       |   6 ++--
 user/libc/web/xml.c                        |   6 ++--
 user/libc/webidb.c                         |   4 +--
 user/libc/webnet.c                         |  36 ++++++++++----------
 user/libc/websitedata.c                    |   4 +--
 user/libc/webstorage.c                     |  10 +++---
 user/libc/webstorage_shared.h              |   6 ++--
 261 files changed, 1935 insertions(+), 1791 deletions(-)
```

## 消したもの

- syscall：`SYS_UPTIME`・`SYS_TIME`・`SYS_CLOCK_INFO`。番号 20〜22 と 44 は空きになった。`SYS_THREAD_CREATE` は `= 45` と書いて番号を保つ。
- 型と定数：`struct n_clockinfo`、`N_CLOCK_USER_TSC`。`n_procinfo` の `cpu_ms`・`start_ms` と `n_sysinfo` の `uptime_ms` は ns の欄に置き換わった。
- libc の口：`uptime_ms`・`uptime_us`・`n_clock_uptime_ms`・`n_sleep_ms`・`clock_info`。秒を返す `n_clock_wall` は ns を返す形に替わった。
- ms を取る口の古い形：`n_app_result`、`n_pci_interrupt_take`・`n_pci_screen_state`・`n_pci_acpi_notify_take`、`n_pager_take`、`n_events_timer`（ms と repeat）、`n_events_wait`、`wait_on_address`・`wait_on_address64`、`net_ping`・`udp_recvfrom`・`tcp_connect`・`tcp_send_until`（ms の期限）・`tcp_recv`・`tcp_accept`・`net_wait_up`、`win_event`。どれも期限（ns）を取る形だけが残る。
- カーネル：
  - `syscall.h` の `syscall_deadline`、`syscall.c` の `address_deadline`（ms を ns の期限へ換える層）。
  - `net.c` の `PING_TIMEOUT_DEFAULT`・`TCP_CONNECT_DEFAULT`（決め打ちの既定）。
  - `events.c` の局所の `struct event`。
  - `clock.c` の `struct clock_data`・`clock_data_pages` と、その上の読み手（`snapshot`・`consistent`・`wall_at`）。`struct n_clock` が置き換えた。
  - `sched.c` の固定の `dl_most`。`sched_policy.share` で抑える形に替わった。
- 移植の接着：
  - nvidia の `os.c` の自前の TSC の読み（`clock_once`・`clock_base`・`read_tsc`）。
  - Go の `_SYS_UPTIME`・`_SYS_TIME`・`_SYS_CLOCK_INFO` と `syscall.Uptime()`、osinit での時計の準備。
  - `cxxabi.cpp` の手書きの宣言。`nocturne.h` を include する形に替わった。

## 組みの結果

- `python -X utf8 scripts/build.py -j 8 kernel`：120 個を組み直して、警告は 0。記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\timeabi\build-kernel.log`。
- `python -X utf8 scripts/build.py -j 8 RUST_SYSROOT=none qemu-image`：rc 0。出力は `D:\Programes\llm vibe slop os\build\wt\agent-a0f0ad79836d456f0\nocturne.img`（704 MiB。C: は 61 folders、584 documents）。
  - `RUST_SYSROOT=none` は依頼文が許す形で付けた。Rust のアプリが std の `setenv`・`unsetenv` で link に失敗するからで、この件は別の書き手が直す。
- 変えた C・C++ のソースの object を消し、明示の目標として組み直した（90 個）。警告は 8 行あり、どれも `-Wmissing-field-initializers`：
  - `user/libc/font.c:305`・`309`
  - `user/libc/web/css.c:8544`
  - `user/libc/web/layout.c:2374`・`5011`

  どれも私が変えていない行で、b6388a38 にある。記録は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\timeabi\build-objs.log`。
- 作業木で組むには、次の三つが要った：
  - 作業木の `build` を main の `build\wt\agent-a0f0ad79836d456f0` への junction にした。`generate.py` と `build_cxx.py` が BUILD でなく ROOT/build へ書くからである。
  - `third_party\tinycc\conftest.c`、`tools\limine`、`licenses\rust` を main から写した。どれも git の外にある。
  - 作業木の patch から作った Rust の std を、作業木の `tools\rust\sysroot` に置いた。NVK のために要った。

  どれも git の外にあり、コミットに入らない。
- Go の patch は gofmt で形だけを確かめた。組んではいない。

## QEMU で見たもの

窓付きの QEMU を一台起こした（q35、2560×1440、HMP 45464・QMP 45465、媒体は上の `nocturne.img`）。終わりに HMP の `quit` で止めた。画像はすべて `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\timeabi\` にある。

- `...\timeabi\desk.png`：デスクトップが出た。タスクバーの時計は「10:57 Sun 1 Nov」。
- `...\timeabi\term.png`：Terminal の `uptime` が「up 0 day(s), 00:00:58, 18 tasks」と出した。`ps` は CPU(ms) の列を出した（`cpu_ns` を ms にして表示）。
- `...\timeabi\term2.png`・`term2c.png`：
  - `date` は「Sunday 01 November 2026, 10:58:14」。
  - `uptime` は 00:01:18 で、`sleep 3` の後に 00:01:22。
  - `ping -c 3 10.0.2.2` は time=0.292・0.168・0.909 ms で、「rtt min/avg/max 168/456/909 us」。
- `...\timeabi\wiki1.png`：ブラウザーで https://en.wikipedia.org/wiki/Clock を開き、頁が描かれた。
- `...\timeabi\f12.png`・`js1.png`・`clock.png`：F12 の Console から、setTimeout の 250 ms と、requestAnimationFrame の 60 回を測った。結果は「timer250 250.5」「raf60 978.6」で、60 回で 978.6 ms は 1 回あたり 16.3 ms。タスクバーの時計は 11:00 に進んでいた。
- `...\timeabi\now.png`：11:02 の画面。時計の表示は進み続けた。
- `...\timeabi\notests.png`：製品の媒体には `C:\Tests` が無い。Terminal で `tcc -o .\nativeabitest C:\Tests\nativeabitest.c` を打つと「file 'C:\Tests\nativeabitest.c' not found」になった。

serial.log（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\timeabi\serial.log`）の行：

```
[   17.157971 0:1] store: C: "Nocturne" on nvme0, 638 MiB, 223 MiB free
[   17.227753 0:1] wm: compositor started at 2560x1440
[  186.820887 3:27] [browser input @186.820] let a=performance.now();setTimeout(()=>console.log('timer250 '+(performance.now()-a).toFixed(1)),250);let n=0,f=t=>{if(++n<60)requestAnimationFrame(f);else console.log('raf60 '+(t-a).toFixed(1))};requestAnimationFrame(f)
[  187.072042 0:27] [browser log @187.072] timer250 250.5
[  187.800178 0:27] [browser log @187.800] raf60 978.6
clock: the RTC's second found within 4639 us; the wall clock on by 299931 us
```

## 残る欠陥と未確認

- ゲストの試験は回していない。`tests/runtests.c` の `nativeabi/documents-channels-apps-events` と `threads/native-shared-threads` が時間の試験に当たる。
  - `nativeabitest` は ns の timer と `n_event.time_ns` を試す。`threadtest` は時計の三つの順、時計の頁の書込みの拒否、`tsc_hz` を試す。
  - `scripts/test.py` はフックが止めた。理由は「媒体は 2 つまで」で、`build` に `mae.img` と `nocturne.img` が既にあり、試験の写しが三つ目になる。
  - 製品の媒体には `C:\Tests` が無く、手でも回せなかった。
- 次の道具は古い ABI で組まれている：
  - 組み済みの `tools/llvm` の runtime（libc++）
  - ゲストの clang
  - `tools/go`
  - `tools/rust/sysroot`

  main へ載せた後に、今の patch から組み直す必要がある。`tools/` は共有なので、ここでは組み直していない。
- 締切の級の受入は、アプリごとに一つの CPU の半分まで。アプリをまたいだ受入の合計は見ていないので、締切の thread を持つアプリが幾つも集まると、一つの CPU に対して取り置きが過ぎる。
- ブラウザーの中の期限は、今も単調時計の ms の double で持つ。ns にしたのは ABI の境目だけである。
- `.claude/oboegaki.md` と `.claude/rules/browser.md` に「uptime_ms」の記述が残っている。規則と覚え書きはユーザーの承認が要るので、直していない。
- `ports/llvm/README.md` には、日付入りの経緯の文が元から残っている。
- ブラウザーの主ループの thread が実際に締切の級に入ったかは、ログで見ていない。rAF の 60 回が 978.6 ms で回ったことだけを見た。
- 性能（fps、合成の時間）は測っていない。前の記録も無い。
- 作業木での組みの穴は四つある：
  - `generate.py`・`build_cxx.py` が BUILD を無視する。
  - `conftest.c` が作業木に無い。
  - Rust の sysroot が作業木に無い。
  - limine と licenses が作業木に無い。
- 実機と Hyper-V では確かめていない。
