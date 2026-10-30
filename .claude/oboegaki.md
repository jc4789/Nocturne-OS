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
対象: common/store/catalog.c, common/store_format.h, host/store/store.c
確かめた: 2026-10-26 304be1a1 の次（併合の直し）
枝の item 0 の鍵は、最初の子が外れた後や、それより小さい鍵が最初の子に入った後には、その子の鍵より上にも、二番目の鍵より上にもなる。枝を探す・調べる処理（child_index、walk）は item 0 の鍵を比べない。枝の鍵を使う処理を足すときも item 1 から見る。item 0 が二番目以降へ移る所（catalog_delete の枝の併合）では、その鍵の代わりに親の区切りの鍵を使う（右の枝の item 0 をそのまま写すと、その子の鍵を見失う。range の手元の試験で再現した）。目録を変えたら `store tree-test <seed> 200000` を幾つかの種で通す（`store test` は木を数枚の葉に保つので、枝の崩れを出さない。tree-test の四つ目の手番が範囲で消して枝を併合させる）。

## 対称のカーネル：ファイルをまたぐ約束
対象: kernel/src/sys/syscall.c, kernel/src/sys/sched.c, kernel/src/arch/idt.c, kernel/src/arch/isr.asm, kernel/src/mm/vmm.c
確かめた: 2026-10-29（BSP へ回す道を消した版、未コミット）
copy_from_user／copy_to_user は isr.asm の user_copy（rep movsb）で写し、頁違反は idt.c の user_copy_fixup が拾う：割り込みが開いていれば vmm_handle_user_fault で頁を作って続け（眠りうる）、閉じていれば user_copy_fault へ飛んで残りのバイト数を返す。だから spinlock の下で写すと、頁が無いときは必ず失敗し、あるときも眠りうる。利用者の指を写し以外で読むと、別の CPU の兄弟 thread が外した頁で核が panic する（その頁違反は直されない）。task_exit は空間を kernel_pml4 へ付け替えるのを task_lock の下で行い、その後に vmm_free_space する：task_lock の下で読んだ t->pml4 の表は lock の間は消えない（proc_user_pages・vmm_user_pages_split を task_lock の下で呼べる理由）。番地の待ち（task_wait_address）は利用者の番地を物理へ訳して直写像で読むので、写しを使わない。panic は panic_enter（printf.c）で他の CPU を NMI で止めてから、止まった CPU が持っていた記録の lock を spin_seize する。

## 外部の ArrayBuffer の free_func は二度呼ばれる
対象: third_party/quickjs/quickjs.c, user/libc/web/js_webaudio.c, user/libc/web/*.c
確かめた: 2026-10-27 215d0d6c
JS_NewArrayBuffer で作った外部の buffer を JS_DetachArrayBuffer で切り離すと、その場で free_func(rt, opaque, data) が呼ばれ、後で ArrayBuffer の finalizer がもう一度 free_func を呼ぶ（data は NULL）。opaque に参照を持たせる free_func は、data が NULL の時に何もしない。2026-10-27、AudioBuffer の channel の free_func が二度参照を落とし、acquire-the-content の試験で解放後の読み出しになった。

## AUTOINIT の大域は閉包を作る時に実体になる
対象: third_party/quickjs/quickjs.c, user/libc/web/js.c, user/libc/web/js_webaudio.c
確かめた: 2026-10-27 215d0d6c
JS_DefineLazyProperty（Nocturne の追加）で定義した大域の名前は、その名前を参照する関数の閉包を作る時（js_closure_global_var）に初めの関数が呼ばれ、実体になる。名前を書いた関数を作るだけで Web Audio の program が走るので、「触れた時に読む」は「その名前を含む関数が作られた時に読む」の意味。初めの関数（JS_AutoInitProperty）は、その property 自身を書き換えてはいけない。

## wasm2c の CWriter::Write(char) は数を書く
対象: third_party/wabt/src/c-writer.cc
確かめた: 2026-10-27 215d0d6c
CWriter の Write には char の重ね書きが無く、Write('.') や Write(MangleType(t)) は Write(uint64_t) へ落ちて「46」のような数を書く。一文字は std::string(1, c) にして渡す。2026-10-27、多値の結果の member 名（.i0）を組む所で起き、生成した C が「cannot convert 'struct wasm_multi_ii'」で TCC に通らず、231 KB の module が解釈器へ落ちていた（再現は Bilibili の動画の頁）。

## 生成の箱は元の要素を node に持ち、style で見分ける
対象: user/libc/web/box.c, user/libc/web/paint.c, user/libc/web/layout.c
確かめた: 2026-10-27 215d0d6c
::before・::after の箱（pseudo_box）は node に元の要素、st にその pseudo の style を持つ。中の text の run も node は元の要素で、st は pseudo の style。箱・run・deco が要素自身のものか生成のものかは、st が node->style->before／after と同じかで分かる（paint.c の hit_pseudo）。restyle は replacement_style で before／after も新しい style へ付け替えるので、この等しさは保たれる。node だけで重複を除くと、要素と、その ::before が一つにまとまる。

## 箱の st が借りる装いは、release の前に付け替えるか作り直す
対象: user/libc/web/css.c, user/libc/web/box.c, user/libc/web/doc.c, user/libc/web/dom.c
確かめた: 2026-10-27 501b0ffd
箱・run・deco の st は節の装いの arena を借りる。cascade の retire_style が古い装いを style_previous へ移し、doc.c の公開が boxes_restyle（restyle_boxes が replacement_style で新しい装いへ付け替える）と局所の作り直しの後に css_styles_release で古い arena を放す。restyle_boxes は style_sync_dirty の道だけを降りるので、借りる箱に印が届かないと解放後の読み出しになる。
- 節を外す（dom.c の doc_node_remove）と css_node_style_unpublish が style を NULL にし、arena は残す。同じ親へ入れ直す（並べ替え）と、cascade は style_previous の無いまま新しい装いを作り、古い arena は retire で放される。箱は local_splice が「box_regen でなければ残す」ので、box_regen を立てないと古い st のまま残る。2026-10-27、これでスクロール中に css_transform_general が落ちた（再現は Bilibili の頁の、shadow DOM の中の並べ替え）。
- display:contents の要素は箱を持たず、その文字・::before・::after の箱が装いを借りる。modal の dialog の ::backdrop の箱は root の子で anon。どちらも mark_style_borrowers が印を付ける。
- 確かめ方：release の直前に全部の箱で replacement_style(box_style_node(b),b->st)!=b->st を探すと付け替え漏れが、st!=node->style（node->box==b）を探すと作り直し漏れが見える。

## guest の試験は作業木のヘッダーで組まれる
対象: scripts/test.py, tests/runtests.c, user/libc/web/webi.h
確かめた: 2026-10-28 cd5be8b6
test.py の test_files は、始まった時点の作業木の tests/*.c と webi.h・cssom.h・quickjs.h などを C:\Tests へ写し、guest の tcc がそれで試験を組む。媒体の libc はビルドした時点のヘッダーで組まれている。だから媒体を作った後に webi.h（style_t・struct cx・web_doc などの形）を変えて test.py を始めると、試験と libc の構造体の形が食い違い、webi.h を含む試験（csscachetest・lexbortest・html5lib_tree_test・paintperformancetest など）が番地 0 や stack の壊れで落ちる。2026-10-28 の 1028i の回で起きた（媒体は 11:09 に組み終え、11:08 の取り込みが webi.h を変え、試験は 11:12 に始まった）。媒体を作った後に作業を続けるなら、媒体のコミットの写し（`git archive <commit> tests user scripts third_party/quickjs/quickjs.h` を build の下へ展開）を `--source-tree` で渡す。test.py が照合する sha256 は kernel と programs だけで、ヘッダーの食い違いは見ない。

## S3 の復帰は QEMU（WHPX）では試せない：休止の往復で確かめる
対象: kernel/src/arch/wake.c, kernel/src/arch/wake.asm, kernel/src/arch/smp.c, kernel/src/dev/pm.c, kernel/src/sys/clock.c
確かめた: 2026-10-30（段①の 1b-2）
QEMU を -accel whpx で起こして眠り（S3）を選ぶと、入るまでは通る（pm の thread が CPU 0 で AP を止め、VM が suspend する）が、起こすと QEMU が「WHPX: Unexpected VP exit code 4」を出して F000:FFF0 で止まり、我々のコードまで来ない。復帰の道（wake の頁、smp_resume の表での AP の一斉の再起動、clock_resume、装置の再開）は休止の往復で通る：scratchpad に build/nocturne.img を下にした qcow2 を作り（qemu-img create -f qcow2 -b … -F raw）、snapshot なしで起こし、Nocturne の menu の Hibernate で休止し、同じ qcow2 で起こし直す。serial に「hibernate: restoring …」「smp: N/N CPUs online after sleep」「hibernate: back, the image is removed」が出る。媒体を組み直したら qcow2 は作り直す（下の媒体が変わると壊れる）。休止で VM を起こし直すと hypervisor の基準時刻は 0 から始まり、休止の前の時計より小さい（clock.c の hv_base はそのため 2^64 を法とする）。

## QEMU が媒体を開いている間に組むと、媒体が古いまま残ることがある
対象: scripts/mkimage.sh, scripts/build.py
確かめた: 2026-10-30
2026-10-30、panic で止まった QEMU（build/nocturne.img を snapshot=on で開いたまま）を残して qemu-image を組むと、出力に IMG の行は出たが、媒体の時刻は kernel.elf より古いままで、次の起動は古い kernel だった（HMP で task の欄が無いのを見て気づいた）。どこで書き損じたか（mkimage.sh の rm -f か store.exe disk か）は未確認。組む前に QEMU を quit し、組んだ後に build/kernel.elf と build/nocturne.img の時刻を並べる。
