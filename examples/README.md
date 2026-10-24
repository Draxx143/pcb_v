# Demo board

`demo-board.PcbDoc` is a tiny ASCII-saved Altium board (8 parts) that exists so the 3D
preview can be tried without any file of your own. Its footprint names are the ones a real
Altium library would use (`DPAK`, `DO41`, `AXIAL-0.4`, `CAP-8x12`, `LED5`, `HDR1X4`,
`SOT23`, `SO8`), which is exactly the case where the viewer has to recognise the part from
its pads instead of its name.

```bash
cd ~/project_pcb_problem_finder
./build/pcbv-cli --3d ~/demo-3d.bmp examples/demo-board.PcbDoc
```

Expected output:

```
library: 8 part(s) matched in 1 file(s)
3D: 8 of 8 part(s) use a real model, the rest get the generated body
wrote /home/<you>/demo-3d.bmp (3D board, 1600x1000)
```

The `.bmp` opens in any image viewer (and in a browser). Inside the app, open the same file
and press `3` for the 3D tab.

The board is a fixture, not a real design: it has no nets and no copper other than the pads.
