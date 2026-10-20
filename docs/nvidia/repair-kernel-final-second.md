# GPU・native PCI修理 統合追補 第二検討

2026-10-20、`gpu_kernel`。前の追補は保持する。最終ソースを対象検索で再読し、受入の意味を再検討した。製品の追加編集はしていない。

- **別命令への置換だけでは原因を直せない。** COPY仮説は実runで失敗し、その仮説を残したまま成功扱いにしなかった。最終はCOPY特例を削除し、等倍も拡縮もBLIT16へ戻し、resource flags0の原点を全operationで共有する。native描画の上原点はviewport負Yと正の半height offsetが保持する。
- **中心画素だけでは上下逆転を見逃す。** 追加した上半分だけの単色三角形は、上側内部が色、下側内部がclearであることを要求する。親の最終runはこの非対称条件も含め55件0失敗。四隅・包含2×2・拡大・元の末端期待値も削除していない。
- **試験契約の訂正と製品の緩和は別。** RO protectは便利APIで-1とDENIED errno、raw syscallでDENIEDを両方要求する。revoke後の別child実読取りfault142と、独立owner mapping保全を別に確認する。失敗を「戻りが負なら何でも可」へ弱めていない。
- **実GPU経路を混同しない。** 実確認された一般GPUはvirglであり、NVIDIAをhost rendererとして使っていてもNocturne RM/NVKのNVIDIA実機成功ではない。eduのnvrmtest成功も「不適合装置を安全に拒否・解放」の範囲。softpipe契約試験、実virgl、実NVIDIA、Hyper-Vは分離する。
- **未計測性能を断言しない。** scheduler待機とPAGE_SIZE checkpointが存在すること、pixel受入が成立したことは確認したが、CPU比の高速化、厳密な待機中GUI latency、任意OOMでの最適なcache運用を証明してはいない。Canvas小clear選択は維持している。

`git diff --check`は最終ソースで終了0。最終GUI/CPU fallback/四媒体と機械map refreshは主担当の実結果を待つ。VM稼働中logへ不変hashを付けず、報告は現在の製品ソースへ束縛する。NVIDIA実機とHyper-V実機を未実証として保持し、担当製品ソースは安定状態で引き渡す。
