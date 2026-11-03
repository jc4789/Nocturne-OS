# Wasm CSP policy 初回報告（2026-10-14）

## 変更

- `html_policy.c/h` に `has_wasm_policy` / `wasm_allowed` と `html_policy_wasm_allowed(rule)` を追加した。
- 既存 native policy parser の一回の走査で script-src と default-src の最初の directive を保持する。script-src が存在すれば常に優先し、存在しなければ default-src を使う。空の source list は拒否する。
- 許可するのは ASCII 大小を区別しない完全 token の `'unsafe-eval'` / `'wasm-unsafe-eval'`。prefix、URL 内の文字列、引用符なし、script-src-elem / attr は代用しない。非ASCIIを含む該当 directive は duplicate 処理前に無視する。
- predicate は report-only を許可へ丸めない。全違反の報告と enforce の拒否は主担当の Window / Worker callback が行う。
- Trusted Types directive がなくても Wasm source policy を既存 rule list へ保存する。original_policy を既存 inherit / snapshot が再解析するため、新たな状態一覧や snapshot 版は不要。

## 一次仕様

- [CSP3 §4.5.1](https://w3c.github.io/webappsec-csp/#can-compile-wasm-bytes): source-list の優先、許可 token、各 policy の報告、enforce 時の CompileError。
- [CSP3 §2.2.1](https://w3c.github.io/webappsec-csp/#parse-serialized-policy): ASCII directive と duplicate first-wins。

## 確認

既存 `tests/frame_embedding_policy_hosttest.c` に29種類の policy と inherit / snapshot、meta / rewind を追加した。製品 html_policy.c、http.c、util.c を実際にリンクした host executable が **265 checks, 0 failures**、終了0。`git diff --check` も終了0。

実行コマンド:

```text
tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -ffunction-sections -fdata-sections '-Wl,--gc-sections' -Iuser/include -Icommon -Iuser/libc/web tests/frame_embedding_policy_hosttest.c user/libc/web/html_policy.c user/libc/http.c user/libc/web/util.c -o build/frame-embedding-policy-hosttest.exe
build/frame-embedding-policy-hosttest.exe
```

最初の compile 呼出しは launcher が comma を解釈して失敗した。flag を引用した後は system http.h / Windows macro が混入する include 優先順位で失敗した。既存 Nocturne header を `-Iuser/include` で先に指定した上記コマンドが成功した。これらを製品修正による成功へすり替えない。

ここでの成功は native policy の解析・保存・継承判定の補助確認。新しい native callback、Window / Worker の違反イベント・network report、更新媒体の実ゲストと実サイトの結果は主担当の別確認が必要である。CSP 追加前のゲスト11群成功や三サイト操作を変更後へ自動的に流用しない。
