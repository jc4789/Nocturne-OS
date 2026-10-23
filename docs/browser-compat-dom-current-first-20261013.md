# DOM共通境界・現契約の第一報（2026-10-13）

現契約は DuckDuckGo／Reddit／GitHub／百度百科（ホームと戦鷹記事）の共通修理である。旧3サイト契約の第一報は保全し、範囲変更後のsource-bound cycleを新しく登録する。

実装は `docs/browser-compat-dom-first-20261013.md` と第二反例記録 `docs/browser-compat-dom-second-20261013.md` にある。JavaScript modulepreload、既存module map／native pending再利用、SRI、requested URL／redirect base分離、世代取消、失敗実解放、native wait ABIを製品へ接続した。QuickJSのparse-only専用flagと明示resolveを使い、preloadのroot loadが依存取得・作者評価を一律に待つ初案を修理した。

IntersectionObserverとCSSOM Viewは既存fragment unionを共有し、slotted targetのexplicitrootは実containing-block chainで扱う。サイト名による特例やvisible結果の偽装は入れない。JSON/CSS/text型module・worker destination・CSP全体の機能拡張は本差分では保証しない。

正規embed、対象構文、diff確認は成功。実guest最初のmodule原WPTはrunnerの同期module host不足で未判定、native jstest選択は不在binaryのexit127で未実行だった。rootがそれぞれ正規実native transport／TinyCC経路を修理中。IO変更は旧媒体の結果へ流用せず、新媒体の独立fixtureと原WPT2ページで確認する。

Redditでは実slot割当とlight subtree存在を確認したが、部分資源内容が空で左欄は未達。4サイトの最新表示・入力・上下scroll・速度・回帰は主担当の実QEMU確認を待つ。構文／資源数／他担当の成功件数をこの受入の代わりにしない。
