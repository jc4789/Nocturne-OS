# Nocturne 用 Go 移植（GOOS=nocturne）

Go 1.27.2（`VERSION` が `go1.27.2` のソース）に `GOOS=nocturne`、`GOARCH=amd64` を足す差分です。
`nocturne.patch` を上流のソースへ当て、ホスト（Windows 等）でツールチェーンを作り、
Nocturne 用の静的 ELF をクロスコンパイルします。Go は BSD 型のライセンスで、
実行ファイルに入るランタイムのライセンス文は初期 RAMfs の `/usr/share/licenses/go` に置きます。

## 作り方

```
git apply --directory=<Go のソース> ports/go/nocturne.patch      # または patch -p1
cd <Go のソース>/src && make.bat                                  # GOROOT_BOOTSTRAP に既存の Go
make GO=<Go のソース>/bin/go.exe                                  # ports/go/apps/* を /bin へ入れる
```

`GO` を渡さない通常のビルドは Go のアプリを作りません。アプリは
`ports/go/apps/<名前>/main.go` に置くと `/bin/<名前>` になります。
単体では `GOOS=nocturne GOARCH=amd64 go build` で作れます。

## Nocturne への対応の形

Nocturne は UNIX でも POSIX でもないので、Go 側を Nocturne の API に合わせています
（`internal/syslist` の `UnixOS` には入れていません）。

- **システムコール**：`INT $0x80`、番号は AX、引数は DI・SI・DX・R10・R8、結果は AX
  （負なら結果コード）。`syscall` パッケージは Nocturne の API そのもの
  （`DocumentOpen`、`DocumentReadAt`、`FolderList`、`ChannelSend`、`AppLaunch` 等）を出し、
  標準ライブラリーが使う `ENOENT` 等の名前は Nocturne の結果コードの別名です。
- **起動**：ELF（ET_EXEC、0x400000 から）。カーネルが渡す設定（`rdx`）を環境として読みます。
- **スレッド**：`SYS_THREAD_CREATE` のスタック（カーネルが確保）を g0 に使い、TLS は
  `SYS_THREAD_TLS`（FS）。ロックは `SYS_WAIT_ADDRESS`／`SYS_WAKE_ADDRESS`。
- **メモリー**：需要ゼロの共有領域（`SYS_SHARED_RESERVE`＋`SYS_SHARED_MAP`）。アドレスは
  カーネルが選ぶので、ヒープのアドレス希望は使いません。
- **時刻**：Hyper-V の参照ページ、または TSC（`SYS_CLOCK_INFO`）。壁時計は `SYS_TIME` を基点にします。
- **文書**：共有カーソルはないので、`os.File` が自分の位置を持ちます。`O_APPEND` は書くたびに末尾。
  権限ビットはなく、`Chmod` は存在確認だけ、`Chtimes`・`Chown`・リンクは非対応を返します。
- **フォルダー**：`FolderList` で一覧。作業フォルダーは設定 `FOLDER`（`os.Chdir` が変え、
  起動するアプリへ渡します）。
- **入出力**：0・1・2 番のハンドル（入力・出力・ログ）。チャンネルはメッセージ単位なので、
  `Read` は大きなメッセージの残りを次へ持ち越し、`Write` は 64KiB ごとに送ります。
  2 番がないアプリの標準エラーとランタイムの panic 出力はシステムログ（シリアル）へ出ます。
  `os.DevNull` はハンドルを持たない File で、起動するアプリにはその位置にハンドルを渡しません。
- **アプリの起動**：`os.StartProcess`／`os/exec` は `AppLaunch`、待つのは `AppResult`、
  `Kill` は `AppStop`。`PATH`（既定 `/bin:/data/bin`）を `:` で区切って探します。
- **シグナルはありません**：`os/signal.Notify` は何も届けません。非同期プリエンプションも使いません。
- **フォルダーの既定**：`os.UserHomeDir` は `HOME`（既定 `/home`）、`UserCacheDir` は `/data/cache`、
  `UserConfigDir` は `/data/etc`。

## 確認したこと（QEMU、WHPX、4 CPU）

`ports/go/apps/gotest` を端末から実行し、画面で確認しました。出力、時刻、`time.Sleep`、
8 本の goroutine（`GOMAXPROCS=1` で 517ms、4 で 152ms）、文書の作成・追記・読み戻し・`Stat`、
`/bin` の一覧、作業フォルダー、存在しない名前のエラー、チャンネルと `select`、GC、
`os/exec` でのアプリ起動、`panic` のトレース表示、パイプからの標準入力。

## まだできないこと

- `net` パッケージ（TCP・DNS・UDP）。TypeScript-Go（tsgo）のビルドに必要です。
- nil 参照などの CPU 例外は Go の panic にならず、アプリが終了します（カーネルに
  例外をアプリへ渡す仕組みがまだありません）。
- 解放したヒープのページは OS へ返しません（`sysUnused` は何もしません）。
- cgo、プロファイラーのシグナル、`runtime/debug.SetCrashOutput`（ハンドルの複製がないため）。
