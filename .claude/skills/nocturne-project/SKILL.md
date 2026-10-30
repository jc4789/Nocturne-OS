---
name: nocturne-project
description: Nocturne OS の実物を動かして確かめる手順。媒体は build/nocturne.img（増分ビルド qemu-image で上書き）と写し一つまで。QEMU は窓付きで二台まで起動して窓が出たことを確かめ、HMP で撮影・キー・マウスを送り、serial.log と F12 の console の行を読む（行の一覧は references/logs.md、起動と落とし穴は references/qemu-visual.md）。三つ目の媒体・三台目や窓なしの QEMU・build の下の記録は作らない。WPT は変更が触れた仕様だけ、対象を絞って走らせる。Nocturne のカーネル・GUI・ブラウザー・ドライバーのどれを直すときも、ビルドする前、QEMU を起動するとき、ログを読むとき、画面を撮るとき、「直った」「速くなった」「退行ではない」と書く前に必ず使う。性能・かくつき・落ちるコマ・固まる・白い画面の報告を受けたときは、推測の前にこのスキルでログを読んで層を絞り、その先はコードと仕様を読んで直す。確かめは直す前に一度、直した後に一度で、確かめの回を重ねない。実機と Hyper-V はユーザーの担当。
---

# Nocturne：実物を動かして確かめる

## 置き場所と道具

- QEMU：`C:/Program Files/qemu/qemu-system-x86_64.exe`（WHPX）。同梱の `tools/msys64/ucrt64/bin/qemu-img.exe`。
- ビルド：`python -X utf8 scripts/build.py -j 8 <目標>`。目標はいつも書く（書かないと `all` になる）。
  - `qemu-image`：img だけ。`build/nocturne.img` を増分で上書きする。確かめはこれ。
  - `all`：vhd・vhdx・iso も作る。Hyper-V へ渡すときと、頼まれたときだけ。
  - `kernel`、`user`：その部分だけ。
  - `run`（`build/run.img`）と `test`・`test-quick`・`test-full`（`build/test.img` へ写す）は、窓なしで起こす作りが残る間は使わない（下の「試験の道具について」）。
- **媒体は二つまで。** 本線は `build/nocturne.img` で、`qemu-image` が上書きする。二つ目は一度に一つ：変える前の写し（`build/mae.img`）か、試験の道具の写し（`build/wpt/wpt.img`・`build/test.img`）。三つ目、ラベル違いの写し、旧版から組んだ媒体を作らない。build の下に、別の組み、書き出したソース、コード、記録のフォルダーを置かない。2026-10-29、ユーザーは build/ を丸ごと消して自分で組み直し「媒体は一つ」「増分で組む」「全体ビルドを習慣にしない」と言い、同じ日の夕方に写し一つと QEMU 二台までを許した。以前の禁止の理由は、QEMU を何台も起こして RAM が尽き、ページングに落ちて速さの値が歪んだこと。
- 媒体の中身：`scripts/layout.py` が `build/c.list` を書き、`build/host/store.exe fill` が C: に詰める。ソースを戻しても媒体は戻らない。
- 撮った画像と serial.log の置き場所は、この会話の作業場（システムプロンプトにある scratchpad の場所）。build の下ではない。残す値は台帳に文章と数字で書く。
- ログの行の一覧と取り方：[references/logs.md](references/logs.md)。QEMU の起動・窓の確認・HMP・落とし穴：[references/qemu-visual.md](references/qemu-visual.md)。

## 進め方

確かめは直すためにある。直す前に一度（要るときだけ）、直した後に一度。確かめた結果が編集につながらないなら、次は読む。

1. **症状の層をログで絞る。** 不具合の報告（遅い、かくつく、固まる、白い、落ちる）を受けたら、推測の前に serial.log と F12 の console の行を読む。動画なら `Native media timing` の `drop`・`starved`、描画なら `Browser loop profile`・`Browser damage profile`、カーネルなら `prof:` と `[memory]`。行の意味は logs.md。層が分かったら、ログを読み足さない。コードで欠陥が見えているなら（`rg` で当たる、仕様と違う手順）、この段も次の段も飛ばして 3 へ。
2. **性能や退行を言うなら、変える前に今の媒体で見て書き残す。** 同じ組・同じ条件（2560×1440、F12 を開いたまま、QEMU は窓付き一台、計測の旗なし）の画面とログを作業場に置き、値と行を台帳か模型に書く。媒体は一つなので、これが「前」の全部。前の回の記録が無ければ、退行かどうかは未確認と書く。前の値が手元にあるなら、前の版を組み直して測り直さない。
3. **読んで直す。これが仕事の本体。** `rg`・codegraph で場所を絞り、原文を読む。照らすのは仕様の原文（W3C・WHATWG、`D:\Programes\OS DEV INFO` の仕様書、`docs/nocturne-native-abi.md`）。欠陥が見えたら、ログや撮影で確かめ直さず直す。
4. **直した後に一度、増分ビルドして同じ条件で確かめる。** `qemu-image` が同じ img を上書きする。動いている QEMU を HMP の `quit` で止め、同じ命令で起動し直し、同じ組を撮って同じ行を並べる。前の写し（`build/mae.img`）があれば二台目で並べる。変更が触れた仕様のディレクトリーの WPT を対象を絞って走らせる。ブラウザーの照合と速さの比べ方は `browser-hikaku` スキル。直っていなければ 3 へ戻る。もう一度撮り直すのではない。
5. ホストの検査（`tests/check_*.py`）で要るものだけ走らせる。合格は、その検査が通る範囲の証拠にしかならない。

計測の旗（`--debug-js`）は原因を探す回に使う。製品の速さは旗なし（または `--log-console` だけ）の回で測る。計測を足したら、その道具自身の費用も測る。

ユーザーが組んだビルドがあれば、そのまま使う。ビルドの規則（Makefile・scripts）を変えたら、空の build/ から組んでも通るか（生成物の規則、PATH に無い道具。ユーザーの PowerShell には Git の usr/bin が無い）を読んで確かめる。2026-10-29、ユーザーの空の build/ からのビルドが、私の変更で二度止まった。前の回の出力が残っていたので隠れていた。ユーザーのビルドで欠けや壊れが出たら私の責任。

## QEMU を窓付きで起動する（二台まで）

```text
"C:/Program Files/qemu/qemu-system-x86_64.exe" -M pc -m 4096M -smp 4 -accel whpx -cpu host -vga none -device VGA,xres=2560,yres=1440,vgamem_mb=32
  -serial file:<作業場>/serial.log -monitor tcp:127.0.0.1:45454,server,nowait
  -device ahci,id=ahci -drive id=boot,file=build/nocturne.img,format=raw,if=none,snapshot=on
  -device ide-hd,drive=boot,bus=ahci.0,bootindex=0 -netdev user,id=qa -device e1000,netdev=qa
```

- 起動する前に、動いている QEMU の数を見る。二台動いていれば三台目を起動せず、どちらかを `quit` する。二台目は `-monitor tcp:127.0.0.1:45455` と別名の serial（`<作業場>/serial-2.log`）で起こす（同じ img を `snapshot=on` で二台が読んでよい）。速さを測る回は一台だけ動かし、もう一台は `quit` する（RAM と CPU を取り合うと値が歪む）。
- **窓が出たことを確かめる。** PowerShell の `Get-Process qemu-system-x86_64 | Select-Object Id,MainWindowHandle` で MainWindowHandle が 0 でないこと。`-display none`・`-nographic` を付けない。窓なしの QEMU は禁止（2026-10-29 ユーザー）。ユーザーはその窓を見ている。
- `-cpu host` は外さない。CPU の下限は Zen 4 で、WHPX はこれで XSAVE・AVX-512・FSGSBASE・INVPCID・SMEP・SMAP・x2APIC を見せる（PCID は見せない。2026-10-30 に起動の行 `cpu: cpuid …` で確かめた）。付けないと「QEMU Virtual CPU version 2.5+」になり、下限を満たさない。
- 起動は `serial.log` の `store: C: "…" on sata0` で確かめる。数秒で出なければ待たずに異常として扱う。
- HMP（TCP 45454）へ一行ずつ送る：`screendump "<作業場>/now.png" -f png`（スラッシュ区切りの絶対 path）、`sendkey ctrl-l`、`mouse_move 400 300`、`mouse_button 1`／`0`。撮った PNG は自分で開いて見る。固定秒数の一発スクリプトや、デスクトップの computer-use の撮影で代用しない。
- `snapshot=on` なので媒体は汚れず、ゲストの変更も残らない。再起動をまたぐ保存の確認は qemu-visual.md。
- ブラウザーに旗を付けるには、ゲストの Terminal で `browser --debug-js <URL>` を打つ（HMP の `sendkey` で）。
- 自分が起動したものは、終わりに HMP の `quit` で止める。

## 試験の道具について

- **WPT は対象を絞って走らせる**（2026-10-29 ユーザー：全部は多すぎる）。`python -X utf8 scripts/wpt.py --label <名> <WPT の path>…`（`dom/`、`fetch/api/basic/`、`css/css-anchor-position/` のように、変更が触れた仕様のディレクトリー。`--limit` も可）。結果は `build/wpt/<名>/`（results.jsonl・summary.txt・failures.txt・serial.log）。前後を比べるなら、同じ path を前の写し（`--boot-image build/mae.img --skip-build-check --runner-source <その版の tests/wptrunner.c>`）と今の媒体で順に走らせ、通った数と failures.txt の形を並べる。もう一台 QEMU が動いているなら `--cpus 4 --memory 4096`。WPT の全部を回すのは、ユーザーが頼んだときだけ。
- **道具を窓付きに直してから使う。** `scripts/wpt.py`（671 行の `"-display", "none"`、677〜678 行の `CREATE_NO_WINDOW`）と `scripts/test.py`（525 行の `-display`）は今も窓なしで起こす。直すのは窓を出すことだけで、写し（`build/wpt/wpt.img`・`build/test.img`）は二つ目の媒体として許される。`--slots`・`--cpus`・`--memory` は一台の中のブラウザーの数と大きさで、台数ではない。フック（`.claude/hooks/mokei.py`）は、窓なしの印がスクリプトに残る間、三つ目の媒体になるとき、三台目の QEMU になるときに、これらの命令を止める。
- `scripts/platform_qa.py`（ラベルごとに `build/nocturne-platform/<label>/` へ写す）と `qtest.py` は使わない。写しが増え、固定秒数の一発の代用になる。

## 撮影と操作の受入

- 画像は主担当が自分で開いて見る。保存した・サブエージェントが見た・hash が違う、では未検証。
- 壊れた画面を「正常」と言わない。参照（Claude Code の内蔵ブラウザーで同じ URL を開いた画面・ユーザーの Floorp の画像）の部品を一つずつ照合する（2026-10-16、関連動画の列とチャンネル情報が欠けた画面を正常と誤報）。前の回の Nocturne の画像は参照ではない。
- ブラウザーの撮影は F12 の console を開いたまま行い、console の行も読む（2026-10-27 ユーザーの硬い規則）。2560×1440 の画面とブラウザーの表示域は別で、要れば窓を最大化する。
- スクロールは実際の PgDn・wheel・scrollbar で、前後を撮って見比べる。下だけでなく戻りも。console・アドレス欄・入力欄に PgDn を送って頁送りの合格にしない。
- クリックは実マウスで、直前に位置と対象を確かめる。JS の `.click()` や DOM 操作だけで GUI 入力の合格にしない。
- 起動中・読み込み中でも今の画面は撮れる。ready marker や author JS の完了を撮影の条件にしない。
- サイトの challenge・認証要求・HTTP エラーは見た状態として記録し、回避や成功の偽装をしない。
- 見た結果はサイトごとに短く残す：画像の絶対 path、実 URL、媒体を組んだ時刻とコミット、見えた本文と配置、操作と前後の変化、合格／失敗／未確認、次に直す境界。

## 証拠の範囲

静的な確認、ビルド、ホストの検査の PASS、対象を絞った WPT の結果、QEMU での実サイトの表示、実際の GUI 操作、性能の比較、Hyper-V・実機は別々に書き、一つの成功を他へ流用しない。fixture・host mock・サブエージェントの PASS はその範囲の証拠だけ。サブエージェントに任せた差分は、取り込む前に自分で読み、同じ媒体で確かめる。機能の検査が通っても画面が白い・重なる・操作できないなら、その不具合を先に直す。
