# RM境界の修理・第二検討（2026-10-20）

第一報 `repair-rm-first.md` は変更せず保存した。親レビューの二つの反例を採用し、製品を追加修理した。

## 反例1：公開C APIが同名でも旧バイナリーは接続できない

Protocol2／新header幅だけでは `/data/bin` に残る静的リンク済みアプリを壊す。そのため **Protocol1の48byte、field offset、従来default名を維持**する方式へ変更した。

- 通常応答で未使用だったhello用の2領域を `rm_status`／`access` とunion化。helloの入力を消す前に処理し、その後に応答用領域へ切り替える。
- RM関数失敗は負のtransport statusと正確なRM statusを両方送る。旧clientも失敗と認識し、新clientは正確なNV_STATUSを受け取る。通信不備と個別引数statusは別のまま。
- BDF別listenerに加え一つのdaemonが従来名aliasを持つ。他GPUはaliasの既存名エラーで終了せず、各BDF endpointを維持する。両listenerをeventsへ登録し、終了時に双方閉じる。
- 旧clientの「常にRW map」は、新しいRO capabilityに対してnative境界で拒否される。RO対応には再リンクが必要だが、互換性のためにROを弱めない。旧headerの実サイズとmap offsetはstatic assertionで照合した。

## 反例2：明示UNMAPだけでは暗黙FREEと重複mapを扱えない

整数hMemory／pLinearAddressだけでmapを照合すると、同じ物理addressの重複mapを誤って失効する。また、RMの親resource FREEは明示UNMAP escapeを通らずにuser mapを破棄できる。

- user mapごとの一意private tokenをRMに所有させる。system memoryは既存 `nv_alloc_user_mapping`／`nv_free_user_mapping`、BARは既存vendor OS境界のmap／unmapへ接続した。
- RMの明示／暗黙user-unmap callbackがそのtokenのnative viewだけをrevokeし、派生viewも失効させる。物理address一致や「FREEなら全map取消」で代用しない。
- client close時はRM cleanupより前にnativeアクセスを失効し、RMがVRAMを再利用する前の境界を保つ。RM cleanup後に残るtokenも回収する。
- 例外的vendor変更は **MIT** の `third_party/nvidia-open/src/nvidia/arch/nvalloc/unix/src/os.c` 一箇所だけ。`NVRM_NOCTURNE` 条件内のprivate lifetime hookであり、他OSコードは参照も取り込んでもいない。`nvrm.mk` はそのtranslation unit一個だけへdefineを追加し、全RMの再コンパイルを要求しない。

## 確認結果と範囲

`python tests/nvrm_dma_hosttest.py --output-dir build/gpu-repair-20261020`

- `rm-hosttest-compat.log` は、actual server全体をfixtureへ接続したことでnative stdoutとMinGWの相違が現れlink失敗。fixtureだけにnative stdout／fflushを定義して修理した。失敗ログは保存した。
- `rm-hosttest-compat-fixed.log`：旧header、RM失敗、RO、BDFとaliasの保持を含む3群が0失敗。
- `rm-hosttest-lifetime.log`：implicit user-unmap、同一物理addressの独立tokenを追加し3群が0失敗。
- **最終 `rm-hosttest-final.log`：3群とも0失敗。担当差分の `git diff --check` も成功。**
- DMA／BAR試験は実 `os_pci.c`、主体／lifetime／completion試験は実 `os.c` と `rm_api.c`、transport試験は実 `nvrm_serve.c` と `libnvrm.c` をcompileして実行する。ただしnative handle、条件変数、RM dispatcherはfixtureである。
- principalの分離／復元、再利用なし、32bit表現幅の枯渇拒否、RO access伝播、全revoke失敗時の非成功、新規map禁止との区別、mapping完了後のnative handle保持、alias衝突時のdaemon継続を確認した。
- `os_wake_up`後の複数waitが通知を消費しないことを確認したが、実native並行waiterのスケジューリングをこのfixtureで証明していない。

## 引渡しと残る実機境界

差分cross buildでは `build/nvrm/rm/arch/nvalloc/unix/src/os.o`、`nvrm-os`、`build/nvrm/libnvrm.o` と最新 `/bin/nvrm` が必要。最終梱包、QEMU edu、実画面の目視・GUI操作は親が担当する。

接続principalは認証済みnative peer PIDではなく、接続単位でfail-closedに分離する代替契約である。同一appの異なる接続も暗黙共有しない。任意のRM sharing経路のNVIDIA実機end-to-end受入、GSP、GPU DMA、実キャッシュ属性、複数GPU描画、Hyper-Vは未確認。

固定GPU数の上限は追加していない。真に非連続なBAR mappingとwrite-only CPU mappingはnative能力外として拒否する。既存のPCI claim取消やDMA隔離は置換していない。
