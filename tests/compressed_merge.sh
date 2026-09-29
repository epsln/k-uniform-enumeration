#!/bin/sh
# Disk mode with mid-run compression: workers compress their .bin output into
# .bin.zst while running, so the merge sees compressed worker files. Merging
# must keep the X.bin.zst name (not X.bin.zst.zst) and the pruner must read
# every solution back.
set -eu
BIN=${1:-./eusolver}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
"$BIN" --mode disk --max-polygons 7 --workers 4 --binary-solutions \
    --compress-solutions --compress-threshold-mb 0 --keep-pruner-inputs \
    --output "$OUT" > "$OUT.log" 2>&1 || { cat "$OUT.log"; rm -f "$OUT.log"; exit 1; }
grep -q "k=7 -> 1472 unique tilings" "$OUT.log" || { cat "$OUT.log"; rm -f "$OUT.log"; exit 1; }
rm -f "$OUT.log"
if ls "$OUT" | grep -q '\.zst\.zst$'; then
    echo "doubly suffixed merged output:"; ls "$OUT" | grep '\.zst\.zst$'; exit 1
fi
echo "compressed merge: ok"
