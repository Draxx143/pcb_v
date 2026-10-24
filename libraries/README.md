# The part library

One folder layout for every part the viewer knows, so a board, a schematic and a 3D model
always agree on where things live:

```
libraries/
  index.json         the part database: MPN -> symbol, footprint, 3D model, datasheet
  symbols/           .kicad_sym  (one library file may hold many symbols)
  footprints/        .kicad_mod  (one footprint per file; Altium .PcbLib works too)
  models3d/          .step       (the real bodies; .wrl is read as well)
  datasheets/        .pdf        (optional local copies)
```

The viewer looks for `libraries/footprints/` next to a board, next to the running binary and
in the current folder, so copying this folder next to your `.PcbDoc` is enough to get real
3D bodies on the board.

## index.json

```json
{
  "format": "pcbv-part-index",
  "version": 1,
  "parts": [
    {
      "id": "R-0603-10k",
      "mpn": "RC0603FR-0710KL",
      "manufacturer": "Yageo",
      "value": "10k",
      "package": "0603",
      "description": "10 kΩ 1 % thick film resistor",
      "datasheet": "datasheets/RC0603.pdf",
      "symbol": "symbols/Device_R.kicad_sym",
      "footprint": "footprints/R_0603_1608Metric.kicad_mod",
      "model": "models3d/R_0603_1608Metric.step",
      "tags": ["resistor", "smd"]
    }
  ]
}
```

Paths are relative to `index.json`; an absolute path or a `http(s)://` URL is left alone.
Only `id` (or `mpn`) is required.

## Commands

```bash
# list the database and verify that every referenced file exists
./build/pcbv-cli --index libraries/index.json

# find parts by number, package, value or tag (every word must match)
./build/pcbv-cli --search "0603 10k" libraries/index.json
./build/pcbv-cli --search "yageo resistor" libraries/index.json

# import footprints, symbols and models from a KiCad installation
python3 tools/import_kicad.py --footprints /usr/share/kicad/footprints/Resistor_SMD.pretty \
                              --symbols    /usr/share/kicad/symbols/Device.kicad_sym \
                              --models3d   /usr/share/kicad/3dmodels \
                              --index      libraries/index.json
```

`import_kicad.py` copies nothing by default: it records the files in `index.json` and, with
`--copy`, copies the selected footprints, symbols and 3D models into this folder so the
project stays self-contained.

## Licensing

KiCad's libraries and 3D models are CC-BY-SA 4.0. Keep the `Credit` line that the import
tool writes into `index.json` when you redistribute them.
