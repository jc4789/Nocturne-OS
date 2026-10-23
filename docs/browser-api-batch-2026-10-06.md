# ブラウザー API の一括拡張（2026-10-06）

## 方針と実装範囲

Lexbor 統合後の不足 API を、関連する機能群として実装してから一括検証した。
QuickJS は言語処理、Nocturne は DOM・通信・イベント・描画を担当する。
別の公開 DOM、外部ブラウザー、Unix/POSIX 互換層、fork、pthread は追加していない。
GUI・RAMfs・永続 `/data`・`/data/bin` の方式も変更していない。

### 属性ノードと名前空間

- `Attr`、live な `NamedNodeMap`、`Element.attributes` の添字・名前による参照。
- 属性ノードの生成・取得・設定・取り外し、namespace 系属性 API。
- native 属性レコードを唯一の保存先にして、所有要素・ownerDocument・オブジェクト同一性を保持。
- clone/import/adopt、MutationObserver、custom element の属性反応にも接続。
- IDL の属性反映は null namespace を参照。見かけの名前だけが同じ別 namespace の属性で
  id/class/style・フォーム状態等が変化しないよう修正。
- Lexbor 内部の属性 namespace（HTML/SVG/Math の文脈情報）と公開 DOM の namespace を分離。
- SVG の `viewBox`、XLink、属性の大文字小文字を描画側まで接続。

### トークンと HTML 要素インターフェース

- `DOMTokenList` の live な添字・列挙・検証・追加削除・置換・toggle。
- `classList`、`relList`、`sizes`、`blocking` を実属性に接続し、SameObject を維持。
- `HTMLMetaElement`、`HTMLLinkElement`、`HTMLStyleElement`、`HTMLBaseElement`、
  `HTMLTitleElement`、`HTMLHeadElement` の native prototype と属性反映。
- URL/base URI、title、media、link の disabled/rel/href 変更を既存の metadata/CSS 処理に接続。
- `relList.supports()` は実処理のある stylesheet だけを肯定。
  preload・modulepreload や CSSStyleSheet を、空の実装で対応済みにはしていない。

### Fetch と native HTTP

- `Headers` のリスト、guard、列挙、`Request`、`Response`、複製、bodyUsed、
  byte-backed body の text/json/arrayBuffer/bytes。
- BufferSource の指定範囲をバイナリーとして転送。既存 XHR の経路も維持。
- AbortController/AbortSignal の reason、any、timeout と native 要求の中止。
- 有効な HTTP method token、HEAD、追加メソッド、CORS preflight。
- redirect:error、各転送先での same-origin 判定、303 と POST 301/302 の method/body 処理。
- A→B→A のリダイレクトでも CORS/Origin の状態を保持し、same-origin credentials を復活させない。
- wire で実際の HTTP reason phrase と redirect/CORS metadata を返し、script 側で偽装可能な
  応答ヘッダーを private metadata として信用しない。
- 応答ヘッダーは 64 KiB まで完全保持し、4 KiB 以上は必要な分だけ動的確保。
  HTTP→worker→browser→QuickJS の全経路で完全な block を使い、CORS 判定や JS への公開時に切り捨てない。
  大きな wire 応答は新しい flag で交渉し、古い static client には黙った切り捨てを返さない。

## 未対応・制約

- Fetch の ReadableStream、Blob、FormData、opaque no-cors/manual 応答、background keepalive は未実装。
  未対応の body/options は明示的に拒否し、動作していない機能を成功扱いしない。
- native HTTP の上限は応答ヘッダー合計 64 KiB、個別行 16 KiB 未満。
  超過時は明示的に失敗し、途中の応答を成功扱いしない。要求ヘッダーの既存 8 KiB 上限は変更していない。
- 属性名には既存 native 表現の上限があり、新 API は UTF-8 で 128 byte 以上を明示拒否する。
  属性値・namespace URI に含まれる NUL も、黙って切り詰めず拒否する。
- CSSOM、現代 CSS の全面対応、Web フォント、動画再生、サイト全体の互換性は未完成。
- HTML5test の古い PI=comment 期待には合わせない。現仕様の ProcessingInstruction を別に検証し、
  古い期待との不一致は診断として残す。

## 検証結果

既存の全ビルド経路が終了コード 0 で完了。生成 JavaScript、raw 内の UEFI loader・kernel・initrd、
initrd 内の browser/webfetch が現在の生成物と一致した。raw と Gen2 向け VHDX の内容も一致した。
これは Hyper-V 実行試験ではない。ユーザーの Hyper-V VM、`build/data.img`、`hyperv/` の媒体は操作していない。

QEMU は UEFI、1 CPU、512 MiB、boot の私有コピー、毎回新規の scratch `/data`。
まとめて追加した機能を、Nocturne 内の TinyCC でコンパイルした一括試験で検証した。

| 対象 | 最終結果 |
| --- | --- |
| jstest（新しい属性・token・HTML 要素・Fetch API 群を含む） | 890 条件、失敗 0、90,586 ms |
| Lexbor native 木構築 | 895 条件、失敗 0 |
| 属性ノードと parser 再開境界 | 16 native 条件、失敗 0 |
| SVG の geometry/cascade/raster と DOM 名前空間 | 30 条件、失敗 0 |
| web / CSS supports | 99 / 143 条件、失敗 0 |
| native HTTP worker と実 HTTP fixture サーバー | webnettest 失敗 0 |
| messaging / import maps | 16 / 89 条件、失敗 0 |
| storage / cookies | 38 / 339 native 条件、失敗 0 |
| media policy / media | 106 / 12 条件、失敗 0。動画デコードの完成を意味しない |
| hover / 画像所有権 / metadata | 447 / 23 / 85 条件、失敗 0 |
| 画像取得 | 失敗 0、予期しないエラー 0、5 要求・5 完了 |
| allocator / sbrk / font / Shift_JIS | 53 / 17 / 83 / 26 条件、失敗 0 |
| QuickJS 割り当て失敗処理 | 207 条件、失敗 0 |

jstest の数は harness 側の集計で、各 JS ファイル内部の assertion 数を加算した値ではない。
新しい4群の内部集計は属性 253、token 180、HTML要素 247、Fetch 155 の計 835 条件で、全て通過した。
これらは jstest に含まれる試験であり、890 に加算した総数ではない。
旧 HTML5test の PI 期待は `MISMATCH` として残っている。点数を上げるため期待をすり替えてはいない。

最初の一括実行で見つかった、Lexbor 属性 namespace の扱い、SVG viewBox の読み取り、
書き換えられた global String への依存も修正して再実行した。
credentialed redirect 試験では、中間応答の ACAC が不足していた fixture を修正し、
production の CORS 判定を緩めず通ることを確認した。

さらに実サイト確認で、4 KiB 超過を明示拒否する中間ビルドでは GitHub の初期 HTML 自体が
読み込めなくなることを確認した。安全な拒否だけで完了とはせず、前述の動的な完全ヘッダー保存・
転送へ拡張した。6 KiB／20 KiB の正常応答、後半の CORS 重複、上限超過、Cookie 秘匿、
completion 後のヘッダー寿命、末尾 MIME を持つ同期 module を回帰条件に追加した。

## 実サイト

全て最終画像からサイトごとに新規 QEMU を起動し、実際の HTTPS 応答・配信 JavaScript・
Nocturne の描画を観測した。実サイト同士やビルド・回帰試験は並列実行していない。
上のローカル回帰試験を実サイト合格の代わりにはしない。

| サイト | 観測できたこと | 残る失敗 |
| --- | --- | --- |
| 通常版 DuckDuckGo `https://duckduckgo.com/?q=Nocturne+OS` | Lite ではない検索結果を表示。複数の配信 JS と posted task が動作 | サイドバー等の描画崩れ。検索欄のフォーカス・入力で例外と error boundary が発生し、再検索は成功しなかった |
| GitHub `https://github.com/` | 4 KiB 拒否を解消し、初期 HTML・module/React を読み込み、背景・見出し・フォーム等を部分表示 | Shadow DOM、document domain、atob 等の不足。後に native allocator 不足（約98.7 MBをJSに計上、128 MiB予算未満）を記録して `abort()` で終了。サイト対応済みではない |
| DeepMind `https://deepmind.google/` | 背景、ナビゲーション、ヒーロー見出し、リンクボタンを部分表示。配信 script と timer が動作 | 動画の遅延初期化で `load()` 不足による例外が繰り返される。動画・全操作・完全な描画は未達 |
| HTML5test `https://html5test.com/` | 公開 script が実行され、**121/555** を表示（前回 Lexbor 記録は119/555） | data: module、Navigation Timing 等が未対応。Parsing rules は2/5のままで、現仕様PIと2016年の期待との相違も残る |

Google 検索、YouTube、ChatGPT、百度百科はこの最終ビルドでは再試験していない。
**ブラウザー API 群の追加は完了したが、全 JavaScript／全 Web API／全サイトの完成ではない。**

### 実サイトから特定した次の共通課題

サイト固有の例外回避や空の stub ではなく、次の実処理が必要である。

- DuckDuckGo の [配信 JS](https://duckduckgo.com/dist/wpmv.6e4a2974b97069286559.js)
  8:503168 は locale の照合処理で `Intl.Collator` を生成する。Nocturne の同梱 Intl にはまだない。
  今回の検索欄エラーは、この照合処理に到達して発生している。
- GitHub の [配信 JS](https://github.githubassets.com/assets/ser-7c872b58ac9f1b39.js)
  2:12412 は未定義の元 `Element.attachShadow` を捕捉した wrapper の `call`。
  tooltip の connectedCallback から到達する。ShadowRoot、木・描画・イベントの実際の統合が必要。
- DeepMind の [配信 JS](https://deepmind.google/static/dist/site/site-302a9a062df19815a977.js)
  2:41265 は動画初期化の `load()`。codec を追加するだけでなく HTMLMediaElement の lifecycle が必要。
- `atob`/`btoa`、document domain の正しい扱い、実サイトの native allocation 失敗時に
  ブラウザーを終了させない処理も残る。JavaScript の watchdog や RAM を増やして隠してはいない。

配信ソースの位置照合は補助的にホストから認証なしで行った。これは Nocturne 動作の代用ではなく、
上表の動作・失敗そのものは QEMU の画面と serial に基づく。

## 証拠

- ビルド: `build/nocturne-audit/platform-build.log`
- 媒体とソースの同一性: `build/nocturne-audit/gemini-repair/final-identity.json`
- 最終一括試験: `build/nocturne-audit/gemini-repair/api-batch-full-headers-contracts/qa-serial.log`
- 完全ヘッダー拡張前の一括試験: `build/nocturne-audit/gemini-repair/api-batch-final-contracts/qa-serial.log`
- 修正前後の途中結果: 同じ親フォルダーの `api-batch-contracts1`・`api-batch-contracts2`・`api-batch-contracts3`
- GitHub 最終画面・serial: `build/nocturne-audit/gemini-repair/api-batch-github-final/`
- 通常版 DuckDuckGo の初期表示と検索欄操作: `build/nocturne-audit/gemini-repair/api-batch-ddg-final/`
- DeepMind: `build/nocturne-audit/gemini-repair/api-batch-deepmind-final/`
- 最終 HTML5test: `build/nocturne-audit/gemini-repair/api-batch-html5-final/`
- 中間ビルドの4KiB拒否画面: `build/nocturne-audit/gemini-repair/api-batch-live-github/github-result.png`

ログ・QEMU 私有媒体・画面はローカル検証成果物で、Git 管理対象にはしていない。
