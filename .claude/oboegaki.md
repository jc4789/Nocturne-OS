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
