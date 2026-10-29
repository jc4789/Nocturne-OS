# Nocturne の TypeScript（tsc）

TypeScript のコンパイラーは Go で書かれています（Microsoft のリポジトリー
`github.com/microsoft/TypeScript` の `tsc/`、モジュール `github.com/microsoft/TypeScript/tsc`）。
これを `GOOS=nocturne` の Go（`ports/go`）でビルドし、`C:\Nocturne\System\tsc` に入れます。
標準ライブラリーの型（`lib.*.d.ts`）は実行ファイルに埋め込まれています。
ライセンスは Apache-2.0 で、`LICENSE.txt` と `NOTICE.txt` を `C:\Nocturne\Licenses\typescript` に置きます。
tsc にリンクされるモジュール（`golang.org/x/text` 等、`go version -m` が示すもの）のライセンス文も、
モジュールのパスごとにその下へ置きます。

## 作り方

リポジトリーの複製を `tools/TypeScript` に置けば、`GO` があるビルドで自動で使います
（別の場所なら `TYPESCRIPT=<このフォルダーの下のパス>`）。複製はそのコミットのまま使い、書き換えません。
Nocturne が変えるファイルは、複製の中と同じ道で `ports/typescript/tsc/` にそのまま置き、
Go の `-overlay` で複製のファイルの代わりに渡します（`build/typescript/overlay.json` は対応の表だけで、
build にソースは写しません）。

```
python scripts/build.py
```

ホストでも同じソースを（差分なしで）`build/host/tsc.exe` にビルドし、デスクトップの TypeScript の
アプリ（`user/apps/*.ts`）をビルド時に型検査・コンパイルします。

`tsc --version` が `Version 7.1.0-dev` のソースで確かめています。

## 移植の内容

Nocturne は UNIX ではないので、アプリを止めるのは `AppStop` で、終了シグナルは届きません。
`syscall.SIGINT`・`syscall.SIGTERM` も無いので、`cmd/tsc` がそれを名指ししていた 3 か所
（`main.go`、`api.go`、`lsp.go`）を、OS ごとのファイルにある `stopSignals` に置き換えています
（Nocturne では `os.Interrupt` だけ）。それ以外は Go の移植（`os`、`net`、`os/exec` 等）の上で
そのまま動きます。

上流を新しくしたら、`ports/typescript/tsc/cmd/tsc/` の `main.go`・`api.go`・`lsp.go` を新しいソースから
取り直し、上の置き換えをもう一度入れます（`stopsignals.go`・`stopsignals_nocturne.go` は Nocturne のファイル）。

## 確認したこと（2026-10-26、QEMU、WHPX、4 CPU、4096 MiB、1920×1080）

- `tsc --version` → `Version 7.1.0-dev`
- `js --init app` → `cd app` → `tsc` → `js dist\main.js` で `Hello, Nocturne!`
- `js src\main.ts Nocturne-tsc` で `Hello, Nocturne-tsc!`。型の誤りは `error TS2322` を表示して実行しない。
- ホストの tsc でコンパイルした「設定」が開き、タブの切り替えが動く。
