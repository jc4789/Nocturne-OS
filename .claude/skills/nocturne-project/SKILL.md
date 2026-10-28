---
name: nocturne-project
description: Nocturne OS の実物を動かして確かめる手順。QEMU の起動と HMP での撮影・キー・マウス、serial.log と F12 の console の読み方（行の一覧は references/logs.md）、ビルドと媒体、scripts/test.py と platform_qa.py の使い方。Nocturne のカーネル・GUI・ブラウザー・ドライバーのどれを直すときも、ビルドする前、QEMU を起動するとき、ログを読むとき、画面を撮るとき、「直った」「速くなった」「退行ではない」と書く前に必ず使う。性能・かくつき・落ちるコマ・固まる・白い画面の報告を受けたときは、推測の前にこのスキルでログを読む。実機と Hyper-V はユーザーの担当。
---

# Nocturne：実物を動かして確かめる

## 置き場所

- QEMU：`C:/Program Files/qemu/qemu-system-x86_64.exe`（WHPX）。同梱の `tools/msys64/ucrt64/bin/qemu-img.exe`。
- ビルド：`python -X utf8 scripts/build.py -j 8 [目標]`。`all`（img・vhd・vhdx・iso）、`qemu-image`（img だけ。試験の繰り返しはこれ）、`kernel`、`user`。出力は `build/nocturne.img`、`build/kernel.elf`、`build/programs/`。
- 媒体の中身：`scripts/layout.py` が `build/c.list` を書き、`build/host/store.exe fill` が C: に詰める。ソースを戻しても媒体は戻らないので、試験の媒体が今のソースから作られたかを先に確かめる（test.py は kernel と programs の sha256 を照合する）。
- ログの行の一覧と取り方：[references/logs.md](references/logs.md)。QEMU の起動・HMP・落とし穴：[references/qemu-visual.md](references/qemu-visual.md)。

## 進め方

1. **ログを先に読む。** 不具合の報告（遅い、かくつく、固まる、白い、落ちる）を受けたら、推測の前に serial.log と F12 の console の行を読む。動画なら `Native media timing` の `drop`・`starved`、描画なら `Browser loop profile`・`Browser damage profile`、カーネルなら `prof:` と `[memory]`。行の意味は logs.md。
2. **基準を取る。** 変更前の媒体で、同じ組・同じ条件（2560×1440、F12 を開いたまま、QEMU は一台、計測の旗なし）の画面とログを残す。基準が無ければ退行かどうかは判定できないと書く。
3. 原因を我々のコードで追う（codegraph で読む所を絞り、原文を読む）。直す。
4. **同じ条件で再確認。** 変更後の媒体で同じ組を撮り、同じ行を並べる。ブラウザーの照合と速さの比べ方は `browser-hikaku` スキル。
5. 要る回帰試験だけ走らせる（下の「試験」）。合格は、その試験が通る範囲の証拠にしかならない。

計測の旗（`--debug-js`）は原因を探す回に使う。製品の速さは旗なし（または `--log-console` だけ）の回で測る。計測を足したら、その道具自身の費用も測る。

## QEMU を一台、窓付きで起動する

```text
"C:/Program Files/qemu/qemu-system-x86_64.exe" -M pc -m 4096M -smp 4 -accel whpx -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=32
  -serial file:<run>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait
  -device ahci,id=ahci -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on
  -device ide-hd,drive=boot,bus=ahci.0,bootindex=0 -netdev user,id=qa -device e1000,netdev=qa
```

- 起動する前に、動いている QEMU が無いかを見る。あればそれを使い、二台目を起動しない。自分が起動したものは終わりに HMP の `quit` で止める。
- 起動は `serial.log` の `store: C: "…" on sata0` で確かめる。数秒で出なければ待たずに異常として扱う。
- HMP（TCP 45454）へ一行ずつ送る：`screendump "D:/…/now.png" -f png`（スラッシュ区切りの絶対 path）、`sendkey ctrl-l`、`mouse_move 400 300`、`mouse_button 1`／`0`。撮った PNG は自分で開いて見る。
- `snapshot=on` なので媒体は汚れず、ゲストの変更も残らない。再起動をまたぐ保存の確認だけ qcow2 の overlay（qemu-visual.md）。
- ブラウザーに旗を付けるには、ゲストの Terminal で `browser --debug-js <URL>` を打つか、`platform_qa.py --command` を使う。

## 試験

- `python -X utf8 scripts/test.py [--quick] [--no-net] [--accel whpx] [群|群/名]`：`build/nocturne.img` を `build/test.img` へ写して窓なしで起動し、`C:\Tests\autorun.sh` から `tests/runtests.c` を tcc で組んで走らせる。群と名は `runtests.c` の `tests[]`（117 群 227 件。例 `web`、`web/fonts`、`web/engine`、`web/javascript`）。既定は tcg・2048 MiB・4 CPU・1920×1080・1200 秒。結果は `build/test-serial.log`・`build/test-metadata.json`、別に残すなら `--output-dir build/<新しい名>`。
- `python -X utf8 scripts/platform_qa.py --label <名> --accel whpx --memory 4096 --resolution 2560x1440 --url <URL> --seconds <秒>`：ラベル専用の写しを snapshot=on で一台起動し、`--command`（ゲストのコマンド）・`--key`・`--timed-key 秒:キー`・`--cmdline prof`（カーネルの標本）・`--expect`・`--reject` で操作と判定。`build/nocturne-platform/<label>/` に serial と画面が残る。既定は tcg・2048 MiB・1920×1080。
- `tests/check_*.py` はホストの検査で、各自 `python tests/check_<名>.py`。まとめる runner は無い。
- 同じ試験を繰り返さない。再実行は、製品を直した、試験の不備を直した、別の条件で切り分ける、のどれかを言えるときだけ。期待値を弱める・skip を足す・製品の経路を迂回して合格を作らない。

## 撮影と操作の受入

- 画像は主担当が自分で開いて見る。保存した・サブエージェントが見た・hash が違う、では未検証。
- 壊れた画面を「正常」と言わない。参照（ユーザーの画像・ホストのブラウザー）の部品を一つずつ照合する（2026-10-16、関連動画の列とチャンネル情報が欠けた画面を正常と誤報）。
- ブラウザーの撮影は F12 の console を開いたまま行い、console の行も読む（2026-10-27 ユーザーの硬い規則）。2560×1440 の画面とブラウザーの表示域は別で、要れば窓を最大化する。
- スクロールは実際の PgDn・wheel・scrollbar で、前後を撮って見比べる。下だけでなく戻りも。console・アドレス欄・入力欄に PgDn を送って頁送りの合格にしない。
- クリックは実マウスで、直前に位置と対象を確かめる。JS の `.click()` や DOM 操作だけで GUI 入力の合格にしない。
- 起動中・読み込み中でも今の画面は撮れる。ready marker や author JS の完了を撮影の条件にしない。
- サイトの challenge・認証要求・HTTP エラーは見た状態として記録し、回避や成功の偽装をしない。
- 見た結果はサイトごとに短く残す：画像の絶対 path、実 URL、媒体の版、見えた本文と配置、操作と前後の変化、合格／失敗／未確認、次に直す境界。

## 証拠の範囲

静的な確認、ビルド、試験の PASS、QEMU での実サイトの表示、実際の GUI 操作、性能の比較、Hyper-V・実機は別々に書き、一つの成功を他へ流用しない。fixture・host mock・サブエージェントの PASS はその範囲の証拠だけ。機能試験が通っても画面が白い・重なる・操作できないなら、その不具合を先に直す。
