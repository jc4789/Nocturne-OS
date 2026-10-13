# Window named access：反例と所有境界

1. **idをglobal自身へ定義すると、削除・改名後の値やvar宣言が壊れる。**
   名前はWindowPropertiesの動的getter。ordinary global ownのshadowを優先し、
   var用にQuickJSのglobalobjectを置換しない。
2. **重複snapshotをHTMLCollectionと称するとretained collectionが壊れる。**
   既存 live readerから都度専用epochを読む。要素が全て外れてもreadは文書へ
   根を持ち、新しい同名要素を再取得する。id/nameの同一nodeは重複しない。
3. **light DOMとflat/shadow treeを混ぜると非公開名がglobalに漏れる。**
   native普通子リンクだけを走査し、shadow_rootへ降りずtemplateを除外する。
   detached作成だけではdocument indexへ登録しない。
4. **通常dom_revisionをkeyにするとstyle animationでも全木を読む。**
   構造・null-namespace id/name・navigable target name専用epochを使う。
   新indexは取得成功後だけpublishし、OOMで部分cacheへ置換しない。
5. **iframe name属性だけではwindow.nameと以後のnative syncが競合する。**
   target名と最後の属性文字列を別所有。属性が本当に変わった時だけ
   targetを置換し、frame破棄時に両方を解放する。
6. **inactive realmからnameを読む／書くと新frame ownerへ漏れる。**
   starting以外のinactive guardを追加。旧WindowProxyのclosed/origin判定は
   既存責務へ依存し、エラーを握り潰さない。

この考察はnative実行、原WPT、実サイトの代わりではない。親の最新媒体で
新42assertと元layer bare targetを確認し、失敗があれば製品側へ戻る。
Redditの新read-only captureは正MIMEのpartial200後ReferenceErrorを確認したが、
sidebar自身の後続eventは旧24診断枠を全loader開始が使ったため未到達。
無関係loader開始を除外し、標準公開missing identifierとnamed-node booleanを
取る新observerは未実行。任意message/body/token/Cookieは保存しない。
