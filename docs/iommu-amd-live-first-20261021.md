# AMD実観測後の幅判定修理・新第一検討（2026-10-21）

## 範囲・保全

- 担当ID `amd` の新しい修理cycleの第一報告。既存の `docs/iommu-amd-first-20261021.md` と `docs/iommu-amd-second-20261021.md` は変更していない。旧報告のソースhashと検証はその時点の履歴であり、今回の変更後へ流用しない。
- 今回の製品編集は `kernel/src/dev/iommu_amd.c` のprobe幅判定、検査編集は `tests/iommu_amd_hosttest.c` のみ。既存差分、共通層、Intel、PCI、ABI、生成媒体は編集していない。
- J-space xhighの既存 `amd` を継続し、自身のresume/pulse、同期mapのviewと `ALLOW work` を確認してから編集した。並行所有者の変更でgateを反復していない。Intel開始gateのための編集・controller更新保留を守り、解除後に本報告を作成した。
- 重いbuild、媒体生成、QEMU、Hyper-V、通常data媒体は本担当では実行・操作していない。他OS、Linux、QEMU本体、NVIDIA `uvm_common.c` を参照していない。

## 症状と一次仕様の再判定

主担当の実QEMU観測では、Cap+10h=`0x00182840` がVA48/PA40なのに、IVinfo=`0x00002801` のVA0によってprobeが拒否された。`build/iommu-repair-20261021/amd-diagnose-serial.log` には `reports reserved address widths` が残る。これらの実register値と画像の観測者は主担当であり、本担当がVMを操作・目視したという意味ではない。

一次資料は抽出済み `build/iommu-amd-spec-48882-3.10.txt`（AMD 48882-PUB、Rev 3.10、2025年2月）。§3.2のCapability+10h（pp203–204）は実VAの定義値を32/40/48/64、実PAを40/48/52とし、それ以外をreservedとする。Table86（p287）はIVinfoの各幅を同じ定義へ参照させる共通情報である。

**仕様はVA0を有効値にしていない。** また「壊れたIVinfoを無視しなければならない」という規範を見つけたわけでもない。修理判断は、共通要約の不定義fieldを能力の上限として使わず、列挙済み全unitの定義済み実能力から安全な共通幅を求める汎用probe方針である。正規の実能力まで一律拒否する必要はないと判断した。ファームウェア要約そのものが仕様適合になった、または実機動作を証明した、という結論ではない。

## 製品変更

1. VA/PAの定義値判定を同じファイル内の小さなpredicateにまとめ、実registerと要約で同じ定義を使う。
2. 有効なIVinfoの幅は従来どおり追加の上限とする。欠損・予約fieldだけは上限根拠にしない。初期値の64/52は仕様で定義された最大幅であり、欠損unitの能力を補う値ではない。
3. 各unitではCap+10hから読んだVA/PAを必ず検証し、両方とも定義済みの場合だけ全体の最小値へ反映する。実VA0/PA0やreserved値を有効な要約で補わない。
4. PCI functionを取得できないunitの既存fallbackは、有効なIVinfoのVA/PAが両方ある場合だけ成立する。一つでも欠損・予約なら拒否する。先頭unitの実能力だけで後続の未知unitを推測しない。
5. HATDis、EFR/HATSのreserved拒否、ファームウェアEFR優先、coverage/alias、page-table深さによる最終幅、確保後の物理幅確認は変更していない。特定エミュレータ、vendor/device ID、既知のVA/PA組合せによる分岐は追加していない。

## 反例と軽量確認

- ホストモデルへ、要約と実能力を独立して入力する検査と、二つのPCI functionに異なる実能力を返すモデルを追加した。製品Cを直接includeし、製品probeを呼ぶ。
- 製品修理前は **110判定中5失敗、終了1**（`build/iommu-amd-live-before.log`）。VA欠損、両幅欠損、予約VA、予約PAと有効VAのfield別扱い、二unit最小化の正例が拒否された。元の失敗logは保存した。
- 修理後は **113判定、失敗0、終了0**（`build/iommu-amd-live-after.log`）。追加のMMIO map成功確認にも到達するため判定数が変わる。113件の独立実機試験ではない。
- 正例は要約VA0＋実VA48/PA40、両要約0、有効要約だけのfield別上限、有効要約と実能力の双方の縮小、複数unitの最小化。負例は実VA0/PA0/PA41、機能なし＋片方の要約0、後続unitのreserved VA、後続機能なし＋欠損要約。既存の実reserved VA拒否と有効legacy IVRSも維持した。
- 最終host command: `./tools/msys64/ucrt64/bin/clang.exe -std=gnu11 -O1 -g -Wall -Wextra -Wno-format -Wno-unused-function -Wno-unused-parameter -ffunction-sections -fdata-sections -fsanitize=undefined -fsanitize-trap=all -Ikernel/src -Icommon tests/iommu_amd_hosttest.c "-Wl,--gc-sections" -o build/iommu-amd-live-hosttest.exe`。compile logは `build/iommu-amd-live-compile.log`、警告なし。実行でもUBtrapは発生しなかった。
- 製品targetはMakefileのkernel flagsに対応する `clang --target=x86_64-unknown-none-elf ... -Wall -Wextra -Werror ... -fsyntax-only kernel/src/dev/iommu_amd.c` で終了0（`build/iommu-amd-live-syntax.log`）。host専用のformat警告抑制は製品targetに持ち込んでいない。
- `git diff --check -- kernel/src/dev/iommu_amd.c tests/iommu_amd_hosttest.c` は終了0。
- 準備中の未実行も区別する。最初のhost commandは引用していないlinker引数のコンマでshell解析に失敗し、引用で訂正した。sandbox内から既存MinGWへのアクセスは拒否され、許可された `require_escalated` で同じ既存compilerを実行した。修理前compileにはLP64製品とLLP64ホストの既存format警告17件が出たが、host stubはformatを実行しない。最終hostではこの差だけを抑え、製品では `-Werror` で別に確認した。

## 共通層との接続・未確認

- 最新の `iommu.c` / `iommu.h` / `pm.c` を読取り確認した。IVRS/DMAR宣言があるのにbackendを取得できない場合に `restore_failed` を保持し、`iommu_power_ready()` をS3/hibernate開始前の `sleep_blocker()` へ接続する主担当の変更がある。本担当のAMD幅修理が拒否すべき実reserved値まで受理することで、このfail-stopを迂回する変更はしていない。
- この共通PM変更の確認はソース読取りだけで、rootが実行している共通host検査や別の割込み移行境界を本担当の成功として扱わない。それらの統合と独立受入は未完了。
- 本モデルはDMA、CPU/fabric coherency、実割込み、性能、S3/hibernate、NVIDIAの実機動作を証明しない。修理後の実QEMU EDU、媒体格納、実画面、Hyper-V/実機は主担当の担当で、本報告時点では本担当の未検証。
- 次は新たな別委任による第二検討。本報告提出後は本担当の製品・検査・docs・controllerを静止する。主担当は本報告と差分を独立確認し、最新媒体の単一QEMU結果と実機未確認を分離して判断する。
