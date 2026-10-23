# 媒体・caption damage・限定 WPT の統合第一報

## 保存した製品変更

- Nocturne 固有 shared-page FD／map ABI と mapping／descriptor の独立所有権を既存 kernel／libc へ統合した。MSE decoded ARGB は二つの共有 raster slot と小さい v8 header で受け渡し、通常 pipe の raster 全量輸送を除いた。親子境界、read-only 権限、FD 再利用時の backing identity、seek／epoch／revision、mapping cleanup を維持する。
- 容量変更で退役した ring の occupied slot も送出 window に数え、古い busy ring を連続 resize で積み上げない。frame loan は共有 mapping から ordinary owned picture への一回の copy 後に返却する。YUV 転送への変更ではなく、共有 ARGB の実装。非 MSE の別 worker 転送は今回の変更範囲外。
- `nmedia_draw` は同寸法 row copy と pixel-center bilinear／SSE2・scalar tail を使い、clip／pitch／alpha／巨大座標の算術を保護する。最終版は 32.32 stepping で画素ごとの除算をしない。
- `web_avmedia_deadline` は実 audio DMA clock と次の authored PTS／再投入期限を使う。媒体 snapshot の coherent publication と通常 tick の接続、同寸法 video damage、intrinsic 寸法変更時の `layout_invalidate` を維持する。dirty だけを理由に旧 coherent picture を停止させない。
- caption rich cues と plain text の二つの setter は、snapshot invalidation を変えず、無分類 `paint_dirty` の代わりに共通 `doc_paint_full` を使う。helper は owner／frame parent に full damage を伝播し、先にある狭い video rect へ分類を下げない。`doc.c`／`webi.h`／`paint.c` の helper／damage 消費経路はこの変更の実質依存であり、最終 report の source binding に含める。

## 検証の区別

- 保存した媒体 host fixture は、実製品 scaler の 76,747 境界、追加 arithmetic／alpha／backing identity 3,480 境界、acquire ordering の identity-only 9 境界、current＋retired window の 13 境界を各 0 失敗で確認した。これらは同じコード／条件では再実行していない。
- 共有 page native guest は親が 25 検査／0 失敗を実施した。後の worker-only resize 修理は kernel が同じであり、guest 25/0 を反復せず区別した。syntax／host fixture は実サイトの代用ではない。
- 親の実サイト記録 `docs/browser-performance-20261012.md` では、YouTube 二回目の実 19 秒動画は author-play→EOS 19.252 秒（基準二回目 19.197 秒）。初回には約 40 秒かかる区間があり、全体改善・遅延解消とはしない。Shaka は実映像進行と full draw なしの video patch 区間を観測したが、ABR／source 寸法／時刻の異なる区間を一律 fps／速度へ換算しない。
- Shaka の spinner／controls と source publication の原因は未確定。readyState を固定、架空 audio clock、dirty による video snapshot 停止、サイト専用処理は追加していない。実音質／Hyper-V は未確認。

## 今回の限定 WPT

`tests/wptperformancetest.c` を既存 native `web_live_response`／`webnet` に接続した。提供 checkout `D:/Programes/cloned repos/wpt` の原本三 HTML と標準 harness 二ファイルは、受信 SHA256／長さを照合する。原 assert／CSS／JS／timeout と harness は変更しない。読み取り専用 capture-load observer だけを標準 completion callback へ登録し、各原 name／status／message／stack を記録する。

元 HEAD／五 hash／三 URL／syntax の設定失敗と訂正は `docs/browser-performance-wpt-20261013.md` に保存済み。第三候補の transition 依存ケースから通常 negative-margin extent へ選び直したのは **実行前** であり、未知 FAIL を除外した選択ではない。

主担当の一回の native guest は `build/browser-performance-wpt-guest-20261013/test-serial.log`。計三ページ／六原ケースで **五 PASS・一 FAIL、ページ二 PASS・一 FAIL**。全 hash 一致、harness status は三つとも 0、基盤失敗 0。最初の clip／negative-z-index ケースは原 JS の `container` named global 未解決で、hit assertion 前に失敗した。clip/hit の互換性をこの未到達ケースから PASS／FAIL と判定しない。原 FAIL はそのまま保存し、global alias 注入・fixture 改変・同じ成功五件の反復はしない。

実際の server は主担当による標準 `http.server`、host bind **127.0.0.1:8766**。第一報の `0.0.0.0` は準備例であり、実施条件は loopback 限定だった。guest は `http://10.0.2.2:8766/` から取得した。詳細原 JSON と再考は別の統合第二報へ保存する。

## 引き渡しと制約

製品 source は凍結。今回担当は runner／報告書以外を変更しておらず、VM／server／全 build を起動していない。主担当の原結果の独立 review と最終 map 同期後、この第一報と別第二報を媒体 source・caption helper 依存・runner／元 manifest／実 guest log に fresh bind する。Window named globals 全面対応や WPT 全体へ範囲を広げない。
