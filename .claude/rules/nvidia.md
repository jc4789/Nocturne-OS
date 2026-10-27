---
paths:
  - "ports/nvidia/**"
  - "ports/mesa/**"
  - "docs/nvidia/**"
---
# NVIDIA ドライバーと Mesa の領域に触れたとき

- 作業台帳は `docs/nvidia/LEDGER.md`（約 115 KB）。始めに読むのは「0. 動かない前提」「1. 決定事項」「3. 段階と状態」「6. 次の一手」。「4. 主張の表」は担当の項目だけ、「7. 記録」は新しい数件だけ読む。全部を読まない。区切りごとに状態・主張・決定を更新する。
- 対象は実機の RTX 4070（AD104）。参照は `D:\Programes\cloned repos` の open-gpu-kernel-modules 615.78.08・open-gpu-doc・mesa。`kernel-open/` も NVIDIA 自身のコードで、比較の基準に入る。
- 監査を頼まれたら、記憶の直近の問題から始めず、参照元の構成（RM の OS 層インターフェース、各エンジンとクラス、open-gpu-doc の各資料）と Nocturne 側の実装を対応づけ、欠けているもの・スタブのもの・誤っているものを一覧にする。所見は検証してから出す。
- 実機の観測手段は写真だけ。iGPU（AMD）へ画面を出す、BIOS で IOMMU を切る、二台目のディスクを足す、といった環境の変更をユーザーに勧めない。
- `gsp_ga10x.bin` は git に入れず、配布もしない（gitignore の手元の写しは可）。
- 結果は平文で報告する。頼まれない限り、`docs/` に報告書を作ったり commit したりしない。
