# 依頼項目と引渡し確認

## 製品へ入れた項目

1. MSE ARGB pipe本体を親子共有二重buffer＋136byte通知へ置換。共有FD／map寿命、seek／revision／resize流量を保全。ARGBを残した共有案でありYUV転送ではない。
2. 最近傍・pixel毎除算をSSE2双線形と32.32座標、等倍row copyへ置換。
3. 同寸法frameの動画patch／atomic damageとPTS／PCM起床を接続。寸法変更とunknown visualは正しい全描画へfallback。
4. computed style固定本体を文書slab、variable tailのみlazy arenaへ。retirement／adopted owner寿命を保全。
5. 無変更layout枝のprepare／intrinsic／inner／relative／overflow再用、overflowとextentの統合。absoluteのprivate clipとnative内部scroll入力も修理。
6. inline／WAAPI宣言ASTの内容・base付きcache。native CSS keyframesは既存sheet ASTを保全。
7. damage union／部分present／offscreen背景culling／box-owned stable stacking-order cache、文書BVH。これは全stacking contextの汎用raster texture cacheではない。
8. 通信のlazy最大32slot、CPU／RAM reserve、初回origin6とFetch5、parked込みadmission／mixed fairness／取消loan。
9. 文書寿命のowned text/fontchoice measurement cache、font世代／失効／LRU/OOM回収。

## 検証と失敗の保全

- 変更前と変更後に実YouTube／Shakaを独立並行VMで撮影し主担当が目視・操作。最終Shakaは内部下移動でfooter、上へ戻って動画を確認。指定動画ではない遷移後YouTube画像を成功へ数えない。
- 第一guest14成功／2失敗、製品回帰と旧fixture不整合を修理して失敗二群のみ2成功／0失敗。共有25／0、style150／0、paint14／0等は各snapshotに対するものとして保持。
- 新paint17／0、最新network callback0失敗、host scheduler26／0。元prefix総括失敗を保全し最新媒体でprefix修理後17PASS。
- 新scroll48中47成功／1誤期待。専用flow対照と同clip入力4／0でdoc40/private350を確認し期待値だけ訂正。旧47を反復していない。
- 原本WPT3ページ／6caseは5成功／1失敗、checksum一致、基盤失敗0。named reference未定義で最初のhit assertion未到達。全適合としない。
- 正規all最終媒体と17変更C objectの整合を確認。IMG／VHD／VHDX／ISOを保全。全fixture成功・全体速度改善とはしない。

## 状態と限界

全指定性能経路に実装を入れたが、ブラウザー全体の高速化／無回帰を全面保証する状態ではない。YouTubeの遷移後旧題名、長いJS/native処理、Shaka再生UI状態、実動画pause／seek／再open、聴取音質、Hyper-V実機は未解決または未確認。元失敗を消した成功・サイト専用処理・DOM置換・POSIX化はしていない。

今回所有のbuild／guest／QEMU／HTTP serverは終了。使い捨て299媒体等（論理約9.875GiB）を停止／絶対path／非symlink確認後に回収し、D空き108.34GiB。最新配布媒体、一基準boot、通常data、source、PNG／小log／metadataは保全。他作業・Hyper-Vの媒体とprocessは触れていない。

主報は `docs/browser-performance-20261012.md`。各担当第一／第二報、元guest serialとmetadata、QEMU原PNGを根拠とする。削除した媒体は再実行可能な保存物として扱わない。
