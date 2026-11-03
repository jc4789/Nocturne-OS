# 検索・Webフォント・Worker修理

運用: J-space LIGHT、共同実装。開始時のソース差分なし。

## 目的と責務
- 主担当: 検索結果の実クリック／遷移、通信・フォント選択、統合ビルドと実QEMU。
- fonts: メモリWebフォントdecoderとそのビルド依存。既存UI／カーネルフォントを保つ。
- workers: 既存ネイティブWorkerを拡張するSharedWorker、接続port、所有権と配送。
- images: 共通ICO decoder。HTTP404を描画成功に偽装しない。

## ソース地図
- `user/apps/browser.c`: native入力、clickのdefault action、トップレベル遷移とnative window spawn。
- `user/libc/web/frame.c`: target／sandboxによるリンクの宛先選択。
- `user/libc/web/js.c`: host API、Worker生成のorigin／policy境界。
- `user/libc/web/css.c`: @font-faceのsrc解析とfamily選択。
- `user/libc/font.c`／`font_sfnt.h`: メモリfont decodeとglyph描画。
- `user/libc/image.c`: 取得した画像bytesの共通decode。
- `user/libc/web/js_worker.c`／`user/apps/browserjsworker.c`: 独立taskとIPC。

## 基準と受入
添付3画面とログを読んだ。検索本文とリンク文字は表示されるがfavicon多数が空白、WOFF/WOFF2/EOT decode失敗。HTML5testはSharedWorker未対応。検索結果の実クリック、戻る、下／上スクロール、font／icon表示、Workerの実通信を最新媒体で確認する。未変更の同条件試験は繰り返さない。

開始時は起動中QEMUなし。D空き約135GiB。隔離VM準備のMSYS権限制限は通常実行へ切り替えた。基準実サイトVM `ddg-baseline-20261011` を起動。ユーザーのHyper-Vと永続dataは操作しない。

## 実装と観測
- 基準媒体の実検索結果を主担当が目視。CSS取得resetを1回観測、別クリック条件の隔離VMではCSS取得成功。HTTP成功を表示成功と混同しない。
- `ddg-click-boundary/link-hover.png`で最初の検索結果の下線・状態欄URL・pointerを確認してPS/2クリック。`after-click.png`で元の結果のまま、遷移なしを確認。
- 配信JSの通常リンクはhidden frameへのpostMessageへ委譲してdefault actionを取消す。実`post3.html`の応答はHTTP200、CSP frame-ancestorsは同originを許可するが、製品のframe gateはそのdirective存在だけで全拒否していた。html_policy責務で正しいsource-list評価を修理中。
- FreeType/Brotliをメモリ限定decoderとして統合、WOFF/WOFF2/CFF/EOT対応。UI/カーネルフォント保持。native30条件で確認、実QEMUはまだ旧媒体。
- ICO decoder追加。native17条件。localStorage eligibility CHECKの不要I/O除去と共有head検証再利用、native14条件。別窓同期・永続・破損検出保持。
- HTTPヘッダーを1回の送信へまとめる。応答公開前のbodyless GET/HEADのconnection resetだけ元deadline内で1回再接続。POST／HTTPエラーを再試行しない。
- CSS srcの未対応format/techを取得前に除外。Webフォント成功でも変化した利用要素だけ再cascadeし、無関係なlayoutを保持。
- Workerは同processの文書/frame間共有を実装しnative12条件確認。別process共有用のNocturne専用named duplex endpointをkernel担当とworker担当が共同実装中。
- 古いHTTP dynamic-header host adapterは現行job定義と不一致でcompile失敗。製品実行の失敗ではなく、成功には数えない。
- 最初の統合全build成功、IMG/VHD/VHDX/ISO更新。DDGとHTML5testを並行撮影して自分で目視。DDGのFandom/百度favicon表示、HTML5testのGothic字体表示を確認。DDGのWOFF/WOFF2失敗は消え、MicroType圧縮EOTは非対応として拒否し次srcへ進む。
- 同媒体の実リンクclickで第二の欠落を特定: child WindowProxyからtop-level locationへのnavigationが未実装。例外と元画面の残留を確認したため、native root hostへのqueue配送を追加、sandbox/同origin/生きた文書世代の検査を保持する。まだ新修理媒体では未確認。
- 幅cacheのint16縮小に加えstb hmtx読出しがsignedで、仕様上のUFWORD幅65535が反転する不具合を発見。int32 cacheとunsigned読出しへ修理。初幅ケース2失敗は本体側の追加故障を特定した結果で、期待を弱めていない。
- 幅修理後native decoder34条件成功。root配送は別担当が独立レビューし、変換前後のlive/origin/ancestor/sandbox再判定、子frameのmetadata不在でrootへ落ちない検査を補強。
- 最初の媒体でDDGとHTML5testへ実PageDown、本文の続きと再描画を一括目視。PageUpで元の先頭へ戻ることを目視。native検索入力でWorker項目へ飛ぶ試みは目的の項目に到達せず、項目確認には数えない。
- Nocturne専用IPC／動的FDが実装完了。namespaceは最後のlistener参照で回収、pending peerへEOF/EPIPE/HUP。初期32FDは容量であって上限でなく、spawn／GUI／dup2／exitを整合。pollも64超をsnapshot／lost-wake保護付きで扱う。実ゲストは最終媒体で確認する。
- SharedWorkerは別process serverへnative channelで接続、clientごとのport/global IDと世代を配送。非空nameのURL不一致拒否、START前の接続待ち、初期owner離脱時の取得再選出を実装。timer heapは安定creation順、server pollのoutput探索を二乗から線形へ変更。

## 最終媒体・実通信
- `ddg-workers-final-build.log`の正規全build成功。最終QA専用bootのkernel/initrdを抽出hashで現buildと照合し一時抽出を回収。Worker本体sourceよりobject／appが新しいことも確認。
- kernel SHA256: `411f51ecee5d74bb80922f60325708e57a12d728ac73dcfa29af718a1aabbbca`。
- initrd SHA256: `affffde37a4c9928f9067501ad7461da834617a17945e4fbfce34056ce1ad01f`。
- 最初の専用QA媒体2組だけを最新payloadへ更新して再利用した。初回metadataのhashは初回画面の証拠であり、更新後の媒体identityには流用しない。
- 実Nocturne kernelのnativechanneltestは218条件、失敗0。96接続／700番FD／96件poll／双方向／EOF・EPIPE・HUP／独立detached serviceを確認。成功したこの検査は再実行していない。
- 共有Worker受入の最初の起動スクリプトは両browserをbackgroundにして終了し、initが正常poweroffした。Workerサイト通信はまだ走っておらず成功に数えない。起動スクリプトを最後のbrowserがforegroundで維持する形へ訂正し、成功済みkernel検査を削除して再実行を避けた。
- WHATWG公式SharedWorker demo3を独立browser process二つで実行。読み取り専用observerで実ページのroot／iframe出力を観測し、二窓合計でconnection #1/#2/#3/#4とpongが現れ、同じnative realmの実共有を確認。DOM/style/サイトWorkerの差替え・追加postMessageは行っていない。
- `final-link-hover.png`で下線／状態欄URL／実pointerを目視してクリック。`final-results.png`で百度百科の実URL・San Tai記事本文へ遷移を確認し、Alt+Leftで実検索結果へ戻る画面も目視した。リンクの操作は成立。ただし百度側ではAxios Network Errorと未読込の情報領域が残り、同サイト全機能対応とはしない。
- 両SharedWorker窓は実画像でconnection #1と#2の別々の表示／pongを主担当が確認。内frameの末尾は300px幅で見切れるため、番号#3/#4はread-only実出力も併用。Alt+Tabの実窓切替も成立。
- 最終成果物: IMG 335544320、VHD 335544832、VHDX 301989888、ISO 290873344 bytes。最新font／Worker／frame root配送sourceとobject一致。Hyper-V実機はユーザー担当で未実施。

## 終了前に見つかった専用Workerの配送不具合
- 実WHATWG素数demoへ通常のアドレス入力で遷移すると、正しいURL／見出しは表示されるが数値が空。専用native taskの起動ログはある。共有Worker成功でこの失敗を相殺しない。
- 原因: native postMessageは出力queueへ入れるだけで、author taskが戻るまで送信しない。公式例の終わらない計算loopでは親realmへ届かず、queueが増える。サイト用例外ではなく共通輸送・背圧を修理する。
- 診断用ログ・画面を保存後、今回起動したQEMU二台を停止。ユーザーのHyper-Vと通常dataには触れていない。
- 本記録の上の全build／hashはこの追加修理前のもの。追加修理後の媒体確認を別に残す。
- 共通Worker輸送に有限nonblocking送信と世代付きbyte-creditを追加。64KiBは処理量の窓でありmessage拒否上限ではなく、単一の大きいmessageは単独許容する。消費が遅いとnative pollで待機し、author JSの再入やtransferの二重確定を行わない。共有clientごとの量と寿命を保持する。
- 製品bootstrapで単一JS task中の2500 postMessage、背圧、LOADED保持、author再入防止、ArrayBuffer確定一回、peer死亡からの脱出をまとめて9条件確認。実サイト画面とは別の証拠。
- 独立レビューで退役文書の未配送実eventにcreditが返らないケースを発見し修理。共通返却helperをdispatch／破棄で使い、成功印で二重返却を防ぎ、共有参照の解放より先に処理する。担当の退役ケースを含む13条件と100KB recordを含む送信10条件が成功し、別担当が当該寿命境界を再レビューした。
- 最初の配送修理buildでは起動早期の二画面だけを撮影した。レビュー修理を先に入れるためVMを停止し、実Worker動作成功とは数えていない。
- `ddg-workers-delivery-build.log`で退役credit修理まで全build成功。専用媒体二組のpayloadを読み取り専用mtypeで照合: kernelは上記と同一、initrdは `5e90071aea80a14d2616ad054590f2869701f76bc8e9a5ab86ac8e0ce29b9644`。最初の照合コマンドは引用符の構文エラーで実行前に失敗し、引用構成を訂正した。
- この配送修理媒体の公式primesで実数値を確認。読み取り専用ログは1069→35831へ進み、主担当が見た実画面は59207→124693へ更新。authorの終わらない計算中も親realmへ配送される。共有demoも二窓とiframeのconnection #1/#2/#3/#4とpongを再確認し、Alt+Tabで別窓を主担当が見た。
- 実Alt+F4でprimes窓が閉じ、foreground起動スクリプト終了により正常shutdownした（QEMU終了0、ログpower: shutting down）。その後のps入力／撮影は停止済みmonitorに接続できず、プロセス一覧による回収確認には数えない。
- 終了前の静的確認で、共有Workerが一切host呼出しをしない純CPU loopではlast-client EOFを読めない寿命欠落を発見。ネイティブだけの割込チェックへ限定修理する。配送修理の実素数成功を、この別経路の確認へ流用しない。
- 純CPU共有Workerの修理: 通常pumpと共通のnative acceptを1回16peerにまとめ、既存16周期の割込でstack64 FD batchのnonblocking pollを行う。全HUPと新規接続なしを確認したときだけ中断。QuickJS値の生成・解析・author callback・shared_disconnectは割込内で呼ばない。未acceptの死亡peerも有限batchで取り込み、live peerを保持する。実QuickJSの終わらないloop、80既存client、96死亡pending、live pending、scan中の接続をまとめたfocused8条件成功。実サイトpureCPU取消そのものは未実施。

## 性能と残る範囲
- 不要なlocalStorage資格確認I/O、HTTPヘッダー分割送信、フォント利用外layoutの無効化、Worker timer全走査、共有serverの二乗探索を削減した。実サイト全体の速度向上率・一般SMPは未確認で、全面的な高速化や無回帰とは報告しない。
- MicroType圧縮EOT／SVG font／完全な字形整形／可変font軸、module／nested Workerと一部clone／brokered port再転送は未対応。通常のWOFF／WOFF2とclassic SharedWorkerの成立とは区別する。
- HTTP404のfaviconは通信先の失敗として残す。偽画像・DOM／CSS差替え・UA偽装・サイト名分岐は入れていない。

## 引渡しと終了
- `ddg-workers-release-build.log`の最終全buildは成功。IMG 335544320、VHD 335544832、VHDX 301989888、ISO 290883584 bytesを更新。最後の変更は共有Worker割込内のnative寿命処理のみ。
- 最終kernel SHA256は `411f51ecee5d74bb80922f60325708e57a12d728ac73dcfa29af718a1aabbbca`、initrdは `afe26aff674221ef998c22763c87f766d42df1d3977797e4f1a6ddf51a613bfd`。最終起動QA媒体内の両payloadと照合済み、browserjsworker objectも最終sourceより新しい。
- 最終媒体のWHATWG共有demoを別browser二つで起動。実画面 `worker-release-early.png`／`worker-release-second.png`を主担当が見てroot #1/#2とpong、read-only実ログでiframe #3/#4を確認した。最終割込修理は共有経路だけなので、専用素数の配送証拠は直前の配送修理媒体のものとして区別する。
- 作業所有QEMUは全て停止、最後のcontrollerも終了0。担当も終了、追加検査は行わない。Hyper-V実機は未実施。
- 今回作った一時媒体7個（準備1、基準2、統合2組の4）を解決済み絶対パス／非リンク／VM停止を確認して回収。変換済みPNGがあるPPM45個も回収。PNG、ログ、最新版配布4形式、通常 `build/data.img` は保全。削除済みQA媒体はそのまま再実行可能とは報告しない。
- 回収後のD空き容量は144971825152 bytes（135.02GiB）。保存対象は小さい画面・ログ・条件と現配布媒体で、使い捨てVM媒体は残していない。
- 変更前の基準・途中修理・最終確認は本記録内で分離した。未変更の同条件検査を合格するまで反復せず、追加確認はいずれも製品修理または失敗した起動／引用構成の訂正に対応する。

一次仕様: [HTML Workers](https://html.spec.whatwg.org/multipage/workers.html)、[CSS Fonts 4](https://www.w3.org/TR/css-fonts-4/)、[CSP frame-ancestors](https://www.w3.org/TR/CSP3/#directive-frame-ancestors)、[OpenType hmtx](https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx)。
