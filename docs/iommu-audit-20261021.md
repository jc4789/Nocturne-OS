# IOMMU追加監査とDMA確保修理（2026-10-21）

## 依頼と保全

外部監査を結論として採用せず、指定のAMD 48882 Rev3.10、Intel VT-d Rev5.20、PCIe Base Rev6.0と現製品で再検証した。開始時に残っていた旧root記録3ファイルの差分は保全し、旧報告を上書きしていない。他OS本体・Linuxの参照、GPLv2-onlyの追加、native ABIの迂回、根拠のないquota/期限は行っていない。

## 判定と製品変更

- **表の拒否と割込み停止を分離。** backendの`probe(bool *plain_safe)`が、全unitを識別し意味のある停止状態を独立に確認した場合だけplainを許可する。parserが隔離に使えないことだけを理由に割込みを止めない。不完全なunit列挙、読取り不能、動作中engine、Intel IRREQ等はplain成功へ変換しない。両backendがNULLなら双方の証明が必要。
- **AMDの最小depth。** HATSは最大値として保持し、identity DMAの実受入幅`min(VA, PA, 52)`を覆う最小階層へ縮小した。実reserved幅やHATDisを推測で受理しない。
- **AMDの停止確認。** 対応するPPR/GA/仮想engineを停止確認へ統合し、firmware imageが実機能を隠しても停止確認を省かない。MMIOの規定16KiB整列も検証する。
- **Intelの継承queue所有権。** 未知の旧descriptor領域へのCPU読書きを撤去した。OS再利用の可能性があるRAMとの交差はmetadataの割当/zero前に拒否する。hardwareで空を確認後、QS/DW/offsetを保ちOS所有のqueueと専用status領域へIQAを移して、新waitの完了を確認する。空だけを完了とせず、公開後の不確定なstorageは保持する。割当前/公開前の失敗と公開後の失敗を区別する。
- **PCI再claim。** RRSの追加初期化を一度のreadiness確認だけで失敗にせず、保存FLR状態を維持して再照会する。kill検査とwait登録をBSPの短いIRQ保護内で原子的にし、長い待機をbusy waitや固定sliceへ変換しない。BME操作・Link Control register/bitsは既存PCI責務へ共用化した。
- **非連続DMA確保。** 有限IOMMU上限の有無で`pmm_alloc_contig(1)`を毎ページ呼ぶ分岐を廃止した。既存PMMの`search_hint`を上限内で使用する`pmm_alloc_below(limit)`へ統合し、syscallのDMA本体とIOMMUの単一metadata頁が共用する。上限はexclusiveで、word途中の末尾もmaskし範囲外の頁を消費しない。公開syscall番号/構造は不変。

第二検討と主担当の再読で追加発見した問題も製品へ修理した。

- AMDはPerfOptの対応判定にも実featureとimageの和を用い、imageが実PerfOptを隠しても迂回を解除する。MARCも対応時のControl.MarcEn解除を確認し、解除未確認ならtableを公開しない。非対応reserved bitへの要求は増やさない。反例は修理前13失敗、修理後409判定/失敗0。
- PCIは**全CT値で**BMEoffと実TP=0をFLR前に要求する。旧driverがCTdisable→要求発行→CTenableを行うと、現在の設定だけでは旧要求のtimer適用を証明できない。従来の「BME stuckでも現CTが既知ならreset成功」という受入は安全根拠を失うため撤回した。TP消失済みなのに最大64秒のCT待機を加える処理も除いた。未完了や新要求禁止を確認できない装置には装置固有のquiesceが必要。
- PCIは対応上流Root PortのRoot Controlをnarrow write/readbackし、RRS Software VisibilityをFLR前と保存reset再開・再照会時に接続する。非対応/拒否/RCRBのhardware内部retryまで取消可能とは主張せず、一律拒否や推測期限も追加しない。新反例は修理前114失敗、修理後失敗0。
- IntelはDRHDの予約Size bit、広告register-set全域へのbase整列、範囲加算overflowをmapping前に検証する。FRO/IROが広告範囲外なら、勝手にmap範囲を広げず拒否する。誤ったbase+8の読取りが実際のactive IRをplain-safeと誤認する反例を、修理前17失敗から修理後0へ直した。
- IntelのRMRRも、短縮構造・scope欠落・逆範囲・非page境界・末端加算overflowを黙って捨てたり丸めて受理しない。無効な隔離用metadataと全DRHD観測の完全性は分け、全off時のplainを妨げない。52bit上端の正常範囲は受理する。新反例は修理前24失敗から修理後0へ直した。

第一検討時の媒体成功は履歴として保全し、下記の最終媒体で受入を更新した。

## 安全性のため維持する境界

multi-function groupを全体で回復する条件は維持する。FLRは対象Functionだけをresetし、旧driverはP2Pでpeerに状態を残せるため、新primaryだけのFLRでは不十分。

S3でIOMMU復帰を確認できない時のpanicも除去しない。未確認のままPCIのBMEや割込みを戻す旧log-only動作は安全な回復ではない。実queue fault/公開後timeoutの永続failedとDMA保全も、遅延statusや旧DMAの寿命を証明できない状態で自動解除しない。

## 検証の区別

- 実PMMを含むUBSan trapモデル：12判定、失敗0。256MiB相当の65,536頁について旧`contig(1)`経路は1.547秒、新helperはこのホスト時計の分解能未満。実GPU性能やsyscall全体の速度比較ではない。
- 共通IOMMUの最終モデル：89判定、失敗0。parser拒否+全offのplain許可、一方のunknown/IRREQの迂回禁止、出力不変、幅・予約・失敗寿命を含む。製品の厳密なtarget構文確認も成功。
- 第一検討製品の単一QEMU：AMD→Intelの順、WHPX/4CPU/4096MiB/1920×1080、AHCI起動、専用boot/data snapshot。flags=0の非連続256MiB確保はAMD411ms、Intel374ms、全65,536頁のaddress取得と先頭末尾zero/読書きは失敗0。通常`pcitest`、二機能`flr`、`nvrmtest`は失敗0、`nativeabitest`225/0。
- 主担当本人が両desktop/Terminal画像と、HMP実キー入力の`help`出力・prompt復帰を確認した。revoke後の意図的な子page fault、禁止IOVAの変換fault、EDUの実GPU初期化拒否は隠していない。
- 初回AMDのquit送信だけでは終了が完了せず、次のIntel起動はmonitor占有でguest開始前に終了した。再送後AMDの終了0とQEMU不在を確認してからIntel単独を起動し、その終了0も確認した。Hyper-Vや通常dataは操作していない。

ログ・画像は`build/iommu-audit-20261021/`、担当のbefore/afterログは`build/iommu-*-audit-*.log`。第一検討のkernelは`0cbaedabb4afbf89831d8af39a2c7f7fdd5ca8cc4d254633b476408743269627`。

## 最終統合受入と独立レビュー

主担当は各第二報告の全文、対応する製品差分、反例の前後ログを実読した。IntelのIQA変更条件とDRHD広告サイズ、AMDのPerfOpt/MARCは一次抽出も再照合した。主担当が別途発見したMARC停止確認とRMRRの境界は担当へ戻し、同じ所有実装へ修理を統合した。担当のPASSだけで採用せず、最新製品の増分梱包と実QEMUも自分で確認した。

最終`python -X utf8 scripts/build.py -j 4 --log build/iommu-audit-20261021/final-build.log image`は終了0。IMG/VHD/VHDX/ISOを更新した。kernelは`242d597e803fdb3c38106eb7a315d7b78dba879b48eb68ccf52fcfe9c735352a`、initrdは`5d91438616d6d71df255af0f56a3d243fc261d5d30457080913a7145eb6531b6`。最終IMGとISOから抽出した双方のpayload、および実QEMU用bootのkernelがこの値と一致した。VHD/VHDXとIMGも`qemu-img compare`でそれぞれ一致した。証拠は`media-content-final.log`、`vhd-final-compare.log`、`vhdx-final-compare.log`。

| 最新製品・同一条件の単一QEMU | AMD | Intel |
|---|---|---|
| 隔離・IRQを使うEDUの非連続256MiB確保 | 375ms、65,536頁確認、失敗0 | 376ms、65,536頁確認、失敗0 |
| 通常pcitest / 二機能flr / nvrmtest | 各失敗0 | 各失敗0 |
| nativeabitest | 225判定、失敗0 | 225判定、失敗0 |
| 実画面・実キー入力 | desktop/Terminalとhelp出力・prompt復帰を主担当目視 | 同左 |

条件はWHPX/4CPU/4096MiB/1920×1080、q35、AHCI boot、専用boot/dataのsnapshot、EDUと二機能peer。AMDを終了0まで確認してからIntelを単独起動し、Intelも終了0を確認した。画像は`amd-final-now.png`、`amd-final-input.png`、`intel-final-now.png`、`intel-final-input.png`で、四枚を主担当自身が開いた。ログは`amd-final-serial.log`と`intel-final-serial.log`。意図したrevoke後の子page fault、禁止DMAの変換fault、EDUに対する実GPU初期化拒否も残している。この測定は新PMMを通る製品経路の成立を示すが、旧製品とのQEMU A/B速度比較や実GPU性能の証明ではない。firmware active queue、壊れたACPI、locked MMIO、不可視RRSはこのQEMUで再現しておらず、対応するhost反例と区別する。

AMD第二報告のPerfOpt/MARC、Intel第二報告のDRHD/RMRR/継承queue、PCI第二報告のCT履歴/RRSの修理を、上記の仕様・差分・反例と統合結果の範囲で受け入れる。報告中の実機未確認と残件は撤回しない。PMM/commonは三担当の独立ソース再読も受け、追加の確定反例はなかった。

## 一時物の回収

全QEMU終了・媒体照合後、解決した絶対pathが今回の`build/iommu-audit-20261021`内にあることを確認し、専用`boot.img`/`data.img`、IMG/ISO展開payload四個、照合用`qemu-kernel.elf`だけを回収した。通常`build/data.img`、標準IMG/VHD/VHDX/ISO、Hyper-V媒体、失敗反例ログ、代表画像と小さい設定は保全した。削除終了0、回収後のD:空きは約110GiB。この試験用scratch媒体は保存済み・即再起動可能とは報告しない。

## 未解決・未確認

実4070のクラッシュ後reset/scanout再建とnvrmの同一主体によるidle接続/thread占有・公平性は未解決。画面hold解除や`N_PCI_NEEDS_RECOVERY`のlogを回復成功としない。画面復元の契約がない汎用FLRや、根拠のないper-client quotaは追加していない。

実機DMA/IRQ、実fabric drain、S3/hibernate、EIME/IRREQ、firmware/SMMのsole-producer引継ぎ、Hyper-V、実GPU性能は未確認。今回QEMUがfirmware active queueの移設を実行したとは主張しない。実機のIVRS dumpも取得しておらず、ユーザー機がどの拒否条件に該当するかは未確定。
