# PCI追加監査：独立第二検討（2026-10-21）

## 範囲

担当IDは既存の `pci`。第一報告、現在の製品・試験、一次抽出、主担当へ届いた監査指摘を独立に実読し、第一報告の弱い仮定を反証した。第一報告は変更せず、SHA-256 `9ef0ba90936cd285d4225649a8ed8ca1da0c4f12de3d6fd7c8ab11ec100cb2ba` を維持している。

主担当の同期窓で本人の `pulse --event resume → repo view → check --stage work` を一度実施し、すべて終了0、`ALLOW work` を確認してから編集した。記録は `build/iommu-pci-audit-second-{pulse,view}.log`。新しい下位委任、全tree gateの反復、重いbuild、QEMU、Hyper-V操作はしていない。製品変更は既存の `kernel/src/dev/pci.c/h`、試験変更は既存の `tests/pci_hosttest.c` へ統合した。共有ABI、共通IOMMU、PMM、scheduler、NVIDIAは編集していない。他OS本体・外部実装を参照せず、GPLv2-onlyコード、人工quota、推測期限を追加していない。

## 反例1：現在のCT値は、古い全要求にtimerがある証明ではない

一次根拠は `build/iommu-pci-spec-6.0-20261021.txt`、PCIe 6.0 §7.5.3.16（抽出56148–56157行）、§6.6.2（40662–40753行）。CT Disableをclearしても、既存outstanding Requestへtimeoutを適用することは任意である。

第一修理は「この関数がCTを有効化して待つ」案を退けたが、**旧driverが既にdisable→要求発行→enableとしていた履歴**を見落としていた。実native config-write経路はその操作を許している。現在のCTL2がenabled/defaultでも、旧要求にtimerがない仕様準拠装置が存在し得る。TP=1のままFLRし、現在の50ms等を根拠に再初期化すると、遅い旧Completionを新要求と混同する危険が残る。

`pci_function_reset` を次の共通条件へ修理した。

- CT値にかかわらず、BME clearと同一装置のreadback成功を必須にする。
- **FLR前のTP実消失まで**、既存 `reset_wait` の取消検査・wait登録をBSPの短いIRQ guardで原子的にしてyieldする。架空の期限は設けない。
- TP=0を実測済みなら、既知CT上限による追加待機を設けず、FLR/RTRの仕様待機へ進む。64秒の不要待機もなくした。
- 古い保存FLRの残待機・BAR/ACS保存・再開、BMEを復元しない条件は維持する。driverの正当なCT設定操作を人工的に禁止して隠していない。

**旧positiveの訂正**：第一報告以前の「BME stuckでも現在CTが既知ならFLR成功」は、この新反例により汎用の安全根拠を失った。主担当の明示承認に基づき、primary/peerともFLR未発行・handle非公開・dirty保持を期待する負例へ訂正した。時間値の増量、失敗の除外、仕様に反する安全期待値の弱化ではない。装置固有のquiesce保証がない状態で安全な汎用回復を捏造しない。

## 反例2：RRSの再照会ループだけではhost bridgeへ接続されない

一次§2.3.2（12421–12455行）は、RRS Software Visibilityが無効ならVendor ID readもRoot Complex内部で再発行されると規定する。Root Control +1Ch bit4のEnableは既定0（§7.5.3.12、55680–55684行）、Root Capabilities +1Eh bit0が対応表示（§7.5.3.13、55727–55729行）。旧host fixtureは可視の `FFFF0001h` を直接返すだけで、製品には対応Root Portの有効化がなかった。

既存PCI層へ `reset_rrs_visible` とnamed register/bitを統合した。

- 列挙済みの対象上流経路からRoot Portを探す。cached typeだけで書かず、BDF範囲、capの整列・通常config領域内の長さ、装置ID/presence、実cap ID・実Root Port type・Root Capabilityを確認する。switch/endpoint、不在や不正なcapへ書かない。
- 広告がある場合だけ、他Root Control bitを保った16bit writeとEnable readbackを使う。隣接Root Capabilitiesを巻き込むdword writeをしない。
- 初回FLRより前、保存FLR再開時の対象configアクセスより前、FLR待機後および各readiness read前に確認する。取消やsleep中にRoot Controlの値が失われても、古い成功を永続保証として使わない。
- 非対応・書込拒否を理由に従来ready装置のFLRを一律拒否しない。**不可視hardware retryまで取消可能になったとはしない。** RCRB経由の管理、対応しないRC、enable拒否の同期config readは依然platform側で待つ可能性がある。この条件を公開PCI内部契約のコメントにも明記した。

## 検証と対照

- 製品修理前の追加反例は **114件失敗**、終了1：`build/iommu-pci-audit-second-before.log`。CT履歴、全CT値のTP前FLR、Root Port既定offと状態喪失、旧BME-stuck positiveの安全条件訂正を含む。重複する順序判定もあるため、114種類の独立不具合という意味ではない。
- 同じ修理後のhost検査は失敗0。さらにCT既知のTP待機中取消とEnable readback拒否の明示対照を加えた最終検査も **失敗0、終了0、UBSan trap有効**：`build/iommu-pci-audit-second-final.log`。中間成功は `build/iommu-pci-audit-second-after.log` に保存している。
- `completion_history_cases` は実 `pci_user_config_write` でCT disable/enableを許し、旧要求の実完了前にFLRしないことを検査する。CT disabled/予約/default/210ms/64sのTP進行、BME stuck拒否、CT既知でも取消、保存FLRの一度だけ実行を含む。
- `visibility_cases` はRoot Control他bitと隣接capの保存、narrow write、一度のenable、取消後再開、sleep中の状態喪失、switch越しの正しいRoot Port選択、非対応/不在/別cap/type/不正BDF/短いcap、write拒否/readback失敗を確認する。既存の全group dirty・未回復peer拒否・画面primary/peer・再遮断・IRQ停止対照は残した。
- 正規kernel相当flags、`--target=x86_64-unknown-none-elf -Wall -Wextra -Werror` による `pci.c/pciuser.c` の構文確認と所有3ファイルの `git diff --check` は終了0。

最終hostコマンド：

```text
tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O0 -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -Wno-format -fsanitize=undefined -fsanitize-trap=undefined -Ikernel/src -Icommon tests/pci_hosttest.c "-Wl,--gc-sections" -o build/iommu-pci-audit-second-final.exe
build/iommu-pci-audit-second-final.exe
```

最終製品/host SHA-256：

| ファイル | SHA-256 |
|---|---|
| `kernel/src/dev/pci.c` | `ed9536747b7c18bb5b37edd409273fa1b59d4a2c437c8c16dec288496da68354` |
| `kernel/src/dev/pci.h` | `5422d279b606b5a5cea6c955e01dc18c6e004732df9bb687cd5ed9d98974cea5` |
| `tests/pci_hosttest.c` | `038a351663b3d787f971b2cb067501ed4fe4d4085218a5e68f7226781c4c840c` |

## 独立再読と限界

- BMEoffは新Memory/I/O要求の禁止、TP0はnon-posted要求の完了であり、全posted DMAのdrainではない。`dma_unsafe = !blocked` と確認済みIOMMU遮断をRAM再利用条件とする契約を維持した。全groupのdirty化や画面GPUのnative回復責務を緩めていない。
- PMMの有限上限でもhintを使う修理を静的再読した。`limit/PAGE_SIZE` による全ページ収容、末尾部分wordのmask、0ページ時の除算回避、実物理ページ範囲による加算/乗算範囲、syscallとIOMMU count=1経路の共用を確認し、追加の確定反例はなかった。本人はPMM host/256MiB guestを再実行していない。
- CF8/CFC、Root Port、clock、kill、装置の完了はhost模擬である。実RCのRRS再発行や実競合、S3、実fabric orderingの再現ではない。`iommu_attach` のgroup/ACS probeは回復処理より先に対象configを読み書きし得る。今回のfixtureはそこを模擬しており、**上位claim全経路のconfig無アクセス・取消可能性を証明しない**。
- 主担当が第一修理後に実施したAMD/Intel QEMUログは独立に読んだが、今回の追加修理前の結果である。DMA 256MiBの411/374ms、通常pcitest/FLR/nvrmtest、nativeabitest 225/0を、新しい製品のVM受入や実GPU性能へ流用しない。本人は新VM、無変更host反復、実画面操作を実施していない。
- 実4070のcrash後reset/scanout再建、nvrm接続公平性、実機DMA/IRQ、S3/Hyper-Vは未解決または未確認のまま。永久TP/RRSの安全な汎用回復を保証しない。

製品・hostは静止済み。主担当の増分build・単一QEMU再受入と最終source窓を待ち、その後だけ本人の既存 `pci` IDでround 2を一度登録する。現時点ではcontroller round 2は未提出。
