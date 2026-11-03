# NVK/Mesa最終追補 第二検討

2026-10-20、gpu_nvk。旧第一報・第二報と最終追補第一報を保持した。今回は新規試験も製品編集も行わず、結論を広げ過ぎる反例を確認した。

1. **native成功でも実NVIDIA成功にはならない。** ログ自身がsoftpipe/caveat=1と明示している。headless instance/surfaceとCPU描画保持の実受入へ更新できるが、BAR1排出・ready後実GPU昇格・複数GPU・GPU性能は未確認のまま。
2. **GCリンクは試験迂回ではない。** native契約targetの`--gc-sections`は通常アプリと同条件で、試験内の拡張二つ・非NULL entrypoint・surface生成破棄・赤緑readbackのCHECKは残っている。実serialの0失敗で、その到達した範囲だけ受け入れる。
3. **一般GPU APIの失敗を混ぜない。** 同一after-serialのvirgl blit失敗が、NVK/Mesa 0失敗で消えるわけではない。親の修理継続対象として区別する。
4. **firmware独立レビューの反例。** GSPだけ欠落した既存installが空manifest成功になる旧条件を指摘し、その後の親修正で拒否を実確認した。さらにpreflight hash後・実read前の変更を指摘した。現在のpack源は実際に格納する`data`のSHA256を再照合し、一時tarから成功時だけreplaceする。現在の試験源にもGSP欠落とhash後変更・旧archive保持・一時物回収の反例が追加されている。今回その試験を再実行したとは主張しない。

結論：NVK/Mesaは既報の製品修理に加え、最終native契約と実ブラウザーCPU描画の証拠が増えた。一般GPU APIの残る失敗と実NVIDIA未確認を除外して全面成功とはしない。最終coreを消費して新round1/2へ再結合し、旧報告を保持した。
