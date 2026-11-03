# WebAssembly 構築・媒体の最終確認

担当：`wasm_build`。2026-10-14。製品ソース固定後の初回報告。

## 製品への統合

- 指定された WABT checkout の revision `1e9fffd80efa901b3e4708d4ba1f0df972e63518`、18 source、必要ヘッダーを保存し、既存 `scripts/build_cxx.py` で Nocturne 向けに構築する。ライセンスは Apache-2.0。外部 checkout を改変しない。
- `Makefile` は bridge、C++ ABI、必要な libc++ と WABT の個別 object を既存の遅延抽出へ統合。同じ一覧を guest の `libc.a` へ渡す。一般の C アプリや TCC 本体に無関係な Wasm 実装を全量リンクしない。
- ブラウザー・Worker の生成元、WABT archive、C++ runtime、ライセンス、sysroot、initrd の依存を接続している。
- Memory/Table の実確保失敗、中断 callback、Memory/Table/Global の変更通知、安定参照を持つ動的 call frame を WABT の既存責務内へ統合した。API と CSP 接続の受入は native/root 担当の証拠と区別する。

## 個別 object 欠損の修理

Makefile の archive と生成 object を grouped target `&:` へ統合し、以前の `@:` だけの object rule を撤去した。使用する正規ツールは GNU Make 4.4.1。

`build/wabt/utf8.cc.o` 一つだけを build 内で退避改名し、`-j4` でその object と archive を同時要求した。コンパイルは一回、終了値 0、再生成 object は退避前と SHA256 一致。直後の dry-run は両 target が最新で recipe を実行しなかった。退避分は確認後に回収した。

証拠：`build/wasm-missing-object-rebuild.log`、`build/wasm-missing-object-dryrun.log`。既存 builder の再利用判定は mtime と存在に基づき、内容 hash cache とは主張しない。

## 最新四媒体の一回の照合

root の `build/wasm-full-build-csp.log` による終了値 0 の全体構築後、媒体の新規生成・再構築・VM 起動なしで読み取り専用の照合を行った。結果は小さな `build/wasm-final-artifacts.log` に保存。

- initrd 内の browser、browserjsworker、tcc、guest libc.a は現在の構築物と SHA256 一致。
- initrd 内の WABT LICENSE、UPSTREAM.json、libc++ LICENSE は保存元と SHA256 一致。
- IMG の実 FAT 内 kernel/initrd は最新の `build/kernel.elf` と `build/initrd.tar` に一致。
- `qemu-img compare` により IMG と VHD、IMG と VHDX が両方同一内容。
- ISO の実 extent から読み出した kernel/initrd も最新の構築物に一致。
- 四媒体のサイズ・SHA256 を記録。四媒体は initrd 以後に生成され、関連する製品ソース・WABT ヘッダー・ABI・math・QuickJS は initrd より新しくない。

これにより、現在の IMG/VHD/VHDX/ISO の payload 不一致は検出していない。Hyper-V 実機の起動・操作成功を意味しない。

## 証拠の境界

担当は実サイトの QEMU 画面を受入担当として検証していない。主担当の実ゲスト API 検査、指定三サイトの実画面・操作・console 結果は別の証拠である。CNN の menu や表示の残件を、媒体一致・リンク成功によって解消済みにしない。
