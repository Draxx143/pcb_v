#!/usr/bin/env bash
# Renames an existing checkout to the current project folder name and rebuilds it.
#
#   bash tools/rename_project_folder.sh            # ~/pcb_v -> ~/project_pcb_problem_finder
#   bash tools/rename_project_folder.sh /opt/pcb_v # any other old location
set -euo pipefail
OLD="${1:-$HOME/pcb_v}"
NEW="$HOME/project_pcb_problem_finder"
if [ -d "$NEW" ]; then
    echo "$NEW already exists — leaving it alone."
elif [ -d "$OLD" ]; then
    mv "$OLD" "$NEW"
    echo "moved $OLD -> $NEW"
else
    echo "neither $OLD nor $NEW exists; clone first:"
    echo "  git clone https://github.com/Draxx143/pcb_v.git $NEW"
    exit 1
fi
cd "$NEW" && bash setup_linux.sh --desktop
