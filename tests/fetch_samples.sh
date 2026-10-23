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
# Altium ASCII exports ("PCB ASCII File" / "Schematic Capture Ascii File")
# — handy to verify that both container types are supported.
ASCII_FILES=(
  "https://raw.githubusercontent.com/KiCad/kicad-source-mirror/master/qa/data/eeschema/plugins/altium/ticket1303/overview.SchDoc|ascii_overview.SchDoc"
  "https://raw.githubusercontent.com/KiCad/kicad-source-mirror/master/qa/data/eeschema/plugins/altium/ticket1303/pagea.SchDoc|ascii_pagea.SchDoc"
  "https://raw.githubusercontent.com/aykevl/things/master/earring-ring/pcb-v3.pcbdoc|ascii_board.pcbdoc"
)
for entry in "${ASCII_FILES[@]}"; do
  url="${entry%%|*}"
  name="${entry##*|}"
  out="$DIR/$name"
  [ -f "$out" ] && { echo "already there: $name"; continue; }
  echo "downloading: $name"
  curl -fsSL --retry 3 "$url" -o "$out" || { echo "failed: $name (network?)" >&2; rm -f "$out"; }
done

ls -la "$DIR"
