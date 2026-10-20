# NVK/Mesa最終追補 第一報

2026-10-20、gpu_nvk。親の検証段階coreをpulseで読み直した。旧`repair-nvk-first.md`・`repair-nvk-second.md`は保持し、今回は記録の整合だけを更新した。再試験、追加build、repo指紋走査は行っていない。

- 監査3・7・8・9・10と11のNVK側の実装修理・ホスト試験・object確認は旧報告に記載した範囲を維持する。
- 親がnative契約試験を通常アプリと同じ`--gc-sections`でリンクし、統合build/pack成功を報告した。現在のMakefileの専用target指定と`integration-build-pack.log`のSYSROOT・TAR・IMG完了、試験target更新済みを読み取った。
- `build/gpu-repair-20261020/after-serial.log`の143–144行を自身で読み、`描画backend: softpipe / caveat=1`、`NVK/Mesa native契約試験: 0件失敗`を確認した。これはheadless依存・instance/surface entrypointとCPU GL二context契約の実製品結果であり、NVIDIA実行ではない。
- `after-webgl-middle.png`を自身で開いた。実Nocturneブラウザーの`file:///data/tests/gpu_browser.html`で上が赤・下が緑の独立canvas、画素保持の結果、CPU描画時のstrict caveat要求がnullである表示を確認した。自分がこの追加確認でマウス操作したとは主張しない。
- 同じafter-serialには一般GPU APIのblit末端画素失敗も記録されている。NVK/Mesaの結果でそれを相殺せず、親の別修理・別結果と区別する。

GPU準備後の実NVIDIA昇格、BAR1の実装置到達、複数GPUの実描画、GPU性能、Hyper-Vの受入は引き続き未確認。`uvm_common.c`は参照も使用もしていない。子個別repo view/work機械gateを未実施とした旧報告の経緯を、今回の記録更新で成功へ書き換えない。
