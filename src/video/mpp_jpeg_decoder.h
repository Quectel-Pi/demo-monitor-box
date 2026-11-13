#ifndef MONITOR_BOX_VIDEO_MPP_JPEG_DECODER_H_
#define MONITOR_BOX_VIDEO_MPP_JPEG_DECODER_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "infer/types.h"

namespace mb {

// RK MPP hardware JPEG decoder for the live UVC MJPG path.
//
// MPP decodes JPEG to NV12 and RGA converts the result to the packed RGB888
// image used by inference, OSD and H.264 encoding. The decoder is stateful and
// must be used by one thread at a time. Failure is reported to the caller so
// the existing libjpeg path can remain as a compatibility fallback.
class MppJpegDecoder {
public:
    MppJpegDecoder();
    ~MppJpegDecoder();

    MppJpegDecoder(const MppJpegDecoder&) = delete;
    MppJpegDecoder& operator=(const MppJpegDecoder&) = delete;

    bool open(int width, int height, std::string* err);
    void close();
    bool ok() const;

    bool decode(const uint8_t* data, size_t size, ImageRGB* out, std::string* err);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_MPP_JPEG_DECODER_H_
