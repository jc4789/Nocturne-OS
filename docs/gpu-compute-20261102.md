# GPU の計算の道：GL も窓も使わないプログラム

2026-11-02 の調べもの。読んだのは今のコードと上流の公開情報で、実機では何も動かしていない。

依頼（原文）：

> what about programs that dont use gl or Vulkan, for gpu use. Ie programs that use/can use gpu compute for nonraster applications, like LLM serving and stuff.

## 結論

- Vulkan の計算を使うプログラムの道は、今の設計で塞がっていない。窓も GL も要らない。
- Vulkan も使わず GPU を直接動かすプログラムの公開の口は、今回の変更で無くなった。特権の半分（RM と UVM）は `nvrm` に残っている。
- CUDA の閉じたライブラリーは動かない。作った者が Nocturne 向けに出さない限り変わらない。Linux の ABI は足さない。

## プログラムの種類ごとの今

| 種類 | 例 | 今 | 足りないもの |
|---|---|---|---|
| Vulkan の計算 | llama.cpp の Vulkan backend | libc のローダーから像へ届く。窓は要らない。`drivertest` は窓なしのプロセスで instance まで作った。 | 実機で計算を走らせた記録。計算の試験。 |
| 自前の実行時で GPU を直接動かす | tinygrad の NV backend | `nvrm` の RM と UVM は接続で届く。見出しと `libnvrm` が SDK から消えた。 | 公開の口。 |
| OpenCL | rusticl | `third_party/mesa` に rusticl が無い。 | 取り込みと組み。 |
| CUDA | PyTorch の CUDA 版 | 動かない。 | 作った者の Nocturne 向けの配布。 |

## 根拠

### 塞がっていない所

- `nvrm` の接続は `n_service_listen` の名前で受ける（`ports/nvidia/nvrm_serve.c:297`）。送り手の身元を確かめ、普通の身元は管理者にならない（同 `:102`）。
- `nvrm_pci_is_gpu` は subclass 0x00 と 0x02 を受ける（`ports/nvidia/include/nvrm.h:33`）。画面を持たない装置も `nvrm` の対象になる。
- NVK は全部の GPU を数える（`nnrm_enumerate_gpus`、`nvkmd_nocturne_rm.c:549`）。同じ作り手の複数の GPU は一つの Vulkan から見える。動かしてはいない。
- NVK の申告：`VK_KHR_cooperative_matrix`（Turing 以降、`nvk_physical_device.c:128`、TU11x は機能を切る `:514`）、8・16 ビットの格納、`shader_float16_int8`、`buffer_device_address`、`shader_integer_dot_product`、`memory_budget`。
- llama.cpp の Vulkan backend は `VK_KHR_cooperative_matrix` の道を持つ。NVK で動くという記録は見つけていない。

### 今回の変更で狭くなった所

- `scripts/mksdk.sh` から `ports/nvidia/include/*.h` を外し、`LIBC_LINK` から `libnvrm` を外した。前は SDK のプログラムが `nvrm.h` を読み、`libc.a` の `libnvrm` を結合できた。
- `struct n_gpu_driver`（`common/gpu_abi.h:68`）は名前が GPU で、中身は Vulkan の三つと画面への直接の出力だけ。`ready` の説明も「描ける装置」。
- `user/libc/vulkan.c:19` の `driver` は一つで、`n_gpu_driver_get` は最初に見つけた像で止まる（`:45`）。作り手の違う GPU を一つのプロセスで使えない。

### 大きな計算で先に当たりそうな所

実機では確かめていない。

- 一回の確保の上限の申告は NVK の値の 2 GiB。VRAM の確保の連続性は RM の既定のまま。`VkBuffer` の一つは 4 GiB まで（`NVK_MAX_BUFFER_SIZE`）。
- VRAM の GPU の写しは全部 4 KiB のページ（`virt_map_locked` の `NVOS46_FLAGS_PAGE_SIZE_4KB`）。大きな写しは PTE の数と TLB の取り合いが増える。速さは測っていない。
- `ports/nvidia/README.md` の既知のリスク：RM へ大きなページでシステムのメモリーを頼むと、連続を強いるので失敗しうる。NVK は 4 KiB で頼むので、今はこの道を通らない。
- 普通のメモリーを GPU に直接見せる口は無い。HMM と ATS は無く（`ports/nvidia/README.md` の UVM の「無いもの」）、NVK は `VK_EXT_external_memory_host` を申告しない。重みは写す。
- NVK の queue family は GRAPHICS・COMPUTE・TRANSFER・SPARSE を合わせた一つで `queue_count = 1`、装置によっては転送だけの family がある（`nvk_physical_device.c:1713`）。計算だけの queue は無い。
- 移植の側（`nvkmd/nocturne`、`nvrm_serve.c`、`rm_api.c`、`nvk_image.c`）に timeslice と preemption の設定は無く、RM の既定のまま。長い計算と他の GPU の利用者の取り合いは確かめていない。
- NVK は 3D の級が無い装置を使わない（`nvkmd_nocturne_pdev.c:120`）。3D を持たない計算専用の装置は、`nvrm` が受けても Vulkan から見えない。
- UVM は `nvrm` の中にあるが、クライアントが無く、実機で動かしていない（`ports/nvidia/README.md` の UVM の節の「未確認」）。

## 案

カーネルは今のまま GPU を知らない。`nvrm` も変えない。変えるのは利用者側の像とプログラムの間の口。

1. **像の表を、口を名前で引く形にする。** `struct n_gpu_driver` を、装置の有無と記録と `interface(name, version)` に分ける。口は `"vulkan"`、`"scanout"`、`"nvidia.rm"`。口を足しても表の形は変わらない。`ready` は「使える装置があるか」にする。
2. **RM と UVM を `"nvidia.rm"` の口で公開する。** `libnvrm` は像の中の一つのままで、プログラムへ写しを結合しない。見出し（`nvrm.h`、使う RM の級と制御の見出し）を SDK へ戻す。RM の形は版ごとに変わるので、これは作った者に固有の口で、プログラムは `version` を確かめる。
3. **像を複数持てる形にする。** `nocturne_gpu.h` の取得を一覧の形にする。Vulkan のローダーが複数の像の装置をまとめる処理は、二つ目の作り手が来たときに書く。
4. **計算の試験を足す。** `tests/computetest.c`：窓なしで Vulkan の計算の pipeline を作り、バッファーの値を GPU で計算して読み戻す。GPU の無い機械では装置が無いと言う。

## しないこと

- Vulkan の上に Nocturne 独自の計算の API を重ねない。呼ぶ者が無く、一つの呼ぶ者のための包みになる。
- 動かない代役の CUDA を作らない。本物の実装を作るかどうかは `docs/rust-cuda-20261102.md`。
- Linux の ABI を足さない。

## 出典

- llama.cpp の `VK_KHR_cooperative_matrix`：https://github.com/ggml-org/llama.cpp/pull/10597
- Vulkan の機械学習の状況：https://www.phoronix.com/news/NVIDIA-Vulkan-AI-ML-Success
- tinygrad の NV backend：https://xl0.github.io/tinygrad-notes/misc_3.html
- tinygrad の Mesa NIR backend（NVK と NAK）：https://www.phoronix.com/news/Tinygrad-Mesa-NIR-Backend
