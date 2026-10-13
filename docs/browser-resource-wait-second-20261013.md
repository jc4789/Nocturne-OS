# 通信待機の共通進捗修理：独立した第二考察

第一報の最弱前提は「共有wakeを減らせば20秒のconnect timeoutも消える」である。SYNに対する外部応答、受信queueの取りこぼし、DNSで得た宛先、native scheduler、helper処理とGUI側pipe排出は別の原因を持ち得る。id=11はqueue待ちが短く、本修理を直接のtimeout解消とは主張しない。

## 所有・取消・再利用の反例

- connect中・送信buffer満杯・受信待ちの各sleepを同じ安定TCBのqueueに移し、packet、reset、close、freeのwake側も同時に移した。一部だけ変更して進捗が届かなくなる形ではない。
- closeは `user_open=false` を公開してからwakeする。sleepから戻った旧receiveはgeneration/user_openを先に確認し、別接続のbytesを消費しない。partial sendの既存戻り値契約も保持する。
- freeはqueueのwaiterをdetachしてからclearする。TIME_WAIT終了やOOM時にqueue headをmemsetで失い、待機taskが消えたqueueへ残ることを避ける。queueはTCBと同じ安定寿命であり、growable pointer tableが移動しても待機先は動かない。
- process exitは既存 `net_process_exit` → `tcp_close` を通るためlocal queueをwakeする。schedulerのkill/timeoutによるwaiter unlink処理は変えていない。FIN/TIME_WAIT tupleとbufferは従来のTCP寿命で保全する。

## 別の無関係wake境界

`kernel/src/sys/syscall.c:sys_poll` はVFS descriptorのcan_read/can_write/hangupだけを検査する。SYS_TCP_* の独自handleはVFS fdではなく、TCP受信そのものはpoll対象のfd ready状態を変えない。それでもTCP data/FIN/closeが `poll_notify` を発行し、GUI・pipeの全pollerを早過ぎる時点で起こしていた。

TCPのこの3通知だけ除去した。実HTTP helperがresponse pipeを書き、閉じ、終了するときは `kernel/src/fs/pipe.c` とschedulerが既に正しいpoll通知を行う。実fd-ready producerの通知を省略して速く見せる修理ではない。UDP/link通知は今回の責務境界で残した。

## 有限待機が無期限へ変わる別の境界

DNS queryは `while (uptime_ms() < until)` の判定後、もう一度時刻を読み、`(int)(until - uptime_ms())` をudp_waitへ渡していた。二つの読出し間に期限を跨ぐとunsigned減算が下溢れし、intでは負の値になり得る。udp_waitでは負のtimeoutが無期限の意味なので、正当な短いDNS待機が無期限へ変わる。UDP receiveとICMPのwait queue残量にも同じ二重読出しがあった。各条件判定・減算を一つの `now` に結び、deadline延長やretry追加なしで修理した。これはコード上の期限意味の不備であり、今回のCSS connect timeoutがDNSで起きたという証拠ではない。

## 受入範囲

第一版時点では構文・diffとsource両側のwait/wake照合だけ確認済みで、新nativeのclose待機5条件等は未実行だった。後続統合nativeでTCP追加5条件は主担当が成功を確認した。これを後続TLS版の実取得・helper pipe・実サイト入力応答や速度の証拠へ流用しない。実測のない高速化率や性能回帰解消は宣言しない。

## 後続TLS修理の別考察

122秒のIO失敗を「read期限だけの問題」と断定すると、同runの100秒で成功した4MB scriptを説明できない。今回新診断は最終transport phaseと、DNS/link待ち・TCP・TLS・最初の最終status line・全header・bodyの期間を区別する。first-headerは全header期間の部分値であり、両方を合計しない。body期間はHTTP callback/展開/終了処理も含み、純粋なネットワーク速度とは呼ばない。成功responseでもTLSに100秒かかったか本文にかかったかを、最新媒体で初めて識別するための観測である。

HandshakeにはTCP開始時の同じ絶対期限を渡し、6000msの小recordが4回来ても20秒を再設定しない。本文では毎public readの期限が新しく始まるため、streamが20秒を越えることを拒否しない。-1は本文の無期限read、zeroは既に到着しているbytesを読める非blockingを保全する。closeも同じ一操作の1000msを複数recordへ分配する。timeout/RST/EOFで失敗したopenはTCPを一度閉じ、成功streamはns_closeで解放する。

ただし `tcp_send` には現行APIでtimeoutがなく、一回の同期sendがTX満杯で待つ場合をTLS側時計だけでは打ち切れない。通常のhandshake小送信と、送信buffer満杯時の不保証を区別する。rootの指示どおり、新ABIを追加して無関係に広げず、この限界を残した。したがって全面的な20秒I/O保証とはしない。

既存HTTP request/hook structを拡張して古いcallerのstackを読む方法は採用せず、新opt-in entryから同じ既存innerへ渡す。plainではTLS期間0、redirect/既存reset retryはattempt数を持って集計する。Fetch streamingにはdebug bitを許可せず、作者APIへinternal行を渡さない。server field filterで作れないcolon無しprivate行を数値検証して除去し、wireの元header長からbody位置を読む所有規則は変えない。古いsynthetic ns_open mockにも同じ追加seamを渡し、native API不足を期待変更や偽測定で隠さない。

最新支持はhost37/12/9だけであり、TLS認証・実HTTP配信・100秒取得改善・実GUI応答の合格ではない。実サイトで再失敗した場合は新phaseとtransport errno/BearSSL codeを読み、原因が特定できた既存責務だけを再修理する。

凍結後の査読では、diagnosticの削除開始をRequest行と同じline+2へ機械的に変える案も検討した。Transport producerは既存終端の後へ新CRLFごと末尾追加するため、line+2削除なら追加CRLFだけ余計に残る。具体的なstatus/meta/実field/空failure wireを再構成して元prefixの完全保持を確認し、製品sourceは変更しない。42も既存redirect/reset loopからの派生値と確認し、容量ポリシーと混同しない。
