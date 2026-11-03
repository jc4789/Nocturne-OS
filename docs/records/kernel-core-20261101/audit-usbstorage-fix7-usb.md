# USB の driver の差分の監査（682f34dd から fe71d739）

## 依頼文

Nocturne OS のカーネルの USB の driver の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 682f34dd HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhid.c kernel/src/dev/usbhub.c kernel/src/dev/usb.h`。
- 今のコード：`kernel/src/dev/xhci.c`、`usbhid.c`、`usbhub.c`、`usb.h`、`kernel/src/fs/store.c` の起動の待ち。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

スコープ：この差分の行が生んだ欠陥か、この差分の行そのものの文書との差だけを報告します。差分の外の前からある欠陥は、port や worker が永久に止まるものだけを報告します。

やること：差分の全部の行を文書の該当の節と照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順を調べます。worker が戻らなくなる道、port が永久に見られなくなる道、起動の settle が遅れる道、良い装置が待たされる道を最初に調べます。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix7-usb.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

指摘は 11 件です。

| 重さ | 数 | 指摘 |
|---|---|---|
| データを失う | 0 | なし |
| 当たれば壊れる | 4 | 1、2、3、4 |
| きれいさ | 7 | 5、6、7、8、9、10、11 |

4 の指摘だけが差分の外の前からある欠陥です。1 の指摘の hub の半分も前からある欠陥です。

先に調べた四つの道の結果は次のとおりです。

- worker が戻らなくなる道は、読んだ範囲に無いです。
- port が永久に見られなくなる道は、指摘 1 と指摘 4 です。
- 起動の settle が遅れる道は、指摘 2、5、6、7 です。
- 良い装置が待たされる道は、指摘 3 と指摘 8 です。

行番号は fe71d739 のものです。

## 当たれば壊れる

### 指摘 1：過電流の後に電源が切れたままの port を、誰も見に来ない

- 場所：`kernel/src/dev/xhci.c:1836-1841`（電源を戻す道）、`xhci.c:1856-1867`（`hc_ports_again`）、`xhci.c:1749-1750`（`port_reset` が変化の旗を全部消す）。hub の側は `kernel/src/dev/usbhub.c:240` と `usbhub.c:345-352`。
- 仕様：xHCI Table 5-27 の bit 3（OCA）、bit 9（PP）、bit 20（OCC）。xHCI 4.19.2 の OCC の段落と、同じ節の Note（reset 中の過電流）。xHCI 4.19.1.2.2。USB 2.0 11.12.5。USB 3.2 10.16.2.6.2 の C_PORT_OVER_CURRENT。
- 差：電源を戻す道は `root_port` の中だけにあり、`root_port` は `port_pending` が立った port にしか走りません。`hc_ports_again` は `PS_CHANGES`、または「接続か stuck」でしか印を付けません。`PP=0` かつ `OCA=0` の port は、変化の旗が無ければ印が付きません。PP が 0 の port は接続も切断も報告しません（Table 5-27 の bit 9）。
- 文書の食い違い：OCC は、Table 5-27 では OCA の両方向で立ちます。4.19.2 では 0 から 1 だけで立ちます。4.19.2 のとおりに作った controller は、過電流の終わりに旗も event も出しません。
- 筋書き A：4.19.2 のとおりの controller で過電流が起きます。`root_port` は `PP=0`、`OCA=1` を見て何もしません。過電流が終わっても旗は立ちません。port は再起動まで死にます。良い装置を差しても見えません。
- 筋書き B：Table 5-27 のとおりの controller で、悪い装置が reset 中に過電流を起こします。4.19.2 の Note により PR は消え、PRC は立たないことがあります。`port_reset` は 500 ms の期限まで待ちます。電源が切れて負荷が消えれば、OCA はその間に 0 へ戻ります。`xhci.c:1750` が OCC を消します。このあと port は `PP=0`、`OCA=0`、旗なしで残り、筋書き A と同じに死にます。
- 筋書き C（hub、前からある）：`usbhub.c:240` の `clear_changes` が `C_PORT_OVER_CURRENT` も消します。USB 2.0 11.12.5 の「別の port の過電流で切れた port」は `C_PORT_OVER_CURRENT` だけが立ち、`PORT_OVER_CURRENT` は立ちません。その旗が reset の間に来ると、hub の port は電源が切れたまま誰にも見られません。hub を抜き差しするまで戻りません。
- 重さ：当たれば壊れる。
- 直す案：`hc_ports_again` の印の条件に `(!(sc & PS_PP) && !(sc & PS_OCA))` を足します。旗でなく今の値で見るので、筋書き A と B の両方が消えます。hub は `usbhub.c:240` を `clear_changes(h, port, change & ~CH_OVER_CURRENT)` にします。hub は旗が残る限り Status Change endpoint で data を返す（USB 2.0 11.12.1）ので、次の回の `hub_port` が電源を戻します。

### 指摘 2：短絡した port の電源を、間を空けずに入れ直し続ける

- 場所：`xhci.c:1836-1841`。hub は `usbhub.c:345-352`。
- 仕様：USB 2.0 11.12.5 の回復の手順（過電流の旗が消えるのを待ち、電源を入れ、数え直す）。xHCI Table 5-27 の bit 3（条件が除かれると OCA は自動で 0 になる）。xHCI 4.19 の実装の注（同じ接続口の port は電源を OR でまとめる）。回数と間隔を決める文書はありません。
- 差：電源を切れば短絡の負荷は消えるので、OCA はすぐ 0 に戻ります。差分の行は、`PP=0`、`OCA=0` を見るたびに無条件で PP を入れます。`usb_port_tries` の間隔はこの道に掛かっていません。xHCI 4.19.4 により過電流で CSC も立つので、`xhci.c:1835` が試しの数も 0 に戻します。
- 筋書き：端子が短絡した装置を root port に差します。OCA が 1 になり、PP が 0 になり、OCA が 0 に戻ります。worker が PP を入れ、20 ms 後にまた過電流になります。これが約 20 ms の周期で、抜くまで続きます。電源の開閉器と装置に突入電流が繰り返し掛かります。同じ接続口の USB2 と USB3 の port が両方巻き込まれます。`port_pending` がこの周期で立つので、`hc_busy` が真を読む回が続き、settle が延びます。その待ちの上限は `FIND_MOST_NS`（60 秒）です。root の道にはログの行がありません。hub の道にはあります。
- 重さ：当たれば壊れる。
- 直す案：port ごとに電源の入れ直しの間隔を持ちます。`usb_port_tries` と同じ倍々（1、2、4 ... 64 秒）にして、`hc_ports_again` の印と `root_port` の両方がその時刻を見ます。数を戻すのは、電源を入れてから一定の時間 OCC が来なかったときだけにします。CSC では戻しません。hub にも同じものを入れます。最初の一回を kprintf に書きます。

### 指摘 3：stuck の間に積んだ回数で、起き上がった装置を一回の失敗で見捨てる

- 場所：`kernel/src/dev/usb.h:41-44`（`usb_port_failed`）、`xhci.c:1750`、`xhci.c:1847`。hub は `usbhub.c:362`。
- 仕様：xHCI Figure 4-27 と 4.19.1.2.5（Error の PP、CCS、PED、PR は 1,0,0,0、Reset は 1,1,0,1）。Table 5-27 の bit 17（CCS の変化で CSC が立つ）。USB 3.2 10.16.2.6.1 の PORT_CONNECTION。
- 差：stuck の port は `count` が 7 まで積みあがり、64 秒ごとに試し続けます。warm reset で起き上がると CCS は 0 から 1 になりますが、その CSC は `port_reset` が消します。`xhci.c:1835` の `usb_port_free` は走らず、`count` は 7 のまま残ります。次の `usb_port_failed` は `stuck` が偽で `count >= 7` なので、`CLOCK_NEVER` にします。
- 筋書き：USB3 の port が起動時から Compliance か Inactive にいます。7 回目の試しは約 63 秒後です。それ以後の warm reset の一回で link が起き上がります。Address Device か最初の GET_DESCRIPTOR が一度だけ失敗します。port はその場で見捨てられ、抜き差しまで試されません。これは USB_PORT_TRIES の「接続された装置を 7 回試す」という設計にも合いません。
- 重さ：当たれば壊れる（抜き差しで戻る）。
- 直す案：`usb_port_failed` の頭で、`if (t->stuck && !stuck) t->count = 1;` とします。stuck から接続へ移ったときに回数を数え直します。

### 指摘 4：報告の止まった hub の port は二度と読まれない（前からある）

- 場所：`usbhub.c:201`（`h->reporting` の失敗にログも代わりの道も無い）、`usbhub.c:370-383`、`xhci.c:1902-1907`（endpoint を `dead` にする）。
- 仕様：USB 2.0 11.12.1 と 11.12.3（port の変化は Status Change endpoint で知る）。文書は endpoint が使えないときの道を決めていません。
- 差：`usbhub_pending` が見るのは、報告が付けた印と `at` の来た port だけです。差分は `ports_to_look_at` を消しましたが、これは `at` のある port だけを読む道で、ここは前から同じです。`port_reset` には `!h->reporting` のときに問い合わせる道がありますが、ふだんの port にはありません。
- 筋書き A：hub の attach で `usb_stream_start` が記憶の不足で失敗します。`power_ports` の最初の一回だけ port が読まれます。その後に差した装置も、抜いた装置も、hub を抜き差しするまで分かりません。ログも出ません。
- 筋書き B：hub の interrupt endpoint が 8 回続けて回復に失敗し、`dead` になります。ログは出ます。hub の下の装置は動き続けますが、新しい接続と切断は見えません。抜かれた子の slot も残ります。
- 重さ：当たれば壊れる。
- 直す案：`!h->reporting` の hub は、全部の port を 1 秒ごとに `pending_mark` します。`usbhub_pending` と `usbhub_work` でそれを見ます。`dead` は hub の driver に伝えて `reporting` を偽にします。hub の control pipe が `xd->ctl_broken` になっていれば、hub ごと外す道も要ります。

## きれいさ

### 指摘 5：過電流の間、USB2 の port の試しが settle を止める

- 場所：`xhci.c:1784-1786`（`port_empty`）、`xhci.c:1846-1847`、`xhci.c:1945-1946`、`usb.h:46-48`。
- 仕様：Table 5-27（CCS は PP が 0 なら 0、PLS は PP が 0 なら未定義）。Table 5-27 の脚注 92（OCA で CCS が消えたときの CSC は任意）と、4.19.4（過電流で PP が 0 になれば CSC を立てる）。文書の中で食い違います。
- 差：差分は `port_empty` に `link_known` を足したので、`PP=0` の port は空と読まれなくなりました。差分の前の USB2 の判定は PP を見ていなかったので、空と読めていました。`xhci.c:1847` の `stuck` は USB2 では常に偽です。
- 筋書き：USB2 の port で悪い装置が試しの最中に過電流を起こします。`at` が立ったまま `PP=0`、`OCA=1` が続きます。脚注 92 の controller では CSC も来ません。CSC が来ても `port_reset` の中なら消えます。`usb_port_trying` が真のまま過電流の間ずっと続き、`hc_busy` も真のままです。`usb_enumerating` を読むのは `store.c:307` だけです。起動のディスクがまだ見つかっていないときの待ちが、最大 60 秒に延びます。指摘 1 の筋書きでは過電流が終わっても解けません。
- USB3 の port はこの道に当たりません。`xhci.c:1847` が `stuck` を真にするからです。
- 重さ：きれいさ（待ちの上限は `FIND_MOST_NS`）。
- 直す案：`port_empty` を、`!(sc & PS_CCS) && !(sc & PS_PR) && (!(sc & PS_PP) || hc->port_major[p] < 3 || PS_PLS(sc) == PLS_RXDETECT)` にします。PP が 0 の port は何も接続されていないと読みます。

### 指摘 6：ディスクでない壊れた装置が settle を 63 秒止め、最後の試しは待ちの上限の外になる

- 場所：`xhci.c:1945-1950`（`hc_busy`）、`usb.h:43`、`kernel/src/fs/store.c:21-22`、`store.c:307`。
- 仕様：文書は決めていません。`usb.h:17` にも、文書がこの時間を決めていないと書いてあります。
- 差：接続されたまま失敗する装置は、7 回の試しが終わるまで `usb_port_trying` が真です。試しの間は 1+2+4+8+16+32 = 63 秒で、これに試しそのものの時間が加わります。`FIND_MOST_NS` は 60 秒です。7 回目の試しは必ず待ちの上限の後になります。
- 筋書き：壊れたキーボードが刺さった機械で、起動のディスクが出てきません。差分の前のコードでは root port の試しは settle を止めないので、10 秒で次へ進みます（コードを読んだ比べで、測っていません）。今のコードでは 60 秒です。このあと panic するか、別のディスクの C: で起動します。起動のディスクが見つかっていれば `boot` が真で先に抜けるので、ふだんの起動は遅れません。
- 同じ道：`usb_attach_child` が hub の段数の上限で NULL を返す port も、決して成功しないのに 7 回試されて settle を止めます（`xhci.c:1694`）。
- 重さ：きれいさ。
- 直す案：settle を止める試しの回数を、`FIND_MOST_NS` の中に収まる数に定数で決めます（例えば、最初の 6 回まで）。段数の上限は hub の側で `CLOCK_NEVER` にして、試しません。

### 指摘 7：SuperSpeed hub の Error の port は接続を読むので、stuck と扱われない

- 場所：`usbhub.c:362`、`usb.h:16-20` のコメント（「a stuck one reads no connection」）。
- 仕様：USB 3.2 10.16.2.6.1 の PORT_CONNECTION（DSPORT.Resetting と DSPORT.Error では前の値を保つ）。10.3.1.4。root の側は xHCI Figure 4-27 のとおりで、Error の CCS は 0 です。コメントの主張は root では正しく、hub では誤りです。
- 差：hub は `usb_port_failed(t, !connected)` と渡します。Enabled から eSS.Inactive へ落ちた port は `connected` が真のままです。`link_stuck` が真でも `stuck` は偽になります。
- 筋書き：SuperSpeed hub の下の装置の link が recovery に失敗します。7 回の warm reset の後に `CLOCK_NEVER` になります。root の port なら 64 秒ごとに続ける所です。その間 `usb_port_trying` は真で、settle を止めます。抜かれれば DSPORT.Disconnected へ移って C_PORT_CONNECTION が立つ（10.3.1.2、10.16.2.6.2）ので、永久には止まりません。
- 重さ：きれいさ。
- 直す案：`usbhub.c:362` を `usb_port_failed(t, link_stuck(h, status))` にします。コメントを、root と hub の違いに合わせて直します。

### 指摘 8：Compliance の port に差した良い装置は最大 64 秒待つ

- 場所：`usb.h:43`（stuck の間隔の上限）、`xhci.c:1777-1781`、`xhci.c:1864`。hub は `usbhub.c:353-356`。
- 仕様：USB 3.2 7.5.5.2（downstream port が Compliance を出るのは warm reset と eSS.Disabled の指示）。xHCI 4.19.1.2.7。xHCI 5.3.9 の CTC（0 なら Compliance への移りは自動で有効）。xHCI 4.19.4（電源を入れたときに最初の LFPS で時間切れになると Compliance へ移り、旗は立たない）。
- 差：差分は間隔の上限を前のまま 64 秒にしています。Inactive は取り外しで RxDetect へ移り（`port_empty` がその回に解く）ます。Compliance は装置を抜いても出ないので、次の warm reset まで何も起きません。`usb.h:18-20` のコメントはこの待ちを認めています。
- 筋書き：壊れた装置で Compliance に入った port から装置を抜き、良い装置を差します。stuck の回数が積もった port では、最大 64 秒（と `hc_ports_again` の 1 秒）の間、良い装置は認識されません。
- 重さ：きれいさ（上限のある待ち）。
- 直す案：stuck の間隔の上限を短くします（例えば 8 秒）。warm reset の相手は動いていない link なので、数秒ごとでも他の仕事を邪魔しません。時間を決める文書は無いので、値はユーザーの判断です。

### 指摘 9：片方の軸だけ absolute の pointer で、もう片方の値が捨てられる

- 場所：`kernel/src/dev/usbhid.c:268-283`、`kernel/src/gui/wm.c:2601-2604`。
- 仕様：HID 6.2.2.5（Absolute と Relative は main item ごとの属性）。HID 5.6（値は別の report でもよい）。軸ごとに違うことを禁じる文は、読んだ節にありません。
- 差：x が absolute で y が relative のとき、`e.absolute` が真になり、`e.y` は 0 のままです。`wm.c` は absolute の event で `dx`、`dy` を読まないので、`e.dy` は捨てられ、カーソルは画面の上端に張り付きます。x が relative で y が absolute のときは、y の値が使われません。y だけの absolute も何も出しません。
- 差分の前も、この形の装置では別の形で誤っていました。
- 重さ：きれいさ（当たる装置はほとんど無いです）。
- 直す案：attach のときに両軸の属性が違う装置を見つけたら、kprintf に書きます。その装置では relative の軸だけを使います。`e.absolute` は両軸が absolute のときだけ立てます。

### 指摘 10：`link_stuck` のコメントの引用が節の内容と違う

- 場所：`usbhub.c:272-273`。
- 仕様：USB 3.2 10.3.1.1、10.3.1.6、10.16.2.6.1。
- 差：コメントは「reset の間と電源の無い間は link の状態に意味が無い（10.3.1）」と書きます。10.3.1 はそう言っていません。10.3.1.1 は Powered-off の link を eSS.Disabled と定めます。PORT_LINK_STATE は link の今の状態を映すと定められています。「PLS を無視せよ」は xHCI 4.19.1.2.5 だけの文です。
- 動きは問題ありません。warm reset の間は Inactive を読むことがあるので、`PS_RESET` を見る判定は要ります。電源を見る判定は無害です。
- 重さ：きれいさ。
- 直す案：コメントを「reset の間は warm reset の前の Inactive を読むことがある。電源の無い port の link は eSS.Disabled で、stuck にならない（10.3.1.1）」にします。

### 指摘 11：USB3 の port は Default の装置のまま残す（4.19.5 の note との差）

- 場所：`xhci.c:1817-1822`。
- 仕様：xHCI 4.19.5 の note（PR が 0 に戻ったら、すぐ Address へ移すか port を無効にする。USB2 と USB3 を分けていない）。4.19.1.2.1（PED の書き込みで Disabled へ）。4.19.1.2.5（Disabled から Reset へは移れない）。4.19.1.2.3（Disabled では Disconnect Detect で移らない）。
- 差：コードは USB2 だけを無効にします。note の字句からは USB3 も対象です。
- 判断：コメントの理由は文書で確かめました。Disabled にすると warm reset が効かず、PLS に 5 を書く道しかありません。その道は再び link を訓練して CSC を立てるので、回数が数え直されます。USB3 の link は一対一で、note の理由である「他の新しい装置の列挙」を妨げません。今のままが正しい判断です。差として残します。
- 重さ：きれいさ（直さなくてよい）。
- 直す案：コメントに「4.19.5 の note の理由は共有のバスの address 0 で、USB3 の link には当たらない」と足します。

## 照らして合っていた手順

- PORTSC のビット位置：CCS 0、PED 1、OCA 3、PR 4、PLS 8:5、PP 9、CSC 17、PEC 18、WRC 19、OCC 20、PRC 21、PLC 22、CEC 23、WPR 31（Table 5-27）。`PS_CHANGES` は bit 17 から 23 で 7 個の旗に合います。`PS_KEEP` は RW1C と動作のビットを含みません。
- `xhci.c:1821` の PED の書き込み：RW1CS で、PR を同時に立てません（脚注 82）。ソフトの書き込みで PEC は立ちません（Table 5-27 の bit 18、4.19.2）。USB2 の Disabled から出るのは PR です（Table 5-27 の bit 1）。4.19.5 の note のとおりです。
- `link_known`：PLS は PP が 0 で未定義です（Table 5-27）。Reset の間は無視します（4.19.1.2.5）。
- `port_stuck`：Error と Compliance は CCS が 0 です（Figure 4-27）。Inactive の PLC は取り外しの通り道では立ちません（Table 5-27 の脚注 94）。PLC は Inactive に入った後の少し遅れで立ちます（同じ表の Note）が、次の event で `root_port` が見るので失われません。`xhci.c:1749` の前にあった `at` の書き込みを消しても、`port_reset` の後の失敗の道は `usb_port_failed` が `at` を立てるので、Inactive の port は失われません。`device_readdress` の `port_reset` も失敗すれば同じ道です。
- 「RxDetect へ移るとき旗が立たない」：Disconnected は CCS が Error と同じ 0 で、PLC の表に無いです（4.19.1.2.6、Table 5-27）。コメントは正しいです。
- `xhci.c:1838-1839` の PP と 20 ms：5.4.8 の頭の文のとおりです。限界は次の節に書きます。
- `connect_debounced`（root と hub）：TATTDB は最低 100 ms で、切断があれば数え直します（USB 2.0 7.1.7.3 の t3、9.1.2 の 3）。USB3 の link には要りません。再試しで省くのは、その間の切断が CSC で試しを数え直すので、文書に反しません。
- `TRSTRCY` の 10 ms：USB 2.0 7.1.7.3 の t6 のとおりです。
- `PORT_ENABLE` の選択子は 1 です（USB 2.0 Table 11-17）。ClearPortFeature で Disabled へ移ります（11.5.1.4）。C_PORT_ENABLE は立ちません（11.24.2.7.2.2）。Enhanced SuperSpeed の hub はこの要求を持ちません（USB 3.2 10.16.2.6.1）。コードは `!h->ss` で分けています。
- `PS_SS_POWER` は bit 9、`PS_RESET` は bit 4、link は 8:5 です（USB 3.2 Table 10-13）。
- hub から ports_to_look_at を消したこと：C_PORT_LINK_STATE は「Compliance へ」「Rx の終端がある eSS.Inactive へ」の完了で立ちます（USB 3.2 10.16.2.6.2）。だから入る方は旗で知れます。出る方は接続の値が変わらなければ旗が無いので、`port_due` のコメントのとおり `at` の時に読みます。消した旧コメントの「旗が立たない」より、今のコメントが正しいです。
- `hc_ports` の `port_major` の判定：Supported Protocol に無い port を参照すると動きが未定義です（xHCI 7.2.2）。これで `port_pending` の印が残り続けることも無くなります。
- `hc_worker` の `now < settle_at`：`settle_at` を過ぎて `hc_busy` が真のとき、差分の前は `next <= now` で眠らずに回っていました。今は最大 1 秒で眠ります。
- `hc_busy` の `hc_running`：動かない controller は `hc_ports` が印を読まないので、差分の前は settle が止まっていました。今は止まりません。
- HID の値の範囲の外：現在の値を変えません（HID 5.10）。`last_x`、`last_y` を保つのはそのとおりです。`x_known`、`y_known` は `h->lock` の下で触れ、`kzalloc` の 0 から始まります（`usbhid.c:609`）。
- 錠：`usb_port_tries` は worker だけが触ります。差分の行は spinlock の下で眠りません。`hub_report` は drain で走りますが、差分はここを変えていません。撃ち落としに関わる行はありません。
- `usb_port_failed` の値：`count` は try の後に 1 以上です。間隔は 1 秒から 64 秒です。`CLOCK_NEVER` の port は `usb_port_try` と `port_due` と `now >= at` が全部偽です。接続が変わると `usb_port_free` が解きます。

### worker が戻らなくなる道の確かめ

- `port_reset`（root）は 500 ms、`link_trained` は 1 秒の期限で終わります。
- `connect_debounced` は root と hub で 5 回までです。
- hub の `port_reset` は `late` で必ず抜けます。
- `hc_command` は 3 秒＋3 秒＋abort の 5 秒で終わり、`wedged` は worker が受けます（`xhci.c:739-760`、4.6.1.2）。
- `control` は 5 秒です（`CONTROL_NS`、USB 2.0 9.2.6.1）。
- `usb_remove` の `sync_drains` は錠を一度取るだけです。
- `usbhid_detach` は眠りません。
- `usbmsc_detach` と `usbnet_detach` は mutex を一つ取ります。持つ者の待ちの長さは読み切っていません（次の節）。
- `hc_ports` と `usbhub_work` の輪は、読み取った語の数で終わります。

### 良い装置の待ちの上限

- 悪い装置の一回の試しは、その間 worker の全部の仕事を止めます。
- 一回の長さは Address Device の時間切れで 11 秒、または GET_DESCRIPTOR の時間切れで 5 秒が目安です。応答したあとで止まる装置は `choose_configuration` の 3 回が加わります。
- これは差分の前の 3 回続けての試しの 3 分の 1 の長さで、差分は短くしています。

## 読み切れなかった範囲

- PDF は開いていません。文字起こしだけを読みました。Figure 4-27 の矢印は文字起こしで失われているので、遷移は 4.19.1.2.x の本文から読みました。
- xHCI 4.19.1.2 の節番号は文字起こしで段組が崩れています。Reset が 4.19.1.2.5、Error が 4.19.1.2.6 であることは、節の順で確かめました。
- `usbmsc.c`、`usbnet.c` は detach だけを読みました。`usbmsc` の `m->lock` と `usbnet_lock` を持つ者の待ちの長さは読み切っていません。worker が戻らない道が無いと言えるのは、この二つの錠を除いた範囲です。
- `transfer_event`、`endpoint_recover`、`tt_clear` は全部を読んでいません。差分はここに触れていません。
- `xhci.c:1839` の後で PP が 1 になったことを読み直していません。Table 5-27 の bit 9 は「次に変える前に読んで確かめる」と定め、脚注 91 は PP の旗が遅れることがあると言います。`sc & PS_KEEP` は PP の値をそのまま書き戻すので、旗が遅れた controller で古い 0 を書くことがあります。脚注の遅れの例は reset の完了待ちで、PP が 0 の port は reset 中でありません。当たる条件を文書から決められないので、指摘にしていません。
- 実機の controller が OCC を 4.19.2 と Table 5-27 のどちらで作っているかは分かりません。指摘 1 はどちらでも当たります。
- 実機、QEMU、ビルドでは何も確かめていません。依頼のとおりです。
- 接続と切断を繰り返す装置は、CSC のたびに試しが数え直されるので、間隔が伸びません。これは「今来たものは前のものでない」という差分の設計どおりで、文書は間隔を決めていません。ログの量と worker の時間を測っていないので、指摘にしていません。
