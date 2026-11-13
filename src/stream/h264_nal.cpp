#include "stream/h264_nal.h"

#include <cstdint>

namespace mb {

std::vector<NalUnit> split_annexb(const uint8_t* p, size_t n) {
    std::vector<NalUnit> out;
    size_t i = 0;
    size_t start = SIZE_MAX;
    while (i + 2 < n) {
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
            if (start != SIZE_MAX) {
                size_t end = i;
                // 4 字节起始码的前导 0 属于下一个起始码, 不属于上一个 NAL
                while (end > start && p[end - 1] == 0) --end;
                if (end > start) out.push_back(NalUnit{p + start, end - start});
            }
            i += 3;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != SIZE_MAX && start < n) out.push_back(NalUnit{p + start, n - start});
    return out;
}

}  // namespace mb
