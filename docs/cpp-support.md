# NocturneのC++対応

既存のCコードと併用するC++20のホストクロスコンパイルを追加した。実物のLLVM libc++をNocturneのCライブラリーへ接続し、C++アプリを通常の静的ELFとして起動する。OS内のTinyCCは従来どおりC用である。

## ビルドと利用

既定のホストツールは`C:/Program Files/LLVM/bin`、ソースは`D:/Programes/cloned repos/llvm-project`。必要なら環境変数`NOCTURNE_LLVM_BIN`、`NOCTURNE_LLVM_SOURCE`で変更する。今回確認したclangは18.1.8。ソースの作業ツリーを変更せず、同じリポジトリーの`llvmorg-18.1.8`、コミット`3b5b5c1ec4a3095ab096dd780e84d7ab81f3d7ff`からlibc++を抽出する。最新の作業ツリーのlibc++は新しいclangを要求するため、ホストと一致する版を使う。

`user/apps/`へ`.cpp`を置くとMakefileが検出する。`nocturne.h`などの既存C公開ヘッダーをそのまま使える。

```sh
python -X utf8 scripts/build.py -j 8 cxx
python -X utf8 scripts/build.py -j 8 all
```

前者はC++アプリを構築し、後者は最新のkernel・initrdとIMG／VHD／VHDX／ISOを生成する。libc++のライセンスもinitrd内の`/usr/share/licenses/libcxx/LICENSE.TXT`へ含める。

OS内のシェルでは次を実行する。

```sh
cppdemo
cppdemo --self-test
```

GUIではEnterキーまたは追加ボタンで整数を追加し、Escで終了する。ビルド済みアプリを`/data/bin`へ配置する場合も、既存の永続領域とPATHを利用する。

## 対応範囲

C++20の言語機能、Cとのリンク、グローバルコンストラクター、終了時デストラクター、関数内staticのスレッド間初期化、C/C++共通の終了コールバック、new/deleteと配列・サイズ・整列・nothrowの各形式を追加した。割当と同期はNocturneのネイティブAPIを使う。Cコンパイルの設定は保全している。

libc++はヘッダーと`algorithm.cpp`、`string.cpp`、`vector.cpp`、`verbose_abort.cpp`を構築する初期構成。実アプリで`std::string`、`std::vector`、`std::unique_ptr`、`std::sort`、`std::accumulate`を確認した。標準ライブラリー全体の対応ではなく、`shared_ptr`／`weak_ptr`の実行時実装、iostream、例外、RTTI、標準スレッド、filesystem、locale、ワイド文字変換、random_device、タイムゾーンは未対応または無効である。メモリー不足時の通常newは、この例外なし構成では終了する。

ELFの`thread_local`はローダーが未対応なので、C++専用リンクスクリプトで拒否する。Nocturneの既存TLSと混同して実行されることを防ぐ。`tests/cxx_tls_probe.cpp`の負例はリンクに失敗し、明示的な未対応診断と`.tdata`のRELRO配置診断が出る。

## 2026-10-10の実行確認

隔離QEMU／WHPX、4 CPU、4096 MiB、1920×1080で確認した。

- `cxx/native-runtime`と、既存の`mem/malloc-stress`、`mem/heap-page-reclamation`、`tcc/run`、`tcc/errors`、`tcc/math-and-printf`、`gui/window-and-screenshot`が合格。合計7、失敗0、skip0。
- C++自己検査ではコンストラクター、コンテナー、C呼出し、RAII、整列・配列・nothrow割当、ネイティブ2スレッドからのstatic初期化、初期化中断後の再試行、DSO別終了処理、40件の終了登録と再登録など15項目が合格。プロセス終了時のデストラクターとC/C++コールバック順も`CPP_EXIT_PASS`で確認した。
- 主担当自身が実画面を見た。C++ GUIへEnterを2回入力し、表示が個数5・合計15へ更新された。Escで終了後、既存C電卓へ入力して`1 + 2 = 3`を確認した。ボタンのマウスクリックは未確認。

代表画面は`build/nocturne-platform/cpp-gui-native/screen-0.png`と`screen-4.png`、ログは同ディレクトリーと`build/cpp-native-tests/test-serial.log`に残した。初期ビルドのLLDライブラリーグループの不正な入れ子、freestandingによるmainのC++名修飾、C++専用リンクスクリプト指定の誤りを修理した。最終ビルドは`build/cpp-release-build.log`で正常終了している。

最終媒体のkernel・initrdは実QEMUで使用した内容とSHA-256が一致した。最後に加えたTLS拒否も、正常アプリのバイナリーを変えていないため、同じ実行検査を繰り返していない。

| 対象 | SHA-256 |
| --- | --- |
| cppdemo（initrd内も一致） | `23b50b684d5afc2002b6deb11782e9a892385eadbf97a010f3a87e45ed2f0d77` |
| kernel.elf | `07e2138949e4efa41023080b35aa84044ba323c5daffcb1007378f7e6bbaeb46` |
| initrd.tar | `7d50ad8a46c5fd7602867c7c9509a186abed87117c3e4cd80855e0ba111d4b5b` |

試験VM停止を確認し、専用boot/dataコピー・展開kernel/initrd・PNGと重複するPPMの18ファイルを回収した。ログ・PNG・metadata、C++開発用ライブラリーと抽出ソース、最新配布媒体、通常の`build/data.img`は保全した。片付け後のDドライブ空きは159.52 GiB。削除した試験媒体は保存済みの再実行環境ではない。

Hyper-V実機は未確認。ブラウザーコードは変更しておらず、この作業からブラウザーの性能改善や既存の性能回帰の解消は主張しない。
