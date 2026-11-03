# NVK/Mesa修理 第一報（2026-10-20）

担当: gpu_nvk。監査3・7・8・9・10、およびRM担当と契約した11のNVK側を修理した。

## 製品変更

- `nvkmd_nocturne_rm.c`: channelごとにRMのVRAMページを確保・CPU写像し、CPU fence→volatile BAR1読戻し→CPU fence→sysmem GPPut→fence→doorbellの順で公開する。channel解放後に読戻し写像・割当を解放する。CPU順序とPCI到達確認を区別した。
- 同ファイルとpdev/dev: グローバル単一RM接続を廃止。動的pcilistによるNVIDIA表示機能の索引をBDF endpointへ結び、pdevにPCI targetを保存しdev再open時の接続先を固定する。RM担当の`nvrm_connect_device`を使用する。
- `nvkmd_nocturne_sync.c`: signal追加時に完了済みpointと既到達値以下の参照を回収する。同一fence・同一seqnoの重複だけを集約する。異なるseqnoの旧値待機は維持し、追加確保失敗を返す。CPU signalで包含された点も直ちに解放する。
- `nocturne_gl.c`: CPU/GPU screenを別々にプロセス寿命で保持する。新context生成時にreadyなRM endpointが現れればZinkを再試行し、既存CPU contextは元screenとCPU clear方式を保持する。
- `js_webgl.c`: `failIfMajorPerformanceCaveat`の無条件拒否を除去し、実際に選択したscreenの`caps.accelerated`によるCPU描画caveatを評価する。GPUだから必ず速いとは判定しない。
- `nvk_physical_device.h`・`nvk_instance.c`: Nocturneをheadless WSIの有効条件へ含め、surface依存広告・既存entrypointテーブル・WSI初期化/終了の条件を一致させた。

## 検証

- `python tests/nvk_sync_hosttest.py`: 終了0。実製品sync源で1万回の完了signal追加、参照数、旧seqno保持、重複集約、別channel高値完了、CPU signal、binary置換を検査。実frontend源で未ready CPU→ready GPU、旧screen保持、CPU/GPU初期clear分離とcaveatを検査。ネイティブサービスとscreen生成はmock。
- `scripts/build.py -j 2`で変更9対象のcrossbuild: 終了0。ログ`build/gpu-repair-20261020/nvk-crossbuild.log`。最終rm/sync変更の追加2対象: 終了0、`nvk-crossbuild-final.log`。
- 初回buildはコンパイル前にMSYS2のサンドボックス起動が拒否された。権限昇格で再実行。元エラーは`nvk-crossbuild-sandbox-failure.log`に保全。
- `git diff --check`で対象差分の空白問題なし。
- 基準`build/gpu-repair-20261020/baseline-browser-open.png`を自身で開き、about:homeの一本のアドレス欄、検索欄、3列カードとスクロールバーを目視した。これは変更前画面であり、修理後GUI受入ではない。

## 未検証と引継ぎ

`tests/nvk_contracttest.c`を追加。親が既存native link規則へ統合し、全体の差分buildと一台QEMUでheadless instance/surface・GL二context画素読み戻しを確認する予定。NVIDIA実機でBAR1排出、GSP、複数GPU、GPU画素・速度、ready後の実GPU昇格は未実証。QEMUで代用しない。

J-spaceの子readは2回lock timeout。親のroot/RM work gateがALLOW後、親の指示に従い現mapを実読して編集開始した。子個別repo view/work機械gateは未実施であり、PASSとは主張しない。開始時のbuild.ps1・scripts/build.py・LEDGERと担当外変更は保全した。
