#include "video/mpp_jpeg_decoder.h"

#include <cstdio>
#include <cstring>

#include <rga/im2d.h>
#include <rga/rga.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_log.h>
#include <rockchip/mpp_meta.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/mpp_task.h>
#include <rockchip/rk_mpi.h>

namespace mb {
namespace {

constexpr int kPollTimeoutMs = 100;

int align16(int v) { return (v + 15) & ~15; }

bool fail(std::string* err, const char* msg, int code = 0) {
    if (err) *err = std::string(msg) + (code ? " (" + std::to_string(code) + ")" : "");
    return false;
}

}  // namespace

struct MppJpegDecoder::Impl {
    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    MppBufferGroup input_group = nullptr;
    MppBufferGroup output_group = nullptr;
    int width = 0;
    int height = 0;
    int hor_stride = 0;
    int ver_stride = 0;
    size_t output_buffer_size = 0;
};

MppJpegDecoder::MppJpegDecoder() = default;
MppJpegDecoder::~MppJpegDecoder() { close(); }

bool MppJpegDecoder::open(int width, int height, std::string* err) {
    close();
    if (width <= 0 || height <= 0) return fail(err, "invalid MPP JPEG dimensions");

    impl_.reset(new Impl());
    Impl& d = *impl_;
    d.width = width;
    d.height = height;
    d.hor_stride = align16(width);
    d.ver_stride = align16(height);
    // UVC MJPEG can carry either 4:2:2 or 4:2:0. Two aligned Y planes is the
    // safe bound MPP itself uses; a smaller 4:2:0 buffer trips mpp_assert on
    // cameras which switch JPEG subsampling between frames.
    d.output_buffer_size = (size_t)d.hor_stride * d.ver_stride * 2;
    // The decoder emits an informational size mismatch for this valid bound.
    // Keep warnings and real errors while avoiding thousands of info lines at 30fps.
    mpp_set_log_level(MPP_LOG_WARN);

    int ret = mpp_create(&d.ctx, &d.mpi);
    if (ret != MPP_OK) {
        close();
        return fail(err, "mpp_create for JPEG failed", ret);
    }
    ret = mpp_init(d.ctx, MPP_CTX_DEC, MPP_VIDEO_CodingMJPEG);
    if (ret != MPP_OK) {
        close();
        return fail(err, "mpp_init(JPEG) failed", ret);
    }

    MppFrameFormat fmt = MPP_FMT_YUV420SP;
    ret = d.mpi->control(d.ctx, MPP_DEC_SET_OUTPUT_FORMAT, &fmt);
    if (ret != MPP_OK) {
        close();
        return fail(err, "MPP JPEG NV12 output is unsupported", ret);
    }
    ret = mpp_buffer_group_get_internal(&d.input_group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) {
        close();
        return fail(err, "MPP JPEG input buffer group failed", ret);
    }
    ret = mpp_buffer_group_get_internal(&d.output_group, MPP_BUFFER_TYPE_DRM);
    if (ret != MPP_OK) {
        close();
        return fail(err, "MPP JPEG output buffer group failed", ret);
    }
    return true;
}

void MppJpegDecoder::close() {
    if (!impl_) return;
    Impl& d = *impl_;
    if (d.input_group) mpp_buffer_group_put(d.input_group);
    if (d.output_group) mpp_buffer_group_put(d.output_group);
    d.input_group = nullptr;
    d.output_group = nullptr;
    if (d.ctx) mpp_destroy(d.ctx);
    d.ctx = nullptr;
    d.mpi = nullptr;
    impl_.reset();
}

bool MppJpegDecoder::ok() const { return impl_ && impl_->ctx && impl_->mpi; }

bool MppJpegDecoder::decode(const uint8_t* data, size_t size, ImageRGB* out,
                            std::string* err) {
    if (!ok() || !data || size == 0 || !out) return fail(err, "invalid MPP JPEG input");
    Impl& d = *impl_;

    MppTask input_task = nullptr;
    MppTask output_task = nullptr;
    MppBuffer input_buffer = nullptr;
    MppBuffer output_buffer = nullptr;
    MppPacket packet = nullptr;
    MppFrame output_frame = nullptr;

    int ret = mpp_buffer_get(d.input_group, &input_buffer, size);
    if (ret != MPP_OK || !input_buffer) {
        return fail(err, "MPP JPEG input buffer allocation failed", ret);
    }
    memcpy(mpp_buffer_get_ptr(input_buffer), data, size);
    ret = mpp_packet_init_with_buffer(&packet, input_buffer);
    mpp_buffer_put(input_buffer);
    input_buffer = nullptr;
    if (ret != MPP_OK || !packet) {
        return fail(err, "MPP JPEG packet creation failed", ret);
    }
    mpp_packet_set_size(packet, size);
    mpp_packet_set_length(packet, size);

    ret = d.mpi->poll(d.ctx, MPP_PORT_INPUT, static_cast<MppPollType>(kPollTimeoutMs));
    if (ret != MPP_OK) {
        mpp_packet_deinit(&packet);
        return fail(err, "MPP JPEG input poll timeout/failure", ret);
    }
    ret = d.mpi->dequeue(d.ctx, MPP_PORT_INPUT, &input_task);
    if (ret != MPP_OK || !input_task) {
        mpp_packet_deinit(&packet);
        return fail(err, "MPP JPEG input dequeue failed", ret);
    }
    mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, packet);

    ret = mpp_buffer_get(d.output_group, &output_buffer, d.output_buffer_size);
    if (ret != MPP_OK || !output_buffer) {
        mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, nullptr);
        d.mpi->enqueue(d.ctx, MPP_PORT_INPUT, input_task);
        mpp_packet_deinit(&packet);
        return fail(err, "MPP JPEG output buffer allocation failed", ret);
    }
    ret = mpp_frame_init(&output_frame);
    if (ret != MPP_OK || !output_frame) {
        mpp_buffer_put(output_buffer);
        mpp_task_meta_set_packet(input_task, KEY_INPUT_PACKET, nullptr);
        d.mpi->enqueue(d.ctx, MPP_PORT_INPUT, input_task);
        mpp_packet_deinit(&packet);
        return fail(err, "MPP JPEG output frame creation failed", ret);
    }
    mpp_frame_set_buffer(output_frame, output_buffer);
    mpp_buffer_put(output_buffer);
    output_buffer = nullptr;

    MppMeta meta = mpp_frame_get_meta(output_frame);
    mpp_meta_set_packet(meta, KEY_INPUT_PACKET, packet);
    mpp_task_meta_set_frame(input_task, KEY_OUTPUT_FRAME, output_frame);
    ret = d.mpi->enqueue(d.ctx, MPP_PORT_INPUT, input_task);
    if (ret != MPP_OK) {
        mpp_frame_deinit(&output_frame);
        mpp_packet_deinit(&packet);
        return fail(err, "MPP JPEG input enqueue failed", ret);
    }
    ret = d.mpi->poll(d.ctx, MPP_PORT_OUTPUT, static_cast<MppPollType>(kPollTimeoutMs));
    if (ret != MPP_OK) return fail(err, "MPP JPEG output poll timeout/failure", ret);
    ret = d.mpi->dequeue(d.ctx, MPP_PORT_OUTPUT, &output_task);
    if (ret != MPP_OK || !output_task) return fail(err, "MPP JPEG output dequeue failed", ret);

    MppFrame frame = nullptr;
    mpp_task_meta_get_frame(output_task, KEY_OUTPUT_FRAME, &frame);
    MppPacket packet_ref = nullptr;
    if (frame) mpp_meta_get_packet(mpp_frame_get_meta(frame), KEY_INPUT_PACKET, &packet_ref);
    if (packet_ref) mpp_packet_deinit(&packet_ref);

    bool converted = false;
    if (frame && !mpp_frame_get_errinfo(frame) && !mpp_frame_get_discard(frame)) {
        MppBuffer buf = mpp_frame_get_buffer(frame);
        const int w = (int)mpp_frame_get_width(frame);
        const int h = (int)mpp_frame_get_height(frame);
        const int hs = (int)mpp_frame_get_hor_stride(frame);
        const int vs = (int)mpp_frame_get_ver_stride(frame);
        const int fmt = (int)mpp_frame_get_fmt(frame);
        const int format = fmt & MPP_FRAME_FMT_MASK;
        if (buf && w > 0 && h > 0 && hs >= w && vs >= h &&
            format == MPP_FMT_YUV420SP) {
            out->width = w;
            out->height = h;
            out->data.resize(out->size_bytes());
            rga_buffer_handle_t src_handle =
                importbuffer_fd(mpp_buffer_get_fd(buf), (int)mpp_buffer_get_size(buf));
            rga_buffer_handle_t dst_handle =
                importbuffer_virtualaddr(out->data.data(), (int)out->size_bytes());
            if (src_handle && dst_handle) {
                rga_buffer_t src =
                    wrapbuffer_handle_t(src_handle, w, h, hs, vs, RK_FORMAT_YCbCr_420_SP);
                rga_buffer_t dst =
                    wrapbuffer_handle_t(dst_handle, w, h, w, h, RK_FORMAT_RGB_888);
                im_rect srect = {0, 0, w, h};
                im_rect drect = {0, 0, w, h};
                im_rect prect;
                memset(&prect, 0, sizeof(prect));
                rga_buffer_t pat;
                memset(&pat, 0, sizeof(pat));
                const IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, 0);
                converted = st > 0;
            }
            if (src_handle) releasebuffer_handle(src_handle);
            if (dst_handle) releasebuffer_handle(dst_handle);
        }
    }

    d.mpi->enqueue(d.ctx, MPP_PORT_OUTPUT, output_task);
    mpp_frame_deinit(&frame);
    if (!converted) return fail(err, "MPP JPEG decode/RGA conversion failed");
    return true;
}

}  // namespace mb
