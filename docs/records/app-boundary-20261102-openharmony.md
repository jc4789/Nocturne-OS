# プログラムと OS の境界の調べ：OpenHarmony の境界

判定は [app-boundary-20261102.md](app-boundary-20261102.md) にあります。これは主担当が自分で読んだ記録です。

依頼は「OS を比べるなら OpenHarmony も見るべき」です。文書は [openharmony/docs](https://github.com/openharmony/docs) の `master` の本文を読みました。ソースは読んでいません。

## カーネルと libc

- 標準の系のカーネルは Linux の LTS です（`en/device-dev/kernel/kernel-standard-overview.md`）。
- libc は musl で、C99、C11、POSIX に沿います（`en/application-dev/reference/native-lib/musl.md`）。
- 故障の記録の例では、libc は `/system/lib/ld-musl-*.so.1` にあります（`guidance-on-ndk-libc-interfaces-affected-by-permissions.md`）。
- `dlopen` は名前空間で狭められ、システムの動的ライブラリーは開けません（`musl.md`）。
- symbol の版を支えます（`musl.md`）。
- 全部のアプリのプロセスに同じ seccomp の方針を当てます。禁じた呼出しは `SIGSYS` で止まります（`en/device-dev/subsystems/subsys-boot-init-seccomp.md`、`guidance-on-ndk-libc-interfaces-affected-by-permissions.md`）。

**読み取り：** 境界は Android に近いシステムライブラリーです。syscall の制限は番号によるもので、Fuchsia のような呼出し元の位置の強制は文書にありません。

## C++ の分離

出どころは `en/application-dev/reference/native-lib/cpp.md` です。

- システムの `libc++.so` はイメージと一緒に、アプリの `libc++_shared.so` は SDK と一緒に更新されます。
- 両者の版は離れることがあり、ABI の問題になります。
- C++ の名前空間を `__h` と `__n1` に分け、互いに使えなくしています。
- 公開の native の口は C だけです。
- 共有ライブラリーが一つのアプリには `c++_static` を勧めています。複数の動的ライブラリーで静的にすると、One Definition Rule を破ることがあります。

**読み取り：** OS の境界に C++ の物を渡さない、という推奨の裏付けです。

## 口の級と窓

出どころは `en/design/OpenHarmony-API-governance.md` です。

| 級 | 使う者 | 互換 | 守る物 |
|---|---|---|---|
| Public API | 全部のアプリの開発者 | 5 版 | XTS |
| Test API | 全部のアプリの開発者 | 3 版 | 作る予定 |
| System API | システムのアプリの開発者 | 約束しない | 作る予定 |
| HDI | システムのサービス | 4 版 | XTS |
| Driver API | ドライバーの開発者 | 約束しない | 無し |
| Inner API | システムの部品 | 約束しない | 無し |

- 公開した口は、原則として互換を壊しません。
- 非推奨にするには印と代わりの口が要り、そこから少なくとも 5 版は残します。

**読み取り：** Fuchsia と違い、窓の長さが数で決まっています。

## 機能の宣言

出どころは `en/application-dev/reference/syscap.md` です。

- SysCap は、Bluetooth やカメラのような独立した機能に付いた名前です。
- 機能の集合は三つです。装置が支える集合、開発で使える集合、アプリが要る集合です。
- 要る集合は RPCID に符号化され、インストールの包みに入ります。
- インストールのとき、包みの管理者が RPCID を解いて装置の集合と比べます。足りればインストールできます。
- 動いている間は `canIUse` で問えます。ArkTS と C の両方に口があります。
- 機能が装置に無いときの誤りは 801 です。

**読み取り：** インストールのときに要る物を照らす形は、機能の単位では OpenHarmony に既にあります。

## 版の自動の刻印

出どころは `en/application-dev/quick-start/app-configuration-file.md` です。

- `minAPIVersion` と `targetAPIVersion` はビルドのときに自動で生成され、手で書けません。

## ハードウェアの口の規則

出どころは `en/design/hdi-design-specifications.md` です。

- 版は `major.minor` で、包みの名に入ります（`ohos.hdi.vibrator.v1_1`）。
- 互換の無い変更は `major`、足す変更は `minor` を上げます。
- 非推奨の口は少なくとも 4 版残します。
- 呼ぶ側は相手の版を確かめ、合う版で呼びます。
- 口は `.idl` で公開します。C や C++ の見出しでの公開は禁じられています。

**読み取り：** HDI はアプリではなく、システムのサービスとドライバーの間の口です。規則はサービスの文にも使えます。

## 包みと共有

出どころは `en/application-dev/quick-start/in-app-hsp.md`、`install-and-update-consistency-verification.md` です。

- HSP はアプリと一緒に配られ、同じプロセスで動く共有の包みです。
- 循環の依存と、依存の連なりの利用は支えません。
- 組織の中で共有する integrated HSP も、道具が bundle の名を置き換え、そのアプリの HSP として作り直します。
- インストールと更新のとき、署名の身元、`versionCode`、権限の級 `apl` などの一致を確かめます。

**読み取り：** 第三者の共有ライブラリーをアプリの間で共有しない形です。DLL hell を避ける方向は同じです。ただ、封じた module ほど閉じてはいません。

## プロセスの間の呼出し

出どころは `en/application-dev/ipc/ipc-rpc-overview.md` です。

- 同じ機械の中は Binder の driver を使います。
- 一度に渡せるのは 200 KB までで、それより大きなデータは共有メモリーで渡します。

**読み取り：** 往復の遅さの数字は文書に見つかりませんでした。

## ネイティブの位置付け

出どころは `en/application-dev/napi/ndk-development-overview.md` です。

- NDK の文書は、C と C++ だけのアプリと、多くの種類の機械で動くアプリには NDK を勧めていません。
- NDK は基礎の機能だけで、ArkTS の口の全部は持ちません。

**読み取り：** アプリの主な境界は ArkTS の側です。ネイティブが第一級の Nocturne とは前提が違います。

## 読んでいないもの

- ソース。
- `targetAPIVersion` で動きを切り替える範囲。
- GPU のドライバーの読み込みの道。
- サービスの登録と `samgr` の詳細。
- OpenHarmony の製品としての実績の数字。
