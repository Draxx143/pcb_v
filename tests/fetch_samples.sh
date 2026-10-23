#!/usr/bin/env bash
# Downloads a small set of public Altium test documents (from the AltiumSharp project)
# so you can try the viewer without your own board.
#
#   bash tests/fetch_samples.sh [target-dir]
set -euo pipefail
DIR="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/samples}"
BASE="https://raw.githubusercontent.com/issus/AltiumSharp/master/TestData"
mkdir -p "$DIR"
for f in "SPI Isolator.PcbDoc" "DAC.SchDoc" "Power Supply.SchDoc" "USB Power Adapter.PcbDoc"; do
  out="$DIR/$f"
  if [ -f "$out" ]; then echo "already there: $f"; continue; fi
  echo "downloading: $f"
  curl -fsSL --retry 3 "$BASE/$(printf '%s' "$f" | sed 's/ /%20/g')" -o "$out" \
    || { echo "failed: $f (network?)" >&2; rm -f "$out"; }
done
ls -la "$DIR"
