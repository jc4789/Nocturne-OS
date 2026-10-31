---
paths:
  - "ports/nvidia/**"
  - "ports/mesa/**"
  - "docs/nvidia/**"
---
# NVIDIA ドライバーと Mesa の領域に触れたとき

- 作業：NVIDIA の open-gpu-kernel-modules 615.78.08 を Mesa につなぎ、RTX 4070（AD104）を実機で動かす。参照は `D:\Programes\cloned repos` の open-gpu-kernel-modules・nvidia-driver 615.78.08・open-gpu-doc・mesa。`kernel-open/` も NVIDIA 自身のコードで、比較の基準に入る。Rust への移植のやり方は私の判断。
- `docs/nvidia/LEDGER.md` は過去の記録。読むなら「0. 動かない前提」「1. 決定事項」「3. 段階と状態」「6. 次の一手」だけ。今の状態は盤（`ban`）。
- 監査は、参照元の構成（RM の OS 層インターフェース `os-interface.h`・`nv.h`、各エンジンとクラス、open-gpu-doc の各資料）と Nocturne 側の実装を対応づけ、欠けているもの・スタブのもの・誤っているものを網羅して一覧にする。所見は検証してから出す。報告は全文を `docs/` へ。
- 実機の観測手段は写真だけ。iGPU（AMD）へ画面を出す、BIOS で IOMMU を切る、二台目のディスクを足す、といった環境の変更を勧めない。
- `gsp_ga10x.bin` は git に入れず、配布もしない（gitignore の手元の写しは可）。
