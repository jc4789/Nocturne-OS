# Intel CFI修理・限定レビューの引継ぎ（2026-10-21）

最新指示に従い、このchildからcontrollerの第二報告は提出しない。第一報告 `docs/iommu-intel-live-first-20261021.md` と以前の第一・第二報告は変更していない。主担当が別の `delegate_task` で独立第二検討を行う。

## 返却された限定レビューと処置

- 一次仕様のCFISがIREかつ非EIME時に適用されるという文言は、適用外時の任意値読出しや書込み無視を明記していない。CFI更新のCFIS通知、GSTSからGCMDを構築する規定もある。したがって「CFI0確認後にIRE開始でCFISが1へ戻り、要請が失われる」という仕様適合反例は立証できず、確定不具合としない。追加の製品変更はしていない。
- **x2APICではEIME自体が互換形式IRQを遮断し、CFIS0は隔離の必要条件ではない**という異論を保持する。現在の製品は全remap unitでCFI0を要求する。新しい第二検討では、このx2条件の必要性を再判断する。今回のhost成功だけであらゆる実機のCFIS動作を保証しない。
- 一時CFI1必須化を撤去する結論は維持。IOAPIC変換はcommonが一回だけ所有し、Intel backendは表・CFI・IRE・TEの起動のみ返す最新契約を反映済み。
- 元の非mask ExtINT/PIC復帰順の反例は第一報告に保存。主担当の共通移行修理の最終動作は、このchildでは未確認。

## 保存状態と検証範囲

- 製品 `kernel/src/dev/iommu_intel.c`: `f9b6c5d03a4609f982547e24392fc000893702fb29732088ec6eabe0373c0e0b`。
- 試験 `tests/iommu_intel_hosttest.c`: `3df88856545c83ef7206447bc2dd3136fef13a46bebd5dcb8fde6d0bfaeb000a`。
- 最新責務版の実行は `build/iommu-intel-live-common-boundary.log`、失敗0・終了0。target freestanding構文検査と対象 `git diff --check` も終了0。
- 永久CFI0、firmware初期CFI1、own0、解除失敗の拒否とlatch保持、後方unit解除失敗前の未enable、x2、条件付きmasked S3、非mask pinの反例を含む。固定edge無損失や実IOAPIC/PIC操作の証明ではない。
- 重いビルド、QEMU、媒体、Hyper-Vは操作していない。修正後EDU・実機DMA・実機S3は主担当の別検証が必要。

独立限定レビューのtask IDは、初回 `node:delegated-task:command%3Amcp%3A5ac1cce2-fe5a-4f95-a279-abaa22f9b203%3Adelegate-task%3Aintel-cfi-spec-challenge-20261021`、再反証 `node:delegated-task:command%3Amcp%3A5ac1cce2-fe5a-4f95-a279-abaa22f9b203%3Adelegate-task%3Aintel-cfi-spec-challenge-round2-20261021`。双方は読み取り専用で終了した。controllerの登録担当は既存の `intel` を継続し、これらの限定返答を正式な製品受入やcontroller第二報告へ昇格していない。
