# PCI寿命と割込み移行：新しい第一検討（2026-10-21）

## 範囲・保全・作業条件

- 担当 `pci` の新しい検討周期の round 1。旧第一・第二報告を全文読み、上書きしていない。旧報告のSHA-256はそれぞれ `e9d001e8e90cf138a5e439f260981c27b7a7145fb681d3eae6e02ed50599d016`、`0a31ca55a4114d5da66124a668d31bf8236e5e333431458799b27f414c3afbd3` のまま。
- 開始時の共有差分と `AGENTS.md` 差分を保全。同期通知までは読取専用とし、主担当が全製品を静止・map同期した窓で `repo view --agent pci`、`read --agent pci`、`check --stage work --agent pci` を一度通した。終了0・ALLOW後に独立所有内の修理を実施した。
- 今回の製品編集は `kernel/src/dev/pci.c`、`pci.h`、`pciuser.c`、`pciuser.h`。試験は既存 `tests/pci_hosttest.c` へ統合。共有ABI・バックエンド・PM・媒体は編集していない。map更新と重いbuild/QEMUは主担当専任。
- 実機が対象。他OS本体・Linux・外部NVIDIA `uvm_common.c` は参照していない。GPLv2-onlyコード、固定quota、人工的な待機上限、新しいbridge/APIは追加していない。

## 独立反証と責務の分離

### 共通割込み・PM：主担当が修理した反例

1. 中止したsleepでも `pic_restore → pic_init` を実行し、保留edgeを失う余地があった。主担当へ通知し、実際に寝た場合だけ完全復元、`slept=false` はmaskだけを復元する修理を現行ソースで確認した。Intel公式 [Core M I/O Datasheet](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/core-m-processor-platform-i-o-datasheet.pdf) §5.6.2.1、印刷p.147は、ICW1後のIRQに新しいlow→high遷移を必要とすることを記載する。これは実機での入力消失を実行再現したとの主張ではない。
2. 初期化失敗後に `hw=NULL` としても、MSI/IOAPIC wrapperがplain成功を返し、新しい経路を開ける反例を通知した。現行の `restore_failed` によるmessage・clear・IOAPICの失敗伝播を静的に確認した。
3. 停止確認済みでもplain配送できるとは限らない。Intel VT-d §5.1.4、抽出txtの4623行付近ではIRREQ=1/IRES=0のinterrupt requestはblockされる。主担当が追加した `interrupt_remapping_required` と、停止確認かつ必須IRでない場合だけplain rollbackする条件を確認した。
4. `ioapic_remap` の変換失敗では元のplain要求をrollback用に保持し、新しいrouteの失敗ではmaskedの要求を保持する区別を読んだ。sleepの保存RTEはmaskを含む実値であり、成功resumeは同じIR tableを再設定してからRTEを戻す。現行コードのこの範囲から、新たなretired vectorの復活や保存RTEのtable解放後参照は確認していない。ただし全register bank・実edge・実配送の検証ではない。

共通/PMの84判定・51判定とtarget成功は主担当の実行結果である。本担当自身のPCIホスト実行とは区別し、そこから実S3や実機配送成功を推定しない。

### PCI group：今回の追加製品修理

旧コードでは、AのclaimがP2Pで到達できたBへ状態を残しても、解放時の `needs_reset` はAだけだった。IOMMUで旧groupを遮断した後、別claimが全memberを新domainへ接続し、Bが新primaryとなる場合やAだけFLRする場合には、旧peerの残存状態を消した根拠がなかった。

一次仕様 `build/iommu-pci-spec-6.0-20261021.txt`、§6.6.2、印刷p.724、40600行は、FLRが対象Functionだけに作用すると明記する。AのFLR成功をBの回復証明にはできない。この反例は新domain外の任意kernel RAMへの到達を実証したものではなく、許可した新DMAページへの旧peerの要求・状態継承を扱う。

- 複数memberの解放では、clean primaryも含め全memberを `needs_reset=true` にする。peerのBMEをclear/readbackするが、exitへ待機・FLRは追加しない。
- DMAページ再利用は従来の第二修理 **`dma_unsafe = !blocked`** を維持。BME停止やFLR成功はIOMMU遮断確認の代用ではない。
- handle公開前の既存 `claim_recover` 責務へgroup処理を統合した。新domainはすでにattachedで全memberを予約し、primaryの `claimed` も回復・file確保全体で維持する。
- fresh peerもBMEoffを確認する。旧dirty peerはBMEoffだけで回復済みにせず、MSI/MSI-X無効化確認とFLRを要求する。BMEclearを拒んでも、dirty memberを既存の仕様準拠FLRで停止できる場合は回復を許す。
- dirty display peer、FLR不能peer、無効化・FLRの失敗、未準備、取消ではhandleを渡さず、groupを再遮断して `NOT_READY`。失敗したresetの保存状態はdevice lifetimeで残す。
- native回復を委ねる既存例外は新primaryだけに限定した。boot display primary等では `NEEDS_RECOVERY` を残し、generic FLRしないが、例外でhandleを渡す前にもBMEoff/readbackを必須にした。FLR可能primaryの既存回復は妨げない。
- `pci_msi_disable` はMSI/MSI-X Enableのreadback失敗もfalseにする。失敗時にもIOMMU側の取消しを試み、vector再利用や旧EnableのFLR保存復元を成功扱いしない。peerに対する追加の無効化はclaim回復側で行い、exit待機は増やしていない。
- `pciuser.h`、claim内部、`pci.h needs_reset` の古い説明を現契約へ修正。`syscall.c` の説明訂正とnative ABI文書は主担当の責務。

## 最後の反証：保留FLRとconfigアクセス

主担当から、取消し後のFLR残待機より前に `guards_init`、peer BME操作、MSI無効化が走る順序を指摘された。

§6.6.2、pp.725–727はFLR中のRequestをsilently discardしてよいこと、FRS後のConfiguration-Ready、古いCompletion待機、100ms後の再設定を区別する。一方、**すべてのFLRで100ms以内のconfigアクセスを一律禁止する規範は今回確認できていない**。§6.6.1のConventional Resetの待機禁止と混同しない。

この未立証の一律禁止とは別に、保留memberはBME/MSI処理より先に既存 `pci_function_reset` へ戻し、保存された残wait・readiness確認・復元を再開するよう修理した。`guards_init` も回復後へ移した。MSIを有効なまま保存し直すことや二度目のFLRは行わない。

ホスト検査のconfig監視は実PCI claim/recovery経路を対象に、保留primary/peerの残wait前のアクセス不在を確認する。ただし `iommu_attach` は模擬であり、実共通group/ACS probeのconfig読出しまでこの検査が不在を証明するわけではない。無関係な全device待機や架空のconfig禁止でこの境界を隠していない。

## 実行結果と失敗の保全

1. 既存fixtureを二機能CF8/CFCモデルへ拡張し、修理前の製品で **15件失敗**を得た。`build/iommu-pci-live-before.log`。fresh peer BME残留、clean旧primaryの交代時未reset、dirty非primary、失敗/取消時のhandle公開等を含む。
2. 修理後は同じ期待値を維持し、MSI/MSI-X readback、primary/peer交代、取消・RRS再開、残wait前の副作用、予約中の別claim拒否、BME停止拒否のFLR回復、clean singleton、display primary/peer、file確保失敗を追加した。最終ホスト検査はUBSan trap有効で終了0、**失敗0**。`build/iommu-pci-live-after.log`。
3. 追加対照の編集途中に、直前の結果判定より先に次ケースのfixtureを初期化して1件失敗した。期待値は変えず判定位置を訂正した。`build/iommu-pci-live-fixture-order-failure.log` を残した。これは試験配置の不備であり、製品回帰や実機成功へ読み替えていない。
4. 正規kernel相当の `clang --target=x86_64-unknown-none-elf`、`-Wall -Wextra -Werror` とinclude設定で `pci.c/pciuser.c` のtarget構文確認：終了0。担当製品・試験限定の `git diff --check`：終了0。
5. compiler実体は既存の `tools/msys64/ucrt64/bin/clang.exe`。sandboxでの実体アクセス制限は許可された `require_escalated` 実行へ切り替えた。途中のCLI comma解釈と存在しない別compiler候補の失敗もあり、同じ失敗の反復や別ツールによる権限迂回はしていない。ホストLLP64 format差だけは既存通り `-Wno-format`、製品targetのformat警告は有効。

再現用ホスト構築条件：

```text
tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O0 -ffunction-sections -fdata-sections -Wno-format -fsanitize=undefined -fsanitize-trap=undefined -Ikernel/src -Icommon tests/pci_hosttest.c "-Wl,--gc-sections" -o build/iommu-pci-live-hosttest.exe
build/iommu-pci-live-hosttest.exe
```

CF8/CFCと実 `pci.c/pciuser.c` は実行するが、IOMMU attach/block・vnode作成・clock・割込み禁止は模擬である。実DTE/IRTE、ACS group発見、PCIe ordering、schedulerの実競合、GPU scanout、実DMA配送は証明しない。

## 未解決・次の受入

- 明示 `appdrivers-unisolated` でdomainがNULLの場合、汎用group安全保証はない。dirty primary拒否と、未遮断DMAページ保全を維持し、opt-inを全group回復保証へ拡大していない。
- `nvrm_serve.c` の同一appによるidle接続/thread資源占有は未解決。根拠のないper-client quotaを加えていない。
- 実RTX 4070等の回復、scanout、実S3、Hyper-Vは未検証。今回、本担当はbuild媒体・QEMU・Hyper-Vを操作していない。主担当による最新媒体の単一QEMU EDU受入も実機受入とは別である。
- 製品と試験は保存して静止済み。主担当が最終差分・反例を独立確認し、map同期、最新媒体の受入、別の委任による新round 2へ進む。新round 2はこの文書や旧報告を上書きしない。
