#include "video/view_guard.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace mb {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kGridX = 8;
constexpr int kGridY = 5;
constexpr int kHalf0 = 20;  // 分析分辨率下的块半宽 (41x41)

struct Plane {
    int w = 0;
    int h = 0;
    std::vector<float> v;
    float at(int x, int y) const { return v[(size_t)y * w + x]; }
};

Plane gradient(const GrayImage& g) {
    Plane p;
    p.w = g.width;
    p.h = g.height;
    p.v.assign((size_t)p.w * p.h, 0.0f);
    for (int y = 1; y + 1 < p.h; ++y) {
        const uint8_t* r = g.data.data() + (size_t)y * g.width;
        for (int x = 1; x + 1 < p.w; ++x) {
            const float gx = (float)r[x + 1] - (float)r[x - 1];
            const float gy = (float)r[x + g.width] - (float)r[x - g.width];
            p.v[(size_t)y * p.w + x] = std::fabs(gx) + std::fabs(gy);
        }
    }
    return p;
}

Plane half(const Plane& s) {
    Plane p;
    p.w = s.w / 2;
    p.h = s.h / 2;
    p.v.resize((size_t)p.w * p.h);
    for (int y = 0; y < p.h; ++y)
        for (int x = 0; x < p.w; ++x)
            p.v[(size_t)y * p.w + x] = 0.25f * (s.at(2 * x, 2 * y) + s.at(2 * x + 1, 2 * y) +
                                                s.at(2 * x, 2 * y + 1) + s.at(2 * x + 1, 2 * y + 1));
    return p;
}

bool inside(const Plane& p, int cx, int cy, int hs) {
    return cx - hs >= 0 && cy - hs >= 0 && cx + hs < p.w && cy + hs < p.h;
}

float patch_std(const Plane& p, int cx, int cy, int hs) {
    double s = 0, ss = 0;
    const int n = (2 * hs + 1) * (2 * hs + 1);
    for (int y = cy - hs; y <= cy + hs; ++y)
        for (int x = cx - hs; x <= cx + hs; ++x) {
            const float v = p.at(x, y);
            s += v;
            ss += (double)v * v;
        }
    const double var = ss / n - (s / n) * (s / n);
    return var > 0 ? (float)std::sqrt(var) : 0.0f;
}

// 归一化互相关, 窗口必须都在图内。
float zncc(const Plane& a, int ax, int ay, const Plane& b, int bx, int by, int hs) {
    double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
    for (int dy = -hs; dy <= hs; ++dy) {
        const float* ra = a.v.data() + (size_t)(ay + dy) * a.w + ax - hs;
        const float* rb = b.v.data() + (size_t)(by + dy) * b.w + bx - hs;
        for (int dx = 0; dx <= 2 * hs; ++dx) {
            const double va = ra[dx], vb = rb[dx];
            sa += va;
            sb += vb;
            saa += va * va;
            sbb += vb * vb;
            sab += va * vb;
        }
    }
    const double n = (double)(2 * hs + 1) * (2 * hs + 1);
    const double cov = sab - sa * sb / n;
    const double va = saa - sa * sa / n;
    const double vb = sbb - sb * sb / n;
    if (va <= 1e-6 || vb <= 1e-6) return -1.0f;
    return (float)(cov / std::sqrt(va * vb));
}

struct Pair {
    float rx, ry, cx, cy;  // 参考 / 当前, 分析分辨率像素
};

// 像素坐标下的相似变换 x' = a x - b y + tx, y' = b x + a y + ty。
struct Sim {
    float a = 1, b = 0, tx = 0, ty = 0;
    void apply(float x, float y, float* ox, float* oy) const {
        *ox = a * x - b * y + tx;
        *oy = b * x + a * y + ty;
    }
};

bool fit_sim(const std::vector<Pair>& ps, const std::vector<int>& idx, Sim* out) {
    if (idx.size() < 2) return false;
    double mx = 0, my = 0, nx = 0, ny = 0;
    for (int i : idx) {
        mx += ps[i].rx;
        my += ps[i].ry;
        nx += ps[i].cx;
        ny += ps[i].cy;
    }
    const double n = (double)idx.size();
    mx /= n;
    my /= n;
    nx /= n;
    ny /= n;
    double sxx = 0, sa = 0, sb = 0;
    for (int i : idx) {
        const double px = ps[i].rx - mx, py = ps[i].ry - my;
        const double qx = ps[i].cx - nx, qy = ps[i].cy - ny;
        sxx += px * px + py * py;
        sa += px * qx + py * qy;
        sb += px * qy - py * qx;
    }
    if (sxx < 1e-6) return false;
    Sim s;
    s.a = (float)(sa / sxx);
    s.b = (float)(sb / sxx);
    s.tx = (float)(nx - (s.a * mx - s.b * my));
    s.ty = (float)(ny - (s.b * mx + s.a * my));
    *out = s;
    return true;
}

std::vector<int> inliers_of(const std::vector<Pair>& ps, const Sim& s, float thr, double* err) {
    std::vector<int> in;
    double e = 0;
    for (size_t i = 0; i < ps.size(); ++i) {
        float x, y;
        s.apply(ps[i].rx, ps[i].ry, &x, &y);
        const float d2 = (x - ps[i].cx) * (x - ps[i].cx) + (y - ps[i].cy) * (y - ps[i].cy);
        if (d2 <= thr * thr) {
            in.push_back((int)i);
            e += d2;
        }
    }
    if (err) *err = e;
    return in;
}

bool in_box(const std::vector<NormBox>& boxes, float x, float y) {
    for (const NormBox& b : boxes) {
        const float mx = (b.x2 - b.x1) * 0.1f, my = (b.y2 - b.y1) * 0.1f;
        if (x >= b.x1 - mx && x <= b.x2 + mx && y >= b.y1 - my && y <= b.y2 + my) return true;
    }
    return false;
}

float image_mean(const GrayImage& g) {
    if (g.data.empty()) return 0;
    uint64_t s = 0;
    for (uint8_t v : g.data) s += v;
    return (float)s / (float)g.data.size();
}

Json affine_json(const Affine2& t) {
    Json a = Json::array();
    for (float v : t.m) a.push(Json::number_value(std::round(v * 1e6) / 1e6));
    return a;
}

bool affine_from(const Json& a, Affine2* out) {
    if (!a.is_array() || a.size() != 6) return false;
    for (int i = 0; i < 6; ++i) out->m[i] = (float)a[i].number(i == 0 || i == 4 ? 1.0 : 0.0);
    return true;
}

Json shift_json(const ViewShift& s) {
    Json j = Json::object();
    j.set("dx", Json::number_value(std::round(s.dx * 1e4) / 1e4));
    j.set("dy", Json::number_value(std::round(s.dy * 1e4) / 1e4));
    j.set("rotation_deg", Json::number_value(std::round(s.rotation_deg * 100) / 100));
    j.set("scale", Json::number_value(std::round(s.scale * 1e4) / 1e4));
    return j;
}

bool write_atomic(const std::string& path, const void* data, size_t n) {
    const std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    bool ok = f && fwrite(data, 1, n, f) == n;
    if (f) ok = (fclose(f) == 0) && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        return false;
    }
    return true;
}

bool write_pgm(const std::string& path, const GrayImage& g) {
    char head[64];
    const int hn = snprintf(head, sizeof(head), "P5\n%d %d\n255\n", g.width, g.height);
    std::vector<uint8_t> buf(head, head + hn);
    buf.insert(buf.end(), g.data.begin(), g.data.end());
    return write_atomic(path, buf.data(), buf.size());
}

bool read_pgm(const std::string& path, GrayImage* g) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    int w = 0, h = 0, mx = 0;
    const bool ok = fscanf(f, "P5 %d %d %d", &w, &h, &mx) == 3 && fgetc(f) != EOF && w > 0 &&
                    h > 0 && w <= 4096 && h <= 4096 && mx == 255;
    if (ok) {
        g->width = w;
        g->height = h;
        g->data.resize((size_t)w * h);
        const bool full = fread(g->data.data(), 1, g->data.size(), f) == g->data.size();
        fclose(f);
        return full;
    }
    fclose(f);
    return false;
}

}  // namespace

void downscale_gray(const ImageRGB& src, int w, int h, GrayImage* out) {
    out->width = w;
    out->height = h;
    out->data.assign((size_t)w * h, 0);
    if (src.width <= 0 || src.height <= 0 || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; ++y) {
        const int y0 = y * src.height / h;
        const int y1 = std::max(y0 + 1, (y + 1) * src.height / h);
        for (int x = 0; x < w; ++x) {
            const int x0 = x * src.width / w;
            const int x1 = std::max(x0 + 1, (x + 1) * src.width / w);
            uint32_t sum = 0, n = 0;
            for (int yy = y0; yy < y1; ++yy) {
                const uint8_t* r = src.row(yy) + x0 * 3;
                for (int xx = x0; xx < x1; ++xx, r += 3) {
                    sum += (77u * r[0] + 150u * r[1] + 29u * r[2]) >> 8;
                    ++n;
                }
            }
            out->data[(size_t)y * w + x] = (uint8_t)(sum / std::max(1u, n));
        }
    }
}

AlignResult align_views(const GrayImage& ref, const GrayImage& cur, float max_shift,
                        const std::vector<std::vector<Point>>& ref_masks,
                        const std::vector<NormBox>& cur_masks) {
    AlignResult res;
    if (ref.width != cur.width || ref.height != cur.height || ref.width < 64 || ref.height < 36)
        return res;
    const int W = ref.width, H = ref.height;
    const Plane r0 = gradient(ref), c0 = gradient(cur);
    const Plane r1 = half(r0), c1 = half(c0);
    const Plane r2 = half(r1), c2 = half(c1);
    const int hs0 = kHalf0, hs1 = kHalf0 / 2, hs2 = kHalf0 / 4;
    const int radius2 = (int)std::ceil(std::max(0.02f, max_shift) * r2.w) + 1;
    const float scale_px = W / 320.0f;

    std::vector<Pair> pairs;
    const int mx = hs0 + 4, my = hs0 + 4;
    for (int gy = 0; gy < kGridY; ++gy) {
        for (int gx = 0; gx < kGridX; ++gx) {
            const int px = mx + (int)std::lround((double)gx * (W - 1 - 2 * mx) / (kGridX - 1));
            const int py = my + (int)std::lround((double)gy * (H - 1 - 2 * my) / (kGridY - 1));
            const Point pn{(float)px / W, (float)py / H};
            bool masked = false;
            for (const std::vector<Point>& poly : ref_masks)
                masked = masked || (poly.size() >= 3 && point_in_polygon(poly, pn));
            if (masked || !inside(r0, px, py, hs0)) continue;
            if (patch_std(r0, px, py, hs0) < 6.0f) continue;
            const int qx2 = px / 4, qy2 = py / 4;
            if (!inside(r2, qx2, qy2, hs2) || patch_std(r2, qx2, qy2, hs2) < 3.0f) continue;
            ++res.textured;

            // 粗: 1/4 分辨率全范围搜, 记住最好和次好 (相隔 2 像素以上) 的位置
            float best = -2, second = -2;
            int bx = 0, by = 0;
            std::vector<std::pair<float, std::pair<int, int>>> scores;
            for (int dy = -radius2; dy <= radius2; ++dy) {
                for (int dx = -radius2; dx <= radius2; ++dx) {
                    const int cx = qx2 + dx, cy = qy2 + dy;
                    if (!inside(c2, cx, cy, hs2)) continue;
                    const float s = zncc(r2, qx2, qy2, c2, cx, cy, hs2);
                    scores.push_back({s, {cx, cy}});
                    if (s > best) {
                        best = s;
                        bx = cx;
                        by = cy;
                    }
                }
            }
            for (const auto& sc : scores) {
                if (std::abs(sc.second.first - bx) <= 2 && std::abs(sc.second.second - by) <= 2)
                    continue;
                second = std::max(second, sc.first);
            }
            if (best < 0.4f || best - second < 0.02f) continue;

            // 细: 每层 ±2 像素
            int fx = bx * 2, fy = by * 2;
            const int qx1 = px / 2, qy1 = py / 2;
            if (!inside(r1, qx1, qy1, hs1)) continue;
            float b1 = -2;
            int nx1 = fx, ny1 = fy;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int cx = fx + dx + (qx1 - 2 * qx2), cy = fy + dy + (qy1 - 2 * qy2);
                    if (!inside(c1, cx, cy, hs1)) continue;
                    const float s = zncc(r1, qx1, qy1, c1, cx, cy, hs1);
                    if (s > b1) {
                        b1 = s;
                        nx1 = cx;
                        ny1 = cy;
                    }
                }
            if (b1 < 0.4f) continue;
            fx = nx1 * 2 + (px - 2 * qx1);
            fy = ny1 * 2 + (py - 2 * qy1);
            float b0 = -2;
            int nx0 = fx, ny0 = fy;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const int cx = fx + dx, cy = fy + dy;
                    if (!inside(c0, cx, cy, hs0)) continue;
                    const float s = zncc(r0, px, py, c0, cx, cy, hs0);
                    if (s > b0) {
                        b0 = s;
                        nx0 = cx;
                        ny0 = cy;
                    }
                }
            if (b0 < 0.5f) continue;
            if (in_box(cur_masks, (float)nx0 / W, (float)ny0 / H)) continue;
            pairs.push_back(Pair{(float)px, (float)py, (float)nx0, (float)ny0});
        }
    }
    res.matched = (int)pairs.size();
    if (pairs.size() < 3) return res;

    // RANSAC: 任意两对点确定一个相似变换。按截断误差 (MSAC) 打分而不是只数内点:
    // 场景里有人车变化时, 只数内点容易挑中"吃进几个错配块"的模型, 截断误差偏向贴得最紧的。
    const float thr = 3.0f * scale_px;
    const double thr2 = (double)thr * thr;
    Sim best_s;
    double best_cost = 1e300;
    bool have = false;
    for (size_t i = 0; i < pairs.size(); ++i) {
        for (size_t j = i + 1; j < pairs.size(); ++j) {
            const float ddx = pairs[j].rx - pairs[i].rx, ddy = pairs[j].ry - pairs[i].ry;
            if (ddx * ddx + ddy * ddy < 400.0f * scale_px * scale_px) continue;
            Sim s;
            if (!fit_sim(pairs, {(int)i, (int)j}, &s)) continue;
            const float sc = std::sqrt(s.a * s.a + s.b * s.b);
            if (sc < 0.8f || sc > 1.25f) continue;
            double cost = 0;
            for (const Pair& q : pairs) {
                float x, y;
                s.apply(q.rx, q.ry, &x, &y);
                const double d2 = (double)(x - q.cx) * (x - q.cx) + (double)(y - q.cy) * (y - q.cy);
                cost += std::min(d2, thr2);
            }
            if (cost < best_cost) {
                best_cost = cost;
                best_s = s;
                have = true;
            }
        }
    }
    if (!have) return res;
    // 内点重新拟合, 再用更紧的门限收一次
    Sim s = best_s;
    std::vector<int> best_in = inliers_of(pairs, s, thr, nullptr);
    if (best_in.size() < 3 || !fit_sim(pairs, best_in, &s)) return res;
    const float tight = 1.5f * scale_px;
    std::vector<int> in = inliers_of(pairs, s, tight, nullptr);
    Sim refined;
    if (in.size() >= 5 && fit_sim(pairs, in, &refined)) {
        s = refined;
        best_in = inliers_of(pairs, s, tight, nullptr);
    }
    double e = 0;
    best_in = inliers_of(pairs, s, best_in.size() >= 5 ? tight : thr, &e);
    res.inliers = (int)best_in.size();
    res.rms_px = res.inliers ? (float)std::sqrt(e / res.inliers) : 0.0f;

    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (int i : best_in) {
        minx = std::min(minx, pairs[i].rx);
        maxx = std::max(maxx, pairs[i].rx);
        miny = std::min(miny, pairs[i].ry);
        maxy = std::max(maxy, pairs[i].ry);
    }
    // 内点要够多、在画面上铺得开 (只靠一角的几块容易被局部运动带偏)
    res.confident = res.inliers >= 5 && res.inliers * 4 >= res.textured &&
                    maxx - minx >= 0.35f * W && maxy - miny >= 0.2f * H;

    Affine2 t;
    t.m[0] = s.a;
    t.m[1] = -s.b * H / W;
    t.m[2] = s.tx / W;
    t.m[3] = s.b * W / H;
    t.m[4] = s.a;
    t.m[5] = s.ty / H;
    res.transform = t;
    return res;
}

ViewShift describe_shift(const Affine2& t, float aspect) {
    ViewShift s;
    const Point c = t.apply(Point{0.5f, 0.5f});
    s.dx = c.x - 0.5f;
    s.dy = c.y - 0.5f;
    // 换到等比例的像素坐标 (宽 = aspect, 高 = 1) 再取旋转和缩放
    const float a = t.m[0], b = t.m[1] * aspect, cc = t.m[3] / aspect, d = t.m[4];
    s.scale = std::sqrt(std::fabs(a * d - b * cc));
    s.rotation_deg = std::atan2(cc - b, a + d) * 180.0f / kPi;
    return s;
}

float transform_distance(const Affine2& a, const Affine2& b, float aspect) {
    float worst = 0;
    for (float y : {0.1f, 0.5f, 0.9f})
        for (float x : {0.1f, 0.5f, 0.9f}) {
            const Point pa = a.apply(Point{x, y}), pb = b.apply(Point{x, y});
            const float dx = pa.x - pb.x, dy = (pa.y - pb.y) / aspect;
            worst = std::max(worst, std::sqrt(dx * dx + dy * dy));
        }
    return worst;
}

float visible_fraction(const Affine2& t) {
    int in = 0;
    const int n = 16;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            const Point p = t.apply(Point{(x + 0.5f) / n, (y + 0.5f) / n});
            if (p.x >= 0 && p.x <= 1 && p.y >= 0 && p.y <= 1) ++in;
        }
    return (float)in / (n * n);
}

// ============================================================ ViewGuard

ViewGuard::Params ViewGuard::Params::from_json(const Json& cfg) {
    Params p;
    const Json& v = cfg["view_guard"];
    p.enabled = v["enabled"].boolean(true);
    p.auto_correct = v["auto_correct"].boolean(true);
    p.interval_sec = (float)std::max(2.0, v["interval_sec"].number(15.0));
    p.confirm_sec = (float)std::max(0.0, v["confirm_sec"].number(60.0));
    p.settle_sec = (float)std::max(0.0, v["settle_sec"].number(10.0));
    p.alert_sec = (float)std::max(0.0, v["alert_sec"].number(600.0));
    p.dead_band = (float)std::max(0.002, v["dead_band"].number(0.01));
    p.max_shift = (float)std::max(0.01, std::min(0.45, v["max_shift"].number(0.2)));
    p.max_rotation_deg = (float)std::max(0.0, v["max_rotation_deg"].number(6.0));
    p.max_scale_change = (float)std::max(0.0, v["max_scale_change"].number(0.1));
    p.min_visible = (float)std::max(0.0, std::min(1.0, v["min_visible"].number(0.7)));
    p.dark_level = std::max(0, v["dark_level"].integer(12));
    p.dir = v["dir"].str("/userdata/monitor_box");
    return p;
}

ViewGuard::~ViewGuard() { stop(); }

void ViewGuard::configure(const Params& p) {
    std::lock_guard<std::mutex> lk(mu_);
    p_ = p;
    if (!p_.enabled) state_ = "off";
    else if (state_ == "off") state_ = "starting";
}

ViewGuard::Params ViewGuard::params() const {
    std::lock_guard<std::mutex> lk(mu_);
    return p_;
}

void ViewGuard::set_ignore(std::vector<std::vector<Point>> polys) {
    std::lock_guard<std::mutex> lk(mu_);
    ignore_ = std::move(polys);
}

bool ViewGuard::load(std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (p_.dir.empty()) return false;
    Json j;
    std::string perr;
    if (!Json::parse_file(p_.dir + "/view_guard.json", &j, &perr)) {
        if (err) *err = perr;
        return false;
    }
    std::vector<Ref> refs;
    const Json& rs = j["references"];
    for (size_t i = 0; rs.is_array() && i < rs.size() && (int)refs.size() < kMaxRefs; ++i) {
        Ref r;
        const std::string file = rs[i]["file"].str("");
        if (file.empty() || file.find('/') != std::string::npos ||
            !read_pgm(p_.dir + "/" + file, &r.img) || !affine_from(rs[i]["base_to_ref"], &r.base_to_ref))
            continue;
        r.mean = (float)rs[i]["mean"].number(image_mean(r.img));
        r.created = (time_t)rs[i]["created"].number(0.0);
        if (!refs.empty() && (r.img.width != refs[0].img.width || r.img.height != refs[0].img.height))
            continue;
        refs.push_back(std::move(r));
    }
    if (refs.empty()) {
        if (err) *err = "参考画面缺失";
        return false;
    }
    refs_.swap(refs);
    affine_from(j["applied"], &applied_);
    notice_ = j["notice"].str("");
    notice_time_ = (time_t)j["notice_time"].number(0.0);
    acked_ = j["acked"].boolean(true);
    aspect_ = (float)refs_[0].img.width / (float)refs_[0].img.height;
    return true;
}

void ViewGuard::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (running_ || synchronous_) return;
    running_ = true;
    th_ = std::thread(&ViewGuard::loop, this);
}

void ViewGuard::stop() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!running_) return;
        running_ = false;
    }
    cv_.notify_all();
    if (th_.joinable()) th_.join();
}

void ViewGuard::loop() {
    std::unique_lock<std::mutex> lk(mu_);
    while (running_) {
        cv_.wait(lk, [this] { return !running_ || has_input_; });
        if (!running_) break;
        GrayImage img = std::move(in_img_);
        std::vector<NormBox> boxes = std::move(in_boxes_);
        const uint64_t ms = in_ms_;
        has_input_ = false;
        busy_ = true;
        lk.unlock();
        process(img, boxes, ms);
        lk.lock();
        busy_ = false;
    }
}

bool ViewGuard::wants_frame(uint64_t now_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!p_.enabled) return false;
    // 中断过 (待机/断线): 摄像头重新打开后先等曝光稳定
    if (last_seen_ms_ == 0 || now_ms > last_seen_ms_ + 3000)
        settle_until_ms_ = now_ms + (uint64_t)(p_.settle_sec * 1000.0f);
    last_seen_ms_ = now_ms;
    if (busy_ || has_input_ || now_ms < settle_until_ms_) return false;
    // 等着建参考/执行命令时每秒试一次 (天黑时不会每帧都缩图)
    if (cmd_ != Cmd::None || refs_.empty()) return now_ms >= last_offer_ms_ + 1000;
    return now_ms >= next_due_ms_;
}

void ViewGuard::offer(GrayImage img, std::vector<NormBox> boxes, uint64_t now_ms) {
    {
        std::lock_guard<std::mutex> lk(mu_);
        last_offer_ms_ = now_ms;
        next_due_ms_ = now_ms + (uint64_t)(p_.interval_sec * 1000.0f);
        if (!synchronous_) {
            in_img_ = std::move(img);
            in_boxes_ = std::move(boxes);
            in_ms_ = now_ms;
            has_input_ = true;
        }
    }
    if (synchronous_) process(img, boxes, now_ms);
    else cv_.notify_one();
}

bool ViewGuard::take_change(Change* out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!has_change_) return false;
    *out = change_;
    has_change_ = false;
    change_ = Change();
    return true;
}

Affine2 ViewGuard::applied() const {
    std::lock_guard<std::mutex> lk(mu_);
    return applied_;
}

void ViewGuard::push_change_locked(const Affine2& applied, bool warp, const Affine2& delta) {
    // 主线程还没取走上一次变化时合并: 热图要按累积的位移搬
    if (has_change_ && change_.warp_heat) {
        change_.heat_delta = warp ? compose(delta, change_.heat_delta) : change_.heat_delta;
    } else {
        change_.warp_heat = warp;
        change_.heat_delta = delta;
    }
    change_.applied = applied;
    has_change_ = true;
}

void ViewGuard::set_notice_locked(const std::string& notice) {
    if (notice == notice_) return;
    notice_ = notice;
    notice_time_ = notice.empty() ? 0 : wall_now();
    acked_ = notice.empty();
}

void ViewGuard::reference_locked(const char* why) {
    const bool was = !applied_.is_identity();
    applied_ = Affine2();
    set_notice_locked("");
    has_pending_ = false;
    low_since_ms_ = 0;
    cmd_ = Cmd::Reference;
    ++gen_;
    state_ = "referencing";
    if (was) push_change_locked(applied_, false, Affine2());
    printf("[view] 以当前画面为新的参考 (%s)\n", why);
    fflush(stdout);
}

void ViewGuard::request_reference(const char* why) {
    std::lock_guard<std::mutex> lk(mu_);
    reference_locked(why);
    std::string err;
    save_locked(false, &err);
}

bool ViewGuard::command(const std::string& action, std::string* err) {
    std::lock_guard<std::mutex> lk(mu_);
    if (action == "ack") {
        acked_ = true;
    } else if (action == "reference" || action == "undo") {
        reference_locked(action == "undo" ? "撤销自动校正" : "用户确认");
    } else if (action == "accept") {
        if (refs_.empty()) {
            if (err) *err = "还没有参考画面";
            return false;
        }
        cmd_ = Cmd::Accept;
        ++gen_;
    } else {
        if (err) *err = "action 只能是 ack / reference / undo / accept";
        return false;
    }
    std::string serr;
    save_locked(false, &serr);
    return true;
}

bool ViewGuard::save_locked(bool refs, std::string* err) {
    if (p_.dir.empty()) return false;
    Json j = Json::object();
    j.set("version", Json::number_value(1));
    j.set("applied", affine_json(applied_));
    j.set("notice", Json::string_value(notice_));
    j.set("notice_time", Json::number_value((double)notice_time_));
    j.set("acked", Json::bool_value(acked_));
    Json rs = Json::array();
    for (size_t i = 0; i < refs_.size(); ++i) {
        const std::string file = "view_ref_" + std::to_string(i) + ".pgm";
        if (refs && !write_pgm(p_.dir + "/" + file, refs_[i].img)) {
            if (err) *err = "写 " + p_.dir + "/" + file + " 失败";
            return false;
        }
        Json r = Json::object();
        r.set("file", Json::string_value(file));
        r.set("base_to_ref", affine_json(refs_[i].base_to_ref));
        r.set("mean", Json::number_value(std::round(refs_[i].mean * 10) / 10));
        r.set("created", Json::number_value((double)refs_[i].created));
        rs.push(r);
    }
    j.set("references", rs);
    const std::string text = j.dump(1) + "\n";
    if (!write_atomic(p_.dir + "/view_guard.json", text.data(), text.size())) {
        if (err) *err = "写 " + p_.dir + "/view_guard.json 失败";
        if (save_error_.empty()) fprintf(stderr, "[view] %s\n", err ? err->c_str() : "保存失败");
        save_error_ = "保存失败";
        return false;
    }
    save_error_.clear();
    return true;
}

void ViewGuard::process(const GrayImage& img, const std::vector<NormBox>& boxes, uint64_t now_ms) {
    std::unique_lock<std::mutex> lk(mu_);
    const Params p = p_;
    const Cmd cmd = cmd_;
    const uint64_t gen = gen_;
    const float mean = image_mean(img);
    last_mean_ = mean;
    const float aspect = (float)img.width / (float)std::max(1, img.height);
    const bool size_changed =
        !refs_.empty() && (refs_[0].img.width != img.width || refs_[0].img.height != img.height);

    if (cmd == Cmd::Reference || refs_.empty() || size_changed) {
        if (mean < p.dark_level) {
            state_ = "dark";
            return;
        }
        Ref r;
        r.img = img;
        r.mean = mean;
        r.created = wall_now();
        refs_.clear();
        refs_.push_back(std::move(r));
        aspect_ = aspect;
        if (!applied_.is_identity()) push_change_locked(Affine2(), false, Affine2());
        applied_ = Affine2();
        set_notice_locked("");
        has_pending_ = false;
        low_since_ms_ = 0;
        cmd_ = Cmd::None;
        state_ = "ok";
        std::string err;
        save_locked(true, &err);
        printf("[view] 已保存参考画面 (%dx%d, 亮度 %.0f)\n", img.width, img.height, mean);
        fflush(stdout);
        return;
    }
    if (mean < p.dark_level) {
        state_ = "dark";
        low_since_ms_ = 0;
        return;
    }
    const std::vector<Ref> refs = refs_;
    const std::vector<std::vector<Point>> ignore = ignore_;
    lk.unlock();

    AlignResult best;
    Affine2 best_t;
    const auto t0 = std::chrono::steady_clock::now();
    for (const Ref& r : refs) {
        std::vector<std::vector<Point>> masks;
        for (const std::vector<Point>& poly : ignore) {
            std::vector<Point> m;
            for (const Point& q : poly) m.push_back(r.base_to_ref.apply(q));
            masks.push_back(std::move(m));
        }
        const AlignResult a = align_views(r.img, img, p.max_shift + 0.05f, masks, boxes);
        if (!best.confident || (a.confident && a.inliers > best.inliers)) {
            if (a.confident || a.inliers >= best.inliers) {
                best = a;
                best_t = compose(a.transform, r.base_to_ref);
            }
        }
    }

    lk.lock();
    if (gen != gen_) return;  // 处理期间用户改了参考, 这次结果作废
    last_ = best;
    last_ms_ = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count();
    last_measured_ = best_t;
    last_time_ = wall_now();

    if (cmd == Cmd::Accept) {
        Ref r;
        r.img = img;
        r.mean = mean;
        r.created = wall_now();
        r.base_to_ref = best.confident ? best_t : applied_;
        if ((int)refs_.size() >= kMaxRefs) refs_.erase(refs_.begin() + 1);
        refs_.push_back(std::move(r));
        cmd_ = Cmd::None;
        set_notice_locked("");
        low_since_ms_ = 0;
        has_pending_ = false;
        state_ = applied_.is_identity() ? "ok" : "corrected";
        std::string err;
        save_locked(true, &err);
        printf("[view] 当前画面已记为补充参考 (共 %zu 张)\n", refs_.size());
        fflush(stdout);
        return;
    }

    if (!best.confident) {
        if (low_since_ms_ == 0) low_since_ms_ = now_ms;
        state_ = "uncertain";
        if (now_ms >= low_since_ms_ + (uint64_t)(p.alert_sec * 1000.0f)) {
            state_ = "unmatched";
            if (notice_ != "unmatched") {
                set_notice_locked("unmatched");
                std::string err;
                save_locked(false, &err);
                printf("[view] 画面持续对不上参考画面 (纹理块 %d, 匹配 %d, 一致 %d), "
                       "摄像头可能被移动或遮挡\n", best.textured, best.matched, best.inliers);
                fflush(stdout);
            }
        }
        return;
    }
    low_since_ms_ = 0;
    if (notice_ == "unmatched") {
        set_notice_locked("");
        std::string err;
        save_locked(false, &err);
    }

    const float d = transform_distance(best_t, applied_, aspect);
    if (d < p.dead_band) {
        has_pending_ = false;
        state_ = applied_.is_identity() ? "ok" : "corrected";
        bool dirty = false;
        if (notice_ == "moved") {
            set_notice_locked("");
            dirty = true;
        }
        // 光照差别很大但仍然对得上 (傍晚/夜视): 记一张补充参考, 夜里更容易对上
        float nearest = 1e9f;
        for (const Ref& r : refs_) nearest = std::min(nearest, std::fabs(r.mean - mean));
        bool refs_dirty = false;
        if ((int)refs_.size() < kMaxRefs && nearest >= 30.0f) {
            Ref r;
            r.img = img;
            r.mean = mean;
            r.created = wall_now();
            r.base_to_ref = best_t;
            refs_.push_back(std::move(r));
            refs_dirty = dirty = true;
            printf("[view] 光照变化 (亮度 %.0f), 补充一张参考画面\n", mean);
            fflush(stdout);
        }
        if (dirty) {
            std::string err;
            save_locked(refs_dirty, &err);
        }
        return;
    }

    if (has_pending_ && transform_distance(best_t, pending_, aspect) < p.dead_band) {
        ++pending_count_;
        pending_ = best_t;
    } else {
        has_pending_ = true;
        pending_ = best_t;
        pending_since_ms_ = now_ms;
        pending_count_ = 1;
    }
    if (notice_ != "moved") state_ = "checking";
    if (pending_count_ < 3 || now_ms < pending_since_ms_ + (uint64_t)(p.confirm_sec * 1000.0f))
        return;

    const ViewShift s = describe_shift(best_t, aspect);
    const bool can_auto = p.auto_correct && std::fabs(s.rotation_deg) <= p.max_rotation_deg &&
                          std::fabs(s.scale - 1.0f) <= p.max_scale_change &&
                          std::max(std::fabs(s.dx), std::fabs(s.dy)) <= p.max_shift &&
                          visible_fraction(best_t) >= p.min_visible;
    if (can_auto) {
        Affine2 inv;
        Affine2 delta;
        if (invert_affine(applied_, &inv)) delta = compose(best_t, inv);
        applied_ = best_t;
        has_pending_ = false;
        state_ = "corrected";
        notice_.clear();
        set_notice_locked("corrected");
        push_change_locked(applied_, true, delta);
        std::string err;
        save_locked(false, &err);
        printf("[view] 摄像头画面偏移, 区域已自动跟随: 平移 %.1f%%, %.1f%%, 旋转 %.1f°, 缩放 %.3f\n",
               s.dx * 100, s.dy * 100, s.rotation_deg, s.scale);
    } else {
        state_ = "moved";
        if (notice_ != "moved") {
            set_notice_locked("moved");
            std::string err;
            save_locked(false, &err);
            printf("[view] 摄像头位置变化过大, 不自动跟随: 平移 %.1f%%, %.1f%%, 旋转 %.1f°, "
                   "缩放 %.3f, 区域需要确认\n", s.dx * 100, s.dy * 100, s.rotation_deg, s.scale);
        }
    }
    fflush(stdout);
}

Json ViewGuard::json() const {
    std::lock_guard<std::mutex> lk(mu_);
    Json j = Json::object();
    j.set("enabled", Json::bool_value(p_.enabled));
    j.set("auto_correct", Json::bool_value(p_.auto_correct));
    j.set("state", Json::string_value(p_.enabled ? state_ : "off"));
    j.set("applied", affine_json(applied_));
    j.set("corrected", Json::bool_value(!applied_.is_identity()));
    j.set("shift", shift_json(describe_shift(applied_, aspect_)));
    j.set("notice", Json::string_value(notice_));
    j.set("notice_time", Json::number_value((double)notice_time_));
    j.set("acked", Json::bool_value(acked_));
    j.set("pending", Json::string_value(cmd_ == Cmd::Reference ? "reference"
                                        : cmd_ == Cmd::Accept  ? "accept" : ""));
    j.set("references", Json::number_value((double)refs_.size()));
    if (!refs_.empty()) j.set("reference_time", Json::number_value((double)refs_[0].created));
    Json last = Json::object();
    last.set("time", Json::number_value((double)last_time_));
    last.set("confident", Json::bool_value(last_.confident));
    last.set("textured", Json::number_value(last_.textured));
    last.set("matched", Json::number_value(last_.matched));
    last.set("inliers", Json::number_value(last_.inliers));
    last.set("brightness", Json::number_value(std::round(last_mean_)));
    last.set("align_ms", Json::number_value(std::round(last_ms_ * 10) / 10));
    if (last_.confident) last.set("shift", shift_json(describe_shift(last_measured_, aspect_)));
    j.set("last", last);
    if (!save_error_.empty()) j.set("error", Json::string_value(save_error_));
    return j;
}

}  // namespace mb
