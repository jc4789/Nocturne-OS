# Nocturne の局所変更

`quickjs.c` の ArrayBuffer backing allocation 前に、要求バイト数を含む
既存 cycle GC の判定を行う。適応 GC 閾値が有限 runtime limit を超えた場合も
割り当て前に回収機会を与える。`SIZE_MAX` による自動 GC 無効化は維持する。

HLS 公式 demo の約13MB remux buffer が、使用量約123MB（117.5MiB）/ 上限128MiBで
GC 機会なしに失敗した実例から追加した。生存 buffer の破棄、上限拡大、
allocator 内の再入 GC、同期での FinalizationRegistry callback 実行は行わない。
live データが上限を超える場合は、従来通り実際のメモリー不足を返す。

上流の著作権・ライセンスは変更しない。
