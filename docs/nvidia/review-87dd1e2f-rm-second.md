# 87dd1e2f再監査・RM第二検討

第一報は保持した。親・NVKからの独立した反例を受けて、公開APIまでの境界を再検討した。

## 反例と変更

1. **PROBED IDはreadyではない。** HELLOで実adapter初期化結果を明示する。presence／ready片方だけ、未知bitだけ、旧serverのpage-aligned arena echoはいずれもreadyとしない。既知presence＋readyに未知の追加能力bitがある場合は、既知の明示readyを保つ。診断接続は成功したまま。
2. **最初のdaemonだけでは後続GPUを発見できない。** `nvrm_connect_ready()` を追加し、動的PCI走査を既存defaultと共通化した。first診断／second readyで、defaultは診断接続、ready専用は後者を返す。NVKとMesa frontendはこの契約を使用するよう担当へ渡した。
3. **transport修理だけではallocのhandleが失われる。** 検証済みreplyがある場合はRM関数errorでもallocのhandle＋arena、controlのarenaをcopybackする。freeはdispatcher errorを個別statusで隠さず、dispatcher成功なら個別statusを返す。partial MAP失敗も返されたlinear addressを使ってRMへ戻す。破損replyでは出力をコピーしない。
4. **非request tokenをfile寿命へ誤所属させない。** orphan tokenは別fileのcloseを跨いで保持し、RM private destructorで回収する。safe=falseでもtoken記録自体は可能で、native公開の可否とOOMを分ける。
5. **動的mapping contextを次requestまで残さない。** 正常MAPかつ個別status成功以外はioctl終端でdropし、file close前後もdropする。親の集合viewへ渡したcontextは成功・属性拒否・公開失敗の全exitでreleaseする。temporary revokeはNOT_SUPPORTED・既存mapとsafe状態の双方を不変とし、通常unmap/free/closeの永久取消を維持した。

## 実行結果

- `rm-host-second.log`：実7TU target構文とhost3群が0失敗。親のsysmem page-offset／丸め／末端拒否追加も含む。
- **最終 `build/nvidia-review-87dd1e2f/rm-host-api-lifetime.log`：7TU確認と3群が0失敗。** API別copyback／free優先順位／partial map cleanup、HELLO能力matrix、複数GPU ready選択、orphanのfile非依存寿命、TLS contextの失敗・非MAP終端を確認した。
- 担当差分の `git diff --check` は成功。初回host linkのnative errno fixture不足は第一報に失敗として保持した。

これらは実adaptation／client/serverコードのfocused検査だが、native handle・RM dispatcher・認証済みsenderの供給はfixtureである。kernelのsend-time認証、集合viewの実PTE、RMの実NVIDIA隔離・GSP・DMA・cache・RTD3をmockから成功へ昇格しない。重いbuild・実QEMU・媒体整合・GUI目視は親の独立受入を待つ。virgl本体は本作業の対象外。

未対応の透明RTD3失効／復元を実装済みとは言わない。旧Protocol1／48byte、診断接続とdefault alias、RO最大権利、通常の永久取消は保全した。
