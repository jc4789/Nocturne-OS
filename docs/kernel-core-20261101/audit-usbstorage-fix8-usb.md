# USB の driver の差分の監査（fe71d739 から 9bea7571）

## 依頼文

Nocturne OS（独自の OS、カーネルは C、対象は実機）の USB の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。

作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。

材料：
- 差分：`git diff fe71d739 HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhub.c kernel/src/dev/usb.h kernel/src/dev/usbhid.c`。周りのコードは今のファイルで読む。
- 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt。原本は `D:\Programes\OS DEV INFO`。
- カーネルの規則：`.claude/rules/kernel.md`（spinlock の下で眠らない、割り込みの処理と呼ぶ側は同じ spinlock、など）。

範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外の問題は、port や worker が永久に止まるものだけを挙げてください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。

報告：全文を `docs/kernel-core-20261101/audit-usbstorage-fix8-usb.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 10 件です。

- 当たれば壊れる：5 件（1〜5）。このうち 2 件（1、5）は差分の外で、port が永久に止まるものです。
- きれいさ：5 件（6〜10）。

確かめ方はコードと文書を読むことだけです。ビルドと実行はしていません。行番号は HEAD（9bea7571）の今のファイルのものです。

## 当たれば壊れる

### 1. USB 2.0 hub の局所電源が戻っても、port の電源を入れ直さない（差分の外、永久に止まる）

- 場所：`kernel/src/dev/usbhub.c:228-229`（`hub_status`）。
- 文書：
  - USB 2.0 11.13：局所電源が切れると、hub は Configured のまま全 port を Powered-off にします。切れている間、port の状態と変化は全部 0 と読め、SetPortFeature は無視されます。通知は Status Change endpoint の hub の変化で来ます。
  - USB 2.0 11.5.1.2：外部電源を失ったら全 port を Powered-off にします。
  - USB 2.0 11.5.1.3：Powered-off から出る道は SetPortFeature(PORT_POWER) だけです。
  - USB 2.0 11.11、11.24.2.7.1.6：C_HUB_LOCAL_POWER と Local Power Status が両方 1 の間、この要求は無効です。
- 道筋：
  1. 自己給電の hub の AC アダプターが抜けて、また挿されます。
  2. port の変化は 0 と読めるので、報告に来るのは hub の bit だけです。
  3. `hub_status` は C_HUB_LOCAL_POWER を消し、`change & 2` が 0 なので 229 行で戻ります。
  4. `power_ports` は呼ばれず、port の印も付きません。
  5. `hub_port` が走らないので、`h->child[p]` は残り、port は Powered-off のままです。
  6. 差分の新しい電源の確かめ（363 行）も `hub_port` の中なので届きません。
  7. この hub を抜き差しするまで、全 port と子の装置は戻りません。
- 直し方の案：`hub_status` で `change & 1` のとき、C_HUB_LOCAL_POWER を消した後で、`status & 1` が 0 なら `power_ports(h)` を呼びます。`status & 1` が 1 なら全 port に `pending_mark` して子を外します。

### 2. 電源を入れ直す枝が接続の変化を食い、残した port の試みが解かれない

- 場所：`kernel/src/dev/usbhub.c:365-377`。対になる root port の側は `kernel/src/dev/xhci.c:1786-1788`、`1873`。
- 文書：
  - USB 2.0 11.12.5、USB 3.2 10.13.5：回復の手順の 5 番目は、影響を受けた port を列挙し直すことです。
  - USB 2.0 11.24.2.7.2.1：C_PORT_CONNECTION は Powered-off の間は 0 に消えます。
  - `usb.h:18-19` の約束：接続のある port を残すのは、接続が変わるまでです。
- 道筋：
  1. 装置が `USB_PORT_TRIES` 回失敗し、`t->at` が `CLOCK_NEVER` になります。
  2. この port で過電流が起き、電源が切れて過電流がすぐ終わります（差分のコメントの「a short takes it again at once」の場合です）。
  3. `hub_port` が読むと PORT_OVER_CURRENT は 0 です。357 行の枝の `usb_port_free(t)` を通りません。
  4. C_PORT_CONNECTION は Powered-off で消えているので、349 行の `usb_port_free(t)` も通りません。
  5. 369 行で電源を入れ、`power_ms`（100 ms 以上）眠ります。この間に装置の接続が見えます。
  6. 371-372 行が読んだ `CH_CONNECTION` をそのまま消します。
  7. 377 行の `usb_port_try(t)` は `CLOCK_NEVER` なので偽です。
  8. 接続の変化はもう報告されないので、抜き差しするまで列挙されません。
- `t->at` が未来の値のときは、遅れるだけで済みます。
- 差分は root port の側では電源の無い port の試みを解くようにしました（`port_empty` の `!(sc & PS_PP)` と `hc_ports_again`）。hub の側には同じ処置がありません。
- 直し方の案：365 行の電源の無い枝の頭で `usb_port_free(t)` を呼びます。6 の 2 番目の道も同時に消えます。

### 3. 答えない hub を毎秒全 port 読み、worker を占める

- 場所：`kernel/src/dev/usbhub.c:420-431`、`342-346`。時間は `kernel/src/dev/xhci.c:124`（`CONTROL_NS` は 5 秒）、`916-921`。
- 文書：要求の 5 秒は USB 2.0 9.2.6.1 です。読む間隔は文書に定めがありません。
- 道筋：
  1. hub の Status Change endpoint が 8 回失敗して `usbhub_reports_lost` が `asking` を立てます。
  2. この hub の制御の要求が時間切れになり、`control_recover` は成功します。`ctl_broken` は立たないので、次の要求もまた 5 秒待ちます。
  3. `usbhub_work` は一回の呼出しで全 port に `hub_port` を呼びます。7 port で 35 秒、15 port で 75 秒です。
  4. `ask_at` は輪の頭で 1 秒先に置くので、輪が終わった時にはもう過ぎています。worker は 1 秒以内に次の輪へ入ります。
  5. 同じ controller の `hc_ports`（root port の接続）、`hc_recover`（他の装置の止まった endpoint）、`usbhid_work`（鍵盤の灯）は、輪と輪の間でしか動きません。
- 差分の前は、endpoint が死んだ hub を読みに行かないので、この占有はありませんでした。
- 344 行のコメントは答えない hub を一度だけ避けていますが、`t->at` のある port だけが対象です。
- 直し方の案：`hub_port` が `port_status` の失敗を返し、`usbhub_work` は失敗の時点で輪を止めます。読めない間は `ask_at` を延ばします（`usb_port_tries` を一つ hub に持たせれば既存の間隔の作りが使えます）。

### 4. hub 単位の過電流の最中に PORT_POWER を送る

- 場所：`kernel/src/dev/usbhub.c:365-369`、`419`。
- 文書：
  - USB 2.0 11.12.5、USB 3.2 10.13.5：hub 単位の過電流では全 port の電源が切れ、port の過電流の bit と変化の bit はどちらも立ちません。回復の手順は、過電流の bit が 0 になるのを待ってから電源を入れます。
  - USB 2.0 Table 11-19：wHubStatus の bit 1 が hub の過電流です。
  - USB 2.0 11.13：局所電源が切れている間は port の状態が全部 0 と読めます。
- 道筋：
  1. 差分の前は `change & CH_OVER_CURRENT` のときだけ電源を入れました。hub 単位の過電流ではこの bit が立たないので、`hub_status` の `power_ports` だけが入れていました。
  2. 差分の後は、`hub_port` が走れば、電源の bit が 0 で port の過電流の bit が 0 なら入れます。
  3. `asking` の hub は毎秒全 port で `hub_port` が走り、`hub_status` は一度も呼ばれません（419 行は `hub_change` のときだけで、`hub_change` は報告からしか立ちません）。
  4. 報告の動く hub でも、`port_due` の port（試みの途中、電源の入れ直しの途中）で同じことが起きます。
  5. どちらも手順の 3 番を飛ばして SetPortFeature(PORT_POWER) を送ります。
  6. USB 2.0 11.24.2.7.1.6 により、局所電源の条件に当たらなければ PORT_POWER の bit は 1 になります。
  7. 局所電源が切れているだけの port でも、ログは「over-current ended」と書きます。
- この要求に hub がどう動くかは文書に定めがありません。文書の手順との差は確かです。
- 直し方の案：`struct hub` に hub の電源の状態（wHubStatus の bit 0、1）を持ち、`hub_status` が更新します。`asking` の輪の頭でも wHubStatus を読みます。この状態が立っている間は 365 行の枝で電源を入れません。

### 5. hub の初めの要求が一度失敗すると、port の電源が入らないまま残る（差分の外、永久に止まる）

- 場所：`kernel/src/dev/usbhub.c:133-136`、`202-205`。呼ぶ側は `kernel/src/dev/xhci.c:1674-1681`、`1391`。
- 文書：
  - USB 3.2 10.16.2.9、10.1.3.1：Enhanced SuperSpeed の hub は Set Hub Depth を受けて初めて route string の自分の桁を知ります。
  - USB 2.0 11.5.1.2、11.5.1.3、USB 3.2 10.3.1.10：port の電源は SetPortFeature(PORT_POWER) が入れます。
  - 失敗した要求をやり直す回数は文書に定めがありません。
- 道筋：
  1. `usbhub_attach` で SET_HUB_DEPTH が一度失敗すると、204 行で戻ります。`power_ports` も `usb_interrupt_start` も呼ばれず、`asking` も立ちません。
  2. `usbhub_prepare` で hub の descriptor が一度読めないと、`false` が返り、`d->hub` が NULL のまま `bind_classes` が hub を飛ばします。
  3. どちらも `device_attach` は `xd` を返すので、親の port は成功と見て `usb_port_free` します。
  4. 親の側に試みは残りません。hub の全 port は、この hub を抜き差しするまで使えません。
- 差分は `usb_interrupt_start` の失敗には `asking` を用意しましたが、同じ関数の一つ前の失敗には道がありません。
- 直し方の案：`usbhub_attach` が失敗を返し、`device_attach` が失敗として slot を返します。そうすれば親の port の `usb_port_tries` の間隔で列挙し直されます。

## きれいさ

### 6. `port_due` が、走っても進まない port を待ちとして数える

- 場所：`kernel/src/dev/usbhub.c:392-395`、`357-361`、`366`。対になる root port の側は `kernel/src/dev/xhci.c:1875-1876`（`!(sc & PS_OCA)` を見ています）。
- 文書：USB 2.0 11.24.2.7.1.4：PORT_OVER_CURRENT は過電流のある間 1 です。
- 道筋は二つあります。
  - `h->power[port].at` が過ぎた後も過電流が続くと、357-361 行で戻り、`at` が変わりません。
  - `h->tries[port].at` が過ぎた port で、電源が切れていて `h->power[port].at` がまだ先だと、366 行で戻り、`at` が変わりません。
- `usbhub_pending` が真のままになります。
  - worker の輪は `kernel/src/dev/xhci.c:2273-2275` で最長 1 秒眠るので、空回りにはなりません。余分な読みは輪ごとに一度です。
  - `hc_busy`（`xhci.c:1962`）が真のままで `hc_settled` が呼ばれません。
  - 起動の disk がまだ見つからないときだけ、`fs/store.c:307` の待ちが `FIND_MOST_NS`（60 秒）まで延びます。
- 直し方の案：過電流の枝で `power->at` を次の輪の先へ置きます。二つ目の道は指摘 2 の直しで消えます。

### 7. `usb_port_failed` の数の巻き戻しが「装置がやっと来た」以外でも起きる

- 場所：`kernel/src/dev/usb.h:42`。`stuck` を真で呼ぶ所は `kernel/src/dev/usbhub.c:344`、`386`、`kernel/src/dev/xhci.c:1855`。
- 文書：定めがありません（`usb.h:17` のとおり）。照らす相手は `usb.h:18-19` の約束です。
- 道筋：
  - `hub_port` の 344 行と 386 行は、hub の読みの失敗にも `stuck` を真で渡します。次の試みで接続が読めると、数が 1 に戻ります。読みがときどき失敗する hub では `USB_PORT_TRIES` に届きません。
  - USB3 の port で warm reset の結果が接続あり、接続なし、接続ありと交互になる装置でも数が 1 に戻ります。warm reset は `port_reset` が変化の bit を消すので、CSC の道とは別です。この装置は 1〜2 秒おきに warm reset を受け続けます。
- コメント「a device has come at last」と動きが合いません。
- 直し方の案：`stuck` の引数を「読めなかった」と分けます。数を戻すのを接続の最初の一度に限るために、印を一つ持ちます。

### 8. `usbhub_reports_lost` を、hub の endpoint でないものの死でも呼ぶ

- 場所：`kernel/src/dev/xhci.c:1919`。条件の作りは `kernel/src/dev/xhci.c:1390-1394`、`1540-1546`。
- 文書：USB 2.0 11.12.1：hub の endpoint は Status Change の一つです。USB 2.0 11.13 の compound device は、hub と常設の device の組です。
- 道筋：`bind_classes` は一つの device の hub の interface と HID の interface を両方受けます。HID の endpoint が死ぬと、hub の報告は動いているのに `reporting` が偽になり、誤ったログが出ます。`asking` で毎秒読むだけで、port は動きます。
- 直し方の案：`struct hub` に endpoint の番地を持ち、`usbhub_reports_lost(d, address)` で照らします。

### 9. `link_stuck` のコメントの言い切りが文書より狭い

- 場所：`kernel/src/dev/usbhub.c:289-290`。
- 文書：
  - USB 3.2 10.16.2.6 の PORT_POWER：0 の port は DSPORT.Powered-off、Powered-off-detect、Powered-off-reset のどれかです。
  - USB 3.2 10.3.1.1：link が eSS.Disabled なのは Powered-off だけです。
  - USB 3.2 10.3.1.10、10.3.1.11：残りの二つの link は Rx.Detect です。
  - USB 3.2 10.13.5：過電流で入るのは Powered-off-reset です。
- 道筋：コメントは電源の無い port の link を eSS.Disabled と言い切ります。このファイルが扱う過電流の場合は Rx.Detect の側です。判定（`PS_SS_POWER` を見ること）の結果は正しく、動作は変わりません。
- 直し方の案：「電源の無い port の link は Inactive でも Compliance でもない（10.3.1.1、10.3.1.10、10.3.1.11）」と書きます。

### 10. `mouse_report` の条件に、効かない項がある

- 場所：`kernel/src/dev/usbhid.c:279`。
- 文書：定めがありません。HID 1.11 は軸の組合せを制約しません。
- 道筋：`x_known`、`y_known` は 270-277 行の `!relative` の枝でしか立ちません。`relative` は attach の後で変わりません（`usbhid.c:386`、`641-642`）。`!h->x.relative && !h->y.relative` は常に真です。
- 直し方の案：`if (h->x_known && h->y_known)` にします。

## 照らして、差が無かったもの

- `port_reset` が `CH_OVER_CURRENT` を残すこと（`usbhub.c:257`）：変化の bit が残る間 hub は報告を続けます（USB 2.0 11.12.4、USB 3.2 10.13.2）。`hub_port` は次の報告で走ります。
- `PS_USB2_POWER`（bit 8）と `PS_SS_POWER`（bit 9）：USB 2.0 Table 11-21、USB 3.2 Table 10-13 と同じです。
- `port_empty` の `!(sc & PS_PP)`：xHCI Table 5-27 で PP が 0 なら CCS、PED、PR は 0 です。
- root port の電源の 20 ms：xHCI 5.4.8（PPC が 1 のときソフトが 20 ms 待ちます）と同じです。
- OCA の終わりを読みに行くこと：xHCI 4.19.2 は OCC を 0 から 1 だけで立てると書き、Table 5-27 は両方の遷移で立てると書きます。`hc_ports_again` は OCC に頼らないので、どちらでも動きます。
- USB2 の root port だけを PED で閉じること（`xhci.c:1819-1825`）：Table 5-27 で USB3 の Disabled からの戻りは PLS への 5 の書込みです。4.19.5 の注は USB3 を除いていませんが、その目的は address 0 の取り合いで、root port の USB3 の link には無いものです。差分はコメントだけです。
- SET_HUB_DEPTH の番号 12：USB 3.2 Table 10-8 です（text の抜き出しは値の列が一行ずれています）。
- `usbhub_pending` と `usbhub_reports_lost` の読み書き：`hc_busy`、`hc_recover`、`hc_devices` は全部 worker で走ります。別の thread との競りはありません。
- spinlock：`usbhub_reports_lost` は `ep->lock` を放した後で呼ばれます。`hub_port` の眠りは worker の thread の上です。
- `h->power` の確保と解放、`port_power` の `hc_start` での消去：漏れはありません。
- X だけの絶対座標の装置：`h->mouse` が X と Y の両方を要る（`usbhid.c:634`）ので、`usbhid.c:279` の変更で消える動きはありません。
