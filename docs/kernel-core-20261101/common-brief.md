# 書き手に共通の依頼

Nocturne OS（独自の OS、カーネルは C、対象は実機）。リポジトリは `D:\Programes\llm vibe slop os`。あなたは自分の作業木（git worktree）で働く。土台は main の df893170（カーネルの中核を置き直したコミット）。

## 持ち主の要件
- GUI・GPU・合成（compositor）が第一級。カーネルが原因のコマ落ち・かくつきがゼロであること。規模の例：600 Hz の画面、3 枚の画面（それぞれ違う周波数、VRR もある）、ゲームと YouTube と Discord を同時に開いて、カーネルのせいで落ちるコマが無い。この規模に耐えない選択は採らない。
- Linux・UNIX・POSIX を標準とも、良い作りの手本とも見なさない。似ているというだけで良い作りを捨てもしない。物差しは「最良か」「GUI が先か」「ただ動くか」。
- 対象は今の PC（持ち主の機械は Ryzen 5 7600X・RTX 4070）。その機械だけに合わせない。下限は Zen 4。上は 2 ソケットの EPYC（論理 CPU 1024、NUMA）。小さな固定の上限を置かない。
- shim・stub・決め打ちの値・一つの呼ぶ側のための包み・特例の分岐は禁止。悪いコードは形を変えて残さず、消してから建てる。互換は持たない（呼ぶ側を新しい口に合わせて直す）。きれいで保てるコード。要る所には抽象化の層を置く。
- ネイティブ ABI（`common/abi.h`・`display_abi.h`・`gpu_abi.h`、仕様 `docs/nocturne-native-abi.md`）を迂回しない。要るなら Nocturne の形で直し、仕様書も同じコミットで合わせる。
- VM だけの処理は `kernel/src/vm/vm.h` の後ろ。
- コメントと文書は今の形だけを書く。周りのコードの書きぶりに合わせる。

## 材料
- 地図 `.claude/rules/map.md`、カーネルの事実 `.claude/rules/kernel.md`（あれば）。
- 中核の設計と報告：`docs/kernel-core-20261101/core-design.md`・`core-report.md`。
- 監査：`docs/kernel-core-20261101/audit-compositor.md`・`audit-kernel.md`。監査は静的な読みで、行番号は df893170 の前の版。原文で照らすこと。
- codegraph（MCP の `codegraph_explore`）と rg。上流のソースは `D:\Programes\cloned repos`、仕様書は `D:\Programes\OS DEV INFO`（AMD APM・PPR、Intel SDM、xHCI、USB、NVMe、PCIe、ACPI、VESA の DisplayPort・EDID など）。ハードウェアの事実はそこの公式の文書で照らす。どの OS のソースコードも読まない、参照しない。GPLv2 だけのコードを足さない。

## 組みと確かめ
- `python -X utf8 scripts/build.py -j 8 kernel`、続けて `qemu-image`（目標はいつも書く）。警告を残さない。作業木での組みは build.py が扱う。組めない理由が作業木にあるなら、そう報告する。
- 窓付きの QEMU で確かめる。手順は `.claude/skills/nocturne-project/SKILL.md` と `references/qemu-visual.md`（q35・UEFI・NVMe・`-cpu host`・`-accel whpx`・USB の tablet と kbd）。`-display none`・`-nographic` は禁止。QEMU は全体で同時に二台まで：起こす前に `Get-Process qemu-system-x86_64` で数を見て、二台動いていれば待つ。ほかの書き手と別のポートを使う（下の自分の番号）。撮った画像は自分で開いて見る。終わったら自分の QEMU を HMP の `quit` で止める。
- serial.log と画像は `C:\Users\cesta\AppData\Local\Temp\claude\D--Programes-llm-vibe-slop-os\7fc71e82-9718-413d-a849-53188fbe3448\scratchpad\<自分の名>\`。
- 変更が触れた仕様のディレクトリーの WPT があれば `scripts/wpt.py` で絞って。
- 作業木の枝にコミットする（日本語のメッセージ、末尾に `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`）。main には触れない。`git stash`・`revert`・`restore`・`checkout -- <path>` を使わない。
- この Windows の Python はテキストモードで LF を CRLF にする。Python で書くなら `newline='\n'`。
- サブエージェントをさらに出さない。

## 報告
`docs/kernel-core-20261101/<自分の名>-report.md` に日本語で全文（依頼文の全文を先頭に。続けて：設計、変えたファイル、消したもの、組みの結果、QEMU で見たもの（画像の絶対 path と serial.log の行）、残った欠陥と未確認）。作業木の枝の名とコミットを返事に書く。
