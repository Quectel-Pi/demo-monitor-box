#ifndef MONITOR_BOX_VIDEO_REGION_OVERLAY_H_
#define MONITOR_BOX_VIDEO_REGION_OVERLAY_H_

#include <cstdint>
#include <vector>

#include "common/geometry.h"
#include "infer/types.h"

namespace mb {

// OSD 上的半透明区域填充 (类似安防 NVR 的区域着色)。
//
// 扫描线算出每个多边形在每一行覆盖的 [x0, x1) 区间, 再逐像素做整数 alpha 混合。
// 区间计算只和行数、边数有关 (1080 行 x 十几条边, 微秒级), 所以每帧重算, 不做缓存;
// 混合的开销和填充面积成正比, alpha = 0 时整层跳过。
struct OverlayLayer {
    std::vector<Point> poly;  // 像素坐标
    uint8_t r = 0, g = 0, b = 0;
    int alpha = 64;  // 0..256
};

void fill_regions(ImageRGB* img, const std::vector<OverlayLayer>& layers);

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_REGION_OVERLAY_H_
