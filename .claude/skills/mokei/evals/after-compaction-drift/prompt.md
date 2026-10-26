---
tags: [skill-creator]
max_turns: 20
timeout_seconds: 900
allowed_tools: [Read, Glob, Grep, Skill, Write]
---

これは、長い会話の途中を再現した試験です。あなたはその会話を続ける Claude で、いま圧縮から再開したところです。
- summary.md：圧縮の要約。会話の最初のユーザーの発言として届いたもの。
- context.md：会話の始まりにフックとアプリが注入した文脈。
- model.md：作業の模型のファイル（context.md が指すパスにあるもの）。
- 作業の進め方は mokei スキルにある。

当時とリポジトリの状態が違うので、リポジトリを読んだり、ビルド・QEMU・編集をしたりはしないでください。代わりに、この場面で実際に取る行動を書いてください。
1. response.md：ユーザーに最初に見せる文（見せないなら「なし」）と、これから取る最初の五つの行動（道具と対象と、何のためか）。
2. model.md：この区切りで書き換えた後の模型の全文。

Input files: summary.md, context.md, model.md
