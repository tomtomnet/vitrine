#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The hardware video (VA-API) a sandbox gets on the render node: the driver's name and the codecs
it has. Pure ctypes, for runtimes without vainfo (vitrine's Mesa for Flatpak apps, 2026-11-15).
    python3 va_info.py [/dev/dri/renderD128]"""
import ctypes, os, sys

try:
    va = ctypes.CDLL('libva.so.2')
    vadrm = ctypes.CDLL('libva-drm.so.2')
except OSError as e:
    raise SystemExit(f'va: no libva in this runtime ({e})')
vp, ci = ctypes.c_void_p, ctypes.c_int
vadrm.vaGetDisplayDRM.restype = vp
va.vaInitialize.argtypes = [vp, ctypes.POINTER(ci), ctypes.POINTER(ci)]
va.vaErrorStr.restype = ctypes.c_char_p
va.vaQueryVendorString.restype = ctypes.c_char_p
va.vaQueryVendorString.argtypes = [vp]
va.vaMaxNumProfiles.argtypes = [vp]
va.vaQueryConfigProfiles.argtypes = [vp, ctypes.POINTER(ci), ctypes.POINTER(ci)]
va.vaTerminate.argtypes = [vp]
node = sys.argv[1] if len(sys.argv) > 1 else '/dev/dri/renderD128'
dpy = vadrm.vaGetDisplayDRM(os.open(node, os.O_RDWR))
major, minor = ci(), ci()
st = va.vaInitialize(dpy, ctypes.byref(major), ctypes.byref(minor))
if st != 0:
    raise SystemExit(f'va: vaInitialize failed: {va.vaErrorStr(st).decode()}')
vendor = va.vaQueryVendorString(dpy)
profiles = (ci * va.vaMaxNumProfiles(dpy))()
num = ci()
va.vaQueryConfigProfiles(dpy, profiles, ctypes.byref(num))
codecs = []
for name, ids in (('MPEG-2', (0, 1)), ('H.264', (5, 6, 7, 13)), ('VC-1', (8, 9, 10)), ('JPEG', (12,)),
                  ('H.265', (17, 18)), ('VP9', (19, 20, 21, 22)), ('AV1', (32, 33))):
    if any(p in ids for p in profiles[:num.value]):
        codecs.append(name)
print(f'va: VA-API {major.value}.{minor.value}, {vendor.decode() if vendor else "?"}')
print(f'va: {num.value} profiles: {", ".join(codecs) or "no known codec"}')
va.vaTerminate(dpy)
