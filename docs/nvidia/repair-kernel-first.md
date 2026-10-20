# GPU・native PCI修理 第一検討

2026-10-20。担当 `gpu_kernel`。開始時HEADは監査と同じ。開始済みの `build.ps1`、`scripts/build.py`、`docs/nvidia/LEDGER.md` の変更は保全した。

## 製品変更

- `SYS_SHARED_VIEW` の未使用第5引数で最大権利を縮小する `n_shared_view_access` を追加。従来の第5引数0は権利継承のまま。ROハンドルを再view・再mapしてもRWへ拡大できない。既存PTEのRO属性も維持する。
- viewの物理参照は既存のroot平坦化を維持し、取消系統は別の参照数付きscopeで表す。`n_shared_revoke` は発行appだけに許可し、派生viewを含む既存全app写像のPTEを失効、以後のmap/viewを拒否する。scope解放は反復式で、深いview連鎖をC再帰で解放しない。
- 非prefetchable BARのWC写像をkernelで拒否。REG/USERMODE用途の識別とUC強制はRM担当との契約。
- PCI IRQ無効化時に待機中のtakeを起こす。解放時はBME停止の読戻しとPCIe pendingの完了が確認できない場合にDMAを隔離する。claim時の早期BME操作は追加しない。
- virgl完了待ちはMSI-Xのnative wait queue、未提供時はschedulerの1ms解像度での待機へ変更。bootのstack無しidle継続だけは既存NVMe同様に割込み待機・scheduleする。serializeとtimeout後のDMA保持を維持する。
- 一般GPUのimage backingを要求寸法・実pmm資源で確保し、完了fence後に旧resourceをdetach/unrefして回収。capset2の実max_texture_2d_sizeとuint32 backing長を検査する。容量は縮小せず小さい後続frameで再確保しない。二つのresource名を交替再利用し、累積要求数でID枯渇しない。
- 旧1936-byte batch ABIはそのまま、新規の可変triangle listを追加。頂点DMAのPAGE_SIZEと実vertex strideから求める単位で分割し、clearは一度、順序を保ち、最後に読戻す。座標±16・wの小範囲拒否は除去し、w=0の未定義射影だけを拒否する。
- scheduler待機でユーザーが入力/出力をunmapできるようになるため、syscallは私有snapshotを保持し、完了後に出力先を再検査する。CPU fallbackとクリップを維持する。
- Canvasの512上限は実device capabilityへ置換。従来の小clear CPU選択は性能回帰を避けるため維持し、64閾値の性能上の根拠は今回測定していない。

## 現時点の証拠と限界

- 実ソースを読み、`clang --target=x86_64-unknown-none-elf -fsyntax-only` で変更kernel 3ファイルとuser/native/Canvas/pcitest/gputestを確認、終了0。
- 親が取得した `build/gpu-repair-20261020/baseline-browser-open.png` を自分でも開き、about:home本文・検索欄・カード・一本のアドレス欄を確認した。修理後の画面ではない。
- edu試験に、RO拡大拒否、派生view取消、独立owner map保全、別appの取消後既存map実fault、IRQ無限待機のdisable wakeを追加した。gputestには641×37・97三角形・旧範囲外座標の可変listを追加した。まだ修理媒体で実行していない。
- J-space `read --agent gpu_kernel` は実施。初回tree全体hashが作業を長く停止させたため、親の明示指示でmapを実読して実装へ進んだ。自分の機械的repo view/work gateは未実施であり、成功と主張しない。最終安定treeで親が直列refreshする。
- virgl capability offsetは公開virgl protocolの構造を独立に照合した。参照：[virgl_caps_v2](https://android.googlesource.com/platform/external/virglrenderer/+/68429e8e1106d0861d9f9f180583bd8381b8bf96/src/virgl_hw.h)。他OS本体ソースや `uvm_common.c` は参照していない。

## 次

親の差分ビルド・単一QEMU edu実行でnative権利とIRQ/DMAを確認する。virgl resource差替え、chunk境界、待機中GUI応答、timeout後寿命は実device経路の確認が必要。NVIDIA実機・Hyper-V実機の成功は未主張。
