# HTML解析・DOM・CSS改善：停止時点

2026-10-11、ユーザーの「まとめて停止」に従って追加実装・build・試験を停止。3担当を中断し、保存済み変更だけ報告を受領した。停止処理時に全3試験コマンドは既に終了しており、実QEMUは0台だった。Hyper-V・ユーザー永続dataは操作していない。

## 保存済み製品変更

- HTML：既存Lexborのtokenizer/tree builderを維持し、DOCTYPE/srcdocの文書モード、fragmentの文脈とscript活性、入力待ち／EOF／取消／協調yieldを区別。stream/scriptの世代と寿命、document.write/open、stylesheet待ち、完了イベントの作者再入を修理。
- native DOM：Lexbor変更通知と差分transaction、node identity・名前空間・owner、Shadow/template/CE registryを接続。clone/import/adopt、frameの初期公開、SVGGElementの本継承とnative geometryを修理。
- 通信：Fetch/XHRのnull本文と空本文をwire/HTTPまで区別。POST/PUTと他method、redirectによる本文保持／破棄、Cookie境界を維持。サイト専用例外は追加していない。
- 画像：合法な透明1×1 palette WebPで幅0除算するデコーダーを修理。Amazonの画像消失は共通float/百分率高さ／匿名箱の参照境界とreplaced sizingを修理し、画像処理を描画まで接続。
- CSS/geometry：実inset・z-index・box sizing・計算長さAST serialization、flex絶対配置とauto margin、小数DOMRect・principal client寸法を接続。
- filter：実none／opacity()関数列・computed/used値、HTML subtree／inline／atomic SVG rootの一括合成、非noneのabsolute/fixed CB、取消／OOMのframe保全を追加。未実装blur等を対応扱いにしていない。内部SVG groupは未接続。
- 補助：試験selectorの16個黙示切捨て、起動／focus確認、公開IDLにないfixture参照、JS文字列と実HTML境界の混同、Shadow CSS所有arenaの計数を訂正。得点や期待を弱めて合格を作らない。

主な製品配置：`user/libc/web/{html_lexbor.c,html_lexbor_bridge.c,doc.c,dom.c,js.c,css.c,cssprop.c,box.c,layout.c,paint.c,webi.h}`、既存JS bindings、Lexbor DOM内部、`third_party/img/jebp.h`、`user/{apps/webfetch.c,apps/browser.c,libc/http.c,libc/webnet.c,include/webnet*.h}`。生成bindingは正規js_embed.pyで再生成した。詳細な段階別変更は`docs/html-parser-20261011-work.md`と担当reportに保全。

## 媒体と試験結果

- 最後の全buildは`build/html-parser-20261011/build-filter-geometry.log`、正規embed/all成功。IMG/VHD/VHDX実内容比較、ISO内kernel/initrd、initrd内browser/worker/webfetch/libcの一致を主担当確認。VHDXも生成済み。
- initrd SHA256：`057bdaa43c0157ce9a55096804ed0f2755315edf4a2ef55388860a350c7fdd57`。browser：`e1329e73a0f8d5e77b97f6ea4143659716fa300ecf4f172ee9d39f22a25a3131`。
- この媒体の影響26群：**23合格／3失敗／skip0**、guest732秒／host778秒。HTML tree選択corpus610/610（parse error検査なし）、tree1014、SVG109、JS解析安全API58、script lifecycle、入力／encoding、Shadow／forms／画像寿命等は合格。
- 失敗① web/engine：415条件中4失敗。新Shadow filter固定CB fixtureだけcustom hostの既定inlineにblock寸法を期待していた。display:block明示の訂正は未保存。別途、現行CSS Position3のinline content-edge基準に対し製品はpadding-edge基準である不足を発見したが、製品修理も未実施。
- 失敗② 全JS：1197条件中10失敗。無効style setterが旧宣言を消すCSSOM失敗が2回ずつ（計4）。無効宣言を属性commit前に拒否する修理は原因特定のみ。残6は停止後DCL/loadがnative deadlineを通知し続ける境界。`js.c`の作者停止ガードと`tests/jstest.c`の計時／idle診断は停止前に保存されたが、**最後のbuild媒体には未収録、build・実ゲスト未確認**。
- 失敗③ CSS supports：364条件中2失敗。取消groupが外側で退役し、旧bitmapではなく現在native DOMを再描画する新条件が未達。停止時点で原因修理未完。
- 直前adoption-cssom媒体の29群は24合格／5失敗。未変更通信・Cookieの合格と基準worker6失敗も保全。最新26群からworkerを外したことを全体合格とはしない。古いSVG exact prototype、架空Element registry、hover=false期待等の元FAILと訂正理由は履歴に残した。

## 実サイト・主担当目視

全サイトは隔離QEMU、WHPX／4CPU／4096MiB／1920×1080。JavaScript consoleと資源失敗も観察し、observerは読取のみ。QEMU成功、HTTP200、採点は表示成功の代用にしていない。

- **最後のfilter媒体・Amazon**：実nocturne検索に到達し、添付と同じSelvaren書籍と2026/2020映画のbitmapをscreen7（console付き）／9で主担当が確認。以前のalt文字だけの領域は解消。ホーム画像も前媒体で高さ298×477へ修復し目視済み。最新QAは300.42秒で終了。最新の上下／戻る／F5は入力・ログがあるが、停止時点では全操作後画像の最終目視まで完了していない。以前の媒体ではそれらを目視受入済み。暗号処理未対応・広告frameのreferrerエラー等は残る。
- **最後のfilter媒体・Reddit**：screen7（console）／8でtooltipが左端ではなく実ボタン下へ復帰。computed.filterの空値と小数geometryの共通修理を作者配置へ接続できた。screen10で実下入力後の本文と次ページエラーを主担当確認。menuは実click／aria-expandedまであり、popup幅256・高さ0で**本体未表示**。次ページ失敗・header badge等も残る。QA300.29秒で終了。challenge suffixを含む画像は最終提示に使わない。
- **前媒体・html5test.co**：透明WebP修理後、338/588まで採点しbrowser窓が生存。console／下部不足項目／about実click／戻る／F5／上下を主担当目視。現行PI仕様を古いcomment期待へ退行させず、tokenizer Noは残る。最終filter媒体では未再確認。
- **前媒体・html5test.com**：319/555、下端の未対応項目、about／履歴／再読込／上下を主担当目視。採点だけでWHATWG全面対応とはしない。最終filter媒体では未再確認。
- **前媒体・百度百科**：全8文字の実検索、`/item/nocturne/56893650`の中国語本文・書影、上下／戻る／F5を主担当目視。Axios資源失敗、skeleton、固定ヘッダー折返しは残る。最終filter媒体では未再確認。
- **直前adoption-cssom媒体・OpenAI指定記事**：Esc後の複数PageDownで日本語本文と図に到達し、PageUp／最終F5＋consoleを主担当目視。WebGL黒hero、図の欠字・配置と資源403/404は未解消。最終filter媒体では未再確認。

最初のfilter Amazon／Reddit QAは主担当の誤った`/data/bin/browser`指定でブラウザー起動に達せず終了。実在する`browser`へ訂正した別ラベルのQAが上記最新結果。製品クラッシュに数えていない。

## 停止・保全・残件

- 保存済みsourceは未commit。停止後の製品修理・追加build・試験は行っていない。
- 専用metadata停止済み／user_data_attached=false、実QEMUなし、解決済みbuild内・リンクなし・実非使用を確認し、今回最後の5出力に属するboot/data **10個だけ非再帰回収**。PNG/log/metadata、基準boot、配布IMG/VHD/VHDX/ISO、ユーザー永続dataを保全。D空き135.58GiB。以前拒否された旧媒体の広域削除は迂回していない。
- 最終保存deadline差分のbuild、上記3群失敗の解消、最後の媒体で全6サイトと全操作の再受入は残件。
- 厳密な同条件性能比較と、以前の「大幅に遅くなった」回帰の解消は未確認。Hyper-V実機はユーザー担当・未試験。
- 単一巨大token内やdoc_sync_tree等の長いnative全走査の中断、内部SVG group、全filter／CSS／DOM／WHATWGの全面対応は未完。API名・固定値・空実装で対応を偽装していない。
