// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
/*
 * The few Qt GUI classes the research side's display client uses
 * (src/vmview: renderer, swapchain, listener), for the GTK build, which
 * links no Qt GUI library: there is no QGuiApplication there, and a real
 * QOpenGLContext cannot exist without one.  The client's files then build
 * as they are, and its changes keep coming over by copying them
 * (src/vmview/README.md).
 *
 * Only what those files call, with Qt's names and signatures:
 *   - QOpenGLContext: an EGL context on the display setEglDisplay() gave,
 *     desktop OpenGL as Fedora's Qt has it (its GL_BGRA uploads need it);
 *   - QOpenGLFunctions, QOpenGLExtraFunctions: the GL calls they make,
 *     resolved through eglGetProcAddress;
 *   - QSurfaceFormat: the alpha size and the swap interval the context takes;
 *   - QWindow: the format and the window state the renderer reads, set by
 *     the GTK view (thread-safe: the render thread reads them);
 *   - QImage: the guest's cursor the listener hands over (ARGB32).
 *
 * The headers named after Qt's (QOpenGLContext, QImage...) in this folder
 * include this one.  Built with Qt GUI as well, these would be a second
 * definition of Qt's classes: the GTK view must never link Qt6::Gui.
 */
#ifdef QT_GUI_LIB
#error "src/gtkview/qtgui stands in for Qt GUI: a target using it must not link Qt6::Gui"
#endif

#include <QtGlobal>
#include <QtCore/qnamespace.h>
#include <QFlags>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace QtGuiCompat {
/* The EGL display the contexts are made on (the GTK view's: its
   Wayland connection's), set before a context is created */
void setEglDisplay(EGLDisplay display);
EGLDisplay eglDisplay();
} // namespace QtGuiCompat

class QSurfaceFormat
{
public:
    void setAlphaBufferSize(int size) { m_alpha = size; }
    int alphaBufferSize() const { return m_alpha; }
    void setSwapInterval(int interval) { m_interval = interval; }
    int swapInterval() const { return m_interval; }

private:
    int m_alpha = -1;
    int m_interval = 1;
};

/* Not a window: what the renderer reads of the display's window */
class QWindow
{
public:
    void setFormat(const QSurfaceFormat &format) { m_format = format; }
    QSurfaceFormat requestedFormat() const { return m_format; }
    void setWindowStates(Qt::WindowStates states) { m_states = int(states); }
    Qt::WindowStates windowStates() const { return Qt::WindowStates(m_states.load()); }

private:
    QSurfaceFormat m_format;
    std::atomic<int> m_states{Qt::WindowNoState};
};

namespace QNativeInterface {
struct QEGLContext
{
    EGLContext nativeContext() const { return context; }
    EGLConfig config() const { return eglConfig; }
    EGLDisplay display() const { return eglDisplay; }

    EGLDisplay eglDisplay = EGL_NO_DISPLAY;
    EGLConfig eglConfig = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
};
} // namespace QNativeInterface

class QOpenGLFunctions
{
public:
    QOpenGLFunctions();

    void glBindFramebuffer(GLenum target, GLuint framebuffer) { m_bindFramebuffer(target, framebuffer); }
    void glBindTexture(GLenum target, GLuint texture) { m_bindTexture(target, texture); }
    GLenum glCheckFramebufferStatus(GLenum target) { return m_checkFramebufferStatus(target); }
    void glClear(GLbitfield mask) { m_clear(mask); }
    void glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { m_clearColor(r, g, b, a); }
    void glDeleteFramebuffers(GLsizei n, const GLuint *framebuffers) { m_deleteFramebuffers(n, framebuffers); }
    void glDeleteTextures(GLsizei n, const GLuint *textures) { m_deleteTextures(n, textures); }
    void glFinish() { m_finish(); }
    void glFlush() { m_flush(); }
    void glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture,
                                GLint level)
    {
        m_framebufferTexture2D(target, attachment, textarget, texture, level);
    }
    void glGenFramebuffers(GLsizei n, GLuint *framebuffers) { m_genFramebuffers(n, framebuffers); }
    void glGenTextures(GLsizei n, GLuint *textures) { m_genTextures(n, textures); }
    void glPixelStorei(GLenum pname, GLint param) { m_pixelStorei(pname, param); }
    void glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                      GLsizei height, GLint border, GLenum format, GLenum type, const void *pixels)
    {
        m_texImage2D(target, level, internalformat, width, height, border, format, type, pixels);
    }
    void glTexParameteri(GLenum target, GLenum pname, GLint param) { m_texParameteri(target, pname, param); }
    void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width,
                         GLsizei height, GLenum format, GLenum type, const void *pixels)
    {
        m_texSubImage2D(target, level, xoffset, yoffset, width, height, format, type, pixels);
    }
    void glViewport(GLint x, GLint y, GLsizei width, GLsizei height) { m_viewport(x, y, width, height); }

private:
    void (*m_bindFramebuffer)(GLenum, GLuint) = nullptr;
    void (*m_bindTexture)(GLenum, GLuint) = nullptr;
    GLenum (*m_checkFramebufferStatus)(GLenum) = nullptr;
    void (*m_clear)(GLbitfield) = nullptr;
    void (*m_clearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = nullptr;
    void (*m_deleteFramebuffers)(GLsizei, const GLuint *) = nullptr;
    void (*m_deleteTextures)(GLsizei, const GLuint *) = nullptr;
    void (*m_finish)() = nullptr;
    void (*m_flush)() = nullptr;
    void (*m_framebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint) = nullptr;
    void (*m_genFramebuffers)(GLsizei, GLuint *) = nullptr;
    void (*m_genTextures)(GLsizei, GLuint *) = nullptr;
    void (*m_pixelStorei)(GLenum, GLint) = nullptr;
    void (*m_texImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum,
                         const void *) = nullptr;
    void (*m_texParameteri)(GLenum, GLenum, GLint) = nullptr;
    void (*m_texSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
                            const void *) = nullptr;
    void (*m_viewport)(GLint, GLint, GLsizei, GLsizei) = nullptr;
};

class QOpenGLExtraFunctions : public QOpenGLFunctions
{
public:
    QOpenGLExtraFunctions();

    void glBlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0,
                           GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)
    {
        m_blitFramebuffer(srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter);
    }

private:
    void (*m_blitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                              GLenum) = nullptr;
};

/*
 * An EGL context of desktop OpenGL on QtGuiCompat::eglDisplay(), made and
 * used on one thread (the render thread).  It draws only to surfaces its
 * user makes current itself (the GTK view's render target): makeCurrent()
 * and swapBuffers() of a QWindow do nothing.
 */
class QOpenGLContext
{
public:
    QOpenGLContext() = default;
    ~QOpenGLContext();
    QOpenGLContext(const QOpenGLContext &) = delete;
    QOpenGLContext &operator=(const QOpenGLContext &) = delete;

    void setFormat(const QSurfaceFormat &format) { m_format = format; }
    QSurfaceFormat format() const { return m_format; }
    bool create();
    bool isValid() const { return m_egl.context != EGL_NO_CONTEXT; }
    bool makeCurrent(QWindow *window);
    void swapBuffers(QWindow *window);
    void doneCurrent();
    GLuint defaultFramebufferObject() const { return 0; }
    QFunctionPointer getProcAddress(const char *name) const;
    QOpenGLFunctions *functions() const;
    QOpenGLExtraFunctions *extraFunctions() const;

    template <typename NativeInterface>
    NativeInterface *nativeInterface() const;

private:
    QSurfaceFormat m_format;
    mutable QNativeInterface::QEGLContext m_egl;
    mutable std::unique_ptr<QOpenGLExtraFunctions> m_functions;
};

template <>
inline QNativeInterface::QEGLContext *
QOpenGLContext::nativeInterface<QNativeInterface::QEGLContext>() const
{
    return isValid() ? &m_egl : nullptr;
}

/* A copy of pixels, or a view of someone's until copy() */
class QImage
{
public:
    /* Qt's values */
    enum Format { Format_Invalid = 0, Format_ARGB32 = 5 };

    QImage() = default;
    QImage(const uchar *data, int width, int height, qsizetype bytesPerLine, Format format)
        : m_data(data), m_width(width), m_height(height), m_stride(bytesPerLine),
          m_format(format)
    {
    }

    QImage copy() const;
    bool isNull() const { return !m_data || m_width <= 0 || m_height <= 0; }
    int width() const { return m_width; }
    int height() const { return m_height; }
    qsizetype bytesPerLine() const { return m_stride; }
    const uchar *constBits() const { return m_data; }

private:
    std::shared_ptr<const std::vector<uchar>> m_owned;
    const uchar *m_data = nullptr;
    int m_width = 0, m_height = 0;
    qsizetype m_stride = 0;
    Format m_format = Format_Invalid;
};
