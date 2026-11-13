#include "video/h264_encoder.h"

#include <cstdio>
#include <cstring>

#include <rga/im2d.h>
#include <rga/rga.h>
#include <rockchip/mpp_buffer.h>
#include <rockchip/mpp_frame.h>
#include <rockchip/mpp_packet.h>
#include <rockchip/rk_mpi.h>
#include <rockchip/rk_venc_cfg.h>

namespace mb {

struct H264Encoder::Slot {
    MppBuffer buf = nullptr;
    rga_buffer_handle_t rga_handle = 0;
    bool busy = false;
};

namespace {

int align16(int v) { return (v + 15) & ~15; }

inline uint8_t clamp_u8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

}  // namespace

H264Encoder::~H264Encoder() { close(); }

bool H264Encoder::open(const H264Config& cfg, std::string* err) {
    close();
    cfg_ = cfg;
    if (cfg_.width <= 0 || cfg_.height <= 0 || (cfg_.width & 1) || (cfg_.height & 1)) {
        if (err) *err = "编码分辨率必须为正偶数";
        return false;
    }
    if (cfg_.fps <= 0) cfg_.fps = 30;
    if (cfg_.gop <= 0) cfg_.gop = cfg_.fps;
    if (cfg_.bitrate_kbps <= 0) cfg_.bitrate_kbps = 4000;
    if (cfg_.input_slots < 2) cfg_.input_slots = 2;

    hor_stride_ = align16(cfg_.width);
    ver_stride_ = align16(cfg_.height);

    MppCtx ctx = nullptr;
    MppApi* mpi = nullptr;
    if (mpp_create(&ctx, &mpi) != MPP_OK) {
        if (err) *err = "mpp_create 失败";
        return false;
    }
    ctx_ = ctx;
    mpi_ = mpi;
    if (mpp_init(ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC) != MPP_OK) {
        if (err) *err = "mpp_init(ENC, AVC) 失败";
        close();
        return false;
    }

    MppEncCfg ecfg = nullptr;
    if (mpp_enc_cfg_init(&ecfg) != MPP_OK) {
        if (err) *err = "mpp_enc_cfg_init 失败";
        close();
        return false;
    }
    mpi->control(ctx, MPP_ENC_GET_CFG, ecfg);

    const int bps = cfg_.bitrate_kbps * 1000;
    mpp_enc_cfg_set_s32(ecfg, "prep:width", cfg_.width);
    mpp_enc_cfg_set_s32(ecfg, "prep:height", cfg_.height);
    mpp_enc_cfg_set_s32(ecfg, "prep:hor_stride", hor_stride_);
    mpp_enc_cfg_set_s32(ecfg, "prep:ver_stride", ver_stride_);
    mpp_enc_cfg_set_s32(ecfg, "prep:format", MPP_FMT_YUV420SP);

    // 采集帧率会随曝光漂 (暗处 15fps), 所以开 flex: 码控按实际送帧节奏走。
    mpp_enc_cfg_set_s32(ecfg, "rc:mode", MPP_ENC_RC_MODE_CBR);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_in_flex", 1);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_in_num", cfg_.fps);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_out_flex", 1);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_out_num", cfg_.fps);
    mpp_enc_cfg_set_s32(ecfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(ecfg, "rc:gop", cfg_.gop);
    mpp_enc_cfg_set_s32(ecfg, "rc:bps_target", bps);
    mpp_enc_cfg_set_s32(ecfg, "rc:bps_max", bps / 16 * 17);
    mpp_enc_cfg_set_s32(ecfg, "rc:bps_min", bps / 16 * 15);

    mpp_enc_cfg_set_s32(ecfg, "codec:type", MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(ecfg, "h264:profile", 100);
    mpp_enc_cfg_set_s32(ecfg, "h264:level", 40);
    mpp_enc_cfg_set_s32(ecfg, "h264:cabac_en", 1);
    mpp_enc_cfg_set_s32(ecfg, "h264:cabac_idc", 0);
    mpp_enc_cfg_set_s32(ecfg, "h264:trans8x8", 1);

    const MPP_RET r = mpi->control(ctx, MPP_ENC_SET_CFG, ecfg);
    mpp_enc_cfg_deinit(ecfg);
    if (r != MPP_OK) {
        if (err) *err = "MPP_ENC_SET_CFG 失败: " + std::to_string((int)r);
        close();
        return false;
    }

    MppEncSeiMode sei = MPP_ENC_SEI_MODE_DISABLE;
    mpi->control(ctx, MPP_ENC_SET_SEI_CFG, &sei);
    MppEncHeaderMode hm = MPP_ENC_HEADER_MODE_EACH_IDR;
    mpi->control(ctx, MPP_ENC_SET_HEADER_MODE, &hm);

    MppBufferGroup grp = nullptr;
    if (mpp_buffer_group_get_internal(&grp, MPP_BUFFER_TYPE_DRM) != MPP_OK) {
        if (err) *err = "mpp_buffer_group_get_internal(DRM) 失败";
        close();
        return false;
    }
    group_ = grp;

    const size_t frame_size = (size_t)hor_stride_ * ver_stride_ * 3 / 2;
    rga_ok_ = true;
    for (int i = 0; i < cfg_.input_slots; ++i) {
        Slot* s = new Slot();
        if (mpp_buffer_get(grp, &s->buf, frame_size) != MPP_OK) {
            delete s;
            if (err) *err = "mpp_buffer_get 失败";
            close();
            return false;
        }
        s->rga_handle = importbuffer_fd(mpp_buffer_get_fd(s->buf), (int)frame_size);
        if (s->rga_handle == 0) rga_ok_ = false;
        slots_.push_back(s);
    }
    return true;
}

void H264Encoder::close() {
    for (Slot* s : slots_) {
        if (s->rga_handle) releasebuffer_handle(s->rga_handle);
        if (s->buf) mpp_buffer_put(s->buf);
        delete s;
    }
    slots_.clear();
    if (group_) {
        mpp_buffer_group_put((MppBufferGroup)group_);
        group_ = nullptr;
    }
    if (ctx_) {
        MppApi* mpi = (MppApi*)mpi_;
        mpi->reset((MppCtx)ctx_);
        mpp_destroy((MppCtx)ctx_);
        ctx_ = nullptr;
        mpi_ = nullptr;
    }
}

int H264Encoder::acquire_slot() {
    std::lock_guard<std::mutex> lk(slot_mutex_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        if (!slots_[i]->busy) {
            slots_[i]->busy = true;
            return (int)i;
        }
    }
    return -1;
}

void H264Encoder::release_slot(int slot) {
    std::lock_guard<std::mutex> lk(slot_mutex_);
    if (slot >= 0 && slot < (int)slots_.size()) slots_[(size_t)slot]->busy = false;
}

bool H264Encoder::fill_rga(Slot* s, const ImageRGB& src) {
    if (!s->rga_handle) return false;
    rga_buffer_handle_t src_handle =
        importbuffer_virtualaddr((void*)src.data.data(), (int)src.size_bytes());
    if (!src_handle) return false;
    rga_buffer_t rsrc =
        wrapbuffer_handle_t(src_handle, src.width, src.height, src.width, src.height, RK_FORMAT_RGB_888);
    rga_buffer_t rdst = wrapbuffer_handle_t(s->rga_handle, cfg_.width, cfg_.height, hor_stride_,
                                          ver_stride_, RK_FORMAT_YCbCr_420_SP);
    im_rect srect = {0, 0, src.width, src.height};
    im_rect drect = {0, 0, cfg_.width, cfg_.height};
    im_rect prect;
    memset(&prect, 0, sizeof(prect));
    rga_buffer_t pat;
    memset(&pat, 0, sizeof(pat));
    const IM_STATUS st = improcess(rsrc, rdst, pat, srect, drect, prect, 0);
    releasebuffer_handle(src_handle);
    if (st <= 0) {
        fprintf(stderr, "[h264] RGA RGB->NV12 失败: %s, 回退 CPU\n", imStrError_t(st));
        return false;
    }
    return true;
}

// CPU 回退: 最近邻缩放 + BT.601 有限范围。1080p 约 15ms, 只是兜底。
void H264Encoder::fill_cpu(Slot* s, const ImageRGB& src) {
    uint8_t* y_plane = (uint8_t*)mpp_buffer_get_ptr(s->buf);
    uint8_t* uv_plane = y_plane + (size_t)hor_stride_ * ver_stride_;
    const int w = cfg_.width, h = cfg_.height;
    for (int y = 0; y < h; ++y) {
        const int sy = (int)((int64_t)y * src.height / h);
        const uint8_t* srow = src.row(sy);
        uint8_t* yrow = y_plane + (size_t)y * hor_stride_;
        uint8_t* uvrow = uv_plane + (size_t)(y / 2) * hor_stride_;
        for (int x = 0; x < w; ++x) {
            const int sx = (int)((int64_t)x * src.width / w);
            const uint8_t* p = srow + sx * 3;
            const int r = p[0], g = p[1], b = p[2];
            yrow[x] = clamp_u8(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (!(y & 1) && !(x & 1)) {
                uvrow[x] = clamp_u8(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uvrow[x + 1] = clamp_u8(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    mpp_buffer_sync_end(s->buf);
}

bool H264Encoder::fill_slot(int slot, const ImageRGB& src, std::string* err) {
    if (slot < 0 || slot >= (int)slots_.size()) {
        if (err) *err = "无效槽位";
        return false;
    }
    if (src.width <= 0 || src.height <= 0 || src.data.size() < src.size_bytes()) {
        if (err) *err = "空输入";
        return false;
    }
    Slot* s = slots_[(size_t)slot];
    if (rga_ok_ && fill_rga(s, src)) return true;
    rga_ok_ = false;  // 失败一次就不再试, 免得每帧打日志
    fill_cpu(s, src);
    return true;
}

bool H264Encoder::encode_slot(int slot, uint64_t ts_ms, EncodedPacket* out, std::string* err) {
    if (!ctx_ || slot < 0 || slot >= (int)slots_.size()) {
        if (err) *err = "编码器未打开";
        return false;
    }
    MppCtx ctx = (MppCtx)ctx_;
    MppApi* mpi = (MppApi*)mpi_;

    MppFrame frame = nullptr;
    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, (RK_U32)cfg_.width);
    mpp_frame_set_height(frame, (RK_U32)cfg_.height);
    mpp_frame_set_hor_stride(frame, (RK_U32)hor_stride_);
    mpp_frame_set_ver_stride(frame, (RK_U32)ver_stride_);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_pts(frame, (RK_S64)ts_ms);
    mpp_frame_set_buffer(frame, slots_[(size_t)slot]->buf);

    MPP_RET r = mpi->encode_put_frame(ctx, frame);
    mpp_frame_deinit(&frame);
    if (r != MPP_OK) {
        if (err) *err = "encode_put_frame 失败: " + std::to_string((int)r);
        return false;
    }

    out->data.clear();
    out->keyframe = false;
    out->ts_ms = ts_ms;

    // 一帧可能被切成多个 partition 包, 收到 EOI 为止。
    for (int guard = 0; guard < 64; ++guard) {
        MppPacket pkt = nullptr;
        r = mpi->encode_get_packet(ctx, &pkt);
        if (r != MPP_OK || !pkt) {
            if (err) *err = "encode_get_packet 失败: " + std::to_string((int)r);
            return false;
        }
        const uint8_t* p = (const uint8_t*)mpp_packet_get_pos(pkt);
        const size_t n = mpp_packet_get_length(pkt);
        out->data.insert(out->data.end(), p, p + n);

        MppMeta meta = mpp_packet_get_meta(pkt);
        RK_S32 intra = 0;
        if (meta && mpp_meta_get_s32(meta, KEY_OUTPUT_INTRA, &intra) == MPP_OK && intra)
            out->keyframe = true;

        const bool partition = mpp_packet_is_partition(pkt);
        const bool eoi = mpp_packet_is_eoi(pkt);
        mpp_packet_deinit(&pkt);
        if (!partition || eoi) break;
    }
    return !out->data.empty();
}

}  // namespace mb
