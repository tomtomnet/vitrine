// SPDX-License-Identifier: GPL-2.0-or-later
#include "qtguicompat.h"

#include "gtkview/viewthreads.h"

#include <cstdio>

namespace {

std::atomic<EGLDisplay> g_display{EGL_NO_DISPLAY};

template <typename F>
void resolve(F &fn, const char *name)
{
    fn = reinterpret_cast<F>(eglGetProcAddress(name));
    if (!fn) {
        fprintf(stderr, "vitrine: display: OpenGL lacks %s\n", name);
    }
}

/* The config of @format's sizes, as Qt picks one: the exact alpha size first */
EGLConfig chooseConfig(EGLDisplay dpy, const QSurfaceFormat &format)
{
    const EGLint alpha = format.alphaBufferSize() > 0 ? format.alphaBufferSize() : 0;
    const EGLint attrs[] = {EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                            EGL_ALPHA_SIZE, alpha, EGL_NONE};
    EGLConfig configs[64];
    EGLint n = 0;

    if (!eglChooseConfig(dpy, attrs, configs, 64, &n) || n <= 0) {
        return nullptr;
    }
    for (EGLint i = 0; i < n; i++) {
        EGLint r = 0, g = 0, b = 0, a = 0;
        eglGetConfigAttrib(dpy, configs[i], EGL_RED_SIZE, &r);
        eglGetConfigAttrib(dpy, configs[i], EGL_GREEN_SIZE, &g);
        eglGetConfigAttrib(dpy, configs[i], EGL_BLUE_SIZE, &b);
        eglGetConfigAttrib(dpy, configs[i], EGL_ALPHA_SIZE, &a);
        if (r == 8 && g == 8 && b == 8 && a == alpha) {
            return configs[i];
        }
    }
    return configs[0];
}

} // namespace

void QtGuiCompat::setEglDisplay(EGLDisplay display)
{
    g_display = display;
}

EGLDisplay QtGuiCompat::eglDisplay()
{
    return g_display;
}

QOpenGLFunctions::QOpenGLFunctions()
{
    resolve(m_bindFramebuffer, "glBindFramebuffer");
    resolve(m_bindTexture, "glBindTexture");
    resolve(m_checkFramebufferStatus, "glCheckFramebufferStatus");
    resolve(m_clear, "glClear");
    resolve(m_clearColor, "glClearColor");
    resolve(m_deleteFramebuffers, "glDeleteFramebuffers");
    resolve(m_deleteTextures, "glDeleteTextures");
    resolve(m_finish, "glFinish");
    resolve(m_flush, "glFlush");
    resolve(m_framebufferTexture2D, "glFramebufferTexture2D");
    resolve(m_genFramebuffers, "glGenFramebuffers");
    resolve(m_genTextures, "glGenTextures");
    resolve(m_pixelStorei, "glPixelStorei");
    resolve(m_texImage2D, "glTexImage2D");
    resolve(m_texParameteri, "glTexParameteri");
    resolve(m_texSubImage2D, "glTexSubImage2D");
    resolve(m_viewport, "glViewport");
}

QOpenGLExtraFunctions::QOpenGLExtraFunctions()
{
    resolve(m_blitFramebuffer, "glBlitFramebuffer");
}

QOpenGLContext::~QOpenGLContext()
{
    if (m_egl.context == EGL_NO_CONTEXT) {
        return;
    }
    if (eglGetCurrentContext() == m_egl.context) {
        doneCurrent();
    }
    eglDestroyContext(m_egl.eglDisplay, m_egl.context);
}

bool QOpenGLContext::create()
{
    if (isValid()) {
        return true;
    }
    const EGLDisplay dpy = QtGuiCompat::eglDisplay();
    if (dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "vitrine: display: no EGL display for the render thread\n");
        return false;
    }
    /* per thread: the one that makes the context current binds it too */
    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "vitrine: display: EGL has no desktop OpenGL: 0x%x\n", eglGetError());
        return false;
    }
    const EGLConfig config = chooseConfig(dpy, m_format);
    if (!config) {
        fprintf(stderr, "vitrine: display: no EGL config for the view: 0x%x\n", eglGetError());
        return false;
    }
    /* the driver starts the context's own threads here (Mesa's glthread,
       its threaded context): the render thread's, raised with it while the
       VM is in front - marked, to tell them from those of GTK's contexts,
       which Mesa names alike */
    ViewThreads::Mark mark;
    const EGLContext context = eglCreateContext(dpy, config, EGL_NO_CONTEXT, nullptr);
    m_startedThreads = mark.end();
    if (context == EGL_NO_CONTEXT) {
        fprintf(stderr, "vitrine: display: no EGL context: 0x%x\n", eglGetError());
        return false;
    }
    m_egl.eglDisplay = dpy;
    m_egl.eglConfig = config;
    m_egl.context = context;
    return true;
}

bool QOpenGLContext::makeCurrent(QWindow *)
{
    /* the GTK view always gives the renderer a target of its own */
    fprintf(stderr, "vitrine: display: a QWindow is no surface here\n");
    return false;
}

void QOpenGLContext::swapBuffers(QWindow *)
{
}

void QOpenGLContext::doneCurrent()
{
    if (m_egl.eglDisplay != EGL_NO_DISPLAY) {
        eglMakeCurrent(m_egl.eglDisplay, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }
}

QFunctionPointer QOpenGLContext::getProcAddress(const char *name) const
{
    return reinterpret_cast<QFunctionPointer>(eglGetProcAddress(name));
}

QOpenGLFunctions *QOpenGLContext::functions() const
{
    return extraFunctions();
}

QOpenGLExtraFunctions *QOpenGLContext::extraFunctions() const
{
    if (!m_functions) {
        m_functions = std::make_unique<QOpenGLExtraFunctions>();
    }
    return m_functions.get();
}

QImage QImage::copy() const
{
    if (isNull()) {
        return QImage();
    }
    auto owned = std::make_shared<std::vector<uchar>>(m_data, m_data + size_t(m_stride) * size_t(m_height));
    QImage image(owned->data(), m_width, m_height, m_stride, m_format);
    image.m_owned = std::move(owned);
    return image;
}
