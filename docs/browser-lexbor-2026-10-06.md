# Lexbor 統合と実サイト検証（2026-10-06）

## 実装範囲

Lexbor 3.1.0、リビジョン `f4cbbcd91359a0ec9499e3ce7e263de629482d61` を
ユーザー指定の複製から採用した。428 個の上流ファイルは未改変で、
`third_party/lexbor/UPSTREAM.json` に SHA-256 を記録している。

- ブラウザーの HTML tokenizer／tree builder を Lexbor に置き換えた。
  文書・文脈付き fragment・不正な HTML の回復処理に同じ入口を使う。
- JavaScript、CSS、リソース管理、描画が参照する DOM は引き続き Nocturne の
  `node_t`。Lexbor の木はパース中だけ保持する私有状態で、別の公開 DOM ではない。
- script 境界で停止し、ネイティブ DOM の変更を同じ Lexbor ノードに戻して再開する。
  detached な開放要素、active formatting、adoption、template の内容も対象とする。
  QuickJS を tokenizer callback 内から再入させない。
- `document.write` の入力寿命・順序を保持する。ただし従来の遅延処理方式を維持しており、
  ブラウザー標準の同期・再入可能な `document.write` 全体を完成させたわけではない。
- 現行 HTML の ProcessingInstruction を独立した type 7 のノードとして扱い、
  対象名・CharacterData・生成・複製・adoption・直列化を接続した。
  PI の疑似属性 API は未対応。
- `css`・`selectors`・`encoding` モジュールもネイティブビルド可能にしたが、
  **それらを既存の CSS 評価・セレクター・文字コード変換の代わりに接続したわけではない**。
  既存の UTF-8／Windows-1252／Shift_JIS 入力処理を保持している。

allocator は既存の Nocturne libc を使う。新しい syscall、POSIX 互換層、
`fork()`、pthread、外部ブラウザーは導入していない。GUI、RAMfs、永続 `/data` の方式は不変。
TinyCC のライブラリー・ヘッダーにも同じ実装を組み込む。

## 制限

入力と入力管理情報には各 16 MiB の制限がある。既存の再帰型描画処理を守るため
深さ 400 を超える木は拒否し、黙って平坦化しない。ネイティブ DOM の既存のメモリ予算とは別に、
Lexbor 自身の割り当て総量の予算管理はまだ必要。連続ナビゲーションのメモリ保持と、
一部のネイティブ割り当て失敗での `abort()` も未解決である。

## ビルドと検証環境

既存の全ビルド経路が正常終了。最終画像について、raw 内の UEFI ローダー・kernel・initrd、
initrd 内の browser／webfetch、生成済み JavaScript が現在の生成物と一致した。
`qemu-img compare` で raw と Gen2 用 VHDX の内容一致も確認した。
これは **Hyper-V 実行試験ではない**。

QEMU は UEFI、512 MiB、私有の boot コピー、毎回新規の scratch `/data` を使用。
ユーザーの稼働 VM、`build/data.img`、`hyperv/` の媒体は操作していない。

回帰試験は QEMU 内の TinyCC でコンパイルして実行する。
`tests/lexbortest.c` は木構築・script 停止再開・DOM 同一性・所有権を、
`tests/js_lexbor_cases.js` は実際の公開 HTML5test の tokenizer 条件を分解し、
追加の現仕様 PI 条件を検証する。これらのローカル試験だけを実サイト合格とはしない。

最終の対象試験では、ネイティブ `lexbortest` が **895 条件・失敗 0**、
JavaScript からの tokenizer／PI は **60 条件・失敗 0**。
同じ最終媒体の `jstest` 全体は **866 条件・失敗 0**（84,548 ms）だった。
これとは別に、未対応 `NamedNodeMap` の診断 2 条件と旧 PI 期待の診断 1 条件は
失敗したまま明示している。PI 追加後の全体試験では allocator 53、web 99、CSS 143、
storage 38、cookies 339、画像所有権 23、メタデータ 85、QuickJS 割り当て失敗 207 の各条件などが通った。

## 実サイトで見えたこと

以下の初回記録は PI 追加前の Lexbor ビルド。後述の最終再試験とは区別する。

| サイト | 観測結果 | 未達 |
| --- | --- | --- |
| 実際の HTML5test | 119/555、Parsing rules 2/5。木構築・標準モードが通った | tokenizer の旧 PI 期待、Element.attributes／NamedNodeMap、各種 Web API、data: module、Navigation Timing など |
| 通常版 DuckDuckGo `?q=Nocturne+OS` | Lite ではない検索結果が表示され、実配信 JavaScript と posted task が実行された | サイドバー・アイコン等の描画崩れ。メニュー操作成功は確認できなかった |
| DeepMind | 背景、ナビゲーション、記事ヒーローの部分表示 | 配信 script の TypeError、動画・描画・操作の不完全さ |
| GitHub（新規起動） | 配信された module／React が実行され、部分的なページが現れた | HTMLMetaElement・Request 等の不足、レイアウト崩れ。その後 `abort()` で終了 |
| Google（上記の連続ナビゲーション後） | JavaScript 初期化時にネイティブ allocator が不足し、webfetch の起動も失敗 | その実行条件では表示不能。新規起動した Google 単独の結果ではない |

PI 追加後のビルドで通常版 DuckDuckGo `?q=Nocturne` を再試験したところ、
約 5 秒の posted task が watchdog に達し、白画面になった。この実行は別 QEMU の
回帰試験と同時であり、原因を PI 変更やホスト負荷のいずれかに断定していない。
**前回の部分表示を根拠に DuckDuckGo を対応済みとは扱わない。**

HTML5test の公開 script は `<?import ...>` を comment とする古い期待を持つが、
現行 [HTML tokenizer](https://html.spec.whatwg.org/multipage/parsing.html#processing-instruction-open-state) と
[DOM](https://dom.spec.whatwg.org/#interface-processinginstruction) は ProcessingInstruction を定義する。
点数を増やすために旧期待へ戻すことはしない。旧期待は診断として保存し、現仕様の type／target／data を別に検証する。
また `Element.attributes`／`NamedNodeMap` はまだ実装されていない。
回帰試験では字句解析結果を既存の `getAttributeNames()`／`getAttribute()` で検証し、
公開 HTML5test が使用する `attributes[0]` の未対応は独立した失敗診断として残す。
これを HTML5test の合格や属性ノード API の実装とは扱わない。

YouTube・ChatGPT・百度百科は今回の Lexbor ビルドでは再検証していない。
**Lexbor 統合は HTML の基礎を改善したが、全 Web API・全サイト・描画・動画再生の完成ではない。**

## 証拠

- ビルドログ: `build/nocturne-audit/platform-build.log`
- 最終媒体の同一性: `build/nocturne-audit/gemini-repair/final-identity.json`
- 初回回帰試験: `build/nocturne-audit/gemini-repair/lexbor-contracts/qa-serial.log`
  （DOCTYPE の誤ったテスト期待を含む初回記録も残した）
- 初回のサイト表示・serial: `build/nocturne-audit/gemini-repair/lexbor-live/`
- GitHub: `build/nocturne-audit/gemini-repair/lexbor-github/`
- 最終回帰試験: `build/nocturne-audit/gemini-repair/lexbor-final-contracts/`
- PI 入力検証の修正後の対象試験: `build/nocturne-audit/gemini-repair/lexbor-final-pi/`
- 最終 DuckDuckGo 再試験: `build/nocturne-audit/gemini-repair/lexbor-final-live/`

これらはローカル検証成果物で、Git 管理対象にはしていない。
