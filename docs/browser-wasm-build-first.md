# WebAssembly ビルド担当：初回報告

担当：`wasm_build`。2026-10-14。

## 保存した製品変更

- `scripts/build_cxx.py`：指定された外部 WABT checkout の固定 revision `1e9fffd80efa901b3e4708d4ba1f0df972e63518` から、検証・バイナリー解析・インタープリターに必要な 18 source とヘッダーを取り込む。外部 checkout は変更しない。C は clang、C++ は既存の Nocturne C++20/libc++ プロファイルでクロスコンパイルする。
- `third_party/wabt`：Apache-2.0 のライセンス、由来、選択した実装を保存。Nocturne 固有変更は `UPSTREAM.json` に列挙した。CLI、WAST パーサー、WASI 実装は構築しない。
- `ports/wabt/include/wabt/config.h`：x86-64、clang、既存 libc の実体に合わせた設定。`common.cc` のファイル型検査は Nocturne の `n_stat` と `N_FT_DIR` を使う。
- `Makefile`：C++ bridge、WABT、必要な libc++/ABI の個別 object を既存の遅延リンク対象へ統合。通常の C アプリと TCC 本体へ不要な C++ 実装を全量リンクしない。同じ object 一覧を in-OS `libc.a` に渡す。WABT ライセンスを initrd に含め、worker 生成元の Wasm 依存を追加した。
- インタープリターの既存命令バッチでブラウザー所有の中断 callback を呼ぶ。解析時にも section/function 境界の中断 callback を追加。毎 byte の時計 syscall は増やしていない。
- call frame は固定の capacity 拒否ではなく、現在命令位置の参照を維持する動的 `deque` に変更。
- Memory/Table の格納は、実際の `realloc` 失敗を返す共有 `FallibleBuffer` に変更。失敗時に旧内容・サイズ・上限を保ち、factory 失敗は空参照、instance 作成失敗は trap へ接続。表現可能性検査は `PTRDIFF_MAX / sizeof(T)` が根拠で、サイト固有の容量枠ではない。
- Memory 成長、Table の変更範囲、Global の更新を bridge へ通知する hook を追加。JS anchor graph の実装自体は native 担当の責務である。

## 実行した確認と範囲

1. `python -X utf8 scripts/build_cxx.py wabt`：WABT の対象 C/C++ を Nocturne ELF 向けにクロスコンパイルし archive 作成成功。
2. `python -X utf8 scripts/build.py -j 4 --log build/wasm-link-build.log build/root/bin/browser build/root/bin/browserjsworker build/root/bin/tcc`：初回統合リンク成功。
3. 参照寿命・失敗回復・callback の修正後、同じ製品対象を `build/wasm-link-final.log` に再構築し成功。ただし最後の `PTRDIFF_MAX` 検査と、その後の native 担当の接続は root の最新全体ビルドで確認する。
4. section GC を無効にした独立 `ld.lld` リンクを実行。最初は `__next_prime`、次は `std::exception` 基底 destructor が不足した。前者は upstream `hash.cpp`、shared pointer の依存は `memory.cpp`、後者は root の既存 `cxxabi.cpp` 修理で解決。`build/wabt/browser-no-gc` のリンクは exit 0。これは TCC の実実行成功を意味しない。

## 引渡し境界

担当は実 QEMU 画面・実サイト操作・console を検証していない。最新の百度百科、指定 YouTube 動画、CNN の受入、in-OS TCC での JS/Wasm 実行、IMG/VHD/VHDX/ISO の更新は root が担当する。初回リンク成功を、後の未試験ソースやサイト受入へ拡張しない。
