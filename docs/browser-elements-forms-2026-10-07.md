# HTML要素・フォームの拡張（2026-10-07）

## 方針

HTML5test の Elements / Forms の不足を優先したバッチ。API名だけを置いて検出結果を変えるのではなく、Nocturne の DOM・描画・入力・送信処理へ接続する。QuickJS は言語エンジンとして使い、Unix/POSIX互換層、fork、スレッドは追加しない。RAMfs・永続 `/data`・GUI-first の構成を維持する。

## 実装範囲

- **要素の識別**: `HTMLUnknownElement` と既知HTML要素のprototypeを区別。section/nav/article/aside/header/footer/main/figure/figcaption/mark はもともとのnative解析・描画を維持する。HTML5test の「未知要素でないか」の検査中に発生していた未定義constructorの例外を解消する。
- **意味的要素**: time.dateTime、data.value、ol.reversed/start/type、li.value。逆順リストの実際の番号付け、wbrの任意改行を描画側へ接続。
- **details/summary**: 閉じた内容のbox生成を抑止し、最初のsummaryと既定の見出しを表示。マウス・キーボード・JSによる開閉、同名グループ、非同期toggleと連続変更の集約に対応。通常の内容部分やsummary内のボタン操作では勝手に開閉しない。
- **input値モデル**: date/month/week/time/datetime-local/number/range/color の値処理、valueAsNumber/valueAsDate、stepUp/stepDown、min/max/stepを共通のnative実装へ接続。number/dateの途中入力と公開する正規化済みvalueを区別し、`-`、`1e`、未完成の日付が入力途中で消えないようにする。上下キーも同じstep処理を使用。
- **検証と送信**: live ValidityState、checkValidity/reportValidity/setCustomValidity、required、email/url、pattern、長さ・範囲・刻み幅、badInput。invalid/submitイベント、requestSubmit、novalidate/formnovalidate、resetへ接続。直接のform.submitは仕様通り検証・submitイベントを迂回する。
- **検証UI**: 不正な入力へフォーカスし、必要ならスクロールしてステータス欄へメッセージを表示。イベント中に修正・削除・移動された古い要素を報告しない。
- **フォーム所有者と無効化**: form属性による外部controlの関連付け、fieldset/legendのdisabled例外を共通処理へ。存在しないform IDの祖先fallbackを排除。
- **select**: optionごとのselectednessとdirty状態、multipleの複数値送信、disabled option/optgroupの除外、sizeの初期選択規則、reset。live options / selectedOptions、HTMLOptionsCollection、Option、length/add/remove/options[index]。
- **CSS**: :valid/:invalid、:required/:optional、:in-range/:out-of-range、:read-only/:read-write、disabled判定をフォームの共通状態と整合させる。

検証用の正規表現とURL parserは、DOM・ネットワーク権限を持たない専用QuickJS realmを使用する。既存のWHATWG URL parserを再利用し、16 MiBのruntime heap上限、正規表現source/入力上限、実行期限を設ける。初期化失敗時のcontextは破棄し、OOMなどを「入力が正しい」に置き換えない。

## 今回の未達

- 日付・時刻・色・ファイル選択の専用picker UI。現在のcolorは従来の不透明 `#rrggbb` で、alpha/colorspace/P3拡張は未対応。
- multiple selectの本格的な一覧・範囲選択UI、select自身のindexed access（options[index]とは別）、labels/RadioNodeList、ElementInternalsによるform-associated custom elements。
- multipart/fileアップロード、dialogのmodal/top-layer、ruby組版。これらはconstructorや検出の成功をもって対応済みとはしない。
- details同名peerの自動open属性除去について、MutationObserverの属性record伝達は不完全。
- HTML5test掲載の古いメニュー、HTML imports、Application Cache等を、得点目的だけで追加しない。

## 検証記録

HTML5test自身も、日時・数値・色の専用UIを直接操作せず、値のsanitization成功からUI対応を推定している。そのため今回の得点が上がっても、picker UIの完成を意味しない。得点・native境界回帰・実際のGUI操作は分けて記録する。

一括ビルド成功。専用QEMU UEFI・2 GiB・新規scratchディスクで、選択/focus修正後の関連10スイートを一括検証し、失敗0。ただしこの段階では下記のhoverクラッシュが残っていたため、完成画像とはしなかった。

- jstest: 927 checks / 0 failed（449443 ms）。内包するJS群では controls 667、validation 365、semantic elements 290、Shadow 156、Collator 7287、Fetch 203 の検査を実行。
- native controls 92、semantic layout/paint/focus 131、web 189、CSS 143、Lexbor 895、Shadow 61＋native 71、属性native 16、SVG 30、すべて失敗0。
- 初回の広範囲バッチでは、時刻の明示stepを秒ではなく既定の分単位として期待した回帰側の誤りを検出・修正。別途レビューでselect移動時の空選択消失、checkboxのキー操作・summary子要素クリック時のfocus不整合を修正し、最終バッチへ反映。
- 初回の周辺検証では通信、ストレージ、画像、メディア方針、TCP接続枠36項目、QuickJS OOM 207項目なども成功。ただし、その初回画像と選択/focusを修正した最終画像は別である。

ログ: `build/nocturne-audit/gemini-repair/elements-forms-batch-01/qa-serial.log` と `elements-forms-final/qa-serial.log`。ローカル回帰は境界条件の検査であり、実サイト互換性の証明ではない。稼働中のHyper-V媒体・ユーザーの永続データは変更していない。

### 実サイトと追加修正

実際の `https://html5test.com/` をNocturneで取得・実行し、合計 **168/555**、**Elements 23/30**、**Forms 54/65** を表示。ユーザー提示の Elements 4/30・Forms 31/65、および前バッチの合計126/555から改善した。

ただし、その後のマウス移動でブラウザーがクラッシュ。原因は今回追加したSubmitEvent辞書処理の `strcmp(event->type,"submit")` が、型名を持たないnative hover入力を考慮していなかったこと。同じクラッシュは初回バッチのhovertestにも出ていたが、失敗件数中心の確認で取りこぼしていた。NULLを許す共通入力契約に修正し、専用harnessも各試験の終了コードを明示記録するようにした。これは実サイト成功として扱わず、再ビルド・追加回帰・実サイト再操作の対象とした。

初回の実サイト証拠: `elements-forms-html5-live/html5-live-score.png`、`html5-live-elements.png`、`html5-live-forms.png`、同ディレクトリの `qa-serial.log`。ディレクトリは `build/nocturne-audit/gemini-repair/` 以下。

**hover修正後の最終確認:** `elements-forms-hover-final/qa-serial.log` で、hover **447**、native controls **92**、semantic **131**、web **189**、Shadow **61＋71** の検査が成功。6試験すべて終了コード0、ゲストfaultなしを確認した。先の927項目のjstestはこのNULL guardを加える直前の画像での結果であり、同じ最終画像で全スイートを再実行したとはしていない。

続いて同じ最終画像で実サイトを再取得し、**168/555・Elements 23/30・Forms 54/65** を再確認。ページをスクロールし、マウス移動・クリックによるdate詳細の展開→折りたたみまで確認した。再現操作ではブラウザークラッシュなし。

証拠は `elements-forms-html5-fixed/html5-fixed-score.png`、`html5-fixed-elements.png`、`html5-fixed-forms.png`、`html5-fixed-date-settled.png`（展開）、`html5-fixed-date-collapsed.png`（折りたたみ）。なお、data: moduleの拒否と計測スクリプトのperformance.timing不足は引き続きログに残る。これらを含めた全面互換性は未達。

同じブラウザープロセスで通常版DuckDuckGoへ移動し、検索欄を `cats` から `dogs` へ編集してEnterを押すと、URL・タイトル・検索結果がdogsへ切り替わった。証拠: `ddg-forms-final-cats.png`、`ddg-forms-final-edited.png`、`ddg-forms-final-submitted.png`（同ディレクトリ）。ただしJSタスクの5秒watchdog停止（5583 ms / 5422 ms）と、レイアウト・サイドパネル表示の崩れは残るため、通常版DDGの全面動作を確認したとはしない。Google/GitHub/YouTube/ChatGPT等は今回のFormsバッチで再合格したことにはしていない。

## 生成物

`build/nocturne.img` / `build/nocturne.vhdx` / `build/nocturne.iso` を生成済み。`final-identity.json` により、埋め込みJS・フォームURL realm・kernel・initrd・UEFIローダー・browser/webfetchの一致と、raw/VHDXの内容一致を確認した。確認用QEMUは停止済み。Hyper-Vそのものは起動・更新しておらず、ユーザーによるGen 2検証とは区別する。

## 仕様の参照先

- [HTMLのフォーム制約検証](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constraint-validation)
- [input要素](https://html.spec.whatwg.org/multipage/input.html)
- [フォーム関連要素](https://html.spec.whatwg.org/multipage/form-elements.html)
- [HTML要素とDOMインターフェース](https://html.spec.whatwg.org/multipage/dom.html#elements-in-the-dom)
