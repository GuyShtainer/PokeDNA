#!/usr/bin/env python3
"""Boot retail Emerald in headless mGBA on a prepared .sav, press through Continue,
walk around Lilycove Museum 2F and save PNG frames. usage: g3_retail_walk.py ROM SAV OUTDIR [script]
script = comma list of steps: 'wN' wait N frames, 'A','B','START','UP','DOWN','LEFT','RIGHT' tap (held 8 frames + 12 settle), 'sNAME' shot.
Note: the Lilycove Museum 2F curator's first-visit speech needs ~30 slow B taps (B,w45) -- 14 fast taps leave it on screen."""
import sys, os
sys.path.insert(0, "/Users/guyshtainer/VSCodeProjects/gba-toolkit/projects/rec2mp4/vendor")
import mgba.core, mgba.image, mgba.log, mgba.vfs
from PIL import Image
mgba.log.silence()
rom, sav, out = sys.argv[1], sys.argv[2], sys.argv[3]
os.makedirs(out, exist_ok=True)
core = mgba.core.load_path(rom)
img = mgba.image.Image(*core.desired_video_dimensions())
core.set_video_buffer(img)
data = open(sav, "rb").read()
vf = mgba.vfs.VFile.fromEmpty()
vf.write(data, len(data)); vf.seek(0, 0)
core.load_save(vf)
core.reset()
KEYS = {"A": 0, "B": 1, "SELECT": 2, "START": 3, "RIGHT": 4, "LEFT": 5, "UP": 6, "DOWN": 7, "R": 8, "L": 9}
def run(n):
    for _ in range(n): core.run_frame()
def shot(name):
    p = os.path.join(out, name + ".png")
    img.to_pil().convert("RGB").save(p); print("shot", p)
def tap(k, hold=8, settle=16):
    core.set_keys(raw=1 << KEYS[k]); run(hold); core.set_keys(raw=0); run(settle)
script = sys.argv[4] if len(sys.argv) > 4 else "w400,s00_boot"
for step in script.split(","):
    step = step.strip()
    if not step: continue
    if step[0] == "w": run(int(step[1:]))
    elif step[0] == "s": shot(step[1:])
    elif step.startswith("hold"):   # holdDIR:N -> hold a direction N frames (walk)
        k, n = step[4:].split(":"); core.set_keys(raw=1 << KEYS[k]); run(int(n)); core.set_keys(raw=0); run(8)
    else: tap(step)
print("frames done")
