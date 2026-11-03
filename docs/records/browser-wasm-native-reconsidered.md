# WebAssembly ネイティブ境界：第二考察

## 最も弱かった前提と修理

全 callback を global State の mark に集約する前提を撤回した。native Store の参照関係に対応する QuickJS の anchor graph を構成し、Instance は import と resource、export 関数は Instance、Table／Global は実際の参照先だけを保持する。Table の変更範囲と参照 Global の更新通知を WABT の所有担当へ依頼し接続した。呼出しごとの全テーブル走査や、累積作成数による固定上限は追加していない。

State と Handle／constructor の C++ 所有権を shared_ptr で分離し、QuickJS の循環 finalizer がどの順序で来ても native State が先に消えないようにした。Store は補助コンテナより先に破棄する配置にした。function identity は弱い順引き・逆引き map で維持し、finalizer が登録を取り除く。

## 別の検査から見つけた境界

- QuickJS の `JS_GetTypedArrayBuffer` は DataView を受け入れない。初期化時に捕捉した DataView の内部 getter で byteOffset と byteLength を取得し、作者の同名 property を信用しないようにした。
- 複数戻り import と compile builtin sequence は、作者が上書き可能な `Array.from` へ委任せず、捕捉した well-known Symbol.iterator を使って iterator protocol を直接処理する。
- import／export 名は C 文字列で切り詰めず、長さ付き Atom で扱う。
- unknown builtin set 名は無視し、重複を拒否する。未使用の js-string 指定だけでは拒否しないが、未実装の文字列組込み import を実際に使う Module は CompileError とする。primitive compile-options は WebIDL dictionary に従い TypeError とする。
- Memory／Table の格納確保は WABT 所有担当が fallible storage へ修理した。bridge は New の失敗を確認し RangeError とし、grow の失敗を成功と偽装しない。
- Memory の最大 page 数は Core の address space と host の size_t overflow に基づく。i64 address は BigInt の符号と 64-bit overflow を確認する。
- Instance.prototype.exports は内部ブランドを調べる getter とした。mutable Global でない primitive import は、数値型／BigInt 型を確認する。

## 検証と残件

修理後の上記クロスコンパイルは終了コード 0。主担当へソース固定を通知し、最新製品による QEMU API 実行と百度百科・指定 YouTube・CNN の画面／操作／console 受入を引き渡した。実行結果をこの担当の静的確認へ後付けで合成していない。

残る要確認は、実 GC を強制した callback／Instance／Table の循環回収、ゲストでの import 再入、Wasm 自身の memory.grow からの buffer detach、異なる realm の独立 Store、巨大 binary の一つの function body 内での中断応答、実アプリの性能である。API の native counter は作者の成功した Module 数・byte 数、Instance 数、JS からの export 呼出し、trap、現在の wrapper 数を観測できるが、それ自体は画面や再生の成功を証明しない。
