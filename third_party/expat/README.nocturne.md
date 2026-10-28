# Nocturne のブラウザーの XML 解析器が使う Expat

Expat 2.8.5（`lib/expat.h` の `XML_MAJOR_VERSION`・`XML_MINOR_VERSION`・`XML_MICRO_VERSION`）。
ライセンス: 同梱 `COPYING`（MIT）。

出どころ: `D:\Programes\cloned repos\cpython\Modules\expat`（CPython が上流の
`R_2_8_5` の配布物 `expat-2.8.5.tar.gz`、SHA-256
`920dde485e15eda0cce8d2310b41d492c534e5e3d89ad407a0b4176dd2ff88fe` から
`refresh.sh` で写した `lib/` の一部）。CPython が足した二か所
（`expat_external.h` の `pyexpatns.h` の include、`xmlparse.c` の `rand_s` の
`XML_POOR_ENTROPY` の条件）は上流の形へ戻してある。`winconfig.h` と CPython の
`expat_config.h`・`pyexpatns.h` は持ち込まない。構成は `ports/expat/expat_config.h`。

Makefile は `lib/xmlparse.c`・`lib/xmlrole.c`・`lib/xmltok.c` だけを組む
（`xmltok_impl.c`・`xmltok_ns.c`・`xcsinc.c` は include される）。
呼ぶのは `user/libc/web/xml.c` だけで、Expat は字句と DTD の内部 subset と実体の
置換までを受け持つ。名前空間の処理、DOM の構築、parsererror の文書、記憶の勘定と
中断点は xml.c の持ち物。外部の実体は、HTML Standard が挙げる XHTML・MathML の
公開識別子の DTD（名前付き文字参照の宣言）のほかは読まない。

上流からの局所修正（`lib/xmlparse.c` だけ、`XML_NOCTURNE` の分岐）:

- `<sys/time.h>`・`<sys/types.h>`・`<unistd.h>`・`<fcntl.h>` を読まず、`nocturne.h` を読む。
- `gather_time_entropy` を組まない。
- `generate_hash_secret_salt` は `n_random` の 16 バイトを使い、それが答えない
  ときだけ `uptime_ms` から作る。xml.c は解析器ごとに `XML_SetHashSalt16Bytes` も渡す。
- 良い乱数の源が無いときの `#error` の条件に `XML_NOCTURNE` を足す。

上流を更新するときは、新しい `lib/` を写し、この四か所を当て直す。
