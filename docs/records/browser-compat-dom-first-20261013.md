# DOM・modulepreload 第一確認（2026-10-13）

## 依頼と境界
- DuckDuckGo、Reddit、GitHub、追加の百度百科で、DOM/CSS/資源の共通境界を修理する。サイト専用の条件分岐、DOM差替え、人工的容量制限は入れない。
- この担当は modulepreload と既存 DOM 更新通知を実装。CSS条件は css_compat、CSS供給とフォントは font_load、描画・レイアウト・実QEMU受入は root。
- 初期 HEAD は 0519429。前回性能実装は保全する。

## 原資料と観測
- 提供12枚で Reddit の左欄が空白、GitHub が未スタイル→途中骨格→本文の順に長く変化することを確認した。
- GitHub の公開原HTMLを取得して、modulepreload 113個、外部 module script 9本を確認した。これは取得時の資源構成であり、遅延の全原因や実製品の速度改善を証明しない。
- 原HTMLには一時的なフォーム／challenge情報が含まれるため、原文の証拠保存や転載を行わず、作業用の2ファイルだけを解決済み build 内の通常ファイルと確認して削除した。
- Reddit のホスト取得は challenge 本文だった。突破やUA偽装をせず、root の実QEMU読み取り専用ログを使う。
- `build/nocturne-platform/compat-diag-reddit-20261013/serial.log` では左欄ホストが幅272、高さ約996、display:block。shadow SLOT は display:contents。slot自身の0寸法は子の消失証拠ではない。割当と内側の CSS を追加観測待ち。

## 製品修理
- `user/libc/web/js.c`: JavaScript modulepreload を既存 native pending／module map に接続。URL同一の複数link、script、importは一つの取得へ参加する。parser挿入とDOM構造／rel・href変更を反映する。
- rootの受領時は COMPILE_ONLY で解析し、作者コードは評価しない。link load/errorはroot取得の後に独立した要素taskとして発行し、文書loadは未使用preloadを待たない。任意の依存先行取得は行わないが、作者が明示した各linkは並列に送る。
- `user/include/web.h`: `wait_request` は既存要求を native network/UI のみでjoinするABI。完成フラグは realm 所有の pending record から借りる。待機中に web_tick／process_pending／作者JS／文書破棄へ再入しない。必要な依存のみ待ち、113資源全体の完了を一律に待たない。
- requested URLがmodule mapの識別子、redirect後URLが相対import／import.metaのbase。CORS／credentialsは既存MODULE transportへ渡す。use-credentialsを同じ経路へ反映。
- `js_fetch.js` の私的SRI実装を共用し、module root bytesが公開される前に検査。未知hashの処理、より強いhash優先、opaque response保護、Fetch側abort検査を保持する。
- network／MIME／SRI失敗は module map 検索から除外し、後の取得を許可。現在のpending／link待ちだけが失敗recordを保持し、最後の利用者で解放する。解析エラーはmodule recordに保持するが、modulepreload自体のloadをfetch errorへ偽装しない。
- input stream世代、encoding restart、文書退役、pending解放を追従。linkの構造索引は既存native構造通知とrel/href通知だけで無効化し、通常のrAF style書込みに全DOM走査を追加しない。
- `js_html_elements.js`: relList.supportsに実処理のmodulepreloadを登録。
- JavaScript module destinationが対象。JSON/CSS/text型moduleは既存engineで未対応のため成功を偽装せずerror。全modulepreload仕様適合とは報告しない。

## 規格
- [DOM の木・接続・Shadow tree](https://dom.spec.whatwg.org/#node-trees) を確認。論理DOM、shadow-including接続、flat tree、CSS box寸法を混同しない。
- [HTML modulepreload](https://html.spec.whatwg.org/multipage/links.html#link-type-modulepreload) と [module map／single module fetch](https://html.spec.whatwg.org/multipage/webappapis.html#fetch-a-single-module-script) を実読。
- 原WPT `preload/modulepreload-sri.html`（1頁1case）の失敗後再取得を反例として使い、失敗を恒久cacheする初案を修理した。原本のassertion／scriptは変更しない。

## 確認結果と限界
- `python -X utf8 user/libc/web/js_embed.py`: 正規生成、exit 0。
- 正規Makefileと同じtarget/includeで `clang -fsyntax-only user/libc/web/js.c tests/jstest.c`: exit 0。既存IndexedDB論理括弧warning 1件。
- CRLF対応 `git diff --check`: exit 0。
- 既存 `tests/jstest.c` に選択 `modulepreload` を追加。二重link/scriptの取得・評価1回、rel後付け、SRI失敗再取得、必要dependencyのみjoin、待ち中作者callback無しを製品経路で確認する準備。
- この第一確認時点では限定guest、原WPT、修理後の実サイトは未実行。構文成功を動作・速度改善・無回帰へ昇格しない。

## 次
rootがまとめて最新媒体をビルドし、限定native選択と原WPT、4サイトの同条件表示・実操作を確認する。第二確認は失敗寿命、割当／接続、native waitの再入反例を扱う。
