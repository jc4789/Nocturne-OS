# WebAssembly統合の主担当による独立レビュー

## 確認方法

担当の報告だけでは受け入れず、Makefile/build_cxx.py、WABT変更、js_wasm.cpp/h/js、QuickJS転送制御、Window/Worker初期化、ポリシーの解析・通知・寿命を主担当が確認した。実製品の全体build、ゲストTCCによるjstest構築、QEMU実API実行、指定三サイトの画像と実入力は別の証拠として扱った。

## ビルド担当

WABTの18翻訳単位を既存C++ツールチェーンへ接続し、個別lazy objectとしてCアプリ/TCCと共存させる方式を確認。必要なlibc++、std::exception ABI、nearbyintの追加を含む。gc-sectionsを外したリンクも確認済み。archiveだけが残った個別object欠損は、担当の再考察で見つけてgrouped targetへ修理した。単一object退避・並列再生成・同一SHA256・次のdry-runでrecipeなし、という反例確認を読み取った。WABTはApache-2.0で、配布initrdにライセンスと上流revisionを同梱する。

## ネイティブ担当

Module/Instance/Memory/Table/Globalが実際のWABT検証・実行・メモリへ接続していること、JS importの例外/再入とi64/複数値の変換、wrapper/JS anchor/native Storeの寿命を確認。初回ゲストで見つけたMemory.PageSizeのページ数契約誤読を修理し、後の実ゲストで確認した。buffer転送による解放済みメモリ参照の反例にはQuickJS所有bufferの非転送化と各transfer listの事前検査を追加。通常ArrayBuffer転送を対照として残した。

native収集はwrapperごとにStore全走査せず、最外task端へまとめる。追加強制GCゲストは256反復とruntime破棄まで成功したが、これはCSP追加前の媒体であり、撮影resolution不一致によるハーネス失敗とも分離する。CSP callbackはModule生成・stats加算前で、拒否時CompileError、負値時既存JS例外を保つ。validateと既存ModuleのInstance化を意図的にコンパイル認可対象としない。

## Web API担当

Promise overload、入力/option/importのsnapshot、作者によるprototype変更への捕捉、Responseのbrand/MIME/status/CORS/body状態を既存Fetch/Streams経路へ接続したことを確認。streamingは全body受信後のコンパイルで、性能上の並列streamingを主張しない。WorkerのsetTimeout初期化問題と初回試験の完了行filter不備を修理した経緯も保全した。

CSP不足を最終確認で発見し、既存html_policy.c/hにscript-src優先/default-src fallback、複数policy交差、重複先勝ち、報告専用とmeta/継承/snapshotを統合。主担当のWindow/Worker callbackと既存通知queueを別担当が再レビューした。meta ruleをparser rewind後のqueueへ借用しないよう、通知本文複製後にrule参照をNULL化した。既存Trusted Types通知にも同じ寿命修理を適用した。

## 初期 CSP ゲストの結果（当時の証拠）

`build/wasm-csp-native-20261014/test-serial.log`：basic168、native boundary41、HTTP Worker29、Window CSP 17/12/22/17、HTTP Worker CSP44、既存Worker policy継承が成功。runnerは6群成功・失敗0・skip0。これは全面Core 3.0/CSP/WPT適合、指定サイト作者のWasm実行、表示全面成功、無回帰・高速化を証明しない。

## wasm2c/TCC と JSPI の最終独立確認

最新の担当第一報・第二考察6件を読み、`PromiseTask` の所有権・実stack切替・Awaitとsettlementの作者再入、`task_mark` のhidden context/停止中native/interpreter参照、frame/Window/Workerの取消接続をソースと照合した。QuickJSのstack/frame/例外交換、intrinsic Promise反応登録、JS再入でrecursion budgetを広げない条件を主担当が確認した。portsの同thread coroutineは実予約量由来で両端guardを持ち、TLS/FP/registerを保存する。`wasm_rt_compile_tcc` のC-only guard・OOM/取消区別、TCCのstack書込前guardと一般TCCへprofileを強制しない境界も確認した。

実ゲストは主担当が最新媒体で実行。`build/wasm-jspi-first-20261014/test-serial.log` の15群pass/fail0/skip0、基本170・境界41・WindowJSPI74、guest wasm2c→guest TCC→実ELF結果42を直接読んだ。追加HTTP Worker JSPI12は別の未実施条件として実行し、`build/wasm-jspi-worker-final-20261014/test-serial.log` のWorker62・1群pass/fail0/skip0を確認。前回50の結果を追加12へ流用していない。

Puterと同じ最新VM内でTCCguard29/0、GC256、native258module/259call/fallback0、停止中JSPI native call260→261、実import-tail fallbackのlocal参照GC、作者非実行の取消、running import/thenable/settlementの退役再入反例が成功した。旧GCハーネスの誤marker・解像度失敗は取り消していない。

`build/wasm-final-artifacts-jspi.log` の16payload・kernel/initrd・IMG/VHD/VHDX/ISOの一致とsource freshnessを独立に読み、配布媒体を最新製品へ接続した。Hyper-V実機は未実施。

## 実サイトと受入判断の範囲

担当の今回の責務実装を上記の範囲で受け入れる。主担当自身が指定4サイトの最大化console実画面と操作前後画像を閲覧した。PuterはLaunch実クリック、loaderのnative初期5→累積10call/fallback0を確認したが、JSPI実ページ使用0、Atomics.waitAsync欠落でFirefox本体は未達。百度百科は初回表示・上下scroll・検索・履歴操作が成立。YouTubeは停止再開・上下scroll・timeline seek後の映像進行を確認した一方、video lookahead allocationによる一時停止も発生した。CNNは本文/写真と上下scroll、実リンクURL遷移は成立したが動画先は黒画面。これらを全面受入や無回帰としない。

共有memory/threads/SAB/Atomics、GC/typed reference拡張、JS Tag/Exception、文字列組込み、realm間共有・Module structured clone、全OOM/巨大処理の応答性は未対応または未検証。CNNメニュー・黒領域、字幕重なり、YouTube確保失敗の因果未確定をAPI成功で相殺しない。音声はPCM供給のみで聴取・同期品質は未確認。最終観測と保全/片付けは `docs/browser-wasm-delivery-20261014.md` に分離する。
