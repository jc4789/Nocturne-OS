# USB hub と記憶の driver の差分の監査（十回目）

## 依頼文

Nocturne OS（独自の OS、カーネルは C、対象は実機）の USB hub と記憶の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。

作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。

材料：
- 差分：`git diff 97e41771 HEAD -- kernel/src/dev/usbhub.c kernel/src/dev/nvme.c kernel/src/dev/ahci.c`。周りのコードは今のファイルで読む。
- 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の usb20.txt、usb32.txt、nvme24.txt、ahci.txt。原本は `D:\Programes\OS DEV INFO`。
- カーネルの規則：`.claude/rules/kernel.md`。

範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外は挙げないでください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。

報告：全文を `docs/records/kernel-core-20261101/audit-usbstorage-fix10.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（データを失う／当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 5 件。

| 重さ | 件数 | 指摘 |
|---|---|---|
| データを失う | 0 | |
| 当たれば壊れる | 2 | 1、2（どちらも `hub_port` の失敗の枝 `usbhub.c:393-399`） |
| きれいさ | 3 | 3、4、5 |

証拠の範囲はコードと文書の静的な読みだけ。ビルド、試験、QEMU、実機は動かしていない。

指摘 1 と 2 の入口は同じで、新しい `port_changes` が変化を確認応答した直後の読み直しに hub が答えないときに限る。97e41771 の `hub_port` は読みを一度だけ行い、確認応答の後に読まずに処理したので、二つの道はこの差分で生まれた。

## 指摘

### 1. 子のあるポートに残った `t->at` が消えず、毎回読まれ続ける

- **重さ：** 当たれば壊れる。
- **場所：** `kernel/src/dev/usbhub.c:396`（生む行）。残り続けるのは `usbhub.c:429`、効くのは `usbhub.c:444-447`、`usbhub.c:455-456`、`kernel/src/dev/xhci.c:1947`、`kernel/src/dev/xhci.c:1962`、`kernel/src/dev/xhci.c:2270`。
- **文書：** USB 2.0 Figure 11-21（11.12.3）、USB 3.2 Figure 10-23（10.13.3）は、変化を読み、確認応答し、読み直す手順だけを定める。読み直しの失敗の扱いは文書に無い。これは文書との差でなく、差分の行が生んだ欠陥。
- **道筋：**
  1. 子のある健全なポートに、接続以外の変化が来る。例は SuperSpeed の `C_PORT_LINK_STATE`。
  2. `port_changes` が読み、確認応答し、読み直しが失敗する。`*change` は 0 でなく、`t->at` は 0。
  3. `usbhub.c:396` の `change != 0` が真になり、`usb_port_failed(t, true)` が `t->at` を 1 秒後にする。
  4. 1 秒後、`port_due` が真になり `hub_port` が走る。接続があり、変化は無く、子がいるので `usbhub.c:429` で戻る。`t` を消す行（`usbhub.c:401`、`411`、`418`、`428`、`432`、`435`）はどれも通らない。
  5. `t->at` は過去のまま残り、`port_due` は接続が変わるまで真のままになる。
- **結果：**
  - `usbhub_work` の毎回の周回で、そのポートへ GET_STATUS を一回ずつ出す。周回は最長 1 秒ごとで、kick があればもっと多い（`xhci.c:2275`）。
  - `usbhub_pending` が真のままになり、`hc_busy` も真のままになる。これが起動の間に起きると `hc_settled` が呼ばれず、`usb_enumerating` が真のままになる。C: の disk がまだ見つかっていない起動では、`fs/store.c:307` の待ちが `FIND_NS`（10 秒）で終わらず `FIND_MOST_NS`（60 秒）まで延びる。
- **直し方の案：** 読み直しの再試行を `t` に載せない。試みの回数（`t`）は接続した装置の enumerate のものなので、確認応答した変化を見直す時刻は別に持つ。指摘 2 の案で一緒に消える。

### 2. 確認応答した変化を、読み直しの失敗で捨てる

- **重さ：** 当たれば壊れる。
- **場所：** `kernel/src/dev/usbhub.c:393-399`。関わるのは `usbhub.c:401`、`usbhub.c:404`、`usbhub.c:429`、`usbhub.c:444-447`、`kernel/src/dev/usb.h:30-34`（`usb_port_try`）。
- **文書：** USB 2.0 Figure 11-21、USB 3.2 Figure 10-23。変化を溜めて確認応答し、残りが無くなってから溜めた変化を処理する。USB 3.2 10.13.3、USB 2.0 11.12.3：確認応答すると、その変化は Status Change endpoint にもう報告されない。USB 3.2 Table 10-13 の PORT_ENABLE：ポートが DSPORT.Enabled にある間は 1。
- **道筋：** 溜めた `change` は `port_changes` が false を返すと捨てられ、次の読みでは 0 になる。hub も二度と報告しない。次の三つの道で誤る。
  - **ア（新しい装置を見ない）：** `t->at` が `CLOCK_NEVER` のポート（`USB_PORT_TRIES` で諦めたポート）に新しい装置が来る。`C_PORT_CONNECTION` を確認応答し、読み直しが失敗する。`usb_port_try(t)` は `clock_now_ns() < CLOCK_NEVER` で false なので `usb_port_failed` を呼ばず、`port_due` も真にならない。`usbhub.c:401` の `usb_port_free(t)` を通らないので、次の接続の変化まで新しい装置を enumerate しない。hub を毎秒読む `asking` の hub でも、`usbhub.c:429` の `usb_port_try(t)` が false で同じになる。
  - **イ（古い子が残る）：** SuperSpeed hub で、子のあるポートの装置が抜かれて挿し直される。`C_PORT_CONNECTION` を確認応答し、読み直しが失敗し、指摘 1 の道で 1 秒後に読む。リンクは自分で訓練して DSPORT.Enabled に戻っているので、接続あり、PORT_ENABLE 1、変化 0 を読む。`usbhub.c:404` の三つの条件はどれも真にならず、古い `h->child[port]` が残り、新しい装置を enumerate しない。USB 2.0 hub では挿し直しの後のポートが無効なので `!(status & PS_ENABLE)` で消え、この道は起きない。
  - **ウ（前の装置の試みを引き継ぐ）：** `t->at` が未来のポート（前の装置の再試行の待ち）に新しい装置が来ると、`usb_port_free(t)` を通らず、前の装置の `count` を引き継ぐ。`usbhub.c:364` の `retry` が真になり、`usbhub.c:365` の接続の debounce（USB 2.0 7.1.7.3）を省く。
- **直し方の案：** 失敗の枝で捨てずに持つ。
  1. `struct hub` にポートごとの `uint16_t *held`（確認応答して未処理の変化）を足す。
  2. `port_changes` の失敗で `held[port] |= *change` とする。
  3. 見直しの時刻は `t` でなく、`power` と同じ形の専用の `struct usb_port_tries` で持ち、`port_due` が見る。
  4. 次の `hub_port` は読んだ `change` に `held[port]` を足して処理し、`held[port]` を 0 に戻す。

  これで `CLOCK_NEVER` のポートも `usbhub.c:401` を通り、古い子も `usbhub.c:404` で消える。指摘 1 も `t` を使わなくなるので消える。

### 3. 接続のときの `hub_status` が電源を二度入れ、誤った行を出す

- **重さ：** きれいさ。
- **場所：** `kernel/src/dev/usbhub.c:234-235`、`usbhub.c:279-286`。
- **文書：** USB 3.2 10.14：Type-C や PD で電力の足りない hub は USB 2.0 の半分だけ Powered に進み、`C_HUB_LOCAL_POWER` と Local Power Source を 1 にする。USB 2.0 Table 11-19、USB 3.2 Table 10-10：Local Power Source の 1 は電源が無いこと。
- **道筋：**
  1. 上の hub が接続される。`usbhub_attach` の `hub_status` が変化を読み、確認応答する。`changed` は真。
  2. Local Power Source は 1 のままで `h->local` は偽なので `h->off` は偽。`usbhub.c:284` が「power good again」を出す。電源は良くなっていない。
  3. `usbhub.c:285` の `power_ports` が全ポートへ PORT_POWER を送り、`power_ms`（100 ms 以上）眠る。
  4. 戻った `usbhub.c:235` の `power_ports` が同じことをもう一度行う。起動のたびにこの hub の分だけ待ちが倍になる。
- **直し方の案：** `hub_status` を二つに分ける。状態を読んで確認応答する部分と、変化に応える部分（`power_ports`、`pending_mark`、`kprintf`）。`usbhub_attach` は読む部分だけを呼ぶ。`usbhub.c:284` の行は `h->local` が真のときだけ「good again」と書く。

### 4. `asking` の意味が新しい呼び手と合わず、一度の失敗が永久の読みになる

- **重さ：** きれいさ。
- **場所：** `kernel/src/dev/usbhub.c:57`（欄の説明）、`usbhub.c:241-244`、`usbhub.c:267`。
- **文書：** USB 2.0 11.12.1、USB 3.2 10.13.1：変化は Status Change endpoint が知らせる。差分はこの節に反しないので、文書との差でなく形の問題。
- **道筋：** `usbhub.c:57` の説明は「endpoint が動かないとき、ずっと」。`usbhub.c:267` の `hub_ask` は endpoint が動いている hub でも `asking` を立てる。`reporting` も `asking` も真のままになり、誰も `asking` を下ろさない。確認応答の直後の一度の失敗で、その hub は抜かれるまで、hub の状態と全ポートを毎秒読む。一度確認応答した変化を見直すだけなら、一度読めば足りる。
- **直し方の案：** 見直しを一度きりの要求にする。例は `hub_change` を再び立て、その周回で `hub_status` が答えたら下ろす形（失敗の間は `ask_gap` で間を空ける）。`asking` は endpoint が失われた hub だけのものに戻す。

### 5. `halt` の説明に「engines」が一語残る

- **重さ：** きれいさ。
- **場所：** `kernel/src/dev/ahci.c:790`。
- **文書：** AHCI 1.3.1 5.3.2.10、6.2.2.1、6.2.2.2：PxCMD.ST を 0 にすると PxCI と PxSACT が消える。止めるのは ST で、FIS 受信は別。
- **道筋：** 差分は同じ説明の前の文を「the command list engine stopped」に直した。次の文の「What had ended before the engines stopped」は複数のまま残った。`halt` が止めるのは `list_stop` の一つだけで（呼出しは `ahci.c:817`、本体は `ahci.c:392-396`）、受信の engine は動き続ける。動作に影響は無い。
- **直し方の案：** 「before the engine stopped」に直す。

## 照らして差が無かったもの

- `usbhub.c:109-121` の `port_changes` と `usbhub.c:264-277` の `hub_status` の輪：USB 2.0 Figure 11-21（Port Status Handling Method）、USB 3.2 Figure 10-23（同名）の、読む、溜める、確認応答する、読み直す手順と一致する。図の番号も正しい。
- `CHANGE_READS` の説明「残りは再び報告される」：USB 2.0 11.12.4（変化の bit が非 0 なら poll に bitmap を返す）、USB 3.2 10.13.4（非 0 なら ERDY を返す）と一致する。
- `h->local` と `h->off`（`usbhub.c:272`、`278`）：USB 2.0 11.5.1.2（self-powered で configure された hub が外部電源を失えば全ポートを Powered-off にする。bus-powered で configure され外部電源へ移った後に失っても同じ）、USB 3.2 10.3.1.1（local power の喪失で Powered-off）と一致する。bit の意味は USB 2.0 Table 11-19・11-20、USB 3.2 Table 10-10・10-11 のとおり。
- `usbhub_attach` の順：`hub_status` は `bind_classes` から呼ばれ、hub は configure 済み（`xhci.c:1675`）。USB 2.0 11.24.2.6、USB 3.2 10.16.2.4 の「configure 前の応答は未定義」に当たらない。確認応答の後に PORT_POWER を送るので、USB 2.0 11.11 と 11.24.2.7.1.6、USB 3.2 10.12 の「C_HUB_LOCAL_POWER と Local Power がともに 1 のとき PORT_POWER は効かない」にも当たらない。
- `h->off` のまま、または `hub_status` が失敗して `h->off` が古いまま戻った道：`hub_ask` の次の読みで `h->off` を読み直し、`usbhub.c:478` が全ポートを見る。子は接続なしで消え、電源は `usbhub.c:419-422` がポートごとに入れ直す。USB 2.0 11.12.5、USB 3.2 10.13.5 の回復の手順を満たす。
- `usbhub.c:424` の失敗：`usb_port_failed(power, true)` が先に見直しの時刻を立て、子は `usbhub.c:404` で既に消え、`t` も `usbhub.c:418` で消えているので、捨てた変化で誤る道は無い。
- `nvme.c:554` の RTD3E：Identify Controller の 91:88（NVMe 2.x Figure 338 の表）で、単位は microsecond、0h は未報告。別の装置でも本人の値を `shutdown_done` が使う形は、3.6.1 の「RTD3E の最小を待ち、0h なら 1 秒待つ」と一致する。移した行は値を読むだけで、別の装置に書き込まない。
- `ahci.c` の差分は説明だけで、指摘 5 のほかは 6.1.5（読みの overflow は fatal の回復、書きの overflow は COMRESET が要る）、6.2.2.1、6.2.2.2、5.3.2.10 と、`ahci.c:309` の動作に一致する。
