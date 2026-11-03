# CSS第二考察：Shadow／非局所関係の境界

第一報を繰り返すのではなく、最も弱い仮定「DOM変異のrootとCSSが属するrootは同じ」を反証した。

- `:host(:has(>.light))` はhostのShadow stylesheetに属するが、witnessのlight DOM変異はdocument treeに属する。document scopeだけを候補検索すると、classを外しても古いhost色が残り得る。
- `relational_node_candidate` はnodeのDOM rootに加え、そのnodeがhostであれば自身のShadow stylesheet索引も参照するよう修理した。外部のShadow subtreeを渡り歩くことはない。
- 内部relative anchorとAPI `:scope` を同一にしてはいけないことを、原WPT `has-argument-with-explicit-scope.html` で照合した。`element.querySelectorAll('.c:has(:is(:scope .d))')` のreferenceはquery scopeであり、各`.c`のrelative anchorではない。明示contextの別fieldを維持した。
- `:has(+ .a + .b)` の終点はimmediate siblingより先へ達する。探索はfollowing forestを見て、内部anchorで正確な最初の`+`を照合する。immediate sibling subtreeだけへ狭める「高速化」はしない。
- 禁止nestedhasをforgiving `:is` 内で見つけてもrule全体を落としてはいけない。原 `parse-has-disallow-nesting-has-inside-has.html` は `:has(:is(:has(*), script))` の `script` branchが正常に働くことを要求する。parserのforgiving recoveryを維持した。
- dependency候補はsubjectの安定id/class/tagを索引し、動的pseudoの現在真偽でpruneしない。消えたwitnessを見て旧matchのdirtyを飛ばすことがない。rootに本当に依存する`:has`はroot subtree再cascadeが必要だが、単に他のsheetが`:has`を含むだけでは全treeにならない。
- 新nativefixtureの独立 `--boundaries` 群はhost再cascade／Shadow非横断／unrelated style寿命／detached childとsiblingを検査する。実行と製品受入結果は主担当の一括guest結果待ち。
- dynamic parser arraysはcapacity4（compound）から成長し、そのarrayをそのままASTに保持する。初期16個＋確定ASTへの二重copyは、仕様には関係なくメモリを増やすので除去した。

静的確認：最終 `css.c` と両modeを含む `cssrelationaltest.c` のuserland ELF freestanding syntaxはexit0。初期host pool/declaration 22件成功は本第二報の新matcher／変異結果として再利用しない。まだ実サイト改善・Chrome同等速度・全selector適合を主張しない。

規格：[Selectors Level 4](https://www.w3.org/TR/selectors-4/)／[CSS Conditional Rules Level 3](https://www.w3.org/TR/css-conditional-3/)。関連原WPT以外の全suiteは実行していない。サイト専用処理、DOM差替え、支持偽装、人工上限は追加していない。
