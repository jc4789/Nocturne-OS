# Windows ホスト限定：virgl fence 待機の修理

これは **Nocturne ゲストの POSIX 移植ではなく、QEMU のホスト側 DLL の限定 port** です。元の QEMU インストール、GPU driver、環境設定、既存 `/data`、Hyper-V VM は変更しません。既定の OS 起動は従来どおり CPU 描画、GPU は `gpu-enable` の明示指定時だけです。

## 固定 source と一行修理

- 公式 source：`https://chromium.googlesource.com/chromiumos/third_party/virglrenderer`
- 固定 revision：`2cb2065b6a0515c5accfa3e44bcb7ce57d2f9983`、source version **1.1.0**。任意の HEAD を使用しません。
- `angle-fence-flush.patch`：`glClientWaitSync(..., 0, timeout)` の flags を正規の `GL_SYNC_FLUSH_COMMANDS_BIT` へ変更する +1/-1 だけです。fence を外す、期限を延長する、ダミー描画を追加する修理ではありません。
- 同梱 ANGLE commit `890b5d8fa298` の `Fence11.cpp` は flags 無しを D3D11 の `DONOTFLUSH` へ写します。実 clear→readback 後の空 tail で 255～256 poll の遅延を再現、flush-bit 対照は追加描画なしで解決しました。256 という回数の非公開 driver 内部の所在までは未確定です。
- インストール済みの元 virgl DLL の exact source revision は不明です。そのため元 DLL との比較だけでなく、**同 revision・同 toolchain・同設定の未修正 DLL と修理 DLL** を比較します。

## 前提（手動準備、システム installer 不使用）

Windows、Python **3.14**、Git、ripgrep、repo 内 `tools/msys64/ucrt64` の GCC/G++ と epoxy 開発ファイルが必要です。元 QEMU は検証時 11.1.0 (`v11.1.0-12130-ge470268ff4`) でした。GPU 実試験にはその ANGLE/EGL/GLES DLL と対応 GPU が必要です。

repo を cwd にして、依存は workspace の build 内だけへ準備します。以下は通常 CLI です。PowerShell cmdlet は使用しません。

```text
tools/msys64/usr/bin/mkdir.exe -p build/virgl-tools
python -X utf8 -m pip install --no-cache-dir --target build/virgl-tools/python meson==1.12.1 ninja==1.13.2 PyYAML==6.0.3
curl.exe --fail --location --output build/virgl-tools/pkgconf.pkg.tar.zst https://mirror.msys2.org/mingw/ucrt64/mingw-w64-ucrt-x86_64-pkgconf-1~3.0.7-1-any.pkg.tar.zst
python -X utf8 ports/virgl/build_runtime.py --stage first --python-tools build/virgl-tools/python --pkgconf-archive build/virgl-tools/pkgconf.pkg.tar.zst
```

pkgconf package の必須 SHA256 は `74b6fe685d84ec6ea19b9ee285d752d122e31b6b93038f933627b05324d8f2a3`。版違い・hash 違いは拒否します。ビルダーは archive 内の必要 2 binary と COPYING だけを新規 stage に抽出します。pip は自動実行しません。

`build_runtime.py` の source 取得・構築順は次のとおりです。全コマンドと stdout/stderr は新規 stage のログに残ります。

1. 新規 `build/nocturne-virgl/<stage>/source` を `git init`、公式 origin を設定。
2. `git fetch --depth=1 origin <上記固定revision>`、`git checkout --detach FETCH_HEAD`、`rev-parse HEAD` と source version を厳密検査。
3. Meson release、`platforms=egl`、DRM renderers 空、tests/video/venus 無効、unstable APIs 有効、LTO 無効で構成。Ninja `-j 4` で未修正版を構築。
4. 同じ checkout に `git apply --check` と一行 patch を適用、`diff --numstat` が +1/-1 のみと検査。同じ build 設定で修理 DLL を構築。
5. 元 QEMU 本体・直下の DLL・COPYING を新規 `qemu-unmodified` と `qemu-fixed` へコピー。virgl DLL だけを各構築物へ置換。他ファイルの一致と元配置不変を全 SHA256 で確認。source、patch、COPYING、著作権通知と provenance を保持。

出力や source の変更は workspace の `build` 内に限定し、既存 stage を上書きしません。clean 後はこの追跡された patch・手順から別の新規 stage を作成します。通常の OS build／initrd／boot 媒体をこのビルダーは変更しません。試験枠を調整し、CPU 性能測定と重い host build／GPU VM を同時に走らせないでください。

## QEMU 選択と独立試験

専用コピーの `qemu-system-x86_64.exe` を検証 harness の `--qemu` に渡し、BIOS は元の `C:/Program Files/qemu/share` を `-L` で読み取り専用使用します。DLL だけを元インストールにコピーする運用は禁止です。

同じ boot/data の scratch snapshot を使い、WHPX／4CPU／2GiB／`-display egl-headless -vga std -device virtio-gpu-gl-pci` の同条件で未修正→修理版を直列比較します。`gpu-enable gpu-timing` を明示指定し、公開 OS 経路は次の autorun です（前者が期待失敗しても後者を実行するため別行）。

```text
tcc -run /data/tests/gputest.c --require-gpu --bench
tcc -run /data/tests/canvastest.c
sleep 10
```

要求：cold 成功だけでなく **通常 2 秒 clear／triangle／readback 検査**、公開 syscall の GPU 必須試験、入力拒否・画素・canary・fence/submission、Canvas 実 paint、同入力ベンチ、全 transaction 時間、所有 PID の終了、媒体と DLL の hash。process exit 0 や trace 数だけでは受入れません。未修正／元配置の host では能力撤回と CPU fallback の画素成功を保持します。

## 2026-10-07 の証拠と反証

旧固定 GPU kernel（SHA256 `e6d9ddd175ce0e1b693ec29bb891b64cba154c2b9c52146f34d85fc5b3edf3cd`）、同一 boot/data、同一 QEMU 本体・他 DLL で比較しました。

- 未修正：fence 約 4 秒、通常 2 秒検査で cap 0 へ撤回。GPU 必須 17 checks／期待どおり 2 失敗。CPU fallback の画素と Canvas 22 checks 成功。
- 一行修理：全 25 transaction 最大 30ms、通常検査とユーザー render 最大 24.090ms。GPU backend 1／caps 3、GPU 必須 **19 checks／0 失敗**、Canvas **22 checks／0 失敗、GPU submissions 2**。
- 同一 input hash `2f3d2d51`、400×400 三角形 3 回の upload／syscall／readback／copy を含む実 total は **GPU 47ms、CPU 6ms**。出力の丸め差は別 hash になるが双方画素・canary 成功。
- **実 3D 機能と通常期限は成立したが、CPU より高速という実用加速は未達**です。保存した公式 v11.1 source は fence を virtual timer で 10ms 後に再 poll しますが、実 binary が表示する exact commit の照合は未完了です。13～24ms の量子は実測であり、host timer 粒度や同期 readback が total の支配要因という説明は追加計測を要する推論です。新しい host／OS 統合版は再検証が必要です。

証拠：`build/nocturne-platform/graphics-stage2/guest-{unmodified,fixed}-public-whpx4` と `guest-comparison-summary.json`。初回 command-not-found の未実行失敗も別ディレクトリで保持し、成功へ数えていません。

Hyper-V の GPU／Enhanced Session、一般 GPU、WebGL、Canvas 全互換、ブラウザ全面高速化、日常 OS の完成はこの修理で証明していません。host source は MIT 系（個別ファイルの通知も維持）、QEMU は元の COPYING/COPYING.LIB に従います。取得 source と出典を build 内へ保持し、巨大な依存ツリーは repo に vendoring しません。
