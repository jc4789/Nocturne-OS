# Nocturne の ACPI（AML）用 uACPI

uACPI 7.0.0（上流タグ `7.0.0`、コミット `686a9a52d9ad72690406d7cada82801377d5e2fa`）。
取得元: https://github.com/uACPI/uACPI
ライセンス: 同梱 `LICENSE`（MIT）。

`source/` と `include/` をそのまま置き、上流の変更はしない。
カーネルへ直接コンパイルする（Makefile の `UACPI_C`）。
カーネル側の関数（`uacpi_kernel_*`）は `kernel/src/dev/acpi.c` が Nocturne の
待ち行列・割り込み・PCI・タイマーで実装する。
