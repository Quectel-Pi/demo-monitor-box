#include "video/region_overlay.h"

#include <algorithm>
#include <cmath>

namespace mb {

namespace {

void blend_span(uint8_t* p, int n, const OverlayLayer& l) {
    const int a = l.alpha, ia = 256 - l.alpha;
    const int cr = l.r * a, cg = l.g * a, cb = l.b * a;
    for (int i = 0; i < n; ++i, p += 3) {
        p[0] = (uint8_t)((p[0] * ia + cr) >> 8);
        p[1] = (uint8_t)((p[1] * ia + cg) >> 8);
        p[2] = (uint8_t)((p[2] * ia + cb) >> 8);
    }
}

}  // namespace

void fill_regions(ImageRGB* img, const std::vector<OverlayLayer>& layers) {
    if (!img || img->width <= 0 || img->height <= 0) return;
    const int w = img->width, h = img->height;
    std::vector<float> xs;
    for (const OverlayLayer& l : layers) {
        const size_t n = l.poly.size();
        if (n < 3 || l.alpha <= 0) continue;
        float ymin = l.poly[0].y, ymax = l.poly[0].y;
        for (const Point& p : l.poly) {
            ymin = std::min(ymin, p.y);
            ymax = std::max(ymax, p.y);
        }
        const int y0 = std::max(0, (int)std::ceil(ymin - 0.5f));
        const int y1 = std::min(h - 1, (int)std::floor(ymax - 0.5f));
        for (int y = y0; y <= y1; ++y) {
            // 行中心与各边求交 (奇偶规则, 与 point_in_polygon 一致)
            const float sy = (float)y + 0.5f;
            xs.clear();
            for (size_t i = 0; i < n; ++i) {
                const Point& a = l.poly[i];
                const Point& b = l.poly[(i + 1) % n];
                if ((a.y <= sy) == (b.y <= sy)) continue;
                xs.push_back(a.x + (sy - a.y) * (b.x - a.x) / (b.y - a.y));
            }
            std::sort(xs.begin(), xs.end());
            uint8_t* row = img->row(y);
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int x0 = std::max(0, (int)std::ceil(xs[k] - 0.5f));
                const int x1 = std::min(w, (int)std::ceil(xs[k + 1] - 0.5f));
                if (x1 > x0) blend_span(row + (size_t)x0 * 3, x1 - x0, l);
            }
        }
    }
}

}  // namespace mb
