# wasm2c・TinyCC・JSPI 実行基盤 第二考察

第一報の肯定結果とは別に、次の反例を source と実ゲストで照合した。

## 1. object だけ消えた場合

archive が存在しても個別 object の `@:` rule は欠損を修理しない。grouped target へ変更し、GNU Make 4.4.1 で対応を確認した。前段の `build/wasm-missing-object-rebuild.log` では、単一 `utf8.cc.o` を build 内へ退避し、object/archive を並列要求して一回だけ再生成、同 hash、退避分回収を確認した。最新全体構築も同じ producer 構造を使用する。

## 2. 開始時取消・OOM で二回 cleanup する場合

最初の中断実装は parser 初期化前の setjmp 内で callback を呼び、前回の `tok_ident` と解放済み scratch を再び cleanup し得た。これは自担当が見つけて修理した実回帰である。

開始時取消を parser に入らない通常 return にし、部分初期化可能な token/CString/allocator cleanup を reset/NULL-safe にした。最新ゲストでは、正常完了→開始取消→正常再実行、実 setup/parser/relocate の代表 OOM→allocator 復元→正常再実行がすべて成功。代表 5 地点は「全確保失敗経路を証明した」と読み替えない。

## 3. check 関数へ入る前に stack が壊れる場合

C 関数冒頭で marker を比較するだけでは、大きい `sub rsp` と次の call の return-address write が検査より先に fault し得る。固定 500 深さや余裕 byte を足す修理は採用しなかった。

TinyCC の実 frame が確定する箇所で、減算前 inline guard の即値を patch する。引数/alloca 等も実操作量を検査する。失敗後に exhausted stack 上で C を call せず、guard descriptor の安全 anchor へ復帰する。最新ゲストでは実 stack 予約と同量の単一 frame、および native 再帰が trap し、その後の正常 native 呼出も成功した。

ただし全外部 C/C++ host frame の事前計量を追加したわけではない。これを全 host stack fault の完全回復と主張しない。一般の TCC/CLI へ R15 必須 ABI を広げないため、profile は private helper でのみ有効化している。

## 4. paused coroutine の trap/TLS が main に残る場合

paused stack 内の trap pointer を main が使うと、別の live C++ frame へ longjmp する危険がある。単に RSP だけを保存する方式では足りない。

resume/yield は TLS runtime state と FP 制御・callee-saved registers を同時交換し、co の実 mapping を stack bounds に使う。補助ゲストは、親 co を止めたまま main の native call、親 co 内で子 co の停止/再開、R15 witness、FP 制御と local の保持、DONE 前 destroy 拒否、取消の通常完了後 destroy を確認した。JS roots/QuickJS state は別所有であり、主担当の GC/退役検査の結果と組み合わせて評価する。

## 5. native 成功件数を実サイト完成へ昇格する場合

最新 Puter serial は、初期の 1 module/54394 bytes/native calls 5 から、Launch の実クリック後に同じ loader が再構築され、最終累積 2 modules/108788 bytes/native calls 10/fallback 0 となった。Retry はクリックしていない。これは実 loader の native 実行の証拠である。一方 JSPI suspensions/resumptions は 0、Firefox 本体 UI 到達とは別である。API/CLI/29 境界検査と四媒体一致も、指定四サイトの表示・入力・スクロール・動画・console や性能比較を代行しない。

## 6. 配布媒体の古さを見落とす場合

`build/wasm-final-artifacts-jspi.log` は旧 CSP/初回 native 媒体の結果を再利用せず、最新 JSPI 全体 build 後に payload と四媒体を一度だけ照合した。initrd/staging、IMG/kernel/initrd、IMG↔VHD/VHDX、ISO extent が一致し、source 更新時刻も旧媒体取り違えを示さなかった。これは内容一致の確認であり、Hyper-V 実機受入ではない。

現時点で追加の確定回帰は発見していない。全 OOM/未 instrumented host stack/性能/未対応 Core features は依然別の未実証範囲であり、既存 interpreter fallback や成功件数で隠さない。
