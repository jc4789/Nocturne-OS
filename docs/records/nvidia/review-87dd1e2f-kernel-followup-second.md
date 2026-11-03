# native 実行結果と channel 試験順序の追補・第二検討

担当 `gpu_kernel`。追補第一報の「試験修理が製品回帰を隠していないか」を再検討した。

- flow 判定は次 message の長さではなく、既に待つ byte 数に対して行う。
  空 queue は任意の大きさを一つ受け入れ、その後は受信で流量を戻すという既存契約に一致する。
  長さ0だけを例外にする製品修理は、この原因を隠すだけなので採用しない。
- 巨大 message の境界、PEEK 非消費、短 buffer 非消費、全量内容一致はすべて残す。
  空 message の境界も、巨大 message の消費後に独立して検査する。同じ期待値の正当な順序訂正である。
- 今回追加した sibling-thread send は queue を空にしてからの6byte、継承 endpoint の child send は5byte。
  いずれも今回特定した flow 閾値による自己停止を新たに作らない。
- credential API は message 本文とは別に kernel が実 send 主体を記録する。
  この試験順序変更で endpoint creator を主体として扱ったり、申告 PID を信頼したりはしない。
- pcitest の0失敗は native 共有/取消と edu の証拠であり、実 NVIDIA の memArea、VRAM/cache 効果や
  任意競合全網羅を証明するものではない。nativechanneltest の新実行成功へも流用しない。

製品 source は前の第二報後に変更せず、変更したのはこの試験の送信位置だけ。
focused 構文・diff 確認は0だが、親が最新の試験をゲストへ配置し実行結果を確認するまで完了とはしない。
