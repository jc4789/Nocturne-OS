# AMD追加監査の独立第二検討（2026-10-21）

## 範囲

既存担当ID `amd` の第二検討。ユーザーの追加監査原文、`docs/iommu-amd-audit-first-20261021.md`、意味マップ、現行製品・反例・一次仕様を実読した。第一報告と旧報告は上書きしていない。今回の編集は `kernel/src/dev/iommu_amd.c`、`tests/iommu_amd_hosttest.c`、本報告のみ。共有ABI・共通層・PM・PMM・PCI・媒体は編集していない。他OS本体参照、外部コード追加、新規委任は行っていない。

主担当の同期窓で、本人の `pulse --event resume --agent amd`、`repo view --agent amd`、`check --stage work --agent amd` を各一度実行し、終了0・`ALLOW work` を確認した。Intelの開始gate完了後に所有内の修理・反例一巡だけを実施した。独断のrepo sync、全tree gate反復、controller reportはしていない。既定のrepo viewが過大なinventoryを出力した点は主担当へ通知し、再実行していない。

## 確定反例と修理

### firmware imageが実PerfOptを隠す

一次資料は `build/iommu-amd-spec-48882-3.10.txt`（AMD 48882-PUB Rev 3.10）。§3.4.9、抽出12134–12167行は、016ChのPerfOptEnが統合GPU等のアクセスをIOMMUから迂回させ、実EFR.PerfOptSupがレジスター対応を示すと定義する。

第一修理は実EFRとfirmware imageの和を `state_efr` に保持したが、`program()` のPerfOpt解除条件だけは通常利用の `u->efr` のままだった。実EFR45=1・image45=0・PerfOptEn=1では、解除もreadbackも省いて新tableを公開し、成功を返せた。

第二修理は、この解除条件だけを `u->efr | u->state_efr` へ変更した。新table公開前に016Chを0へ書き、定義済みPerfOptEnだけをreadbackする。解除できなければtable書込み前に失敗する。通常利用のfeature選択はimage優先のままで、実featureによる機能追加へすり替えていない。

**旧positiveの訂正:** 第一報告の347判定成功は、このbypass停止漏れを含む全引継ぎ安全性の証明ではなかった。既存の明示PerfOpt対応例は通っていたが、実featureをimageが隠す組合せが欠けていた。

### MARCのControl解除readback

これは主担当から提示された境界を本人が独立に仕様・製品で確認したもの。§2.9、抽出9533–9553行はMARC apertureによる変換迂回とMarcEn=0時の通常IOMMU経路を定義する。Control.MarcEnはbit40（10789–10791行）、対応はEFR.MarcSup[43:42]の01/10（11271–11276行）。

旧 `engines_off()` はMarcEnを確認しなかった。対応unitでControl=0書込み後もMarcEnが残るreadback失敗を模擬すると、`program()` はtableを公開し、`shutdown()` も成功した。停止専用の実feature/imageの和にMarcSupがある場合だけ、既存のControl停止maskへMarcEnを加えた。新しい期限・quota・別停止処理は設けていない。MARCの個別apertureレジスターへ触れず、通常Control=0で解除できる場合は従来どおり成功する。

正常な実機でMarcEnが残ることは観測していない。本反例は、readback失敗時に成功を返す欠落を検査したものである。

## 同一反例の前後比較と対照

指定の `tools/msys64/ucrt64/bin/clang.exe` で実製品 `.c` を取り込む既存ホストモデルを使用した。共通条件は `-O1 -g -Wall -Wextra`、section GC、`-fno-unwind-tables -fno-asynchronous-unwind-tables`、`-fsanitize=undefined -fsanitize-trap=all`。既存のホストformat等の抑制は製品targetへ持ち込んでいない。

| 実行 | 結果 | 保存先 |
|---|---|---|
| 第一修理の製品＋新反例、製品未変更 | 405判定・13失敗・終了1 | `build/iommu-amd-audit-second-before.log` |
| 第二修理＋同一反例と全既存モデル | 409判定・0失敗・終了0、UBtrapなし | `build/iommu-amd-audit-second-after.log` |
| 正規kernel target設定、`-Werror -fsyntax-only` | 終了0 | `build/iommu-amd-audit-second-syntax.log` |
| 所有製品・試験の `git diff --check` | 終了0 | コマンド結果 |

修理前13失敗の内訳はPerfOptが5、MARCが8。修理後の判定数増加4は、以前省略されていたPerfOpt用 `rd32` / `wr32` 内の既存CHECKが実行されたためで、反例の除外や期待値弱化ではない。前後のcompile logは同名の `-before-compile.log` / `-after-compile.log` に保存し、両方とも空で警告なし。構文logも空で、空logをsource証拠にはしない。

対照は次のとおり。

- 実EFRのみ、imageのみ、双方にあるPerfOptを区別し、通常 `efr` がimageのままであることを確認。実EFRのみの解除不能例ではtable書込み0・IommuEn無効を確認した。
- MARCの定義済み両encodingを、実EFRのみ／imageのみで検査。残存MarcEnではtable書込み0・shutdown false、通常clearではprogram／shutdown成功を確認した。
- 実EFR・imageとも未対応なら、reservedのControl bit40や016Ch bit13を0と要求せず、016Chを製品から読書きしない。
- 隔離用の幅拒否があっても、IommuEn=0・PerfOptEn=1ならupstreamはplainである（IommuEnの定義、10997–11001行）。`plain_safe` はtrue、拒否後MMIO書込み0、016Chへの読書き0を維持する。変換を使わない配送と新しい翻訳開始を混同しない。
- 第一修理の複数unit／未対応segment／外周破損／revision／PPR・GA・virtual engine停止／最小depth等の既存反例は、そのまま同じ一巡に含まれる。削除・skipはしていない。

## 独立再読した他の境界

HATSは最大深さで、DTE.Modeの1–6階層と現行identity受入幅 `min(VA, PA, 52)` の最小depthは整合している。VA32は3、VA48は4、VA64/PA52でHATS6でも5階層で足りる。IOMMU共通層のindex幅とDMA上限も追跡し、この範囲で新しい反例は確認していない。

PMMの `pmm_alloc_below()` とsyscall／IOMMU count1呼出しを独立に再読した。上限はexclusiveで全ページを収め、ゼロwordでは剰余式を実行せず、末尾の部分wordは範囲外bitを除く。hintは制限内wordへ折り返され、非zero availableだけへctzを使う。`limit / PAGE_SIZE` によりpagesとwordsの加算範囲も収まる。既存PMM12判定／共通89判定のログと、主担当のAMD/Intel QEMUの256MiB・65,536ページのログを読んだが、本人による再実行ではない。hostの新計測0.000000秒を速度ゼロや実GPU性能の証明とは解釈しない。

S3復帰未確認時のfail-stopは維持した。未確認の変換下でPCI DMA／割込みを再開するログ-onlyへの変更はしていない。

## 最終状態と限界

- 製品SHA-256: `03fa27df804c2085a3b4dbb6f9b3e768d3ed7f857c4fffe0eaa5d97f0fa5aed6`
- 試験SHA-256: `a488005bc2d37c4490302361f5d45d27c68daedcefd55fe2f100c60211705220`
- 一次抽出SHA-256: `e94831daa95193c69599d4fe46b450d8d106411c027c76696c5c2c3d606c7de7`

製品・試験・host実行は静止済み。本報告作成は主担当の明示許可後に行った。controller round2提出は最終共有source窓まで保留する。

本人は重いbuild、QEMU、画像撮影、実キー入力、S3、Hyper-V、NVIDIAを実行していない。第一受入の実QEMU結果を第二修理後の媒体・動作証拠へ流用しない。実queueの時間・fabric drain・coherency・実GPU／scanout・nvrm公平性は本ホストモデルでは証明できない。次は主担当が最新製品を増分build・媒体更新し、単一QEMUの受入を更新してから、最終共有source窓で本人のround2を一度提出する。
