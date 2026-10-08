#!/bin/sh
# One line per dumped XML: file, size, first tags.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
cd $P/logs/xmldump || exit 1
for f in *.xml; do
  printf '%s %7d  %s\n' "$f" "$(wc -c < "$f")" "$(head -c 400 "$f" | tr '\n\t' '  ' | grep -oE '<[A-Za-z_]+[^>]{0,70}' | grep -v '^<?' | head -2 | tr '\n' ' ')"
done
