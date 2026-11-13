#ifndef MONITOR_BOX_VIDEO_H264_ENCODER_H_
#define MONITOR_BOX_VIDEO_H264_ENCODER_H_

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "infer/types.h"

namespace mb {

struct H264Config {
    int width = 1920;   // 编码分辨率; 与输入不同时由 RGA 顺带缩放
    int height = 1080;
    int fps = 30;
    int bitrate_kbps = 4000;
    int gop = 30;       // IDR 间隔 (帧)。录像预录和 UDP 中途加入都靠 IDR, 别设太大
    int input_slots = 3;
};

// 一个编码后的访问单元 (Annex-B, 带 00 00 00 01 起始码)。
// 每个 IDR 前都带 SPS/PPS (MPP_ENC_HEADER_MODE_EACH_IDR), 所以从任意 IDR 开始
// 截取都是可独立解码的。
struct EncodedPacket {
    std::vector<uint8_t> data;
    bool keyframe = false;
    uint64_t ts_ms = 0;
};

// RK MPP 硬件 H.264 编码 (VEPU)。
//
// 输入是 OSD 之后的 RGB888。RGB -> NV12 + 缩放交给 RGA, 直接写进 MPP 的 DRM 缓冲
// (零拷贝给 VEPU); RGA 不可用时回退 CPU 最近邻 + BT.601 转换。
//
// 为了不阻塞主循环, 输入分成 "槽位": 主线程 acquire_slot + fill_slot (RGA, 几毫秒),
// 编码线程 encode_slot + release_slot。两个线程永远不碰同一个槽位。
class H264Encoder {
public:
    H264Encoder() = default;
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool open(const H264Config& cfg, std::string* err);
    void close();
    bool is_open() const { return ctx_ != nullptr; }
    const H264Config& config() const { return cfg_; }

    // 线程安全: 返回空闲槽位号, 没有则 -1 (调用方丢帧)。
    int acquire_slot();
    void release_slot(int slot);

    // RGB -> NV12 写入槽位。
    bool fill_slot(int slot, const ImageRGB& src, std::string* err);

    // 编码一个已填好的槽位, 输出一个完整访问单元。
    bool encode_slot(int slot, uint64_t ts_ms, EncodedPacket* out, std::string* err);

    bool using_rga() const { return rga_ok_; }

private:
    struct Slot;
    bool fill_rga(Slot* s, const ImageRGB& src);
    void fill_cpu(Slot* s, const ImageRGB& src);

    H264Config cfg_;
    void* ctx_ = nullptr;   // MppCtx
    void* mpi_ = nullptr;   // MppApi*
    void* group_ = nullptr; // MppBufferGroup
    int hor_stride_ = 0;
    int ver_stride_ = 0;
    std::vector<Slot*> slots_;
    bool rga_ok_ = true;
    std::mutex slot_mutex_;
};

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_H264_ENCODER_H_
