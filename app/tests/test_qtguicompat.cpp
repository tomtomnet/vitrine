// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * The Qt GUI stand-ins of the GTK view (src/gtkview/qtgui): what the
 * research side's renderer and listener ask of QImage, QWindow,
 * QSurfaceFormat and QOpenGLContext.  The context is made for real on the
 * host's render node (GBM, no window), and does what the renderer does
 * with it: a 2D scanout's GL_BGRA upload into a framebuffer, and a blit.
 */
#include <QOpenGLContext>
#include <QOpenGLExtraFunctions>
#include <QTest>

#include <cstring>
#include <fcntl.h>
#include <gbm.h>
#include <unistd.h>

#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif

class TestQtGuiCompat : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    /* The listener's cursor: a view of QEMU's bytes, then a copy of its own */
    void image()
    {
        QVERIFY(QImage().isNull());
        QVERIFY(QImage().copy().isNull());
        uchar pixels[2 * 2 * 4];
        for (size_t i = 0; i < sizeof(pixels); i++) {
            pixels[i] = uchar(i);
        }
        const QImage view(pixels, 2, 2, 8, QImage::Format_ARGB32);
        QVERIFY(!view.isNull());
        QVERIFY(view.constBits() == pixels);
        QImage copy = view.copy();
        QVERIFY(copy.constBits() != pixels);
        QCOMPARE(copy.width(), 2);
        QCOMPARE(copy.height(), 2);
        QCOMPARE(copy.bytesPerLine(), qsizetype(8));
        pixels[5] = 200;
        QCOMPARE(copy.constBits()[5], uchar(5));
        /* copies share the pixels they own, which outlive the first */
        QImage second = copy;
        copy = QImage();
        QCOMPARE(second.constBits()[7], uchar(7));
        QVERIFY(QImage(nullptr, 2, 2, 8, QImage::Format_ARGB32).isNull());
    }

    /* What the renderer reads of its window: the format, full screen or not */
    void window()
    {
        QWindow window;
        QCOMPARE(window.windowStates(), Qt::WindowStates(Qt::WindowNoState));
        QVERIFY(!(window.windowStates() & Qt::WindowFullScreen));
        window.setWindowStates(Qt::WindowFullScreen);
        QVERIFY(window.windowStates() & Qt::WindowFullScreen);
        QSurfaceFormat format;
        QCOMPARE(format.alphaBufferSize(), -1);
        QCOMPARE(format.swapInterval(), 1);     // Qt's default
        format.setAlphaBufferSize(0);
        format.setSwapInterval(0);
        window.setFormat(format);
        QCOMPARE(window.requestedFormat().alphaBufferSize(), 0);
        QCOMPARE(window.requestedFormat().swapInterval(), 0);
    }

    /* Without a display set, no context, and no crash: the renderer reports it */
    void noDisplay()
    {
        QtGuiCompat::setEglDisplay(EGL_NO_DISPLAY);
        QOpenGLContext ctx;
        QVERIFY(!ctx.create());
        QVERIFY(!ctx.isValid());
        QVERIFY(!ctx.nativeInterface<QNativeInterface::QEGLContext>());
        QWindow window;
        QVERIFY(!ctx.makeCurrent(&window));
        QCOMPARE(ctx.defaultFramebufferObject(), GLuint(0));
    }

    void renderNode()
    {
        const int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
        if (fd < 0) {
            QSKIP("no render node to make a context on");
        }
        gbm_device *gbm = gbm_create_device(fd);
        EGLDisplay dpy = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, nullptr)
                             : EGL_NO_DISPLAY;
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, nullptr, nullptr)) {
            if (gbm) {
                gbm_device_destroy(gbm);
            }
            close(fd);
            QSKIP("no EGL on the render node");
        }
        QtGuiCompat::setEglDisplay(dpy);
        {
            QOpenGLContext ctx;
            QSurfaceFormat format;
            format.setAlphaBufferSize(0);
            format.setSwapInterval(0);
            ctx.setFormat(format);
            QVERIFY(ctx.create());
            QCOMPARE(ctx.format().swapInterval(), 0);
            auto *egl = ctx.nativeInterface<QNativeInterface::QEGLContext>();
            QVERIFY(egl);
            QCOMPARE(egl->display(), dpy);
            QVERIFY(egl->config());
            EGLint alpha = -1, surfaces = 0;
            eglGetConfigAttrib(dpy, egl->config(), EGL_ALPHA_SIZE, &alpha);
            eglGetConfigAttrib(dpy, egl->config(), EGL_SURFACE_TYPE, &surfaces);
            QCOMPARE(alpha, 0);                 // opaque, as the renderer asks
            QVERIFY(surfaces & EGL_WINDOW_BIT); // the view's EGL window surface
            QVERIFY(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl->nativeContext()));
            QVERIFY(ctx.getProcAddress("glEGLImageTargetTexture2DOES"));

            /* a 2D scanout's upload: pixman x8r8g8b8, B G R X in memory */
            QOpenGLFunctions *f = ctx.functions();
            QOpenGLExtraFunctions *ef = ctx.extraFunctions();
            const uint8_t pixels[4 * 4] = {0x10, 0x20, 0x30, 0xff, 0x10, 0x20, 0x30, 0xff,
                                           0x10, 0x20, 0x30, 0xff, 0x10, 0x20, 0x30, 0xff};
            GLuint tex[2] = {}, fbo[2] = {};
            f->glGenTextures(2, tex);
            f->glGenFramebuffers(2, fbo);
            for (int i = 0; i < 2; i++) {
                f->glBindTexture(GL_TEXTURE_2D, tex[i]);
                f->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 2, 2, 0, GL_BGRA, GL_UNSIGNED_BYTE,
                                nullptr);
                f->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                f->glBindFramebuffer(GL_FRAMEBUFFER, fbo[i]);
                f->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                          tex[i], 0);
                QCOMPARE(f->glCheckFramebufferStatus(GL_FRAMEBUFFER), GLenum(GL_FRAMEBUFFER_COMPLETE));
            }
            f->glBindTexture(GL_TEXTURE_2D, tex[0]);
            f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 2);
            f->glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
            f->glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            /* the renderer's draw: clear, then a blit */
            f->glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
            f->glViewport(0, 0, 2, 2);
            f->glClearColor(0, 0, 0, 1);
            f->glClear(GL_COLOR_BUFFER_BIT);
            f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
            ef->glBlitFramebuffer(0, 0, 2, 2, 0, 0, 2, 2, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            f->glFinish();
            /* read back (not a call the renderer makes: resolved here) */
            auto readPixels = reinterpret_cast<void (*)(GLint, GLint, GLsizei, GLsizei, GLenum,
                                                        GLenum, void *)>(
                eglGetProcAddress("glReadPixels"));
            QVERIFY(readPixels);
            uint8_t out[4] = {};
            f->glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[1]);
            readPixels(1, 1, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, out);
            QCOMPARE(out[0], uint8_t(0x30));    // red
            QCOMPARE(out[1], uint8_t(0x20));
            QCOMPARE(out[2], uint8_t(0x10));    // blue
            f->glDeleteFramebuffers(2, fbo);
            f->glDeleteTextures(2, tex);
            f->glFlush();
            ctx.doneCurrent();
            QCOMPARE(eglGetCurrentContext(), EGL_NO_CONTEXT);
        }
        QtGuiCompat::setEglDisplay(EGL_NO_DISPLAY);
        eglTerminate(dpy);
        gbm_device_destroy(gbm);
        close(fd);
    }
};

QTEST_GUILESS_MAIN(TestQtGuiCompat)
#include "test_qtguicompat.moc"
