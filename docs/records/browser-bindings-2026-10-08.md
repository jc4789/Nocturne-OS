# ブラウザー bindings 修理（2026-10-08）

## 今回実装したもの

- ブラウザーの通常 JavaScript task 予算を 5 秒から 15 秒へ変更。`--js-budget-ms 1000..30000` で変更可能。共有 web API の未指定値は従来の 5 秒。無限ループ、入れ子 callback、microtask は同じ有限期限を共有し、compile/startup の制限は維持。
- UTF-8 一括 decode を native の二段階変換へ接続。巨大な一文字配列を作らず、streaming も 512 文字の小さい chunk に制限。XHR の text/json は元バイトを解放し、JSON parse 結果を再利用する。128 MiB の JS heap 制限を無制限化していない。
- secure context の CredentialsContainer を追加。空の検索は null、authenticator がない PublicKey 操作は明示拒否、abort は伝播。パスキー署名、資格情報保存、WebAuthn 完成を偽装しない。
- 実 Reddit の `document.forms[0]` 不足を、native DOM を読む live HTMLCollection と SameObject cache で修理。`ChildNode.replaceWith` を native insert/remove と custom-element reaction scope へ接続。Document の原子的な root 置換は明示未対応。
- script/module の data URL を HTTP 用 2 KiB buffer に切り詰めず保持する。登録済み Blob URL も文書私有 registry から実バイトを取得し、native 16 MiB/MIME/watchdog 制限を通して実行する。失効・別文書・未登録・MediaSource URL は executable Blob と扱わない。opaque base の相対 import は拒否。OS ファイル権限や一般 native fetch の許可を増やしていない。
- HTTP request target の 1 KiB/2 KiB 不一致を共通容量へ修理し、黙った切り詰めを拒否する。
- Baidu の初期 containing block で失われていた viewport 高さを子へ引き継ぎ、明示高さには min/max clamp を適用。通常の auto/min-only 親は indefinite のまま保持。
- 実 Window/EventTarget prototype 接続を追加し、Rocket Loader の prototype instrumentation が window にも届くよう修理。iframe browsing context/WindowProxy の完成ではない。
- `<style>.sheet` に native owner を持つ StyleSheet/CSSStyleSheet handle を追加。disabled は実 native CSS 再走査に接続。偽 cssRules や第二 CSS tree は作らない。constructed sheets、replace、adoptedStyleSheets、rule mutation は未対応。
- formAssociated の登録 metadata と四つの form lifecycle callback を保持し、サイト自身の ElementInternals polyfill に到達できるようにした。native attachInternals/form linkage は広告しない。
- 実 FormData の ordered entry list、append/set/delete/get/getAll/has、live iterator/forEach、USVString、Blob/File を追加。Fetch/XHR の共通 body 抽出へ、乱数 boundary・改行/引用 escape・UTF-8・16 MiB 事前計数を持つ multipart 実バイト出力を接続。`FormData(form)` と受信 body の `formData()` 解析は明示未対応。
- 重複 shadow `<style>` の expanded CSS AST を一つの arena snapshot 内で共有。各 sheet の order と shadow scope は独立 wrapper に保存。exact CSS/base/media 一致だけを再利用し、imports を持つ発行元は除外。再走査・失敗時は cache を破棄。32 MiB AST 上限は維持。

## SuttaCentral：公式コードから判断した境界

[公式リポジトリ](https://github.com/suttacentral/suttacentral)の commit `34391d1f2c351314895a3027d65964ad257c19c3` を限定取得した。`sc-action-items-universal.js` は Material の icon-button 等を先に import し、その後の `sc-menu-more.js` が element-internals-polyfill を import する。Material の formAssociated 登録自体を拒否すると、サイトの polyfill に到達する前に停止する。[実際の import 元](https://github.com/suttacentral/suttacentral/blob/34391d1f2c351314895a3027d65964ad257c19c3/client/elements/menus/sc-action-items-universal.js)、[polyfill import](https://github.com/suttacentral/suttacentral/blob/34391d1f2c351314895a3027d65964ad257c19c3/client/elements/menus/sc-menu-more.js)。

lock に一致する Material web 2.3.0 と element-internals-polyfill 3.0.2 の関連コードだけを取得し、tarball integrity を照合した。全依存の install やサイト全体の build はしていない。この公式 commit と配信 `main.js` が同じビルドだとは断定しない。

実配信 main.js は 567311 bytes、SHA256 `4b3d6030bdbd1f9ced9cc753c88ce372cbf5c0f44b4a62ebea1f31ac497b8786`。照合記録は `build/browser-bindings-20261008/sutta/official-source-comparison.md`。

multipart は [HTML の encoding algorithm](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#multipart/form-data-encoding-algorithm) と [FormData API](https://xhr.spec.whatwg.org/#interface-formdata) を参照。filename の改行を name/value と同じように正規化せず、指定された byte escape のみ行う。

## 実サイト確認：成功と未達を区別

全 VM は QEMU/WHPX、4 CPU、2048 MiB、隔離した起動媒体と `/data`。metadata に起動 kernel/initrd の hash と `user_data_attached:false` を保存。ユーザーの Hyper-V disk、アカウント、challenge URL/token を再利用していない。以下は段階別イメージの証拠であり、すべて同じ最終イメージで再実行したものではない。

| 実サイト | 到達した結果 | 残る境界 / 記録 |
|---|---|---|
| Google / 公開 Accounts | main page を描画。実 console で UTF-8 181 checks、XHR text/json、空資格情報・abort・非対応結果を確認。有限 6 秒処理が終了し、無限 microtask は 15 秒で停止 | 個人アカウントのログイン後 challenge は未確認。`bindings-google-after-01` / `bindings-budget-01` |
| DuckDuckGo | 実 `nocturne os` 検索結果を描画、forms/input を取得 | 全機能を保証しない。`bindings-duck-01` |
| Baidu | ロゴ y=-168→122、search form y=-20→270。viewport 高さを受け取り、スクリーンで上端切れの修理を確認 | native window title の文字化け等は残る。`bindings-baidu-after-01` |
| SuttaCentral | 白画面からロゴ・メニュー・三蔵カードを描画。旧 Window/CSSStyleSheet/登録拒否と FormData 未定義は最終実行で消失 | radio 部品の setValidity と空 message の例外が残る。`bindings-sutta-after-03`。console-file を /tests と誤指定したため補助入力が未実行、期待ログ欠落で QA は終了 1。正しいマウントは /data/tests。multipart は下記の補正実行で確認 |
| Reddit | 旧停止を除去し、最新画像でロゴ・検索欄・投稿本文・コメントと右側カードを描画。Blob module 拒否と CSS AST 32 MiB exhaustion をこの実行では再現しない | iframe/document.write、追加 constructor/TypeError、keepalive、reCAPTCHA timeout と赤いサイト内警告が残る。本文描画の改善であり、全機能の正常動作ではない。`bindings-reddit-after-02`。補助 console-file は同じパス誤指定で未実行 |

VM の終了 0 はサイトの合格判定ではない。画面と serial の両方を読んでいる。Wikipedia は利用せず、HTML5test の点数だけを成果として使っていない。架空の互換サイトも作成していない。

FormData の最初の Google 補助入力も同じ /tests 誤指定で終了 1。これらは製品の不具合と扱わず、未実行の probe を成功に数えない。

補正した `bindings-formdata-google-02` は実 Google ページの内蔵 console で、ordered entries / set / File / filename を六つの true、実 multipart の Content-Type・UTF-8 CRLF・filename escape・末尾 delimiter・binary payload を五つの true として確認。期待ログを取得し終了 0。POST は送信しておらず、native transport のアップロード成功までは主張しない。最新 kernel SHA256 は `8bac8211e21ebacdedc9fff91d07a19b8366c3ea974bdb6534e79d840acd5f80`、initrd は `2bf3a6fdd4bd52caf5e2df290eae5364d6e6bdf8be56ae468f5fae531a32301d`。

## 未完成を隠さない

- Sutta radio validator は detached `input[type=radio][name=group]` の message と独自 valueMissing を使う。実 stack が同じ module を指す。Material の base validator は省略 flag を明示 undefined にするが、配信および integrity 一致の polyfill `ValidityState.js` は `undefined !== false` で invalid と判定する。これが正常な空 message と衝突する直接原因。native input の値を捏造変更しても直らない。Nocturne の真正な未実装は、Web IDL の boolean default false を適用する native ElementInternals と native FACE のフォーム接続。サイト依存コードや正当な setValidity 拒否を改変して回避していない。
- FormData 自体は私有ブランドを使用するが、Blob→File clone が再利用する既存 File constructor には mutable RegExp/String/Array prototype 依存が残る。全面的な prototype 改変耐性は主張しない。
- `replaceWith` の完全な例外原子性・mutation record 合体、CSSOM 全体、native form-associated internals、iframes、認証器、背景 keepalive の完成は今回の実装に含まれない。
- MSE/media/GPU/SMP の既存変更は保持。この bindings 修理だけで全ウェブ機能や日常 OS 完成を宣言しない。UNIX/POSIX 互換を追加していない。

## ビルド

`python scripts/build.py -j4 --log build/browser-bindings-20261008/build-final-integrated.log`：終了 0。生成済み `js_bootstrap.inc` は新 include/private hook を含む。差分の whitespace check も終了 0。
