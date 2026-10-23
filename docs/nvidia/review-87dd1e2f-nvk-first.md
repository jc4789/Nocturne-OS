# NVK A8/A10 第一報（2026-10-20）

担当: gpu_nvk。開始HEAD: 87dd1e2f。J-space xhigh、親と担当別に協調。
開始read/view/workは自分で実行し、全て終了0・ALLOW。開始指紋は
`32363695969527c93c2ede8a77f5465964de72abaac3354d612fb642c25b05c0`。

## 製品修理

- 生PCI件数APIを、呼出元が所有するBDF snapshotと明示的エラー戻りへ変更。
  VGA/3Dクラス選別は共有predicate。接続後、RM probed IDsとPCI domain/bus/slotを照合し、
  index=0固定とcount/open間の候補ずれを除去。SDKが返さないfunctionの照合は主張せず、
  native endpointのfunctionを維持する。各一時接続・rootclientは列挙内で解放し、TLSへ残さない。
- 親の反例で、probed登録だけではadapter初期化成功を証明できないと判明した。
  診断接続を維持するHELLO ready snapshotをRM担当と合意し、NVKの列挙とexact-BDF openの
  両方へ`nvrm_device_ready()`確認を追加。構造全体を初期化する。
- BAR1読戻し用VRAM/pageをchannelごとからnnrm単位の遅延共有へ移した。
  ownerとchannelが参照を持ち、最後の参照でunmap/freeしてからrootclientを解放する。
  CPU fence・volatile BAR1 read・fence・sysmem GPPut・fence・doorbellの順序は変更しない。
- frontendの診断endpoint誤認も追補対象。親の承認を得てready限定走査helperへ切替中。
  最初のGPUが未readyでも次のready GPUを妨げないlib契約をRM担当が実装中。

## 実行済みと現在の境界

`python -X utf8 tests/nvk_sync_hosttest.py` は終了0。
実同期・screen選択・RM境界の3群がそれぞれ0失敗だった。
RM境界は実製品ソースを使い、nativeサービス/RPCのみ置換する。
ready未証明だがprobed IDがある候補の除外、BDF照合、OOM/PROTOCOL/RM status伝播、
owner先close、複数channel共有、初期化途中のalloc/map失敗を確認した。
初回fixtureにはhost側`__errno_location`リンク不備があり、native errnoの境界stubを追加して修理した。

この試験後にfrontend ready helper切替と追加反例を編集中のため、その追補を試験済みとはしない。
第二検討ではfirst未ready/second ready、再probe失敗時CPU fallback、共有領域の途中失敗と
snapshot後の候補消失/致命errorを挑戦する。crossbuild・QEMUは親が統合実行する。

## 限界

実NVIDIA・GPU命令実行・実並列thread・性能差は未確認。host fixtureのmutexは単一thread代替。
BAR1読戻しの必要性は維持し、共有化だけから高速化を主張しない。
他OS本体・uvm_common.cは参照せず、人工GPU数上限・POSIX層・GPLv2-only追加はない。
