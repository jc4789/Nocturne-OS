# USB の driver の差分の監査（六回目）

## 依頼文

Nocturne OS のカーネルの USB の driver の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 26b39b07 HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhid.c kernel/src/dev/usbhub.c kernel/src/dev/usb.h`。
- 今のコード：`kernel/src/dev/xhci.c`、`usbhid.c`、`usbhub.c`、`usbmsc.c`、`usb.h`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順を調べます。差分が新しく生んだ行き詰まりを最初に探します。worker が戻らなくなる道、port が永久に見られなくなる道、起動の settle が遅れる道、良い装置が待たされる道を調べます。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/kernel-core-20261101/audit-usbstorage-fix6-usb.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 16 件です。

| 重さ | 件数 |
|---|---|
| データを失う | 0 |
| 当たれば壊れる | 9 |
| きれいさ | 7 |

差分が新しく生んだ待ちの道は 3 件です（指摘 1〜3）。

- 永久に戻らない worker は、差分の中にありません。
- 永久に見られなくなる port は、差分の中にありません。
- 良い装置は、差分のために最大 64 秒待たされます（指摘 1）。
- worker は、差分のために同じ長い待ちを繰り返します（指摘 2、3）。

差分の外に、永久の行き詰まりが 2 件あります。

- 過電流の後の root port は、PP が戻らず、再起動か眠りまで死にます（指摘 5）。
- 止まった controller の worker は、settle の前なら休まず回り続けます（指摘 6）。

行番号は HEAD（682f34dd）のものです。
時間の値はコードの定数からの計算で、実測ではありません。

## 差分が生んだ指摘

### 指摘 1：新しい接続の知らせが、試しの待ちを解かない

- 場所：`kernel/src/dev/xhci.c:1819`、`xhci.c:1787`、`xhci.c:1796`、`xhci.c:1837`、`kernel/src/dev/usbhub.c:281`、`usbhub.c:327-328`。
- 仕様の節：xHCI 4.19.2 と Table 5-27 の CSC（CCS の変化を知らせる）。USB 3.2 Table 10-14 と USB 2.0 Table 11-22 の C_PORT_CONNECTION。USB 2.0 7.1.7.3 の t3（切断があれば debounce をやり直す）。
- 差：CSC と C_PORT_CONNECTION は装置が入れ替わったことを知らせます。コードはそれを読み捨て、`usb_port_try` が `at` より前なら何もしません。待ちが解けるのは、接続なしを読んだときだけです。
- 筋書き 1：壊れた装置が port にあり、`count` が 6 です。
  - 利用者が抜いて、良い装置を差します。
  - worker は別の port の試しで塞がっていて（指摘 2）、接続なしの時を読みません。
  - 次の読みは CCS=1 かつ CSC=1 です。
  - `usb_port_try` は偽を返し、CSC は書き戻しで消えます。
  - 良い装置は最大 64 秒待ちます。
- 筋書き 2：USB2 の port で差し込みの接点が跳ねます。
  - debounce の終わりの一度の読みで CCS=0 です（`xhci.c:1787`）。
  - `root_port_attach` が戻り、失敗として `at` が 2 秒先になります。
  - 直後に CCS=1 に戻っても、次の読みで接続ありなので待ちは解けません。
  - 良い装置が 2 秒待ちます。
  - 差分の前は、この CSC の事象ですぐ試しました。
- 筋書き 3：warm reset が失敗して接続なしで戻り、その直後に link が訓練を終えます。
  - その間の読みが RxDetect でなく Polling なら、`xhci.c:1837` は解きません。
  - 次の読みは CCS=1 で、`at` まで待ちます。
- 重さ：当たれば壊れる。
- 直す案：
  - `root_port` は `CSC && CCS` で `usb_port_free` します。`hub_port` は `CH_CONNECTION && connected` で同じにします。
  - 跳ねる装置が待ちを抜け続けないように、試しの中で接続が消えた場合は失敗に数えません。
  - そのために `root_port_attach` と `hub_port_attach` が「装置が居なくなった」を返すようにします。

### 指摘 2：試しが worker を長く塞ぎ、それが今は繰り返す

- 場所：`kernel/src/dev/usb.h:16-18`（コメント）、`xhci.c:1783-1807`、`xhci.c:1846-1856`、`usbhub.c:271-298`、`usbhub.c:362-369`。
- 仕様の節：文書は再試行の時間を定めません。AGENTS.md の「長いネイティブ処理には中断点を置く」に照らします。
- 差：`usb.h` のコメントは試しが worker の他の仕事の邪魔にならないと書いています。実際の一回の試しは、worker の上で続けて 3 回の試行をします。
- 時間の計算（SET_ADDRESS には答え、GET_DESCRIPTOR に NAK を返し続ける USB2 の装置）：
  - debounce が 100 ms です。
  - reset の待ちが最大 510 ms です（`xhci.c:1743`、`xhci.c:1753`）。
  - GET_DESCRIPTOR の待ちが 5 秒です（`CONTROL_NS`、`xhci.c:123`）。
  - 試行の後の待ちが 100 ms です（`xhci.c:1805`）。
  - 合計は 100 ms + 3 ×（510 ms + 5 s + 100 ms）で、約 16.9 秒です。
  - 64 秒おきなら、worker の約 2 割をこの port が使います。
  - 壊れた port が N 個あれば、一周の塞がりは N 倍です。
- 筋書き：壊れた装置が差さったままの機械で、キーボードを別の port に差します。
  - キーボードは試しが終わるまで列挙されません。
  - 同じ間、stream の復旧（`hc_recover`）、hub の変化、装置の取り外しの処理、致命的な誤りの検出も待ちます。
  - 差分の前は、この待ちは接続の変化ごとに一度でした。
- 重さ：当たれば壊れる。
- 直す案：
  - worker の一周で試行は一つにします。
  - 試行の番号を `struct usb_port_tries` に持たせ、3 回の試行の間に一周を挟みます。
  - これで一度の塞がりは約 5.6 秒以下になり、間に他の仕事が入ります。
  - `usb.h:16-18` のコメントを実際の動きに合わせます。

### 指摘 3：答えない hub を、毎秒以上の頻度で読み続ける

- 場所：`usbhub.c:303`、`usbhub.c:336-341`、`usbhub.c:356-360`。
- 仕様の節：USB 2.0 9.2.6.1（`xhci.c:123` が引く control の上限）。
- 差：試し中の port は、hub の報告が無くても読みます。`port_status` の失敗は `tries` に何も残さず、読みに間隔の伸びがありません。差分の前は、hub の報告が来ない限り読みませんでした。
- 筋書き：hub の control が NAK を返し続ける状態になり、試し中の port が k 個あります。
  - `hub_port` が port ごとに 5 秒待ちます。
  - 一周は 5k 秒です。
  - 一周の終わりに `look_at` は過ぎていて、次の周で同じことをします。
  - worker の時間のほとんどがこの hub に消えます。
  - hub の親の port の取り外しも、周の切れ目まで処理されません。
  - settle の前なら `hc_busy` が真のままで、settle は hub が抜けるまで来ません（`store.c:307` の 60 秒が上限です）。
- 重さ：当たれば壊れる。
- 直す案：
  - `port_status` が失敗したら、その周の hub の読みを打ち切ります。
  - `look_at` を hub ごとの失敗の回数で伸ばします。

### 指摘 4：`port_reset` に足した 3 行は働かない

- 場所：`xhci.c:1749-1751`。
- 仕様の節：xHCI Table 5-27 の PLC と脚注 94。
- 差：コメントはこの PLC だけが Inactive を伝えると書いています。しかし `port_reset` の呼び手は全部 `root_port_attach` の中です（`xhci.c:1791`、`xhci.c:1793`、device_readdress の `xhci.c:1518`）。その後に `usb_port_tried` が必ず `at` を上書きします（`xhci.c:1821`）。
  - 試しが失敗なら、`at` は未来の値になり、3 行が無くても Inactive を見続けます。
  - 試しが成功なら、`at` は 0 に戻ります。
  - どちらも 3 行の結果を使いません。
- 筋書き：壊れません。読む人がこの行に頼って順序を考えます。
- 重さ：きれいさ。
- 直す案：3 行を消します。

## 差分の外の行き詰まり

### 指摘 5：過電流の後の root port は、PP が戻らず永久に見られない

- 場所：`xhci.c:1809-1822`、`xhci.c:2123-2128`。
- 仕様の節：xHCI Table 5-27 の PP（過電流で controller が PP を 0 にする）と OCA。4.19.4（PP=0 の port は働かない）。4.19.1.2.2（PP=1 の書き込みで Disconnected へ）。5.4.8（PP を立てた後 20 ms 待つ）。USB 2.0 11.12.5（OCA が消えたら電源を入れ直して列挙し直す）。
- 差：PP を書くのは `hc_start` だけです。`root_port` は OCC を消すだけで、OCA が 0 に戻っても PP を立てません。hub の側は同じ手順を持っています（`usbhub.c:317-324`）。
- 筋書き：壊れた装置が過電流を起こします。
  - 影響する port の PP が 0 になります。
  - USB2 と USB3 が過電流の回路を共有すれば、両方の port が落ちます（4.24.2）。
  - 利用者が装置を抜き、OCA が 0 に戻ります。
  - その port は接続を報告しないので、何を差しても見えません。
  - 直るのは controller の再起動か、眠りからの復帰だけです。
- 重さ：当たれば壊れる。
- 直す案：
  - `root_port` で OCC を見ます。
  - OCA=0 かつ PP=0 なら PP を書きます。
  - 20 ms 待ちます。
  - PP を読んで確かめます（Table 5-27 の PP の要求）。
  - その port を pending にします。
  - `PS_OCA`（bit 3）を足します。

### 指摘 6：止まった controller の worker が、settle の前なら休まず回る

- 場所：`xhci.c:1847`、`xhci.c:1915-1924`、`xhci.c:2151-2187`、`xhci.c:2226-2231`。
- 仕様の節：文書の外の、コードの制御の欠陥です。差分は変えていません。
- 差：`hc_ports` は controller が走っていないと `port_pending` を消しません。`hc_busy` はその bit を見ます。`hc_restart` は `port_pending` を消さず、`hc_settled` も呼びません。
- 筋書き：`hc_start` が全部の port に印を付けた直後に HSE が出ます。
  - `hc_restart` の中の `hc_start` が失敗するか、controller が止まりません。
  - `running` は偽で、`port_pending` は残ります。
  - `hc_busy` はずっと真で、settle しません。
  - `next` は `settle_at` 以下で、`next > now` が偽なので待ちません。
  - worker は CPU を使い切って回り続けます。
  - `usb_enumerating` もずっと真です。
- 重さ：当たれば壊れる。
- 直す案：
  - `hc_restart` の入口で `port_pending` を消します。
  - `hc_busy` は `hc_running` が偽なら偽を返します。
  - 再起動を諦めた道で `hc_settled` を呼びます。

### 指摘 7：試し中の port を settle が数えない

- 場所：`xhci.c:1915-1924`、`xhci.c:2226`、`kernel/src/fs/store.c:307`、`store.c:315`。
- 仕様の節：文書は時間を定めません。`usb.h:143` の「まだ見ている」の主張に照らします。
- 差：装置のある port が再試行を待っていても、`hc_busy` は偽になります。`usb_enumerating` はもう見ていないと答えます。
- 筋書き：起動の媒体の USB メモリーが、最初の試しに失敗します。
  - 再試行は約 2 秒後、その約 4 秒後、その約 8 秒後です。
  - 試しの時間を除いても、合計 14 秒です。
  - 媒体が 9 秒で準備できても、次の試しは 14 秒より後です。
  - `store.c:307` は 10 秒で待ちをやめます。
  - 他に C: が無ければ `store.c:315` が panic します。
- 差分の前は、3 回の試行の後に二度と試さなかったので、同じ媒体はやはり見つかりませんでした。差分はこの道を悪くしていません。再試行の仕組みが起動の待ちにつながっていないことが残りです。
- 重さ：当たれば壊れる。
- 直す案：
  - `settle` の前は、接続のある port の `tries.at` を busy に数えます。
  - その数え方に `FIND_MOST_NS` より短い上限を設けます。

## 差分に触れる残りの指摘

### 指摘 8：リセット中の PLS を読む道が残っている

- 場所：`xhci.c:1764-1765`（link_trained）、`xhci.c:1750`、`xhci.c:1779`、`xhci.c:1836-1837`、`usbhub.c:279-280`、`usbhub.c:325-326`。
- 仕様の節：xHCI 4.19.1.2.5（Reset の間の PLS は無視する）。Table 5-27 の PLS（PP=0 の間は未定義）と脚注 83（reset は割り込む作りでない）。USB 3.2 10.3.1 の図の DSPORT.RESETTING（PLS Undefined）。
- 差：差分は `port_stuck` に PR の確かめを入れました。同じ規則を要する次の読みには入っていません。
  - `link_trained` は PR を見ません。
  - `hub_port` と `hub_port_attach` は PORT_RESET（bit 4）を見ません。
  - `port_stuck` と `hc_ports_again` は PP を見ません。
- 筋書き 1：HCRST が port を Reset に入れます（4.19.1.2.5）。
  - 最初の周で CCS=1、PR=1 を読みます。
  - `link_trained` が未定義の PLS を 6 か 10 と読むと、偽を返します。
  - reset の途中に WPR を書きます。
- 筋書き 2：過電流で PP=0 になった USB3 の port の未定義の PLS を、Compliance と読みます。
  - `port_stuck` が真になります。
  - 効かない warm reset で worker が 510 ms 待ちます。
- 重さ：当たれば壊れる。
- 直す案：
  - PLS を読む判定を一つの関数にまとめ、`PR` と `!PP` で偽を返します。
  - `usbhub.c` に `PS_RESET` を足し、`hub_port` と `hub_port_attach` の判定を同じ形にします。

### 指摘 9：失敗した装置の port を有効のまま残す

- 場所：`xhci.c:1797-1806`、`usbhub.c:287-297`、`usbhub.c:329-330`。
- 仕様の節：xHCI 4.19.5 の注（PR が下りたら装置を Address にするか、port を無効にする）。USB 2.0 9.1.2 の手順 3〜5。USB 2.0 11.5.2.2（port が有効で Default の装置は系の誤りを生む）。USB 2.0 11.5.1.4（ClearPortFeature(PORT_ENABLE) で Disabled へ）。
- 差：`device_attach` が失敗しても、port を無効にしません。差分は再試行を足しましたが、試しの間も port は有効のままです。
- 筋書き：USB 2.0 の hub に、SET_ADDRESS に失敗した装置が残ります。
  - その装置は Default の状態で、address 0 に答えます。
  - 同じ hub の別の port に良い装置を差します。
  - reset の後、二つの装置が address 0 に答えます。
  - 良い装置の列挙が崩れます。
- 重さ：当たれば壊れる。
- 直す案：
  - USB 2.0 の hub では、`hub_port_attach` の失敗の道で ClearPortFeature(PORT_ENABLE) を送ります。
  - SuperSpeed の hub にこの要求はありません（USB 3.2 10.3 の図の注）。
  - root port は link が port ごとに別なので、この衝突は起きません。4.19.5 の注に従うなら USB2 の root port も PED に 1 を書きます。

### 指摘 10：debounce が切断でやり直さない

- 場所：`xhci.c:1786-1787`、`usbhub.c:275`、`usbhub.c:278-281`。
- 仕様の節：USB 2.0 7.1.7.3 の t3（TATTDB は最小 100 ms で、切断があればやり直す）。USB 2.0 9.1.2 の手順 3。
- 差：100 ms の終わりに一度だけ接続を読みます。その間の切断と再接続を見ません。root port の CSC、hub の C_PORT_CONNECTION で分かります。
- 筋書き：差し込みの途中で接点が 50 ms の時点で切れて戻ります。
  - 装置の電源が安定して 100 ms になる前に reset を受けます。
  - 列挙に失敗すれば、差分のために 2 秒待ちます。
- 重さ：当たれば壊れる。
- 直す案：
  - 100 ms の終わりに CSC を読み、立っていれば消してもう一度待ちます。
  - 待ちの回数に上限を設けます。
  - hub は `port_status` の `change` で同じにします。

### 指摘 11：hub のコメントが USB 3.2 10.16.2.6.2 と違う

- 場所：`usbhub.c:333-335`。
- 仕様の節：USB 3.2 10.16.2.6.2 の C_PORT_LINK_STATE と C_PORT_CONNECTION。USB 3.2 10.3 の図の DSPORT.ERROR。
- 差：コメントは「あきらめた link」は変化の bit を立てないと書いています。文書は、Rx の終端がある状態で eSS.Inactive か Compliance へ移ると C_PORT_LINK_STATE を立てると定めます。
  - 「離れた port」も半分だけ正しいです。
  - DSPORT.ERROR は CCS を変えないので、CCS=1 から入った port が切れれば C_PORT_CONNECTION が立ちます。
  - 立たないのは、Training から CCS=0 のまま入った port が離れたときです。
- 筋書き：毎秒の読みの理由を誤って伝えます。読み自体は再試行の時を計るために要ります。動きは壊れません。
- 重さ：きれいさ。
- 直す案：コメントを「CCS=0 で入った Inactive の port が離れても変化の bit は立たない。試しの時は読んで計る」に直します。

### 指摘 12：`hc_ports_again` のコメントと動きの差

- 場所：`xhci.c:1824`、`xhci.c:2219`。
- 仕様の節：xHCI 4.19.2（コメントの主張自体は合っています）。
- 差：コメントは「worker の毎秒」と書いています。実際は事象のたびの毎周で、最大 255 個の MMIO を読みます。
- 筋書き：壊れません。kick が多い機械で読みの回数が増えます。
- 重さ：きれいさ。
- 直す案：コメントを「worker の周ごとに、遅くとも毎秒」に直すか、1 秒に一度に絞ります。

### 指摘 13：`tt_clear` の偽の扱いが道ごとに違う

- 場所：`xhci.c:839`、`xhci.c:873`、`xhci.c:1233-1235`、`xhci.c:1255-1256`、`xhci.c:1506`、`xhci.c:1875-1883`。
- 仕様の節：USB 2.0 11.17.5、11.24.2.3。xHCI 4.6.8 の注。
- 差：コメントは偽なら buffer が塞がったままかもしれないと書いています。`pipe_reset` はこれを失敗にします。`endpoint_recover` と `address_device` は捨てます。
  - control の失敗と `usb_bulk` の timeout の後には、もう一度 clear する道がありません。
  - `hc_recover` の道では、hub の clear の失敗が装置の endpoint の失敗の数に入り、8 回で `dead` になります。
- 筋書き：hub の一時の不調で TT の buffer が残ると、control の道ではそのままです。同じ TT の他の装置の bulk と control が止まります。文書によれば、健全な hub はこの要求を断りません（11.24.2.3）。
- 重さ：きれいさ。
- 直す案：
  - 結果を捨てる道を無くし、失敗を hub の持ち物として覚えます。
  - hub の次の仕事で clear をやり直します。

### 指摘 14：絶対座標の pointer の最初の報告が片方の軸だけのとき

- 場所：`kernel/src/dev/usbhid.c:267`、`usbhid.c:271-275`。
- 仕様の節：HID 1.11 5.6、5.10。
- 差：差分は軸が別の報告で来る形を受けます。報告の前の値が無いときも `last_x`、`last_y` の 0 を座標として渡します。`x_came ? h->x.relative : h->y.relative` は、片方の軸の種類を両方に当てます。
- 筋書き：X と Y を別の報告で出す絶対座標の装置で、最初の X の報告が pointer を上の端へ動かします。Y の報告で戻ります。
- 重さ：きれいさ。
- 直す案：
  - 両方の軸の値が一度来るまで、絶対座標の事象を出しません。
  - 種類は軸ごとに判定します。

### 指摘 15：直らない条件も永久に再試行する

- 場所：`xhci.c:1693`、`usbhub.c:296`、`usbhub.c:329-330`、`xhci.c:1798`。
- 仕様の節：xHCI Table 7-14（hub の段数）。USB 2.0 4.1.1 の段の上限（根を除く hub は五つまで）。文書は再試行を定めません。
- 差：段数の上限を超えた `usb_attach_child` は、slot を取らずに NULL を返します。この失敗は何度試しても同じです。それでも port は 3 回 reset され、最大 64 秒おきに繰り返します。
  - USB2 の失敗の試しは一回に 3 行以上の log を書きます（`xhci.c:1634` など）。
  - 装置が差さったままなら、64 秒おきの場合で 1 日に約 4000 行です。
- 筋書き：段を超えた hub の下の装置が、繰り返し reset されます。log が `System.log` を伸ばします。
- 重さ：きれいさ。
- 直す案：
  - 段の超過を「直らない失敗」として返し、port の変化まで試しません。
  - log を port ごとに最初の一度と、回数のまとめにします。

### 指摘 16：事象の port は Supported Protocol で絞られていない

- 場所：`xhci.c:638`、`xhci.c:1853`、`xhci.c:1810`。
- 仕様の節：xHCI 7.2.2（定義の無い port 番号を触ると未定義の動き）。
- 差：差分は `hc_start` と `hc_ports_again` で `port_major` が 0 の port を除きました。`root_port` の入口と `hc_ports` は絞っていません。
- 筋書き：定義の無い port 番号の事象が来ると、その PORTSC を読み書きします。文書の範囲では、そのような事象は来ません。
- 重さ：きれいさ。
- 直す案：`hc_ports` の `p <= hc->max_ports` の判定に `hc->port_major[p]` を足します。

## 照らして合っていた手順

### xHCI の port

- PORTSC の bit の位置は Figure 5-20 と Table 5-27 に合っています（PR 4、PLS 8:5、PP 9、PIC 15:14、CSC 17、PEC 18、WRC 19、OCC 20、PRC 21、PLC 22、CEC 23、WCE から WOE 27:25、WPR 31）。
- `PS_KEEP` と `PS_CHANGES` の書き戻しは、RW1C を保つ規則に合っています。
- 4.19.2 の「全部の変化の bit が消えるまで事象が出ない」は、コメントの主張のとおりです。
- `hc_ports_again` が残った変化の bit を拾うのは、この規則に合う手当てです。
- Compliance に入るときに変化の bit が立たないことは、4.19.4 に明記があり、Table 5-27 の PLC の表にも無いことで合います。
- CTC と CTE の扱いのコメントは 5.3.9 と 4.19.1.2.4.1 に合っています。
- Inactive から RxDetect へ離れても PLC が立たないことは、Error の節（4.19.1.2.6）と脚注 94 に合っています。
- 図 4-27 の Error と Compliance の組は PP=1、CCS=0、PED=0、PR=0 で、`port_stuck` の前提に合っています。
- warm reset の完了の判定（WRC と PRC）は 4.19.5 と 4.19.5.1 に合っています。
- reset の後の 10 ms は USB 2.0 7.1.7.5 の TRSTRCY に合っています。
- 定義の無い port を `hc_start` で触らないことは 7.2.2 に合っています。
- CAS は HCRST の後に見る必要がありません。4.19.8 の条件は D3 の場合で、このコードは眠りの後に HCRST をします。

### hub

- Table 10-9 の feature selector の値（25、26、28、29 など）は合っています。
- Table 10-14 の bit の位置と `clear_changes` の対応は合っています。
- Table 10-13 の PORT_LINK_STATE は bit 8:5 で、値 6 と 0xA は合っています。
- trained の link が Enabled であるというコメントは 10.3.1.5 に合っています。
- BH_PORT_RESET と PORT_RESET を DSPORT.ERROR に送るのは 10.3.1.6 に合っています。
- status change の bit が消えるまで報告が繰り返されることは USB 2.0 11.12.4 に合っています。
- したがって `hub_port` の最初の読みの失敗で pending の bit を捨てても、port は見失われません。

### TT と pipe

- CLEAR_TT_BUFFER の wValue は Table 11-18 に合っています（endpoint 3:0、address 10:4、type 12:11、direction 15）。
- wIndex の 1 は 11.24.2.3 と、alternate 0 の hub が single TT で動くという 11.23.1 の記述に合っています（Slot Context の MTT も 0 です）。
- `pipe_reset` の halted の道の順序（Reset Endpoint、CLEAR_TT_BUFFER、ENDPOINT_HALT、Set TR Dequeue）は 4.6.8 の注に合っています。
- `cleared` を最後に評価するので、他の手順は省かれません。
- 2 回目の `pipe_reset` は、`set_dequeue` が済んだ Stopped の道から正しく Drop と Add に進みます。
- Address Device の失敗の後の clear は 4.6.5 の注に合っています。

### HID

- 軸ごとの報告の扱いは HID 1.11 5.6 に合っています。
- 範囲の外の値で今の値を保つのは 5.10 に合っています。
- `value_came` は 8.4 の短い報告の規則に合っています。

### 順序と錠

- `usb_port_try`、`usb_port_tried`、`usb_port_free` は worker の上だけで呼ばれます（`usb.h` の主張のとおりです）。
- `hc_busy` と `hc_devices` の `usbhub_pending` も worker の上です。`tries` と `look_at` に競りはありません。
- `usb_port_tried` の shift は `NS_PER_S`（`uint64_t`）に対して最大 6 で、溢れません。
- 間隔は 2、4、8、16、32、64 秒で、コメントの上限に合っています。
- 「各 port を一度」にした `hc_ports` と `usbhub_work` は、仕事を失いません。
  - 事象は `hc_kick` で `kicks` を進めます。
  - `seen` は周の頭で読むので、次の待ちはすぐ戻ります。
  - `hc_ports_again` と `ports_to_look_at` の印は、同じ周で消費されます。
- `hc_ports` の周の待ちは `wq_wait_unless` で、`kernel.md` の規則に合っています。
- 差分は新しい spinlock を足していません。眠る呼出しは spinlock の外です。
- `tt_clear` の入れ子は子の `ctl_mutex` から親の `ctl_mutex` への一方向です。順序の循環はありません。
- `usbhub_prepare` と `usbhub_detach` の `tries` の確保と解放は対になっています。
- `hc_start` の `memset` は `hc_ports` より前で、同じ worker の上です。
- `usb.h` の `usb_bulk` のコメントの `*moved` の説明は、`xhci.c:1271-1311` の動きに合っています。
- `usbmsc.c` の `bot` は timeout の `moved` が 0 でも `BOT_BROKEN` に行き、データを正常として渡しません。データを失う道は見つかりませんでした。

## 読み切れなかった範囲

- 実機と QEMU の動きは見ていません。依頼のとおり、ビルドも実行もしていません。
- `usbnet.c` は読んでいません。`pipe_reset` が偽を返す増えた道が網の装置に与える影響は、`hc_recover` の側までしか確かめていません。
- `input_mouse` の受け手は読んでいません。指摘 14 の画面の動きは、受け手が座標をそのまま使うという前提です。
- 図 4-27 と USB 3.2 の hub の図は、文字起こしの文字の並びから読みました。PDF の図は開いていません。
- xHCI の 4.19.1.2 の小節の番号は、文字起こしで見出しの行が崩れています。Error が 4.19.1.2.6、Compliance が 4.19.1.2.7 というのは見出しの順からの読みです。
- 指摘 2 の 16.9 秒は一つの故障の形の計算です。Address Device が答えない形では `hc_command` の 3 秒と abort の 5 秒に変わります。
- CLEAR_TT_BUFFER を実際に断る hub があるかは、文書に従い無いものとして扱いました。
