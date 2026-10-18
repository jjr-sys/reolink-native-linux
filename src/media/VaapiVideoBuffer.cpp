#include "VaapiVideoBuffer.h"

#ifdef RL_HAVE_VAAPI_DMABUF_RHI

#include <QtGui/private/qrhi_p.h>
#include <QtMultimedia/private/qhwvideobuffer_p.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

extern "C" {
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_drm.h>
#include <libavutil/pixfmt.h>
}

#include <QDebug>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>

#ifndef GL_TEXTURE_EXTERNAL_OES
#define GL_TEXTURE_EXTERNAL_OES 0x8D65
#endif

namespace rl {
namespace {

constexpr uint32_t drmFourcc(char a, char b, char c, char d)
{
    return uint32_t(a) | (uint32_t(b) << 8) | (uint32_t(c) << 16) | (uint32_t(d) << 24);
}

constexpr uint32_t kDrmFormatR8 = drmFourcc('R', '8', ' ', ' ');
constexpr uint32_t kDrmFormatGr88 = drmFourcc('G', 'R', '8', '8');

using CreateImage = PFNEGLCREATEIMAGEKHRPROC;
using DestroyImage = PFNEGLDESTROYIMAGEKHRPROC;
using ImageTargetTexture = PFNGLEGLIMAGETARGETTEXTURE2DOESPROC;

class VaapiTextures final : public QVideoFrameTextures
{
public:
    explicit VaapiTextures(EGLDisplay display, DestroyImage destroyImage)
        : m_display(display), m_destroyImage(destroyImage)
    {
    }

    ~VaapiTextures() override
    {
        // These objects are owned by the Qt video texture pool. It releases the
        // old slot on the render thread after that RHI slot is safe to reuse.
        for (auto &texture : m_textures) {
            if (texture)
                texture->destroy(); // QRhi wrapper; does not own the imported GL name
            texture.reset();
        }
        for (GLuint texture : m_glTextures) {
            if (texture)
                glDeleteTextures(1, &texture);
        }
        if (m_display != EGL_NO_DISPLAY && m_destroyImage) {
            for (EGLImageKHR image : m_images) {
                if (image != EGL_NO_IMAGE_KHR)
                    m_destroyImage(m_display, image);
            }
        }
    }

    QRhiTexture *texture(uint plane) const override
    {
        return plane < m_textures.size() ? m_textures[plane].get() : nullptr;
    }

    bool importPlane(QRhi &rhi, uint plane, int fd, int width, int height,
                     uint32_t fourcc, int offset, int pitch, uint64_t modifier,
                     CreateImage createImage, ImageTargetTexture imageTarget)
    {
        EGLint attributes[] = {
            EGL_WIDTH,
            width,
            EGL_HEIGHT,
            height,
            EGL_LINUX_DRM_FOURCC_EXT,
            static_cast<EGLint>(fourcc),
            EGL_DMA_BUF_PLANE0_FD_EXT,
            fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT,
            offset,
            EGL_DMA_BUF_PLANE0_PITCH_EXT,
            pitch,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
            static_cast<EGLint>(modifier & 0xffffffffu),
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
            static_cast<EGLint>(modifier >> 32),
            EGL_NONE,
        };

        const EGLImageKHR image = createImage(m_display, EGL_NO_CONTEXT,
                                              EGL_LINUX_DMA_BUF_EXT, nullptr, attributes);
        if (image == EGL_NO_IMAGE_KHR)
            return false;
        m_images[plane] = image;

        GLuint glTexture = 0;
        glGenTextures(1, &glTexture);
        if (!glTexture)
            return false;
        m_glTextures[plane] = glTexture;
        glBindTexture(GL_TEXTURE_2D, glTexture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        imageTarget(GL_TEXTURE_2D, image);
        if (glGetError() != GL_NO_ERROR)
            return false;

        const QRhiTexture::Format format = plane == 0 ? QRhiTexture::R8 : QRhiTexture::RG8;
        const QSize size(width, height);
        std::unique_ptr<QRhiTexture> rhiTexture(rhi.newTexture(format, size));
        if (!rhiTexture || !rhiTexture->createFrom({glTexture, 0}))
            return false;
        m_textures[plane] = std::move(rhiTexture);
        return true;
    }

private:
    EGLDisplay m_display = EGL_NO_DISPLAY;
    DestroyImage m_destroyImage = nullptr;
    std::array<EGLImageKHR, 2> m_images{EGL_NO_IMAGE_KHR, EGL_NO_IMAGE_KHR};
    std::array<GLuint, 2> m_glTextures{};
    std::array<std::unique_ptr<QRhiTexture>, 2> m_textures;
};

class VaapiVideoBuffer final : public QHwVideoBuffer
{
public:
    VaapiVideoBuffer(AVFrame *hwFrame, AVFrame *drmFrame, QVideoFrameFormat format,
                     std::shared_ptr<std::atomic_bool> interopUnavailable)
        : QHwVideoBuffer(QVideoFrame::RhiTextureHandle),
          m_hwFrame(hwFrame), m_drmFrame(drmFrame), m_format(std::move(format)),
          m_interopUnavailable(std::move(interopUnavailable))
    {
    }

    ~VaapiVideoBuffer() override
    {
        av_frame_free(&m_cpuFrame);
        av_frame_free(&m_drmFrame);
        av_frame_free(&m_hwFrame);
    }

    QVideoFrameFormat format() const override { return m_format; }

    MapData map(QVideoFrame::MapMode mode) override
    {
        if ((mode & QVideoFrame::WriteOnly) || !m_hwFrame)
            return {};

        if (!m_cpuFrame) {
            m_cpuFrame = av_frame_alloc();
            if (!m_cpuFrame || av_hwframe_transfer_data(m_cpuFrame, m_hwFrame, 0) < 0) {
                av_frame_free(&m_cpuFrame);
                return {};
            }
        }

        MapData mapped;
        mapped.planeCount = 2;
        for (int plane = 0; plane < mapped.planeCount; ++plane) {
            const int lines = m_cpuFrame->linesize[plane];
            const int height = plane == 0 ? m_cpuFrame->height : (m_cpuFrame->height + 1) / 2;
            if (!m_cpuFrame->data[plane] || lines <= 0)
                return {};
            mapped.data[plane] = m_cpuFrame->data[plane];
            mapped.bytesPerLine[plane] = lines;
            mapped.dataSize[plane] = lines * height;
        }
        return mapped;
    }

    void unmap() override { av_frame_free(&m_cpuFrame); }

    QVideoFrameTexturesUPtr mapTextures(QRhi &rhi, QVideoFrameTexturesUPtr &) override
    {
        if (!m_interopUnavailable || m_interopUnavailable->load(std::memory_order_relaxed))
            return {};

        auto disableInterop = [this] {
            if (m_interopUnavailable && !m_interopUnavailable->exchange(true))
                qInfo() << "VAAPI DMA-BUF presentation unavailable; using CPU texture upload";
        };

        if (rhi.backend() != QRhi::OpenGLES2) {
            disableInterop();
            return {};
        }

        const EGLDisplay display = eglGetCurrentDisplay();
        if (display == EGL_NO_DISPLAY || eglGetCurrentContext() == EGL_NO_CONTEXT) {
            disableInterop();
            return {};
        }
        const char *extensions = eglQueryString(display, EGL_EXTENSIONS);
        if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import")
            || !hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers")) {
            disableInterop();
            return {};
        }

        auto createImage = reinterpret_cast<CreateImage>(eglGetProcAddress("eglCreateImageKHR"));
        auto destroyImage = reinterpret_cast<DestroyImage>(eglGetProcAddress("eglDestroyImageKHR"));
        auto imageTarget = reinterpret_cast<ImageTargetTexture>(
            eglGetProcAddress("glEGLImageTargetTexture2DOES"));
        if (!createImage || !destroyImage || !imageTarget || !m_drmFrame || !m_drmFrame->data[0]) {
            disableInterop();
            return {};
        }

        const auto *descriptor = reinterpret_cast<const AVDRMFrameDescriptor *>(m_drmFrame->data[0]);
        if (descriptor->nb_objects < 1 || descriptor->nb_layers != 2
            || descriptor->layers[0].format != kDrmFormatR8
            || descriptor->layers[1].format != kDrmFormatGr88
            || descriptor->layers[0].nb_planes != 1 || descriptor->layers[1].nb_planes != 1) {
            disableInterop();
            return {};
        }

        auto textures = std::make_unique<VaapiTextures>(display, destroyImage);
        for (uint plane = 0; plane < 2; ++plane) {
            const auto &layer = descriptor->layers[plane];
            const auto &planeDesc = layer.planes[0];
            if (planeDesc.object_index < 0 || planeDesc.object_index >= descriptor->nb_objects) {
                disableInterop();
                return {};
            }
            const auto &object = descriptor->objects[planeDesc.object_index];
            const int width = plane == 0 ? m_format.frameWidth() : (m_format.frameWidth() + 1) / 2;
            const int height = plane == 0 ? m_format.frameHeight() : (m_format.frameHeight() + 1) / 2;
            if (!textures->importPlane(rhi, plane, object.fd, width, height, layer.format,
                                       static_cast<int>(planeDesc.offset),
                                       static_cast<int>(planeDesc.pitch), object.format_modifier,
                                       createImage, imageTarget)) {
                disableInterop();
                return {};
            }
        }
        return textures;
    }

private:
    static bool hasExtension(const char *extensions, const char *wanted)
    {
        if (!extensions || !wanted || std::strchr(wanted, ' '))
            return false;
        const size_t length = std::strlen(wanted);
        for (const char *p = extensions; (p = std::strstr(p, wanted)); p += length) {
            if ((p == extensions || p[-1] == ' ')
                && (p[length] == '\0' || p[length] == ' '))
                return true;
        }
        return false;
    }

    AVFrame *m_hwFrame = nullptr;
    AVFrame *m_drmFrame = nullptr;
    AVFrame *m_cpuFrame = nullptr;
    QVideoFrameFormat m_format;
    std::shared_ptr<std::atomic_bool> m_interopUnavailable;
};

} // namespace

QVideoFrame makeVaapiVideoFrame(const AVFrame *frame,
                                const std::shared_ptr<std::atomic_bool> &interopUnavailable)
{
    if (!frame || !frame->hw_frames_ctx || !interopUnavailable
        || interopUnavailable->load(std::memory_order_relaxed))
        return {};

    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(frame->hw_frames_ctx->data);
    if (!frames || frames->sw_format != AV_PIX_FMT_NV12 || frame->width <= 0 || frame->height <= 0
        || (frame->width & 1) || (frame->height & 1)) {
        interopUnavailable->store(true, std::memory_order_relaxed);
        return {};
    }

    AVFrame *drmFrame = av_frame_alloc();
    if (!drmFrame) {
        interopUnavailable->store(true, std::memory_order_relaxed);
        return {};
    }
    drmFrame->format = AV_PIX_FMT_DRM_PRIME;
    if (av_hwframe_map(drmFrame, frame, AV_HWFRAME_MAP_READ | AV_HWFRAME_MAP_DIRECT) < 0) {
        av_frame_free(&drmFrame);
        interopUnavailable->store(true, std::memory_order_relaxed);
        return {};
    }

    AVFrame *hwFrame = av_frame_clone(frame);
    if (!hwFrame) {
        av_frame_free(&drmFrame);
        interopUnavailable->store(true, std::memory_order_relaxed);
        return {};
    }

    QVideoFrameFormat format(QSize(frame->width, frame->height), QVideoFrameFormat::Format_NV12);
    if (frame->color_range == AVCOL_RANGE_JPEG)
        format.setColorRange(QVideoFrameFormat::ColorRange_Full);

    auto buffer = std::make_unique<VaapiVideoBuffer>(hwFrame, drmFrame, format, interopUnavailable);
    return QVideoFrame(std::move(buffer));
}

} // namespace rl

#else

namespace rl {

QVideoFrame makeVaapiVideoFrame(const AVFrame *,
                                const std::shared_ptr<std::atomic_bool> &)
{
    return {};
}

} // namespace rl

#endif
