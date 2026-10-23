# Intel VT-d 第一報告（2026-10-21）

## 範囲と状態

- 担当ID: `intel`。独立した第一検討。製品変更は `kernel/src/dev/iommu_intel.c` のみ、模擬検証は `tests/iommu_intel_hosttest.c`。既存の `AGENTS.md` を変更していない。共有契約の編集は主担当が実施。
- J-spaceはxhigh。開始時のresume・map viewを実施。work gateはcontroller同時更新で2回競合した後、`ALLOW work` を確認して編集した。AMD開始gate中は編集を保留し、主担当の再開通知後に再開した。編集後のmap再同期は主担当の統合段階で行う。
- 重いビルド、QEMU、Hyper-V、通常のdata媒体は操作していない。実機、EDU DMA、音声・画面の合格を主張しない。

## 一次資料と確認した境界

一次資料は明示許可された `D:/Programes/OS DEV INFO/D51397-019-vt-directed-io-spec.pdf`（Rev.5.20）。既存PyMuPDFのgettextで `build/iommu-intel-spec-20261021.txt` へ抽出した。抽出のSHA-256は `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。他OS本体のコードを開いて参照・移植していない。

| 仕様節 | 結果 |
|---|---|
| 6.5.2、11.4.9.1～3 | queueの幅・QS・head/tail整列、N-1空き管理、register invalidation完了後のQIE、終端wait完了かつemptyでのQIE停止を照合。既存firmware queueにも元IQA/幅/長さを保持した終端waitを追加する。 |
| 6.5.2.9、6.5.2.12 | waitのSW/FNとDWORD coherent status writeを確認。現行のwait形式は正しかった。descriptor公開前にsfenceを追加。 |
| 6.5.2.11、11.4.9.9 | IQEIは低4bit。1はtail不正、2はfetch失敗でありdescriptor置換では直らない。ICESIDは63:48、ITESIDは47:32。IQE/ICE/ITEの不確定性を保持し、エラー記録をclearせず、descriptorをskipしない。 |
| 6.6、6.7、11.4.4 | SRTP→global context/IOTLB invalidate→wait→TE、SIRTP→global IEC invalidate→wait→IREの既存順序を確認。GCMDは一操作ずつ確認し、失敗後の後続commandを拒否する。 |
| 6.8、6.9 | invalidationに伴う暗黙write-buffer flushと、必要時の明示WBFを区別。WBFSは完了時clear。RWBF不要経路もqueue異常を確認。firmwareのWBFS/PMEN/拡張command待ちとtimeout後のhardware所有を保持する。 |
| 8.1、11.4.2、9.8 | DMAR HAW、CAP.MGAW、採用SAGAW、entryの物理address fieldから共通契約のiova_bits/phys_bitsを設定。全backend表・queue・semaphore・IRT・contextを共通の幅検証付きallocatorへ統合。大ページはPS bitでleaf判定する。 |

## 製品修理

- IQE等やwait/full-queue timeoutを永続的な不確定状態にする。後続syncの成功、遅延status writeの再利用、RWBF不要経路からの成功への昇格を防ぐ。faultイベントはmaskし、原因は消さない。
- 既存firmware queueはTE/IRE停止確認後、emptyとproducer状態の再確認、既存direct-mapの物理一致、物理幅・整列確認を経てfree tail slotだけへwaitを置く。128/256bit、QS全値を扱い、元queueを再配置・解放しない。自分のqueueもwait statusだけでなくhead==tailを確認する。
- PMENのPRS停止失敗を成功として返していた経路を修正。GCMD timeout後もcleanup commandを書き続けない。
- resume/shutdownをbool化し、全unit確認を返す。既に不確定なunitは停止成功にせず、TEを落として非変換DMAへ変えない。表やqueue、semaphoreは保全する。PM/hibernateでのfail-stopと共通failure latchは主担当の責務。
- IR-only起動でCAP.PTなしの場合、存在しないidentity domainを参照していた問題を修正。変換用context/root entriesはtranslate時だけ構成する。hardwareへ公開前のidentity作成失敗時は解放する。

## 実行した軽量検証

1. Windows clangで実製品 `.c` をincludeしたMMIO模擬検証: 最終実行は失敗0、終了0。
   - IQEI 0～15、ICE/ITE、エラー非clear・非skip、late completion、queue full、QIES喪失、ring wrap、非coherent公開、WBF clear、拡張command/WBFS/PMEN停滞、register invalidation中のQIE拒否。
   - firmware QS 0～7 × 幅128/256bit、wrap、未mapping・幅不整合・TE残存の拒否。
   - PMEN失敗、CAP.PTなしIR-only、shutdown/resumeの成功・失敗、複数unit中一部失敗、MGAW/HAW/SAGAWの交差、PS leaf判定。
2. `clang --target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror -fsyntax-only kernel/src/dev/iommu_intel.c`: 終了0。
3. `git diff --check -- kernel/src/dev/iommu_intel.c tests/iommu_intel_hosttest.c`: 終了0。

模擬検証の再現コマンド:

```sh
clang -std=gnu11 -O1 -g -finput-charset=UTF-8 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Ikernel/src -Icommon -fsanitize=undefined -fsanitize-trap=all tests/iommu_intel_hosttest.c -o build/iommu_intel_hosttest.exe && ./build/iommu_intel_hosttest.exe
```

途中の失敗も残す。通常UBSan runtime方式はWindows配布runtimeの未解決symbolでリンク失敗し、runtime不要のtrap方式へ変更した。PM追加検証の初回4失敗はfixtureのGCMD=0/GSTS.QIES=1の不一致が原因で、直前要求をQIEへ整合させて修正した。製品の期待値・失敗条件は弱めていない。最終実行後の製品編集はfirmware所有権前提の説明コメントのみ。

## 未確認・統合時の注意

- 再起動なしのqueue回復は実装していない。不明なfetch/configuration失敗を推測修理せず、DMA/IRQ資源の寿命を保つ安全な失敗を選ぶ。
- firmware queueへの書込みには、boot/wakeの引継ぎ後にOSだけがproducerであることを要する。TE/IRE停止そのものがfirmware/SMMの所有権移譲を証明するわけではない。実機でのfirmware引継ぎは未確認。
- 仕様6.5.4/11.4.2によると、major version 1でDRD/DWDがないunitはread/write drainを保証しない。この古いhardwareでのDMA寿命保証は、主担当のPCI停止/reset・claim隔離方針との追加照合が必要。major version 2以降は必要なdrainが必須。
- 既存100msの応答監視予算を流用した。仕様に全commandの100ms完了保証があるとは扱わず、timeoutは必ず不確定・資源保全とする。新しい固定件数制限は追加していない。
- 共通allocator・PM・native ABIの統合、増分ビルド、最新媒体、実QEMU EDU、実機DMAは主担当の検証が必要。hosttest内のallocator/IRQ/時刻は模擬であり、それらの製品実装の証明ではない。

次の一手は、主担当による統合と別委任の第二検討。firmware queue所有権と旧versionのdrain保証を最優先で反証検討する。
