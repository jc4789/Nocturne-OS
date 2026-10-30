# Nocturne 用 Go 移植（GOOS=nocturne）

Go 1.27.2（`VERSION` が `go1.27.2` のソース）に `GOOS=nocturne`、`GOARCH=amd64` を足す差分です。
`nocturne.patch` を上流のソースへ当て、ホスト（Windows 等）でツールチェーンを作り、
Nocturne 用の静的 ELF をクロスコンパイルします。Go は BSD 型のライセンスで、
実行ファイルに入るランタイムのライセンス文は `C:\Nocturne\Licenses\go` に置きます。

## 作り方

```
git apply --directory=<Go のソース> ports/go/nocturne.patch      # または patch -p1
cd <Go のソース>/src && make.bat                                  # GOROOT_BOOTSTRAP に既存の Go
make GO=<Go のソース>/bin/go.exe                                  # ports/go/apps/* もディスクへ入れる
```

`GO` を渡さない通常のビルドは Go のアプリを作りません。アプリは
`ports/go/apps/<名前>/main.go` に置くと `build/programs/<名前>` になり、ディスクでは
`C:\Nocturne\System\<名前>`（デスクトップのアプリは `C:\Programs\<アプリ>\<名前>`、
`scripts/layout.py`）に入ります。
単体では `GOOS=nocturne GOARCH=amd64 go build` で作れます。

## Nocturne への対応の形

Nocturne は UNIX でも POSIX でもないので、Go 側を Nocturne の API に合わせています
（`internal/syslist` の `UnixOS` には入れていません）。

- **システムコール**：`SYSCALL`、番号は AX、引数は DI・SI・DX・R10・R8、結果は AX
  （負なら結果コード）。CX と R11 は命令が使う（`docs/nocturne-native-abi.md` §0）。
  **今の差分（`nocturne.patch`）はまだ廃止した `INT $0x80` と `SYS_THREAD_TLS` を使っていて、
  作り直すまで Go のプログラムは最初の呼出しで止まる（2026-10-30）。**`syscall` パッケージは Nocturne の API そのもの
  （`DocumentOpen`、`DocumentReadAt`、`FolderList`、`ChannelSend`、`AppLaunch` 等）を出し、
  標準ライブラリーが使う `ENOENT` 等の名前は Nocturne の結果コードの別名です。
- **起動**：ELF（ET_EXEC、0x400000 から）。カーネルが渡す設定（`rdx`）を環境として読みます。
- **スレッド**：`SYS_THREAD_CREATE` のスタック（カーネルが確保）を g0 に使い、TLS は
  `WRFSBASE` で FS の基底を直に書く。ロックは `SYS_WAIT_ADDRESS`／`SYS_WAKE_ADDRESS`。
- **メモリー**：需要ゼロの共有領域（`SYS_SHARED_RESERVE`＋`SYS_SHARED_MAP`）。アドレスは
  カーネルが選ぶので、ヒープのアドレス希望は使いません。
- **時刻**：Hyper-V の参照ページ、または TSC（`SYS_CLOCK_INFO`）。壁時計は `SYS_TIME` を基点にします。
- **文書**：共有カーソルはないので、`os.File` が自分の位置を持ちます。`O_APPEND` は書くたびに末尾。
  権限ビットはなく、`Chmod` は存在確認だけ、`Chtimes`・`Chown`・リンクは非対応を返します。
- **パス**：ドライブは `C:` の一つだけ。区切りは `\`（`os.PathSeparator`）と `/` のどちらでもよく、
  名前は大文字・小文字を区別しません。`C:\a`、`\a`、`/a` が完全なパス（`filepath.IsAbs`）で、
  `C:` と `C:a` は作業フォルダーの中です。パスの規則は `syscall`（`FullPath`、`PathIsComplete`）の
  一か所にあり、`internal/filepathlite`・`path/filepath`・`os` はそれを使います。`filepath.Abs` と
  `os` が開く名前は C の `n_path_join` と同じく `C:\...` に完全化され、`.` と `..` も解決されます。
  一覧の区切り（`filepath.SplitList`）は `;`。`os.DevNull` は `NUL` です。
- **フォルダー**：`FolderList` で一覧。作業フォルダーは設定 `FOLDER`（`os.Chdir` が変え、
  起動するアプリへ渡します）。
- **入出力**：0・1・2 番のハンドル（入力・出力・ログ）。チャンネルはメッセージ単位なので、
  `Read` は大きなメッセージの残りを次へ持ち越し、`Write` は 64KiB ごとに送ります。
  2 番がないアプリの標準エラーとランタイムの panic 出力はシステムログ（シリアル）へ出ます。
  `os.DevNull` はハンドルを持たない File で、起動するアプリにはその位置にハンドルを渡しません。
- **アプリの起動**：`os.StartProcess`／`os/exec` は `AppLaunch`、待つのは `AppResult`、
  `Kill` は `AppStop`。`PATH` はありません：名前だけのプログラムは C の `find_program` と同じく
  `C:\Nocturne\System`、次に `C:\Programs` の各アプリのフォルダーで探します（`syscall.FindProgram`、
  `exec.LookPath` と `os.Executable` が使います）。
- **シグナルはありません**：`os/signal.Notify` は何も届けません。非同期プリエンプションも使いません。
- **フォルダーの既定**：`os.UserHomeDir` は設定 `USERFOLDER`（`C:\Users\<名前>`）、
  `UserCacheDir` と `UserConfigDir` はその `AppData`、`os.TempDir` は設定 `TEMP`。`HOME` はありません。
- **書込みの確定**：`File.Sync` は `DocumentCommit`（`COMMIT_WAIT`）で文書をディスクまで確定します。
- **通信**：TCP・UDP の番号はハンドルではないので、`net` は Nocturne の通信呼出し
  （`TCPConnect`、`TCPRecv`、`TCPSendUntil`、`UDPSocket` 等）を直接使います。待ちは
  250ms ごとに区切り、その間に `Close` や期限（`SetDeadline`）の変更を見ます。`Close` は
  使用中の呼出しが終わってから接続を渡すので、送信待ちのデータも送られます（FIN）。
  アドレスの解決はシステムの解決器（`NetDNSAll`、全アプリ共通のキャッシュ）、それ以外の
  レコード（MX、TXT、SRV 等）と `PreferGo` は Go の DNS クライアントで同じ解決器へ問い合わせます。
  名前の固定表（hosts）はありません。IPv4 のみです。
- **待受け**：`net.Listen("tcp", ...)` はカーネルの受動オープン（RFC 9293、`TCPListen`・
  `TCPAccept`）を使います。アドレスを省くと全ローカルアドレス、`127.0.0.1` はループバック
  （インターフェースを通らずカーネル内で折り返し）、ポート 0 は空きポートを選びます。
  `Close` は待っている `Accept` を起こし、まだ受け取っていない接続を拒否（RST）します。
  `CloseWrite` は `TCPFinish`（送信待ちの後に FIN、受信は続く）、`LocalAddr` は実際の
  アドレスとポート（`TCPAddress`）です。
- **TLS**：`crypto/x509` は `C:\Nocturne\Certificates\ca-bundle.crt`（ブラウザーと同じ信頼する
  証明書）を読みます。

## 確認したこと（QEMU、WHPX、4 CPU）

`ports/go/apps/gotest` を端末から実行し、画面で確認しました。出力、時刻、`time.Sleep`、
8 本の goroutine（`GOMAXPROCS=1` で 517ms、4 で 152ms）、文書の作成・追記・読み戻し・`Stat`、
`C:\Nocturne\System` の一覧、作業フォルダー、存在しない名前のエラー、チャンネルと `select`、GC、
`os/exec` でのアプリ起動、`panic` のトレース表示、パイプからの標準入力。

`gotest net` で、インターフェース一覧、`example.com` の解決（21ms）、存在しない名前、
Go の DNS クライアントでの MX、TCP 接続、読み取り期限（300ms で期限切れ）、
HTTP と HTTPS（TLS 1.3、証明書の検証）を確認しました。

`gotest listen` で、`127.0.0.1:0` の HTTP サーバーへの GET（11ms）、並列 64 本（34ms）、
32 MiB の転送と `CloseWrite`（74ms、約 434 MiB/s）、`Accept` の期限（200ms）、`Close` で
起きる `Accept`、同じポートの二重待受け（address in use）、閉じたポートへの接続の拒否、
`127.0.0.1` だけの待受けにインターフェースのアドレスから接続できないことを確認しました。
`gotest serve`（`:8080`）にはホストから QEMU のポート転送で curl（20 本並列）、
Nocturne のブラウザーから `http://127.0.0.1:8080/` で接続し、ページが表示されました。

## まだできないこと

- Unix ドメインソケットと IP 層のソケットはありません（アプリ間はチャンネルを使います）。
- nil 参照などの CPU 例外は Go の panic にならず、アプリが終了します（カーネルに
  例外をアプリへ渡す仕組みがまだありません）。
- 解放したヒープのページは OS へ返しません（`sysUnused` は何もしません）。
- cgo、プロファイラーのシグナル、`runtime/debug.SetCrashOutput`（ハンドルの複製がないため）。
