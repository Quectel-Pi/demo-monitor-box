#include "common/geometry.h"

#include <cmath>
#include <cstring>

namespace mb {
namespace {

bool solve_linear(double a[8][9], double x[8]) {
    // 列主元高斯消元。n 固定 8, 规模小, 不做分块。
    constexpr int n = 8;
    for (int col = 0; col < n; ++col) {
        int pivot = col;
        double best = std::fabs(a[col][col]);
        for (int r = col + 1; r < n; ++r) {
            const double v = std::fabs(a[r][col]);
            if (v > best) {
                best = v;
                pivot = r;
            }
        }
        if (best < 1e-12) return false;
        if (pivot != col) {
            for (int c = col; c <= n; ++c) {
                const double t = a[col][c];
                a[col][c] = a[pivot][c];
                a[pivot][c] = t;
            }
        }
        const double d = a[col][col];
        for (int c = col; c <= n; ++c) a[col][c] /= d;
        for (int r = 0; r < n; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            if (f == 0.0) continue;
            for (int c = col; c <= n; ++c) a[r][c] -= f * a[col][c];
        }
    }
    for (int i = 0; i < n; ++i) x[i] = a[i][n];
    return true;
}

}  // namespace

bool point_in_polygon(const std::vector<Point>& poly, Point p) {
    const size_t n = poly.size();
    if (n < 3) return false;
    bool inside = false;
    for (size_t i = 0, j = n - 1; i < n; j = i++) {
        const Point& pi = poly[i];
        const Point& pj = poly[j];
        // 边界上的点算在内: 先判叉积是否共线且落在包围盒里。
        const float d = cross(pj, pi, p);
        if (std::fabs(d) < 1e-6f && p.x >= std::fmin(pi.x, pj.x) - 1e-3f &&
            p.x <= std::fmax(pi.x, pj.x) + 1e-3f && p.y >= std::fmin(pi.y, pj.y) - 1e-3f &&
            p.y <= std::fmax(pi.y, pj.y) + 1e-3f) {
            return true;
        }
        // 标准的奇偶规则: 用 (yi > y) != (yj > y) 避免顶点被数两次。
        if (((pi.y > p.y) != (pj.y > p.y)) &&
            (p.x < (pj.x - pi.x) * (p.y - pi.y) / (pj.y - pi.y) + pi.x)) {
            inside = !inside;
        }
    }
    return inside;
}

float distance_to_polygon_edge(const std::vector<Point>& poly, Point p) {
    if (poly.size() < 2) return 1e30f;
    float best = 1e30f;
    const size_t n = poly.size();
    for (size_t i = 0; i < n; ++i) {
        const Point& a = poly[i];
        const Point& b = poly[(i + 1) % n];
        const float dx = b.x - a.x;
        const float dy = b.y - a.y;
        const float len2 = dx * dx + dy * dy;
        float t = 0.0f;
        if (len2 > 1e-12f) t = ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
        const float ex = a.x + t * dx - p.x;
        const float ey = a.y + t * dy - p.y;
        const float d = std::sqrt(ex * ex + ey * ey);
        if (d < best) best = d;
    }
    return best;
}

float polygon_area(const std::vector<Point>& poly) {
    if (poly.size() < 3) return 0.0f;
    float s = 0.0f;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++) {
        s += poly[j].x * poly[i].y - poly[i].x * poly[j].y;
    }
    return std::fabs(s) * 0.5f;
}

Point polygon_centroid(const std::vector<Point>& poly) {
    Point c;
    if (poly.empty()) return c;
    for (const Point& p : poly) {
        c.x += p.x;
        c.y += p.y;
    }
    c.x /= (float)poly.size();
    c.y /= (float)poly.size();
    return c;
}

float cross(const Point& a, const Point& b, const Point& c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool segments_intersect(Point a, Point b, Point c, Point d, Point* hit) {
    const float d1 = cross(c, d, a);
    const float d2 = cross(c, d, b);
    const float d3 = cross(a, b, c);
    const float d4 = cross(a, b, d);

    // 端点接在线上也算相交 (轨迹点正好落在计数线上是常态)。
    const bool straddle1 = (d1 > 0.f && d2 < 0.f) || (d1 < 0.f && d2 > 0.f) || d1 == 0.f || d2 == 0.f;
    const bool straddle2 = (d3 > 0.f && d4 < 0.f) || (d3 < 0.f && d4 > 0.f) || d3 == 0.f || d4 == 0.f;
    if (!straddle1 || !straddle2) return false;

    // 参数式求交点: a + t(b-a) = c + u(d-c)
    const float r_x = b.x - a.x, r_y = b.y - a.y;
    const float s_x = d.x - c.x, s_y = d.y - c.y;
    const float denom = r_x * s_y - r_y * s_x;
    if (std::fabs(denom) < 1e-9f) {
        // 共线重叠: 没有唯一交点, 取线段 a 的端点里落在 c-d 上的那个。
        if (hit) {
            if (d3 == 0.f) *hit = c;
            else if (d4 == 0.f) *hit = d;
            else *hit = a;
        }
        return true;
    }
    const float t = ((c.x - a.x) * s_y - (c.y - a.y) * s_x) / denom;
    if (hit) {
        hit->x = a.x + t * r_x;
        hit->y = a.y + t * r_y;
    }
    return true;
}

bool homography_from_quad(const Point src[4], const Point dst[4], Mat3* out) {
    // 每组对应点给出两行方程, 4 组共 8 行, 未知数 h0..h7 (h8=1)。
    double a[8][9];
    memset(a, 0, sizeof(a));
    for (int i = 0; i < 4; ++i) {
        const double x = src[i].x, y = src[i].y;
        const double u = dst[i].x, v = dst[i].y;
        double* r0 = a[i * 2];
        double* r1 = a[i * 2 + 1];
        r0[0] = x; r0[1] = y; r0[2] = 1;
        r0[6] = -u * x; r0[7] = -u * y; r0[8] = u;
        r1[3] = x; r1[4] = y; r1[5] = 1;
        r1[6] = -v * x; r1[7] = -v * y; r1[8] = v;
    }
    double h[8];
    if (!solve_linear(a, h)) return false;

    Mat3 m;
    m.m[0] = (float)h[0]; m.m[1] = (float)h[1]; m.m[2] = (float)h[2];
    m.m[3] = (float)h[3]; m.m[4] = (float)h[4]; m.m[5] = (float)h[5];
    m.m[6] = (float)h[6]; m.m[7] = (float)h[7]; m.m[8] = 1.0f;
    if (out) *out = m;
    return true;
}

bool apply_homography(const Mat3& h, Point p, Point* out) {
    const float w = h.m[6] * p.x + h.m[7] * p.y + h.m[8];
    if (std::fabs(w) < 1e-9f) return false;
    const float x = (h.m[0] * p.x + h.m[1] * p.y + h.m[2]) / w;
    const float y = (h.m[3] * p.x + h.m[4] * p.y + h.m[5]) / w;
    if (!std::isfinite(x) || !std::isfinite(y)) return false;
    if (out) *out = Point{x, y};
    return true;
}

bool invert_mat3(const Mat3& in, Mat3* out) {
    const float* m = in.m;
    const float det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
                      m[2] * (m[3] * m[7] - m[4] * m[6]);
    if (std::fabs(det) < 1e-12f) return false;
    const float inv = 1.0f / det;
    Mat3 r;
    r.m[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
    r.m[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
    r.m[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
    r.m[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
    r.m[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
    r.m[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
    r.m[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
    r.m[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
    r.m[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
    if (out) *out = r;
    return true;
}

bool Affine2::is_identity(float eps) const {
    static const float id[6] = {1, 0, 0, 0, 1, 0};
    for (int i = 0; i < 6; ++i)
        if (std::fabs(m[i] - id[i]) > eps) return false;
    return true;
}

Affine2 compose(const Affine2& o, const Affine2& i) {
    Affine2 r;
    r.m[0] = o.m[0] * i.m[0] + o.m[1] * i.m[3];
    r.m[1] = o.m[0] * i.m[1] + o.m[1] * i.m[4];
    r.m[2] = o.m[0] * i.m[2] + o.m[1] * i.m[5] + o.m[2];
    r.m[3] = o.m[3] * i.m[0] + o.m[4] * i.m[3];
    r.m[4] = o.m[3] * i.m[1] + o.m[4] * i.m[4];
    r.m[5] = o.m[3] * i.m[2] + o.m[4] * i.m[5] + o.m[5];
    return r;
}

bool invert_affine(const Affine2& in, Affine2* out) {
    const float* m = in.m;
    const float det = m[0] * m[4] - m[1] * m[3];
    if (std::fabs(det) < 1e-9f) return false;
    Affine2 r;
    r.m[0] = m[4] / det;
    r.m[1] = -m[1] / det;
    r.m[3] = -m[3] / det;
    r.m[4] = m[0] / det;
    r.m[2] = -(r.m[0] * m[2] + r.m[1] * m[5]);
    r.m[5] = -(r.m[3] * m[2] + r.m[4] * m[5]);
    if (out) *out = r;
    return true;
}

}  // namespace mb
