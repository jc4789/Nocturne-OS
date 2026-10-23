# 既存CSS取消fixtureの契約訂正

元失敗 `build/browser-compat-guest-20261013/test-serial.log:174-177` は保全する。370件中、outer recoveryのflag期待とrecovery後red pixelの2件が失敗した。原WPT47件、parallel CSS12件、render11件の成功で相殺しない。

## 基準と現在の照合

`git show HEAD:tests/csssupportstest.c` のfilter cancellation群は、現在と同じく私的 `web_js_native_recover()` を直接呼び、その直後に `native_recovery_pending=false` を要求していた。

しかし基準HEADと現在 `js.c:web_js_native_recover()` は双方、独立native取消の場合に `native_cancelled` をclearし、`native_recovery_active=true` として復旧transactionを開始するだけである。基準HEADの `doc.c:web_layout()` でも `native_recovery_pending/active` をclearするのはlayout commit時である。また基準／現paintは未commitのpending/active状態ではbitmapを上書きしない。したがって、layoutを行わず直後にflag clearとred描画を要求するfixtureは基準契約にも合わない。

## 訂正

`tests/csssupportstest.c` の該当群だけ、通常製品のpublic `web_layout()` 外側経路を実行して復旧を完了する。flag期待を弱めず、従来のcancelled/pending clearに加え **active clear、layout_valid、root_box存在** を要求した。red pixelの期待値 `0xff7f7f` は変更していない。初期実childの存在も明示assertし、nullなら失敗のまま安全に終了する。

新 `--filter-cancel` modeは訂正した取消／coherent pixel／clip／outer rebuild／red描画群だけを再確認する。未変更の宣言・条件・長いfilter vector・hover全370件を繰り返さない。通常full modeでは従来のvectorチェックも引き続き実行するよう分離した。

製品selector parser、matcher checkpoint、css.c/cssprop.c、JS recoveryとpaintのflag契約を本訂正で変更していない。今回の関係selector実装が取消を握り潰したとの証拠はなく、旧fixtureの実経路不一致を訂正したことを示す。

userland ELF syntax確認成功。訂正した実guest結果は主担当待ち。期待緩和・skip・製品経路の迂回で合格を作っていない。
