# Intel VT-d 第二報告（2026-10-21）

## 範囲・第一検討との差

- 担当IDは `intel`。第一報告 `docs/iommu-intel-first-20261021.md` を全文再読し、その本文は変更していない。第一報告のSHA-256は `8e520bf50d59bd56df4c42009ee69cd47c28896f4d0def15de0b485042f22d44`。
- 製品の所有範囲は `kernel/src/dev/iommu_intel.c`、模擬試験は `tests/iommu_intel_hosttest.c`。共有root・ABI・AMD・PCI・NVIDIAの変更を上書きしていない。重いビルド、QEMU、Hyper-V、通常のdata媒体は操作していない。
- 開始時に差分・指示・既存mapを確認し、既存のxhigh登録を継続した。自身のpulse/read/repo view後、work gateは同時controller更新による競合が一度あり、次の確認で `ALLOW work`。その後、独立所有内の編集ごとに再gateする運用には戻っていない。
- 第一検討の「major1の排出能力は追加照合が必要」「firmwareのTE/IRE停止後にqueueを静止」という結論をそのまま採用せず、下記の反例で製品を追加修理した。第一報告に結び付いた旧ソースのhashが変わることは履歴であり、旧試験結果を新ソースへ流用していない。

## 仕様の再照合

一次資料は許可されたIntel VT-d仕様 Rev.5.20、`D51397-019-vt-directed-io-spec.pdf`。抽出 `build/iommu-intel-spec-20261021.txt` のSHA-256は `8ba9a310674cee3597034a750def56f956beddf22184976194e3b9f6df021003`。他OS本体・Linux・NVIDIAの `uvm_common.c` は参照していない。GPLv2-onlyのコードを追加していない。

- **§6.5.4、§11.4.2（抽出8333–8397、14494–14515行）**：drainは変換済みでもRoot-Complex内に残る未commitの要求を対象とする。major1ではDRD/DWDの各能力が任意で、IOTLB invalidation後のwait完了だけでは欠けた方向のdrainを保証しない。major2以降は必要なdrainが必須で、両能力bitも互換性のため1を報告する仕様。PCIのBME停止やTransactions Pendingだけを、全TC/RO/IDOのposted writeを含む汎用drainの代用品とはしない。
- **§6.5.2、§11.4.9.1–3**：QI停止には「empty」だけでなく最後のwait完了が必要。TE/IREを先に落とす要件はなく、その操作はproducer所有権移譲の証明でもない。元のIQA/QS/descriptor幅を保ったまま終端waitを完了させる。
- **§6.5.2.11、§11.4.9.9**：IQE/ICE/ITEを未観測のままTE offに進めない。未知fetch失敗・遅延status writeをclear/skipで回復したことにせず、既存の永続failedとcommand_failedを維持する。
- **§6.9（抽出8533–8546行）**：Global/Enhanced/Context/IOTLB/PMENの各command間で、前のcommandの完了確認が必要。firmwareのICC/IVTが立ったままTE offを発行することも避ける。
- **§3.16（抽出3442–3470行）**：RMRRをidentity RWで維持するだけでなく、OS-visible DMAのinput addressが予約域と重ならないこともsystem softwareの責務。共通層のRW予約とDMA object寿命を再検討する根拠となった。
- **UEFI 2.10 §7.4.6**：EBS成功後にOS loaderが継続運用を担い、Boot Servicesが終了するという引継ぎを[公式仕様の検索抜粋](https://uefi.org/specs/UEFI/2.10/07_Services_Boot_Services.html)で照合した。本文openはHTTP 403で失敗。これをSMMの並行producer停止を実測した証拠とは扱わない。

## 反例と製品修理

### 1. 排出能力不足での偽の完了

反例はmajor1かつDRD/DWDの一方以上が0のunit。旧 `intel_flush(dom, true)` / `intel_block()` は対応するdrainを要求できなくてもsync成功をそのまま返した。attachもpass-through時代のin-flight要求を排出したと証明せず、app domainを公開できた。

修理は `dma_draining()` に集約した。全unitで能力がそろうかmajor2以降の必須保証があることを確認する。attachはcontext公開・dom->written設定前に拒否する。unmap/blockでは必要なinvalidation自体は実行するが、排出保証がなければfalseを返し、共通層のDMA保持・domain quarantineを残す。単にログを増やした修理ではない。能力不足をqueue異常と混同してfailedにせず、IRQ-onlyなどの別機能は維持する。

major2の対照は仕様の必須保証を検査するためCAP bitを省略した模擬入力も含む。実在する準拠major2で両bitが0になると主張していない。ATSで変換済み要求を出す機器のDevice-TLB排出、peer DMA全般の安全性をこの変更だけで証明していない。

### 2. まだISRが観測していないqueue異常

反例は `failed == false` だがFSTSにIQE/ICE/ITEが立ったunit。旧停止はfailedだけを見てTEをoffにし、その後のqueue確認で初めて異常を発見し得た。

修理後は全unitのqueue状態を先に検査し、全unitの終端waitとemptyを確認してからTE/IRE/QIEを解除する。後方unitの異常が前方unitの保護解除を許さない。quiet失敗はfailedとして残し、後続resume/shutdownでsemaphoreを再利用しない。global commandが確認されなければ後続unitへ停止commandを発行し続けない。

同時に、正当なS3後のレジスタresetをshutdown時のQIES消失と区別した。`intel_resume()` は「実際にsleepした後だけ呼ぶ」という共通契約に従い、旧queue_onを所有権の証拠にせず、現在activeなら継承queueとして再確認する。failed/command_failedは解除しない。これにより、通常shutdownのQIES喪失検知を保ちつつ、正常なreset後resumeを拒否する回帰を避けた。

追加の仕様照合で、旧command待機がContext/IOTLB register invalidationのICC/IVTを含まない反例も見つけた。`command_idle()` をcommand発行、queue開始、全unit quietに共有し、未完了register invalidationをTE offで追い越さないようにした。後方unitのICC停滞でも前方unitの保護を解除しない。

### 3. firmware active QI

- firmware queueの終端waitも、TE/IREがonの間に完了を確認する。失敗時に先に保護を落とさない。
- IQA、QSの全encoding、128/256bit幅を保存し、in-flight slotをCPUで書き換えない。未知descriptorのfetchはhardwareへ任せ、emptyを待って空きtailへwaitを追加する。fetchが完了しなければ失敗として既存保護を保持する。
- 物理幅・pointer整列・範囲と、既存direct-mapが対象物理addressに一致することを確認する。未知addressへ新規mappingして書き込む方式にしない。descriptorはページ内に収まり、元queueを解放・移設しない。
- semaphoreはbackend所有の物理page。wait公開後に失敗しても解放・再利用せず、late writeで次の要求の成功を偽装しない。
- 無変更の `start(false)` 直後や確保失敗ではsemaphoreが存在しない場合がある。`queue_quiet()` はsemsが0ならwaitを追加せずfalseを返す。active QIは保護を保持し、全unitが既にoffならsemaphoreなしでも停止確認できる対照を追加した。

ただし、これらは**OSが唯一のproducerであるというplatform引継ぎ契約をレジスタだけで立証するものではない**。TE/IRE off、IQT/IQAの一時的な安定、EBS自体をSMM排他の証拠にしない。Nocturneのqueue操作はOS内で直列化されるが、SMMが同じqueueを並行使用する実機への協調引継ぎは未確認。また、active firmware queue等をfirmwareがfreeable RAMとして報告し、backend probe前にPMMが再利用してしまう場合の寿命は、このbackendからは保証できない。正しいfirmware memory mapの前提と、必要なら早期予約という統合境界が残る。既知hardwareでもprobe自体がNULLを返した場合は共通層が停止callbackを呼べず、初期引継ぎの不確定性を新しいstart失敗latchだけでは検出できない。この残件を修理済みとは扱わない。

## 共通層の独立検討

- `range_fits` / `pages_fit` はaddress上位bitと加算wrapをPTE更新前に拒否し、入力全体を先に検査する。`iommu_alloc_pages` は物理幅を満たさないframeを解放し、一枚の場合のみ低位first-fitを試す。多枚のallocatorも元から低位first-fitであり、不必要な再走査を追加していない。
- reservationの同一範囲はaccessの積、重なる範囲は境界分割と積で扱い、no-accessをRWへ変えない点を確認した。
- 独立反例として「RW予約域をDMA objectへ割当→unmapでRW予約PTEを復元→その物理pageを再利用しても古いdomainからRW到達が残る」を主担当へ通知した。§3.16の根拠も追加通知。所有外なので自身では編集していない。
- 主担当の最終追加修理を再読した。DMA objectから全該当予約域とのoverlapを拒否し、最終accessの積が非zeroの各cutだけでOS-owned RAM交差を検査する。no-accessの重なりで正当にRAMを除外できる場合まで拒否しない。allocation failureのr=0残存による非隔離成功化も修理され、large leafを下位tableとして辿らない防護もある。共通層の実行検証は主担当の報告へ委ね、ソースの独立再読と区別する。
- unmapは予約mappingの寿命をDMA objectと混同しないこと、PMのrestore_failedが一度失敗するとactive/attachを成功へ戻さず、明示非隔離opt-inでも失敗後のdomain quarantineを解除しないことをソースで確認した。共通層全経路の形式的証明や実機証明ではない。
- 追加反例として、`iommu_init()` が不確定なstart失敗後にhwをNULLへ落とすと、後の `iommu_shutdown()` が成功扱いし、hibernate復元がactive hardwareの表を上書きし得る境界を通知した。最終共通層ではhwを保持してshutdownを要求し、失敗ならrestore_failedをlatchしてからhwをNULLにする。NULLになってもlatchがattachのopt-in、resume/shutdown、hibernate上書きの成功を拒否する。sems0のIntel防護と接続し、無変更falseで全停止確認済みの場合と区別した。最新headerもfalseで停止を保証せずcallerがshutdown確認する契約へ訂正された。停止不能なIRREQ機で起動可用性まで保証したことにはならない。
- 未知TSCに10GHzを仮定する方式は主担当が撤去した。未測定時はready状態を一度確認できても、未完了pollの経過時間を証明できず保全側へ失敗する。cache flushはCPUIDのCLFLUSH幅を使い、利用不能時の全cache writebackへ分岐する点をソースで確認した。

## 検証

製品Cを直接includeする既存のMMIO模擬試験に、元反例と対照を追加した。

- major1のDRD/DWD全組合せ、major2の必須drain対照、複数unitの能力交差。attach無公開、mapping追加、unmap、block、実descriptorのDR/DW要求bitを確認。
- 未観測IQE/ICE/ITE、own/firmware queueへのwait fetch失敗、未知の既存fetch停滞、direct-map不在、late semaphore、失敗後のresume/shutdown。TE/IREを保ったまま失敗し、エラーを消さないことを確認。
- QS全値×128/256bit幅、wrapと元queue保存を維持し、終端waitより先の保護解除を検出する対照を追加。
- 正常shutdown/resume、S3相当のQIES reset後resume、SRTP timeout、複数unit中の後方既知/未観測faultを検証。第一検討のqueue skip/late completion/IR-only/幅/leaf検証も残した。
- sems未確保のstart(false)後のactive QI拒否と既停止の成功、ICC/IVT pending時のcommand拒否、複数unitの後方register command停滞も追加検証した。

再現コマンド：

```sh
clang -std=gnu11 -O1 -g -finput-charset=UTF-8 -Wall -Wextra -Werror -Wno-unused-function -Wno-unused-parameter -Ikernel/src -Icommon -fsanitize=undefined -fsanitize-trap=all tests/iommu_intel_hosttest.c -o build/iommu_intel_hosttest.exe && ./build/iommu_intel_hosttest.exe
```

最終結果は失敗0・終了0。ログは `build/iommu-intel-second-hosttest.log`。target clangの `--target=x86_64-unknown-none-elf`、freestanding/kernel/no-red-zone等の既存flagsと `-Wall -Wextra -Werror -fsyntax-only` も終了0。`git diff --check` も終了0。

途中で旧PM fixtureの期待したcommand_failedが成立せず1失敗した。新しい停止前検査が先にQIES消失を検出したためである。この検討を契機に実S3後resetという製品回帰候補も反証し、resumeの所有状態再確認を修理した。正常reset後resumeとSRTP自体のtimeoutを別々に検証し、期待値を単に削除して合格にはしていない。ホストの模擬時計・MMIO・allocatorは実hardwareの代用ではない。

## 引継ぎ・未確認

Intel所有の最終製品SHA-256は `2da376aa73072c8990aaa06edea699fbef7390df62c4e8012d596e0606ede6b3`、試験は `96d7daa412ee7804dc494473d8cee27d550cd677c237102a78b1920054339c47`。新しい固定件数上限・ABI回避・他OS由来コードは追加していない。既存の100ms監視予算は仕様上の全command完了保証とは扱わず、超過時は不確定として資源を保持する。主担当から別環境のMinGW UBtrapでも共通61判定/失敗0、Intel失敗0との通知があったが、自分の実機受入へ昇格させない。

最終独立再読した共通ソースは `iommu.c` が `73a0d6796a17b54c5f6e8d4792ad701647227c13b009868bfc1f7d7d0dba358b`、`iommu.h` が `0adda9cb4cbf52763c09c2b177fb2855825786116e8b39a4a1b8c9093e175213`。第二報告はこれらと所有ソース・試験・一次抽出・寿命に関わる参照ソースをcontrollerのsource依存へ結ぶ。提出後は製品編集を止め、主担当の新たな実QEMU失敗があれば別のcycleで扱う。

次は主担当による最新共通層との独立受入、差分ビルドと梱包、可能な単一QEMU EDU操作。実機のfirmware引継ぎ、DMA drain、NVIDIA reset、S3/hibernate、Hyper-Vは未確認であり、host成功やQEMU成功から昇格しない。
