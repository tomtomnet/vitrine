#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""A frame through the GL driver a sandbox gets, without a window: EGL on the surfaceless platform
(the render node), the driver's name and GL_RENDERER, then a triangle from a shader into a
framebuffer object and its centre pixel read back. Pure ctypes (vitrine's Mesa for Flatpak apps,
2026-11-15). Run with LIBGL_DEBUG=verbose EGL_LOG_LEVEL=debug for the loader's lines on stderr.
    python3 egl_render.py"""
import ctypes

egl = ctypes.CDLL('libEGL.so.1')
gl = ctypes.CDLL('libGLESv2.so.2')
vp, ci, cu, cf = ctypes.c_void_p, ctypes.c_int, ctypes.c_uint, ctypes.c_float
egl.eglGetProcAddress.restype = vp
egl.eglGetProcAddress.argtypes = [ctypes.c_char_p]
egl.eglGetPlatformDisplay.restype = vp
egl.eglGetPlatformDisplay.argtypes = [cu, vp, vp]
egl.eglInitialize.argtypes = [vp, ctypes.POINTER(ci), ctypes.POINTER(ci)]
egl.eglQueryString.restype = ctypes.c_char_p
egl.eglQueryString.argtypes = [vp, ci]
egl.eglCreateContext.restype = vp
egl.eglCreateContext.argtypes = [vp, vp, vp, vp]
egl.eglMakeCurrent.argtypes = [vp, vp, vp, vp]
egl.eglGetError.restype = ci
gl.glGetString.restype = ctypes.c_char_p
gl.glGetString.argtypes = [cu]
gl.glClearColor.argtypes = [cf] * 4
gl.glShaderSource.argtypes = [cu, ci, ctypes.POINTER(ctypes.c_char_p), vp]
gl.glVertexAttribPointer.argtypes = [cu, ci, cu, ctypes.c_ubyte, ci, vp]
gl.glTexImage2D.argtypes = [cu, ci, ci, ci, ci, ci, cu, cu, vp]
gl.glReadPixels.argtypes = [ci, ci, ci, ci, cu, cu, vp]
gl.glCreateShader.restype = cu
gl.glCreateProgram.restype = cu

dpy = egl.eglGetPlatformDisplay(0x31DD, None, None)   # EGL_PLATFORM_SURFACELESS_MESA
major, minor = ci(), ci()
if not dpy or not egl.eglInitialize(dpy, ctypes.byref(major), ctypes.byref(minor)):
    raise SystemExit(f'egl: eglInitialize failed, error 0x{egl.eglGetError():x}')
f = egl.eglGetProcAddress(b'eglGetDisplayDriverName')
drv = ctypes.CFUNCTYPE(ctypes.c_char_p, vp)(f)(dpy) if f else None
print(f'egl: EGL {major.value}.{minor.value}, driver {drv.decode() if drv else "?"}')
egl.eglBindAPI(0x30A0)   # EGL_OPENGL_ES_API
ctx = egl.eglCreateContext(dpy, None, None, (ci * 3)(0x3098, 2, 0x3038))
if not ctx or not egl.eglMakeCurrent(dpy, None, None, ctx):
    raise SystemExit(f'egl: no context, error 0x{egl.eglGetError():x}')
for what, enum in (('GL_RENDERER', 0x1F01), ('GL_VERSION', 0x1F02)):
    s = gl.glGetString(enum)
    print(f'egl: {what}: {s.decode() if s else None}')

# a 64x64 texture to draw into
tex, fbo = cu(), cu()
gl.glGenTextures(1, ctypes.byref(tex))
gl.glBindTexture(0x0DE1, tex)
gl.glTexImage2D(0x0DE1, 0, 0x1908, 64, 64, 0, 0x1908, 0x1401, None)   # GL_RGBA, GL_UNSIGNED_BYTE
gl.glGenFramebuffers(1, ctypes.byref(fbo))
gl.glBindFramebuffer(0x8D40, fbo)
gl.glFramebufferTexture2D(0x8D40, 0x8CE0, 0x0DE1, tex, 0)
if gl.glCheckFramebufferStatus(0x8D40) != 0x8CD5:
    raise SystemExit('egl: the framebuffer is not complete')
gl.glViewport(0, 0, 64, 64)
gl.glClearColor(0.0, 0.0, 1.0, 1.0)
gl.glClear(0x4000)


def shader(kind, text):
    s = gl.glCreateShader(kind)
    src = ctypes.c_char_p(text)
    gl.glShaderSource(s, 1, ctypes.byref(src), None)
    gl.glCompileShader(s)
    ok = ci()
    gl.glGetShaderiv(s, 0x8B81, ctypes.byref(ok))
    if not ok.value:
        raise SystemExit('egl: a shader does not compile')
    return s


prog = gl.glCreateProgram()
gl.glAttachShader(prog, shader(0x8B31, b'attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }'))
gl.glAttachShader(prog, shader(0x8B30, b'precision mediump float; void main() { gl_FragColor = vec4(0.25, 0.5, 0.75, 1.0); }'))
gl.glBindAttribLocation(prog, 0, b'p')
gl.glLinkProgram(prog)
ok = ci()
gl.glGetProgramiv(prog, 0x8B82, ctypes.byref(ok))
if not ok.value:
    raise SystemExit('egl: the program does not link')
gl.glUseProgram(prog)
verts = (cf * 6)(-1.0, -1.0, 3.0, -1.0, -1.0, 3.0)   # one triangle over the whole view
gl.glEnableVertexAttribArray(0)
gl.glVertexAttribPointer(0, 2, 0x1406, 0, 0, verts)
gl.glDrawArrays(0x0004, 0, 3)
px = (ctypes.c_ubyte * 4)()
gl.glReadPixels(32, 32, 1, 1, 0x1908, 0x1401, px)
got = tuple(px)
good = all(abs(a - b) <= 1 for a, b in zip(got, (64, 128, 191, 255)))
print(f'egl: drawn pixel {got}: {"as expected" if good else "WRONG, expected (64, 128, 191, 255)"}')
