#!/bin/sh
# Most frequent file operations the loader traced in the last pulled log.
P=${NOVA3_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}
L=$P/logs/phone.log
grep -oE "io: [a-z_]+ '[^']*'" $L | sort | uniq -c | sort -rn | head -15
echo "-- lines mentioning io: $(grep -c 'io:' $L)"
