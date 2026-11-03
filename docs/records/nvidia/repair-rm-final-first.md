# RM最終整合・新しい第一検討（2026-10-20）

J-spaceの最新goal/coreを `gpu_rm` 自身でreadした。旧 `repair-rm-first.md`／`repair-rm-second.md` は保持する。

## 追加の実不具合と修理

親の正規統合buildで `rm_api.c` の `NV_PROTECT_*` が未宣言となった。include型のhost fixtureでは先に読んだ `os.c` の推移includeが不足を隠していた。

- `rm_api.c` に既存MITの正本 `<os/nv_memory_type.h>` を直接includeした。定数は複製していない。
- `tests/nvrm_dma_hosttest.py` は実OS/clientの7翻訳単位を個別に `--target=x86_64-unknown-none-elf -fsyntax-only` で確認してから、従来のhost3群を実行する。
- `build/gpu-repair-20261020/rm-hosttest-standalone.log`：単独7TU確認とhost3群が成功。統合後の不要な再試験は実行していない。

## 親の統合・実QEMU証拠を確認した範囲

- 初回統合の未宣言エラーは `integration-build.log` に残る。その後の `integration-build-fixed.log` はRMを再compileしたが、別のNVK試験link不足で停止した。この途中失敗を成功へ読み替えない。
- `integration-build-linked.log` は `/bin/nvrm` 等の正規link、`integration-build-pack.log` は最新initrdと640MiB IMGの正規梱包を記録する。親による修理後の統合成功として確認した。後者だけで全媒体・実機成功とはしない。
- `after-serial.log` の119–134行：QEMU eduへattachし、RMが非対応装置を拒否、driver終了後にclaimを再取得し、`nvrmtest: 0 failed`。これは実QEMUのRM境界確認だが、NVIDIA GPU成功の証拠ではない。
- 同ログのGPU blit末端画素は失敗のままであり、親が別途修理・再検証中。RMの成功でこの失敗を相殺しない。

RM主体分離、RO最大権利、精密なmap寿命token、BAR配置・cache、RMエラー、Protocol1／旧alias互換の実装内容は旧第二報と同じ。NVIDIA実機のGSP、DMA、描画、キャッシュ、複数GPU描画、Hyper-Vは未確認のまま。
