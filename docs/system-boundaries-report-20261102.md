# 境界・権限・サービス・設定の第一段の報告（2026-11-02）

枝は `system-boundaries`。統合した枝は五本。

## 消したもの

- `kernel/src/sys/keep.c`、`AppData\Nocturne\*.conf`、`older[]` の引き継ぎ。
- `init_pid` の比較、`appdrivers=any`、`noappdrivers`、名前の早い者勝ちの channel（`SYS_CHANNEL_LISTEN`・`CONNECT`・`ACCEPT`）。
- 製品の `cmdline_has` の読み手、`nogui`、DNS の決め打ち。
- `user/apps/init.c`、`user/apps/desktop.c`、`user/apps/settings.ts`。
- カーネルの利用者フォルダーの決め打ち（`store_user_folder`、`FOLDER`・`USERFOLDER`・`TEMP`）。
- カーネルの中のタスクバー、開始メニュー、音量の盤、通知、壁紙、印（`gui/desktop.c`）、`SYS_GUI_LAUNCH`。
- `n_settings`、システムの設定の文書（`agent.conf`、webprefs の文書）。

## 建てたもの

| 層 | 持ち物 |
|---|---|
| カーネル | `struct n_identity`、能力の handle（`common/authority_abi.h`、`sys/authority.c`）、`SYS_CHANNEL_PEER`、サービスの道（`sys/service.c`）、私的な領域（`SYS_STORE_PRIVATE_OPEN`）、設定の適用（`sys/settings.c`）、起動の記録（`fs/bootrecord.c`）、C: のパスの権限と項目の持ち主 |
| システムのプロセス | `services`（宣言の表、依存、起こし直し）、`accounts`、`session`、`settingsd`（宣言、確かめ、保存、期限つきの試し）、`machine`（電源と画面の取得） |
| UI | `shell`（タスクバー、開始メニュー、通知、絵、印）、`settings`（宣言の表から頁を組む窓。サービスと利用者の頁を含む）、ログインの窓 |

## 確かめた範囲

- ビルド：`qemu-image` は値 0。
- ホストの検査：`check_boundaries` 0 件、`check_settings` 宣言 16 件で失敗 0 件。
- ゲストの試験：`scripts/test.py --quick --accel whpx` は `pass=202 fail=21 skip=3`（613 秒）。C: の検査は問題 0 件。
  - `test.py` の `all passed` は誤りだった。`^FAIL ` がログの行頭の時刻に阻まれて一致しなかった。判定は直した。
  - 変更前の版の同じ試験の結果は無い。21 件のうち今回の変更で増えた数は分からない。
- QEMU の画面（窓付き、WHPX、2560×1440）：
  - 初回の利用者の作成、ログイン、シェル、開始メニュー、印のダブルクリック。
  - 設定の窓の音、画面、時刻、サービス、利用者の頁。
  - 誤った合言葉は拒まれる。
  - 消音と時間帯 540 分は、QEMU を切って同じ overlay で起こし直しても残る。時計は 04:49 UTC に 540 分を足した 13:49 を描く。
  - `touch C:\Nocturne\x` は `not permitted`。`touch C:\Users\tester\x` は通る。
  - `gotest` は全項目を出して終わる。ゲストの `clang a.c -o a` は誤りなしで終わる。
  - ブラウザーは Wikipedia の Moon を描画する。

## 前と今の比べ

同じ URL を同じ機械の形で開き、`[browser trace]` の最初の二行の間隔を比べた。

| 版 | JS の実行環境の行 | 次の行 | 間隔 |
|---|---|---|---|
| 変更前の写し（2026-10-30 の `build/mae.img`、F12 あり、`--log-console`） | 28.256 s | 30.788 s | 2.53 s |
| 変更後（`--log-console`、F12 なし） | 127.814 s | 130.301 s | 2.49 s |

この比べは一つのサイトの一区間で、F12 の条件が違う。fps と窓の操作の遅れは測っていない。

## 確かめていないこと

- 実機と Hyper-V。nvrm の道は QEMU を通らない。
- 再起動の実行。WHPX は CPU の reset ができない。カーネルは `pm: stopping the devices` まで進んだ。
- 他の利用者のフォルダーへの書込みの拒否。
- ログアウト、眠り、hibernate。

## 持ち主の判断が要ること

- `.claude/rules/kernel.md` と `nocturne-project` の `logs.md` の `prof` の記述は、今は `kernel-test` の組みでだけ成り立つ。規則の変更の案を見せてから書く。
- 窓の管理と合成、TCP/IP をカーネルの外へ出すか。今回は残した。
