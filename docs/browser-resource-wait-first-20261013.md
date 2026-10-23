# 通信待機の共通進捗修理：第一報

## 実観測と切り分け

主担当の `build/nocturne-platform/compat-dom-subtree-baike-after-20261013/serial.log` では、stylesheet要求 id=11 の queue→dispatch は157ms、dispatch→complete は20,071ms。Resource #3 は `connect ... timed out` でHTTP 0、20,241ms。この資源失敗を、単にブラウザーの16枠待ち、gzip不足、またはDOM長タスクと断定しない。後段の画像にはqueue待ちが数十秒のものもあり、各層の期間は別である。

## 修理した製品境界

`kernel/src/net/net.c` のTCP connect/send/recvはすべて共有 `net_wq` へ待っていた。ACK、順序通りの本文、FIN、RST、resetが同じ共有queueをwakeし、一つの接続の進捗で、別のTCP helperとDNS/UDPの待機者まで再実行していた。動的TCBを保全し、その安定したメタデータに `progress` wait queue を置いた。各TCPイベントは該当接続だけwakeする。UDP/DNS/ARPの共有進捗は今回は不変更。

`tcp_free` は待機者を先にdetachしてからメタデータをclearする。既存のgeneration・user_open判定、process所有、FIN/TIME_WAIT tupleの寿命、動的poolとOOMエラーを保つ。新しい接続を待つためにclosing slotを盗まない。recv期限の確認と残量減算には同じ時刻を使い、二回の時刻読出しが期限を跨いだ際のunsigned下溢れも防いだ。20秒connect期限の延長や全GET再試行は追加しない。

第二考察で同じ二重時刻減算がUDP recv、DNS queryの外側、ICMP待機にも残っていたため、同じ一回の時刻スナップショットへ統一した。特にDNSは負の残量をudp_waitへ渡すと無期限待機の意味になっていた。DNSの2秒×3回という従来試行条件は変更しない。

## 確認と限界

- kernel製品ソースと `tests/webnettest.c` のtarget構文確認、CRLF対応diff確認は成功。
- 既存 `--tcp-capacity` に、siblingのblocked recv、close wake、別接続の進捗、旧generationのEBADF拒否を5条件追加した。既存24接続×2群の期待は不変更。
- 新条件のguest実行、完全媒体での同条件実サイト・スクロール確認は主担当調整待ち。担当は追加VMを起動していない。
- これは不要な全接続wakeを修理した証拠であり、サイト表示の速度改善や外部CDN connect timeoutの解消を実測したものではない。

上のguest待ちは第一版時点の記録。後続の統合nativeでTCP追加5条件は主担当が成功を確認した。後続TLS修理の実サイト速度まで確認済みと読み替えない。

## 後続のTLS確立と位相診断修理

主担当の `build/nocturne-platform/compat-dom-dispatch-net-baike-quoted-20261013/serial.log` では、id=2のscriptがqueue13ms、dispatch後122,189msで `TLS: connection error`。後のhome用scriptには4,117,334Bを101,807msで取得できた例もある。失敗だけ早く返すことを、この成功取得の速度改善と混同しない。

`ns_open` はTCP接続の20秒だけを制限し、続くBearSSL handshakeの各record readへ本文用の長いtimeoutを渡していた。`tls.c` はTCP開始時の既存予算をTLS確立も含む絶対期限へ接続し、短い呼出元期限も保持した。public readとcloseの内部record反復にも一操作の期限を渡し、record到着ごとの再延長を防ぐ。本文の長時間read、明示-1、ns_set_timeout(0)の非blockingは維持し、ns_open(-1)も本文で-1を保持する。暗号検証・ALPN・root store・サイト別処理は変更しない。

EOF、transport errno、BearSSL errorをIO失敗時に保持し、handshake失敗と本文失敗を区別する。debug時だけ既存private metadataへresolve/TCP/TLS/最初の最終status line/全HTTP header/body期間・最終transport phase・attempt数を出す。redirect/reset既存再試行は期間を集計し、HTTP request/hook/wire structのlayoutは不変更。`http.c` の既存innerを共用するopt-in entryを追加した。script/native navigationにも既存debug bitを許可し、metadataはnative callback前に検証・除去する。Cookie、URLquery、本文の診断保存は行わない。

### 支持確認

- TLS deadline host: **37条件、0失敗**。実tls.cのcallbackと期限を使い、native transportとBearSSL I/Oの駆動部分は代替。TCP＋handshakeの短い共有予算、6000msずつのrecord到着、本文180秒、-1、zero read、closeの300msずつ到着、EOF/ECONNRESET、plainを確認。実TLS認証や実サイトの証明ではない。
- HTTP timing/header host: **12条件、0失敗**。既存実HTTP parser/inner、合成streamと時計。新opt-in期間と従来headerバッチの期待を確認。
- private timing host: **9条件、0失敗**。実webnet parser/unpack。通常header保全、失敗時空header、範囲/overflow拒否、bodyの元offset保持とdiagnostic除去を確認。
- 最初のMSVC host link不足、診断hostの未定義native mock、PowerShellによるcomma引数解釈はホスト検証経路の不備として訂正した。製品の失敗や成功と混同しない。旧mockの追加API seamを更新しただけで、元の期待を弱めていない。
- 新完全媒体のguest/実サイトは主担当調整待ち。担当は追加VMや全体buildを起動していない。

### 凍結後の独立査読への回答

producerは既存header prefixの末尾へ `\r\nHTTP/Nocturne-Transport ...\r\n` を追加する。statusのみ、status＋meta、status＋meta＋実headerの各prefixはもともとの終端CRLFを持つので、`line` は追加した新CRLFを指す。`memmove(line,p+2,...)` はその追加部分だけを除去し、元prefixとbyte一致。空の失敗headerでは診断除去後も空になる。これは末尾追加の契約であり、中間挿入へ将来変更する場合は同じ削除規則を流用しない。

attemptsの42は要求数の人工的な容量制限ではない。既存 `run_http` のhop 0〜20（初回＋20 redirect）による最大21回に、各hopのbodyless GET/HEAD・未公開reset再試行がif一回だけで最大2回なので、private記録の正当な最大は21×2＝42。preflightはこのcounterへ加算しない。新しい再試行やredirect上限は追加していない。
