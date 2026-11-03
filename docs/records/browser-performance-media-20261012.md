# 媒体性能改善 第一報（2026-10-12）

## 変更と契約

- `user/libc/media_mse_worker.c`、`user/apps/browsermseworker.c`、`user/include/media_mse_worker_private.h`: v8 の共有ページ二重バッファ。従来の ARGB raster の pipe 搬送を廃止し、VIDEO pipe は 136 byte のヘッダだけ。子が共有スロットへ一回コピーして release 公開し、親は acquire 取得して display/lookahead の既存専有割当へ一回コピーして返却する。生の共有ポインターを heap の free に渡さない。
- スロット busy と親子の mapping reference が所有権を保持する。寸法増加時の旧共有 object は全スロット返却まで子が保持。generation、epoch、SourceBuffer revision、sequence、seek/drop による stale 拒否を維持。EOF は既存 VIDEO header/loan より先へ進まない。
- `common/abi.h`、`user/include/nocturne.h`、`user/libc/sys.c`、`kernel/src/sys/syscall.c` と既存 `sched` / `vfs` 配置: `n_shared_create(bytes)` と `n_shared_map(owner_pid, fd, bytes, prot)`。fdmap の三個という契約は変更しない。子が生成した FD を親が直接親子関係の検証後に import。mapping は FD close / 他側 exit を超えて参照を持ち、whole munmap / 所有プロセス exit で回収。partial unmap と共有 RO への WRITE 権限付与を拒否する。共有 object をグローバル名・私的ファイルに保存しない。
- `user/libc/media_draw.c` と `user/include/media.h`: aspect-fit、clip、canvas pitch を保持した pixel-centre 双線形補間。固定小数点の二つの step 除算は draw ごと一度、SSE2 は二画素・全 channel を並列補間。等倍は row memcpy。元コードの実際の除算は横方向一回/画素と縦方向一回/行であり、ユーザーの「二回/画素」はそのまま確定していない。
- `user/libc/web/avmedia.c`: next PTS と実 DMA playhead / playback rate から起床期限を計算し、PCM queue の refill 期限と複数 stream の最小期限を統合。MSE background は既存 service backoff の実期限を使う。固定十 ms の媒体全体 polling を廃止。
- 同寸法の video は既存の guarded snapshot partial present を普通 tick でも使い、成功時は全文書 paint を要求しない。不成立時は親担当の `doc_paint_damage(doc,node)` へ戻る。寸法変化は `layout_invalidate(node->box)` と doc dirty を立てて再 layout / full paint を維持する。

## 実行した確認

- 正規 native clang の対象と flags による `-fsyntax-only`: `media_mse_worker.c`、`browsermseworker.c`、`media_draw.c`、`avmedia.c`、`sys.c`、`tests/sharedpagetest.c` は exit 0、最終 warning なし。kernel の `syscall.c` / `sched.c` も exit 0、warning なし。全体 build ではない。
- `tests/media_draw_hosttest.c` は製品 scaler を直接使い、独立した浮動小数の参照、1〜64 の source サイズ、負の destination、aspect-fit、clip、stride 外 guard、等倍完全一致を確認。76,747 検査、0 失敗。
- 同じホスト、480×270→1920×1080、12 frame、noinline と volatile 消費を使う比較は旧 nearest 59 ms / 最終 SSE2 双線形 32 ms。初期 scalar 双線形は 103 ms で遅かったため SIMD へ修理した。初期の nearest 2 ms は dead-store 除去の試験不備であり棄却、条件を訂正した。ホスト測定は QEMU / Hyper-V の速度改善を証明しない。
- host 実行コマンド: Python は subprocess と bundled runtime PATH 設定だけに使用し、`tools/msys64/ucrt64/bin/clang.exe -O2 -std=c11 -idirafter user/include -Icommon tests/media_draw_hosttest.c -o build/media-draw-hosttest.exe` 後に同 executable を実行。最初の直接 GCC 起動は runtime PATH がなく exit 1 / 出力なしで失敗したため、bundled PATH の clang に切り替えた。
- owned 14 source 差分への `git diff --check -- ...`: exit 0。共同全体差分の一度の確認は親 `browser.c` の CRLF 追加行による trailing whitespace で exit 1。親へ通知済みであり媒体側の成功へ合算しない。

## 未確認と引渡し

- 親のみが full build と QEMU を起動する契約を守り、担当は VM / Hyper-V を起動・操作していない。実サイト動画、音声聴取、pause / seek / 再open、GUI応答と同条件性能は未確認。
- `tests/sharedpagetest.c` は新 native ABI の実OS検証用。self mapping の close 後寿命、RO、partial unmap、無効 FD / pid、親による子 FD の import と子 close / munmap / exit 後の生存を一度の経路で確認する。syntax のみ完了、実OSではまだ未実行。完全 build 後に通常 libc / crt0 と link し、専用 VM の `/tests/sharedpagetest` で実行する。
- 通常媒体の `media_worker.c` の別 transport は今回の所有外で未変更。ユーザーが挙げた MSE VIDEO 経路を修理した。shared raster は ARGB のままであり、YUV 伝送は追加していない。実共有ページは FFmpeg heap 統計とは別の物理メモリ使用で、kernel PMM が割当・解放を管理する。
- 親による独立 source/ABI review、共有 map の最終同期、同じ担当への second task と第二報を待つ。
