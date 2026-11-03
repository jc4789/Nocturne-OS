# Intel VT-d 追加監査・第一報告（2026-10-21）

## 範囲と実施の区別

- 既存担当ID `intel` を継続。開始時差分は主担当の旧記録3ファイルだけで、保全した。旧first/second、live-first/live-second/live-handoff、final-first/final-secondは上書きしていない。
- 指定のNocturne/J-spaceスキル、既存map、旧報告、現ソースと一次抽出を読み、指定の直列窓で自身のpulse resume/read、repo view、work checkを一度実施。終了0・`ALLOW work`を主担当へ通知し、一斉編集許可まで製品・tests・文書・controllerを変更しなかった。
- 製品変更は `kernel/src/dev/iommu_intel.c`、模擬検査は `tests/iommu_intel_hosttest.c`。共有h/c、PMM、PM、PCI、ABIの変更は主担当・別担当の所有であり上書きしていない。本報告以外の文書も未変更。追加委任はしていない。
- 重いbuild、QEMU、媒体、Hyper-V、通常dataは操作していない。他OS本体・Linux・QEMU実装を参照せず、GPLv2-only素材、native ABI迂回、根拠のない上限を追加していない。

## 一次仕様と所有権の反例

一次資料は指定済みIntel VT-d Rev.5.20の抽出 `build/iommu-intel-spec-20261021.txt`。SHA-256は `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。今回は原PDFを新たに開いたとは主張しない。

| 根拠 | 今回の判断 |
|---|---|
| §6.5.2、抽出6740–6781行 | head/tail一致だけではQI停止不可。最後のwait完了も必要。QIESが立つ間のregister invalidationによる迂回も不可。 |
| §11.4.9.3、17168–17199行 | IQA変更は禁止条件が「queue not empty」。空を確認後、QS/DW/offsetを維持してOS所有領域へIQAを移し、その新領域へ終端waitを置く。これは§6.5.2と組み合わせた仕様解釈であり、実機での移設成功証明ではない。 |
| §6.5.2.9、7637–7668行 | SWのcoherent DWORD書込みとfenceを実際に待つ。専用status領域を持ち、遅延書込み先を再利用しない。 |
| §6.5.2.11、7715–7768行 | IQEは原因訂正前のclear不可。ICEはqueueが進んでも不正応答、ITEはwaitをabortし後続の実行保証を失う。汎用clear/skipによる回復根拠は得られない。 |
| §6.5.1、6617–6636行、§11.4.4.1、15354–15368行 | QIES=0のregister-modeは、既存command完了を待った後にTEを正規commandで停止できる。未知QIをregister-modeで強制停止する抜道にはしない。 |
| §5.1.4、4623–4628行 | IRES=0でもIRREQ=1ならplain割込みは遮断される。enable bitがすべて0というだけではplain-safeとしない。 |
| §8.2、9487–9515行 | 未知の構造は長さに従ってskipする前方互換性を維持。ただし未知状態までplain-safeと証明しない。 |

旧 `queue_quiet()` は未知firmware queueの空きslotへ16/32バイトを書き込んだ。direct-mapのVA/PA一致は、当該ページをPMMや別の所有者が再利用していない証明ではない。旧報告の「初期queue寿命は未確認」は確定した安全性修理の必要性を過小評価していた。

PMM/init順を実読した。PMMはUSABLEだけを初期freeにし、BOOTLOADER_RECLAIMABLEを一括解放してはいない。しかしPMM bitmap設置、VMM・heap・initrd等の割当はIOMMU probeより前であり、moduleの個別解放もある。したがって現在のused bitや後付けclaimから、元queue内容が保持されたとは推定しない。

## 製品修理

1. **未知元queueのCPU読書きを撤去。** `queue_address_safe()` は完全な `pmm_ram_ranges()` の全範囲との非交差を要求する。USABLE、bootloader reclaimable、kernel/modulesのいずれとの交差も、途中再利用を否定できないため拒否する。`pmm_ram_complete()` がfalseでも拒否する。これは件数quotaではなく所有権を証明できない場合の拒否である。既知の保持済みOS queueはIQAの厳密一致で別扱いにする。
2. **全unitをtranslation/queue metadataの割当・zero前に確認。** `intel_start()` と `intel_shutdown()` の事前確認は、一つでも未知RAM交差があれば後続割当・移設・保護解除に進まない。既にOSRAMとして書換えられたかもしれない元内容をhardwareに正当なqueueとして排出させる前提も置かない。
3. **予約済みfirmware領域からOS所有queueへ移設。** 元queueをCPUで読まず、hardwareのemptyを待つ。元QS/DWに対応するqueue全域と、その直後の専用status一頁を正規allocatorで確保。pointer/enable/errorを再確認後、OS領域へfenced waitを置き、IQAを切替えてtailを公開する。新status値とhead、さらにIQA/IQT/QIESを確認した後だけquietとする。
4. **寿命を分離。** 全unitがquietかつ停止を確認したときだけ移設用領域を解放する。公開後timeout・queue error・pointer不一致では保持し、late status writeを次の成功に流用しない。未公開の割当失敗・元queueの未排出はOS status writeを発生させていないため、それだけで永続failedにせず、安全な再試行を許す。IQE/ICE/ITEやcommand timeoutの既存latchは解除しない。
5. **不要な起動不能を避ける。** 元queueがOSRAM外と証明できれば、通常semaphore未確保でもprivate statusによってshutdownできる。QIが既にoffのregister-mode停止は追加queue割当やwaitを要求しない。S3後も保持済みOS queueの厳密一致なら再確認して使用できる。単なる旧queue_onフラグは未知IQAの使用許可にしない。
6. **拒否理由と状態観測を分離。** `iommu_intel_probe(bool *plain_safe)` は初期false、DMAR不在だけ無条件true。全DRHDが確定するscope破損／非zero segmentでも、全unitのMMIOを読んで独立にoffを確認できればNULL+trueを返す。外周/DRHD破損・未列挙・未知構造・読取不能・IRREQはfalse。GSTSのTE/IRE/QIE/WBF、未完了register/enhanced command、PMENを確認し、拒否を理由とする停止command、mask、fault clearは発行しない。PMENはplain IRQ自体ではなくhibernateも含む厳格off契約の保守判定として含める。
7. 未公開tableの割当失敗時にfreeしたroot/pass/queueの記録を0へ戻し、解放済みpointerを後のOS所有証明へ使わせない。

## 実行した軽量検証

- 正規 `tools/msys64/ucrt64/bin/clang.exe`、section GC、unwind無効、UBSan trapで製品C直接includeのMMIO模擬試験を実行。最終は**失敗0・終了0**。保存先は `build/iommu-intel-audit-hosttest-20261021.log`。
- QS全encoding×128/256bit幅、wrap、元queue全byte不変、新IQAのQS/DW維持、private status完了と成功後だけの回収を確認。旧アドレスを実際にはdereferenceできない入力でも、空から新領域へ移して停止できる対照を含む。
- OSRAMの一頁との交差、不完全map、後方unitの未知RAM交差では、translation/queue allocator呼出しが増えず、保護を落とさないことを確認。
- head==tailでもstatus未到着なら失敗し、private queue/statusを保持する。late write到着後もshutdown/resumeを成功へ戻さず、freeを行わない。未公開allocation failureの安全な再試行、既知OS queueの厳密一致、register-mode停止も確認。
- probeでは既停止の未対応QIとactive TE/IRE/QIE/WBF、IRREQ、読取不能、外周/DRHD破損、scope破損、非zero segment、未知構造、後方unitのTE/IRREQ/読取不能を確認。拒否前後の全register snapshot不変を確認した。
- 既存のIQE全reason、ICE/ITE、late completion、full queue、drain能力、CFI/EIME、失敗後PM、IR-only、address幅の検査を残した。
- 途中の最初の試行は4失敗。追加fixtureがGCMD=0/GSTS.QIES=1という不一致でmodelが自動停止していたため、直前要求をQIEへ揃えた。元の安全性期待値を弱めず訂正後0。旧「元firmware slotへwaitを書け」という期待は、今回修理する禁止動作なので「元全域不変＋新所有領域でwait完了」へ明示訂正した。sems未確保時の停止成功は、新private status経路の完了証拠を必須にした機能変更である。
- Makefileの既存kernel flagsに `-Werror -fsyntax-only` を加えたfreestanding target検査は終了0。`build/iommu-intel-audit-syntax-20261021.log` は診断なし。対象 `git diff --check` も終了0。

ホスト再現コマンド：

```sh
./tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -g -finput-charset=UTF-8 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon tests/iommu_intel_hosttest.c '-Wl,--gc-sections' -o build/iommu_intel_audit_hosttest.exe && ./build/iommu_intel_audit_hosttest.exe
```

## スナップショット・残件

| 対象 | SHA-256 |
|---|---|
| `kernel/src/dev/iommu_intel.c` | `23a707bb4600631a4bd937ffba3c5995d927bcf2bb754d5935fc0607f90d35ae` |
| `tests/iommu_intel_hosttest.c` | `cea9a74004c8d5557369bfbdadbe4ec6f2b518125bf17a0917f7e8f8f885d7a3` |

実機のIQA live移設、firmware/SMMのsole-producer引継ぎ、DMA/IRQ drain、S3/hibernate、Hyper-V、NVIDIAは未確認。今回の模擬結果を実機成功へ昇格しない。OSRAM内の未知firmware queueは保全を証明できないため未対応であり、将来対応するなら最初のPMM書込み以前の所有権保存が必要になる。queue errorの汎用回復も未実装であり、隔離ページ・IR entryを解放する成功偽装はしていない。

次は主担当の差分確認・増分build/媒体・単一QEMU受入と、別の委任による独立第二検討。controller第一報告は主担当のsource静止窓で提出する。主担当が所有する共有sourceの後続変更を、旧snapshotの検証済み結果へ流用しない。
