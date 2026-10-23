# Window named access：製品修理

## 原因と仕様

原 `layer-vs-inline-style.html` は DOM id `target` を裸のglobal identifierで
読む。元製品は Document の idが存在しても global/prototype に名前getterがなく、
ReferenceErrorで原assert前に停止した。試験へ代替targetを定義せず、
[HTML Window named access](https://html.spec.whatwg.org/multipage/nav-history-apis.html#named-access-on-the-window-object) と
[WebIDL named-properties object](https://webidl.spec.whatwg.org/#named-properties-object) の共通責務を実装した。

## 製品接続

- `js_frames.js` の既存frameBridgeへ WindowPropertiesを接続。global本体の
  identity／document.defaultView／既存イベント送信を保持。通常global own値と
  prototype built-inが優先し、作者var／代入はordinary own propertyでshadowする。
- 動的名前をreal own一覧へ固定配置しない。named-objectのdescriptorは
  writable/configurable、non-enumerable。ownKeysは名前を列挙しない。
- `frame_native.h` は普通のdocument-treeだけを反復する。idは全namespace、
  name属性はHTML embed/form/img/object。shadow/template/detachedは混入させない。
- 重複は既存 HTMLCollection に接続。保持されたcollectionは0/1/複数要素へ
  変化しても文書に根を持つ。各nodeのid/name一致は一回だけ。
- `dom.c` は構造・id/nameの専用epochを更新。style/rAFやshadow内部変更で
  全documentの名前indexを再構築しない。削除では既に外れたsubtreeではなく
  変更されたparentのrootを判定する。
- `frame.h/frame.c/frame_native.h` のtarget_name/name_attributeは別owned。
  window.nameの書換えを以後のsyncで古属性へ巻き戻さず、HTML name変更は
  target名へ反映する。名付きnavigationは同じmetadataを使う。
- 取得／変更はinactive document guardを通り、退避realmのborrowed name setter
  から新文書のframeへ結果を渡さない。nativeチェックポイント、OOM、実解放を保つ。

## 確認範囲

- 正規 `js_embed.py` は最新CSSOM sourceとの交差後に再生成し、source/includeの
  sort・custom list・CSSStyleProperties継承も照合した。
- C syntax-onlyは js.c/dom.c/frame.c/jstest.c exit0（元のwarningのみ）。
  JS構文検査とCRLF許容diff checkもexit0。
- `tests/js_frames_cases.js` に独立42assertと `jstest window-named` modeを追加。
  親がnativeで実行するまで未検証とする。
- 原WPT named-access basics6、live-collection2が関連。原assertは変更しない。
  Window indexed property／異なるorigin・navigationの全仕様対応をこれだけで
  保証しない。今回の実装は既存WindowProxyのorigin再検査へ依存する。

Reddit sidebarの成功やGitHub実表示、性能比較をこの静的確認から主張しない。
