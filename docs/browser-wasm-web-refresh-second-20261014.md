# WebAssembly Web API・data Fetch・JSPI 独立反証第二考察（2026-10-14）

第一報の実装説明・成功件数を繰り返すのではなく、「成功に見える構成でも壊れる具体的経路」から寿命・再入・規範との差を再検討した。製品sourceは固定。新しい実装や追加試験をこの記録のために開始していない。

## 1. 返却JS objectだけを保持しても、native refは生存しない

反例は mutable externref Global にだけ残ったobjectを Wasm localへ読み、host importでGlobalをnullにしてGCし、そのlocalを返す呼出しである。native callbackでshadow rootsを保持しても、実行復帰後にouter frameへ同期した時点で返却refの唯一のanchorを捨てれば壊れる。

最初の修理案は結果を `to_js()` して Active rootsへ保持するものだった。しかし externref の `to_js` は内部 Foreign wrapper でなく raw JS object を返す。そのobjectが生きていても、wrapper finalizerがbindingを消すと同じRefの次の変換がundefinedになる。この二段目の具体反証を通知し、最終的に **返却Refの `anchor(ctx,state,ref)` wrapperそのものを旧native anchorsの整理前に保持する** 形へ修理された。

最新serialの `returned-ref-outlives-cleared-global` と native call統計はこの反例の実製品回帰確認である。一般的な「到達可能なJS objectを残した」だけの試験や、wrapper消失を伴わない呼出し成功とは区別する。全GC経路の証明とはしない。

## 2. cancelフラグは、その後の作者呼出しを自動では止めない

running taskのcancelを「フラグを立ててreturn」だけにすると、現在のhost callbackから戻ったWasmが次の普通importを呼び、作者JSを実行できた。またキャンセルされたSuspending callbackの戻り値をPromiseResolveすると、thenableの作者getterをさらに実行できる。

この具体経路を通知し、Stateのcancel / retired判定をnative・interpreter・compile pollとhost import入口、JS_Call直後、await / PromiseResolve前後へ統合した。multi-value iterableや引数変換の後続作者呼出しも止める。単に「Promiseをsettleしない」では取消の保証にならない。

静的修理確認に続き、最新serialの `running-retirement-before-next-import`、`running-retirement-before-thenable-getter`、`jspi-retirement-unwinds-without-author-callback` が成功している。未変更の同条件を繰り返して成功を積み増すことはしていない。

## 3. Promise resolver自身も同期で作者へ再入する

coroutine完了後だから安全としてrunning=falseにしてからresolveすると、externref結果の `then` getterが同期でretireを呼び、done taskのcontextをcleanupできた。resolverから復帰したresumeがまだそのctxを使うため、実行中contextの早期解放になる。

修理はsettlement中もrunning=trueを維持し、外側cancelのdone cleanupを `!running` に限定するもの。Promise reactionsからのself保持だけではこの再入を防げない。最新の `settlement-retirement-keeps-context-until-return` がこの具体反例を確認している。

## 4. 中断中のnative rootsとinterpreter threadは別の正本

JSPIの通常結果が42でも、保存stack上のActive chain、native anchor、pending例外、JS_DupContextのGC申告、fallback threadのrefsが申告されなければGCで壊れる。Taskのowner/function/promiseをmarkするだけで十分とは置かなかった。

最新実装は実行中 / 保存中のExecutionFrameを区別して申告し、context参照、Active全段、native anchorsと保存例外を辿る。実ログの `jspi-suspended-code-and-ref-gc` と `jspi-fallback-thread-ref-gc` は別の検査である。native経路成功をfallbackの証拠へ流用せず、native calls before260 / after261 と interpreter fallback boundaryも読んだ。

## 5. Overviewとホストエンジンを、現行規範の代わりにしない

旧Overviewの「Promiseならsuspend」に合わせるとsync値やthenableが誤り、普通exportからsync Suspending importを許可するとactive context条件も壊れる。現行formalのPromiseResolve / Awaitと普通exportのSuspendErrorを採用した。

一方、Node v26.8.1 は引数変換エラーを同期throwし、現行formalがasync runner内で変換する期待と一致しなかった。既存71検査成功の後のこの失敗を、試験削除・skip・両方許す期待値で隠していない。Nocturne Window74の成功は別の製品実行であり、ホストとの差を消す理由にはしない。

作者JSframeを跨がない制約も、単に全再入を禁止してはならない。nonsuspending Wasmへの再入、直接Wasm→Wasm、nested promisingを肯定対照として通し、Wasm→作者JS→Wasmのsuspendだけを拒否する。Workerの追加12検査は別realmの最低限確認であり、Window74の結果をWorkerへ引き継がない。最終 `build/wasm-jspi-worker-final-20261014/test-serial.log` を独立に読み、実HTTP Worker62検査・1群成功・失敗0・skip0を確認した。うち12は今回のJSPI追加である。

## 6. Fetch成功をネットワーク同等やサイト起動へ拡大しない

WorkerはWindowと同じFetch facadeを使わない。共有責務はnative data processor / ArrayBuffer wrapperに置き、各realmの既存Response、task、Abortへ接続した。Workerの非空integrityは明示NotSupportedのままなので、Window SRI成功をWorkerへ転記しない。

data URLのmetadataまでpercent decodeするとMIMEを誤認し、末尾fragmentを本文復号後に切ると `%23` を失う。forgiving-base64は不正paddingやNULを許す意味ではない。MIME原文をheaderへ連結せずparse/serializeし、parameterを落としてWasm streamingのstrict MIME検査を誤って通さない。

POST XHRのheaders/chunk/endだけが出てもupload完了がなければ既存操作を壊す。この実反例はsource固定前に修理した。共有decoderの抽出で別用途hex helperを落としたcompile退行も記録し、修理後doc.o成功と他objectの以前の成功を分けた。こうした失敗はホスト53 / C108という件数で相殺しない。

## 7. Puterは次の不足を可視化したのであって、Firefox対応を証明しない

最新の実クリックでLaunchが動いた後、実エラーは `Atomics.waitAsync` のundefinedへ進んだ。loaderのnative5→累積10、fallback0はサイト自身のWasm実行の証拠である。一方JSPIは0 / 0 / 0であり、APIが公開されたことだけから実ページのsuspend/resume成功を主張できない。Firefox UIとFirefox内Puterページは未達である。

SAB / Atomicsは上流QuickJSに本体があってもNocturneでは公開・Worker共有とも未対応。raw-pointer SAB serializationをprocess間配送へ使ったり、共有memoryを各Storeで独立reallocしたり、shared=trueだけ許可するのは修理ではない。既存Nocturne共有capability、Worker broker、Memory backing / grow正本、COOP/COEP、Moduleの実cloneへ統合する設計が必要であり、現段階で強制公開はしていない。

## 結論の境界

具体的なdata取得・Wasm native実行・Window / HTTP WorkerのJSPI中断再開・GC/取消反例には製品改善と実行証拠がある。ただし最新実サイトの表示・入力・上下scroll・性能・動画、Firefox起動、Hyper-V実機、shared系とModule clone、仕様全体の完成をそれらから推測しない。三指定サイトの以前の操作と最新媒体の受入を分ける。HTTP Workerの最終実数62は上記ログで固定し、主担当の実画面・実操作受入とは別の証拠として扱う。

根拠: [現行JSPI JS API](https://webassembly.github.io/js-promise-integration/js-api/)、[ユーザー指定Overview](https://github.com/WebAssembly/js-promise-integration/blob/main/proposals/js-promise-integration/Overview.md)、[Fetch data URL processor](https://fetch.spec.whatwg.org/#data-url-processor)、第一報に挙げた現行WebAssembly / CSP / MIME / Infra仕様。実ログの配置と媒体payloadは第一報に明示した。
