# FormData(form, submitter) 実装（2026-10-12）

## 製品変更

- `form_value.c` の既存successful-control collectorを型付きentry sinkへ拡張し、フォーム本文送信と同じnative owner／disabled／fieldset-first-legend／checked／option-selected／option-disabled／tree-orderの責務を再利用。重複名・順序を保持し、別JSフォーム状態や疑似localStorageを作らない。
- collectorのDOM traversalをiterative preorderへ変更。datalist／template subtreeを除外し、foreign ancestorからHTML子孫への下降は維持する。深いDOMをnative再帰stackで拒否しない。
- `_charset_` hidden entryはUTF-8。文字列・textareaのentry段階では改行を早期CRLF化せず、multipart／URL serializationの既存責務へ残す。dirnameは既存native directionalityを使用。
- File inputはnative選択数と実bytes／name／type／lastModifiedを使用。選択なしの場合だけ、仕様で要求される名前空・`application/octet-stream`・空bodyの新Fileを生成する。選択Fileの代わりに空Fileを捏造しない。
- 選択済input Fileは `formControlBridge` の既存FileListキャッシュを再利用し、FormData生成のために動画／File bytesを再snapshotしない。FileList setter／nativeFile helperの作者Array iterator／map依存を除き、指定された選択内容とFile identityを保つ。
- FACE値は既存 `web_face_entry` の明示長さ・string／File型を使用。複数entry、NULを含むFACE文字列／filenameをC `strlen`で短縮しない。
- `js_formdata.js` のconstructorはWindow branchでのみHTML brand／submit button／ownerを確認し、native `entriesBegin`で全entryを採取。種類誤りはTypeError、別form ownerはNotFoundError、entry構築再入はInvalidStateError。
- `FormDataEvent` のprivate brandとreadonly `formData` を追加。DOMString type→dictionary→Event初期化のWebIDL変換順を維持する。`formdata`はtrusted／bubblesで発火し、event専用FormDataへ関連付けたentry listを配送後にshallow cloneしてconstructor戻り値へ入れる。イベント後のevent.formData編集は返したFormDataを変更しない。
- native nodeのconstructing-entry guardはイベント配送中も保持してfinallyで解除。通常native submit側もこのguardによる再入を中断する。root担当が既存DOM_formValue／bootstrap handler／非cloneable登録を統合。
- Workerの無引数FormData／body parse・serializeは別担当が同moduleで再利用。Window constructor処理をWorkerへ持ち込まず、HTML要素引数はTypeErrorとする。

## 共有契約

`web_form_collect_entries(d,form,submitter,sink,opaque)` のsink引数は `(opaque, const web_face_entry*, empty_file, file_control, file_index)`。bytesはcallback中だけborrow、falseは全収集失敗で部分リストを返さない。

`dom('formValue', form, 'entriesBegin', submitter)` → native typed rows、`entriesEnd` → guard解除。rowは `[name, string/ArrayBuffer/null, filename/null, mime,lastModified,emptyFile,fileControl/null,fileIndex]`。実input FileはfileControlで既存cacheへ接続し、byte値nullを偽空Fileとして扱わない。

## 確認と残る境界

最新の「追加観測・試験を止める」を遵守し、この追加まとまりについて実行試験／VM／buildを開始していない。製品コード・一次仕様の静的照合であり、実動作合格・回帰なしとは報告しない。

今回の `formdata` eventはFormData constructor経路。通常native GUI form送信自体のformdata eventと、その変更リストをnative送信本文へ戻す接続は別境界で未実装。cross-realm FormDataEvent brandや全WPT互換、通常native文字列の全NUL保持も、このまとまりだけで全面対応としない。File／FACEの長さ付きentryは実接続済み。

image submitterのselected coordinateは初期(0,0)。主担当は画像available＋明示的な実pointer activationだけをcontent-box相対CSS座標へ接続し、cancelled clickでは更新しない。非pointer activationでは既存selected coordinateを保持し、border／padding上の合法な負座標も保持する。collectorはこのnative値を使う。これは主担当の実装引継ぎであり、実操作受入済みとは報告しない。

## 一次仕様

- [HTML constructing the entry list](https://html.spec.whatwg.org/multipage/form-control-infrastructure.html#constructing-the-entry-list)
- [XMLHttpRequest FormData constructor](https://xhr.spec.whatwg.org/#dom-formdata)
- [HTML image-button selected coordinates](https://html.spec.whatwg.org/multipage/input.html#image-button-state-(type=image))

## 検証再開（同日・ユーザー訂正後）

ユーザーが検証再開を明示したため、既存 `tests/js_form_controls_cases.js` に実DOM用entry-list確認を追加し、既存 `tests/js_form_controls_native.c` の `formdata-entries` 引数で新しいまとまりだけを実行する。disabled／fieldset第一legend／選択option／tree-order／重複／dirname／charset／実empty File／submitter例外／FormDataEvent変更コピーと再入／WebIDL順／FACEの実File bytesを確認する。native `web_input_files_replace` で選択Fileを設置し、実JS bootstrapのFile identity・内容・lastModified・FileList setterも確認する。画像座標はnative格納した合法な負座標を、非pointer `.click()` とcollectorが保持することを確認する範囲で、実pointerクリック受入ではない。

主担当の正規guestバッチへ引き渡した。まだguest結果を受信しておらず、追加fixtureの作成を実行成功と報告しない。独立VMは起動していない。

その後の初回実guest（`build/media-formdata-header-closure-20261012/test-serial.log`）では `HTMLFormElement is not available in this realm` が発生し、追加まとまりは14確認・4失敗。Windowのprivate `rawDom` は操作methodを持つcallable functionなのに、moduleのrealm判定が `typeof rawDom==='object'` だけを認めていたのが製品原因。`js_formdata.js` を既存callable dispatch契約へ合わせて修理した。公開constructorの存在や偽hostへ差替えて成功にしない。WorkerはDOM capabilityなしを保持する。修理後guestは主担当再実行待ち。先のfixture-header配置不足2回は主担当が `scripts/test.py` で修理してから今回の実製品失敗に到達している。
