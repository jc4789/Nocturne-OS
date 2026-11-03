# 設定の窓とサービスの見直し：画像 20 枚とコードの照合

## 依頼

ユーザーの言葉（原文）：

> settings.c is still realy bad and our serves seem to be for nothing more then theater. infact most of the os seems to only be for theater. our settings menu, is just wrong. windows has better ui then we do by a massive margin and windows has bad ui. the settings menu is a glorified text editor with random hardcoded choices and zero forethought as to what its purpose is. iommu shoud never be a user setting. that is just dangrous. I have attached ALOT of screenshot to help you get a better understanding of what is normal. and how much we are doing wrong.

> the brower stuff is fine(i need to delete a lot of bad code before its touched again as its architechue is fundumently wrong) . go over the pics to check your under standing as a whole of everything.

## 範囲と確かめ方

- 確かめたのは画像 20 枚とソースだけ。この仕事のために OS は動かしていない。
- 我々の窓の画像は Hyper-V の拡張セッションのもの。画像があるのはアプリ、サービス、起動、言語と地域、時刻、網、画面の 7 頁。
- 音、窓、入力、電源、利用者の頁は画像が無く、コードだけを読んだ。
- NVIDIA のドライバーが動く実機の画面の頁は見ていない。`user/apps/settings.c:908` の `listed` が真なら、解像度と拍の一覧が出る。
- サービスのうち中身まで読んだのは `user/apps/services.c` の宣言の表と止める道、`user/apps/settings.c` の札。ほかのサービスは先頭の説明と宣言だけを読んだ。
- ブラウザーは対象の外。音の報告 `docs/records/audit-audio-20261102.md` の A1〜A12、A14、A16 の修理は予定に入れない。

## 根

窓の形は、保存の表の型から機械的に決まる。`user/apps/settings.c:541` の `setting_card` は宣言の型で分ける。

| 型 | 部品 |
|---|---|
| `N_SETTING_BOOL` | 切替 |
| `N_SETTING_ENUM` | 横に並ぶ選択 |
| `N_SETTING_INT`、範囲が 1000 以下 | 滑り棒 |
| `N_SETTING_INT`、範囲が 1000 より大きい | 文字の欄 |
| `N_SETTING_STRING`、`N_SETTING_ENDPOINT` | 文字の欄 |

サービス `settings` の説明は「領域の意味を持たない」。`user/apps/settingsd.c:1` に書いてある。

この規則から画像の形がそのまま出る。

- 時間帯は -720〜840 で幅が 1560 なので、文字の欄になる。
- 倍率は 100〜`INT32_MAX` なので、文字の欄になる。
- ドライバーが画面を握る秒数は 0〜86400 なので、文字の欄になる。
- TCP の容量は 1〜`INT32_MAX` なので、文字の欄になる。

頁は領域の番号で分かれる（`user/apps/settings.c:34` の `pages[]`）。人が持っている物（このモニター、このスピーカー、この網の装置）からは分かれていない。例外は札を手で書いた画面、音、網、入力、サービス、利用者、アプリ。

Windows の窓は、機械につながっている物と人の用事から頁ができている。モニターは名前で並び、解像度はそのモニターの一覧から選び、音の出口は装置の名前で並ぶ。

項目を足しても、この形のままなら長くなるだけ。

## 試験の旗がそのまま設定になっている

試験のカーネルが読む語と、利用者の窓の項目が同じ値を動かす。

| 試験の語 | 利用者の窓の項目 | 場所 |
|---|---|---|
| `noiommu` | IOMMU | `kernel/src/fs/bootrecord.c:63` |
| `noaml` | ACPI の AML | 同上 |
| `nosmp` | 全部の CPU | 同上 |
| `noresume` | 休止からの復帰 | `kernel/src/fs/bootrecord.c:64` |
| `appdrivers-unisolated` | 隔離できない装置のドライバー | 同上 |
| `tcp-rx-bytes` | TCP の受信の容量 | `kernel/src/sys/settings.c:130` |
| `tcp-tx-bytes` | TCP の送信の容量 | `kernel/src/sys/settings.c:131` |
| `fbhold` | ドライバーが画面を握る秒数 | `kernel/src/sys/settings.c:132` |
| `fbsim10` | `window.smooth-gradients`（8 bpc の画面へ誤差拡散で出す 10 bpc の試し。`kernel/src/gui/wm.c:1808`） | `kernel/src/sys/settings.c:140` |

どちらが先にできたかは、git を読んでいないので未確認。

### 起動の頁の危うさ

- `kernel/src/dev/iommu.c:826` のコメントは「No automatic loss of the security boundary」と言う。同じ境界を、窓の切替が外せる。
- IOMMU を切ると、DMA の訳がなくなる（`kernel/src/dev/iommu.c:132`）。
- IOMMU を切ると、アプリのドライバーは `isolation is required` で断られる（`kernel/src/dev/iommu.c:878`）。今の GPU の道は `nvrm` なので、GPU のドライバーが動かなくなる。
- 「隔離できない装置のドライバー」も入れると、この断りが無くなる（`kernel/src/dev/iommu.c:876`）。アプリのドライバーが、訳のない DMA を使える。
- 起動の項目は一つで（`boot/limine.conf`）、製品のカーネルは起動の引数を読まない（`test_flag` は `NOCTURNE_TEST_FLAGS` の中だけ）。
- `kernel/src/fs/bootrecord.c`、`kernel/src/init.c`、`user/apps/session.c`、`user/apps/settings.c` に、安全な起動や回復の語は無い。ACPI の AML か全部の CPU を切って起動しなくなれば、OS の中から戻す道は無い。実際に起動しなくなるかは試していない。
- 窓の下の文は、どの頁でも「変更はすぐに効き、再起動の後も残ります。」（`user/apps/settings.c` の `show`、`W_APPLIED`）。起動の頁の札は「次の起動から効きます」と言い、下の文と食い違う。

## 頁ごとの照合

### 画面

普通の形（画像）：
- モニターは名前で出る（Acer X27U Z1、HP X27q、Acer KG271U X）。
- 配置は二次元のドラッグで、縦置きのモニターも同じ図に載る。「識別」がある。
- 解像度は一覧で、推奨の印がある。一覧には 1920×1440 や 1600×1200 のように、モニターの形と違うものもある。
- OS の窓：夜間モード、カラープロファイル、HDR、拡大縮小（推奨の印）、解像度、向き、複数の画面の使い方。
- ドライバーの窓：コネクタ、解像度、拍（280 Hz）、デスクトップの色の深度（32 ビット）、出力の色の深度（10 bpc）、カラーフォーマット（RGB）、ダイナミックレンジ（フル）、回転、HDCP、デジタルオーディオ、サイズと位置、G-SYNC。
- 解像度と回転は両方の窓にある。これがユーザーの言う重なり。

我々の形：
- 札の値は 1024 × 768。画像の机は幅が約 2560 ある。コードの横の欄は 216 画素で、画像では約 169 画素なので、そこから見積もった。値が違う原因は未確認。札は `screen.offered.shown` を出す（`user/apps/settings.c:906`）。
- 倍率の札は「描く大きさはまだ変わりません」と自分で言う。`scale_percent` を読むのは `user/apps/display.c`、`user/apps/settings.c`、カーネルの `kernel/src/gui/wm.c` だけで、描く側（`user/libc/gui.c`、`user/libc/ui.c`、`user/apps/shell.c`）に読み手は無い。
- 配置は主画面に対する左、右、上、下のボタン（`user/apps/settings.c:788`）。
- `common/display_abi.h`、`user/apps/display.c`、`kernel/src/gui/screenmode.c`、`kernel/src/gui/wm.c`、`user/apps/settings.c` に、回転、HDR、夜間モード、色のプロファイルの語は無い。
- 色の深度の決め打ちと、VRR の作り手が無いことは `docs/records/audit-display-20261102.md` の D-03、D-04。
- 「ドライバーが画面を握る秒数」は、見張りが持ち主のプロセスを殺す上限（`kernel/src/dev/pciuser.c:210`）。人が選ぶものではない。

### 音

普通の形（画像）：
- 出口は装置の名前で並ぶ：HP X27q、スピーカー（Sound BlasterX G6）、KG271U X、X27U Z1、Digital Audio (S/PDIF)。
- モニターの名前が、画面の窓と音の窓の両方に出る。一つの物が二つの持ち主にまたがる。
- 入力の一覧がある（What U Hear、Line In、SPDIF In、External Mic）。
- 音量、モノラル、装置ごとの頁（提供者、ドライバーの日付と版、形式の一覧、テスト、音量、強化、立体音響）。
- 形式はビットと周波数の組の一覧（16、24、32 ビットと 44100〜96000 Hz）。

我々の形（コードのみ。画像なし）：
- 出口の一覧と形式の選択はある。名前は HDA の ELD から取れる（`kernel/src/dev/hda_codec.c:382`）。
- 形式の札は、今動いている出口だけに出る（`user/apps/settings.c:862` の `if (!now) return`）。
- `kernel/src/dev/` の USB の装置は `usbhid`、`usbhub`、`usbmsc`、`usbnet`。USB の音は無い。画像の既定の出口 Sound BlasterX G6 は USB の装置なので、この道が無い。
- `common/audio_abi.h`、`kernel/src/dev/audio.c`、`kernel/src/dev/audio.h`、`user/apps/audio.c` に、録音の語は無い。入力の道が無い。

### 網

普通の形（画像）：
- 読むだけの事実が一つの塊で、コピーのボタンがある：リンク速度、IPv4、IPv6、ゲートウェイ、DNS、製造元、説明、ドライバーの版、MAC。
- 変える所は用事ごとの対話になる：IP の割り当て、DNS（自動か手動、IPv4 と IPv6、HTTPS 経由の DNS、保存と取消）。
- プロファイル（公開か私的か）、従量制。

我々の形（画像）：
- TCP の容量のバイト数が札になっている（1048576）。
- 残りのリース秒数が出る。「借りた住所の残り 85972 秒」。
- DNS は `0.0.0.0` を「DHCP の言うもの」の意味で使う。
- proxy を使っていなくてもポート 8080 の札がある。
- 固定の住所の欄が、DHCP の間も出ている。
- `kernel/src/net/net.c`、`user/apps/network.c`、`common/abi.h` に IPv6 の語は無い。
- `kernel/src/`、`user/apps/`、`common/` の C に Wi-Fi の語は無い。
- 札に速さの行は無い。

### 時刻

普通の形（画像）：
- 名前のある時間帯、夏時間の自動、自動の設定、最後に同期した時刻と「今すぐ同期」、大きな今の時刻。
- 言語、地域、時間帯は別々に選ぶ。画像の機械は日本語、日本、米国東部標準時。

我々の形（画像）：
- 時間帯は分の数を打つ欄で、値は 0。説明に `localtime` と書いてある。
- タスクバーは 04:53 11 月 3 日。Windows の画像は 23:56 11 月 2 日。五時間ずれているので、タスクバーは UTC を出している。
- 最初の利用者を作る窓（`user/apps/session.c:208`）は時間帯を聞かない。
- `user/`、`common/`、`kernel/src/` の C に、時間帯の表も夏時間の語も無い。冬は -300、夏は -240 を手で打つことになる。

### 言語と地域

普通の形（画像）：表示言語の選択、優先する言語の並び替えと追加、国、地域の書式、ようこそ画面と新しい利用者へのコピー。

我々の形（画像）：言語は 2 つ、地域は 3 つで固定。日付の順と時刻と数の書式も横並びの選択。ログインの窓の言語は別の札。

### サービス

普通の形（画像）：
- 設定の窓とは別の管理の道具。
- 列は名前、説明、状態、スタートアップの種類。人が読める説明がある。
- 選んだサービスの操作だけが左に出る。
- タスクマネージャーは、プロセスとその中のサービスを木で並べ、CPU、メモリ、ディスク、網、GPU の使用を出す。

我々の形（画像）：
- 設定の窓の一頁で、内部の名前が並ぶ（`accounts`、`session`、`timesync`）。
- 行は「動いています。起こし直し 0 回。」とボタン二つ。
- `struct n_service_status`（`common/services_abi.h:63`）と宣言の表に、説明の欄もスタートアップの種類も無い。
- 全部の行に同じボタンがある。`tests` は「この媒体にありません」なのにボタンがある。
- `change_instance`（`user/apps/services.c:625`）は管理者の確かめだけで止める。依存や重要さを見ない。`session` や `settings` を止めたときの画面は試していない。
- `user/apps/sysmon.c`（開始メニューの「状況」）はある。`struct n_procinfo`（`common/abi.h:418`）は thread ごとの CPU とメモリーで、ディスク、網、GPU の欄は無い。

### 電源

普通の形（画像）：名前のあるプラン（バランス、高パフォーマンス、省電力）と説明、電源ボタンの動作、画面を切る時間、眠るまでの時間。

我々の形（コードのみ）：`power.performance` は 0〜255 の数（`user/apps/settings_table.h:56`、値はカーネルの持ち物）。ボタン、蓋、放置はサービス `machine` の宣言（`user/apps/machine.c:37`）。一つの領域の持ち主が二つに割れている。「もう一度で閉じない窓を閉じる秒数」のように、内部の段取りの数が札になっている。

### アプリ

我々の形（画像）：拡張子ごとの札が 1 枚ずつ並び、`.aac` や `.mp4` の選択肢にメモ帳がある。プログラムの追加は名前、パス、種類を打つ欄（`user/apps/settings.c:1240` あたり）。

### 全体の並び

普通の形（画像）：検索、上の段と下の段、頁の場所の表示、各項目の下に中身の短い説明。ストレージ、通知、フォーカス、共有、マルチタスク、トラブルシューティング、回復、クリップボードがある。

我々の形（画像とコード）：平らな 12 頁。検索は無い。12 頁のうち 8 頁の印が同じ歯車（`pages[]` の `"settings"`）。

ユーザーは Windows の窓も悪いと言っている。上の「普通の形」は、写す手本ではなく、何が普通かの照らし。

## サービスと OS の「見せかけ」

見せかけを「名乗っている持ち物に、中身が無い」と読む。コードで言えるものだけを書く。

| 名乗り | 実際 | 証拠 |
|---|---|---|
| `display` は倍率の持ち主 | 値を運ぶだけで、描く側に読み手が無い | 上の画面の節 |
| `display` は画面の管理 | モードの選択と配置だけ。回転、HDR、色、深度が無い。一覧の作り手は NVIDIA の道だけ | 上の画面の節、`docs/records/audit-display-20261102.md` |
| `gpu` は GPU のサービス | `program="nvrm"`、`client_image="nvk"`、`nvidia_display` の決め打ち | `user/apps/services.c:65`、`docs/records/audit-gpu-generic-20261102.md` |
| `settings` は設定の持ち主 | 型と範囲つきの保存。値の間の関係を持たない。ある値が別の値を変える重なりを書く場所が無い | `user/apps/settingsd.c:1` |
| `programs` はプログラムの台帳 | C の表と、パスを打つ欄 | `user/apps/programs.c:33`、`user/apps/settings.c:1181` |
| `session` はログインとシェル | 製品の宣言で `tests` に依存する | `user/apps/services.c:84` |
| 時間帯の設定 | 分の数。時間帯の持ち主は無く、読む側が足す | `user/apps/timesync.c:2`、`user/libc/time.c:66` |
| 窓の下の文「すぐに効き」 | 全部の頁に同じ文 | 上の起動の頁の節 |
| 画面の札の値 | 机の大きさと違う | 上の画面の節 |

動いていると観測で言えるもの：

- C: の store は起動する。Hyper-V の記録に `store: C: "Nocturne" on scsi0` がある。
- `network` は住所を借りる。Hyper-V の記録に `network: adapter 1: … DHCP 86400 seconds` がある。
- 窓の管理、ログイン、シェルは動く。画像にある。

`accounts`、`timesync`、`audio`、`machine` は先頭の説明と宣言しか読んでいない。説明が正しいかは未確認。

## 全体の理解

1. 窓は保存の表の写しで、作りの目的（人が機械を使えるようにする）から設計されていない。
2. 項目の多くは、内部の調整の値か試験の旗で、人の用事ではない。
3. 裏に中身が無い項目がある。窓だけを作り直しても、画面の色や回転や時間帯は出せない。
4. 一つの物が複数の持ち主にまたがる。モニターは画面と音に、モードは OS とドライバーとモニター自身にまたがる。今の形には、物を一つ持つ所が無い。
5. 危ない項目と普通の項目が同じ窓の同じ部品に並ぶ。
