# ネイティブfocusとtextarea寸法：独立第二考察

第一段の成功件数ではなく、別の反例から担当差分を再読した。

1. **イベントとfocusは別責務。** mousedown取消を無視してfocusしないこと、pointerdown取消によるcompat抑制もnative許可へ伝えることを確認。clickは独立であり、作者onClickによるfocus変更をclick後のUA再focusで上書きしない。BODYのままという不備は確定だが、GitHub menuの唯一原因と断言しない。
2. **候補とtargetを分ける。** SVG/inline子、slotを通るflat祖先、delegatesFocus、子文書の現在のframe chainを既存native helperで扱う。候補読取はfocusを変更しない。disabled/inert/removed/unrelated documentと通常背景の違いをbool/out契約へ残した。全DOM/全shadowの新たな探索は入れない。
3. **heightとscroll areaを分ける。** rowsを返すだけ、font-sizeに定数を掛けるだけ、空のHTML子を測るだけでは、native value/placeholderの改行・wrapと一致しない。実font幅と既存のused line-heightを用い、paintとmeasurementが同じ行記述を消費する。height:0でも内容行の領域を計算するが、CSS display:noneの箱を作らない。
4. **文字境界。** soft wrapはextended grapheme内を切らず、CJKの既存共通改行機会を利用する。0幅でも最低一つのgraphemeを進め、末尾LFには次の空行がある。CRLFの値は既存value責務でLFへ正規化し、placeholderのCRLFも一つのhard breakになる。完全なUnicode line-breakingや複雑な文字shape対応を今回の差分だけから宣言しない。
5. **性能と安全な中断。** 追加の全DOM走査・永続行array・毎文字の時計syscallはない。行幅のprefixを文字ごとに再測定しない。tab描画の初案はtabごとのprefix再走査になり得たため、局所offsetの線形進行へ修理した。64 cluster単位の既存checkpointを用い、pure AP layoutでは既存の取消観測のみ、通常layout/paintでは文書所有者のsafe checkpointを使う。性能改善は未測定。
6. **描画とscroll。** selection/caretは同じline spansを用い、soft wrap境界のcaretを次の行へ置く。native scroll offsetをcontrol文字だけへ適用し、padding clipを動かさない。scriptが選んだscrollTopをpaintから勝手に書き換えない。入力後のcaret自動reveal全体は今回の新受入条件として未確認。
7. **cascadeと内部領域の追加反例。** 原24条件は作者pre-wrapとwrap=offの優先順位を誤認していた。HTML renderingのhint契約を再読し、既存pres_hintsへ接続、helperの属性直読を撤去した。作者overrideと内部領域の親漏出反例を追加し27にした。textareaの値は既存native padding clipで描画されるため、その領域を外側overflow:visibleの通常子boxと同じように親へ公開しない。外側高さを内容高へ変更する修理ではない。
8. **支持と受入を区別。** hostのEvent14/Pointer11/WorkerEvent14とtarget構文はこの源への支持。最終native27条件は構文までで未実行。原geometry17・新child-list・native focus・通信の統合結果と、実GitHub menu/Reddit入力・百度の同条件task/scrollはrootの最新媒体で確認する。旧Fetch Worker複合fixtureの3221226505失敗、debugger不足、実検索の旧feed/Abort未達を、この修理で解消済みにはしない。

rootのbuild前に担当源を凍結する。二段資料の登録後はsource/reportを変更せず、rootの結果に必要な原因修理だけを新しいphaseとして行う。

## 検査伝達の独立再考察

原native outer FAILは欠けた計数文字列によるものだった。正常出力が抑制されることと、JS未実行/例外/27条件失敗を混同しない。既存external_caseはapi-done待機とerrors/js_failures==0を検査し、各checkのFAILをそのままnative失敗へ送る。新しいconsole prefixを既存のAPI計数契約へ合わせる一行修理であり、markerを削除したり値を弱めたりする修理ではない。元の27条件とcount exact27は不変で、訂正後の外側実行結果はまだない。

## 訂正後支持の第二確認

最新serialの外側PASSと `Textarea native scroll API checks 27`、末尾native26/0を原ファイルで照合した。これにより計数伝達の不足は訂正済みの支持結果になった。期待27を下げて合格させたものではない。旧marker不合格を後からPASSへ書き換えない。同じrunの通信1FAIL、別のWorker複合fixture失敗、百度41104ms taskは残る。

rootの実GHmenu/Reddit入力・対応本文・上下scrollの報告は、主担当の実受入証拠として共有するが、私の操作/目視とは呼ばない。機能受入と性能を区別し、87flush/layout40968msの未解消を次の製品修理に残す。私は製品/fixtureを追加編集していない。前のwork BLOCKを読み替えず、観測終了後の別receiptで `ALLOW work`を確認して今回の報告を再登録する。
