# AMD実観測後の幅判定修理・独立第二検討（2026-10-21）

## 範囲・保全・結論

- 既存担当ID `amd` を継続した、新しいAMD live修理cycleの独立第二検討。旧 `iommu-amd-first-20261021.md`、旧 `iommu-amd-second-20261021.md`、新 `iommu-amd-live-first-20261021.md` を全文読み、一次資料と現行製品・検査を照合した。
- 今回の追加製品修理は不要と判断し、製品・検査は変更していない。変更するのは本報告だけ。開始時から存在する `AGENTS.md` と他担当の差分、native ABI、生成媒体を保全した。
- 指定の静止期間中は読取りとAMD独立host検査だけを進め、製品/tests/docs/controllerとwork gateを保留した。主担当の最終静止・map同期通知後、自身のresume/pulse、repo view、work gateを一度実行し、`ALLOW work` を得て本報告を保存した。大きいinventoryは `build/iommu-amd-live-second-repo-view.log` に退避し、map本文と関連実装を読んだ。独断のmap同期、重いbuild、VM操作はしていない。
- round 2の最初の提出では、正常時に空だったcompile/syntax logまでsourceに指定したため、controllerが `Evidence must not be empty` と拒否した。空logへ成功文を後書きせず、製品・検査・実行結果などの非空依存に指定を訂正した。work gateやhost検査を再実行していない。
- 本検討の結論は「不定義の共通要約を上限にしない場合も、取得できる各unit実能力の予約値拒否と、取得できないunitの保守的な拒否が残る」。仕様がVA0を定義している、実機受入が完了した、という結論ではない。

## 1. 元一次PDFからの独立照合

ユーザー許可の `D:/Programes/OS DEV INFO/48882-3.10.pdf`（AMD 48882-PUB、Rev 3.10、2025年2月）を、既存Calibreの `pdftotext.exe -layout -enc UTF-8` で独立再抽出した。他OS、Linux、QEMU本体、NVIDIA `uvm_common.c` のソースは参照していない。

- 元PDFのSHA-256: `cd7559334b3f46a72d56e9f2fac786fcbf5d6a2c41e7770a46fcacc080bc8a12`。
- 再抽出先 `build/iommu-amd-live-second-spec.txt` と既存 `build/iommu-amd-spec-48882-3.10.txt` は、双方ともSHA-256 `e94831daa95193c69599d4fe46b450d8d106411c027c76696c5c2c3d606c7de7` で一致した。その実本文を読んだ。
- §3.2、Capability+10h（pp203–204、抽出本文10292–10305行）はVA=32/40/48/64、PA=40/48/52のみを定義し、それ以外をreservedとする。Table84/86（pp286–287、14672–14715行）のIVinfoは全unit共通情報で、幅はCapabilityの同じ定義を参照する。VA0は正規値ではない。
- EFR.HATS（p221、11351–11356行）は4/5/6階層と予約符号を区別する。HATDis（Table96/100、15060–15068・15168–15176行）はhost変換自体の不支持で、幅の欠損とは別に拒否すべき情報である。

**規範と実装方針を区別する。** 「壊れたIVinfoを無視せよ」という仕様上の命令は得られなかった。現行修理は、信頼できない幅fieldを能力根拠に使わず、取得できる全unitの定義済み実幅から制約を再構成する汎用の互換性方針である。この方針を、ファームウェア要約の仕様適合性の証明とは扱わない。

## 2. 幅・複数unit・欠損functionの反証

現行 `iommu_amd_probe()` と、製品Cを直接includeする最新 `test_probe_widths()` を実読した。

1. **実reserved能力を要約で補うという仮説は不成立。** `va_width_valid()` / `pa_width_valid()` は上記定義値だけを受理する。functionがあれば必ずそのCapability+10hを読み、VA0、PA0、PA41、予約VAは有効要約があっても拒否する。要約の緩和がこの判定を飛ばす経路はない。
2. **先頭unitだけで後続を推測するという仮説も不成立。** 全列挙unitを巡回し、各実幅の検証後だけminへ反映する。二functionの64/52と40/40は40/40に縮み、後続実VAが予約値なら全probeを拒否する。先頭functionだけが見つかり、後続functionも有効要約もない入力も拒否する。
3. **欠損functionを最大幅で補う処理はない。** functionを取得できないunitでは既存のIVinfo fallbackを使うが、VA/PAの両fieldが定義済みの場合だけ通る。片方でも0・予約値なら拒否する。したがって「全unitの実registerを直接取得した」とは言えないが、「要約が不定義でunitも不明なのに推測で成功する」処理ではない。
4. **正規の小さい上限は維持される。** 有効要約VA32/PA40と実VA64/PA52は32/40になる。有効VA40・予約PA41の要約ならVA40だけを上限に使い、PAは実値から得る。実側が小さければ実値が優先する。初期64/52は仕様の最大定義値であり、未知unitの能力を保証する値ではない。
5. **別の不支持情報は緩和されない。** HATDisと予約HATSの拒否、4/5/6階層のmin、階層が表現できる最終VA上限、firmware EFR優先、coverage/alias、確保後の物理幅確認は残っている。特定エミュレータ名、vendor/device ID、観測値の組合せへの専用分岐はない。

元の実観測値Cap+10h=`0x00182840`（VA48/PA40）、IVinfo=`0x00002801`（VA0/PA40）に対し、VA0を正規化するのではなく、実VA48と有効要約PA40を使うという修理内容はこの境界と整合する。実QEMUの当初観測者は主担当であり、本担当ではない。

## 3. 本担当が独立実行した確認

正規MinGWで次を一度独立実行した。既存の修理前・修理後ログは上書きしていない。

```text
./tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -g -Wall -Wextra -Wno-format -Wno-unused-function -Wno-unused-parameter -ffunction-sections -fdata-sections -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon tests/iommu_amd_hosttest.c "-Wl,--gc-sections" -o build/iommu-amd-live-second-hosttest.exe
./build/iommu-amd-live-second-hosttest.exe
```

- compile終了0・警告なし。実行は **113判定、失敗0、終了0**、UBtrapなし。ログは `build/iommu-amd-live-second-compile.log` と `build/iommu-amd-live-second-hosttest.log`。
- Makefileのkernel flagsに対応する `clang --target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror ... -fsyntax-only kernel/src/dev/iommu_amd.c` も終了0。ログは `build/iommu-amd-live-second-syntax.log`。hostだけのLP64/LLP64 format警告抑制を製品検査へ持ち込んでいない。
- 独立実行時と最終静止時のAMD製品SHA-256は `fc58992603919bc721906c4ed00c493c178ee1641719606ef69f95e77bcd778a`、AMD testは `b56054d1831afa911d8394abf095810c1a3986407d6e2b3337cf5cfc7c5dcbe7` で変わっていない。
- この独立実行の後には共有header・IRQ/PM・PCIの更新があった。したがって本担当が最終依存全体を再実行したという報告にはしない。最新依存は次節のソース読取りで再確認し、主担当が別途実行した最終AMD113/0とは区別する。指示に従い、無変更host検査を反復していない。
- 旧 `build/iommu-amd-live-before.log` の110判定中5失敗と、第一修理後ログの113判定失敗0を読んだ。これらは第一担当の歴史的証拠であり、本担当が旧製品へ差し戻して再現した結果ではない。
- `git diff --check -- kernel/src/dev/iommu_amd.c tests/iommu_amd_hosttest.c` は終了0。今回は製品/testの追加変更なし。
- 準備操作の失敗も区別する。空白を含むPDF変換ツールの最初の起動指定は実行前に失敗し、既存実行ファイルの短いパスへ訂正した。再抽出後の `cmp.exe` はこの環境に存在せず、既存 `sha256sum.exe` で一致を確認した。これらを仕様不一致や成功した検査に数えていない。

113はhostモデル内の判定数で、113件の独立実機検査ではない。モデルはPCI/MMIO/メモリー所有境界を置換しており、実DMA、fabric drain、cache coherency、実割込み、性能を証明しない。

## 4. 最終共有ソースとの接続

主担当の最終同期通知後、`iommu.h/c`、`pm.c`、`arch/ioapic.h/c`、`arch/cpu.h`、`arch/idt.c` の関連実装を自分で読んだ。PCI側の最新 `pci.h`、`pciuser.h/c` と `pci.c` のMSI/FLR境界も追跡した。本担当はこれらを編集していない。

- 初期IOMMU移行はPIC mask保存・全maskとIOAPIC maskの後にbackendをstartし、共通のbool `ioapic_remap()` で失敗を伝播する。全停止を確認できない場合、またはIRREQでplain配信が成立しない場合はfailure latchを保持する。AMDの新しい幅選択がこのlatchを解除する処理はない。
- 新 `interrupt_remapping_required` はAMD initializerではfalseで、停止中のAMD-Viもplain割込みを通す既存のbackend契約と整合する。IntelのIRREQ要件をAMDに機械的適用していない。
- IOAPICは移行中に失敗した既存要求をrollback用に保持し、新規route失敗はmask状態にする。`restore_failed` 後の新しいMSI/IOAPIC要求も拒否する。これは主担当の共通修理であり、AMD hostモデルの113判定には含めない。
- S3側はPIC/IOAPICをmaskし、wake後もmaskしたまま `iommu_resume()` を確認してから経路を戻す。resume失敗時はPCI DMA再開へ進まない。実際に寝なかった経路は `pic_restore_mask()` だけを呼び、`pic_init()` によるpending edge消失を避ける。実S3・中止sleepのハードウェア試験を本担当が行ったわけではない。
- 最新PCI release/reclaimはgroup全memberの残留状態を扱い、保留FLRの残waitを再参加後のBME/MSIアクセスより先に置く。IOMMU block確認とデバイスresetは異なる保証であり、FLR成功だけでDMA quarantineを解くという解釈はしていない。このPCI修理の独立受入はPCI担当と主担当の責務である。
- 主担当通知の共通84/0、実PIC/IOAPIC込みPM51/0、最終AMD113/0・Intel失敗0・PCI失敗0は、本担当による追加実行ではない。

旧第一報告・旧第二報告・新第一報告の内容は保存した。新第一報告のhashは `78124e17ccb4b63efcfbd253167898b76ede0573b81d76a105e5624bdebf4447`。その共有ソース依存が後続修理でstaleになった履歴は残し、本第二報告を最新読取りのsource依存で提出する。

## 5. 実QEMU結果の位置づけと未確認

主担当から、修理後AMD QEMUの終了、同条件Intel VM一台への切替、AMD画面 `amd-final-now.png` / `amd-final-input.png` の主担当自身による目視とPS/2 `help` 入力確認の報告を受けた。本担当はVMを操作・撮影・目視していない。

本担当が読んだ `build/iommu-repair-20261021/amd-final-serial.log` には、AMD-Vi起動、`isolated by an IOMMU: yes`、`pcitest: 0 failed`、`nvrmtest: 0 failed`、二機能virtioの `pcitest flr: 0 failed`、`nativeabitest: 225 checks, 0 failed`、`nvrm_servetest: 0 failed` がある。本文ログの確認と、主担当の実画面・実入力の観測は別の証拠として扱う。

ログを例外なしとは報告しない。pcitestの二つのpage faultは、検査ソースのrevoke後アクセスを子でfaultさせる負例と対応する。NVRMはEDUを実NVIDIAとして初期化できない旨も記録している。`nvrm_servetest` の127 clients/33 thread-limit refusalsは、同一主体のidle接続によるthread占有が解消した証明ではない。この残件、実NVIDIAのreset/scanout回復、実機DMA、firmware引継ぎ、S3/hibernate、性能は未確認のまま保持する。

元のAMD event再開・PerfOpt指摘も一律成立にはしない。一次仕様§2.5.1（p141）の停止確認→EventLogEn解除→pointer→Overflow W1C→再有効化、§3.4.9（p237）の016Ch/32-bit bit13/EFR bit45は現行処理と整合することを再照合した。そこから実機event処理やGPU迂回防止全体の受入完了までは主張しない。

**次の一手:** 主担当が本第二報告と依存ソースを独立受入し、最新媒体・Intel側の結果と既知の未達を統合する。本担当はround 2提出後に編集を終了する。QEMU成功は実機・Hyper-V受入の代わりではない。
