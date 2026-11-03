# Reddit partial と共通 Fetch 境界

## 反証と未達

`build/nocturne-platform/compat-constructor-reddit-20261013/serial.log` の
NAV_MIN_STATE は作者 partial constructor 成立、request object 成立、
registry identity 一致、lazy controller 成立を確認。後の2157..2158行は
両controller取消済み、loading／requestInProgress false。
従って「DOM生成なし」「custom element upgrade失敗」「IOが発火しない」
だけで左sidebarを説明できない。左sidebar空は依然未達。
原コードは configureLoader の Promise、request response、MIME
`text/vnd.reddit.partial+html`、response.text、fragment挿入を経る。
timeoutログは存在するが、どのPromise由来かは未確定。

## 確定した別不備の製品修理

`js.c` の resource診断kind名配列は7名だったが、既存policy-reportは
enum7、新moduleはenum8。取得失敗時に範囲外ポインタを `%s` に渡す
メモリ不備があった。9名対応と index 検証を実装。
native request／DOM処理を握り潰したり、失敗を除外していない。
js.c freestanding clang syntax-only exit0（既存IndexedDB括弧warningのみ）、
CRLF許容diff check exit0。これを左sidebarの確定原因とは報告しない。

## 次の原境界観測

既存 `tests/compat_site_observer.js` 先頭へcapture listenerを追加。
作者 load-start／response／load／error／alert eventを24件まで記録し、
type、target tag、boolean、status、MIME mediatype、HTTP(S) publicPath、
Error.nameと既知timeout／MIMEエラーのbooleanだけを出す。
Cookie、任意meta、本文、query、fragment、URL userinfo、私的runtime値を
保存しない。作者 DOM・style・function・request は差し替えない。
これは未実行の追加観測であり、rootの次全build VMで結果を得て
設定／通信／stream／fragmentの実故障を修理する。ログ追加で完了とはしない。
