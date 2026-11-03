# ネイティブfocusとtextarea寸法：第一報

## 製品修理

- 実GitHubの読取診断は通常のbuttonに到達し、ReactのonClick関数を持つが、実押下後もactiveElementはBODYだった。details/summaryではないため、aria-controlsをUAが開閉する特例は入れない。
- `browser.c`は未取消の主ボタンmousedown後に、既存DOMのfocusable areaを解決して一度だけfocusing stepsを実行する。`doc.c`の既存focus・slot・delegatesFocus・frame所有者検証を再利用し、SVG/inline子をイベントtargetのままにして親controlへfocusする。disabled/inert/失活文書は現在のfocusを動かさず、通常の背景はviewportを選ぶ。
- 既存click後の入力、checkbox/radio、details、media、select、背景の再focusを撤去した。click作者が選んだfocusを奪わない。labelの既存転送、フォームのpreactivation/reset、navigationは保全。実キーボードactivationは既にfocusしたtargetを用いる。
- `js_pointer_events.js`ではpointerdown取消に伴う互換mousedown抑制がnative focus許可を返さない。後のclickまで取消す変更はしない。
- textareaはnative値が通常のtext runにならず、height:0のscrollHeightが0へ潰れる実装だった。`layout.c`の既存font/line-height/grapheme/改行責務に、値の行配置を統合。空行、末尾改行、文字幅、折返し、wrap=off、CSS nowrap、paddingからscroll areaを算出する。既存のrows/colsとatomic intrinsic sizeは変更しない。
- `paint.c`も同じ行配置を利用する。折返しと硬い改行を分け、選択範囲とcaretをbyte位置から同じ行へ配置する。native scrollLeft/scrollTopを文字の原点へ反映し、controlの固定padding clipは保つ。placeholderの色/透明度とfontは既存styleを使う。値やDOMを差し替えない。

## 担当源

`user/apps/browser.c`（mouse_down/page_click/page_focus）、`user/include/web.h`（focus候補契約）、`user/libc/web/doc.c`（focus候補）、`user/libc/web/js_pointer_events.js`、`user/libc/web/layout.c`（control文字配置/scroll area）、`user/libc/web/paint.c`（control文字描画）、`user/libc/web/webi.h`（共有行記述）。rootのscroll診断、CSS/box局所化、Fetch/referrer、以前のserializer・DOM寿命修理は保全した。生成incは編集していない。

最終の規格再読で、`css.c`の既存pres_hintsにwrap=off→white-space:preを局所追加した。既存UAのpre-wrapは重複しない。共有文字配置はcomputed white-spaceだけを消費し、作者CSSがhintを上書きできる。textarea内部のpadding clipに閉じたscroll areaは、外側overflow:visibleでも親へ伝播させず、outer sizingを変えない。

## 支持結果と元の失敗

- exact product sourceの軽いhost確認：Event 14、pointer取消11、Worker Event 14条件は成功。最初のfixtureはローカル変数nativeが既存Worker変数と衝突してSyntaxErrorになったため、aliasのみ修正した。期待と元のWorker条件は保全。
- target構文：doc/browser/focus fixture、layout/paint、textarea fixtureは成功。元の最初の構文実行はfixture用include不足、次のlayout実行はform_value.h未接続で失敗。includeを正しい既存責務へ接続した。原失敗と修正後ログを別に保存した。
- 修正後ログ：`build/dom-focus-syntax-includes-20261013.log`、`build/dom-control-text-syntax-include-20261013.log`、`build/dom-control-text-test-syntax-20261013.log`。既存sandbox unused警告2とline fragment initializer警告1は残る。
- `tests/js_form_controls_native.c pointer-focus`にはDOM候補のnative条件と、実QuickJS/native textarea scrollの27条件を登録した。**まだゲストで実行していない**。空行、改行、wrap、grapheme、0幅の進行、padding/border、selection、scroll clamp、作者autosize、display:none、切断/再接続、Shadowを含む。
- 原24条件のwrap=off期待は、作者pre-wrapを直接属性で負かす誤りだった。実行前の仕様査読で誤りを確認し、hintを検査する条件では作者white-spaceを外した。作者override1条件と、内部領域/親への漏出2条件は追加し、合計27をexactにする。原24を成功した試験として報告しない。
- 最終追加修理のC4入力構文・JS構文・CRLF diffは成功。`build/dom-control-text-final-syntax-20261013.log`には既存警告4件が残る。
- `tests/js_event_state_hosttest.js`は座標/DOM所有を薄いstubで供給する支持試験であり、実GUI入力の証拠ではない。

## 目視した原画・未達

自分で開いた `compat-button-target-github-20261013/screen-9.png` は右上・About・ellipsisを保全するがPlatform menuは非表示、矢印は右向き。`compat-abort-interaction-reddit-20261013/root-typed-current.png` は左nav復旧、検索panel内の文字が不可視。これらは修正前の実症状であり、今回の源による改善証拠ではない。

最新生成・all/媒体更新・native支持・実menu/検索入力の再受入はrootが統合して行う。全ページ性能回帰解消、menu開封、検索完了、Hyper-V実機は未確認。Fetch Abort/transportは別担当、CSS rotateも別の不足として保持する。

## 規格根拠

- [HTMLのclick focusabilityとfocusing steps](https://html.spec.whatwg.org/multipage/interaction.html#click-focusable)。synthetic clickをfocusに昇格しない。
- [UI Events Algorithmsのnative mousedown](https://w3c.github.io/uievents/event-algo.html#handle-native-mouse-down)は既存挙動を整理する草案であり、HTMLの規範契約と区別した。
- [CSSOM View scrollHeight](https://drafts.csswg.org/cssom-view/#dom-element-scrollheight)。CSSの実heightと内容のscroll areaを混同しない。

## 統合nativeの原失敗と検査伝達修理

`build/browser-local-layout-stream-focus-native-20261013/test-serial.log`ではpointerfocus外側検査が必須出力欠落で不合格だった。内部は `API batch js_element_scroll_cases.js`、末尾26条件/0失敗で、期待の `Textarea native scroll checks 27` は出力されなかった。これを原FAILとして保持する。

`jstest.c receive_log`は正常の `OK `をmarkとして保存し、計数行は既存の ` API checks `などの公開ログ契約だけを印字する。今回の計数行はその契約に合わず抑制されていた。共有harnessへ別prefixを足さず、担当fixture一行の表示名を `Textarea native scroll API checks `へ修理した。rootのruntests必須markerも同じ文字列へ更新済み。exact27、全27条件、例外/FAIL伝達、api-done、native候補は変更していない。製品focus/textarea/CSS/描画源も変更していない。

この変更後のnative再実行は未確認。rootの単独百度性能観測を汚さないため、新VM/重い支持/全repo inventoryの反復は止め、最終同期と報告再登録は主担当の観測終了後に行う。

## 訂正後の最新結果

`build/browser-stream-focus-fixture-native-20261013/test-serial.log`で外側 `PASS pointerfocus/native-area-and-textarea-scroll`、計数 `Textarea native scroll API checks 27`、nativeプログラム末尾 `26 checks, 0 failed`を読んだ。27はJS textarea条件、26は候補とharnessを含むnative集計であり、同じ集合の件数ではない。原marker欠落のFAILを削除せず、訂正された伝達契約での新しい支持結果として扱う。このrun全体は通信の保持本文条件に1FAILがあり、全native成功とはしない。

主担当rootからは新QEMUの実操作/原画で、GitHub Platformのmenu開封、Reddit可視codex入力/候補・Enter後の対応検索本文・PgDn/PgUp成立が報告された。私はその新実操作/原画を自分で確認していないため、root観測として区別する。

単独百度はtask41104ms/layout40968ms/87flushが残り、全体性能は不合格。今回の機能支持、menu/入力の受入を性能回帰解消へ昇格しない。以前の元Worker複合fixture失敗も別件として保全する。

前の State lock timeout / pending broadcast のwork BLOCKはその時点の失敗として保持した。観測終了後の新read/view/workはfingerprint `075ed8fdb17e2c888d177f81af787b91d8810cd5820a8010b51ab5027dfd6ee5`で `ALLOW work`。`build/dom-pointer-final-native-work-20261013.log`を消費した。源/fixtureは不変更、更新はこの最新結果の二段報告と登録だけである。
