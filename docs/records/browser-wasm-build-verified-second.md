# WebAssembly 構築・媒体の反例再確認

担当：`wasm_build`。2026-10-14。最終初回報告を保存したまま行う第二考察。

## 反例：archive があっても個別 member が残っているとは限らない

旧 Makefile の `object: archive` と `@:` だけの rule は、archive を残して object 一つを失うと再生成できなかった。通常の全体 build 成功から、この復旧経路を推論できない。

修理は archive と object 群の grouped target 化であり、常時 phony builder や別スクリプトを増やしていない。実際に一つを退避して、並列に object/archive を要求する反例を一回実行した。builder は必要な UTF-8 object だけを一回構築し、復旧後の dry-run は追加作業なしだった。複数の独立した make プロセスが同じ生成物を同時更新することまで、grouped target が排他するとは主張しない。

## 反例：GC 付き host link と guest TCC link は同じではない

初期の GC 無しリンクで `__next_prime`、次に `std::exception` 基底 destructor の不足を確認した。必要な upstream `hash.cpp`/`memory.cpp` と root の既存 ABI 修理で解決し、独立 GC 無しリンクは終了値 0 となった。これは静的な依存解決の証拠であり、TCC の relocation、archive 抽出、実行は主担当の guest 検査と分けて扱う。

## 反例：四媒体の存在・mtime だけでは最新内容を証明しない

最終照合では四ファイルが存在することだけを確認していない。IMG 内の kernel/initrd、initrd 内の browser/worker/TCC/libc.a/ライセンス、ISO の実 extent を保存元と比較し、VHD/VHDX は RAW IMG と論理内容を比較した。この照合で current payload の一致を確認できた。一方、媒体 hash や payload 一致は、GUI 応答性、サイト作者の Wasm 実行、音声聴取、CSP の振舞い、Hyper-V 実機の成功を証明しない。

## 残る範囲

WABT の section/function と既存実行バッチの中断点は接続できるが、単一の極端に大きい関数・すべての OOM・すべての参照破棄順まで実証していない。Memory/Table の確保失敗は実 allocation の失敗へ修理したが、すべての C++ コンテナの OOM を JS 例外へ変換したとは主張しない。Core 3.0 全面適合や重い実サイトの全操作成功として引き渡さない。

## 現在の結論

ビルド依存、個別 object の復旧、ライセンス梱包、最新四媒体の payload 一致は確認した。サイト・動画・性能・Hyper-V の別層に未達がある場合、その未達はそのまま最終報告へ残す。
