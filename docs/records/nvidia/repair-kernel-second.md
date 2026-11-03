# GPU・native PCI修理 第二検討

2026-10-20、担当 `gpu_kernel`。第一報は変更せず、弱い前提を別に検討した。

## 反例と追加修理

1. **scheduler待機で生じる新しいユーザー寿命境界。** 従来はsyscall冒頭で出力を検査した後に直接書いていたが、待機中に別threadがunmapできる。入力をkernelへsnapshotし、出力も私有bufferで完了を待ち、copyout時に再検査する形を採った。上限撤去後の巨大な一括copy自体もGUIを止めるため、GPU専用のPAGE_SIZE単位検査/copyとresched・取消を追加した。他APIの既存atomicityは変更していない。
2. **resource差替えの命令値。** 自分の第一実装ではsurface破棄に誤ってBIND_OBJECTのop2を使っていた。公開wire定義で再照合し、DESTROY_OBJECTのop3へ修理した。構文検査だけではこの誤りを検出できなかった。根拠：[virgl_protocol.h](https://android.googlesource.com/platform/external/virglrenderer/+/af3d1de900fb0bf9094ed5d6ed361d8c2c71a543/src/virgl_protocol.h)。先頭のRed Hat MIT/X11許諾を確認。コードを取り込まず、wire定義を照合した。
3. **要求履歴が人工的な拒否を作る反例。** 横長の後に縦長を要求すると、保持済みwidthと新heightのcross-productが不必要に巨大になる。拡大capacityの確保に失敗した場合は正確な要求寸法で再試行する。二組のresource IDを交替再利用するため、累積作成数によるID枯渇は生じない。古いDMA backingの解放はdetach/unref完了後だけ。
4. **座標制限撤去とCPU clipping配列。** 旧16要素の黙示的打切りではなく、凸三角形を6平面でclipすると各平面で最大1頂点が増えるという幾何学的な3+6の容量を用いた。clip後のw=0の同次原点は射影に渡さない。実製品のclip/raster本体をincludeした `tests/gpu_cpu_hosttest.c` でint32境界と決定的100000ケースをundefined-trap付きで実行、終了0。これはnative syscallや実GPUの検査ではない。
5. **ROの再view逃れと既存mapping。** 物理viewをrootへ平坦化しても取消親scopeは残す。edu試験は親scope→派生view→別appの既存読取りmapまで作り、取消前の実読取りと「受信appに取消権限がない」を確認してから、取消後の実読取りでnative page fault終了(128+14)を要求する。親試験自身はfaultさせない。ownerの独立mapは残ることも検査する。

## 確認済みの範囲

- 変更したkernel `gpu.c`、`pciuser.c`、`syscall.c` とuser `gpu.c`、`sys.c`、`js_canvas.c`、`pcitest.c`、`gputest.c` はtarget clang構文検査が終了0。
- `git diff --check` 終了0。
- host実行：`tools/msys64/ucrt64/bin/clang.exe -O1 -std=gnu11 -fsanitize=undefined -fsanitize-trap=all -idirafter user/include -Icommon tests/gpu_cpu_hosttest.c -o build/gpu-repair-20261020/gpu_cpu_hosttest.exe` の後、同exeを実行し `gpu_cpu_hosttest: clip/raster boundary and 100000 deterministic cases passed`、終了0。Windowsホストではlongが32bitのため、未使用のnative syscall wrapperにpointer-to-long警告が出た。host検査はそのwrapperを使わずGPU IOをstubにしている。Nocturne用LP64 target構文検査ではその警告は出ていない。
- virgl capset2のmax2dは公開構造の77-dword v1に続くfield配置からdword121を算出し、必要prefix488bytesを検査する。根拠：[virgl_hw.h](https://android.googlesource.com/platform/external/virglrenderer/+/68429e8e1106d0861d9f9f180583bd8381b8bf96/src/virgl_hw.h)、同じくMIT/X11許諾。先頭のライセンスとstruct配置を実読した。
- 第一報の時点以後、共有ABIは変更していない。既存の1936-byte batchはlayout保持、新しい可変listで旧配列数を超える。

## 残る実検証と限界

- 親の修正前virgl実行ではnative backend1/caps15を得たが、既存 `blit-max-source-last-pixel` が失敗したとの報告を受けた。新しい成功と取り違えず、同じ試験を修理後に残した。現時点では原因・修理後結果は未確認。
- 修理後の単一QEMU edu/virgl実行・GUI応答・動的resource差替え・chunk境界は親が実施する段階。自分のhost試験や構文通過を実device合格へ昇格しない。
- キャッシュするtarget/sourceは各一資源で、次の大きい要求に合わせて拡張し小さい後続要求に再利用する。任意の大画像の性能・メモリ圧迫下の最適なcache方針は未測定。割当失敗時はCPU fallbackを保つ。
- Canvasの旧64最小clear選択は親の明示指示で保持。512上限のみdevice capabilityへ変更。小clearのGPU化や新しい自動計測機構は追加しない。
- NVIDIA実機、posted writeの実機順序、Hyper-V実機は担当範囲内で実証していない。他OS本体ソース・GPLv2-onlyコード・`uvm_common.c` は参照/追加していない。
- 個別J-space機械的repo view/work gateは開始時hashの停止時間を避ける親の明示指示で未実施。mapは実読、readとround1報告は実施済み。最終安定treeの機械refreshは親の直列管理で行う。

## 引渡し

製品ソースは安定。親の最新差分ビルドで `kernel/src/dev/gpu.c` の最終op3・capacity再試行・PAGE_SIZE checkpointを含め、eduとvirglの同一受入を行う。失敗があれば試験期待値を弱めず製品修理へ戻る。
