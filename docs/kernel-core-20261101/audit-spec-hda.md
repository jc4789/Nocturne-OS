# 依頼文

Nocturne OS（独自の OS。カーネルは C）の差分を、公式の文書に対して敵対的に監査してください。コードは変えないでください。ビルドと QEMU は動かさないでください。返事と文書は日本語で書いてください。

ユーザーの言葉："any and all work involeing offical specs needs to be adversesrally audtied to insure that the changes are following the relevent offical docs and specs to a Tee."
対象の前提：今の PC を基準にした一般の OS で、特定の機械に決め打ちしない。実機が的で、VM で通ることは証拠にならない。規模の例は "600hz moniters, 3 moniters, game, youtube and discord open with no droped frames or stuttering cuased by the kernel its self"。音のずれについてのユーザーの言葉は "there should never be a build up of drift at all ever." です。

対象の差分：`git diff e67f52ed 4b5fbd9d -- kernel/ common/ tests/hda_codec_hosttest.c`（リポジトリは D:\Programes\llm vibe slop os）。主なファイルは `kernel/src/dev/hda.c`、`hda_codec.c`、`hda_codec.h`、`audio.c`、`audio.h`、`vm/ac97.c`、`iommu.c`。今のコードも読んでください。コードの注釈は書き手の主張なので、真実と見なさず文書で確かめてください。

文書：`D:\Programes\OS DEV INFO` の下を探してください（High Definition Audio Specification、PCIe、PCI、MSI・MSI-X など）。無い文書（CEA-861、HDMI、DisplayPort の InfoFrame や secondary data packet など）は、無いと書き、その部分の値は照らせなかったと書いてください。推測で合否を書かないでください。

見ること：レジスターのオフセットとビット、reset の手順と待ち時間、CORB・RIRB の初期化と読み方、BDL、stream の format と run・stop の手順、LPIB の読み方、verb の形、widget の木の歩き方、pin の configuration default、jack と unsolicited response、amp、遅れの申告、電源の状態、ELD、割り込み、複数の制御器と codec、複数の出口、資源の固定の上限、位置の読みが溜まりのない形か、競りと錠、suspend と resume。

報告：全文を `docs/kernel-core-20261101/audit-spec-hda.md` に書いてください（この依頼文も先頭に）。指摘ごとに、場所（ファイル:行）、文書の節、日本語の要旨、どう壊れるか、重さ（データや安全を失う／当たれば壊れる／きれいさ）を書いてください。原文の文を長く写さないでください。合っていた手順も領域ごとに一行書いてください。読み切れなかった範囲を書いてください。コミットはしないでください。

途中の伝言（調整役から）：ユーザーの言葉 "You can download the docs you need yourself. You dont have to ask me to do it every time." 照らすのに要る文書は、作り手の公式の場所から `D:\Programes\OS DEV INFO` へ落としてよい。公式の場所に無いものは取らず、照らせなかったと書く。

---

# 監査の結果：実機の音の driver（e67f52ed..4b5fbd9d）

## 結論

- 指摘は 41 件。データや安全を失うものが 1 件、当たれば壊れるものが 26 件、きれいさが 14 件。
- 一番重い誤りは ELD の Conn_Type で、別の byte を読んでいる（B1）。host 試験も同じ誤りの形で作ってあるので、試験は通る。
- 二番目は `hda_thread` が verb の答えを奪う競り（B2）。当たると、その codec は次の link reset まで無音になる。
- レジスターのオフセットとビット、reset の順、CORB・RIRB の置き方、BDL、format、verb の番号と形、parameter の番号は、全部文書と合っていた。
- 位置の報告の道に溜まりは無い。ただし組の時刻は ABI を越えず、`hda.c:24-25` の注釈の主張はこの版では偽である（B15）。
- コードは変えていない。ビルド、QEMU、host 試験は動かしていない。指摘は全部、文書とコードの読みから出した。

## 使った文書

| 文書 | 場所 |
|---|---|
| High Definition Audio Specification Rev 1.0a | `D:\Programes\OS DEV INFO\high-definition-audio-specification.pdf` |
| PCI Express Base Specification 6.0 | `D:\Programes\OS DEV INFO\PCI Express Base Specification Revision 6.0.pdf` |
| PCI Code and ID Assignment 1.19 | `D:\Programes\OS DEV INFO\FINAL RC-PCI_Code-ID_r_1_19__v2025-05-22.pdf` |
| Intel ICH7 HD Audio/AC'97 Programmer's Reference Manual（今回 intel.com から落とした。509,265 byte） | `D:\Programes\OS DEV INFO\intel-ich7-hda-ac97-prm\io-controller-hub-7-hd-audio-ac97-manual.pdf` |
| Intel ICH7 Family Datasheet（今回 intel.com から落とした。4,820,330 byte） | `D:\Programes\OS DEV INFO\intel-ich7-datasheet\i-o-controller-hub-7-datasheet.pdf` |

## 指摘：データや安全を失う

### D1. 止まったと確かめずに DMA の頁を返す

- 場所：`kernel/src/dev/hda.c:923-930`（`controller_add` の失敗の道）。
- 節：HDA 3.3.7、3.3.35（RUN が 0 を読むまで確かめる）。
- 要旨：`link_down` と `pci_disable_busmaster` の戻り値を捨て、`pmm_free_contig` で頁を返す。
- 壊れ方：RIRB の RUN が 1 ms で消えない制御器は、返した頁へ書き続ける。この function は IOMMU を素通りする（`hda.c:27`）。別の持ち主の RAM が壊れる。当たる条件は、stream が無い、または reset から出ないと判定された制御器で、DMA が止まらないとき。
- 重さ：データや安全を失う。

## 指摘：当たれば壊れる

### B1. ELD の Conn_Type を別の byte で読む

- 場所：`kernel/src/dev/hda_codec.c:383-385`。`tests/hda_codec_hosttest.c:78` が同じ誤りの形を返す。
- 節：HDA 7.3.3.34、Figure 79、Table 121。
- 要旨：Conn_Type は byte 5 の bit 3:2 である。コードは byte 7 の bit 3:2 を読む。byte 7 は speaker allocation で、この 2 bit は RLR と FC である。
- 壊れ方：stereo の DisplayPort の表示器（byte 7 が 01h）は HDMI と判定され、HDMI の頭の DIP を受ける（`hda_codec.c:456-457`）。FC あり RLR なしの HDMI の sink は DisplayPort と判定される。名前の "HDMI: " と "DisplayPort: " も入れ替わる。試験は誤りを見つけられない。読みが失敗したときは `o->displayport` に前の値が残る。
- 重さ：当たれば壊れる。

### B2. `hda_thread` が verb の答えを奪う

- 場所：`kernel/src/dev/hda.c:800-802`、`:363-382`。冒頭の注釈 `:6-8` の主張に反する。
- 節：HDA 4.4.2（応答の振り分けは software の責任）。
- 要旨：`hda_thread` は `verb_lock` を持たず、`engine` だけで `rirb_take(h, NULL)` を呼ぶ。`answer` が NULL だと、solicited の答えは捨てられる。
- 壊れ方：audio thread の `hda_output_enable`（`hda_open`、`fill_begin`）は `verb_lock` だけで答えを待つ。RINTCNT が 1 なので、応答ごとに割り込みが来て `hda_thread` が起きる。答えを奪うと `link_verb` は 10 ms で `silent` を立て、その codec は次の link reset まで無音になる。verb は一度の出口の開きで数十から数百出るので、機会も同じ数ある。
- 重さ：当たれば壊れる。

### B3. 答えの codec を照らさず、一度の欠落で codec を捨てる

- 場所：`kernel/src/dev/hda.c:369-380`、`:407-413`。
- 節：HDA 4.4.2、Table 54（bit 3:0 が codec）、3.3.30（RIRBOIS）。
- 要旨：solicited の答えの codec 番号を見ない。overrun を消すだけで扱わない。10 ms の欠落で `silent` を立て、link reset まで戻さない。欠落した verb は 0 を返す。
- 壊れ方：遅れた答えは、別の codec の verb の答えになる。`hda_output_sense` は 0 を「抜けた」と読み（`hda_codec.c:402-404`）、音は別の出口へ移る。
- 重さ：当たれば壊れる。

### B4. WAKEEN を書かず、STATESTS を割り込みで消さない

- 場所：`kernel/src/dev/hda.c:435-438`、`:804`。WAKEEN（0Ch）の定義と書き込みが無い。
- 節：HDA 3.3.8（値を仮定せず設定する）、3.3.9、3.3.15。
- 要旨：WAKEEN は power-on reset でしか消えない。firmware が立てた bit は CRST をまたいで残る。`hda_irq` は RIRBSTS しか消さない。
- 壊れ方：CIE が立った後に codec の State Change が来ると CIS が消えない。INTx の機械では割り込みが止まらず、その CPU は戻らない。
- 重さ：当たれば壊れる。

### B5. Descriptor Error で止まった stream を走っていると扱う

- 場所：`kernel/src/dev/hda.c:429-434`、`:560-563`。
- 節：HDA 3.3.36（DESE は致命で、RUN が落ちる）。
- 要旨：DESE を数えるだけで、`running` のままにする。
- 壊れ方：位置は動かず、割り込みも来ず、音は戻らない。fill が別の起こしで走ると、`position_update` は時計で周回を足し、played が鳴っていない音の分だけ進む。
- 重さ：当たれば壊れる。

### B6. 表示の mode change を抜けたと扱う

- 場所：`kernel/src/dev/hda_codec.c:403-404`、`kernel/src/dev/hda.c:776-783`。
- 節：HDA 7.3.3.15、Figure 72。
- 要旨：PD=1・ELDV=0 は、装置を見せたまま流れの中身を捨てる状態である。見せるのをやめるのは time out の後である。コードは PD=0 と同じに扱う。
- 壊れ方：解像度の切り替えのたびに、`audio_sink_state` から `outputs_update` へ進み、音は別の出口へ移って戻る。そのたびに B21 の段差が出る。
- 重さ：当たれば壊れる。

### B7. 双方向の stream で DIR を最後に書く

- 場所：`kernel/src/dev/hda.c:532-537`。
- 節：HDA 3.3.35（DIR は SRST の解除の後、他の stream のレジスターより前）。
- 要旨：CBL、LVI、FMT、BDL の後に `SD_CTL2` で DIR を書く。
- 壊れ方：OSS が 0 の制御器で動作が文書の範囲から外れる。
- 重さ：当たれば壊れる。

### B8. amp の index が 4 bit を超えて漏れる

- 場所：`kernel/src/dev/hda_codec.c:99`、`:440-443`。
- 節：HDA 7.3.3.7、Figure 64（index は bit 11:8）。
- 要旨：`AMP_INDEX(k)` に幅の制限が無い。Connection List は 16 を超えられる。
- 壊れ方：k=16 は index 0 に bit 12 を重ねる。道の外の入力を閉じる word が input 0 を閉じる。input 0 が道の上なら無音になる。k=64 は別の方向の bit を立てる。
- 重さ：当たれば壊れる。

### B9. 複数の SDI の codec を二つに数える

- 場所：`kernel/src/dev/hda.c:749-752`。
- 節：HDA 7.3.2、7.3.4.1。
- 要旨：一つの root が複数の SDI で State Change を出す codec を、SDI ごとの別の codec として読む。
- 壊れ方：出口が重複して並ぶ。同じ pin へ別の番地で verb が行く。
- 重さ：当たれば壊れる。

### B10. Power Widget を D0 にしない

- 場所：`kernel/src/dev/hda_codec.c:370-374`、`:436`、`:467-469`。
- 節：HDA 7.2.3.6、7.3.3.10。
- 要旨：D0 にするのは function group と道の widget だけである。Type 5 の Power Widget は上限を決めるが、触れない。
- 壊れ方：Power Widget が低い電源で残ると、従う widget は D0 に上がらず無音になる。
- 重さ：当たれば壊れる。

### B11. Function Reset をせず、残った設定を引き継ぐ

- 場所：`kernel/src/dev/hda.c:744-760`、`kernel/src/dev/hda_codec.c:370-374`。
- 節：HDA 7.3.3.33、5.5.2、Table 84、7.3.3.32。
- 要旨：Table 84 の設定は link reset をまたいで残る。コードは Function Reset を出さず、Stripe Control も書かない。
- 壊れ方：firmware が残した別の converter の stream 番号が 1 なら、その converter も同じ流れを別の pin へ鳴らす。残った pin control や mixer の道も生きたままになる。
- 重さ：当たれば壊れる。

### B12. S3 の前に codec の電源を下げない

- 場所：`kernel/src/dev/hda.c:826-835`。
- 節：HDA 7.3.3.10（ClkStopOk）、7.3.4.12。
- 要旨：`hda_quiesce` は codec を D0 のまま CRST する。quiesce は mutex を待てないので、verb を出せない形である。
- 壊れ方：codec の電源の遷移が持つクリックの抑えが効かず、眠りと起きのたびに音が出る。D3cold を持つ codec に D3cold への手順を踏まない。
- 重さ：当たれば壊れる。

### B13. digital の出口を 1 秒の無音で止め、KAE を使わない

- 場所：`kernel/src/dev/hda.c:122`、`:614-619`、`kernel/src/dev/hda_codec.c:473`。
- 節：HDA 7.3.3.9（KAE、本文と脚注）。
- 要旨：digital の相手は時計が止まると、再生まで 1 秒以上かかることがある。KAE はその対策で、接続中に立てることを文書が勧める。コードは DigEn だけを書く。
- 壊れ方：HDMI・DisplayPort・S/PDIF で、1 秒以上の無音の後の通知音の頭が欠ける。動画の再開の頭も欠ける。
- 重さ：当たれば壊れる。

### B14. Aud_Synch_Delay を使わない

- 場所：`kernel/src/dev/hda_codec.c:379-398`、`kernel/src/dev/hda.c:601-602`。
- 節：HDA Figure 79（byte 6）、Table 123（値 ×2 ms、最大 500 ms、合わせるべき遅れ）。
- 要旨：コードは pin の遅れだけを引く。表示器が申告する遅れを読まない。
- 壊れ方：表示器の遅れが丸ごと音と映像の一定の差として残る。溜まらないが、差は最大 500 ms である。
- 重さ：当たれば壊れる。

### B15. 組の時刻が ABI を越えず、注釈が偽である

- 場所：`kernel/src/dev/audio.c:68`、`:239`、`:909-911`、`kernel/src/dev/hda.c:24-25`、`user/libc/web/avmedia.c:238-245`、`:292-293`。
- 節：HDA Table 68 の注 3（BCLK は 100 ppm）。3.3.16、4.5.7.1（Wall Clock）。
- 要旨：`sink_played_at` は書くだけで読み手が無い。`struct n_audio_status` に時刻が無い。`avmedia.c` は `queued_frames` と `device_pending_frames` を読むだけで、組を受けない。`hda.c:24-25` の「媒体の時計はこの組に従う」は、この版では偽である。
- 壊れ方：媒体の時計は 5.33 ms ごとの古い報告を、時刻なしで受ける。読みは階段になり、盤の線 ±1 ms に届かない。音と TSC の比を補正する輪はこの差分に無い。音の無い時計や実時間との差は、文書の許容の上限で 1 日 8.64 秒まで開く。
- 重さ：当たれば壊れる。

### B16. ELD_Ver を見ない

- 場所：`kernel/src/dev/hda_codec.c:379-397`。
- 節：HDA Figure 78、Table 118。
- 要旨：11111b の ELD は、canned の欄以外を無視せよと文書が書く。名前と Conn_Type はその canned の欄に入らない。
- 壊れ方：表示の driver が埋める前の ELD から、でたらめな名前と接続の種類を読む。
- 重さ：当たれば壊れる。

### B17. SAD を見ず、相手の format を確かめない

- 場所：`kernel/src/dev/hda_codec.c:379-398`、`kernel/src/dev/hda.c:730-731`。
- 節：HDA Figure 79、Table 122。
- 要旨：SAD_Count と SAD を読まない。converter の format だけで決める。SAD の中身の照合には CEA-861 が要り、それは照らせなかった。
- 壊れ方：sink が受けない format や、SAD_Count が 0 の sink へ流す。
- 重さ：当たれば壊れる。

### B18. function を D0 にしない

- 場所：`kernel/src/dev/pci.c:282-287`、`kernel/src/dev/hda.c:917-918`。
- 節：PCIe 6.0 5.3.1.4.1。
- 要旨：D3Hot の function は configuration と message しか受けない。カーネルの driver の道に PMCSR を書く所は無い（`pciuser.c:591-594` の見張りだけ）。
- 壊れ方：firmware が D3Hot で残した function では、register は全部 1 を読み、`does not come out of reset` で捨てられる。音の出口が消える。
- 重さ：当たれば壊れる。

### B19. 止まったと確かめずに reset へ進む

- 場所：`kernel/src/dev/hda.c:292-299`、`:528-531`、`:336`、`:338`、`:510`。
- 節：HDA 3.3.7、3.3.21、3.3.35。
- 要旨：RUN が消えなくても CRST を書く。SRST が読めなくても stream のレジスターを書き続け、RUN を立てる。CORBRP の reset の読みの結果を捨てる。`:335` の注釈は 3.3.21 の読みの手順を省く理由を書くが、文書にその例外は無い。
- 壊れ方：動いている DMA のまま reset が入り、結果は文書の範囲の外になる。`stream_start` は戻り値で失敗を返さない。
- 重さ：当たれば壊れる。

### B20. 客の停止の失敗を捨て、claim の後で音が戻らない

- 場所：`kernel/src/dev/hda.c:857`、`:860`、`kernel/src/dev/pciuser.c:501`、`:802`（`claim_form`）、`kernel/src/dev/iommu.c:919-924`。
- 節：HDA 3.3.7、3.3.35。
- 要旨：`hda_guest_stop` は DMA が止まらなくても group を渡す。guest を再び起こすのは claim の形成の成功だけである。claim の終わりと、`claim_recover` 以降の形成の失敗では起こさない。
- 壊れ方：`nvrm` が終わると、GPU の音の出口は次の claim まで消える。止まらない DMA は次の domain へ届く。
- 重さ：当たれば壊れる。

### B21. 出口の切り替えで、輪の中身を played と数える

- 場所：`kernel/src/dev/hda.c:680-687`、`kernel/src/dev/audio.c:434-455`。
- 節：文書の違反ではない。依頼の「位置の読み」の項で挙げる。
- 要旨：`close` は鳴っていない最大 16 block（85 ms）を played と報告する。
- 壊れ方：媒体の時計が一度跳び、その音は聞こえない。溜まりではない。B6 と重なると、mode change ごとに出る。
- 重さ：当たれば壊れる。

### B22. AC'97 で RUN が立ったまま Reset Registers を書く

- 場所：`kernel/src/vm/ac97.c:230`（`ac97_close` の `:247` から）。
- 節：ICH7 PRM 2.2.7（RR は RPBM が消えた後。立ったまま書くと結果は未定義）。
- 要旨：`outb(PO_CR, CR_RESET)` は RPBM を落とすのと RR を立てるのを一度に行う。RPBM が消えた読みと DCH を待たない。
- 壊れ方：実機の ICH で結果が未定義になる。QEMU で通ることは証拠にならない。下限の Zen 4 に ICH は無いので、影響は VM の道に限られる。
- 重さ：当たれば壊れる。

### B23. AC'97 で codec の準備を確かめずに進む

- 場所：`kernel/src/vm/ac97.c:274-276`、`:283`、`:289-292`、`:335`。
- 節：ICH7 PRM 2.2.9（S0CR を bus master の前に確かめる）。
- 要旨：100 ms で ready が立たなくても mixer に書く。`hw_init` の結果を起動では捨て、resume では `hw_ready` を立ててから false を返す。
- 壊れ方：codec の無い箱で DMA が走り、音は出ない。
- 重さ：当たれば壊れる。

### B24. Port 10b と 11b の pin を sense しない

- 場所：`kernel/src/dev/hda_codec.c:345`。
- 節：HDA 7.3.3.31、Table 109。
- 要旨：`senses` は Port 00b だけで立つ。11b は jack と内蔵の両方を持つ。Port 10b の表示の pin は ELD も読まれない。
- 壊れ方：11b の jack の抜き差しが出口の選択に効かない。10b の表示の pin は、表示器が無くても jack なしの出口として残り、音を吸う。
- 重さ：当たれば壊れる。

### B25. 一度に鳴る出口が機械で一つだけである

- 場所：`kernel/src/dev/audio.c:424-455`（`active`）、`kernel/src/dev/hda.c:10`。
- 節：文書の違反ではない。依頼の「複数の出口」の項で挙げる。
- 要旨：複数の制御器と複数の出口は数えるが、同時に鳴らせない。制御器ごとの stream も一本である。
- 壊れ方：スピーカーと HDMI へ別々に、また同時に出せない。
- 重さ：当たれば壊れる。

### B26. 流れの数の固定の上限

- 場所：`kernel/src/dev/audio.c:19`、`:855`。差分の前からある。
- 節：文書の違反ではない。AGENTS.md の「上限と中断」に反する。
- 要旨：流れは 8 本で、9 本目は開けない。
- 壊れ方：ゲーム、複数のタブ、通話を同時に使う規模で、音を開けないアプリが出る。
- 重さ：当たれば壊れる。

## 指摘：きれいさ

| 番号 | 場所 | 節 | 要旨 |
|---|---|---|---|
| C1 | `hda.c:258-259` | 3.1.2、3.3.24、3.3.31 | CORBSIZE・RIRBSIZE の RO の bit 7:4 へ 0 を書く。文書は読んだ値を書けと定める。 |
| C2 | `hda.c:537` | 3.1.2、3.3.35 | `SD_CTL2` の TP と RsvdP を 0 で書く。 |
| C3 | `hda.c:267`、`:269`、`:279`、`:332`、`:343`、`:508`、`:528-530`、`:544` | 3.1.2 | GCTL、CORBWP、RINTCNT、INTCTL、`SD_CTL` の RsvdP を read-modify-write しない。 |
| C4 | `hda.c:559` | 3.3.37 | LPIB は CBL の値を含む。注釈の "no valid position" は誤りである。報告が一度遅れるだけで、溜まらない。 |
| C5 | `hda.c:539` | 3.3.40 | FIFOS の節は 3.3.40 で、注釈の 3.3.41 は誤りである。 |
| C6 | `hda.c:317` | 3.3.7 | reset 中の制御器へ `streams_halt` を出す。読みは既定値で、書きは効かないので害は無い。 |
| C7 | `hda.c:330-331`、`:315-323` | 3.3.9、4.2.2 | STATESTS を CRST の前に消さない。古い bit があると、居ない codec へ verb が行き 10 ms を失う。 |
| C8 | `hda.c:534` | 3.3.11 | OUTSTRMPAY を見ない。今の 48 kHz・8 byte 以下では足りる。 |
| C9 | `hda.c:392-396` | 4.4.1.4 | CORBRP を見て空きを確かめない。一度に一 verb なので普段は当たらない。 |
| C10 | `hda_codec.c:283-290`、`hda_codec.h:70` | Table 111 | Device が Other（Fh）の出力の pin を捨てる。`HDA_OUTPUT_OTHER` は作られない枝である。 |
| C11 | `hda_codec.c:438-444` | 7.1.3 規則 5 | mono mixer の例外（入力 2 つ）を扱わない。 |
| C12 | `hda_codec.c:436`、`:468` | 7.3.3.10 | PS-Error を見ない。D3 からの復帰の待ちを取らない。文書に待てとは書いてなく、品質の問題である。 |
| C13 | `hda.c:569-573`、`:630` | 文書なし | `at` が `written` を飛ぶと、`ordinal_at` はしばらく低く出す。単調で、溜まらない。 |
| C14 | `ac97.c:5`、`:135`、`:138` | ICH7 PRM 2.2.4、2.2.5 | LVBCI は fetch で立ち、PICB はメモリーから読んでいない標本の数である。注釈の「played」は誤りで、報告は FIFO の分だけ先走る。一定の差で、溜まらない。 |

## 位置の読みが溜まりのない形か

- `position_update` の `pos` は LPIB の差の和で、周回を法とするので LPIB に収束する（`hda.c:553-566`）。
- 時計が足すのは丸ごとの周回だけで、最寄りの周へ丸める。誤りは 8192 frame の倍数の段差で、溜まらない。
- LPIB の位置は毎回装置から読む。codec の 48 kHz は BCLK か SYNC から出る（5.4.1）ので、位置は link の時計に乗る。
- 溜まりの判定を崩すのは三つである。DESE の後の空回り（B5）、組の時刻が ABI を越えないこと（B15）、音と TSC の比の輪が無いこと（B15）。
- 音と映像の差：映像が音の frame の数に従う間は溜まらない。
- 音と実時間の差：この差分に補正は無く、100 ppm まで開く。

## 合っていた手順

- レジスター：全オフセットとビット（3.3、Table 2。GCAP、GCTL、STATESTS、INTCTL、INTSTS、CORB・RIRB の全部、SD の CTL、STS、LPIB、CBL、LVI、FIFOS、FMT、BDPL、BDPU）。
- reset：CRST=0 を読み、100 µs 待ち、CRST=1 を読み、521 µs 後に STATESTS を読む（4.3、5.5.1.2）。
- CORB・RIRB：停止中の配置、size、WP、RP の reset、RINTCNT、最初の verb と最初の読みは +1、CORB はページの頭、RIRB は +2048（4.4.1、4.4.2、3.3.18、3.3.25）。
- BDL：16 byte、IOC は bit 0、ページ境界、buffer は 128 の倍数、CBL は和と一致、LVI は 31（3.6.3、3.3.39）。
- format：48 kHz、BITS、CHAN、container、左詰め、codec と stream で同じ値（3.3.41、3.7.1、4.5.1、7.3.3.8）。48 kHz の PCM は全 codec の必須（Table 139）。
- stream：BDL は RUN=0 で作り、STS は byte で書き、FIFOS は FMT の後に読む（4.5.6、3.3.40）。
- verb の形：codec、NID、4-bit と 12-bit、全 verb の ID（7.3）。
- parameter：番号、Node Count、AFG の Output Delay、Widget caps、Type、PCM、Pin caps、Amp caps（7.3.4）。
- Connection List：short・long、範囲、Selector と Mixer の Select の扱い（7.1.2、7.1.3、7.3.3.2、7.3.3.3）。
- pin：Config default の全欄、Assoc 0 と 15、PinCntl、Pin Sense の bit、Unsolicited、EAPD の read-modify-write、DigEn、channel count（7.3.3.13〜15、7.3.3.31、7.3.3.35、Table 92、93、111）。
- amp：Set の形、0 dB は Offset、範囲外を書かない（Figure 64、7.3.4.10）。
- ELD の読み：F2F の valid bit と data、MNL の幅と上限、名は byte 20 から（Table 117、119、Figure 79）。Conn_Type は B1。
- DIP と ASP の verb の手順：size、index、data、Xmit、slot の map（Table 125〜131、136）。頭と本文の値は下の「照らせなかったもの」に書く。
- 遅れ：group の Output Delay、0 なら道の widget の和（7.3.4.5、7.3.4.6）。
- 割り込み：SIE の番号は入力→出力→双方向（3.3.14）、RIRB の形（Table 54）。
- 複数の codec：番地 14 まで（5.5.3.2）。stream tag は 1（3.3.35）。
- PCI：class 04h・subclass 03h は prog-if 00h と 80h のどちらも HDA（Code-ID 1.19）。BAR は UC（`vmm.c:59`）。
- メモリー：`pmm_alloc_contig(…, 32)` は 4 GiB 未満で、64OK の無い制御器が届く（3.3.2）。CORB・RIRB・BDL・輪は clflush と mfence の後で装置へ渡す。
- IOMMU：`iommu_map(dom, NULL, phys, pages)` は恒等の写像で、アプリの DMA も番地は物理と同じなので衝突しない。`bus_reset` は客を止めた後でしか及ばない（`pciuser.c:661-684`、`:802`）。
- 電源：`hda_quiesce` は止まりを確かめて返し、世代の仕掛けで途中の verb と開きを捨て直す。S3 の後は `link_up` と再度の開きが走る（pm.h の順と一致）。
- AC'97：レジスターのオフセット、CR・SR の bit、SR_CLEAR、BDBAR の境界、32 bit で CIV・LVI・SR を読むこと、GLOB_CNT の bit 1、GLOB_STA の bit 8（ICH7 PRM 2.2.1〜2.2.9、Table 2-2）。

## 照らせなかったもの

- CEA-861（CTA-861）：公式の入手先は有料の販売だけである。公開のコピーは CTA の削除通知の対象なので取らなかった。`hda_codec.c:456` の HDMI の InfoFrame の 5 byte は照らせなかった。B17 の SAD の中身も照らせなかった。
- HDMI：1.4b 以降は採用者だけで、1.3a は hdmi.org の申請の form が要る。個人の情報を入れる手続きなので行わなかった。上と同じ値は照らせなかった。
- DisplayPort：VESA の会員だけである。`hda_codec.c:456` の secondary data packet の頭は照らせなかった。
- Intel AC'97 2.3 の codec の仕様：公式の場所を確かめられなかった。mixer のオフセットは PRM の表で照らした。`NAM_MASTER` と `NAM_PCM_OUT` の値と `NAM_RESET` の扱い、VRA の待ちは照らせなかった。
- AC'97 の buffer descriptor の bit：PRM と datasheet を検索したが、形の定義が見つからなかった。8 byte であることと、32 個までであることだけを照らした。`BD_IOC` と `samples` の単位は照らせなかった。
- 範囲を展開した Connection の index を Select と amp に送ること（`hda_codec.c:143`、`:438`）：7.3.3.2 は Length 以上を不定と書くだけで、範囲つきの番号を定めていない。文書から決まらない。

## 読み切れなかった範囲

- 全文を読んだもの：`hda.c`、`hda_codec.c`、`hda_codec.h`、`audio.c`、`audio.h`、`vm/ac97.c`。
- 差分だけを読んだもの：`iommu.c`、`iommu.h`、`pci.h`、`pciuser.c`、`rdp.c`、`common/abi.h`。
- 関数だけを読んだもの：`pci.c`（有効化、MSI、復元）、`pmm.c`、`vmm.c`、`avmedia.c`（`sink_status`、`current`、周辺）。
- `tests/hda_codec_hosttest.c` は頭と ELD の行だけを読んだ。
- 読んでいないもの：`iommu_intel.c`、AMD の IOMMU の裏、`docs/nocturne-native-abi.md` の本文（`played_at` の検索だけ）、`user/libc/media*.c`。
- MSI・MSI-X の手順は差分の外なので、PCIe 6.1.4 と照らしていない。
- HDA 仕様の 5 章の前半（link の frame）と 6 章の電気の部分は、BCLK の許容の他は読んでいない。
