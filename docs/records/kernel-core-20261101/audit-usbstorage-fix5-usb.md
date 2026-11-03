# USB の driver の差分の敵対的な監査（五回目）

## 依頼文

Nocturne OS のカーネルの USB の driver の差分を、公式の文書と敵対的に照らしてください。読むだけで、ソースは変えません。ビルドも QEMU も動かしません。ダウンロードもしません。

場所：作業木 `D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`。今の版は枝の HEAD です。git は単独の簡単な命令で打ち、`git -C` は使いません（cwd は作業木です）。

材料：
- 差分：作業木で `git diff 883c546d HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhid.c kernel/src/dev/usb.h`。
- 今のコード：`kernel/src/dev/xhci.c`、`usbhid.c`、`usbhub.c`、`usbmsc.c`、`usb.h`。
- 公式の文書の文字起こし：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt、hid.txt、hut.txt、bot.txt。元の PDF は `D:\Programes\OS DEV INFO`。
- 規則：`.claude/rules/kernel.md`（錠、眠り、撃ち落とし）と `AGENTS.md`。

やること：差分の全部の行を文書の該当の節と一字ずつ照らし、順序・値・ビット位置・状態の遷移・誤りの道・競り・錠の順を調べます。差分が新しく生んだ行き詰まりを最初に探します。動いているように見えることを根拠にしません。コードのコメントの主張も確かめます。節番号は自分で開いて確かめます。

報告：
- `docs/records/kernel-core-20261101/audit-usbstorage-fix5-usb.md` に全文を書きます。先頭にこの依頼文をそのまま載せます。このファイルだけを新しく作ります。`docs/records/kernel-core-20261101/` の他の監査の報告は読みません。
- 指摘ごとに、場所（ファイルと行）、仕様の節、差、当たる筋書き、重さ（データを失う／当たれば壊れる／きれいさ）、直す案を書きます。
- 照らして合っていた手順と、読み切れなかった範囲も分けて書きます。
- 日本語で書きます。普通の日本語で一文に一つのこと。仕様の原文は長く写さず、節番号と要旨を書きます。OS のソースコードは参照しません。
- 最後は SubagentHandback で、指摘の数と重さの内訳、報告のパスを短く返します。

## 結論

- 差分が新しく生んだ行き詰まり（錠の輪、終わらない待ち）は無い。
- 指摘は 13 件ある。
- 重さの内訳は、データを失う 0 件、当たれば壊れる 8 件、きれいさ 5 件である。
- 一番重いのは指摘 1 である。接続できない装置が一つあると、その controller の worker が `hc_ports` から戻らなくなる。これは差分の前からある欠陥で、差分の backoff はこの道を守っていない。
- 差分が新しく生んだ指摘は 4、5、6、7、11、12 である。
- 差分の前からある欠陥で差分の行に関わる指摘は 3、8、9、10 である。
- 差分の外の指摘は 1、2、13 である。

## 行き詰まりの調べ

差分が足した錠と待ちを、次のとおり順に調べた。

- `tt_clear` の呼び手は `endpoint_recover`（`xhci.c:873`）と `pipe_reset`（`xhci.c:1233`、`1254`）と `address_device`（`xhci.c:1505`）である。
- `control_recover` は子の `ctl_mutex` を持ったまま `endpoint_recover` を呼ぶ。そこから `tt_clear` が親の hub の `ctl_mutex` を取る。
- この順は常に子から親である。親の `ctl_mutex` を持って子の `ctl_mutex` を取る道は無いので、輪はできない。
- `tt_clear` の間、`cmd_mutex` と `in_mutex` は持っていない。`pipe_reset` の `in_mutex` は `xhci.c:1248` で放してある。
- spinlock の下で `tt_clear` を呼ぶ道は無い。
- usbmsc の thread が `m->lock` を持って `tt_clear` にいるとき、worker は `usbmsc_detach` で `m->lock` を待つ。
- この待ちは終わる。hub が消えるなら `usb_remove` が先に hub の `removed` を立てて `ctl_wq` を起こす。子だけが消えるなら hub の要求は drain で完了するか、5 秒で切れる。
- hub が応答しないとき、`control` の時間切れの一回は最長で約 15 秒かかる。内訳は子の 5 秒と、hub への `CLEAR_TT_BUFFER` 二回の 10 秒である。これは有限である。
- `hc_stuck_ports` は spin しない。`root_port` の stuck の道が `port_warm_at` を未来へ進めるので、同じ port を次に見るのは 2 秒以上先である。
- warm reset の PRC と WRC の event で `root_port` に戻っても、stuck のままなら `xhci.c:1790` で帰り、新しい event を生まない。

## 指摘

### 指摘 1：接続に失敗する装置で worker が `hc_ports` から戻らない

- 場所：`kernel/src/dev/xhci.c:1799-1819`（`root_port` の輪）、`xhci.c:1838-1847`（`hc_ports`）、`xhci.c:2195-2209`（worker の順）。
- 仕様：xHCI 4.19.2。change bit の立ち上がりで PSCEG が 0 から 1 になると Port Status Change Event が一つ出る。xHCI 4.19.5.1。reset の完了で PRC が立つ。
- 差：`root_port` の 3 回の試しは一回の呼出しの中だけの上限である。呼出しをまたぐ上限と backoff は無い。差分の backoff は `stuck` の port だけを守る。
- 筋書き：
  1. USB2 か USB3 の root port に、Address Device か descriptor の読出しに失敗する装置がつながっている。
  2. `device_attach` が NULL を返す。
  3. 次の試しの `port_reset` が PRC を立て、drain が `port_pending` に port の bit を立てる。
  4. 3 回の試しの後、`hc_ports` の内側の `for` が `port_pending` をもう一度取り、同じ port の `root_port` へ入る。
  5. 装置が抜かれるまで 3 から 5 を繰り返す。
- 結果：
  - worker は `hc_recover`、`hc_devices`、`hc_stuck_ports`、`USBSTS` の検査、`hc_settled` へ進まない。
  - hub の後ろの装置は列挙されない。止まった endpoint は回復しない。
  - port 32 以上は `port_pending` の次の語なので見られない。
  - `wedged` になった controller は `hc_restart` へ進めない。`hc_command` が -1 を返し、`device_attach` が即座に失敗するので、この輪はかえって速く回る。
  - 起動の disk がこの controller の hub の後ろにあれば現れない。`fs/store.c:307` は `FIND_MOST_NS` の 60 秒まで待って進む。
- 重さ：当たれば壊れる。
- 直す案：
  - `port_warm_at` を「この port を次に試してよい時刻」へ広げ、`device_attach` が全部の試しで失敗したときにも進める。
  - `root_port` は stuck でなくても `now < port_warm_at[p]` なら帰る。
  - `hc_stuck_ports` は `CCS` が立って `root[p]` が NULL の port も、時刻が来たら `port_pending` に立てる。
  - `hc_ports` の内側の `for` を一回きりにする。残りの bit は次の一周で扱い、worker の他の仕事へ必ず戻る。

### 指摘 2：hub の port には root port と同じ backoff が無く、同じように worker を止める

- 場所：`kernel/src/dev/usbhub.c:291-316`（`hub_port`）、`usbhub.c:328-339`（`usbhub_work`）。
- 仕様：USB 3.2 10.16.2.6.2 の `C_PORT_LINK_STATE`。port の link が Compliance か、Rx の終端がある eSS.Inactive へ移ると立つ。
- 差：差分は root port にだけ backoff を入れた。hub の port は stuck を見るたびに warm reset を 3 回まで打ち、次の変化でまたすぐ打つ。
- 筋書き：
  1. SuperSpeed hub の port に link が訓練できない装置がつながっている。
  2. `hub_port` が warm reset を打ち、`port_reset` が変化の bit を消す。
  3. 3 回の試しの後で port がまた eSS.Inactive へ入り、`C_PORT_LINK_STATE` が立つ。
  4. 状態の変化の報告が `pending` を立てる。
  5. `usbhub_work` の内側の `for` がそれを取り、`hub_port` へ戻る。
  6. `hc_devices` から戻らないので、`hc_ports` と `hc_recover` が止まる。
- 重さ：当たれば壊れる。
- 直す案：
  - stuck の判定と次の試しの時刻を一つの部品にまとめ、root port と hub の port が同じものを使う。
  - hub の状態に port ごとの時刻を持つ。
  - `usbhub_work` の内側の `for` を一回きりにする。

### 指摘 3：読みと書きの間に立った change bit で、その port の event が止まる

- 場所：`kernel/src/dev/xhci.c:1778-1779`（`root_port`）、`xhci.c:1747-1748`（`port_reset`）、`xhci.c:1827`（`hc_stuck_ports`）。
- 仕様：xHCI 4.19.2 の Figure 4-34 の注。全部の change bit が消えるまで、その port の Port Status Change Event は出ない。
- 差：コードは読んだ bit だけを消し、読み直さない。読みと書きの間に別の bit が立つと、その bit は残る。PSCEG は 1 のままなので、次の event は出ない。
- 筋書き：
  1. 接続中の装置の port で `root_port` が `PORTSC` を読む。
  2. その直後、書く前に装置が抜かれて `CSC` が立つ。
  3. 書きは読んだ bit だけを消すので、`CSC` が残る。
  4. event は二度と来ない。`root[p]` は残り、port は装置の抜き差しに応えなくなる。
- 差分との関係：`hc_stuck_ports` が救うのは `root[p]` が NULL で Inactive か Compliance の USB3 port だけである。USB2 の port と、装置のある port は救わない。
- 重さ：当たれば壊れる。窓は短い。
- 直す案：
  - `hc_stuck_ports` を全部の port の検査へ広げ、`sc & PS_CHANGES` が残っていれば `port_pending` に立てる。
  - または `root_port` が書いた後に読み直し、change bit が残る間は繰り返す。

### 指摘 4：抜かれた悪い装置の backoff が、次の装置を最長 64 秒待たせる

- 場所：`kernel/src/dev/xhci.c:1790`、`xhci.c:1827`、`xhci.c:1831-1834`。
- 仕様：xHCI 4.19.1.2.6。Error の状態は Disconnect Detect で Disconnected へ移る。この移りに change bit は立たない（Error の CCS は 0 で、変わらない）。
- 差：`hc_stuck_ports` の 1827 行は `now < port_warm_at[p]` の port を飛ばす。忘れる判定（`PLS_RXDETECT` で `CCS` が 0）はその後ろにある。コメントの「a good device to come」は、この間の port には当たらない。
- 筋書き：
  1. 悪い装置で `port_warms` が 6 まで進み、`port_warm_at` が 64 秒先にある。
  2. 利用者が装置を抜く。port は RxDetect へ移るが、時刻が来るまで忘れられない。
  3. 別の装置を差す。その装置の最初の訓練が eSS.Inactive へ落ちる。
  4. PLC が立っても `xhci.c:1790` で帰る。warm reset は最長 64 秒後である。
- 重さ：当たれば壊れる。遅れは 64 秒までで、その後は直る。
- 直す案：忘れる判定を時刻の判定の前へ移す。warm reset が失敗した直後の一瞬の RxDetect で忘れることがあるが、そのときは次の warm reset が 2 秒後になるだけである。

### 指摘 5：Reset の状態の PLS を読んでいる

- 場所：`kernel/src/dev/xhci.c:1772-1775`（`port_stuck`）、`xhci.c:1831`（`hc_stuck_ports`）。
- 仕様：xHCI 4.19.1.2.5。Reset の状態では、software は PLS の値を無視する。xHCI 4.19.1.2.3 の注。Disconnected の port に HCRST が来たとき、xHC は PR や WPR を立ててよい。
- 差：`port_stuck` と忘れる判定は `PS_PR` を見ない。
- 筋書き：
  - `hc_start` が全部の port を `port_pending` に立て、`root_port` が HCRST の後の Reset の最中の port を読む。
  - PLS がたまたま 10 なら stuck と見て `port_warms` が進み、WPR を書く。
  - PR が 1 の間の書きは Table 5-27 の脚注 83 で無視されるので、害は `port_warms` が一つ進むことと、`port_reset` の待ちだけである。
- 重さ：きれいさ。
- 直す案：`port_stuck` と忘れる判定の条件に `!(sc & PS_PR)` を足す。

### 指摘 6：`port_reset` が消す PLC で、Inactive の証拠が失われる

- 場所：`kernel/src/dev/xhci.c:1747-1748`（`port_reset`）、`xhci.c:1774`（`port_stuck`）、`xhci.c:1801`（stuck でない道の warm reset）、`xhci.c:1517`（`device_readdress` の hot reset）。
- 仕様：xHCI Table 5-27 の PLC と脚注 94。PLC は、装置が残って eSS.Inactive へ入った後の最初の Disconnect.Detect から Quiet への移りで一度だけ立つ。USB 3.2 7.5.2 の `teSSInactiveQuietTimeout` は 12 ms である。
- 差：差分の前は PLS だけで stuck を決めていた。差分の後は、`port_warm_at` が 0 の port で Inactive を stuck と見る証拠が PLC の一度だけになった。`port_reset` の最後の書きは、読んだ PLC を見ずに消す。
- 筋書き：
  1. stuck でない port で、`device_attach` の失敗の後の warm reset か、`device_readdress` の hot reset が走る。この時点で `port_warm_at` は 0 である。
  2. reset は成功して port が Enabled になる。直後に link が eSS.Inactive へ落ちる。
  3. worker が負荷で 12 ms 以上遅れ、`port_reset` の読みが PLC と Inactive を同時に見て消す。
  4. その後の `root_port` は、PLC が 0 で `port_warm_at` が 0 の Inactive を見て帰る。
  5. `hc_stuck_ports` も何もしない。warm reset は二度と打たれず、port は装置を抜き差しするまで止まる。
- 重さ：当たれば壊れる。窓は狭い。
- 直す案：`port_reset` の読みで PLS が Inactive で PLC が立っていたら、`port_warm_at[p]` が 0 のときに今の時刻を入れる。これで `port_stuck` の「前に stuck だった」の道へ入る。

### 指摘 7：`hc_restart` が backoff の状態を消さない

- 場所：`kernel/src/dev/xhci.c:268-271`（「0: not stuck」のコメント）、`xhci.c:2138-2174`（`hc_restart`）、`xhci.c:2063-2066`（`hc_start` の初期化）。
- 仕様：xHCI 4.19.1.2.3、4.19.1.2.5、4.19.2。HCRST で port は Disconnected か Reset へ移り、change bit は消える。
- 差：HCRST の後も `port_warms` と `port_warm_at` は前の値のまま残る。コメントの「0: not stuck」は、前の値が残った port では成り立たない。
- 筋書き：
  - 眠りからの復帰か fatal error の後で `hc_start` が走る。
  - `port_warm_at` が 64 秒先のままである。
  - 今度の link が eSS.Inactive へ落ちても、warm reset は最長 64 秒後になる。
  - 通り過ぎるだけの Inactive も、PLC なしで stuck と数える。
- 重さ：当たれば壊れる。遅れは 64 秒までで、その後は直る。
- 直す案：`hc_start` が HCRST の後で `port_warms` と `port_warm_at` を 0 にする。

### 指摘 8：`CLEAR_TT_BUFFER` の失敗を捨てている

- 場所：`kernel/src/dev/xhci.c:843-852`（`tt_clear` は void）、`xhci.c:873`、`xhci.c:1233`、`xhci.c:1254`。
- 仕様：USB 2.0 11.17.5。endpoint の halt の処理の一部として、host の software は Clear_TT_Buffer で buffer を busy でなくする。11.24.2.3。buffer に無い取引への要求は no-op で、誤りではない。
- 差：要求が失敗しても誰も知らない。差分はこの呼出しを時間切れの道と止まっていない `pipe_reset` の道へ広げた。`pipe_reset` は `tt_clear` が失敗しても成功を返せる。
- 筋書き：
  1. hub が一時的に要求へ応えない。
  2. TT の buffer は busy のままである。
  3. `pipe_reset` は `CLEAR_FEATURE` が通れば true を返し、`hc_recover` は pipe を再開する。
  4. 同じ TT の全装置の bulk と control の split が、残りの buffer だけで動く。全部の buffer が busy になると動かない。
- 重さ：当たれば壊れる。
- 直す案：`tt_clear` は hub が消えたとき以外の失敗で false を返す。`pipe_reset` はそれを失敗として返し、`hc_recover` の退避と再試行に乗せる。`endpoint_recover` は false の意味が「古い TRB が残る」なので、そこでは一度の再試行と log にとどめる。

### 指摘 9：Y だけが別の report にある pointer の Y を読まない

- 場所：`kernel/src/dev/usbhid.c:262-277`、コメントは `usbhid.c:249-250`。
- 仕様：HID 1.11 5.6。一つの endpoint に複数の report の形があってよく、それぞれは少数の field だけを載せてよい。
- 差：`mouse_report` は X がこの report にあるときだけ Y を読む。コメントは「motion」を一つの単位に書いているが、5.6 と 8.4 に X と Y を同じ report に置く決まりは無い。
- 筋書き：X を report 1、Y を report 2 に置く装置では、`value_came(&h->x, 2, len)` が false なので Y の report は捨てられる。Y がいつも 0 になる。
- 重さ：当たれば壊れる。
- 直す案：X と Y を別々に判定する。相対なら来た軸の差分だけを出す。絶対なら `last_x` と `last_y` を来た軸だけ更新する。

### 指摘 10：`usb.h` の `*moved` の説明が `-1` の一部に合わない

- 場所：`kernel/src/dev/usb.h:102-103`、`kernel/src/dev/xhci.c:1273`、`xhci.c:1298`、`xhci.c:527`。
- 仕様：xHCI 4.6.9。Stop Endpoint は Stopped の Transfer Event を出す。
- 差：コメントは「USB_TIMEOUT のほかは動いた byte 数」と書く。装置が消えたときと controller が止まったときの `-1` も、動いた byte の有無によらず `*moved` は 0 である。
- 害：`usbmsc.c` の `bot` はこの値を `-1` のときに使わないので、今は害が無い。
- 重さ：きれいさ。
- 直す案：コメントを「転送の完了を受け取ったときだけ動いた byte 数で、時間切れと装置の消失では 0」に直す。

### 指摘 11：Compliance のコメントの根拠の節が足りない

- 場所：`kernel/src/dev/xhci.c:1822`。
- 仕様：xHCI 4.19.1.2.4.1 は Compliance への移りの条件を書き、change bit については書かない。bit を立てないことを決めているのは Table 5-27 の PLC の表と 4.19.2 である。5.3.9 の CTC も関わる。
- 差：コメントは 4.19.1.2.4.1 だけを挙げる。CTC が 1 の controller では、software が PLS に 10 を書くまで Compliance へ移らない。この driver は書かない。
- 害：動きの誤りは無い。
- 重さ：きれいさ。
- 直す案：コメントの根拠を「Table 5-27 の PLC の表にも 4.19.2 にも無い。CTC が 0 の controller で自動で入る（5.3.9、4.19.1.2.4.1）」にする。

### 指摘 12：差分の小さな形の乱れ

- 場所：`kernel/src/dev/xhci.c:98-100`、`xhci.c:871`。
- 差：
  - `PS_PLC`（bit 22）が `PS_WPR`（bit 31）の後ろにある。
  - `WARM_DOUBLINGS` はレジスターの値でないのに、レジスターの定義の中にある。
  - `endpoint_recover` の `halted` は差分の後は誰も読まない。`endpoint_quiet` の出口のために置いてあるだけである。
- 重さ：きれいさ。
- 直す案：
  - `PS_PLC` を `PS_PRC` の次へ移す。
  - `WARM_DOUBLINGS` を `port_stuck` の近くへ移す。
  - `endpoint_quiet` の `halted` を NULL でもよい引数にするか、`pipe_reset` だけが使う形に分ける。

### 指摘 13：Supported Protocol に無い port に触れている

- 場所：`kernel/src/dev/xhci.c:2112-2118`（`hc_start`）、`xhci.c:1778`（`root_port`）。
- 仕様：xHCI 7.2.2。Supported Protocol Capability が定義しない port 番号に触れると、動きは未定義である。7.2.2 の注では番号の隙間が許される。
- 差：`hc_start` は 1 から `max_ports` まで全部の `PORTSC` を読み書きし、全部を `port_pending` に立てる。差分の `hc_stuck_ports` は `port_major[p] < 3` で飛ばすので、正しい。
- 筋書き：番号に隙間のある controller で、定義の無い port の `PORTSC` に書く。
- 重さ：きれいさ。今の差分の外である。
- 直す案：両方の輪で `port_major[p]` が 0 の port を飛ばす。

## 照らして合っていた手順

### ビット位置と値

- `PS_PLC` の bit 22、`PS_WPR` の bit 31、`PS_WRC` の bit 19、`PS_PRC` の bit 21、`PS_CSC` の bit 17 は Table 5-27 に合う。
- `PS_CHANGES` の bit 17 から 23 は、xHCI 4.19.2 の七つの change bit に合う。
- `PLS_RXDETECT` の 5、`PLS_INACTIVE` の 6、`PLS_COMPLIANCE` の 10 は Table 5-27 の PLS の読みの表に合う。
- `PS_KEEP` は PP、PIC、WCE、WDE、WOE で、書き戻す bit として Table 5-27 に合う。LWS を書かないので PLS は変わらない。PED は 0 を書くので無効にならない。
- `WARM_DOUBLINGS` の 6 と `NS_PER_S << port_warms` は 2 秒から 64 秒で、コメントどおりである。`NS_PER_S` は `uint64_t` なので桁あふれしない。
- `port_pending[p >> 5]` と `1u << (p & 31)` は、`max_ports` の上限 255 に対して 8 語で足りる。
- `IMODI_GATHER` の 1000 は 5.5.2.2 の 250 ns 単位で 250 us である。コメントの「packet ごとに一度」は、`usb_interrupt_start` の TD が 1 packet 分である形に合う。

### Inactive と Compliance

- Figure 4-27 の PP、CCS、PED、PR は、Error と Compliance の両方で 1、0、0、0 である。コメントの「読みは未接続」に合う。
- 節番号を文字起こしの順で確かめた。4.19.1.2.5 は Reset、4.19.1.2.6 は Error、4.19.1.2.7 は Compliance である。
- 脚注 94 は、抜かれた装置の Recovery から Inactive を経る移りに PLC を立てないと書く。`port_stuck` のコメントに合う。
- Compliance は WPR か HCRST で Reset へ移る（4.19.1.2.7）。warm reset で合う。
- Error は Disconnect Detect か reset で出る（4.19.1.2.5、4.19.1.2.6）。warm reset で合う。
- 4.19.1.2.5 で失敗した reset は Disconnected へ移る。`root_port` の 1806 行の `!CCS` の帰りと、その後の PLC の event で辻褄が合う。
- 4.19.1.2.4.2 の注に、Config Error は reset を繰り返しても cable を抜くまで繰り返すとある。backoff はこの場面に合う。
- controller がバスから消えて全部が 1 を読むとき、PLS は 15 である。`hc_stuck_ports` は何も書かない。

### Clear_TT_Buffer

- bmRequestType 0x23、bRequest 8 は USB 2.0 11.24.2.3 と Table 11-16 に合う。
- wValue の bit 3:0 endpoint、bit 10:4 address、bit 12:11 type、bit 15 direction は Table 11-18 に合う。
- wIndex の 1 は、multiple TT の hub が interface protocol 1 で単一の TT として動く形（USB 2.0 11.23.1）で合う。`bind_classes` は alternate 0 だけを使い、`usb_hub_info` の `multi_tt` はいつも false である。Slot Context の MTT の 0 も、Slot Context の初期化の節（xHCI 4.5.2）の「Set Interface で Multi-TT を有効にしたときだけ 1」に合う。
- 周期の endpoint に送らない条件（type が 0 か 2 だけ）は 11.24.2.3 の「周期には未定義」に合う。
- 止まった endpoint の順（Reset Endpoint、Clear_TT_Buffer、ENDPOINT_HALT の解除、Set TR Dequeue Pointer）は xHCI 4.6.8 の注の手順と同じである。control endpoint が解除を飛ばす形も同じである。
- 時間切れの道は、Stop Endpoint の後に送る。4.6.9 の注で Stop Endpoint は split の完了を待つので、順が合う。
- 差分の「諦めた転送にも送る」は、11.24.2.3 の「buffer に無ければ no-op」で害が無い。
- Address Device の Transaction Error の後の送りは 4.6.5 の注に合う。

### 速度

- 差分の `port_speed_id` の 20 Gb/s と 10 Gb/s の分け方は xHCI Table 7-13 に合う。Gen2x1 は ID 5、Gen1x2 は ID 6 で両方 10 Gb/s、Gen2x2 は ID 7 で 20 Gb/s である。
- `USB_SPEED_FULL` の 1、`LOW` の 2、`HIGH` の 3、`SUPER` の 4 は、同じ表の既定の ID に合う。
- PSI の bit（PSIV 3:0、PSIE 5:4、LP 15:14、PSIM 31:16）は Table 7-10 に合う。
- rank の 0 から 3 は USB 3.2 7.5.4.5.1 の順に合う。
- PSI に lane の数が無いというコメントは Table 7-10 に合う。

### HID

- `value_came` の `bit + size <= len * 8` は、`report_whole` が ID を除いた後の byte 数で測っているので合う。
- `report_bytes` は ID を含み、`parse_report_descriptor` の `offset` は ID を含まない。この区別も合う。
- HID 8.4 は、最長でない report は short packet で終わると書く。コメントに合う。
- 値が丸ごと来ないときに前のボタンの状態を保つ形は、`mouse_report` の頭の説明に合う。
- 計算は `REPORT_BITS_MOST` の上限で桁あふれしない。
- 差分は `h->lock` の下の純粋な計算で、錠の順に変更が無い。

### その他のコメント

- `CONTROL_LONG_PAGES` のコメントの 6.4.1 は正しい。Transfer TRB の buffer は 64 KB の境界をまたがない。
- `dma_block` の 16 頁は 64 KB の境界をまたがない。
- `*moved` を `USB_TIMEOUT` で 0 にすることは、`usb_bulk` の 1273 行と 1302 行に合う。
- `hc_stuck_ports` の worker の中の順は、`hc_running` を見てから `hc_ports` の前に立てるので、同じ一周で処理される。

## 読み切れなかった範囲

- 実機と QEMU で動かしていない。読むだけの監査である。
- 元の PDF は開いていない。文字起こしの節番号が崩れている所（4.19.1.2.x と 7.2.2.1.x）は、本文の順と目次で確かめた。
- `usbnet.c` は `usb_control` の呼び手と錠を grep で見ただけで、全部は読んでいない。
- warm reset の完了に `port_reset` の 500 ms の期限が足りるかは、USB 3.2 の全部の時間を足し合わせていないので、分からない。期限を過ぎても後の PRC の event で `root_port` が戻るので、この道の害は無い。
- `CAS`（bit 24）をコードは読まない。xHCI 4.19.8 は D3 からの復帰で `CAS` の見回りを求めるが、この driver の復帰は HCRST と列挙のやり直しである。HCRST が `CAS` を消すかは 4.19.8 と Table 5-27 に書いていない。
- キーボードの短い report は、来なかった usage を離したと見る（`usbhid.c:154`、`values_in`）。マウスは前の状態を保つ。仕様はどちらも決めていない。
