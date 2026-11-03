# MLFQ・描画性能・SuttaCentral 実装修理（2026-10-08）

## 実装したもの

### CPU・描画の仕事削減
- kernel/src/sys/sched.c/h：四段FIFOのMLFQ。実行枠2/4/8/16ms、累積降格予算8/16/32/64ms、250ms boost。短いsleep/yieldで返金しない。校正済みTSCとsub-ms会計、優先task起床時の安全な再選択へ接続。
- AP会計・再登録・解放はretire ACK後のBSP所有を維持。一般user taskはBSP＋一つのAP、二つのAPはpure worker、GUI/browserはBSP固定。四つの一般user CPUや新しいkernel preemptionの完成ではない。
- kernel/src/gui/wm.c：raw clientがclip全体を覆うときだけ隠れた下位描画を省く。部分遮蔽/shadow/border/角丸titleは遮蔽物にしない。元の参照経路とwmocclusion=offを保持。
- kernel/src/mm/vmm.c/h、kernel/src/sys/proc.c：map/unmap/freeにexact owned-page counterを接続。反復プロセス一覧のpage-table全走査を除いた。present非shared leaf、GUI共有除外、PID再利用、ACK、容量超過時の旧full walkを維持。推測値や期限付きcacheではない。
- user/libc/image.c：64bitで画像・clip・物理canvasを交差し、有効画素のポインターだけを作る。x+w overflow、配列外の-x行ポインター、画面外clipを修理。alphaとgfx_mixは保持、copy/allocationを増やさない。

独自ABI、spawn、RAM root、/dataを維持。fork/POSIX、DRMは追加しない。CPU compositorの実仕事削減であり、hardware GPU 3D/WebGLの完成とは扱わない。

### フォント
user/libc/font.c：固定4096エントリー・1024組×4wayの実face/glyph解決cache。missingも記憶し、元の候補順・幅・scale・rasterを保持。約97KiB/process。全Unicode配列やfont複製なし。

CSS/CanvasはSans/Serif/Monoを分け、既存fallbackを必要時にだけ開く。許可済み追加八ファイル92,840,580 bytesを維持：Noto統合四、Plangothic二、後から許可されたCJK Sans/Serif Regular各一。他の新規fontなし。

独立比較6000文字×12face一致、warm cmap探索4,440,000→0。局所中央値58→7msはOS全体の速度比ではない。

### SuttaCentralのメニュー・リンク・寿命
対象は「どのリンクも同じDiscourseへ飛ぶ」「右上メニューが画面外に開く」。Discourseの見た目やUA応答をその修理と混同しない。

- native LTR layoutとcomputed directionを一致。空文字をMaterialがRTL判定していた。サイト専用clamp/座標固定なし。
- 実PointerEvent、有限animation時計、private数値・色cascadeを接続。通常宣言＜animation＜important。author属性を毎frame書き換えない。一つのsample失敗は対象だけ解除・報告。detached style寿命も修理。
- Document/ShadowRoot.elementFromPointを実rendererのhit treeへ接続。shadow retargetとslotを維持。pointer-events:auto/noneの継承・cascadeをnative hitへ接続し、親none/子autoを扱う。
- physical/programmatic clickはdispatch前の同じAを捕捉し、listener後のそのAの最新hrefでactivation。別の祖先Aを再選択しない。cancel/control/form/label等の既存順を保持。
- QEMU PS/2をWHPXのMicrosoft hypervisor CPUIDだけでHyper-V入力と誤判定していた。checksum有効RSDPのQEMU/Bochs OEMを識別しnormal Yへ修理。明示mouse_yと未知Hyper-V判断は保持。QEMUで確定した原因を未実行Hyper-Vの全リンク根因と断定しない。[QEMU OEM定義](https://github.com/qemu/qemu/blob/master/include/hw/acpi/aml-build.h)
- history callbackがSVG cacheを解放した後の旧box描画による本物のGPFを修理。通知後・redraw前に既存native snapshotを再構築。finish_navigationとconsole evalにも同じ寿命境界を適用。例外やNULLで隠さない。

### nativeフォーム連携とSVG
- native FACE/ElementInternals：node所有のvalidity/message/anchor、値/state snapshot、form owner、fieldset disabled/first legend、label、実native送信collector、reset/associated/disabled reactionsへ接続。
- string/File/FormDataは実長/NUL/binaryを保持。64entries/16MiBと既存DOM family quotaで有限化。setValidity例外無視のfacadeではない。NotFoundError前のflags/message更新も[HTML規定](https://html.spec.whatwg.org/multipage/custom-elements.html#dom-elementinternals-setvalidity)に従う。
- upgrade/cloneの不要な全FACE再走査を削減。各木mutationの中間owner/disabled反応は同期に保持。
- Map配信LeafletはcreateSVGRect欠落でrendererがnullとなりin例外になっていた。native SVG rootのfactory、真の可変DOMRect/SVGRect、private brand、四辺/fromRect/toJSONを実装。既存SVG serializer→rasterへ接続。[SVG2](https://www.w3.org/TR/SVG2/struct.html#InterfaceSVGSVGElement)、[Geometry Interfaces](https://drafts.csswg.org/geometry/#DOMRect)

new FormData(form)のentrylist、履歴フォーム復元、全ARIA/CustomStateSet、全SVG geometry/path hit、完全CSS transform/additive/scroll timeline、touch/pen/captureは完成扱いしない。

## ビルドとnative結果
最新build/browser-menu-20261008/build-map-image.log：終了0、警告なし、raw/VHD/VHDX/ISO生成。WHPX、4vCPU、2048MiB、std VGA、隔離snapshot。個人の永続dataを接続しない。Hyper-V Gen2受入れは人間側。

build/nocturne-platform/mlfq-pages-native-final-20261008：
- 負荷四子＋GUI子5/5完了、GUI BSP pin成功。p50=0/p95=0/最大194ms、GUI wall1583ms。
- user overlap31、XSTATE/stack、fault CR2/kill/retire成功。故意addr12345000 faultは負経路。
- font245件失敗0。wmbench full/clipped/tiny/odd-pitch/extrema/guards、AP mask=d成功。
- host WM oracle3529、page counter1032、image画素/番兵2050件一致。counter warm10000照会はfull walk0。

同じ最新kernel、initrdは当時のものと区別する。初回の/tests起動指定は親の誤りで、実/data/testsへ一回直して成功。製品成功へ混ぜない。

旧RR→MLFQ各一回はp95=11→4ms、最大220→227ms、GUI wall1705→1867ms。媒体/cache/起動wmbenchが異なり因果の単独比較ではない。最新最大194msも残る。全OSのlag解消、全CPU/GPU性能完成を主張しない。

## 実Suttaの対象成功
build/nocturne-platform/sutta-links-native-fixed-20261008：
isTrusted=trueの実マウスでIntroduction→戻る→Donations→戻る→Map、それぞれ正しい/introduction、/donations、/map。history GPFと実setValidity例外は消えた。Map renderer例外は当時残り、後段のSVG実装で修理した。全65リンクを実クリックした主張ではない。

最新媒体build/nocturne-platform/sutta-menu-cards-map-final-20261008：
実マウスの開く→閉じる→再開は全て成功。menu left807/right1082/width275/height378で画面内。次のカードの空白部分への移動でDOM mousemoveが発行されず、入力補助の応答待ちが失敗。全体時間切れや製品停止と断定しない。harness全合格ではないが、メニューの実証と区別する。

カードのみの実マウス補助も同じ応答待ち条件で停止した（sutta-cards-map-final-20261008）。既存hoverの空targetではmousemoveがdocumentへ発行されないため、補助が最新座標としてDOMログを使う方法は不適切だった。これを製品の全リンク失敗とは数えない。

本物ページのカードをコンソールからクリックする最終受入れはbuild/nocturne-platform/sutta-cards-map-dom-final-20261008。サイトhref・作者コード・DOMを差し替えず、nativeプログラムclick→実router→historyを使う。native実入力とisTrusted=falseのDOM操作を区別し、最終結果を下段へ記録する。

### 最終受入れ

同じ最新媒体で三つの既存カードは、それぞれ /pitaka/sutta、/pitaka/vinaya、/pitaka/abhidhamma を開き、戻る操作も完了。続くMapも正しい/map。isTrusted=falseのDOM clickとして記録し、実マウスの証拠へ混ぜない。harness終了0、全marker、クラッシュ・対象例外なし、隔離QEMU停止済み。

SVG rendererは実ページで29本のpathにdを生成し、15枚のtileを15枚ともロード。以前のnull renderer例外は無い。ただし最終画面は地図上方の見出しまでで、地図本体は viewport 下方にある。地図初期化と通信の成功は確認したが、完全なpan/zoomや全画素表示をこの画面の証拠とはしない。

メニューと三つの上部リンクは実マウス、三つの別カードは実サイトDOM clickで対象成功を確認した。全65リンクの総当たり検証は行わず、目的の修理を終えた後に同じサイト確認を無制限に繰り返さない。媒体・source hash・失敗の区別はbuild/browser-menu-20261008/final-verification.jsonに保存する。

## 五つの実サイト・媒体の境界
前段release媒体で次の五つをNocturne内に表示し、親が画面を読んだ。Wikipedia/架空互換ページ/HTML5test点数だけではない。現在のAPI全体の回帰合格ではなく、媒体hashで区別する。

|サイト|実観測と限界|
|---|---|
|SuttaCentral|実メニューの開閉再開、275px幅、ripple時計。旧FACE例外は今回実装で修理|
|Google|公開ページとCanvas Sans/Serif/Mono七条件true。認証操作なし|
|Baidu|実検索欄548×44が画面内、focus/activeElement/CJK。logo/titleの?残り|
|DuckDuckGo|本物nocturne os検索結果、実JS/timer。body前診断でnullを渡しmarker不成立。sendBeacon/icon残り|
|discourse.suttacentral.net|公開本文と簡易crawler応答。全Suttaリンク修理の根拠ではない|

記録はbuild/nocturne-platform内のsutta-menu-release-20261008、google-release-20261008、baidu-release-bounded-20261008、duckduckgo-release-corrected-20261008、discourse-release-20261008。metadata/serial/画面を保存。初回Baidu時間不足、DuckDuckGo URL起動ミスも別保存。

## 引渡し
担当者は具体的な差分を実装し、親が報告・第二考察・sourceと限定native/実サイトを独立確認して統合。過去menu/MLFQ担当も再開可能であり、不在を推測して退役にしない。

大きな残りは長いBSP cooperative区画、一般multicore ownership、hardware GPU/3D/WebGL、その他未完成web API。「日常OS完成」「全サイト対応」とは呼ばない。ユーザー差分・個人data・許可fontを保持する。
