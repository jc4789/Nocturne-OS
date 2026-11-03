# 実サイト互換性の拡大検証 — build19

2026-10-06。結論は **部分的な改善であり、一般のWebを正常に利用できる完成状態ではない**。
以前の4サイトだけで判断せず、Google、GitHub、HTML5test、指定された百度百科を追加して、実際の配信ページとJavaScriptをNocturneで実行した。

## 検証方法と保護したもの

- QEMU内のNocturne純正ブラウザー、512MiB RAM、1280×800。ホストのChromeによる表示を成果に数えていない。
- ビルド済みブート媒体のsnapshotと、試験専用に新規作成した別の128MiBデータ媒体を使用。ユーザーのHyper-V VM・既存のデータ媒体は停止・更新していない。
- GUI操作、実画面、配信JSの例外位置を採取。公開ソースをホストから補助取得した場合、それだけでゲストの応答全byteが同一だったとは断定しない。
- NocturneのGUI優先、RAM上での実行、`/data`永続、`/data/bin`によるアプリ起動を維持。Unix/POSIX互換層や`fork()`は追加していない。
- 自作fixtureはAPI境界・回帰の補助検査であり、実サイト合格の代用ではない。

## 追加4サイトの実測

| サイト | build19で実際に確認できたこと | 未達・失敗 |
| --- | --- | --- |
| [Google](https://www.google.com/) | ロゴ・検索欄・ボタンを描画。巨大化していたappsアイコンを修正。GUIで「Nocturne OS」を入力・送信した。トップのXHR未定義例外は再現しなかった。 | 検索先は異常トラフィック認証画面。`MessageChannel`未実装でreCAPTCHAが停止し、iframeも実表示できない。検索結果を利用できたとは言えない。appsのpopupも未確認。 |
| [GitHub](https://github.com/) | 検索URLを直接開くと結果を表示。結果の`bellard/quickjs`リンクから実リポジトリーに移動。`template.content.appendChild`による以前の停止は解消。 | CSS表示が大きく崩れ、Platformメニューと`/`検索は正常動作せず。Shadow DOM、bare module/import maps、`HTMLFormElement`等が不足。直接URL・通常リンクによる閲覧とJSアプリの動作は別。 |
| [HTML5test](https://html5test.com/) | `screen`未定義での停止を解消。実ページのJSが採点表示まで進み、**104 / 555**。Aboutリンク往復後も同じ点数を表示。 | 旧2016版の機能検出結果であって規格準拠率や実用性の証明ではない。偽陽性も排除していない。`performance.timing.navigationStart`等の例外は残る。 |
| [百度百科「三体」](https://baike.baidu.com/item/%E4%B8%89%E4%BD%93/5739303) | 白紙だった画面から、百科ロゴ・三体見出し・刘慈欣・中国語本文を描画。再読込でも表示を確認。 | 本体JSはAxiosの`HTMLAnchorElement.pathname`参照で停止。ACS内の`apply`例外も残り、対象APIは未特定。検索・リンク・音声等の操作は未確認。ウィンドウ装飾の中国語タイトルも`?`のまま。 |

画面と実ログ：

- Google：`build/nocturne-audit/compat18-google/build19-google/`
- 百度：`build/nocturne-audit/compat18-google/build19-baike/`
- GitHub：`build/nocturne-audit/compat18-github/github-build19-run/`
- HTML5test：`build/nocturne-audit/compat18-github/html5test-build19-run/`
- Google・百度の独立再検討：`build/nocturne-audit/compat18-google/svg-report-round2.md`

## 以前の4サイトも再確認

同じbuild19で通常版DuckDuckGo、YouTube、DeepMind、ChatGPTも実際に開いた。証拠は`build/nocturne-audit/js-live-19/`。

- **通常版DuckDuckGo**：`?q=quickjs+javascript`でヘッダー・検索語は表示するが、結果欄は空。DOMParser未定義は解消した一方、Reactのmutation commitを実行するtimerが5秒制限に達する。`vl/yl`の交互stackだけでは循環DOMの証拠にならない。直前scriptのprofileは停止したtimerを計測していないため、その数値から原因を断定しない。
- **YouTube**：10,845,860byteの本体を約11.3秒でコンパイル後、HTML要素の`async`setterで`NocturneDOMNode object expected`。白地に三本線だけで、検索・動画一覧・再生はできていない。以前のtemplate未定義は越えたが、ほかのメディア・要素型・タイミングAPIも未完成。
- **DeepMind**：トップの見出し・背景・ナビゲーション・ボタンを描画。ただしcookie通知と本体に例外があり、`localStorage`不足も確認。Modelsクリックで`/models/`へ遷移したが、遷移後画面の十分な待機確認は行っていない。トップ表示を全操作の合格とはしない。
- **ChatGPT**：ロゴと「Just a moment...」の認証前画面から先へ進まない。入力欄・会話機能に到達せず。この系列のログだけでは停止原因を特定できていない。

## 今回の共通実装

### XMLHttpRequestとNocturneの通信経路

`user/libc/web/js_xhr.js`から既存のNocturne非同期通信・取消経路を使用する。readyState、イベント、文字列POST、text/json/arraybuffer応答、timeout、abort、再利用と古い応答の排除を追加した。公開`fetch`やページの`Promise`差替えには依存しない。MIMEの引用付きcharset、孤立surrogateのUTF-8変換、資源不足時の状態巻戻しも検査した。

upload listenerが必要とするCORS preflightを、JS→browser→webnet→webfetchへ伝える。新しいOS ABIを増やさず、worker packetの既存64byte layoutを維持し予約bitを利用する。旧0/1値も有効。ただし永続ディスクにある旧実アプリを起動して確認したわけではない。

**未対応**：同期XHR、GET/POST以外、Blob/document応答、完全なstreaming progress、legacy charsetのXHRデコード。現在の応答はbufferedで、upload完了通知にも制限がある。XHR全規格対応を主張しない。

### 独立Document・DOMParser・template

`user/libc/web/js_document.js`とnative DOMに、独立したHTML文書、`DOMParser`、`createHTMLDocument`、clone/import/adopt、本物の`template.content` fragmentを追加した。logical ownerとallocation ownerを分け、adopt後もwrapper identityと割当寿命を維持する。解析だけの文書から通信・スクリプトを勝手に起動しない。

同一runtimeの文書群に64個・DOM arena/control計32MiBの上限を設け、拒否後も既存文書が利用できることを確認した。XML、Shadow DOM、完全なHTML parser/Custom Elements互換、不要文書arenaの早期GCは未完成。解析済みscriptをliveへ移した場合の全互換も未達。

最終読取で追加の未解決境界も見つかった。main文書で最初に解析したtemplate内の画像は、logical ownerをinert文書へ変更しても、物理的な`owned_nodes`リストに残る。画像の再走査・完了処理はこのリストを使うため、要求抑止やadopt後のimage indexに問題がある可能性がある。**まだnative再現していない候補**であり、独立inert文書の補助成功から、この境界まで安全と拡張しない。

### screenとSVG

- `screen`はNocturneの実framebuffer/workarea情報から取得。固定の架空画面サイズを返すものではない。
- SVG `viewBox`の読み取りが、Nocturneで未対応の`sscanf` scansetに依存していた。専用の有界4数parserへ変更。
- HTML側の小文字`viewbox`を、XML画像codecへ渡す際に`viewBox`へ調整。CSSの確定した親内寸・auto・intrinsic ratioを区別し、属性とCSS寸法の二重解釈も修正。
- Googleの9個の円は、中心間隔56–57pxから6pxへ正常化。サイト名判定やGoogle専用24px指定は追加していない。

## 補助回帰とビルド

Nocturne内のtccでコンパイル・実行した最終系列：

| 検査 | 結果 |
| --- | --- |
| JavaScript統合 | 820検査、失敗0。内側のXHR80・独立Document150 assertionと文書数/arena上限試験を含む |
| フォント / Shift_JIS / Web | 83 / 26 / 99検査、すべて失敗0 |
| SVG | 26検査、失敗0 |
| media policy / media events / hover | 106 / 12 / 447検査、すべて失敗0 |
| QuickJS割当失敗処理 | 207検査、失敗0 |
| 画像 | 失敗0、要求5・完了5 |
| native通信worker | 成功55件、失敗0。実OPTIONS/POST、拒否時に実要求を送らないこと、取消・deadline・reap等を含む |

証拠：`build/nocturne-audit/js-contracts-19b/qa-serial.log`、`build/nocturne-audit/webnet-19/qa-serial.log`。

初回build19ではSVG pixel fixture6件と旧Document prototype期待値が失敗した。ログは`js-contracts-19/`に残す。SVGの`fill=red/>`は正しくHTML上`red/`と解析されるためfixtureに引用符を付け、prototype期待値は実装した`HTMLDocument`継承へ修正。同じ製品imageで全回帰を再実行し、成功だけを選別して報告していない。

`make -j8 all`成功。`build/nocturne.img`、`build/nocturne.vhdx`、`build/nocturne.iso`を生成した。raw/VHDXの仮想内容、raw内kernel/initrd/UEFI loader、initrd内browser/webfetch、生成済みJS bindingと元ソースの一致を照合。

- 証拠：`build/nocturne-audit/build19.log`、`build/nocturne-audit/final19-identity.json`
- raw SHA-256：`8dce02c746d8267aedbd1755ba02e391c2b9b202c45da62e1982bd82907c28d3`
- これは限定したソース・生成物の同一性検証。Hyper-V Gen2の実起動はユーザー担当で未検証。稼働中Hyper-V媒体まで対象にする全リポジトリー監査ゲートは未合格のまま。

## 次に直す共通の不足

1. リンク要素のURL成分反映（`pathname`等）。百度Axiosで実停止位置を確認済み。
2. 要素ごとのIDL/prototype境界、`HTMLFormElement`、Shadow DOM、import maps。YouTube/GitHubの実停止を基準にする。
3. 実際のメッセージ配送を伴う`MessageChannel`、iframe、永続storage。空stubで対応済みにしない。
4. DDGの停止timer自身を計測し、DOM挿入・再走査の実負荷を分離する。単に制限時間を延ばして合格にしない。
5. CSS・メディア・フォントとGUI装飾の残る差を、JS例外とは別に検査する。

前回の履歴は[build17の記録](browser-native-progress-2026-10-06.md)を参照。
