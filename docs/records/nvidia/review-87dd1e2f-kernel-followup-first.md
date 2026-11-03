# native 実行結果と channel 試験順序の追補・第一検討

担当 `gpu_kernel`。既存二報を上書きしない。

親が最新 kernel と native 試験を QEMU edu で実行した。
`build/nvidia-review-87dd1e2f/serial.log` の本追補時点の118行は `pcitest: 0 failed`。
同106〜111行の二つの child page fault は、取消後の実アクセス失効を確認する試験の期待動作である。
構築中 NOT_READY と取消後の kernel copy 拒否を含む pcitest が成功したという親の観測とも一致する。
このログは VM 稼働中であり不変証拠とはせず、本報告は製品・試験 source に結合する。

続く nativechanneltest が完了しなかったため、親の依頼で変更・再試験せずソースを切り分けた。
3MiB+17 の message を送った直後、同じ thread が空 message を送り、巨大 message の受信はその後だった。
queued bytes が flow 閾値以上なら長さ0でも send_wait へ入るため、受け手を兼ねる自身の停止を待つ。
`git show 87dd1e2f:tests/nativechanneltest.c` でも同じ順序を確認し、今回の credential 差分で
message_queue_full や send_wait を変更していないことを確認した。既存試験の自己 deadlock である。

親の承認後、巨大 message の PEEK、短 buffer 拒否、全量一致を先に行い、次に空 send/receive を行う順序へ修理した。
削除した検査、弱めた期待値、追加 skip は無い。製品の flow 契約を変えて試験を通す変更もしない。
nativechanneltest の focused clang 構文確認と `git diff --check` は終了0。

親が停止するのは該当 nativechanneltest だけであり、再実行・VM 操作は親が担当する。
修理した試験の実結果はまだ未確認。NVIDIA 実機、Hyper-V、性能の成功は主張しない。
