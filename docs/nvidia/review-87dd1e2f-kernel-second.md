# 87dd1e2f 再監査・native 境界の第二検討

担当 `gpu_kernel`。第一報は保持する。これは同じ契約に対する反例の再検討である。
実装・試験 source は本報告時点で安定。親が増分 build と隔離 QEMU を担当する。

## 1. 混合 cache と部分 view

「混合 object 全体へ一律の cache を指定できない」ことと、「その一部分だけへ正しい cache を指定できる」ことは別。
`shared_leaf` は view の first を加えた logical page で segment を二分探索し、cache と backing offset を解決する。
明示 cache の検査は要求した部分だけを走査する。DEFAULT では segment の型を潰さない。
DMA は保持した root の現在の型を使い、map 中の cache pin は各 selected segment に取得する。

`pcitest` に、混合 UC BAR/WB RAM から BAR だけを UC で切り出す場合と、
派生 view の RAM 部分だけを WB で再切り出し実データを読む場合を追加した。
全体を WB にする拒否の期待値は維持する。

## 2. 取消と map の公開

第一案の「最後に取消を再確認してから公開」だけでは不十分だった。
別 thread が未返却 VA を推測すると、未公開 PTE が revoker の走査から漏れ、取消完了後に一時利用できる反例がある。

修理:

- VA 予約と同じ BSP guard 内で構築 record を別の building list へ載せ、DMA pin を先に取得する。
- scope/claim 取消は building record も対象とし、cancelled を先に公開する。
  各一ページの PTE install はその flag 検査と同じ critical section に置き、取消後の追加 install を防ぐ。
- 大きな PTE 除去を一つの巨大 IRQ-off 処理にしない。既存 `vmm_user_free` の batch を維持する。
  `revoking` 中は constructor が native wait で join し、record・backing・owner VM を先に解放しない。
  再度の取消も進行中の除去完了を待つ。list の解除・公開・waiter wake は BSP guard 内。
- 未返却 building 範囲に対する release/protect は `N_ERR_NOT_READY`。
  正常完了時は building list から既存 app list へ原子的に移し、失敗時は全 PTE/TLB 除去後に pin・予約 VA・参照を回収する。
- 停止 flag をページ境界で検査し rollback する。
  scheduler は kernel continuation を勝手に捨てず、app 終了は兄弟 thread を join してから shared cleanup と VM 破棄へ進む。
  revoker 待機中に停止しても、まず continuation が参照を回収してから user-return の停止処理へ進む。
- IRQ の kernel 帰還だけでは map loop は切り替わらないため、親承認で page 境界に
  `need_resched` 時の schedule を追加した。時計 syscall、固定 delay、人工的なページ上限は追加しない。

新しい `pcitest` 反例は実 free_mem と同じ大きさの sparse shared object を使用する。
先頭一ページだけを触り、既知の解放 VA を builder に再利用させ、構築中 NOT_READY を観測して取消する。
取消完了直後、その推測 VA を channel send の kernel copy に渡すと BAD_ADDRESS でなければならない。
全 frame の確保や試験専用 kernel hook は不要。実行結果はまだ親の QEMU 待ちであり、観測成功とは記さない。

## 3. credential と失敗時の旧 API

- 実 send 主体を message に整数で snapshot するので、endpoint 継承後も creator の主体を偽装できない。
  送信者終了後も pointer の解放や ID 再利用で意味が変わらない。
- 新 API の PEEK は送信主体を返すが attachment を設置しない。DISCARD は attachment を閉じ、主体を返す。
  output が必須で、初期不正なら message を消費しない。文字装置には認証主体を捏造せず UNSUPPORTED。
- attachment 設置に実資源失敗が起きると install が元参照を閉じ、receive は負の結果を返す。
  output が途中で失効した場合も設置済み handle を閉じて BAD_ADDRESS とする。
  既に取り出した message は再送しない。失敗時に利用可能な credential を成功として返さない。
- 旧 receive の番号、引数形式、文字装置、正常な添付、PEEK の非消費は保全した。
  旧経路も設置失敗や出力先失効を正しく負の結果へする。失敗を正の本文長で隠す旧挙動は維持しない。

nativechanneltest は旧 receive の既存試験を残し、新 API の metadata 必須、PEEK 添付非取得、
通常添付、DISCARD、同 app 別 thread/別接続、譲渡 endpoint と sender 終了後の主体を追加した。
任意 OOM の強制注入による attachment 設置失敗そのものは未実施で、取得/失敗/回収経路の読取り確認と区別する。

## 検証範囲

最終の kernel 3 source と libc/sys・pcitest・nativechanneltest の focused clang 構文確認は終了 0。
`git diff --check` も終了 0。全体 build、native/edu 実行と GUI は親の受入結果と照合する。
静的確認を NVIDIA 実機・Hyper-V・任意競合全網羅・性能向上と呼ばない。
virgl は変更せず、POSIX、人工上限、GPLv2-only、他 OS 本体参照、`uvm_common.c` 参照/利用は追加しない。
