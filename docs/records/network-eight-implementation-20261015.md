# 通信監査8項目の実装記録（2026-10-15）

開始時は `main`、作業ツリーに変更なし。`git fetch origin` 後も HEAD と
origin/main は `3f726aca2790d1844485ac8dcf9f5b9ab6cac9bb`。
既存の未コミット変更の取消・push は行わない。

Hyper-V 固有項目2・3は製品実装、静的レビュー、通常ビルドだけを行う。
ユーザー指定により Hyper-V VM 起動・設定変更・専用ランタイム試験は禁止。
QEMU は WHPX、4096MiB、1920×1080、1CPU/複数CPUで確認する。

| 項目 | 実装状態 | 根拠・検証・残件 |
|---|---|---|
| 1 Fetch/XHR pacing | 実装・実ゲスト回帰成功 | `webnet.c` の公平な時間/byte予算、creditとpause、`browser.c` のGUI/通信poll。1/4CPUで全byte・END・別要求完了を確認。 |
| 2 Hyper-V受信 | 実装・静的確認 | `netvsc.c` IRQ通知とthread予算処理、借用RX所有権と負荷統計。動作試験禁止・未検証。 |
| 3 Hyper-V completion | 実装・静的確認 | `netvsc.c`/`vmbus.c` にpending trans_id保持、ring進行時再送、切断所有権。動作試験禁止・未検証。 |
| 4 worker/HTTP/TLS/H2再利用 | 実装・実ゲスト回帰成功 | 持続service、origin/partition pool、TLS再開、HPACK/H2多重化。1/4CPUとも正規TLSの同接続2streamが応答前に同時送信され、両200/本文成功。 |
| 5 HTTP cache | 実装・回帰成功 | 共有cache、鮮度/条件付き304/Vary/partition/cache mode、preflight取消。host196条件とトップ文書/iframe/真トップ遷移A/B/Aの実ゲスト回帰成功。 |
| 6 DNS/再接続 | 実装・実ゲスト回帰成功 | 候補API・TCP fallback・typed DNS、phase別安全retry。DNS/ARP/connect取消とservice48要求の回収も1/4CPU成功。 |
| 7 送信期限 | 実装・実ゲスト回帰成功 | ABI→kernel buffer待ちに絶対期限/取消/非同期prefix。1/4CPUとも満杯送信が499msで期限終了。 |
| 8 CPU別通信 | 実装・実ゲスト回帰成功 | BSP gateとsocket io_ownerでAPのbounded TCP copy。最終4CPUでAP受信917504bytes・CPU mask=6、close/free/同handle再利用競合成功。性能比較は下記。 |

## 実装箇所とレビュー根拠

1. `user/libc/webnet.c` の `pump` / `pump_slot` / `webnet_service` / `webnet_wait` と
   `user/apps/browser.c` の `browser_event` / `host_script_checkpoint`。
   要求32KiB、全体1MiB/3msの巡回予算と、GUI/通信FDの同時pollを実装。
   checkpointはauthor callbackを呼ばず、通常pumpへ1frame保持する。
   `user/include/webnet_wire.h` の双方向256KiB creditと64KiB単位補充でpause中の先読みを制限。
   3msはcallbackそのものの強制中断ではない。
2. `kernel/src/hv/netvsc.c` の `netvsc_callback` / `netvsc_poll_rx` と
   `kernel/src/net/net.c` のnet thread。IRQで受信を解析せず、threadが128message/128KiB/2msずつ処理。
   descriptor/range/RNDIS位置を次回へ保持し、通常frameは同期借用でstackへ配送して旧63packet stagingを通らない。
   初期化中のframe/keepalive保存は実受信buffer由来のメモリ予算内。drop/不正入力/予算終了を統計化。
3. `netvsc.c` の `complete_receive` / `netvsc_shutdown`、`vmbus.c` の
   `ring_write` / `vmbus_write_ready` / `vmbus_close_confirmed`。
   enqueue成功まで同じtransaction IDと共有buffer所有権を保持する。
   ring満杯時はpending_send_sz公開後に再確認し、IRQの固定spinを廃止。
   rescindで古いIDを失効し、明示teardown ACKなしの共有ページは再利用しない。
   切断後rebindと未確認ページの後日回収は未実装。既存他driverのclose契約は変更しない。
4. `user/apps/webfetch.c` の持続serviceと要求別channel、`user/libc/http.c` の
   origin/port/TLS/partition/全credentialによる16接続pool、`tls.c` のsession再開/ALPN、
   新規 `user/libc/http2.c` / `http2.h` のH2/HPACK。
   HTTP1は1xx、HEAD、CL、chunked/trailer、EOFとstaleを扱う。
   H2は同接続の並行stream、flow control、RST/GOAWAY、partial frame取消を実装。
   TLS engine lock中は非blocking prefix I/Oのみで、callbackはlock外。
   H2出力4MiB内に制御64KiBを予約し、DATA/HEADERSのframe全体が収まらなければ待機。
   streamを巡回し、HEADERS待機には空きを譲る。
   handshake取消hookはopening専用で、共有接続公開前にclearする。
5. `webfetch.c` の `cache_*` と `run_http`、`user/libc/web/js_fetch.js` / `js.c`。
   cacheの16MiB/1object2MiBは保存容量だけの制約で、Fetch受信上限ではない。
   完了したGETだけを公開し、要求はevictionから独立したsnapshotを所有する。
   鮮度、Age/Date/Expires、Cache-Control、Vary、ETag/Last-Modified/304、cache mode、
   Cookie/Auth/initiator origin/credentials partition、no-storeと取消を扱う。
   Cookie/Authは全重複値を長さ付きで保持し、Varyは欠落/空値/重複/順序変更を区別。
   公開webnetの既存duplicate拒否は維持。JS生成includeは正規js_embed.pyから再生成。
6. `kernel/src/net/net.c` / `dns_name.h` のDNS候補・TC TCP fallback・型付きstatusと
   256KiB byte-budget cache、`user/libc/tls.c` の候補順接続、`http.c` / `webfetch.c` のtyped retry。
   応答公開後やcookie更新後は再送せず、送信済み非冪等POSTも再送しない。
   持続workerのsetupはthread単位のchannel強参照でDNS/ARP/connectを取消可能にした。
7. `common/abi.h` / `user/include/nocturne.h` / `user/libc/net.c` の
   `tcp_send_until` / `tcp_send_some` / `tcp_cancel` とkernel send待機。
   絶対uptime期限、受付済みprefix、満杯時EAGAIN、切断/取消/timeoutを区別。
   部分送信をpeer ACK完了とは扱わない。TLSもkernelへの期限とpartial ACKへ接続。
8. `kernel/src/sys/sched.c` / `sched.h`、`net.c` の `net_ap_syscall` / BSP gate / socket io_owner。
   APはmapping確認済みの最大64KiB ring prefixだけを所有してcopyし、bitmask+IPIでBSPへ通知。
   BSPはgateを閉じAPの完了をjoinしてからmetadata/registry/freeを扱い、待機中はgateを開く。
   socket generationはclose/free/再利用を跨ぐ旧waiterの消費を拒否する。
   packet構築・NIC・protocol timer・allocator/wait queueはBSP所有のまま。
   したがって全TCP protocolが複数CPUへ分散したという主張はしない。
   `kernel/src/net/e1000.c` は有限受信batchでdescriptorを保持して継続し、
   net threadは複数batchを2ms/256KiBの協調windowへまとめる。空状態は直接待機へ入る。

追跡済みファイルの実差分は `build/network-eight-reviewed-tracked.patch`。
新規製品ファイルは `user/libc/http2.c` / `http2.h`、新規回帰は
`tests/{nettransporttest,transport8,transport8_host,dns_transport_hosttest,webfetch_cache_hosttest,webnet_pacing_cases}.c`、
`tests/check_webnet_pacing.py`、`tests/http_mock_lifecycle.h`、
`tests/http2_queue_hosttest.c`、`tests/net8_browser_observer.js`、
`tests/fixtures/dns_server.py`、`tests/fixtures/js/{h2_server,transport_selftest}.py`。
これらはpatchの追跡済み部分に含まれないため、作業ツリーの実ファイルも合わせてレビューする。
既存HTTP1 host fixtureの変更は `tests/http_mock_lifecycle.h` でcaseごとに借用static wireを
poolから解放するもの。実pool/H2の検証は別のreal socket/guest試験で行い、mockで代用しない。
旧close_notify待ち13msを期待していたbody試験は、正しいCL終端後poolへ返すため0msへ訂正した。
本文上限、切断、protocol errorの期待値を弱めたものではない。

## 実行・観測

- 基準画面: `python -X utf8 scripts/platform_qa.py --label net8-baseline-20261015 --url https://www.python.org/ --accel whpx --memory 4096 --cpus 4 --resolution 1920x1080x32 --seconds 35 --screen-interval 10`
- 基準媒体は修正前の既存 build。ハーネスが kernel/initrd と媒体内 payload の一致を確認。
- 初回35秒の4CPU実行は全画面がLimine読込、serial空、実解像度1280×800で未達。成功扱いしない。
- 起動観察時間を100秒に変更した1CPU `net8-baseline1cpu-20261015` と4CPU `net8-baseline4cpu-long-20261015` は1920×1080の本文へ到達。主担当が1CPUのscreen-2/4/final、4CPUのscreen-3を開き、Pythonロゴ、ナビ、本文、コード例の変化を見た。実クリック・スクロールはこの基準実行では未確認。
- 生成前の D ドライブ実空き容量は約75GiB。永続 `build/data.img`、ユーザーHyper-V媒体は試験へ接続しない。
- `python -X utf8 tests/fixtures/js/transport_selftest.py` はfixtureのHTTP1接続再利用・HEAD・chunked trailerとH2同一接続2streamを確認して成功。初回はselftestの変数名衝突を修理、次のsandbox実行はloopback接続がWinError10013で拒否され、限定した権限付き実行で成功。これはゲスト製品実装の検証ではない。
- 通常の `git diff --check` は既存CRLFファイルbrowser.cの追加CRを末尾空白と判定する。既存の `i/crlf w/crlf` を保ち、`git -c core.whitespace=blank-at-eol,blank-at-eof,space-before-tab,cr-at-eol diff --check` で実際の末尾空白を確認する。
- 元コードの再現: HEAD（3f726aca）から通信ソースとヘッダーだけを `build/network-eight-baseline/` へ取り出し、提供archiveの検証済み `root-probe-pump.c` を未変更でLLVMコンパイルして実行。最初のpumpは `frames=1 bytes=4096 available_bytes=8288 timeout=10 paused=0`、次も1フレームだけ。非stream対照は8240byteを一回で読み切った。これは元の不具合を検出するメモリpipe試験で、Hyper-V帯域実測ではない。
- 最初の通常allビルドはTLS API追加中の古いobjectで `ns_bytes_sent` 未定義参照となり停止（`build/network-eight-build-first.log`）。整合したソースで `network-eight-build-integrated.log` と `network-eight-build-phase2.log` は終了0、IMG/VHD/VHDX/ISOを生成。
- 1CPU実ゲスト初回: `scripts/test.py --accel whpx --qemu "C:/Program Files/qemu/qemu-system-x86_64.exe" --memory 4096 --cpus 1 --resolution 1920x1080x32 --timeout 480 --output-dir build/network-eight-guest1-initial nettransport networkpacing web/worker-network tcp`。
  worker通信・実pacing・bulk/lossの3群成功、DNS/期限群は試験ソースのTinyCC非対応 `__atomic_store_n` でコンパイル失敗。試験側を既存のx86 load/store方式へ修理。成功期待値の除外・緩和はしない。実pacingは1MiB/265chunks/38pumps、最大10chunks/pump、resume後drain5ms、全bytes/ENDと別要求完了を確認。媒体/serial/元失敗は同ディレクトリへ保全。

## 統合回帰の経過

- `build/network-eight-guest1-final` はJS Fetch/XHR/streamの3群とpacing成功。DNS群の実assertは全成功だがrunnerが説明文中のSERVFAILをFAILとして誤検出。説明だけを訂正し、型付きSERVFAILの期待値は維持した。H2負荷試験には別のTinyCC非対応atomicが残り、同じ既存方式へ訂正した。
- `build/network-eight-guest4-final` はCORS/通信境界、worker/cache、pacingの3群成功。上記と同じ試験側2不備を保存。
- 修理後の `build/network-eight-guest1-repaired` / `build/network-eight-guest4-repaired` はDNS/期限/SMP群成功。4CPUのTLS5項目（h2 ALPN、最初のfull handshake、同partitionのsession再開、別partition非再開）と通常HTTPS・期限切れ証明書拒否・webfetch TLSも成功。
- 両CPUのH2群は48項目中2項目が実際に失敗。大送信中の別stream期限200msと送信取消300msが、各141〜142ms/192msでIOエラーになった。queued=5488640、native送信受付=1310781。出力queueの残量がDATA frame未満でも追加して接続全体をENOBUFSにする境界を修理中。期待値や期限を緩めて成功扱いしない。
- 実H2の同接続2stream、192KiB flow control、150KiB upload、RST/GOAWAYの基本assertは上記実行で成功。ただし負荷時失敗が残るため全体完了とはしない。
- 通常ビルドは `network-eight-build-integrated.log`、`phase2.log`、`final.log`、`reviewed.log`、`goaway.log` が終了0。最後の媒体以降のsetup/preflight修理は再ビルド待ち。過去成功を未梱包の修理へ流用しない。

最後に `network-eight-build-cancel-budget.log` で全修理をall再ビルドし、以下の最新媒体で再検証した。

| 最新実行 | 結果 | 実際に確認した範囲 |
|---|---|---|
| `build/network-eight-guest1-accept/test-serial.log` | 7群成功・失敗0、FAT347files正常、QEMU終了0 | JS stream/Fetch298/XHR、DNS/期限/所有権、pacing/setup48、H1/H2、TLS handshake取消 |
| `build/network-eight-guest4-accept/test-serial.log` | 10群成功・失敗0、FAT347files正常、QEMU終了0 | native境界、worker/cache、DNS/期限/close競合、pacing/setup48、H1/H2、TLS取消、ALPN/session、HTTPS/不正証明書/webfetch TLS |

共通コマンドprefixは
`python -X utf8 scripts/test.py --accel whpx --qemu "C:/Program Files/qemu/qemu-system-x86_64.exe" --memory 4096 --resolution 1920x1080x32 --timeout 480`。
1CPUは `--cpus 1 --output-dir build/network-eight-guest1-accept nettransport networkpacing transport8 slop8/fetch-regression slop8/xhr-regression slop8/network-stream`、
4CPUは `--cpus 4 --output-dir build/network-eight-guest4-accept nettransport networkpacing transport8 networkboundaries web/worker-network net/tls-alpn-session-resume net/https net/https-rejects-bad-cert net/webfetch-tls`。

- H2は48条件失敗0。fixtureの `/multiplex` は2本が同じ接続へ到着するまで応答しないため、
  逐次取得だけでPASSにならない。最新4CPUのbackpressureは別stream200ms/送信取消300ms、
  queued=5436484・native accepted=1310781、型付きtimeout=4/cancel=8で期待通り。
- 最新pacingは1MiB全byte/ENDとpause中の別完了を確認。1CPU最大11chunks/pump・drain6ms。
  1/4CPUとも未応答setup48件が実service thread待機へ到達してから取消され、callback0・残存thread0。
  同serviceの正常取得は17/15ms。単なるsubmit後即取消で未実行を数えていない。
- DNS群は失敗候補→到達可能候補、TC→TCP、型付きNXDOMAIN/NODATA/一時エラー、取消を確認。
  channel FDをclose/dup2しても元endpointを保持し、thread exitで最終参照を解放。
  context取消中でも既存TCP dataは通る。
- close競合は実BLOCKED recvを確認→cancel/close/free→同slot再利用を強制し、
  旧buffer未更新・新応答32byte全量保持を確認。24回の最大64KiB enqueue対closeも実行。
- TLSの新規handshake/同partition再開/別partition非再開/h2 ALPNの5条件を実外部TLS接続で確認。
  silent TLS fixtureに対する250ms取消も成功し、元の30秒handshake期限は緩めていない。

補助hostログは `network-pacing/webnet-pacing-result.log`（16）、
`browser-performance-layout/webnet-scheduler-perf-result.log`（48）、`dns-transport-host.log`（161）、
`webfetch-cache-host.log`（146）、`transport8-host.log`（48）、
`http2-queue-before.log`（修理前2失敗）/`http2-queue-after.log`（修理後20）、
`tls-deadline-host.log`（51）。hostとguest、実サイトの証拠は相互に代用していない。

## 性能の扱い

4CPU/WHPX/4096MiB/1920×1080、他VMとbuild停止状態で旧媒体と当時最新媒体の
通常TCP全payload/loss試験は成功。clean 4MiB downloadは25→24ms、uploadは62→72ms。
loss試験はdrop件数も異なり、全体改善・回帰なしの根拠には使わない。
追加依頼に従い、同じ検証コードでclean転送を各boot5回、旧/新それぞれ複数bootで
比較する。通常loss回帰は削除せず、`tcpperf/clean-repeat` を別に追加した。
基準source snapshotの変更はこの追加計測用 `tcptest.c` / `runtests.c` だけで、
製品ヘッダー・実行媒体は3f726acaのまま保持する。

旧媒体の独立した3boot×5回（`network-eight-perf-baseline4-{b,c,d}`）は全payload成功。
download中央値27ms（25〜29、標本標準偏差1.25ms）、upload中央値80ms（77〜85、1.79ms）。
追加群の初回 `-a` はDHCP待ちを適用し忘れ、全接続がENETDOWN相当で失敗した。
既存TCP群と同じDHCP待ちを追加してから上記を採用した。最初の62msと後の72msだけで
退行や改善を判定しない。新媒体の同条件反復測定は最新回帰後に行う。

最新媒体の最初の2bootはULが[94,96,88,97,91]、[94,90,88,95,93]msとなり、
旧中央値80msより遅い傾向が再現した。DLは[31,25,25,29,24]、[31,23,24,26,24]ms。
機能試験成功と区別して送信性能の退行として扱い、AP enqueueからBSP送信へのhandoffと
公平なyieldの境界を調査・修理する。3回目は計測後だけ既存CPU別counterを読む診断を追加。
timed payload/接続/送受信条件を変えず、通常loss試験も維持する。

3boot15回の最初の新媒体はdownload中央値26ms（23〜33、標準偏差3.02）、
upload93ms（88〜97、3.03）。旧upload80msから16.25%の退行が再現した。
3回目の計測終了後counterはcpu1 calls2595/fallback1575/TX4669552/RX11808800byte。
AP処理は実際に通っており、送信copy後の通知を実行中consumerへ繰り返す引継ぎが修理候補。
consumerのsleep arm→pending再確認→待機と、sleep中だけdoorbellを送る方式で修理する。
公平yieldやNIC/IRQのBSP所有を除去して速度を作ることはしない。

`network-eight-build-doorbell.log` の通知修理媒体は4CPU UL[93,92,87,92,89]msで、
中央値92ms。退行解消とは認めない。次の切り分けとして旧/新を同じ1CPUで比較した。
`perf-baseline1-a` はUL[69,69,72,68,71]（中央値69ms）、`perf-doorbell1-a` は
[68,66,67,68,67]（67ms）。1CPUのDLは旧18〜20、新20〜21ms。
UL退行がAP経路へ限定される根拠として、gateの実contention、AP fallback理由、
AP enqueue→BSP push、net taskの課金/yieldをcompile-time限定で計測する。
診断は明示net_stats取得時のsnapshotだけを出し、出荷時に常時計測を残さない。

`perf-handoff-diag4` の限定観測は全payload成功。gateは1回/611cycles（約0.13µs）、
AP最初enqueue→BSP pushは275回/41757299cycles（合計約8.9ms、max約95µs）。
約0.662秒の5回転送中にnet taskが2465回yieldし、うち空状態でも333回readyへ再投入。
この過剰なscheduleを主因候補とし、次のready確認後だけ2ms/256KiBの協調境界でyieldする修理を実装。
空なら直接wq待機へ入り、e1000も64frame/128KiBごとにdescriptor所有権を保って継続する。
実際の応答上限は2ms window＋単一の有限NIC batchであり、パケットごとの時計読取りは追加しない。

## 最終レビューで見つけたトップ文書境界

同じchild originがtop A→top Bへ再埋込みされると、旧keyはinitiator/credentialsだけで一致した。
ブラウザーのservice/cacheは遷移後も残るため、これは要求されたpartition分離の不足として修理する。
trusted top URLの8byte flagged extensionを追加し、既存64byte wire prefixは保持。
通常Fetch/資源はtop canonical originとinitiatorを別々にkeyへ含め、cache/pool/TLSへ同じkeyを渡す。
Cookie siteにもtop URLを使い、credential/SAME_ORIGINとimage securityのorigin判定はinitiatorを保つ。
opaque topは保守的に要求別隔離しcache読書きを行わない。旧native caller未指定はdocument由来fallback。
真のtop navigationでは各redirect hopのtargetをtop keyにし、iframe相当NAVIGATIONはtrustedtopを保持する。
後者の区別は現browserの真topnavが常にuser_navigation=true、frame requestがfalseである実装を根拠にする。
HTTP cache host196条件と親側host pacing16/scheduler48は成功。
同child/top A/B/Aの取得1/2/1、異topの接続分離と同top復元、Cookie top境界、iframe相当を実ゲストで追加確認する。

## 実画面・実操作（cancel-budget媒体）

`python -X utf8 scripts/build.py -j 8 --log build/network-eight-build-cancel-budget.log all`
は終了0。以下はその最新IMGから隔離した2VMを同時起動したもので、ホストブラウザーではない。
両方WHPX/4CPU/4096MiB/1920×1080、maximized、480秒以内、20秒間隔撮影。
ready marker/defer-screen-pngなし。読み取り専用 `tests/net8_browser_observer.js` を
`/data/tests/`へ配置し、URL/scroll/focus/trusted inputだけを観測した。
初回のcurrent-startは両VMともLimine/initrd読込み中で、主担当がその時点でも開いて確認した。

- `build/nocturne-platform/net8-final-python/`：Python.orgのlogo、carousel、nav、本文、画像と
  下の4columnを確認。consoleをF12で閉じたbody-top原本を目視。
  inputの実rect→native pointer到達→hover INPUT→実clickを確認し、実キーでthreadingを入力・Enter。
  `https://www.python.org/search/?q=threading` の入力値と検索結果をsearch-result/search-bodyで目視。
  実hoverのstatusbarとtargetを確認後Thread-SIGリンクをclickし、正しい
  `/community/sigs/retired/thread-sig/` の本文をlink-resultで目視。
  Alt-Left→検索結果、Alt-Right→Thread-SIGをhistory-back/history-forwardで確認。
  本文の空きDIVを実clickしてPageDown→899、PageUp→0。
  body-downで末尾のlist/footer、body-upで元の見出し/本文へ戻ることを主担当が目視。
- `build/nocturne-platform/net8-final-mdn/`：Fetch APIの見出し、3column、logo、SVG/icon、広告画像、
  code font、scrollbarをbody-topで確認。本文Pを実clickしPageDown→899で
  Deferred Fetch/Interfaces/HTTP headersが現れたbody-down、PageUp→0のbody-upを目視。
  PS/2ホイール-8で48px下、+8で先頭へ戻るwheel-down/wheel-upを目視。
  Using the Fetch APIへの実hover targetを確認してclickし、正しいUsing_Fetch URLと
  新見出し/本文/code例をusing-fetch-linkで確認。Alt-Left後のhistory-returnも目視。

上記すべての代表画像は主担当自身が原本PNGを画像ツールで開いた。
本文を覆うconsole画像も別に保持し、本文用と混同しない。
この受入範囲で本文・検索・入力・リンク・戻る/進む・上下scrollが成立。
MDN全機能、動画、全サイト互換性の合格は主張しない。
両ハーネスはcontrol stopで終了0。Hyper-V操作なし。

## 一時媒体の回収（途中）

各metadataのprocess_stopped=true/user_data_attached=falseと解決済みbuild内pathを確認し、
旧12回分の `test-{boot.img,data.img,kernel.elf,initrd.tar}` 計48個、論理10.91GiBを回収。
対象はguest1-initial、guest1-final、guest4-final、guest1-repaired、guest4-repaired、
baseline-tcp4-fixed、baseline-tcp4-isolated、current-tcp4、perf-baseline4-a/b/c/d。
serial/metadata/画面/失敗ログ、3f基準boot、最新配布媒体、通常build/data.imgは保持。
回収直後のD実空きは61→72GiB。削除した試験媒体が保存済みとは扱わない。

続いて停止済み14実行の専用 `test-{boot.img,data.img,kernel.elf,initrd.tar}` 56個、
論理12.73GiBを同じ条件で回収した。対象はguest1/4-accept、perf-current4-a/b/c、
perf-doorbell4-a、perf-baseline1-a、perf-doorbell1-a、perf-handoff-diag4、
perf-budget-diag4、perf-release4-a、perf-ring4-a、perf-paired-old/new4-a。
小さいserial/metadataは残し、D実空きは58→71GiB。

## AP性能修理の比較（継続中）

過剰yieldを除いた限定診断 `perf-budget-diag4` はyield21回（空状態0）、
UL中央値85ms、DL28msとなった。診断OFFの `perf-release4-a` はUL83/DL30ms。
e1000のbatchを64frame/128KiBから有限の全256descriptorへ戻した
`perf-ring4-a` はUL85/DL29msで、退行解消とは扱わない。

ホスト時間差を排除するため旧/新を連続して再比較した。
`perf-paired-old4-a` はDL[29,26,27,26,28]、UL[80,81,81,81,84]ms。
直後の `perf-paired-new4-a` はDL[34,28,28,29,29]、UL[86,86,85,81,84]ms。
旧中央値DL27/UL81、新DL29/UL85で差が残るため修理を継続する。

ソースと実ELFで、CPU別単独writerの統計へ不要なlocked RMWを払い、
gate/active/pending/sleepingの4個が同じ64byte cache lineに配置されることを確認。
所有権CASと通知のSC順序は維持し、このcoherence費用を減らす。
診断は `NET_HANDOFF_DIAG=0` が出荷設定で、常時rdtsc/ログを有効にしない。

coherence修理後の `perf-coherence4-a` はDL[41,30,28,28,29]、
UL[85,87,84,82,82]msで、まだ解消しなかった。
AP由来TX/受信窓ACKだけをRX前に進める共通helperを追加した。
早期処理全体を64frame/128KiBに限定し、残りは通常のRX後flushへ渡す。
`perf-output-order4-a` はDL[30,26,26,27,28]（中央値27msで旧と一致）、
UL[86,87,86,84,84]ms。送信側の修理は継続した。

実net.oのdisassemblyで `csum_add` の8byte loopごとに `memcpy` 関数呼出しが
残ることを確認。固定幅8/4byteの2箇所だけを `__builtin_memcpy` にし、
unaligned/alias安全性を保った直接loadへ変更した。通常checksum計算は変えない。
独立したbyte-pair golden、全64alignment、0〜1024長、MSS/page/16KiB/65535境界、
奇数長、6seedと全00/ffを含むhost照合は成功。既存DNS検証も維持した。
`build/dns-transport-host.log` は797346条件失敗0。
独立target objectではloop内call消滅を確認。`network-eight-build-checksum.log` all終了0。
WHPXで性能と実payloadを確認した。

最終 `perf-checksum4-a/b/c` の独立3起動×5回は全payload成功。
DL中央値24ms（24〜29、標本標準偏差1.86）、UL81ms（79〜84、1.60）。
旧3起動×5回はDL27ms（25〜29、1.25）、UL80ms（77〜85、1.79）。
時刻を挟んだ旧版再測定 `perf-paired-old4-a` はDL27/UL81ms、
`perf-paired-old4-b` はDL26/UL79ms。新ULの1ms単位の差とばらつきもそのまま残す。
初期の一貫したUL93msへの退行は解消したが、全条件での無回帰を保証する比較ではない。
1CPUの旧 `perf-baseline1-a` はDL18/UL69ms、最終 `perf-checksum1-a` は
DL17ms（16〜18）/UL62ms（61〜66）。いずれも5回、4MiBを両方向へ転送し全byteを検証。

すべてWHPX/4096MiB/1920×1080、CPU数以外の条件、faults=0、検証sourceは同一。
測定中は他の本作業VM・build・host compileを停止。ホスト全体を専有したとは主張しない。
新4CPUの各bootでAP TX約9.8〜10.0MiB、RX約12.6MiBを実際に処理した。
1CPUより4CPUが常に速いという主張や、全TCP protocolのSMP化という主張はしない。

## 最終媒体の一致

`network-eight-build-checksum.log` の通常allビルドは終了0。
IMG/VHD/VHDX/ISOを生成し、`qemu-img compare -f raw -F vpc` と
`-F vhdx` はともに `Images are identical.`。
initrd内のbrowser、browserjsworker、webfetchと公開4headerがbuild/sourceとSHA256一致。
出荷kernel内にNETDIAG文字列はなく、限定診断は無効。

| 媒体 | SHA256 |
|---|---|
| kernel.elf | `157a66fd53f25be33c620f17baf844eb4b3cf97a0ce2e4a2c683d5434e6ce8c7` |
| initrd.tar | `ee99899070fd2747a4c365e4ddbb975d69108772486b2c380a030d82fc2bd668` |
| nocturne.img | `8465dcc19100efad45208428d94e4de95efb812520165461d0a30397366db8a4` |
| nocturne.vhd | `4f559a3e2d54742ba92685713434cd78f0f33a2b26db03838261b10937ba51bb` |
| nocturne.vhdx | `e487b83d455c39cf27804a985585a8ffd58c6bbaeccf0883102605a8abdf38b2` |
| nocturne.iso | `80144132a8a0a42557422b4b4a742bdfa16b6f40fd92160679b338648ebf0f3a` |

TLS上の2要求試験は同接続ID/異stream IDに加え、最初の応答headerが届く前に
2件目の送信受付が成立する同期へ強化した。2つのread-only GETだけを送り、
既存stale retryが追加要求を送ろうとしたら試験側cancel hookで止める。
host fixtureも2実streamが揃うまで応答しない。モデル10条件成功
（`build/transport8-tls-h2-model.log`）、実TLSも下記最終QEMU群で成功した。

## 最終媒体での実ゲスト回帰

| 最終実行 | 結果 | 根拠 |
|---|---|---|
| `build/network-eight-guest1-verified/test-serial.log` | 9群成功、失敗0、FAT347files正常、終了0 | JS Fetch/XHR/stream、DNS/期限/close寿命、pacing/cache区画、H1/H2/負荷/TLS取消、TLS再開＋同時2GET、通常bulk/loss |
| `build/network-eight-guest4-verified/test-serial.log` | 11群成功、失敗0、FAT347files正常、終了0 | 上記native通信＋worker/cache/CORS境界、HTTPS/不正証明書拒否、実webfetch TLS、bulk/loss |

共通prefixは `python -X utf8 scripts/test.py --accel whpx --qemu "C:/Program Files/qemu/qemu-system-x86_64.exe" --memory 4096 --resolution 1920x1080x32 --timeout 480`。
1CPUは `--cpus 1 --output-dir build/network-eight-guest1-verified nettransport networkpacing transport8 slop8/fetch-regression slop8/xhr-regression slop8/network-stream net/tls-alpn-session-resume tcp`。
4CPUは `--cpus 4 --output-dir build/network-eight-guest4-verified nettransport networkpacing transport8 networkboundaries web/worker-network net/tls-alpn-session-resume net/https net/https-rejects-bad-cert net/webfetch-tls tcp`。

- 両方の実TLSはconnection=1/1、stream=1/3、h2=1/1、status=200/200。
  accepted=82/82、second-before-first-headers=1。再開/別partition分離も含め15条件成功。
- 最終4CPUの満杯送信は1311048byte受付後499msでETIMEDOUT。
  H2負荷は別stream200ms・送信取消300msで期待したtyped error。接続全体は壊さない。
- 最終4CPUの実AP受信は917504byte、mask=6（2つのAP）。旧世代receiver/同handle再利用と
  24回のenqueue対close、setup channelのFD差替え/exit寿命を確認。
- A/B/A cache/接続復元、iframeと真トップ遷移の区別、Strict Cookieのtop境界が両CPUで成功。
- pacingは1MiB全byte/END。1CPUは最大8chunks/pump、drain5ms。
  2VM同時機能試験中の4CPUは最大8chunks/pump、drain46ms。これを隔離した性能測定と混同しない。
  setup48件は全て実待機へ到達後取消、callback0/live thread0、後続取得17/15ms。

Hyper-V VM、構成変更、専用ランタイム試験は実行していない。
項目2・3の実動作、およびHyper-V Enhanced Sessionの実機受入は指定通り未検証。

## 最終媒体の実画面と実入力

`platform_qa.py` を `--label net8-verified-python` / `net8-verified-mdn`、
`--accel whpx --memory 4096 --cpus 4 --resolution 1920x1080x32 --seconds 360 --screen-interval 20`
で並行実行。`--guest-file tests/net8_browser_observer.js`、
`--command "browser --debug-js --maximized --console-file-each-document --console-file /data/tests/net8_browser_observer.js URL"`
を指定した。URLはPython.orgとMDN Fetch_API。ready/defer指定なし。
両metadataのkernel/initrdは上表の最終SHAと一致する。

主担当自身が両VMのcurrent-start（Limine）、loaded-console、body-topをまとめて開いた。
1920×1080の本文、画像、font、SVG/icon、column、scrollbarを確認。
さらに以下の原本PNGも主担当自身が開いた。

- `build/nocturne-platform/net8-verified-python/` のsearch-body、focus-check、search-down、search-up。
  実INPUT hover/click→threading実入力→Enterで `/search/?q=threading` の結果と入力値が表示された。
  本文focus用の移動中に開いたメニューのリンクを誤クリックし、Scientificの本文へ遷移した。
  これは意図したfocusの成功には数えず、Alt-Leftで検索結果へ戻した。
  新たにDIV#contentへのhover/clickを確認してPageDown→899、PageUp→0、
  下の検索結果と元の先頭を画像で確認した。
- `build/nocturne-platform/net8-verified-mdn/` のbody-down、body-up、using-fetch-link、history-return。
  本文Pの実click→PageDown→899でInterfaces/HTTP headers、PageUp→0で元の3columnに戻った。
  実hoverのUsing_Fetch targetを確認後clickし、正しいURL・見出し・本文・codeへ遷移。
  Alt-LeftでFetch_APIの本文へ戻った。画像も正常に表示された。

MDNはcontrol stopで終了0（320.79秒）、Pythonは観察期限で終了0（360.30秒）。
全QEMU停止をmetadataで確認。例外/ChunkLoadError/通信status0に該当する記録なし。
MDNの既存sendBeacon未対応warnはfetch fallbackと200が記録され、消していない。
今回確認した範囲の受入であり、全サイト・動画・Hyper-Vの合格へ広げない。

## 最終回収と引渡し

最後に停止済み専用9実行の `test-{boot.img,data.img,kernel.elf,initrd.tar}` 36個、
論理8.19GiBと、専用platformのboot/data 13個、論理3.125GiBを回収した。
前者はperf-coherence4-a、perf-output-order4-a、perf-checksum4-a/b/c、
perf-paired-old4-b、perf-checksum1-a、guest1/4-verified。
後者はbaseline短時間/1CPU、final/verifiedのPython/MDNのboot/dataと、
baseline4cpu-longのdataだけ。解決済みbuild内path、通常file、停止済み、
user_data_attached=falseを先に確認した。回収直後の実空きは61→73GiB。
全段階合計の回収媒体は153個・論理約34.96GiB。疎性・一時overlayにより実使用量とは異なる。

3f726acaの基準bootとsource/manifest、最終IMG/VHD/VHDX/ISO、通常build/data.img、
小さいログ・metadata・代表PNGは保全した。削除した試験mediaを再実行可能な保存物とは扱わない。
本作業のbuild/test/QEMU/fixture serverは終了済み。push/commit/他人の差分取消なし。

8項目は製品実装済み。項目1/4/5/6/7/8は最終WHPX実回帰と実画面の範囲で確認済み。
項目2/3は指定通り通常build・静的レビューのみで、Hyper-V動作は未検証。
残存の明示制約は、netvsc切断後の自動rebind、ACK未確認共有ページの後日回収、
完全な全祖先SameSite判定、全TCP protocolのSMP化（現在はAP ring copy＋BSP protocol所有）。
これらを今回の成功範囲へ含めない。権限・外部接続による未解決阻害要因はない。
