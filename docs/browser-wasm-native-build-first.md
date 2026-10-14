# wasm2c・TinyCC・JSPI 実行基盤 第一報

## 担当と実装

- 担当は `wasm_build`。ブラウザー本体・JSPI の JS/QuickJS 接続と実画面受入は別担当が所有する。既存インタープリターを消していない。
- 指定された外部 WABT checkout は変更せず、`scripts/build_cxx.py:17,132` で revision `1e9fffd80efa901b3e4708d4ba1f0df972e63518` を固定した必要 source を取り込む。WABT は Apache-2.0。既存 TinyCC の該当 source は LGPL-2.0-or-later で、GPLv2-only や他 OS の source を追加していない。
- WABT 基盤 18 source と wasm2c 翻訳器 21 source を既存 C++20/libc++ 構築へ統合。6 個の template string は上流の CMake generator を実行して生成する。生成済み文字列の手編集は行わない。
- `Makefile:238–250` の grouped target は archive と個別 object を同じ producer に結び、欠損 object を再生成する。既存の mtime/存在確認を使い、内容 hash cache と誤記しない。
- `Makefile:215,260,303` で browser/worker が必要時に抽出する libtcc・翻訳器・C runtime と、ゲスト `/bin/wasm2c` を統合。C アプリ/TCC 自身へ全 C++ runtime を無条件にリンクしない。
- `scripts/mksysroot.sh` は runtime headers、C source、`libwasm-rt.a`、ライセンスを配置する。JIT 用 host symbol binder は standalone runtime archive に混ぜず、ブラウザー既存の libc/FILE/runtime を共有する。

## 中断・資源・stack

- `third_party/tinycc/libtcc.c:154,336` の C-only guard が setup/parse/relocate の実 OOM を `-2` で戻す。C++ frame を longjmp で飛ばさず、callback は通常 return 後に C 内部の error recovery へ進む。
- lexer の処理量を既存 `IO_BUF_SIZE` 単位にまとめて中断を照会する。単一巨大関数を容量上限で拒否する代用はしない。開始時取消は parser 未初期化の cleanup に入らず通常 return する。
- `x86_64-gen.c:628` 以降の明示 Wasm profile が、実 frame・引数・動的 alloca・変換 scratch・call return address の stack write 前を検査する。一般 TCC/通常 ELF/CLI は profile 無効のまま。
- `ports/wabt/wasm_rt_nocturne.c:148` の独自 native entry が R15 descriptor/R12 scratch を保存復元。失敗時は安全な anchor に RSP を戻し trap へ jump し、下端で検査関数を call して先に fault する経路を避ける。
- native poll は既存 interpreter batch を根拠に 1000 checkpoint ごとへまとめ、設定時と run entry は必ず照会する。これは受理上限でない。時計の fast path もあるため、実測せず syscall 削減数や高速化量は主張しない。

## JSPI 用 coroutine

- `wasm_rt_coroutine_create/resume/yield/bounds/destroy` は同じ native thread で fresh stack を開始し、保存済み C++ frame をコピー・破棄せず保持する。
- stack の usable 量は実 `thread_stack_bounds` の span。Nocturne `SYS_MMAP` と `N_PAGE_SIZE` による下端 unmap、および既存 trailing guard を使い、実 OOM/errno を返す。任意の coroutine 件数・深さ・byte 上限を追加していない。
- callee-saved registers、RSP/RIP、MXCSR/x87 制御と、TLS slot 3 の trap target/poll/work/実 stack bounds を main/co 間で交換する。JS/QuickJS snapshot と JS roots は native/主担当の責務。
- suspended な live frame の destroy は `EBUSY`。取消は resume → 通常 error/unwind → DONE の後に解放する。

## 検証済みの範囲

- 最新全体構築は `build/wasm-full-build-jspi-first.log`、主担当実行で終了 0。
- 自担当の限定 cross-build/link は `build/wasm2c-tcc-unwind-build.log`、`build/wasm2c-stack-guard-build.log`、`build/wasm-jspi-coroutine-build.log`、`build/wasm-jspi-coroutine-test-build.log` で成功。
- 最新ゲスト `build/nocturne-platform/wasm-jspi-puter-first-20261014/serial.log:76–111` を独立に読取り、`tccguardtest: 29 passed, 0 failed` を確認。実確保 170 回中 1/75/76/122/170 地点の OOM、開始/関数内部取消後の正常再実行、実予約量の単 frame・再帰 guard 後の回復、co/TLS/R15/FP/入れ子/取消完了と解放を含む。
- 同 log:112–128 に GC 256 反復、native modules 258/calls 259/fallback 0、JSPI native call 増加、fallback reference と retirement 境界の成功がある。これは実サイト受入と区別する。
- 実 Puter ページは初期に 54394 byte loader module が native modules 1/native calls 5/fallback 0。主担当の最新 serial 再読では、Launch の実クリック後に同じ loader が再構築され、最終累積は 2 modules/108788 bytes、native calls 10/fallback 0、JSPI suspensions/resumptions 0。Retry はクリックしていない。これは Firefox 本体起動や実ページでの JSPI 使用の証拠にはしない。
- `build/wasm-final-artifacts-jspi.log` で initrd 内 16 payload、IMG 内 kernel/initrd、IMG↔VHD/VHDX の論理内容、ISO の kernel/initrd extent が現在成果物と一致。四媒体 hash と関連 157 source/kernel ABI 4 source の更新時刻を記録した。媒体コピー・再作成は行っていない。

## 限界

固定 payload 256 byte の例外 runtime は製品経路で構築/同梱しておらず、native EH は具体的未対応理由で interpreter fallback。SIMD/共有 memory/Module clone/全 tail-call lowering 等の全面適合、全 OOM 地点、任意の未 instrumented C/C++ host call の stack exhaustion、性能無回帰、四サイト全操作、Hyper-V 実機成功をこの報告から推論しない。
