# 媒体協調供給の公平性修理：第二考察

第一報の「開始時計で供給自身が次の間隔を消費する」という境界を、媒体を止めれば速いという別問題と区別した。

- 低PCMの閾値1,440framesと1ms間隔は不変更。供給済みPCMやpending出力を捨てず、通常tick経路の進行・codecとWorkerの所有権・seek世代を変えない。固定10msへ戻す修理ではない。
- 終了時計はserviceの再入guardが立った状態で更新する。pump中の再入は引き続き拒否し、次のregistry ownerへの巡回も維持。作者wheel取消を無視せず、FIFO・default action・祖先scroll chainは変更していない。
- 借用画像のIPC、PTS、audio DMA、committed video suffixは既存pump/snapshotのまま。未commitのlayout boxで媒体を描き直す修理や、F12後の非coherent画面へ強制publishする修理ではない。
- 原serialの4.36GBは途中の約2,095枚×960×540×4と整合するraw画像IPC。最終記録には9.09GBもある。これらをHTTP本文量やネット通信の遅さの証拠と混同しない。
- 反証条件：単一の非blockingコピー／snapshot自身が大幅に予算を超える、frame viewport commitが別の二乗処理になる、供給完了から1msを空けても音声がstarveする、または同じ実記事でF12／native上下wheelが固まるなら、この差分だけで解決済みとはしない。host16条件や先行geometry17成功で相殺しない。
- 現在は対象源を凍結。最新実媒体で同じF12close・本文wheel4下／4上と動画進行を確認し、原79秒ループとの同条件比較で判断する。

