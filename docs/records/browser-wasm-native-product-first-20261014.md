# wasm2c/TCC 初回製品実行

## 実行媒体

`build/wasm-full-build-native-unwind.log` の全体ビルドは終了0。
この節は後続JSPI・stack guard修理前の媒体に限定する。
kernel SHA256: `d5c907e33a698ea276b5d4ee853fde69007fea24827adf15da4375ad44bbf002`。
initrd SHA256: `c0563b47d39145d885684c02e53285f003e4759cf2619c5bd386f6b50fe07f04`。

## 実Nocturne補助試験

- `build/wasm-native-first-20261014/test-serial.log`: WHPX、4 CPU、4096MiB、1920×1080。14群成功、失敗0、skip0。基本170、境界41、HTTP Worker50、Window/Worker CSP、Fetch/XHR/DOM/Worker回帰とnative threadを含む。FAT338ファイル正常。全体109秒。
- `wasmcli`群ではゲストのwasm2cが生成したCをゲストTCCが実行ファイルへコンパイルし、実行結果42を確認。ホストの生成C実行で代用していない。
- `build/nocturne-platform/wasm-native-gc-first-20261014/serial.log`: 256反復のWasm→JS強制GC、循環、Memory.buffer寿命、globalを消した後のexternref戻り値、Module/Instanceラッパー消失後のexport/Table経由実行、無限Wasm loop取消が成功。ネイティブ258モジュール、259呼出し、フォールバック0。
- 同VMのTCC guard検査は16成功、0失敗。実確保151回の1/75/76/113/151地点のOOMを-2で回復し、その都度通常コンパイル・実行へ戻った。完了済parser後の即時取消と単関数内部のlexer取消も回復。
- **GCハーネス全体は失敗。** 主担当が存在しない`TCC_GUARD_PASS`を期待値へ指定した。またCLI autorun完了で電源断し、WM描画前の1280×800画像しか撮れず、要求1920×1080と不一致。製品補助プログラムの実行成功と、撮影・期待markerの失敗を分離する。未変更製品で同じ試験を繰り返していない。後続製品修理の確認では正しい完了行と、描画まで生存する後続browserを使う。

## Puter実サイト

`build/nocturne-platform/wasm-native-puter-first-20261014`。最大化・JSコンソール開放。
主担当が`current-console.png`、`download-console.png`、`launch-state-console.png`、`after-disabled-launch-click-console.png`を閲覧。

- data URL Fetch修理により、以前の`fetch supports HTTP(S) resources only`から前進。54394バイトのサイト自身のWasmをwasm2c/TCCでコンパイルし、ネイティブ呼出し5、trap0、fallback0をコンソールから取得。
- ダウンロード78%の本文、続いて`Launch Firefox`とJSPI未対応警告を実画面で確認。
- 読取り専用console式で`start-btn.disabled === true`、`Suspending/promising`が未定義と確認。実target読取りは`start-btn`、native pointerはDOM座標955,466到達後に実クリック。無効状態のまま、Firefoxは起動しない。DOM改変・強制有効化・JS `.click()` は行っていない。
- 抽出コンソールは同runの`console-evidence.log`。提供されたFirefox内Puter画面へは**未到達**。

ユーザーが続いてJSPI公式提案を指定したため、ここを最終限界として終了せず、正式アルゴリズムに従う中断・再開を追加実装中。Overviewと現行formal draftの同期値/thenable規則の差も区別する。
