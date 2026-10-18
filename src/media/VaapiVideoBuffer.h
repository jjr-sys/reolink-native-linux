#pragma once

#include <QVideoFrame>

#include <atomic>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace rl {

// Wrap a VAAPI NV12 frame so Qt Quick can import its DRM PRIME planes on the
// render thread. An invalid result means the caller should use the CPU path.
QVideoFrame makeVaapiVideoFrame(const AVFrame *frame,
                                const std::shared_ptr<std::atomic_bool> &interopUnavailable);

} // namespace rl
