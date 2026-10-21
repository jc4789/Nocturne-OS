# NVK A8/A10 第二検討（2026-10-20）

担当: gpu_nvk。第一報は変更せず保持。対象はNVK native RM境界と承認されたMesa frontend。

## 反例から変えた点

1. **probed IDはreadyの証拠ではない。** 親が示したprivate初期化中のID登録を受け、
   接続成功・probed/BDF一致だけという判断を撤回した。HELLOの明示readyを必須にする。
   診断daemonおよび旧serverのready未証明接続は、診断用には成功を保ち、NVKでは非公開。
   列挙とexact-BDF再openが同じ確認を通る。SDK構造のexcludedGpuIds/gpuFlagsも初期化する。
2. **最初のGPUが診断用、次がready。** frontendに単純なdefault-connect/ready検査を足す案は
   後者を見落とすため採用しない。RM担当の共有動的走査`nvrm_connect_ready()`を利用した。
   lib fixtureの.0321診断接続を飛ばし.0400へ到達するassertionを独立にソース確認した。
   frontend実ソースfixtureでは診断のみ→CPU保持、ready probe後Zink失敗→CPU保持、
   後の構築成功→GPU昇格、既存CPU context/caveat不変を確認した。
3. **snapshot後に最初のBDFが消える。** 配列内の次BDFは変わらず、消えたBDF再openだけが
   候補外になる。native OOM/PROTOCOL/CLOSEDとRM照合時のOOM/invalid-stateは
   空一覧へ弱めず失敗を伝播する。列挙内の一時接続/rootclientは各経路で閉じる。
4. **共有BAR1作成途中で失敗する。** 既存の部分応答契約ではerrorにも非zero handleが
   返り得るため、`rm_alloc`でそのhandleを解放してから返す製品修理を追加した。
   初回共有page作成失敗と、既存channelがある状態の各alloc/map失敗を注入し、
   参照が戻り、既存page/objectsが残り、残存channelのsubmitが成立することを確認した。

## 寿命と順序の再確認

- pageとmapはnnrmのmutex内で全成功後に一緒に公開し、失敗中のpointerは公開しない。
  一度公開したpointerは最後の参照まで不変。pageはchannelの私有bufferではない。
- channel生成はnnrm参照を取り、部分初期化失敗も通常destroyへ集約して参照を戻す。
  ownerを先closeしてもchannel参照がある間、RM接続と共有pageが生存する。
- 破棄順はRM channel free→私有buffer解放→channel参照返却。
  最後の参照だけが共有pageをunmap/freeし、その後usermode/rootclientを解放する。
- 同じchannelのsubmit/destroyは既存の外部同期が前提。異channelには別の参照があり、
  他channelの破棄で読戻し先は消えない。共有mutexは初期化/参照更新に使い、submitごとの
  RPCや大域lockを追加しない。初期化と同じownerの最終closeを競合させる使い方は許可しない。
- 全USERDがsysmemなので読戻し自体は削除しない。fenceだけをPCI posted drainと呼ばず、
  同deviceの生存BAR1からvolatile read完了後にGPPutを公開する元の順序を保つ。

## 検証と残る境界

最終変更後 `python -X utf8 tests/nvk_sync_hosttest.py` 終了0。
ログ: `build/nvk-review-87dd1e2f-host.log`。
同期・screen・実RM境界の3群は各0失敗。`git diff --check` も終了0。
host fixtureは単一threadのサービス代替であり、実thread競合・PCIバス排出・GPU実行の証拠ではない。

親へ渡す最小対象は`build/mesa/o/third_party/mesa/src/nouveau/vulkan/`以下の
`nvk_physical_device.c.o`、`nvkmd/nvkmd.c.o`、
`nvkmd/nocturne/nvkmd_nocturne_pdev.c.o`、`nvkmd/nocturne/nvkmd_nocturne_rm.c.o`、
および`build/mesa/o/ports/mesa/nocturne_gl.c.o`。公開header依存TUとlibは通常依存で再buildする。
crossbuild・梱包・QEMUは親の統合受入待ち。実NVIDIA、Hyper-V、速度差は未確認。
新しい人工的GPU上限や互換層は追加せず、uvm_common.c/他OS本体は参照していない。
