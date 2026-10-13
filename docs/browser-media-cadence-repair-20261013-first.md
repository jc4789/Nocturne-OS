# 媒体協調供給の公平性修理：第一報

- 担当：placeholder_paint_repair。対象は既存 `user/libc/web/avmedia.c` の協調供給だけ。全体ビルド・実QEMU受入は主担当。
- 原失敗：`build/nocturne-platform/compat-wheel-maximized-baike-article-20261013/serial.log` の最終ループは84,901ms、うちホストlayout2回79,110ms。8回の実wheelは全部allowed=1、coherent=1、root範囲12,292px、遅れて0→192→0へまとめて進んだ。取消やscroll extent不足ではない。
- 源の原因：`web_avmedia_service()` は1/5msの適応供給間隔を供給開始時計で測っていた。2ms予算の非blocking IPC収集・動画patch自身でその間隔を使い切ると、次のnative layout safepointが即座にもう一度供給し、呼出元の進行を圧迫する。
- 修理：既存service出口で完了時計を記録。2ms共有予算・PCM不足時1ms／通常5ms・registry巡回・pump・snapshot・世代・PTS・取消を変えず、供給処理自身を次の待間隔に算入しない。作者JSや入力dispatchをlayout途中へ再入させない。
- 支持確認：`python -X utf8 tests/check_media_count_native.py --service-cadence` は製品のservice関数をそのままCへ抽出して16条件0失敗。4msのIPC/patch後の同時刻再供給抑制、低PCM、通常間隔、時計逆行、native guard、再入guard、registry巡回、retired/detached owner、MSEを確認した。旧stream-count試験は反復していない。
- 対象源のfreestanding構文と `git diff --check` 成功。ログは `build/browser-media-cadence-host-20261013.log`。
- 限界：hostの時計・媒体供給は制御用mock。実79秒遅延の解消、音声・映像・F12後の本文高さ、上下入力の応答はまだ未受入。次は最新完全媒体の同条件実記事で主担当が確認する。

