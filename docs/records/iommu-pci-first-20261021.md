# PCI・FLR修理：第一検討（2026-10-21）

## 範囲と実装

- 担当 `pci`。既存の `AGENTS.md` 差分は保全。独立した第一検討であり、他担当の結論や実機成功を前提にしていない。
- `kernel/src/dev/pciuser.c`：claim解放で200ms待機やFLRを行わず、BAR取消・BME停止・IOMMU遮断と非待機のpending判定を行う。停止未確認は `pci_device.needs_reset` に保持し、隔離未確認のDMAページをFLR成功だけで再利用しない。
- 異常終了後の非隔離再claimは `N_ERR_NOT_READY`。隔離した非画面機能の次claimでFLRを実施する。起動画面GPUはgeneric FLRを行わず、`N_PCI_NEEDS_RECOVERY` でnative driverの回復責務を明示する。レイアウト・syscall番号は変更しない。
- `kernel/src/dev/pci.c`：FLR用保存CommandからBMEを必ず除去し、復元後のBME残留も失敗にする。BME停止確認に失敗した場合はpending=0を一瞬読めても既知のCompletion Timeoutを待つ。予約符号に64秒を捏造せず、pendingと期限不明の組合せではFLRを発行しない。
- 未準備または取消のFLRは、enumeration済みdevice所有の保存状態を保持する。次回はFLRを再発行せず復元を再開する。callerは `d->claimed` を待機中予約し、復元未完了なら新handleを渡さずdomainを再遮断する。完了時に保存状態を回収する。
- 一律1秒のPF readiness期限を除去。有効なReadiness Time Reportingがあれば広告時間を使い、無ければFLRの100ms後に一度確認する。未準備は保留として返し、無制限待機も行わない。RRSのVendor ID 0001hを通常の完了と誤認しない。
- `pciuser.c/h` の `pci_user_dma_limit` はroot所有の `iommu_dma_limit` を接続し、DMA確保側へ実際の変換幅を渡す。
- `pci.h` の説明をTC0・non-posted要求の範囲へ修正。画面hold解除はCPU描画の再開であり、GPUのBAR・scanout回復保証ではないと明記した。

## 一次仕様

対象は指定された `D:/Programes/OS DEV INFO/PCI Express Base Specification Revision 6.0.pdf`。Calibre同梱 `pdftotext.exe -layout -enc UTF-8` で `build/iommu-pci-spec-6.0-20261021.txt` に抽出した。他OS本体・Linuxソースは参照していない。

- §6.6.2、印刷pp.724–727：FLRはBME/MSI等を初期化し100ms以内に完了する。pending時は古いCompletionとの混同を防ぐ待機が必要。FLR後のConfiguration-Readyと、既存要求の完了は同義ではない。追加初期化中のRRSは許可される。
- §7.5.1.1.3、表7-4、pp.918–919：BMEはMemory/I/O要求を制御し、MSI/MSI-XもMemory Writeに含まれる。
- §7.5.3.5、表7-22、pp.965–966：Transactions Pendingはnon-posted要求だけを示し、FLR自体でもクリアされる。
- §6.4、p.717、§2.4、pp.223–224：TC0 posted writeのhost readによるflushと、他TC・RO/IDOのdriver固有同期を区別する。generic pending/ID readだけを万能のDMA drainとしない。
- §7.5.3.16、表7-34、p.995：Completion Timeoutの定義値だけを採用する。100µsはschedulerのms単位へ切り上げる。無効・予約値には一般的な待機上限がない。
- §7.9.16、図7-315、表7-280、pp.1264–1267：有効なRTR値を32の指数倍nsとして復号し、msへ切り上げる。FLR Timeの最大符号A1Ehを超す広告は使わない。旧コメントのPF一律1秒に§5.9を引用する説明は誤りで、1秒の明示はVFの§5.10.1にある。

## focused検証

1. 対象 `pci.c` / `pciuser.c` を正規kernel flags相当の `clang --target=x86_64-unknown-none-elf ... -fsyntax-only` で確認：成功。
2. `tests/pcitest.c` を正規user flags相当のtarget構文確認：成功。dirty非隔離拒否、隔離次claim FLR、EDUの回復要求bitを追加した。古い「非隔離crash後も再claim可」は、安全でない契約の訂正として期待値を変更したもので、受入失敗の除外ではない。
3. 新規許可された `tests/pci_hosttest.c` は実製品 `pci.c` をincludeし、CF8/CFC・clock・taskだけを模擬する。BME clear拒否時の保存復元、復元後BME残留、RRS後の保存状態再開、取消と残り待機、期限不明時にFLRを発行しない反例、non-wait pending、RTR復号を確認：0件失敗。模擬対象外の経路はabortし、成功値で代用しない。
4. ホストfixture作成中に、CLI引数のcomma解釈、sandboxからのtoolchain実体アクセス、GSベース `current_task`、未使用経路のlink依存を修正した。これらは試験基盤の失敗で、製品成功とは扱っていない。ホストLLP64のformat差だけを `-Wno-format` で除外し、製品targetは通常のformat警告有効で確認した。
5. 自分の変更ファイル限定 `git diff --check`：成功。全体検査は開始時からの `AGENTS.md` 空白差分で非zeroだったため、無関係な修正を混ぜず対象を限定した。

## 未確認・境界

- 実QEMU・EDU/virtio受入、実機IOMMU/DMA、NVIDIA RMの再初期化と画面復旧、Hyper-Vは未実施。重いbuild/VMはroot専任であり起動していない。
- `N_PCI_NEEDS_RECOVERY` と既定隔離必須・明示boot opt-inの共通方針はroot所有。native driverが正常回復したとの誤報をしない。画面holdの解除も画面再建成功ではない。
- FLRはIOMMU隔離の代用品ではない。保留中の保存状態はdevice lifetimeで維持され、再claimがなければ少量の状態を意図的に残す。
- 同期map更新とsource依存付き第一報告はrootの統合時に実施する。次はrootが同じ差分とfocused検証を独立確認し、第二検討へ引き渡す。
