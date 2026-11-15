#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every shared object of the GL extensions a sandbox has (/usr/lib/x86_64-linux-gnu/GL/*), loaded
with dlopen(RTLD_NOW), each in a process of its own: a library or a symbol the runtime lacks shows
here (vitrine's Mesa for Flatpak apps, one build for every runtime, 2026-11-15).
    python3 gl_load.py"""
import os, subprocess, sys

root = '/usr/lib/x86_64-linux-gnu/GL'
names = sorted(os.listdir(root)) if os.path.isdir(root) else []
print('GL extensions mounted: ' + (', '.join(names) or 'none'))
objects = []
for top, dirs, files in os.walk(root):
    dirs.sort()
    for f in sorted(files):
        p = os.path.join(top, f)
        if ('.so' in f) and os.path.isfile(p) and not os.path.islink(p):
            objects.append(p)
bad = []
for p in objects:
    r = subprocess.run([sys.executable, '-c', 'import ctypes, os, sys; ctypes.CDLL(sys.argv[1], mode=os.RTLD_NOW)', p],
                       capture_output=True, text=True)
    if r.returncode != 0:
        lines = [l for l in r.stderr.strip().split('\n') if l]
        bad.append((p[len(root) + 1:], lines[-1] if lines else f'status {r.returncode}'))
print(f'shared objects: {len(objects) - len(bad)} of {len(objects)} load')
for p, why in bad:
    print(f'  NOT LOADED {p}: {why}')
