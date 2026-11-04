// SPDX-License-Identifier: GPL-2.0-or-later
#include "framestill.h"

#include "vmview/framemailbox.h"

#include <gdk/gdk.h>

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <unistd.h>
#include <vector>

namespace {

/* The rows of the visible rectangle, upright, 4 bytes a pixel */
GdkTexture *cropped(const uint8_t *data, size_t stride, uint32_t x, uint32_t y, uint32_t width,
                    uint32_t height, bool flip)
{
    const size_t rowBytes = size_t(width) * 4;
    auto *out = static_cast<uint8_t *>(g_malloc(rowBytes * height));
    for (uint32_t row = 0; row < height; row++) {
        const uint32_t from = flip ? y + height - 1 - row : y + row;
        memcpy(out + rowBytes * row, data + stride * from + size_t(x) * 4, rowBytes);
    }
    GBytes *bytes = g_bytes_new_take(out, rowBytes * height);
    GdkTexture *texture = gdk_memory_texture_new(int(width), int(height), GDK_MEMORY_B8G8R8X8,
                                                 bytes, rowBytes);
    g_bytes_unref(bytes);
    return texture;
}

struct Fds {
    std::vector<int> fds;
    ~Fds()
    {
        for (int fd : fds) {
            close(fd);
        }
    }
};

GdkTexture *dmabufStill(const Scanout &s, GdkDisplay *display)
{
    if (s.planes.empty() || s.x + s.width > s.backingWidth || s.y + s.height > s.backingHeight) {
        return nullptr;
    }
    auto *fds = new Fds;
    GdkDmabufTextureBuilder *builder = gdk_dmabuf_texture_builder_new();
    gdk_dmabuf_texture_builder_set_display(builder, display);
    gdk_dmabuf_texture_builder_set_width(builder, s.backingWidth);
    gdk_dmabuf_texture_builder_set_height(builder, s.backingHeight);
    gdk_dmabuf_texture_builder_set_fourcc(builder, s.fourcc);
    gdk_dmabuf_texture_builder_set_modifier(builder, s.modifier);
    gdk_dmabuf_texture_builder_set_premultiplied(builder, TRUE);
    gdk_dmabuf_texture_builder_set_n_planes(builder, unsigned(s.planes.size()));
    for (unsigned i = 0; i < s.planes.size(); i++) {
        /* the texture's own: the scanout keeps its fds */
        const int fd = fcntl(s.planes[i].fd, F_DUPFD_CLOEXEC, 0);
        fds->fds.push_back(fd);
        gdk_dmabuf_texture_builder_set_fd(builder, i, fd);
        gdk_dmabuf_texture_builder_set_stride(builder, i, s.planes[i].stride);
        gdk_dmabuf_texture_builder_set_offset(builder, i, s.planes[i].offset);
    }
    GError *error = nullptr;
    GdkTexture *texture = gdk_dmabuf_texture_builder_build(
        builder, [](gpointer data) { delete static_cast<Fds *>(data); }, fds, &error);
    g_object_unref(builder);
    if (!texture) {
        fprintf(stderr, "vitrine: display: no still of the guest's frame: %s\n",
                error ? error->message : "GTK refused the buffer");
        g_clear_error(&error);
        delete fds;
        return nullptr;
    }
    /* frozen: read back now, the buffer is the guest's */
    GdkTextureDownloader *downloader = gdk_texture_downloader_new(texture);
    gdk_texture_downloader_set_format(downloader, GDK_MEMORY_B8G8R8X8);
    gsize stride = 0;
    GBytes *bytes = gdk_texture_downloader_download_bytes(downloader, &stride);
    gdk_texture_downloader_free(downloader);
    g_object_unref(texture);
    gsize size = 0;
    const auto *data = static_cast<const uint8_t *>(g_bytes_get_data(bytes, &size));
    GdkTexture *still = nullptr;
    if (data && size >= stride * s.backingHeight) {
        /* row 0 is the top unless y0_top (QEMU's egl_fb_blit, the renderer) */
        still = cropped(data, stride, s.x, s.y, s.width, s.height, s.y0Top);
    }
    g_bytes_unref(bytes);
    return still;
}

} // namespace

GdkTexture *takeFrameStill(FrameMailbox *mailbox, GdkDisplay *display)
{
    std::shared_ptr<Scanout> scanout;
    std::vector<uint8_t> pixels;
    {
        std::lock_guard g(mailbox->lock);
        scanout = mailbox->scanout;
        /* an Image scanout's pixels change under the lock */
        if (scanout && scanout->kind == Scanout::Image) {
            pixels = scanout->pixels;
        }
    }
    if (!scanout || !scanout->width || !scanout->height) {
        return nullptr;
    }
    const Scanout &s = *scanout;
    switch (s.kind) {
    case Scanout::Dmabuf:
        return dmabufStill(s, display);
    case Scanout::Image:
        if (pixels.size() < size_t(s.stride) * s.height) {
            return nullptr;
        }
        /* pixman x8r8g8b8 or a8r8g8b8: B, G, R, X in memory; drawn opaque */
        return cropped(pixels.data(), s.stride, 0, 0, s.width, s.height, false);
    case Scanout::Map:
        if (!s.map || s.mapOffset + size_t(s.stride) * s.height > s.mapLength) {
            return nullptr;
        }
        return cropped(static_cast<const uint8_t *>(s.map) + s.mapOffset, s.stride, 0, 0, s.width,
                       s.height, false);
    }
    return nullptr;
}
