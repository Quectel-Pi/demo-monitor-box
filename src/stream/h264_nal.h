#ifndef MONITOR_BOX_STREAM_H264_NAL_H_
#define MONITOR_BOX_STREAM_H264_NAL_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mb {

struct NalUnit {
    const uint8_t* data = nullptr;  // 不含起始码
    size_t size = 0;
    int type() const { return size ? (data[0] & 0x1F) : 0; }
};

// 把 Annex-B 字节流按 00 00 01 / 00 00 00 01 切成 NAL。
std::vector<NalUnit> split_annexb(const uint8_t* p, size_t n);

inline bool has_idr(const std::vector<NalUnit>& nals) {
    for (const NalUnit& u : nals)
        if (u.type() == 5) return true;
    return false;
}

}  // namespace mb

#endif  // MONITOR_BOX_STREAM_H264_NAL_H_
