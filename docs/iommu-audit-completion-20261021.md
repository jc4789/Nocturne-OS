# 追加監査修理の引渡し確認（2026-10-21）

## 依頼ごとの結果

- 表の拒否だけで割込みを停止する問題：全unitの読取り専用状態監査とplain契約へ修理。不明状態は安全と捏造しない。
- Intelの未知firmware queueへのCPU書込み：撤去。OS再利用RAMとの非交差を先に確認し、空のqueueをOS所有領域へ移設してwaitを確認。不確定storageは保持。
- AMDの最大depth常用：実identity受入幅を覆う最小depthへ修理。追加発見した任意engine・PerfOpt・MARC停止漏れも修理。
- PCI再claim：TP実消失、RRS再照会・対応Root Portの可視化、取消登録の原子化、BME共用化、raw Link Control offsetを修理。現在CT値から旧全要求のtimeoutを推測しない。
- GPU用非連続DMAの二乗走査：上限付きhint allocatorへ修理。実PMM反例12/0、製品経路の256MiB確保と全頁確認をAMD/Intelで実施。
- S3復旧失敗時panic・QI実fault後の永続保全：無根拠な自動再開へ戻さず維持。実機でのS3成功は未確認。
- 実4070 crash後の画面reset/scanout再建、nvrm idle接続/thread占有・主体別公平性：**未解決**。人工quotaや画面復元契約のないgeneric FLRは追加していない。

## 配布と証拠

最新kernel、initrd、IMG/VHD/VHDX/ISOを正規増分経路で生成し、payloadと媒体内容の一致を確認した。最新kernelは`242d597e803fdb3c38106eb7a315d7b78dba879b48eb68ccf52fcfe9c735352a`。AMD/Intelを同時に動かさず各終了0を確認し、主担当自身が四枚の実QEMU画面とhelp実キー応答を目視した。実機・Hyper-Vは未実施。専用scratchと展開コピーは回収し、標準媒体・通常data・旧差分と旧報告を保全した。

修理・反例の失敗履歴・受入範囲・未確認の詳細は`docs/iommu-audit-20261021.md`と各`iommu-*-audit-second-20261021.md`。今回の追加監査修理の引渡しであって、全実機問題の解決宣言ではない。
