# 専用 Worker・空き時間処理・リンク既定動作（2026-10-09）

## 実装

- `Worker` は独立した Nocturne 子タスク `browserjsworker` と専用 QuickJS runtime。
  メインページのタイマーによる模倣ではない。既存native scheduler/IPCを使い、POSIX互換層は追加しない。
- classic script、同一origin HTTP(S)／文書の私有Blob URL、`postMessage`、
  ArrayBufferの複製・実detach転送、`terminate`、子側`close`、タイマー、
  親ローダーを経由する`importScripts`と限定GET `fetch`を追加。
- Blob起動はconstructor中にsourceを保持するため、直後の`revokeObjectURL`で起動を壊さない。
  メッセージはnative送信キューが受理してからtransferをcommitする。
- 子にDOM、任意ファイル、ソケット、process操作、native moduleは公開しない。
  取得・cookie・CORS・redirect検査は既存browser host、文書世代違いの完了は破棄。
- native Workerはブラウザープロセス全体で4個、子QuickJS heapはlazy 64MiB/個。
  packet 8MiB、文書の送受信・event合計キュー16MiB。親pumpは子ごと16KiBまでの非blocking処理。
  高速producerの受信は有限pipeで流量制御し、消費が遅いことだけを理由に終了させない。
- 作者の長時間計算をメインページの15秒watchdogへ戻さない。
  native schedulerでpreemptでき、`terminate`と文書破棄でkill、非blocking回収する。
  私有bootstrapは10秒、source compileは30秒の上限を維持。
- consoleの子起動・出力にWorker IDとnative PIDを表示。別runtimeかをログで区別できる。
- `requestIdleCallback`／`cancelIdleCallback`／`IdleDeadline`をnative queueへ接続。
  通常仕事・描画を優先し、timeoutは公平に交互選択。最大50ms、periodごと8件、tickごと1件。
  Workerの到着eventとdeadlineもidle優先度・時間残量へ反映。
- Canvas `isPointInPath`／`isPointInStroke` の現在path・Path2D両形式を追加。
  fill rule、暗黙close、境界、CTM、非等方pen、dash、既定butt/miter形状をnativeで判定。
  同じstroke outlineを描画と共有し、交差・joinのalpha二重合成も修正。

## 指摘された2点の確認

1. HTML5testの公開コードはdocument capture clickでpopupを削除する。
   native `page_click` のdispatch後に接続性を再検査していた経路が原因だった。
   activated anchorのidentityをdispatch前に保持し、終了後はそのanchorの最新hrefだけを読む。
   一般hit-testの接続性は緩和せず、defaultPrevented、owner/activity、inertの制約を維持する。
   [現行cannot-navigate規則](https://html.spec.whatwg.org/multipage/links.html#cannot-navigate)は非接続HTML `a`を例外とする。
2. `<?import …>` のProcessingInstruction・nodeType 7は
   [現行tokenizer](https://html.spec.whatwg.org/multipage/parsing.html#tag-open-state)と一致する。
   古いHTML5testのcomment期待に合わせてLexborを変更しない。
   引用の「18/19」を今回新たに測定した結果とは扱わない。

## 実サイトで確認した結果

すべてユーザーVMではなく、匿名の隔離QEMU/WHPX・4GiB/4CPU。
人工Webページや試験用Worker sourceは配信していない。

| 公開サイト | 実画面・操作の結果 | 証拠directory |
|---|---|---|
| HTML5test | 310点。Worker/idle Yes。実PS/2クリックでcapture後にリンクが非接続でも比較URLへ遷移 | `build/nocturne-platform/round2-html5-popup` |
| MDN Worker公開例 | 実サイトの入力7/6と本文Result:42。別native PIDでResult:72、ArrayBuffer転送元0、terminate | `build/nocturne-platform/round2-worker-mdn` |
| WHATWG素数公開例 | 21秒連続計算中main timerが8回進行、素数148991まで実表示。terminate後停止 | `build/nocturne-platform/round2-worker-primes` |
| SuttaCentral日本語 | 実日本語本文「放逸等の章」と折返しをscreen-2で確認 | `build/nocturne-platform/round2-sutta-jp-fixed` |
| YouTube匿名ホーム | logo・検索欄・feed nudge本文をscreen-finalで確認 | `build/nocturne-platform/round2-youtube` |

最新ビルドでMDN実例を最後に一度再確認した
（`build/nocturne-platform/round2-worker-final`）。サイトの未変更`worker.js`を取得して
Blob化し、Worker生成直後にURLを失効させてもResult:42を受信した。
9MiBの転送をpacket上限で拒否しても元ArrayBufferはdetachされず、
cross-origin Worker生成はSecurityError。ゲストの公開Canvas APIをconsoleから呼び出し、
Path2D内外・stroke内外4条件が成功した。Canvas使用サイト全体の受入れとは区別する。

HTML5test比較URLは現在のサーバーが引退案内を返し、相対script取得も失敗する。
修理した「クリックが届いて遷移する」と、遷移先サイト全体が動くとは区別する。
SuttaCentralの最初のQAはURLの`&`がguest shellで解釈された起動手順の失敗であり、成功に含めない。
コンソールの本文markerにはscript文字列も含まれ得るため、marker単独を描画成功の根拠にしていない。
Worker追加による性能向上率、全4CPU上の同時実行数、動画再生成功は測定・確認していない。

## 残る範囲と補助証拠

- module／shared／nested Worker、MessagePortとBlobのprocess間clone、
  SharedArrayBuffer/Atomics、完全なWorkerLocation/Navigator、Request/POST/headerful fetch、
  streaming Responseは未対応。未対応機能を成功する空stubにはしない。
- Canvas可変cap/join/miterLimit、blend/export/WebGLは未追加。
  HTML5testの古いhit-test項目は`addHitRegion`検査なので、現行point/path判定の追加で点数を偽装しない。
- source/native幾何/IDL等の担当補助検査はCanvas35+6、idle54、worker13。
  native IPC・公開API統合・実サイト全体を代替する証拠ではない。
- 最終全ビルド`build/browser-round2-20261009/build-backpressure.log`は終了0。
  `nocturne.img`／`.vhd`／`.vhdx`／`.iso`は今回の実装を含んで更新済み。
  実行記録のkernel/initrd digestで各QAに使用したビルドを識別する。
