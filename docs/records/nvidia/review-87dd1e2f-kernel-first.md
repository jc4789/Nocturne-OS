# 87dd1e2f 再監査・native 境界の第一検討

担当: `gpu_kernel`。開始は `87dd1e2f`。J-space は親と協調する `xhigh`。
開始時の read / repo view / work は各終了 0、work は ALLOW。
開始 fingerprint は `32363695969527c93c2ede8a77f5465964de72abaac3354d612fb642c25b05c0`。
全員の開始 gate 完了通知を待って編集を開始した。以後の controller は親と直列にする。

## 製品の変更

- A4: channel の各 message に、実 send 時の `task_process()->pid` を kernel が記録する。
  `n_channel_receive_from` と `n_channel_message` は attached handle と認証済み `sender_app` を返す。
  endpoint の creator 固定方式は採用しない。launch で endpoint を別 app に渡せるためであり、
  受信側は HELLO の主体へ bind し、各 request の主体も確認する契約を RM 担当へ共有した。
- 同じ app の別 thread・別 connection は同じ native app ID。別 app は異なる。
  queued message は task pointer を保持せず整数 snapshot を保持する。
  scheduler の採番は既存 signed 32-bit ABI の表現幅を超える前に失敗させ、wrap と再利用を防ぐ。
- A7: `n_shared_compose(ranges,count,access)` がページ境界の ordered slice を一つの shared handle にする。
  既存 shared の内部へ flat backing segment と取消 scope 依存を統合した。
  nested composite の backing は平坦化し、source scope と nested composite scope は独立参照で維持する。
- RO source を RW にできない。既存 map/view と同じ最大権利を保持する。
  source ごとの cache を PTE に反映し、DEFAULT view は異種 cache を維持する。
  明示単一 cache が対象全 segment と一致しなければ再解釈せず拒否する。
- source handle close 後も backing は生存する。source revoke は派生 composite の既存 map にも及ぶ。
  PCI claim 喪失は該当 BAR page を失効し、合成中の独立 RAM は別 backing として保持する。
- source 数の固定上限は追加しない。配列積、総 byte 数、metadata の overflow と実確保失敗を検査する。
  部分失敗では実際に獲得した file/scope 参照を回収する。
- DMA cache pin は map 構築前に取得し、取消の最終検査と map 公開を同じ BSP critical section にした。
  親の独立レビューで、rollback/unmap の pin 解放が PTE 除去より先だと cache alias が生じ得ると指摘された。
  PTE/TLB の除去完了後まで pin を維持する順序へ修理した。

既存 syscall 番号/layout は変更せず、新 API は enum 末尾に追加した。
旧 channel receive、共有 RO/取消、PCI 隔離を保全する。virgl 本体は今回変更していない。

## 確認と反例

実施した focused 構文確認は終了 0:

- kernel の通常 target・warning 設定で `syscall.c`、`sched.c`、`channel.c`。
- native user include と clang resource header で `user/libc/sys.c`、`pcitest.c`、`nativechanneltest.c`。
- `git diff --check` は終了 0。

初回の構文確認では `copy_from_user` の前方宣言漏れを検出し修理した。
user 確認の初回は `-nostdinc` が compiler の `stdarg.h`/`stddef.h` も除外したため失敗した。
製品標準の include 条件に直した上記確認と区別する。

既存 native 試験へ追加した反例は、実 QEMU ではまだ未実行:

- send 時 credential、別 thread、別接続、endpoint 継承先の別 app、送信者終了後の queued credential。
- RO 合成の昇格拒否、非隣接 slice と demand page、source close、境界を跨ぐ view、後半入力失敗。
- nested composite/view の source revoke 後、別 child による既存 map アクセスが fault になること。
- 非連続 edu BAR page と RAM の合成、cache 混在、claim 喪失後の BAR all-ones と独立 RAM の生存。

## 未確認と次の一手

親の増分 build と一台の隔離 QEMU による native/edu 実行を待つ。
構文成功を実行成功・NVIDIA 実機・Hyper-V・性能改善へ読み替えない。
RM の HELLO/request 接続と OS 層の range 保持は別担当であり、統合差分の独立確認が必要。
他 OS 本体、GPLv2-only、`uvm_common.c` は参照も追加もしていない。POSIX 互換は追加しない。
