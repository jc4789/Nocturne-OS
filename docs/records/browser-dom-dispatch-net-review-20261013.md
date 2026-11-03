# 統合前の主担当独立確認

## 保つ契約
サイト専用処理は入れず、既存のGC寿命、custom element反応・MutationObserver・Range、script実行順、TCP tuple・文書世代を保つ。実サイト速度・入力受入は別に行う。

## DOM挿入
`dom.c` の `detach` は削除した要素のindexを0にし、保存したnextからだけ再番号付けする。挿入側はdetach後のcoherent prefixを使用し、非要素追加でも他の構造通知を省略していない。parser formでは通常leafを除外するが外部controlを所有し得るformを残し、登録linkの直接解除を確認した。既存host検査を主担当自身で実行し20条件成功。先頭大量挿入の線形仕事まで消えたとは主張しない。

## Script挿入
`js.c` の移動前静的候補列・非再帰shadow walker・template除外、候補ごとの接続確認と世代確認を確認した。nativeノードは文書arenaに保持され、候補のraw pointerをJS wrapperの生存だけへ依存させていない。AがBを切断する場合、再入したC、接続済scriptのText追加、空fragmentを区別するchanged flagを確認。主担当実行の19条件は成功。元のnative作者script反例22条件は統合媒体でこれから確認する。

## TCP・期限
stable TCB内queueにconnect/send/recvの待機とACK/data/FIN/reset/close/freeの通知が両方移ったことを確認。`wq_wake_all` はwaiterをunlinkしてからreadyにし、`tcp_free` はその後にmetadataを消す。再開後のgeneration/user_open確認が旧待機から新接続へのbytes流用を拒否する。TCP handleはVFS fdではなく、sys_pollはVFSのreadyだけを見る。pipe write/closeとprocess exitの本来のpoll_notifyは維持される。UDP/DNS/ICMPは同じnowで期限判定・減算する。構文確認は成功、native追加5条件はこれから確認する。

## 未達
前の百度記事はstylesheet #3のTCP connect timeoutでunstyledだった。新しい局所待機だけで外部経路のtimeoutが解消すると断定しない。旧媒体の28.4→5.0秒はposted DOM taskの実時間だけの比較。全ページ速度・読み込み中wheel・hover画像維持・Reddit sidebarは統合実サイトで確認を続ける。
