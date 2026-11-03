# コードの作られ方の監査（2026-11-03）

## 結論

- 持ち主の言う「上に足すだけ」は、10-29 までの履歴に数字で出ている。
- エージェントのコミットの「消した行／足した行」は、10-05..15 で .08、10-16..27 で .42、10-28..29 で .25 だった。
- 10-30..11-03 は .75 に上がった。ただしこの期間には、大きく消して次に戻したコミットが入っている（§3.1.3）。
- 標本の症状の修理は、足した行 23656 に対して消した行 3410（比 .14）だった。修理はほとんど上に足している。
- 原因の多くは書き手の癖より、書き手に渡した文にある。
  - 「悪いコードは消してから建てる」の初出は 10-30 で、724 本の 74% の後だった。
  - それより前の規則は「既存機能を消した全面置換」を禁じていた。
  - 10-31 と 11-01 の建て直しの依頼文は「周りのコードの書きぶりに合わせる」「口を変えるための最小の行だけ」と命じていた。
  - 消してあるかを確かめる機械は無い。
- 試験は今、組みにつながっていない。試験を動かす道具は c51812ff で消え、`Makefile` は `tests/check_*.py` を一つも走らせない。
- 走る形の試験の多くは内部の static 関数をソースから切り出しており、製品の形を固定している。
- カーネルの行の質は、測った範囲では悪くない。関数は短く、製品の重複と死んだ関数は少ない。
- 詰まっているのは、試験、記録、残った古い道、libc の網と媒体の書きぶりである。
- 規則の文も壊れている。`AGENTS.md`:10 は混ざった文で、`AGENTS.md`:100 は空の見出しで、「組みの中の検査」は実在しない。

## 1. 依頼文

````text
あなたは Nocturne OS（作業場所 `D:\Programes\llm vibe slop os`）の監査を一人で担当します。読むだけの監査です。今日は 2026-11-03 です。問いはコードそのものより、コードがどう作られてきたかです。

## このプロジェクトの持ち主の言葉（原文）

"things are allways added on top, nothing is ever refracted when needed or anything, we just keep strut'ing and glueing ourselving into slop. ... our code qualitly is not the best, we need to set guidlines( skills? agentsmd? idk. but claude keeps being "respetfull" of the code and glueing more code on top inseted of just riping out the slop and doing it proerply in lines lines of code) and what not, as well. ... this is a pervasive issue with how implementations are done, how code is written, and lack of consideration as to what purpose the code is serving."

## 材料

- 測りの台本：`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\99c80791-99f7-4ecb-96ea-1be1814365c0\scratchpad\measure.py`（領域ごとの行数、長い行、一行に文が三つ以上の行、履歴の増減）。`python -X utf8 <パス>` で走る。読んで、数え方が正しいかも確かめる。
- コードの大半は AI のエージェントが書いた。指示は `AGENTS.md`、`.claude/rules/*`、`.claude/skills/*`、`docs/` の報告にある。

## 範囲

リポジトリの全部。ただしブラウザーの中（`user/libc/web`、`user/apps/browser*`）は数を数えるだけで、中身の判定はしない。別の監査がカーネル、サービス、libc と移植の構造を見ているので、ここでは層の設計を判定しない。

答える問い：
1. コミットの型。724 コミットのうち、十分な数を `git log --stat` で標本にして分類する：新しい機能、症状の修理、構造の置き換え、戻し。標本の取り方を書く。同じ場所を繰り返し直している連なりと、直後に戻されたか直されたコミットを挙げる。
2. 消さずに足した具体例を 10 個以上。古い道と新しい道が両方残っているもの、特例の分岐が積もったもの、包みの上の包み。コミットの hash と今の `file:line` を付ける。
3. 規則と現実。`AGENTS.md` の「製品のコード」「進め方」「確かめ方」の各項目について、今のコードか履歴で破られている例を挙げる。規則があるのに守られなかった理由を、履歴と依頼の文（`docs/` にある）から言えるか。規則の文の形に問題はないか（長さ、矛盾、測れない語）。
4. 試験。`tests/`（約 8 万 4 千行、550 ファイル）と `tests/check_*.py`（約 90 個）。それぞれが確かめているのは振る舞いか、ソースの文字列か。名前に段の番号が残るもの（`check_svg_paint27.py`、`check_margin_hoist22.py` など）。製品の今の形を固定して消せなくしている試験。消してよい試験の一覧と行数。製品を動かさずに合格する試験の割合。
5. 書きぶり。ブラウザー以外で：長い関数の上位、一行に複数の文、名前、コメントと実際のずれ（コメントが言うことをコードがしていない例）。重複コード（同じ処理の写し。簡単な hash で探してよい）。呼ばれない関数と読まれない値。
6. 記録。`docs/` のファイルの数と、生きているものと死んでいるものの割合。`docs/` の日付入りの報告が直したものと、放置された所見。前の監査の所見のうち、後で直されなかったものの例。
7. 機械で止められるか。上で見つけた型ごとに、`tests/check_boundaries.py` のような組みの中の検査で止められるか、人の判断が要るか。案を書く。実装はしない。
8. エージェントへの指示で変えるべきこと。コードの「周りに合わせる」「差分を小さくする」という既定の傾向に対して、規則のどの文が必要か。規則（`AGENTS.md`）、手順（スキル）、機械の検査（`tests/`）のどれに置くべきかを項目ごとに分ける。

## 守ること

- コードを変えない。ビルド、QEMU、Hyper-V を動かさない。git の状態を変えない。書いてよいのは下の報告のファイル一つと、作業場 `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\99c80791-99f7-4ecb-96ea-1be1814365c0\scratchpad` の中の使い捨ての台本だけ。Python で書くなら `newline='\n'` を付ける。
- 今あるコードと文書に敬意を払わない。コメントや報告の「直した」は主張であって証拠ではない。
- 数字は数え方と一緒に書く。標本から言ったことは標本と書く。正しくできている所は、できていると書く。
- codegraph（MCP の `codegraph_explore`）と rg で探す。`SYS_*` の番号での呼出しと表からの呼出しは索引に出ない。
- 全部を読めないなら、読んだ範囲と読まなかった範囲を正直に書く。

## 報告

全文を `docs/records/audit-structure-quality-20261103.md` に日本語で書く。節は次のとおり。

1. 依頼文（この文を全部そのまま）
2. 読んだ範囲と読まなかった範囲、数え方
3. 上の問い 1〜8 の答え（節を分ける）
4. 消してよいものの一覧（ファイル、行数、理由、確かさ）

最後の返事は 15 行以内：ファイルのパス、重い所見の題、主な数字。全文は返事に書かない。
````

## 2. 読んだ範囲と読まなかった範囲、数え方

### 2.1 動かしていないもの

- ビルド、QEMU、Hyper-V、試験は一つも走らせていない。
- だから「試験が止まる」は全部、ソースを読んだ静的な判定である。実際に走らせた結果ではない。
- コードと git の状態は変えていない。書いたのは、この報告と作業場の台本だけである。

### 2.2 読んだ範囲

| 対象 | 読み方 | 範囲 |
|---|---|---|
| 規則 | 全文 | `AGENTS.md`、`.claude/rules/kernel.md`・`map.md`。`AGENTS.md` の版の履歴 |
| コミット | numstat と題 | 全 724 本を機械で。103 本を手で分類。連なりの本文は一部 |
| カーネルと libc とサービス | 関数の数えは全部、中身は所見の場所だけ | 全部を通読はしていない |
| 試験 | 全ファイルを字句で。中身を読んだのは 20 本ほど | 残りの判定は正規表現の目安 |
| `docs/` | 全件を名前と参照で数えた。中身を読んだのは 20 件ほど | 主に `docs/records/kernel-rebuild-20261031/prompt-common.md`・`00-design.md`、`docs/records/kernel-core-20261101/` の `common-brief.md`・`0-prep.md`・`audit-kernel.md`・`audit-spec-cpu.md`・`audit-spec-hda.md`・`audit-usbstorage-fix*.md`・`day-end-report.md`、`docs/records/kernel-audit-20261030/00-index.md`、`docs/records/workflow-review-20261101/00-report.md`、`docs/records/audit-settings-20261103.md` |

### 2.3 読まなかった範囲

- ブラウザーの中（`user/libc/web`、`user/apps/browser*`）は数えただけで、中身を判定していない。
- 移植の上流の差分の中身は、ほとんど読んでいない。
- 層の設計は判定していない。別の監査の担当である。
- 同じ日の `docs/records/audit-structure-kernel-20261103.md`・`docs/records/audit-structure-services-20261103.md` は読んでいない。
- フックの中身は読んでいない。リポジトリの `.claude/settings.json` は e22f850d で消えており、今のフックはリポジトリの外にある。
- 性能は測っていない。性能の規則の判定もしていない。
- 構造体の欄が読まれているかは数えていない。

### 2.4 `measure.py` の確かめ

台本は走り、次の値を出した。

- 合計 1371 ファイル、423721 行、120 字を超える行 15635、200 字を超える行 1583、一行に `;` が三つ以上の行 14201、生成物 5324 行。
- 履歴（docs と third_party を除く）は +636901 −214711、比 .34。消した方が多いコミットは 37 本。

数え方に次の欠点がある。

- `split("\n")` なので、各ファイルが 1 行多い。
- `GEN` に `ports/mesa/generated` が入らない。約 2 万 9 千行の生成物が「我々のコード」に数えられている（`glsl_parser.cpp` 6527、`program_parse.tab.c` 5467、`glcpp-parse.c` 5050、`glsl_lexer.cpp` 5039、`lex.yy.c` 3691、`glcpp-lex.c` 3225）。
- `tests/html5lib_cases.h` 3467 行も生成物である。
- 上の三つを引くと、我々のコードは約 39 万行である（423721 − 28999 − 3467 − 1371）。
- 「一行に文が三つ以上」は、文字列とコメントの中の `;` も数える。`for` で始まる行は外すが、途中の `for(;;)` は数える。
- 最古のコミットは「消した方が多い」の判定に入らない。害は無い。
- `kernel/hv`・`kernel/rdp` は移動前の道の領域として履歴に残る。

### 2.5 領域ごとの数（`measure.py`、上の欠点を含む）

| 領域 | ファイル | 行 | `;` 三つ以上の行 | 消した／足した（履歴） |
|---|---:|---:|---:|---:|
| `user/libc/web` | 230 | 133624 | 4766 | .33 |
| `tests` | 550 | 84214 | 7070 | .20 |
| `user/libc` | 71 | 33427 | 1186 | .20 |
| `ports/mesa` | 22 | 32773 | 8 | .01 |
| `user/apps` | 84 | 27137 | 424 | .24 |
| `kernel/dev` | 43 | 21389 | 10 | .38 |
| `ports/nvidia` | 34 | 15713 | 98 | .11 |
| `kernel/sys` | 44 | 10536 | 18 | .69 |
| `user/apps`（browser） | 9 | 7495 | 483 | .55 |
| `kernel/vm` | 24 | 7257 | 25 | .89 |
| `common` | 27 | 6782 | 13 | 未集計 |
| `kernel/mm` | 17 | 5866 | 3 | .58 |
| `kernel/arch` | 33 | 5010 | 16 | .54 |
| `kernel/gui` | 5 | 4101 | 0 | .74 |
| `kernel/fs` | 10 | 3528 | 0 | .66 |
| `kernel/net` | 3 | 3159 | 8 | .63 |

変更の多いファイルは、`js.c` 127 本、`webi.h` 107、`syscall.c` 88（+9306 −7922）、`js_bootstrap.js` 83、`Makefile` 82、`abi.h` 79、`browser.c` 76、`nocturne.h` 75、`doc.c` 74、`wm.c` 73（+11961 −8592）、`css.c` 68、`sys.c` 62、`sched.c` 54（+7056 −5762）、`runtests.c` 50 である。

### 2.6 自分の台本

台本は作業場の `audit\` の下にある（`C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\99c80791-99f7-4ecb-96ea-1be1814365c0\scratchpad\audit`）。

- `commits.py`、`classify.py`、`sample.py`、`noauthor.py`、`author.py`：履歴。
- `testgraph2.py`、`stale2.py`、`checks.py`、`dellist.py`：試験。
- `funcs.py`：codegraph の索引 `.codegraph/codegraph.db` から関数の長さ。
- `dead2.py`、`dup.py`、`wrap.py`、`abi.py`、`comments.py`：呼ばれない関数、重複、包み、ABI、コメント。
- `docs.py`、`refs.py`：記録。

`style.py` は正規表現の暴走で止まったので打ち切った。`style2.py` も時間の上限で止まったので、結果を使っていない。書式の混在は rg で数え直した（§3.5.3）。

## 3. 問いの答え

### 3.1 問い 1：コミットの型

#### 3.1.1 標本の取り方

- 全 724 本のうち、差分のあるものは 619 本である。残り 105 本は差分が 0（merge など。merge は 103 本）である。
- 619 本を時刻順に並べ、オフセット 2 から 6 本目ごとに取った。これが 103 本の標本である。
- 103 本を `git log --numstat` の増減と題で手で分類した。差分の全文は読んでいない。
- 比率の誤差は、103 本でおよそ ±9 ポイントである（28% の正規近似、有限母集団の補正なし）。

#### 3.1.2 標本の結果（手の分類）

| 型 | 本数 | 割合 | 足した行 | 消した行 | 消した／足した |
|---|---:|---:|---:|---:|---:|
| 症状の修理 | 29 | 28% | 23656 | 3410 | .14 |
| 新しい機能 | 22 | 21% | 154203 | 68733 | .45 |
| 構造の置き換え | 20 | 19% | 36853 | 38613 | 1.05 |
| 記録・規則 | 19 | 18% | 3154 | 68 | .02 |
| 性能 | 7 | 7% | 1465 | 532 | .36 |
| 試験だけ | 3 | 3% | 未集計 | 未集計 | 未集計 |
| 道具 | 3 | 3% | 未集計 | 未集計 | 未集計 |
| 戻し | 0 | 0% | 0 | 0 | 標本に入らなかった |

読み方は次のとおりである。

- 症状の修理が一番多く、足した行の 14% しか消していない。
- 構造の置き換えは 19% ある。「一度も作り直していない」という読みは、標本とは合わない。
- 新しい機能の比 .45 には、大きな移動と取り込みが入っている。これを「きれいに消した」と読まない。

#### 3.1.3 全数の数え（機械、目安）

規則の分類は題の語で分けた。手の分類との一致は 62/103（60%）なので、目安として読む。

| 型 | 全 724 本 |
|---|---:|
| 新しい機能 | 239 |
| 構造の置き換え | 114 |
| 症状の修理 | 114 |
| 記録・規則 | 106 |
| merge | 105 |
| 試験だけ | 19 |
| 性能 | 11 |
| 道具 | 9 |
| 戻し | 7 |

期間ごとの構造の置き換えの割合（規則の分類、全本数に対して）は次のとおりである。

| 期間 | 本数 | 新機能 | 症状 | 構造 | 構造の割合 |
|---|---:|---:|---:|---:|---:|
| 10-05..15 | 75 | 46 | 19 | 4 | 5% |
| 10-16..27 | 184 | 60 | 52 | 29 | 16% |
| 10-28..29 | 273 | 103 | 14 | 33 | 12% |
| 10-30..11-03 | 192 | 30 | 29 | 48 | 25% |

コードだけの増減を書き手で分けた。third_party、docs、`.jspace`、md、png、json、inc、生成物を除いた。

| 書き手 | 本数 | 足した | 消した | 比 |
|---|---:|---:|---:|---:|
| エージェント（共同作者の行あり） | 637 | 434848 | 181881 | .42 |
| 持ち主（共同作者の行なし） | 84 | 233831 | 42575 | .18 |
| 合計 | 721 | 668679 | 224456 | .34 |

- 共同作者の行で分けたので、行の無いコミットをエージェントが作った可能性は否定できない（未確認）。
- 持ち主の比 .18 には、大きな取り込みが入っている。分解していない。
- 10-30..11-03 の持ち主は 17 本で +932 −7244、比 7.77 である。この期間で一番消したのは持ち主である。

エージェントの比は期間で次のように動いた。

| 期間 | 比 |
|---|---:|
| 10-05..15 | .08 |
| 10-16..27 | .42 |
| 10-28..29（271 本、+116635 −29059） | .25 |
| 10-30..11-03 | .75 |

10-30 以降の .75 は、規則が変わった後の値である。ただし 5e9c0146（−32343）と、それを zip から戻した 372752e6（+47406）の両方を含むので、純粋な作り直しの量ではない。

その他の数は次のとおりである。

- 題が曖昧なコミットは 50 本（「Fixing Claude's Slop」「slop」など）。10-15 より後は題が具体的である。
- 50 ファイル以上を変えるコミットは 51 本、100 以上は 12 本、+5000 行以上は 40 本である。
- 持ち主が自分で消したものは、5b974fc2（−6679、vfs・fat・ramfs・nos ほか）、f1d0b3ee と fe8095c9（−11780 と +11576、247 ファイル）、540df2f3（+12645 −4516）、63b181ff（−6282、mm と sys の中核）、23f80428（+134 −985）、e22f850d（−271、`.claude`）、88a17463、6d503047、2aaf2bb0 である。
- エージェントの大きな削除は、5e9c0146、894423c6、14ba8527、aa139a6a、3a4e8df8、78d65017、fa54dda3、c51812ff、661db797 である。

#### 3.1.4 同じ場所を繰り返し直す連なり

| 場所 | コミット | 読み |
|---|---|---|
| USB と記憶（11-01） | 545a102f +1614/−608、34ab1dbe 735/330、883c546d 471/250、26b39b07 225/87、682f34dd 273/154、fe71d739 230/123、9bea7571 104/44、97e41771 92/55、592078e0 66/28、36cac80c 44/23、cc4f3729 6/0 | 監査と修理を 11 回繰り返した。報告は `docs/records/kernel-core-20261101/audit-usbstorage-fix*.md` にある。day-end-report は `usbhub_work` の末尾 6 行がなお未監査と書く。 |
| IOMMU（10-21〜22） | 0d7daeb5 1788/166、8131ca1a 1180/423、2d0973be 111/62、d0e3000a 1298/368、b84ea440（持ち主）2950/311、d32cffc3 802/354、420f027d 2490/446、a20478c4（持ち主）1411/369 | 8 本で足し続けた。 |
| NVIDIA（10-19〜26） | c0c95af7、95b3df57、24c752ff、b9000ad5、3838d79c、87db38e4（戻し）、837f8789 → 5a774572（残り）、8acc1589（+13205）・b13cfa79 → 54965e2c（監査で致命的）、5f15728c、0a1d812a → fbf47b8d、6e912418、5af24747、c31dddde | 27 本。実機の 1〜11 回目が題に並ぶ。 |
| スケジューラー | MLFQ（ef0d7a25、10-08）→ 朔Ⅱ 7242636b → 継 11237234（10-18）→ 対称（1d6817c0、ba3af5b8、5874c3e4、aa139a6a）→ 持ち主が消す 63b181ff → df893170 締切の級（11-01） | 全体を四回換えた。持ち主が一度消した。 |
| 設定 | a51c4398（10-27）→ 持ち主が否定 → 3ed59b03・5d099ff7 → 11-01 の設計 6356fbef、建て直し 74708779・71fd2593 → `docs/records/audit-settings-20261103.md` がなお誤りと書く | 建て直した後も所見が残る。 |
| 目録 | 304be1a1 → 82f32e43（2 本後） | 足してすぐ置き換えた。 |
| 電源 | b1bb40e1 → 67026b51 | 同じ。 |
| USB の初期 | bb775248 → 26dba191、3e90829e → 7cfbd0e5 | 同じ。 |

連なりの共通点は、監査の指摘を一つずつ直す形である。同じ場所の所見が続いても、設計を書き直す段に戻っていない（§3.3.2）。

#### 3.1.5 直後に戻されたか直されたコミット

| 元 | 後 | 間 | 内容 |
|---|---|---|---|
| 661db797 | d927e533 | 次の本 | 試験の部品の削除を打ち消した。巻き添えで virtio-gpu の修理も戻った（§3.2 の例 1）。 |
| 68a5c117 | d651370a | 次の本 | 起動の早い段の panic を画面に出す変更を戻した。 |
| 5e9c0146 | 372752e6 | 次の本 | カーネルの −32343 を zip から +47406 で戻した。 |
| 24c752ff・b9000ad5・3838d79c | 87db38e4 | 3838d79c の次の本 | NVIDIA の三本を戻した。 |
| 6e074896 | 282785b0 | 次の本 | SDK の部材の一覧を直した。 |
| 282785b0 | 64d72679 | 23 本後 | wasm の module の組みを直した。 |
| e5969375 | 1b0dc75e | 2 本後 | 配置のキャッシュを直した。 |
| 54acda16 | 0efdf8a6 | 2 本後 | selectors の試験を直した。 |
| 5ad2150d（空） | 585dc5a6 | 次の本 | 報告の全文を入れ直した。 |
| 31d01a62 | 42ef4cf0 | 15 本後 | 閲覧文脈の無い文書への挿入を直した。 |
| 0a181f68 | 397ff559 | 49 本後 | Blob URL の UUID を直した。 |
| 83562fd7 | 76f16425 → 29e8b2e9 | 54 本後、さらに 9 本後 | URL の query と fragment の扱いを層ごとに直した。 |
| 99e0686c | 0ef71b96 | 40 本後 | CSS の AST の大きさを直した。 |

- 間の本数は `git rev-list --count 元..後` である（merge を含む）。
- 内容は後のコミットの題の要約である。差分の全文は読んでいないものがある。
- merge を除くコミットのうち、メッセージが前のコミットの hash を指すものは 67 本である。
- 題に「退行」「戻す」「残り」「期待値」の語があるものは 38 本である。
- 試験を製品に後から合わせたコミットの例は、9785fa80、d6f15b9b、8a771c2d、44a30ab2、cd389e7d、930c2d13、24f8a055、98872eda、304472b7、59762722、04a466d3、5a9d4968、db2cd04b である（題と増減からの判定で、差分の全文は全部は読んでいない）。

### 3.2 問い 2：消さずに足した具体例

18 例を挙げる。hash は残りを作ったか残したコミットである。

| # | 何が残っているか | 今の場所 | hash | 確かさ |
|---|---|---|---|---|
| 1 | 製品のカーネルで virgl が決して始まらない。`virtio_gpu.c` の門が `test_flag("gpu-enable")` を要るが、`kernel/src/kernel.h`:68-77 で製品の `test_flag` は false である。ABI `SYS_GPU_*`（`common/abi.h`:60-63,136）、`user/libc/gpu.c`、`ports/virgl/` は残っている。 | `kernel/src/vm/virtio_gpu.c`:579-583、ほかに :240,:265,:672 | 661db797 が直し、d927e533 の全戻しで戻った | 高 |
| 2 | Hyper-V 第 1 世代の網と ATA。第 1 世代の道は消えたが、装置は残った。 | `kernel/src/vm/tulip.c`:1、`vm/ata.c`:1-2、`vm/vm.h`:18、`vm/vm.c`:62,68 | dbb7f1a5（10-31）が道を消した | 高 |
| 3 | `errno` の古いデータの記号。「古い呼ぶ者のため」とコメントにある。 | `user/libc/sys.c`:10-13 | a8b239a3 | 高 |
| 4 | 字体の解析が二つある。stb_truetype（0df974d9、10-05）の後に FreeType（fe16789a、10-12）を足した。`f->outline` の分岐が 6 か所ある。 | `user/libc/font.c`:28,:42,:70,:82,:126,:373,:478,:482,:537、`user/libc/third_party_ttf.c`:8-9 | 0df974d9、fe16789a | 高 |
| 5 | GL の下が三段ある。Zink、llvmpipe、softpipe と `#ifdef GALLIUM_*`。llvmpipe を足したとき softpipe が残った。 | `ports/mesa/nocturne_gl.c`:113-122 | 6e074896 | 中 |
| 6 | 製品のサービスの表に試験のサービスがあり、`session` が依存する。`Makefile` は媒体に入れない testrun を毎回組む。 | `user/apps/services.c`:80-85、`Makefile`:578,597-598 | 16310d67、d927e533 | 高 |
| 7 | 試験の旗のカーネル一式。読み手を動かす運転者は消えた。 | `Makefile`:57-75、`kernel/src/boot.c`:61-141、`sys/settings.c`:125-146、`fs/bootrecord.c`:58-66、`sys/prof.c`:275、`mm/vmm.c`:649 | c51812ff が運転者を消した | 高 |
| 8 | `scripts/qtest.py` と `make run`。規則が禁じるのに残り、README が勧める。 | `scripts/qtest.py`、`Makefile`:621、`AGENTS.md`:80、`README.md`:316 | 9b8fee63 | 高 |
| 9 | シェルの引き抜きが整数の平方根の写しを残した。 | `kernel/src/gui/wm.c`:1035、`user/apps/shell_art.h`:16 | 6c144d29 | 高 |
| 10 | 同じ本体の関数が名を変えて二つある。 | `user/apps/display.c`:58 の `id_of`、`user/apps/settings.c`:198 の `screen_id` | d2ca6cee、74708779 | 高 |
| 11 | `common/store` があるのに、host と kernel に写しがある。 | `host/store/store.c`:754 の `crc32_ieee` と `kernel/src/fs/store.c`:148 の `gpt_crc`、`host/store/store.c`:302 と `kernel/src/fs/cache.c`:115 の `unheld_out`、`common/store/namespace.c`:17 と `host/store/store.c`:71 の `be64`、`common/store/namespace.c`:632 と `common/store/space.c`:73 の `zalloc` | 540df2f3 | 高 |
| 12 | 別の道具の残り。誰も呼ばず、`.jspace` は git に 0 件である。`agents/openai.yaml` は Codex のものである。 | `scripts/jspace_html_control.py`、`.claude/skills/*/agents/openai.yaml` | 734b025d | 高 |
| 13 | SDK に内部の見出しが出る。`cp user/include/*.h` が全部を写す。`media_*_private.h` 7 個、`js_worker_wire.h`、`webnet_wire.h`、`browser_identity.h`。 | `scripts/mksdk.sh`:45 | 540df2f3、f4bdf42b | 高 |
| 14 | 他の OS のソースを参照したというコメントである。前の監査が指摘し、未修理である。 | `kernel/src/net/rtl8125.c`:6 | 9920ba92 | 高（コメントの文として） |
| 15 | `#!` の解釈と UNIX の道具の群（`cat`、`ls`、`grep`、`ps`、`kill`、`cp`、`mv`、`rm`、`touch`、`uname`、`wc`、`head`、`true`、`false`、`echo`、`sh`、`ifconfig`、`ping` など）。 | `user/libc/sys.c`:57-79、`user/apps/` | 2c5b39d1 | 中（消すかは持ち主の決め） |
| 16 | 呼ぶ者のいない `wq_wait`。ヘッダーと規則の文書は説明を続ける。 | `kernel/src/sys/wait.c`:223,229,261、`wait.h`:86,99、`.claude/rules/kernel.md` | aa139a6a、36083380 で呼ぶ者が消えたと読める（`git log -S` の結果で、差分の全文は未確認） | 高 |
| 17 | PCI は segment 0 だけである。規則の上限は 2 ソケットの EPYC である。 | `kernel/src/dev/pci.c`:18,:205 | 未特定 | 高 |
| 18 | shim の残り。`cond_resched() ((void)0)`、`set_current_state(state) ((void)(state))`、`jiffies`、`schedule` を偽の名で置く。 | `ports/nvidia/os_stubs.c`（304 行）、`ports/nvidia/uvm_os.h`（1493 行）、`tests/fuzz/browser/shim/`、`tests/image_host_shim/`、`ports/lexbor/include/memory.h`、`ports/ffmpeg/include/assert.h` | fd006ad8、8acc1589 | 高（文の照合として。層の判定はしない） |

「包みの上の包み」は、ブラウザーの外では少ない。

- 一度しか呼ばない関数は 1038 個である。うち純粋な渡しは 235 個である（`user/libc` 84、`ports/nvidia` 93、`kernel/dev` 32）。
- 渡しの鎖は 11 本で、大半は NVIDIA である。
- `user/libc/math.c`:876-891 の `sqrtf -> sqrt` の形が目立つが、ISO C の面なので害は小さい。

この型の見え方を一言でいうと、問題は包みの層の厚さより、古い道の残りと、持ち主の無い写しである。

### 3.3 問い 3：規則と現実

#### 3.3.1 項目ごとの照合

| 規則 | 破られている例 | 確かさ |
|---|---|---|
| `AGENTS.md`:44 悪いコードは消してから建てる | `font.c` の二つの解析、`tulip.c`・`ata.c`。持ち主が自分で消した 10 本（§3.1.3） | 高 |
| :45 互換は持たない | `user/libc/sys.c`:10-13 | 高 |
| :45 下限は Zen 4 | `kernel/src/arch/cpu.c` の OPTIONAL の表がある。WHPX が機能を隠すからで、規則の文と現実が合っていない。 | 中（どちらを直すかは持ち主の決め） |
| :46-48 設計は全体から、shim 禁止、スパゲッティ禁止 | `ports/nvidia/uvm_os.h`、`os_stubs.c`、`services.c`:84 の依存、`virtio_gpu.c` の `test_flag` | 高 |
| :49 既存の持ち主へ入れる | `isqrt` の写し、`id_of` の写し、`common/store` の写し | 高 |
| :50 Web の標準 | 判定しない（範囲の外） | － |
| :51 サイト専用の処理を入れない | 数えただけである。`user/libc/web`、`browser*`、`webnet`、`webfetch` で youtube 1、duckduckgo 2、x.com 2、cnn 3、ほかの主なサイト名は 0。中身は判定していない。 | 破れの証拠は出ていない |
| :52 VM の処理は実機と分ける | `kernel/src/arch/cpu.c`:143,:183 の `f->hypervisor`。`tests/check_boundaries.py` は検出しない。 | 高 |
| :53 UNIX ではない | `sys.c`:57-79 の `#!`、UNIX の道具の群、`nocturne.h` の `sysinfo`・`proclist`・`poweroff` の名 | 中 |
| :54 言語は C だけではない | 破れの例は探していない | 未確認 |
| :55 GUI が先 | CLI の道具の群。設定の窓の所見は `docs/records/audit-settings-20261103.md` | 中 |
| :56 ディスクは一台 | 破れは無い。`"/bin/`、`/data`、`sh.c` の PATH は libc・apps・kernel で 0 件である。 | 守られている |
| :57 生成物は生成元から | `ports/mesa/generated` は `map.md` の生成物の表に無い。生成元があるかは未確認である。 | 未確認 |
| :58 上限と中断 | `pci.c` の segment 0。`#define` の固定の数はカーネルで 66、ブラウザー外のユーザー側で 92（仕様の定数を含む目安）。 | 中 |
| :59 性能 | 測っていないので判定しない | － |
| :25 途中で見つけた不具合は目的の後 | 守られている。その結果、`docs/records/browser/LEDGER.md` に K 番号が 140 個たまった。後回しの文そのものが問題である（§3.3.2）。 | 高 |
| :29 書く前に：誰のものか | シェルの引き抜きが平方根の写しを残した。 | 中 |
| :30 直す前に：原因はどの層か | 判定には差分の全文が要るので、読んでいない。 | 未確認 |
| :31 区切りごとに：物差しは動いたか | USB の 11 回。指摘が尽きるまで同じ方向へ進んだ。 | 高 |
| :32 言う前に：何と何を比べたか | コミットの題の「直した」は比べた二つを書かない。規則の後の本数は数えていない。 | 中 |
| :69 回帰を許さない | 661db797 → d927e533 で virgl の修理が失われた。 | 高 |
| :70 証拠の範囲を分ける | USB の「直した」の後に監査が致命的な所見を出し続けた。 | 中 |
| :71 QEMU は窓付き | `scripts/qtest.py`、`Makefile`:621 が残る。 | 高 |
| :73 同じ試験を繰り返さない | 試験の方を一度きりに歪めた型がある（`assert not (out/'runtime-started.json').exists(), '同一実行禁止'`、4 本）。 | 高。ただしこの試験は 10-09 の作で、今の規則より前の版の依頼に従ったものと推定する（推定） |
| :80 組みの中の検査 | `Makefile` と `scripts/build.py` に `check_` は 0 件である。組みの中の検査は実在しない。 | 高 |

正しくできている例は次のとおりである。

- `/bin`・`/data`・PATH は製品に無い。
- サイト名の語は少ない（数えただけ）。
- `map.md` の壊れたパスは 0 件である。
- `SYS_*` 183 個は全部、カーネル側とユーザー側で使われている。

#### 3.3.2 守られなかった理由（履歴と依頼の文から言えること）

言える。理由は六つある。

**1. 長い間、規則は逆のことを言っていた。**

- 最初の `AGENTS.md`（b2274962、10-10、25.6 KB、53 本目）は「既存機能を消した全面置換や、二重DOM管理で接続を壊さない」と書く。
- 同じ版の節の題は「既存変更の保全と編集方法」で、「コードの追加は、対応する責務を持つ既存の配置へ統合する」と続く。
- この文が消えたのは 217c5ff7（10-26、212 本目）である。
- 「悪いコードは rm -rf で消してから建てる」の初出は 4b597384（10-30、535 本目）である。724 本の 74% の後で、その後は 189 本である。
- 持ち主の 10-30 の言葉は `docs/records/kernel-audit-20261030/00-index.md` にある。要旨は「悪いコードが極端な量で足された」「slop に patch を当てる案を出すな」「あなたが決めることがこの slop の原因だ」である（英語の原文を訳した）。
- つまり、10-30 までの「足すだけ」は規則どおりの働きだった。

**2. 依頼文が「周りに合わせる」「最小」と命じている。**

- `docs/records/kernel-rebuild-20261031/prompt-common.md`:25-26 は「口を変えるための最小の行だけ」「今あるコードの書き方（名前、注記の濃さ）に合わせる」と書く。
- `docs/records/kernel-core-20261101/common-brief.md` は「周りのコードの書きぶりに合わせる」と書く。
- `docs/records/kernel-core-20261101/0-prep.md` は「動きを変えない。名前を変えない」と書く。
- この語群は `docs/` に 36 回、18 ファイルにある。
- 同じ依頼文には「持ち主の単位で消してから建てる。古い道を残して新しい道を足さない」もある（19 ファイル）。
- 書き手は矛盾する二つの指示を受け取っている。安全な方は「足す」である。

**3. 後回しの文が掃除を先送りする。**

`AGENTS.md`:25「途中で見つけた別の不具合は報告に一行書き、目的が済んでから」（fbc36b5a、10-27）は、触っているファイルの中の悪いコードにも当てはまる読みができる。結果は K 番号 140 個である。

**4. 止める機械が無い。**

- `tests/check_boundaries.py`（b8f0448a が最後に変えた）は組みにつながっていない。
- `.claude/settings.json` は e22f850d で消えた。規則が言うフックは言語についてのものだけである（`AGENTS.md`:96。中身は未確認）。
- `Makefile`:37 の `-Wno-unused-function` が死んだ static 関数を隠す。

**5. 試験が消す費用を上げた。**

切り出し型の試験は、内部の static 関数の署名とソースの形を固定する（§3.4）。関数を消すと試験も壊れるので、作り直しの費用が上がる。試験を後から合わせるコミットが 13 本見つかる（§3.1.5）。

**6. 振れ幅が大きく、両方向に失敗している。**

- 消しすぎの例は 5e9c0146 → 372752e6 と、661db797 → d927e533 である。
- 足しすぎの例は `docs/records/workflow-review-20261101/00-report.md` が書く 10-28 の自走である（14.5 時間、サブエージェント 58 本、作業木 58 本、237 コミット、+124815 行。コミットと行は Claude の数えで、ここでは照らしていない）。
- 同じ報告は「言葉が言い換えで規則になる」「極端から極端へ振れる」と書く。案 8 個は「決めるのはあなた」のまま適用されていない。
- 規則の変更は、`AGENTS.md` で 43 本、`.claude` で 131 本ある。文を足す方向で何度も直してきた。

#### 3.3.3 規則の文の形

**長さ。**

- `AGENTS.md` は 100 行、15857 バイトである。
- 規則の文書の合計は約 96 KB である（`AGENTS.md` 15857、`map.md` 17911、`kernel.md` 10814、`browser.md` 4200、`nvidia.md` 1356、`nocturne-project` スキル 10919、`browser-hikaku` スキル 14244、`logs.md` 12559、`qemu-visual.md` 8213）。
- `kernel.md` の錠の順は一項目が非常に長い。

**壊れた文。**

- `AGENTS.md`:10 は「守る線：GPLv2 only のコードは足さない。Linux や他の OS のソースは参照しない。は GUI が先の独自の OS。…」である。e242325a の統合で混ざり、冒頭の段落と重複する。
- `AGENTS.md`:100 は空の見出し `## ` である。
- `AGENTS.md`:53 は英語と日本語が混ざる。意味は明確で、害は小さい。

**矛盾。**

| 文 A | 文 B | 起きること |
|---|---|---|
| 「消してから建てる、ビルドが通らない間があってよい」（:44） | 「回帰を許さない」（:69） | どの時点で回帰と数えるかが書いていない。 |
| 「既存の持ち主へ入れる」（:49） | 「消してから建てる」（:44） | 持ち主が悪いときにどちらを取るかが書いていない。 |
| 「途中で見つけた…報告に一行」（:25） | 「設計は全体から」（:46） | 触るファイルの中の悪いコードをどうするかが書いていない。 |
| 「下限 Zen 4、前提にしてよい」（:45） | `cpu.c` の OPTIONAL | 試験の道具が隠す機能の扱いが書いていない。 |
| 「全文を `docs/` へ」（:38） | グローバルの「文書には今の形だけ」、`AGENTS.md`:18「文書は古いことがある」 | 記録と仕様が同じ場所に混ざる。 |
| 「消す」（:44） | グローバルの「削除は頼んだ範囲」 | 持ち主の単位の削除に誰の許可が要るかが書いていない。 |
| 「組みの中の検査（`tests/check_*.py`）」（:80） | `Makefile` | 実在しないものを指す。 |

揺れは次のとおりである。作業木は 961c3e7c で禁止され、今は可である。媒体は 2ab74839 で一つ、cdd168f7 で二つである。

**測れない語。** 「長く保てる」「要る所」「要らない」「最良」「汚れ」は、書き手が自分で満たしたと判定できる。これらには照らす基準が無い。

**形の問題の本体。** 文を足して直す形が続いた。機械に移せる文（§3.7）を移せば、文は減らせる。

### 3.4 問い 4：試験

#### 3.4.1 内訳

`tests/` の直下は 538 ファイルである。ほかに `fixtures` 23、`fuzz` 18、`media-fixtures` 31 などがある。`check_*.py` は 92 本、10710 行である。入口の数え方は `testgraph2.py` で、ファイル名が他のファイルの字句に出るかで分けた。

| 入口 | ファイル | 行 |
|---|---:|---:|
| `.py` の入口 | 123 | 12686 |
| `.py` から組まれるか読まれる | 144 | 28453 |
| ゲストの表 `tests/runtests.c` から直接 | 99 | 21643 |
| 他の試験から include | 77 | 9450 |
| 誰も指さない | 70 | 8104 |
| node の契約試験 | 18 | 4768 |
| `Makefile` | 6 | 1209 |
| 合計 | 537 | 86313 |

`testgraph2.py` の行は `split` の数えで各ファイル 1 行多い。合計の本数は 538 と出るが、内訳の和は 537 である。残り 1 件は調べていない。

ゲストの表 `tests/runtests.c` は約 237 件、124 群である。群の名に `slop8`、`compatrepair`、`transport8` がある。

#### 3.4.2 試験は今、走らない

- c51812ff が `scripts/test.py`（785 行）、`wpt.py`（715）、`platform_qa.py`（689）と `Makefile` の test の目標を消した。
- d927e533 の本文は「消すのは scripts/test.py、wpt.py、platform_qa.py だけ」と書く。持ち主の指示と推定する（本文に指示とは書いていない）。
- 今、OS の中の試験を媒体へ入れる道具は無い。`scripts/layout.py` と `scripts/build.py` に test の語は 0 件である。
- `user/apps/services.c`:79 のコメントは「試験の媒体にだけあるプログラム」と書くが、その媒体を作る道は無い。
- `Makefile` と `scripts/build.py` に `check_` は 0 件である。
- `README.md`:314 と `AGENTS.md`:80 の「ビルドの中の検査」は実在しない。
- `tests/runtests.c`:1,152,202、`tests/testrun.c`:1 は、消えた `scripts/test.py` を今も指す。ほかに `audiotest.c`:2、`fstest.c`:5、`nettransporttest.c`:2、`tcptest.c`:1、`tcpcapacitytest.c`:4、`wptrunner.c`:1、`tests/wpt/*`:1、`check_test262.py`:9、`ports/virgl/README.md`:50 がある（計 13 ファイル）。

#### 3.4.3 確かめているのは振る舞いか、ソースの文字列か

`.py` の入口 123 本を字句の特徴で分けた。正規表現の目安で、中身を読んだのは 20 本ほどである。

| 型 | 本数 | 例 | 読み |
|---|---:|---|---|
| 切り出し。ソースの文字列から関数を抜いて生成した C に貼る | 90（73%） | `check_margin_hoist22.py`（`fn(source, signature)` で抜き、`.replace` で計測を差し込み、`(void)x;unexpected++` の stub を貼る） | 振る舞いを見るが、本物の組み合わせではない。署名を固定する。 |
| コンパイルしない静的な検査 | 5 | `check_boundaries`、`check_settings`、`check_testdriver_vendor`、`layout_firmware_test`、`test_unifont` | 目的どおりで良い。 |
| 残り。丸ごと組むか道具を見る | 28 | `check_hda_codec.py`（実物の `.c` を丸ごと組む）、`check_cache_storage_native.py`、`check_tsc_rate.py` | 良い型である。全部は確かめていない。 |

ソースの文字列そのものを assert する形は少ない。

- `assert '…' in 変数` か順序の `.index` を持つのは 8 本、16 件である（`check_native_active18.py` 5、`worker_heap21_new.py` 2、`check_scroll_area20.py` 2、`check_cascade_winner23.py` 2、`check_absolute_height18.py` 2、ほか）。
- 例は `check_native_active18.py` の `assert 'if(result>0)native_active_event(event);' in browser` である。
- つまり固定の主な道は文字列の assert でなく、切り出しである。

#### 3.4.4 名前に段の番号が残るもの

作業の段の番号か `_new` が名に残る入口（`.py`・`.cjs`）は 37 本である。同じ形の名の連れは、さらに 38 本ほどある（ファイル名の正規表現の目安）。

- 番号（17 本）：`check_absolute_height18`、`check_cascade_winner23`、`check_flex_reverse25`、`check_inline_link19`、`check_layout_extent18`、`check_layout_text26`、`check_margin_hoist22`、`check_native_active18`、`check_native_hover_overlay17`、`check_range_extract27_native`、`check_scroll_area20`、`check_stretch_height21`、`check_stylesheets24`、`check_svg_paint27`、`check_svg_paint27_codec`、`check_svg_paint27_percent`、`check_svg_paint27_viewport`。
- `_new` か番号（20 本）：`auxiliary_link29_new`、`css_rule_list_proxy_new.cjs`、`cssom_native_new`、`fetch_keepalive_bridge_new.cjs`、`fetch_keepalive_lifetime_new`、`http_hostname19_new`、`http_method18_new`、`http_origin20_new`、`http_owned_span21_new`、`image_upgrade20_new`、`paint_stacking_new`、`webnet_url22_new`、`worker_broker26_new`、`worker_broker26_ownership`、`worker_clone25_new`、`worker_heap21_new`、`worker_lease27_new`、`worker_messaging23_new`、`worker_origin17_new`、`worker_timer17_new.cjs`。
- `check_margin_hoist22` と `worker_clone25_new` は 953ee059（10-09）から変わっていない。`check_ephemeron_native` は c3505d83 から変わっていない。
- 番号は作業の段の番号で、製品の意味を持たない。

#### 3.4.5 製品の今の形を固定している試験、走らない試験

全部ソースを読んだ静的な判定である。走らせていない。

| 型 | 本数 | 証拠の形 | 行（`.py` だけ） |
|---|---:|---|---:|
| A 切り出しの署名が今のソースに無く、`.index` で止まる | 16 | `stale2.py` の結果から偽陽性を手で除いた。`check_js_dynamic_queues` は退き道があるので入れない | 1088 |
| B `open('x')` で存在しない名を読むか、その形に依る | 11 | 字句 | 342 |
| C 残っていた build の中身を入力にする（`build/goal-*`、`build/nocturne-audit` は今は無い） | 19 | 字句 | 873 |
| D `同一実行禁止`。一度しか走らない作り | 4 | 字句 | 151 |
| E 説明文が「一度だけ」「新しい分だけ」と言う | 29 | 説明文 | 1113 |
| 和 | 53 | 重なりを除いた | 2537 |

- 連れのファイル（和の 53 本だけが指し、他の試験、`Makefile`、ゲストの表が指さないもの）は 59 本、4152 行である。
- 別に `build/goal-2026*` を固定する `.py` は 62 本ある。うち `mkdir` も `makedirs` も無いのは 30 本である。
- A・B・C の和は 39 本（入口の 32%）である。`mkdir` の無い 30 本を足すと 53 本（43%）である。これらは今の形では走らないと読む。この 53 本は上の表の和の 53 本とは別の集合で、数が同じなのは偶然である。

#### 3.4.6 製品を動かさずに合格する試験の割合

- 今、動かせる試験は全部、OS を起動しない。入口の数で 141 本（`.py` 123 と node 18）の全部である。
- 全試験の行に対して、この形の行は 53% である（`.py` の入口、`.py` から組まれるもの、node の 45907 行を 86313 行で割った）。
- OS を起動して試すゲストの表 237 件は、今は走らせる道が無い。
- 合格している本数は、走らせていないので不明である。

正しくできている点も書く。良い型はある。`check_hda_codec.py` は実物の `.c` を組んで codec の表で試す。`check_boundaries.py` は目的が明確である。残すべき型はこれらである。

### 3.5 問い 5：書きぶり（ブラウザー以外）

#### 3.5.1 長い関数

数え方は codegraph の索引の `start_line`・`end_line` である。関数は 14337 個ある。100 行を超えるものは 127 個、200 行を超えるものは 23 個、500 行を超えるものは 2 個である。

| 行 | 関数 | 場所 |
|---:|---|---|
| 618 | `EmitInstr` | `ports/wabt/interp-x64.cc`:556 |
| 611 | `main` | `tests/anchor_position_native_cases.c`:227 |
| 417 | `main` | `tests/fetch_metadata_host_cases.c`:187 |
| 380 | `main` | `tests/line_breaking_native_cases.c`:277 |
| 373 | `main` | `tests/pcitest.c`:1168 |
| 371 | `api` | `tests/fixtures/js/server.py`:106 |
| 362 | `syscall_dispatch` | `kernel/src/sys/syscall.c`:1034 |
| 294 | `request_inner` | `user/libc/http.c`:494 |
| 263 | `win_input` | `user/libc/ui.c`:2402 |
| 245 | `paint` | `user/libc/ui.c`:1562 |
| 242 | `run_http` | `user/apps/webfetch.c`:1624 |
| 237 | `inflate_feed` | `user/libc/deflate.c`:716 |
| 195 | `nmedia_draw` | `user/libc/media_draw.c`:56 |
| 189 | `tcp_segment` | `kernel/src/net/net.c`:1432 |
| 189 | `checks` | `user/apps/syscallfuzz.c`:311 |
| 186 | `run_one` | `user/apps/webfetch.c`:2205 |
| 175 | `art_icon` | `user/apps/shell_art.h`:208 |

関数の長さの p95 は、`kernel/dev` 42、`kernel/sys` 31、`kernel/arch` 35、`kernel/mm` 40、`user/libc` 37、`user/apps` 52 である。カーネルの関数は短い。これは正しくできている所である。`syscall_dispatch` は表の形の分岐なので、長さの害は小さい。

#### 3.5.2 一行に複数の文

数え方は `measure.py` である（§2.4 の欠点を含む）。

- カーネルは少ない（`kernel/dev` 10、`kernel/sys` 18、`kernel/mm` 3、`kernel/gui` 0、`kernel/fs` 0）。
- 多いのは `tests` 7070、`user/libc` 1186 である。
- ファイルでは `user/libc/http2.c` 114、`user/libc/tls.c` 97、`user/libc/media_mse.c` 94、`user/libc/webcookie.c` 83、`user/libc/webstorage_shared.h` 80、`user/apps/webfetch.c` 117 である。

#### 3.5.3 書式の混在

リポジトリの直下に `.clang-format` も `.editorconfig` も無い。`if(`・`for(`・`while(`・`switch(` と、空白ありの形を rg で数えた。

| 場所 | 空白なし | 空白あり |
|---|---:|---:|
| `kernel/src` | 191 | 8606 |
| `common` | 47 | 717 |
| `user/libc/http2.c` | 340 | 0 |
| `user/libc/ui.c` | 0 | 804 |
| `user/libc/tls.c` | 102 | 415 |
| `user/libc/http.c` | 110 | 135 |
| `user/apps/webfetch.c` | 303 | 391 |
| `user/apps/settings.c` | 0 | 260 |
| `host/store/store.c` | 0 | 420 |

網のファイルでは二つの書き手が同じファイルに混ざっている。カーネルはほぼ揃っている。`kernel/src/vm/virtio_gpu.c` は他と書式が違う。

#### 3.5.4 名前

- 試験の名に作業の語が残る：`slop8`、`transport8`、`net8`、`compatrepair`、`goal-20261009`、段の番号。
- `user/include/nocturne.h` の関数 222 個（粗い正規表現の目安）のうち、`n_` の接頭辞が 146 個、無いものが 76 個ある。無いものの例は `thread_create`、`tcp_connect`、`win_open`、`sysinfo`、`proclist`、`poweroff` である。接頭辞の規則が揃っていない。

#### 3.5.5 コメントと実際のずれ

| 場所 | コメントの言うこと | 実際 |
|---|---|---|
| `user/libc/tls.c`:7 | 時刻は CMOS の時計から来る | BearSSL は `time()` を読む（`Makefile`:134 の `-DBR_USE_UNIX_TIME=1`）。`user/libc/sys.c`:239 の `time` は `n_clock_wall()` を読む。 |
| `user/include/nocturne.h`:396-397 | 走るスレッドは 128 まで | `thread.c`・`process.c`・`syscall.c` に 128 の上限は見つからない（rg で探した範囲）。 |
| `.claude/rules/kernel.md` | 起動の cmdline に `prof` | `kernel/src/kernel.h`:68-70・`sys/prof.c`:275。製品のカーネルは読まない。 |
| `user/apps/services.c`:79 | 試験の媒体にだけあるプログラム | その媒体を作る道が無い。 |
| `kernel/src/vm/vm.h`:18 | 第 1 世代の古い装置 | 第 1 世代の道は dbb7f1a5 で消えた。 |
| `tests/` の 13 ファイル | `scripts/test.py` で走る | 消えた。 |
| `README.md`:314、`AGENTS.md`:80 | ビルドの中の検査 | 無い。 |
| `kernel/src/sys/wait.h`:86,99 | `wq_wait` の使い方 | 呼ぶ者が無い。 |

機械の数えも付ける（`comments.py`）。コメントの中の識別子で、コードに無いものは 80 件（重複を除いて 67 種類）である。大半は上流の名なので、製品のコメントのずれは多くない。

#### 3.5.6 重複

数え方は `dup.py` である。有意な 8 行の窓の hash が別の場所と一致する行を数えた。

- 製品は 735 行である（`user/libc` 431、kernel 148、`user/apps` 65、common 54、host 37）。全体の規模に比べて少ない。
- 試験は 1723 行である（`css_cascade_hosttest`、`typed_om_hosttest`、`compression_hosttest`、`webcrypto_hosttest`、`editing_hosttest` の前置き、`cssanimationregressiontest` と `cssanimationtest`、`check_margin_hoist22` と `check_stretch_height21` など）。
- 同じ本体の関数は 26 群で、余りは 330 行である。製品の中は 10 群で、余りは 66 行である。

8 行より短い写しは数えていない。§3.2 の例 9〜11 はこの型である。

#### 3.5.7 呼ばれない関数と読まれない値

数え方は `dead2.py` である。定義の名が、製品と third_party の字句に定義の一度しか出ないものを取った。試験だけが呼ぶものも入る。

| 領域 | 個数 | 行 | 読み |
|---|---:|---:|---|
| `ports/nvidia` | 93 | 279 | 上流の外部参照で不確か |
| `user/libc` | 91 | 463 | うち試験だけが呼ぶもの 35。ISO C の面（`mbrtoc16` など）と SDK の口を含む |
| kernel | 18 | 75 | 確か。§4.1 |
| `user/apps` | 5 | 33 | 全部誤り。4 件は正規表現の誤り（`(d)`・`(o)`）で、`early_init`（`user/apps/cppdemo.cpp`:32）は constructor である |
| common | 2 | 2 | 確か。§4.1 |
| `ports/mesa` | 2 | 2 | 不確か |
| 合計 | 211 | 854 | 上の誤りを含む。製品の規模に比べて少ない |

- enum と定数で製品に一度しか出ないものは 256 個である。ABI の定数は持っているだけのものもあるので、目安である。
- `Makefile`:37 の `-Wno-unused-function` を外せば、static 関数はコンパイラーが教える。

### 3.6 問い 6：記録

#### 3.6.1 数

数え方は `docs.py`・`refs.py` である。今日の 2 件の未追跡の報告は入れていない。

- `docs/` の追跡ファイルは 376 件、6001592 バイトである（md 355、json 11、png 10）。
- 外から指されるのは 17 件（5%）である。png 10、`nocturne-native-abi.md`（75 回）、`docs/records/browser/LEDGER.md`（3）、`docs/records/nvidia/LEDGER.md`（6）、`cpp-support`（4）、`browser-missing`（4）、`browser-wasm-delivery`（2）、`review-87dd1e2f-map.json`（1）である。「外」は `docs/` の外のコード、規則、スキル、README である。
- 中からも外からも指されないものは 208 件（55%）である。
- 残り 151 件（40%）は `docs/` の中からだけ指される。

| 種類 | 件 | バイト |
|---|---:|---:|
| 監査 | 53 | 1937175 |
| 日付入り | 108 | 974672 |
| 二人の書き手と回の報告 | 140 | 617206 |
| カーネルの報告 | 27 | 617113 |
| 建て直しの依頼と設計 | 24 | 464413 |
| 台帳 | 2 | 330121 |
| 生きた仕様 | 1 | 157882 |
| 地図の json | 11 | 未集計 |
| 画像 | 10 | 未集計 |

- 日付の山は、10-13 が 85 件、11-01 が 60 件、10-12 が 33 件、10-31 が 30 件、10-21 が 27 件である。
- `docs/` の中のパスの参照は 2311 件で、今は無いものは 163 件（7%）である。
- 5 回以上参照を持つ 112 件のうち、半分以上が古いのは 5 件である（`system-boundaries-report-20261102`、`settings-ownership-20261027-system`、`kernel-rebuild` の legacy-boot ほか）。
- 規則の文書のパスは壊れていない。

持ち主は 10-30 に、記録が好きで、記録を書くなという意味ではないと書いている（`docs/records/kernel-audit-20261030/00-index.md`、英語の原文を訳した）。記録を残すこと自体が悪いのではない。問題は、記録と仕様が同じ場所に混ざり、記録の所見が閉じないことである。

#### 3.6.2 報告の所見のうち、後で直されたもの

今のコードか、修理のコミットで照らした。

- B2（38b08a8a）。
- B26（`MAX_STREAMS` は 78d65017 で消えた）。
- entropy（1ca9c21e）。
- log の serial（4ad82190）。
- block cache（905def04）。
- store の cursor（f62a3b70）。
- shared（06c9df30）。
- 時間の ABI（8837d5bd）。
- NVMe（ba50bfdf）。
- AHCI（cdb91913）。

#### 3.6.3 後で直されなかった所見

今のコードで照らした。

| 所見 | 報告 | 今の場所 | 証拠 |
|---|---|---|---|
| F1：MCE の扱いで UC・PCC の bank を飛ばす | `docs/records/kernel-core-20261101/audit-spec-cpu.md`（dd28a519） | `kernel/src/arch/cpu.c`:338,:385 | 同じ形が残る。dd28a519 の後、`cpu.c`・`tsc_rate.h` を変えたコミットは 0 本である（`git rev-list --count dd28a519..HEAD`）。 |
| F4：AMD の TSC の判定が family だけである | 同じ | `kernel/src/arch/tsc_rate.h`:43 | `family == 0x19 \|\| family == 0x1A` のまま |
| F5：`LockTscToCurrentP0` を見ない | 同じ | `kernel/src/arch/tsc_rate.h`:28-29 | コメントは 0 と言うが、`kernel/src` にこの名はコメントの一度しか無い |
| D1：`link_down` の戻りを捨てて DMA の頁を解放する | `docs/records/kernel-core-20261101/audit-spec-hda.md`（2aaea0b3） | `kernel/src/dev/hda.c`:906-909 | 同じ形が残る |
| PCI の segment 0 | `docs/records/kernel-core-20261101/audit-kernel.md` 5.3 | `kernel/src/dev/pci.c`:18,:205 | 残る |
| `f->hypervisor` の分岐 | 同じ 5.1、`audit-spec-cpu.md` F14 | `kernel/src/arch/cpu.c`:143,:183 | 残る |
| 他の OS のソースの参照 | `audit-kernel.md` 5.3 | `kernel/src/net/rtl8125.c`:6 | 残る |
| USB：`usbhub_work` の末尾 6 行 | `docs/records/kernel-core-20261101/day-end-report.md` | 未確認（報告が未監査と書く） | － |
| HD Audio の 41 指摘、threadtest など | 同じ | 全部は照らしていない | － |

古い PIC の残りも見た。`kernel/src/arch/irq.c`:587 の `pic_disable` は MADT の PCAT_COMPAT に応じて 8259 を伏せるものなので、残りではなく今の働きである。

#### 3.6.4 型としての所見

- 監査は 53 件、約 1.9 MB ある。所見に一つの番号の系統が無く、閉じた印も無い。
- どの所見が開いているかを知るには、今のコードと照らすしかない。私も今回それをした。
- `docs/records/browser/LEDGER.md` の K 番号 140 個は番号を持つ。これは型として良いが、閉じるかどうかは確かめていない。

### 3.7 問い 7：機械で止められるか

案である。実装はしない。既にある違反は一覧で許し、増やさない形（ラチェット）を前提にする。

| 型 | 止められるか | 検査の形 | 誤検出 |
|---|---|---|---|
| 検査が組みにつながらない | はい | `Makefile` に `check` の目標を作り、`qemu-image` の前提にする。`check_*.py` の全部が表にあるかを照らす | なし。切り出し試験を整理してから（今つなぐと止まる） |
| 切り出し試験 | はい | `tests/` の `.py` で、`.index('static `・`fn(source` の形を拒む | 低 |
| 段の番号と `_new` の名 | はい | ファイル名の正規表現（`\d{2}(_\|\.\|$)`、`_new`） | 低（`utf8`・`http2` のような名は除く一覧が要る） |
| 一度きりの試験 | はい | `同一実行禁止`、`open('x')`、`build/goal-`、`nocturne-audit` の語を拒む | 低 |
| 呼ばれない関数 | はい | `-Wno-unused-function` を外す。非 static は `dead2.py` の形か `--print-gc-sections` | 中（表、asm、公開の口） |
| コメントと試験の中の消えたパス | はい | パスに見える字句がリポジトリにあるかを照らす（`comments.py` の形）。上流の名は許可の一覧 | 中 |
| 製品の中の試験の門 | はい | `test_flag(` の場所を一覧に限る | 低 |
| サービスの表の試験の項目 | はい | `services.c` の `declared[]` の名を、`session` の `.needs` と照らす | 低 |
| 共通コードの hypervisor の判定 | はい | `check_boundaries.py` に `hypervisor` の語を足す。`kernel/src/arch/cpu.c` を直してから | 低 |
| 他の OS のソースの参照 | 一部 | コメントの `FreeBSD`、`Linux driver` の語 | 中 |
| 写し | はい | `dup.py` の形、8 有意行の窓、今の値を上限に | 中 |
| 短い写し（`isqrt` など） | 一部 | 同じ本体の関数の hash | 中 |
| 長い関数 | はい | codegraph か字句で、今の値を上限に | 低 |
| 一行に複数の文、書式の混在 | はい | リポジトリに `.clang-format` を置く。上流の移植は除く | 低 |
| SDK に出る内部の見出し | はい | `mksdk.sh` を明示の一覧にする。`*_private.h`・`*_wire.h` を拒む | 低 |
| 規則の文の壊れ | はい | 空の見出しを拒む。`AGENTS.md` が指すパスが実在するかを照らす | 低 |
| `docs/` の名前と場所 | はい | 新しい記録は保管の場所へ。`-first-`・`-second-` の名を拒む | 低 |
| 監査の所見の放置 | 一部 | 所見に ID と `file:line` を持たせ、一覧を出す。止めない | － |
| 同じファイルの連続修理 | 一部 | 同じファイルを N 本続けて直すときに警告する。止めるかは人 | 中 |
| 足しただけのコミット | 一部 | 足した行が多く消した行が無い差分に警告する。良い新機能でも鳴る | 高 |
| 古い道と新しい道の二重 | 人の判断 | 字句では同じ責務を判定できない。作り直しの報告で古い名の rg を 0 件と示す形までは機械でできる | － |
| 特例の分岐の妥当性 | 人の判断 | － | － |
| この関数の目的 | 人の判断 | － | － |
| 層の持ち主 | 人の判断（字句で書ける部分は `check_boundaries.py`） | － | － |
| 削除の範囲 | 人の判断 | － | － |

順序の案は次のとおりである。

1. 走らない試験を整理する（§4.2）。
2. `check_boundaries.py` と `check_settings.py` を組みの前提にする。
3. 上の「はい」の行をラチェットで足す。

### 3.8 問い 8：エージェントへの指示で変えるべきこと

案である。グローバルの指示「規則とメモリーは、案を見せて『はい』の後に書く」に従い、適用していない。

#### 3.8.1 規則（`AGENTS.md`）に置くもの

毎回必要で、判断を変える文だけを置く。

| # | 文の案 | 置く場所 | 直す型 |
|---|---|---|---|
| 1 | 周りが悪ければ周りに合わせない。新しいコードは、触るファイルの今の書きぶりでなく、この文書の基準で書く。 | 製品のコードの先頭 | 周りに合わせる傾向 |
| 2 | 差分の小ささは良さの尺度ではない。正しい形が大きな差分なら、大きな差分を出す。 | 同じ | 差分を小さくする傾向 |
| 3 | 新しい持ち主を入れる仕事は、古い持ち主を消して終わる。残すなら、理由と消す時をコミットの本文に書く。 | :44 に足す | 二重の道 |
| 4 | 足す前に問う。この関数・値・分岐の目的は何か。その目的は既に満たされていないか。目的を言えないコードは足さない。 | :29 を置き換える | 目的の欠如 |
| 5 | 同じ場所で監査の指摘が二回続いたら、指摘ごとに直さず、その場所の設計を書き直して見せる。 | 進め方 | USB の 11 回 |
| 6 | 触るファイルの中の悪いコードは、同じ仕事で直す。ファイルの外の不具合は、報告に一行書く。 | :25 を置き換える | 先送り |
| 7 | 既存の持ち主が悪ければ、入れずに持ち主を直す。 | :49 に足す | :44 と :49 の矛盾 |
| 8 | 試験は公開の口で書く。内部の static 関数の名と本体を固定しない。 | 確かめ方 | 切り出し |
| 9 | 製品のコードの分岐を試験の旗で変えない。 | :46-48 に足す | `virtio_gpu.c` |
| 10 | 建て直しの途中は回帰に数えない。建て終わりで数える。 | :69 に足す | :44 と :69 の矛盾 |
| 11 | 自分の仕事で置き換えた古い道と、呼ぶ者の無いコードは、聞かずに消す。持ち主の単位の削除は、案を見せて「はい」の後に消す。 | :44 に足す | 661db797 と 5e9c0146 の振れ |

11 は反動を止める但し書きである。これが無いと、1〜3 は 10-31 のカーネルの削除をまた呼ぶ。

文の修理も要る。

- `AGENTS.md`:10 の混ざった文を直す。
- `AGENTS.md`:100 の空の見出しを消す。
- :80 の「組みの中の検査」を実物に合わせる。
- 測れない語を、照らせる形に置き換える。例えば「要らない分割」は「呼ぶ者が一つで、渡すだけの関数」と書く。
- 「下限 Zen 4」と WHPX が隠す機能の扱いを、持ち主の決めで一文に書く。
- 「全文を `docs/` へ」に、置き場所を足す（生きた仕様と記録を分ける）。

#### 3.8.2 手順（スキル）に置くもの

毎回は要らず、その仕事のときに要る長さの文を置く。

| # | 手順の案 | 置く場所 |
|---|---|---|
| 1 | 作り直しの段：持ち主と呼ぶ側を数える → 消す範囲を見せる → 消す → 建てる → 呼ぶ側を直す → 古い名が 0 件であることを rg で示す → 足した行と消した行を書く。 | 新しいスキルか `nocturne-project` |
| 2 | サブエージェントの依頼文の雛形。「周りに合わせる」「最小の行」「動きを変えない」を消し、3.8.1 の 1〜3 を入れる。 | スキルに雛形を置く。`docs/records/kernel-rebuild-20261031/prompt-common.md` は過去の記録として残す |
| 3 | 監査の後の修理：所見を設計の欠陥ごとにまとめる。指摘の数が二回続けて減らないときは止まる。 | 同じ |
| 4 | 所見の閉じ方：ID、`file:line`、閉じたコミット。 | 同じ |
| 5 | 区切りの報告に、足した行、消した行、残した古い道の数を書く。 | `AGENTS.md`:38 から指す |

#### 3.8.3 機械の検査（`tests/`）に置くもの

§3.7 の「はい」の行である。特に先に効くものは次のとおりである。

| # | 検査 | 止める型 |
|---|---|---|
| 1 | `make check` を組みの前提にする | 全部の入口 |
| 2 | 切り出し試験を拒む | 試験による固定 |
| 3 | 段の番号、`_new`、`同一実行禁止`、`build/goal-` を拒む | 一度きりの試験 |
| 4 | `-Wno-unused-function` を外す | 死んだ static 関数 |
| 5 | `test_flag` の場所の制限、共通コードの `hypervisor` の禁止 | 門と VM の混入 |
| 6 | コメントの中の消えたパスの照合 | 古い文 |
| 7 | 関数の長さ、一行の文の数、写しのラチェット | 悪くなる方向 |

機械に移した文は、`AGENTS.md` から消す。文を増やす形を止めるためである。

#### 3.8.4 置かない方がよいもの

- 「きれいに」「長く保てる」のような語を増やすこと。
- 依頼ごとの言い換えを規則へ足すこと。前の振り返りが「言葉が言い換えで規則になる」と書いている。
- 行数の上限を製品の目的にすること。短い誤ったコードも通る。

## 4. 消してよいものの一覧

全部は案である。消す前に持ち主へ確かめる。

理由は三つある。

- d927e533 は試験の部品の削除を打ち消し、消すのは `scripts/test.py`・`wpt.py`・`platform_qa.py` だけと書く。持ち主の指示と推定する。§4.2 はその戻された範囲と重なる可能性がある。
- 持ち主のメモリーは「GPU の道を消さない」と書く。`docs/records/kernel-rebuild-20261031/00-design.md` §3 は「VM の virgl を支える」と書く。だから virgl は消す一覧に入れない。
- 持ち主は記録を好む。だから `docs/` は消す一覧に入れず、分ける案にする。

確かさは「消しても製品の働きが変わらないこと」の確かさである。走らせて確かめたものは無い。

### 4.1 製品と道具

| ファイル | 行 | 理由 | 確かさ | 一緒に直すもの |
|---|---:|---|---|---|
| `scripts/jspace_html_control.py` | 16 | 別の道具の場所を指す。呼ぶ者が無い。734b025d 以来不変である。 | 高 | なし |
| `.claude/skills/browser-hikaku/agents/openai.yaml`、`.claude/skills/nocturne-project/agents/openai.yaml` | 12 | 別の道具の残りである。 | 高 | なし |
| `scripts/qtest.py` | 196 | `AGENTS.md`:80 が禁じる。 | 高 | `README.md`:316-320、`README_EN.md` |
| `Makefile`:621-632 の `run`、`scripts/mkimage.sh` の `--run`（:8,:115） | 12 + 未数 | `AGENTS.md`:80 が禁じる。 | 高 | `Makefile`:571、`clean` のコメント |
| カーネルと common の呼ばれない関数 20 個 | 77 | 名が定義の一度しか出ない。一覧は下にある。 | 高（`test_text` は下の kernel-test と一緒に） | `wait.h`:86,99、`.claude/rules/kernel.md` |
| `kernel/src/vm/tulip.c` | 217 | 第 1 世代の網である。第 1 世代の道は dbb7f1a5 で消えた。 | 中 | `vm/vm.c`:68、`vm/devices.h`、`Makefile`、`map.md` |
| `kernel/src/vm/ata.c`、`ata.h` | 231 + 20 | 第 1 世代か QEMU の `pc` の機械だけが要る。試験の QEMU は `-M q35` と `nvme` である。 | 中 | `vm/vm.c`:62、`vm/vm.h`:18、`tests/ata_pio_io_cases.c`（38）、`map.md` |
| `user/libc/sys.c`:10-13 の `errno` の記号 | 4 | 「古い呼ぶ者のため」である。規則は互換を持たない。 | 中（ゲストの tcc の組みの確かめが要る） | なし |
| `scripts/mksdk.sh`:45 の全部の写し | 1 行を一覧へ | 内部の見出しが SDK へ出る。 | 高（消すのは内部の見出しの公開で、ファイルではない） | `user/include/` |

呼ばれない関数の一覧は次のとおりである（`kernel/src/` からのパス、括弧は行数）。

`common/gfx.c`:271 `gfx_font_w`(1)、`common/store/latch.h`:51 `latch_held`(1)、`arch/apic.c`:170 `apic_send_ipi_set`(1)、`arch/apic.c`:172 `apic_send_ipi_others`(1)、`arch/cpu.h`:170 `cpu_ibpb`(3)、`arch/fpu.c`:81 `fpu_state_component`(6)、`arch/spin.h`:185 `spin_held`(3)、`arch/topology.c`:400 `topology_cpu_of`(10)、`boot.c`:86 `test_text`(6)、`dev/fb.c`:369 `screen_owned_by`(6)、`dev/input.c`:193 `input_mouse_present`(1)、`dev/pci.c`:276 `pci_find_class`(5)、`gui/wm.c`:131 `tiles_any`(5)、`sys/clock.c`:728 `clock_wall_seconds`(4)、`sys/log.c`:233 `klog`(6)、`sys/wait.c`:223 `wq_wait`(5)、`sys/wait.c`:229 `wq_wait_until`(6)、`sys/wait.c`:261 `sleep_until_boot`(5)、`vm/ata.c`:128 `ata_count`(1)、`vm/ata.c`:129 `ata_get`(1)。

持ち主の決めが要るもの（消すか、道を作り直すか）は次のとおりである。

| ファイル | 行 | 決めること | 確かさ |
|---|---:|---|---|
| `kernel/src/vm/virtio_gpu.c`:579-583,:240,:265,:672 の試験の門 | 数行 | **消すのは門で、道ではない。** 製品で virgl を起こすなら門を外す。これは 661db797 の修理を戻す形である。 | 高 |
| kernel-test の一式：`Makefile`:57-75、`kernel/src/boot.c`:61-141、`sys/settings.c`:125-146、`fs/bootrecord.c`:58-66、`sys/prof.c`:275、`mm/vmm.c`:649 | 約 130 | 試験の媒体の道を作り直すなら残す。作らないなら消す。 | 決め次第 |
| `user/apps/services.c`:80-82 と :84 の `"tests"`、`tests/testrun.c`（66）、`Makefile`:578,597-598 | 約 70 | 同じ決めである。 | 決め次第 |
| `user/libc/font.c` の stb_truetype の道、`user/libc/third_party_ttf.c`、`third_party/img/stb_truetype.h`（4019） | 約 4100 | FreeType が全部の場合を受けるかを先に確かめる。未確認である。 | 中 |
| `ports/mesa/nocturne_gl.c`:113-122 の softpipe の枝 | 数行 | llvmpipe が組めない機械があるか。 | 低 |
| `user/libc/sys.c`:57-79 の `#!`、`user/apps` の UNIX の道具 | 未数 | AGENTS の「UNIX ではない」との関係を決める。道具自体は ABI の互換ではない。 | 低 |

消すのでなく一つにまとめるものは次のとおりである。

- `isqrt32`（`kernel/src/gui/wm.c`:1035）と `isqrt`（`user/apps/shell_art.h`:16）。カーネルとユーザーの間なので、`common/` へ置くかを決める。
- `id_of`（`user/apps/display.c`:58）と `screen_id`（`user/apps/settings.c`:198）。
- `crc32_ieee` と `gpt_crc`、`unheld_out` の二つ、`be64` の二つ、`zalloc` の二つ。`common/store` へ。

### 4.2 試験

#### 4.2.1 走らない形の `.py` 53 本と連れ 59 本

合わせて 6689 行である（`.py` 2537、連れ 4152）。全試験 86313 行の 8% である。

型の記号は §3.4.5 のとおりである。A は署名が無く止まる、B は `open('x')`、C は消えた build の中身に依る、D は同一実行禁止、E は説明文が一度きりと言う。

確かさは次のとおりである。

- A・B・C・D は高（今の形では走らないか、一度しか走らない。静的な判定で、走らせていない）。
- E だけのものは中（説明文を信じた判定である）。

| 行 | ファイル | 型 |
|---:|---|---|
| 240 | `tests/dom_unbounded_hosttest.py` | A |
| 144 | `tests/check_navigator_native.py` | C |
| 112 | `tests/check_webnet_pacing.py` | A |
| 95 | `tests/check_flex_reverse25.py` | E |
| 88 | `tests/check_css_depth_native.py` | A |
| 79 | `tests/check_stretch_height21.py` | AE |
| 76 | `tests/check_frame_transfers_native.py` | C |
| 76 | `tests/check_margin_hoist22.py` | AE |
| 73 | `tests/check_dom_position_exception_native.py` | C |
| 73 | `tests/check_webnet_scheduler_perf.py` | A |
| 72 | `tests/check_svg_paint27.py` | DE |
| 72 | `tests/worker_broker26_new.py` | BCE |
| 61 | `tests/check_js_dynamic_queues.py` | CE |
| 60 | `tests/check_absolute_height18.py` | AE |
| 60 | `tests/check_stylesheets24.py` | AC |
| 59 | `tests/check_cascade_winner23.py` | A |
| 58 | `tests/check_frame_inline_geometry_native.py` | E |
| 58 | `tests/check_range_extract27_native.py` | C |
| 45 | `tests/auxiliary_link29_new.py` | AE |
| 44 | `tests/check_native_active18.py` | AE |
| 44 | `tests/cssom_native_new.py` | A |
| 44 | `tests/worker_lease27_new.py` | BC |
| 42 | `tests/check_frame_messages_native.py` | C |
| 42 | `tests/check_video_snapshot_native.py` | E |
| 41 | `tests/check_native_hover_overlay17.py` | A |
| 38 | `tests/image_upgrade20_new.py` | B |
| 36 | `tests/worker_heap21_new.py` | BCE |
| 34 | `tests/check_svg_paint27_viewport.py` | D |
| 33 | `tests/check_child_module_sync_native.py` | CE |
| 33 | `tests/check_css_query_unbounded_native.py` | E |
| 32 | `tests/crypto_aes_hosttest.py` | C |
| 30 | `tests/check_retired_tasks_native.py` | C |
| 28 | `tests/webnet_url22_new.py` | B |
| 27 | `tests/check_control_physical_edit.py` | E |
| 27 | `tests/http_origin20_new.py` | B |
| 26 | `tests/check_inner_text_native.py` | A |
| 26 | `tests/check_same_forest_native.py` | AE |
| 26 | `tests/worker_messaging23_new.py` | BCE |
| 25 | `tests/http_request_headers_hosttest.py` | E |
| 23 | `tests/check_frame_unbounded_native.py` | E |
| 23 | `tests/check_inline_link19.py` | E |
| 23 | `tests/check_svg_paint27_percent.py` | DE |
| 22 | `tests/check_range_delete_native.py` | CE |
| 22 | `tests/check_svg_paint27_codec.py` | DE |
| 21 | `tests/check_node_cache_native.py` | C |
| 21 | `tests/http_owned_span21_new.py` | BE |
| 20 | `tests/worker_clone25_new.py` | BCE |
| 19 | `tests/http_hostname19_new.py` | B |
| 17 | `tests/http_dynamic_headers_hosttest.py` | E |
| 15 | `tests/paint_stacking_new.py` | A |
| 12 | `tests/check_ephemeron_native.py` | CE |
| 11 | `tests/worker_broker26_ownership.py` | BCE |
| 9 | `tests/http_method18_new.py` | E |

連れ（上の `.py` だけが指すもの）59 本、4152 行は次のとおりである。すべて `tests/` の直下にある。行数はファイル名の後の数である。

`video_snapshot_native_cases.c` 167、`child_module_sync_native_cases.c` 149、`http_request_headers_hosttest.c` 138、`flex_reverse25_cases.c` 134、`webnet_scheduler_perf_cases.c` 131、`node_cache_native_cases.c` 125、`webnet_url22_new.c` 123、`stretch_height21_cases.c` 113、`cascade_winner23_cases.c` 111、`worker_stream_hosttest.c` 106、`worker_heap21_new.c` 105、`frame_unbounded_native_cases.c` 104、`http_origin20_new.c` 102、`js_dynamic_queues_prefix.c` 99、`css_depth_native_cases.c` 96、`http_hostname19_new.c` 94、`stylesheets24_native_prefix.c` 91、`http_dynamic_headers_hosttest.c` 89、`webnet_pacing_cases.c` 86、`frame_messages_native_prefix.c` 82、`auxiliary_link29_new.c` 79、`native_active18_cases.c` 76、`range_delete_native_cases.c` 76、`http_method18_new.c` 73、`margin_hoist22_cases.c` 71、`frame_messages_native_cases.c` 69、`frame_transfers_native_cases.c` 68、`native_hover_overlay17_cases.c` 68、`same_forest_native_cases.c` 66、`retired_tasks_native_cases.c` 65、`cssom_native_new_cases.c` 63、`frame_inline_geometry_native_cases.c` 62、`sharedworker_hosttest.c` 62、`image_upgrade20_new_main.inc` 60、`http_owned_span21_new.c` 59、`css_query_unbounded_native_cases.c` 58、`js_range_extract27_cases.js` 56、`absolute_height18_cases.c` 55、`stylesheets24_native_cases.c` 55、`js_range_delete_cases.js` 53、`svg_paint27_cases.c` 52、`ephemeron_native_cases.c` 48、`inner_text_native_cases.c` 47、`worker_broker26_new.c` 44、`frame_commit_fault_cases.c` 43、`inline_link19_cases.c` 43、`worker_lease27_new.c` 42、`stylesheets24_cases.js` 41、`svg_paint27_codec_cases.c` 39、`worker_broker26_ownership.c` 35、`frame_transfers_poison_cases.c` 32、`auxiliary_link29_new.js` 28、`paint_stacking_new.c` 25、`range_delete_native_prefix.js` 25、`retired_tasks_native_prefix.c` 22、`svg_paint27_percent_cases.c` 21、`webnet_url22_alloc.h` 14、`flex_reverse25_clamp_case.c` 7、`worker_clone25_new.c` 5。

注意が二つある。

- 消すと、これらが確かめていた内容の覆いも失う。試験の内容（例：フレックスの逆順、Range の削除）に価値があるものは、公開の口で書き直す方がよい。どれに価値があるかは一本ずつの判断で、私はしていない。
- `build/goal-` を固定して `mkdir` の無いものが、A・B・C の外に 14 本ある。うち 6 本は E として上の表に入っている。残り 8 本（`check_font_probe_native`、`check_layout_extent18`、`check_scroll_area20`、`clone_attributes_hosttest`、`element_scroll_hosttest`、`fetch_keepalive_lifetime_new`、`scroll_into_view_hosttest`、`worker_origin17_new`）は、パスを直せば走る可能性があるので一覧に入れていない。

#### 4.2.2 誰も指さない 70 本

合わせて 8034 行である（下の表の行は `testgraph2.py` の数えで各ファイル 1 行多く、和は 8104 になる）。全試験の 9% である。

確かさは中である。「誰も指さない」は `.py`、`Makefile`、ゲストの表、他の試験の字句に名が無いことを言う。消えた `scripts/test.py` だけが指していたものも入る可能性がある。d927e533 が戻した範囲と重なる可能性がある。

| 行 | ファイル |
|---:|---|
| 1541 | `tests/pcitest.c` |
| 394 | `tests/nvk_rm_hosttest.c` |
| 345 | `tests/tccguardtest.c` |
| 318 | `tests/zink_start_hosttest.c` |
| 318 | `tests/webfetch_cache_hosttest.c` |
| 249 | `tests/mesa_screen_hosttest.c` |
| 221 | `tests/indexeddb_native_cases.c` |
| 212 | `tests/storage_shared_native_cases.c` |
| 212 | `tests/js_event_state_hosttest.js` |
| 204 | `tests/js_activation_hosttest.js` |
| 195 | `tests/smptest.c` |
| 186 | `tests/compat_site_observer.js` |
| 182 | `tests/wasmgctest.c` |
| 180 | `tests/nvk_ctx_hosttest.c` |
| 178 | `tests/nativechanneltest.c` |
| 148 | `tests/mlfqtest.c` |
| 138 | `tests/qjs_oomtest.c` |
| 134 | `tests/network_scheduler_hosttest.c` |
| 134 | `tests/media_http_cors_hosttest.c` |
| 115 | `tests/svgtest.c` |
| 108 | `tests/html_interface_site_observer.js` |
| 96 | `tests/image_ico_hosttest.c` |
| 89 | `tests/wmramtest.c` |
| 86 | `tests/nvk_sync_hosttest.c` |
| 86 | `tests/http2_queue_hosttest.c` |
| 82 | `tests/wmframetest.c` |
| 82 | `tests/network_url_hosttest.c` |
| 76 | `tests/media_draw_hosttest.c` |
| 75 | `tests/nvrmtest.c` |
| 72 | `tests/http_body_limit_hosttest.c` |
| 69 | `tests/unifont_guest_test.c` |
| 69 | `tests/gpu_browser.html` |
| 68 | `tests/slop8_site_observer.js` |
| 68 | `tests/frame_initial_failure_hosttest.c` |
| 65 | `tests/js_regexp_legacy_cases.js` |
| 63 | `tests/http_header_batch_native.c` |
| 62 | `tests/js_document_collections_hosttest.js` |
| 61 | `tests/shiftjistest.c` |
| 56 | `tests/media_yuv_sse_test.c` |
| 55 | `tests/media_boundary_hosttest.c` |
| 51 | `tests/transport8_host.c` |
| 50 | `tests/js_element_scroll_hosttest.js` |
| 47 | `tests/wmholdtest.c` |
| 46 | `tests/data_url_hosttest.c` |
| 44 | `tests/js_element_scroll_native.c` |
| 43 | `tests/storage_hotpath_native_cases.c` |
| 41 | `tests/js_avmedia_range_cases.js` |
| 41 | `tests/gpu_cpu_hosttest.c` |
| 39 | `tests/storage_batch_native_cases.c` |
| 39 | `tests/js_scroll_into_view_native.c` |
| 39 | `tests/ata_pio_io_cases.c` |
| 38 | `tests/js_avmedia_worker_cases.js` |
| 37 | `tests/slop8_search_observer.js` |
| 37 | `tests/js_range_delete_live_cases.js` |
| 36 | `tests/js_frame_inline_geometry_cases.js` |
| 35 | `tests/web_font_site_observer.js` |
| 35 | `tests/net8_browser_observer.js` |
| 34 | `tests/wasm_sites_observer.js` |
| 34 | `tests/media_worker_quota_hosttest.c` |
| 34 | `tests/media_shared_window_hosttest.c` |
| 33 | `tests/js_fragment_constructor_hosttest.js` |
| 32 | `tests/media_performance_hosttest.c` |
| 30 | `tests/webfetch_body_limit_hosttest.c` |
| 26 | `tests/worker_shared_cancel_hosttest.c` |
| 26 | `tests/gfxmix_hosttest.c` |
| 25 | `tests/sharedworker_runtime_hosttest.c` |
| 21 | `tests/worker_site_observer.js` |
| 11 | `tests/js_clone_attributes_native.c` |
| 4 | `tests/worker_site_acceptance.sh` |
| 4 | `tests/cxx_tls_probe.cpp` |

`mlfqtest.c` は消えたスケジューラーの名 MLFQ を持つ（窓の題と `printf`。全部は読んでいない）。`pcitest.c` は 1541 行で一番大きく、`main` が 373 行ある。

#### 4.2.3 試験の小計

| 群 | ファイル | 行 | 確かさ |
|---|---:|---:|---|
| 走らない形の `.py` | 53 | 2537 | 高（E だけのものは中） |
| その連れ | 59 | 4152 | 高（`.py` と同じ） |
| 誰も指さない | 70 | 8034 | 中 |
| 合計 | 182 | 14723 | 全試験 約 8 万 6 千行の 17% |

### 4.3 記録（消さない。分ける案）

| 群 | 件 | 案 | 確かさ |
|---|---:|---|---|
| 指されない記録 | 208 | `docs/archive/` か同じ形の保管の場所へ動かす | 高（消す話ではない） |
| 記録の中だけで指されるもの | 151 | 同じ。参照の相対パスを一緒に直す | 中 |
| 生きた仕様 `docs/nocturne-native-abi.md` | 1 | 今の場所に残す | － |
| 台帳 | 2 | 残す。開いた所見の一覧を別に作る | － |

`AGENTS.md` の「全文を `docs/` へ」は、この分け方に合わせて保管の場所を指すように変える。
