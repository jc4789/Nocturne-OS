# native channel・event 消費の追補第一検討

親の第二実行 `build/nvidia-review-87dd1e2f/channel-serial.log` は完走し、
`nativechanneltest: 235 checks, 1 failures`。
失敗は `one wait reports every ready client` の一件で、他の credential・添付・旧 receive は成功した。

ソースと契約を照合すると、`docs/nocturne-native-abi.md` §6 は wait を一回最大64件のバッチと明記し、
`kernel/src/sys/events.c` は caller max が大きくても既存内部 batch へ切り詰める。
「96接続が ready なら一回の戻り値が96」という既存試験はこの契約を誤読しており、今回の credential 回帰ではない。

親承認により、製品を拡張せず試験を正しいバッチ消費へ修理した。
戻り件数に従って各 event の一意 token・範囲内 token・RECEIVABLE を検査し、対応 channel の本文値と
sender_app を確認して readiness を消費する。次の wait では残りを受け取る。
重複、範囲外、途中0件・負値、本文/主体不一致は失敗し、最終96接続をすべて検査する。
単に `<=64` を許す検査や skip にはせず、新たに64という試験上限も置かない。

既存64件内部 batch と製品 source は今回未変更。新たな人工上限・巨大確保・IRQ-off 走査は追加しない。
修理した nativechanneltest の focused clang 構文と diff 確認は終了0。
新しい実行結果は親の QEMU 待ち。pcitest0や前実行の他検査成功を、新しい試験の成功へ流用しない。
