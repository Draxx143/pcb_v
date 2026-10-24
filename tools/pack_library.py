#!/usr/bin/env python3
"""Packs the bundled part library into one zip that can be downloaded and unzipped.

    python3 tools/pack_library.py            # -> dist/project_pcb_problem_finder-library-<version>.zip

The archive holds both bundles plus the credits/how-to text, so a user who only wants the
library never needs to clone the repository.
"""

from __future__ import annotations

import os
import re
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIBRARY = os.path.join(ROOT, "library")
DIST = os.path.join(ROOT, "dist")
FILES = ["pcbv-starter.pcbvlib", "pcbv-symbols.pcbvlib", "README.md"]


def version() -> str:
    with open(os.path.join(ROOT, "CMakeLists.txt"), encoding="utf-8") as handle:
        match = re.search(r"project\s*\(\s*PcbAiViewer\s+VERSION\s+([0-9.]+)", handle.read())
    return match.group(1) if match else "0"


def main() -> int:
    os.makedirs(DIST, exist_ok=True)
    target = os.path.join(DIST, f"project_pcb_problem_finder-library-{version()}.zip")
    missing = [name for name in FILES if not os.path.isfile(os.path.join(LIBRARY, name))]
    if missing:
        print(f"missing in library/: {', '.join(missing)}", file=sys.stderr)
        return 1
    with zipfile.ZipFile(target, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in FILES:
            archive.write(os.path.join(LIBRARY, name), arcname=os.path.join("library", name))
    size = os.path.getsize(target)
    print(f"wrote {target} ({size / 1024:.0f} kB)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
