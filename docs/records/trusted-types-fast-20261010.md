# Trusted Types文字列高速経路（2026-10-10）

現行ソースを優先して実装した。開始時のGit差分は空。J-spaceは指定のmediumで、既存の解析マップを探索先として使い、下記の製品契約と結果を現在のソース・媒体で照合した。旧HTML解析作業の停止記録は変更していない。

## 製品経路と保全した契約

- `user/libc/web/html_policy.h/.c`：`html_policy_requires_script()`は現在のrule列を走査する。report-onlyも対象。キャッシュしないため後追加meta CSPを反映する。
- `user/libc/web/js_html_safety_host.h`と`js.c`：`safetyRequired`は既存`native_safety`と同じ所有文書を選び、真偽値だけ返す。動的コードのCフックは、require規則なしの非オブジェクト値と組立済みFunctionソースでJSフックを省く。
- `user/apps/browserjsworker.c`：Workerの`policy_doc`で同じ判定・動的コード分岐を実装。
- `user/libc/web/js_html_safety.js`：指定の文字列シンクで高速経路を追加。オブジェクト、null、Trusted値、require規則ありの場合は従来の処理を使う。native不在時も従来処理へ戻る。brand・引数数・変換順を維持する。
- 非シンク属性は属性型のネイティブ照会を省く。属性名・namespaceは一度だけ変換し、その変換済み引数を元メソッドへ渡す。`js_attributes.js`は未変更。
- スクリプト本文の高速経路にも既存の承認済み本文保存を残した。現行`js_script_source_check`は保存値と本文が一致すれば、接続時に追加されたCSPで再判定しない。これを省くという旧プロンプトの提案は、設定→meta追加→接続の動作を変えるため採用しなかった。
- `js_bootstrap.inc`と`js_worker_runtime.inc`は正規のMake依存関係から再生成した。

仕様照合：[Trusted Typesの適合文字列](https://w3c.github.io/trusted-types/dist/spec/#get-trusted-type-compliant-string)、[report-onlyを含むrequire判定](https://w3c.github.io/trusted-types/dist/spec/#does-sink-type-require-trusted-types)、[承認済みスクリプト本文](https://w3c.github.io/trusted-types/dist/spec/#prepare-the-script-text)。

## 検証

変更前・変更後とも次の指定を各一回実行した。

`python -X utf8 scripts/test.py --accel whpx --memory 4096 --cpus 4 --resolution 1920x1080x32 --timeout 600 --output-dir build/tt-fast-{before,after} htmlparser htmlworker htmlcsp web/javascript web/attribute-nodes web/shadow-dom-native`

- 解析系5項目、Worker、CSP、属性ノード、native Shadow DOMの9項目は変更前後ともPASS。
- `web/javascript`は変更前後とも1165外側チェック、19件失敗。失敗行の多重集合は一致。フレーム寿命、外国文書のinterface、解析イベント順・document.write、実行予算の既存失敗は未修理であり、全検査成功とは扱わない。
- `tests/js_html_safety_cases.js`に指定6条件を追加。さらに属性変換の回数・順番、brand先行、後追加meta CSP、meta追加前に承認したprimitive/object文字列スクリプトの接続を確認するケースを追加。変更後の当該バッチに例外・FAIL・完了待ち失敗なし。既存の`html-safety-count n>=50`は維持。
- 全体ビルドは最終版で成功。IMG／VHD／VHDX／ISOを更新。実行した試験媒体とAmazon媒体のkernel/initrdハッシュは最新生成物と一致し、initrd内のbrowser／browserjsworker／webfetchも`build/root/bin`のバイナリーと一致した。
- 最新kernel SHA-256：`6614ef3888494269107c0116fbf9c09b319fe0cc845c9f5ce5c56207dd8b53c3`。
- 最新initrd SHA-256：`56ee61454542d7268ff0187d101cdae5bda478c809656b1490435faaa23132d9`。

## 実サイト画面と操作

Amazonを最大化し、WHPX／4CPU／4096MiB／1920×1080の隔離QEMUで確認。読み取り専用の位置・target観測後にPS/2クリック、`qemu`の実キー入力とEnterを送った。検索結果URLと本文、PageDown後の下の商品、PageUp後の先頭、F12コンソールを主担当が画像で確認した。

見た画像は`build/nocturne-platform/tt-fast-amazon/startup-0.png`、`screen-0.png`、`screen-2.png`、`screen-3.png`、`screen-4.png`、`screen-6.png`。検索・上下移動は成立。商品画像欠落、ホームの大きな空白、暗号APIの既存例外は残る。戻る／進むキーも送ったが、目的画面への往復成立は未確認。性能改善量、全サイト互換性、Hyper-V実機は未確認。

## 終了・回収

今回の3VMは停止済みでユーザーデータ未接続。絶対パス・リンク不在・PID停止を確認し、`build/tt-fast-before`、`build/tt-fast-after`、`build/nocturne-platform/tt-fast-amazon`の一時boot/data媒体、抽出kernel/initrd、PNG化済みPPMだけ回収した。配布媒体、通常の`build/data.img`、PNG、ログ、metadataは保全。回収後のDドライブ空きは167.05GiB。削除した試験媒体はそのまま再実行できない。

最終監査では、所有文書・Worker判定、report-only、後追加meta、Trusted値のネイティブpayload、script本文保存、属性変換、正規生成物、変更前失敗の保全を依頼と照合した。同じ未変更検査は反復していない。
