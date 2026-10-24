#!/usr/bin/env python3
"""import_kicad.py - add parts from a KiCad installation to libraries/index.json.

A footprint file (.kicad_mod) is the unit of import: its name, its description, its
datasheet property and the 3D model reference it carries are enough to fill a row of the
part database. Symbols are matched by name when a symbol library is given.

Examples
--------
  # see what would be imported, change nothing
  python3 tools/import_kicad.py --footprints /usr/share/kicad/footprints/Resistor_SMD.pretty \\
                                --models3d   /usr/share/kicad/3dmodels --dry-run

  # import one footprint family and copy the files into the project
  python3 tools/import_kicad.py --footprints /usr/share/kicad/footprints/Package_TO_SOT_SMD.pretty \\
                                --symbols    /usr/share/kicad/symbols/Device.kicad_sym \\
                                --models3d   /usr/share/kicad/3dmodels \\
                                --only TO-252-2,SOT-23 --copy

  # after importing, check the result
  ./build/pcbv-cli --index libraries/index.json
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import shutil
import sys

INDEX_FORMAT = "pcbv-part-index"
INDEX_VERSION = 1


# ---------------------------------------------------------------------------
# reading the KiCad S-expression files
# ---------------------------------------------------------------------------

def parse_sexpr(text):
    """Parses a small S-expression file into nested lists of strings."""
    tokens = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "(":
            tokens.append("(")
            i += 1
        elif c == ")":
            tokens.append(")")
            i += 1
        elif c == '"':
            i += 1
            buf = []
            while i < n:
                ch = text[i]
                if ch == "\\" and i + 1 < n:
                    nxt = text[i + 1]
                    buf.append({"n": "\n", "t": "\t", "r": "\r"}.get(nxt, nxt))
                    i += 2
                    continue
                if ch == '"':
                    i += 1
                    break
                buf.append(ch)
                i += 1
            tokens.append("".join(buf))
        elif c == "|":
            j = text.find("|", i + 1)
            if j < 0:
                j = n
            tokens.append(text[i + 1:j])
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()";':
                j += 1
            tokens.append(text[i:j])
            i = j

    position = 0

    def read():
        nonlocal position
        if position >= len(tokens):
            raise ValueError("unexpected end of file")
        token = tokens[position]
        position += 1
        if token != "(":
            return token
        items = []
        while position < len(tokens) and tokens[position] != ")":
            items.append(read())
        position += 1
        return items

    return read()


def children(node, head):
    return [item for item in node if isinstance(item, list) and item and item[0] == head]


def first_child(node, head):
    found = children(node, head)
    return found[0] if found else None


def atom(node, index, default=""):
    if isinstance(node, list) and len(node) > index and not isinstance(node[index], list):
        return node[index]
    return default


def properties(node):
    """(property "Name" "Value" …) and (fp_text reference "R1" …) -> dict."""
    out = {}
    for item in children(node, "property"):
        if len(item) >= 3:
            out[str(item[1])] = str(item[2])
    for item in children(node, "fp_text"):
        if len(item) >= 3:
            if item[1] in ("reference", "value"):
                out[item[1].capitalize()] = str(item[2])
    return out


def read_footprints(folder):
    """Every .kicad_mod of a folder (or of the one file given)."""
    paths = []
    if os.path.isdir(folder):
        for name in sorted(os.listdir(folder)):
            if name.lower().endswith(".kicad_mod"):
                paths.append(os.path.join(folder, name))
    elif folder:
        paths.append(folder)
    for path in paths:
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                tree = parse_sexpr(handle.read())
        except (OSError, ValueError) as error:
            print("  skipped %s (%s)" % (path, error), file=sys.stderr)
            continue
        root = tree if atom(tree, 0) in ("footprint", "module") else None
        if root is None:
            print("  skipped %s (not a footprint)" % path, file=sys.stderr)
            continue
        yield path, root


def model_references(root):
    """The (model "path" …) records of a footprint."""
    out = []
    for item in children(root, "model"):
        path = atom(item, 1)
        if path:
            out.append(path.replace("\\", "/"))
    return out


def read_symbols(path):
    """The symbols of a .kicad_sym file: name -> properties."""
    symbols = {}
    if not path:
        return symbols
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            tree = parse_sexpr(handle.read())
    except (OSError, ValueError) as error:
        print("  skipped %s (%s)" % (path, error), file=sys.stderr)
        return symbols
    for item in children(tree if isinstance(tree, list) else [], "symbol"):
        name = atom(item, 1)
        if not name or name.endswith("_0_1") or name.endswith("_1_1"):
            continue
        symbols[name] = properties(item)
    return symbols


# ---------------------------------------------------------------------------
# building a row
# ---------------------------------------------------------------------------

def guess_package(name, description):
    """A short package name: 0603, SOT-23, TO-252-2 …"""
    for source in (name, description):
        match = re.search(r"\b(0[24]02|0[46]03|0[81]05|1206|1210|2010|2512)\b", source)
        if match:
            return match.group(1)
    match = re.search(r"\b(TO-\d+[A-Z]?[-\d]*|SOT-\d+[A-Z]?[-\d]*|SOD-\d+[A-Z]?[-\d]*|"
                      r"QFN-\d+|DFN-\d+|SOIC-\d+|SO-\d+|SSOP-\d+|TSSOP-\d+|DIP-\d+|"
                      r"BGA-\d+|LQFP-\d+|TQFP-\d+)", name)
    if match:
        return match.group(1)
    return ""


def split_name(name):
    """'Package_TO_SOT_SMD:TO-252-2' -> ('Package_TO_SOT_SMD', 'TO-252-2')."""
    if ":" in name:
        library, item = name.split(":", 1)
        return library, item
    return "", name


def slug(text):
    cleaned = re.sub(r"[^A-Za-z0-9]+", "-", text).strip("-").lower()
    return cleaned or "part"


def build_row(footprint_path, root, symbols, model_lookup):
    full_name = atom(root, 1) or os.path.splitext(os.path.basename(footprint_path))[0]
    library, item = split_name(full_name)
    props = properties(root)
    descr = atom(first_child(root, "descr") or [], 1) or props.get("Description", "")
    datasheet = props.get("Datasheet", "")
    if datasheet in ("~", ""):
        datasheet = ""

    symbol_name = props.get("Symbol", "") or props.get("Value", "") or item
    if ":" in symbol_name:
        symbol_name = symbol_name.split(":", 1)[1]
    symbol_props = symbols.get(symbol_name, {})
    keyword = symbol_props.get("ki_description") or descr or item

    value = props.get("Value", "")
    if value in ("", item):
        value = ""

    row = {
        "id": slug(item if not value else "%s-%s" % (item, value)),
        "mpn": "",
        "manufacturer": "",
        "value": value,
        "package": guess_package(item, keyword),
        "description": keyword,
        "datasheet": datasheet,
        "symbol": "",
        "footprint": footprint_path,
        "model": "",
        "tags": sorted({tag for tag in (
            library.replace("_", " ").lower(),  # "package to sot smd"
            guess_package(item, keyword).lower(),
            "kicad") if tag}),
    }
    if symbol_props:
        row["symbol"] = symbol_file_of.get(symbol_name, "")

    models = model_references(root)
    if models:
        base = models[0].rsplit("/", 1)[-1]
        if base in model_lookup:
            row["model"] = model_lookup[base]
        else:
            row["model"] = models[0]
    return row


def scan_models(folder):
    """basename -> path for every .step/.stp/.wrl under `folder`."""
    found = {}
    if not folder:
        return found
    if os.path.isfile(folder):
        found[os.path.basename(folder)] = folder
        return found
    for root, _dirs, files in os.walk(folder):
        for name in files:
            if name.lower().endswith((".step", ".stp", ".wrl")):
                found.setdefault(name, os.path.join(root, name))
    return found


# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------

symbol_file_of = {}


def copy_into(source, destination_folder, relative_to):
    """Copies a file into the project folder and returns the new relative path."""
    os.makedirs(destination_folder, exist_ok=True)
    target = os.path.join(destination_folder, os.path.basename(source))
    if os.path.abspath(source) != os.path.abspath(target):
        shutil.copyfile(source, target)
    return os.path.join(relative_to, os.path.basename(source)).replace(os.sep, "/")


def main():
    parser = argparse.ArgumentParser(description="Import KiCad parts into libraries/index.json")
    parser.add_argument("--footprints", action="append", default=[],
                        help=".pretty folder or single .kicad_mod (repeatable)")
    parser.add_argument("--symbols", action="append", default=[],
                        help=".kicad_sym file (repeatable)")
    parser.add_argument("--models3d", default="", help="KiCad 3dmodels folder")
    parser.add_argument("--index", default="libraries/index.json", help="the part database")
    parser.add_argument("--only", default="",
                        help="comma separated footprint names to import (default: all)")
    parser.add_argument("--limit", type=int, default=0, help="stop after N parts")
    parser.add_argument("--copy", action="store_true",
                        help="copy the files into libraries/ so the project is self-contained")
    parser.add_argument("--dry-run", action="store_true", help="print what would be added")
    args = parser.parse_args()

    if not args.footprints and not args.symbols:
        parser.error("give at least --footprints or --symbols")

    only = {name.strip() for name in args.only.split(",") if name.strip()}

    symbols = {}
    for path in args.symbols:
        found = read_symbols(path)
        for name, props in found.items():
            symbols[name] = props
            symbol_file_of[name] = path
        print("symbols    %-60s %d" % (path, len(found)))

    model_lookup = scan_models(args.models3d)
    if args.models3d:
        print("3D models  %-60s %d file(s)" % (args.models3d, len(model_lookup)))

    rows = []
    for folder in args.footprints:
        print("footprints %s" % folder)
        for path, root in read_footprints(folder):
            name = atom(root, 1)
            _library, item = split_name(name)
            if only and item not in only and name not in only:
                continue
            row = build_row(path, root, symbols, model_lookup)
            rows.append(row)
            if args.limit and len(rows) >= args.limit:
                break
        if args.limit and len(rows) >= args.limit:
            break

    if not rows:
        print("\nNothing to import: no footprint matched.")
        return 1

    # Merge with what the database already holds (same fingerprint -> replace).
    index = {"format": INDEX_FORMAT, "version": INDEX_VERSION, "parts": []}
    index_path = args.index
    if os.path.isfile(index_path):
        try:
            with open(index_path, "r", encoding="utf-8") as handle:
                loaded = json.load(handle)
            if isinstance(loaded, dict) and isinstance(loaded.get("parts"), list):
                index = loaded
                index.setdefault("format", INDEX_FORMAT)
                index.setdefault("version", INDEX_VERSION)
        except (OSError, ValueError) as error:
            print("warning: %s could not be read (%s); a new database is written"
                  % (index_path, error), file=sys.stderr)

    def key(row):
        return (row.get("footprint") or row.get("id") or "").replace("\\", "/").lower()

    existing = {key(row): row for row in index["parts"]}
    added = 0
    for row in rows:
        if args.copy:
            base = os.path.dirname(os.path.abspath(index_path))
            row["footprint"] = copy_into(row["footprint"], os.path.join(base, "footprints"),
                                         "footprints")
            if row["symbol"] and os.path.isfile(row["symbol"]):
                row["symbol"] = copy_into(row["symbol"], os.path.join(base, "symbols"), "symbols")
            if row["model"] and os.path.isfile(row["model"]):
                row["model"] = copy_into(row["model"], os.path.join(base, "models3d"), "models3d")
        if key(row) not in existing or existing[key(row)] != row:
            added += 1
        existing[key(row)] = row

    index["parts"] = sorted(existing.values(), key=lambda row: row.get("id", ""))
    index["updated"] = datetime.datetime.now().strftime("%Y-%m-%d %H:%M")

    print("\n%d footprint(s) read, %d new or changed row(s), %d row(s) in total"
          % (len(rows), added, len(index["parts"])))
    if args.dry_run:
        for row in rows[:20]:
            print("  %-28s %-12s %s" % (row["id"][:28], row["package"], row["footprint"]))
        if len(rows) > 20:
            print("  … %d more" % (len(rows) - 20))
        print("\ndry run: %s not written" % index_path)
        return 0

    os.makedirs(os.path.dirname(os.path.abspath(index_path)) or ".", exist_ok=True)
    with open(index_path, "w", encoding="utf-8", newline="\n") as handle:
        json.dump(index, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    print("wrote %s\n\nCheck it with:  ./build/pcbv-cli --index %s" % (index_path, index_path))
    return 0


if __name__ == "__main__":
    sys.exit(main())
