# 画面のモードの管理の監査（2026-11-02）

## 依頼文（全文）

```text
Nocturne OS（`D:\Programes\llm vibe slop os`）の、画面のモードの管理を、読むだけで敵対的に監査してください。返事と報告は全部日本語で書きます。識別子とパスは元の表記のままです。

## 依頼者の言葉（原文）
"Our graphics pipeline needs to be audited. and needs to be assumed wrong. includeing our compositer, and and our lack of disply settings or handleing. as the os is the one that manages the displys hz, color depth, 8bpc or 10bpc, resolution, ect from what the display presents as options. we do not have that. ... use a couple subagents to do a read only adversarial audit. have them assume that our code base is a mess and that "fake" implmentations of things are rampent."

あなたの担当は画面のモードの管理です。画面が申告する選択肢を読み、OS が解像度、Hz、色の深さ（8bpc、10bpc）などを選び、ハードウェアへ適用し、利用者が変えられるまでの全部です。合成器の中身と音は、別の担当が見ます。

## 読むだけ
- コード、設定、媒体、git の状態を変えません。ビルド、QEMU、Hyper-V、スクリプトの実行をしません。OS は動かせないので、証拠はソースと仕様だけです。
- 書いてよいファイルは報告の `docs/records/audit-display-20261102.md` の一つだけです。頭にこの依頼文を全文そのまま写し、続けて報告の全文を書きます。

## 監査の構え
- コードは散らかっていて、偽の実装がはびこっていると仮定します。名前、コメント、文書、コミットメッセージ、試験の合格を信じません。依頼者の「we do not have that」も、そのまま受け入れず否定もせず、道を辿って確かめます。設定の頁に項目があっても、選んだ値がハードウェアのレジスターや RM の呼出しまで届くかを確かめます。表から呼ばれる関数と `SYS_*` の番号での呼出しは codegraph に出ないので、`rg` でも探します。
- 確かめる偽物の形：成功を返して何もしない、見せるだけの UI、決め打ちの値、読んだのに使わない値、保存するだけで適用しない設定、呼ぶ者のいない道、途中で切れた道、8 ビットへ黙って落とす所、コメントと食い違う動き、VM の道だけがある所。
- 証拠の無い所見は出しません。各所見に `file:line` を付けます。数を稼ぎません。読んで本物だと確かめた部分は、本物と書きます。確かめられないことは、確かめられないと書きます。

## 守る線（監査の物差し）
先に `AGENTS.md` と `.claude/rules/map.md` を読みます。`.claude/rules/kernel.md` と `.claude/rules/nvidia.md` も読みます。要点は次のとおりです。
- 層は分かれています。カーネルは GPU を知らず、`nvrm` がアプリのドライバーです。画面の選択と保存の持ち主はサービス `display` と `settings` で、設定の窓は窓だけです。VM の処理は `kernel/src/vm/vm.h` の後ろだけです。
- 設定の持ち主はシステムで、変えた瞬間に効きます。GUI が先です。解像度と拍は EDID を変数にして決め、固定しません。
- メーカーの申告と公式の文書を信じ、そのとおりに使います。VRR の画面に固定の拍を強いません。対象は実機で、機械を決め打ちしません。VM で通ることは実機で動く証拠になりません。
- 文書は古いことがあります。事実は今のコードで確かめます。

## 読む場所
`common/edid.h`、`common/display_abi.h`、`kernel/src/gui/screenmode.c`、`kernel/src/gui/wm.c`（`SYS_SCREEN_*` と画面の配置）、`kernel/src/dev/fb.c`、`user/apps/display.c`、`user/apps/settings.c` の画面の頁、`user/apps/settings_words.h`、`user/apps/settingsd.c`、`user/apps/settings_table.h`、`common/settings_abi.h`、`user/libc/settings.c`、`kernel/src/fs/bootrecord.c`、`ports/nvidia/display.c`・`display.h`・`nvkms_kapi_rm.c`・`nvkms_os.c`・`nvrm.c`・`nvrm_serve.c`、`kernel/src/vm/` の `virtio_gpu.c`・`vbe`・`rdp.c`・Hyper-V の映像、`docs/nocturne-native-abi.md` の画面の節。ほかに要る所は辿ってください。

参照：NVIDIA は `D:\Programes\cloned repos\open-gpu-kernel-modules`（`src/nvidia-modeset` など）と `open-gpu-doc`、仕様は `D:\Programes\OS DEV INFO` を探します。無ければ VESA と CTA の公開の範囲で述べ、確かめられない事実は確かめられないと書きます。Linux や他の OS のソースは参照しません。

## 答えてほしいこと
1. 次の表を作ります。行は項目です：解像度、Hz（小数の拍を含む）、色の深さ（6、8、10、12bpc）、画素の形式（RGB、YCbCr）、色域と転送関数（HDR、sRGB）、VRR と Adaptive-Sync、スケーリング、回転、複数画面の配置と主画面、倍率。列は段です：画面の申告を読む → カーネルの持ち物 → サービス `display` → 設定の窓 → 保存 → ドライバー（実機の nvrm と nvkms、VM）→ ハードウェアへの適用 → 適用の確かめ。各升目に、本物、一部、偽物、無し を `file:line` つきで書きます。
2. 申告の読みの正しさ：EDID の base block、CTA-861 の拡張（VIC、YCbCr、HDR、VRR 範囲、VSDB）、DisplayID。何を読み、何を捨て、何を誤って読むか。EDID が無い、壊れている、複数 block の場合。
3. モードの適用：実機の NVIDIA の道で、モードを選んで適用する処理は本当にあるか。nvkms の validate と commit、リンクの帯域の確かめ（DP、HDMI、DSC）、失敗時の扱い、期限つきの試しと戻し。既定のモードは誰がどう決めるか（最高の Hz と解像度を選ぶか、firmware のままか）。
4. 深さの道：10bpc を選んだとき、scanout の形式、合成器の緩衝、dither、窓の緩衝まで一貫するか。どこで 8 ビットへ落ちるか。
5. 動的な変化：hotplug、画面の抜き差し、モード変更のとき、窓、合成器、スレッド、緩衝はどうなるか。
6. 偽物の一覧と、Windows 級の OS の画面の管理と比べた欠けの一覧。

## 報告の形
各所見に、重さ（致命、重い、中、軽い）、種類、`file:line`、呼出しの順の根拠、起きる条件、確信（コードで証明、可能性あり）を書きます。最後に、読んで本物と確かめた部分と、読めなかった範囲を書きます。修理の案は、原因を消す形で短く書きます。

最後の返事は、報告のパスと、重い順の要点を 15 行以内で返します。
```

## 追加の指示（全文）

### 追加の指示 1

```text
依頼者から画面の管理の監査へ追加の指示です。OS の設定とドライバーの設定を混同せず、重なりも扱ってください。

原文："OS settings and driver settings should NOT get confused. and there will be setting over lap."

監査に次を足します。原文の範囲を超える線引きは、私からは渡しません。
1. 答えの 1 の表の各行に、その項目が OS の設定か、ドライバーの設定か、両方に現れる重なりかを足します。根拠も書きます。
2. 今のコードで二つが混ざっている所を `file:line` つきで挙げます。例：ドライバーの都合がサービス `display` や設定の窓やカーネルの口へ漏れている所、OS の選択をドライバーが持っている所、NVIDIA の語や値が OS の共通の ABI に入っている所。
3. ドライバーの設定を置く場所が今あるかを確かめます。実機の nvrm と nvkms が持つ設定と、それを変える道を調べます。無ければ無いと書きます。
4. 重なる設定で、どちらが決めるか、両方が食い違ったときにどうなるか、一方の変更がもう一方へどう見えるかを、今のコードの動きとして書きます。その決まりが無ければ、無いと書きます。
5. 修理の案では、二つの持ち物を分けた形を短く書きます。

ほかの条件は最初の依頼と同じです。読むだけで、書いてよいのは報告の `docs/records/audit-display-20261102.md` だけです。報告の頭の依頼文のすぐ後に、この追加の指示も全文写してください。
```

### 追加の指示 2

```text
依頼者から重なる設定の決まりが来ました。これを監査の物差しにしてください。

原文："the overlaped settings will change one another if one is changed.  we have no generic/default gpu drivers or display handing and are randomely hard codeding mesa and nvidia stuff. that is a big issue"

1. 重なる設定は、片方を変えるともう片方も変わるのが決まりです。前の追加の指示の 4 は、この決まりと今のコードの差として書いてください。項目ごとに、OS 側を変えたらドライバー側が変わるか、ドライバー側を変えたら OS 側が変わるかの両方向を、呼出しの順で確かめます。そうならない項目は、そう書きます。
2. 汎用のドライバーと画面の扱いの欠け、Mesa と NVIDIA の決め打ちは、別の担当が全体を調べます（報告は `docs/records/audit-gpu-generic-20261102.md`）。あなたは画面の管理の道の中で出会ったものだけを `file:line` で挙げてください。全体の棚卸しはしなくて結構です。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

### 追加の指示 3

```text
依頼者から、設定を変える三つ目の者が示されました。モニター自身の変更も監査に入れてください。

原文："And the settings can also be changed by the moniter its self.(resolution hz, hdr)   there needs to be an abstarction of some kind or a service of some kind"

1. 物差し：解像度、Hz、HDR は、OS、ドライバー、モニター自身の三者が変えられます。重なる設定は、一つを変えると他も変わるのが決まりです。
2. モニターから始まる変更の道を、今のコードで呼出しの順に辿ります。hotplug、DP の IRQ_HPD のような短い通知、EDID の変化、モニターの OSD での HDR やモードの切り替え、リンクの再学習がその例です。ドライバーのどこで受け、サービス `display`、設定の窓、合成器まで届くかを確かめます。届かない所は `file:line` で示します。仕様で確かめられない事実は、確かめられないと書きます。
3. 答えの 1 の表の重なりの欄に、モニターの列を足します。項目ごとに、三者それぞれの変更が他の二者へ伝わるかを書きます。
4. 三者の間を取り持つ抽象や持ち主のサービスが今あるかを確かめます。今の画面の状態の正本を誰が持つか、複数の者が別々の写しを持っていないか、変更の知らせの流れはどちら向きかを書きます。無ければ無いと書きます。
5. 修理の案に、その抽象またはサービスの形を短く書きます。責務、持ち物、三者との間の口、片方の変更が他へ伝わる順序、誰の持ち物でないかを含めます。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

### 追加の指示 4

```text
画面の管理の監査の物差しとして、依頼者の線を渡します。

- 1440p の画面三枚（一番速いもので 280 Hz、遅いもので 165 Hz。三枚とも VRR 対応、パネルは OLED と VA の両方）と、消費者向けの 1080p 600 Hz の画面を、それぞれ申告どおりに選んで適用できるか。Hz が画面ごとに違い、VRR も画面ごとに扱えるか。
- 極みは試す線で、決め打ちではありません。Hz、解像度、画面の数、色の深さをコードの定数や固定の大きさの表で縛っている所は、それ自体が所見です。値は装置の申告から計算するのが決まりです。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

### 追加の指示 5

```text
性能の線を足します。依頼者によると、消費者向けの 1000 Hz の画面がすでに売られています（LG UltraGear 25G590B：1920×1080、1000 Hz、DisplayPort 2.1 UHBR20、HDR10、G-SYNC Compatible、FreeSync Premium）。

この画面を申告どおりに選んで適用できるかを見てください。EDID と DisplayID の読み、拍の値の型と範囲（mHz の幅、画素クロックの上限）、DP 2.1 UHBR のリンクの帯域と DSC の確かめ、nvkms の道、設定の窓の表示を含めます。これも試す線で、1000 を定数にするという意味ではありません。RTX 4070 が DP 2.1 UHBR を出せるかどうかは、作った者の文書で確かめ、確かめられなければそう書きます。

ほかの条件は変わりません。報告の頭の依頼文の後に、この指示も全文写してください。
```

---

# 報告

## 0. 結論

- 解像度と Hz には、申告から窓、保存、NVKMS の commit までの道があります。
- 色の深さ、画素の形式、HDR、VRR、回転、実機の複数画面には、適用の道がありません。
- 依頼者の「we do not have that」は、この範囲で正しいです。
- 解像度と Hz の道も、実機では起動の競争の下にあります。nvrm は最初の望みを待たず、来ていなければ自分の表示を止めます（D-01）。
- 依頼者の線は通りません。実機で映るのは一枚だけで、VRR はどの画面でも入りません（D-02、D-03）。
- 1000 Hz の線では、OS の層に 1000 を縛る定数はありません。選べるかは NVKMS の判定で決まり、動かしていないので確かめられません（11.1 節）。
- RTX 4070 は NVIDIA の公式の頁で DisplayPort 1.4a です。UHBR を出しません。DP では 1000 Hz と 600 Hz に DSC が要ります（11.1 節）。
- 三者の間を取り持つ正本はありません。HDR、VRR、リンクの bpc の変化は OS のどこにも届きません（D-09）。
- ドライバーの設定の置き場はありません（D-10）。
- OS を動かしていません。証拠はソースだけで、実行の証拠はありません。
- `D:\Programes\OS DEV INFO` に DisplayPort、EDID、CTA、HDMI、DisplayID の文書はありません。これらの事実はコードの算術か NVIDIA のコメントで述べ、それ以外は確かめられないと書きます。RTX 4070 の事実は NVIDIA の公式の頁で確かめました。

## 略号

| 略号 | パス |
|---|---|
| `nv/display.c` | `ports/nvidia/display.c` |
| `svc/display.c` | `user/apps/display.c` |
| `apps/settings.c` | `user/apps/settings.c` |
| `settingsd.c` | `user/apps/settingsd.c` |
| `words.h` | `user/apps/settings_words.h` |
| `abi.h`・`display_abi.h`・`settings_abi.h`・`edid.h` | `common/` の下 |
| `screenmode.c`・`wm.c` | `kernel/src/gui/` の下 |
| `fb.c`・`fb.h`・`pciuser.c` | `kernel/src/dev/` の下 |
| `vbe.c`・`rdp.c`・`virtio_gpu.c` | `kernel/src/vm/` の下 |
| 上流 | `D:\Programes\cloned repos\open-gpu-kernel-modules-615.78.08`。`nvkms-*` は `src/nvidia-modeset` の下にあり、ファイル名で引きます。 |

## 1. 所見（重い順）

### D-01 致命：nvrm の起動がサービス `display` との競争で決まる

- 種類：途中で切れた道。コメントと食い違う動き。試験の偽物。
- 場所：`nv/display.c:1814-1823`、`nv/display.c:1431-1436`、`screenmode.c:127-134`、`ports/nvidia/nvrm.c:344`。
- nvrm の順：
  1. `nvrm.c:344` が `nvrm_display_start` を呼びます。戻り値は捨てます。
  2. `nv/display.c:1814` の `modes_offer(true)` が `n_pci_screen_modes` を呼びます。
  3. `screenmode.c:129` は `offer_serial++` だけをします。望みは立てません。
  4. `nv/display.c:1819` の `mode_follow_now` が `n_pci_screen_state` を期限 0 で読みます（`:1433`）。
  5. `mode_serial` が進んでいなければ、そのまま戻ります。
  6. `:1820` は `scr.given` が無いので `fail` へ行きます（`:1821-1822`）。
  7. `:1859-1861` が `nvrm_display_stop` を呼びます。
- サービスの順：
  1. 見張りで起きて `look()`（`svc/display.c:310`）を走らせます。
  2. `n_screen_device` と、`settingsd` への往復を含む `n_setting_get`（`user/libc/settings.c:63-76`）を通ります。
  3. `mode_answer_offer`（`svc/display.c:142-148`）が `n_screen_mode_apply` を呼びます。
  4. `kernel/src/sys/syscall.c:1291-1297` を通り、`screenmode.c:177-190` が望みを立てます。
- 遅い望みを拾う状態のスレッドは、検査の後の `nv/display.c:1831` で立ちます。
- nvrm の側の猶予は、`modes_offer` の中のログ（`nv/display.c:1370`）だけです。
- 負けた結果：
  - NVKMS の表示の全部が止まり、画面は firmware の面のままです。
  - やり直しはありません。`gpu` の `on_failure`（`user/apps/services.c:65-67`）は process が終わらないので働きません。
  - 窓は「表示のドライバーが画面を返したので、起動の時の解像度です。」と出すと読みました（`screenmode.c:236`、`apps/settings.c:910`、`words.h:155`）。
- 古い前提の文：`nv/display.c:1816`、`nv/display.c:9-13`、`ports/nvidia/display.h:5-7`、`ports/nvidia/README.md:321`。
- 試験の偽物：
  - `tests/nvrm_display_hosttest.c:332-352` の `n_pci_screen_modes` は、中で同期に望みを立てます。
  - だからこの試験の合格は、競争が無いことの証拠になりません。
  - この試験の最後の変更は `a60e224f` で、望みをサービスへ移した `d2ca6cee` より前です。
- 比べた二つ：`d2ca6cee` の前の `screen_modes_offer` は同期に望みを立て、今の `screen_modes_offer` は立てません。
- 実機の記録はコミットの文の「9 回目で 120 Hz にできた」（`6e912418`）だけです。これは `d2ca6cee` より前の版の話で、今の版の証拠になりません。
- 起きる条件：サービスの答えが `mode_follow_now` の読みに間に合わないときです。
- 確信：順序の欠陥はコードで証明。どちらが勝つかは、動かしていないので確かめられません。
- 修理：`:1819-1823` の検査を消します。状態のスレッドを先に立て、望みが来た時に映します。試験の偽物を非同期にします。

### D-02 致命（依頼者の線に対して）：実機は一つの出力だけを出す

- 種類：決め打ち。VM の道だけがある所。
- 場所：`nv/display.c:324-381`、`:375-377`、`:132-210`（単数の `scr`）、`:219-220`（単数の `offer`、`kmodes`）。
- 出力 0 の決め打ち：
  - `n_pci_screen_modes`：`nv/display.c:1266,1360,1423`。
  - `n_pci_give_screen`：`nv/display.c:1111,1114,1152`。
  - `n_pci_screen_state`：`nv/display.c:1433,1511,1660`。
  - フリップ：`nv/display.c:1057,1194,1549`。
- `:375-377` のログは、firmware が複数を点けていても一つだけを出すと自分で言います。
- `rescan`（`:1455-1471`）は、target がつながっている間、別のモニターを見ません。
- 上の層は複数画面を持ちます：`pciuser.c:356-388`、`fb.c:283-307`、`wm.c:1723-1757`、`svc/display.c:179-276`、`apps/settings.c:254-287,764-797`。
- 複数の画面に実際に届くのは、VM の二枚目の VGA（`vbe.c:140-153,169`）だけです。
- 起きる条件：実機でモニターが二枚以上のときです。残りは映りません。
- 確信：コードで証明。
- 修理：nvrm の状態を出力ごとの記録にし、頭を割り当て、全部の出力を申告します。

### D-03 重い：VRR は読み手だけで作り手がいない

- 種類：呼ぶ者のいない道。読んだのに使わない値。見せるだけの UI。
- 読み手：`screenmode.c:102,171`、`fb.c:391-396`、`wm.c:1660-1665,1689-1691,1736,1744`、`apps/settings.c:954`、`abi.h:572-575,590,598`。
- 作り手：製品のドライバーにいません。`mode_of` は flags を 0 に決め打ちします（`nv/display.c:240-243`）。
- NVKMS の側で使わないもの：
  - `dyn.vrrSupported`（上流 `nvkms-kapi.h:538`）を読みません。
  - `vrrEnabled`（`nvkms-kapi.h:395`、`nvkms-kapi.c:3681-3683` で `allowVrr` へ）を立てません。
  - `reply.vrrFlip`（`nvkms-kapi.h:463`）を読みません。
  - `minPresentInterval` は 1 です（`nv/display.c:697-698`）。
- ABI の欠け：
  - 保存の `struct n_setting_monitor_mode`（`settings_abi.h:124-129`）に VRR の欄がありません。
  - `same()`（`screenmode.c:59-62`、`svc/display.c:65-68`）は flags を見ません。
  - `abi.h:587-588` の「no two alike」により、同じ拍の固定と可変を並べられません。
  - `vrr_min_mhz` はモニターに一つで、モードごとではありません。
- `edid.h` は 0xFD を読みません。
- 起きる条件：常にです。VRR の画面は、固定の拍で出ます。
- 確信：コードで証明。
- 修理：申告に VRR の範囲を載せ、望みの欄にし、`allowVrr` と `vrrFlip` を通します。

### D-04 重い：色の深さは 8 ビットの決め打ちで、窓の項目は死んだ道

- 種類：決め打ち。見せるだけの UI。8 ビットへ黙って落とす所。
- 決め打ち：`nv/display.c:242`（`8, N_SCANOUT_XRGB8888`）、`:554`（`NvKmsSurfaceMemoryFormatX8R8G8B8`）、`:1106,1113`、`vbe.c:166`。
- `mode_index`（`nv/display.c:408-414`）は形式を無視します。
- `modes_list`（`:443-461`）は大きさと拍ごとに一つだけを申告します。
- 窓の「色の深さ」（`apps/settings.c:961-979`）は、形式の違う項目があるときだけ出ます。
- 製品のドライバーはそのような項目を出さないので、この項目は出ません。
- `format_name`（`apps/settings.c:755-760`）は面の形式を「10 ビット／8 ビット」と呼びます。
- 面の形式とリンクの bpc は別の物です。窓の語はこの二つを混ぜています。
- 起きる条件：常にです。
- 確信：コードで証明。
- 修理：画面ごとの深さを申告と望みの欄にし、`mode_of` と `surface_of` の決め打ちを消します。

### D-05 重い：10 ビットの扱いが ABI の文と逆で、報告が偽る

- 種類：コメントと食い違う動き。8 ビットへ黙って落とす所。
- `fb.deep` を書くのは `fb_set_simulated`（`fb.c:482-488`）だけです。`fb_init`（`fb.c:73-103`）は書きません。
- 本物の 10 ビットの面では `fb.bpc=10`、`fb.deep=false` になります。
- すると次が起きます：
  - `wm.c:3122` の報告は 8 になります。
  - `wm.c:1534` は deep の面を持ちません。
  - `fb.c:207-210` は 8 ビットを複製して広げます。
- `display_abi.h:7-8,97-99` は「10 ビットの画面でだけ deep を読む」と言います。
- 実際に deep があるのは、8 ビットの面の上の模擬だけです（`fb.c:199-206`）。
- なめらかな階調の間は `bits_per_channel=10`（`wm.c:3118-3122`）です。
- その値は CSS の `color`（`user/libc/web/css.c:5152`）と `colorDepth`（`user/libc/web/js.c:4810`）へ渡ります。面は 8 ビットのままです。
- `bits_per_pixel` と `red_bits` は firmware の値で、ドライバーの後は古くなります。
- `fb.bpc`、`fb.deep`、`simulated` は全画面で一つです（`fb.h:15-18`）。`fb_dither_able` は画面 0 だけを見ます（`fb.c:474-477`）。
- OLED を 10 ビット、VA を 8 ビットにする道はありません。
- 起きる条件：報告の偽りは階調の設定を入れたときです。逆転は本物の 10 ビットの面が来たときで、今の製品の道には来ません。
- 確信：コードで証明。
- 修理：深さを画面ごとの持ち物にし、報告は実効の値から作ります。

### D-06 重い：リンクの bpc と画素の形式を OS は決めず、見もしない

- 種類：途中で切れた道。
- 上流の決め方：
  - `nvkms-modeset.c:260-340`：`colorBpcSpecified` が無ければ UNKNOWN です。
  - `nvkms-evo.c:2725-2830`：RGB をその最大の bpc で先に選びます（`:2788-2793`）。
  - `nvkms-dpy.c:3609-3624`：EDID の bpc と引数の小さい方です。
- KAPI の `NvKmsKapiHeadModeSetConfig`（`nvkms-kapi.h:371-423`）に bpc と形式の欄はありません。
- 欄は下の層の `nvkms-api.h:2053,2059,2065` にあります。
- 今の値は `NVKMS_IOCTL_GET_DPY_ATTRIBUTE` にありますが、KAPI は使いません。
- KAPI の事象の表（`nvkms-kapi.c:70-74`）に `DPY_ATTRIBUTE_CHANGED` はありません。
- 上限の `nvkms_max_output_color_bpc` は既定 10 です（`ports/nvidia/nvkms_os.c:73,195-202`。NVIDIA 自身の `kernel-open/nvidia-modeset/nvidia-modeset-linux.c:95` も同じです）。
- 帯域が足りないと、NVKMS は bpc と形式を黙って下げて試し直します（`nvkms-evo.c:7066`）。OS は下がったことを知りません。
- DSC の有無とリンクの速さも OS に来ません。`nvkms-kapi.c` に DSC の語はありません（rg）。
- 起きる条件：10 ビットを申告する画面で、リンクは 10 ビットになる見込みです。OS は 8 と言います。
- 確信：欄の無いことと、読まないことはコードで証明。実機のリンクの値は可能性ありで、確かめられません。
- 修理：KAPI の上に新しい口を足すか、KAPI を広げて、bpc と形式を望みと実効として通します。カーネルを変えずに済むかは設計の時に確かめます。

### D-07 重い：HDR、色域、回転、モニターの電源の道は無い

- 種類：無し。
- 固定の値：`inputColorSpace = NONE`（`nv/display.c:705`）、`colorimetry = DEFAULT`（`:716`）、`olutFpNormScale` 既定（`:735`）、`csc` 恒等（`:740`）。
- `bActive` は常に TRUE です（`:712`）。
- 使わない欄：`hdrInfoFrame`、`dithering`、`olut`、`rrParams`（上流 `nvkms-kapi.h:275,397-401,417-420`）。
- `kernel/src/gui`、`fb.c`、`display_abi.h`、`svc/display.c`、`words.h`、`nv/display.c` に、HDR、回転、ガンマ、YCbCr、DPMS の語は出ません（rg で確かめました）。
- `nvkms-hdmi.c:641-648` と `nvkms-api.h:1598` の `supportsHDR` は NVKMS の中だけで止まります。
- バックライトの口は NULL です（`ports/nvidia/nvkms_os.c:232-237`）。
- 起きる条件：常にです。
- 確信：コードで証明。
- 修理：申告、望み、実効に、色の範囲、転送関数、HDR、回転、電源の欄を足します。

### D-08 重い：OS の EDID の読みは base block だけで、NVKMS の読みは OS へ渡らない

- 種類：読んだのに使わない値。途中で切れた道。
- `edid.h` が捨てるもの：拡張（`e[126]`）、0xFA、0xF7、0xF8、0xFD、ビット深さ（byte 20）、CTA-861、DisplayID。
- `edid_ids`（`edid.h:38-58`）は checksum を見ません。
- 固定の表：`fb.c:48-69` の `modes[32]`、`vbe.c:89-110` の 128 byte と `modes[8]`。
- nvrm は `edid_ids` だけを使い、`edid_valid` を呼びません（`nv/display.c:351,396-397`）。
- 実機の全部の解析は NVKMS の NvTiming です（`nvkms-modepool.c:336-375`）。結果のモードの一覧だけが OS へ来ます。
- `edid.h:85` のクロックは 16 ビット × 10 kHz で、最大 655.35 MHz です。
- 比べた算術（可視の画素だけ）：
  - 2560×1440×280 = 1032 MHz で、DTD に入りません。
  - 1920×1080×600 = 1244 MHz で、DTD に入りません。
  - 2560×1440×165 = 608 MHz で、可視の分では入ります。帰線の分を足して入るかはモニター次第です。
- 起きる条件：自前の `edid.h` の道（firmware の拍、VBE）には 280 Hz と 600 Hz が見えません。実機の nvrm の道は NVKMS の一覧に従います。
- 確信：コードで証明。NvTiming が CTA-861 と DisplayID の全部を正しく読むかは確かめていません。
- 修理：解析の持ち主を一つにし、深さ、色、VRR、HDR を申告へ通します。

### D-09 重い：正本が無く、モニターの変化の多くが誰にも届かない

- 種類：途中で切れた道。
- `kapi_event`（`nv/display.c:1441-1447`）が受けるのは `DPY_CHANGED` と `DYNAMIC_DPY_CONNECTED` だけです（`:1793-1794`）。
- `modes_offer(false)`（`:1359,1460`）の memcmp は、モードと名前しか比べません。
- HDR、VRR、bpc の違いは `offer` に無いので、変化があっても同じに見えます。
- 詳しくは 9 節と 10 節に書きます。
- 起きる条件：EDID のモードと名前を変えない変化のときです。
- 確信：nvrm の側はコードで証明。モニターの OSD の切り替えが HPD や EDID の変化を伴うかはメーカー次第で、確かめられません。
- 修理：13 節の抽象です。

### D-10 重い：ドライバーの設定の置き場が無い

- 種類：呼ぶ者のいない道。
- 設定の入口は全部環境変数です：
  - `NVKMS_OPTIONS`（`ports/nvidia/nvkms_os.c:46-48,50-79,108-172`）。
  - `config_file`（`nvkms_os.c:905-943`）。
  - `NVRM_REGISTRY_DWORDS`（`ports/nvidia/nvrm.c:271-275`）。
  - `NVreg_*`（`ports/nvidia/os.c:1070-1104`）。
  - `UVM_OPTIONS`。
- サービスの管理者の `n_launch`（`user/apps/services.c:363-365`）は環境を渡しません。
- `abi.h:357-363` に欄はあります。`user/libc/sys.c:717-721` の環境は空です。
- だからドライバーの設定は常に既定です。
- `ports/nvidia/*.c`、`*.h` に `n_setting_` はありません。領域も頁もありません。
- 起きる条件：常にです。
- 確信：コードで証明。
- 修理：13 節のとおりドライバーが設定を宣言し、環境の読みを消します。

### D-11 中：画面を抜いても窓と合成はその画面に残る

- 種類：途中で切れた道。
- 順：`rescan`（`nv/display.c:1463-1467`）が `vblank_off` と `head_off` をします。
- 面は渡したままで、デスクトップの部分も残ります（`nv/display.c:36-38`）。
- `screen_modes_offer` の count 0（`screenmode.c:112-119`）は望みを消すだけです。
- `desk_flip` は blank が無いのでその場で shown と言います（`nv/display.c:1183-1198`）。
- `wm.c:1482,1736` は古い `period` で拍を取ります。
- 窓は動きません。
- 起きる条件：実機で target のモニターを抜いたときです。
- 確信：コードで証明。
- 修理：出力の無い画面を off にし、窓を残る画面へ動かします。

### D-12 中：既定は最高の Hz でなく、モニターの preferred

- 種類：決まりの欠け。
- 順（`svc/display.c:122-134`）：利用者の選択 → preferred → 同じ大きさの近い拍 → 最大の面積。
- 最高の拍を選ぶのは preferred が無いときだけです。
- 上流の preferred は `NVT_TYPE_EDID_DTD` の seq 1 で `preferred_timing_is_native` のときです（`nvkms-modepool.c:483-493`）。
- DisplayID の Type VII の preferred の印（`nvt_displayid20.c:677`）は、この判定に入りません（`nvkms-modepool.c:476-493`）。
- D-08 の算術により、280 Hz、600 Hz、1000 Hz は DTD に入りません。だから既定は別の拍になります。
- 一覧に載っていれば、手で選べます。
- 起きる条件：DTD に入らない拍の画面です。
- 確信：順はコードで証明。実物のモニターの EDID は確かめられません。
- 修理：既定の決まりを依頼者が決めます。最高の拍を既定にするなら、サービス `display` の一か所で変わります。

### D-13 中：試しの戻しが黙って何もしない時がある

- 種類：成功を返して何もしない。
- 戻す前の値が firmware のモードで一覧に無いと（拍 0 や丸めの違い）、`svc/display.c:357-363` は黙って 0 を返します。
- すると試しの期限でも何も望みません。
- 条件：保存値が無く、source が DRIVER でないときです。`settingsd.c:758-760` は保存値を先に使います。
- 確信：コードで証明。起きる頻度は確かめられません。
- 修理：戻す値が一覧に無いときは preferred へ戻します。

### D-14 中：適用の確かめが自分の記憶

- 種類：偽物の確かめ。
- `offer.shown = mode_of(&scr.mode)`（`nv/display.c:1421-1422`）は nvrm の記憶です。
- NVKMS からの読み戻しはありません。
- CRC はログだけです（`:1209-1228`）。
- 本物の部分：commit の結果とフリップの完了は見ます（`:761-776,863-889`）。
- 二度目の失敗を nvrm は黙ります（`unshown_said`、`:1409-1416`）。そのコメント（`:1384-1394`）はサービスの動きを理由にしています。
- 確信：コードで証明。
- 修理：実効を NVKMS から読み戻して報告します。

### D-15 中：倍率は保存され、試しが走り、何も変わらない

- 種類：保存するだけで適用しない設定。
- 道：`apps/settings.c:243` → `settingsd` → `svc/display.c:267` → `wm.c:2427` → `wm.c:3124,3229`。
- `scale_percent` を読むプログラムはありません。rg で `user/libc`、`user/include` に語が出ず、`user/apps` は窓とサービスだけです。
- 札は正直です（`words.h:246`「描く大きさはまだ変わりません」）。
- それでも `N_APPLY_TRIAL`（`svc/display.c:27-29`）で 15 秒の試しが走ります。
- 確信：コードで証明。
- 修理：UI と字体の層に画面ごとの倍率を通します。通るまで試しを出しません。

### D-16 中：固定の大きさの表と数

- 種類：決め打ち（指示 4 により、それ自体が所見です）。

| 定数 | 場所 | 超えると |
|---|---|---|
| `N_SCREEN_MODES_MAX 128` | `abi.h:592,599`、`pciuser.c:1004`、`screenmode.c:126`、`nv/display.c:220,454` | 黙って捨てます。preferred が後ろにあれば失います（`nv/display.c:454-458`）。 |
| `DESK_BUFFERS 2`、`SCREEN_BUFFERS_MAX 4` | `abi.h:541`、`nv/display.c` | 緩衝の数を申告から決めません。 |
| `N_SCREEN_POINTER_SIZE 32` | `abi.h:615`、`wm.c:55` | 大きなポインターを出せません。 |
| `N_SCANOUT_*` が三つ | `abi.h:521-523` | 12 ビットと FP16 を言えません。 |
| `lut_r[1024]` と deep の下位 2 ビット | `fb.c:33,41`、`display_abi.h:163-174` | 10 ビットを超えられません。 |
| 単数の `scr`、`offer` | `nv/display.c:132-210,219-220` | D-02 です。 |
| `framebuffers[0]` | `kernel/src/boot.c:40,172-173` | 起動の面は一枚だけです。 |
| `modes[32]`、`modes[8]` | `fb.c:48-69`、`vbe.c:89-110` | 自前の道の一覧が切れます。 |

- 128 は上流の `NVT_MAX_TOTAL_TIMING`（`src/common/modeset/timing/nvtiming.h:197`）と同じ値です。ただし NVKMS は EDID の後に VESA の表も試します（`nvkms-modepool.c:119,126`）。
- 上流の KAPI の `pixelClockHz` は NvU32 です（`nvkms-kapi.h:92`）。中の値は NvU64 です（`src/common/unix/common/inc/nv_mode_timings.h:78`）。
- `nvkms-kapi.c:2485` で黙って切れます。限界は 4.29 GHz です。1080p 1000 Hz は入ります。この限界は NVIDIA の口の物で、KAPI を通る間は残ります。
- 確信：コードで証明。

### D-17 中：同じ型のモニターの身元が接続の順で動く

- 種類：決まりの欠け。
- 同じ身元には番号を後置します（`user/libc/gui.c:238-242`）。
- EDID が無い画面の身元は `#n` です（`user/libc/gui.c:237,242`）。
- nvrm はコネクターの名前を身元にします（`nv/display.c:392,402-403`）。
- 保存は身元ごとです。だから接続の順が変わると、別のモニターの選択を読みます。
- 起きる条件：製造番号の同じモニターが二枚あるか、EDID が無いときです。
- 確信：順はコードで証明。起きる頻度は確かめられません。
- 修理：身元をコネクターの道と EDID の組にします。

### D-18 中：ドライバーの語と OS の語が混ざる

- 種類：層の混ざり。一覧は 8 節にあります。
- 確信：コードで証明。

### D-19 中：試験が非同期の道を試さない

- 種類：試験の偽物。
- `tests/nvrm_display_hosttest.c:1-2` は「screenmode.c as the driver sees it」と言います。今の `screenmode.c` と違います（D-01）。
- QEMU でモードの選択を試すのは `tests/pcitest.c:525-693` の偽のドライバーだけです。
- この部分は grep の範囲だけを読みました。
- 確信：`nvrm_display_hosttest.c` はコードで証明。`pcitest.c` の細部は読んでいません。

### D-20 中：VM の道にモードの選択が無い

- 種類：VM の道。
- `vbe.c:114-178`：モードは固定で、一覧を出しません。XRGB8888 です（`:166`）。
- `virtio_gpu.c`：scanout がありません（頭の 1-7 行。`screen_` の語が出ません）。
- Hyper-V：`kernel/src/vm/hv/vmbus.c:155` に名前だけあり、映像のドライバーはありません。
- `rdp.c:1223-1243` → `wm.c:2476-2531`：大きさは RDP の窓に従います。
- だから QEMU と Hyper-V で画面が映っても、モードの管理の証拠になりません。
- 確信：`vbe.c` はコードで証明。`rdp.c` と `hv/` は grep の範囲です。

### D-22 中：NVKMS が無効としたモードは理由なしで消える

- 種類：途中で切れた道。
- `modes_list` は `!valid` のモードを黙って飛ばします（`nv/display.c:438-442`）。
- `kapi.getDisplayMode` の引数に理由の欄はありません（`nv/display.c:430`）。
- NVKMS は理由を `pInfoString` に書きます。例は「DP Bandwidth check failed」です（`nvkms-modepool.c:1752-1760`）。この文は nvrm に届きません。
- 起きる条件：帯域、DSC、IMP で落ちたモードです。1000 Hz が一覧に無くても、窓にもログにも理由が出ません。
- 確信：コードで証明。
- 修理：申告に、断られたモードと理由の語を載せます。

### D-21 軽い：文の誤りと切り捨て

- VBE では source が DRIVER（`screenmode.c:234`）で count 0 なので、窓は「表示のドライバーがモニターを見ていません」（`words.h:154`）と出します。モニターは映っています。
- `%u.%02u` は mHz を切り捨てます（`apps/settings.c:953`）。`key`（`:956`）が重なることがありえます。
- 古い文：`.claude/rules/kernel.md:22`（「カーネルと表示経路が決める」）、`fb.c:629-635`、`pciuser.c:182` の `fbhold=`。
- 語だけの NVIDIA：`fb.c:403`、`fb.h:38`。
- 確信：コードで証明。

## 2. 答え 1：項目と段の表

判定の語は依頼のとおり、本物、一部、偽物、無し です。「本物」はコードの道があるという意味で、実機で動いたという意味ではありません。

| 項目 | 申告を読む | カーネル | サービス `display` | 設定の窓 | 保存 | ドライバー（実機／VM） | 適用 | 確かめ |
|---|---|---|---|---|---|---|---|---|
| 解像度 | 本物：`nv/display.c:420-462`。firmware は `edid.h:74-106`、`fb.c:48-69` | 本物（運ぶだけ）：`screenmode.c:105-145,177-190` | 本物：`svc/display.c:122-134` | 本物：`apps/settings.c:926-960` | 本物：`settingsd.c:685-693`、`settings_abi.h:124-129` | 実機は本物（出力一つ）：`nv/display.c:1304,1320`。VM は一覧なし：`vbe.c:114-178`、RDP は窓に従う：`rdp.c:1223-1243` | 本物（D-01 の下）：`nv/display.c:761-776` | 一部：完了は見る `:863-889`、読み戻しなし `:1421-1422` |
| Hz（小数を含む） | 本物：mHz を保つ `nv/display.c:408-414`。自前の道は推量 `fb.c:48-69` | 本物：`screenmode.c:97-103`、`wm.c:1659` | 本物：`svc/display.c:131-133` | 一部：表示を切り捨てる `apps/settings.c:953,956` | 本物：`settings_abi.h:124-129` | 実機は本物：同じ面でモード設定（`ports/nvidia/README.md:326` の文と `mode_show`）。VM は無し | 本物（D-01 の下） | 一部：同上 |
| 色の深さ | 無し：`edid.h` は byte 20 を読まない。上流の中だけ `nvkms-dpy.c:3609-3624` | 偽物：`bits_per_channel` は面の写し `abi.h:570-582`、`wm.c:3118-3122` | 一部：形式を身元として比べるだけ `svc/display.c:65-68,127` | 偽物：出ない項目 `apps/settings.c:961-979` | 一部：`format` だけ `settings_abi.h:124-129` | 無し：`nv/display.c:242,554,1106,1113`、`vbe.c:166` | 無し：NVKMS が決める `nvkms-modeset.c:260-340` | 無し |
| 画素の形式 | 無し：上流の中だけ `nvkms-evo.c:2725-2830` | 無し：`N_SCANOUT_*` は面の形式 `abi.h:521-523` | 無し | 無し | 無し | 無し：KAPI に欄なし `nvkms-kapi.h:371-423` | 無し：RGB を先に選ぶ `nvkms-evo.c:2788-2793` | 無し |
| 色域と転送関数 | 無し：`nvkms-api.h:1598` は上流の中だけ | 無し（rg） | 無し（rg） | 無し（rg） | 無し | 無し：固定 `nv/display.c:705,716,735,740` | 無し | 無し |
| VRR | 無し：`nvkms-kapi.h:538` を読まない。`nv/display.c:240-243` | 偽物：読み手だけ `screenmode.c:102,171`、`fb.c:391-396`、`wm.c:1660-1665` | 無し：`svc/display.c:65-68` | 偽物：札だけ `apps/settings.c:954` | 無し：`settings_abi.h:124-129` | 無し：`nv/display.c:697-698`。VM は無し | 無し：`nvkms-kapi.h:395` を立てない | 無し：`nvkms-kapi.h:463` を読まない |
| スケーリング | 無し | 無し：デスクトップは常にモードの大きさ `wm.c:1531-1597` | 無し | 無し | 無し | 一部：直接表示の面だけ `nv/display.c:691-707` | 一部：同上 | 無し |
| 回転 | 無し（rg） | 無し（rg） | 無し（rg） | 無し（rg） | 無し | 無し：`nvkms-kapi.h:417-420` の `rrParams` を使わない | 無し | 無し |
| 配置と主画面 | 実機は一枚：`nv/display.c:324-381` | 本物：`wm.c:1723-1757,2412-2443`、`pciuser.c:356-388`、`fb.c:283-307` | 本物：`svc/display.c:179-276` | 本物：`apps/settings.c:254-287,764-797` | 本物：`settings_abi.h:314,316` | 実機は偽物（一枚）：`nv/display.c:375-377`。VM は二枚目の VGA：`vbe.c:140-153,169` | 実機は無し。VM は本物。配置は合成の中で本物 | 無し |
| 倍率 | 該当なし | 運ぶだけ：`wm.c:354,2427,3124` | 本物：`svc/display.c:267` | 札が正直：`apps/settings.c:243`、`words.h:246` | 本物：`settings_abi.h:315` | 該当なし | 偽物：読むプログラムが無い | 無し |

### 表の続き：OS の設定か、ドライバーの設定か、重なりか

線引きは私の判断で、根拠を書きます。

| 項目 | 分類 | 根拠 |
|---|---|---|
| 解像度 | 重なり（三者） | 利用者の選択は OS の物です。出せるか、timing は何かはドライバーの物です。候補はモニターの申告です。 |
| Hz | 重なり（三者） | 解像度と同じです。 |
| 色の深さ | 重なり | 面の深さは OS と合成器の物です。リンクの bpc はドライバーの物です（`nvkms-evo.c:2725-2830`）。上限は EDID から来ます（`nvkms-dpy.c:3609-3624`）。 |
| 画素の形式 | 重なり | 利用者が選びうる物です。帯域に合わせる判断はドライバーの物です。 |
| 色域と転送関数 | 重なり（三者） | 依頼者の原文が HDR を三者の物と言います。 |
| VRR | 重なり | オンとオフは OS の物です。範囲はモニターの物です。実行はドライバーの物です。 |
| スケーリング | 重なり | 表示エンジンでも合成器でもモニターでもできます。 |
| 回転 | 重なり | 合成器でも表示エンジンでもできます。 |
| 配置と主画面 | OS | デスクトップの座標は合成器の物です。ドライバーは座標を知らずに済みます。 |
| 倍率 | OS | 描く大きさの話です。ドライバーとモニターは関わりません。 |
| `max_output_color_depth`、登録値など | ドライバー | NVKMS と RM の中の値です（`ports/nvidia/nvkms_os.c:73,195-202`）。 |

### 表の続き：三者の伝わり（モニターの列を含む）

| 項目 | OS の変更 → ドライバー、モニター | ドライバーの変更 → OS、モニター | モニターの変更 → ドライバー、OS |
|---|---|---|---|
| 解像度 | 届く：5 節の下り道の後、モードの commit でモニターへ | 断りの形だけ OS へ届く：`screenmode.c:135-140` → `svc/display.c:159-170`。保存値は変わらない。ドライバーの設定の道は無い | 一覧か名前が変われば両方へ届く：`nv/display.c:1455-1471`。変わらなければ届かない：`:1359` |
| Hz | 届く：同上 | 届く：`refresh_from`（`screenmode.c:97-103`）。断りは同上 | 同上 |
| 色の深さ | 届かない：OS の選択が無い | 届かない：NVKMS の bpc を OS は読まない。リンクとしてモニターへは届く | NVKMS へは EDID で届く。OS へは届かない |
| 画素の形式 | 届かない：選択が無い | 届かない：同上 | NVKMS へは届く。OS へは届かない |
| HDR と色 | 届かない：選択が無い | 届かない：`hdrInfoFrame` を使わない | `supportsHDR` は NVKMS で止まる。OSD の切り替えの知らせは確かめられない |
| VRR | 届かない：選択が無い | 届かない：`allowVrr` を立てない | `vrrSupported` は KAPI で止まる（nvrm が読まない） |
| スケーリング | 届かない：選択が無い | 直接表示の面だけで、OS は知らない | 知らせは無い。OSD の動きは確かめられない |
| 回転 | 届かない | 届かない | 届かない |
| 配置と主画面 | ドライバーへは届かない（要らない）。実機の残りの画面は暗い | 該当なし | 抜き差しは配置へ届く：`svc/display.c:179-276` |
| 倍率 | ドライバーへは届かない（要らない）。プログラムへも届かない | 該当なし | 該当なし |

## 3. 答え 2：申告の読み

**読むもの（`common/edid.h`）**

- DTD、標準、確立のタイミングを復号します（`edid_modes`、`edid.h:74-106`）。
- 読んで計算を確かめました。
- DTD の interlaced は飛ばします（`:92`）。
- 確立のタイミングの 1024×768 87i は表に入れません（`:78`）。
- 標準タイミングの 4 つの比は正しいです（`:98-99`）。
- 製造者、製品、製造番号、名前を読みます（`edid_ids`、`:38-58`）。checksum は見ません。

**捨てるもの**

- 拡張の block（`e[126]`）を見ません。
- CTA-861（VIC、YCbCr、HDR、VSDB）を読みません。
- DisplayID を読みません。
- 0xFA、0xF7、0xF8、0xFD、byte 20 を読みません。
- だから VRR の範囲とビット深さは OS に届きません。

**読み手は三つあります**

| 読み手 | 場所 | 範囲 |
|---|---|---|
| firmware の面 | `fb.c:48-69` | `edid_modes` の `modes[32]`。拍は推量です。 |
| VBE | `vbe.c:89-110` | 128 byte だけ、`modes[8]`。 |
| nvrm | `nv/display.c:351,396-397` | `edid_ids` だけ。`edid_valid` を呼びません。2048 byte の緩衝を読みません。 |

**実機の解析**

- 全部 NVKMS の NvTiming です（`nvkms-modepool.c:336-375`。T7 は `:413`）。
- preferred の決め方は `:483-493` です。
- 結果のモードの一覧だけが KAPI を通って nvrm へ来ます。
- 上流が CTA-861 と DisplayID をどこまで正しく読むかは確かめていません。

**誤って読むもの**

- 復号の誤りは見つけていません。
- 誤りに近いのは二つです。DTD のクロックの上限（D-08）と、firmware の道の拍の推量（`fb.c:48-69`）です。

**EDID が無いとき**

- firmware の道は `monitor ""`、拍 0 です（`fb.c:50`、`wm.c:1683-1688`）。
- 身元は `#n` です（`user/libc/gui.c:237,242`）。
- nvrm はコネクターの名前を身元にします（`nv/display.c:392,402-403`）。
- NVKMS は EDID の次に VESA の表を試します（`nvkms-modepool.c:119,126`）。呼ぶ順だけを読みました。
- `isNativeDID` は緩衝を写しません（`nvkms-kapi.c:1537`）。

**壊れているとき**

- firmware の道は無いのと同じに扱います。
- `monitor_of` は検証しないので、NVKMS が不正な緩衝を渡せば偽の身元になります。確信は可能性ありです。

**複数 block のとき**

- OS の読み手は最初の block だけを読みます。
- 実機のモードの一覧には NVKMS の読みが効きます。

## 4. 答え 3：モードの適用

**処理は本当にあります**

1. `modes_list`（`nv/display.c:420-462`）が NVKMS の一覧を出します。
2. `validateDisplayMode`（`:1304`）で確かめます。
3. `checked`（`:779-784,1320`）が commit なしで検査します。
4. 検査と確定の間に `desk_give`（`:1320`）で面を渡します。
5. `commit`（`:761-776`）で確定します。
6. `flip_completed`（`:863-889`）がフリップの完了を見ます。

**リンクの帯域**

- nvrm は何もしません。NVKMS に任せます。
- `nvDPValidateModeEvo`（`nvkms-modepool.c:1752`）、`nvHdmiFrlQueryConfigOneColorFormatAndBpc`（`:1736`）、IMP（`:1775` 付近）があります。
- DSC は `NVKMS_DSC_MODE_DEFAULT`（`nvkms-api.h:418`）です。
- 実機で動くかは確かめられません。

**失敗時**

- 確定の失敗は `desks_regive`（`:1333-1344`）で前の面を渡し直します。
- フリップが `FLIP_TIMEOUT_MS 3000` を超えると `hand_back`（`:1239-1259`）です。
- 次の望みで `take_again`（`:1276-1285`）です。
- 出せないモードは断られ、サービスが次の候補を望みます（`screenmode.c:135-140`、`svc/display.c:159-170`）。
- 起動の時の失敗は D-01 です。

**期限つきの試しと戻し**

- 本物です：`apps/settings.c:203-217,1430-1439`、`SCREEN_MODE_TRIAL_MS`（`display_abi.h:58`）、`settingsd.c:741-809,949-961,1220-1230,1264`。
- 欠陥は D-13 です。

**既定のモード**

- 決めるのはサービス `display` です（`svc/display.c:122-134`）。
- 順は、利用者の選択、preferred、同じ大きさの近い拍、最大の面積です。
- 最高の Hz は preferred が無いときだけです（D-12）。
- nvrm が起きるまでは firmware のモードのままです。
- 起動の記録に画面の値はありません（`kernel/src/fs/bootrecord.c:15-17`）。
- 起動が見るのは `framebuffers[0]` だけです（`kernel/src/boot.c:40,172-173`）。

## 5. 重なりの決まりと今のコードの差（追加の指示 1 の 4、指示 2 の 1）

決まりは「片方を変えるともう片方も変わる」です。今のコードで成り立つのは解像度と Hz だけです。

**OS を変える（解像度、Hz）：変わります**

1. 窓の `apps/settings.c:203-217` が `N_APPLY_TRIAL` で設定を変えます。
2. `settingsd.c:741-809` が試しを持ちます。
3. 見張りでサービス `display` が起きます。
4. `svc/display.c:142-148` が `n_screen_mode_apply` を呼びます。
5. `syscall.c:1291-1297` → `screenmode.c:177-190` が望みを立てます。
6. `publish` → `wm.c:2356-2365` が状態を渡します。
7. nvrm の状態のスレッド（`nv/display.c:1506-1529`）が `mode_follow` → `mode_show` を呼びます。
8. NVKMS の commit です。

**ドライバーを変える（解像度、Hz）：半分だけです**

- 利用者がドライバーの設定を変える道はありません（D-10）。
- ドライバーが別のモードを出した時は `offer.shown` と `done` が返ります（`nv/display.c:1421-1424`）。
- `screenmode.c:135-140` が `refused_serial` を立てます。
- `refresh_from`（`screenmode.c:97-103`）が拍を合成器へ渡します。
- サービスが断りを覚えて次を望みます（`svc/display.c:159-170`）。
- 窓は `refused_serial` で文を出します（`apps/settings.c:911`）。
- 保存値は変わりません。

**どちらが決めるか、食い違ったとき**

| 項目 | 決める者 | 食い違ったとき | 一方の変更の見え方 |
|---|---|---|---|
| 解像度、Hz | OS が望み、ドライバーが出せるかを決めます。 | 断りの道で次の候補へ移ります。保存値は残ります。 | 本物です。 |
| 色の深さ | NVKMS が一人で決めます。OS は 8 と言います。 | 食い違ったままで、誰も気づきません。決まりはありません。 | 見えません。 |
| 画素の形式 | NVKMS が一人で決めます。 | OS に語がありません。決まりはありません。 | 見えません。 |
| HDR と色 | 誰も決めません。 | 決まりはありません。 | 見えません。 |
| VRR | 誰も決めません。`allowVrr` が立たないので入りません。 | OS は固定の拍と言い、実際も固定です。ただし窓の札の道は死んでいます。 | 見えません。 |
| スケーリング | 直接表示の面だけ nvrm が決めます。 | 決まりはありません。 | 見えません。 |
| 回転 | 誰も決めません。 | 決まりはありません。 | 見えません。 |

## 6. 答え 4：深さの道

10 bpc を選ぶ道は始まりません。窓の項目が出ないからです。

| 段 | 今の値 | 場所 | 判定 |
|---|---|---|---|
| 窓の「色の深さ」 | 形式の違う項目があるときだけ出ます | `apps/settings.c:961-979,755-760` | 死んだ道 |
| 保存 | `format` の欄だけです | `settings_abi.h:124-129` | 運ぶだけ |
| サービス | 形式を身元として比べるだけです | `svc/display.c:65-68,127` | 運ぶだけ |
| nvrm の申告 | `8, N_SCANOUT_XRGB8888` | `nv/display.c:242` | 決め打ち |
| `mode_index` | 形式を無視します | `nv/display.c:408-414` | 落とす所 |
| scanout の面 | `NvKmsSurfaceMemoryFormatX8R8G8B8` | `nv/display.c:554,1106,1113` | 決め打ち |
| 合成器の緩衝 | 本物の 10 ビットでは deep を持ちません | `fb.c:73-103,482-488`、`wm.c:1534`、`fb.c:207-210` | 逆 |
| dither | 8 ビットの面の上の模擬だけです。全画面で一つです | `fb.c:199-206,474-477`、`fb.h:15-18` | 一部 |
| 窓の緩衝 | 下位 2 ビットの byte です | `display_abi.h:163-174` | 固定 |
| 報告 | 模擬の間は 10 と言います | `wm.c:3118-3122`、`user/libc/web/css.c:5152`、`user/libc/web/js.c:4810` | 偽り |
| リンク | NVKMS が決めます。OS は読みません | `nvkms-evo.c:2725-2830` | 無し |
| NVKMS の上限 | 既定 10 で、変える道がありません | `ports/nvidia/nvkms_os.c:73,195-202` | 無し |

8 ビットへ落ちる所は三つです。

1. `mode_of` の決め打ち（`nv/display.c:242`）です。
2. `surface_of` の決め打ち（`nv/display.c:554`）です。
3. 10 ビットの面が来ても、deep が無いので 8 ビットを複製する所です（`fb.c:207-210`）。

合成器の中の詳しい動きは別の担当が見ます。

## 7. 答え 5：動的な変化

| 変化 | 動き | 判定 |
|---|---|---|
| モード変更（大きさが変わる） | `pci_user_give_screen`（`pciuser.c:891-943`）→ `screen_set_buffers`（`fb.c:333-367`）→ `screen_changed`（`wm.c:1780`）→ `follow`（`wm.c:1648-1696`）→ `wscreen_resize`（`wm.c:1531-1597`）→ `layout`、`windows_keep_in`、`windows_refit`（`wm.c:1500-1510` の EV_RESIZE）。 | 本物です。 |
| モード変更のメモリー切れ | 古い大きさのままです（`wm.c:1675-1676`）。 | 本物です。 |
| モード変更（拍だけ） | 同じ面でモード設定をします。拍は `refresh_from` で渡ります。 | 本物です。 |
| モニターを挿す | `choose_display` → `modes_offer(true)` → 状態のスレッド → `mode_show`（`vblank_on`、`nv/display.c:1327-1331`）。 | 本物です。`mode_follow_now` は早すぎて空振りしますが、状態のスレッド（`:1506-1529`）が拾います。 |
| モニターを抜く | D-11 です。 | 窓が残ります。 |
| 抜いた後のスレッド | 合成のスレッドは古い拍で回ります（`wm.c:1482,1736`）。 | 中身のない仕事です。 |
| 二枚目のモニター | 映りません（D-02）。 | 無し。 |
| 手放しと取り戻し | `hand_back`、`take_again`（`nv/display.c:1239-1259,1276-1285`）。 | 本物です。 |

## 8. 混ざっている所（追加の指示 1 の 2）とドライバーの設定の置き場（同 3）

**混ざっている所**

| 所 | 混ざり | 場所 |
|---|---|---|
| 面の形式がモードの身元 | `format`、`bits_per_channel` がモードの身元と保存と窓の項目に入ります。 | `abi.h:570-582`、`settings_abi.h:124-129`、`apps/settings.c:961-979` |
| ドライバーの流れが ABI と窓に | `SCREEN_SOURCE_HANDED_BACK` とその文です。 | `display_abi.h:62-65`、`words.h:155` |
| OS の方針がドライバーに | どの画面を出すか、モードの絞り、断りを黙ること。 | `nv/display.c:324-381,438-442,1409-1416` |
| ドライバーの語が OS の身元に | コネクターの名前が保存の鍵になります。 | `nv/display.c:392,402-403` → `n_setting_device` |
| nvrm の像だけの形式 | `N_SCANOUT_XBGR8888` です。 | `abi.h:523`、`nv/display.c:1596,1623` |
| 私的な口 | `NVRM_SCANOUT` です。 | `ports/nvidia/include/nvrm_protocol.h`、`ports/mesa/nocturne_gl.h:60`、`ports/nvidia/libnvrm.c` |
| OS がドライバーを見張る値が画面の頁に | `N_DISPLAY_HOLD_SECONDS` です。 | `settings_abi.h:53`、`user/apps/settings_table.h:28`、`pciuser.c:231-249` |
| 窓の都合が共通の ABI に | `SCREEN_MODE_TRIAL_MS` です。 | `display_abi.h:58` |
| 語だけ | `NVIDIA` の語です。 | `fb.c:403`、`fb.h:38` |

**画面の道で出会った NVIDIA と Mesa の決め打ち（指示 2 の 2）**

- サービスの名は `gpu` で、中身は `.program = "nvrm"`、`.client_image = "nvk"`、`nvidia_display` です（`user/apps/services.c:59,65-67`、`ports/nvidia/nvrm.h:33`）。
- 表示のドライバーの口は、NVIDIA の一つしか無い前提で書かれています（D-02）。
- 全体の棚卸しは `docs/records/audit-gpu-generic-20261102.md` の担当です。

**ドライバーの設定の置き場**

無いです。D-10 に証拠を書きました。実機の nvrm と nvkms の設定は、環境変数の入口だけが残り、環境が空なので常に既定です。

## 9. 答え（追加の指示 3 の 2）：モニターから始まる変更の道

| 変化 | ドライバーで受ける所 | nvrm | サービス `display` | 窓 | 合成器 | 判定 |
|---|---|---|---|---|---|---|
| hotplug（挿す、抜く） | `nvkms-rm.c:1806,1816,2063,2096`、`nvkms-event.c:204,218`。DP は `nvDPNotifyLongPulse` → `newDevice`・`lostDevice`（`nvdp-connector-event-sink.cpp:326-406,415-452`） | `kapi_event`（`nv/display.c:1441-1447`）→ `nvKmsKapiHandleEventQueueChange`（`nvkms-kapi.c:3859-3925`）→ `display_thread`（`:1473-1490`）→ `rescan`（`:1455-1471`） | 届く | 届く | 挿すは届く。抜くは D-11 | 一部 |
| EDID の変化（モードか名前が違う） | 同じ口 | `modes_offer` → `offer_serial` | 届く：`svc/display.c:171-173` | 届く | 届く | 本物 |
| EDID の変化（HDR、VRR、bpc だけ） | 同じ口 | memcmp が同じで止まる：`nv/display.c:1359,1460` | 届かない | 届かない | 届かない | 無し |
| DP の短い通知（IRQ_HPD） | `nvHandleDPIRQEventDeferredWork`（`nvkms-event.c:228`）→ `nvDPNotifyShortPulse` → `ConnectorImpl::notifyShortPulse`（`dp_connectorimpl.cpp:8833`） | 届かない | 届かない | 届かない | 届かない | DP のライブラリーの中で止まる |
| DP リンクの再学習 | 同じライブラリーの中（`dp_connectorimpl.cpp:7064` のコメント）。`notifyMustDisconnect` は空（`nvdp-connector-event-sink.cpp:455-457`）。`bandwidthChangeNotification` は中の更新だけ（`:473-477`） | 届かない | 届かない | 届かない | 届かない | 無し |
| DP の Zombie | 挿抜が変わるときだけ `DPY_CHANGED`（`nvdp-connector-event-sink.cpp:479-517`） | 挿抜の道へ | 同左 | 同左 | 同左 | 一部 |
| HDMI FRL の再学習 | `nvHandleHDMIFRLRetrainEventDeferredWork`（`nvkms-event.c:244`）。`:250` が `DPY_CHANGED` を送り `reassessFrlLinkCaps` を立てる | memcmp が同じで何もしない：`nv/display.c:1359,1460`。モード設定をやり直さない | 届かない | 届かない | 届かない | 無し |
| OSD の HDR やモードの切り替え | HPD や EDID の変化を伴うならその口です。伴うかはメーカー次第で、確かめられません | 伴わなければ受けようが無い | 同左 | 同左 | 同左 | 確かめられない |

## 10. 正本と写し（追加の指示 3 の 4）

三者を取り持つ抽象はありません。サービス `display` は選ぶ者です。実効の値を持ちません。

**正本は誰も持ちません。写しは次のとおりです。**

| 持ち主 | 写し | 場所 |
|---|---|---|
| NVKMS | `pDpyEvo`、`apiHeadState`。本当の値です。 | 上流 |
| nvrm | `scr.mode`、`offer`、`offered`、`kmodes` | `nv/display.c:132-220` |
| カーネルの選択 | `struct choice` | `screenmode.c` |
| カーネルの面 | `struct screen.mode` | `fb.c` |
| カーネルの合成 | `wscreen` の w、h、refresh、vrr、period、state.mode と `sw`、`sh` | `wm.c:1648-1696` |
| カーネルの全体 | `fb.bpc`、`fb.deep`、`fb.simulated` | `fb.h:15-18` |
| サービス | `struct screen` の now、refused、place、scale と `primary` | `svc/display.c` |
| 設定 | 値と試し | `settingsd.c` |
| 窓 | `screen`、`trial_ends_ns` | `apps/settings.c` |

**知らせの流れは三本で、どれも一方向です。**

- 下り：窓 → `settingsd` → `display` → カーネル → nvrm → NVKMS。
- 上り：nvrm → `n_pci_screen_modes`・`n_pci_give_screen` → カーネル → 見張り → `display`・窓・合成器。
- モニター：NVKMS → nvrm → 申し出 → `display` → 下り。

上りに載るのはモード一つの記憶だけです。実効の bpc、形式、VRR、HDR は載りません。

## 11. 依頼者の線の判定（追加の指示 4）

| 問い | 判定 | 根拠 |
|---|---|---|
| 1440p 三枚と 1080p 600 Hz を、それぞれ選んで適用できるか | できません。実機で映るのは一枚です。 | D-02 |
| Hz が画面ごとに違うか | 上の層は違えます（`wm.c:1723-1757`、`wm.c:2246-2270`）。実機は一枚なので試しになりません。 | D-02 |
| VRR を画面ごとに扱えるか | できません。VRR がどの画面でも入りません。 | D-03 |
| OLED と VA で深さを分けられるか | できません。深さは全体で一つです。 | D-04、D-05 |
| 280 Hz と 600 Hz が既定になるか | なりません。preferred が DTD に入らないからです。 | D-08、D-12 |
| 280 Hz と 600 Hz が一覧に載るか | NVKMS の一覧に載れば載ります。実物の EDID は確かめられません。帯域の算術は 11.1 節にあります。 | D-08 |
| 四枚を一つの GPU で点けられるか | 確かめられません。NVIDIA の公式の頁は RTX 4070 を up to 4 と書きますが、この四枚の組は書いていません。2 頭 1 OR で頭を二つ食う画面があると、残りの頭が減ります（`nvkms-modeset.c:781-783`）。 | 11.1 節 |

**拍と大きさで計算している本物の部分**

- `period_ns`（`wm.c:1659`）は申告から計算します。
- `refresh_mhz` は uint32 で、600000 が入ります。
- `SCHED_PERIOD_MIN_NS 125000`（`kernel/src/sys/sched.h:57`）は 600 Hz の 1.667 ms より短いので縛りません。
- デスクトップのスレッドの期限は一番速い画面に従います（`wm.c:2246-2270`）。
- `UINT16_MAX`（`fb.c:109`、`pciuser.c:1006`、`nv/display.c:439`、`wm.c:2411`）は KAPI の NvU16 の欄（`nv/display.c:700-704`）から来ます。
- 表示の道に Hz の決め打ちは見つけていません（`wm.c`、`screenmode.c`、`fb.c`、`svc/display.c`、`apps/settings.c` を rg で探しました）。

**固定の表**は D-16 です。

## 11.1 依頼者の線：1000 Hz の画面（追加の指示 5）

対象は LG UltraGear 25G590B です。仕様は依頼者の言葉のとおりに使います。この画面の EDID の中身は手元にありません。

**判定**

| 依頼者の仕様 | 今のコード | 根拠 |
|---|---|---|
| 1920×1080 | 通ります。 | 2 節の表 |
| 1000 Hz | OS の層に縛る定数はありません。NVKMS が有効と言えば一覧に載り、窓で選べます。有効と言うかは確かめられません。 | 下の表 |
| 既定 | 1000 Hz にはなりません。preferred が DTD に入らないからです。preferred が無い画面だけは最高の拍を選びます。 | D-12 |
| 適用 | 起動の競争の下にあります。 | D-01 |
| DP 2.1 UHBR20 | RTX 4070 は DP 1.4a です。OS はリンクの速さを見ません。 | 下の節、D-06 |
| HDR10 | 入りません。 | D-07 |
| G-SYNC Compatible、FreeSync Premium | 入りません。固定の拍で出ます。 | D-03 |
| 一覧に無いときの理由 | 出ません。 | D-22 |

1 ms ごとの合成が間に合うかは別の担当の範囲で、ここでは見ていません。

**RTX 4070 と DP 2.1 UHBR**

- 作った者の文書で確かめました：NVIDIA の RTX 4070 の頁（`https://www.nvidia.com/en-us/geforce/graphics-cards/40-series/rtx-4070-family/`）の接続の脚注は「DisplayPort 1.4a」です。
- 同じ頁の最大は「4K at 240Hz or 8K at 60Hz with DSC, HDR」です。
- 同じ頁の画面の数は up to 4 で、脚注は DSC の 4K 240 Hz を 2 台と書きます。
- だから RTX 4070 は UHBR を出しません。リンクの上限は HBR3 です。
- 頁は取得の道具を通して読みました。頁に 1080p 1000 Hz は書いてありません。
- ソースからは確かめられません。`dpVersionsSupported` は RM の問い合わせから来ます（`dp_evoadapter.cpp:314`）。
- `dispcmnCtrlCmdDpGetCaps_IMPL` は `src/nvidia/generated/` に名前があるだけで、本体は `src/nvidia` に見つかりません。
- 25G590B を HBR3 につないだ時に 1000 Hz を申告するかは確かめられません。

**帯域の算術**

NVIDIA の定数（`src/common/displayport/inc/dp_linkconfig.h:225,231`）を使います。HBR3 は 1 レーン 810 MB/s で、4 レーンは 3240 MB/s です。UHBR20 は 1 レーン 2424 MB/s で、4 レーンは 9697 MB/s です。可視の画素だけの計算で、帰線と FEC を含みません。

| モード | MP/s | 24 bpp | 30 bpp | HBR3×4、DSC なし | UHBR20×4、DSC なし |
|---|---|---|---|---|---|
| 1920×1080×1000 | 2073.6 | 6221 MB/s | 7776 MB/s | 入りません | 入ります |
| 1920×1080×600 | 1244.2 | 3732 MB/s | 4666 MB/s | 入りません | 入ります |
| 2560×1440×280 | 1032.2 | 3097 MB/s | 3871 MB/s | 24 bpp だけ入ります | 入ります |
| 2560×1440×165 | 608.3 | 1825 MB/s | 2281 MB/s | 入ります | 入ります |

- HBR3×4 で 1080p 1000 Hz に使えるのは平均 12.5 bpp までです。DSC が要ります。
- HBR3×4 の DSC なしの 1080p は、24 bpp で約 520 Hz までです。
- 公称の 4K 240 Hz と 8K 60 Hz は、どちらも 1990.7 MP/s です。2073.6 MP/s はこれを 4.2% 超えます。
- この値が上限だとは公式の頁に書いてありません。だから RTX 4070 で出るかは確かめられません。

**値の型と範囲**

| 段 | 型と範囲 | 1000 Hz では | 判定 | 場所 |
|---|---|---|---|---|
| EDID の DTD | 16 ビット × 10 kHz、655.35 MHz まで | 入りません | 書式の限界 | `edid.h:85` |
| EDID の標準タイミング | 60 から 123 Hz | 入りません | 書式の限界 | `edid.h:100` |
| OS の自前の読み | base block だけ | firmware と VBE の道に見えません | 欠け（D-08） | `fb.c:48-69`、`vbe.c:89-110` |
| DisplayID の Type VII（上流） | 3 byte × 1 kHz、約 16.8 GHz まで | 入ります | 読みがある | `nvt_displayid20.c:1596-1598` |
| CTA-861 の中の Type VII（上流） | 同じ形を受けます | 入ります | 読みがある | `nvt_edidext_861.c:2071-2083` |
| 上流の拍 | `rr` は NvU16、`rrx1k` は NvU32 | 入ります | 本物 | `nvtiming.h:125-126`、`nvt_displayid20.c:1681,1685` |
| 上流の画素クロック | `pclk` は 10 kHz の NvU32、`pixelClockHz` は NvU64 | 入ります | 本物 | `nvtiming.h:189-190`、`nv_mode_timings_utils.c:81` |
| KAPI | `refreshRate`、`pixelClockHz` は NvU32 | 入ります | 4.29 GHz の限界（D-16） | `nvkms-kapi.h:91-92`、`nvkms-kapi.c:2484-2485` |
| nvrm の絞り | 拍の上限はありません | 通ります | 本物 | `nv/display.c:438-442` |
| `n_screen_mode` と保存 | `refresh_mhz` は uint32、約 429 万 Hz まで | 1000000 が入ります | 本物 | `abi.h:578`、`settings_abi.h:126` |
| カーネルの確かめ | 0 でないこと、一覧にあること | 通ります | 本物 | `pciuser.c:1007`、`screenmode.c:185` |
| 周期 | uint64 で計算します | 1000000 ns | 本物 | `wm.c:1659` |
| スケジューラー | 125 us まで | 1 ms は範囲の中です | 本物 | `kernel/src/sys/sched.h:57`、`wm.c:1694,2269` |
| 窓 | `%u.%02u Hz`、`line[96]` | 「1000.00 Hz」と出ます | 本物 | `apps/settings.c:897,953` |
| 窓の並び | 申告の順で、拍の順に並べません | 一覧のどこかに出ます | 軽い | `apps/settings.c:946-958` |

**nvkms の道**

1. `kapi.getDisplayMode`（`nv/display.c:430`）が `NVKMS_IOCTL_VALIDATE_MODE_INDEX`（`nvkms-kapi.c:2558`）を呼びます。
2. `ValidateModeIndexEdid`（`nvkms-modepool.c:337`）が NvTiming の一覧を順に調べます。
3. `nvConstructHwModeTimingsEvo`（`nvkms-modepool.c:1707`）が timing を作ります。
4. `nvEvoUse2Heads1OR`（`nvkms-modepool.c:1724`）が頭の数を決めます。
5. `nvDPValidateModeEvo`（`nvkms-modepool.c:1752`、`nvkms-evo.c:7035`）が帯域を確かめます。落ちれば bpc と形式を下げて試し直します（`nvkms-evo.c:7066`）。
6. IMP の確かめ（`nvkms-modepool.c:1775-1778`）を通します。
7. 結果の `valid` だけが nvrm へ来ます。

- 読んだのは呼ぶ順までです。DP ライブラリーの帯域の計算の本文は読んでいません。
- DSC：KAPI は `dscMode` を立てないので `NVKMS_DSC_MODE_DEFAULT`（`nvkms-api.h:418`）です。DP の道では `DSC_DEFAULT` になります（`nvdp-connector.cpp:486-487,503-504`）。
- 要るかを決めるのは DP ライブラリーです。OS は選べず、見えません。
- 可能性あり：Type VII の DSC pass-through の印は `flags.dscPassThrough` になります（`nvkms-modepool.c:413-415`）。KAPI の timing の flags に対応する欄はありません（`nvkms-kapi.h:103-112`）。この印のモードが KAPI の確かめを通るかは確かめていません。

**頭の上限**

- 画素クロックが頭の上限を超えると 2 頭 1 OR を使います（`nvkms-evo.c:9851`）。
- 頭の上限はレジスターの 8 ビット × 10 MHz です（`nvkms-evo3.c:5257-5259`、`src/common/sdk/nvidia/inc/class/clc373.h:340`）。この書式で 2550 MHz までです。
- 初期値は 0x85 で 1330 MHz です（`clc373.h:341`）。AD104 の実値はソースから確かめられません。
- 2 頭 1 OR は DSC か YUV420 の packer を要します（`nvkms-evo.c:9831-9832`）。timing の偶奇の条件もあります（`:9840-9845`）。
- 頭の組は 0 と 1、2 と 3 だけです（`nvkms-modeset.c:781-783`）。
- KAPI の `overrides` は `NO_RRX1K_CHECK` だけで、2 頭 1 OR を禁じません（`nvkms-kapi.c:2529`）。
- nvrm は一番低い頭を選びます（`nv/display.c:361`）。一枚なら組の頭が空いています。
- D-02 を直す時は、画面ごとに頭を数で決めず、NVKMS の割り当てに従う必要があります。
- 公式の頁の「DSC の 4K 240 Hz を 2 台」は頭の組と合います。これは私の読みで、頁に理由は書いてありません。

**UHBR を出せる GPU のとき**

- `ports/nvidia` に `UHBR` の語はありません（rg）。nvrm は UHBR を止めません。
- DP 2.x の道は `dp_connectorimpl.cpp:9488-9491` が RM の申告で選びます。
- `dp_connectorimpl2x.cpp`、`dp_configcaps2x.cpp`、`dp_evoadapter2x.cpp` は組みに入ります（`third_party/nvidia-open/src/nvidia-modeset/srcs.mk:102,104,111`、`ports/nvidia/nvkms.mk:27`）。
- 実機で動くかは確かめられません。

**抽象への入れ方**

- 1 頭に収まらない時の頭の消費は、今の OS の記録にありません。
- 13 節の申告の「リンクの限界」に、モードごとの頭の数と DSC の要否を入れます。NVIDIA の語は入れません。

## 12. 答え 6：偽物の一覧と、Windows 級との欠け

**偽物の一覧**

| 形 | 物 | 所見 |
|---|---|---|
| 見せるだけの UI | 窓の「色の深さ」 | D-04 |
| 見せるだけの UI | 窓の VRR の札 | D-03 |
| 読み手だけ | `N_SCREEN_MODE_VRR`、`vrr_min_mhz` | D-03 |
| 決め打ち | `8, N_SCANOUT_XRGB8888`、`X8R8G8B8`、flags 0 | D-03、D-04 |
| 保存するだけ | 倍率 | D-15 |
| 途中で切れた道 | NVKMS の HDR、VRR、bpc | D-06、D-07、D-09 |
| 呼ぶ者のいない道 | `NVKMS_OPTIONS` と登録値 | D-10 |
| コメントと食い違う | `display_abi.h:7-8,97-99`、`nv/display.c:1816`、`ports/nvidia/README.md:321` | D-01、D-05 |
| 報告の偽り | `bits_per_channel=10`、CSS の `color` | D-05 |
| 確かめの偽物 | `offer.shown` | D-14 |
| 試験の偽物 | `tests/nvrm_display_hosttest.c:332-352` | D-01、D-19 |
| 実機にない道 | 二枚目の画面は VM の VGA だけ | D-02 |
| 成功で何もしない | 試しの戻し | D-13 |

**Windows 級との欠け**

Windows のソースと文書は読んでいません。比べる先は一般に知られている機能です。

- 複数のモニターへ実際に出すこと。
- 画面ごとの VRR のオンとオフ。
- HDR のオンとオフ、輝度の校正。
- 色の深さと画素の形式の選択。
- 色のプロファイル。
- 回転。
- 効果のある画面ごとの倍率。
- 複製の表示。
- モニターの電源を切ること（`bActive` は常に TRUE、`nv/display.c:712`）。
- 抜いた時に窓を残る画面へ移すこと。
- モニターの組ごとの配置の記憶。
- ドライバーの設定の頁。

## 13. 修理の案（追加の指示 1 の 5、同 3 の 5）

**抽象：サービス `display` を持ち主にした三つの記録**

| 記録 | 中身 | 作る者 |
|---|---|---|
| 申告 | モード、モードごとの bpc と形式と色の範囲、VRR の範囲、HDR、リンクの限界。可変長で、世代を持ちます。 | ドライバーがモニターと自分の限界から作ります。 |
| 望み | 利用者と OS の選択。 | サービス `display` が持ち、`settingsd` が保存します。 |
| 実効 | 今のモード、リンクの bpc、形式、VRR、HDR。世代を持ちます。 | ドライバーが読み戻します。 |

**責務**

- サービス `display` は、申告と望みを照らして次の望みを決めます。
- 実効を見張る者へ知らせます。

**三者との口**

- ドライバーとの口は共通の ABI です。ドライバーは申告と実効を報告し、望みを受けて test と commit をします。NVIDIA の語を入れません。
- OS（窓）との口は、望みを変える口と、三つの記録を読む口です。
- モニターとの口はドライバーの中です。ドライバーが HPD、EDID、リンクの事象を受けて、申告か実効を更新します。

**伝わる順序（三者のどれから始まっても同じ）**

1. 変更が起きます。
2. ドライバーが申告か実効を更新し、世代が進みます。
3. サービスが望みと照らして決めます。
4. ドライバーが test と commit をします。
5. ドライバーが実効を報告します。
6. 見張りへ知らせます。
7. 合成器が画面ごとの形式と拍に従います。

**誰の持ち物でないか**

- カーネルは運ぶだけです。意味を持ちません。
- `settingsd` は保存と試しだけです。意味を持ちません。
- 窓は窓だけです。
- ドライバーは選びません（`choose_display` の方針を消します）。

**OS の設定とドライバーの設定を分ける形**

- OS の設定（解像度、Hz、深さ、形式、HDR、VRR、配置、倍率）は、望みの記録に入ります。
- ドライバーの設定はドライバーが宣言します（`n_setting_owner_open`）。設定の窓の別の頁に出します。
- 環境変数の読みを消します。
- 重なるもの（`max_output_color_depth` など）は、申告の上限として OS へ見せます。ドライバーの設定を変えると申告が変わり、望みが照らし直されます。

**依頼者に決めてもらう点**

- モニターの OSD で HDR を切り替えた時に、保存された望みも変えるかです。
- 決まりの「一つを変えると他も変わる」によれば、今の値は変わります。
- 一覧が一時的に変わった時（抜き差し）まで保存値を消すと、利用者の選択を失います。
- 私の案は、抜き差しでは保存値を保ち、利用者がモニターで切り替えたと分かる変化では保存値も変えることです。

**個別の修理**は D-01 から D-22 の各項に書きました。

## 14. 読んで本物と確かめた部分

- `edid_modes` の復号（`edid.h:74-106`）。
- `screenmode.c` の運び（`:105-190`）。
- サービス `display` の選択、断り、配置（`svc/display.c:122-174,179-276`）。
- `settingsd` の試し（`settingsd.c:741-809,949-961,1220-1230`）。
- モードの check、give、commit、flip の順（`nv/display.c:761-889,1304-1344`）。
- 挿し直し（`nv/display.c:1455-1471,1506-1529`）。
- `wscreen_resize` の下流（`wm.c:1531-1597,1648-1696`）。
- mHz の保持（`nv/display.c:408-414`）。
- 拍の計算（`wm.c:1659`）。

ここで本物は、コードの道があり、読んで矛盾が無いという意味です。実行は確かめていません。

## 15. 読めなかった範囲

- 実行の証拠は全部ありません。OS を動かしていません。
- 仕様の文書（DP、CTA、EDID、HDMI、DisplayID）は手元にありません。
- `tests/pcitest.c`、`rdp.c`、`hv/` は grep の範囲だけです。
- `settingsd.c` と `wm.c` の残り（`frame`、`direct_update`）、`shell.c` は読んでいません。
- 上流の DP ライブラリーの本文と、EDID が無いときのモードの出所は、呼ぶ順の外を読んでいません。
- NvTiming の CTA-861 と DisplayID の読みの正しさは確かめていません。
- 実機の頭の数と `HEAD_CLK_CAP_PCLK_MAX` の実値は確かめられません。
- RTX 4070 の DP の版は NVIDIA の公式の頁で確かめました。1080p 1000 Hz が出るかは確かめられません。
- 25G590B の EDID と DSC の能力は確かめられません。
- モニターの OSD の切り替えの動きは確かめられません。
- `settingsd.c:577` は `N_SCOPE_DEVICE` を `N_CLASS_ADMIN` 以上に限ります。一行だけ読みました。普通の利用者が画面のモードを変えられるかは確かめていません。
- `boot/limine.conf` のコメントの真偽は確かめていません。
