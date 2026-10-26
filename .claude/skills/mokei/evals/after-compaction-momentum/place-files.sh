#!/usr/bin/env bash
# 要約・文脈・模型は after-compaction-drift と同じ場面のもの。ここには直前のやり取りだけを置く。
set -eu
here="$(dirname "$0")"
cp -R "$here/../after-compaction-drift/files/." .
cp -R "$here/files/." .
