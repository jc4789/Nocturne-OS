# CSS nativefixture初期化不備の訂正

元guestログ `build/browser-compat-guest-20261013/test-serial.log:154` 以降は保全する。

- 通常modeは `native targets` がnullで失敗。
- Shadow境界modeは初期HTML由来の`other`がnullなのに`other->style`へ触りpage fault。
- media-calc modeはtargetがnullのため、1908/767/1908の3style期待が失敗。

原因はfixtureが `web_live` の後に `web_layout` だけ呼び、incremental HTMLparserを駆動する通常 `web_tick` を呼ばなかったことである。`web_layout` はparser driverではない。この失敗をselector/calc productの結果として扱わず、同時に元失敗を消したり成功へ書換えたりしない。

訂正は `tests/cssrelationaltest.c` のみ：3modeとも、作者／nativeと同じ `web_tick` 経路でparserが終了しhtml/bodyが存在することを確認してからDOM queryする。5秒はfixtureの異常終了検出だけで、製品parserやbrowserの上限ではない。computed styleとShadow unrelatedtargetのnullguardを追加した。元のselector・mutation・style・resize期待は変更していない。CSS製品ソース、原WPT、期待値の弱化、skipは変更していない。

原WPTは同一guestで detachedhas12、Conditional3 17、Conditional4 selector18 の47原ケースが成功しているが、これを本fixture3modeの成功へ流用しない。fixture訂正後の再確認は失敗した3modeだけが対象。その他未変更原WPT／host24件／全suiteは繰り返さない。

fixture userland ELF freestanding syntaxはexit0。訂正後guest結果は主担当の実行待ち。
