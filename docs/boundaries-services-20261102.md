# 境界・権限・システムのプロセス：動く形まで（2026-11-02）

## 結論

- 統合で組めなくなっていた `main` は組めます。`qemu-image` で確かめました。
- 利用者の権限、サービス、設定の窓、網、音、キーは、窓付きの QEMU で使って動きました。
- 実機と Hyper-V では確かめていません。
- `scripts/test.py`、`scripts/wpt.py`、`scripts/platform_qa.py` は、ユーザーの判断で消しました。動くかどうかは、スクリプトでなく OS を直接使って確かめます。

## 確かめの範囲

### 組み

- `python -X utf8 scripts/build.py -j 8 qemu-image` は通りました。Makefile を変えた後にもう一度組みました。空の `build/` からは組んでいません。
- `tests/check_boundaries.py` は 0 件、`tests/check_settings.py` は宣言 31 件で失敗 0 件でした。

### 今の媒体で直接使ったこと

機械は `-M q35`、WHPX、2560×1440、NVMe、e1000、`ich9-intel-hda`（音を wav へ録音）です。

| 見たもの | 結果 |
|---|---|
| 初回の利用者の作成とログイン | 動きました。 |
| admin の `touch C:\Nocturne\x` | `not permitted`。 |
| admin の `mkdir C:\Programs\d` と自分のフォルダーへの書込み | 通りました。 |
| 一般の利用者 bob の作成（設定の窓）とログイン | 動きました。 |
| bob の `touch C:\z`、`touch C:\Users\admin\x`、`ls C:\Users\admin`、`touch C:\Programs\y`、`rm C:\Nocturne\System\date` | 全部 `not permitted`。 |
| bob の自分のフォルダーへの書込み | 通りました。 |
| サービスの頁 | 見えた 6 件（`systemlog`、`accounts`、`settings`、`machine`、`programs`、`audio`）は動いています。頁の残りはこの媒体では見ていません。`ps` では `network`、`timesync`、`display` も動いています。 |
| `beep` | 録音に 440 Hz が 0.2 秒、振幅 10484 で残りました。 |
| プログラムの設定 | admin がブラウザーの設定で言語に Afrikaans を足し、電源を切って起動し直すと、`navigator.languages` は `ja,af` でした。 |
| プログラムの部品 | 同じブラウザーの Worker の `navigator.languages` も `ja,af` でした。 |
| 利用者ごとの分離 | bob のブラウザーは `ja` でした。 |
| 百度百科「月球」（`https://baike.baidu.com/item/%E6%9C%88%E7%90%83/30767`） | 頁が出て、動画は 18 秒まで進み、復号 457 コマのうち落ちたのは 4 コマ、音の underrun は 0 でした。実ブラウザーとの照合はしていません。 |

serial.log の `segfault` は、この回は 0 件でした。

### 前の媒体で使ったこと（直しの後に繰り返していない）

- セッション：ログアウト、前の利用者のアプリの停止、`session` の起こし直し。
- サービス：止める、起こし直す、全部を順に起こし直す。
- 設定の窓の全部の頁。時間帯、音量、配列が起動をまたいで残ること。
- 網：DHCP、`ifconfig`、`host`、`ping`、timesync。
- 音：音量 80 で振幅 10481、35 で 2006。消音で録音に音が無いこと。
- キー：Super、Ctrl+Esc、Ctrl+Alt+T、Alt+Tab、Alt+F4、メニューの矢印と Enter。日本語の配列。
- 言語：シェルと設定の窓が窓を閉じずに切り替わること。
- 休止：保存と復帰。
- コンソール：VGA を外した起動で、ログインと `date`、Backspace、上矢印、Ctrl+C。

### 確かめていないこと

- 実機と Hyper-V。
- S3 の復帰。WHPX では戻りません。
- 画面のモードの変更。QEMU では表示のドライバーがモニターを見ていません。
- 英語の言語が切り直しで残ること。
- 印の Enter。

## 直した欠陥

| 場所 | 欠陥 | 直し |
|---|---|---|
| `kernel/src/sys/authority.c`、`kernel/src/fs/store.c` | 一般のアプリの `program` が 0 で、`settings` が `N_SCOPE_PROGRAM` の宣言を拒否しました。 | 番号はカーネルが起こす文書のパスから決めます（64 ビット、大小と書き方によらない）。`N_LAUNCH_SAME_PROGRAM` で部品は親の番号を継ぎます。 |
| `user/libc/webprefs.c` | 設定が読めないと言語の一覧が空のままでした。 | まだ値が無ければ既定で動きます。 |
| `user/apps/services.c` | 名前の範囲を `program != 0` で決めていました。 | 管理者が起こしたプログラム（`user` が 0）で決めます。 |
| `user/libc/web/avmedia.c` | 音の流れがポインターになった時に `p->sink>=0` が残り、NULL で落ちました。 | `p->sink!=NULL`。 |
| `kernel/src/net/net.c` | `net_watch` の初期化が無く、起動の途中で panic しました。 | 頭で初期化します。 |
| `kernel/src/dev/hda.c` | 検査のスレッドが verb の応答を捨て、codec が silent になりました。 | 応答を残します。 |
| `kernel/src/arch/wake.c` | XSAVES の領域が未初期化で、復帰の XRSTORS が #GP でした。 | 0 で確保します。 |

カーネルの変更の方針は `docs/nocturne-native-abi.md` に書きました。

## 消した道具

- `scripts/test.py`、`scripts/wpt.py`、`scripts/platform_qa.py`。
- Makefile の `test`、`test-quick`、`test-full`。

`test.py` は、音の装置が AC'97 で、製品の HD Audio の道を通りませんでした。試験の結果を根拠にした `settingsd` の変更と、試験のサービスへの能力の追加は外しました。

## 残っているもの

消した道具がなくなったので、次のものは呼ぶ者がありません。

- `tests/testrun.c` と `user/apps/services.c` の `tests` の項目。
- `make kernel-test`。
- `tests/wptrunner.c`、`tests/wpt/`、`--wpt-automation`。
- `tests/runtests.c` と、host の相手を要する試験（`tcptest.c`、`nettransporttest.c` など）。`tests/` のコメントに `scripts/test.py` の名が残っています。
- サービス `audio` の `n_audio_reply.streams` は、`tests/avmediatest.c` だけが読みます。

## 途中で見つけた別のこと

- 一般の利用者が `poweroff` で機械を切れます。開始メニューと同じ方針です。
- 誤りの文 `n_result_text` は英語です（サービスの頁、利用者の頁の括弧）。
- 端末の窓の題は `Terminal` です。
- サービスの頁に古い「起動しています」が一度残りました。再現していません。
- 出口の codec が死ぬと `beep` が待ち続けます。
- `volume` は表示だけの道具です。
- `touch` は既にある文書では誤りを出しません。
- `.claude/ban/` は権限の判定で消せず、残っています。
- プログラムの番号は設定の名前の範囲で、同じ利用者のプログラムどうしの権限の壁ではありません。スクリプトの番号は解釈系の文書から決まります。
- 百度百科の console に、頁のスクリプトが捕まえた例外の行が並びます。追っていません。
