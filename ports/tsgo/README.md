# Nocturne 用 TypeScript-Go（tsgo）

TypeScript 7 の公式コンパイラー（TypeScript-Go、`github.com/microsoft/typescript-go`）を
`GOOS=nocturne` の Go（`ports/go`）でビルドし、`/bin/tsgo` に入れます。
TypeScript の標準ライブラリー（`lib.*.d.ts`）は実行ファイルに埋め込まれています。
ライセンスは Apache-2.0 で、`LICENSE` と `NOTICE.txt` を `/usr/share/licenses/typescript-go` に置きます。

## 作り方

```
patch -p1 -d <TypeScript-Go のソース> < ports/tsgo/nocturne.patch
make GO=<Go のソース>/bin/go.exe TSGO=<TypeScript-Go のソース> image
```

TypeScript 7.0.2 のソース（`TypeScript-7.0.2/tsc`）で確認しました。

## 移植の内容

Nocturne は UNIX ではないので、アプリを止めるのは `AppStop` で、終了シグナルは届きません。
`cmd/tsgo` が `syscall.SIGTERM` を名指ししていた 3 か所を、OS ごとのファイルにある
`stopSignals` に置き換えています（Nocturne では `os.Interrupt` だけ）。それ以外は
Go の移植（`os`、`net`、`os/exec` 等）の上でそのまま動きます。

## 確認したこと（QEMU、WHPX、4 CPU）

- `tsgo --version` → `Version 7.0.2`
- 単一ファイル：`tsgo /tmp/a.ts` で JavaScript を出力、型の誤りは `error TS2322` を表示。
  `--extendedDiagnostics` で 83 ファイル（標準ライブラリー込み）、検査 0.42 秒、合計 0.57 秒。
- プロジェクト：`tsgo --init` で `tsconfig.json` を作り、`tsgo` がフォルダーを走査して
  `.js`・`.d.ts`・`.map` を出力。

出力した JavaScript を端末で実行する JS 実行環境は、まだ `/bin` にありません。
