/* Expat の Nocturne 向けの構成（third_party/expat/README.nocturne.md）。
   上流の配布物には含まれない。名前空間の処理は Expat に任せず
   user/libc/web/xml.c が修飾名から行うので、XML_ParserCreateNS は使わない。 */
#ifndef EXPAT_CONFIG_H
#define EXPAT_CONFIG_H 1

/* xmlparse.c の Nocturne の分岐：POSIX の見出しを読まず、hash の塩は n_random。 */
#define XML_NOCTURNE 1

#define BYTEORDER 1234
#define HAVE_MEMMOVE 1

/* 内部 subset の実体の宣言と ATTLIST の既定値は XML 1.0 の非検証の解析器の
   務め。外部の実体は xml.c が既知の XHTML の DTD のほかは読まない。 */
#define XML_DTD 1
#define XML_GE 1
#define XML_NS 1
#define XML_CONTEXT_BYTES 1024

#endif
