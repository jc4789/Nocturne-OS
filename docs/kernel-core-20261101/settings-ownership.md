# 設定の持ち主の表

設定の全部について、普通の OS と driver で誰が持つかと、今の Nocturne で誰が持つかを並べる。
この表は、OS 全体を通す設定の層が扱う scope と持ち主の分け方を決める材料である。
土台は main の 5043e3e6。Nocturne の列は原文で読んだ。
普通の列は下の「出典」の公式の文書で読んだ。公式の文書に無いことは、そう書く。
「普通」は物差しであり、従う義務ではない。普通と違う持ち主を選ぶ行は、理由を書く。

## 言葉

持ち主は、値を効かせる部品である。

| 語 | 意味 |
|---|---|
| 部品 | カーネルか OS のサービスの中で、値を効かせるもの（`gui/wm.c`、`dev/audio.c` など）。 |
| driver | 装置の持ち物の設定を効かせるアプリ（`nvrm`）と、プロセスの中の user mode driver（Mesa の Zink・NVK）。 |
| プログラム | 自分の設定を自分で持つアプリ（ブラウザーなど）。 |

scope は、一つの設定が値をいくつ持つかである。

| scope | 意味 |
|---|---|
| 機械 | 機械に一つ。利用者に依らない。 |
| 利用者 | 利用者ごとに一つ。 |
| 装置 | モニター、音の出口、網の装置ごとに一つ。その装置が繋がっていないときも値は残る。 |
| プログラム | プログラムごとに一つ。無ければ上の scope の値を継ぐ。 |
| 起動 | そのプロセスの起動の間だけ。保存しない。 |

「合うか」の列の語は次のとおり。

| 語 | 意味 |
|---|---|
| 合う | 普通と同じ持ち主で、設定として動く。 |
| 範囲が違う | 持ち主は合うが、scope が足りない。 |
| 違う | 持ち主か道が違う。 |
| 決め打ち | 効く部品はあるが、値が定数で、利用者が変えられない。 |
| 無い | 機能も設定も無い。 |
| 普通に無い | 普通の OS に対応するものが無い、Nocturne だけの設定。 |
| 比べられない | 普通の形を公式の文書で読めなかった。 |

「窓に出す」は、効く道が全部通ってから出す。stub の項目は窓に出さない。

## VRR で普通に分かれる四つ

VRR は一つの設定ではない。公式の文書は次の四つを分ける。

1. モニターの申告。動く範囲は EDID か DisplayID が持つ（[V1]、[V2]）。申告は「できる」であり、「使う」ではない。
2. driver の設定。G-SYNC と G-SYNC Compatible は driver の control panel にある（[N2]）。FreeSync も driver の画面にある（[A1]）。
3. OS の設定。Windows の Variable refresh rate の切り替えは、Graphics Settings の一つの toggle で、既定は切である（[W1]）。効果は、VRR を持たない DX11 の全画面のゲームに VRR を足すことである（[W1]）。この切り替えは G-SYNC・FreeSync・Adaptive-Sync の設定を上書きしない（[W1]）。別に Dynamic refresh rate の切り替えが Advanced display にあり、していることに合わせて周波数を自動で変える（[W2]）。
4. アプリの選択。present の旗で tearing と VRR を選ぶのはアプリである（[W3]）。

OS の切り替えは、[W1] と [W2] のどちらにも画面ごとと書いていない。画面ごとの選択が文書にあるのは driver の G-SYNC の頁である（[N2]）。

## 表

### 画面

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 解像度・周波数 | OS。出力ごと。`SetDisplayConfig` が適用し、`SDC_SAVE_TO_DATABASE` で保存する（[W4]、[W2]）。 | `gui/screenmode.c`。`SCREEN_MODE_SET`、`Displays.conf` の `mode.<monitor>`。鍵は EDID の maker・product・serial（`edid_monitor_id`）。候補は driver が出す。 | 合う | 持ち主はそのまま。scope は装置（モニター）。serial が 0 の同じ型は同じ鍵になる。 |
| 試して戻す | 公式の文書で読めなかった。 | `screenmode.c`。15 秒で戻る（`SCREEN_MODE_TRIAL_MS`）。 | 比べられない | 持ち主はそのまま。層の「書く」が試行を持つ形にする。mode だけの特例にしない。 |
| HDR | OS。画面ごとの切り替え（[W5]）。 | 無い。`ports/nvidia/display.c` は colorimetry を既定に固定する。 | 無い | 持ち主は `screenmode.c`、scope は装置。driver の口と合成の色の道が通るまで窓に出さない。 |
| 拡大（scale） | OS。モニターごとの DPI。変わると窓へ `WM_DPICHANGED`（[W6]）。 | 無い。`ui.h` の `fontSize` などと `wm.c` の `TASKBAR_H` が定数。 | 無い | 持ち主は合成（`wm.c`）、scope は装置。窓とエンジンへ通知で届ける。 |
| 配置 | OS。`SetDisplayConfig` が topology と layout を持つ（[W7]）。 | `wm.c` の `layout()` が番号順に左から並べる。 | 決め打ち | 持ち主は `wm.c`。scope は機械のモニターの組。 |
| 主画面 | OS。Display の頁（[W8]）。主画面の文そのものは読めなかった。 | 画面 0 が主（`wm.c`。タスクバーも画面 0）。 | 決め打ち | 持ち主は `wm.c`。scope は機械のモニターの組。 |
| 回転 | OS。`SetDisplayConfig` の orientation（[W7]）。頁は `ms-settings:screenrotation`（[W8]）。 | 無い。 | 無い | 持ち主は `screenmode.c`、scope は装置。driver の口が要る。 |
| OS の VRR | OS。上の「四つ」の 3。既定は切（[W1]）。Dynamic refresh rate は Advanced display の切り替え（[W2]）。 | 設定が無い。`wm.c` の合成は、mode に `vrr_min_mhz` があれば無条件に VRR で回す。 | 違う | 持ち主は合成（`wm.c`）。scope は装置。既定は切（[W1] の唯一の公式の既定）。切なら固定の周期で回す。Windows の切り替えは一つで、普通と違う持ち主ではなく scope が違う。画面ごとにする理由：歩調を持つのは画面ごとの合成の thread で、規模の例は周波数と VRR の違う 3 枚の画面である。driver の設定を上書きしない（[W1]）。 |
| VRR の申告 | モニターが EDID・DisplayID で（[V1]、[V2]）。driver が読む。 | `n_screen_mode` の `N_SCREEN_MODE_VRR`、`n_screen_modes` の `vrr_min_mhz`。`ports/nvidia/display.c` はどちらも立てない。KAPI には `vrrSupported`（出力）と `vrrEnabled`（入力）がある。 | 無い（実機の driver で VRR の道に入らない） | 持ち主は driver。設定でなく事実。層の窓は「申告があるモニターにだけ VRR を出す」ためにこれを読む。 |
| Night light | OS（`ms-settings:nightlight`、[W8]）。 | 無い。 | 無い | 持ち主は合成、scope は利用者。今の仕事の範囲に入れない。 |
| 滑らかな階調 | 公式の文書で対応物を読めなかった。 | `wm.c`。`SCREEN_SETTINGS_SET`、`Desktop.conf` の `smooth_gradients`。 | 普通に無い | 持ち主はそのまま。scope は利用者。 |

### driver の表示と 3D の設定

普通の形：driver の設定は OS の設定と別の頁で、全体の値とプログラムごとの値を持つ（[N1]、[A2]）。NVIDIA の DRS では、Application Profile が Current Global Profile に勝ち、それが Base Profile に勝つ（[N3]）。プロセスの中の driver は、driver の初期化のときだけ設定を読む（[N3]）。既定に戻す口は全部・profile ごと・設定ごとにある（[N4]）。

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| G-SYNC の入り切り | driver。Display の「Set up G-SYNC」。画面ごと、または同じ型の全部に（[N2]）。検証済みのモニターは driver の導入で自動で入り、未検証は手で入れる（[N5]）。 | 無い。 | 無い | 持ち主は `nvrm`、scope は装置。効く所は KAPI の `vrrEnabled`。 |
| 窓か全画面か | driver。「Set up G-SYNC」（[N2]）。 | 無い。Nocturne にも二つの道がある：合成の面と、全画面の窓の直接表示（`N_SCANOUT_DIRECT`、`NVRM_SCANOUT`）。どちらの道にするかは合成が決める（`wm.c` の `direct_able`）。 | 無い | 持ち主は `nvrm`、scope は装置。合成の面でも VRR を使うか、直接表示のときだけかの二択。 |
| Monitor Technology | driver。全体とプログラムごと（G-SYNC、ULMB、固定）（[N1]）。 | 無い。 | 無い | 持ち主は `nvrm`、scope は全体とプログラム。効く道は、そのプログラムの窓を直接表示するときの flip。 |
| 垂直同期の上書き | driver。全体とプログラムごと（アプリの設定に従う、切、入、Adaptive、Fast）（[N1]）。 | 無い。アプリの選択は `WIN_MAILBOX` の旗だけ。 | 無い | 持ち主は user mode driver、scope は全体とプログラム。既定は「アプリの設定に従う」。 |
| 優先する周波数 | driver。Application-controlled か Highest Available（[N1]）。 | 無い。直接表示はデスクトップの mode のまま出す。 | 無い | 上書きする相手（アプリが mode を変える道）が無いので作らない。窓に出さない。 |
| 最大フレーム レート | driver。全体とプログラムごと。既定は切、20〜1000 fps（[N1]）。 | 無い。 | 無い | 持ち主は user mode driver の present。scope は全体とプログラム。 |
| 低遅延モード | driver。全体とプログラムごと。既定は切。On は行列を 1 コマに（[N1]）。AMD の Anti-Lag も全体とゲームごとで既定は切（[A2]）。 | 無い。 | 無い | 持ち主は user mode driver。scope は全体とプログラム。 |
| トリプル バッファ | driver。OpenGL だけ（[N1]）。 | 面の数はアプリの `SYS_WIN_CREATE`。 | 無い | 持ち主は user mode driver。Mesa の present の道で決める。 |
| 電源管理モード | driver。全体とプログラムごと（[N1]）。 | 無い。 | 無い | 持ち主は `nvrm`。効く道が RM に要る。確かめてから出す。 |
| FreeSync | driver。Display の画面。既定は入。ゲームごとに AMD Optimized・入・切（[A1]）。 | AMD の表示の driver が無い。 | 無い | AMD の driver が来たら、その driver が同じ層へ登録する。層の形は vendor を知らない。 |
| RM・NVKMS の引数 | NVIDIA の公開の driver の module 引数。利用者の設定でない。 | `NVRM_REGISTRY_DWORDS`、`NVKMS_OPTIONS` の起動設定（`ports/nvidia/nvrm.c`・`nvkms_os.c`）。`user/apps/init.c` は `nvrm` を起動設定なしで起こすので、どちらも入らない。 | 違う | 利用者の設定にしない。要るものは `nvrm` の設定として登録し、起動設定の道は消す。消す範囲は二段目で決める。 |

### GPU とゲーム

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| アプリごとの GPU | OS。Graphics settings。プログラムごと。control panel より優先する。頼みであり、アプリが最後に選べる（[W9]）。 | 無い。`ports/mesa/nocturne_gl.c` が、`nvrm` が使えれば GPU、無ければ CPU と決める。 | 無い | 持ち主は OS（値を読むのは GL と Vulkan の初期化）、scope はプログラム。GPU が二つ以上あるときだけ窓に出す。今の下限の機械で意味を持つのは GPU か CPU かの選択。 |
| 窓のゲームの最適化 | OS。全体とプログラムごと（[W10]）。 | アプリが `WIN_FRAME_QUEUE` で選び、全画面は合成が直接表示を決める。設定は無い。 | 無い | 設定にしない。アプリの選択と合成の判断で足りる。入り切りが要る理由が出たら足す。 |
| Game Mode | OS（`ms-settings:gaming-gamemode`、[W8]）。 | 無い。 | 無い | 持ち主は scheduler。中核の設計に任せる。今の仕事の範囲に入れない。 |

### 音

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 音量と消音 | OS。出口の装置ごと（`IAudioEndpointVolume`、[W11]）。 | `dev/audio.c`。`n_audio_output`、`Sound.conf`。全体で一つ。 | 範囲が違う | 持ち主はそのまま。scope を装置へ。 |
| アプリごとの音量 | OS。session ごと（`ISimpleAudioVolume`、[W12]）。頁は Volume mixer（[W8]）。 | 無い。 | 無い | 持ち主は `dev/audio.c`、scope はプログラム。 |
| 既定の出口 | OS。役割ごと（[W13]）。 | `audio.c` が今のカードを使い、遠隔の視聴者があればそちら。 | 無い | 持ち主は `dev/audio.c`、scope は機械。カードが二つ以上あるときだけ窓に出す。 |
| 入力の音 | OS（`ms-settings:sound-defaultinputproperties`、[W8]）。 | 無い。音の録音の道が無い。 | 無い | 機能が来たら同じ層へ。 |
| 出口の形式 | OS（装置の properties、[W8]）。 | `AUDIO_RATE` 48000 の定数。 | 決め打ち | 持ち主は `dev/audio.c`。mix の設計が変わるときに決める。窓に出さない。 |

### 入力

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| ポインターの速さ | OS、利用者（`SPI_SETMOUSESPEED`、[W14]、[W15]）。 | `wm.c`。`Desktop.conf` の `pointer_speed`。 | 合う | 持ち主はそのまま。範囲 -2〜2 を窓の文字列に重ねて書かず、登録から取る。 |
| 加速 | OS、利用者（`SPI_SETMOUSE`、[W15]）。 | `wm.c` の `if (ABS(dx) > 5) dx *= 2`。 | 決め打ち | 持ち主は `wm.c`、scope は利用者。 |
| ダブルクリックの間 | OS、利用者（`SPI_SETDOUBLECLICKTIME`、[W15]、`DoubleClickTime`、[W16]）。 | `wm.c` の `DOUBLE_CLICK_NS` 450 ms。 | 決め打ち | 持ち主は `wm.c`、scope は利用者。アプリのエンジンへ読める値にする。 |
| ホイールの行数 | OS、利用者（`SPI_SETWHEELSCROLLLINES`、[W15]）。 | 無い。アプリが notch を自分で解く。 | 無い | 持ち主は入力、scope は利用者。アプリが読める値にする。 |
| キーの repeat | OS、利用者（`SPI_SETKEYBOARDDELAY`・`SPEED`、[W15]）。 | `dev/usbhid.c` の `REPEAT_DELAY` 500 ms、`REPEAT_RATE` 33 ms。 | 決め打ち | 持ち主は `dev/input.c`。USB の driver が持つのは誤りの場所で、ほかの鍵盤の driver では効かない。scope は利用者。 |
| 配列 | OS。Windows 8 以降は system 全体で切り替える（[W17]）。 | `dev/input.c` の `map` 表。一つ。 | 決め打ち | 持ち主は `dev/input.c`、scope は利用者。 |
| IME | OS（input locale が IME を含む、[W17]）。 | 無い。 | 無い | 持ち主は OS のサービス。この層の外の機能。来たら同じ層へ。 |
| 主ボタン・ポインターの大きさ | OS（`HandPreference`、`CursorSize`、[W16]、`ms-settings:easeofaccess-mousepointer`、[W8]）。 | 無い。 | 無い | 持ち主は `wm.c`、scope は利用者。 |

### 電源

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 電源の計画（AC・DC の値） | OS。値は AC と DC に分かれる。適用は `PowerSetActiveScheme`（[W18]）。 | 無い。 | 無い | 持ち主は `dev/pm.c`。scope は機械。 |
| 画面を消す時間・眠る時間 | OS。計画の値（[W18]）。 | 無い。使っていない時間を見る部品も無い。 | 無い | 持ち主は `dev/pm.c` と合成。部品の設計が要る。 |
| 電源ボタンの動作 | OS（`ms-settings:powersleep`、[W8]。ボタンの頁は文書で確かめていない）。 | `dev/acpi.c` の `power_button` が常に `PM_POWER_OFF`。 | 決め打ち | 持ち主は `dev/pm.c`、scope は機械。 |
| 蓋の動作 | OS（文書で確かめていない）。 | 無い。 | 無い | 持ち主は `dev/pm.c`。蓋の装置が来たら。 |
| 電源の変化の通知 | `RegisterPowerSettingNotification`（[W19]）。 | 無い。 | 無い | 層の通知で届ける。 |

### 時刻と地域

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 時間帯 | OS。機械。権限が要る（`SetDynamicTimeZoneInformation`、[W20]）。 | 無い。`user/include/time.h` は時間帯を持たず、`localtime` は `gmtime` と同じ。 | 無い | 持ち主は `sys/clock.c`（値）と libc（読み手）。scope は機械。アプリから読める口と変化の通知が要る。ISO C の `localtime` もこれを読む。 |
| 時刻の同期 | OS のサービス（W32Time、NTP、[W21]）。 | 無い。`sys/clock.h` に、網の時刻のサービスは参照の一つとなると書いてある。 | 無い | 持ち主は網の時刻のサービス（`clock_wall_proof` へ渡す）。scope は機械。 |
| 手で時刻を変える | OS（`ms-settings:dateandtime`、[W8]）。 | `clock_set_wall` はカーネルの中だけ（呼び手は Hyper-V）。syscall が無い。 | 無い | 持ち主は `sys/clock.c`、scope は機械。 |
| 地域の形式 | OS、利用者（`ms-settings:regionformatting`、[W8]）。 | `user/libc/locale.c` は "C" だけ。タスクバーの時計は `wm.c` の英語の月と曜日の表。 | 決め打ち | 持ち主は OS（利用者）。カーネルの時計の表は消す対象。 |
| 表示の言語 | OS、利用者（`ms-settings:regionlanguage`、[W8]）。 | 無い。`settings.ts` と `wm.c` は英語、`browser_settings.h` は日本語。 | 無い | 持ち主は OS（利用者）。文字列の表の仕組みが要る。層の外の機能。 |

### 網

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| IP の取り方 | OS、装置ごと（`ms-settings:network-ethernet`、[W8]）。 | `net/net.c` が DHCP だけ。 | 決め打ち | 持ち主は `net/net.c`、scope は装置。 |
| DNS | OS、装置ごと（同じ）。 | DHCP か、起動引数 `dns-server=`（`net.c`）。 | 違う | 持ち主は `net.c`、scope は装置。起動引数は消す。AGENTS.md は旗の起動を禁じる。 |
| TCP の緩衝 | 普通は利用者の設定の頁に無い（`ms-settings` の一覧に無い、[W8]）。 | 起動引数 `tcp-rx-bytes=`・`tcp-tx-bytes=`（`net.c`）。 | 違う | 設定にせず、持ち主が決める。起動引数は消す。 |
| proxy | OS（`ms-settings:network-proxy`、[W8]）。 | 無い。 | 無い | 持ち主は網のサービス。ブラウザーのプログラムの設定にしない。 |
| Wi-Fi・VPN | OS（[W8]）。 | 装置の driver が無い。 | 無い | 来たら同じ層へ。 |

### 外観と作業

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 壁紙 | OS、利用者（`ms-settings:personalization-background`、[W8]）。 | `gui/desktop.c` の手続きの絵。一つ。 | 決め打ち | 持ち主は合成（機能が要る）、scope は利用者。 |
| 色（テーマ・アクセント） | OS、利用者。`GetColorValue`、変化は `ColorValuesChanged`（[W16]）。 | 定数が二か所。`user/include/ui.h` の `UI_THEME_COLOURS` と、`wm.c`・`desktop.c` の `RGB()` 直書き。 | 決め打ち | 持ち主は一つにする（外観の部品）。合成とエンジンが読み、変化を聞く。今の二重は消す対象。 |
| 字の大きさ | OS、利用者。`TextScaleFactor`、変化は `TextScaleFactorChanged`（[W16]）。 | `ui.h` の定数。 | 決め打ち | 持ち主は外観の部品。scope は利用者。 |
| 字体 | OS（`ms-settings:fonts`、[W8]）。 | `user/libc/font.c` の決まった探し方。起動設定 `FONT_DIR`。 | 決め打ち | 持ち主は外観の部品。起動設定は残すか二段目で決める。 |
| アニメーション・透明 | OS、利用者（`AnimationsEnabled`、`AdvancedEffectsEnabled`、[W16]）。 | 無い。 | 無い | 持ち主は外観の部品。 |
| タスクバー | OS、利用者（`ms-settings:taskbar`、[W8]）。 | `wm.c` の `TASKBAR_H`、画面 0 に固定。 | 決め打ち | 持ち主は `wm.c`。 |
| 開始の一覧と机の上の絵 | OS、利用者（`ms-settings:personalization-start`、[W8]）。 | `wm.c` の `apps[]`・`desktop_apps[]`。カーネルの表で、アプリの追加で組み直しになる。 | 決め打ち | 持ち主は `wm.c`。一覧はプログラムの登録から作る。登録の仕組みは層の外の機能。 |
| 窓の整列 | OS（`ms-settings:multitasking`、[W8]）。 | 最大化だけ。 | 無い | 機能が来たら同じ層へ。 |
| 通知 | OS（`ms-settings:notifications`、[W8]）。 | `wm.c` の `wm_notify`。設定が無い。 | 決め打ち | 持ち主は `wm.c`。scope は利用者とプログラム。 |
| アクセシビリティ | OS、利用者。変化は `WM_SETTINGCHANGE`（[W14]）。 | 無い。 | 無い | 持ち主は外観と入力の部品。 |

### 起動と利用者

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| 起動の項目と待ち時間 | OS。BCD の store（`/timeout`、`/default`）。管理者の権限（[W22]）。 | `boot/limine.conf`。組むときに決まる。`timeout: 3`、desktop と `nogui`。 | 決め打ち | 持ち主は起動の部品。scope は機械。書き換えの口は Limine の文書で決める。 |
| 起動の旗 | OS。BCD の `/set`（[W23]）。 | カーネルの起動引数：`nogui`、`noiommu`、`nopcid`、`nosmp`、`noaml`、`noresume`、`noappdrivers`、`appdrivers=any`、`appdrivers-unisolated`、`nogpu`、`gpu-enable`、`gpu-slow-test`、`gpu-timing`、`fbsim10`、`fbhold`、`prof`、`prof_ms`、`e1000-itr-us`。 | 普通と同じ場所 | 開発と試験の旗で、利用者の設定でない。層に入れない。利用者の値を混ぜない（`dns-server=` と `tcp-*` は上の網の行）。 |
| 起動のときのアプリ | OS。機械と利用者の Run の鍵（[W24]）。頁は `ms-settings:startupapps`（[W8]）。 | 無い。`user/apps/init.c` は表示の driver の claim と、試験の媒体の `autorun` だけ。 | 無い | 持ち主は init。scope は利用者。システムの中身（`nvrm` の claim）は設定にしない。 |
| 利用者と機械の分け方 | 利用者の好みは `HKEY_CURRENT_USER`、機械の状態は `HKEY_LOCAL_MACHINE`（[W25]）。Apple の preferences は全部利用者ごとで、利用者の間で共有しない（[P1]）。 | 利用者は一人。`store_user_folder` は `C:\Users` の中の一つ。`keep.c` は全部を利用者のフォルダーへ書く。機械の scope の置き場が無い。 | 違う | 層に scope を入れる。時間帯・起動・網・既定の出口は機械の値で、利用者のフォルダーに置かない。 |
| サインイン | OS（`ms-settings:signinoptions`、[W8]）。 | 無い。 | 無い | 層の外の機能。層は利用者の切り替えを前提に scope を分けておく。 |

### プログラムと既定

| 設定 | 普通の持ち主と scope（出典） | 今の Nocturne の持ち主 | 合うか | Nocturne の決定 |
|---|---|---|---|---|
| プログラムの設定 | プログラム。自分の data に保存（`LocalSettings`、[W26]）。Apple も application domain（[P1]）。 | `n_app_data_folder` の `AppData\<name>`。ブラウザーの `webprefs.c`、`agent.c` の `agent.conf`。カーネルも共有の文書も通らない。 | 合う | そのまま。移植したプログラムの設定に手を入れない。層は「使ってよい口」で、強制しない。 |
| 既定のアプリ | OS、利用者（`ms-settings:defaultapps`、[W8]）。 | `user/apps/files.c` の型ごとの決まったアプリ。 | 決め打ち | 持ち主は OS（利用者）。一覧の登録は層の外の機能。 |
| 起動の一時の値 | Apple の argument domain は揮発で、保存値に勝つ（[P1]）。 | `n_settings`（起動設定、`getenv`）。`FOLDER`、`FONT_DIR` など。 | 合う | 別の仕組みのまま。保存しない。「設定」という語が二つを指すので、層の名で区別する。 |
| 許可（カメラ・マイク・アプリの権限） | OS（`ms-settings:privacy-*`、[W8]）。 | 無い。ブラウザーの中だけ。 | 無い | 層の外の機能。来たら scope はプログラム。 |
| 保存先・空き容量の管理 | OS（`ms-settings:savelocations`・`storagepolicies`、[W8]）。 | 無い。ディスクは一台。 | 無い | 今の仕事の範囲に入れない。 |
| 更新 | OS（`ms-settings:windowsupdate`、[W8]）。 | 無い。 | 無い | 今の仕事の範囲に入れない。 |
| 遠隔デスクトップ | OS（`ms-settings:remotedesktop`、[W8]）。 | `kernel/src/vm/rdp.c`（VM だけ）。 | 無い | 実機の機能が無い。 |

## 仕組みの比較

個別の値でなく、設定を支える仕組みを並べる。

| 仕組み | 普通（出典） | 今の Nocturne | 欠け |
|---|---|---|---|
| 保存 | 木の鍵（registry、[W25]）と、プログラムの domain（[P1]）。プログラムごとの container（[W26]）。 | `sys/keep.c`：部品ごとの `AppData\Nocturne\<name>.conf`。カーネルの部品だけが登録できる（`struct keep` はカーネルの中）。`webprefs.c` は同じ仕事を自分で書く。 | アプリの持ち物（`nvrm`）が登録できない。機械の scope が無い。 |
| 読む口 | 名前で引く（`SystemParametersInfo`、`UISettings`、[W14]、[W16]）。DRS は設定の id と取りうる値を列挙できる（[N4]）。 | 部品ごとの syscall（`SYS_SCREEN_INFO` の op、`SYS_AUDIO_OUTPUT`）と固定の構造体（`struct desktop_settings`）。 | 列挙できない。設定を足すたびに ABI の構造体と `js.c` の包みと `ui.d.ts` と `settings.ts` を全部書く。 |
| 検証 | DRS は取りうる値を列挙する（[N4]）。 | 部品の `line` が文字列から確かめる（`keep_number`）。範囲の知識は窓にも重ねて書く（`settings.ts` の -2〜2）。 | 範囲と列挙が持ち主の宣言でない。窓が持ち主の知識を写す。 |
| 既定 | DRS の predefined と restore（[N3]、[N4]）。Apple の registration domain（[P1]）。 | 部品の C の初期値。既定に戻す口が無い（ブラウザーの言語だけ別に持つ）。 | 既定を読めない。戻せない。 |
| 通知 | 全体の一つの知らせ（`WM_SETTINGCHANGE`、[W14]）と、鍵の見張り（`RegNotifyChangeKeyValue`、[W27]）と、値ごとの事象（`UISettings`、[W16]、電源、[W19]）。 | `N_KIND_WATCH` は良い形。開く口が話題ごとに別（`n_screen_watch`、`n_audio_output_watch`、`n_entry_watch`）。どの値が変わったかを言わない。 | 話題を足すたびに ABI の口が増える。 |
| プログラムごとの値 | DRS の三段の優先（[N3]）。AMD の全体と profile の継承（[A2]）。Windows の Graphics settings の custom options（[W10]）。 | 無い。 | 「全体、無ければ既定、プログラムの値があれば勝つ」の解決が無い。プログラムの身元の決め方も無い。 |
| 装置ごとの値 | Windows の persistence database（[W4]）。 | `Displays.conf` だけが自前で持つ（`kept[]`）。音は持たない。 | 共通の形が無い。 |
| 効く時 | 変えた瞬間に効くものと、プロセスの初期化でだけ読むもの（[N3]）が両方ある。 | AGENTS.md の規則は「変えた瞬間に効く」。 | プロセスの初期化でだけ読む user mode driver の扱いが無い。Nocturne は変えた瞬間に効かせるので、Mesa の present が通知を聞く形にする。 |
| 窓 | OS の設定の頁と、driver の頁は別のアプリ（[N1]、[A2]）。第三者の頁の入口（`ms-settings:extras`、[W8]）。 | `settings.ts` が手で頁を書く。ブラウザーは自分の頁（`browser_settings.h`）。 | 持ち主の登録から頁を作れない。driver が頁を足せない。 |

## 公式の文書で分からなかったこと

- 電源ボタンと蓋の Windows の公式の文書は、読めなかった。
- NVIDIA の G-SYNC の既定は、検証済みのモニターは自動で入り（[N5]）、未検証は手で入れる（[N5]）までしか読めない。Monitor Technology の既定の値は読めない。
- 次の文書は頁を開けず、検索の抜粋で読んだ：[W4]、[W5]、[W7]、[W9]、[W11]、[W12]、[W13]、[W15]、[W18]、[W22]、[W23]、[W26]、[A1]、[A2]、[V1]、[V2]。AMD の頁は取得が時間切れになる。
- HDR の適用の API（advanced color）は、`SetDisplayConfig` の頁に無い。別の頁を読んでいない。
- `D:\Programes\OS DEV INFO` に VESA の DisplayPort と EDID の仕様書は無い。VESA の公開の資料（[V1]、[V2]）で読んだ。

## 出典

- [W1] DirectX Developer Blog, OS Variable Refresh Rate: https://devblogs.microsoft.com/directx/os-variable-refresh-rate/
- [W2] Change the refresh rate on your monitor in Windows: https://support.microsoft.com/en-us/windows/hardware/display-graphics/change-the-refresh-rate-on-your-monitor-in-windows
- [W3] Variable refresh rate displays (DXGI): https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/variable-refresh-rate-displays
- [W4] SetDisplayConfig: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig
- [W5] HDR settings in Windows: https://support.microsoft.com/en-us/windows/hdr-settings-in-windows-2d767185-38ec-7fdc-6f97-bbc6c5ef24e6
- [W6] WM_DPICHANGED: https://learn.microsoft.com/en-us/windows/win32/hidpi/wm-dpichanged
- [W7] SetDisplayConfig Summary and Scenarios: https://learn.microsoft.com/en-us/windows-hardware/drivers/display/setdisplayconfig-summary-and-scenarios
- [W8] Launch Windows Settings（ms-settings の一覧）: https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-settings
- [W9] Windows 10 Insider Preview Build 17093（Graphics settings）: https://blogs.windows.com/windows-insider/2018/02/07/announcing-windows-10-insider-preview-build-17093-pc/
- [W10] Optimizations for windowed games in Windows 11: https://support.microsoft.com/en-gb/windows/hardware/display-graphics/optimizations-for-windowed-games-in-windows-11
- [W11] IAudioEndpointVolume: https://learn.microsoft.com/en-us/windows/win32/api/endpointvolume/nn-endpointvolume-iaudioendpointvolume
- [W12] ISimpleAudioVolume: https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nn-audioclient-isimpleaudiovolume
- [W13] Device Roles: https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-roles
- [W14] Accessibility parameters: https://learn.microsoft.com/en-us/windows/win32/winauto/accessibility-parameters
- [W15] SystemParametersInfo: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-systemparametersinfoa
- [W16] UISettings: https://learn.microsoft.com/en-us/uwp/api/windows.ui.viewmanagement.uisettings
- [W17] LoadKeyboardLayout: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-loadkeyboardlayoutw
- [W18] PowerWriteACValueIndex: https://learn.microsoft.com/en-us/windows/win32/api/powersetting/nf-powersetting-powerwriteacvalueindex ; Power Scheme Management: https://learn.microsoft.com/en-us/windows/win32/power/managing-power-schemes
- [W19] Registering for Power Events: https://learn.microsoft.com/en-us/windows/win32/power/registering-for-power-events
- [W20] SetDynamicTimeZoneInformation: https://learn.microsoft.com/en-us/windows/win32/api/timezoneapi/nf-timezoneapi-setdynamictimezoneinformation
- [W21] Windows Time Service: https://learn.microsoft.com/en-us/windows-server/networking/windows-time-service/windows-time-service-top
- [W22] BCDEdit Command-Line Options: https://learn.microsoft.com/en-us/windows-hardware/manufacture/desktop/bcdedit-command-line-options
- [W23] BCDEdit /set: https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/bcdedit--set
- [W24] Run and RunOnce Registry Keys: https://learn.microsoft.com/en-us/windows/win32/setupapi/run-and-runonce-registry-keys
- [W25] Predefined Keys: https://learn.microsoft.com/en-us/windows/win32/sysinfo/predefined-keys
- [W26] Store and retrieve settings and other app data: https://learn.microsoft.com/en-us/windows/apps/develop/data/store-and-retrieve-app-data
- [W27] RegNotifyChangeKeyValue: https://learn.microsoft.com/en-us/windows/win32/api/winreg/nf-winreg-regnotifychangekeyvalue
- [N1] NVIDIA Control Panel, Manage 3D Settings (reference): https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/nv3d/Manage_3D_Settings_(reference).htm
- [N2] NVIDIA Control Panel, To set up G-SYNC: https://www.nvidia.com/content/Control-Panel-Help/vLatest/en-us/mergedProjects/nvdsp/To_use_variable_refresh_rates.htm
- [N3] NVIDIA Driver Settings Programming Guide PG-5116-001-v02: https://developer.download.nvidia.com/tools/docs/PG-5116-001_v02_public.pdf
- [N4] NVAPI Reference, Driver Settings (DRS): https://docs.nvidia.com/nvapi/group__drsapi.html
- [N5] New Game Driver Introduces Support For RTX 2060 and G-SYNC Compatible Displays: https://www.nvidia.com/en-us/geforce/news/geforce-rtx-2060-g-sync-compatible-game-ready-driver/
- [A1] How to Enable AMD FreeSync with AMD Software: https://www.amd.com/en/resources/support-articles/faqs/DH3-013.html
- [A2] Customize Graphics Settings with AMD Software: https://www.amd.com/en/resources/support-articles/faqs/DH3-012.html ; Configure Radeon Anti-Lag: https://www.amd.com/en/resources/support-articles/faqs/DH3-033.html
- [V1] VESA Adaptive-Sync Display CTS r1.0: https://vesa.org/wp-content/uploads/2022/05/Adaptive-Sync-Display-CTS-r1.0.pdf
- [V2] VESA Adaptive-Sync White Paper: https://www.vesa.org/wp-content/uploads/2014/07/VESA-Adaptive-Sync-Whitepaper-140620.pdf
- [P1] Apple, Preferences and Settings Programming Guide, About the User Defaults System: https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/UserDefaults/AboutPreferenceDomains/AboutPreferenceDomains.html
