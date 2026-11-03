# 設定の仕組み：他の OS の公式の文書の調査（2026-11-01）

## 依頼文（写し）

あなたは OS の設計の調査者です。Web を調べて報告を書きます。WebSearch と WebFetch は deferred tool なので、先に ToolSearch で `select:WebSearch,WebFetch` を読み込みます。製品のコードは変えません。

### 背景
Nocturne は GUI が先の独自の OS です（POSIX でも Linux でも UNIX でもありません。カーネルは C）。持ち主は、設定の仕組みを OS の全領域で作り直そうとしています。持ち主の言葉：「全部ファイルは誤り」「境界がある：UI、システムのプロセス、カーネル」「編むファイルは無い、変えて適用する設定がある」「正しい物が正しい持ち主を持つ」。例として見せたのは Windows のサウンドの設定で、装置ごとに対応する bit 数とサンプルレートの組が出て、既定の形式を選んで適用します。

### 調べること
各 OS の作り手の公式の文書だけを根拠にします（Microsoft Learn、Apple Developer、fuchsia.dev、Haiku の公式の文書など）。**どの OS のソースコードも読みません。** Linux と UNIX の設計は調べません。ブログや掲示板を根拠にしません。URL を一つずつ書きます。

1. Windows：WASAPI と MMDevice（装置の列挙、`IAudioClient::GetMixFormat`・`IsFormatSupported`、共有モードと排他モード、装置ごとのプロパティストア、既定の装置の選択、変更の通知 `IMMNotificationClient`）。画面の設定（`ChangeDisplaySettingsEx`、`QueryDisplayConfig`・`SetDisplayConfig`）で、能力の一覧・選択・試して戻す・適用をどう分けているか。Windows のサービス、Settings のアプリ、レジストリーの役割と、ドライバー・サービス・UI の境界。グループポリシーの優先順位。
2. Apple：Core Audio の HAL のプロパティ（`kAudioDevicePropertyNominalSampleRate`、`kAudioDevicePropertyAvailableNominalSampleRates`、`kAudioStreamPropertyAvailablePhysicalFormats`、property listener）、IOKit と DriverKit の境界、launchd のサービス、`NSUserDefaults`・`CFPreferences` とその層（アプリ、利用者、ホスト、管理の値）、構成プロファイル。
3. Fuchsia：コンポーネントの能力の模型（capability routing）、FIDL、`fuchsia.settings`、ドライバーの枠組み、「ファイルでない」IPC の形、Audio Driver の能力の問い合わせ。
4. Haiku：`BMessage`、preflet、Media Kit、BeOS から続く形（公式の文書の範囲）。
5. Android の公開の文書：`Settings` の provider、`AudioManager` の装置の能力、system と app の権限の境界（公式の developer.android.com の範囲）。

### 引き出すこと
各 OS について、次の問いに答えます。比較の表も作ります。
- 能力（装置が何をできるか）と、選択（利用者が何を選んだか）と、実効の値（今何で動いているか）を分けているか。
- 設定の値の持ち主は誰か。保存は誰がするか。UI は値を持つか。
- 変更の道：UI → 何 → 誰 → 装置。適用、失敗、戻し、通知はどう行うか。
- 機械ごと、利用者ごと、装置ごと、アプリごとの区別。
- 権限の検査をどこで行うか。
- カーネルにある物と、カーネルの外にある物の線。
- 保存の形（レジストリー、preferences、DB）が API の後ろに隠れているか。利用者がファイルを編む前提があるか。

最後に、共通の原則と、OS ごとに違う所と、各 OS が自ら認めている失敗（公式の文書にある範囲）を書きます。Nocturne の今のコードは読みません。

### 報告
全文を `D:\Programes\llm vibe slop os\docs\settings-audit-external-20261101.md` に日本語で書きます。頭にこの依頼文を写します。識別子と API の名前は原語のまま書き、引用は一か所につき 15 語以内にして、それ以外は自分の言葉でまとめます。書きぶりは説明書のように、一文に一つのことを書きます。調べても分からないことは、分からないと書きます。

返事は、報告のパスと共通の原則の一行ずつだけにします。

---

## 0. 調べ方と証拠の程度

- 根拠は各 OS の作り手の公式の文書だけ。ソースコード、ブログ、掲示板、Q&A は根拠にしていない。
- 各出典の末尾に、読み方の別を付けた。
  - 「本文」：WebFetch で頁を取得し、その結果から書いた。
  - 「要約」：WebSearch が返した要約から書いた。頁の本文は読んでいない。
- WebFetch は頁を小さなモデルで要約して返すことがある。Microsoft Learn の頁は本文がそのまま返った。それ以外の頁は第二手の要約を含む。
- Apple の Core Audio の property 一覧の頁（`kAudioDevicePropertyAvailableNominalSampleRates` など）は、公式の文書の中に説明の文章が無い。宣言と関連の定数だけがある。この三つの property の意味は、Apple の公式の文書だけからは書けない。
- Nocturne のコードは読んでいない。

---

## 1. Windows

### 1.1 音（MMDevice と WASAPI）

#### 能力・選択・実効

三つは別の物として存在する。

| 区別 | 物 | 持ち主 |
|---|---|---|
| 能力 | `IAudioClient::IsFormatSupported`。共有モードでは音声エンジンに、排他モードではドライバーに問う。 | エンジン、ドライバー |
| 能力の既定 | `PKEY_AudioEngine_OEMFormat`。装置の既定の形式。OEM が INF に書く。 | OEM（ドライバーの組） |
| 選択 | `PKEY_AudioEngine_DeviceFormat`。利用者が共有モードで選んだ形式。Sound の画面（mmsys.cpl）の Advanced の Default Format で選ぶ。 | 利用者 |
| 実効 | `IAudioClient::GetMixFormat`。エンジンが共有モードの内部処理に使う形式。 | 音声エンジン |

- 実効の形式は、選択した形式と同じチャンネル数と同じサンプルレートで、サンプルだけ浮動小数点にしたもの。文書は「近いが、同一とは限らない」と書く。[W3]
- 共有モードでは、応用の stream は混合形式と同じチャンネル数とサンプルレートが要る。エンジンが変えるのは標準の PCM の標本幅と浮動小数点の間だけ。[W3]
- 排他モードでは、応用はハードウェアが明示して対応する形式を使う。エンジンは間に入らない。非 PCM は排他モードだけ。[W3]
- 能力を一覧で返す API は、読んだ文書には出てこない。応用は候補の表を作り、`IsFormatSupported` に一つずつ渡して、通った物を使う。[W3]
- 一部のドライバーは、1・2 チャンネルの PCM を `WAVEFORMATEX` なら通し、`WAVEFORMATEXTENSIBLE` なら断る。文書は、両方で二回問うように勧める。[W3]
- Sound の画面が、選べる形式の一覧をどう作るかは、読んだ文書に書いていない。

装置の状態は `DEVICE_STATE_ACTIVE`・`DISABLED`・`NOTPRESENT`・`UNPLUGGED` の四つ。[W6]

#### 装置の単位

- 利用者が見る単位は「エンドポイント装置」（スピーカー、ヘッドホン、マイク）。アダプター上の部品ではない。[W1]
- 文書は、Vista より前の「応用が PnP の装置から推測する」作りを、誤りの多い作りとして書く。エンドポイントの考えは、その推測を OS の仕事に移すために入れた。[W1]
- エンドポイントは、OS が数える物。応用は一覧から選んで有効にする。マイクが多重器を持つとき、OS が入力を選ぶ。[W1]
- エンドポイント ID は不透明な文字列。形式は変わりうるので、応用は中身を読まない。[W2]

#### 持ち主と保存

- 基本のプロパティはエンドポイントマネージャーが設定する。応用は読めるが、書かないように文書が求める。[W2]
- 音声サービス（Windows audio service）が、装置の増減、役割の割り当てを追い、エンジンの制御の値を設定して「音声の方針」を実装する。[W7]
- エンジンは応用とは別の保護されたプロセスで動く。サービスが両者の間に、プロセスをまたぐ endpoint buffer を割り当てる。[W7]
- 値の保存先は、音の文書には書いていない。ドライバー側の文書は、INF の AddReg が装置の software key に値を書くと書く。[W12]
- UI が値を持つかは、文書に書いていない。形式の選択は、プロパティの値として装置に付く。

#### 変更の道

- 応用の道：応用が `IAudioClient::Initialize` で形式を渡す。共有モードなら混合形式に合わせる。排他モードなら装置の形式で直接やり取りする。[W3][W7]
- 利用者の道：Sound の画面で形式を選ぶ。この画面から先の道（どのプロセスに、何で書くか）は、読んだ文書に書いていない。
- 通知：`IMMDeviceEnumerator::RegisterEndpointNotificationCallback` に `IMMNotificationClient` を渡す。[W6]
  - `OnDefaultDeviceChanged`、`OnDeviceAdded`、`OnDeviceRemoved`、`OnDeviceStateChanged`、`OnPropertyValueChanged` の五つ。
  - 登録した応用は、全装置の全種類の事象を受ける。必要な物を選ぶのは応用の仕事。
- 失敗と戻し：排他モードの要求は、他が排他で使っていれば失敗する。共有で使われているときの扱いは、利用者の設定「Give exclusive mode applications priority」が決める。[W4]

#### 区別の軸

- 装置ごと：形式、排他モードの許可、排他の優先は、装置ごとに利用者が決める。[W3][W4]
- 役割ごと：`eConsole`・`eMultimedia`・`eCommunications` の三つの役割に、描画と取り込みで別々に一つずつ既定の装置を割り当てる。[W5]
- 応用ごと：応用は、stream ごとに装置を選べる。役割に合う装置を選ぶ使い方を文書が示す。[W5]
- 機械ごと、利用者ごとの区別は、音の文書には出てこない。

#### 権限

- 音の文書は、権限の検査の場所を書いていない。
- 排他モードの可否は、利用者が装置ごとに決める設定で、応用の権限ではない。[W4]

#### カーネルの線

- core audio の API は、利用者空間の薄い層。クライアントを、カーネルのオーディオドライバーとハードウェアから切り離す。[W7]
- 音声サービスとエンジンは利用者空間。その下に、カーネルのドライバー（Portcls.sys、Hdaudio.sys、USB Audio 用の Usbaudio.sys など）がある。[W7]
- 排他モードで WaveRT ドライバーなら、endpoint buffer はハードウェアと応用が直接共有する。システムの介在が要らない。[W4]
- 低遅延の応用は、MMCSS でスレッドの優先度を上げる。[W4][W7]

### 1.2 画面（三世代の API）

#### `EnumDisplaySettings` と `ChangeDisplaySettingsEx`

- `EnumDisplaySettings` が返す `DEVMODE` は、ドライバーが対応する値だけを含む。`ChangeDisplaySettingsEx` に渡す値は、これから取るよう文書が求める。[W9]
- 試す：`CDS_TEST` は、変更せずに、その形式が設定できるかを検査する。[W9]
- 一時の適用：フラグ 0 は、その場で変えるだけで保存しない。[W9]
- 保存つきの適用：`CDS_UPDATEREGISTRY` は、変えて、利用者のプロファイルに保存する。[W9]
- 機械全体の保存：`CDS_GLOBAL`（`CDS_UPDATEREGISTRY` と併用）は、全利用者に効く場所に保存する。[W9]
- 保存だけ：`CDS_NORESET` は、保存して適用しない。[W9]
- 戻し：`lpDevMode` を `NULL`、`dwFlags` を 0 にして呼ぶと、レジストリーの値に戻る。[W9]
- 複数画面：画面ごとに `CDS_UPDATEREGISTRY | CDS_NORESET` で保存し、最後に装置 `NULL` で一度だけ適用する。[W9]
- 結果：`DISP_CHANGE_SUCCESSFUL`、`BADMODE`、`FAILED`、`RESTART`、`NOTUPDATED` など。[W9]
- 通知：モードが動的に変わると、`WM_DISPLAYCHANGE` が全応用に送られる。[W9]

#### CCD（`QueryDisplayConfig`・`SetDisplayConfig`）

- 能力：`QDC_ALL_PATHS` が、source と target の組の可能な全部を返す。高価なので、可能な接続の一覧が要る場合だけ使うよう文書が書く。[W10]
- 実効：`QDC_ONLY_ACTIVE_PATHS` が、今の有効な path を返す。[W10]
- 保存した選択：`QDC_DATABASE_CURRENT` が、接続中の表示器について CCD のデータベースにある有効な path を返す。[W10]
- 検査：`SDC_VALIDATE` は、設定できるかを検査するだけ。`SDC_APPLY` と同時には使えない。[W11]
- 一時の適用：`SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG`。保存しない。[W11]
- 保存つきの適用：`SDC_APPLY | SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_SAVE_TO_DATABASE`。[W11]
- 戻し：一時のモードから最後に保存した設定へ戻すには、`SDC_APPLY | SDC_USE_DATABASE_CURRENT`。[W11]
- 調整の許可：既定では、渡した path とモードを変えない。作れなければ `ERROR_BAD_CONFIGURATION` で失敗する。`SDC_ALLOW_CHANGES` を付けると、関数が値を調整してよい。[W11]
- 文書は、呼び手が `QueryDisplayConfig` で今の構成と他の有効な可能性を取り、`SetDisplayConfig` で検査と設定をする流れを想定していると書く。[W11]
- 権限：コンソールのセッションに触れない呼び手は `ERROR_ACCESS_DENIED`。WDDM でないドライバーなら `ERROR_NOT_SUPPORTED`。[W10][W11]
- データベースの保存先と形式は、読んだ文書に書いていない。

### 1.3 サービス、Settings、レジストリー、ドライバー、UI の境界

- サービス：Service Control Manager（SCM）が、インストール済みサービスとドライバーサービスのデータベースを持つ。起動時に始まる RPC サーバーで、遠隔の機械のサービスも管理できる。[W20]（要約）
- セッション 0 の隔離：Vista 以降、サービスはセッション 0 に置かれ、利用者と対話しない。サービスは自分で対話窓を出せず、`WTSSendMessage` で他のセッションに出す。[W19]（要約）
- つまり、サービス（持ち主）と UI（別のセッションのプロセス）は、プロセスの境界で分かれている。
- Settings のアプリ：公式の文書にあるのは、`ms-settings:` の URI で頁を開く方法だけ。内部の構造の文書は見つけていない。[W21]（要約）
- Sound の画面（mmsys.cpl）と Settings の頁は、両方とも UI。同じ値を見せ合う。値の持ち主は UI ではない。
- レジストリー：木の形の階層データベース。機械ごとの `HKEY_LOCAL_MACHINE` と、利用者ごとの `HKEY_CURRENT_USER` に分かれる。[W15]
- `HKEY_CLASSES_ROOT` は、機械の既定と利用者の上書きを合わせた見え方。書き込みの行き先は、既存の鍵の有無で決まる。[W15]
- ドライバーは、レジストリーの位置を決め打たない。PnP マネージャーが、ドライバーごとの software key と装置ごとの hardware key を渡す。ドライバーはその開く関数（`WdfDeviceOpenRegistryKey`、`IoOpenDeviceRegistryKey` など）を使う。[W12][W22]
- 文書は「位置と形式は版で変わりうる。位置、形式、意味を仮定しない」と書く。[W13]
- レジストリーを直接編む利用者の前提：文書は、変更は Windows の UI で行うのを勧める。レジストリー エディターは、誤ると OS の再インストールに至りうると警告する。直接編集は、管理者と KB の手順の道として残している。[W15]
- INF：OEM がドライバーの組に入れる。`AddReg` で、インストール時に software key や hardware key に値を書く。OEMFormat はこの道で入る。[W8][W12]

### 1.4 方針（グループポリシー、MDM）

- 利用者の選択（preference）と管理者の強制（policy）は、別の鍵に置く。方針は `Software\Policies` の下、選択は別の場所。両方があるとき、方針が先。[W14]
- 部品は、起動時と方針の再適用のときに、方針の鍵を見る。[W14]
- 再適用の通知は二つ。`RegisterGPNotification` と、`WM_SETTINGCHANGE`（lParam が "Policy"）。[W14]
- 既定の再適用は 90 分ごとに乱数で最大 30 分を足す。ドメインコントローラーは 5 分ごと。[W14][W16]
- GPO の適用の順は、ローカル、サイト、ドメイン、OU。後に適用した物が前を上書きする。入れ子の OU は親が先。[W16]
- 同じ container に複数の GPO があるときは、link order が小さい物が優先。[W16]
- `Enforced` は下位に上書きされない。`Block Inheritance` は上位の設定を止めるが、`Enforced` には効かない。[W16]
- loopback の merge モードでは、計算機側の GPO が利用者側より優先。[W16]
- Policy CSP（MDM の道）は、device 範囲と user 範囲を分ける。同じ方針を複数の設定元が設定でき、`Result` の下で衝突を解決した結果を読める。[W17]
- MDM と GP の衝突：`MDMWinsOverGP` の既定は 0。1 にすると、同等の GP が止まる。効くのは Policy CSP の物だけ。両方に同じ設定を置くと、どちらが勝つかの保証が無いと文書が書く。[W18]
- 方針は preference を消さない。preference は、GPO が外れても残る（「tattooing」）。[W24]（要約）

---

## 2. Apple

### 2.1 Core Audio（HAL）

#### 能力・選択・実効

- Apple の公式の文書で確認できた物：
  - `AudioObjectIsPropertySettable`：その property が `AudioObjectSetPropertyData` で変えられるかを問う。[A9]
  - `AudioObjectSetPropertyData`：文書は「HAL が listener を呼ぶまで、値は変わったと見なさない。多くの property は非同期に変わる」と書く。[A7]
  - `AudioObjectAddPropertyListener`：property の変化の通知を受ける関数を登録する。[A8]
- `kAudioDevicePropertyNominalSampleRate`、`kAudioDevicePropertyAvailableNominalSampleRates`、`kAudioStreamPropertyAvailablePhysicalFormats` の三つは、公式の文書の頁に宣言と関連の定数はあるが、説明の文章が無い。[A10]
- 名前から言えること：利用可能な値（能力）と今の値（選択または実効）を、別の property の選択子（selector）に分けている。これは名前の読みで、文書の記述ではない。
- 設計の原則は、一つの汎用の入口（`AudioObjectPropertyAddress` ＝ 選択子・範囲・要素）で、能力、現在値、変更、通知を扱う形。

#### 装置と stream の持ち主（AudioDriverKit）

- 装置が sample rate を持つ。`SetAvailableSampleRates`（能力）と `SetSampleRate`（実効）。[A5]
- stream が形式を持つ。`SetAvailableStreamFormats`（能力）と `SetCurrentStreamFormat`（実効）。[A5]
- つまり、能力と実効を、ドライバーが別の関数で出す。

#### 変更の道

- 構成の変更の手順（ドライバーの側から始める場合）。[A5]
  1. ドライバーが `RequestDeviceConfigurationChange` を呼ぶ。
  2. HAL が listener に通知し、IO が動いていれば止める。
  3. HAL が `PerformDeviceConfigurationChange` を呼ぶ。ドライバーが装置とハードウェアの状態を変えてよいのは、この一点だけ。
  4. HAL が新しい状態を取り込み、listener に通知し、IO を再開し、終わりを知らせる。
- 利用者の道：Audio MIDI Setup の Format のメニューで、sample rate と bit depth を選ぶ。選べる値は装置ごとに違う。[A11]（要約）
- Core Audio の概説は、利用者が Audio MIDI Setup で経路を決めると書く。既定の形式を誰が決めるかは、この概説に書いていない。[A4]
- HAL を動かすプロセスの名前は、読んだ文書に書いていない。

#### 区別の軸

- 装置ごと：sample rate。stream ごと：形式。[A5]
- 利用者ごと、ホストごと、アプリごとの区別は、下の preferences の層で扱う。

#### カーネルの線

- DriverKit のドライバー（dext）は利用者空間で動く。dext が落ちても、カーネルと他は動き続ける。kext が落ちると、機械全体が落ちる。[A12]（要約）
- Monterey より前は、音のドライバーが二部品だった：HAL と話す audio server plug-in と、ハードウェアと話す dext。AudioDriverKit は、二つを dext 一つにまとめた。HAL との通信は、AudioDriverKit が持つ専用の user client が行う。[A5]
- 仮想装置には、audio server plug-in の道が残る。[A5]
- dext は、sample time と host time の組を `UpdateCurrentZeroTimestamp` で HAL に渡す。stream の IO buffer は HAL に写され、できれば DMA と同じ記憶を使う。[A5]

### 2.2 Preferences の層

#### NSUserDefaults の領域（検索の順）

| 順 | 領域 | 性質 |
|---|---|---|
| 1 | argument domain | 揮発。起動時の引数で一時の上書き。 |
| 2 | application domain | 永続。アプリ自身の値。 |
| 3 | global domain | 永続。全アプリが共有する値。主にシステムの枠組みが使う。 |
| 4 | languages domain | 揮発。言語ごとの地域の値。 |
| 5 | registration domain | 揮発。`registerDefaults:` で置く初期値。 |

- 最初に見つかった領域で検索が止まる。前の領域が後ろを上書きする。[A1]
- アプリは、global domain に自分の値を書かない。global の値を自分用に変えるときは、同じ鍵をアプリの領域に書く。[A1]
- アプリの領域は、`$HOME/Library/Preferences/<bundle-id>.plist` に保存される。文書は、アプリがこの plist を直接変えないよう求める。デバッグで中身を見ることは認める。[A1]

#### CFPreferences の層

- 領域は、利用者・アプリ ID・ホストの三つの軸で決まる。それぞれ「Current」か「Any」で、八つの領域になる。[A2][A3]
- 検索は、現在の利用者・そのアプリ・現在のホストの最も具体的な領域から、最も一般的な領域（Any の三つ）へ進み、最初の一致で止まる。[A3]
- 書き込みの既定の行き先は、現在の利用者・そのアプリ・Any のホスト。[A3]
- 明示の `CFPreferencesCopyValue` と `CFPreferencesSetValue` は、領域を引数で指定し、検索しない。[A3]
- 値は記憶にキャッシュされる。ディスクへの書き出しは `CFPreferencesSynchronize`。費用が大きいので多用しない。[A3]
- 他の利用者の preferences を読むには、特別な権限が要る。[A2]

#### 管理の値

- 構成プロファイルは、device channel（システム）と user channel（利用者）に分かれる。iPhone と iPad は利用者を区別せず、常に device。Mac と共有 iPad は両方に向けられる。[A6]
- プロファイルを外す道：管理サービス、構成ツール、利用者の登録解除。Mac で手動のプロファイルを外すには、ローカル管理者の資格情報が要る。[A6]
- `NSUserDefaults` の `objectIsForced(forKey:)` が、「管理者が値を与えたか」を返す。[A15]（要約）
- 古い Configuration Profile Reference は、payload の追加の鍵を、その preference domain の管理された preference として使えると書く。[A14]（要約）
- 複数のプロファイルが同じ payload を違う値で持つとき、結果は未定義と、Apple Platform Deployment の文書が書く。該当の頁は特定していない。[A14]（要約）

#### 変更の通知

- preference pane は、変えるたびに保存する。閉じるときにまとめて保存しない。システムが終了時に SIGKILL で止めうるから。まとまりが要る設定は、一時の領域に置いて Apply で本物にコピーする。[A2]
- ディスクに保存しても、動いているアプリは気づかない。別に通知が要る。一方向なら distributed notification（機械ごとのセンターで非同期に配る）を文書が勧める。[A2]

#### ファイルを編む前提

- Mac 専用の対象には Preference Services を使う。[A2]
- 他の OS と共通の対象（BSD の daemon など）は、API が使えない。pane が daemon 自身の設定ファイルを読み、解析し、書き、保存する。形式は プログラムごとに違う。書いた後に SIGHUP を送って読み直させる。[A2]
- つまり、Apple は、持ち込んだ移植物のためにファイルを編む道を、公式の文書で認めている。

### 2.3 launchd

- 種類は置き場所で決まる。`/Library/LaunchDaemons` はシステムの文脈で動く daemon、`/Library/LaunchAgents` と利用者の `LaunchAgents` は利用者の文脈で動く agent。[A13]（要約）
- daemon はウィンドウサーバーに触れず、画面の UI を出せない。agent は出せる。[A13]（要約）
- 必須の鍵は `Label` と `ProgramArguments`。job は自分で daemon 化しない。`UserName` などの鍵で権限を宣言し、`setuid` を自分で呼ばない。[A13]（要約）
- Startup Items と mach_init の daemon は、非推奨と文書が書く。[A13]（要約）
- plist は、開発者が書いて置く物。利用者が編む前提は無い。

---

## 3. Fuchsia

### 3.1 能力の模型（capability routing）

- コンポーネントは、自分の namespace にある物だけに触れる。それ以外は使えない。文書は、これを「環境にある権限が無い」と述べる。[F1]
- 能力の種類：protocol（FIDL の protocol へ開く channel）、service、directory、storage、dictionary。runner と resolver は、コンポーネントでなく環境へ経路を引く。[F1]
- 経路の宣言は四つ：`capabilities`（自分が出す）、`use`（使う）、`offer`（子へ渡す）、`expose`（親か枠組みへ出す）。[F1][F2]
- 能力が実行時に働くのは、消費者から提供者まで有効な経路がつながるときだけ。経路は親から子へ通るのが普通で、親が子の sandbox を決める。[F1]
- realm は、親と子の集まり。能力は、`expose` が許さなければ realm から出られない。[F2]（要約）
- 権限は、「許可の表」でなく、経路の宣言で決まる。manifest（.cml）が宣言する。[F1][F2]

### 3.2 「ファイルでない」IPC

- FIDL は、IPC を記述する言語。`.fidl` の protocol がメソッドの集まりを定める。メッセージは bytes と handle（能力）を、Zircon の channel で運ぶ。[F3]
- OS 自身は FIDL を知らない。束縛を作るのは、同じ定義から生成した両側の符号。[F3]
- namespace は、名前の付いた物（file、directory、socket、service、device）の木。コンポーネントごとに持ち、「全体の根が無い」。[F4]
- `/svc` は渡された protocol と service、`/dev` は device の木、ほかに `pkg`、`data`、`tmp`、`config`。[F4]
- 注意：`/svc/<名前>` は file の見た目の path だが、枠組みが渡した handle の表の入口。利用者が編む file ではない。
- ドライバーの通信も同じ能力の仕組み。ドライバー同士は service で話し、Driver Manager が親から子への経路を、子ができたときに引く。[F10]
- 古い `devfs` は、新しい符号では勧めない。[F10]

### 3.3 設定

#### 設定の機構（コンポーネントの構成）

三つの機構。[F5]

| 機構 | 値が決まる時 | 変えられる者 |
|---|---|---|
| structured configuration（既定の選択） | build、製品の組み立て、起動時 | 開発者、製品の統合者。起動時に渡す。変更には再起動。 |
| package-based（package 内の file） | 出荷時 | 出荷物を作る者 |
| service-based（FIDL の server が持つ） | 実行中 | 利用者（UI を通して） |

- 利用者が変えられるのは service-based だけ。structured configuration は、利用者の操作に向かないと文書が書く。[F5]
- structured configuration の値は、コンポーネントの枠組みが起動時に渡す。コンポーネントは file を開かない。[F5]
- package-based は、コンポーネントが file を開いて解析する。[F5]

#### `fuchsia.settings`（service-based の実体）

- 領域ごとの protocol：Accessibility、Audio、Display、DoNotDisturb、FactoryReset、Input、Intl など。[F6]
- `Set`：未指定の欄は変えない（部分更新）。Audio の `Set2` は、設定した欄だけ更新する。[F6]
- `Watch`：hanging get。最初の呼びは今の値をすぐ返す。次からは変わるまで待つ。[F6]
- 失敗：`Set` の結果に誤りが入る。値は `REQUEST_NOT_SUPPORTED`、`INTERNAL_SERVICE_ERROR`、`PERSISTENT_STORAGE_ERROR` など。`Watch` の失敗は致命で、channel が閉じる。[F6]
- 保存：リファレンスは、誰が保存するかを書いていない。構成の機構の頁は、設定 service が利用者の設定を電源の入れ直しをまたいで保つと書く。[F5][F13]
- Audio の設定の `source`：`USER` は利用者が決めた音量で、保存する。`SYSTEM` と `SYSTEM_WITH_FEEDBACK` はシステムが常に計算する音量で、保存しない。[F6]（要約）
- つまり、利用者の選択と、システムが決める実効の値を、設定の型の中で分けている。
- 旧 Audio の `Set` と `Watch` は、API level 26 で非推奨、31 で削除。`Set2` と `Watch2` が置き換えた。[F6]

#### 方針（`fuchsia.settings.policy`）

- 設定は常に存在して既定値がある。方針は、設定の API への要求に掛ける変換（例：最小の明るさ）。[F7]
- 別の名前空間で、読むと変えるに別の権限が要る。[F7]
- 操作は `AddPolicy` と `RemovePolicy`。追加すると一意の ID が付き、外すときに使う。標的ごとに方針は 0 個以上。`GetProperties` が、可能な変換と有効な方針を返す。[F7]
- 複数の方針は、その積を取る。最大音量なら、最も低い限度が勝つ。それを外すと、次に低い物が効く。[F7]
- 方針の変化を見る `Watch` は無い。呼び手は、方針の変化に反応する想定ではないから。[F7]

### 3.4 Audio Driver

- クライアント向けの口は Audio Composite。StreamConfig、Codec、DAI の旧い口は非推奨で、Composite に写る。[F8]
- 旧 StreamConfig の流れ（非推奨と頁の頭に書いてある）。[F9]
  1. 探す：`/dev/class/audio-input`・`audio-output` の node を開き、`GetChannel` で stream の channel を得る。
  2. 能力：クライアントが `GetSupportedFormats` を呼ぶ。ドライバーは、チャンネル数、標本の形式、フレームレート、標本のバイト幅、有効な bit 数の組を返す。一つの組で全部を書けないときは、複数の組を返す。
  3. 選択：クライアントが形式を選び、`CreateRingBuffer` で渡す。選ぶのはクライアント。
  4. 実効：返ってきた ring buffer の channel から、共有の buffer（VMO）を得る。
- 形式を変えるとき、すでに有効な ring buffer があれば、ドライバーはそれを閉じ、できるだけ穏やかに止める。[F9]
- 接続の検知：`GetProperties` が `plug_detect_capabilities` を返す。`HARDWIRED` は常に接続。`CAN_ASYNC_NOTIFY` なら `WatchPlugState`（hanging get）で変化を受ける。[F9]
- ドライバーが何かを保存するかは、書いていない。[F9]
- Composite の口での問い合わせ（形式、topology、element）の細部は、読んだ頁が別の頁へ譲っている。読んでいない。[F8]
- `fuchsia.media` の `AudioDeviceEnumerator`：`GetDevices` が装置の一覧と `is_default` を返す。`SetDeviceGain` は、フラグを立てた欄だけ変える。不正な token は無視する。`OnDefaultDeviceChanged` は非推奨。[F12]（要約）

### 3.5 カーネルの線

- ドライバーは、コンポーネントと同じ能力の仕組みに乗る。ドライバーをどのプロセスに置くか（driver host）は、読んだ頁には書いていない。[F10]
- ring buffer は VMO（共有の記憶）で渡す。[F9]

---

## 4. Haiku

### 4.1 BMessage と Application Kit

- `BMessage` は、`what`（4 バイトの型の値）と、名前・型・添字で引く欄の集まり。`Add*()` で入れ、`Find*()` で取る。意味は文脈が決める。[H4][H7]
- 送るのは `BMessage` でなく、`BLooper`・`BHandler`・`BMessenger`。[H4]
- `BLooper` は専用のスレッドで受信ループを回す。`BApplication` と `BWindow` も `BLooper`。[H4]
- `PostMessage()` は非同期。`BMessenger::SendMessage()` の一部は同期で、返信を受けられる。[H4]
- `BMessenger` は、同じ team の宛先にも、別の address space の宛先にも送る。応用の間の通信の主な道具。[H4][H7]
- 応用ごとに address space が違うので、pointer を送っても意味が無い。値を copy するか、共有の記憶を使う。[H7]
- 設定の保存：`BMessage` は `Flatten()` で file に書き、`Unflatten()` で読み戻せる。多くの応用が設定を message として保存すると、`BMessage` のリファレンスが書く。[H7][H8]（要約）
- 読んだ `Messaging Foundations` の頁は、送受信の節が「To do」のまま。app_server、registrar、input_server、media_server への message の話は無い。[H4]

### 4.2 Media Kit

- `media_server` に node が登録される。応用ごとに `BMediaRoster` が最大一つあり、応用の窓口になる。[H3]
- node は `BMediaNode` から派生する：producer、consumer、filter、controllable（利用者が調整できる parameter を出す）、time source。[H3]
- 形式の交渉：`media_source` と `media_destination` が接続の端。`media_output` と `media_input` が形式を含む全体を記述する。`media_format` は基本の型と、型ごとの詳細。wildcard で「この欄は何でもよい」と言える。[H3]
- 応用が決める物：どの node を選んで繋ぐか、形式の交渉、開始、停止、再生速度。[H3]
- システムが持つ物：既定の time source への同期。音声 mixer は常に動き、応用は開始も停止もしない。標準の node は、利用者が Media の preference で設定する。[H3]
- 能力・選択・実効：`media_format` の交渉と、node の接続が、実効を決める。利用者の選択（既定の出力装置）は Media の preflet。能力の問い合わせの API は、読んだ頁では確認していない。

### 4.3 preflet と設定の file

- 利用者の設定は `~/config/settings/` に置く。応用ごとの subfolder か、一つの file。[H5]
- システム全体の設定は `/boot/system/settings/`。多くの `/boot/system` と `/boot/home/config` は package マネージャーが管理し、読み取り専用。利用者自身の `~/config/settings` は書ける。[H5]
- Media の preflet：音の入力と出力の装置、HD audio の入出力の周波数、水準、mixer のチャンネルの写し、再標本化の質を選ぶ。[H1]
  - 触る file：`~/config/settings/Media/*`、`~/config/settings/System Audio Mixer`、窓の位置の `MediaPrefs`。[H1]
  - 変更がいつ効くかは、頁に書いていない。直し方として「Restart media services」を勧める。[H1]
  - 2026 年 8 月の活動報告によると、出力装置の切り替えは、media_server の再起動なしで数秒で効くようになった。[H6]
- Screen の preflet：窓ごとの作業領域（workspace）の解像度、色の深さ、リフレッシュレートを選ぶ。[H2]
  - Apply で切り替わり、「保持するか」を問う。12 秒、応答が無ければ自動で前のモードに戻る。Revert ボタンは、窓を開いたときの設定に戻す。[H2]
  - 安全モード：`Shift+Ctrl+Alt+Esc` で、どこからでも退避の video mode に切り替わる。同じ 12 秒の戻しがある。[H2]
  - 触る file：`~/config/settings/system/app_server/workspaces`、`~/config/settings/kernel/drivers/vesa`。[H2]
- カーネルのドライバーの設定は、利用者が file の `#` を外して有効にする形で、注意を添えて文書化されている。[H5]
- preflet が、どの process に何で値を渡すかは、読んだ頁に書いていない。

### 4.4 カーネルの線

- app_server、media_server は利用者空間の「サーバー」。`kernel/drivers/` の設定 file がカーネルのドライバーに効く。細部の境界は読んだ頁に書いていない。[H5]

---

## 5. Android

### 5.1 Settings の provider

- 三つの表：`Settings.System`（雑多な設定の名前と値）、`Settings.Secure`（アプリは読めるが書けない）、`Settings.Global`（全利用者に同じく効く）。[D6]（要約）
- `Settings.System.putInt` は、値を文字列に変えて保存する。表が名前と文字列の対であることを示す。[D6]（要約）
- 書く権限：
  - `WRITE_SETTINGS`：API 23 以降は、manifest に宣言し、かつ利用者が「権限の管理」の画面で個別に許可する。許可を求める意図（intent）は `ACTION_MANAGE_WRITE_SETTINGS`。確認は `Settings.System.canWrite()`。[D7]（要約）
  - `WRITE_SECURE_SETTINGS`：第三者のアプリ向けでない。[D7]（要約）
  - `Settings.Secure` を変える者：システムのアプリの UI。[D6]（要約）
- `System` と `Secure` が利用者ごとかは、読んだ文書に書いていない。`Global` は、全利用者に同じと書く。
- 値の保存を誰がするかは、読んだ文書に書いていない。

### 5.2 音の能力と選択

- 列挙：`AudioManager.getDevices(int)` が、今つながっている装置を返す。[D3]（要約）
- 能力：`AudioDeviceInfo.getSampleRates()`、`getChannelCounts()`、`getEncodings()` が配列を返す。空の配列は「任意の値に対応」の意味。API 31 以降は `getAudioProfiles()`。[D2]（要約。`AudioDeviceInfo` の頁は本文を読めていない）
- 通知：`registerAudioDeviceCallback` に `onAudioDevicesAdded` と `onAudioDevicesRemoved` を渡す。[D3]（要約）
- 選択（通話の装置）：`setCommunicationDevice` は、その装置が今使えないと拒否される。選択は、要求したアプリのプロセスが生きている間、`clearCommunicationDevice()` まで、または装置が外れるまで有効。複数のアプリが争うときは、音声モードを握るアプリが優先。[D3][D10]（要約）
- 選択の反映の待ち：文書は、装置の設定の後、30 秒まで待ち、`AudioDeviceCallback` で結果を知るよう案内する。誤りなら `clearCommunicationDevice()` からやり直す。[D10]（要約）

### 5.3 mixer の属性（形式の能力・選択・実効に最も近い API）

- Android 14（API 34）から、USB 音声の再生について、応用が好みの mixer 属性（形式、チャンネルマスク、sample rate、mixer の動作）を問い合わせて設定できる。[D1]
- 能力：`getSupportedMixerAttributes(device)` が、`setPreferredMixerAttributes` に渡せる `AudioMixerAttributes` を返す。USB 以外は空の一覧でありうる。[D4][D5]（要約）
- 選択：`setPreferredMixerAttributes(AudioAttributes, device, mixerAttributes)`。`MODIFY_AUDIO_SETTINGS` の権限が要る。今は `USAGE_MEDIA` だけ。持ち主は uid ごと。[D5]（要約）
- 戻し：`clearPreferredMixerAttributes`。`getPreferredMixerAttributes` は、設定なしか解除後なら `null`。[D5]（要約）
- 通知：`addOnPreferredMixerAttributesChangedListener`。[D5]（要約）
- 実効：必要なら、フレームワークが新しい属性で出力 stream を開き直す。[D1]
- 既定では、フレームワークがすべての音源を混ぜ、音量と効果を掛ける。`BIT_PERFECT` は、混合も sample rate の変換も効果も無しで HAL に渡す。AIDL の Audio HAL と、dynamic mix port の `AUDIO_OUTPUT_FLAG_BIT_PERFECT` が要る。[D1]
- 「USB だけが設定可能」と文書が書く。[D1]
- ベンダーは、USB 装置を dynamic mix port へ経路づけ、audio policy の設定か `getAudioPorts` と `getAudioRoutes` で宣言する。[D1]

### 5.4 system と app の権限の境界

- 権限の種類：normal（インストール時に与える）、signature（同じ署名の者だけ）、dangerous（実行時に利用者へ問う）、special（利用者が「Special App Access」の画面で与える）。[D8]（要約）
- `signature|privileged` のように、基本の型に flag を付けられる。`signatureOrSystem` は API 23 で非推奨で、文書は使わないよう勧める。[D8]（要約）
- `knownSigner` で、製造時に署名しなくても、列挙した署名者へ signature の権限を渡せる。[D8]（要約）
- 権限は、システムが検査する。サービスへの binder を、システムだけに許す signature の権限も使う。[D8]（要約）
- 方針の層：`DevicePolicyManager` の頁が検索で出たが、中身は読んでいない。

### 5.5 カーネルの外の audio

- audio policy の設定は XML の file（`audio_policy_configuration.xml`、`/system/etc`）。audio の topology を記述する。機器の作り手（OEM）が組む物で、利用者や応用が編む物ではない。HIDL の Audio HAL の一部として schema に沿い、VTS が検査する。[D9]（要約）
- policy の層は、規則（engine）と、binder・スレッド・錠の層（service）に分かれる。AudioFlinger が mixer。[D9]（要約）
- policy の file が、同時に開ける入力 stream の数も決める。[D9]（要約）
- 内部の細部（AudioFlinger と AudioPolicyService の呼び合い）は、読んでいない。

---

## 6. 比較の表

「－」は、読んだ公式の文書に書いていないこと。

| 問い | Windows | Apple | Fuchsia | Haiku | Android |
|---|---|---|---|---|---|
| 能力の問い | `IsFormatSupported`（形式を一つずつ）。`OEMFormat`（既定）。画面は `EnumDisplaySettings`、`QDC_ALL_PATHS`。 | `AvailableNominalSampleRates` などの property（文書に説明の文章なし）。AudioDriverKit の `SetAvailable…`。 | `GetSupportedFormats`（旧）、Composite。`GetProperties`。 | － | `AudioDeviceInfo` の配列。`getSupportedMixerAttributes`。 |
| 選択 | `DeviceFormat`（利用者）。既定の装置を役割ごと。画面は `CDS_UPDATEREGISTRY`、`SDC_SAVE_TO_DATABASE`。 | Audio MIDI Setup の Format。`NominalSampleRate`。 | 形式はクライアントが選ぶ。設定は `fuchsia.settings`（`USER`）。 | Media と Screen の preflet。 | 設定の表。mixer 属性の preference（uid ごと）。 |
| 実効 | `GetMixFormat`。`QDC_ONLY_ACTIVE_PATHS`。 | listener の通知で確定。`SetCurrentStreamFormat`。 | ring buffer。`SYSTEM` の値は保存しない。 | `media_format` の交渉の結果。 | 出力 stream を開き直した結果。 |
| 試す | `CDS_TEST`、`SDC_VALIDATE`、`IsFormatSupported`。 | `IsPropertySettable`。 | 形式の交渉の失敗で拒否。 | Apply 後に「保持するか」を問う。 | `setCommunicationDevice` の拒否。 |
| 戻す | `SDC_USE_DATABASE_CURRENT`。`NULL` と 0 で `ChangeDisplaySettingsEx`。 | － | `RemovePolicy`。 | 12 秒で自動で戻る。Revert。 | `clear…`。 |
| 通知 | `IMMNotificationClient`、`WM_DISPLAYCHANGE`。 | `AudioObjectAddPropertyListener`。 | hanging get（`Watch`）。 | －（読んだ頁に preflet の変更通知の記述が無い）。 | `AudioDeviceCallback`、listener。 |
| 値の持ち主 | 音声サービス、エンドポイントマネージャー（音）。CCD データベース（画面）。 | HAL（装置の property）。preferences は Preferences の API（持ち主のプロセスの名前は読んだ文書に無い）。 | 設定 service。 | 各 server と、その設定 file。 | Settings の provider。audio policy。 |
| 保存 | レジストリー、CCD データベース（API の後ろ）。 | plist（API の後ろ。直接編集を求めない）。 | 設定 service が保つ（誰が保存するかはリファレンスに無い）。 | `~/config/settings` の file（利用者に見える）。 | 表（API の後ろ）。 |
| 利用者・機械の軸 | `HKCU`・`HKLM`、`CDS_GLOBAL`、device と user の方針。 | 利用者 × アプリ × ホスト、device channel と user channel。 | 方針の別名前空間と権限。 | `~/config/settings` と `/boot/system/settings`。 | `System`・`Secure`・`Global`。 |
| 権限の検査 | セッションとデスクトップ（画面の API）。応用の権限は文書に出ない（音）。 | 他の利用者の領域は特権。profile の除去は管理者の資格情報。 | 経路の宣言（`use`・`offer`・`expose`）。 | － | 権限の種類。システムが検査。 |
| カーネルの線 | 音声サービスとエンジンは利用者空間。ドライバーはカーネル。 | dext は利用者空間。kext は非推奨。 | ドライバーも能力の仕組み。host の配置は未確認。 | － | HAL と policy は作り手の build。 |
| 利用者が file を編む前提 | 無い（レジストリー編集は警告つきの管理者の道）。 | 無い。移植物の設定は例外として認める。 | 無い。 | 部分的にある（`kernel/drivers` の file）。 | 無い。 |

---

## 7. 共通の原則

資料の範囲で、五つの OS（と画面の API）が揃っている物だけを挙げる。

1. 能力、選択、実効の値を、別の問い合わせの口にする。
   - Windows は `IsFormatSupported`、`DeviceFormat`、`GetMixFormat`。画面は `QDC_ALL_PATHS`、`QDC_DATABASE_CURRENT`、`QDC_ONLY_ACTIVE_PATHS`。[W3][W10]
   - Apple は、ドライバーの `SetAvailable…` と `SetCurrent…`。[A5]
   - Fuchsia は、`GetSupportedFormats` と、クライアントが選ぶ `CreateRingBuffer`。設定の `source` は、利用者の値と、システムが決める値を分ける。[F6][F9]
   - Android は、`getSupportedMixerAttributes`、`setPreferredMixerAttributes`、開き直した stream。[D1][D5]
   - Haiku は `media_format` の交渉。[H3]
2. 能力を申告するのは、装置とドライバー。選ぶのは利用者か、利用者を代理するシステム。応用の選択は「要求」で、通ったかは別。
   - Android の `setCommunicationDevice` は、拒否されうる。[D3]
   - Apple は「listener が呼ばれるまで変わったと見なさない」。[A7]
3. 変更は「要求、適用、通知」の三つに分ける。値の変化は、通知が届いて初めて確定と見なす。
   - Windows の `IMMNotificationClient`、Apple の property listener、Fuchsia の hanging get、Android の callback。
4. 検査、一時の適用、保存を別の動作にする。
   - Windows の `CDS_TEST`、フラグ 0（一時）、`CDS_UPDATEREGISTRY`（保存）。`SDC_VALIDATE`、`SDC_APPLY`、`SDC_SAVE_TO_DATABASE`。[W9][W11]
   - 保存しない一時の適用は、戻しの手段でもある。`SDC_USE_DATABASE_CURRENT` で最後に保存した設定に戻る。[W11]
   - Haiku の Screen は、Apply の後、12 秒で自動で戻る。[H2]
   - Apple のドライバーは、構成の変更を HAL の手順に乗せ、変えてよい一点を決める。[A5]
5. 保存の形は、API の後ろに置く。呼び手に場所と形式を仮定させない。
   - Windows は「位置と形式は版で変わりうる」と明記する。[W13]
   - Apple は plist を直接変えるなと書く。[A1]
   - Fuchsia は、設定を service の FIDL で返す。[F5][F6]
   - Android は、表を `ContentResolver` の API で出す。[D6]
6. 利用者の選択と、管理者の強制は、別の層に置く。強制が先に効く。
   - Windows は `Policies` の鍵が `Preference` の鍵に先立つ。[W14]
   - Fuchsia は、方針を別の名前空間と別の権限で出す。[F7]
   - Apple は、管理された値と、`objectIsForced`。[A15]
7. 機械、利用者、装置、役割、アプリを、別の軸として持つ。
   - Windows：`HKLM` と `HKCU`、`CDS_GLOBAL`、三つの役割。
   - Apple：利用者 × アプリ × ホスト。
   - Android：`System`、`Secure`、`Global`。uid ごとの mixer の preference。
8. 権限の検査は、持ち主の側（システムのサービス、枠組み）が、入口で行う。
   - Fuchsia は、経路が無ければ能力が無い。[F1]
   - Android は、システムが権限を検査する。[D8]
   - Windows の画面の API は、セッションに触れない呼び手を `ERROR_ACCESS_DENIED` で断る。[W10][W11]
9. カーネルの外に、サービスと UI がある。ドライバーも外へ出す流れがある。
   - Windows の音声サービスとエンジンは利用者空間。[W7]
   - Apple の dext は利用者空間。kext は非推奨。[A5][A12]
   - Fuchsia のドライバーは、能力の仕組みの中のコンポーネントとして通信する。[F10]
   - UI がサービスと別のプロセスにある例は、Windows のセッション 0 の隔離。[W19]
10. 大きな音のデータは、一度確保した共有の記憶を、サービスと装置の間で共有する。
    - Windows の排他モード（WaveRT）、Apple の dext の IO buffer、Fuchsia の VMO。[W4][A5][F9]

---

## 8. OS ごとに違う所

- 一つの汎用の入口か、型つきの口か。
  - Apple は、`AudioObjectPropertyAddress`（選択子・範囲・要素）の一つの口で、能力、現在値、変更、通知を扱う。[A7][A8][A9]
  - Windows は、プロパティストアの鍵（`PROPERTYKEY`）と、型つきの COM の口。
  - Fuchsia は、領域ごとの FIDL の protocol。`Set` と `Watch` の形が揃う。[F6]
  - Android は、領域ごとの Java の API と、名前と文字列の表の二本立て。[D6]
- 権限の置き方。
  - Fuchsia は、権限を経路の宣言に一本化する。表も、環境にある権限も無い。[F1]
  - Android は、権限の種類の階層。
  - Windows は、アクセス制御の仕組みと、セッションの境界。
- 保存と file。
  - Haiku は、設定の file を利用者の目に見せ、一部は手で編む前提を残す。[H5]
  - 他の四つは、API の後ろに隠す。
  - Apple は、移植物のために、daemon の設定 file を編む道を認める。[A2]
- 管理者の層。
  - Windows は、グループポリシーと MDM の二つを持ち、衝突の規則がある。[W16][W18]
  - Apple は、構成プロファイルの device と user の channel。[A6]
  - Fuchsia は、方針を設定の変換として持つ。[F7]
  - Haiku の文書には、管理の層が無い。
- 戻しの扱い。
  - Windows の画面は、API の一部に「戻し」（`SDC_USE_DATABASE_CURRENT`）がある。[W11]
  - Haiku は、UI が時間で戻す。[H2]
  - Apple と Fuchsia と Android の音は、読んだ文書に戻しの仕組みが無い（`clear…` と `RemovePolicy` は、設定の取り消し）。

---

## 9. 各 OS が自ら認めている失敗（公式の文書にある範囲）

#### Windows

- Vista より前の音の作りは、応用が装置を推測する形で、ハードウェアの構成が想定外だと失敗しやすかった。エンドポイントの考えは、その反省の上にある。[W1]
- 排他モードは、システム音と他の応用の音を止めうる。文書は、焦点を失ったら速やかに手放すよう求める。[W4]
- 一部のドライバーは、同じ形式を `WAVEFORMATEX` なら通し、`WAVEFORMATEXTENSIBLE` なら断る。応用が二回問う回避が要る。[W3]
- レジストリー：誤った編集で OS の再インストールに至りうる。Microsoft は解決を保証しない。[W15]
- レジストリーの位置、形式、意味は、版で変わりうる。外の部品が重要な鍵を消したり、権限を変えたりして、利用者の問題になったと文書が書く。[W13]
- MDM と GP の同じ設定の衝突は、どちらが勝つかの保証が無い。[W18]
- `ChangeDisplaySettingsEx` の旧い API は、複数画面の変更のために、保存だけの呼びと、適用だけの呼びを組み合わせる手順を要する。[W9]

#### Apple

- kext は、落ちると機械全体を落とす。それで DriverKit へ移し、kext を非推奨にしている。[A12]（要約）
- Startup Items と mach_init の daemon は非推奨。[A13]（要約）
- 音のドライバーは、Monterey より前は二部品に分かれていた。AudioDriverKit がまとめた。[A5]
- preference pane は、`NSUserDefaults` を使えない（pane のアプリの領域に書いてしまう）。移植物の設定は file を直接編む必要がある。[A2]
- Core Audio の property の公式の文書は、説明の文章が空の頁がある。[A10]

#### Fuchsia

- StreamConfig、Codec、DAI の旧い audio の口は非推奨。Composite に集約している。[F8][F9]
- `devfs` は、新しい符号では勧めない。[F10]
- Audio の `Set` と `Watch` は、削除した。[F6]
- structured configuration は、利用者の操作に向かない。fleet management（多数の機械の管理）のような機能は、まだ作っていないと文書が書く。[F5]
- 設定のリファレンスは、同時の `Set` の衝突をどう扱うかを書いていない。[F6]

#### Haiku

- 新しく挿した USB の音声装置は、media_server を再起動するまで検出されない。活動報告が、未完の部分として書く。[H6]
- 新しい media add-on は、media_server を再起動して初めて使える。[H3]
- 音の不調の直し方として「Restart media services」を勧める。[H1]
- `Messaging Foundations` の頁は、送受信の節が未執筆。[H4]

#### Android

- `signatureOrSystem` は API 23 で非推奨。`privileged` の flag も、避けるよう文書が書く。[D8]（要約）
- 設定の書き込みは、API 23 で、利用者の個別の許可を要する形に締めた。[D7]（要約）
- mixer の属性を設定できるのは USB の装置だけ。[D1]
- Telecom を使うときに、`setCommunicationDevice` と混ぜないよう警告する。混ぜると音の問題が起きる。[D10]（要約）

---

## 10. 分からなかったこと

- Windows
  - Sound の画面が、選べる形式の一覧をどう作るか。
  - Sound の画面が、値を書く先（どのプロセスに、何で）。
  - エンドポイントのプロパティの保存先。
  - CCD データベースの保存先と形式。
  - Settings のアプリの内部構造。公式の文書は URI での起動だけを示す。
- Apple
  - 三つの property（`NominalSampleRate`、`AvailableNominalSampleRates`、`AvailablePhysicalFormats`）の公式の説明。
  - HAL を動かすプロセスの名前。
  - 既定の装置と形式の保存の持ち主。
  - 構成プロファイルの値と NSUserDefaults の合流の細部。
- Fuchsia
  - 設定の値を誰が保存するか（リファレンスに無い）。
  - Composite の口での形式と topology の問い合わせの細部。
  - `fuchsia.settings` のリファレンスの後半（100000 字より後）は読んでいない。
  - ドライバーを置くプロセスの構成。
- Haiku
  - preflet が server に値を渡す道。
  - `BMessage` を設定の保存に使う話は、検索の要約に基づく。
  - app_server と kernel の境界。
- Android
  - `System` と `Secure` が利用者ごとか。
  - 設定の provider の保存の持ち主。
  - `AudioDeviceInfo` の頁の本文（取得の結果が navigation だけだった）。
  - `AudioTrack.setPreferredDevice` が保証か助言か。
  - AudioFlinger と AudioPolicyService の呼び合い。

---

## 11. 出典（URL）

### Windows
- [W1] https://learn.microsoft.com/en-us/windows/win32/coreaudio/audio-endpoint-devices（本文）
- [W2] https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-properties（本文）
- [W3] https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-formats（本文）
- [W4] https://learn.microsoft.com/en-us/windows/win32/coreaudio/exclusive-mode-streams（本文）
- [W5] https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-roles（本文）
- [W6] https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-events（本文）
- [W7] https://learn.microsoft.com/en-us/windows/win32/coreaudio/user-mode-audio-components（本文）
- [W8] https://learn.microsoft.com/en-us/windows/win32/coreaudio/pkey-audioengine-oemformat（本文）
- [W9] https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-changedisplaysettingsexa（本文）
- [W10] https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-querydisplayconfig（本文）
- [W11] https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setdisplayconfig（本文）
- [W12] https://learn.microsoft.com/en-us/windows-hardware/drivers/wdf/introduction-to-registry-keys-for-drivers（本文）
- [W13] https://learn.microsoft.com/en-us/windows-hardware/drivers/install/accessing-registry-keys-safely（本文）
- [W14] https://learn.microsoft.com/en-us/previous-versions/windows/desktop/policy/implementing-registry-based-policy（本文）
- [W15] https://learn.microsoft.com/en-us/troubleshoot/windows-server/performance/windows-registry-advanced-users（本文）
- [W16] https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/group-policy/group-policy-processing（本文）
- [W17] https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-configuration-service-provider（本文）
- [W18] https://learn.microsoft.com/en-us/windows/client-management/mdm/policy-csp-controlpolicyconflict（本文）
- [W19] https://learn.microsoft.com/en-us/windows/win32/services/service-changes-for-windows-vista（要約）
- [W20] https://learn.microsoft.com/en-us/windows/win32/services/service-control-manager（要約）
- [W21] https://learn.microsoft.com/en-us/windows/apps/develop/launch/launch-settings（要約）
- [W22] https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/plug-and-play-registry-routines（要約）
- [W24] https://learn.microsoft.com/en-us/windows-server/identity/ad-ds/manage/group-policy/group-policy-preferences（要約）

### Apple
- [A1] https://developer.apple.com/library/archive/documentation/Cocoa/Conceptual/UserDefaults/AboutPreferenceDomains/AboutPreferenceDomains.html（本文を要約モデル経由）
- [A2] https://developer.apple.com/library/archive/documentation/UserExperience/Conceptual/PreferencePanes/Concepts/Managing.html（本文を要約モデル経由）
- [A3] https://developer.apple.com/library/archive/documentation/UserExperience/Conceptual/PreferencePanes/Tasks/Preferences.html（本文を要約モデル経由）
- [A4] https://developer.apple.com/library/archive/documentation/MusicAudio/Conceptual/CoreAudioOverview/CoreAudioEssentials/CoreAudioEssentials.html（本文を要約モデル経由）
- [A5] https://developer.apple.com/videos/play/wwdc2021/10190/（Apple の公式の WWDC21 の頁。要約モデル経由）
- [A6] https://support.apple.com/guide/deployment/intro-to-device-management-profiles-depc0aadd3fe/web（本文を要約モデル経由）
- [A7] https://developer.apple.com/tutorials/data/documentation/coreaudio/audioobjectsetpropertydata(_:_:_:_:_:_:).json（Apple の文書の data。要約モデル経由）
- [A8] https://developer.apple.com/tutorials/data/documentation/coreaudio/audioobjectaddpropertylistener(_:_:_:_:).json（同上）
- [A9] https://developer.apple.com/tutorials/data/documentation/coreaudio/audioobjectispropertysettable(_:_:_:).json（同上）
- [A10] https://developer.apple.com/tutorials/data/documentation/coreaudio/kaudiodevicepropertyavailablenominalsamplerates.json、同じ並びの `kaudiostreampropertyavailablephysicalformats.json`、`kaudiodevicepropertynominalsamplerate.json`（宣言だけで説明の文章が空）
- [A11] https://support.apple.com/guide/audio-midi-setup/set-up-audio-devices-ams59f301fda/mac（要約）
- [A12] https://developer.apple.com/system-extensions/ と https://developer.apple.com/videos/play/wwdc2019/702/（要約）
- [A13] https://developer.apple.com/library/archive/documentation/MacOSX/Conceptual/BPSystemStartup/Chapters/CreatingLaunchdJobs.html（要約）
- [A14] https://developer.apple.com/business/documentation/Configuration-Profile-Reference.pdf と https://support.apple.com/guide/deployment/review-device-management-payloads-dep5370d089/web（要約。「衝突は未定義」の文を載せた頁は特定していない）
- [A15] https://developer.apple.com/documentation/foundation/nsuserdefaults/1416306-objectisforcedforkey（要約）

### Fuchsia
- [F1] https://fuchsia.dev/fuchsia-src/concepts/components/v2/capabilities（本文を要約モデル経由）
- [F2] https://fuchsia.dev/fuchsia-src/concepts/components/v2/realms と https://fuchsia.dev/reference/cml（要約）
- [F3] https://fuchsia.dev/fuchsia-src/concepts/fidl/overview（本文を要約モデル経由）
- [F4] https://fuchsia.dev/fuchsia-src/concepts/process/namespaces（本文を要約モデル経由）
- [F5] https://fuchsia.dev/fuchsia-src/development/components/configuration/mechanisms（本文を要約モデル経由）
- [F6] https://fuchsia.dev/reference/fidl/fuchsia.settings（前半 100000 字を要約モデル経由。`AudioStreamSettingSource` は検索の要約）
- [F7] https://fuchsia.dev/fuchsia-src/development/settings/policy_concepts（本文を要約モデル経由）
- [F8] https://fuchsia.dev/fuchsia-src/development/audio/drivers/architecture（本文を要約モデル経由）
- [F9] https://fuchsia.dev/fuchsia-src/development/audio/drivers/streaming（本文を要約モデル経由）
- [F10] https://fuchsia.dev/fuchsia-src/concepts/drivers/driver_communication（本文を要約モデル経由）
- [F11] https://fuchsia.dev/reference/fidl/fuchsia.hardware.audio（要約）
- [F12] https://fuchsia.dev/reference/fidl/fuchsia.media（要約）
- [F13] https://fuchsia.dev/fuchsia-src/development/internationalization/i18n_preferences（要約）

### Haiku
- [H1] https://www.haiku-os.org/docs/userguide/en/preferences/media.html（本文を要約モデル経由）
- [H2] https://www.haiku-os.org/docs/userguide/en/preferences/screen（本文を要約モデル経由）
- [H3] https://www.haiku-os.org/legacy-docs/bebook/TheMediaKit_Overview_Introduction.html（本文を要約モデル経由。Be Book の古い文書）
- [H4] https://www.haiku-os.org/docs/api/app_messaging.html（本文を要約モデル経由）
- [H5] https://www.haiku-os.org/docs/userguide/en/filesystem-layout.html（本文を要約モデル経由）
- [H6] https://www.haiku-os.org/blog/waddlesplash/2026-09-10-haiku_activity_contract_report_august_2026/（Haiku プロジェクトの活動報告。公式のサイトにある開発者の報告で、仕様の文書ではない）
- [H7] https://api.haiku-os.org/classBMessage.html（要約）
- [H8] https://www.haiku-os.org/legacy-docs/bebook/TheSupportKit_Archiving.html（要約）

### Android
- [D1] https://source.android.com/docs/core/audio/preferred-mixer-attr（本文を要約モデル経由）
- [D2] https://developer.android.com/reference/android/media/AudioDeviceInfo（要約。本文は取得できなかった）
- [D3] https://developer.android.com/reference/android/media/AudioManager（要約）
- [D4] https://developer.android.com/reference/android/media/AudioMixerAttributes（要約）
- [D5] https://developer.android.com/media/platform/improve-audio-playback（要約）
- [D6] https://developer.android.com/reference/android/provider/Settings.System 、 https://developer.android.com/reference/android/provider/Settings.Secure 、 https://developer.android.com/reference/android/provider/Settings.Global（要約）
- [D7] https://developer.android.com/reference/android/Manifest.permission（要約）
- [D8] https://developer.android.com/guide/topics/permissions/overview と https://developer.android.com/guide/topics/manifest/permission-element（要約）
- [D9] https://source.android.com/docs/core/audio/implement-policy（要約）
- [D10] https://developer.android.com/develop/connectivity/bluetooth/ble-audio/audio-manager（要約）
