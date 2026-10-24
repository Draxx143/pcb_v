#!/usr/bin/env bash
# Launcher: starts the GUI (or the CLI when the GUI is not available).
#
#   ./run.sh                        open the app
#   ./run.sh board.PcbDoc           open the app with a document
#   ./run.sh --list board.PcbDoc    use the terminal tool even if the GUI exists
set -uo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GUI="$DIR/build/pcbv-gui"
CLI="$DIR/build/pcbv-cli"

if [ "${1:-}" = "--cli" ]; then
    shift
    [ -x "$CLI" ] || { echo "pcbv-cli not built yet — run: bash setup_linux.sh" >&2; exit 1; }
    exec "$CLI" "$@"
fi

if [ -x "$GUI" ]; then
    exec "$GUI" "$@"
fi

if [ -x "$CLI" ]; then
    if [ $# -eq 0 ]; then
        echo "The Qt6 GUI is not built; showing the CLI help instead."
        exec "$CLI" --help
    fi
    exec "$CLI" "$@"
fi

echo "Nothing is built yet. Run:  bash setup_linux.sh" >&2
exit 1
