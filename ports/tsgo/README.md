# Nocturne 用 TypeScript-Go（tsgo）

TypeScript 7 の公式コンパイラー（TypeScript-Go、`github.com/microsoft/typescript-go`）を
`GOOS=nocturne` の Go（`ports/go`）でビルドし、`C:\Nocturne\System\tsgo` に入れます。
TypeScript の標準ライブラリー（`lib.*.d.ts`）は実行ファイルに埋め込まれています。
ライセンスは Apache-2.0 で、`LICENSE` と `NOTICE.txt` を `C:\Nocturne\Licenses\typescript-go` に置きます。

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
Go の移植（`os`、`net`、`os/exec` 等）の上でそのまま動きます。TypeScript のパス（`tspath`）は
OS によらず `C:/...` をドライブ付きの完全なパスとして扱い、作業フォルダー（`C:\Users\<名前>`）の
`\` は `/` にそろえて使います。Nocturne の名前は大文字・小文字を区別しないので、tsgo の判定
（実行ファイルを大文字・小文字を入れ替えた名前で探す）も区別しない側になります。

## 確認したこと（QEMU、WHPX、4 CPU。ドライブパスのファイルシステムより前）

- `tsgo --version` → `Version 7.0.2`
- 単一ファイル：`tsgo /tmp/a.ts` で JavaScript を出力、型の誤りは `error TS2322` を表示。
  `--extendedDiagnostics` で 83 ファイル（標準ライブラリー込み）、検査 0.42 秒、合計 0.57 秒。
- プロジェクト：`tsgo --init` で `tsconfig.json` を作り、`tsgo` がフォルダーを走査して
  `.js`・`.d.ts`・`.map` を出力。

出力した JavaScript は `js` で実行します。`js file.ts` は tsgo で検査・コンパイルしてから実行し、
`js --init` は tsgo と js 用のプロジェクトを作ります（README の「JavaScript と TypeScript」）。
`js --init` のプロジェクトで `tsgo` → `js dist/main.js` が動くことを QEMU で確認しました。
