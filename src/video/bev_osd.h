#ifndef MONITOR_BOX_VIDEO_BEV_OSD_H_
#define MONITOR_BOX_VIDEO_BEV_OSD_H_

#include <string>

#include "common/json.h"
#include "infer/types.h"
#include "locate/ground_locator.h"

namespace mb {

// OSD 鸟瞰图画中画 (display.bev_pip), 设置页可开关, 热加载生效。
struct BevPipParams {
    bool enabled = false;
    std::string corner = "bottom_left";  // top_left / top_right / bottom_left / bottom_right
    float size = 0.2f;                   // 画中画宽度占画面宽度的比例 (0.15 .. 0.5)

    static BevPipParams from_json(const Json& cfg);
};

// 世界 (地面米) -> 画中画像素。y 轴向上 = 远离镜头, 与 APP 鸟瞰图一致。
struct BevView {
    int x = 0, y = 0, w = 0, h = 0;  // 画中画在画面上的矩形
    float minx = 0, miny = 0, maxx = 1, maxy = 1;  // 世界范围
    float scale = 1.0f;  // 像素 / 米
    float ox = 0, oy = 0;

    Point to_px(Point g) const { return Point{ox + g.x * scale, oy - g.y * scale}; }
};

// 按场景范围和画面尺寸算出画中画布局。场景无效时返回 false。
bool bev_layout(const BevScene& s, const BevPipParams& p, int img_w, int img_h, BevView* out);

// 在 img 上画鸟瞰图画中画; 没画 (未开启 / 无标定) 返回 false。
bool draw_bev_pip(ImageRGB* img, const BevScene& s, const BevPipParams& p, BevView* view = nullptr);

}  // namespace mb

#endif  // MONITOR_BOX_VIDEO_BEV_OSD_H_
