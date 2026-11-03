# 通信スケジューラー独立レビュー

読取り専用で `user/libc/webnet.c`、`user/include/webnet.h`、`http.c` のURL解析、`js.c` のFetch callback経路、関連guest試験期待値を確認。VM/試験の再実行、製品編集はしていない。

確認snapshot: webnet.c SHA-256 `ec7b24009693cc26cb97039e614b2a4e86817354208aae8615e5ecc808d148a6`。以下の行番号はこのsnapshot。

## 要修正の反例

1. **低RAMでbudget縮小後も新workerを開始する。** `worker_limit`更新は527行、開始条件は536行のslot番号比較だけ。16→1へ縮小しslots 1～15が生存、slot 0が空なら新workerを開始する。既存超過分をdrainするという説明に対し、新規admissionが停止せず、低メモリreserveの実効制御にならない。実生存数を比較して超過中は新規を止める必要がある。
2. **parked transportはglobal budgetから外れる。** 239～240行ではorigin数に含まれる一方、533～543行ではparked child/process/TLS状態をglobalの開始条件へ含めていない。異なるoriginのpaused streamを順に作ると、生存child数が最大32を越える。pausedは通信backpressureが効くが、CPU/TLS/stack所有を解放したわけではない。global資源admissionにも生存parkedを扱う必要がある。
3. **limit 1の混在queueではFetchが飢餓する。** 255～259行はslot 0でclass 1→2→3の固定順序。class 1/2が継続供給されるとclass 3は無期限にdispatchされない。Fetchだけなら進行するが、1～3の公平性を持つ以前の複数slot経路から低RAM条件で回帰する。limit 1ではnavigation優先を保ちつつ1～3をround-robinにする必要がある。

## 既存境界・保証範囲

- callback generation取消: 456行のstream callbackから `webnet_cancel_generation` を呼ぶと、565～595行の同期completeが現requestを解放し、457～458行とpumpの後続request参照が解放後使用になる。native callback APIはsubmit/cancelを許可しているため寿命を安全化すべき。ただしこれは本差分以前の境界でもあり、`js.c:5726`の実Fetch経路はpump内でauthor JSを実行しない。現在のJSでの再現を主張しない。
- 60行のCPU budgetは `MAX(8, online*4)` で、1CPUでも8。CPU×4という説明なら4と一致しない。意図した最低並列なら明記が必要。
- originキーはsubmitの初期URL（173～181行）でありredirectのsocket宛先へ更新されない。複数初期originが同じ最終originへredirectする場合、その最終originの実HTTP接続6本を保証しない。初期originのdispatch制限という範囲と区別する。
- eligible class内ではqueueの先頭から探すため、同originでblockedな先行要求を別originが追い越す。これはhead-of-line回避として妥当で、厳密な全class FIFOではなく実行可能要求のFIFO。

## 確認できた保全

- origin登録が無いopaque/file/不正URLはworkerの既存navigation/error policyへ届けられ、URL解析がadmission拒否の代用ではない。今回のfile遷移修正方針と一致。
- origin集計は通常slotとparked双方を含み、Fetch/Reportは初期originで5まで、文書依存は6まで。
- budgetを越える番号の生存slotも全32slotのpump/reap対象のまま。slot配列の縮小・生存request破棄はしていない。
- generation取消ではkeepaliveを保全し、個別取消は非blocking reap、generation取消は同期終了という既存契約を保つ。
- request送信buffer/response payloadとqueued_bytes、countの終了時処理は既存completeに集約され、deadline queue callback後はqueueを再読する。
- guest試験の16384 URL/16MiB body/64KiB header/30秒拒否の旧期待値は現在のUINT32 wire/dynamic storageと一致しない。現在の完全URL/正確body/35秒完了/late不正Content-LengthとCookie非公開を期待する訂正は、現在の実装仕様を弱めるskipではない。

## 結果と次

上の3反例とcallback既存境界を主担当へ直接通知。主担当の修正と独立guest確認を待つ。本レビューだけで通信の性能改善・無回帰・試験成功とはしない。
