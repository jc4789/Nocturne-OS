# GPU・native PCI修理 統合追補 第一報

2026-10-20、`gpu_kernel`。旧第一・第二報を保持し、統合段階のcoreへ最終ソースを再結合する。

## 実失敗からの修理

- 修正後最初のeduではRO protect試験が1件失敗した。原因は試験側が `n_memory_protect` の便利API(-1/errno)をraw native負エラーと比較していたこと。`nocturne.h`、`sys.c`、VMMのRO拒否を照合し、戻り-1かつ`errno=N_ERR_DENIED`と、raw syscallの`-N_ERR_DENIED`を両方要求する試験に修理した。書込み許可を成功扱いにする変更はしていない。VMM製品コードの変更は不要だった。
- 修正前からの末端pixel失敗は、修正後も返り0・実pixel=`ff102030`、期待=`42123456`として残った。実pixelは直前の縮小描画の左上だった。正式COPY_REGIONへの変更仮説も、四隅・包含2×2で旧pixelが残り棄却された。一方、同じsource末端1pixelの拡大は成功した。
- 主担当・NVK担当・自分が独立に、MIT許諾のvirglrendererで等倍copy fast pathは生Y座標、scaled blitはY_0_TOP変換を使う差を確認した。Y_0_TOPを指定したまま両者へ同じnative座標を渡すと、copyは別の物理行へ書き、top-origin readbackは旧行を読むという実結果と一致する。
- **最終製品修理**：全GPU画像をwire flags0の一原点へ統一し、upload/crop/copy/scale/readbackのrow0を揃えた。nativeの上原点描画だけviewportの負Y scaleで明示変換する。不要になったCOPY特例は撤去しBLIT一本へ戻した。既存rasterizerのcull bitsは0、両面fillであり、負Yのwinding変化で三角形を消さない。期待pixelを変更していない。

## 実確認

親所有の単一QEMUによる `build/gpu-repair-20261020/origin-after-serial.log` を自分も対象行検索で読み、以下を確認した。VM稼働中のためこの可変logをsource hashへ束縛しない。

- `pcitest: 0 failed`。RO拡大拒否、raw/便利API、派生view取消、取消前読取りと別childの取消後既存map fault142、IRQ無限待機のdisable wakeを含む。
- `nvrmtest: 0 failed`。edu上の不適合装置拒否とclaim解放の範囲であり、NVIDIA RM描画成功ではない。
- 一般GPUはbackend1/caps15、`gputest: 55 checks, 0 failed`。元の末端pixel、四隅、包含2×2、末端拡大、上半分だけの非対称三角形、641×37・97三角形、旧ABIとclip/fallback契約を含む。
- NVK/Mesa native契約試験は0件失敗、実選択はsoftpipe/caveat1。NVIDIA実GPU経路の実証へ置き換えない。
- 自分のtarget構文検査と`git diff --check`は0。CPU clip/raster本体のundefined-trap付き境界+100000ケースは前報の通り成功。

## 引渡し・限界

製品編集は安定化した。最終GUI操作、CPU fallback条件、四媒体更新と最終map/機械refreshは親担当。NVIDIA実機、Hyper-V実機、一般GPUの厳密な性能比較は未実証。eduは従来PCIなのでDMA停止完了の証明がなく隔離する。ログの旧表現「requests in flight」を、実際にpendingを観測した証拠とは解釈しない。

独立ライブラリーの参照はMIT許諾であり、コード取り込みなし。他OS本体・GPLv2-only・`uvm_common.c`は参照/追加していない。[rendererのcopy/blit境界](https://android.googlesource.com/platform/external/virglrenderer/+/af3d1de900fb0bf9094ed5d6ed361d8c2c71a543/src/vrend_renderer.c)を比較根拠とした。
