# AMD追加監査・製品修理の第一報告（2026-10-21）

## 範囲と保全

- 既存担当ID `amd` を継続する追加監査の第一検討。製品は `kernel/src/dev/iommu_amd.c`、検査は `tests/iommu_amd_hosttest.c` のみ編集した。本報告は新規で、旧第一／第二報告を上書きしていない。
- 開始時の差分は主担当の旧記録3ファイル。これら、他担当の差分、共有IOMMUヘッダー／共通層、PM、PMM、PCI、native ABI、生成媒体を編集していない。他OS・Linux・QEMU本体のソースを参照せず、外部コード／GPLv2-onlyを追加していない。
- 指定のスキルとJ-spaceの現行資料・意味マップを実読した。主担当の静止窓で、自身の `pulse --event resume --agent amd`、`repo view --agent amd`、`check --stage work --agent amd` を一度実行し、`ALLOW work` を確認。その後も一斉編集許可まで静止した。全tree gateの反復、独断のマップ同期、controller報告はしていない。本報告時点のcontroller提出は、共有ソース静止窓待ち。
- 重いビルド、QEMU、Hyper-V、通常data媒体、媒体コピー／変換を実行していない。新しい試験媒体やVMは作成していない。

## 一次仕様と監査指摘の分類

資料は既存抽出 `build/iommu-amd-spec-48882-3.10.txt`（AMD 48882-PUB、Rev 3.10、2025年2月）。そのSHA-256は `e94831daa95193c69599d4fe46b450d8d106411c027c76696c5c2c3d606c7de7`。今回は抽出済み一次本文を読み、元PDFの再抽出はしていない。

1. **拒否表の存在だけで割込み不能とする判定は過剰。** §2.1、Control.IommuEn（p216、抽出10997–11001行）は、無効時にupstream transactionを変換・remapしないと定義する。しかしIommuEnだけで全queue／任意engineの停止を代表できない。ControlのvIommuEn/vCmdEn（10766–10767行）、PPR/GAのenable、StatusのPPRLogRun/GALogRun（12846–12854行）、vIOMMU Status 0190h（§3.4.11、12334–12359行）も対象とした。
2. **拒否そのものを緩めない。** IVHD header不足、device entryの欠損・未終端range・内部端数、IVMD長32以外（Table110、15459–15477行）、実VA/PAやHATSの予約値、HATDis、必要PCI capability範囲外は従来どおり変換に用いない。IVRS/entryは詰めて格納しpaddingを許さない（Tables83/87、14624–14625・14765–14768行）。HATDisはhost変換不能であり、単なる性能上の推奨ではない。
3. **非zero segmentは実装未対応であって、hardware停止の証明ではない。** 通常PCI隔離は従来のsegment zeroに留める。一方、拒否後の状態監査はIVHDに記載された物理MMIOを使い、非zero segmentのunitも省かない。
4. **HATSは最大深さであって必要深さではない。** EFR.HATS（p221、11351–11357行）は4/5/6階層を上限として定義。DTE.Mode（p73、3242–3247行）は1〜6階層を定義し、3階層は39-bit、4階層は48-bit、5階層は57-bit、6階層は64-bitを覆う。現行共通層のidentity DMAが受け入れる `min(VA, PA, 52)` を覆う最小階層を選ぶのが妥当。
5. **MMIO baseの整列不足は追加の確定不具合。** Capability+04h（p200、10131–10138行）は最低16KiB整列を要求。旧製品は非zeroであれば不整列baseを受理した。反例を加え、0／16KiB不整列／範囲加算がoverflowするbaseを拒否した。これは任意quotaではなくregister配置とアドレス演算の条件である。
6. **S3失敗panicを除去する根拠はない。** 既存programのIommuEn、CmdBufRun/EventLogRun、ExEn/Allow、対応時PerfOptEnのreadbackは定義済みの意味あるbitに限定され、reserved全bitやRO値の一律一致を要求していない。未確認の復帰後にPCI DMA／割込みを再開させない主担当のpanicは意図した安全境界として維持する。

## 製品変更

### 拒否経路とhardware状態を分離

`iommu_amd_probe(bool *plain_safe)` は開始時にfalse、IVRSが存在しない場合だけ即trueでNULLを返す。変換に使えない場合は、隔離用parserと別責務の `ivrs_plain_safe()` がIVRS外周を読み、対象の全unitを独立して確認する。

- 外周の全IVHDを巡回し、非zero segmentや、最初のunitの幅拒否より後にあるunitも確認する。既にmapしたMMIOは再利用する。
- unit identity、完全なIVHD header、妥当なbase、MMIO map、意味を確認できるengineの停止をすべて満たす場合だけtrue。未知IVDB、未定義revision、外周端数、外周length破損、unitなし、header/base不明、map失敗はfalseを維持する。
- device entryの解釈失敗、IVMDのサイズ違反／overflow、cap範囲、幅、HATDisによる拒否は、外周unitの完全列挙まで失ったことを意味しない。全対象の停止を独立に確認できた場合に限りplain配送を許せる。
- この経路からControl、queue、pointer、tableへ一切書き込まない。拒否を理由とするshutdown、firmware queueの取得、失敗を成功へ変換する処理はない。実ホストモデルで拒否後のMMIO書込み0を検査した。

### 停止確認を共通化

追加の反証では、旧program/shutdownが対応済みPPRLogRun／GALogRun／vIommuRun／vCmdRunを無視し、停止未確認でもtable設定または停止成功へ進むことが分かった。`engines_off()` を同一モジュール内の共通責務として、拒否時観測、program、shutdownへ接続した。

- 拒否時は一度の読取り、program/shutdownは既存の時間根拠付きpollを使う。新しい期限・quotaは設けていない。
- 通常利用の `efr` はfirmware imageの優先を維持し、停止専用の `state_efr` に実MMIOとfirmware imageの双方の既存engineを保持する。機能の抑止情報で、実在queueの停止確認を省かない。
- 任意bitは対応するfeatureがある場合だけ確認する。未対応機能のreserved bit、ControlのCoherent等のpolicy bit、無関係なRO bitを0と要求しない。vIOMMU非対応時は0190hを読まない。この負の対照も検査した。
- これらの停止状態確認は実fabric全体のin-flight DMA drainの証明とは区別する。新しい失敗条件が実機で発生した場合、無視して再開せず原因を診断する必要がある。

### 最小ページ表深さ

全unitの定義済み実幅／有効要約とHATSの最小上限を求めた後、入力能力 `iova_bits` は保持し、現行identity mappingの受入幅を覆う最小depthを選ぶ。32-bit入力なら3階層、48-bitなら4階層、VA64/PA52でHATS6対応でも5階層となる。VA64/PA40なら4階層。HATSに由来する実入力上限48/57も保全する。共通層が将来非identity IOVAの受入範囲を変える場合は、この接続条件も見直す必要がある。

## 反例と軽量検証

1. 新APIへの機械的なsignature／out-param初期化だけを適合した修理前製品に、新しい拒否状態・最小depth・不整列baseの反例を入力した。**310判定、24失敗、終了1**。`build/iommu-amd-audit-before.log` を保全。これは完全な未変更旧ソースを現行共有headerでそのまま実行した結果ではない。
2. 上記修理後、任意engine停止漏れの新反例を追加した段階では **335判定、8失敗、終了1**。`build/iommu-amd-audit-stop-before.log` に保存。PPR、GA、仮想IOMMU、仮想commandの各program／shutdownが誤成功した。途中の主担当への通知で「337」と記載したが、正しくはこの実ログの335である。
3. 停止確認修理後の最終モデルは **347判定、失敗0、終了0**。`build/iommu-amd-audit-after.log`。基本／任意のenable/run、複数unitの後続on・map失敗、非zero segment、内部／外周破損の区別、予約値拒否、最小depth、firmware imageが実engineを隠さないこと、reserved bitの負の対照を含む。旧検査の期待値弱化・skipはしていない。

最終ホストcommandは指定の正規compilerで実行した。

```text
./tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -g -Wall -Wextra -Wno-format -Wno-unused-function -Wno-unused-parameter -ffunction-sections -fdata-sections -fno-unwind-tables -fno-asynchronous-unwind-tables -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon tests/iommu_amd_hosttest.c "-Wl,--gc-sections" -o build/iommu-amd-audit-hosttest.exe
```

- `build/iommu-amd-audit-compile.log` は警告なし。UBSan trapは発生していない。ホスト用format抑制は既存のLP64製品／LLP64ホスト差だけで、製品targetへ持ち込んでいない。
- Makefileのkernel flagsに対応する `--target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror ... -fsyntax-only kernel/src/dev/iommu_amd.c` は終了0。`build/iommu-amd-audit-syntax.log` は空。空logを成功文で埋めず、controllerのsource証拠にも指定しない。
- `git diff --check -- kernel/src/dev/iommu_amd.c tests/iommu_amd_hosttest.c` は終了0。
- 最終製品SHA-256: `bce1f984de0123c3ee8ef58221cd0fd49844d96222b4847c461fde8ef95ead50`。最終検査SHA-256: `2ef07d36fbd1281a8d41e1024024e20163dee41c4a30f549d9bc19d7c3621613`。

## 未確認と次の一手

主担当の最新共通 `iommu.c`／`iommu.h` とPM／hibernateをソースで再読した。両NULL時は双方のplain_safeだけで判定し、復帰／停止失敗をlatchしてPCI再開や生存table上書きを阻止する。これは本担当による共通経路実行試験の成功報告ではない。

本モデルは実DMA、実queue停止の時間、coherency、割込み、S3／hibernate、NVIDIA、性能を証明しない。実build・媒体一致・単一QEMU・実画面・Hyper-V／実機は未確認で、主担当の責務。次は共有ソース静止窓で本第一報告をcontrollerへ提出し、別委任の独立第二検討で拒否状態契約、任意engineの適用条件と実幅／depthを反証する。本担当の製品・検査編集はここで静止する。
