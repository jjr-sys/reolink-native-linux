#pragma once

#include <QVideoFrame>

#include <atomic>
#include <memory>

extern "C" {
#include <libavutil/frame.h>
}

namespace rl {

// Per-stream zero-copy state, shared by the decode thread and the render thread.
struct VaapiInteropState {
    // Set once zero-copy is known not to work for this stream; every later frame then
    // takes the CPU transfer/upload path.
    std::atomic_bool unavailable{false};
    // av_hwframe_map failures in a row. A single failure is usually one corrupt frame
    // (VAAPI "Failed to sync surface: internal decoding error"), so that frame alone
    // takes the CPU path; only a sustained run turns zero-copy off for the stream.
    std::atomic_int consecutiveMapFailures{0};
};

// Wrap a VAAPI NV12 frame so Qt Quick can import its DRM PRIME planes on the
// render thread. An invalid result means the caller should use the CPU path.
QVideoFrame makeVaapiVideoFrame(const AVFrame *frame,
                                const std::shared_ptr<VaapiInteropState> &interop);

} // namespace rl
