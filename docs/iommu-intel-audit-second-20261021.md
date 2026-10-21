# Intel VT-d 追加監査・独立第二報告（2026-10-21）

## 範囲と保全

- 別の委任による第二検討。既存担当ID `intel` を継続し、旧first/second、live、final各報告と追加第一報告、意味map、一次抽出、現ソースを実読した。追加第一報告はSHA-256 `a8a450c7068f1c0f64b834afcf3be25e544f4bfea6440632b9054d1ab087da95` のまま保全した。
- 主担当の通知まで読取り専用。その後、自身のpulse resume/read、repo view、work checkを各一度実施し、終了0・`ALLOW work` を通知した。所有内の `kernel/src/dev/iommu_intel.c` と `tests/iommu_intel_hosttest.c` だけを修理した。共有C/header、PMM、PCI、ABI、第一報告は編集していない。
- 重いbuild、QEMU、媒体、Hyper-V、通常data、追加委任は実行していない。他OS本体・Linux・QEMU実装を参照せず、GPLv2-only素材・人工上限・ABI迂回を追加していない。本書作成時点でcontrollerの第二報告は未提出で、主担当の最終同期窓を待つ。

## 第一修理の最弱前提を再検討

資料はIntel VT-d Rev.5.20の既存抽出 `build/iommu-intel-spec-20261021.txt`、SHA-256 `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。今回は原PDFを新たに開いていない。

- **IQA移設：** §11.4.9.3が変更を禁じる条件はqueueが空でない場合。§6.5.2の空判定、§6.5.2.9のwait、§11.2の64bitアクセス規則を照合した。空を再確認し、QS/DWとoffsetを維持してIQAを書き、readback後にtailを公開する現経路への追加反例は得られなかった。ただし、これは仕様解釈とソースの確認であり、実機のlive移設成功証明ではない。
- **所有権と寿命：** 未知元queueへのCPU読書きはなく、完全なPMM RAM一覧の全範囲と非交差であることを全unit・metadata確保前に確認する。USABLEだけでなくbootloader/moduleも除外する。既知OS queueはIQAの厳密一致を要求する。未公開のvalidation/確保失敗と、IQA公開後の不一致・timeout・遅延status writeを区別し、後者のfailedと記憶域保持を解除しないことを追った。
- **失敗境界：** 新statusとhead、IQA/IQT/QIESの確認、全unit quiet後のdisable、CCMD等の直列化、major1のdrain能力拒否、既存latchは維持されている。firmware/SMMが唯一producerをOSへ引き渡した保証や、元descriptorが参照する記憶域の初期寿命は依然未確認。
- **拒否時の観測：** scope等の不支持を示す `parse_lost` と、全DRHDの状態を証明できない `units_incomplete` は別。未知構造は§8.2どおり起動解析ではskipするが、plain安全の根拠にしない。PMMの有限上限確保も実読し、実ページ上限・末尾wordのmask・hint折返しを確認した。自分で性能試験を再実行したとはしない。

## 新反例と製品修理

### DRHDの誤った観測起点

§8.3はregister baseに報告サイズ全体への整列を要求する。旧処理は非zeroだけを確認し、baseが8byteずれたIR-only稼働unitを、別registerの誤読から `NULL + plain_safe=true` と判断した。初期説明のTE例にはRTPSの余分な仮定があったため、実行反例は正しいIR-only状態（IRE/IRTPSあり、RTPSなし）へ訂正した。この反例でplain誤認を実測した。

修理は既存parser/probeへ統合した。

- Size上位4bitを予約bitとして拒否。下位4bitの定義値N=0〜15は狭めず、`uint64_t` の報告サイズ、全域整列、base+size非overflowをmapping前に検証する。Size=31をN=31としてshiftしない。
- 報告apertureをそのまま一度mapする。FRO/NFRとECAP.IROの末尾がその中に収まらなければ拒否し、従来の範囲外への拡張mappingを撤去した。最小4KiBにはFSTSからECRSPまでの使用する固定offsetが収まる。
- 不正なDRHDは全unit観測済みの証拠にしない。正しい整列、FRO/IRO末端ちょうど、報告2page、最大N15は受理する対照を残した。N15は論理PAをMMIOへ対応させるモデルで、128MiBを実確保していない。

### RMRRを空範囲として失う境界

主担当の追加指摘を実読・再現した。旧処理は短いtype1や逆範囲を拒否せず、`limit+1` または末尾丸めのwrapを共通層へ渡し、必須予約を空範囲として失い得た。

§8.4の固定header＋一つ以上のscope、baseとsizeの4KiB整列、limit>baseを根拠に、短縮・scope欠落・逆/同値・非整列・exclusive endのwrapを `parse_lost` とする。丸めによる拡張を撤去し、正規のbase/endをそのまま渡す。全DRHD列挙の完全性とは混同せず、全unit停止なら `NULL + plain_safe=true` を維持し、稼働中ならfalseとする。正常な52bit物理上端の最終pageと広域予約も受理した。

## 検証と最終状態

正規 `tools/msys64/ucrt64/bin/clang.exe`、UBSan trap、section GC、unwind無効の製品C直接include試験を使用した。

| 検証 | 結果・保存先 |
|---|---|
| DRHD修理前、13条件と既存検査 | 17失敗・終了1。`build/iommu-intel-audit-second-before.log` |
| DRHD修理後、同条件 | 失敗0・終了0。`build/iommu-intel-audit-second-after.log` |
| RMRR修理前、8条件×停止/稼働 | 24失敗・終了1。`build/iommu-intel-audit-second-rmrr-before.log` |
| 最終製品 | DRHD13条件、RMRR元条件＋仕様境界の12条件×停止/稼働、既存QI/late write/drain/IRQ/PM/probe検査すべて失敗0・終了0。`build/iommu-intel-audit-second-rmrr-after.log` |
| 最終kernel target | 既存kernel flags＋`-Werror -fsyntax-only`、終了0・診断なし。`build/iommu-intel-audit-second-rmrr-syntax.log`。対象diff checkも終了0 |

RMRRのscope欠落・同値・基点/末端非整列は最終検査で追加した境界であり、修理前24失敗の内訳へ遡って含めない。元反例・期待値は弱めていない。既存fixtureのMMIO配列は仕様どおり実ページ整列へ訂正し、範囲外の第二pageをモデル内で明示した。最初の未引用リンク引数はホストの構文エラーでコンパイラー未実行となり、引用を訂正して検証した。

| 対象 | SHA-256 |
|---|---|
| `kernel/src/dev/iommu_intel.c` | `7618f043fb7b11a0ac8b76efcfaaf8d241e8944da3fe7759d2827a9505ce5908` |
| `tests/iommu_intel_hosttest.c` | `8f79fe6bf8d152267cea237c224fa4237aa38a0e7d3d2802d5412b9519a0d1ff` |

製品/testsは静止した。旧QEMUログを今回のparser修理後の受入へ流用しない。実画面・実入力・増分媒体・単一QEMU受入は主担当が更新する。本担当は画像を開いたり実操作したりしていない。firmware active QIの実機移設、SMM引継ぎ、実drain、EIME/IRREQ、高位物理RAM、S3/hibernate、Hyper-V、実GPU回復とnvrm公平性は未確認または未解決のまま保持する。
