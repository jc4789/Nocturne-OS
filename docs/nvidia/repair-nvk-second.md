# NVK/Mesa修理 第二検討（2026-10-20）

第一報は変更せず保存した。今回は「参照回収がGPU使用中メモリを解放しないか」「未完了点集約で待機を遅らせないか」「CPU contextが昇格でGPU方式へ変わらないか」「WSIの条件が実際のobjectへ届いたか」を反証対象にした。

## 所有権と待機点

- `nvkmd_nocturne_ctx.c:create_fence`はfenceにrefcnt=1を設定してexec contextへ保持させる。timeline pointは別の追加参照であり、完了値以下のpointを回収してもchannel所有参照は残る。
- exec context破棄は自分の最後のseqnoを待機し、fenceをdetachし、RM channelを破棄し、その後で最後のcontext所有fence参照を外す。readback用VRAMページも`nnrm_chan_destroy`のRM channel free後にunmap/freeする。
- 他のqueueが保持するCPU待機点は`nvkmd_nocturne_sync_gpu_wait_point`で独立にrefされるため、timeline配列の圧縮とは独立に残る。既存GPU待機・device破棄全般の安全性を全面証明したとは主張しない。
- 同一fenceのseqno=1とseqno=2を一つの後方点にすると、旧値待機が後続signalへ遅延し、跨queue循環を作り得る。このため集約条件を同一fenceかつ**同一seqno**だけに限定した。ホスト試験は旧値の待機先が旧seqnoのまま残ることを直接検査した。
- 実製品sync源の1万回完了signal試験は、各追加直後に保留point=1、fence参照=2を検査した。CPU signalで既到達値を上げた後もpoint=0へ解放する。試験固定反復数は負荷入力であり、製品上限ではない。

## 実objectの公開順序

最終crossbuild objectを`llvm-objdump --disassemble-symbols=nnrm_chan_submit --no-show-raw-insn`で確認した。該当順序は以下だった（offsetはこのobject固有の観測値であり製品定数ではない）。

1. `0x10f` CPU fence（lock or）。
2. `0x114` channelのposted_map読出し、`0x11b`そこからvolatile 32bit load。
3. `0x11d` CPU fence。
4. `0x129` USERD GPPut store。
5. `0x130` CPU fence、`0x13c`doorbell store。

読戻しがコンパイラーに消されていないことと順序は確認した。実PCIe fabricでの到達、キャッシュ属性の正しさ、装置消失競合の実機受入はこの逆アセンブルで代用できない。

## screenとWSIの別条件

- frontendホスト試験で、ready後の新contextがGPU screenを取得しても旧contextのfscreenと`on_gpu=false`が変わらず、旧contextの初期clearはCPU map、新contextはGPU clearを使うことを実関数で確認した。
- `caps.accelerated`によるcaveatは「選択したbackendがCPU rasterizerか」という製品ポリシー。GPU readback込み速度・低消費電力・高性能の測定結果ではない。GPUが選ばれたというだけの性能PASSは出さない。
- `llvm-nm`で最終`nvk_physical_device.c.o`に`U nvk_init_wsi`と`U nvk_finish_wsi`があり、Nocturneの条件がコンパイル済み初期化/終了経路へ届いたことを確認した。instance/physical/deviceの既存WSI entrypointテーブルは維持した。
- native契約試験には、拡張二つの広告、両拡張指定instance、CreateHeadlessSurfaceEXTとDestroySurfaceKHRの非NULL・生成破棄を含めた。弱参照entrypointが最終リンクで脱落していないかも、この試験で親が確認する。GPU不在ではpdev WSI/swapchain実行を証明しない。

## 現在の検証と境界

最終ホスト二群は0件失敗。最終rm/syncの追加差分crossbuildは終了0。第一報時の9対象crossbuildを、後から変更した2対象へ流用していない。native testのリンク・QEMU実行・最新browser描画は親の共有ABI安定後の統合検証へ引き継ぐ。実機NVIDIAのBAR1、複数GPU、ready後GPU昇格、画素・性能、Hyper-Vは未確認。

子の個別repo view/work機械gate未実施という第一報の制限は維持する。初回lock timeout後に親の明示指示でmap/sourceを実読し修理を進め、修理後の`control.py read --agent gpu_nvk`は終了0。repoの最終sync/viewは親が直列実行を調整する。
