#!/usr/bin/env python3
"""Build the bundled PCB Viewer libraries ("*.pcbvlib") from open sources.

Every footprint comes from the KiCad footprint library (`kicad-footprints`) and every 3D
body from the KiCad 3D library (`kicad-packages3D`), both CC-BY-SA 4.0 with the models
coming from the manufacturers. The script fetches the files with the GitHub CLI, converts
the S-expressions to the bundle format and writes:

    library/pcbv-starter.pcbvlib    footprints + real STEP bodies + datasheet links
    library/pcbv-symbols.pcbvlib    matching schematic symbols

Run it from the repository root (it needs network access and an authenticated `gh`):

    python3 tools/build_library.py

The generated files are committed, so building the application never needs the network.
"""

from __future__ import annotations

import base64
import json
import math
import os
import subprocess
import sys

FOOTPRINTS_REPO = "KiCad/kicad-footprints"
MODELS_REPO = "KiCad/kicad-packages3D"
OUT_DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "library")
MIL = 1.0 / 0.0254  # mm -> mil

CREDITS = (
    "Footprints: KiCad kicad-footprints (CC-BY-SA 4.0). "
    "3D models: KiCad kicad-packages3D (CC-BY-SA 4.0), originally from the manufacturers."
)


# --------------------------------------------------------------------------
# GitHub access
# --------------------------------------------------------------------------
def fetch_raw(repo: str, path: str) -> bytes:
    """Fetches one file through the GitHub API (raw media type)."""
    out = subprocess.run(
        ["gh", "api", "-H", "Accept: application/vnd.github.raw", f"repos/{repo}/contents/{path}"],
        check=True,
        capture_output=True,
    )
    return out.stdout


# --------------------------------------------------------------------------
# S-expression parsing (KiCad .kicad_mod)
# --------------------------------------------------------------------------
def tokenize(text: str):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c == "(" or c == ")":
            yield c
            i += 1
        elif c == '"':
            i += 1
            start = i
            buf = []
            while i < n:
                if text[i] == "\\" and i + 1 < n:
                    buf.append(text[i : i + 2])
                    i += 2
                    continue
                if text[i] == '"':
                    break
                buf.append(text[i])
                i += 1
            i += 1
            yield "".join(buf) if not buf else "".join(buf)
        else:
            start = i
            while i < n and text[i] not in " \t\r\n()":
                i += 1
            yield text[start:i]


def parse_sexpr(text: str):
    tokens = list(tokenize(text))
    pos = 0

    def walk():
        nonlocal pos
        assert tokens[pos] == "("
        pos += 1
        node = []
        while pos < len(tokens) and tokens[pos] != ")":
            if tokens[pos] == "(":
                node.append(walk())
            else:
                node.append(tokens[pos])
                pos += 1
        pos += 1
        return node

    while pos < len(tokens) and tokens[pos] != "(":
        pos += 1
    return walk()


def children(node, name: str):
    return [child for child in node if isinstance(child, list) and child and child[0] == name]


def first(node, name: str):
    found = children(node, name)
    return found[0] if found else None


def numbers(node, name: str, count: int):
    child = first(node, name)
    if child is None:
        return None
    return [float(value) for value in child[1 : 1 + count]]


def layer_of(node):
    child = first(node, "layer")
    return child[1] if child else ""


def width_of(node) -> float:
    """Stroke width in mm, for both the old `(width w)` and new `(stroke (width w))`."""
    stroke = first(node, "stroke")
    if stroke is not None:
        width = first(stroke, "width")
        if width is not None:
            return float(width[1])
    width = first(node, "width")
    if width is not None:
        return float(width[1])
    return 0.0


# --------------------------------------------------------------------------
# Geometry helpers
# --------------------------------------------------------------------------
def circle_from_three(p1, p2, p3):
    (x1, y1), (x2, y2), (x3, y3) = p1, p2, p3
    d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
    if abs(d) < 1e-9:
        return None
    ux = ((x1**2 + y1**2) * (y2 - y3) + (x2**2 + y2**2) * (y3 - y1) + (x3**2 + y3**2) * (y1 - y2)) / d
    uy = ((x1**2 + y1**2) * (x3 - x2) + (x2**2 + y2**2) * (x1 - x3) + (x3**2 + y3**2) * (x2 - x1)) / d
    radius = math.hypot(x1 - ux, y1 - uy)
    return (ux, uy, radius)


def arc_points(start, mid, end, steps=24):
    circle = circle_from_three(start, mid, end)
    if circle is None:
        return [start, end]
    cx, cy, radius = circle
    a0 = math.atan2(start[1] - cy, start[0] - cx)
    a1 = math.atan2(mid[1] - cy, mid[0] - cx)
    a2 = math.atan2(end[1] - cy, end[0] - cx)
    # Walk from a0 to a2 the way that passes a1.
    def norm(angle):
        while angle < 0:
            angle += 2 * math.pi
        return angle

    sweep = norm(a2 - a0)
    if not (norm(a1 - a0) < sweep):
        sweep -= 2 * math.pi
    points = []
    for i in range(steps + 1):
        angle = a0 + sweep * i / steps
        points.append((cx + radius * math.cos(angle), cy + radius * math.sin(angle)))
    return points


def to_mil(value_mm: float) -> float:
    return round(value_mm * MIL, 4)


def flip(point):
    """KiCad draws footprints with Y down, the viewer (like Altium) uses Y up."""
    return (point[0], -point[1])


# --------------------------------------------------------------------------
# .kicad_mod -> bundle records
# --------------------------------------------------------------------------
def read_placement(tree):
    """The 3D model placement stored in the footprint: (offset, rotate, scale)."""
    offset, rotate, scale = (0.0, 0.0, 0.0), (0.0, 0.0, 0.0), (1.0, 1.0, 1.0)
    for node in children(tree, "model"):
        for name in ("offset", "rotate", "scale"):
            child = first(node, name)
            if child is None:
                continue
            xyz = first(child, "xyz")
            if xyz is None:
                continue
            values = tuple(float(value) for value in xyz[1:4])
            if name == "offset":
                offset = values
            elif name == "rotate":
                rotate = values
            else:
                scale = values
        break
    return offset, rotate, scale


def read_footprint(path_text: str):
    tree = parse_sexpr(path_text)
    pads, tracks, regions = [], [], []
    offset, rotate, scale = read_placement(tree)

    for node in tree:
        if not isinstance(node, list) or not node:
            continue
        kind = node[0]

        if kind == "pad":
            name = node[1] if len(node) > 1 and isinstance(node[1], str) else ""
            pad_type = node[2] if len(node) > 2 else "smd"
            at = numbers(node, "at", 3) or [0.0, 0.0, 0.0]
            size = numbers(node, "size", 2) or [0.0, 0.0]
            drill = numbers(node, "drill", 2)
            hole = 0.0
            if drill:
                hole = max(drill)
            x, y = flip((at[0], at[1]))
            rotation = -at[2] if len(at) > 2 else 0.0
            through = pad_type in ("thru_hole", "np_thru_hole") or hole > 0
            layer = 74 if through else 1
            pads.append(
                dict(
                    name=name,
                    x=to_mil(x),
                    y=to_mil(y),
                    w=to_mil(size[0]),
                    h=to_mil(size[1]),
                    rotation=round(rotation, 4),
                    layer=layer,
                    hole=to_mil(hole),
                )
            )

        elif kind in ("fp_line", "fp_rect", "fp_circle", "fp_arc", "fp_poly"):
            if "SilkS" not in layer_of(node) and "SilkS" not in " ".join(
                value for value in node if isinstance(value, str)
            ):
                continue
            width = to_mil(width_of(node))
            if width <= 0:
                width = to_mil(0.12)
            points = []

            if kind == "fp_line":
                start = numbers(node, "start", 2)
                end = numbers(node, "end", 2)
                if start and end:
                    points = [flip(start), flip(end)]
            elif kind == "fp_rect":
                start = numbers(node, "start", 2)
                end = numbers(node, "end", 2)
                if start and end:
                    x0, y0 = start
                    x1, y1 = end
                    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
                    points = [flip(corner) for corner in corners]
            elif kind == "fp_circle":
                center = numbers(node, "center", 2)
                end = numbers(node, "end", 2)
                if center and end:
                    radius = math.hypot(end[0] - center[0], end[1] - center[1])
                    for i in range(25):
                        angle = 2 * math.pi * i / 24
                        points.append(
                            flip(
                                (
                                    center[0] + radius * math.cos(angle),
                                    center[1] + radius * math.sin(angle),
                                )
                            )
                        )
            elif kind == "fp_arc":
                start = numbers(node, "start", 2)
                mid = numbers(node, "mid", 2)
                end = numbers(node, "end", 2)
                if start and mid and end:
                    points = [flip(point) for point in arc_points(start, mid, end)]
                elif start and end:
                    points = [flip(start), flip(end)]
            else:  # fp_poly
                pts = first(node, "pts")
                if pts is not None:
                    for child in pts:
                        if isinstance(child, list) and child and child[0] == "xy":
                            points.append(flip((float(child[1]), float(child[2]))))
                if points and points[0] != points[-1]:
                    points.append(points[0])

            for a, b in zip(points, points[1:]):
                tracks.append((to_mil(a[0]), to_mil(a[1]), to_mil(b[0]), to_mil(b[1]), width))
            if len(points) > 2 and kind == "fp_poly":
                regions.append([(to_mil(x), to_mil(y)) for x, y in points])

    if any(abs(value - 1.0) > 1e-6 for value in scale):
        raise SystemExit(f"unsupported model scale {scale}")
    return pads, tracks, regions, offset, rotate


# --------------------------------------------------------------------------
# Symbols: a clean box symbol per part, with the real pin names of the package
# --------------------------------------------------------------------------
def build_symbol(part: dict):
    pins = part["pins"]  # list of (number, name, electrical)
    body_height = max(300.0, 100.0 * (len(pins) + 1))
    half_h = body_height / 2.0
    half_w = 300.0
    shapes = []
    shapes.append(dict(kind="RECT", points=[(-half_w, -half_h), (half_w, -half_h),
                                            (half_w, half_h), (-half_w, half_h), (-half_w, -half_h)],
                       width=10.0, fill=0))
    pin_records = []
    pitch = 100.0
    for index, (number, name, electrical) in enumerate(pins):
        y = (len(pins) - 1) * pitch / 2.0 - index * pitch
        pin_records.append(
            dict(number=number, name=name, electrical=electrical, x=-half_w - 100.0, y=y,
                 length=100.0, rotation=0)
        )
    return shapes, pin_records


# --------------------------------------------------------------------------
# The parts
# --------------------------------------------------------------------------
PARTS = [
    dict(
        name="TO-252-2",
        description="TO-252 (DPAK) 2-lead, tab = pin 2",
        category="Transistor / regulator",
        datasheet="https://www.vishay.com/docs/95016/dpak252a.pdf",
        manufacturer="",
        mpn="",
        footprint="Package_TO_SOT_SMD.pretty/TO-252-2.kicad_mod",
        model="Package_TO_SOT_SMD.3dshapes/TO-252-2.step",
        pins=[("1", "G", "input"), ("2", "D", "passive"), ("3", "S", "passive")],
        prefix="Q",
    ),
    dict(
        name="DO-41",
        description="DO-41 / SOD-81 axial diode, 10.16 mm pitch",
        category="Diode",
        datasheet="https://www.vishay.com/docs/88503/1n4001.pdf",
        manufacturer="Vishay",
        mpn="1N4007",
        footprint="Diode_THT.pretty/D_DO-41_SOD81_P10.16mm_Horizontal.kicad_mod",
        model="Diode_THT.3dshapes/D_DO-41_SOD81_P10.16mm_Horizontal.step",
        pins=[("1", "K", "passive"), ("2", "A", "passive")],
        prefix="D",
    ),
    dict(
        name="R-Axial-DIN0207",
        description="Axial resistor, body 6.3 x 2.5 mm, 10.16 mm pitch",
        category="Resistor",
        datasheet="https://www.vishay.com/docs/28729/pr010203.pdf",
        manufacturer="",
        mpn="",
        footprint="Resistor_THT.pretty/R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal.kicad_mod",
        model="Resistor_THT.3dshapes/R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive")],
        prefix="R",
    ),
    dict(
        name="CP-Radial-D8-P3.5",
        description="Radial electrolytic capacitor, 8 mm can, 3.5 mm pitch",
        category="Capacitor",
        datasheet="https://industrial.panasonic.com/cdbs/www-data/pdf/RDF0000/ABA0000C1053.pdf",
        manufacturer="",
        mpn="",
        footprint="Capacitor_THT.pretty/CP_Radial_D8.0mm_P3.50mm.kicad_mod",
        model="Capacitor_THT.3dshapes/CP_Radial_D8.0mm_P3.50mm.step",
        pins=[("1", "+", "passive"), ("2", "-", "passive")],
        prefix="C",
    ),
    dict(
        name="LED-5MM",
        description="5 mm through-hole LED",
        category="LED",
        datasheet="https://www.kingbrightusa.com/images/catalog/spec/wp7113srd-d.pdf",
        manufacturer="Kingbright",
        mpn="WP7113SRD/D",
        footprint="LED_THT.pretty/LED_D5.0mm.kicad_mod",
        model="LED_THT.3dshapes/LED_D5.0mm.step",
        pins=[("1", "K", "passive"), ("2", "A", "passive")],
        prefix="D",
    ),
    dict(
        name="HDR-1X04-2.54",
        description="Pin header 1x4, 2.54 mm, vertical",
        category="Connector",
        datasheet="https://www.we-online.com/components/products/datasheet/61300411121.pdf",
        manufacturer="Würth Elektronik",
        mpn="61300411121",
        footprint="Connector_PinHeader_2.54mm.pretty/PinHeader_1x04_P2.54mm_Vertical.kicad_mod",
        model="Connector_PinHeader_2.54mm.3dshapes/PinHeader_1x04_P2.54mm_Vertical.step",
        pins=[("1", "Pin_1", "passive"), ("2", "Pin_2", "passive"),
              ("3", "Pin_3", "passive"), ("4", "Pin_4", "passive")],
        prefix="J",
    ),
    dict(
        name="SOT-23",
        description="SOT-23 3-lead small outline transistor",
        category="Transistor",
        datasheet="https://www.onsemi.com/download/package-drawing/pdf/318.pdf",
        manufacturer="",
        mpn="",
        footprint="Package_TO_SOT_SMD.pretty/SOT-23.kicad_mod",
        model="Package_TO_SOT_SMD.3dshapes/SOT-23.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive"), ("3", "3", "passive")],
        prefix="Q",
    ),
    dict(
        name="SOIC-8",
        description="SOIC-8, 3.9 x 4.9 mm body, 1.27 mm pitch",
        category="IC",
        datasheet="https://www.onsemi.com/download/package-drawing/pdf/751az.pdf",
        manufacturer="",
        mpn="",
        footprint="Package_SO.pretty/SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod",
        model="Package_SO.3dshapes/SOIC-8_3.9x4.9mm_P1.27mm.step",
        pins=[(str(i), str(i), "passive") for i in range(1, 9)],
        prefix="U",
    ),
    dict(
        name="R-0603",
        description="0603 (1608 metric) chip resistor",
        category="Resistor",
        datasheet="https://www.vishay.com/docs/28729/pr010203.pdf",
        manufacturer="",
        mpn="",
        footprint="Resistor_SMD.pretty/R_0603_1608Metric.kicad_mod",
        model="Resistor_SMD.3dshapes/R_0603_1608Metric.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive")],
        prefix="R",
    ),
    dict(
        name="R-0805",
        description="0805 (2012 metric) chip resistor",
        category="Resistor",
        datasheet="https://www.vishay.com/docs/28729/pr010203.pdf",
        manufacturer="",
        mpn="",
        footprint="Resistor_SMD.pretty/R_0805_2012Metric.kicad_mod",
        model="Resistor_SMD.3dshapes/R_0805_2012Metric.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive")],
        prefix="R",
    ),
    dict(
        name="C-0603",
        description="0603 (1608 metric) chip capacitor",
        category="Capacitor",
        datasheet="https://content.kemet.com/datasheets/KEM_C1002_X7R_SMD.pdf",
        manufacturer="",
        mpn="",
        footprint="Capacitor_SMD.pretty/C_0603_1608Metric.kicad_mod",
        model="Capacitor_SMD.3dshapes/C_0603_1608Metric.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive")],
        prefix="C",
    ),
    dict(
        name="C-0805",
        description="0805 (2012 metric) chip capacitor",
        category="Capacitor",
        datasheet="https://content.kemet.com/datasheets/KEM_C1002_X7R_SMD.pdf",
        manufacturer="",
        mpn="",
        footprint="Capacitor_SMD.pretty/C_0805_2012Metric.kicad_mod",
        model="Capacitor_SMD.3dshapes/C_0805_2012Metric.step",
        pins=[("1", "1", "passive"), ("2", "2", "passive")],
        prefix="C",
    ),
]


def build():
    os.makedirs(OUT_DIR, exist_ok=True)
    pcb_lines = [
        f"|PCBV-LIB|VERSION=1|TYPE=PCB|NAME=PCB Viewer starter library|LICENSE=CC-BY-SA-4.0"
        f"|CREDIT={CREDITS}|"
    ]
    sch_lines = [
        f"|PCBV-LIB|VERSION=1|TYPE=SCH|NAME=PCB Viewer starter symbols|LICENSE=CC-BY-SA-4.0"
        f"|CREDIT={CREDITS}|"
    ]

    for index, part in enumerate(PARTS):
        print(f"[{index + 1}/{len(PARTS)}] {part['name']}", flush=True)
        footprint_text = fetch_raw(FOOTPRINTS_REPO, part["footprint"]).decode("utf-8", "replace")
        pads, tracks, regions, offset, rotate = read_footprint(footprint_text)
        model_bytes = fetch_raw(MODELS_REPO, part["model"])
        if not model_bytes.lstrip().startswith(b"ISO-10303-21"):
            raise SystemExit(f"{part['model']} is not a STEP file")

        pcb_lines.append(
            "|PART|NAME={name}|DESCRIPTION={description}|CATEGORY={category}"
            "|DATASHEET={datasheet}|MANUFACTURER={manufacturer}|MPN={mpn}|".format(**part)
        )
        for pad in pads:
            pcb_lines.append(
                "|PAD|PART={index}|NAME={name}|X={x}mil|Y={y}mil|XSIZE={w}mil|YSIZE={h}mil"
                "|LAYER={layer}|ROTATION={rotation}|HOLE={hole}mil|".format(index=index, **pad)
            )
        for x1, y1, x2, y2, width in tracks:
            pcb_lines.append(
                f"|TRACK|PART={index}|LAYER=33|X1={x1}mil|Y1={y1}mil|X2={x2}mil|Y2={y2}mil"
                f"|WIDTH={width}mil|"
            )
        for region in regions:
            points = ";".join(f"{x},{y}" for x, y in region)
            pcb_lines.append(f"|REGION|PART={index}|LAYER=33|POINTS={points}|")
        pcb_lines.append(
            f"|STEP|PART={index}|NAME={os.path.basename(part['model'])}"
            f"|DX={to_mil(offset[0])}mil|DY={to_mil(offset[1])}mil|DZ={to_mil(offset[2])}mil"
            f"|ROTX={rotate[0]}|ROTY={rotate[1]}|ROTZ={rotate[2]}|PLACED=1"
            f"|BYTES={len(model_bytes)}|"
        )
        pcb_lines.append(model_bytes.decode("utf-8", "replace").rstrip("\n"))
        pcb_lines.append(f"|ENDSTEP|PART={index}|")

        shapes, pins = build_symbol(part)
        sch_lines.append(
            "|PART|NAME={name}|DESCRIPTION={description}|CATEGORY={category}"
            "|DATASHEET={datasheet}|PREFIX={prefix}|".format(**part)
        )
        for pin in pins:
            sch_lines.append(
                "|PIN|PART={index}|NUMBER={number}|NAME={name}|X={x}mil|Y={y}mil|LENGTH={length}mil"
                "|ROTATION={rotation}|ELECTRICAL={electrical}||".format(index=index, **pin)
            )
        for shape in shapes:
            points = ";".join(f"{x},{y}" for x, y in shape["points"])
            sch_lines.append(
                f"|SHAPE|PART={index}|KIND={shape['kind']}|POINTS={points}"
                f"|WIDTH={shape['width']}mil|FILL={shape['fill']}|"
            )

    pcb_path = os.path.join(OUT_DIR, "pcbv-starter.pcbvlib")
    sch_path = os.path.join(OUT_DIR, "pcbv-symbols.pcbvlib")
    with open(pcb_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(pcb_lines) + "\n")
    with open(sch_path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write("\n".join(sch_lines) + "\n")
    print(f"wrote {pcb_path} ({os.path.getsize(pcb_path) / 1024:.0f} kB)")
    print(f"wrote {sch_path} ({os.path.getsize(sch_path) / 1024:.0f} kB)")


if __name__ == "__main__":
    try:
        build()
    except subprocess.CalledProcessError as error:
        print(f"download failed: {error}", file=sys.stderr)
        sys.exit(1)
