# 媒体性能改善 第二考察（2026-10-12）

第一報 `docs/browser-performance-media-20261012.md` は保持し、親の独立 review の四境界だけを追加検討した。第二報時点で媒体 source は統合 build 用に凍結。

## 四境界と製品修理

1. **同プロセス thread の VA 予約**: `syscall_dispatch` は BSP continuation で、finite kernel-mode page loop の timer は user-return まで continuation を切り替えず、`vmm_map_page` / `sched_quiesce_space` は sleep しない。それでも、将来の handoff と thread mmap / clock / stack 予約への安全性を明確にするため `sys_shared_map` は mapping loop 前の短い IRQ guard で `mmap_next` を予約し、成功 list publication も guard 化。失敗 rollback は `mmap_next` がこの予約終端のままの場合だけ戻し、後続予約を巻き戻さない。長い page loop 全体で IRQ を止めていない。
2. **子 FD 再利用 / import**: kernel の直接親子検証・descriptor lookup・`vfs_dup` は同じ短い IRQ guard。以後は file reference が vnode と物理ページを所有し、子 close / exit に依存しない。正常 MSE では未受信 header の slot は occupied のままなので子の old storage collector が FD を閉じない。ただし wire の `shared_id` だけでは同サイズの再利用 FD を識別できない反証があるため、共有 object 本体にも immutable id を格納し、occupied acquire **後**に backing id と header id を比較する製品修理を追加した。import 後に別 object なら fail-closed、heap / shared slot へ誤った free をしない。
3. **resize / seek / stop**: resize で忙しい旧 object は子 retired list へ移し、occupied が全て返却されるまで保持。親は現在 loan を返した後にしか次 header / geometry を import しない。stale generation / epoch / revision は `step` で release。seek は queued old header を読んで stale release するため、途中 packet を捨てて stream を壊さない。stop は子を kill して reap 後に親 map を destroy、各 address-space map reference は exit / munmap で回収する。stop 中の busy slot を再利用する子は存在しない。
4. **scaler alpha / clip / arithmetic**: 有効な decoder output は opaque ARGB 契約で、双線形の weight 合計 256 により alpha 255 が不変。任意 alpha でも四 channel の直線補間を保つ。巨大 destination で16.16 step が0、また中央 clip の初期座標が累積誤差を持つ反証を発見したため、32.32 step と unsigned 64-bit の整数部/分数部分離による正確な初期中心座標を実装。巨大 source を小さい destination にした最後の不要加算は signed overflow の危険があるため最終 sample 後は step を加えない。canvas `pitch*height*sizeof(pixel)` の PTRDIFF 表現可能性を検証してから書く。SSE2 の二画素 path と odd scalar tail は同じ変換を使う。

## 新しい証拠だけ

- `tests/media_boundary_hosttest.c` は第一報の76,747幾何検査と速度比較を反復しない。共有 id の差替え、occupied 0 / 2 / 無効slot / id0、shared size、alpha が0〜255の奇数幅、1,700,000,000 と INT_MAX destination の中央 clip、INT_MIN offscreen、表現不能 canvas を追加。
- 同じ bundled clang / host PATH の `-O2 -std=c11 -idirafter user/include -Icommon tests/media_boundary_hosttest.c -o build/media-boundary-hosttest.exe` とその実行は **3,480 検査、0 失敗**。
- acquire-before-id 順の製品修理後は、この変更だけの `--identity` 分岐を再コンパイルして実行し、**9 検査、0 失敗**。全描画試験を無意味に反復していない。共有 memory ordering と kernel peer lifetime の実OS証明ではない。
- 最終 native clang `-fsyntax-only` userspace 六 source / kernel 二 source は exit0、warningなし。owned source の `git diff --check` も exit0。最終 native対象は第一報と同じで、source変更後の依存境界確認として実施した。

## 残る検証と事実の境界

- 親担当が `tests/sharedpagetest.c` を既存 test runner の sharedpages group に接続し、統合済み kernel / libc / initrd の専用 QEMU で一度実行する。担当自身は VM を起動していない。そこでの結果を本書の host 成功から推定しない。
- 全媒体成果の実動画進行・pause / seek / 再open・聴取・GUI応答、同条件 QEMU性能と Hyper-V実機は依然ここでは未確認。第一報の host 59→32ms は第二考察の32.32初期座標変更後の速度保証ではなく、旧snapshotの限定測定。
- 初期 pointer / scalar 方式の性能失敗、誤った benchmark の訂正、native共有ABI未実行を第一報に残した。報告書の存在や syntax 成功で実サイト対応・無回帰・全体完了を主張しない。

## 追加の resize フロー制御修理

- 凍結後の寿命精査で、連続して寸法が増えると「current object だけ満杯判定」では occupied な旧 ring を一つずつ retired に追加できる反証を見つけた。親へ直ちに通知し、`browsermseworker.c` の `video_full` を **current + 全 retired の occupied 合計**で二枠の transfer window を判定する方式へ修理。これは decoded frame の拒否・ドロップや解像度上限ではなく、旧 loan が返るまで次の image loan を保持する通常 backpressure。二重バッファが resize のたびに無制限 lookahead へ変わらない。
- `tests/media_shared_window_hosttest.c` は製品 child source の `video_full` / `video_storage_busy` を直接使う。current と旧 ring で一枠ずつ、旧二個で current 空、返却後、通常二枠、header pending を確認し、**13 検査 0 失敗**。native child entry は host の無関係な syscall / codec を呼ばないよう internal な未使用 entry とし、期待値や製品 predicate は差替えていない。最初は host に `strlcpy` の宣言がなく compile 失敗、その宣言を追加後は PE linker が external native entry を保持して unresolved references、未使用 entry の linkage を正しく internal にして修正した。製品 compile の失敗ではない。
- 最終 `browsermseworker.c` の native clang syntax と owned diff check は exit0。kernel / shared ABI / scaler はこの追加修理で変更していない。親の第一統合 guest は追加修理前の worker であるため、共有ページ kernel と他の変更が同じ検査は再実行せず、次の正規梱包の実 ABR / seek / 動画進行で修理を受け入れる。
- 追加修理の second disposition として EOF / sequence / PCM horizon の callsite を再考した。ring の満杯は decode を拒否せず `slots[i].pending` の次 image loan を保持する。control の END が共有 old slot の busy 中に生じても、親 `nmedia_mse_worker_step` の `video_head_pos` / `video_ready` / `video_expected_sequence > video_sequence` が最終 header の受領・返却まで END の追越しを防ぐ。PCM horizon は immutable raster 公開後に完全 header を書いたときのみ進む。window の合計 occupied 判定はこれらの sequence / authored duration を変更せず、過剰先読みだけを止める。この再考は静的境界判断であり、親の最後の実動画による動作受入を代替しない。
