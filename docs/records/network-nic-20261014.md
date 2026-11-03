# NIC通信改善の担当記録（2026-10-14）

## 範囲と共有契約

- 基準は `d962b59`、開始時の差分はなし。先行の描画改善は変更しない。
- J-space は指定どおり medium、並列担当。今回の共有地図は `.jspace/network-map.json`。旧ブラウザーの大台帳は再開・更新しない。
- 担当製品は `kernel/src/net/e1000.c`、`kernel/src/net/net.h`、`kernel/src/hv/netvsc.c`。TCP/IP本体と受入は主担当が統合する。
- `netif.rx_pending()` はIRQ禁止区間でも判定できる。`poll_rx(input)` はnet threadのみが呼び、`input` が返るまでDMA bufferを借用し、返った後にNICへ所有権を戻す。
- `net_rx_notify()` は受信だけでなくTX資源返却の通知にも使う。IRQ内の待機は増やさない。

## 製品修正

1. e1000のIRQでpacketをコピー・protocol処理・descriptor返却しない。ICR確認後、RX IRQをmaskしてnet threadを通知する。
2. net threadが完成済みdescriptorを直接処理し、drain末尾にRDTを一回だけ更新する。空判定とIRQ再arm間の新着も再確認し、待ちへ落とさない。
3. RX/TXそれぞれに4KiBのdescriptor pageを一枚確保し、16byte legacy descriptorから各256個を導出する。小さいIRQ用queueではなく実DMA資源がバッファリングする。packet bufferは各512KiB、descriptor二枚を合わせた確保量は1,056,768byte。確保失敗では既に確保したDMAページだけを返す。
4. TXはhead/tail同値がemptyを意味するため一個を予約する。満杯では100万回のspinをやめ、`-EAGAIN`を返す。TXDW IRQがnet threadを起こすので、主担当のTCP retryへ接続できる。
5. ITRを有効化し、既定はIntelの推奨範囲の低い側である651tick（1tick=256ns、約166.7µs）。`e1000-itr-us=<microsecond>`で設定可能、`0`で無効化する。数値overflowと実16bitレジスター範囲だけを検査する。ITR/RADV非対応の82544GC/EI（device 0x100C）には書かない。
6. netvscは基準コードでもsection枯渇で既に`-EAGAIN`を返していた。修理の中心は主担当によるTCP側への失敗伝播である。本担当ではhostの送信completion時だけnet threadへ進捗通知する。`vmbus_send`失敗後のsection返却は進捗ではないため通知しない。重複completionで未使用sectionを返した場合も通知しない。
7. netvscのsection/bitmap確保失敗、RNDIS headerを含むsection長、64bit completion IDの範囲を検査する。IRQ内blockingや巨大な再試行は追加しない。

## 採用しなかったRDTR/RADVとオフロード

- Intel 8254xマニュアル§13.4.30はRDTRのdelay field利用を強く非推奨とし、ITRを推奨している。82546GBのerratum 17はRDTR/RADV利用時の受信IRQ欠落を記載する。従ってRDTR/RADVを0のまま保ち、ITRだけを使う。QEMUでもRDTRの相対timerは未実装で、RDTRはRADVを有効化する条件として扱われている。
- チェックサム/TSO自体は否定しない。ただし現行e1000はlegacy descriptor、netvscはRNDIS packet headerのみで、必要なchecksum contextやper-packet-info、NDIS offload capability交渉が未接続である。Intelマニュアル§3.6はchecksum context初期化なしの有効化が予測不能な結果になると警告する。今回、未成立のオフロードをbitだけ有効化しない。ソフトウェアchecksum改善は主担当の範囲。
- [Intel 8254x一次マニュアル](https://www.intel.com/content/dam/doc/manual/pci-pci-x-family-gbe-controllers-software-dev-manual.pdf)
- [Intel 82546GB仕様更新](https://www.intel.com/content/dam/doc/specification-update/82546gb-gbe-controller-spec-update.pdf)
- [QEMU e1000実装](https://gitlab.com/qemu-project/qemu/-/raw/master/hw/net/e1000.c)

## 確認結果と限界

- `git diff --check` は成功。
- kernel target flagsによるe1000/netvsc二つの翻訳単位のClang構文確認は成功。初回はdescriptor ABIのstatic assertionをstruct定義より前へ置いてしまい失敗し、順序を修理して成功した。
- `tests/e1000_batch_test.c` は実製品のe1000.cをincludeし、DMA/MMIOとIRQ状態だけを模擬する。製品コードへTEST分岐は加えない。
- `tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O2 -Wall -Wextra -Wno-unused-function -finput-charset=UTF-8 -Ikernel/src -Icommon tests/e1000_batch_test.c -o build/e1000_batch_test.exe && build/e1000_batch_test.exe` を実行し、65確認、0失敗。
- 確認した境界はRX batch、ring wrap、callback完了前の借用寿命とRDT保全、IRQがコピー/解放しないこと、不正DMA長と受信error、空判定から再arm間の新着、TX満杯と`-EAGAIN`、completion起床とretry、TX wrap、ITR token/overflow/レジスター境界。
- これは限定ホスト確認であり、実NIC、Internet throughput、パケットloss率、Hyper-Vの成功証明ではない。全体build、媒体更新、QEMU製品確認は主担当の結果を別に記録する。本担当はQEMUを起動せず、性能向上率も主張しない。

次は主担当がTCPのACK/readiness集約・送信失敗伝播とsleep通知raceを統合し、最新版で製品経路を確認する。
