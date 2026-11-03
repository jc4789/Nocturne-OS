# Intel VT-d 実検証回帰の第二報告（2026-10-21）

## 範囲と履歴

- 担当ID `intel` を継続した別委任の第二検討。旧 `iommu-intel-first/second-20261021.md`、新第一報告 `iommu-intel-live-first-20261021.md` と `iommu-intel-live-handoff-20261021.md` を全文再読し、上書きしていない。
- 新第一報告のSHA-256は `f2c0a0681542f0c0d37916ad1e1be29224f77b965e79c26a51b1ac215f3e6cfa`、引継ぎは `3827c642074baca6a3b9f43ee10d2454cd8677b25f205a568d39bd34cc38b507`。第一報告が依存した製品・試験の旧hashは履歴に残す。旧結果を今回の最終ソースの検証へ流用しない。
- 開始時の差分、AGENTS.md、指定スキル、意味mapを確認。主担当の同期通知までは読取りに限定した。その後、自身の `pulse resume`、`read --agent intel`、`repo view --agent intel` を実行し、work gate終了0・`ALLOW work` を確認して即通知した。以後は独立所有のphaseとし、他担当の報告変更ごとの再gateはしていない。
- 製品変更は `kernel/src/dev/iommu_intel.c`、模擬検査変更は `tests/iommu_intel_hosttest.c` のみ。他担当の共通・PM・arch・AMD・PCI・ABI差分を上書きしていない。重いビルド、QEMU、Hyper-V、通常data媒体は操作していない。他OS本体、Linux、QEMU実装、NVIDIAの `uvm_common.c` は参照せず、GPLv2-onlyコード、人工上限、ABI回避を追加していない。

## 一次仕様への独立照合

資料はIntel VT-d仕様 Rev.5.20、`D:/Programes/OS DEV INFO/D51397-019-vt-directed-io-spec.pdf`。許可された読取りで原PDFの77・78・253・255・257ページを確認し、既存抽出 `build/iommu-intel-spec-20261021.txt` と照合した。抽出SHA-256は `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。

| 仕様と抽出位置 | 判断 |
|---|---|
| §5.1.4、4648–4653行、原PDF78ページ | IRES=1かつEIME=1では、CFISの値にかかわらず互換形式割込みを遮断する。EIME経路にCFIS=0を成功条件として課す必要はない。 |
| §11.4.4.1、15495–15507行、原PDF255ページ | CFIの有効条件はIR有効かつ非EIME。更新状態はCFISで報告する。一時CFI=1をIR起動前提にする規定はない。 |
| §11.4.4.2、15634–15645行、原PDF257ページ | CFISの値の適用条件もIR有効かつ非EIME。ただし、これを適用外で任意値を返してよい／CFI更新を無視してよいという許可へ読み替える根拠は得られなかった。 |
| §11.4.4.1、15331–15339行、原PDF253ページ | 他の制御状態をGSTSから保持し、一つずつGCMDを変更して完了を確認する。現 `command()` はこの経路を維持する。 |
| IRTA、17572–17580行、およびSIRTP、15478–15487行 | EIMEはSIRTP後に有効になり、IR有効中の変更は禁止。現製品は全unitでIRTA/SIRTPを確認し、IECを含む無効化完了後にIREへ進む。 |

**主張の強さを分離する。** EIMEでCFIS=0を不要とする判断には直接の仕様根拠がある。一方「準拠実機がCFISを1へ固定してCFI更新を無視する」という実機反例は未立証である。後述の固定1入力は、不要条件が残っていないことを調べる意図的な模擬入力であり、仕様適合実機の再現とは呼ばない。

xAPICでIRE前に読んだCFIS=0が、その後無関係な制御変更で任意に1へ戻る、という旧限定異論も再検討した。CFI更新報告とGCMDの状態保持手順は保持解釈を支持し、現製品はIRE書込みにもCFI=0を含めて再送する。適用外という文言だけで保持しない実装を仕様適合とする反例は成立しなかった。逆に、IRE前のCFIS値だけを全実機での割込み隔離の実測証拠とはしない。

読取り専用の限定子にも独立照合を委任し、同じ未立証事項を保持した。子は製品・試験・文書・controllerを変更していない。この返答への同意を実機受入へ昇格していない。

## 今回の修理

1. `units_on()` のCFI解除を `remap && !x2` に限定した。xAPICでは引き続き**全unit**のCFI解除確認後に、どのunitのIRE/TEも有効化する。x2では先行するIRTA.EIME/SIRTPによる遮断を使用し、隔離に無関係なCFIS=0待ちを発行しない。
2. 既存 `failed` / `command_failed` を解除しない。EIMEであっても、すでに不確定なcommandがある場合は起動・復帰・停止の成功へ戻さない。xAPICのCFI解除失敗をIR-only成功へ緩和する経路も復活させていない。
3. 模擬モデルの互換形式判定をEIMEにも接続した。EIME下のCFIS=1を誤ってbypassと数えず、plain pinは遮断されることを確認する。SIRTP完了前のEIME/IRE有効化監視、CFI解除要求の有無、既存失敗latchの対照も追加した。
4. 古い「実ioapic_suspendは保存だけ」という試験コメントを訂正した。現共通層は保存後にmaskする。backend単体試験と実IOAPIC/PIC書込みの検証を混同しないコメントへ置き換えた。

最終製品SHA-256は `bfbdfc5b207f22bb772ff89aafa98f0e081a987d456405f1c96346e8612bb554`、試験は `eef44563da3f0dd8af5a8bd0719b10a1c92a54a549430ec6fec0318db630d1bb`。

## 最新共通IRQ・PM・archの独立確認

主担当が静止した後の実ソースを再読した。

- `iommu_init()` はPIC maskを保存して全PIC/IOAPICをmaskし、backend起動後の `ioapic_remap()` を一回だけ所有する。変換失敗を起動失敗として伝播し、backend停止を確認してからhwを外してplainへ戻す。停止不明ならpinをmaskしたままfailure latchを保持する。Intel backendにIOAPIC変換の二重呼出しはない。
- `ioapic_remap()` は失敗した既存のplain要求をrollback用に保持する。新規route失敗は `given` もmaskedにして、退役したvectorを後の移行で復活させない。`ioapic_suspend()` は保存後にmaskし、resumeはdestination変更中もmaskする。
- `pic_mask_all()` / `pic_restore_mask()` はICWを再送せずmaskだけを扱う。通常起動でPIC初期化をやり直して保留IRRを消す処理ではない。
- `machine_sleep()` はS3前に保存・maskし、wake firmware後もPIC/IOAPICをmaskし直す。実際に眠った場合だけIOMMUを再設定し、成功後IOAPIC→PIC→PCIへ進む。中止sleepではPICを再初期化せずmaskだけを戻す。IOMMU復帰失敗はIOAPIC/PIC/PCI復帰より前にpanicする。
- `iommu.h` の失敗時停止未保証契約、init呼出順、`cpu.h` と `ioapic.h` の契約、native ABI説明の今回のIRQ・PM記述も実読した。ABI番号・引数・ネイティブ経路は変更していない。

この確認は実固定edgeの無損失配送、全pinのregister bank、実S3の証明ではない。実IOAPIC/PM試験も単一pinのIOWIN最終値と呼出順を扱う限界を明記している。

## 実行した確認と失敗

- 追加したEIME/CFIS=1の反証入力は、修理前製品で**18失敗、終了1**。`build/iommu-intel-live-second-before.log`。仕様適合実機の故障再現とは分離する。
- 修理後のWindows clang UB trap検査は失敗0。さらに最終試験を正規MinGWで独立実行し、**失敗0、終了0**。`build/iommu-intel-live-second-mingw.log`。
- 最終の既存共通試験を同じMinGW/UB trapで独立実行し、**81件、失敗0、終了0**。`build/iommu-intel-live-second-common.log`。
- 実 `idt.c` / `ioapic.c` / `pm.c` を含む既存PM試験も独立実行し、**51判定、失敗0、終了0**。`build/iommu-intel-live-second-pm.log`。中止sleepのPIC保留要求モデル、通常sleep、復帰失敗前停止、route失敗rollbackを含む。
- 製品のfreestanding x86_64 targetに既存kernel flagsと `-Wall -Wextra -Werror -fsyntax-only` を適用し、終了0。`build/iommu-intel-live-second-syntax.log`。対象 `git diff --check` も終了0。

最終Intel模擬試験の再現コマンド：

```sh
./tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -g -finput-charset=UTF-8 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -fno-unwind-tables -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon tests/iommu_intel_hosttest.c '-Wl,--gc-sections' -o build/iommu_intel_live_second_mingw.exe && ./build/iommu_intel_live_second_mingw.exe
```

共通・PM試験も同じ基本flagsで、ホストstubが実行しないLP64/LLP64のformat差だけを抑制した。製品targetの警告判定は弱めていない。CFI永久0、xAPIC解除失敗、後方unit失敗前の未enable、firmware queue、未観測IQE/ICE/ITE、遅延completion、旧世代drain、幅の既存検査を削除していない。

## 未確認と次の一手

元の実QEMUログ `build/iommu-repair-20261021/intel-serial.log` は、一時CFI=1必須化による起動失敗・EDU未達の証拠のままである。この第二検討ではQEMUを起動していない。修理後最新媒体の単一QEMU EDUのDMA/IRQ/claim/reset受入は主担当の次の作業であり、host成功で置き換えない。

実機のfirmware/SMM引継ぎでOSがsole producerである保証、初期firmware queueとPMMの寿命、実DMA drain、実固定edge、実S3/hibernate、NVIDIA reset・音声・GUI応答、Hyper-Vは未確認。前cycleで挙げたこれらを今回のCFI条件修理で解決済みとはしない。既存のcommand監視予算も全実機の仕様上の完了保証とは呼ばない。

製品・試験を静止し、本報告を最新共通/PM/archと試験・一次抽出へsource依存付きでround 2として一度提出する。主担当は独立受入と増分ビルド・最新媒体の実検証を行う。
