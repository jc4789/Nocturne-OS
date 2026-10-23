# RM境界の修理・第一検討（2026-10-20）

担当は `gpu_rm`。既存の `build.ps1`、`scripts/build.py`、`docs/nvidia/LEDGER.md` は編集していない。
私は接続の認可と共有写像の権利を保ち、次に旧静的リンク済みアプリとの通信互換を反例として検討する。

## 製品変更

- `rm_api.c`／`os.c`：接続ごとに一意かつ再利用しない32bit RM主体を割当て、要求とfile cleanupの間だけTLSへ設定。daemonのPIDを別アプリの共通PIDとして渡さない。番号の表現幅が尽きれば拒否する。同一アプリの別接続も暗黙には同一主体としない、安全側の分離である。
- `rm_api.c`／`os_pci.c`／`libnvrm.c`：RMのRO/RW保護をnative viewの不変最大権利と実map accessへ接続。送付viewをRM側で保持し、unmap／close／GPU revokeで派生viewを含む既存mapを失効。新規map禁止と「全mapの失効完了」を区別した。
- `os_pci.c`：有効memory BARをPCIの64bit幅に従ってRM apertureへ割当てる。register／doorbell／instance窓はUC、通常framebuffer apertureかつprefetchable BARだけWCを許す。16個の固定range制限を除き、任意数の隣接rangeを結合する。真に非連続なrangeはnative shared viewの能力外として拒否する。
- `nvrm_serve.c`／`libnvrm.c`：transport、RM関数、個別引数statusを分離し、RM関数エラーを成功扱いしない。引数の固定1024byte ceilingとhelloの固定1GiB ceilingも除き、実memoryとwire表現幅で検査する。
- BDF別endpointと `nvrm_connect_device(N_PCI_TARGET(...))` を追加。既存C API `nvrm_connect()` は動的なPCI列挙から使用可能なGPUを探す。
- `os.c`：wait queueをconsume-onceから全waiter向けone-shot completionへ修理。現在および後続waiterが通知を奪い合わない。

## 実行した確認

`python tests/nvrm_dma_hosttest.py --output-dir build/gpu-repair-20261020`

- 第1回は存在しないnativeエラー名とhost fixtureのNvP64型不一致でcompile失敗。製品のエラー名とfixture型を修理した。
- 第2回はnative channel mockのflags型不一致でcompile失敗。fixtureを現行native宣言へ合わせた。
- 第3回はDMA、RM契約、actual client/server framingの3群がすべて0失敗。
- 証拠：`build/gpu-repair-20261020/rm-hosttest-first.log`、`rm-hosttest-second.log`、`rm-hosttest-third.log`。
- 検査対象は実OS adaptationとframingのコードだが、native handle・RM dispatcherはmock。PCI、GPU、GSP、実キャッシュ、実並行待機やNVIDIA認可のend-to-end成功の証拠ではない。

## 未確定と次の一手

この時点のwire拡張はProtocol2であり、公開C API互換だけでは旧 `/data/bin` の静的リンク済みアプリのbinary互換を証明しない。親レビューの指摘を受け、第二検討では旧header幅・Protocol1とdefault endpointを維持しながらRMエラーとRO保護を弱めない形式へ修理する。

全体cross build、梱包、QEMU edu、画像・GUI操作は親担当。NVIDIA実機・Hyper-Vは未確認。上流RM本体と禁止された `uvm_common.c` は編集・利用していない。
