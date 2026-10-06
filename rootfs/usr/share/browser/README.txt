Nocturne ブラウザーの信頼済み Public Suffix List

取得元: https://publicsuffix.org/list/public_suffix_list.dat
ライセンス: Mozilla Public License 2.0 (https://mozilla.org/MPL/2.0/)
原データのライセンス、VERSION、COMMITコメントをデータ内に保持。
取得日時と取得原文のSHA256もデータ先頭に記録。

更新: 開発ホストで python scripts/update_public_suffix.py
依存: Python idna (IDNA2008 / UTS #46、non-transitional)。今回利用: 3.16。
ICANN / PRIVATE、通常規則 / wildcard / exception を全て保持し、Unicode
ドメイン規則だけA-labelへ変換する。変換不能な規則は無視せず更新を停止。
OS実行時にダウンロードしない。Cookie jarはこのローカル信頼済み表だけを
使い、ページから指定されたPSLは使わない。読込失敗時はDomain拡張を拒否。
