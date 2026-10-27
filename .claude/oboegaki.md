# コードの覚え書き

コードを読んでも分からない、次にこのコードを触る人が知らないと困ることを、ファイルに結び付けて置く。
対象のファイルを読む・書くと、mokei のフック（.claude/hooks/mokei.py）がその覚え書きを出し、
確かめた後に対象へ入ったコミットの数を添える。違っていたら、見つけた人がその場で直すか消す。
書き方は .claude/skills/mokei/SKILL.md の「コードの覚え書き」。

## 締切は絶対の uptime_ms か -1
対象: user/libc/web/doc.c, user/libc/web/css.c, user/libc/web/marquee.c, user/libc/web/js.c, user/libc/web/avmedia.c, user/libc/web/frame.c, user/apps/browser.c
確かめた: 2026-10-26 67bc115e
文書の締切の源（web_js_deadline・web_avmedia_deadline・css_motion_deadline・web_marquee_deadline・web_frames_deadline・web_media_background_deadline）は、どれも「-1＝締切なし、それ以外は絶対の uptime_ms()」を返す。web_deadline（doc.c）がその最小を取り、browser.c の主ループはそこまで眠る。約束が書いてあるのは宣言の側（web.h の web_deadline、webi.h の css_motion_deadline・web_marquee_deadline）で、実装のファイルからは見えない。相対の ms を返すと待ちが常に 0 になり、主スレッドが空回りする（2026-10-25、css_motion_deadline と web_marquee_deadline で起きて直した）。新しい締切の源を足すときも同じ単位で返す。

## README の道具の記述に LLVM が無い
対象: README.md, README_EN.md
確かめた: 2026-10-26 67bc115e
README の「開発」の道具の行と ports の一覧は tcc だけを挙げ、LLVM が無い。今は ports/llvm（LLVM 23.1.3 の clang・clang++・ld.lld と libc++。作り方は ports/llvm/README.md）があり、tools/llvm/nocturne があればディスクの C: に入る。tests/runtests.c にも clang の群（clang -O2、clang++ -std=c++23 -O2）がある。ゲストで試験や計測の道具を組むとき、tcc だけを前提にしない。

## 描画の中断点は窓の画布に描くことがある
対象: user/libc/web/paint.c, user/apps/browser.c, user/libc/web/js.c
確かめた: 2026-10-26 95bb56b1
web_native_checkpoint は host_script_checkpoint（browser.c）を呼び、F12 などをその場で処理して draw_console() で窓の画布 view に描く。描画の途中で呼ばれるので、parallel_for_stage の service に checkpoint を渡してよいのは、仕事の帯が私的な緩衝だけに書く段（影のぼかし、変形の面の被覆）だけ。窓の画布に書く段（raster_draw の帯、変形の面の再標本）は service なしで合流させる。2026-10-26、再標本の段に service を渡したら、Reddit で F12 を押した直後に browser が gfx_fill で落ちた。

## 動画の継ぎ当ての枚数は二つある
対象: user/libc/web/avmedia.c
確かめた: 2026-10-26 95bb56b1
snapshot.delivered は写し（snapshot）の中にあり、snapshot_clear で 0 に戻る。描き直しを頼んだ枚は写しの外の damaged_frames が持つ。写しが無いときの doc_paint_damage は damaged_frames と presented_frames を比べて一度だけ出す。delivered で比べると、止まった動画・画面外の動画が毎拍損傷を出し、静かな頁が 60 Hz で部分描画を続けた（2026-10-26、Reddit で再現）。

## 子の枠の描画の通知は枠の箱へ
対象: user/libc/web/frame.c, user/libc/web/paint.c
確かめた: 2026-10-26 95bb56b1
子の文書の paint_dirty は web_frames_tick で親へ上がる。親では枠の要素の箱（B_ATOMIC の AT_PLACEHOLDER）だけを doc_paint_damage にし、箱が無い・visibility が hidden・大きさ 0 の枠は何もしない（frame_box_shown は面の用意と同じ判定）。web_frames_prepare_paint は根の描画の切り抜き（文書の座標）を受け、上の段の枠はそこだけを描く。入れ子の枠は親の枠の切り抜きを継ぐ。変形の下の枠には切り抜きを当てない（文書の座標と合わない）。

## 枠の realm は一つの JS の実行時を共有する
対象: user/libc/web/js.c, third_party/quickjs/quickjs.c
確かめた: 2026-10-26 51aba1a4
子の枠の文書も、上の文書の JSRuntime を使う（context だけが別）。JS_SetModuleLoaderFunc・JS_SetModuleImportFunc の opaque は上の文書の状態なので、loader・normalize・import の hook の中では opaque でなく state(ctx) で realm の状態を取る。QuickJS の census（--debug-js の JavaScript heap census）の「binary_object」は読み込んだ bytecode の累積で、ArrayBuffer の大きさではない。

## モジュールはグラフをそろえてから link する
対象: user/libc/web/js.c
確かめた: 2026-10-26 51aba1a4
module script（外部・inline）と import() は、module_graph_settled がグラフの全部を取得し終えるまで待ち、そろってから load_module で link する（HTML の fetch the descendants）。load_module の host.sync_request の取得は、グラフの外から来た module のための最後の道で、主スレッドがネットワークを待つ。--debug-js では「Module loaded synchronously, outside a fetched graph」と出る。新しく module を読む経路を足すときも、先に request_module と module_graph_settled を通す。失敗した module は親の children が users を持つので、release_module_user で消えない。

## CSSOM の規則の text は終端されていない
対象: user/libc/web/css.c, user/libc/web/cssom.c, user/libc/web/js_cssom.h, tests/styleincrementaltest.c
確かめた: 2026-10-26 51aba1a4
css_rule_info の text はシートの元の文（注釈を除いた写し）の中を指し、text_len バイトで終わる。NUL で終わらないので strstr・strcmp・strdup を使わず、text_len と memcmp・strndup・JS_NewStringLen を使う。@import の URL と media は import（struct css_rule_import）にあり、@import でない規則では NULL。

## 目録の枝の最初の鍵は何も区切らない
対象: kernel/src/fs/catalog.c, common/store_format.h, host/store/store.c
確かめた: 2026-10-26 304be1a1
枝の item 0 の鍵は、最初の子が外れた後や、それより小さい鍵が最初の子に入った後には、その子の鍵より上にも、二番目の鍵より上にもなる。枝を探す・調べる処理（child_index、walk）は item 0 の鍵を比べない。枝の鍵を使う処理を足すときも item 1 から見る。目録を変えたら `store tree-test <seed> 200000` を幾つかの種で通す（`store test` は木を数枚の葉に保つので、枝の崩れを出さない）。

## 呼出しは BSP が受け持つ。長い核の処理は全部の app を止める
対象: kernel/src/sys/sched.c, kernel/src/arch/idt.c, kernel/src/sys/syscall.c
確かめた: 2026-10-26 304be1a1
user の thread は AP で走るが、AP で片付かない呼出しは BSP へ回り（sched_ap_interrupt → isr_dispatch_remote → syscall_dispatch）、BSP が一つずつ行う。ファイルの読み込み（大きなプログラムの起動）や核の中の回り続けは、その間の他の app の呼出しを全部待たせる。ゲストが止まって見えたら、QEMU の HMP で `info registers -a` を取り、CPU0 の RIP と RBP の鎖（x /2gx）を build/kernel.elf で llvm-symbolizer に引く。
