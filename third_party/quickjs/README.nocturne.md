# Nocturne の局所変更

`quickjs.c` の ArrayBuffer backing allocation 前に、要求バイト数を含む
既存 cycle GC の判定を行う。適応 GC 閾値が有限 runtime limit を超えた場合も
割り当て前に回収機会を与える。`SIZE_MAX` による自動 GC 無効化は維持する。

HLS 公式 demo の約13MB remux buffer が、使用量約123MB（117.5MiB）/ 上限128MiBで
GC 機会なしに失敗した実例から追加した。生存 buffer の破棄、上限拡大、
allocator 内の再入 GC、同期での FinalizationRegistry callback 実行は行わない。
live データが上限を超える場合は、従来通り実際のメモリー不足を返す。

## 読み取り専用アクセサーの診断

`call_setter` に既存のプロパティ atom を渡し、setter がない例外に名前を
含める。atom の所有権、Strict mode の判定、例外になる条件は変更しない。
ページの getter や文字列化処理を追加で実行しない。
実 YouTube の曖昧な例外が `style` 代入だと判明し、ブラウザー側で欠落していた
CSSOM `PutForwards=cssText` を実装するために利用した。

上流の著作権・ライセンスは変更しない。

## modulepreload の構文解析と依存解決の分離

上流の `JS_EVAL_FLAG_COMPILE_ONLY` は module の依存も同期で解決するため、
未使用の preload から依存取得を開始し、HTML の root preload 完了を待たせていた。
`JS_EVAL_FLAG_MODULE_PARSE_ONLY` を局所追加し、module と compile-only の両方を
指定した場合だけ root の構文解析で返す。他の eval の動作は変更しない。

ブラウザーの module map が返された module 参照を保持し、実際に import／script が
必要になった時点で既存 `JS_ResolveModule` を呼ぶ。preload だけでは作者コードを
評価せず、必要になった取得中の依存はネイティブ要求へ join する。root の構文エラー、
後の依存解決エラー、URL／redirect base、文書破棄時の参照解放はブラウザー側が扱う。
関連する限定回帰は `tests/jstest.c` の `modulepreload` 条件群。

## モジュールの依存の一覧と非同期の import()

上流の module loader は同期の関数で、`import()` も依存の解決もその場で取得を
待つ。ブラウザーでは主スレッドがネットワークを一つずつ待ち、依存の多い頁の
読み込みが直列になっていた。HTML の fetch the descendants of a module script
のとおり、ブラウザーが解決の前にグラフを並べて取得できるよう、三つを足した。

- `JS_GetModuleRequestCount`・`JS_GetModuleRequestName`：構文解析しただけの
  module が求める指定子を、元の順で返す（atom は呼ぶ側が解放する）。
- `JS_SetModuleImportFunc`：`import()` の job が module を host へ渡す。host は
  resolving functions を自分で保持し、グラフがそろってから
  `JS_LoadModuleResolving`（上流の `JS_LoadModuleInternal` と同じ）で終える。
  hook が無ければ上流どおり同期で読む。

評価・循環・エラーの扱いは上流のまま変えない。解決の失敗の後始末だけを足した。

構文解析しただけの module は、作った時に `loaded_modules` に名前で載る。上流の
`js_resolve_module` は依存を探す前に `resolved` を立て、失敗しても戻さない。
上流では依存は compile の中で解決され、失敗した module は compile の失敗で消える
ので困らないが、構文解析と解決を分けると、失敗の道の上の module が「解決済み、
依存は NULL」のまま残り、次の import が名前で見つけて NULL を link して落ちる。
一番外の `js_resolve_module` が失敗したときに、解決済みの印を持つのに依存が NULL か
未解決の module を（それに届くものも）未解決へ戻し、依存の参照を消す
（`js_unresolve_broken_modules`）。外の解決が進んでいる間は、まだ見ていない依存が
NULL なので戻さない。戻した module は上流の `js_free_modules` が一覧から外し、
次の import は host の loader から同じ module を受け取って解決し直す。

## 初めて使う時に作る属性

`JS_DefineLazyProperty` は上流の `JS_PROP_AUTOINIT`（組み込みの prototype や
module の名前空間を、初めて触れた時に作る仕組み）に、host の作る値を足したもの。
属性は最初からデータの属性として見え、読む・記述子を取る・書くのどれかで初めて
host の関数が値を作る。ブラウザーは Web Audio の interface をこれで置き、使わない
realm では読み込まない。上流の三種の autoinit と、属性の位置と値の扱いは変えない。

## RegExp の旧式静的キャプチャー

実 Baidu の公開 ESL loader が `extReg.test(source)` の直後に
`RegExp.$1` を拡張子として使っており、欠落値 `undefined` がスクリプト等の URL に
後置されていたため、JavaScript monkeypatch ではなく native exec 成功境界で
`$1`〜`$9`、`input`、`lastMatch`、`lastParen`、`leftContext`、`rightContext`
と各別名を実装した。exec を迂回する既存高速 replace 経路も同じ更新を行う。

保持するのは realm 単位の subject 参照と UTF-16 offset 表だけで、成功のたびに
全文 context や capture string を複製しない。getter 呼出時に必要な部分文字列
だけを生成する。失敗した一致では前状態を維持し、getter の割当失敗でも状態は
変更しない。未参加 capture は空文字、lastParen は9を超える最後のcaptureも
扱う。`input`/`$_` への代入は文字列化し、他のcaptureを変更しない。

一次資料は https://github.com/tc39/proposal-regexp-legacy-features の Stage 3
legacy draft（最終ECMA-262本文ではない）。このdraftのrealm/receiver境界に
合わせ、別realmのexecを借用しても相手のstatic状態を変更せず、RegExp subclass
による成功はそのrealmのlegacy状態を無効化する。getterは無効なreceiver/状態で
TypeErrorを返す。input以外はsetterなし、全accessorは非enumerable/configurable。
既存 `compile` の挙動全体は今回の修理対象に含めない。

RegExpから作成realmへの所有参照には既存の `JS_DupContext`/`JS_FreeContext`
方式とGC markを使い、context解放時に保持subjectを解放する。補助回帰は
`tests/js_regexp_legacy_cases.js`。実Baiduの表示・操作受入れとは区別する。

## 関数の元の文の共有

上流は関数ごとに自分の元の文（`Function.prototype.toString` 用）を
`js_strndup` で写すため、n 段に入れ子になった関数は同じ文を n 回持つ。
実 YouTube の 10.8 MB の bundle では、別スレッドの翻訳の大半がこの写しと、
それを bytecode に何重にも書く memcpy だった。

構文解析の入力を、最初に元の文を残す関数が出た時に一度だけ参照数付きの
`JSFunctionSource` へ写し、各関数はその中の位置と長さを持つ。bytecode には
同じ文を一度だけ書き、後の関数は番号で指す（`BC_VERSION` 6）。読み戻しは
番号ごとに一つの写しを作る。toString の結果、行番号、strip の扱いは変えない。
関数が一つでも生きていれば入力全体が残るが、その量は入力一つ分を超えない。

## WindowProxy の番号の性質

HTML の「The WindowProxy exotic object」では、窓の番号 P（array index）は
document-tree child navigables の P 番目の active WindowProxy で、
{value, writable: false, enumerable: true, configurable: true}。番号の define と
set は false、delete は子がある間 false、[[OwnPropertyKeys]] は 0..n-1 が先。
ブラウザーは realm の global object を WindowProxy として使うので、global に
この口を足した。global の類に is_exotic を立てる一般の exotic にはしない：
global には global の変数の道（var_ref、uninitialized_vars、JS_GetGlobalVar）が
あり、exotic にするとそこを外れる。

`JS_SetGlobalIndexMethods(realm, methods)` で realm ごとに count（子の数）と
get（番号の値、値を求めないときは有無だけ）を渡す。global object は自分の
realm を弱い参照で持つ（`JSGlobalObject.realm`、JS_FreeContext が消す）。
JSObject の大きさは変わらない（union 32 バイトの中、host の x86-64 で確かめた）。

口は global object の番号の鍵だけに効く。足した所と原文の手続き：

- `JS_GetOwnPropertyInternal`：[[GetOwnProperty]] 手順 2（自分の性質を探して
  無かった後、atom の印のビットを試してから）。
- `JS_GetPropertyInternal`：[[Get]]（OrdinaryGet を上の [[GetOwnProperty]] で）。
  子が無ければ prototype へ続く。
- `JS_HasProperty`：`JS_GetOwnPropertyInternal` を通るので変えていない。
- `JS_SetPropertyInternal`：[[Set]] 手順 3.1。prototype の道で global に着いた所で
  false（strict なら TypeError）。prototype の setter も受け手も見ない。
- `JS_CreateProperty`：[[DefineOwnProperty]] 手順 2.1。データの性質は既存の
  global の枝の中、アクセサーは類の比べ一回の後。ここだけ 2^31〜2^32-2（文字列の
  atom）も `JS_AtomIsArrayIndex` で拒む（[[Set]] の 2^31 以上はここで止まる）。
- `delete_property`：[[Delete]] 手順 2.1。毎回 get に有無を聞く（覚えない）。
- `JS_GetOwnPropertyNamesInternal`：[[OwnPropertyKeys]] 手順 2〜4。count は形を
  読む前に聞く（host が script を走らせうる）。for-in、Object.keys などはここを通る。

global object は番号の自分の性質を持たない（define と set が作らない）ので、
番号の答えを普通の自分の性質の探索より前に置かなくてよい。番号でない鍵は
上の道で atom の印のビットの試し一回か類の比べ一回だけを足し、自分の性質が
見つかる道は変わらない。IsPlatformObjectSameOrigin は真とみなす：別の origin の
script は global object を直接持たず、ブラウザーの WindowProxy の Proxy を通る。

## 標本取りの問い合わせ

`--debug-js` の JavaScript の時間計測（`user/libc/web/js_profile.c`）のため、
割り込みの handler から呼べる二つの関数を足した。`JS_GetRunningFunction` は
いま走っている bytecode 関数を、その関数が生きている間だけ通用する識別子として
返す。`JS_DescribeFunction` はその名前、定義された script、始まる行と桁を返し、
JavaScript を一切走らせない。桁は、一行に詰めた（minify した）script の関数を
見分けるため。解釈器の動きは変えない。
