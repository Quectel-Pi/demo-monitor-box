#include "video/bev_osd.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "image_drawing.h"
#include "video/region_overlay.h"

namespace mb {

namespace {

constexpr float kDeg2Rad = 0.017453292519943295f;

unsigned int group_color(const std::string& g) {
    if (g == "person") return 0xFF30D158;
    if (g == "vehicle") return 0xFF00A5FF;
    if (g == "pet") return 0xFFFFB020;
    return 0xFFBCAAA4;
}

image_buffer_t wrap(ImageRGB* img) {
    image_buffer_t b{};
    b.width = img->width;
    b.height = img->height;
    b.width_stride = img->width;
    b.height_stride = img->height;
    b.format = IMAGE_FORMAT_RGB888;
    b.virt_addr = img->data.data();
    b.size = (int)img->size_bytes();
    b.fd = -1;
    return b;
}

// Liang-Barsky: 把线段裁到画中画矩形里; 完全在外返回 false。
// zoo 的 draw_line 按线段外接矩形逐像素扫, 不裁的话一条飞出去的轨迹能扫满整幅画面。
// 线宽会向两侧各多画 th/2+1 像素, 所以裁剪框按线宽内缩。
bool clip(const BevView& v, int th, Point* a, Point* b) {
    const float in = (float)(th / 2 + 1);
    const float xmin = v.x + in, ymin = v.y + in;
    const float xmax = v.x + v.w - 1 - in, ymax = v.y + v.h - 1 - in;
    const float dx = b->x - a->x, dy = b->y - a->y;
    float t0 = 0.0f, t1 = 1.0f;
    const float p[4] = {-dx, dx, -dy, dy};
    const float q[4] = {a->x - xmin, xmax - a->x, a->y - ymin, ymax - a->y};
    for (int i = 0; i < 4; ++i) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return false;
            continue;
        }
        const float t = q[i] / p[i];
        if (p[i] < 0.0f) t0 = std::max(t0, t);
        else t1 = std::min(t1, t);
        if (t0 > t1) return false;
    }
    const Point a0 = *a;
    *a = Point{a0.x + t0 * dx, a0.y + t0 * dy};
    *b = Point{a0.x + t1 * dx, a0.y + t1 * dy};
    return true;
}

void line(image_buffer_t* osd, const BevView& v, Point a, Point b, unsigned int c, int th) {
    if (!clip(v, th, &a, &b)) return;
    draw_line(osd, (int)std::lround(a.x), (int)std::lround(a.y), (int)std::lround(b.x),
              (int)std::lround(b.y), c, th);
}

void poly(image_buffer_t* osd, const BevView& v, const std::vector<Point>& pts, unsigned int c,
          int th, bool closed) {
    for (size_t i = 0; i + 1 < pts.size() + (closed ? 1 : 0); ++i)
        line(osd, v, v.to_px(pts[i]), v.to_px(pts[(i + 1) % pts.size()]), c, th);
}

bool inside(const BevView& v, Point p, float pad) {
    return p.x >= v.x + pad && p.y >= v.y + pad && p.x <= v.x + v.w - 1 - pad &&
           p.y <= v.y + v.h - 1 - pad;
}

}  // namespace

BevPipParams BevPipParams::from_json(const Json& cfg) {
    BevPipParams p;
    const Json& j = cfg.path("display.bev_pip");
    if (!j.is_object()) return p;
    p.enabled = j["enabled"].boolean(false);
    const std::string c = j["corner"].str(p.corner);
    if (c == "top_left" || c == "top_right" || c == "bottom_left" || c == "bottom_right")
        p.corner = c;
    p.size = (float)std::max(0.15, std::min(0.5, j["size"].number(0.2)));
    return p;
}

bool bev_layout(const BevScene& s, const BevPipParams& p, int img_w, int img_h, BevView* out) {
    if (!s.valid || s.hull_m.size() < 3 || img_w < 160 || img_h < 120) return false;
    float minx = s.hull_m[0].x, maxx = minx, miny = s.hull_m[0].y, maxy = miny;
    auto grow = [&](Point q) {
        minx = std::min(minx, q.x);
        maxx = std::max(maxx, q.x);
        miny = std::min(miny, q.y);
        maxy = std::max(maxy, q.y);
    };
    for (const Point& q : s.hull_m) grow(q);
    for (const auto& z : s.zones)
        for (const Point& q : z.pts) grow(q);
    // 目标只在标定范围附近时才撑大视野, 远处的离群点会把整张图压成一个点
    const float ex = std::max(1.0f, maxx - minx), ey = std::max(1.0f, maxy - miny);
    const float lx = minx - ex, hx = maxx + ex, ly = miny - ey, hy = maxy + ey;
    for (const auto& t : s.targets)
        if (t.pos.x >= lx && t.pos.x <= hx && t.pos.y >= ly && t.pos.y <= hy) grow(t.pos);
    // 至少 2m, 四周留 5%
    const float cx = (minx + maxx) * 0.5f, cy = (miny + maxy) * 0.5f;
    const float bx = std::max(2.0f, maxx - minx) * 1.1f, by = std::max(2.0f, maxy - miny) * 1.1f;

    BevView v;
    v.minx = cx - bx * 0.5f;
    v.maxx = cx + bx * 0.5f;
    v.miny = cy - by * 0.5f;
    v.maxy = cy + by * 0.5f;
    const float aspect = std::max(0.5f, std::min(1.5f, by / bx));
    v.w = (int)(std::max(0.15f, std::min(0.5f, p.size)) * (float)img_w);
    v.h = (int)((float)v.w * aspect);
    if (v.h > img_h * 6 / 10) {
        v.h = img_h * 6 / 10;
        v.w = (int)((float)v.h / aspect);
    }
    v.w = std::max(80, v.w);
    v.h = std::max(60, v.h);
    // 四角统一贴边, 只留一条极窄的安全边距; 半透明背景不会让角落内容完全消失
    constexpr int kEdgeInset = 4;
    const bool right = p.corner == "top_right" || p.corner == "bottom_right";
    const bool bottom = p.corner == "bottom_left" || p.corner == "bottom_right";
    v.x = right ? img_w - kEdgeInset - v.w : kEdgeInset;
    v.y = bottom ? img_h - kEdgeInset - v.h : kEdgeInset;
    v.x = std::max(0, std::min(img_w - v.w, v.x));
    v.y = std::max(0, std::min(img_h - v.h, v.y));

    const int pad = std::max(4, v.w / 40);
    const int header = text_height(std::max(5, std::min(9, v.w / 45))) + pad;
    const float sx = (float)(v.w - 2 * pad) / bx;
    const float sy = (float)(v.h - 2 * pad - header) / by;
    v.scale = std::max(1e-3f, std::min(sx, sy));
    // 世界中心对齐到画中画绘图区中心
    const float area_cx = v.x + v.w * 0.5f;
    const float area_cy = v.y + header + (v.h - header) * 0.5f;
    v.ox = area_cx - cx * v.scale;
    v.oy = area_cy + cy * v.scale;
    if (out) *out = v;
    return true;
}

bool draw_bev_pip(ImageRGB* img, const BevScene& s, const BevPipParams& p, BevView* view) {
    if (!p.enabled || !img || img->data.size() < img->size_bytes()) return false;
    BevView v;
    if (!bev_layout(s, p, img->width, img->height, &v)) return false;
    if (view) *view = v;

    // 背景压暗到 3/8, 画面内容仍隐约可见
    for (int y = v.y; y < v.y + v.h; ++y) {
        uint8_t* row = img->row(y) + (size_t)v.x * 3;
        for (int i = 0; i < v.w * 3; ++i) row[i] = (uint8_t)((row[i] * 3) >> 3);
    }
    image_buffer_t osd = wrap(img);
    const int font = std::max(5, std::min(9, v.w / 45));

    // 网格: 选一个让格数不超过 8 的整齐步长
    static const float steps[] = {0.5f, 1, 2, 5, 10, 20, 50, 100, 200};
    const float extent = std::max(v.maxx - v.minx, v.maxy - v.miny);
    float step = steps[0];
    for (float st : steps) {
        step = st;
        if (extent / st <= 8.0f) break;
    }
    const float wx0 = (v.x - v.ox) / v.scale, wx1 = (v.x + v.w - v.ox) / v.scale;
    const float wy0 = (v.oy - (v.y + v.h)) / v.scale, wy1 = (v.oy - v.y) / v.scale;
    for (float gx = std::ceil(wx0 / step) * step; gx <= wx1; gx += step)
        line(&osd, v, v.to_px(Point{gx, wy0}), v.to_px(Point{gx, wy1}),
             std::fabs(gx) < 1e-4f ? 0xFF707070 : 0xFF383838, 1);
    for (float gy = std::ceil(wy0 / step) * step; gy <= wy1; gy += step)
        line(&osd, v, v.to_px(Point{wx0, gy}), v.to_px(Point{wx1, gy}),
             std::fabs(gy) < 1e-4f ? 0xFF707070 : 0xFF383838, 1);

    // 有人的地面区域淡红填充
    std::vector<OverlayLayer> fills;
    for (const auto& z : s.zones) {
        if (!z.occupied) continue;
        OverlayLayer l;
        for (const Point& q : z.pts) l.poly.push_back(v.to_px(q));
        l.r = 0xFF;
        l.g = 0x40;
        l.b = 0x40;
        l.alpha = 70;
        fills.push_back(std::move(l));
    }
    fill_regions(img, fills);

    poly(&osd, v, s.hull_m, 0xFF9E9E9E, 2, true);
    for (const auto& z : s.zones) {
        const unsigned int c = z.occupied ? 0xFFFF4040 : z.alarm ? 0xFFFFA500 : 0xFF00D5C8;
        poly(&osd, v, z.pts, c, z.occupied ? 3 : 2, true);
        if (!z.pts.empty() && !z.name.empty()) {
            Point c0{0, 0};
            for (const Point& q : z.pts) {
                c0.x += q.x;
                c0.y += q.y;
            }
            const Point cp = v.to_px(Point{c0.x / z.pts.size(), c0.y / z.pts.size()});
            const int tw = text_width(z.name.c_str(), font);
            if (inside(v, Point{cp.x - tw * 0.5f, cp.y}, 1) &&
                inside(v, Point{cp.x + tw * 0.5f, cp.y + text_height(font)}, 1))
                draw_text(&osd, z.name.c_str(), (int)(cp.x - tw * 0.5f), (int)cp.y, c, font);
        }
    }

    const int r = std::max(4, v.w / 55);
    for (const auto& t : s.targets) {
        const unsigned int c = group_color(t.group);
        if (t.trail.size() >= 2) poly(&osd, v, t.trail, c, 2, false);
        const Point q = v.to_px(t.pos);
        if (!inside(v, q, (float)r)) continue;
        const bool solid = t.visible && t.in_calib;
        draw_circle(&osd, (int)q.x, (int)q.y, r, c, solid ? -1 : 2);
        if (t.has_heading) {
            const float a = t.heading_deg * kDeg2Rad;
            const Point tip{q.x + std::cos(a) * r * 3.0f, q.y - std::sin(a) * r * 3.0f};
            line(&osd, v, q, tip, c, 2);
        }
        char id[16];
        snprintf(id, sizeof(id), "%d", t.id);
        const Point lp{q.x + r + 2, q.y - r - text_height(font)};
        if (inside(v, lp, 0) &&
            inside(v, Point{lp.x + text_width(id, font), lp.y + text_height(font)}, 0))
            draw_text(&osd, id, (int)lp.x, (int)lp.y, COLOR_WHITE, font);
    }

    char title[48];
    snprintf(title, sizeof(title), "BEV  grid %gm", step);
    const int pad = std::max(4, v.w / 40);
    if (text_width(title, font) < v.w - 2 * pad)
        draw_text(&osd, title, v.x + pad, v.y + pad / 2, COLOR_WHITE, font);
    draw_rectangle(&osd, v.x, v.y, v.w - 1, v.h - 1, 0xFFB0B0B0, 1);
    return true;
}

}  // namespace mb
