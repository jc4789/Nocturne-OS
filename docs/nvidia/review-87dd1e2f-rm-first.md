# 87dd1e2f再監査・RM第一検討

開始HEADは `87dd1e2f`。私はRM契約と既存隔離を両立し、次に未知HELLO能力・部分成功のAPI別返却・非request寿命を反例で再検討する。開始read/view/workは終了0・ALLOW、開始fingerprintは `32363695969527c93c2ede8a77f5465964de72abaac3354d612fb642c25b05c0`。

## 製品修理

- **A1**：正本 `nvos.h` がRO/WO要求時のRW写像を許すことを確認し、WRITEABLE単独をnative RWへ受理した。READABLEの不変RO最大権利は保持する。
- **A2**：RM関数エラーでも元の引数長を保持し、native拒否と区別して返す。libの検証済み応答copybackと、allocのhObjectNew／arena、controlのarenaまで接続した。RM関数errorは個別statusで上書きしない。旧48byte／Protocol1と旧client用の負statusは保持する。
- **A3**：一時的なGPU写像失効を永久shared revokeで代用せず、`NV_ERR_NOT_SUPPORTED`・状態無変更とする。safe_to_mmapは独立状態、all-revokedは未達を示すfalse。通常unmap／free／closeの永久取消は残した。RTD3を対応済みと偽装しない。
- **A4**：合成counterを削除。nativeの送信時認証済みsenderをHELLOへbindし、全requestで同一appを要求。endpoint譲渡による親主体偽装を拒否し、同一appの別connectionは同じRM主体になる。
- **A7接続**：親の集合view helperへ公開処理を委譲し、takeした全exitで動的contextをrelease。正常MAPかつ個別status成功だけを直後のtakeまで保持し、その他はioctl終端、file close前後でdropする。
- **A8**：display選別を共有predicate（10de/03、subclass00又は02、progif00）へ統一。native失敗を区別するconnect status APIを追加。PROBED IDだけでreadyを推論せず、HELLOのpresence＋実adapter_ready snapshotと `nvrm_device_ready()` を追加した。診断接続は成功し、旧serverのpage-aligned arena echoはready未証明となる。
- **A9**：token確保からrequest／safe条件を外し、非request tokenをorphan所有リストへ保持する。NULLは実calloc失敗だけであり、合法な非requestの登録をENOMEM扱いしない。

## focused確認

`python -X utf8 tests/nvrm_dma_hosttest.py --output-dir build/nvidia-review-87dd1e2f`

- 初回 `rm-host-first.log` はstatus APIが使うnative errnoのhost fixture未定義でlink失敗。fixtureへ正規 `__errno_location` を追加した。
- `rm-host-fixed.log`：実7TUの個別target構文確認とhost3群が0失敗。
- `rm-host-ready.log`：診断／旧HELLO echo、API別partial出力、sender変化拒否、WO／RO、非request／OOM、失敗／非MAP時context dropを加えて3群0失敗。
- source inclusion型のfixtureは実OS adaptationとtransportをcompileするが、nativeサービスとRM dispatcherはmock。実NVIDIAの認可・GSP・DMA・cache・RTD3の証明ではない。親の最新DMA試験追加分と正規統合はこの報告時点で未実行。

controllerは親の予約枠で実行する。重いbuild・VM・媒体操作は本担当では行っていない。virgl本体は変更していない。
