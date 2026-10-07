#!/bin/sh
# Disk mode default: solutions go to per-worker SQLite databases, which the
# pruner reads directly and removes afterwards.
set -eu
BIN=${1:-./eusolver}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT" "$OUT.log"' EXIT
"$BIN" --mode disk --max-polygons 7 --workers 4 --binary-solutions \
    --output "$OUT" > "$OUT.log" 2>&1 || { cat "$OUT.log"; exit 1; }
grep -q "k=7 -> 1472 unique tilings" "$OUT.log" || { cat "$OUT.log"; exit 1; }
if ls "$OUT" | grep -q -e '^_worker_' -e '^chunks$' -e '^eusolver_'; then
    echo "raw solver output left behind:"; ls "$OUT"; exit 1
fi
echo "solution databases: ok"
