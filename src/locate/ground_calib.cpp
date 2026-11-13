#include "locate/ground_calib.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace mb {

namespace {

// 对称矩阵的 Jacobi 特征分解 (n <= 9, 迭代到非对角元可忽略)。
// a 会被破坏; vals[i] 与 vecs 的第 i 列对应。
void jacobi_eigen(double a[9][9], int n, double vals[9], double vecs[9][9]) {
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) vecs[i][j] = i == j ? 1.0 : 0.0;
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) off += a[i][j] * a[i][j];
        if (off < 1e-30) break;
        for (int p = 0; p < n; ++p) {
            for (int q = p + 1; q < n; ++q) {
                if (std::fabs(a[p][q]) < 1e-300) continue;
                const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < n; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < n; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < n; ++k) {
                    const double vkp = vecs[k][p], vkq = vecs[k][q];
                    vecs[k][p] = c * vkp - s * vkq;
                    vecs[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    for (int i = 0; i < n; ++i) vals[i] = a[i][i];
}

// Hartley 归一化: 平移到质心, 缩放到平均距离 sqrt(2)。t 为 3x3 行主序。
bool normalizer(const std::vector<Point>& pts, double t[9]) {
    double cx = 0, cy = 0;
    for (const Point& p : pts) {
        cx += p.x;
        cy += p.y;
    }
    cx /= (double)pts.size();
    cy /= (double)pts.size();
    double d = 0;
    for (const Point& p : pts) d += std::hypot(p.x - cx, p.y - cy);
    d /= (double)pts.size();
    if (d < 1e-12) return false;
    const double s = std::sqrt(2.0) / d;
    const double m[9] = {s, 0, -s * cx, 0, s, -s * cy, 0, 0, 1};
    std::copy(m, m + 9, t);
    return true;
}

void mul3(const double a[9], const double b[9], double out[9]) {
    double r[9];
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 3; ++j)
            r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
    std::copy(r, r + 9, out);
}

bool inv3(const double m[9], double out[9]) {
    const double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                       m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-300) return false;
    const double r[9] = {(m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det,
                         (m[1] * m[5] - m[2] * m[4]) / det, (m[5] * m[6] - m[3] * m[8]) / det,
                         (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
                         (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det,
                         (m[0] * m[4] - m[1] * m[3]) / det};
    std::copy(r, r + 9, out);
    return true;
}

// 归一化后让最大元素量级为 1 (h8 可能接近 0, 不能拿它归一)。
Mat3 to_mat3(const double h[9]) {
    double s = std::fabs(h[8]) > 1e-9 ? h[8] : 0.0;
    if (s == 0.0) {
        for (int i = 0; i < 9; ++i) s = std::max(s, std::fabs(h[i]));
    }
    Mat3 m;
    for (int i = 0; i < 9; ++i) m.m[i] = (float)(h[i] / s);
    return m;
}

bool parse_pairs(const Json& arr, const char* what, std::vector<GroundPair>* out,
                 std::string* err) {
    out->clear();
    if (arr.is_null()) return true;
    if (!arr.is_array()) {
        if (err) *err = std::string(what) + " 必须是 [[u,v,X,Y], ...]";
        return false;
    }
    for (size_t i = 0; i < arr.size(); ++i) {
        const Json& p = arr[i];
        bool ok = p.is_array() && p.size() == 4;
        for (size_t k = 0; ok && k < 4; ++k) ok = p[k].is_number() && std::isfinite(p[k].number(0));
        if (!ok) {
            if (err)
                *err = std::string(what) + "[" + std::to_string(i) + "] 必须是 4 个数字 [u,v,X,Y]";
            return false;
        }
        GroundPair g;
        g.img = Point{(float)p[0].number(0), (float)p[1].number(0)};
        g.ground = Point{(float)p[2].number(0), (float)p[3].number(0)};
        if (g.img.x < -0.5f || g.img.x > 1.5f || g.img.y < -0.5f || g.img.y > 1.5f) {
            if (err)
                *err = std::string(what) + "[" + std::to_string(i) + "] 的画面坐标要归一化到 0..1";
            return false;
        }
        out->push_back(g);
    }
    return true;
}

float rms(const std::vector<float>& v) {
    if (v.empty()) return 0.0f;
    double s = 0;
    for (float x : v) s += (double)x * x;
    return (float)std::sqrt(s / (double)v.size());
}

Json number_array(const float* v, int n) {
    Json a = Json::array();
    for (int i = 0; i < n; ++i) a.push(Json::number_value(v[i]));
    return a;
}

Json point_array(const std::vector<Point>& pts) {
    Json a = Json::array();
    for (const Point& p : pts) {
        Json q = Json::array();
        q.push(Json::number_value(p.x));
        q.push(Json::number_value(p.y));
        a.push(q);
    }
    return a;
}

float cross3(Point o, Point a, Point b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}

}  // namespace

std::vector<Point> convex_hull(std::vector<Point> pts) {
    std::sort(pts.begin(), pts.end(),
              [](Point a, Point b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
    pts.erase(std::unique(pts.begin(), pts.end(),
                          [](Point a, Point b) { return a.x == b.x && a.y == b.y; }),
              pts.end());
    if (pts.size() < 3) return pts;
    std::vector<Point> h(pts.size() * 2);
    size_t k = 0;
    for (size_t i = 0; i < pts.size(); ++i) {
        while (k >= 2 && cross3(h[k - 2], h[k - 1], pts[i]) <= 0) --k;
        h[k++] = pts[i];
    }
    for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i) {
        while (k >= t && cross3(h[k - 2], h[k - 1], pts[i - 1]) <= 0) --k;
        h[k++] = pts[i - 1];
    }
    h.resize(k - 1);
    return h;
}

float CalibQuality::error_estimate_m() const {
    if (has_check) return check_rmse_m;
    if (n_points > 4) return rmse_m;
    return -1.0f;
}

bool GroundCalib::parse_region(const Json& r, std::vector<GroundPair>* calib,
                               std::vector<GroundPair>* check, std::string* mode,
                               std::string* err) {
    if (err) err->clear();
    calib->clear();
    check->clear();
    if (!parse_pairs(r["check_points"], "check_points", check, err)) return false;
    const Json& cp = r["calib_points"];
    if (!cp.is_null()) {
        if (!parse_pairs(cp, "calib_points", calib, err)) return false;
        if (calib->size() < 4) {
            if (err) *err = "calib_points 至少要 4 个点";
            return false;
        }
        if (mode) *mode = "points";
        return true;
    }
    const Json& real = r["real_size_m"];
    if (real.is_null()) return false;
    const Json& pts = r["points"];
    if (!real.is_array() || real.size() < 2 || !pts.is_array() || pts.size() != 4) {
        if (err) *err = "矩形地面标定需要正好 4 个 points 和 real_size_m [宽, 深]";
        return false;
    }
    const float w = (float)real[0].number(0.0), h = (float)real[1].number(0.0);
    if (!(w > 0.0f) || !(h > 0.0f)) {
        if (err) *err = "real_size_m 必须是正数";
        return false;
    }
    // 约定 p0=画面左下(最近), p1=右下, p2=右上, p3=左上, 地面 y 轴指向远处。
    const Point dst[4] = {Point{0, 0}, Point{w, 0}, Point{w, h}, Point{0, h}};
    for (int i = 0; i < 4; ++i) {
        const Json& p = pts[(size_t)i];
        calib->push_back(
            GroundPair{Point{(float)p[0].number(0.0), (float)p[1].number(0.0)}, dst[i]});
    }
    if (mode) *mode = "rect";
    return true;
}

bool GroundCalib::fit(const std::vector<GroundPair>& calib, const std::vector<GroundPair>& check,
                      std::string* err) {
    valid_ = false;
    q_ = CalibQuality();
    q_.n_points = (int)calib.size();
    q_.n_check = (int)check.size();
    hull_m_.clear();
    auto fail = [&](const std::string& why) {
        q_.grade = "invalid";
        q_.warnings.push_back(why);
        if (err) *err = why;
        return false;
    };
    if (calib.size() < 4) return fail("标定点少于 4 个");

    std::vector<Point> src, dst;
    for (const GroundPair& p : calib) {
        src.push_back(p.img);
        dst.push_back(p.ground);
    }
    double ts[9], td[9];
    if (!normalizer(src, ts) || !normalizer(dst, td)) return fail("标定点重合, 无法拟合");

    double ata[9][9] = {};
    for (size_t i = 0; i < calib.size(); ++i) {
        const double x = ts[0] * src[i].x + ts[2], y = ts[4] * src[i].y + ts[5];
        const double u = td[0] * dst[i].x + td[2], v = td[4] * dst[i].y + td[5];
        const double r0[9] = {-x, -y, -1, 0, 0, 0, u * x, u * y, u};
        const double r1[9] = {0, 0, 0, -x, -y, -1, v * x, v * y, v};
        for (int a = 0; a < 9; ++a)
            for (int b = 0; b < 9; ++b) ata[a][b] += r0[a] * r0[b] + r1[a] * r1[b];
    }
    double vals[9], vecs[9][9];
    jacobi_eigen(ata, 9, vals, vecs);
    int order[9];
    for (int i = 0; i < 9; ++i) order[i] = i;
    std::sort(order, order + 9, [&](int a, int b) { return vals[a] < vals[b]; });
    const double vmax = std::max(1e-300, vals[order[8]]);
    // 第二小的特征值也接近 0: 解不唯一 (三点共线、点挤在一起), 标定退化
    if (vals[order[1]] / vmax < 1e-9) return fail("标定点退化 (三点共线或重复), 请换位置重新标定");

    double hn[9];
    for (int i = 0; i < 9; ++i) hn[i] = vecs[i][order[0]];
    double td_inv[9], h[9];
    if (!inv3(td, td_inv)) return fail("标定点退化");
    mul3(td_inv, hn, h);
    mul3(h, ts, h);
    h_ = to_mat3(h);
    if (!invert_mat3(h_, &h_inv_)) return fail("单应性矩阵奇异, 请重新标定");

    // 所有标定点必须在地平线同一侧, 否则说明有点标在了天上或者对应关系错位
    int pos = 0, neg = 0;
    for (const Point& p : src) {
        const float w = h_.m[6] * p.x + h_.m[7] * p.y + h_.m[8];
        if (w > 0) ++pos;
        else ++neg;
    }
    if (pos && neg) return fail("标定点跨越地平线, 检查画面点和地面坐标的对应顺序");
    w_sign_ = pos ? 1.0f : -1.0f;
    valid_ = true;

    for (const GroundPair& p : calib) {
        Point g;
        const float e = to_ground(p.img, &g) ? std::hypot(g.x - p.ground.x, g.y - p.ground.y)
                                              : 1e6f;
        q_.residuals_m.push_back(e);
    }
    q_.rmse_m = rms(q_.residuals_m);
    q_.max_err_m = q_.residuals_m.empty()
                       ? 0.0f
                       : *std::max_element(q_.residuals_m.begin(), q_.residuals_m.end());
    for (const GroundPair& p : check) {
        Point g;
        const float e = to_ground(p.img, &g) ? std::hypot(g.x - p.ground.x, g.y - p.ground.y)
                                              : 1e6f;
        q_.check_residuals_m.push_back(e);
    }
    q_.has_check = !check.empty();
    q_.check_rmse_m = rms(q_.check_residuals_m);
    q_.check_max_m = q_.check_residuals_m.empty()
                         ? 0.0f
                         : *std::max_element(q_.check_residuals_m.begin(),
                                             q_.check_residuals_m.end());
    q_.coverage = polygon_area(convex_hull(src));
    hull_m_ = convex_hull(dst);

    // ---- 分级 ----
    char buf[160];
    int level = 0;  // 0 good, 1 fair, 2 poor
    const float e = q_.error_estimate_m();
    if (e < 0.0f) {
        level = 1;
        q_.warnings.push_back("只有 4 个标定点且没有检查点, 无法评估精度; 建议再量 1~2 个检查点");
    } else if (e > 0.4f) {
        level = 2;
    } else if (e > 0.15f) {
        level = 1;
    }
    if (q_.n_points > 4) {
        std::vector<float> sorted = q_.residuals_m;
        std::sort(sorted.begin(), sorted.end());
        const float median = sorted[sorted.size() / 2];
        for (size_t i = 0; i < q_.residuals_m.size(); ++i) {
            if (q_.residuals_m[i] > std::max(0.3f, 3.0f * median)) {
                snprintf(buf, sizeof(buf), "第 %zu 个标定点残差 %.2fm, 可能点错或量错", i + 1,
                         q_.residuals_m[i]);
                q_.warnings.push_back(buf);
                level = std::max(level, 1);
            }
        }
    }
    for (size_t i = 0; i < q_.check_residuals_m.size(); ++i) {
        if (q_.check_residuals_m[i] > 0.5f) {
            snprintf(buf, sizeof(buf), "第 %zu 个检查点误差 %.2fm", i + 1, q_.check_residuals_m[i]);
            q_.warnings.push_back(buf);
        }
    }
    if (q_.coverage < 0.05f) {
        snprintf(buf, sizeof(buf), "标定点只覆盖画面 %.0f%%, 标定范围外的位置误差会很大",
                 q_.coverage * 100.0f);
        q_.warnings.push_back(buf);
        level = std::min(2, level + 1);
    }
    static const char* grades[] = {"good", "fair", "poor"};
    q_.grade = grades[level];
    return true;
}

bool GroundCalib::to_ground(Point img_norm, Point* g) const {
    if (!valid_) return false;
    const float w = h_.m[6] * img_norm.x + h_.m[7] * img_norm.y + h_.m[8];
    if (w * w_sign_ <= 1e-6f) return false;
    return apply_homography(h_, img_norm, g);
}

bool GroundCalib::to_image(Point g, Point* img_norm) const {
    if (!valid_) return false;
    return apply_homography(h_inv_, g, img_norm);
}

float GroundCalib::sigma_m(Point p, float sigma_px, int w, int h) const {
    if (!valid_ || w <= 0 || h <= 0) return -1.0f;
    Point g0, gx, gy;
    const Point px{p.x + 1.0f / (float)w, p.y};
    const Point py{p.x, p.y + 1.0f / (float)h};
    if (!to_ground(p, &g0) || !to_ground(px, &gx) || !to_ground(py, &gy)) return -1.0f;
    const float jx = std::hypot(gx.x - g0.x, gx.y - g0.y);
    const float jy = std::hypot(gy.x - g0.x, gy.y - g0.y);
    return sigma_px * std::sqrt(0.5f * (jx * jx + jy * jy));
}

bool GroundCalib::in_hull(Point g) const {
    return hull_m_.size() >= 3 && point_in_polygon(hull_m_, g);
}

void GroundCalib::bounds(float out[4]) const {
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    if (hull_m_.empty()) return;
    out[0] = out[2] = hull_m_[0].x;
    out[1] = out[3] = hull_m_[0].y;
    for (const Point& p : hull_m_) {
        out[0] = std::min(out[0], p.x);
        out[1] = std::min(out[1], p.y);
        out[2] = std::max(out[2], p.x);
        out[3] = std::max(out[3], p.y);
    }
}

Json GroundCalib::to_json() const {
    Json j = Json::object();
    j.set("valid", Json::bool_value(valid_));
    if (valid_) {
        j.set("h", number_array(h_.m, 9));
        j.set("h_inv", number_array(h_inv_.m, 9));
        j.set("hull_m", point_array(hull_m_));
        float b[4];
        bounds(b);
        j.set("bounds_m", number_array(b, 4));
    }
    Json q = Json::object();
    q.set("grade", Json::string_value(q_.grade));
    q.set("n_points", Json::number_value(q_.n_points));
    q.set("n_check", Json::number_value(q_.n_check));
    q.set("rmse_m", Json::number_value(q_.rmse_m));
    q.set("max_err_m", Json::number_value(q_.max_err_m));
    q.set("check_rmse_m", q_.has_check ? Json::number_value(q_.check_rmse_m) : Json());
    q.set("check_max_m", q_.has_check ? Json::number_value(q_.check_max_m) : Json());
    q.set("residuals_m", number_array(q_.residuals_m.data(), (int)q_.residuals_m.size()));
    q.set("check_residuals_m",
          number_array(q_.check_residuals_m.data(), (int)q_.check_residuals_m.size()));
    q.set("coverage", Json::number_value(q_.coverage));
    Json w = Json::array();
    for (const std::string& s : q_.warnings) w.push(Json::string_value(s));
    q.set("warnings", w);
    j.set("quality", q);
    return j;
}

}  // namespace mb
