# Intel VT-d 最終統合の第一記録（2026-10-21）

## 範囲と履歴の保全

- 既存担当ID `intel`、J-space xhighの新しいround 1。旧 `iommu-intel-first/second`、`iommu-intel-live-first/second`、`iommu-intel-live-handoff` の五報告を実読し、変更していない。開始時の共有差分と `AGENTS.md` のnative ABI保全追記を保全した。
- 自身のpulse/read、同期mapのrepo view、work gateを実施し、終了0・`ALLOW work` を主担当へ即通知した。独立した文書phaseであり、製品・tests・mapは編集しない。build、検査再実行、QEMU、媒体、Hyper-V、通常dataの操作もしていない。追加委任は行わず、次のround 2は主担当による別委任とする。
- `iommu-intel-live-second-20261021.md` はSHA-256 `57474d810c21b937f5f0ad02dddb510f8b9ccbe1f532dd0d27f6da52737b4100` のまま保全。その提出後のIRREQ伝播と共通失敗ガードは旧報告のsource依存を変えたため、旧受入を現ソースへ流用せず、本記録で差分を結ぶ。

## 最新ソースへの接続

1. `kernel/src/dev/iommu_intel.c:1041,1066` は全unitの `ECAP.IRREQ` をOR集約し、`intel.interrupt_remapping_required = irreq` をprobe返却前に設定する。一次抽出 `build/iommu-intel-spec-20261021.txt:4623–4628`（VT-d Rev.5.20 §5.1.4）では、IRES=0かつIRREQ=1の割込み要求は遮断される。したがって「停止した」だけをplain配送可能の根拠にしない。
2. `iommu.c:91–137` はPIC/IOAPICをmaskしてbackend start、成功後に一回の `ioapic_remap()` を所有する。失敗時はshutdownを確認し、`stopped && !interrupt_remapping_required` の場合だけplainへのrollbackを試す。IRREQ、停止不確実、rollback失敗は `restore_failed` を保持する。probeがNULLでもIVRS/DMAR宣言があれば同じ不確実性を保持する。
3. `iommu.c:757–764,853–876` のclaim、新MSI、MSI clear、IOAPIC、resume/shutdownはこのlatchを迂回しない。`iommu_power_ready()` と `pm.c:136–144` は不確実な状態でのsleep/hibernateを拒否する。初期化時のPIC復帰はmaskのみで、PICの再初期化ではない。
4. Intelの `unit_tables()` → `units_on()` は全unitのSIRTP、必要なglobal invalidation/sync、通常modeの全unit CFI0確認、IRE/TEの順。CFI0条件は `remap && !x2` で、一時CFI1必須化はない。§5.1.4、抽出4648–4653行のEIME遮断を再読した。`command_idle()` の既存 `command_failed` 拒否、xAPIC解除失敗拒否、queue異常の非skip・不確実性保持も残っている。
5. `pm.c:170–201` と実 `idt.c/ioapic.c` を再読。S3前の保存とmask、wake後の再mask、実際に眠った場合だけIOMMU復帰、その後IOAPIC→PIC→PCIの順を維持する。中止sleepではPIC maskだけを復帰し、保留edgeを再初期化で消す旧経路へ戻っていない。実edge配送や実S3の証明ではない。

この範囲の静的再読で、新たな根拠ある重大反例は見つからなかった。製品修理は追加していない。他OS本体・Linux・QEMU実装・外部NVIDIA `uvm_common.c` は参照せず、GPLv2-onlyコード、人工上限、ABI回避を追加していない。

## 今回確認した証拠と限界

- 自身が実行したのはソース・一次抽出・報告・ログの読取り、hash照合、対象 `git diff --check`（終了0）とcontroller操作だけ。無変更のhost試験は再実行していない。
- 主担当実行の最終ログを実読：`build/iommu-repair-20261021/intel-final-hosttest.log` は失敗0、`common-live-hosttest.log` は84判定/失敗0、`pm-live-hosttest.log` は51判定/失敗0。自分が実行した結果とはしない。共通試験はIRREQ必須と停止成功の区別、probe NULLかつfirmware宣言あり、MSI/clear/IOAPIC/PM拒否の対照を持つことをソースで確認した。
- 主担当の `intel-final-serial.log` はVT-d/IR起動、EDUの隔離yes、禁止DMA遮断、`pcitest`・`nvrmtest`・group FLR・`nvrm_servetest` の失敗0、native ABI 225判定/失敗0を記録する。pcitest子のアクセスfaultはrevoke後のfaultを期待する試験と照合した。NVIDIA未対応EDUの拒否とthread limitによるclient拒否も記録されており、実GPU回復や無制限の接続成功とは読まない。
- ただし同QEMUのECAPは `f00f4a` でIRREQ(bit62)は0。IRREQ=1の実probe→共通失敗経路の実機受入を、この成功から証明できない。Intel host側のprobe検査も幅検査であり、IRREQ集約値の明示的な判定はない。今回はその伝播を静的に確認した範囲に留める。
- 主担当報告のWHPX/4CPU/4096MiB/1080p/AHCI条件、実keyboard/desktop目視、媒体一致、VM停止は主担当の実施。本担当は画像を開いておらず、画像目視・実操作・媒体照合を自身の実施と偽らない。QEMU成功を実機・x2APIC・S3・Hyper-V成功へ昇格しない。

## 最終スナップショットと次の一手

| 対象 | SHA-256 |
|---|---|
| `kernel/src/dev/iommu_intel.c` | `73b743817c2471bf01df7e27acda3ecc62dea5d9034f22121aea087f93b83a14` |
| `tests/iommu_intel_hosttest.c` | `eef44563da3f0dd8af5a8bd0719b10a1c92a54a549430ec6fec0318db630d1bb` |
| `kernel/src/dev/iommu.c` | `f3dea5f4f8de569fdbfda76d1af0c0e5bfc01458ff466d3fd0c6deee8187c66e` |
| `kernel/src/dev/iommu.h` | `622b99536fef07e7abb91443ad64eca654f4e9297d8b077a05a2c6bb08d818d5` |

実機firmware/SMMの唯一producer引継ぎ、初期queueとPMMの寿命、実DMA drain、実edge、S3/hibernate、RTX 4070のreset/scanout復帰は未確認。nvrm同一appのidle接続によるthread占有は未解決。CFIS固定1は不要条件を反証する模擬入力であり、準拠実機の再現という旧断定へ戻さない。

本記録を最新ソース依存付きのfresh round 1として提出する。次は主担当が独立受入し、別の `delegate_task` で最終round 2を行う。追加製品編集や再build/QEMUはこの文書依頼から再開しない。
