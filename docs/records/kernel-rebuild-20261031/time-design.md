# 時刻の基準：UTC・時間帯・網の時刻（設計）

2026-10-31、main `aac2bafd` のコードで確かめた。段 1-B2（P0 の速さと HPET の直し）の続き。実装は 2b の三人を取り込んでから（syscall.c・net.c・settings が重なる）。

## 1. 今の形

- 時計（`kernel/src/sys/clock.h`）：TSC だけ。速さは起動から P0（実機）か hypervisor の登録。基準は二つの口で入る：速さの基準 `clock_rate_sample`（今は HPET、`dev/refclock.c`）、壁の基準 `clock_wall_proof`（今は RTC、clock.c が自分で読む）。見出しは「網の時刻の service はそれぞれの口の一つになる」と書いている。
- 壁の証しは下限だけ：`clock_wall_proof(unix_ns, by)` は「`by` の時に時刻は少なくとも `unix_ns`」で、壁の時計を前へだけ動かす。進みすぎた壁の時計を遅らせるのは `clock_adjust_wall` だが、カーネルの中に呼び手は無い。
- 速さの窓は基準一つ：clock.c の `disc.ref` が違う基準の標本を受けると窓を始めからにする（`discipline`）。
- 時間帯は無い：`user/libc/time.c:51` は「RTC は地方時を持つので localtime == gmtime」、QuickJS の `getTimezoneOffset` は 0（`CONFIG_NOCTURNE`）。タスクバーの時計（`kernel/src/gui/wm.c` の `time_to_parts(clock_wall_seconds())`）は壁の時計をそのまま出す。
- Hyper-V の時刻の同期（`kernel/src/vm/hv/hv_util.c`）は host の UTC に、壁の時計との差を 15 分に丸めた `tz_offset` を足して `clock_set_wall` する（壁の時計を地方時に保つため）。
- 一方で UTC として読む所：文書の変更の時刻（`kernel/src/fs/document.h`「1970 UTC からの ns」）、HTTP の日付・Cookie の期限・TLS の証明書の期間（libc）、JS の `Date.now()`。
- DHCP（`kernel/src/net/net.c` の `dhcp_send`）が頼む option は 1・3・6・51 だけ。NTP の server（option 42）は頼まない。
- 時間帯の表はブラウザーの中にだけある：`third_party/formatjs`（IANA tzdb を詰めた全部の zone、`lazy-tz.js`）。
- TLS 1.3 は libc の `user/libc/tls.c` にある（RFC 8446）。

## 2. 欠陥

1. **壁の時計が UTC ではない機械がある。** Windows と同じ機械（ユーザーの 7600X もそう見込み）の RTC は地方時。Nocturne はそれを Unix 時刻として読むので、UTC として読む所が全部、時間帯の分ずれる：文書の時刻、Cookie の期限、HTTP の cache、JWT の期限、TOTP の番号（6 桁の認証は時間帯の分ずれると通らない）。今は「地方時を UTC と呼ぶ」ことで画面の時計だけを合わせている。
2. **速さが絶対ではない。** HPET に対して ppb で測れても、HPET 自身の誤差は証されない（HPET の仕様は 500 ppm までを許す、Intel のチップセットの資料の引用より）。記録の行の「within N ppb of HPET」は HPET に対しての値。
3. **壁の時計を正しく戻す道が無い。** 下限の証ししか無いので、RTC が進んでいると直らない。
4. **基準を二つ並べられない。** HPET と網の時刻を同時に入れると、交互に窓を壊す。

## 3. 設計

### 3.1 壁の時計は UTC

壁の時計は UTC の Unix 時刻（閏秒を数えない）。カーネルの中も ABI も UTC だけを渡し、地方時は表示の時だけ時間帯で作る。libc の `gmtime` は UTC、`localtime` は時間帯で。QuickJS の `getTimezoneOffset` は時間帯から。

### 3.2 時間帯の持ち主はカーネル

タスクバーの時計をカーネル（wm.c）が描き、RTC の地方時を読むのもカーネルなので、時間帯はカーネルの持つシステムの設定にする。

- 設定：IANA の zone の名（例 `Asia/Tokyo`）。keep の部品「Time」として `AppData\Nocturne\Time.conf` に書き、init より前に読み戻す（`sys/keep.h`）。変えた瞬間に効く：タスクバーを描き直し、見張りの事象でプログラムへ知らせる。
- 表：tzdb をホストの道具で一つの二進の表に組み、C: の `Nocturne\System\` に置く。各 zone は移り目の列（UTC の時刻、ずれの秒、夏時間の印、略号）と、表の最後の年の後の規則。読み手は `common/` の一つの部品（カーネルと libc が組む、`common/store` と同じ形）。
- ABI：「時刻 t の地方時のずれ・略号・夏時間・次の移り目」と「今の zone の名」を引く口、zone を変える口（システムの設定の権限）。libc の `localtime`・`mktime`・`strftime` はこの口で。
- ブラウザーの Intl：既定の時間帯をシステムの zone の名にする。表は二つ持たない：システムの表を tzdb の唯一の写しにし、Intl の zone のデータもそこから読む（formatjs の詰めたデータを消す）。
- 表の元：今 tree にある tzdb は formatjs の詰めたデータだけ。手元の上流 `D:\Programes\cloned repos\go\lib\time\zoneinfo.zip` は tzdb 2026c を zic で組んだ TZif（RFC 8536、`update.bash` の CODE・DATA=2026c）で、ダウンロードが要らない。推す：これを `third_party/tzdb/` に写し（版と出どころを README に）、システムの表と Intl の両方をそこから組む。

### 3.3 RTC の意味は設定

RTC が UTC か地方時かは機械が教えないので、時間帯と並ぶ設定にする（既定は地方時：PC の多くは Windows と同じ RTC の使い方で出荷される）。カーネルは RTC を読むとき、地方時なら設定の時間帯の表で UTC に直す（夏時間の移り目の前後の重なる一時間は、RTC だけでは決まらない区間として下限と上限の幅に入れる）。網の時刻が UTC を証したら、カーネルは RTC をその意味のまま秒の境で書き直す（Windows を同じ機械で起こしても正しい）。RTC を読み書きする持ち主は `dev/refclock.c`。

### 3.4 壁の基準の口：下限と上限

`clock_wall_proof` を区間の口にする：「`by` の時に時刻は `[at_least, at_most]` の中」。

- 壁の時計が下限より遅れていれば、今と同じく前へ動かす。
- 上限より進んでいれば遅らせる。最初に網の基準が証すまでは跳ばし（step）、その後は `clock_adjust_wall` で滑らかに（slew、500 ppm まで：1 秒の差を約 33 分で入れ、TSC の速さの誤差よりずっと大きいので必ず追いつく）。滑らかにすると基準の次の標本までに入りきらない差なら跳ばす。
- RTC は秒の境の読みなので、幅は今の探しが証す幅。網の時刻の幅は往復の時間から（3.6）。
- 単調時計と起きている時計（boot clock）は壁の基準で動かない（今と同じ）。

### 3.5 速さの基準を並べる

- clock.c の窓を基準ごとに持つ（今の `disc` を基準の記録へ）。各基準は自分の許容の誤差を言う：HPET は 500 ppm（仕様）、網の時刻は 0（server の誤差は標本の幅に入れる、3.6）。
- 時計が使う速さは、測った幅と基準の許容を足して一番小さい基準のもの。HPET は起動の直後から数分を受け持ち、網の時刻の窓が伸びて幅が HPET の 500 ppm を下回ったら、網の時刻が引き継ぐ（往復 50 ms なら窓 100 秒ほどで 500 ppm、1 日で 1 ppm 未満）。
- 二つの基準の速さが幅を超えて食い違えば記録に出す（嘘をつく server か、壊れた装置）。
- 利用者のプログラムが渡す標本の時刻は単調時計の ns。clock.c は一番古い窓の始めから速さの移り目（TSC・ns・速さの組、窓が倍に伸びるので数は少ない）を持ち、ns を TSC に正確に戻す。

### 3.6 網の時刻：NTS で NTPv4

持ち主は時刻の service（システムのプログラム、init が起こす）。カーネルに入れない理由：網から来る形の解析と暗号（TLS 1.3、AES-SIV）は libc にあり、核に入れると攻撃の面が増える。カーネルが渡すのは、正確な時刻印と、時計へ証す権限だけ。

- **時刻印。** UDP の受け取りと送り出しに、装置の割り込みの時の単調時計の ns を付ける（波 3 の「socket を handle に」「事象の時刻」と一緒に）。予定の遅れが時刻の誤差に入らない。
- **証す口。** 権限つきの handle（波 3 の権限）で、速さの標本（3.5）と壁の区間（3.4）を時計へ渡す。handle を持てるのは init が渡した時刻の service だけ（`SYS_PCI_*` の claim と同じ形）。
- **NTS（RFC 8915）を既定にする。** 鍵の確立：TCP 4460、TLS 1.3 だけ、ALPN `ntske/1`、鍵は TLS の exporter（label `EXPORTER-network-time-security`）、AEAD は AES-SIV-CMAC-256（番号 15、RFC 5297）、cookie を 8 つほど持ち、使うたびに補う。時刻の交換：NTPv4 の客の形（UDP 123、RFC 5905）に、Unique Identifier・Cookie・Cookie の空き・Authenticator の拡張の欄。libc に要るもの：`tls.c` に TLS の exporter、暗号に AES-SIV-CMAC-256。
- **NTS の無い server。** DHCP の option 42（RFC 2132）の server と設定で足した server は認証されない。使うかは設定（既定：NTS の server が一つも答えない間だけ、記録と設定の画面に「認証なし」と出して使う）。
- **一つの交換。** 送る時刻の欄は乱数（手元の時計を相手に見せない、返ってきた欄と照らす）、受け取った origin の欄が送った値と違えば捨てる。KoD（stratum 0 の RATE・DENY・RSTR）に従う。標本は t1（送り出しの印）・t4（受け取りの印）・server の T2・T3 から：速さの標本は数え = (T2+T3)/2 の ns、ゆれ = (T3−T2)/2 + root delay/2 + root dispersion、区間 = [t1, t4]。壁の区間は「t4 の時に時刻は [T3 − ゆれ, T3 + (t4 − t1) − (T3 − T2) + ゆれ]」。
- **server を選ぶ。** 3 つ以上の server の区間の重なり（RFC 5905 §11.2.1 の選び方）。多数に重ならない server は捨てる。
- **間隔。** 64 秒から始め、合う間は倍に、上は 1024 秒。網が変われば（DHCP の取り直し）64 秒へ戻す。
- **証明書と時刻の鶏と卵。** 網の時刻が一度も証していない間、NTS-KE の証明書の期間は「組んだ時刻（3.7）より後のどこかで有効だった」で照らす。証した後は普通に照らす。
- **設定の画面。** `user/apps/settings.ts` に「日付と時刻」：時間帯（検索つきの一覧）、RTC の意味、網の時刻の入り切り、server の一覧（NTS か否か）、今の状態（最後に合わせた時、ずれ、幅、server）。網の時刻の設定は service の持ち物（自分のファイル）、時間帯と RTC の意味はカーネルの keep の部品。

### 3.7 起動の下限：組んだ時刻

カーネルを組んだ時刻は本物の下限（この核はその後に動いている）。起動の時に `clock_wall_proof(組んだ時刻, 0)` を一度渡す。RTC の電池が切れて 2000 年を読んでも、壁の時計は組んだ時刻より前にならない。

### 3.8 閏秒

壁の時計は閏秒を数えない。予告された閏秒（NTP の LI の欄）は、前の 24 時間で滑らかにする（`clock_adjust_wall`、約 11.6 ppm）。速さの窓は server の数えから予告の閏秒を除く。閏秒を滑らかにする server（Google など）は他の server と食い違うので、既定の一覧に入れない。CGPM 2022 は遅くとも 2035 年に UT1 と UTC の差の上限を広げると決めた（閏秒は無くなる見込み）ので、この道は小さく保つ。

### 3.9 VM

Hyper-V の時刻の同期は、壁の基準の区間を渡す一つの基準になる（`vm/` の中、`clock` の口だけを呼ぶ）。`tz_offset` は消す：壁の時計が UTC になれば host の UTC をそのまま証せる。

## 4. Linux・NT と軸ごとに（公開された設計だけ）

| 軸 | Linux | NT | Nocturne |
|---|---|---|---|
| 核の中の壁の時計 | UTC | UTC | UTC（今は違う：直す） |
| RTC | 既定 UTC、地方時も選べる | 既定 地方時、UTC も選べる | 設定、既定 地方時、網の時刻で書き直す |
| 時間帯 | 利用者の側の tzdata | システムの設定 | システムの設定（カーネル）、表は tzdb の一つの写し |
| 網の時刻 | 利用者の側の daemon（chrony は NTS も） | 利用者の側の service、既定は認証なし | 利用者の側の service、既定 NTS |
| 時刻印 | socket の時刻印 | — | 割り込みの時の単調時計を受け取りの記録に |
| 速さ | daemon が核の PLL・FLL を動かす | 核の時刻の調整を service が動かす | 核が基準ごとの窓で測り、証された幅の一番小さい基準に従う |

劣る軸は残らない見込み。NT より良い点は認証つきの既定、Linux より良い点は時間帯が変えた瞬間に効くシステムの設定であること。

## 5. 段

| 段 | 中身 | 待つもの |
|---|---|---|
| T1 | UTC の壁の時計、時間帯（表・keep の部品・ABI・libc・QuickJS・タスクバー・設定の頁）、RTC の意味、区間の壁の口、組んだ時刻の下限、hv_util の `tz_offset` を消す | 2b の取り込み（syscall.c の重なり） |
| T2 | clock.c の基準を並べる（基準ごとの窓と許容、速さの移り目の履歴）、利用者の基準の口（権限つき handle）、DHCP の option 42 を網の情報へ、UDP の時刻印 | 3-T（事象の時刻）と波 4 の網の建て直し（socket を handle に、権限。3-design.md §4） |
| T3 | 時刻の service（NTS-KE・NTPv4・選び・閏秒・RTC の書き戻しの依頼）、libc の TLS exporter と AES-SIV-CMAC-256、設定の頁の網の時刻 | T2 |

確かめ：QEMU は `-rtc base=localtime` と `base=utc` の両方で起こし、壁の時計が UTC、タスクバーが地方時になることを見る。網の時刻は QEMU の user の網（UDP 123 と TCP 4460 は host を通る）で本物の server に当てる。実機（RTC が地方時の 7600X）はユーザーに短い一覧で頼む。

## 6. 決めること・未確認

- tzdb の元：手元の Go の zoneinfo.zip（2026c）を推す（3.2）。TZif の最後の規則の文字列は RFC 8536 §3.3 の形で、組むときにホストで読んで表の自分の形に直す。
- 既定の NTS の server の一覧（例 time.cloudflare.com）。地域で選ぶか。
- RTC の意味の既定（地方時）が合わない機械では、最初に網の時刻が証したとき RTC と UTC の差（15 分に丸めた値）を設定の画面に「RTC は地方時に見える」と出すか（推す：出すだけで、勝手に変えない）。
- HPET の 500 ppm は Intel のチップセットの資料の引用で確かめた値。IA-PC HPET 仕様 1.0a の原文（§2.4 の精度の規則）は手元に無く、未確認。
