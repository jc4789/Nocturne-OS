# 通信スケジューラー独立レビュー 第2報

## 修理と検査範囲

主担当が第1報の3反例を修理した後、実製品の `worker_budget`、`origin_available`、`live_workers`、`available_class`、`take_request` と実request/slot/net定義を無変更で抽出して限定host検査を実施した。

- source SHA-256: `8c55823434826ee09f285daf44660e9cb9e045ee6a0ad000a35cbb8ad7af5007` (`user/libc/webnet.c`)
- 無変更抽出部分SHA-256: `4d548bde8681385511188e07d6d898945505628f1e4adfbd3210aee79196d18e`
- 検査元: `tests/webnet_scheduler_perf_cases.c`、実行補助: `tests/check_webnet_scheduler_perf.py`
- 生成C、実行ファイル、source/hash/command/exit証拠は `build/browser-performance-layout/webnet-scheduler-perf-*`。製品本文は変更していない。

## 実行と結果

`python -X utf8 tests/check_webnet_scheduler_perf.py` を一度実行。compile exit 0、runtime exit 0、**追加26件成功・0失敗**。

- CPU/RAM予算の現在値、最大32、空きRAMからのreserve、情報取得失敗、最小1の進行枠。
- budget 16→1の縮小時、slots 1～15の生存childを数え、slot 0が空でも新規admissionを禁止。cancelled/finishingでもPIDをreapするまでは物理reserveに含む。reap後はadmission回復。
- parked 30＋active 2を同じglobal 32として扱う。最後のreserveはFetchへ渡さずnavigationに渡す。reserved laneが実際に埋まった後の余剰capacityはFetchへ渡す。
- limit 1でscript/resource/Fetchを継続補充し、18回のdispatchで各classが6回ずつ進行。eligible class内のFIFOとnavigation優先を併せて確認。
- parked Fetch 5本で初回originの第6接続を文書依存へ残す。blocked originを別eligible originが追い越す。初回origin全体6と、cancelled socketと未reap PIDの違いを確認。
- opaque/local URLのorigin bookkeepingはadmission政策の代用品にならない。

新しい境界だけを調べ、既存network全面guest、以前の26以外のsuite、VM/実サイトをこの担当では再実行していない。CPU/RAM/syscall値はhostの決定的代用品であり、実physical memory圧力の計測ではない。

## callback寿命の再読

- stream callback中に `in_callback` を立て、generation取消ではPIDを停止・reapした後も現在responseのloanを解放しない。callback帰還後にbufferを解放し、外側pumpでcancelled requestをcompleteする経路を読取確認。
- nested `webnet_pump` は `pumping` guardでreturnし、同一requestの二重consume/reapを避ける。
- callbackの実guest新分岐は主担当所有で、この26件には含まれない。作者JSが通常tickで実行される現経路とnative callback API境界を区別する。

## 判断と限界

第1報の3反例は上の実製品関数検査では解消した。検査した修正範囲に追加のadmission/fairness反例は見つからなかった。実サイトの速度改善・native socket上限・GUI無回帰・guest callback成功をこの検査から主張しない。

現在のCPU policyは最低8を含む `MAX(8, CPU*4)`。origin上限は初回request originであり、redirect先の実socket数6を保証する仕組みではない。この設計範囲は主担当が最終説明へ明記する。
