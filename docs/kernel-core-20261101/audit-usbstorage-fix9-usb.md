# USB の driver の差分の監査（九回目の差分、97e41771）

## 依頼文

Nocturne OS（独自の OS、カーネルは C、対象は実機）の USB の driver の差分を、公式の文書と敵対的に照らしてください。返事と報告は日本語です。

作業場所：`D:\Programes\llm vibe slop os\.claude\worktrees\agent-a85eb97cac31b3d64`（読むだけ。コードは変えない）。

材料：
- 差分：`git diff 9bea7571 HEAD -- kernel/src/dev/xhci.c kernel/src/dev/usbhub.c kernel/src/dev/usb.h kernel/src/dev/usbhid.c`。周りのコードは今のファイルで読む。
- 文書の text（節番号を自分で確かめる）：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\spec\` の xhci.txt、usb20.txt、usb32.txt。原本は `D:\Programes\OS DEV INFO`。
- カーネルの規則：`.claude/rules/kernel.md`。

範囲：差分の行が生んだ欠陥か、差分の行そのものと文書の差だけを挙げてください。差分の外の問題は、port や worker が永久に止まるものだけを挙げてください。推測で疑わず、一件ごとにコードの場所と文書の節を確かめてから書いてください。`docs/` の下の他の監査の報告は読まないでください。Linux その他の OS のソースは参照しないでください。

報告：全文を `docs/kernel-core-20261101/audit-usbstorage-fix9-usb.md` に書いてください（先頭にこの依頼文）。各指摘の形：重さ（当たれば壊れる／きれいさ）、場所（ファイル:行）、文書の節と日本語の要旨（原文を長く写さない）、起きる道筋、直し方の案。指摘が無ければ無いと書いてください。コミットはしないでください。最後は SubagentHandback で、件数と重さごとの内訳と報告のパスを返してください。

## 結論

指摘は 3 件。全部「当たれば壊れる」で、全部 hub の port が電源の切れたまま、または未検出のまま止まる。

| 番号 | 重さ | 場所 | 要旨 | 差分との関係 |
|---|---|---|---|---|
| 1 | 当たれば壊れる | `usbhub.c:239` | Local Power Source の 1 を「全 port が切れた」と読む。bus-powered で動く hub の port が戻らない | 差分が生んだ。hub 全体の過電流の回復は差分の前は動いた |
| 2 | 当たれば壊れる | `usbhub.c:235-240` | 読んだ後に変更の bit を消すので、その間の終わりを失い、`h->off` が真のまま残る | 差分の `h->off` の行。失う窓は差分の前からある |
| 3 | 当たれば壊れる | `usbhub.c:354-361` | port で同じ読みと消しの順。その間に繋がった装置と終わった過電流を失う | 差分の外。port が永久に止まる |

試したのは文書とコードの照合だけで、ビルド・QEMU・実機では動かしていない。

## 指摘 1：Local Power Source の 1 を「全 port が切れた」と読む

- **重さ**：当たれば壊れる。
- **場所**：`kernel/src/dev/usbhub.c:239`（`h->off = status & 3`）。効く所は `usbhub.c:241`（電源を戻す枝を選ぶ）、`usbhub.c:380`（port の電源を戻さない）、`usbhub.c:408`（電源の再試行の時を見ない）。`usbhub.c:61` の注釈と、`usbhub.c:229-231` の注釈も同じ読みを書いている。
- **文書**：
  - USB 3.2 Table 10-10（10.16.2.4）：Local Power Source は、hub の電力が外部から来るか USB から来るかを示す。USB 2.0 Table 11-19（11.24.2.6）も同じ。1 は「lost (inactive)」。
  - USB 2.0 Table 11-12（11.13）：自己給電と bus 給電の両方ができる hub が、今は bus 給電でだけ使える場合を定めている。
  - USB 2.0 11.5.1.2：bus 給電で configure された hub は、外部電力へ移っても port の状態を変えなくてよい。外部電力を失って port を Powered-off に置くのは、その後のこと。
  - USB 3.2 10.3.1.1：全 port を Powered-off に置くのは、local power があった状態で configure され、その後に失ったとき。
  - USB 3.2 Table 10-1：`ds.vbus.source = on` は、Local Power Source が On か、hub が bus-powered のとき。
  - USB 3.2 10.16.2.6.1 の PORT_POWER、USB 2.0 11.24.2.7.1.6、USB 2.0 11.11：SetPortFeature(PORT_POWER) が効かないのは、C_HUB_LOCAL_POWER と Local Power Source の両方が 1 のときだけ。Local Power Source が 1 でも、変更の bit が 0 なら port の電源は入る。
  - USB 3.2 10.16.2.6.1 の PORT_OVER_CURRENT：bus-powered の hub の過電流の保護は任意で、持ってよい。
- **起きる道筋**：
  1. 外部電力の無い bus 給電の hub は、ずっと Local Power Source が 1 で、port は動いている。
  2. hub 全体で過電流を報告する hub で、過電流が終わる。読む値は `status = 1`、`change = 2`。
  3. `usbhub.c:239` で `h->off` が真になり、`usbhub.c:241` が「切れた」の枝を選び、`power_ports` を呼ばない。差分の前のコードは `status & 2` だけを見て `power_ports` を呼んでいた。
  4. `usbhub.c:380` が各 port の再投入を止める。hub を抜き差しするまで全 port が切れたまま残る。
  5. 報告の endpoint を諦めた hub（`h->asking`）では、毎秒の `hub_status` で `h->off` が真になる。その hub で port ごとの過電流が終わると、その port が同じく戻らない。
- **直し方の案**：local power を失ったかどうかを、「一度は有った」の上で判定する。`usbhub_attach` で `power_ports` の前に Get Hub Status を読み、Local Power Source が 0 なら `h->local = true` を立てる。`hub_status` で 0 を読むたびにも立てる。`h->off = (status & 2) || (h->local && (status & 1))` とする。`h->local` が偽の hub では、Local Power Source の 1 を切断と読まない。`usbhub.c:61`・`usbhub.c:229-231` の注釈もこの形に書き直し、local power の根拠に USB 2.0 11.5.1.2、USB 3.2 10.3.1.1 を挙げる。

## 指摘 2：読んだ後に変更の bit を消すので、`h->off` が真のまま残る

- **重さ**：当たれば壊れる。窓は制御転送の間で短く、当たれば永久に止まる。
- **場所**：`kernel/src/dev/usbhub.c:235-240`。
- **文書**：
  - USB 2.0 Figure 11-21（11.12.3）、USB 3.2 Figure 10-23（10.13.3）：ソフトは状態を読み、変更があれば蓄えて消し、また読む。変更が無くなってから処理する。USB 2.0 の原本の PDF で矢印を確かめた。
  - USB 2.0 11.12.4：Status Change endpoint が bitmap を返すのは、変更の bit が 0 でないとき。USB 3.2 10.13.4 も同じで、ERDY を返す条件が変更の bit。
  - USB 2.0 11.24.2.1、USB 3.2 10.16.2.1：ClearHubFeature は変更を認めて消す。
- **起きる道筋**：
  1. hub 全体の過電流で、Get Hub Status が `status = 2`、`change = 2` を返す。
  2. `usbhub.c:238` の ClearHubFeature が届く前に過電流が終わる。C_HUB_OVER_CURRENT は既に 1 なので、hub から見て新しい変更は立たない。
  3. ClearHubFeature がその 1 を消す。変更の bit が 0 になるので、次の報告は来ない（11.12.4）。
  4. `usbhub.c:239` は古い `status` で `h->off` を真にする。`usbhub_work` は報告が来なければ `hub_status` を呼ばない。`usbhub.c:380` と `usbhub.c:408` が全 port を止め続ける。
  5. C_HUB_LOCAL_POWER でも同じ。`h->asking` の hub は毎秒読むので元に戻り、当たるのは報告の endpoint が動く hub。
- **直し方の案**：`hub_status` を図の形の繰り返しにする。Get Hub Status を読み、`change & 3` が 0 になるまで消して読み直す。`h->off` は最後の読みの値で決める。繰り返しの中で一度でも変更を見たかを持ち、終わった時点で `h->off` が偽なら `power_ports`、真なら全 port の印を付ける。回数には上限を置き、上限に達したら `h->asking` の時刻のように後で読み直す。

## 指摘 3：port で同じ順。繋がった装置と終わった過電流を失う（差分の外）

- **重さ**：当たれば壊れる。差分の外で、port が永久に止まるので挙げる。
- **場所**：`kernel/src/dev/usbhub.c:354-361`（`port_status` の値を、`clear_changes` の後も `connected` と過電流の判定に使う）。
- **文書**：
  - USB 2.0 Figure 11-21（11.12.3）、USB 3.2 Figure 10-23（10.13.3）：読み、消し、読み直して変更が無くなるまで繰り返す。
  - USB 2.0 11.12.4、USB 3.2 10.13.4：変更の bit が 0 なら報告は来ない。
  - USB 2.0 11.12.5、USB 3.2 10.13.5：過電流の回復は、過電流の bit が 0 になるのを待ってから電源を入れる。
- **起きる道筋（繋がった装置）**：
  1. 差し込みの接点の揺れなどで、Get Port Status が `status` の接続 0、`change` の C_PORT_CONNECTION 1 を返す。
  2. `usbhub.c:360` の ClearPortFeature が届く前に繋がる。C_PORT_CONNECTION は既に 1 なので、新しい変更は立たない。
  3. ClearPortFeature がその 1 を消す。`usbhub.c:361` は古い値で未接続と読み、`usbhub.c:390-391` で何も試さずに戻る。
  4. 報告は来ず、`h->tries[port].at` は 0 なので `port_due` も真にならない。次の変更が来るまで、繋がった装置は見つからない。
- **起きる道筋（過電流）**：
  1. Get Port Status が PORT_OVER_CURRENT 1、C_PORT_OVER_CURRENT 1 を返し、その後に過電流が終わる。
  2. ClearPortFeature が変更を消す。`usbhub.c:370-375` は古い値で過電流中と読んで戻る。
  3. 初めての過電流では `power->at` が 0 なので、`usbhub.c:373` の毎秒の読み直しは動かない。報告も来ず、port の電源は切れたまま残る。
- **当たる範囲**：報告の endpoint が動く hub。`h->asking` の hub は毎秒読む。root port は `hc_ports_again`（`xhci.c:1865`）が毎秒見るので当たらない。
- **直し方の案**：`hub_port` の頭を指摘 2 と同じ繰り返しにする。`change` が 0 になるまで読み、消す。`CH_CONNECTION` と `CH_OVER_CURRENT` は蓄えて、最後の `status` と合わせて判定する。`hub_status` と共通の形にできる。

## 照らして指摘にしなかったもの

差分の行のうち、文書とコードの両方を読んで欠陥が無かったもの。

- `usbhub_set_depth` と `xhci.c:1674-1675`：`#define SET_HUB_DEPTH 12` は USB 3.2 Table 10-8 と合う（text では表の列がずれるので、並びから読んだ）。要求の形は 10.16.2.9 と合う。呼ぶ位置は `configure` の後で、10.16.2.9 の「configure の後」と 10.1.3.1 に合う。depth は root port の直下が 0 で、route string の位置と合う。depth が 5 の hub は 10.16.2.9 の Request Error になり、`goto fail` で放す。そのような hub の子は `usb_attach_child` も作らない。
- `!usbhub_prepare(d)` で `goto fail` へ行く変更：`d->hub` は NULL のままか、`usbhub_detach` が放す。port は `usb_port_failed` の間隔で試し直す。
- `link_stuck` の注釈：USB 3.2 10.3.1.1 は eSS.Disabled、10.3.1.10 は Rx.Detect、10.3.1.11 は Rx.Detect.Reset。PORT_POWER が 0 の三つの状態はどれも Inactive と Compliance ではない（10.16.2.6.1）。
- `usb_port_failed` の `failed`：`count` は待ち、`failed` は打ち切りで分かれている。`stuck` の試しは数えず、`CLOCK_NEVER` の port は `usb_port_try` が偽を返す。hub が答えないときも `at` を先へ送るので、時が来たままの port は残らない。
- `power` の扱い（`usbhub.c:357`、`usbhub.c:373`、`usbhub.c:379-380`）：過電流の間は 1 秒ごとに読み、終わると 11.12.5 の手順 4・5 に進む。`at` が時を過ぎたまま残る道は無い。
- `usbhub_work` の `ask_gap`：倍にして 64 で止まる。`NS_PER_S` は `uint64_t` で、掛け算はあふれない。答えない hub の port は `port_due` の分だけ読む。
- `usbhub_reports_lost` の `address`：`hc_recover` の `ep->address` と比べ、hub の報告の endpoint だけが `asking` へ移る。`ep->dead` を立てる所は `xhci.c:1917` だけ。
- `usbhid.c:279`：`x_known`・`y_known` は `!relative` の枝でだけ立つ（`usbhid.c:270-277`）。`relative` は解析の後に変わらない。差分の前と同じ条件で、欠陥は無い。
